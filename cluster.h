/* Copyright 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* This file contains page/cluster index translators and offset modulators
   See http://www.namesys.com/cryptcompress_design.html for details */

#if !defined( __FS_REISER4_CLUSTER_H__ )
#define __FS_REISER4_CLUSTER_H__

static inline loff_t min_count(loff_t a, loff_t b)
{
	return (a < b ? a : b);
}

static inline loff_t max_count(loff_t a, loff_t b)
{
	return (a > b ? a : b);
}

static inline __u8 inode_cluster_shift (struct inode * inode)
{
	assert("edward-92", inode != NULL);
	assert("edward-93", reiser4_inode_data(inode) != NULL);
	assert("edward-94", inode_get_flag(inode, REISER4_CLUSTER_KNOWN));

	return reiser4_inode_data(inode)->cluster_shift;
}

/* returns number of pages in the cluster */
static inline int inode_cluster_pages (struct inode * inode)
{
	return (1 << inode_cluster_shift(inode));
}

static inline size_t inode_cluster_size (struct inode * inode)
{
	assert("edward-96", inode != NULL);

	return (PAGE_CACHE_SIZE << inode_cluster_shift(inode));
}

static inline unsigned long
pg_to_clust(unsigned long idx, struct inode * inode)
{
	return idx >> inode_cluster_shift(inode);
}

static inline unsigned long
clust_to_pg(unsigned long idx, struct inode * inode)
{
	return idx << inode_cluster_shift(inode);
}

static inline unsigned long
pg_to_clust_to_pg(unsigned long idx, struct inode * inode)
{
	return clust_to_pg(pg_to_clust(idx, inode), inode);
}

static inline unsigned long
off_to_pg(loff_t off)
{
	return (off >> PAGE_CACHE_SHIFT);
}

static inline loff_t
pg_to_off(unsigned long idx)
{
	return ((loff_t)(idx) << PAGE_CACHE_SHIFT);
}

static inline unsigned long
off_to_clust(loff_t off, struct inode * inode)
{
	return pg_to_clust(off_to_pg(off), inode);
}

static inline loff_t
clust_to_off(unsigned long idx, struct inode * inode)
{
	return pg_to_off(clust_to_pg(idx, inode));
}

static inline loff_t
off_to_clust_to_off(loff_t off, struct inode * inode)
{
	return clust_to_off(off_to_clust(off, inode), inode);
}

static inline unsigned long
off_to_clust_to_pg(loff_t off, struct inode * inode)
{
	return clust_to_pg(off_to_clust(off, inode), inode);
}

static inline unsigned
off_to_pgoff(loff_t off)
{
	return off & (PAGE_CACHE_SIZE - 1);
}

static inline unsigned
off_to_cloff(loff_t off, struct inode * inode)
{
	return off & ((loff_t)(inode_cluster_size(inode)) - 1);
}

static inline unsigned
pg_to_off_to_cloff(unsigned long idx, struct inode * inode)
{
	return off_to_cloff(pg_to_off(idx), inode);
}

/* if @size != 0, returns index of the page
   which contains the last byte of the file */
static inline pgoff_t
size_to_pg(loff_t size)
{
	return (size ? off_to_pg(size - 1) : 0);
}

/* minimal index of the page which doesn't contain
   file data */
static inline pgoff_t
size_to_next_pg(loff_t size)
{
	return (size ? off_to_pg(size - 1) + 1 : 0);
}

static inline unsigned
off_to_pgcount(loff_t off, unsigned long idx)
{
	if (idx > off_to_pg(off))
		return 0;
	if (idx < off_to_pg(off))
		return PAGE_CACHE_SIZE;
	return off_to_pgoff(off);
}

static inline unsigned
off_to_count(loff_t off, unsigned long idx, struct inode * inode)
{
	if (idx > off_to_clust(off, inode))
		return 0;
	if (idx < off_to_clust(off, inode))
		return inode_cluster_size(inode);
	return off_to_cloff(off, inode);
}

static inline unsigned
fsize_to_count(reiser4_cluster_t * clust, struct inode * inode)
{
	assert("edward-288", clust != NULL);
	assert("edward-289", inode != NULL);

	return off_to_count(inode->i_size, clust->index, inode);
}

static inline void
reiser4_cluster_init (reiser4_cluster_t * clust){
	assert("edward-84", clust != NULL);
	xmemset(clust, 0, sizeof *clust);
	clust->stat = DATA_CLUSTER;
}

int inflate_cluster(reiser4_cluster_t *, struct inode *);
int find_cluster_item(hint_t * hint, const reiser4_key *key, int check_key,
		      znode_lock_mode lock_mode, ra_info_t *ra_info,
		      lookup_bias bias);
int page_of_cluster(struct page *, reiser4_cluster_t *, struct inode *);
int find_cluster(reiser4_cluster_t *, struct inode *, int read, int write);
int flush_cluster_pages(reiser4_cluster_t *, jnode *, struct inode *);
int deflate_cluster(reiser4_cluster_t *, struct inode *);
void truncate_cluster(struct inode * inode, pgoff_t start, long count);
void set_hint_cluster(struct inode * inode, hint_t * hint, unsigned long index, znode_lock_mode mode);
int get_disk_cluster_locked(reiser4_cluster_t * clust, znode_lock_mode lock_mode);
int hint_prev_cluster(reiser4_cluster_t * clust);
void set_nrpages_by_inode(reiser4_cluster_t * clust, struct inode * inode);
int grab_cluster_pages(struct inode * inode, reiser4_cluster_t * clust);
void release_cluster_pages(reiser4_cluster_t * clust, int from);
void put_cluster_handle(reiser4_cluster_t * clust, tfm_action act);
int grab_tfm_stream(struct inode * inode, tfm_cluster_t * tc, tfm_stream_id id);
int tfm_cluster_is_uptodate (tfm_cluster_t * tc);
void tfm_cluster_set_uptodate (tfm_cluster_t * tc);
void tfm_cluster_clr_uptodate (tfm_cluster_t * tc);

static inline int
alloc_clust_pages(reiser4_cluster_t * clust, struct inode * inode )
{
	assert("edward-791", clust != NULL);
	assert("edward-792", inode != NULL);
	clust->pages = reiser4_kmalloc(sizeof(*clust->pages) << inode_cluster_shift(inode), GFP_KERNEL);
	if (!clust->pages)
		return -ENOMEM;
	return 0;
}

static inline void
free_clust_pages(reiser4_cluster_t * clust)
{
	reiser4_kfree(clust->pages);
}

#endif /* __FS_REISER4_CLUSTER_H__ */


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
