/* Copyright 2002 by Hans Reiser, licensing governed by reiser4/README */

/* Implementation of emergency flush. */

/* OVERVIEW:
  
     Reiser4 maintains all meta data in the single balanced tree. This tree is
     maintained in the memory in the form different from what will be
     ultimately written to the disk. Roughly speaking, before writing tree
     node to the disk, some complex process (flush.[ch]) is to be
     performed. Flush is main necessary preliminary step before writing pages
     back to the disk, but it has some characteristics that make it completely
     different from traditional ->writepage():
     
        1 it is not local, that is it operates on a big number of nodes,
        possibly far away from the starting node, both in tree and disk order.
  
        2 it can involve reading of nodes from the disk
        (for example, bitmap nodes are read during extent allocation that is
        deferred until flush).
  
        3 it can allocate unbounded amount of memory (during insertion of
        allocated extents).
  
        4 it participates in the locking protocol which reiser4 uses to
        implement concurrent tree modifications.
  
        5 it is CPU consuming and long
  
     As a result, flush reorganizes some part of reiser4 tree and produces
     large queue of nodes ready to be submitted for io (as a matter of fact,
     flush write clustering is so good that it used to hit BIO_MAX_PAGES all
     the time, until checks were added for this).
  
     Items (3) and (4) alone make flush unsuitable for being called directly
     from reiser4 ->vm_writeback() callback, because of OOM and deadlocks
     against threads waiting for memory.
  
     So, flush is performed from within balance_dirty_page() path when dirty
     pages are generated. If balance_dirty_page() fails to throttle writers
     and page replacement finds dirty page on the inactive list, we resort to
     "emergency flush" in our ->vm_writeback(). Emergency flush is relatively
     dumb algorithm, implemented in this file, that tries to write tree nodes
     to the disk without taking locks and without thoroughly optimizing tree
     layout. We only want to call emergency flush in desperate situations,
     because it is going to produce sub-optimal disk layouts.
  
   DELAYED PARENT UPDATE
  
     Important point of emergency flush is that update of parent is sometimes
     delayed: we don't update parent immediately if:
  
      1 Child was just allocated, but parent is locked. Waiting for parent
      lock in emergency flush is impossible (deadlockable).
  
      2 Part of extent was allocated, but parent has not enough space to
      insert allocated extent unit. Balancing in emergency flush is
      impossible, because it will possibly wait on locks.
  
     When we delay update of parent node, we mark it as such (and possibly
     also mark children to simplify delayed update later). Question: when
     parent should be really updated?
  
   WHERE TO WRITE PAGE INTO?
  
    
   *****HISTORICAL SECTION****************************************************
  
     So, it was decided that flush has to be performed from a separate
     thread. Reiser4 has a thread used to periodically commit old transactions,
     and this thread can be used for the flushing. That is, flushing thread
     does flush and accumulates nodes prepared for the IO on the special
     queue. reiser4_vm_writeback() submits nodes from this queue, if queue is
     empty, it only wakes up flushing thread and immediately returns.
  
     Still there are some problems with integrating this stuff into VM
     scanning:
  
        1 As ->vm_writeback() returns immediately without actually submitting
        pages for IO, throttling on PG_writeback in shrink_list() will not
        work. This opens a possibility (on a fast CPU), of try_to_free_pages()
        completing scanning and calling out_of_memory() before flushing thread
        managed to add anything to the queue.
  
        2 It is possible, however unlikely, that flushing thread will be
        unable to flush anything, because there is not enough memory. In this
        case reiser4 resorts to the "emergency flush": some dumb algorithm,
        implemented in this file, that tries to write tree nodes to the disk
        without taking locks and without thoroughly optimizing tree layout. We
        only want to call emergency flush in desperate situations, because it
        is going to produce sub-optimal disk layouts.
  
        3 Nodes prepared for IO can be from the active list, this means that
        they will not be met/freed by shrink_list() after IO completion. New
        blk_congestion_wait() should help with throttling but not
        freeing. This is not fatal though, because inactive list refilling
        will ultimately get to these pages and reclaim them.
  
   REQUIREMENTS
  
     To make this work we need at least some hook inside VM scanning which
     gets triggered after scanning (or scanning with particular priority)
     failed to free pages. This is already present in the
     mm/vmscan.c:set_shrinker() interface.
  
     Another useful thing that we would like to have is passing scanning
     priority down to the ->vm_writeback() that will allow file system to
     switch to the emergency flush more gracefully.
  
   POSSIBLE ALGORITHMS
  
     1 Start emergency flush from ->vm_writeback after reaching some priority.
     This allows to implement simple page based algorithm: look at the page VM
     supplied us with and decide what to do.
  
     2 Start emergency flush from shrinker after reaching some priority.
     This delays emergency flush as far as possible.
  
   *****END OF HISTORICAL SECTION**********************************************
  
*/

#include "forward.h"
#include "debug.h"
#include "page_cache.h"
#include "tree.h"
#include "jnode.h"
#include "znode.h"
#include "inode.h"
#include "super.h"
#include "block_alloc.h"
#include "emergency_flush.h"

#include <linux/mm.h>
#include <linux/writeback.h>
#include <linux/slab.h>

static int flushable(const jnode * node, struct page *page);
static eflush_node_t *ef_alloc(int flags);
static reiser4_ba_flags_t ef_block_flags(const jnode *node);
static int ef_free_block(jnode *node, const reiser4_block_nr *blk);
static int ef_prepare(jnode *node, reiser4_block_nr *blk, eflush_node_t **enode);
static int eflush_add(jnode *node, reiser4_block_nr *blocknr, eflush_node_t *ef);

/* slab for eflush_node_t's */
static kmem_cache_t *eflush_slab;

#define EFLUSH_START_BLOCK ((reiser4_block_nr)0)

/* try to flush @page to the disk */
int
emergency_flush(struct page *page, struct writeback_control *wbc)
{
	struct super_block *sb;
	jnode *node;
	int result;
	reiser4_block_nr blk;
	eflush_node_t *efnode;

	assert("nikita-2721", page != NULL);
	assert("nikita-2722", wbc != NULL);
	assert("nikita-2723", PageDirty(page));
	assert("nikita-2724", PageLocked(page));

	/*
	 * Page is locked, hence page<->jnode mapping cannot change.
	 */

	sb = page->mapping->host->i_sb;
	node = jprivate(page);

	if (node == NULL)
		return 0;

	jref(node);
	reiser4_stat_add_at_level(jnode_get_level(node), emergency_flush);

	result = 0;
	blk = 0ull;
	efnode = NULL;
	spin_lock_jnode(node);
	if (flushable(node, page)) {
		result = ef_prepare(node, &blk, &efnode);
		if (flushable(node, page) && result == 0 && 
		    test_clear_page_dirty(page)) {
			assert("nikita-2759", efnode != NULL);
			eflush_add(node, &blk, efnode);

			spin_unlock_jnode(node);

			/* FIXME-NIKITA JNODE_WRITEBACK bit is not set here */
			result = page_io(page, 
					 node, WRITE, GFP_NOFS | __GFP_HIGH);
			if (result == 0) {
				--wbc->nr_to_write;
				result = 1;
			} else
				/* 
				 * XXX may be set_page_dirty() should be called
				 */
				__set_page_dirty_nobuffers(page);
		} else {
			spin_unlock_jnode(node);
			if (blk != 0ull)
				ef_free_block(node, &blk);
			if (efnode != NULL)
				kmem_cache_free(eflush_slab, efnode);
		}
	}
	jput(node);
	return result;
}

static int
flushable(const jnode * node, struct page *page)
{
	assert("nikita-2725", node != NULL);
	assert("nikita-2726", spin_jnode_is_locked(node));

	if (atomic_read(&node->d_count) != 0)   /* used */
		return 0;
	if (jnode_is_loaded(node))              /* loaded */
		return 0;
	if (JF_ISSET(node, JNODE_FLUSH_QUEUED)) /* already pending io */
		return 0;
	if (PageWriteback(page))                /* already under io */
		return 0;
	/* don't flush bitmaps or journal records */
	if (!jnode_is_znode(node) && !jnode_is_unformatted(node))
		return 0;
	if (JF_ISSET(node, JNODE_EFLUSH))       /* already flushed */
		return 0;
	if (jnode_page(node) == NULL)           /* nothing to flush */
		return 0;
	/* jnode is in relocate set and already has block number
	 * assigned. Skip it to avoid complications with flush queue code. */
	if (JF_ISSET(node, JNODE_RELOC) &&
	    !blocknr_is_fake(jnode_get_block(node)))
		return 0;
	/* extents of jnode's inode are being allocated. Don't flush */
	if (jnode_is_unformatted(node)) {
		struct inode *obj;

		obj = node->key.j.mapping->host;
		assert("nikita-2800", is_reiser4_inode(obj));
		if (inode_get_flag(obj, REISER4_BEING_ALLOCATED))
			return 0;
	}
	return 1;
}

static inline int
jnode_eq(jnode * const * j1, jnode * const * j2)
{
	assert("nikita-2733", j1 != NULL);
	assert("nikita-2734", j2 != NULL);

	return *j1 == *j2;
}

static inline __u32
jnode_hfn(jnode * const * j)
{
	assert("nikita-2735", j != NULL);
	return (((unsigned long)*j) / sizeof(**j)) & (REISER4_EF_HASH_SIZE - 1);
}

struct eflush_node {
	jnode           *node;
	reiser4_block_nr blocknr;
	ef_hash_link     linkage;
};

/* The hash table definition */
#define KMALLOC(size) reiser4_kmalloc((size), GFP_KERNEL)
#define KFREE(ptr, size) reiser4_kfree(ptr, size)
TS_HASH_DEFINE(ef, eflush_node_t, jnode *, node, linkage, jnode_hfn, jnode_eq);
#undef KFREE
#undef KMALLOC

int 
eflush_init(void)
{
	eflush_slab = kmem_cache_create("eflush_cache", sizeof (eflush_node_t), 
					0, SLAB_HWCACHE_ALIGN, NULL, NULL);
	if (eflush_slab == NULL)
		return -ENOMEM;
	else
		return 0;
}

int 
eflush_done(void)
{
	return kmem_cache_destroy(eflush_slab);
}

int
eflush_init_at(struct super_block *super)
{
	return ef_hash_init(&get_super_private(super)->efhash_table, 
			    REISER4_EF_HASH_SIZE);
}

void
eflush_done_at(struct super_block *super)
{
	ef_hash_done(&get_super_private(super)->efhash_table);
}

static ef_hash_table *
get_jnode_enhash(const jnode *node)
{
	struct super_block *super;

	assert("nikita-2739", node != NULL);

	super = jnode_get_tree(node)->super;
	return &get_super_private(super)->efhash_table;
}

static eflush_node_t *
ef_alloc(int flags)
{
	return kmem_cache_alloc(eflush_slab, flags);
}

static int
eflush_add(jnode *node, reiser4_block_nr *blocknr, eflush_node_t *ef)
{
	assert("nikita-2737", node != NULL);
	assert("nikita-2738", !JF_ISSET(node, JNODE_EFLUSH));
	assert("nikita-2765", spin_jnode_is_locked(node));

	if (ef == NULL)
		ef = ef_alloc(GFP_NOFS);
	if (ef != NULL) {
		reiser4_tree *tree;

		tree = jnode_get_tree(node);

		ef->node = node;
		ef->blocknr = *blocknr;
		jref(node);
		spin_lock_tree(tree);
		ef_hash_insert(get_jnode_enhash(node), ef);
		++ get_super_private(tree->super)->eflushed;
		spin_unlock_tree(tree);
		JF_SET(node, JNODE_EFLUSH);
		return 0;
	} else
		return -ENOMEM;
}

/* Arrghh... cast to keep hash table code happy. */
#define C(node) ((jnode *const *)&(node))

reiser4_block_nr *
eflush_get(const jnode *node)
{
	eflush_node_t *ef;

	assert("nikita-2740", node != NULL);
	assert("nikita-2741", JF_ISSET(node, JNODE_EFLUSH));
	assert("nikita-2767", spin_jnode_is_locked(node));

	ef = UNDER_SPIN(tree, jnode_get_tree(node),
			ef_hash_find(get_jnode_enhash(node), C(node)));

	assert("nikita-2742", ef != NULL);
	return &ef->blocknr;
}

void
eflush_del(jnode *node)
{
	eflush_node_t *ef;
	ef_hash_table *table;
	reiser4_tree  *tree;

	assert("nikita-2743", node != NULL);
	assert("nikita-2770", spin_jnode_is_locked(node));

	if (JF_ISSET(node, JNODE_EFLUSH)) {
		reiser4_block_nr blk;

		table = get_jnode_enhash(node);

		tree = jnode_get_tree(node);

		spin_lock_tree(tree);
		ef = ef_hash_find(table, C(node));
		assert("nikita-2745", ef != NULL);
		blk = ef->blocknr;
		ef_hash_remove(table, ef);
		-- get_super_private(tree->super)->eflushed;
		spin_unlock_tree(tree);

		JF_CLR(node, JNODE_EFLUSH);
		spin_unlock_jnode(node);
		ef_free_block(node, &blk);
		assert("nikita-2766", atomic_read(&node->x_count) > 1);
		jput(node);
		kmem_cache_free(eflush_slab, ef);
		spin_lock_jnode(node);
	}
}

int
emergency_unflush(jnode *node)
{
	int result;

	assert("nikita-2778", node != NULL);

	if (JF_ISSET(node, JNODE_EFLUSH)) {
		result = jload(node);
		if (result == 0) {
			struct page *page;

			assert("nikita-2777", !JF_ISSET(node, JNODE_EFLUSH));
			page = jnode_page(node);
			assert("nikita-2779", page != NULL);
			wait_on_page_writeback(page);
			jrelse(node);
		}
	} else
		result = 0;
	return result;
}

static reiser4_ba_flags_t
ef_block_flags(const jnode *node)
{
	return jnode_is_znode(node) ? BA_FORMATTED : 0;
}

static int ef_free_block(jnode *node, const reiser4_block_nr *blk)
{
	block_stage_t stage;
	int result = 0;

	stage = blocknr_is_fake(jnode_get_block(node)) ? 
		BLOCK_UNALLOCATED : BLOCK_GRABBED;

	/* We cannot just ask block allocator to return block into flush
	 * reserved space, because there is no current atom at this point. */
	result = reiser4_dealloc_block(blk, stage, ef_block_flags(node));
	if (result == 0 && stage == BLOCK_GRABBED) {
		txn_atom *atom;

		/* further, transfer block from grabbed into flush reserved
		 * space. */
		atom = atom_get_locked_by_jnode(node);
		assert("nikita-2785", atom != NULL);
		grabbed2flush_reserved_nolock(atom, 1);
		spin_unlock_atom(atom);
	}
	return result;
}

static int ef_prepare(jnode *node, reiser4_block_nr *blk, 
		      eflush_node_t **efnode)
{
	int result;
	reiser4_block_nr     one;
	reiser4_blocknr_hint hint;

	assert("nikita-2760", node != NULL);
	assert("nikita-2761", blk != NULL);
	assert("nikita-2762", efnode != NULL);
	assert("nikita-2763", spin_jnode_is_locked(node));

	one = 1ull;

	hint.blk         = EFLUSH_START_BLOCK;
	hint.max_dist    = 0;
	hint.level       = jnode_get_level(node);
	if (blocknr_is_fake(jnode_get_block(node)))
		hint.block_stage = BLOCK_UNALLOCATED;
	else {
		txn_atom *atom;

		/* We cannot just ask block allocator to take block from flush
		 * reserved space, because there is no current atom at this
		 * point. */
		atom = atom_get_locked_by_jnode(node);
		assert("nikita-2785", atom != NULL);
		flush_reserved2grabbed(atom, 1);
		spin_unlock_atom(atom);
		hint.block_stage = BLOCK_GRABBED;
	}

	/* XXX protect @node from being concurrently eflushed. Otherwise,
	 * there is a danger of underflowing block space */
	spin_unlock_jnode(node);

	result = reiser4_alloc_blocks(&hint, blk, &one, ef_block_flags(node));
	if (result == 0) {
		*efnode = ef_alloc(GFP_NOFS | __GFP_HIGH);
		if (*efnode == NULL)
			result = -ENOMEM;
	}
	spin_lock_jnode(node);
	return result;
}

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
