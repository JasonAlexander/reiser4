/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* ctails (aka "clustered tails") are items for cryptcompress objects */

/* DESCRIPTION:

Each cryptcompress object is stored on disk as a set of clusters sliced
into ctails.

Internal on-disk structure:

        HEADER   (1)  Here stored disk cluster shift
	BODY
*/

#include "../../forward.h"
#include "../../debug.h"
#include "../../dformat.h"
#include "../../kassign.h"
#include "../../key.h"
#include "../../coord.h"
#include "item.h"
#include "../node/node.h"
#include "../plugin.h"
#include "../object.h"
#include "../../znode.h"
#include "../../carry.h"
#include "../../tree.h"
#include "../../inode.h"
#include "../../super.h"
#include "../../context.h"
#include "../../page_cache.h"
#include "../../cluster.h"
#include "../../flush.h"
#include "../../tree_walk.h"
#include "../file/funcs.h"

#include <linux/pagevec.h>
#include <linux/swap.h>
#include <linux/fs.h>

/* return body of ctail item at @coord */
static ctail_item_format *
ctail_formatted_at(const coord_t * coord)
{
	assert("edward-60", coord != NULL);
	return item_body_by_coord(coord);
}

reiser4_internal int
cluster_shift_by_coord(const coord_t * coord)
{
	return d8tocpu(&ctail_formatted_at(coord)->cluster_shift);
}

static unsigned long
pg_by_coord(const coord_t * coord)
{
	reiser4_key  key;

	return get_key_offset(item_key_by_coord(coord, &key)) >> PAGE_CACHE_SHIFT;
}

static int
coord_is_unprepped_ctail(const coord_t * coord)
{
	assert("edward-1233", coord != NULL);
	assert("edward-1234", item_id_by_coord(coord) == CTAIL_ID);
	assert("edward-1235", 
	       ergo((int)cluster_shift_by_coord(coord) == (int)UCTAIL_SHIFT,
	       nr_units_ctail(coord) == (pos_in_node_t)UCTAIL_NR_UNITS));
	
	return (int)cluster_shift_by_coord(coord) == (int)UCTAIL_SHIFT;
}

reiser4_internal unsigned long
clust_by_coord(const coord_t * coord, struct inode * inode)
{
	int shift;
	
	if (inode != NULL) {
		shift = inode_cluster_shift(inode);
		assert("edward-1236", 
		       ergo(!coord_is_unprepped_ctail(coord),
			    shift == cluster_shift_by_coord(coord)));
	}
	else {
		assert("edward-1237", !coord_is_unprepped_ctail(coord));
		shift = cluster_shift_by_coord(coord);
	}
	return pg_by_coord(coord) >> shift;
}

static int unsigned long
disk_cluster_size (const coord_t * coord)
{
	assert("edward-1156", 
	       item_plugin_by_coord(coord) == item_plugin_by_id(CTAIL_ID));
	/* calculation of disk cluster size
	   is meaninless if ctail is unprepped */
	assert("edward-1238", !coord_is_unprepped_ctail(coord));
	
	return PAGE_CACHE_SIZE << cluster_shift_by_coord(coord);
}

/* true if the key is of first disk cluster item */
static int
is_disk_cluster_key(const reiser4_key * key, const coord_t * coord)
{
	assert("edward-1239", item_id_by_coord(coord) == CTAIL_ID);
	
	return coord_is_unprepped_ctail(coord) ||
		((get_key_offset(key) & ((loff_t)disk_cluster_size(coord) - 1)) == 0);
}

static char *
first_unit(coord_t * coord)
{
	/* FIXME: warning: pointer of type `void *' used in arithmetic */
	return (char *)item_body_by_coord(coord) + sizeof (ctail_item_format);
}

/* plugin->u.item.b.max_key_inside :
   tail_max_key_inside */

/* plugin->u.item.b.can_contain_key */
reiser4_internal int
can_contain_key_ctail(const coord_t *coord, const reiser4_key *key, const reiser4_item_data *data)
{
	reiser4_key item_key;

	if (item_plugin_by_coord(coord) != data->iplug)
		return 0;

	item_key_by_coord(coord, &item_key);
	if (get_key_locality(key) != get_key_locality(&item_key) ||
	    get_key_objectid(key) != get_key_objectid(&item_key))
		return 0;
	if (get_key_offset(&item_key) + nr_units_ctail(coord) != get_key_offset(key))
		return 0;
	if (is_disk_cluster_key(key, coord))
		return 0;
	return 1;
}

/* plugin->u.item.b.mergeable
   c-tails of different clusters are not mergeable */
reiser4_internal int
mergeable_ctail(const coord_t * p1, const coord_t * p2)
{
	reiser4_key key1, key2;

	assert("edward-62", item_id_by_coord(p1) == CTAIL_ID);
	assert("edward-61", item_type_by_coord(p1) == UNIX_FILE_METADATA_ITEM_TYPE);

	if (item_id_by_coord(p2) != CTAIL_ID) {
		/* second item is of another type */
		return 0;
	}

	item_key_by_coord(p1, &key1);
	item_key_by_coord(p2, &key2);
	if (get_key_locality(&key1) != get_key_locality(&key2) ||
	    get_key_objectid(&key1) != get_key_objectid(&key2) ||
	    get_key_type(&key1) != get_key_type(&key2)) {
		/* items of different objects */
		return 0;
	    }
	if (get_key_offset(&key1) + nr_units_ctail(p1) != get_key_offset(&key2))
		/*  not adjacent items */
		return 0;
	if (is_disk_cluster_key(&key2, p2))
		return 0;
	return 1;
}

/* plugin->u.item.b.nr_units */
reiser4_internal pos_in_node_t
nr_units_ctail(const coord_t * coord)
{
	return (item_length_by_coord(coord) - sizeof(ctail_formatted_at(coord)->cluster_shift));
}

/* plugin->u.item.b.estimate:
   estimate how much space is needed to insert/paste @data->length bytes
   into ctail at @coord */
reiser4_internal int
estimate_ctail(const coord_t * coord /* coord of item */,
	     const reiser4_item_data * data /* parameters for new item */)
{
	if (coord == NULL)
		/* insert */
		return (sizeof(ctail_item_format) + data->length);
	else
		/* paste */
		return data->length;
}

#if REISER4_DEBUG_OUTPUT
/* ->print() method for this item plugin. */
reiser4_internal void
print_ctail(const char *prefix /* prefix to print */ ,
	  coord_t * coord /* coord of item to print */ )
{
	assert("edward-63", prefix != NULL);
	assert("edward-64", coord != NULL);

	if (item_length_by_coord(coord) < (int) sizeof (ctail_item_format))
		printk("%s: wrong size: %i < %i\n", prefix, item_length_by_coord(coord), sizeof (ctail_item_format));
	else
		printk("%s: disk cluster shift: %d\n", prefix, cluster_shift_by_coord(coord));
}
#endif

/* ->init() method for this item plugin. */
reiser4_internal int
init_ctail(coord_t * to /* coord of item */,
	   coord_t * from /* old_item */,
	   reiser4_item_data * data /* structure used for insertion */)
{
	int cluster_shift; /* cpu value to convert */

	if (data) {
		assert("edward-463", data->length > sizeof(ctail_item_format));
		cluster_shift = *((int *)(data->arg));
		data->length -= sizeof(ctail_item_format);
	}
	else {
		assert("edward-464", from != NULL);
		assert("edward-855", ctail_ok(from));
		cluster_shift = (int)(cluster_shift_by_coord(from));
	}
	cputod8(cluster_shift, &ctail_formatted_at(to)->cluster_shift);
	assert("edward-856", ctail_ok(to));
	return 0;
}

reiser4_internal int
ctail_ok (const coord_t *coord)
{
	return coord_is_unprepped_ctail(coord) || 
		(cluster_shift_by_coord(coord) <= MAX_CLUSTER_SHIFT);
}

/* plugin->u.item.b.lookup:
   NULL: We are looking for item keys only */
reiser4_internal int
check_ctail (const coord_t * coord, const char **error)
{
	if (!ctail_ok(coord)) {
		if (error)
			*error = "bad cluster shift in ctail";
		return 1;
	}
	return 0;
}

/* plugin->u.item.b.check */


/* plugin->u.item.b.paste */
reiser4_internal int
paste_ctail(coord_t * coord, reiser4_item_data * data, carry_plugin_info * info UNUSED_ARG)
{
	unsigned old_nr_units;

	assert("edward-268", data->data != NULL);
	/* copy only from kernel space */
	assert("edward-66", data->user == 0);

	old_nr_units = item_length_by_coord(coord) - sizeof(ctail_item_format) - data->length;

	/* ctail items never get pasted in the middle */

	if (coord->unit_pos == 0 && coord->between == AT_UNIT) {

                /* paste at the beginning when create new item */
		assert("edward-450", item_length_by_coord(coord) == data->length + sizeof(ctail_item_format));
		assert("edward-451", old_nr_units == 0);
	}
	else if (coord->unit_pos == old_nr_units - 1 && coord->between == AFTER_UNIT) {

                /* paste at the end */
		coord->unit_pos++;
	}
	else
		impossible("edward-453", "bad paste position");

	memcpy(first_unit(coord) + coord->unit_pos, data->data, data->length);
	
	assert("edward-857", ctail_ok(coord));

	return 0;
}

/* plugin->u.item.b.fast_paste */

/* plugin->u.item.b.can_shift
   number of units is returned via return value, number of bytes via @size. For
   ctail items they coincide */
reiser4_internal int
can_shift_ctail(unsigned free_space, coord_t * source,
		znode * target, shift_direction direction UNUSED_ARG, 
		unsigned *size /* number of bytes */ , unsigned want)
{
	/* make sure that that we do not want to shift more than we have */
	assert("edward-68", want > 0 && want <= nr_units_ctail(source));

	*size = min(want, free_space);

	if (!target) {
		/* new item will be created */
		if (*size <= sizeof(ctail_item_format)) {
			*size = 0;
			return 0;
		}
		return *size - sizeof(ctail_item_format);
	}
	return *size;
}

/* plugin->u.item.b.copy_units 
   cooperates with ->can_shift() */
reiser4_internal void
copy_units_ctail(coord_t * target, coord_t * source,
		 unsigned from, unsigned count /* units */, 
		 shift_direction where_is_free_space, 
		 unsigned free_space /* bytes */)
{
	/* make sure that item @target is expanded already */
	assert("edward-69", (unsigned) item_length_by_coord(target) >= count);
	assert("edward-70", free_space == count || free_space == count + 1);

	assert("edward-858", ctail_ok(source));
	
	if (where_is_free_space == SHIFT_LEFT) {
		/* append item @target with @count first bytes of @source:
		   this restriction came from ordinary tails */
		assert("edward-71", from == 0);
		assert("edward-860", ctail_ok(target));
		
		memcpy(first_unit(target) + nr_units_ctail(target) - count, first_unit(source), count);
	} else {
		/* target item is moved to right already */
		reiser4_key key;

		assert("edward-72", nr_units_ctail(source) == from + count);
		
		if (free_space == count) {
			init_ctail(target, source, NULL);
			//assert("edward-861", cluster_shift_by_coord(target) == d8tocpu(&ctail_formatted_at(target)->body[count]));
		}
		else {
			/* new item has been created */
			assert("edward-862", ctail_ok(target));
		}
		memcpy(first_unit(target), first_unit(source) + from, count);
		
		assert("edward-863", ctail_ok(target));
		
		/* new units are inserted before first unit in an item,
		   therefore, we have to update item key */
		item_key_by_coord(source, &key);
		set_key_offset(&key, get_key_offset(&key) + from);
		
		node_plugin_by_node(target->node)->update_item_key(target, &key, 0 /*info */);
	}
}

/* plugin->u.item.b.create_hook */
reiser4_internal int
create_hook_ctail (const coord_t * coord, void * arg)
{
	assert("edward-864", znode_is_loaded(coord->node));
	
	znode_set_convertible(coord->node);
	return 0;
}

/* plugin->u.item.b.kill_hook */
reiser4_internal int
kill_hook_ctail(const coord_t *coord, pos_in_node_t from, pos_in_node_t count, carry_kill_data *kdata)
{
	struct inode *inode;
	
	assert("edward-1157", item_id_by_coord(coord) == CTAIL_ID);
	assert("edward-291", znode_is_write_locked(coord->node));
	
	inode = kdata->inode;
	if (inode) {
		reiser4_key key;
		item_key_by_coord(coord, &key);
		
		if (from == 0 && is_disk_cluster_key(&key, coord)) {
			cloff_t start = off_to_clust(get_key_offset(&key), inode);
			truncate_page_cluster(inode, start);
		}
	}
	return 0;
}

/* for shift_hook_ctail(),
   return true if the first disk cluster item has dirty child
*/
static int
ctail_convertible (const coord_t *coord)
{
	int result;
	reiser4_key  key;
	jnode * child = NULL;

	assert("edward-477", coord != NULL);
	assert("edward-478", item_id_by_coord(coord) == CTAIL_ID);
	
	if (coord_is_unprepped_ctail(coord))
		/* unprepped ctail should be converted */
		return 1;
	
	item_key_by_coord(coord, &key);
	child =  jlookup(current_tree, 
			 get_key_objectid(&key), 
			 clust_by_coord(coord, NULL) << cluster_shift_by_coord(coord));
	if (!child)
		return 0;
	LOCK_JNODE(child);
	if (jnode_is_dirty(child))
		result = 1;
	else
		result = 0;
	UNLOCK_JNODE(child);
	jput(child);
	return result;
}

/* plugin->u.item.b.shift_hook */
reiser4_internal int
shift_hook_ctail(const coord_t * item /* coord of item */ ,
		 unsigned from UNUSED_ARG /* start unit */ ,
		 unsigned count UNUSED_ARG /* stop unit */ ,
		 znode * old_node /* old parent */ )
{
	assert("edward-479", item != NULL);
	assert("edward-480", item->node != old_node);

	if (!znode_convertible(old_node) || znode_convertible(item->node))
		return 0;
	if (ctail_convertible(item))
		znode_set_convertible(item->node);
	return 0;
}

static int
cut_or_kill_ctail_units(coord_t * coord, pos_in_node_t from, pos_in_node_t to, int cut,
			void *p, reiser4_key * smallest_removed, reiser4_key *new_first)
{
	pos_in_node_t count; /* number of units to cut */
	char *item;

 	count = to - from + 1;
	item = item_body_by_coord(coord);

	assert("edward-74", ergo(from != 0, to == coord_last_unit_pos(coord)));

	if (smallest_removed) {
		/* store smallest key removed */
		item_key_by_coord(coord, smallest_removed);
		set_key_offset(smallest_removed, get_key_offset(smallest_removed) + from);
	}

	if (new_first) {
		assert("vs-1531", from == 0);

		item_key_by_coord(coord, new_first);
		set_key_offset(new_first, get_key_offset(new_first) + from + count);
	}

	if (!cut)
		kill_hook_ctail(coord, from, 0, (struct carry_kill_data *)p);

	if (from == 0) {
		if (count != nr_units_ctail(coord)) {
			/* part of item is removed, so move free space at the beginning
			   of the item and update item key */
			reiser4_key key;
			memcpy(item + to + 1, item, sizeof(ctail_item_format));
			item_key_by_coord(coord, &key);
			set_key_offset(&key, get_key_offset(&key) + count);
			node_plugin_by_node(coord->node)->update_item_key(coord, &key, 0 /*info */ );
		}
		else {
			/* cut_units should not be called to cut evrything */
			assert("vs-1532", ergo(cut, 0));
			/* whole item is cut, so more then amount of space occupied
			   by units got freed */
			count += sizeof(ctail_item_format);
		}
		if (REISER4_DEBUG)
			memset(item, 0, count);
	}
	else if (REISER4_DEBUG)
		memset(item + sizeof(ctail_item_format) + from, 0, count);
	return count;
}

/* plugin->u.item.b.cut_units */
reiser4_internal int
cut_units_ctail(coord_t *item, pos_in_node_t from, pos_in_node_t to,
		carry_cut_data *cdata, reiser4_key *smallest_removed, reiser4_key *new_first)
{
	return cut_or_kill_ctail_units(item, from, to, 1, NULL, smallest_removed, new_first);
}

/* plugin->u.item.b.kill_units */
reiser4_internal int
kill_units_ctail(coord_t *item, pos_in_node_t from, pos_in_node_t to,
		 struct carry_kill_data *kdata, reiser4_key *smallest_removed, reiser4_key *new_first)
{
	return cut_or_kill_ctail_units(item, from, to, 0, kdata, smallest_removed, new_first);
}

#if 0
static int 
ctail_get_extension(hint_t * hint)
{
	return hint->coord.extension.ctail.shift;
}

static void
ctail_set_extension(hint_t * hint)
{
	assert("edward-1240", item_id_by_coord(&hint->coord.base_coord) == CTAIL_ID);
	hint->coord.extension.ctail.shift = cluster_shift_by_coord(&hint->coord.base_coord);
}

reiser4_internal int
hint_is_unprepped_ctail(hint_t * hint)
{
	return ctail_get_extension(hint) == (int)UCTAIL_SHIFT;
#endif

/* plugin->u.item.s.file.read */
reiser4_internal int
read_ctail(struct file *file UNUSED_ARG, flow_t *f, hint_t *hint)
{
	uf_coord_t *uf_coord;
	coord_t *coord;

	uf_coord = &hint->coord;
	coord = &uf_coord->base_coord;
	assert("edward-127", f->user == 0);
	assert("edward-129", coord && coord->node);
	assert("edward-130", coord_is_existing_unit(coord));
	assert("edward-132", znode_is_loaded(coord->node));

	/* start read only from the beginning of ctail */
	assert("edward-133", coord->unit_pos == 0);
	/* read only whole ctails */
	assert("edward-135", nr_units_ctail(coord) <= f->length);

	assert("edward-136", schedulable());
	assert("edward-886", ctail_ok(coord));

	if (f->data)
		memcpy(f->data, (char *)first_unit(coord), (size_t)nr_units_ctail(coord));
	
	dclust_set_extension(hint);
	mark_page_accessed(znode_page(coord->node));
	move_flow_forward(f, nr_units_ctail(coord));
	
	return 0;
}

/* this reads one cluster form disk,
   attaches buffer with decrypted and decompressed data */
reiser4_internal int
ctail_read_cluster (reiser4_cluster_t * clust, struct inode * inode, int write)
{
	int result;
	compression_plugin * cplug;
#if REISER4_DEBUG
	reiser4_inode * info;
	info = reiser4_inode_data(inode);
#endif
	assert("edward-671", clust->hint != NULL);
	assert("edward-140", clust->dstat == INVAL_DISK_CLUSTER);
	assert("edward-672", crc_inode_ok(inode));
	assert("edward-145", inode_get_flag(inode, REISER4_CLUSTER_KNOWN));
	
	/* set input stream */
	result = grab_tfm_stream(inode, &clust->tc, TFM_READ, INPUT_STREAM);
	if (result)
		return result;

	result = find_cluster(clust, inode, 1 /* read */, write);
	if (cbk_errored(result))
		return result;

	if (!write)
		set_hint_cluster(inode, clust->hint, 
				 clust->index + 1, ZNODE_READ_LOCK);
	
	assert("edward-673", znode_is_any_locked(clust->hint->coord.lh->node));
	
	if (clust->dstat == FAKE_DISK_CLUSTER ||
	    clust->dstat == UNPR_DISK_CLUSTER) {
		tfm_cluster_set_uptodate(&clust->tc);
		return 0;
	}
 	cplug = inode_compression_plugin(inode);
	if (cplug->alloc && !get_coa(&clust->tc, cplug->h.id)) {
		result = alloc_coa(&clust->tc, cplug, TFM_READ);
		if (result)
			return result;
	}
	result = inflate_cluster(clust, inode);
	if(result)
		return result;
	tfm_cluster_set_uptodate(&clust->tc);
	return 0;
}

/* read one locked page */
reiser4_internal int
do_readpage_ctail(reiser4_cluster_t * clust, struct page *page)
{
	int ret;
	unsigned cloff;
	struct inode * inode;
	char * data;
	size_t pgcnt;
	tfm_cluster_t * tc = &clust->tc;
	
	assert("edward-212", PageLocked(page));
	
	if(PageUptodate(page))
		goto exit;
	
	inode = page->mapping->host;
	
	if (!tfm_cluster_is_uptodate(&clust->tc)) {
		clust->index = pg_to_clust(page->index, inode);
		unlock_page(page);
		ret = ctail_read_cluster(clust, inode, 0 /* read only */);
		lock_page(page);
		if (ret)
			return ret;
	}
	if(PageUptodate(page))
		/* races with another read/write */
		goto exit;
	
	/* bytes in the page */
	pgcnt = off_to_pgcount(i_size_read(inode), page->index);
#if 0
	if (pgcnt == 0)
		return RETERR(-EINVAL);
#endif	
	assert("edward-119", tfm_cluster_is_uptodate(tc));
	
	switch (clust->dstat) {
	case UNPR_DISK_CLUSTER:
		assert("edward-1285", 0);
#if REISER4_DEBUG
		warning("edward-1168",
			"page %lu is not uptodate and disk cluster %lu (inode %llu) is unprepped\n",
			page->index, clust->index, (unsigned long long)get_inode_oid(inode));
#endif
	case FAKE_DISK_CLUSTER:
		/* fill the page by zeroes */
		data = kmap_atomic(page, KM_USER0);
		
		memset(data, 0, PAGE_CACHE_SIZE);
		flush_dcache_page(page);
		kunmap_atomic(data, KM_USER0);
		SetPageUptodate(page);
		break;
	case PREP_DISK_CLUSTER:
		/* fill the page by transformed data */
		assert("edward-1058", !PageUptodate(page));
		assert("edward-120", tc->len <= inode_cluster_size(inode));
		
		/* start page offset in the cluster */
		cloff = pg_to_off_to_cloff(page->index, inode);
		
		data = kmap(page);
		memcpy(data, tfm_stream_data(tc, OUTPUT_STREAM) + cloff, pgcnt);
		memset(data + pgcnt, 0, (size_t)PAGE_CACHE_SIZE - pgcnt);
		flush_dcache_page(page);
		kunmap(page);
		SetPageUptodate(page);
		break;
	default:
		impossible("edward-1169", "bad disk cluster state");
	}
 exit:
	return 0;	
}

/* plugin->u.item.s.file.readpage */
reiser4_internal int readpage_ctail(void * vp, struct page * page)
{
	int result;
	hint_t hint;
	lock_handle lh;
	reiser4_cluster_t * clust = vp;

	assert("edward-114", clust != NULL);
	assert("edward-115", PageLocked(page));
	assert("edward-116", !PageUptodate(page));
	assert("edward-117", !jprivate(page) && !PagePrivate(page));
	assert("edward-118", page->mapping && page->mapping->host);
	assert("edward-867", !tfm_cluster_is_uptodate(&clust->tc));

	clust->hint = &hint;
	result = load_file_hint(clust->file, &hint);
	if (result)
		return result;
	init_lh(&lh);
	hint.coord.lh = &lh;

	result = do_readpage_ctail(clust, page);
	
	assert("edward-213", PageLocked(page));
	assert("edward-1163", ergo (!result, PageUptodate(page)));
	assert("edward-868", ergo (!result, tfm_cluster_is_uptodate(&clust->tc)));
	       
	unlock_page(page);

	hint.coord.valid = 0;
	save_file_hint(clust->file, &hint);
	done_lh(&lh);
	tfm_cluster_clr_uptodate(&clust->tc);
	
	return result;
}

/* only for read methods which don't modify data */
static int
ctail_read_page_cluster(reiser4_cluster_t * clust, struct inode * inode)
{
	int i;
	int result;
	assert("edward-779", clust != NULL);
	assert("edward-1059", clust->win == NULL);
	assert("edward-780", inode != NULL);

	result = prepare_page_cluster(inode, clust, 0 /* do not capture */);
	if (result)
		return result;
	result = ctail_read_cluster(clust, inode, 0 /* read */);
	if (result)
		goto out;
	/* stream is attached at this point */
	assert("edward-781", tfm_cluster_is_uptodate(&clust->tc));

	for (i=0; i < clust->nr_pages; i++) {
		struct page * page = clust->pages[i];
		lock_page(page);
		result = do_readpage_ctail(clust, page);
		unlock_page(page);
		if (result)
			break;
	}
	tfm_cluster_clr_uptodate(&clust->tc);
 out:
	release_cluster_pages_nocapture(clust);
	assert("edward-1060", !result);
	
	return result;
}

#define list_to_page(head) (list_entry((head)->prev, struct page, lru))
#define list_to_next_page(head) (list_entry((head)->prev->prev, struct page, lru))

#if REISER4_DEBUG
#define check_order(pages)                                                    \
assert("edward-214", ergo(!list_empty(pages) && pages->next != pages->prev,   \
       list_to_page(pages)->index < list_to_next_page(pages)->index))
#endif

/* plugin->s.file.writepage */

/* plugin->u.item.s.file.readpages
   populate an address space with page clusters, and start reads against them.
   FIXME_EDWARD: this function should return errors
*/
reiser4_internal void
readpages_ctail(void *vp, struct address_space *mapping, struct list_head *pages)
{
	int ret = 0;
	hint_t hint;
	lock_handle lh;
	reiser4_cluster_t clust;
	struct page *page;
	struct pagevec lru_pvec;
	struct inode * inode = mapping->host;

	assert("edward-214", ergo(!list_empty(pages) &&
				  pages->next != pages->prev,
				  list_to_page(pages)->index < list_to_next_page(pages)->index));
	pagevec_init(&lru_pvec, 0);
	reiser4_cluster_init(&clust, 0);
	clust.file = vp;
	clust.hint = &hint;
	
	init_lh(&lh);

	ret = alloc_cluster_pgset(&clust, cluster_nrpages(inode));
	if (ret)
		goto out;
	ret = load_file_hint(clust.file, &hint);
	if (ret) 
		goto out;
 	hint.coord.lh = &lh;	

	/* address_space-level file readahead doesn't know about
	   reiser4 page clustering, so we work around this fact */
	
	while (!list_empty(pages)) {
		page = list_to_page(pages);
		list_del(&page->lru);
		if (add_to_page_cache(page, mapping, page->index, GFP_KERNEL)) {
			page_cache_release(page);
			continue;
		}
		if (PageUptodate(page)) {
			unlock_page(page);
			continue;
		}
		unlock_page(page);
		reset_cluster_params(&clust);
		clust.index = pg_to_clust(page->index, inode);
		ret = ctail_read_page_cluster(&clust, inode);
		if (ret)
			goto exit;
		assert("edward-869", !tfm_cluster_is_uptodate(&clust.tc));
		
		lock_page(page);
		ret = do_readpage_ctail(&clust, page);
		if (!pagevec_add(&lru_pvec, page))
			__pagevec_lru_add(&lru_pvec);
		if (ret) {
			warning("edward-215", "do_readpage_ctail failed");
			unlock_page(page);
		exit:
			while (!list_empty(pages)) {
				struct page *victim;
				
				victim = list_to_page(pages);
				list_del(&victim->lru);
				page_cache_release(victim);
			}
			break;
		}
		assert("edward-1061", PageUptodate(page));
		
		unlock_page(page);
	}
	assert("edward-870", !tfm_cluster_is_uptodate(&clust.tc));
	save_file_hint(clust.file, &hint);
 out:	
	done_lh(&lh);
	hint.coord.valid = 0;
	put_cluster_handle(&clust, TFM_READ);
	pagevec_lru_add(&lru_pvec);
	return;
}

/*
   plugin->u.item.s.file.append_key
   key of the first item of the next disk cluster
*/
reiser4_internal reiser4_key *
append_key_ctail(const coord_t *coord, reiser4_key *key)
{
	assert("edward-1241", item_id_by_coord(coord) == CTAIL_ID);
	assert("edward-1242", cluster_shift_by_coord(coord) <= MAX_CLUSTER_SHIFT);
	
	item_key_by_coord(coord, key);
	set_key_offset(key, ((__u64)(clust_by_coord(coord, NULL)) + 1) << cluster_shift_by_coord(coord) << PAGE_CACHE_SHIFT);
	return key;
}

 static int
insert_unprepped_ctail(reiser4_cluster_t * clust, struct inode * inode)
{
	int result;
	char buf[UCTAIL_NR_UNITS];
	reiser4_item_data data;
	reiser4_key key;
	int shift = (int)UCTAIL_SHIFT;
	
	memset(buf, 0, (size_t)UCTAIL_NR_UNITS);
	result = key_by_inode_cryptcompress(inode, 
					    clust_to_off(clust->index, inode),
					    &key);
	if (result)
		return result;
	data.user = 0;
	data.iplug = item_plugin_by_id(CTAIL_ID);
	data.arg = &shift;
	data.length = sizeof(ctail_item_format) + (size_t)UCTAIL_NR_UNITS;
	data.data = buf;
	
	result = insert_by_coord(&clust->hint->coord.base_coord,
				 &data, 
				 &key, 
				 clust->hint->coord.lh, 0);
	return result;
}

static int
insert_crc_flow(coord_t * coord, lock_handle * lh, flow_t * f, struct inode * inode)
{
	int result;
	carry_pool *pool;
	carry_level lowest_level;
	carry_op *op;
	reiser4_item_data data;
	int cluster_shift = inode_cluster_shift(inode);

	pool = init_carry_pool();
	if (IS_ERR(pool))
		return PTR_ERR(pool);
	init_carry_level(&lowest_level, pool);

	assert("edward-466", coord->between == AFTER_ITEM || coord->between == AFTER_UNIT ||
	       coord->between == BEFORE_ITEM || coord->between == EMPTY_NODE
	       || coord->between == BEFORE_UNIT);

	if (coord->between == AFTER_UNIT) {
		coord->unit_pos = 0;
		coord->between = AFTER_ITEM;
	}
	op = post_carry(&lowest_level, COP_INSERT_FLOW, coord->node, 0 /* operate directly on coord -> node */ );
	if (IS_ERR(op) || (op == NULL)) {
		done_carry_pool(pool);
		return RETERR(op ? PTR_ERR(op) : -EIO);
	}
	data.user = 0;
	data.iplug = item_plugin_by_id(CTAIL_ID);
	data.arg = &cluster_shift;

	data.length = 0;
	data.data = 0;

	op->u.insert_flow.flags = COPI_DONT_SHIFT_LEFT | COPI_DONT_SHIFT_RIGHT;
	op->u.insert_flow.insert_point = coord;
	op->u.insert_flow.flow = f;
	op->u.insert_flow.data = &data;
	op->u.insert_flow.new_nodes = 0;

	lowest_level.track_type = CARRY_TRACK_CHANGE;
	lowest_level.tracked = lh;

	result = carry(&lowest_level, 0);
	done_carry_pool(pool);

	return result;
}

/* Implementation of CRC_APPEND_ITEM mode of ctail conversion */
static int
insert_crc_flow_in_place(coord_t * coord, lock_handle * lh, flow_t * f, struct inode * inode)
{
	int ret;
	coord_t pos;
	lock_handle lock;

	assert("edward-674", f->length <= inode_scaled_cluster_size(inode));
	assert("edward-484", coord->between == AT_UNIT || coord->between == AFTER_ITEM);
	assert("edward-485", item_id_by_coord(coord) == CTAIL_ID);
	
	coord_dup (&pos, coord);
	pos.unit_pos = 0;
	pos.between = AFTER_ITEM;
	
	init_lh (&lock);
	copy_lh(&lock, lh);
	
	ret = insert_crc_flow(&pos, &lock, f, inode);
	done_lh(&lock);
	
	assert("edward-1228", !ret);
	return ret;
}

/* Implementation of CRC_OVERWRITE_ITEM mode of ctail conversion */
static int
overwrite_ctail(coord_t * coord, flow_t * f)
{
	unsigned count;
	
	assert("edward-269", f->user == 0);
	assert("edward-270", f->data != NULL);
	assert("edward-271", f->length > 0);
	assert("edward-272", coord_is_existing_unit(coord));
	assert("edward-273", coord->unit_pos == 0);
	assert("edward-274", znode_is_write_locked(coord->node));
	assert("edward-275", schedulable());
	assert("edward-467", item_id_by_coord(coord) == CTAIL_ID);
	assert("edward-1243", ctail_ok(coord));
	
	count = nr_units_ctail(coord);

	if (count > f->length)
		count = f->length;
	memcpy(first_unit(coord), f->data, count);
	move_flow_forward(f, count);
	coord->unit_pos += count;
	return 0;
}

/* Implementation of CRC_CUT_ITEM mode of ctail conversion:
   cut ctail (part or whole) starting from next unit position */
static int
cut_ctail(coord_t * coord)
{
	coord_t stop;

	assert("edward-435", coord->between == AT_UNIT &&
	       coord->item_pos < coord_num_items(coord) &&
	       coord->unit_pos <= coord_num_units(coord));

	if(coord->unit_pos == coord_num_units(coord))
		/* nothing to cut */
		return 0;
	coord_dup(&stop, coord);
	stop.unit_pos = coord_last_unit_pos(coord);

	return cut_node_content(coord, &stop, NULL, NULL, NULL);
}

int ctail_insert_unprepped_cluster(reiser4_cluster_t * clust, struct inode * inode)
{
	int result;
	
	assert("edward-1244", inode != NULL);
	assert("edward-1245", clust->hint != NULL);
	assert("edward-1246", clust->dstat == FAKE_DISK_CLUSTER);
	assert("edward-1247", clust->reserved == 1);
	assert("edward-1248",  get_current_context()->grabbed_blocks == 
	       estimate_insert_cluster(inode, 1));
	
	result = get_disk_cluster_locked(clust, inode, ZNODE_WRITE_LOCK);
	if (cbk_errored(result))
		return result;
	assert("edward-1249", result == CBK_COORD_NOTFOUND);
	assert("edward-1250", znode_is_write_locked(clust->hint->coord.lh->node));
	
	clust->hint->coord.base_coord.between = AFTER_ITEM;
	clust->hint->coord.base_coord.unit_pos = 0;
	
	result = insert_unprepped_ctail(clust, inode);
	all_grabbed2free();
	
	assert("edward-1251", !result);
	assert("edward-1252", crc_inode_ok(inode));
	assert("edward-1253", znode_is_write_locked(clust->hint->coord.lh->node));
	assert("edward-1254", reiser4_clustered_blocks(reiser4_get_current_sb()));
	assert("edward-1255", znode_convertible(clust->hint->coord.base_coord.node));
 
	return result;
}

static int
do_convert_ctail(flush_pos_t * pos, crc_write_mode_t mode)
{
	int result = 0;
	convert_item_info_t * info;
	
	assert("edward-468", pos != NULL);
	assert("edward-469", pos->sq != NULL);
	assert("edward-845", item_convert_data(pos) != NULL);

	info = item_convert_data(pos);
	assert("edward-679", info->flow.data != NULL);
	
	switch (mode) {
	case CRC_APPEND_ITEM:
		assert("edward-1229", info->flow.length != 0);
		assert("edward-1256", cluster_shift_by_coord(&pos->coord) <= MAX_CLUSTER_SHIFT);
		result = insert_crc_flow_in_place(&pos->coord, &pos->lock, &info->flow, info->inode);
		break;
	case CRC_OVERWRITE_ITEM:
		assert("edward-1230", info->flow.length != 0);
		overwrite_ctail(&pos->coord, &info->flow);
		if (info->flow.length != 0)
			break;
	case CRC_CUT_ITEM:
		assert("edward-1231", info->flow.length == 0);
		result = cut_ctail(&pos->coord);
		break;
	default:
		result = RETERR(-EIO);
		impossible("edward-244", "bad convert mode");
	}
	return result;
}

/* plugin->u.item.f.scan */
reiser4_internal int scan_ctail(flush_scan * scan)
{
	int result = 0;
	struct page * page;
	struct inode * inode;
	jnode * node = scan->node;
	
	assert("edward-227", scan->node != NULL);
	assert("edward-228", jnode_is_cluster_page(scan->node));
	assert("edward-639", znode_is_write_locked(scan->parent_lock.node));
	
	page = jnode_page(node);
	inode = page->mapping->host;
	
	if (!scanning_left(scan))
		return result;
	if (!znode_is_dirty(scan->parent_lock.node))
		znode_make_dirty(scan->parent_lock.node);

	if (!znode_convertible(scan->parent_lock.node)) {
		LOCK_JNODE(scan->node);
		if (jnode_is_dirty(scan->node)) {
			warning("edward-873", "child is dirty but parent not squeezable");
			znode_set_convertible(scan->parent_lock.node);
		} else {
			warning("edward-681", "cluster page is already processed");
			UNLOCK_JNODE(scan->node);
			return -EAGAIN;
		}
		UNLOCK_JNODE(scan->node);
	}
	return result;
}

/* If true, this function attaches children */
static int
should_attach_convert_idata(flush_pos_t * pos)
{
	int result;
	assert("edward-431", pos != NULL);
	assert("edward-432", pos->child == NULL);
	assert("edward-619", znode_is_write_locked(pos->coord.node));
	assert("edward-470", item_plugin_by_coord(&pos->coord) == item_plugin_by_id(CTAIL_ID));

	/* check for leftmost child */
	utmost_child_ctail(&pos->coord, LEFT_SIDE, &pos->child);

	if (!pos->child)
		return 0;
	LOCK_JNODE(pos->child);
	result = jnode_is_dirty(pos->child) &&
		pos->child->atom == ZJNODE(pos->coord.node)->atom;
	UNLOCK_JNODE(pos->child);
	if (!result && pos->child) {
		/* existing child isn't to attach, clear up this one */
		jput(pos->child);
		pos->child = NULL;
	}
	return result;
}

/* plugin->init_convert_data() */
static int
init_convert_data_ctail(convert_item_info_t * idata, struct inode * inode)
{
	assert("edward-813", idata != NULL);
	assert("edward-814", inode != NULL);

	idata->inode = inode;
	idata->d_cur = DC_FIRST_ITEM;
	idata->d_next = DC_INVALID_STATE;
	
	return 0;
}

static int
alloc_item_convert_data(convert_info_t * sq)
{
	assert("edward-816", sq != NULL);
	assert("edward-817", sq->itm == NULL);

	sq->itm = reiser4_kmalloc(sizeof(*sq->itm), GFP_KERNEL);
	if (sq->itm == NULL)
		return RETERR(-ENOMEM);
	return 0;
}

static void
free_item_convert_data(convert_info_t * sq)
{
	assert("edward-818", sq != NULL);
	assert("edward-819", sq->itm != NULL);
	assert("edward-820", sq->iplug != NULL);

	reiser4_kfree(sq->itm);
	sq->itm = NULL;
	return;
}

static int
alloc_convert_data(flush_pos_t * pos)
{
	assert("edward-821", pos != NULL);
	assert("edward-822", pos->sq == NULL);

	pos->sq = reiser4_kmalloc(sizeof(*pos->sq), GFP_KERNEL);
	if (!pos->sq)
		return RETERR(-ENOMEM);
	memset(pos->sq, 0, sizeof(*pos->sq));
	return 0;
}

reiser4_internal void
free_convert_data(flush_pos_t * pos)
{
	convert_info_t * sq;

	assert("edward-823", pos != NULL);
	assert("edward-824", pos->sq != NULL);

	sq = pos->sq;
	if (sq->itm)
		free_item_convert_data(sq);
	put_cluster_handle(&sq->clust, TFM_WRITE);
	reiser4_kfree(pos->sq);
	pos->sq = NULL;
	return;
}

static int
init_item_convert_data(flush_pos_t * pos, struct inode * inode)
{
	convert_info_t * sq;

	assert("edward-825", pos != NULL);
	assert("edward-826", pos->sq != NULL);
	assert("edward-827", item_convert_data(pos) != NULL);
	assert("edward-828", inode != NULL);

	sq = pos->sq;

	memset(sq->itm, 0, sizeof(*sq->itm));

	/* iplug->init_convert_data() */
	return init_convert_data_ctail(sq->itm, inode);
}

/* create and attach disk cluster info used by 'convert' phase of the flush
   squalloc() */
static int
attach_convert_idata(flush_pos_t * pos, struct inode * inode)
{
	int ret = 0;
	convert_item_info_t * info;
	reiser4_cluster_t *clust;
	file_plugin * fplug = inode_file_plugin(inode);
	compression_plugin * cplug = inode_compression_plugin(inode);
	
	assert("edward-248", pos != NULL);
	assert("edward-249", pos->child != NULL);
	assert("edward-251", inode != NULL);
	assert("edward-682", crc_inode_ok(inode));
	assert("edward-252", fplug == file_plugin_by_id(CRC_FILE_PLUGIN_ID));
	assert("edward-473", item_plugin_by_coord(&pos->coord) == item_plugin_by_id(CTAIL_ID));
	
	if (!pos->sq) {
		ret = alloc_convert_data(pos);
		if (ret)
			return ret;
	}
	clust = &pos->sq->clust;
	if (cplug->alloc && !get_coa(&clust->tc, cplug->h.id)) {
		ret = alloc_coa(&clust->tc, cplug, TFM_WRITE);
		if (ret)
			goto err;
	}
	
	if (convert_data(pos)->clust.pages == NULL) {
		ret = alloc_cluster_pgset(&convert_data(pos)->clust,
					  MAX_CLUSTER_NRPAGES);
		if (ret)
			goto err;
	}
	reset_cluster_pgset(&convert_data(pos)->clust,
			    MAX_CLUSTER_NRPAGES);
	
	assert("edward-829", pos->sq != NULL);
	assert("edward-250", item_convert_data(pos) == NULL);

	pos->sq->iplug = item_plugin_by_id(CTAIL_ID);

	ret = alloc_item_convert_data(pos->sq);
	if (ret)
		goto err;
	ret = init_item_convert_data(pos, inode);
	if (ret)
		goto err;
	info = item_convert_data(pos);
	
	clust->index = pg_to_clust(jnode_page(pos->child)->index, inode);
	
	ret = flush_cluster_pages(clust, pos->child, inode);
	if (ret)
		goto err;

	assert("edward-830", equi(get_coa(&clust->tc, cplug->h.id), cplug->alloc));

	ret = deflate_cluster(clust, inode);
	if (ret)
		goto err;
	
	inc_item_convert_count(pos);

	/* make flow by transformed stream */
	fplug->flow_by_inode(info->inode, 
			     tfm_stream_data(&clust->tc, OUTPUT_STREAM), 
			     0/* kernel space */,
			     clust->tc.len,
			     clust_to_off(clust->index, inode),
			     WRITE_OP,
			     &info->flow);
	jput(pos->child);
	
	assert("edward-683", crc_inode_ok(inode));
	return 0;
 err:
	jput(pos->child);
	free_convert_data(pos);
	return ret;
}

/* clear up disk cluster info */
static void
detach_convert_idata(convert_info_t * sq)
{
	convert_item_info_t * info;
	
	assert("edward-253", sq != NULL);
	assert("edward-840", sq->itm != NULL);
	
	info = sq->itm;
	assert("edward-255", info->inode != NULL);
	assert("edward-1175", 
	       inode_get_flag(info->inode, REISER4_CLUSTER_KNOWN));
	assert("edward-1212", info->flow.length == 0);
	
	/* the final release of pages */
	forget_cluster_pages(sq->clust.pages, sq->clust.nr_pages);
	free_item_convert_data(sq);
	return;
}

/* plugin->u.item.f.utmost_child */

/* This function sets leftmost child for a first cluster item,
   if the child exists, and NULL in other cases.
   NOTE-EDWARD: Do not call this for RIGHT_SIDE */

reiser4_internal int
utmost_child_ctail(const coord_t * coord, sideof side, jnode ** child)
{
	reiser4_key key;

	item_key_by_coord(coord, &key);

	assert("edward-257", coord != NULL);
	assert("edward-258", child != NULL);
	assert("edward-259", side == LEFT_SIDE);
	assert("edward-260", item_plugin_by_coord(coord) == item_plugin_by_id(CTAIL_ID));

	if (!is_disk_cluster_key(&key, coord))
		*child = NULL;
	else
		*child = jlookup(current_tree, get_key_objectid(item_key_by_coord(coord, &key)), pg_by_coord(coord));
	return 0;
}

/* Returns true if @p2 is the next item to @p1
   in the _same_ disk cluster.
   Disk cluster is a set of items. If ->clustered() != NULL,  
   with each item the whole disk cluster should be read/modified
*/
static int 
clustered_ctail (const coord_t * p1, const coord_t * p2)
{
	return mergeable_ctail(p1, p2);
}

/* Go rightward and check for next disk cluster item, set 
   d_next to DC_CHAINED_ITEM, if the last one exists.
   If the current position is last item, go to right neighbor. 
   Skip empty nodes. Note, that right neighbors may be not in
   the slum because of races. If so, make it dirty and
   convertible. 
*/
static int
next_item_dc_stat(flush_pos_t * pos)
{
	int ret = 0; 
	int stop = 0;
	znode * cur;
	coord_t coord;
	lock_handle lh;
	lock_handle right_lock;
	
	assert("edward-1232", !node_is_empty(pos->coord.node));
	assert("edward-1014", pos->coord.item_pos < coord_num_items(&pos->coord));
	assert("edward-1015", chaining_data_present(pos));
	assert("edward-1017", item_convert_data(pos)->d_next == DC_INVALID_STATE);
	
	item_convert_data(pos)->d_next = DC_AFTER_CLUSTER;

	if (item_convert_data(pos)->d_cur == DC_AFTER_CLUSTER)
		return ret;
	if (pos->coord.item_pos < coord_num_items(&pos->coord) - 1)
		return ret;
	
	/* check next slum item */
	init_lh(&right_lock);
	cur = pos->coord.node;

	while (!stop) {
		init_lh(&lh);
		ret = reiser4_get_right_neighbor(&lh,
						 cur,
						 ZNODE_WRITE_LOCK,
						 GN_CAN_USE_UPPER_LEVELS);
		if (ret)
			break;
		ret = zload(lh.node);
		if (ret) {
			done_lh(&lh);
			break;
		}
		coord_init_before_first_item(&coord, lh.node);
		
		if (node_is_empty(lh.node)) {
			znode_make_dirty(lh.node);
			znode_set_convertible(lh.node);
			stop = 0;
		} else if (clustered_ctail(&pos->coord, &coord)) {

			item_convert_data(pos)->d_next = DC_CHAINED_ITEM;
			
			if (!znode_is_dirty(lh.node)) {
				warning("edward-1024",
					"next slum item mergeable, " 
					"but znode %p isn't dirty\n",
					lh.node);
				znode_make_dirty(lh.node);
			}
			if (!znode_convertible(lh.node)) {
				warning("edward-1272",
					"next slum item mergeable, " 
					"but znode %p isn't convertible\n",
					lh.node);
				znode_set_convertible(lh.node);
			}
			stop = 1;
		} else
			stop = 1;
		zrelse(lh.node);
		done_lh(&right_lock);
		copy_lh(&right_lock, &lh);
		done_lh(&lh);
		cur = right_lock.node;
	}
	done_lh(&right_lock);
	
	if (ret == -E_NO_NEIGHBOR)
		ret = 0;
	return ret;
}

static int
assign_convert_mode(convert_item_info_t * idata, 
		   crc_write_mode_t * mode)
{
	int result = 0;

	assert("edward-1025", idata != NULL);
	
	if (idata->flow.length) {
		/* append or overwrite */
		switch(idata->d_cur) {
		case DC_FIRST_ITEM:
		case DC_CHAINED_ITEM:
			*mode = CRC_OVERWRITE_ITEM;
			break;
		case DC_AFTER_CLUSTER:
			*mode = CRC_APPEND_ITEM;
			break;
		default:
			impossible("edward-1018", 
				   "wrong current item state");
		}
	} else {
		/* cut or invalidate */
		switch(idata->d_cur) {
		case DC_FIRST_ITEM:	
		case DC_CHAINED_ITEM:	
			*mode = CRC_CUT_ITEM;
			break;
		case DC_AFTER_CLUSTER:
			result = 1;
			break;
		default:
			impossible("edward-1019", 
				   "wrong current item state");
		}
	}
	return result;
}

/* plugin->u.item.f.convert */
/* write ctail in guessed mode */
reiser4_internal int
convert_ctail(flush_pos_t * pos)
{
	int result;
	int nr_items;
	crc_write_mode_t mode = CRC_OVERWRITE_ITEM;
	
	assert("edward-1020", pos != NULL);
	assert("edward-1213", coord_num_items(&pos->coord) != 0);
	assert("edward-1257", item_id_by_coord(&pos->coord) == CTAIL_ID);
	assert("edward-1258", ctail_ok(&pos->coord));
	assert("edward-261", pos->coord.node != NULL);
	
	nr_items = coord_num_items(&pos->coord);
	if (!chaining_data_present(pos)) {
		if (should_attach_convert_idata(pos)) {
			/* attach convert item info */
			struct inode * inode;
			
			assert("edward-264", pos->child != NULL);
			assert("edward-265", jnode_page(pos->child) != NULL);
			assert("edward-266", jnode_page(pos->child)->mapping != NULL);
			
			inode = jnode_page(pos->child)->mapping->host;
			
			assert("edward-267", inode != NULL);
			
			/* attach item convert info by child and put the last one */
			result = attach_convert_idata(pos, inode);
			pos->child = NULL;
			if (result == -E_REPEAT) {
				/* jnode became clean, or there is no dirty
				   pages (nothing to update in disk cluster) */
				warning("edward-1021",
					"convert_ctail: nothing to attach");
				return 0;
			}
			if (result != 0)
				return result;
		}
		else
			/* unconvertible */
			return 0;
	}
	else {
		/* use old convert info */
		
		convert_item_info_t * idata;
		
		idata = item_convert_data(pos);
		
		result = assign_convert_mode(idata, &mode);
		if (result) {
			/* disk cluster is over, 
			   nothing to update anymore */
			detach_convert_idata(pos->sq);
			return 0;
		}
	}
	
	assert("edward-433", chaining_data_present(pos));
	assert("edward-1022", pos->coord.item_pos < coord_num_items(&pos->coord));
	
	result = next_item_dc_stat(pos);
	if (result) {
		detach_convert_idata(pos->sq);
		return result;
	}
	result = do_convert_ctail(pos, mode);
	if (result) {
		detach_convert_idata(pos->sq);
		return result;
	}
	switch (mode) {
	case CRC_CUT_ITEM:
		assert("edward-1214", item_convert_data(pos)->flow.length == 0);
		assert("edward-1215",
		       coord_num_items(&pos->coord) == nr_items ||
		       coord_num_items(&pos->coord) == nr_items - 1);
		if (item_convert_data(pos)->d_next == DC_CHAINED_ITEM)
			break;
		if (coord_num_items(&pos->coord) != nr_items) {
			/* the item was killed, no more chained items */
			detach_convert_idata(pos->sq);
			if (!node_is_empty(pos->coord.node))
				/* make sure the next item will be scanned */ 
				coord_init_before_item(&pos->coord);
			break;
		}
	case CRC_APPEND_ITEM:
		assert("edward-434", item_convert_data(pos)->flow.length == 0);
		detach_convert_idata(pos->sq);
		break;
	case CRC_OVERWRITE_ITEM:
		if (coord_is_unprepped_ctail(&pos->coord)) {
			/* convert unpprepped ctail to prepped one */
			int shift;
			shift = inode_cluster_shift(item_convert_data(pos)->inode);
			assert("edward-1259", shift <= MAX_CLUSTER_SHIFT);
			cputod8(shift, &ctail_formatted_at(&pos->coord)->cluster_shift);
		}
		break;
	}
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
