/* Copyright 2001, 2002, 2003, 2004 by Hans Reiser, licensing governed by
 * reiser4/README */

/* this file contains implementations of inode/file/address_space/file plugin
   operations specific for "unix file plugin" (plugin id is
   UNIX_FILE_PLUGIN_ID)
*/

#include "../../inode.h"
#include "../../super.h"
#include "../../tree_walk.h"
#include "../../carry.h"
#include "../../page_cache.h"
#include "../../ioctl.h"
#include "../object.h"
#include "../../safe_link.h"
#include "funcs.h"

#include <linux/writeback.h>
#include <linux/pagevec.h>
#include <linux/syscalls.h>

/* "Unix file" are built either of tail items only (FORMATTING_ID) or of extent
   items only (EXTENT_POINTER_ID) or empty (have no items but stat data)
*/

static int unpack(struct inode *inode, int forever);

/* get unix file plugin specific portion of inode */
unix_file_info_t *unix_file_inode_data(const struct inode *inode)
{
	return &reiser4_inode_data(inode)->file_plugin_data.unix_file_info;
}

static int file_is_built_of_tails(const struct inode *inode)
{
	return unix_file_inode_data(inode)->container == UF_CONTAINER_TAILS;
}

static int file_state_is_unknown(const struct inode *inode)
{
	return unix_file_inode_data(inode)->container == UF_CONTAINER_UNKNOWN;
}

static void set_file_state_extents(struct inode *inode)
{
	unix_file_inode_data(inode)->container = UF_CONTAINER_EXTENTS;
}

static void set_file_state_tails(struct inode *inode)
{
	unix_file_inode_data(inode)->container = UF_CONTAINER_TAILS;
}

static void set_file_state_empty(struct inode *inode)
{
	unix_file_inode_data(inode)->container = UF_CONTAINER_EMPTY;
}

static void set_file_state_unknown(struct inode *inode)
{
	unix_file_inode_data(inode)->container = UF_CONTAINER_UNKNOWN;
}

static int less_than_ldk(znode *node, const reiser4_key *key)
{
	int result;

	read_lock_dk(znode_get_tree(node));
	result = keylt(key, znode_get_ld_key(node));
	read_unlock_dk(znode_get_tree(node));
	return result;
}

int equal_to_rdk(znode *node, const reiser4_key *key)
{
	int result;

	read_lock_dk(znode_get_tree(node));
	result = keyeq(key, znode_get_rd_key(node));
	read_unlock_dk(znode_get_tree(node));
	return result;
}

#if REISER4_DEBUG

static int less_than_rdk(znode * node, const reiser4_key * key)
{
	int result;

	read_lock_dk(znode_get_tree(node));
	result = keylt(key, znode_get_rd_key(node));
	read_unlock_dk(znode_get_tree(node));
	return result;
}

int equal_to_ldk(znode * node, const reiser4_key * key)
{
	int result;

	read_lock_dk(znode_get_tree(node));
	result = keyeq(key, znode_get_ld_key(node));
	read_unlock_dk(znode_get_tree(node));
	return result;
}

/* get key of item next to one @coord is set to */
static reiser4_key *get_next_item_key(const coord_t * coord,
				      reiser4_key * next_key)
{
	if (coord->item_pos == node_num_items(coord->node) - 1) {
		/* get key of next item if it is in right neighbor */
		read_lock_dk(znode_get_tree(coord->node));
		*next_key = *znode_get_rd_key(coord->node);
		read_unlock_dk(znode_get_tree(coord->node));
	} else {
		/* get key of next item if it is in the same node */
		coord_t next;

		coord_dup_nocheck(&next, coord);
		next.unit_pos = 0;
		check_me("vs-730", coord_next_item(&next) == 0);
		item_key_by_coord(&next, next_key);
	}
	return next_key;
}

/**
 * item_of_that_file
 * @coord:
 * @key:
 *
 * Returns true if @key is a key of position if @coord is set to item of fileif item of file
 */
static int item_of_that_file(const coord_t *coord, const reiser4_key *key)
{
	reiser4_key max_possible;
	item_plugin *iplug;

	iplug = item_plugin_by_coord(coord);
	assert("vs-1011", iplug->b.max_key_inside);
	return keylt(key, iplug->b.max_key_inside(coord, &max_possible));
}

static int check_coord(const coord_t * coord, const reiser4_key * key)
{
	coord_t twin;

	if (!REISER4_DEBUG)
		return 1;
	node_plugin_by_node(coord->node)->lookup(coord->node, key,
						 FIND_MAX_NOT_MORE_THAN, &twin);
	return coords_equal(coord, &twin);
}

static int file_is_built_of_extents(const struct inode *inode)
{
	return unix_file_inode_data(inode)->container == UF_CONTAINER_EXTENTS;
}

static int file_is_empty(const struct inode *inode)
{
	return unix_file_inode_data(inode)->container == UF_CONTAINER_EMPTY;
}

#endif				/* REISER4_DEBUG */

static void init_uf_coord(uf_coord_t * uf_coord, lock_handle * lh)
{
	coord_init_zero(&uf_coord->coord);
	coord_clear_iplug(&uf_coord->coord);
	uf_coord->lh = lh;
	init_lh(lh);
	memset(&uf_coord->extension, 0, sizeof(uf_coord->extension));
	uf_coord->valid = 0;
}

static inline void validate_extended_coord(uf_coord_t * uf_coord, loff_t offset)
{
	assert("vs-1333", uf_coord->valid == 0);
	assert("vs-1348",
	       item_plugin_by_coord(&uf_coord->coord)->s.file.
	       init_coord_extension);

	item_body_by_coord(&uf_coord->coord);
	item_plugin_by_coord(&uf_coord->coord)->s.file.
	    init_coord_extension(uf_coord, offset);
}

write_mode_t how_to_write(uf_coord_t * uf_coord, const reiser4_key * key)
{
	write_mode_t result;
	coord_t *coord;
	ON_DEBUG(reiser4_key check);

	coord = &uf_coord->coord;

	assert("vs-1252", znode_is_wlocked(coord->node));
	assert("vs-1253", znode_is_loaded(coord->node));

	if (uf_coord->valid == 1) {
		assert("vs-1332", check_coord(coord, key));
		return (coord->between ==
			AFTER_UNIT) ? APPEND_ITEM : OVERWRITE_ITEM;
	}

	if (less_than_ldk(coord->node, key)) {
		assert("vs-1014", get_key_offset(key) == 0);

		coord_init_before_first_item(coord, coord->node);
		uf_coord->valid = 1;
		result = FIRST_ITEM;
		goto ok;
	}

	assert("vs-1335", less_than_rdk(coord->node, key));

	if (node_is_empty(coord->node)) {
		assert("vs-879", znode_get_level(coord->node) == LEAF_LEVEL);
		assert("vs-880", get_key_offset(key) == 0);
		/*
		 * Situation that check below tried to handle is follows: some
		 * other thread writes to (other) file and has to insert empty
		 * leaf between two adjacent extents. Generally, we are not
		 * supposed to muck with this node. But it is possible that
		 * said other thread fails due to some error (out of disk
		 * space, for example) and leaves empty leaf
		 * lingering. Nothing prevents us from reusing it.
		 */
		assert("vs-1000", less_than_rdk(coord->node, key));
		assert("vs-1002", coord->between == EMPTY_NODE);
		result = FIRST_ITEM;
		uf_coord->valid = 1;
		goto ok;
	}

	assert("vs-1336", coord->item_pos < node_num_items(coord->node));
	assert("vs-1007",
	       ergo(coord->between == AFTER_UNIT
		    || coord->between == AT_UNIT,
		    keyle(item_key_by_coord(coord, &check), key)));
	assert("vs-1008",
	       ergo(coord->between == AFTER_UNIT
		    || coord->between == AT_UNIT, keylt(key,
							get_next_item_key(coord,
									  &check))));

	switch (coord->between) {
	case AFTER_ITEM:
		uf_coord->valid = 1;
		result = FIRST_ITEM;
		break;
	case AFTER_UNIT:
		assert("vs-1323", (item_is_tail(coord) || item_is_extent(coord))
		       && item_of_that_file(coord, key));
		assert("vs-1208",
		       keyeq(item_plugin_by_coord(coord)->s.file.
			     append_key(coord, &check), key));
		result = APPEND_ITEM;
		validate_extended_coord(uf_coord, get_key_offset(key));
		break;
	case AT_UNIT:
		/* FIXME: it would be nice to check that coord matches to key */
		assert("vs-1324", (item_is_tail(coord) || item_is_extent(coord))
		       && item_of_that_file(coord, key));
		validate_extended_coord(uf_coord, get_key_offset(key));
		result = OVERWRITE_ITEM;
		break;
	default:
		assert("vs-1337", 0);
		result = OVERWRITE_ITEM;
		break;
	}

      ok:
	assert("vs-1349", uf_coord->valid == 1);
	assert("vs-1332", check_coord(coord, key));
	return result;
}

/* obtain lock on right neighbor and drop lock on current node */
int goto_right_neighbor(coord_t * coord, lock_handle * lh)
{
	int result;
	lock_handle lh_right;

	assert("vs-1100", znode_is_locked(coord->node));

	init_lh(&lh_right);
	result = reiser4_get_right_neighbor(&lh_right, coord->node,
					    znode_is_wlocked(coord->
							     node) ?
					    ZNODE_WRITE_LOCK : ZNODE_READ_LOCK,
					    GN_CAN_USE_UPPER_LEVELS);
	if (result) {
		done_lh(&lh_right);
		return result;
	}

	done_lh(lh);

	coord_init_first_unit_nocheck(coord, lh_right.node);
	move_lh(lh, &lh_right);

	return 0;

}

/* this is to be used after find_file_item and in find_file_item_nohint to
 * determine real state of file */
static void
set_file_state(struct inode *inode, int cbk_result, tree_level level)
{
	assert("vs-1649", inode != NULL);

	if (cbk_errored(cbk_result))
		/* error happened in find_file_item */
		return;

	assert("vs-1164", level == LEAF_LEVEL || level == TWIG_LEVEL);

	if (inode_get_flag(inode, REISER4_PART_CONV)) {
		set_file_state_unknown(inode);
		return;
	}

	if (file_state_is_unknown(inode)) {
		if (cbk_result == CBK_COORD_NOTFOUND)
			set_file_state_empty(inode);
		else if (level == LEAF_LEVEL)
			set_file_state_tails(inode);
		else
			set_file_state_extents(inode);
	} else {
		/* file state is known, check that it is set correctly */
		assert("vs-1161", ergo(cbk_result == CBK_COORD_NOTFOUND,
				       file_is_empty(inode)));
		assert("vs-1162",
		       ergo(level == LEAF_LEVEL
			    && cbk_result == CBK_COORD_FOUND,
			    file_is_built_of_tails(inode)));
		assert("vs-1165",
		       ergo(level == TWIG_LEVEL
			    && cbk_result == CBK_COORD_FOUND,
			    file_is_built_of_extents(inode)));
	}
}

/**
 * find_file_item - look for file item in the tree
 * @hint: provides coordinate, lock handle, seal
 * @key: key for search
 * @mode: mode of lock to put on returned node
 * @ra_info:
 * @inode:
 *
 * This finds position in the tree corresponding to @key. It first tries to use
 * @hint's seal if it is set.
 */
static int find_file_item(hint_t *hint, const reiser4_key *key,
			  znode_lock_mode lock_mode,
			  struct inode *inode)
{
	int result;
	coord_t *coord;
	lock_handle *lh;

	assert("nikita-3030", schedulable());
	assert("vs-1707", hint != NULL);
	assert("vs-47", inode != NULL);

	coord = &hint->ext_coord.coord;
	lh = hint->ext_coord.lh;
	init_lh(lh);

	result = hint_validate(hint, key, 1 /* check key */ , lock_mode);
	if (!result) {
		if (coord->between == AFTER_UNIT
		    && equal_to_rdk(coord->node, key)) {
			result = goto_right_neighbor(coord, lh);
			if (result == -E_NO_NEIGHBOR)
				return RETERR(-EIO);
			if (result)
				return result;
			assert("vs-1152", equal_to_ldk(coord->node, key));
			/* we moved to different node. Invalidate coord
			   extension, zload is necessary to init it again */
			hint->ext_coord.valid = 0;
		}

		set_file_state(inode, CBK_COORD_FOUND,
			       znode_get_level(coord->node));
		return CBK_COORD_FOUND;
	}

	coord_init_zero(coord);
	result = object_lookup(inode, key, coord, lh, lock_mode,
			       FIND_MAX_NOT_MORE_THAN,
			       TWIG_LEVEL, LEAF_LEVEL,
			       (lock_mode == ZNODE_READ_LOCK) ? CBK_UNIQUE :
			       (CBK_UNIQUE | CBK_FOR_INSERT), NULL);

	set_file_state(inode, result, znode_get_level(coord->node));

	/* FIXME: we might already have coord extension initialized */
	hint->ext_coord.valid = 0;
	return result;
}

int
find_file_item_nohint(coord_t * coord, lock_handle * lh,
		      const reiser4_key * key, znode_lock_mode lock_mode,
		      struct inode *inode)
{
	int result;

	result = object_lookup(inode, key, coord, lh, lock_mode,
			       FIND_MAX_NOT_MORE_THAN,
			       TWIG_LEVEL, LEAF_LEVEL,
			       (lock_mode ==
				ZNODE_READ_LOCK) ? CBK_UNIQUE : (CBK_UNIQUE |
								 CBK_FOR_INSERT),
			       NULL /* ra_info */ );
	set_file_state(inode, result, znode_get_level(coord->node));
	return result;
}

/* plugin->u.file.write_flowom = NULL
   plugin->u.file.read_flow = NULL */

void hint_init_zero(hint_t * hint)
{
	memset(hint, 0, sizeof(*hint));
	init_lh(&hint->lh);
	hint->ext_coord.lh = &hint->lh;
}

/* find position of last byte of last item of the file plus 1. This is used by truncate and mmap to find real file
   size */
static int find_file_size(struct inode *inode, loff_t * file_size)
{
	int result;
	reiser4_key key;
	coord_t coord;
	lock_handle lh;
	item_plugin *iplug;

	assert("vs-1247",
	       inode_file_plugin(inode)->key_by_inode ==
	       key_by_inode_and_offset_common);
	key_by_inode_and_offset_common(inode, get_key_offset(max_key()), &key);

	init_lh(&lh);
	result =
	    find_file_item_nohint(&coord, &lh, &key, ZNODE_READ_LOCK, inode);
	if (cbk_errored(result)) {
		/* error happened */
		done_lh(&lh);
		return result;
	}

	if (result == CBK_COORD_NOTFOUND) {
		/* empty file */
		done_lh(&lh);
		*file_size = 0;
		return 0;
	}

	/* there are items of this file (at least one) */
	/*coord_clear_iplug(&coord); */
	result = zload(coord.node);
	if (unlikely(result)) {
		done_lh(&lh);
		return result;
	}
	iplug = item_plugin_by_coord(&coord);

	assert("vs-853", iplug->s.file.append_key);
	iplug->s.file.append_key(&coord, &key);

	*file_size = get_key_offset(&key);

	zrelse(coord.node);
	done_lh(&lh);

	return 0;
}

static int find_file_state(unix_file_info_t * uf_info)
{
	int result;

	assert("vs-1628", ea_obtained(uf_info));

	result = 0;
	if (uf_info->container == UF_CONTAINER_UNKNOWN) {
		loff_t file_size;

		result =
		    find_file_size(unix_file_info_to_inode(uf_info),
				   &file_size);
	}
	assert("vs-1074",
	       ergo(result == 0, uf_info->container != UF_CONTAINER_UNKNOWN));
	return result;
}

/* estimate and reserve space needed to truncate page which gets partially truncated: one block for page itself, stat
   data update (estimate_one_insert_into_item) and one item insertion (estimate_one_insert_into_item) which may happen
   if page corresponds to hole extent and unallocated one will have to be created */
static int reserve_partial_page(reiser4_tree * tree)
{
	grab_space_enable();
	return reiser4_grab_reserved(reiser4_get_current_sb(),
				     1 +
				     2 * estimate_one_insert_into_item(tree),
				     BA_CAN_COMMIT);
}

/* estimate and reserve space needed to cut one item and update one stat data */
static int reserve_cut_iteration(reiser4_tree * tree)
{
	__u64 estimate = estimate_one_item_removal(tree)
	    + estimate_one_insert_into_item(tree);

	assert("nikita-3172", lock_stack_isclean(get_current_lock_stack()));

	grab_space_enable();
	/* We need to double our estimate now that we can delete more than one
	   node. */
	return reiser4_grab_reserved(reiser4_get_current_sb(), estimate * 2,
				     BA_CAN_COMMIT);
}

int update_file_size(struct inode *inode, reiser4_key * key, int update_sd)
{
	int result = 0;

	INODE_SET_FIELD(inode, i_size, get_key_offset(key));
	if (update_sd) {
		inode->i_ctime = inode->i_mtime = CURRENT_TIME;
		result = reiser4_update_sd(inode);
	}
	return result;
}

/* cut file items one by one starting from the last one until new file size (inode->i_size) is reached. Reserve space
   and update file stat data on every single cut from the tree */
int
cut_file_items(struct inode *inode, loff_t new_size, int update_sd,
	       loff_t cur_size, int (*update_actor) (struct inode *,
						     reiser4_key *, int))
{
	reiser4_key from_key, to_key;
	reiser4_key smallest_removed;
	file_plugin *fplug = inode_file_plugin(inode);
	int result;
	int progress = 0;

	assert("vs-1248",
	       fplug == file_plugin_by_id(UNIX_FILE_PLUGIN_ID) ||
	       fplug == file_plugin_by_id(CRC_FILE_PLUGIN_ID));

	fplug->key_by_inode(inode, new_size, &from_key);
	to_key = from_key;
	set_key_offset(&to_key, cur_size - 1 /*get_key_offset(max_key()) */ );
	/* this loop normally runs just once */
	while (1) {
		result = reserve_cut_iteration(tree_by_inode(inode));
		if (result)
			break;

		result = cut_tree_object(current_tree, &from_key, &to_key,
					 &smallest_removed, inode, 1,
					 &progress);
		if (result == -E_REPEAT) {
			/* -E_REPEAT is a signal to interrupt a long file truncation process */
			if (progress) {
				result =
				    update_actor(inode, &smallest_removed,
						 update_sd);
				if (result)
					break;
			}
			all_grabbed2free();
			reiser4_release_reserved(inode->i_sb);

			/* cut_tree_object() was interrupted probably because
			 * current atom requires commit, we have to release
			 * transaction handle to allow atom commit. */
			txn_restart_current();
			continue;
		}
		if (result
		    && !(result == CBK_COORD_NOTFOUND && new_size == 0
			 && inode->i_size == 0))
			break;

		set_key_offset(&smallest_removed, new_size);
		/* Final sd update after the file gets its correct size */
		result = update_actor(inode, &smallest_removed, update_sd);
		break;
	}
	all_grabbed2free();
	reiser4_release_reserved(inode->i_sb);

	return result;
}

int find_or_create_extent(struct page *page);

static int filler(void *vp, struct page *page)
{
	return readpage_unix_file(vp, page);
}

/* part of truncate_file_body: it is called when truncate is used to make file
   shorter */
static int shorten_file(struct inode *inode, loff_t new_size)
{
	int result;
	struct page *page;
	int padd_from;
	unsigned long index;
	char *kaddr;

	/* all items of ordinary reiser4 file are grouped together. That is why we can use cut_tree. Plan B files (for
	   instance) can not be truncated that simply */
	result =
	    cut_file_items(inode, new_size, 1 /*update_sd */ ,
			   get_key_offset(max_key()), update_file_size);
	if (result)
		return result;

	assert("vs-1105", new_size == inode->i_size);
	if (new_size == 0) {
		set_file_state_empty(inode);
		return 0;
	}

	result = find_file_state(unix_file_inode_data(inode));
	if (result)
		return result;
	if (file_is_built_of_tails(inode))
		/* No need to worry about zeroing last page after new file end */
		return 0;

	padd_from = inode->i_size & (PAGE_CACHE_SIZE - 1);
	if (!padd_from)
		/* file is truncated to page boundary */
		return 0;

	result = reserve_partial_page(tree_by_inode(inode));
	if (result) {
		reiser4_release_reserved(inode->i_sb);
		return result;
	}

	/* last page is partially truncated - zero its content */
	index = (inode->i_size >> PAGE_CACHE_SHIFT);
	page = read_cache_page(inode->i_mapping, index, filler, NULL);
	if (IS_ERR(page)) {
		all_grabbed2free();
		reiser4_release_reserved(inode->i_sb);
		if (likely(PTR_ERR(page) == -EINVAL)) {
			/* looks like file is built of tail items */
			return 0;
		}
		return PTR_ERR(page);
	}
	wait_on_page_locked(page);
	if (!PageUptodate(page)) {
		all_grabbed2free();
		page_cache_release(page);
		reiser4_release_reserved(inode->i_sb);
		return RETERR(-EIO);
	}

	/* if page correspons to hole extent unit - unallocated one will be
	   created here. This is not necessary */
	result = find_or_create_extent(page);

	/* FIXME: cut_file_items has already updated inode. Probably it would
	   be better to update it here when file is really truncated */
	all_grabbed2free();
	if (result) {
		page_cache_release(page);
		reiser4_release_reserved(inode->i_sb);
		return result;
	}

	lock_page(page);
	assert("vs-1066", PageLocked(page));
	kaddr = kmap_atomic(page, KM_USER0);
	memset(kaddr + padd_from, 0, PAGE_CACHE_SIZE - padd_from);
	flush_dcache_page(page);
	kunmap_atomic(kaddr, KM_USER0);
	unlock_page(page);
	page_cache_release(page);
	reiser4_release_reserved(inode->i_sb);
	return 0;
}

static loff_t
write_flow(hint_t *, struct file *, struct inode *, const char __user *buf,
	   loff_t count, loff_t pos, int exclusive);

/* it is called when truncate is used to make file longer and when write
   position is set past real end of file. It appends file which has size
   @cur_size with hole of certain size (@hole_size). It returns 0 on success,
   error code otherwise */
static int
append_hole(hint_t * hint, struct inode *inode, loff_t new_size, int exclusive)
{
	int result;
	loff_t written;
	loff_t hole_size;

	assert("vs-1107", inode->i_size < new_size);

	result = 0;
	hole_size = new_size - inode->i_size;
	written = write_flow(hint, NULL, inode, NULL /*buf */ , hole_size,
			     inode->i_size, exclusive);
	if (written != hole_size) {
		/* return error because file is not expanded as required */
		if (written > 0)
			result = RETERR(-ENOSPC);
		else
			result = written;
	} else {
		assert("vs-1081", inode->i_size == new_size);
	}
	return result;
}

/**
 * truncate_file_body - change length of file
 * @inode: inode of file
 * @new_size: new file length
 *
 * Adjusts items file @inode is built of to match @new_size. It may either cut
 * items or add them to represent a hole at the end of file. The caller has to
 * obtain exclusive access to the file.
 */
static int truncate_file_body(struct inode *inode, loff_t new_size)
{
	int result;

	if (inode->i_size < new_size) {
		hint_t *hint;

		hint = kmalloc(sizeof(*hint), GFP_KERNEL);
		if (hint == NULL)
			return RETERR(-ENOMEM);
		hint_init_zero(hint);
		result = append_hole(hint, inode, new_size,
				     1 /* exclusive access is obtained */ );
		kfree(hint);
	} else
		result = shorten_file(inode, new_size);
	return result;
}

/* plugin->u.write_sd_by_inode = write_sd_by_inode_common */

/* get access hint (seal, coord, key, level) stored in reiser4 private part of
   struct file if it was stored in a previous access to the file */
int load_file_hint(struct file *file, hint_t * hint)
{
	reiser4_file_fsdata *fsdata;

	if (file) {
		fsdata = reiser4_get_file_fsdata(file);
		if (IS_ERR(fsdata))
			return PTR_ERR(fsdata);

		spin_lock_inode(file->f_dentry->d_inode);
		if (seal_is_set(&fsdata->reg.hint.seal)) {
			*hint = fsdata->reg.hint;
			init_lh(&hint->lh);
			hint->ext_coord.lh = &hint->lh;
			spin_unlock_inode(file->f_dentry->d_inode);
			/* force re-validation of the coord on the first
			 * iteration of the read/write loop. */
			hint->ext_coord.valid = 0;
			assert("nikita-19892", coords_equal(&hint->seal.coord1,
							    &hint->ext_coord.
							    coord));
			return 0;
		}
		memset(&fsdata->reg.hint, 0, sizeof(hint_t));
		spin_unlock_inode(file->f_dentry->d_inode);
	}
	hint_init_zero(hint);
	return 0;
}

/* this copies hint for future tree accesses back to reiser4 private part of
   struct file */
void save_file_hint(struct file *file, const hint_t * hint)
{
	reiser4_file_fsdata *fsdata;

	assert("edward-1337", hint != NULL);

	if (!file || !seal_is_set(&hint->seal))
		return;
	fsdata = reiser4_get_file_fsdata(file);
	assert("vs-965", !IS_ERR(fsdata));
	assert("nikita-19891",
	       coords_equal(&hint->seal.coord1, &hint->ext_coord.coord));
	assert("vs-30", hint->lh.owner == NULL);
	spin_lock_inode(file->f_dentry->d_inode);
	fsdata->reg.hint = *hint;
	spin_unlock_inode(file->f_dentry->d_inode);
	return;
}

void unset_hint(hint_t * hint)
{
	assert("vs-1315", hint);
	hint->ext_coord.valid = 0;
	seal_done(&hint->seal);
	done_lh(&hint->lh);
}

/* coord must be set properly. So, that set_hint has nothing to do */
void set_hint(hint_t * hint, const reiser4_key * key, znode_lock_mode mode)
{
	ON_DEBUG(coord_t * coord = &hint->ext_coord.coord);
	assert("vs-1207", WITH_DATA(coord->node, check_coord(coord, key)));

	seal_init(&hint->seal, &hint->ext_coord.coord, key);
	hint->offset = get_key_offset(key);
	hint->mode = mode;
	done_lh(&hint->lh);
}

int hint_is_set(const hint_t * hint)
{
	return seal_is_set(&hint->seal);
}

#if REISER4_DEBUG
static int all_but_offset_key_eq(const reiser4_key * k1, const reiser4_key * k2)
{
	return (get_key_locality(k1) == get_key_locality(k2) &&
		get_key_type(k1) == get_key_type(k2) &&
		get_key_band(k1) == get_key_band(k2) &&
		get_key_ordering(k1) == get_key_ordering(k2) &&
		get_key_objectid(k1) == get_key_objectid(k2));
}
#endif

int
hint_validate(hint_t * hint, const reiser4_key * key, int check_key,
	      znode_lock_mode lock_mode)
{
	if (!hint || !hint_is_set(hint) || hint->mode != lock_mode)
		/* hint either not set or set by different operation */
		return RETERR(-E_REPEAT);

	assert("vs-1277", all_but_offset_key_eq(key, &hint->seal.key));

	if (check_key && get_key_offset(key) != hint->offset)
		/* hint is set for different key */
		return RETERR(-E_REPEAT);

	assert("vs-31", hint->ext_coord.lh == &hint->lh);
	return seal_validate(&hint->seal, &hint->ext_coord.coord, key,
			     hint->ext_coord.lh, lock_mode, ZNODE_LOCK_LOPRI);
}

/* look for place at twig level for extent corresponding to page, call extent's writepage method to create
   unallocated extent if it does not exist yet, initialize jnode, capture page */
int find_or_create_extent(struct page *page)
{
	int result;
	uf_coord_t uf_coord;
	coord_t *coord;
	lock_handle lh;
	reiser4_key key;
	item_plugin *iplug;
	znode *loaded;
	struct inode *inode;

	assert("vs-1065", page->mapping && page->mapping->host);
	inode = page->mapping->host;

	/* get key of first byte of the page */
	key_by_inode_and_offset_common(inode,
				       (loff_t) page->index << PAGE_CACHE_SHIFT,
				       &key);

	init_uf_coord(&uf_coord, &lh);
	coord = &uf_coord.coord;

	result =
	    find_file_item_nohint(coord, &lh, &key, ZNODE_WRITE_LOCK, inode);
	if (IS_CBKERR(result)) {
		done_lh(&lh);
		return result;
	}

	/*coord_clear_iplug(coord); */
	result = zload(coord->node);
	if (result) {
		done_lh(&lh);
		return result;
	}
	loaded = coord->node;

	/* get plugin of extent item */
	iplug = item_plugin_by_id(EXTENT_POINTER_ID);
	result =
	    iplug->s.file.capture(&key, &uf_coord, page,
				  how_to_write(&uf_coord, &key));
	assert("vs-429378", result != -E_REPEAT);
	zrelse(loaded);
	done_lh(&lh);
	return result;
}

/**
 * has_anonymous_pages - check whether inode has pages dirtied via mmap
 * @inode: inode to check
 *
 * Returns true if inode's mapping has dirty pages which do not belong to any
 * atom. Those are either tagged PAGECACHE_TAG_REISER4_MOVED in mapping's page
 * tree or were eflushed and can be found via jnodes tagged
 * EFLUSH_TAG_ANONYMOUS in radix tree of jnodes.
 */
static int has_anonymous_pages(struct inode *inode)
{
	int result;

	read_lock_irq(&inode->i_mapping->tree_lock);
	result = radix_tree_tagged(&inode->i_mapping->page_tree, PAGECACHE_TAG_REISER4_MOVED)
#if REISER4_USE_EFLUSH
		| radix_tree_tagged(jnode_tree_by_inode(inode), EFLUSH_TAG_ANONYMOUS)
#endif
		;
	read_unlock_irq(&inode->i_mapping->tree_lock);
	return result;
}

/**
 * capture_page_and_create_extent -
 * @page: page to be captured
 *
 * Grabs space for extent creation and stat data update and calls function to
 * do actual work.
 */
static int capture_page_and_create_extent(struct page *page)
{
	int result;
	struct inode *inode;

	assert("vs-1084", page->mapping && page->mapping->host);
	inode = page->mapping->host;
	assert("vs-1139", file_is_built_of_extents(inode));
	/* page belongs to file */
	assert("vs-1393",
	       inode->i_size > ((loff_t) page->index << PAGE_CACHE_SHIFT));

	/* page capture may require extent creation (if it does not exist yet)
	   and stat data's update (number of blocks changes on extent
	   creation) */
	grab_space_enable();
	result =
	    reiser4_grab_space(2 *
			       estimate_one_insert_into_item(tree_by_inode
							     (inode)),
			       BA_CAN_COMMIT);
	if (likely(!result))
		result = find_or_create_extent(page);

	all_grabbed2free();
	if (result != 0)
		SetPageError(page);
	return result;
}

/* this is implementation of method commit_write of struct
   address_space_operations for unix file plugin */
int
commit_write_unix_file(struct file *file, struct page *page,
		       unsigned from, unsigned to)
{
	reiser4_context *ctx;
	struct inode *inode;
	int result;

	assert("umka-3101", file != NULL);
	assert("umka-3102", page != NULL);
	assert("umka-3093", PageLocked(page));

	SetPageUptodate(page);

	inode = page->mapping->host;
	ctx = init_context(page->mapping->host->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	page_cache_get(page);
	unlock_page(page);
	result = capture_page_and_create_extent(page);
	lock_page(page);
	page_cache_release(page);

	/* don't commit transaction under inode semaphore */
	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);
	return result;
}

/*
 * Support for "anonymous" pages and jnodes.
 *
 * When file is write-accessed through mmap pages can be dirtied from the user
 * level. In this case kernel is not notified until one of following happens:
 *
 *     (1) msync()
 *
 *     (2) truncate() (either explicit or through unlink)
 *
 *     (3) VM scanner starts reclaiming mapped pages, dirtying them before
 *     starting write-back.
 *
 * As a result of (3) ->writepage may be called on a dirty page without
 * jnode. Such page is called "anonymous" in reiser4. Certain work-loads
 * (iozone) generate huge number of anonymous pages. Emergency flush handles
 * this situation by creating jnode for anonymous page, starting IO on the
 * page, and marking jnode with JNODE_KEEPME bit so that it's not thrown out of
 * memory. Such jnode is also called anonymous.
 *
 * reiser4_sync_sb() method tries to insert anonymous pages and jnodes into
 * tree. This is done by capture_anonymous_*() functions below.
 */

/**
 * capture_anonymous_page - involve page into transaction
 * @pg: page to deal with
 *
 * Takes care that @page has corresponding metadata in the tree, creates jnode
 * for @page and captures it. On success 1 is returned.
 */
static int capture_anonymous_page(struct page *page)
{
	struct address_space *mapping;
	jnode *node;
	int result;

	if (PageWriteback(page))
		/* FIXME: do nothing? */
		return 0;

	mapping = page->mapping;

	lock_page(page);
	/* page is guaranteed to be in the mapping, because we are operating
	   under rw-semaphore. */
	assert("nikita-3336", page->mapping == mapping);
	node = jnode_of_page(page);
	unlock_page(page);
	if (!IS_ERR(node)) {
		result = jload(node);
		assert("nikita-3334", result == 0);
		assert("nikita-3335", jnode_page(node) == page);
		result = capture_page_and_create_extent(page);
		if (result == 0) {
			/*
			 * node has beed captured into atom by
			 * capture_page_and_create_extent(). Atom cannot commit
			 * (because we have open transaction handle), and node
			 * cannot be truncated, because we have non-exclusive
			 * access to the file.
			 */
			assert("nikita-3327", node->atom != NULL);
			result = 1;
		} else
			warning("nikita-3329",
				"Cannot capture anon page: %i", result);
		jrelse(node);
		jput(node);
	} else
		result = PTR_ERR(node);

	return result;
}

/**
 * capture_anonymous_pages - find and capture pages dirtied via mmap
 * @mapping: address space where to look for pages
 * @index: start index
 * @to_capture: maximum number of pages to capture
 *
 * Looks for pages tagged REISER4_MOVED starting from the *@index-th page,
 * captures (involves into atom) them, returns number of captured pages,
 * updates @index to next page after the last captured one.
 */
static int
capture_anonymous_pages(struct address_space *mapping, pgoff_t *index,
			unsigned int to_capture)
{
	int result;
	struct pagevec pvec;
	unsigned int i, count;
	int nr;

	pagevec_init(&pvec, 0);
	count = min(pagevec_space(&pvec), to_capture);
	nr = 0;

	/* find pages tagged MOVED */
	write_lock_irq(&mapping->tree_lock);
	pvec.nr = radix_tree_gang_lookup_tag(&mapping->page_tree,
					     (void **)pvec.pages, *index, count,
					     PAGECACHE_TAG_REISER4_MOVED);
	if (pagevec_count(&pvec) == 0) {
		/* there are no pages tagged MOVED in mapping->page_tree
		   starting from *index */
		write_unlock_irq(&mapping->tree_lock);
		*index = (pgoff_t)-1;
		return 0;
	}

	/* clear tag for all found pages */
	for (i = 0; i < pagevec_count(&pvec); i++) {
		void *p;

		page_cache_get(pvec.pages[i]);
		p = radix_tree_tag_clear(&mapping->page_tree, pvec.pages[i]->index,
					 PAGECACHE_TAG_REISER4_MOVED);
		assert("vs-49", p == pvec.pages[i]);
	}
	write_unlock_irq(&mapping->tree_lock);


	*index = pvec.pages[i - 1]->index + 1;

	for (i = 0; i < pagevec_count(&pvec); i++) {
		/* tag PAGECACHE_TAG_REISER4_MOVED will be cleared by
		   set_page_dirty_internal which is called when jnode is
		   captured */
		result = capture_anonymous_page(pvec.pages[i]);
		if (result == 1)
			nr++;
		else {
			if (result < 0) {
				warning("vs-1454",
					"failed to capture page: "
					"result=%d, captured=%d)\n",
					result, i);

				/* set MOVED tag to all pages which
				   left not captured */
				write_lock_irq(&mapping->tree_lock);
				for (; i < pagevec_count(&pvec); i ++) {
					radix_tree_tag_set(&mapping->page_tree,
							   pvec.pages[i]->index,
							   PAGECACHE_TAG_REISER4_MOVED);
				}
				write_unlock_irq(&mapping->tree_lock);

				pagevec_release(&pvec);
				return result;
			} else {
				/* result == 0. capture_anonymous_page returns
				   0 for Writeback-ed page. Set MOVED tag on
				   that page */
				write_lock_irq(&mapping->tree_lock);
				radix_tree_tag_set(&mapping->page_tree,
						   pvec.pages[i]->index,
						   PAGECACHE_TAG_REISER4_MOVED);
				write_unlock_irq(&mapping->tree_lock);
				if (i == 0)
					*index = pvec.pages[0]->index;
				else
					*index = pvec.pages[i - 1]->index + 1;
			}
		}
	}
	pagevec_release(&pvec);
	return nr;
}

/**
 * capture_anonymous_jnodes - find and capture anonymous jnodes
 * @mapping: address space where to look for jnodes
 * @from: start index
 * @to: end index
 * @to_capture: maximum number of jnodes to capture
 *
 * Looks for jnodes tagged EFLUSH_TAG_ANONYMOUS in inode's tree of jnodes in
 * the range of indexes @from-@to and captures them, returns number of captured
 * jnodes, updates @from to next jnode after the last captured one.
 */
static int
capture_anonymous_jnodes(struct address_space *mapping,
			 pgoff_t *from, pgoff_t to, int to_capture)
{
#if REISER4_USE_EFLUSH
	int found_jnodes;
	int count;
	int nr;
	int i;
	int result;
	jnode *jvec[PAGEVEC_SIZE];
	reiser4_tree *tree;
	struct radix_tree_root *root;

	count = min(PAGEVEC_SIZE, to_capture);
	nr = 0;
	result = 0;

	tree = &get_super_private(mapping->host->i_sb)->tree;
	root = jnode_tree_by_inode(mapping->host);

	write_lock_irq(&mapping->tree_lock);

	found_jnodes =
	    radix_tree_gang_lookup_tag(root, (void **)&jvec, *from, count,
				       EFLUSH_TAG_ANONYMOUS);
	if (found_jnodes == 0) {
		/* there are no anonymous jnodes from index @from down to the
		   end of file */
		write_unlock_irq(&mapping->tree_lock);
		*from = to;
		return 0;
	}

	for (i = 0; i < found_jnodes; i++) {
		if (index_jnode(jvec[i]) < to) {
			void *p;

			jref(jvec[i]);
			p = radix_tree_tag_clear(root, index_jnode(jvec[i]),
						 EFLUSH_TAG_ANONYMOUS);
			assert("", p == jvec[i]);

			/* if page is tagged PAGECACHE_TAG_REISER4_MOVED it has
			   to be untagged because we are about to capture it */
			radix_tree_tag_clear(&mapping->page_tree, index_jnode(jvec[i]),
					     PAGECACHE_TAG_REISER4_MOVED);
		} else {
			found_jnodes = i;
			break;
		}
	}
	write_unlock_irq(&mapping->tree_lock);

	if (found_jnodes == 0) {
		/* there are no anonymous jnodes in the given range of
		   indexes */
		*from = to;
		return 0;
	}

	/* there are anonymous jnodes from given range */

	/* start i/o for eflushed nodes */
	for (i = 0; i < found_jnodes; i++)
		jstartio(jvec[i]);

	*from = index_jnode(jvec[found_jnodes - 1]) + 1;

	for (i = 0; i < found_jnodes; i++) {
		result = jload(jvec[i]);
		if (result == 0) {
			result = capture_anonymous_page(jnode_page(jvec[i]));
			if (result == 1)
				nr++;
			else if (result < 0) {
				jrelse(jvec[i]);
				warning("nikita-3328",
					"failed for anonymous jnode: result=%i, captured %d\n",
					result, i);
				/* set ANONYMOUS tag to all jnodes which left
				   not captured */
				write_lock_irq(&mapping->tree_lock);
				for (; i < found_jnodes; i ++)
					/* page should be in the mapping. Do
					 * not tag jnode back as anonymous
					 * because it is not now (after
					 * jload) */
					radix_tree_tag_set(&mapping->page_tree,
							   index_jnode(jvec[i]),
							   PAGECACHE_TAG_REISER4_MOVED);
				write_unlock_irq(&mapping->tree_lock);
				break;
			} else {
				/* result == 0. capture_anonymous_page returns
				   0 for Writeback-ed page. Set ANONYMOUS tag
				   on that jnode */
				warning("nikita-33281",
					"anonymous jnode in writeback: (%lu %lu)\n",
					mapping->host->i_ino, index_jnode(jvec[i]));
				write_lock_irq(&mapping->tree_lock);
				radix_tree_tag_set(&mapping->page_tree,
						   index_jnode(jvec[i]),
						   PAGECACHE_TAG_REISER4_MOVED);
				write_unlock_irq(&mapping->tree_lock);
				if (i == 0)
					*from = index_jnode(jvec[0]);
				else
					*from = index_jnode(jvec[i - 1]) + 1;
			}
			jrelse(jvec[i]);
		} else {
			warning("vs-1454",
				"jload for anonymous jnode failed: result=%i, captured %d\n",
				result, i);
			break;
		}
	}

	for (i = 0; i < found_jnodes; i++)
		jput(jvec[i]);
	if (result)
		return result;
	return nr;
#else				/* REISER4_USE_EFLUSH */
	*from = to;
	return 0;
#endif
}

/*
 * Commit atom of the jnode of a page.
 */
static int sync_page(struct page *page)
{
	int result;
	do {
		jnode *node;
		txn_atom *atom;

		lock_page(page);
		node = jprivate(page);
		if (node != NULL) {
			spin_lock_jnode(node);
			atom = jnode_get_atom(node);
			spin_unlock_jnode(node);
		} else
			atom = NULL;
		unlock_page(page);
		result = sync_atom(atom);
	} while (result == -E_REPEAT);
	/*
	 * ZAM-FIXME-HANS: document the logic of this loop, is it just to
	 * handle the case where more pages get added to the atom while we are
	 * syncing it?
	 */
	assert("nikita-3485", ergo(result == 0,
				   get_current_context()->trans->atom == NULL));
	return result;
}

/*
 * Commit atoms of pages on @pages list.
 * call sync_page for each page from mapping's page tree
 */
static int sync_page_list(struct inode *inode)
{
	int result;
	struct address_space *mapping;
	unsigned long from;	/* start index for radix_tree_gang_lookup */
	unsigned int found;	/* return value for radix_tree_gang_lookup */

	mapping = inode->i_mapping;
	from = 0;
	result = 0;
	read_lock_irq(&mapping->tree_lock);
	while (result == 0) {
		struct page *page;

		found =
		    radix_tree_gang_lookup(&mapping->page_tree, (void **)&page,
					   from, 1);
		assert("", found < 2);
		if (found == 0)
			break;

		/* page may not leave radix tree because it is protected from truncating by inode->i_sem downed by
		   sys_fsync */
		page_cache_get(page);
		read_unlock_irq(&mapping->tree_lock);

		from = page->index + 1;

		result = sync_page(page);

		page_cache_release(page);
		read_lock_irq(&mapping->tree_lock);
	}

	read_unlock_irq(&mapping->tree_lock);
	return result;
}

static int commit_file_atoms(struct inode *inode)
{
	int result;
	unix_file_info_t *uf_info;

	/* close current transaction */
	txn_restart_current();

	uf_info = unix_file_inode_data(inode);

	/*
	 * finish extent<->tail conversion if necessary
	 */
	get_exclusive_access(uf_info);
	if (inode_get_flag(inode, REISER4_PART_CONV)) {
		result = finish_conversion(inode);
		if (result != 0) {
			drop_exclusive_access(uf_info);
			return result;
		}
	}

	/*
	 * find what items file is made from
	 */
	result = find_file_state(uf_info);
	drop_exclusive_access(uf_info);
	if (result != 0)
		return result;

	/*
	 * file state cannot change because we are under ->i_sem
	 */
	switch (uf_info->container) {
	case UF_CONTAINER_EXTENTS:
		/* find_file_state might open join an atom */
		txn_restart_current();
		result =
		    /*
		     * when we are called by
		     * filemap_fdatawrite->
		     *    do_writepages()->
		     *       reiser4_writepages()
		     *
		     * inode->i_mapping->dirty_pages are spices into
		     * ->io_pages, leaving ->dirty_pages dirty.
		     *
		     * When we are called from
		     * reiser4_fsync()->sync_unix_file(), we have to
		     * commit atoms of all pages on the ->dirty_list.
		     *
		     * So for simplicity we just commit ->io_pages and
		     * ->dirty_pages.
		     */
		    sync_page_list(inode);
		break;
	case UF_CONTAINER_TAILS:
		/*
		 * NOTE-NIKITA probably we can be smarter for tails. For now
		 * just commit all existing atoms.
		 */
		result = txnmgr_force_commit_all(inode->i_sb, 0);
		break;
	case UF_CONTAINER_EMPTY:
		result = 0;
		break;
	case UF_CONTAINER_UNKNOWN:
	default:
		result = -EIO;
		break;
	}

	/*
	 * commit current transaction: there can be captured nodes from
	 * find_file_state() and finish_conversion().
	 */
	txn_restart_current();
	return result;
}

/* reiser4 writepages() address space operation this captures anonymous pages
   and anonymous jnodes. Anonymous pages are pages which are dirtied via
   mmapping. Anonymous jnodes are ones which were created by reiser4_writepage
 */
int
writepages_unix_file(struct address_space *mapping,
		     struct writeback_control *wbc)
{
	int result;
	unix_file_info_t *uf_info;
	pgoff_t pindex, jindex, nr_pages;
	long to_capture;
	struct inode *inode;

	inode = mapping->host;
	if (!has_anonymous_pages(inode)) {
		result = 0;
		goto end;
	}
	jindex = pindex = wbc->start >> PAGE_CACHE_SHIFT;
	result = 0;
	nr_pages =
	    (i_size_read(inode) + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
	uf_info = unix_file_inode_data(inode);
	do {
		reiser4_context *ctx;
		int dont_get_nea;

		if (wbc->sync_mode != WB_SYNC_ALL)
			to_capture = min(wbc->nr_to_write, CAPTURE_APAGE_BURST);
		else
			to_capture = CAPTURE_APAGE_BURST;

		ctx = init_context(inode->i_sb);
		if (IS_ERR(ctx)) {
			result = PTR_ERR(ctx);
			break;
		}
		/* avoid recursive calls to ->sync_inodes */
		ctx->nobalance = 1;
		assert("zam-760", lock_stack_isclean(get_current_lock_stack()));
		assert("", LOCK_CNT_NIL(inode_sem_w));
		assert("", LOCK_CNT_NIL(inode_sem_r));

		txn_restart_current();

		/*
		 * suppose thread T1 has got nonexlusive access (NEA) on a file
		 * F, asked entd to flush to reclaim some memory and waits
		 * until entd completes. Another thread T2 tries to get
		 * exclusive access to file F. Then entd will deadlock on
		 * getting NEA to file F (because read-down request get blocked
		 * if there is write request in a queue in linux read-write
		 * semaphore implementation). To avoid this problem we make
		 * entd to not get NEA to F if it is obtained by T1.
		 */
		dont_get_nea = 0;
		if (get_current_context()->entd) {
			entd_context *ent = get_entd_context(inode->i_sb);

			if (ent->cur_request->caller != NULL &&
			    mapping == ent->cur_request->caller->vp)
				/*
				 * process which is waiting for entd has got
				 * NEA on a file we are about to capture pages
				 * of. Skip getting NEA therefore.
				 */
				dont_get_nea = 1;
		}
		if (dont_get_nea == 0)
			get_nonexclusive_access(uf_info, 0);
		while (to_capture > 0) {
			pgoff_t start;

			assert("vs-1727", jindex <= pindex);
			if (pindex == jindex) {
				start = pindex;
				result =
				    capture_anonymous_pages(inode->i_mapping,
							    &pindex,
							    to_capture);
				if (result <= 0)
					break;
				to_capture -= result;
				wbc->nr_to_write -= result;
				if (start + result == pindex) {
					jindex = pindex;
					continue;
				}
				if (to_capture <= 0)
					break;
			}
			/* deal with anonymous jnodes between jindex and pindex */
			result =
			    capture_anonymous_jnodes(inode->i_mapping, &jindex,
						     pindex, to_capture);
			if (result < 0)
				break;
			to_capture -= result;
			get_current_context()->nr_captured += result;

			if (jindex == (pgoff_t) - 1) {
				assert("vs-1728", pindex == (pgoff_t) - 1);
				break;
			}
		}
		if (to_capture <= 0)
			/* there may be left more pages */
			__mark_inode_dirty(inode, I_DIRTY_PAGES);

		if (dont_get_nea == 0)
			drop_nonexclusive_access(uf_info);
		if (result < 0) {
			/* error happened */
			reiser4_exit_context(ctx);
			return result;
		}
		if (wbc->sync_mode != WB_SYNC_ALL) {
			reiser4_exit_context(ctx);
			return 0;
		}
		result = commit_file_atoms(inode);
		reiser4_exit_context(ctx);
		if (pindex >= nr_pages && jindex == pindex)
			break;
	} while (1);

      end:
	if (is_in_reiser4_context()) {
		if (get_current_context()->nr_captured >= CAPTURE_APAGE_BURST) {
			/* there are already pages to flush, flush them out, do
			   not delay until end of reiser4_sync_inodes */
			writeout(inode->i_sb, wbc);
			get_current_context()->nr_captured = 0;
		}
	}
	return result;
}

/*
 * ->sync() method for unix file.
 *
 * We are trying to be smart here. Instead of committing all atoms (original
 * solution), we scan dirty pages of this file and commit all atoms they are
 * part of.
 *
 * Situation is complicated by anonymous pages: i.e., extent-less pages
 * dirtied through mmap. Fortunately sys_fsync() first calls
 * filemap_fdatawrite() that will ultimately call reiser4_writepages(), insert
 * all missing extents and capture anonymous pages.
 */
int sync_unix_file(struct file *file, struct dentry *dentry, int datasync)
{
	int result;
	reiser4_context *ctx;

	ctx = init_context(dentry->d_inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	assert("nikita-3486", ctx->trans->atom == NULL);
	result = commit_file_atoms(dentry->d_inode);
	assert("nikita-3484", ergo(result == 0, ctx->trans->atom == NULL));
	if (result == 0 && !datasync) {
		do {
			/* commit "meta-data"---stat data in our case */
			lock_handle lh;
			coord_t coord;
			reiser4_key key;

			coord_init_zero(&coord);
			init_lh(&lh);
			/* locate stat-data in a tree and return with znode
			 * locked */
			result =
			    locate_inode_sd(dentry->d_inode, &key, &coord, &lh);
			if (result == 0) {
				jnode *node;
				txn_atom *atom;

				node = jref(ZJNODE(coord.node));
				done_lh(&lh);
				txn_restart_current();
				spin_lock_jnode(node);
				atom = jnode_get_atom(node);
				spin_unlock_jnode(node);
				result = sync_atom(atom);
				jput(node);
			} else
				done_lh(&lh);
		} while (result == -E_REPEAT);
	}
	reiser4_exit_context(ctx);
	return result;
}

/* plugin->u.file.readpage
   page must be not out of file. This is called either via page fault and in
   that case vp is struct file *file, or on truncate when last page of a file
   is to be read to perform its partial truncate and in that case vp is 0
*/
int readpage_unix_file(struct file *file, struct page *page)
{
	reiser4_context *ctx;
	int result;
	struct inode *inode;
	reiser4_key key;
	item_plugin *iplug;
	hint_t *hint;
	lock_handle *lh;
	coord_t *coord;

	assert("vs-1062", PageLocked(page));
	assert("vs-976", !PageUptodate(page));
	assert("vs-1061", page->mapping && page->mapping->host);
	assert("vs-1078",
	       (page->mapping->host->i_size >
		((loff_t) page->index << PAGE_CACHE_SHIFT)));

	inode = page->mapping->host;
	ctx = init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	hint = kmalloc(sizeof(*hint), GFP_KERNEL);
	if (hint == NULL) {
		reiser4_exit_context(ctx);
		return RETERR(-ENOMEM);
	}

	result = load_file_hint(file, hint);
	if (result) {
		kfree(hint);
		reiser4_exit_context(ctx);
		return result;
	}
	lh = &hint->lh;

	/* get key of first byte of the page */
	key_by_inode_and_offset_common(inode,
				       (loff_t) page->index << PAGE_CACHE_SHIFT,
				       &key);

	/* look for file metadata corresponding to first byte of page */
	unlock_page(page);
	result = find_file_item(hint, &key, ZNODE_READ_LOCK, inode);
	lock_page(page);
	if (result != CBK_COORD_FOUND) {
		/* this indicates file corruption */
		done_lh(lh);
		unlock_page(page);
		kfree(hint);
		reiser4_exit_context(ctx);
		return result;
	}

	if (PageUptodate(page)) {
		done_lh(lh);
		unlock_page(page);
		kfree(hint);
		reiser4_exit_context(ctx);
		return 0;
	}

	coord = &hint->ext_coord.coord;
	result = zload(coord->node);
	if (result) {
		done_lh(lh);
		unlock_page(page);
		kfree(hint);
		reiser4_exit_context(ctx);
		return result;
	}

	if (hint->ext_coord.valid == 0)
		validate_extended_coord(&hint->ext_coord,
					(loff_t) page->
					index << PAGE_CACHE_SHIFT);

	if (!coord_is_existing_unit(coord)) {
		/* this indicates corruption */
		warning("vs-280",
			"Looking for page %lu of file %llu (size %lli). "
			"No file items found (%d). File is corrupted?\n",
			page->index, (unsigned long long)get_inode_oid(inode),
			inode->i_size, result);

		zrelse(coord->node);
		done_lh(lh);
		unlock_page(page);
		kfree(hint);
		reiser4_exit_context(ctx);
		return RETERR(-EIO);
	}

	/* get plugin of found item or use plugin if extent if there are no
	   one */
	iplug = item_plugin_by_coord(coord);
	if (iplug->s.file.readpage)
		result = iplug->s.file.readpage(coord, page);
	else
		result = RETERR(-EINVAL);

	if (!result) {
		set_key_offset(&key,
			       (loff_t) (page->index + 1) << PAGE_CACHE_SHIFT);
		/* FIXME should call set_hint() */
		unset_hint(hint);
	} else {
		unlock_page(page);
		unset_hint(hint);
	}
	zrelse(coord->node);
	done_lh(lh);

	save_file_hint(file, hint);
	kfree(hint);

	assert("vs-979",
	       ergo(result == 0, (PageLocked(page) || PageUptodate(page))));
	assert("vs-9791", ergo(result != 0, !PageLocked(page)));

	reiser4_exit_context(ctx);
	return result;
}

/* returns 1 if file of that size (@new_size) has to be stored in unformatted
   nodes */
/* Audited by: green(2002.06.15) */
static int should_have_notail(const unix_file_info_t * uf_info, loff_t new_size)
{
	if (!uf_info->tplug)
		return 1;
	return !uf_info->tplug->have_tail(unix_file_info_to_inode(uf_info),
					  new_size);

}

static reiser4_block_nr unix_file_estimate_read(struct inode *inode,
						loff_t count UNUSED_ARG)
{
	/* We should reserve one block, because of updating of the stat data
	   item */
	assert("vs-1249",
	       inode_file_plugin(inode)->estimate.update ==
	       estimate_update_common);
	return estimate_update_common(inode);
}

#define NR_PAGES_TO_PIN 8

static int
get_nr_pages_nr_bytes(unsigned long addr, size_t count, int *nr_pages)
{
	int nr_bytes;

	/* number of pages through which count bytes starting of address addr
	   are spread */
	*nr_pages = ((addr + count + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT) -
	    (addr >> PAGE_CACHE_SHIFT);
	if (*nr_pages > NR_PAGES_TO_PIN) {
		*nr_pages = NR_PAGES_TO_PIN;
		nr_bytes =
		    (*nr_pages * PAGE_CACHE_SIZE) -
		    (addr & (PAGE_CACHE_SIZE - 1));
	} else
		nr_bytes = count;

	return nr_bytes;
}

static size_t adjust_nr_bytes(unsigned long addr, size_t count, int nr_pages)
{
	if (count > nr_pages * PAGE_CACHE_SIZE)
		return (nr_pages * PAGE_CACHE_SIZE) -
		    (addr & (PAGE_CACHE_SIZE - 1));
	return count;
}

static int
reiser4_get_user_pages(struct page **pages, unsigned long addr, int nr_pages,
		       int rw)
{
	down_read(&current->mm->mmap_sem);
	nr_pages = get_user_pages(current, current->mm, addr,
				  nr_pages, (rw == READ), 0, pages, NULL);
	up_read(&current->mm->mmap_sem);
	return nr_pages;
}

static void reiser4_put_user_pages(struct page **pages, int nr_pages)
{
	int i;

	for (i = 0; i < nr_pages; i++)
		page_cache_release(pages[i]);
}

/* this is called with nonexclusive access obtained, file's container can not change */
static size_t read_file(hint_t * hint, struct file *file,	/* file to read from to */
			char __user *buf,	/* address of user-space buffer */
			size_t count,	/* number of bytes to read */
			loff_t * off)
{
	int result;
	struct inode *inode;
	flow_t flow;
	int (*read_f) (struct file *, flow_t *, hint_t *);
	coord_t *coord;
	znode *loaded;

	inode = file->f_dentry->d_inode;

	/* build flow */
	assert("vs-1250",
	       inode_file_plugin(inode)->flow_by_inode ==
	       flow_by_inode_unix_file);
	result =
	    flow_by_inode_unix_file(inode, buf, 1 /* user space */ , count,
				    *off, READ_OP, &flow);
	if (unlikely(result))
		return result;

	/* get seal and coord sealed with it from reiser4 private data
	   of struct file.  The coord will tell us where our last read
	   of this file finished, and the seal will help to determine
	   if that location is still valid.
	 */
	coord = &hint->ext_coord.coord;
	while (flow.length && result == 0) {
		result =
			find_file_item(hint, &flow.key, ZNODE_READ_LOCK, inode);
		if (cbk_errored(result))
			/* error happened */
			break;

		if (coord->between != AT_UNIT)
			/* there were no items corresponding to given offset */
			break;

		loaded = coord->node;
		result = zload(loaded);
		if (unlikely(result))
			break;

		if (hint->ext_coord.valid == 0)
			validate_extended_coord(&hint->ext_coord,
						get_key_offset(&flow.key));

		assert("vs-4", hint->ext_coord.valid == 1);
		assert("vs-33", hint->ext_coord.lh == &hint->lh);
		/* call item's read method */
		read_f = item_plugin_by_coord(coord)->s.file.read;
		result = read_f(file, &flow, hint);
		zrelse(loaded);
		done_lh(hint->ext_coord.lh);
	}

	return (count - flow.length) ? (count - flow.length) : result;
}

static int is_user_space(const char __user *buf)
{
	return (unsigned long)buf < PAGE_OFFSET;
}

/**
 * read_unix_file - read of struct file_operations
 * @file: file to read from
 * @buf: address of user-space buffer
 * @read_amount: number of bytes to read
 * @off: position in file to read from
 *
 * This is implementation of vfs's read method of struct file_operations for
 * unix file plugin.
 */
ssize_t
read_unix_file(struct file *file, char __user *buf, size_t read_amount,
	       loff_t *off)
{
	reiser4_context *ctx;
	int result;
	struct inode *inode;
	hint_t *hint;
	unix_file_info_t *uf_info;
	struct page *pages[NR_PAGES_TO_PIN];
	int nr_pages;
	size_t count, read, left;
	reiser4_block_nr needed;
	loff_t size;
	int user_space;

	if (unlikely(read_amount == 0))
		return 0;

	assert("umka-072", file != NULL);
	assert("umka-074", off != NULL);
	inode = file->f_dentry->d_inode;
	assert("vs-972", !inode_get_flag(inode, REISER4_NO_SD));

	ctx = init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	hint = kmalloc(sizeof(*hint), GFP_KERNEL);
	if (hint == NULL) {
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		return RETERR(-ENOMEM);
	}

	result = load_file_hint(file, hint);
	if (result) {
		kfree(hint);
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		return result;
	}

	left = read_amount;
	count = 0;
	user_space = is_user_space(buf);
	nr_pages = 0;
	uf_info = unix_file_inode_data(inode);
	while (left > 0) {
		unsigned long addr;
		size_t to_read;

		addr = (unsigned long)buf;
		txn_restart_current();

		size = i_size_read(inode);
		if (*off >= size)
			/* position to read from is past the end of file */
			break;
		if (*off + left > size)
			left = size - *off;

		if (user_space) {
			to_read = get_nr_pages_nr_bytes(addr, left, &nr_pages);
			nr_pages =
			    reiser4_get_user_pages(pages, addr, nr_pages, READ);
			if (nr_pages < 0) {
				result = nr_pages;
				break;
			}
			to_read = adjust_nr_bytes(addr, to_read, nr_pages);
			/* get_user_pages might create a transaction */
			txn_restart_current();
		} else
			to_read = left;

		get_nonexclusive_access(uf_info, 0);

		/* define more precisely read size now when filesize can not change */
		if (*off >= inode->i_size) {
			if (user_space)
				reiser4_put_user_pages(pages, nr_pages);

			/* position to read from is past the end of file */
			drop_nonexclusive_access(uf_info);
			break;
		}
		if (*off + left > inode->i_size)
			left = inode->i_size - *off;
		if (*off + to_read > inode->i_size)
			to_read = inode->i_size - *off;

		assert("vs-1706", to_read <= left);
		read = read_file(hint, file, buf, to_read, off);

		if (user_space)
			reiser4_put_user_pages(pages, nr_pages);

		drop_nonexclusive_access(uf_info);

		if (read < 0) {
			result = read;
			break;
		}
		left -= read;
		buf += read;

		/* update position in a file */
		*off += read;
		/* total number of read bytes */
		count += read;
	}
	save_file_hint(file, hint);
	kfree(hint);

	if (count) {
		/*
		 * something was read. Grab space for stat data update and
		 * update atime
		 */
		needed = unix_file_estimate_read(inode, read_amount);
		result = reiser4_grab_space_force(needed, BA_CAN_COMMIT);
		if (result == 0)
			update_atime(inode);
		else
			warning("", "failed to grab space for atime update");
	}

	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);

	/* return number of read bytes or error code if nothing is read */
	return count ? count : result;
}

typedef int (*write_f_t) (struct inode *, flow_t *, hint_t *, int grabbed,
			  write_mode_t);

/* This searches for write position in the tree and calls write method of
   appropriate item to actually copy user data into filesystem. This loops
   until all the data from flow @f are written to a file. */
static loff_t
append_and_or_overwrite(hint_t * hint, struct file *file, struct inode *inode,
			flow_t * flow,
			int exclusive
			/* if 1 - exclusive access on a file is obtained */ )
{
	int result;
	loff_t to_write;
	write_f_t write_f;
	file_container_t cur_container, new_container;
	znode *loaded;
	unix_file_info_t *uf_info;

	assert("nikita-3031", schedulable());
	assert("vs-1109", get_current_context()->grabbed_blocks == 0);
	assert("vs-1708", hint != NULL);

	init_lh(&hint->lh);

	result = 0;
	uf_info = unix_file_inode_data(inode);

	to_write = flow->length;
	while (flow->length) {

		assert("vs-1123", get_current_context()->grabbed_blocks == 0);

		if (to_write == flow->length) {
			/* it may happend that find_file_item will have to insert empty node to the tree (empty leaf
			   node between two extent items) */
			result =
			    reiser4_grab_space_force(1 +
						     estimate_one_insert_item
						     (tree_by_inode(inode)), 0);
			if (result)
				return result;
		}
		/* when hint is set - hint's coord matches seal's coord */
		assert("nikita-19894",
		       !hint_is_set(hint) ||
		       coords_equal(&hint->seal.coord1,
				    &hint->ext_coord.coord));

		/* look for file's metadata (extent or tail item) corresponding to position we write to */
		result = find_file_item(hint, &flow->key, ZNODE_WRITE_LOCK,
					inode);
		all_grabbed2free();
		if (IS_CBKERR(result)) {
			/* error occurred */
			done_lh(&hint->lh);
			return result;
		}
		assert("vs-32", hint->lh.node == hint->ext_coord.coord.node);
		cur_container = uf_info->container;
		switch (cur_container) {
		case UF_CONTAINER_EMPTY:
			assert("vs-1196", get_key_offset(&flow->key) == 0);
			if (should_have_notail
			    (uf_info,
			     get_key_offset(&flow->key) + flow->length)) {
				new_container = UF_CONTAINER_EXTENTS;
				write_f =
				    item_plugin_by_id(EXTENT_POINTER_ID)->s.
				    file.write;
			} else {
				new_container = UF_CONTAINER_TAILS;
				write_f =
				    item_plugin_by_id(FORMATTING_ID)->s.file.
				    write;
			}
			break;

		case UF_CONTAINER_EXTENTS:
			write_f =
			    item_plugin_by_id(EXTENT_POINTER_ID)->s.file.write;
			new_container = cur_container;
			break;

		case UF_CONTAINER_TAILS:
			if (should_have_notail
			    (uf_info,
			     get_key_offset(&flow->key) + flow->length)) {
				done_lh(&hint->lh);
				if (!exclusive) {
					drop_nonexclusive_access(uf_info);
					txn_restart_current();
					get_exclusive_access(uf_info);
				}
				result = tail2extent(uf_info);
				if (!exclusive) {
					drop_exclusive_access(uf_info);
					txn_restart_current();
					get_nonexclusive_access(uf_info, 0);
				}
				if (result)
					return result;
				all_grabbed2free();
				unset_hint(hint);
				continue;
			}
			write_f =
			    item_plugin_by_id(FORMATTING_ID)->s.file.write;
			new_container = cur_container;
			break;

		default:
			done_lh(&hint->lh);
			return RETERR(-EIO);
		}

		result = zload(hint->lh.node);
		if (result) {
			done_lh(&hint->lh);
			return result;
		}
		loaded = hint->lh.node;
		assert("vs-11", hint->ext_coord.coord.node == loaded);
		result = write_f(inode, flow, hint, 0 /* not grabbed */ ,
				 how_to_write(&hint->ext_coord, &flow->key));

		assert("nikita-3142",
		       get_current_context()->grabbed_blocks == 0);
		/* seal has either to be not set to set properly */
		assert("nikita-19893",
		       ((!hint_is_set(hint) && hint->ext_coord.valid == 0) ||
			(coords_equal
			 (&hint->seal.coord1, &hint->ext_coord.coord)
			 && keyeq(&flow->key, &hint->seal.key))));

		if (cur_container == UF_CONTAINER_EMPTY
		    && to_write != flow->length) {
			/* file was empty and we have written something and we are having exclusive access to the file -
			   change file state */
			assert("vs-1195",
			       (new_container == UF_CONTAINER_TAILS
				|| new_container == UF_CONTAINER_EXTENTS));
			uf_info->container = new_container;
		}
		zrelse(loaded);
		done_lh(&hint->lh);
		if (result && result != -E_REPEAT && result != -E_DEADLOCK)
			break;
		preempt_point();
	}

	/* if nothing were written - there must be an error */
	assert("vs-951", ergo((to_write == flow->length), result < 0));
	assert("vs-1110", get_current_context()->grabbed_blocks == 0);

	return (to_write - flow->length) ? (to_write - flow->length) : result;
}

/* make flow and write data (@buf) to the file. If @buf == 0 - hole of size @count will be created. This is called with
   uf_info->latch either read- or write-locked */
static loff_t
write_flow(hint_t * hint, struct file *file, struct inode *inode,
	   const char __user *buf, loff_t count, loff_t pos, int exclusive)
{
	int result;
	flow_t flow;

	assert("vs-1251",
	       inode_file_plugin(inode)->flow_by_inode ==
	       flow_by_inode_unix_file);

	result = flow_by_inode_unix_file(inode,
					 buf, 1 /* user space */ ,
					 count, pos, WRITE_OP, &flow);
	if (result)
		return result;

	return append_and_or_overwrite(hint, file, inode, &flow, exclusive);
}

static struct page *unix_file_filemap_nopage(struct vm_area_struct *area,
					     unsigned long address, int *unused)
{
	struct page *page;
	struct inode *inode;
	reiser4_context *ctx;

	inode = area->vm_file->f_dentry->d_inode;
	ctx = init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return (struct page *)ctx;

	/* block filemap_nopage if copy on capture is processing with a node of this file */
	down_read(&reiser4_inode_data(inode)->coc_sem);
	/* second argument is to note that current atom may exist */
	get_nonexclusive_access(unix_file_inode_data(inode), 1);

	page = filemap_nopage(area, address, NULL);

	drop_nonexclusive_access(unix_file_inode_data(inode));
	up_read(&reiser4_inode_data(inode)->coc_sem);

	reiser4_exit_context(ctx);
	return page;
}

static struct vm_operations_struct unix_file_vm_ops = {
	.nopage = unix_file_filemap_nopage,
};

/* This function takes care about @file's pages. First of all it checks if
   filesystems readonly and if so gets out. Otherwise, it throws out all
   pages of file if it was mapped for read and going to be mapped for write
   and consists of tails. This is done in order to not manage few copies
   of the data (first in page cache and second one in tails them selves)
   for the case of mapping files consisting tails.

   Here also tail2extent conversion is performed if it is allowed and file
   is going to be written or mapped for write. This functions may be called
   from write_unix_file() or mmap_unix_file(). */
static int check_pages_unix_file(struct inode *inode)
{
	reiser4_invalidate_pages(inode->i_mapping, 0,
				 (inode->i_size + PAGE_CACHE_SIZE -
				  1) >> PAGE_CACHE_SHIFT, 0);
	return unpack(inode, 0 /* not forever */ );
}

/**
 * mmap_unix_file - mmap of struct file_operations
 * @file: file to mmap
 * @vma:
 *
 * This is implementation of vfs's mmap method of struct file_operations for
 * unix file plugin. It converts file to extent if necessary. Sets
 * reiser4_inode's flag - REISER4_HAS_MMAP.
 */
int mmap_unix_file(struct file *file, struct vm_area_struct *vma)
{
	reiser4_context *ctx;
	int result;
	struct inode *inode;
	unix_file_info_t *uf_info;
	reiser4_block_nr needed;

	inode = file->f_dentry->d_inode;
	ctx = init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	uf_info = unix_file_inode_data(inode);

	down(&uf_info->write);
	get_exclusive_access(uf_info);

	if (!IS_RDONLY(inode) && (vma->vm_flags & (VM_MAYWRITE | VM_SHARED))) {
		/*
		 * we need file built of extent items. If it is still built of
		 * tail items we have to convert it. Find what items the file
		 * is built of
		 */
		result = finish_conversion(inode);
		if (result) {
			drop_exclusive_access(uf_info);
			up(&uf_info->write);
			reiser4_exit_context(ctx);
			return result;
		}

		result = find_file_state(uf_info);
		if (result != 0) {
			drop_exclusive_access(uf_info);
			up(&uf_info->write);
			reiser4_exit_context(ctx);
			return result;
		}

		assert("vs-1648", (uf_info->container == UF_CONTAINER_TAILS ||
				   uf_info->container == UF_CONTAINER_EXTENTS ||
				   uf_info->container == UF_CONTAINER_EMPTY));
		if (uf_info->container == UF_CONTAINER_TAILS) {
			/*
			 * invalidate all pages and convert file from tails to
			 * extents
			 */
			result = check_pages_unix_file(inode);
			if (result) {
				drop_exclusive_access(uf_info);
				up(&uf_info->write);
				reiser4_exit_context(ctx);
				return result;
			}
		}
	}

	/*
	 * generic_file_mmap will do update_atime. Grab space for stat data
	 * update.
	 */
	needed = inode_file_plugin(inode)->estimate.update(inode);
	result = reiser4_grab_space_force(needed, BA_CAN_COMMIT);
	if (result) {
		drop_exclusive_access(uf_info);
		up(&uf_info->write);
		reiser4_exit_context(ctx);
		return result;
	}

	result = generic_file_mmap(file, vma);
	if (result == 0) {
		/* mark file as having mapping. */
		inode_set_flag(inode, REISER4_HAS_MMAP);
		vma->vm_ops = &unix_file_vm_ops;
	}

	drop_exclusive_access(uf_info);
	up(&uf_info->write);
	reiser4_exit_context(ctx);
	return result;
}

static ssize_t write_file(hint_t * hint, struct file *file,	/* file to write to */
			  const char __user *buf,	/* address of user-space buffer */
			  size_t count,	/* number of bytes to write */
			  loff_t * off /* position in file to write to */ ,
			  int exclusive)
{
	struct inode *inode;
	ssize_t written;	/* amount actually written so far */
	loff_t pos;		/* current location in the file */

	inode = file->f_dentry->d_inode;

	/* estimation for write is entrusted to write item plugins */
	pos = *off;

	if (inode->i_size < pos) {
		/* pos is set past real end of file */
		written = append_hole(hint, inode, pos, exclusive);
		if (written)
			return written;
		assert("vs-1081", pos == inode->i_size);
	}

	/* write user data to the file */
	written = write_flow(hint, file, inode, buf, count, pos, exclusive);
	if (written > 0)
		/* update position in a file */
		*off = pos + written;

	/* return number of written bytes, or error code */
	return written;
}

/**
 * write_unix_file - write of struct file_operations
 * @file: file to write to
 * @buf: address of user-space buffer
 * @write_amount: number of bytes to write
 * @off: position in file to write to
 *
 * This is implementation of vfs's write method of struct file_operations for
 * unix file plugin.
 */
ssize_t write_unix_file(struct file *file, const char __user *buf,
			size_t write_amount, loff_t *off)
{
	reiser4_context *ctx;
	int result;
	struct inode *inode;
	hint_t *hint;
	unix_file_info_t *uf_info;
	struct page *pages[NR_PAGES_TO_PIN];
	int nr_pages;
	size_t count, written, left;
	int user_space;
	int try_free_space;

	if (unlikely(write_amount == 0))
		return 0;

	inode = file->f_dentry->d_inode;
	ctx = init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	assert("vs-947", !inode_get_flag(inode, REISER4_NO_SD));

	uf_info = unix_file_inode_data(inode);

	down(&uf_info->write);

	result = generic_write_checks(file, off, &write_amount, 0);
	if (result) {
		up(&uf_info->write);
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		return result;
	}

	/* linux's VM requires this. See mm/vmscan.c:shrink_list() */
	current->backing_dev_info = inode->i_mapping->backing_dev_info;

	if (inode_get_flag(inode, REISER4_PART_CONV)) {
		/* we can not currently write to a file which is partially converted */
		get_exclusive_access(uf_info);
		result = finish_conversion(inode);
		drop_exclusive_access(uf_info);
		if (result) {
			current->backing_dev_info = NULL;
			up(&uf_info->write);
			context_set_commit_async(ctx);
			reiser4_exit_context(ctx);
			return result;
		}
	}

	if (inode_get_flag(inode, REISER4_HAS_MMAP)
	    && uf_info->container == UF_CONTAINER_TAILS) {
		/* file built of tails was mmaped. So, there might be
		   faultin-ed pages filled by tail item contents and mapped to
		   process address space.
		   Before starting write:

		   1) block new page creation by obtaining exclusive access to
		   the file

		   2) unmap address space of all mmap - now it is by
		   reiser4_invalidate_pages which invalidate pages as well

		   3) convert file to extents to not enter here on each write
		   to mmaped file */
		get_exclusive_access(uf_info);
		result = check_pages_unix_file(inode);
		drop_exclusive_access(uf_info);
		if (result) {
			current->backing_dev_info = NULL;
			up(&uf_info->write);
			context_set_commit_async(ctx);
			reiser4_exit_context(ctx);
			return result;
		}
	}

	/* UNIX behavior: clear suid bit on file modification. This cannot be
	   done earlier, because removing suid bit captures blocks into
	   transaction, which should be done after taking either exclusive or
	   non-exclusive access on the file. */
	result = remove_suid(file->f_dentry);
	if (result != 0) {
		current->backing_dev_info = NULL;
		up(&uf_info->write);
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		return result;
	}
	grab_space_enable();

	hint = kmalloc(sizeof(*hint), GFP_KERNEL);
	if (hint == NULL) {
		current->backing_dev_info = NULL;
		up(&uf_info->write);
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		return RETERR(-ENOMEM);
	}

	/* get seal and coord sealed with it from reiser4 private data of
	 * struct file */
	result = load_file_hint(file, hint);
	if (result) {
		current->backing_dev_info = NULL;
		up(&uf_info->write);
		kfree(hint);
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		return result;
	}

	left = write_amount;
	count = 0;
	user_space = is_user_space(buf);
	nr_pages = 0;
	try_free_space = 1;

	while (left > 0) {
		unsigned long addr;
		size_t to_write;
		int excl = 0;

		addr = (unsigned long)buf;

		/* getting exclusive or not exclusive access requires no
		   transaction open */
		txn_restart_current();

		if (user_space) {
			to_write = get_nr_pages_nr_bytes(addr, left, &nr_pages);
			nr_pages =
			    reiser4_get_user_pages(pages, addr, nr_pages,
						   WRITE);
			if (nr_pages < 0) {
				result = nr_pages;
				break;
			}
			to_write = adjust_nr_bytes(addr, to_write, nr_pages);
			/* get_user_pages might create a transaction */
			txn_restart_current();
		} else
			to_write = left;

		if (inode->i_size == 0) {
			get_exclusive_access(uf_info);
			excl = 1;
		} else {
			get_nonexclusive_access(uf_info, 0);
			excl = 0;
		}

		all_grabbed2free();
		written = write_file(hint, file, buf, to_write, off, excl);
		if (user_space)
			reiser4_put_user_pages(pages, nr_pages);

		if (excl)
			drop_exclusive_access(uf_info);
		else
			drop_nonexclusive_access(uf_info);

		/* With no locks held we can commit atoms in attempt to recover
		 * free space. */
		if ((ssize_t) written == -ENOSPC && try_free_space) {
			txnmgr_force_commit_all(inode->i_sb, 0);
			try_free_space = 0;
			continue;
		}
		if ((ssize_t) written < 0) {
			result = written;
			break;
		}
		left -= written;
		buf += written;

		/* total number of written bytes */
		count += written;
	}

	if ((file->f_flags & O_SYNC) || IS_SYNC(inode)) {
		txn_restart_current();
		result =
		    sync_unix_file(file, file->f_dentry,
				   0 /* data and stat data */ );
		if (result)
			warning("reiser4-7", "failed to sync file %llu",
				(unsigned long long)get_inode_oid(inode));
	}

	save_file_hint(file, hint);
	kfree(hint);
	up(&uf_info->write);
	current->backing_dev_info = NULL;

	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);

	return count ? count : result;
}

/**
 * release_unix_file - release of struct file_operations
 * @inode: inode of released file
 * @file: file to release
 *
 * Implementation of release method of struct file_operations for unix file
 * plugin. If last reference to indode is released - convert all extent items
 * into tail items if necessary. Frees reiser4 specific file data.
 */
int release_unix_file(struct inode *inode, struct file *file)
{
	reiser4_context *ctx;
	unix_file_info_t *uf_info;
	int result;
	int in_reiser4;

	in_reiser4 = is_in_reiser4_context();

	ctx = init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	result = 0;
	if (in_reiser4 == 0) {
		uf_info = unix_file_inode_data(inode);

		down(&uf_info->write);
		get_exclusive_access(uf_info);
		if (atomic_read(&file->f_dentry->d_count) == 1 &&
		    uf_info->container == UF_CONTAINER_EXTENTS &&
		    !should_have_notail(uf_info, inode->i_size) &&
		    !rofs_inode(inode)) {
			result = extent2tail(uf_info);
			if (result != 0) {
				warning("nikita-3233",
					"Failed to convert in %s (%llu)",
					__FUNCTION__,
					(unsigned long long)
					get_inode_oid(inode));
			}
		}
		drop_exclusive_access(uf_info);
		up(&uf_info->write);
	} else {
		/*
		   we are within reiser4 context already. How latter is
		   possible? Simple:

		   (gdb) bt
		   #0  get_exclusive_access ()
		   #2  0xc01e56d3 in release_unix_file ()
		   #3  0xc01c3643 in reiser4_release ()
		   #4  0xc014cae0 in __fput ()
		   #5  0xc013ffc3 in remove_vm_struct ()
		   #6  0xc0141786 in exit_mmap ()
		   #7  0xc0118480 in mmput ()
		   #8  0xc0133205 in oom_kill ()
		   #9  0xc01332d1 in out_of_memory ()
		   #10 0xc013bc1d in try_to_free_pages ()
		   #11 0xc013427b in __alloc_pages ()
		   #12 0xc013f058 in do_anonymous_page ()
		   #13 0xc013f19d in do_no_page ()
		   #14 0xc013f60e in handle_mm_fault ()
		   #15 0xc01131e5 in do_page_fault ()
		   #16 0xc0104935 in error_code ()
		   #17 0xc025c0c6 in __copy_to_user_ll ()
		   #18 0xc01d496f in read_tail ()
		   #19 0xc01e4def in read_unix_file ()
		   #20 0xc01c3504 in reiser4_read ()
		   #21 0xc014bd4f in vfs_read ()
		   #22 0xc014bf66 in sys_read ()
		 */
		warning("vs-44", "out of memory?");
	}

	reiser4_free_file_fsdata(file);

	reiser4_exit_context(ctx);
	return result;
}

static void set_file_notail(struct inode *inode)
{
	reiser4_inode *state;
	formatting_plugin *tplug;

	state = reiser4_inode_data(inode);
	tplug = formatting_plugin_by_id(NEVER_TAILS_FORMATTING_ID);
	plugin_set_formatting(&state->pset, tplug);
	inode_set_plugin(inode,
			 formatting_plugin_to_plugin(tplug), PSET_FORMATTING);
}

/* if file is built of tails - convert it to extents */
static int unpack(struct inode *inode, int forever)
{
	int result = 0;
	unix_file_info_t *uf_info;

	uf_info = unix_file_inode_data(inode);
	assert("vs-1628", ea_obtained(uf_info));

	result = find_file_state(uf_info);
	assert("vs-1074",
	       ergo(result == 0, uf_info->container != UF_CONTAINER_UNKNOWN));
	if (result == 0) {
		if (uf_info->container == UF_CONTAINER_TAILS)
			result = tail2extent(uf_info);
		if (result == 0 && forever)
			set_file_notail(inode);
		if (result == 0) {
			__u64 tograb;

			grab_space_enable();
			tograb =
			    inode_file_plugin(inode)->estimate.update(inode);
			result = reiser4_grab_space(tograb, BA_CAN_COMMIT);
			if (result == 0)
				update_atime(inode);
		}
	}

	return result;
}

/* implentation of vfs' ioctl method of struct file_operations for unix file
   plugin
*/
int
ioctl_unix_file(struct inode *inode, struct file *filp UNUSED_ARG,
		unsigned int cmd, unsigned long arg UNUSED_ARG)
{
	reiser4_context *ctx;
	int result;

	ctx = init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	switch (cmd) {
	case REISER4_IOC_UNPACK:
		get_exclusive_access(unix_file_inode_data(inode));
		result = unpack(inode, 1 /* forever */ );
		drop_exclusive_access(unix_file_inode_data(inode));
		break;

	default:
		result = RETERR(-ENOSYS);
		break;
	}
	reiser4_exit_context(ctx);
	return result;
}

/* implentation of vfs' bmap method of struct address_space_operations for unix
   file plugin
*/
sector_t bmap_unix_file(struct address_space * mapping, sector_t lblock)
{
	reiser4_context *ctx;
	sector_t result;
	reiser4_key key;
	coord_t coord;
	lock_handle lh;
	struct inode *inode;
	item_plugin *iplug;
	sector_t block;

	inode = mapping->host;

	ctx = init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	key_by_inode_and_offset_common(inode,
				       (loff_t) lblock * current_blocksize,
				       &key);

	init_lh(&lh);
	result =
	    find_file_item_nohint(&coord, &lh, &key, ZNODE_READ_LOCK, inode);
	if (cbk_errored(result)) {
		done_lh(&lh);
		reiser4_exit_context(ctx);
		return result;
	}

	result = zload(coord.node);
	if (result) {
		done_lh(&lh);
		reiser4_exit_context(ctx);
		return result;
	}

	iplug = item_plugin_by_coord(&coord);
	if (iplug->s.file.get_block) {
		result = iplug->s.file.get_block(&coord, lblock, &block);
		if (result == 0)
			result = block;
	} else
		result = RETERR(-EINVAL);

	zrelse(coord.node);
	done_lh(&lh);
	reiser4_exit_context(ctx);
	return result;
}

/**
 * flow_by_inode_unix_file - initizlize structure flow
 * @inode: inode of file for which read or write is abou
 * @buf: buffer to perform read to or write from
 * @user: flag showing whether @buf is user space or kernel space
 * @size: size of buffer @buf
 * @off: start offset fro read or write
 * @op: READ or WRITE
 * @flow:
 *
 * Initializes fields of @flow: key, size of data, i/o mode (read or write).
 */
int flow_by_inode_unix_file(struct inode *inode,
			    const char __user *buf, int user,
			    loff_t size, loff_t off,
			    rw_op op, flow_t *flow)
{
	assert("nikita-1100", inode != NULL);

	flow->length = size;
	memcpy(&flow->data, &buf, sizeof(buf));
	flow->user = user;
	flow->op = op;
	assert("nikita-1931", inode_file_plugin(inode) != NULL);
	assert("nikita-1932",
	       inode_file_plugin(inode)->key_by_inode ==
	       key_by_inode_and_offset_common);
	/* calculate key of write position and insert it into flow->key */
	return key_by_inode_and_offset_common(inode, off, &flow->key);
}

/* plugin->u.file.set_plug_in_sd = NULL
   plugin->u.file.set_plug_in_inode = NULL
   plugin->u.file.create_blank_sd = NULL */
/* plugin->u.file.delete */
/*
   plugin->u.file.add_link = add_link_common
   plugin->u.file.rem_link = NULL */

/* plugin->u.file.owns_item
   this is common_file_owns_item with assertion */
/* Audited by: green(2002.06.15) */
int
owns_item_unix_file(const struct inode *inode /* object to check against */ ,
		    const coord_t * coord /* coord to check */ )
{
	int result;

	result = owns_item_common(inode, coord);
	if (!result)
		return 0;
	if (item_type_by_coord(coord) != UNIX_FILE_METADATA_ITEM_TYPE)
		return 0;
	assert("vs-547",
	       item_id_by_coord(coord) == EXTENT_POINTER_ID ||
	       item_id_by_coord(coord) == FORMATTING_ID);
	return 1;
}

static int setattr_truncate(struct inode *inode, struct iattr *attr)
{
	int result;
	int s_result;
	loff_t old_size;
	reiser4_tree *tree;

	inode_check_scale(inode, inode->i_size, attr->ia_size);

	old_size = inode->i_size;
	tree = tree_by_inode(inode);

	result = safe_link_grab(tree, BA_CAN_COMMIT);
	if (result == 0)
		result = safe_link_add(inode, SAFE_TRUNCATE);
	all_grabbed2free();
	if (result == 0)
		result = truncate_file_body(inode, attr->ia_size);
	if (result)
		warning("vs-1588", "truncate_file failed: oid %lli, "
			"old size %lld, new size %lld, retval %d",
			(unsigned long long)get_inode_oid(inode),
			old_size, attr->ia_size, result);

	s_result = safe_link_grab(tree, BA_CAN_COMMIT);
	if (s_result == 0)
		s_result =
		    safe_link_del(tree, get_inode_oid(inode), SAFE_TRUNCATE);
	if (s_result != 0) {
		warning("nikita-3417", "Cannot kill safelink %lli: %i",
			(unsigned long long)get_inode_oid(inode), s_result);
	}
	safe_link_release(tree);
	all_grabbed2free();
	return result;
}

/* plugin->u.file.setattr method */
/* This calls inode_setattr and if truncate is in effect it also takes
   exclusive inode access to avoid races */
int setattr_unix_file(struct dentry *dentry,	/* Object to change attributes */
		      struct iattr *attr /* change description */ )
{
	int result;

	if (attr->ia_valid & ATTR_SIZE) {
		reiser4_context *ctx;
		unix_file_info_t *uf_info;

		/* truncate does reservation itself and requires exclusive
		   access obtained */
		ctx = init_context(dentry->d_inode->i_sb);
		if (IS_ERR(ctx))
			return PTR_ERR(ctx);

		uf_info = unix_file_inode_data(dentry->d_inode);
		down(&uf_info->write);
		get_exclusive_access(uf_info);
		result = setattr_truncate(dentry->d_inode, attr);
		drop_exclusive_access(uf_info);
		up(&uf_info->write);
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
	} else
		result = setattr_common(dentry, attr);

	return result;
}

/* plugin->u.file.init_inode_data */
void
init_inode_data_unix_file(struct inode *inode,
			  reiser4_object_create_data * crd, int create)
{
	unix_file_info_t *data;

	data = unix_file_inode_data(inode);
	data->container = create ? UF_CONTAINER_EMPTY : UF_CONTAINER_UNKNOWN;
	init_rwsem(&data->latch);
	sema_init(&data->write, 1);
	data->tplug = inode_formatting_plugin(inode);
	data->exclusive_use = 0;

#if REISER4_DEBUG
	data->ea_owner = NULL;
	atomic_set(&data->nr_neas, 0);
#endif
	init_inode_ordering(inode, crd, create);
}

/**
 * delete_object_unix_file - delete_object of file_plugin
 * @inode: inode to be deleted
 *
 * Truncates file to length 0, removes stat data and safe link.
 */
int delete_object_unix_file(struct inode *inode)
{
	unix_file_info_t *uf_info;
	int result;

	/*
	 * transaction can be open already. For example:
	 * writeback_inodes->sync_sb_inodes->reiser4_sync_inodes->
	 * generic_sync_sb_inodes->iput->generic_drop_inode->
	 * generic_delete_inode->reiser4_delete_inode->delete_object_unix_file.
	 * So, restart transaction to avoid deadlock with file rw semaphore.
	 */
	txn_restart_current();

	if (inode_get_flag(inode, REISER4_NO_SD))
		return 0;

	/* truncate file bogy first */
	uf_info = unix_file_inode_data(inode);
	get_exclusive_access(uf_info);
	result = truncate_file_body(inode, 0 /* size */ );
	drop_exclusive_access(uf_info);

	if (result)
		warning("", "failed to truncate file (%llu) on removal: %d",
			get_inode_oid(inode), result);

	/* remove stat data and safe link */
	return delete_object_common(inode);
}

/**
 * sendfile_unix_file - sendfile of struct file_operations
 * @file: file to be sent
 * @ppos: position to start from
 * @count: number of bytes to send
 * @actor: function to copy data
 * @target: where to copy read data
 *
 * Reads @count bytes from @file and calls @actor for every page read. This is
 * needed for loop back devices support.
 */
ssize_t
sendfile_unix_file(struct file *file, loff_t *ppos, size_t count,
		   read_actor_t actor, void *target)
{
	reiser4_context *ctx;
	ssize_t result;
	struct inode *inode;
	unix_file_info_t *uf_info;

	inode = file->f_dentry->d_inode;
	ctx = init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	/*
	 * generic_file_sndfile may want to call update_atime. Grab space for
	 * stat data update
	 */
	result = reiser4_grab_space(estimate_update_common(inode),
				    BA_CAN_COMMIT);
	if (result)
		goto error;
	down(&inode->i_sem);
	inode_set_flag(inode, REISER4_HAS_MMAP);
	up(&inode->i_sem);

	uf_info = unix_file_inode_data(inode);
	get_nonexclusive_access(uf_info, 0);
	result = generic_file_sendfile(file, ppos, count, actor, target);
	drop_nonexclusive_access(uf_info);
 error:
	reiser4_exit_context(ctx);
	return result;
}

int
prepare_write_unix_file(struct file *file, struct page *page,
			unsigned from, unsigned to)
{
	reiser4_context *ctx;
	unix_file_info_t *uf_info;
	int ret;

	ctx = init_context(file->f_dentry->d_inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	uf_info = unix_file_inode_data(file->f_dentry->d_inode);
	get_exclusive_access(uf_info);
	ret = find_file_state(uf_info);
	if (ret == 0) {
		if (uf_info->container == UF_CONTAINER_TAILS)
			ret = -EINVAL;
		else
			ret = do_prepare_write(file, page, from, to);
	}
	drop_exclusive_access(uf_info);

	/* don't commit transaction under inode semaphore */
	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);
	return ret;
}

/*
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   scroll-step: 1
   End:
*/
