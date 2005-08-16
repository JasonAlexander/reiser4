/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Memory pressure hooks. Fake inodes handling. */
/* We store all file system meta data (and data, of course) in the page cache.

   What does this mean? In stead of using bread/brelse we create special
   "fake" inode (one per super block) and store content of formatted nodes
   into pages bound to this inode in the page cache. In newer kernels bread()
   already uses inode attached to block device (bd_inode). Advantage of having
   our own fake inode is that we can install appropriate methods in its
   address_space operations. Such methods are called by VM on memory pressure
   (or during background page flushing) and we can use them to react
   appropriately.

   In initial version we only support one block per page. Support for multiple
   blocks per page is complicated by relocation.

   To each page, used by reiser4, jnode is attached. jnode is analogous to
   buffer head. Difference is that jnode is bound to the page permanently:
   jnode cannot be removed from memory until its backing page is.

   jnode contain pointer to page (->pg field) and page contain pointer to
   jnode in ->private field. Pointer from jnode to page is protected to by
   jnode's spinlock and pointer from page to jnode is protected by page lock
   (PG_locked bit). Lock ordering is: first take page lock, then jnode spin
   lock. To go into reverse direction use jnode_lock_page() function that uses
   standard try-lock-and-release device.

   Properties:

   1. when jnode-to-page mapping is established (by jnode_attach_page()), page
   reference counter is increased.

   2. when jnode-to-page mapping is destroyed (by jnode_detach_page() and
   page_detach_jnode()), page reference counter is decreased.

   3. on jload() reference counter on jnode page is increased, page is
   kmapped and `referenced'.

   4. on jrelse() inverse operations are performed.

   5. kmapping/kunmapping of unformatted pages is done by read/write methods.

   DEADLOCKS RELATED TO MEMORY PRESSURE. [OUTDATED. Only interesting
   historically.]

   [In the following discussion, `lock' invariably means long term lock on
   znode.] (What about page locks?)

   There is some special class of deadlock possibilities related to memory
   pressure. Locks acquired by other reiser4 threads are accounted for in
   deadlock prevention mechanism (lock.c), but when ->vm_writeback() is
   invoked additional hidden arc is added to the locking graph: thread that
   tries to allocate memory waits for ->vm_writeback() to finish. If this
   thread keeps lock and ->vm_writeback() tries to acquire this lock, deadlock
   prevention is useless.

   Another related problem is possibility for ->vm_writeback() to run out of
   memory itself. This is not a problem for ext2 and friends, because their
   ->vm_writeback() don't allocate much memory, but reiser4 flush is
   definitely able to allocate huge amounts of memory.

   It seems that there is no reliable way to cope with the problems above. In
   stead it was decided that ->vm_writeback() (as invoked in the kswapd
   context) wouldn't perform any flushing itself, but rather should just wake
   up some auxiliary thread dedicated for this purpose (or, the same thread
   that does periodic commit of old atoms (ktxnmgrd.c)).

   Details:

   1. Page is called `reclaimable' against particular reiser4 mount F if this
   page can be ultimately released by try_to_free_pages() under presumptions
   that:

    a. ->vm_writeback() for F is no-op, and

    b. none of the threads accessing F are making any progress, and

    c. other reiser4 mounts obey the same memory reservation protocol as F
    (described below).

   For example, clean un-pinned page, or page occupied by ext2 data are
   reclaimable against any reiser4 mount.

   When there is more than one reiser4 mount in a system, condition (c) makes
   reclaim-ability not easily verifiable beyond trivial cases mentioned above.

   THIS COMMENT IS VALID FOR "MANY BLOCKS ON PAGE" CASE

   Fake inode is used to bound formatted nodes and each node is indexed within
   fake inode by its block number. If block size of smaller than page size, it
   may so happen that block mapped to the page with formatted node is occupied
   by unformatted node or is unallocated. This lead to some complications,
   because flushing whole page can lead to an incorrect overwrite of
   unformatted node that is moreover, can be cached in some other place as
   part of the file body. To avoid this, buffers for unformatted nodes are
   never marked dirty. Also pages in the fake are never marked dirty. This
   rules out usage of ->writepage() as memory pressure hook. In stead
   ->releasepage() is used.

   Josh is concerned that page->buffer is going to die. This should not pose
   significant problem though, because we need to add some data structures to
   the page anyway (jnode) and all necessary book keeping can be put there.

*/

/* Life cycle of pages/nodes.

   jnode contains reference to page and page contains reference back to
   jnode. This reference is counted in page ->count. Thus, page bound to jnode
   cannot be released back into free pool.

    1. Formatted nodes.

      1. formatted node is represented by znode. When new znode is created its
      ->pg pointer is NULL initially.

      2. when node content is loaded into znode (by call to zload()) for the
      first time following happens (in call to ->read_node() or
      ->allocate_node()):

        1. new page is added to the page cache.

        2. this page is attached to znode and its ->count is increased.

        3. page is kmapped.

      3. if more calls to zload() follow (without corresponding zrelses), page
      counter is left intact and in its stead ->d_count is increased in znode.

      4. each call to zrelse decreases ->d_count. When ->d_count drops to zero
      ->release_node() is called and page is kunmapped as result.

      5. at some moment node can be captured by a transaction. Its ->x_count
      is then increased by transaction manager.

      6. if node is removed from the tree (empty node with JNODE_HEARD_BANSHEE
      bit set) following will happen (also see comment at the top of znode.c):

        1. when last lock is released, node will be uncaptured from
        transaction. This released reference that transaction manager acquired
        at the step 5.

        2. when last reference is released, zput() detects that node is
        actually deleted and calls ->delete_node()
        operation. page_cache_delete_node() implementation detaches jnode from
        page and releases page.

      7. otherwise (node wasn't removed from the tree), last reference to
      znode will be released after transaction manager committed transaction
      node was in. This implies squallocing of this node (see
      flush.c). Nothing special happens at this point. Znode is still in the
      hash table and page is still attached to it.

      8. znode is actually removed from the memory because of the memory
      pressure, or during umount (znodes_tree_done()). Anyway, znode is
      removed by the call to zdrop(). At this moment, page is detached from
      znode and removed from the inode address space.

*/

#include "debug.h"
#include "dformat.h"
#include "key.h"
#include "txnmgr.h"
#include "jnode.h"
#include "znode.h"
#include "block_alloc.h"
#include "tree.h"
#include "vfs_ops.h"
#include "inode.h"
#include "super.h"
#include "entd.h"
#include "page_cache.h"
#include "ktxnmgrd.h"

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/mm.h>		/* for struct page */
#include <linux/swap.h>		/* for struct page */
#include <linux/pagemap.h>
#include <linux/bio.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>

static struct bio *page_bio(struct page *, jnode *, int rw, unsigned int gfp);

static struct address_space_operations formatted_fake_as_ops;

static const oid_t fake_ino = 0x1;
static const oid_t bitmap_ino = 0x2;
static const oid_t cc_ino = 0x3;

/* one-time initialization of fake inodes handling functions. */
int init_fakes(void)
{
	return 0;
}

static void
init_fake_inode(struct super_block *super, struct inode *fake,
		struct inode **pfake)
{
	assert("nikita-2168", fake->i_state & I_NEW);
	fake->i_mapping->a_ops = &formatted_fake_as_ops;
	fake->i_blkbits = super->s_blocksize_bits;
	fake->i_size = ~0ull;
	fake->i_rdev = super->s_bdev->bd_dev;
	fake->i_bdev = super->s_bdev;
	*pfake = fake;
	/* NOTE-NIKITA something else? */
	unlock_new_inode(fake);
}

/* initialize fake inode to which formatted nodes are bound in the page cache. */
int init_formatted_fake(struct super_block *super)
{
	struct inode *fake;
	struct inode *bitmap;
	struct inode *cc;
	reiser4_super_info_data *sinfo;

	assert("nikita-1703", super != NULL);

	sinfo = get_super_private_nocheck(super);
	fake = iget_locked(super, oid_to_ino(fake_ino));

	if (fake != NULL) {
		init_fake_inode(super, fake, &sinfo->fake);

		bitmap = iget_locked(super, oid_to_ino(bitmap_ino));
		if (bitmap != NULL) {
			init_fake_inode(super, bitmap, &sinfo->bitmap);

			cc = iget_locked(super, oid_to_ino(cc_ino));
			if (cc != NULL) {
				init_fake_inode(super, cc, &sinfo->cc);
				return 0;
			} else {
				iput(sinfo->fake);
				iput(sinfo->bitmap);
				sinfo->fake = NULL;
				sinfo->bitmap = NULL;
			}
		} else {
			iput(sinfo->fake);
			sinfo->fake = NULL;
		}
	}
	return RETERR(-ENOMEM);
}

/* release fake inode for @super */
int done_formatted_fake(struct super_block *super)
{
	reiser4_super_info_data *sinfo;

	sinfo = get_super_private_nocheck(super);

	if (sinfo->fake != NULL) {
		assert("vs-1426", sinfo->fake->i_data.nrpages == 0);
		iput(sinfo->fake);
		sinfo->fake = NULL;
	}

	if (sinfo->bitmap != NULL) {
		iput(sinfo->bitmap);
		sinfo->bitmap = NULL;
	}

	if (sinfo->cc != NULL) {
		iput(sinfo->cc);
		sinfo->cc = NULL;
	}
	return 0;
}

void reiser4_wait_page_writeback(struct page *page)
{
	assert("zam-783", PageLocked(page));

	do {
		unlock_page(page);
		wait_on_page_writeback(page);
		lock_page(page);
	} while (PageWriteback(page));
}

/* return tree @page is in */
reiser4_tree *tree_by_page(const struct page *page /* page to query */ )
{
	assert("nikita-2461", page != NULL);
	return &get_super_private(page->mapping->host->i_sb)->tree;
}

/* completion handler for single page bio-based read.

   mpage_end_io_read() would also do. But it's static.

*/
static int
end_bio_single_page_read(struct bio *bio, unsigned int bytes_done UNUSED_ARG,
			 int err UNUSED_ARG)
{
	struct page *page;

	if (bio->bi_size != 0) {
		warning("nikita-3332", "Truncated single page read: %i",
			bio->bi_size);
		return 1;
	}

	page = bio->bi_io_vec[0].bv_page;

	if (test_bit(BIO_UPTODATE, &bio->bi_flags)) {
		SetPageUptodate(page);
	} else {
		ClearPageUptodate(page);
		SetPageError(page);
	}
	unlock_page(page);
	bio_put(bio);
	return 0;
}

/* completion handler for single page bio-based write.

   mpage_end_io_write() would also do. But it's static.

*/
static int
end_bio_single_page_write(struct bio *bio, unsigned int bytes_done UNUSED_ARG,
			  int err UNUSED_ARG)
{
	struct page *page;

	if (bio->bi_size != 0) {
		warning("nikita-3333", "Truncated single page write: %i",
			bio->bi_size);
		return 1;
	}

	page = bio->bi_io_vec[0].bv_page;

	if (!test_bit(BIO_UPTODATE, &bio->bi_flags))
		SetPageError(page);
	end_page_writeback(page);
	bio_put(bio);
	return 0;
}

/* ->readpage() method for formatted nodes */
static int
formatted_readpage(struct file *f UNUSED_ARG,
		   struct page *page /* page to read */ )
{
	assert("nikita-2412", PagePrivate(page) && jprivate(page));
	return page_io(page, jprivate(page), READ, GFP_KERNEL);
}

/**
 * page_io - submit single-page bio request
 * @page: page to perform io for
 * @node: jnode of page
 * @rw: read or write
 * @gfp: gfp mask for bio allocation
 *
 * Submits single page read or write.
 */
int page_io(struct page *page, jnode *node, int rw, int gfp)
{
	struct bio *bio;
	int result;

	assert("nikita-2094", page != NULL);
	assert("nikita-2226", PageLocked(page));
	assert("nikita-2634", node != NULL);
	assert("nikita-2893", rw == READ || rw == WRITE);

	if (rw) {
		if (unlikely(page->mapping->host->i_sb->s_flags & MS_RDONLY)) {
			unlock_page(page);
			return 0;
		}
	}

	bio = page_bio(page, node, rw, gfp);
	if (!IS_ERR(bio)) {
		if (rw == WRITE) {
			SetPageWriteback(page);
			unlock_page(page);
		}
		reiser4_submit_bio(rw, bio);
		result = 0;
	} else {
		unlock_page(page);
		result = PTR_ERR(bio);
	}

	return result;
}

/* helper function to construct bio for page */
static struct bio *page_bio(struct page *page, jnode * node, int rw,
			    unsigned int gfp)
{
	struct bio *bio;
	assert("nikita-2092", page != NULL);
	assert("nikita-2633", node != NULL);

	/* Simple implementation in the assumption that blocksize == pagesize.

	   We only have to submit one block, but submit_bh() will allocate bio
	   anyway, so lets use all the bells-and-whistles of bio code.
	 */

	bio = bio_alloc(gfp, 1);
	if (bio != NULL) {
		int blksz;
		struct super_block *super;
		reiser4_block_nr blocknr;

		super = page->mapping->host->i_sb;
		assert("nikita-2029", super != NULL);
		blksz = super->s_blocksize;
		assert("nikita-2028", blksz == (int)PAGE_CACHE_SIZE);

		blocknr = *UNDER_SPIN(jnode, node, jnode_get_io_block(node));

		assert("nikita-2275", blocknr != (reiser4_block_nr) 0);
		assert("nikita-2276", !blocknr_is_fake(&blocknr));

		bio->bi_bdev = super->s_bdev;
		/* fill bio->bi_sector before calling bio_add_page(), because
		 * q->merge_bvec_fn may want to inspect it (see
		 * drivers/md/linear.c:linear_mergeable_bvec() for example. */
		bio->bi_sector = blocknr * (blksz >> 9);

		if (!bio_add_page(bio, page, blksz, 0)) {
			warning("nikita-3452",
				"Single page bio cannot be constructed");
			return ERR_PTR(RETERR(-EINVAL));
		}

		/* bio -> bi_idx is filled by bio_init() */
		bio->bi_end_io = (rw == READ) ?
		    end_bio_single_page_read : end_bio_single_page_write;

		return bio;
	} else
		return ERR_PTR(RETERR(-ENOMEM));
}

/* this function is internally called by jnode_make_dirty() */
int set_page_dirty_internal(struct page *page, int tag_as_moved)
{
	struct address_space *mapping;

	mapping = page->mapping;
	BUG_ON(mapping == NULL);

	if (!TestSetPageDirty(page)) {
		if (mapping_cap_account_dirty(mapping))
			inc_page_state(nr_dirty);

		__mark_inode_dirty(mapping->host, I_DIRTY_PAGES);
	}
	return 0;
}

static int can_hit_entd(reiser4_context * ctx, struct super_block *s)
{
	if (get_super_private(s)->entd.tsk == current)
		return 0;
	if (ctx == NULL || ((unsigned long)ctx->magic) != context_magic)
		return 1;
	if (ctx->super != s)
		return 1;
	return 0;
}

/**
 * reiser4_writepage -
 * @page: page to write
 * @wbc:
 */
/* Common memory pressure notification. */
int reiser4_writepage(struct page *page /* page to start writeback from */ ,
		      struct writeback_control *wbc)
{
	struct super_block *s;
	reiser4_context *ctx;
	reiser4_tree *tree;
	txn_atom *atom;
	jnode *node;
	int result;

	assert("vs-828", PageLocked(page));

	s = page->mapping->host->i_sb;
	ctx = get_current_context_check();

#if REISER4_USE_ENTD
	if (can_hit_entd(ctx, s) ||
	    (ctx && lock_stack_isclean(get_current_lock_stack()) &&
	     ctx->trans->atom == NULL && ctx->entd == 0)) {
		/* Throttle memory allocations if we were not in reiser4 or if
		   lock stack is clean and atom is not opened */
		return write_page_by_ent(page, wbc);
	}
#endif				/* REISER4_USE_ENTD */

	BUG_ON(ctx == NULL);
	BUG_ON(s != ctx->super);

	tree = &get_super_private(s)->tree;
	node = jnode_of_page(page);
	if (!IS_ERR(node)) {
		int phantom;

		assert("nikita-2419", node != NULL);

		LOCK_JNODE(node);
		/*
		 * page was dirty, but jnode is not. This is (only?)
		 * possible if page was modified through mmap(). We
		 * want to handle such jnodes specially.
		 */
		phantom = !jnode_is_dirty(node);
		atom = jnode_get_atom(node);
		if (atom != NULL) {
			if (!(atom->flags & ATOM_FORCE_COMMIT)) {
				atom->flags |= ATOM_FORCE_COMMIT;
				ktxnmgrd_kick(&get_super_private(s)->tmgr);
			}
			UNLOCK_ATOM(atom);
		}
		UNLOCK_JNODE(node);

		result = emergency_flush(page);
		if (result == 0)
			if (phantom && jnode_is_unformatted(node))
				JF_SET(node, JNODE_KEEPME);
		jput(node);
	} else {
		result = PTR_ERR(node);
	}
	if (result != 0) {
		/*
		 * shrink list doesn't move page to another mapping
		 * list when clearing dirty flag. So it is enough to
		 * just set dirty bit.
		 */
		set_page_dirty_internal(page, 0);
		unlock_page(page);
	}
	return result;
}

/* ->set_page_dirty() method of formatted address_space */
static int formatted_set_page_dirty(struct page *page)
{
	assert("nikita-2173", page != NULL);
	return __set_page_dirty_nobuffers(page);
}

/* writepages method of address space operations in reiser4 is used to involve
   into transactions pages which are dirtied via mmap. Only regular files can
   have such pages. Fake inode is used to access formatted nodes via page
   cache. As formatted nodes can never be mmaped, fake inode's writepages has
   nothing to do */
static int
writepages_fake(struct address_space *mapping, struct writeback_control *wbc)
{
	return 0;
}

/* address space operations for the fake inode */
static struct address_space_operations formatted_fake_as_ops = {
	/* Perform a writeback of a single page as a memory-freeing
	 * operation. */
	.writepage = reiser4_writepage,
	/* this is called to read formatted node */
	.readpage = formatted_readpage,
	/* ->sync_page() method of fake inode address space operations. Called
	   from wait_on_page() and lock_page().

	   This is most annoyingly misnomered method. Actually it is called
	   from wait_on_page_bit() and lock_page() and its purpose is to
	   actually start io by jabbing device drivers.
	 */
	.sync_page = block_sync_page,
	/* Write back some dirty pages from this mapping. Called from sync.
	   called during sync (pdflush) */
	.writepages = writepages_fake,
	/* Set a page dirty */
	.set_page_dirty = formatted_set_page_dirty,
	/* used for read-ahead. Not applicable */
	.readpages = NULL,
	.prepare_write = NULL,
	.commit_write = NULL,
	.bmap = NULL,
	/* called just before page is being detached from inode mapping and
	   removed from memory. Called on truncate, cut/squeeze, and
	   umount. */
	.invalidatepage = reiser4_invalidatepage,
	/* this is called by shrink_cache() so that file system can try to
	   release objects (jnodes, buffers, journal heads) attached to page
	   and, may be made page itself free-able.
	 */
	.releasepage = reiser4_releasepage,
	.direct_IO = NULL
};

/* called just before page is released (no longer used by reiser4). Callers:
   jdelete() and extent2tail(). */
void drop_page(struct page *page)
{
	assert("nikita-2181", PageLocked(page));
	clear_page_dirty(page);
	ClearPageUptodate(page);
#if defined(PG_skipped)
	ClearPageSkipped(page);
#endif
	if (page->mapping != NULL) {
		remove_from_page_cache(page);
		unlock_page(page);
		page_cache_release(page);
	} else
		unlock_page(page);
}

/* this is called by truncate_jnodes_range which in its turn is always called
   after truncate_mapping_pages_range. Therefore, here jnode can not have
   page. New pages can not be created because truncate_jnodes_range goes under
   exclusive access on file obtained, where as new page creation requires
   non-exclusive access obtained */
static void invalidate_unformatted(jnode * node)
{
	struct page *page;

	LOCK_JNODE(node);
	page = node->pg;
	if (page) {
		loff_t from, to;

		page_cache_get(page);
		UNLOCK_JNODE(node);
		/* FIXME: use truncate_complete_page instead */
		from = (loff_t) page->index << PAGE_CACHE_SHIFT;
		to = from + PAGE_CACHE_SIZE - 1;
		truncate_inode_pages_range(page->mapping, from, to);
		page_cache_release(page);
	} else {
		JF_SET(node, JNODE_HEARD_BANSHEE);
		uncapture_jnode(node);
		unhash_unformatted_jnode(node);
	}
}

#define JNODE_GANG_SIZE (16)

/* find all eflushed jnodes from range specified and invalidate them */
static int
truncate_jnodes_range(struct inode *inode, pgoff_t from, pgoff_t count)
{
	reiser4_inode *info;
	int truncated_jnodes;
	reiser4_tree *tree;
	unsigned long index;
	unsigned long end;

	truncated_jnodes = 0;

	info = reiser4_inode_data(inode);
	tree = tree_by_inode(inode);

	index = from;
	end = from + count;

	while (1) {
		jnode *gang[JNODE_GANG_SIZE];
		int taken;
		int i;
		jnode *node;

		assert("nikita-3466", index <= end);

		RLOCK_TREE(tree);
		taken =
		    radix_tree_gang_lookup(jnode_tree_by_reiser4_inode(info),
					   (void **)gang, index,
					   JNODE_GANG_SIZE);
		for (i = 0; i < taken; ++i) {
			node = gang[i];
			if (index_jnode(node) < end)
				jref(node);
			else
				gang[i] = NULL;
		}
		RUNLOCK_TREE(tree);

		for (i = 0; i < taken; ++i) {
			node = gang[i];
			if (node != NULL) {
				index = max(index, index_jnode(node));
				invalidate_unformatted(node);
				truncated_jnodes++;
				jput(node);
			} else
				break;
		}
		if (i != taken || taken == 0)
			break;
	}
	return truncated_jnodes;
}

void
reiser4_invalidate_pages(struct address_space *mapping, pgoff_t from,
			 unsigned long count, int even_cows)
{
	loff_t from_bytes, count_bytes;

	if (count == 0)
		return;
	from_bytes = ((loff_t) from) << PAGE_CACHE_SHIFT;
	count_bytes = ((loff_t) count) << PAGE_CACHE_SHIFT;

	unmap_mapping_range(mapping, from_bytes, count_bytes, even_cows);
	truncate_inode_pages_range(mapping, from_bytes,
				   from_bytes + count_bytes - 1);
	truncate_jnodes_range(mapping->host, from, count);
}

#if REISER4_DEBUG

#define page_flag_name( page, flag )			\
	( test_bit( ( flag ), &( page ) -> flags ) ? ((#flag "|")+3) : "" )

void print_page(const char *prefix, struct page *page)
{
	if (page == NULL) {
		printk("null page\n");
		return;
	}
	printk("%s: page index: %lu mapping: %p count: %i private: %lx\n",
	       prefix, page->index, page->mapping, page_count(page),
	       page->private);
	printk("\tflags: %s%s%s%s %s%s%s %s%s%s %s%s\n",
	       page_flag_name(page, PG_locked), page_flag_name(page, PG_error),
	       page_flag_name(page, PG_referenced), page_flag_name(page,
								   PG_uptodate),
	       page_flag_name(page, PG_dirty), page_flag_name(page, PG_lru),
	       page_flag_name(page, PG_slab), page_flag_name(page, PG_checked),
	       page_flag_name(page, PG_reserved), page_flag_name(page,
								 PG_private),
	       page_flag_name(page, PG_writeback), page_flag_name(page,
								  PG_nosave));
	if (jprivate(page) != NULL) {
		print_jnode("\tpage jnode", jprivate(page));
		printk("\n");
	}
}

#endif


/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   scroll-step: 1
   End:
*/
