/* Copyright 2002 by Hans Reiser, licensing governed by reiser4/README */

/* Implementation of emergency flush. */

/* OVERVIEW:
  
     Reiser4 maintains all meta data in a single balanced tree. This tree is
     maintained in memory in a form different from what will ultimately be
     written to the disk. Roughly speaking, before writing a tree
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
     large queue of nodes ready to be submitted for io.
  
     Items (3) and (4) alone make flush unsuitable for being called directly
     from reiser4 ->writepage() callback, because of OOM and deadlocks
     against threads waiting for memory.
  
     So, flush is performed from within balance_dirty_page() path when dirty
     pages are generated. If balance_dirty_page() fails to throttle writers
     and page replacement finds dirty page on the inactive list, we resort to
     "emergency flush" in our ->vm_writeback(). Emergency flush is relatively
     dumb algorithm, implemented in this file, that tries to write tree nodes
     to the disk without taking locks and without thoroughly optimizing tree
     layout. We only want to call emergency flush in desperate situations,
     because it is going to produce sub-optimal disk layouts.
  
  DETAILED DESCRIPTION

     Emergency flush (eflush) is designed to work as low level mechanism with
     no or little impact on the rest of (already too complex) code.

     eflush is initiated from ->writepage() method called by VM on memory
     pressure. It is supposed that ->writepage() is rare call path, because
     balance_dirty_pages() throttles writes and tries to keep memory in
     balance.

     eflush main entry point (emergency_flush()) checks whether jnode is
     eligible for emergency flushing. Check is performed by flushable()
     function which see for details. After successful check, new block number
     ("emergency block") is allocated and io is initiated to write jnode
     content to that block.

     After io is finished, jnode will be cleaned and VM will be able to free
     page through call to ->releasepage().

     emergency_flush() also contains special case invoked when it is possible
     to avoid allocation of new node.

     Node selected for eflush is marked (by JNODE_EFLUSH bit in ->flags field)
     and added to the special hash table of all eflushed nodes. This table
     doesn't have linkage within each jnode, as this would waste memory in
     assumption that eflush is rare. In stead new small memory object
     (eflush_node_t) is allocated that contains pointer to jnode, emergency
     block number, and is inserted into hash table. Per super block counter of
     eflushed nodes is incremented. See section [INODE HANDLING] below on
     more.

     It should be noted that emergency flush may allocate memory and wait for
     io completion (bitmap read).

     Basically eflushed node has following distinctive characteristics:

      (1) JNODE_EFLUSH bit is set

      (2) no page

      (3) there is an element in hash table, for this node

      (4) node content is stored on disk in block whose number is stored in
      the hash table element

  UNFLUSH

      Unflush is reverse of eflush, that is process bringing page of eflushed
      inode back into memory.

      In accordance with the policy that eflush is low level and low impact
      mechanism, transparent to the rest of the code, unflushing is performed
      deeply within jload_gfp() which is main function used to load and pin
      jnode page into memory.

      Specifically, if jload_gfp() determines that it is called on eflushed
      node it gets emergency block number to start io against from the hash
      table rather than from jnode itself. This is done in
      jnode_get_io_block() function. After io completes, hash table element
      for this node is removed and JNODE_EFLUSH bit is cleared.

  PROBLEMS

  1. INODE HANDLING

      Usually (i.e., without eflush), jnode has a page attached to it. This
      page pins corresponding struct address_space, and, hence, inode in
      memory. Once inode has been eflushed, its page is gone and inode can be
      wiped out of memory by the memory pressure (prune_icache()). This leads
      to the number of complications:

       (1) jload_gfp() has to attach jnode tho the address space's radix
       tree. This requires existence if inode.

       (2) normal flush needs jnode's inode to start slum collection from
       unformatted jnode.

      (1) is really a problem, because it is too late to load inode (which
      would lead to loading of stat data, etc.) within jload_gfp().

      We, therefore, need some way to protect inode from being recycled while
      having accessible eflushed nodes.

      I'll describe old solution here so it can be compared with new one.

      Original solution pinned inode by __iget() when first its node was
      eflushed and released (through iput()) when last was unflushed. This
      required maintenance of inode->eflushed counter in inode.

      Problem arise if last name of inode is unlinked when it has eflushed
      nodes. In this case, last iput() that leads to the removal of file is
      iput() made by unflushing from within jload_gfp(). Obviously, calling
      truncate, and tree traversals from jload_gfp() is not a good idea.

  DISK SPACE ALLOCATION

      This section will describe how emergency block is allocated and how
      block counters (allocated, grabbed, etc.) are manipulated. To be done.

   *****HISTORICAL SECTION****************************************************
  
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

#if REISER4_USE_EFLUSH

static int flushable(const jnode * node, struct page *page);
static int needs_allocation(const jnode * node);
static eflush_node_t *ef_alloc(int flags);
static reiser4_ba_flags_t ef_block_flags(const jnode *node);
static int ef_free_block(jnode *node, const reiser4_block_nr *blk);
static int ef_free_block_with_stage(jnode *node, const reiser4_block_nr *blk, block_stage_t stage);
static int ef_prepare(jnode *node, reiser4_block_nr *blk, eflush_node_t **enode, reiser4_blocknr_hint *hint);
static int eflush_add(jnode *node, reiser4_block_nr *blocknr, eflush_node_t *ef);

/* slab for eflush_node_t's */
static kmem_cache_t *eflush_slab;

#define EFLUSH_START_BLOCK ((reiser4_block_nr)0)

/* try to flush @page to the disk */
int
emergency_flush(struct page *page)
{
	struct super_block *sb;
	jnode *node;
	int result;
	assert("nikita-2721", page != NULL);
	assert("nikita-2724", PageLocked(page));

	/*
	 * Page is locked, hence page<->jnode mapping cannot change.
	 */

	sb = page->mapping->host->i_sb;
	node = jprivate(page);

	if (node == NULL)
		return 0;

	jref(node);
	reiser4_stat_inc_at_level(jnode_get_level(node), emergency_flush);

	trace_on(TRACE_EFLUSH, "eflush: %i...", get_super_private(sb)->eflushed);

	result = 0;
	LOCK_JNODE(node);
	if (flushable(node, page)) {
		if (needs_allocation(node)) {
			reiser4_block_nr blk;
			eflush_node_t *efnode;
			reiser4_blocknr_hint hint;

			blk = 0ull;
			efnode = NULL;

			blocknr_hint_init(&hint);
			
			result = ef_prepare(node, &blk, &efnode, &hint);
			if (flushable(node, page) && result == 0) {
				assert("nikita-2759", efnode != NULL);
				eflush_add(node, &blk, efnode);

				/* XXX JNODE_WRITEBACK bit is not set here */
				result = page_io(page, 
						 node, WRITE, GFP_NOFS | __GFP_HIGH);
				if (result == 0) {
					result = 1;
					trace_on(TRACE_EFLUSH, "ok: %llu\n", blk);
				} else {
					/* 
					 * XXX may be set_page_dirty() should be called
					 */
					__set_page_dirty_nobuffers(page);
					trace_on(TRACE_EFLUSH, "submit-failure\n");
				}
			} else {
				UNLOCK_JNODE(node);
				if (blk != 0ull)
					ef_free_block_with_stage(node, &blk, hint.block_stage);
				if (efnode != NULL)
					kmem_cache_free(eflush_slab, efnode);
				trace_on(TRACE_EFLUSH, "failure-2\n");
			}
			
			blocknr_hint_done(&hint);
		} else {
			txn_atom *atom;
			flush_queue_t *fq;

			/* eflush without allocation temporary location for a node */
			trace_on(TRACE_EFLUSH, "flushing to relocate place: %llu..", *jnode_get_block(node));
			
			/* get flush queue for this node */
			result = fq_by_jnode(node, &fq);

			if (result) {
				return result;
			}

			atom = node->atom;

			if (!flushable(node, page) || needs_allocation(node) || !jnode_is_dirty(node)) {
				trace_on(TRACE_EFLUSH, "failure-3\n");
				UNLOCK_JNODE(node);
				UNLOCK_ATOM(atom);
				fq_put(fq);
				return 0;
			}

			/* ok, now we can flush it */
			reiser4_unlock_page(page);

			queue_jnode(fq, node);

			UNLOCK_JNODE(node);
			UNLOCK_ATOM(atom);

			result = write_fq(fq, 0);
			trace_on(TRACE_EFLUSH, "flushed %d blocks\n", result);
			/* Even if we wrote nothing, We unlocked the page, so let know to the caller that page should
			   not be unlocked again */
			result = 1; 
			fq_put(fq);
		}
		
	} else {
		UNLOCK_JNODE(node);
		trace_on(TRACE_EFLUSH, "failure-1\n");
	}

	jput(node);
	return result;
}

static int
flushable(const jnode * node, struct page *page)
{
	assert("nikita-2725", node != NULL);
	assert("nikita-2726", spin_jnode_is_locked(node));

	if (!jnode_is_dirty(node))
		return 0;
	if (node->d_count != 0)   /* used */
		return 0;
	if (jnode_is_loaded(node))              /* loaded */
		return 0;
	if (JF_ISSET(node, JNODE_FLUSH_QUEUED)) /* already pending io */
		return 0;
	if (JF_ISSET(node, JNODE_EPROTECTED))   /* protected from e-flush */
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
	return 1;
}

/* does node need allocation for eflushing? */
static int
needs_allocation(const jnode * node)
{
	return !(JF_ISSET(node, JNODE_RELOC) && !blocknr_is_fake(jnode_get_block(node)));
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
#if REISER4_DEBUG
	block_stage_t    initial_stage;
#endif
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
			    REISER4_EF_HASH_SIZE, 
			    reiser4_stat(super, hashes.eflush));
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
	reiser4_tree  *tree;

	assert("nikita-2737", node != NULL);
	assert("nikita-2738", !JF_ISSET(node, JNODE_EFLUSH));
	assert("nikita-2765", spin_jnode_is_locked(node));

	tree = jnode_get_tree(node);

	ef->node = node;
	ef->blocknr = *blocknr;
	jref(node);
	WLOCK_TREE(tree);
	ef_hash_insert(get_jnode_enhash(node), ef);
	++ get_super_private(tree->super)->eflushed;
	WUNLOCK_TREE(tree);
	/*
	 * set JNODE_EFLUSH bit on the jnode. inode is not yet pinned at this
	 * point. We are safe, because page it is still attached to both @node
	 * and its inode. Page cannot be released at this point, because it is
	 * locked.
	 */
	JF_SET(node, JNODE_EFLUSH);
	UNLOCK_JNODE(node);

	if (jnode_is_unformatted(node)) {
		struct inode  *inode;
		reiser4_inode *info;

		inode = jnode_mapping(node)->host;
		info = reiser4_inode_data(inode);
		/* pin inode containing eflushed pages. Otherwise it
		 * may get evicted */
		spin_lock_inode(inode);
		++ info->eflushed;
		spin_unlock_inode(inode);
	}
	return 0;
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

	ef = UNDER_RW(tree, jnode_get_tree(node), read,
		      ef_hash_find(get_jnode_enhash(node), C(node)));

	assert("nikita-2742", ef != NULL);
	return &ef->blocknr;
}

void
eflush_del(jnode *node, int page_locked)
{
	eflush_node_t *ef;
	ef_hash_table *table;
	reiser4_tree  *tree;

	assert("nikita-2743", node != NULL);
	assert("nikita-2770", spin_jnode_is_locked(node));

	if (JF_ISSET(node, JNODE_EFLUSH)) {
		reiser4_block_nr blk;
		struct page *page;
		struct inode  *inode = NULL;

		table = get_jnode_enhash(node);

		tree = jnode_get_tree(node);

		WLOCK_TREE(tree);
		ef = ef_hash_find(table, C(node));
		assert("nikita-2745", ef != NULL);
		blk = ef->blocknr;
		ef_hash_remove(table, ef);
		-- get_super_private(tree->super)->eflushed;
		WUNLOCK_TREE(tree);

		if (jnode_is_unformatted(node)) {
			reiser4_inode *info;
			int despatchhim = 0;

			inode = jnode_mapping(node)->host;
			info = reiser4_inode_data(inode);
			/* unpin inode after unflushing last eflushed page
			 * from it. Dual to __iget() in eflush_add(). */
			spin_lock_inode(inode);
			assert("vs-1194", info->eflushed > 0);
			-- info->eflushed;
			if (info->eflushed == 0 && (inode->i_state & I_GHOST))
				despatchhim = 1;
			spin_unlock_inode(inode);
			if (despatchhim)
				inode->i_sb->s_op->destroy_inode(inode);
		}

		JF_CLR(node, JNODE_EFLUSH);

		page = jnode_page(node);

		/* there is no reason to unflush node if it can be flushed
		 * back immediately */
		assert("nikita-3083", !flushable(node, page) || page_locked);

		assert("nikita-2806", ergo(page_locked, page != NULL));
		assert("nikita-2807", ergo(page_locked, PageLocked(page)));
		if (!page_locked && page != NULL) {
			/* emergency flush hasn't reclaimed page yet. Wait
			 * until io is submitted. Otherwise there is a room
			 * for a race: emergency_flush() calls page_io() and
			 * we clear JNODE_EFLUSH bit concurrently---page_io()
			 * gets wrong block number. */
			page_cache_get(page);
			UNLOCK_JNODE(node);
			wait_on_page_locked(page);
			page_cache_release(page);
			LOCK_JNODE(node);
		}
		assert("nikita-2766", atomic_read(&node->x_count) > 1);

		UNLOCK_JNODE(node);

#if REISER4_DEBUG
		if (blocknr_is_fake(jnode_get_block(node))) {
			assert ("zam-817", ef->initial_stage == BLOCK_UNALLOCATED);
		} else {
			assert ("zam-818", ef->initial_stage == BLOCK_GRABBED);
		}
#endif

		jput(node);

		kmem_cache_free(eflush_slab, ef);
		ef_free_block(node, &blk);

		LOCK_JNODE(node);

		trace_on(TRACE_EFLUSH, "unflush: %i...\n", 
			 get_super_private(tree->super)->eflushed);
	}
}

int
emergency_unflush(jnode *node)
{
	int result;

	assert("nikita-2778", node != NULL);
	assert("nikita-3046", schedulable());

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

static int ef_free_block_with_stage(jnode *node, const reiser4_block_nr *blk, block_stage_t stage)
{
	int result = 0;
	reiser4_block_nr one;

	one = 1ull;
	/* We cannot just ask block allocator to return block into flush
	 * reserved space, because there is no current atom at this point. */
	result = reiser4_dealloc_blocks(blk, &one, stage, ef_block_flags(node), "ef_free_block_with_stage");
	if (result == 0 && stage == BLOCK_GRABBED) {
		txn_atom *atom;

		/* further, transfer block from grabbed into flush reserved
		 * space. */
		LOCK_JNODE(node);
		atom = atom_locked_by_jnode(node);
		assert("nikita-2785", atom != NULL);
		grabbed2flush_reserved_nolock(atom, 1, "ef_free_block_with_stage");
		UNLOCK_ATOM(atom);
		UNLOCK_JNODE(node);
	}
	return result;
}

static int ef_free_block(jnode *node, const reiser4_block_nr *blk)
{
	block_stage_t stage;

	stage = blocknr_is_fake(jnode_get_block(node)) ? 
		BLOCK_UNALLOCATED : BLOCK_GRABBED;

	return ef_free_block_with_stage(node, blk, stage);
}

static int 
ef_prepare(jnode *node, reiser4_block_nr *blk, eflush_node_t **efnode, reiser4_blocknr_hint * hint)
{
	int result;
	reiser4_block_nr one;

	assert("nikita-2760", node != NULL);
	assert("nikita-2761", blk != NULL);
	assert("nikita-2762", efnode != NULL);
	assert("nikita-2763", spin_jnode_is_locked(node));

	hint->blk         = EFLUSH_START_BLOCK;
	hint->max_dist    = 0;
	hint->level       = jnode_get_level(node);
	if (blocknr_is_fake(jnode_get_block(node)))
		hint->block_stage = BLOCK_UNALLOCATED;
	else {
		txn_atom *atom;

		/* We cannot just ask block allocator to take block from flush
		 * reserved space, because there is no current atom at this
		 * point. */
		atom = atom_locked_by_jnode(node);
		assert("nikita-2785", atom != NULL);
		flush_reserved2grabbed(atom, 1);
		UNLOCK_ATOM(atom);
		hint->block_stage = BLOCK_GRABBED;
	}

	/* XXX protect @node from being concurrently eflushed. Otherwise,
	 * there is a danger of underflowing block space */
	UNLOCK_JNODE(node);

	one = 1ull;
	result = reiser4_alloc_blocks(hint, blk, &one, ef_block_flags(node), "ef_prepare");
	if (result == 0) {
		*efnode = ef_alloc(GFP_NOFS | __GFP_HIGH);
		if (*efnode == NULL)
			result = -ENOMEM;
		else {
#if REISER4_DEBUG
			(*efnode)->initial_stage = hint->block_stage;
#endif
		}
	}
	LOCK_JNODE(node);
	return result;
}

#endif

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   LocalWords: " unflush eflushed LocalWords eflush writepage VM releasepage unflushing io "
   End:
*/
