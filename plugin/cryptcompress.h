/* Copyright 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */
/* See http://www.namesys.com/cryptcompress_design.html */

#if !defined( __FS_REISER4_CRYPTCOMPRESS_H__ )
#define __FS_REISER4_CRYPTCOMPRESS_H__

#include "compress/compress.h"

#include <linux/pagemap.h>
#include <linux/crypto.h>
#include <linux/vmalloc.h>

#define MIN_CLUSTER_SIZE PAGE_CACHE_SIZE
#define MAX_CLUSTER_SHIFT 4
#define MAX_CLUSTER_NRPAGES (1 << MAX_CLUSTER_SHIFT)
#define DEFAULT_CLUSTER_SHIFT 0
#define MIN_SIZE_FOR_COMPRESSION 64
#define MIN_CRYPTO_BLOCKSIZE 8
#define CLUSTER_MAGIC_SIZE (MIN_CRYPTO_BLOCKSIZE >> 1)

//#define HANDLE_VIA_FLUSH_SCAN

/* cluster status */
typedef enum {
	DATA_CLUSTER = 0,
	HOLE_CLUSTER = 1, /* indicates hole for write ops */
	FAKE_CLUSTER = 2  /* indicates absence of disk cluster for read ops */
} reiser4_cluster_status;
/* EDWARD-FIXME-HANS: comment each transform below */
/* reiser4 transforms */
typedef enum {
	CRYPTO_TFM,
	DIGEST_TFM,
	COMPRESS_TFM,
	LAST_TFM
} reiser4_tfm;

typedef struct tfm_stream {
	__u8 * data;
	size_t size;
} tfm_stream_t;

typedef enum {
	INPUT_STREAM,
	OUTPUT_STREAM,
	LAST_STREAM
} tfm_stream_id;

typedef tfm_stream_t * tfm_unit[LAST_STREAM];

static inline __u8 *
ts_data(tfm_stream_t * stm)
{
	assert("edward-928", stm != NULL);
	return stm->data;
}

static inline size_t
ts_size(tfm_stream_t * stm)
{
	assert("edward-929", stm != NULL);
	return stm->size;
}

static inline void
set_ts_size(tfm_stream_t * stm, size_t size)
{
	assert("edward-930", stm != NULL);
	
	stm->size = size;
}

static inline int
alloc_ts(tfm_stream_t ** stm)
{
	assert("edward-931", stm);
	assert("edward-932", *stm == NULL);
	
	*stm = reiser4_kmalloc(sizeof ** stm, GFP_KERNEL);
	if (*stm == NULL)
		return -ENOMEM;
	xmemset(*stm, 0, sizeof ** stm);
	return 0;
}

static inline void
free_ts(tfm_stream_t * stm)
{
	assert("edward-933", !ts_data(stm));
	assert("edward-934", !ts_size(stm));

	reiser4_kfree(stm);
}

static inline int
alloc_ts_data(tfm_stream_t * stm, size_t size)
{
	assert("edward-935", !ts_data(stm));
	assert("edward-936", !ts_size(stm));
	assert("edward-937", size != 0);

	stm->data = vmalloc(size);
	if (!stm->data)
		return -ENOMEM;
	set_ts_size(stm, size);
	return 0;
}

static inline void
free_ts_data(tfm_stream_t * stm)
{
	assert("edward-938", equi(ts_data(stm), ts_size(stm)));
	
	if (ts_data(stm))
		vfree(ts_data(stm));
	memset(stm, 0, sizeof *stm);
}

/* Write modes for item conversion in flush squeeze phase */
typedef enum {
	CRC_FIRST_ITEM = 1,
	CRC_APPEND_ITEM = 2,
	CRC_OVERWRITE_ITEM = 3,
	CRC_CUT_ITEM = 4
} crc_write_mode_t;

typedef struct tfm_cluster{
	coa_set coa;
	tfm_unit tun;
	int uptodate;
	int len;
} tfm_cluster_t; 

static inline coa_t 
get_coa(tfm_cluster_t * tc, reiser4_compression_id id)
{
	return tc->coa[id];
}

static inline void
set_coa(tfm_cluster_t * tc, reiser4_compression_id id, coa_t coa)
{
	tc->coa[id] = coa;
}

static inline int
alloc_coa(tfm_cluster_t * tc, compression_plugin * cplug, tfm_action act)
{
	coa_t coa;

	coa = cplug->alloc(act);
	if (IS_ERR(coa))
		return PTR_ERR(coa);
	set_coa(tc, cplug->h.id, coa);
	return 0;
}

static inline void
free_coa_set(tfm_cluster_t * tc, tfm_action act)
{
	reiser4_compression_id i;
	compression_plugin * cplug;
	
	assert("edward-810", tc != NULL);
	
	for(i = 0; i < LAST_COMPRESSION_ID; i++) {
		if (!get_coa(tc, i))
			continue;
		cplug = compression_plugin_by_id(i);
		assert("edward-812", cplug->free != NULL);
		cplug->free(get_coa(tc, i), act);
		set_coa(tc, i, 0);
	}
	return;
}

static inline tfm_stream_t *
tfm_stream (tfm_cluster_t * tc, tfm_stream_id id)
{
	return tc->tun[id];
}

static inline void
set_tfm_stream (tfm_cluster_t * tc, tfm_stream_id id, tfm_stream_t * ts)
{
	tc->tun[id] = ts;
}

static inline __u8 *
tfm_stream_data (tfm_cluster_t * tc, tfm_stream_id id)
{
	return ts_data(tfm_stream(tc, id));
}

static inline void
set_tfm_stream_data(tfm_cluster_t * tc, tfm_stream_id id, __u8 * data)
{
	tfm_stream(tc, id)->data = data;
}

static inline size_t
tfm_stream_size (tfm_cluster_t * tc, tfm_stream_id id)
{
	return ts_size(tfm_stream(tc, id));
}

static inline void
set_tfm_stream_size(tfm_cluster_t * tc, tfm_stream_id id, size_t size)
{
	tfm_stream(tc, id)->size = size;
}

static inline int
alloc_tfm_stream(tfm_cluster_t * tc, size_t size, tfm_stream_id id)
{
	assert("edward-939", tc != NULL);
	assert("edward-940", !tfm_stream(tc, id));
	
	tc->tun[id] = reiser4_kmalloc(sizeof(tfm_stream_t), GFP_KERNEL);
	if (!tc->tun[id])
		return -ENOMEM;
	xmemset(tfm_stream(tc, id), 0, sizeof(tfm_stream_t));
	return alloc_ts_data(tfm_stream(tc, id), size);
}

static inline int
realloc_tfm_stream(tfm_cluster_t * tc, size_t size, tfm_stream_id id)
{
	assert("edward-941", tfm_stream_size(tc, id) < size);
	free_ts_data(tfm_stream(tc, id));
	return alloc_ts_data(tfm_stream(tc, id), size);
}

static inline void
free_tfm_stream(tfm_cluster_t * tc, tfm_stream_id id)
{
	free_ts_data(tfm_stream(tc, id));
	free_ts(tfm_stream(tc, id));
	set_tfm_stream(tc, id, 0);
}

static inline void
free_tfm_unit(tfm_cluster_t * tc)
{
	tfm_stream_id id;
	for (id = 0; id < LAST_STREAM; id++) {
		if (!tfm_stream(tc, id))
			continue;
		free_tfm_stream(tc, id);
	}
}

static inline void
put_tfm_cluster(tfm_cluster_t * tc, tfm_action act)
{
	assert("edward-942", tc != NULL);
	free_coa_set(tc, act);
	free_tfm_unit(tc);
}

static inline int
tfm_cluster_is_uptodate (tfm_cluster_t * tc)
{
	assert("edward-943", tc != NULL);
	assert("edward-944", tc->uptodate == 0 || tc->uptodate == 1);
	return (tc->uptodate == 1);
}

static inline void
tfm_cluster_set_uptodate (tfm_cluster_t * tc)
{
	assert("edward-945", tc != NULL);
	assert("edward-946", tc->uptodate == 0 || tc->uptodate == 1);
	tc->uptodate = 1;
	return;
}

static inline void
tfm_cluster_clr_uptodate (tfm_cluster_t * tc)
{
	assert("edward-947", tc != NULL);
	assert("edward-948", tc->uptodate == 0 || tc->uptodate == 1);
	tc->uptodate = 0;
	return;
}

static inline int
tfm_stream_is_set(tfm_cluster_t * tc, tfm_stream_id id)
{
	return (tfm_stream(tc, id) && 
		tfm_stream_data(tc, id) &&
		tfm_stream_size(tc, id));
}

static inline int
tfm_cluster_is_set(tfm_cluster_t * tc)
{
	int i;
	for (i = 0; i < LAST_STREAM; i++)
		if (!tfm_stream_is_set(tc, i))
			return 0;
	return 1;
}

static inline void
alternate_streams(tfm_cluster_t * tc)
{
	tfm_stream_t * tmp = tfm_stream(tc, INPUT_STREAM);
	
	set_tfm_stream(tc, INPUT_STREAM, tfm_stream(tc, OUTPUT_STREAM));
	set_tfm_stream(tc, OUTPUT_STREAM, tmp);
}

/* reiser4 cluster manager transforms page cluster into disk cluster (and back) via
   input/output stream of crypto/compression algorithms using copy on clustering.
   COC means that page cluster will be assembled into united stream before compression,

EDWARD-FIXME-HANS: discuss with vs whether coc is already taken as an acronym, and resolve the issue

   and output stream of decompression algorithm will be split into pages.
   This manager consists mostly of operations on the following object which represents
   one cluster:
*/
typedef struct reiser4_cluster{
	tfm_cluster_t tc; /* transform cluster */
	int nr_pages;    /* number of attached pages */
	struct page ** pages; /* page cluster */
	struct file * file;
	hint_t * hint;        /* disk cluster */
	reiser4_cluster_status stat;
	/* sliding frame of cluster size in loff_t-space to translate main file 'offsets'
	   like read/write position, size, new size (for truncate), etc.. into number
	   of pages, cluster status, etc..*/
	unsigned long index; /* cluster index, coord of the frame */
	unsigned off;    /* offset we want to read/write/truncate from */
	unsigned count;  /* bytes to read/write/truncate */
	unsigned delta;  /* bytes of user's data to append to the hole */
	int reserved;    /* indicates that space for disk cluster insertion is reserved */
} reiser4_cluster_t;

static inline int
alloc_page_cluster(reiser4_cluster_t * clust, int nrpages)
{
	assert("edward-949", clust != NULL);
	assert("edward-950", nrpages != 0 && nrpages <= MAX_CLUSTER_NRPAGES);
	
	clust->pages = reiser4_kmalloc(sizeof(*clust->pages) * nrpages, GFP_KERNEL);
	if (!clust->pages)
		return -ENOMEM;
	xmemset(clust->pages, 0, sizeof(*clust->pages) * nrpages);
	return 0;
}

static inline void
free_page_cluster(reiser4_cluster_t * clust)
{
	assert("edward-951", clust->pages != NULL);
	reiser4_kfree(clust->pages);
}

static inline void
put_cluster_handle(reiser4_cluster_t * clust, tfm_action act)
{
	assert("edward-435", clust != NULL);
	
	put_tfm_cluster(&clust->tc, act);
	if (clust->pages)
		free_page_cluster(clust);
	xmemset(clust, 0, sizeof *clust);
}

/* security attributes supposed to be stored on disk
   are loaded by stat-data methods (see plugin/item/static_stat.c */
typedef struct crypto_stat {
	__u8 * keyid;  /* pointer to a fingerprint */
	__u16 keysize; /* key size, bits */
} crypto_stat_t;

/* cryptcompress specific part of reiser4_inode */
typedef struct cryptcompress_info {
	struct crypto_tfm *tfm[LAST_TFM];
	__u32 * expkey;
} cryptcompress_info_t;

cryptcompress_info_t *cryptcompress_inode_data(const struct inode * inode);
int equal_to_rdk(znode *, const reiser4_key *);
int equal_to_ldk(znode *, const reiser4_key *);
int goto_right_neighbor(coord_t *, lock_handle *);
int load_file_hint(struct file *, hint_t *);
void save_file_hint(struct file *, const hint_t *);

/* declarations of functions implementing methods of cryptcompress object plugin */
void init_inode_data_cryptcompress(struct inode *inode, reiser4_object_create_data *crd, int create);
int create_cryptcompress(struct inode *, struct inode *, reiser4_object_create_data *);
int open_cryptcompress(struct inode * inode, struct file * file);
int truncate_cryptcompress(struct inode *, loff_t size);
int readpage_cryptcompress(void *, struct page *);
int capture_cryptcompress(struct inode *inode, const struct writeback_control *wbc, long *);
ssize_t write_cryptcompress(struct file *, const char *buf, size_t size, loff_t *off);
int release_cryptcompress(struct inode *inode, struct file *);
int mmap_cryptcompress(struct file *, struct vm_area_struct *vma);
int get_block_cryptcompress(struct inode *, sector_t block, struct buffer_head *bh_result, int create);
int flow_by_inode_cryptcompress(struct inode *, char *buf, int user, loff_t, loff_t, rw_op, flow_t *);
int key_by_inode_cryptcompress(struct inode *, loff_t off, reiser4_key *);
int delete_cryptcompress(struct inode *);
int owns_item_cryptcompress(const struct inode *, const coord_t *);
int setattr_cryptcompress(struct inode *, struct iattr *);
void readpages_cryptcompress(struct file *, struct address_space *, struct list_head *pages);
void init_inode_data_cryptcompress(struct inode *, reiser4_object_create_data *, int create);
int pre_delete_cryptcompress(struct inode *);
void hint_init_zero(hint_t *);
void destroy_inode_cryptcompress(struct inode * inode);
int crc_inode_ok(struct inode * inode);

static inline struct crypto_tfm *
inode_get_tfm (struct inode * inode, reiser4_tfm tfm)
{
	return cryptcompress_inode_data(inode)->tfm[tfm];
}

static inline struct crypto_tfm *
inode_get_crypto (struct inode * inode)
{
	return (inode_get_tfm(inode, CRYPTO_TFM));
}

static inline struct crypto_tfm *
inode_get_digest (struct inode * inode)
{
	return (inode_get_tfm(inode, DIGEST_TFM));
}

static inline unsigned int
crypto_blocksize(struct inode * inode)
{
	assert("edward-758", inode_get_tfm(inode, CRYPTO_TFM) != NULL);
	return crypto_tfm_alg_blocksize(inode_get_tfm(inode, CRYPTO_TFM));
}

#define REGISTER_NONE_ALG(ALG, TFM)                                  \
static int alloc_none_ ## ALG (struct inode * inode)                 \
{                                                                    \
        cryptcompress_info_t * info;                                 \
        assert("edward-760", inode != NULL);                         \
	                                                             \
	info = cryptcompress_inode_data(inode);                      \
                                                                     \
                                                                     \
	cryptcompress_inode_data(inode)->tfm[TFM ## _TFM] = NULL;    \
	return 0;                                                    \
                                                                     \
}                                                                    \
static void free_none_ ## ALG (struct inode * inode)                 \
{                                                                    \
        cryptcompress_info_t * info;                                 \
        assert("edward-761", inode != NULL);                         \
	                                                             \
	info = cryptcompress_inode_data(inode);                      \
	                                                             \
	assert("edward-762", info != NULL);                          \
	                                                             \
	info->tfm[TFM ## _TFM] = NULL;                               \
}

#endif /* __FS_REISER4_CRYPTCOMPRESS_H__ */

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
