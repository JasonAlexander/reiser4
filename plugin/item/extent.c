/* Copyright 2001, 2002 by Hans Reiser, licensing governed by reiser4/README */

#include "../../forward.h"
#include "../../dformat.h"
#include "../../debug.h"
#include "../../key.h"
#include "../../kassign.h"
#include "../../coord.h"
#include "item.h"
#include "extent.h"
#include "../plugin.h"
#include "../object.h"
#include "../../txnmgr.h"
#include "../../jnode.h"
#include "../../block_alloc.h"
#include "../../znode.h"
#include "../../carry.h"
#include "../../tap.h"
#include "../../super.h"
#include "../../page_cache.h"
#include "../../tree.h"
#include "../../inode.h"

#include <asm/uaccess.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/quotaops.h>
#include <linux/fs.h>		/* for struct address_space  */

static const reiser4_block_nr null_block_nr = (__u64) 0;

/* prepare structure reiser4_item_data to put one extent unit into tree */
/* Audited by: green(2002.06.13) */
static reiser4_item_data *
init_new_extent(reiser4_item_data * data, void *ext_unit, int nr_extents)
{
	if (REISER4_DEBUG)
		memset(data, 0, sizeof (reiser4_item_data));

	data->data = ext_unit;
	/* data->data is kernel space */
	data->user = 0;
	data->length = sizeof (reiser4_extent) * nr_extents;
	data->arg = 0;
	data->iplug = item_plugin_by_id(EXTENT_POINTER_ID);
	return data;
}

/* how many bytes are addressed by @nr (or all of @nr == -1) first extents of
   the extent item.  FIXME: Josh says this (unsigned) business is UGLY.  Make
   it signed, since there can't be more than INT_MAX units in an extent item,
   right? */
static reiser4_block_nr
extent_size(const coord_t * coord, unsigned nr)
{
	unsigned i;
	reiser4_block_nr blocks;
	reiser4_extent *ext;

	ext = item_body_by_coord(coord);
	if ((int) nr == -1) {
		nr = extent_nr_units(coord);
	} else {
		assert("vs-263", nr <= extent_nr_units(coord));
	}

	blocks = 0;
	for (i = 0; i < nr; i++, ext++) {
		blocks += extent_get_width(ext);
	}

	return blocks * current_blocksize;
}

/* plugin->u.item.b.max_key_inside */
reiser4_key *
extent_max_key_inside(const coord_t * coord, reiser4_key * key)
{
	item_key_by_coord(coord, key);
	set_key_offset(key, get_key_offset(max_key()));
	return key;
}

/* plugin->u.item.b.can_contain_key
   this checks whether @key of @data is matching to position set by @coord */
int
extent_can_contain_key(const coord_t * coord, const reiser4_key * key, const reiser4_item_data * data)
{
	reiser4_key item_key;

	if (item_plugin_by_coord(coord) != data->iplug)
		return 0;

	item_key_by_coord(coord, &item_key);
	if (get_key_locality(key) != get_key_locality(&item_key) ||
	    get_key_objectid(key) != get_key_objectid(&item_key)) return 0;

	return 1;
}

/* plugin->u.item.b.mergeable
   first item is of extent type */
/* Audited by: green(2002.06.13) */
int
extent_mergeable(const coord_t * p1, const coord_t * p2)
{
	reiser4_key key1, key2;

	assert("vs-299", item_id_by_coord(p1) == EXTENT_POINTER_ID);
	/* FIXME-VS: Which is it? Assert or return 0 */
	if (item_id_by_coord(p2) != EXTENT_POINTER_ID) {
		return 0;
	}

	item_key_by_coord(p1, &key1);
	item_key_by_coord(p2, &key2);
	if (get_key_locality(&key1) != get_key_locality(&key2) ||
	    get_key_objectid(&key1) != get_key_objectid(&key2) || get_key_type(&key1) != get_key_type(&key2))
		return 0;
	if (get_key_offset(&key1) + extent_size(p1, extent_nr_units(p1)) != get_key_offset(&key2))
		return 0;
	return 1;
}

/* extents in an extent item can be either holes, or unallocated or allocated
   extents */
typedef enum {
	HOLE_EXTENT,
	UNALLOCATED_EXTENT,
	ALLOCATED_EXTENT
} extent_state;

static extent_state
state_of_extent(reiser4_extent * ext)
{
	switch ((int) extent_get_start(ext)) {
	case 0:
		return HOLE_EXTENT;
	case 1:
		return UNALLOCATED_EXTENT;
	default:
		break;
	}
	return ALLOCATED_EXTENT;
}

int
extent_is_unallocated(const coord_t * item)
{
	assert("jmacd-5133", item_is_extent(item));

	return state_of_extent(extent_by_coord(item)) == UNALLOCATED_EXTENT;
}

#if REISER4_DEBUG_OUTPUT
/* plugin->u.item.b.print */
/* Audited by: green(2002.06.13) */
static const char *
state2label(extent_state state)
{
	const char *label;

	label = 0;
	switch (state) {
	case HOLE_EXTENT:
		label = "hole";
		break;

	case UNALLOCATED_EXTENT:
		label = "unalloc";
		break;

	case ALLOCATED_EXTENT:
		label = "alloc";
		break;
	}
	assert("vs-376", label);
	return label;
}

void
extent_print(const char *prefix, coord_t * coord)
{
	reiser4_extent *ext;
	unsigned i, nr;

	if (prefix)
		info("%s:", prefix);

	nr = extent_nr_units(coord);
	ext = (reiser4_extent *) item_body_by_coord(coord);

	info("%u: ", nr);
	for (i = 0; i < nr; i++, ext++) {
		info("[%Lu (%Lu) %s]", extent_get_start(ext), extent_get_width(ext), state2label(state_of_extent(ext)));
	}
	info("\n");
}
#endif

/* extent_check ->check() method for extent items
  
   used for debugging, every item should have here the most complete
   possible check of the consistency of the item that the inventor can
   construct 
*/
int
extent_check(const coord_t * coord /* coord of item to check */ ,
	     const char **error /* where to store error message */ )
{
	reiser4_extent *ext, *first;
	unsigned i, j;
	reiser4_block_nr start, width, blk_cnt;
	unsigned num_units;

	assert("vs-933", REISER4_DEBUG);

	if (item_length_by_coord(coord) % sizeof (reiser4_extent) != 0) {
		*error = "Wrong item size";
		return -1;
	}
	ext = first = extent_item(coord);
	blk_cnt = reiser4_block_count(reiser4_get_current_sb());
	num_units = coord_num_units(coord);

	for (i = 0; i < num_units; ++i, ++ext) {

		start = extent_get_start(ext);
		if (start < 2)
			continue;
		/* extent is allocated one */
		width = extent_get_width(ext);
		if (start >= blk_cnt) {
			*error = "Start too large";
			return -1;
		}
		if (start + width > blk_cnt) {
			*error = "End too large";
			return -1;
		}
		/* make sure that this extent does not overlap with other
		   allocated extents extents */
		for (j = 0; j < i; j++) {
			if (state_of_extent(first + j) != ALLOCATED_EXTENT)
				continue;
			if (!((extent_get_start(ext) >= extent_get_start(first + j) + extent_get_width(first + j))
			      || (extent_get_start(ext) + extent_get_width(ext) <= extent_get_start(first + j)))) {
				*error = "Extent overlaps with others";
				return -1;
			}
		}

	}
	return 0;
}

/* plugin->u.item.b.nr_units */
unsigned
extent_nr_units(const coord_t * coord)
{
	/* length of extent item has to be multiple of extent size */
	if ((item_length_by_coord(coord) % sizeof (reiser4_extent)) != 0)
		impossible("vs-10", "Wrong extent item size: %i, %i",
			   item_length_by_coord(coord), sizeof (reiser4_extent));
	return item_length_by_coord(coord) / sizeof (reiser4_extent);
}

/* plugin->u.item.b.lookup */
lookup_result extent_lookup(const reiser4_key * key, lookup_bias bias UNUSED_ARG, coord_t * coord)
{				/* znode and item_pos are
				   set to an extent item to
				   look through */
	reiser4_key item_key;
	reiser4_block_nr lookuped, offset;
	unsigned i, nr_units;
	reiser4_extent *ext;
	size_t blocksize;

	item_key_by_coord(coord, &item_key);
	offset = get_key_offset(&item_key);
	nr_units = extent_nr_units(coord);

	/* key we are looking for must be greater than key of item @coord */
	assert("vs-414", keygt(key, &item_key));

	if (keygt(key, extent_max_key_inside(coord, &item_key))) {
		/* @key is key of another file */
		coord->unit_pos = nr_units - 1;
		coord->between = AFTER_UNIT;
		return CBK_COORD_NOTFOUND;
	}

	assert("vs-11", get_key_objectid(key) == get_key_objectid(&item_key));

	assert("vs-1010", coord->unit_pos == 0);
	ext = extent_by_coord(coord);
	blocksize = current_blocksize;

	/* offset we are looking for */
	lookuped = get_key_offset(key);

	/* go through all extents until the one which address given offset */
	for (i = 0; i < nr_units; i++, ext++) {
		offset += (blocksize * extent_get_width(ext));
		if (offset > lookuped) {
			/* desired byte is somewhere in this extent */
			coord->unit_pos = i;
			coord->between = AT_UNIT;
			return CBK_COORD_FOUND;
		}
	}

	/* set coord after last unit */
	coord->unit_pos = nr_units - 1;
	coord->between = AFTER_UNIT;
	return CBK_COORD_FOUND;	/*bias == FIND_MAX_NOT_MORE_THAN ? CBK_COORD_FOUND : CBK_COORD_NOTFOUND; */
}

/* set extent width and convert type to on disk representation */
/* Audited by: green(2002.06.13) */
static void
set_extent(reiser4_extent * ext, extent_state state, reiser4_block_nr start, reiser4_block_nr width)
{
	switch (state) {
	case HOLE_EXTENT:
		start = 0ull;
		break;
	case UNALLOCATED_EXTENT:
		start = 1ull;
		break;
	case ALLOCATED_EXTENT:
		break;
	default:
		impossible("vs-338", "do not create extents but holes and unallocated");
	}
	extent_set_start(ext, start);
	extent_set_width(ext, width);
}

/* plugin->u.item.b.paste
   item @coord is set to has been appended with @data->length of free
   space. data->data contains data to be pasted into the item in position
   @coord->in_item.unit_pos. It must fit into that free space.
   @coord must be set between units.
*/
int
extent_paste(coord_t * coord, reiser4_item_data * data, carry_plugin_info * info UNUSED_ARG)
{
	unsigned old_nr_units;
	reiser4_extent *ext;
	int item_length;

	ext = extent_item(coord);
	item_length = item_length_by_coord(coord);
	old_nr_units = (item_length - data->length) / sizeof (reiser4_extent);

	/* this is also used to copy extent into newly created item, so
	   old_nr_units could be 0 */
	assert("vs-260", item_length >= data->length);

	/* make sure that coord is set properly */
	assert("vs-35", ((!coord_is_existing_unit(coord)) || (!old_nr_units && !coord->unit_pos)));

	/* first unit to be moved */
	switch (coord->between) {
	case AFTER_UNIT:
		coord->unit_pos++;
	case BEFORE_UNIT:
		coord->between = AT_UNIT;
		break;
	case AT_UNIT:
		assert("vs-331", !old_nr_units && !coord->unit_pos);
		break;
	default:
		impossible("vs-330", "coord is set improperly");
	}

	/* prepare space for new units */
	xmemmove(ext + coord->unit_pos + data->length / sizeof (reiser4_extent),
		 ext + coord->unit_pos, (old_nr_units - coord->unit_pos) * sizeof (reiser4_extent));

	/* copy new data from kernel space */
	assert("vs-556", data->user == 0);
	xmemcpy(ext + coord->unit_pos, data->data, (unsigned) data->length);

	/* after paste @coord is set to first of pasted units */
	assert("vs-332", coord_is_existing_unit(coord));
	assert("vs-333", !memcmp(data->data, extent_by_coord(coord), (unsigned) data->length));
	return 0;
}

/* plugin->u.item.b.can_shift */
int
extent_can_shift(unsigned free_space, coord_t * source,
		 znode * target UNUSED_ARG, shift_direction pend UNUSED_ARG, unsigned *size, unsigned want)
{
	*size = item_length_by_coord(source);
	if (*size > free_space)
		/* never split a unit of extent item */
		*size = free_space - free_space % sizeof (reiser4_extent);

	/* we can shift *size bytes, calculate how many do we want to shift */
	if (*size > want * sizeof (reiser4_extent))
		*size = want * sizeof (reiser4_extent);

	if (*size % sizeof (reiser4_extent) != 0)
		impossible("vs-119", "Wrong extent size: %i %i", *size, sizeof (reiser4_extent));
	return *size / sizeof (reiser4_extent);

}

/* plugin->u.item.b.copy_units */
void
extent_copy_units(coord_t * target, coord_t * source,
		  unsigned from, unsigned count, shift_direction where_is_free_space, unsigned free_space)
{
	char *from_ext, *to_ext;

	assert("vs-217", free_space == count * sizeof (reiser4_extent));

	from_ext = item_body_by_coord(source);
	to_ext = item_body_by_coord(target);

	if (where_is_free_space == SHIFT_LEFT) {
		assert("vs-215", from == 0);

		/* At this moment, item length was already updated in the item
		   header by shifting code, hence extent_nr_units() will
		   return "new" number of units---one we obtain after copying
		   units.
		*/
		to_ext += (extent_nr_units(target) - count) * sizeof (reiser4_extent);
	} else {
		reiser4_key key;
		coord_t coord;

		assert("vs-216", from + count == coord_last_unit_pos(source) + 1);

		from_ext += item_length_by_coord(source) - free_space;

		/* new units are inserted before first unit in an item,
		   therefore, we have to update item key */
		coord = *source;
		coord.unit_pos = from;
		extent_unit_key(&coord, &key);

		node_plugin_by_node(target->node)->update_item_key(target, &key, 0	/*info */
		    );
	}

	xmemcpy(to_ext, from_ext, free_space);
}

/* plugin->u.item.b.create_hook
   @arg is znode of leaf node for which we need to update right delimiting key */
int
extent_create_hook(const coord_t * coord, void *arg)
{
	coord_t *child_coord;
	znode *node;
	reiser4_key key;
	reiser4_tree *tree;

	if (!arg)
		return 0;

	/* find a node on the left level for which right delimiting key has to
	   be updated */
	child_coord = arg;
	if (coord_wrt(child_coord) == COORD_ON_THE_LEFT) {
		assert("vs-411", znode_is_left_connected(child_coord->node));
		node = child_coord->node->left;
	} else {
		assert("vs-412", coord_wrt(child_coord) == COORD_ON_THE_RIGHT);
		node = child_coord->node;
	}

	if (!node)
		return 0;

	tree = znode_get_tree(node);
	UNDER_SPIN_VOID(dk, tree, *znode_get_rd_key(node) = *item_key_by_coord(coord, &key));

	/* break sibling links */
	spin_lock_tree(tree);
	if (ZF_ISSET(node, JNODE_RIGHT_CONNECTED) && node->right) {
		/*ZF_CLR (node->right, JNODE_LEFT_CONNECTED); */
		node->right->left = NULL;
		/*ZF_CLR (node, JNODE_RIGHT_CONNECTED); */
		node->right = NULL;
	}
	spin_unlock_tree(tree);
	return 0;
}

/* plugin->u.item.b.kill_item_hook
   this is called when @count units starting from @from-th one are going to be removed */
int
extent_kill_item_hook(const coord_t * coord, unsigned from, unsigned count)
{
	reiser4_extent *ext;
	unsigned i;
	reiser4_block_nr start, length, j;
	oid_t oid;
	reiser4_key key;

	assert ("zam-811", znode_is_write_locked(coord->node));
	
	item_key_by_coord(coord, &key);
	oid = get_key_objectid(&key);

	ext = extent_item(coord) + from;
	for (i = 0; i < count; i++, ext++) {
		reiser4_tree *tree;
		coord_t twin;

		start = extent_get_start(ext);
		length = extent_get_width(ext);
		if (state_of_extent(ext) == HOLE_EXTENT)
			continue;
		/* at this time there should not be already jnodes
		 * corresponding any block from this extent. Check that */

		coord_dup(&twin, coord);
		twin.unit_pos = from + i;
		twin.between = AT_UNIT;
		tree = current_tree;

		/* kill all jnodes of extent being removed */

		/* Usually jnode is un-captured and destroyed in
		 * ->invalidatepage() as part of truncate_inode_pages(). But
		 * it is possible that after jnode has been flushed and its
		 * dirty bit cleared, it was detached from page by
		 * ->releasepage() and hence missed by ->invalidatepage(). */

		for (j = 0; j < length; j ++) {
			jnode *node;

			node = UNDER_SPIN(tree, tree, 
					  jlook(tree, oid, 
						extent_unit_index(&twin) + j));
			if (node != NULL) {
				assert("vs-1095", 
				       UNDER_SPIN(jnode, node, 
						  jnode_page(node) == NULL));
				JF_SET(node, JNODE_HEARD_BANSHEE);
				jput(node);
			}
		}
		if (state_of_extent(ext) == UNALLOCATED_EXTENT) {
			/* FIXME-VITALY: this is necessary??? */
			fake_allocated2free(extent_get_width(ext), 0 /* unformatted */ );
		}

		if (state_of_extent(ext) != ALLOCATED_EXTENT) {
			continue;
		}

		/* FIXME-VS: do I need to do anything for unallocated extents */
		/* BA_DEFER bit parameter is turned on because blocks which get freed
		   are not safe to be freed immediately */
		
		reiser4_dealloc_blocks(&start, &length, 0 /* not used */, 
			BA_DEFER/* unformatted with defer */);
	}
	return 0;
}

static reiser4_key *
last_key_in_extent(const coord_t * coord, reiser4_key * key)
{
	/* FIXME-VS: this actually calculates key of first byte which is the
	   next to last byte by addressed by this extent */
	item_key_by_coord(coord, key);
	set_key_offset(key, get_key_offset(key) + extent_size(coord, extent_nr_units(coord)));
	return key;
}

static int
cut_or_kill_units(coord_t * coord,
		  unsigned *from, unsigned *to,
		  int cut, const reiser4_key * from_key, const reiser4_key * to_key, reiser4_key * smallest_removed)
{
	reiser4_extent *ext;
	reiser4_key key;
	unsigned blocksize, blocksize_bits;
	reiser4_block_nr offset;
	unsigned count;
	__u64 cut_from_to;

	count = *to - *from + 1;

	blocksize = current_blocksize;
	blocksize_bits = current_blocksize_bits;

	/* make sure that we cut something but not more than all units */
	assert("vs-220", count > 0 && count <= extent_nr_units(coord));
	/* extent item can be cut either from the beginning or down to the end */
	assert("vs-298", *from == 0 || *to == coord_last_unit_pos(coord));

	item_key_by_coord(coord, &key);
	offset = get_key_offset(&key);

	if (smallest_removed) {
		/* set @smallest_removed assuming that @from unit will be
		   cut */
		*smallest_removed = key;
		set_key_offset(smallest_removed, (offset + extent_size(coord, *from)));
	}

	cut_from_to = 0;

	/* it may happen that extent @from will not be removed */
	if (from_key) {
		reiser4_key key_inside;
		__u64 last;

		/* when @from_key (and @to_key) are specified things become
		   more complex. It may happen that @from-th or @to-th extent
		   will only decrease their width */
		assert("vs-311", to_key);

		key_inside = key;
		set_key_offset(&key_inside, (offset + extent_size(coord, *from)));
		last = offset + extent_size(coord, *to + 1) - 1;
		if (keygt(from_key, &key_inside)) {
			/* @from-th extent can not be removed. Its width has to
			   be decreased in accordance with @from_key */
			reiser4_block_nr new_width, old_width;
			reiser4_block_nr first;

			/* cut from the middle of extent item is not allowed,
			   make sure that the rest of item gets cut
			   completely */
			assert("vs-612", *to == coord_last_unit_pos(coord));
			assert("vs-613", keyge(to_key, extent_max_key(coord, &key_inside)));

			ext = extent_item(coord) + *from;
			first = offset + extent_size(coord, *from);
			old_width = extent_get_width(ext);
			new_width = (get_key_offset(from_key) + (blocksize - 1) - first) >> blocksize_bits;
			assert("vs-307", new_width > 0 && new_width <= old_width);
			if (new_width < old_width) {
				/* FIXME-VS: debugging zam-528 */
				if (state_of_extent(ext) == UNALLOCATED_EXTENT && !cut) {
					/* FIXME-VITALY: this is necessary??? */
					fake_allocated2free(old_width - new_width, 0	/*unformatted */
					    );
				}

				if (state_of_extent(ext) == ALLOCATED_EXTENT && !cut) {
					reiser4_block_nr start, length;
					/* truncate is in progress. Some blocks
					   can be freed. As they do not get
					   immediately available, set defer
					   parameter of reiser4_dealloc_blocks
					   to 1
					*/
					start = extent_get_start(ext) + new_width;
					length = old_width - new_width;

					reiser4_dealloc_blocks(&start, &length, 0 /* not used */, 
						BA_DEFER /* unformatted with defer */);
				}
				extent_set_width(ext, new_width);
				znode_set_dirty(coord->node);
			}
			(*from)++;
			count--;
			if (smallest_removed) {
				set_key_offset(smallest_removed, get_key_offset(from_key));
			}
		}

		/* set @key_inside to key of last byte addressed to extent @to */
		set_key_offset(&key_inside, last);

		if (keylt(to_key, &key_inside)) {
			/* @to-th unit can not be removed completely */

			reiser4_block_nr new_width, old_width;

			/* cut from the middle of extent item is not allowed,
			   make sure that head of item gets cut and cut is
			   aligned to block boundary */
			assert("vs-614", *from == 0);
			assert("vs-615", keyle(from_key, &key));
			assert("vs-616", ((get_key_offset(to_key) + 1) & (blocksize - 1)) == 0);

			ext = extent_item(coord) + *to;

			new_width = (get_key_offset(&key_inside) - get_key_offset(to_key)) >> blocksize_bits;

			old_width = extent_get_width(ext);
			cut_from_to = (old_width - new_width) * blocksize;

			/* FIXME:NIKITA->VS I see this failing with new_width
			   == old_width (@to unit is not affected at all). */
			assert("vs-617", new_width > 0 && new_width <= old_width);

			/* FIXME-VS: debugging zam-528 */
			if (state_of_extent(ext) == UNALLOCATED_EXTENT && !cut) {
				/* FIXME-VITALY: this is necessary??? */
				fake_allocated2free(old_width - new_width, 0 /*unformatted */ );
			}

			if (state_of_extent(ext) == ALLOCATED_EXTENT && !cut) {
				reiser4_block_nr start, length;
				/* extent2tail is in progress. Some blocks can
				   be freed. As they do not get immediately
				   available, set defer parameter of
				   reiser4_dealloc_blocks to 1
				*/
				start = extent_get_start(ext);
				length = old_width - new_width;
				reiser4_dealloc_blocks(&start, &length, 0 /* not used */,
					BA_DEFER/* unformatted with defer */);
			}

			/* (old_width - new_width) blocks of this extent were
			   free, update both extent's start (for allocated
			   extent only) and width */
			if (state_of_extent(ext) == ALLOCATED_EXTENT) {
				extent_set_start(ext, extent_get_start(ext) + old_width - new_width);
			}
			extent_set_width(ext, new_width);
			znode_set_dirty(coord->node);
			(*to)--;
			count--;
		}
	}

	if (!cut)
		/* call kill hook for all extents removed completely */
		extent_kill_item_hook(coord, *from, count);

	if (*from == 0 && count != coord_last_unit_pos(coord) + 1) {
		/* part of item is removed from item beginning, update item key
		   therefore */
		item_key_by_coord(coord, &key);
		set_key_offset(&key, (get_key_offset(&key) + extent_size(coord, count) + cut_from_to));
		node_plugin_by_node(coord->node)->update_item_key(coord, &key, 0);
	}

	if (REISER4_DEBUG) {
		/* zero space which is freed as result of cut between keys */
		ext = extent_item(coord);
		xmemset(ext + *from, 0, count * sizeof (reiser4_extent));
	}

	return count * sizeof (reiser4_extent);
}

/* plugin->u.item.b.cut_units */
int
extent_cut_units(coord_t * item, unsigned *from, unsigned *to,
		 const reiser4_key * from_key, const reiser4_key * to_key, reiser4_key * smallest_removed)
{
	return cut_or_kill_units(item, from, to, 1, from_key, to_key, smallest_removed);
}

/* plugin->u.item.b.kill_units */
int
extent_kill_units(coord_t * item, unsigned *from, unsigned *to,
		  const reiser4_key * from_key, const reiser4_key * to_key, reiser4_key * smallest_removed)
{
	return cut_or_kill_units(item, from, to, 0, from_key, to_key, smallest_removed);
}

/* plugin->u.item.b.unit_key */
reiser4_key *
extent_unit_key(const coord_t * coord, reiser4_key * key)
{
	assert("vs-300", coord_is_existing_unit(coord));

	item_key_by_coord(coord, key);
	set_key_offset(key, (get_key_offset(key) + extent_size(coord, (unsigned) coord->unit_pos)));

	return key;
}

/* plugin->u.item.b.estimate
   plugin->u.item.b.item_data_by_flow */

/* union union-able extents and cut an item correspondingly */
static void
optimize_extent(const coord_t * item)
{
	unsigned i, old_num, new_num;
	reiser4_extent *cur, *new_cur, *start;
	reiser4_block_nr cur_width, new_cur_width;
	extent_state cur_state;
	ON_DEBUG(const char *error);

	assert("vs-765", coord_is_existing_item(item));
	assert("vs-763", item_is_extent(item));
	assert("vs-934", extent_check(item, &error) == 0);

	cur = start = extent_item(item);
	old_num = extent_nr_units(item);
	new_num = 0;
	new_cur = NULL;
	new_cur_width = 0;

	for (i = 0; i < old_num; i++, cur++) {
		cur_width = extent_get_width(cur);
		if (!cur_width)
			continue;

		cur_state = state_of_extent(cur);
		if (new_cur && state_of_extent(new_cur) == cur_state) {
			/* extents can be unioned when they are holes or
			   unallocated extents or when they are adjacent
			   allocated extents */
			if (cur_state != ALLOCATED_EXTENT) {
				new_cur_width += cur_width;
				set_extent(new_cur, cur_state, 0, new_cur_width);
				continue;
			} else if (extent_get_start(new_cur) + new_cur_width == extent_get_start(cur)) {
				new_cur_width += cur_width;
				extent_set_width(new_cur, new_cur_width);
				continue;
			}
		}

		/* @ext can not be joined with @prev, move @prev forward */
		if (new_cur)
			new_cur++;
		else {
			assert("vs-935", cur == start);
			new_cur = start;
		}

		/* FIXME-VS: this is not necessary if new_cur == cur */
		*new_cur = *cur;
		new_cur_width = cur_width;
		new_num++;
	}

	if (new_num != old_num) {
		/* at least one pair of adjacent extents has merged. Shorten
		   item from the end correspondingly */
		int result;
		coord_t from, to;

		assert("vs-952", new_num < old_num);

		coord_dup(&from, item);
		from.unit_pos = new_num;
		from.between = AT_UNIT;

		coord_dup(&to, &from);
		to.unit_pos = old_num - 1;

		/* wipe part of item which is going to be cut, so that
		   node_check will not be confused by extent overlapping */
		memset(extent_by_coord(&from), 0, sizeof (reiser4_extent) * (old_num - new_num));
		result = cut_node(&from, &to, 0, 0, 0, DELETE_DONT_COMPACT, 0);

		/* nothing should happen cutting
		   FIXME: JMACD->VS: Just return the error! */
		assert("vs-456", result == 0);
	}
	check_me("nikita-2755", reiser4_grab_space_force(1, BA_RESERVED) == 0);
	znode_set_dirty(item->node);
}

/* return 1 if offset @off is inside of extent unit pointed to by @coord. Set pos_in_unit inside of unit
   correspondingly */
static int
offset_is_in_extent(const coord_t * coord, loff_t off, reiser4_block_nr * pos_in_unit)
{
	reiser4_key unit_key;
	loff_t unit_off;

	extent_unit_key(coord, &unit_key);
	unit_off = get_key_offset(&unit_key);
	if (off < unit_off)
		return 0;
	if (off >= (unit_off + (current_blocksize * extent_get_width(extent_by_coord(coord)))))
		return 0;
	if (pos_in_unit)
		*pos_in_unit = ((off - unit_off) >> current_blocksize_bits);
	return 1;
}

/* @coord is set to allocated extent. offset @off is inside that extent. return number of block corresponding to offset
   @off */
static reiser4_block_nr
blocknr_by_coord_in_extent(const coord_t * coord, reiser4_block_nr off)
{
	reiser4_block_nr pos_in_unit;

	assert("vs-12", coord_is_existing_unit(coord));
	assert("vs-264", state_of_extent(extent_by_coord(coord)) == ALLOCATED_EXTENT);
	check_me("vs-1092", offset_is_in_extent(coord, off, &pos_in_unit));

	return extent_get_start(extent_by_coord(coord)) + pos_in_unit;
}

/* Return the reiser_extent and position within that extent. */
static reiser4_extent *
extent_utmost_ext(const coord_t * coord, sideof side, reiser4_block_nr * pos_in_unit)
{
	reiser4_extent *ext;

	if (side == LEFT_SIDE) {
		/* get first extent of item */
		ext = extent_item(coord);
		*pos_in_unit = 0;
	} else {
		/* get last extent of item and last position within it */
		assert("vs-363", side == RIGHT_SIDE);
		ext = extent_item(coord) + coord_last_unit_pos(coord);
		*pos_in_unit = extent_get_width(ext) - 1;
	}

	return ext;
}

/* Return the child. Coord is set to extent item. Find jnode corresponding
   either to first or to last unformatted node pointed by the item */
int
extent_utmost_child(const coord_t * coord, sideof side, jnode ** childp)
{
	reiser4_extent *ext;
	reiser4_block_nr pos_in_unit;

	ext = extent_utmost_ext(coord, side, &pos_in_unit);

	switch (state_of_extent(ext)) {
	case HOLE_EXTENT:
		*childp = NULL;
		return 0;
	case ALLOCATED_EXTENT:
	case UNALLOCATED_EXTENT:
		break;
	}

	{
		reiser4_key key;
		reiser4_tree *tree;
		unsigned long index;

		if (side == LEFT_SIDE) {
			/* get key of first byte addressed by the extent */
			item_key_by_coord(coord, &key);
		} else {
			/* get key of last byte addressed by the extent */
			extent_max_key(coord, &key);
		}

		assert("vs-544", (get_key_offset(&key) >> PAGE_CACHE_SHIFT) < ~0ul);
		/* index of first or last (depending on @side) page addressed
		   by the extent */
		index = (unsigned long) (get_key_offset(&key) >> PAGE_CACHE_SHIFT);
		tree = current_tree;
		*childp = UNDER_SPIN(tree, tree, jlook(tree, get_key_objectid(&key), index));
	}

	return 0;
}

/* Return the child's block, if allocated. */
int
extent_utmost_child_real_block(const coord_t * coord, sideof side, reiser4_block_nr * block)
{
	reiser4_extent *ext;
	reiser4_block_nr pos_in_unit;

	ext = extent_utmost_ext(coord, side, &pos_in_unit);

	switch (state_of_extent(ext)) {
	case ALLOCATED_EXTENT:
		*block = extent_get_start(ext) + pos_in_unit;
		break;
	case HOLE_EXTENT:
	case UNALLOCATED_EXTENT:
		*block = 0;
		break;
	}

	return 0;
}

/* plugin->u.item.b.real_max_key_inside */
reiser4_key *
extent_max_key(const coord_t * coord, reiser4_key * key)
{
	last_key_in_extent(coord, key);
	assert("vs-610", get_key_offset(key) && (get_key_offset(key) & (current_blocksize - 1)) == 0);
	set_key_offset(key, get_key_offset(key) - 1);
	return key;
}

/* plugin->u.item.b.key_in_item
   return true if unit pointed by @coord addresses byte of file @key refers
   to */
int
extent_key_in_item(coord_t * coord, const reiser4_key * key)
{
	reiser4_key item_key;
	unsigned i, nr_units;
	__u64 offset;
	reiser4_extent *ext;

	assert("vs-771", coord_is_existing_item(coord));

	if (keygt(key, extent_max_key(coord, &item_key))) {
		/* key > max key of item */
		if (get_key_offset(key) == get_key_offset(&item_key) + 1) {
			coord->unit_pos = extent_nr_units(coord) - 1;
			coord->between = AFTER_UNIT;
			return 1;
		}
		return 0;
	}

	/* key of first byte pointed by item */
	item_key_by_coord(coord, &item_key);
	if (keylt(key, &item_key))
		/* key < min key of item */
		return 0;

	/* maybe coord is set already to needed unit */
	if (coord_is_existing_unit(coord) && extent_key_in_unit(coord, key))
		return 1;

	/* calculate position of unit containing key */
	ext = extent_item(coord);
	nr_units = extent_nr_units(coord);
	offset = get_key_offset(&item_key);
	for (i = 0; i < nr_units; i++, ext++) {
		offset += (current_blocksize * extent_get_width(ext));
		if (offset > get_key_offset(key)) {
			coord->unit_pos = i;
			coord->between = AT_UNIT;
			return 1;
		}
	}
       	impossible("vs-772", "key must be in item");
	return 0;
}

/* plugin->u.item.b.key_in_unit
   return true if unit pointed by @coord addresses byte of file @key refers
   to */
int
extent_key_in_unit(const coord_t * coord, const reiser4_key * key)
{
	reiser4_extent *ext;
	reiser4_key ext_key;

	assert("vs-770", coord_is_existing_unit(coord));

	/* key of first byte pointed by unit */
	unit_key_by_coord(coord, &ext_key);
	if (keylt(key, &ext_key))
		return 0;

	/* key of a byte next to the last byte pointed by unit */
	ext = extent_by_coord(coord);
	set_key_offset(&ext_key, get_key_offset(&ext_key) + extent_get_width(ext) * current_blocksize);
	return keylt(key, &ext_key);
}

/* plugin->u.item.b.item_stat */
void
extent_item_stat(const coord_t * coord, void *vp)
{
	reiser4_extent *ext;
	struct extent_stat *ex_stat;
	unsigned i, nr_units;

	ex_stat = (struct extent_stat *) vp;

	ext = extent_item(coord);
	nr_units = extent_nr_units(coord);

	for (i = 0; i < nr_units; i++) {
		switch (state_of_extent(ext + i)) {
		case ALLOCATED_EXTENT:
			ex_stat->allocated_units++;
			ex_stat->allocated_blocks += extent_get_width(ext + i);
			break;
		case UNALLOCATED_EXTENT:
			ex_stat->unallocated_units++;
			ex_stat->unallocated_blocks += extent_get_width(ext + i);
			break;
		case HOLE_EXTENT:
			ex_stat->hole_units++;
			ex_stat->hole_blocks += extent_get_width(ext + i);
			break;
		}
	}
}

/* @coord is set either to the end of last extent item of a file
   (coord->node is a node on the twig level) or to a place where first
   item of file has to be inserted to (coord->node is leaf
   node). Calculate size of hole to be inserted. If that hole is too
   big - only part of it is inserted */
static int
add_hole(coord_t * coord, lock_handle * lh, const reiser4_key * key)
{				/* key of position in a file for write */
	int result;
	znode *loaded;
	reiser4_extent *ext, new_ext;
	reiser4_block_nr hole_width;
	reiser4_item_data item;
	reiser4_key hole_key;

	result = zload(coord->node);
	if (result)
		return result;
	loaded = coord->node;

	if (znode_get_level(coord->node) == LEAF_LEVEL) {
		/* there are no items of this file yet. First item will be
		   hole extent inserted here */

		/* @coord must be set for inserting of new item */
		assert("vs-711", coord_is_between_items(coord));

		hole_key = *key;
		set_key_offset(&hole_key, 0ull);

		hole_width = ((get_key_offset(key) + current_blocksize - 1) >> current_blocksize_bits);
		assert("vs-710", hole_width > 0);

		/* compose body of hole extent */
		set_extent(&new_ext, HOLE_EXTENT, 0, hole_width);

		result = insert_extent_by_coord(coord, init_new_extent(&item, &new_ext, 1), &hole_key, lh);
		zrelse(loaded);
		coord->node = 0;
		return result;
	}

	/* last item of file may have to be appended with hole */
	assert("vs-708", znode_get_level(coord->node) == TWIG_LEVEL);
	assert("vs-714", item_id_by_coord(coord) == EXTENT_POINTER_ID);

	/* make sure we are at proper item */
	assert("vs-918", keylt(key, extent_max_key_inside(coord, &hole_key)));

	/* key of first byte which is not addressed by this extent */
	last_key_in_extent(coord, &hole_key);

	if (keyle(key, &hole_key)) {
		/* there is already extent unit which contains position
		   specified by @key */
		zrelse(loaded);
		return 0;
	}

	/* extent item has to be appended with hole. Calculate length of that
	   hole */
	hole_width = ((get_key_offset(key) - get_key_offset(&hole_key) +
		       current_blocksize - 1) >> current_blocksize_bits);
	assert("vs-954", hole_width > 0);

	/* set coord after last unit */
	coord_init_after_item_end(coord);

	/* get last extent in the item */
	ext = extent_by_coord(coord);
	if (state_of_extent(ext) == HOLE_EXTENT) {
		/* last extent of a file is hole extent. Widen that extent by
		   @hole_width blocks. Note that we do not worry about
		   overflowing - extent width is 64 bits */
		set_extent(ext, HOLE_EXTENT, 0, extent_get_width(ext) + hole_width);
		znode_set_dirty(coord->node);
		zrelse(loaded);
		return 0;
	}

	/* append item with hole extent unit */
	assert("vs-713", (state_of_extent(ext) == ALLOCATED_EXTENT || state_of_extent(ext) == UNALLOCATED_EXTENT));

	/* compose body of hole extent */
	set_extent(&new_ext, HOLE_EXTENT, 0, hole_width);

	result = insert_into_item(coord, lh, &hole_key, init_new_extent(&item, &new_ext, 1), 0 /*flags */ );
	zrelse(loaded);
	return result;
}

/* reiser4_read->unix_file_read->page_cache_readahead->reiser4_readpage->unix_file_readpage->extent_readpage
   or
   filemap_nopage->reiser4_readpage->unix_file_readpage->->extent_readpage
   
   At the beginning: coord.node is read locked, zloaded, page is
   locked, coord is set to existing unit inside of extent item
*/
int
extent_readpage(coord_t * coord, lock_handle * lh, struct page *page)
{
	ON_DEBUG(reiser4_key key;)
	reiser4_block_nr block;
	reiser4_block_nr pos;
	jnode *j;

	trace_on(TRACE_EXTENTS, "RP: index %lu, count %d..", page->index, page_count(page));

	assert("vs-1040", PageLocked(page));
	assert("vs-1050", !PageUptodate(page));
	assert("vs-757", !jprivate(page) && !PagePrivate(page));
	assert("vs-1039", page->mapping && page->mapping->host);
	assert("vs-1044", znode_is_loaded(coord->node));
	assert("vs-758", item_is_extent(coord));
	assert("vs-1046", coord_is_existing_unit(coord));
	assert("vs-1045", znode_is_rlocked(coord->node));
	assert("vs-1047", (page->mapping->host->i_ino == get_key_objectid(item_key_by_coord(coord, &key))));
	check_me("vs-1048", offset_is_in_extent(coord, ((loff_t) page->index) << PAGE_CACHE_SHIFT, &pos));

	switch (state_of_extent(extent_by_coord(coord))) {
	case HOLE_EXTENT:
		{
			char *kaddr = kmap_atomic(page, KM_USER0);

			memset(kaddr, 0, PAGE_CACHE_SIZE);
			flush_dcache_page(page);
			kunmap_atomic(kaddr, KM_USER0);
			SetPageUptodate(page);
			reiser4_unlock_page(page);

			trace_on(TRACE_EXTENTS, " - hole, OK\n");

			return 0;
		}

	case ALLOCATED_EXTENT:
		j = jnode_of_page(page);
		if (IS_ERR(j)) {
			reiser4_unlock_page(page);
			return PTR_ERR(j);
		}
		jnode_set_mapped(j);
		block = extent_get_start(extent_by_coord(coord)) + pos;
		jnode_set_block(j, &block);
		reiser4_stat_extent_add(unfm_block_reads);

		trace_on(TRACE_EXTENTS, " - allocated, read issued\n");

		break;

	case UNALLOCATED_EXTENT:
		{
			reiser4_tree *tree;

			info("extent_readpage: " "reading node corresponding to unallocated extent\n");
			tree = current_tree;
			j = UNDER_SPIN(tree, tree, jlook(tree, get_inode_oid(page->mapping->host), page->index));
			assert("nikita-2688", j);
			assert("nikita-2802", JF_ISSET(j, JNODE_EFLUSH));
			break;
		}

	default:
		impossible("vs-957", "extent_readpage: wrong extent");
		return -EIO;
	}

	page_io(page, j, READ, GFP_NOIO);
	jput(j);
	return 0;
}

static int make_extent(struct inode *inode, coord_t * coord, lock_handle * lh, jnode * j);

/* At the beginning: coord.node is read locked, zloaded, page is
   locked, coord is set to existing unit inside of extent item */
int
extent_writepage(coord_t * coord, lock_handle * lh, struct page *page)
{
	int result;
	jnode *j;

	trace_on(TRACE_EXTENTS, "WP: index %lu, count %d..", page->index, page_count(page));

	assert("vs-1052", PageLocked(page));
	assert("vs-1051", page->mapping && page->mapping->host);
	assert("vs-864", znode_is_wlocked(coord->node));

	j = jnode_of_page(page);
	if (IS_ERR(j))
		return PTR_ERR(j);

	reiser4_unlock_page(page);
	result = make_extent(page->mapping->host, coord, lh, j);
	reiser4_lock_page(page);
	if (result) {
		trace_on(TRACE_EXTENTS, "extent_writepage failed: %d\n", result);
		return result;
	}

	result = try_capture_page(page, ZNODE_WRITE_LOCK, 0);
	if (result)
		return result;
	jnode_set_dirty(j);
	jput(j);

	assert("vs-1073", PageDirty(page));

	trace_on(TRACE_EXTENTS, "OK\n");

	return 0;
}

int extent_get_block_address(const coord_t *coord, sector_t block, struct buffer_head *bh)
{
	if (state_of_extent(extent_by_coord(coord)) != ALLOCATED_EXTENT)
		bh->b_blocknr = 0;
	else
		bh->b_blocknr = blocknr_by_coord_in_extent(coord, block * current_blocksize);
	return 0;
}

/* this is filler for read_cache_page called via
   extent_read->read_cache_page->extent_readpage2 */
static int
filler(void *vp, struct page *page)
{
	return extent_readpage(vp, NULL, page);
}

/* Implements plugin->u.item.s.file.read operation for extent items. */
int
extent_read(struct inode *inode, coord_t *coord, flow_t * f)
{
	int result;
	struct page *page;
	unsigned long page_nr;
	unsigned page_off, count;
	char *kaddr;
	jnode *j;

	result = zload(coord->node);
	if (result)
		return result;

	if (!extent_key_in_item(coord, &f->key)) {
		zrelse(coord->node);
		return -EAGAIN;
	}

	page_nr = (get_key_offset(&f->key) >> PAGE_CACHE_SHIFT);

	/* this will return page if it exists and is uptodate, otherwise it
	   will allocate page and call extent_readpage to fill it */
	page = read_cache_page(inode->i_mapping, page_nr, filler, coord);

	if (IS_ERR(page)) {
		zrelse(coord->node);
		return PTR_ERR(page);
	}

	reiser4_lock_page(page);
	if (PagePrivate(page)) {
		j = jnode_by_page(page);
		if (REISER4_USE_EFLUSH && j)
			UNDER_SPIN_VOID(jnode, j, eflush_del(j, 1));
	}
	reiser4_unlock_page(page);

	if (!PageUptodate(page)) {
		page_detach_jnode(page, inode->i_mapping, page_nr);
		page_cache_release(page);
		warning("jmacd-97178", "extent_read: page is not up to date");
		zrelse(coord->node);
		return -EIO;
	}

	/* position within the page to read from */
	page_off = (get_key_offset(&f->key) & ~PAGE_CACHE_MASK);

	/* number of bytes which can be read from the page */
	if (page_nr == (inode->i_size >> PAGE_CACHE_SHIFT)) {
		/* we read last page of a file. Calculate size of file tail */
		count = inode->i_size & ~PAGE_CACHE_MASK;
	} else {
		count = PAGE_CACHE_SIZE;
	}
	assert("vs-388", count > page_off);
	count -= page_off;
	if (count > f->length)
		count = f->length;

	/* AUDIT: We must page-in/prepare user area first to avoid deadlocks */
	kaddr = kmap(page);
	assert("vs-572", f->user == 1);
	schedulable();

	result = __copy_to_user(f->data, kaddr + page_off, count);
	kunmap(page);

	page_cache_release(page);
	if (result) {
		zrelse(coord->node);
		return -EFAULT;
	}

	zrelse(coord->node);
	move_flow_forward(f, count);
	return 0;

}

#if 0

/* list of such structures (linked via field @next) is created during extent
   scanning. Every element has a list of pages (attached to field
   @pages). Number of pages in that list is stored in field @nr_pages. Those
   pages are freshly created and contiguous (belong to the same extent unit
   (@unit_pos)). Field @sector is block number of first page in the range */
/* FIXME-VS: why we do not create bio during scanning of extent? The reason is
   that we want (similar to sequence of these actions in the path
   page_cache_readahead -> do_page_cache_readahead -> read_pages) to allocate
   all readahead pages and to insert them into mapping's tree separately. Bio-s
   are created when pages get inserted into mapping's page tree by ourself. If
   we created bio-s during extent scanning we would have to split bio-s when we
   were not able to insert page into mapping's tree of pages
*/
struct page_range {
	struct list_head next;
	struct list_head pages;
	sector_t sector;
	int nr_pages;
};

#if REISER4_DEBUG_OUTPUT
static void
print_range_list(struct list_head *list)
{
	struct list_head *cur;
	struct page_range *range;
	unsigned long long sector;

	list_for_each(cur, list) {
		range = list_entry(cur, struct page_range, next);
		sector = range->sector;
		info("range: sector %llu, nr_pages %d\n", sector, range->nr_pages);
	}
}
#else
#define print_range_list(l) noop
#endif

static int
extent_end_io_read(struct bio *bio, unsigned int bytes_done UNUSED_ARG, int err UNUSED_ARG)
{
	int uptodate;
	struct bio_vec *bvec;
	struct page *page;
	int i;

	if (bio->bi_size != 0)
		return 1;

	uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	bvec = &bio->bi_io_vec[0];
	for (i = 0; i < bio->bi_vcnt; i++, bvec++) {
		page = bvec->bv_page;
		if (uptodate) {
			SetPageUptodate(page);
		} else {
			ClearPageUptodate(page);
			SetPageError(page);
		}
		reiser4_unlock_page(page);
	}
	bio_put(bio);
	return 0;
}

/* Implements plugin->u.item.s.file.readahead operation for extent items.
  
   scan extent item @coord starting from position addressing @start_page and
   create list of page ranges. Page range is a list of contiguous pages
   addressed by the same extent. Page gets into page range only if it is not
   yet attached to address space (this is checked by radix_tree_lookup). When
   page is found in address space - it is skipped and all the next pages get
   into different page range (even pages addressed by the same
   extent). Scanning stops either at the end of item or if number of pages for
   readahead @intrafile_readahead_amount is over.
  
   For every page range of the list: for every page from a range: add page into
   page cache, create bio and submit i/o. Note, that page range can be split
   into several bio-s as well when adding page into address space fails
   (because page is there already)
  
*/
int
extent_page_cache_readahead(struct file *file, coord_t * coord,
			    lock_handle * lh UNUSED_ARG,
			    unsigned long start_page, unsigned long intrafile_readahead_amount)
{
	struct address_space *mapping;
	reiser4_extent *ext;
	unsigned i, j, nr_units;
	__u64 pos_in_unit;
	LIST_HEAD(range_list);
	struct page_range *range;
	struct page *page;
	unsigned long left;
	__u64 pages;
	int sectors_per_block;

	sectors_per_block = (current_blocksize >> 9);

	page = 0;
	assert("vs-789", file && file->f_dentry && file->f_dentry->d_inode && file->f_dentry->d_inode->i_mapping);
	mapping = file->f_dentry->d_inode->i_mapping;

	assert("vs-779", current_blocksize == PAGE_CACHE_SIZE);
	assert("vs-782", intrafile_readahead_amount);
	/* make sure that unit, @coord is set to, addresses @start page */
	assert("vs-795", inode_file_plugin(mapping->host));
	assert("vs-796", inode_file_plugin(mapping->host)->key_by_inode);
	assert("vs-786", ( {
			  reiser4_key key;
			  inode_file_plugin(mapping->host)->key_by_inode(mapping->host,
									 (loff_t) start_page << PAGE_CACHE_SHIFT,
									 &key); extent_key_in_unit(coord, &key);
			  }));

	sectors_per_block = (current_blocksize >> 9);

	nr_units = extent_nr_units(coord);
	/* position in item matching to @start_page */
	ext = extent_by_coord(coord);
	pos_in_unit = in_extent(coord, (__u64) start_page << PAGE_CACHE_SHIFT);

	/* number of pages to readahead */
	left = intrafile_readahead_amount;

	/* while not all pages are allocated and item is not over */
	for (i = 0; left && (coord->unit_pos + i < nr_units); i++, pos_in_unit = 0) {
		extent_state state;

		/* how many pages from current extent to read ahead */
		pages = extent_get_width(&ext[i]) - pos_in_unit;
		if (pages > left)
			pages = left;

		state = state_of_extent(&ext[i]);
		if (state == UNALLOCATED_EXTENT) {
			/* these pages are in memory */
			start_page += pages;
			left -= pages;
			continue;
		}

		range = 0;
		read_lock(&mapping->page_lock);
		for (j = 0; j < pages; j++) {
			page = radix_tree_lookup(&mapping->page_tree, start_page + j);
			if (page) {
				/* page is already in address space */
				if (range) {
					/* contiguousness is broken */
					range = 0;
				}
				continue;
			}
			if (!range) {
				/* create new range of contiguous pages
				   belonging to one extent */
				/* FIXME-VS: maybe this should be after
				   read_unlock (&mapping->page_lock) */
				range = kmalloc(sizeof (struct page_range), GFP_KERNEL);
				if (!range)
					break;
				memset(range, 0, sizeof (struct page_range));
				range->nr_pages = 0;
				if (state == HOLE_EXTENT)
					range->sector = 0;
				else
					/* convert block number to sector
					   number (512) */
					range->sector = (extent_get_start(&ext[i]) + pos_in_unit) * sectors_per_block;
				INIT_LIST_HEAD(&range->pages);
				list_add_tail(&range->next, &range_list);
			}

			read_unlock(&mapping->page_lock);
			page = page_cache_alloc(mapping);
			read_lock(&mapping->page_lock);
			if (!page)
				break;
			page->index = start_page + j;
			list_add_tail(&page->list, &range->pages);
			range->nr_pages++;
			pos_in_unit++;
		}
		read_unlock(&mapping->page_lock);

		left -= pages;
		start_page += j;
	}
	/* FIXME-VS: remove after debugging */
	print_range_list(&range_list);

	/* submit bio for all ranges */
	{
		struct list_head *cur, *tmp;
		struct bio *bio;
		struct bio_vec *bvec;

		list_for_each_safe(cur, tmp, &range_list) {
			unsigned long prev;

			range = list_entry(cur, struct page_range, next);

			/* remove range from the list of ranges */
			list_del(&range->next);

			bio = NULL;

			/* FIXME-VS: if some of pages from the range are added
			   into mapping's tree already or number of pages is
			   too big for one bio - we will have to spilt the
			   range into several bio-s
			*/
			prev = ~0l u;
			while (range->nr_pages) {
				/* list of pages should contain at least one
				   page in it */
				assert("vs-798", !list_empty(&range->pages));

				if (range->sector != 0 && !bio) {
					/* create bio if it is not created yet
					   and if pages in this range are not
					   of hole */
					int vcnt;

					vcnt = BIO_MAX_SIZE / PAGE_CACHE_SIZE;
					if (vcnt > range->nr_pages)
						vcnt = range->nr_pages;
					bio = bio_alloc(GFP_KERNEL, vcnt);
					if (!bio) {
						/* FIXME-VS: drop all allocated pages? */
						continue;
					}
					bio->bi_bdev = mapping->host->i_sb->s_bdev;
					bio->bi_vcnt = vcnt;
					bio->bi_idx = 0;
					bio->bi_size = 0;
					bio->bi_end_io = extent_end_io_read;
					bio->bi_sector = range->sector;
					bio->bi_io_vec[0].bv_page = NULL;
				}

				/* take first page off the list */
				page = list_entry(range->pages.next, struct page, list);
				list_del(&page->list);
				range->nr_pages--;

				/* make sure that pages are in right order */
				assert("vs-800", ergo(prev != ~0l u, prev + 1 == page->index));
				prev = page->index;

				if (add_to_page_cache(page, mapping, page->index)) {
					/* someone else added this page */
					page_cache_release(page);
					if (bio) {
						bio->bi_vcnt = bio->bi_idx;
						if (bio->bi_idx) {
							/* only submit not
							   empty bio */
							reiser4_submit_bio(READ, bio);
							bio = 0;
						} else {
							;	/* use the same bio */
						}
					}
					continue;
				}

				/* page is added into mapping's tree of pages */

				if (range->sector == 0) {
					char *kaddr;
					/* this range matches to hole
					   extent. no bio is created */
					assert("vs-799", bio == 0);
					kaddr = kmap_atomic(page, KM_USER0);
					memset(kaddr, 0, PAGE_CACHE_SIZE);
					flush_dcache_page(page);
					kunmap_atomic(kaddr, KM_USER0);
					SetPageUptodate(page);
					reiser4_unlock_page(page);
					page_cache_release(page);
					continue;
				} else {
					assert("vs-814", bio);
					/* put this page into bio */
					bvec = &bio->bi_io_vec[bio->bi_idx];
					bvec->bv_page = page;
					bvec->bv_len = current_blocksize;
					bvec->bv_offset = 0;
					bio->bi_size += bvec->bv_len;
					bio->bi_idx++;
					page_cache_release(page);
					if (bio->bi_idx == BIO_MAX_SIZE / PAGE_CACHE_SIZE || !range->nr_pages) {
						/* bio is full or there are no
						   pages anymore */
						reiser4_submit_bio(READ, bio);
						bio = 0;
					}
				}

			}
			assert("vs-783", list_empty(&range->pages));
			assert("vs-788", !range->nr_pages);
			kfree(range);
		}
		assert("vs-785", list_empty(&range_list));
	}
	/* return number of pages readahead was performed for. It is not
	   necessary pages i/o was submitted for. Pages from readahead range
	   which were in cache are skipped here but they are included into
	   returned value */
	return intrafile_readahead_amount - left;
}
#endif

/* ask block allocator for some blocks */
static int
extent_allocate_blocks(reiser4_blocknr_hint * preceder,
		       reiser4_block_nr wanted_count, reiser4_block_nr * first_allocated, reiser4_block_nr * allocated)
{
	int result;

	*allocated = wanted_count;
	preceder->max_dist = 0;	/* scan whole disk, if needed */
	preceder->block_stage = BLOCK_UNALLOCATED;
	
	result = reiser4_alloc_blocks (preceder, first_allocated, allocated, BA_PERMANENT);

	if (result) {
		/* no free space
		   FIXME-VS: returning -ENOSPC is not enough
		   here. It should not happen actually
		*/
		impossible("vs-420", "could not allocate unallocated: %d", result);
	}

	return result;
}

/* unallocated extent of file with @objectid corresponding to @offset was
   replaced allocated extent [first, count]. Look for corresponding buffers in
   the page cache and map them properly
   FIXME-VS: this needs changes if blocksize != pagesize is needed
*/
static int
assign_jnode_blocknrs(reiser4_key * key, reiser4_block_nr first,
		      /* FIXME-VS: get better type for number of
		         blocks */
		      reiser4_block_nr count, flush_position * flush_pos)
{
	loff_t offset;
	unsigned long blocksize;
	unsigned long ind;
	jnode *j;
	int i, ret = 0;
	reiser4_tree *tree;

	blocksize = current_blocksize;
	assert("vs-749", blocksize == PAGE_CACHE_SIZE);

	tree = current_tree;

	/* offset of first byte addressed by block for which blocknr @first is
	   allocated */
	offset = get_key_offset(key);
	assert("vs-750", ((offset & (blocksize - 1)) == 0));

	for (i = 0; i < (int) count; i++, first++) {

		ind = offset >> PAGE_CACHE_SHIFT;

		j = UNDER_SPIN(tree, tree, jlook(tree, get_key_objectid(key), ind));
		if (!j) {
			/* this is possible through concurrent truncate */
			info("jnode not found. oid %llu, index %lu\n", get_key_objectid(key), ind);
			continue;
		}

		jnode_set_block(j, &first);

		/* If we allocated it cannot have been wandered -- in that case
		   extent_needs_allocation returns 0. */
		assert("jmacd-61442", !JF_ISSET(j, JNODE_OVRWR));
		jnode_set_reloc(j);

		/* Submit I/O and set the jnode clean. */
		ret = flush_enqueue_unformatted(j, flush_pos);
		jput(j);
		if (ret) {
			break;
		}

		offset += blocksize;
	}

	return ret;
}

/* return 1 if @extent unit needs allocation, 0 - otherwise. Try to update preceder in
   parent-first order for next block which will be allocated.
  
   Modified by Josh: Handles writing and relocating previously allocated extents.  The
   extent can be relocated by setting all of its blocks to dirty (assuming they are
   in-memory--more complex approaches are possible such as splitting the allocated extent
   and relocating part of it), then deallocating the old extent blocks (deferred) and
   setting the state to UNALLOCATED.  The calling code will then re-allocate them.
  
   If the ALLOCATED extent is not relocated, then its dirty blocks should be written via a
   call to flush_enqueue_unformatted.
  
   FIXME: JMACD->VS: Have I done it right? Can we remove the old FIXME below?
  
   FIXME-VS: this only returns 1 for unallocated extents. It may be modified to return 1
   for allocated extents all unformatted nodes of which are in memory. But that would
   require changes to allocate_extent_item as well.
*/
static int
extent_needs_allocation(reiser4_extent * extent, const coord_t * coord, flush_position * pos)
{
	extent_state st;
	reiser4_blocknr_hint *preceder;
	int relocate = 0;
	int ret;
	jnode *check;		/* this is used to check that all dirty jnodes are of
				 * the same atom */

	/* Handle the non-allocated cases. */
	switch ((st = state_of_extent(extent))) {
	case UNALLOCATED_EXTENT:
		return 1;
	case HOLE_EXTENT:
		return 0;
	default:
	}
	assert("jmacd-83112", st == ALLOCATED_EXTENT);
	assert("vs-1023", coord_is_existing_unit(coord));
	assert("vs-1022", item_is_extent(coord));
	assert("vs-1024", extent_by_coord(coord) == extent);

	preceder = flush_pos_hint(pos);

	/* Note: code very much copied from assign_jnode_blocknrs. */
	/* Find the inode (if it is in-memory). */
	{
		jnode *j;
		reiser4_key item_key;
		reiser4_block_nr start, count;
		loff_t offset;
		/*struct page * pg; */
		reiser4_tree *tree;
		unsigned long i, blocksize;
		unsigned long ind;
		int all_need_alloc = 1;

		unit_key_by_coord(coord, &item_key);

		/* Offset of first byte, blocksize */
		start = extent_get_start(extent);
		count = extent_get_width(extent);
		offset = get_key_offset(&item_key);
		blocksize = current_blocksize;
		assert("jmacd-748", count > 0);
		assert("jmacd-749", blocksize == PAGE_CACHE_SIZE);
		assert("jmacd-750", ((offset & (blocksize - 1)) == 0));
#if 0
		/* See if the extent is entirely dirty. */
		for (i = 0; i < count; i += 1, offset += blocksize) {

			ind = offset >> PAGE_CACHE_SHIFT;
			pg = reiser4_lock_page(inode->i_mapping, ind);

			if (pg == NULL) {
				all_need_alloc = 0;
				break;
			}

			j = jnode_of_page(pg);
			reiser4_unlock_page(pg);
			page_cache_release(pg);

			if (IS_ERR(j)) {
				all_need_alloc = 0;
				break;
			}

			/* Was (! jnode_check_dirty (j)) but the node may
			   already have been * allocated, in which case we
			   take the * previous allocation for this *
			   extent. */
			if (jnode_check_flushprepped(j)) {
				jput(j);
				all_need_alloc = 0;
				break;
			}

			jput(j);
		}
#endif
		/* If all blocks are dirty we may justify relocating this
		   extent. */

		/* FIXME: JMACD->HANS: It is very complicated to use the
		   formula you give in the document for extent-relocation
		   because asking "is there a closer allocation" may not have
		   a great answer.  There may be a closer allocation but it
		   may be not large enough.
		  
		   JOSH-FIXME-HANS
		  
		   Keep it simple and allocate it closer anyway.  If you keep
		   it simple, it will be easier for other future optimizations
		   to arrange for the right thing to be done.  For now just
		   implement the leaf_relocate threshold policy.  
		  
		   What does this mean?  Did you do it as requested or
		   differently?
		*/
		relocate = (all_need_alloc == 1)
		    && flush_pos_leaf_relocate(pos);
		/* FIXME-VS: no relocation of allocated extents yet */
		relocate = 0;
		check = 0;

		tree = current_tree;	/*tree_by_inode (inode); */

		/* Now scan through again. */
		offset = get_key_offset(&item_key);
		for (i = 0; i < count; i += 1, offset += blocksize) {
			ind = offset >> PAGE_CACHE_SHIFT;

			j = UNDER_SPIN(tree, tree, jlook(tree, get_key_objectid(&item_key), ind));
			if (!j) {
				continue;
			}

			if (!jnode_check_dirty(j)) {
				jput(j);
				continue;
			}

			if (REISER4_DEBUG) {
				/* all jnodes of this extent unit must belong
				   to one atom. Check that */
				if (check) {
					assert("vs-936", jnodes_of_one_atom(check, j));
				} else {
					check = jref(j);
				}
			}

			if (!jnode_check_flushprepped(j)	/* Was (jnode_check_dirty (j)),
								   * but allocated check prevents us
								   * from relocating/wandering a
								   * previously allocated block  */ ) {

				if (relocate == 0) {
					/* WANDER it */
					jnode_set_wander(j);
					jnode_set_clean(j);
				} else {
					/* this does not work now */
					/* Or else set RELOC.  It will get set again, but... */
					jnode_set_reloc(j);
					if ((ret = flush_enqueue_unformatted(j, pos))) {
						assert("jmacd-71891", ret < 0);
						jput(j);
						goto fail;
					}
				}
			}

			jput(j);
		}

		if (REISER4_DEBUG && check)
			jput(check);

		/* Now if relocating, free old blocks & change extent state */
		if (relocate == 1) {

			/* FIXME-VS: grab space first */
			/* FIXME: JMACD->ZAM: Is this right? */
			if ((ret = reiser4_dealloc_blocks(&start, &count, BLOCK_ALLOCATED, 
				BA_DEFER/* unformatted with defer */))) 
			{
				assert("jmacd-71892", ret < 0);
				goto fail;
			}

			extent_set_start(extent, 1ull /* UNALLOCATED_EXTENT */ );
		}
	}

	/* Recalculate preceder */
	if (relocate == 0) {
		preceder->blk = extent_get_start(extent) + extent_get_width(extent) - 1;
	}

	return relocate;

      fail:
	return ret;
}

/* if @key is glueable to the item @coord is set to */
static int
must_insert(coord_t * coord, reiser4_key * key)
{
	reiser4_key last;

	if (item_id_by_coord(coord) == EXTENT_POINTER_ID && keyeq(last_key_in_extent(coord, &last), key))
		return 0;
	return 1;
}

/* helper for allocate_and_copy_extent
   append last item in the @node with @data if @data and last item are
   mergeable, otherwise insert @data after last item in @node. Have carry to
   put new data in available space only. This is because we are in squeezing.
  
   FIXME-VS: me might want to try to union last extent in item @left and @data
*/
static int
put_unit_to_end(znode * node, reiser4_key * key, reiser4_item_data * data)
{
	int result;
	coord_t coord;
	cop_insert_flag flags;

	/* set coord after last unit in an item */
	coord_init_last_unit(&coord, node);
	coord.between = AFTER_UNIT;

	flags = COPI_DONT_SHIFT_LEFT | COPI_DONT_SHIFT_RIGHT | COPI_DONT_ALLOCATE;
	if (must_insert(&coord, key)) {
		result = insert_by_coord(&coord, data, key, 0 /*lh */ , 0 /*ra */ ,
					 0 /*ira */ , flags);

	} else {
		result = insert_into_item(&coord, 0 /*lh */ , key, data, flags);
	}

	assert("vs-438", result == 0 || result == -ENOSPC);
	return result;
}

/* if last item of node @left is extent item and if its last unit is allocated
   extent and if @key is adjacent to that item and if @first_allocated is
   adjacent to last block pointed by that last extent - expand that extent by
   @allocated and return 1, 0 - otherwise
*/
static int
try_to_glue(znode * left, reiser4_block_nr first_allocated, reiser4_block_nr allocated, const reiser4_key * key)
{
	coord_t last;
	reiser4_key last_key;
	reiser4_extent *ext;

	assert("vs-463", !node_is_empty(left));

	coord_init_last_unit(&last, left);
	if (!item_is_extent(&last))
		return 0;

	if (!keyeq(last_key_in_extent(&last, &last_key), key))
		return 0;

	ext = extent_by_coord(&last);
	if (state_of_extent(ext) != ALLOCATED_EXTENT) {
		assert("vs-971", state_of_extent(ext) == HOLE_EXTENT);
		return 0;
	}

	if (extent_get_start(ext) + extent_get_width(ext) != first_allocated)
		return 0;

	extent_set_width(ext, extent_get_width(ext) + allocated);
	znode_set_dirty(left);
	return 1;
}

#if REISER4_USE_EFLUSH
static void
unflush_finish(coord_t *coord, __u64 done)
{
	reiser4_extent *ext;
	reiser4_key     key;
	oid_t           oid;
	__u64           i;
	unsigned long   ind;
	int             result;

	assert("nikita-2793", item_is_extent(coord));

	result = 0;

	ext = extent_by_coord(coord);

	unit_key_by_coord(coord, &key);

	oid   = get_key_objectid(&key);
	ind   = get_key_offset(&key) >> PAGE_CACHE_SHIFT;
	
	for (result = 0, i = 0 ; i < done ; ++ i, ++ ind) {
		jnode  *node;
		reiser4_tree *tree;

		tree = current_tree;
		node = UNDER_SPIN(tree, tree, jlook(tree, oid, ind));
		if (node == NULL)
			continue;
		jrelse(node);
		jput(node);
	}
}

static int
unflush(coord_t *coord)
{
	reiser4_extent *ext;
	reiser4_key     key;
	oid_t           oid;
	__u64           width;
	__u64           i;
	unsigned long   ind;
	int             result;

	assert("nikita-2793", item_is_extent(coord));

	result = 0;

	ext = extent_by_coord(coord);

	unit_key_by_coord(coord, &key);

	width = extent_get_width(ext);
	oid   = get_key_objectid(&key);
	ind   = get_key_offset(&key) >> PAGE_CACHE_SHIFT;
	
	for (result = 0, i = 0 ; i < width ; ++ i, ++ ind) {
		jnode  *node;
		reiser4_tree *tree;

		tree = current_tree;
		node = UNDER_SPIN(tree, tree, jlook(tree, oid, ind));
		if (node == NULL)
			continue;
		result = jload(node);
		jput(node);
		if (result != 0) {
			unflush_finish(coord, i);
			break;
		}
	}
	return result;
}
#else
#define unflush_finish(coord, done) noop
#define unflush(coord) (0)
#endif

/* @right is extent item. @left is left neighbor of @right->node. Copy item
   @right to @left unit by unit. Units which do not require allocation are
   copied as they are. Units requiring allocation are copied after destinating
   start block number and extent size to them. Those units may get inflated due
   to impossibility to allocate desired number of contiguous free blocks
   @flush_pos - needs comment
*/
int
allocate_and_copy_extent(znode * left, coord_t * right, flush_position * flush_pos,
			 /* biggest key which was moved, it is maintained
			    while shifting is in progress and is used to
			    cut right node at the end
			 */
			 reiser4_key * stop_key)
{
	int result;
	reiser4_item_data data;
	reiser4_key key;
	unsigned long blocksize;
	reiser4_block_nr first_allocated;
	reiser4_block_nr to_allocate, allocated;
	reiser4_extent *ext, new_ext;

	blocksize = current_blocksize;

	optimize_extent(right);

	/* make sure that we start from first unit of the item */
	assert("vs-419", item_id_by_coord(right) == EXTENT_POINTER_ID);
	assert("vs-418", right->unit_pos == 0);
	assert("vs-794", right->between == AT_UNIT);

	result = SQUEEZE_CONTINUE;
	item_key_by_coord(right, &key);

	ext = extent_item(right);
	for (; right->unit_pos < coord_num_units(right); right->unit_pos++, ext++) {
		reiser4_block_nr width;

		trace_on(TRACE_EXTENTS, "alloc_and_copy_extent: unit %u/%u\n", right->unit_pos, coord_num_units(right));

		width = extent_get_width(ext);
		if ((result = extent_needs_allocation(ext, right, flush_pos)) < 0) {
			goto done;
		}

		if (!result) {
			/* unit does not require allocation, copy this unit as
			   it is */
			result = put_unit_to_end(left, &key, init_new_extent(&data, ext, 1));
			if (result == -ENOSPC) {
				/* left->node does not have enough free space
				   for this unit */
				result = SQUEEZE_TARGET_FULL;
				trace_on(TRACE_EXTENTS, "alloc_and_copy_extent: target full, !needs_allocation\n");
				/* set coord @right such that this unit does
				   not get cut because it was not moved */
				right->between = BEFORE_UNIT;
				goto done;
			}
			/* update stop key */
			set_key_offset(&key, get_key_offset(&key) + width * blocksize);
			*stop_key = key;
			set_key_offset(stop_key, get_key_offset(&key) - 1);
			result = SQUEEZE_CONTINUE;
			continue;
		}

		assert("vs-959", state_of_extent(ext) == UNALLOCATED_EXTENT);

		if((result = unflush(right)))
			goto done;

		/* extent must be allocated */
		to_allocate = width;
		/* until whole extent is allocated and there is space in left
		   neighbor */
		while (to_allocate) {
			result =
			    extent_allocate_blocks(flush_pos_hint(flush_pos),
						   to_allocate, &first_allocated, &allocated);
			if (result)
				goto finish_unflush;

			trace_on(TRACE_EXTENTS,
				 "alloc_and_copy_extent: to_allocate = %llu got %llu\n", to_allocate, allocated);

			to_allocate -= allocated;

			/* FIXME: JMACD->ZAM->VS: I think the block allocator should do this. */
			flush_pos_hint(flush_pos)->blk += allocated;

			if (!try_to_glue(left, first_allocated, allocated, &key)) {
				/* could not copy current extent by just
				   increasing width of last extent in left
				   neighbor, add new extent to the end of
				   @left
				*/
				extent_set_start(&new_ext, first_allocated);
				extent_set_width(&new_ext, (reiser4_block_nr) allocated);

				result = put_unit_to_end(left, &key, init_new_extent(&data, &new_ext, 1));
				if (result == -ENOSPC) {
					/* @left is full, free blocks we
					   grabbed because this extent will
					   stay in right->node, and, therefore,
					   blocks it points to have different
					   predecer in parent-first order. Set
					   "defer" parameter of
					   reiser4_dealloc_block to 0 because
					   these blocks can be made allocable
					   again immediately.
					*/
					reiser4_dealloc_blocks(&first_allocated, &allocated,
						BLOCK_UNALLOCATED, BA_PERMANENT);

					result = SQUEEZE_TARGET_FULL;
					trace_on(TRACE_EXTENTS,
						 "alloc_and_copy_extent: target full, to_allocate = %llu\n",
						 to_allocate);
					/**/ if (to_allocate == width) {
						/* nothing of this unit were
						   allocated and copied. Take
						   care that it does not get
						   cut */
						right->between = BEFORE_UNIT;
					} else {
						/* part of extent was allocated
						   and copied to left
						   neighbor. Leave coord @right
						   at this unit so that it will
						   be cut properly by
						   squalloc_right_twig_cut */
					}

					goto finish_unflush;
				}
			}
			/* find all pages for which blocks were allocated and
			   assign block numbers to jnodes of those pages */
			result = assign_jnode_blocknrs(&key, first_allocated, allocated, flush_pos);
			if (result)
				goto finish_unflush;

			/* update stop key */
			set_key_offset(&key, get_key_offset(&key) + allocated * blocksize);
			*stop_key = key;
			set_key_offset(stop_key, get_key_offset(&key) - 1);
			result = SQUEEZE_CONTINUE;
		}
	finish_unflush:
		unflush_finish(right, width);

		if (result < 0)
			break;
	}
 done:

	assert("vs-421", result < 0 || result == SQUEEZE_TARGET_FULL || SQUEEZE_CONTINUE);

	assert("vs-678", item_is_extent(right));

	if (right->unit_pos == coord_num_units(right)) {
		/* whole item was allocated and copied, set coord after item */
		right->unit_pos = 0;
		right->between = AFTER_ITEM;
	}
	return result;
}

/* used in allocate_extent_item_in_place and plug_hole to replace @un_extent
   with either two or three extents

   (@un_extent) with allocated (@star, @alloc_width) and unallocated
   (@unalloc_width). Have insert_into_item to not try to shift anything to
   left.
*/
static int
replace_extent(coord_t * un_extent, lock_handle * lh,
	       reiser4_key * key, reiser4_item_data * data, const reiser4_extent * new_ext, unsigned flags)
{
	int result;
	coord_t coord_after;
	lock_handle lh_after;
	tap_t watch;
	reiser4_extent orig_ext;	/* this is for debugging */
	znode *orig_znode;
	reiser4_block_nr grabbed, needed;

	assert("vs-990", coord_is_existing_unit(un_extent));

	coord_dup(&coord_after, un_extent);
	init_lh(&lh_after);
	copy_lh(&lh_after, lh);
	tap_init(&watch, &coord_after, &lh_after, ZNODE_WRITE_LOCK);
	tap_monitor(&watch);

	orig_ext = *extent_by_coord(un_extent);
	orig_znode = un_extent->node;

	/* make sure that key is set properly */
	if (REISER4_DEBUG) {
		reiser4_key tmp;

		unit_key_by_coord(un_extent, &tmp);
		set_key_offset(&tmp, get_key_offset(&tmp) + extent_get_width(new_ext) * current_blocksize);
		assert("vs-1080", keyeq(&tmp, key));
	}

	grabbed = get_current_context()->grabbed_blocks;
	needed = estimate_internal_amount(1, znode_get_tree(orig_znode)->height);
	/* Grab from 100% of disk space, not 95% as usual. */
	if (reiser4_grab_space_force(needed, BA_RESERVED))
		reiser4_panic("vpf-340", "No space left in reserved area.");
	
	/* set insert point after unit to be replaced */
	un_extent->between = AFTER_UNIT;
	result = insert_into_item(un_extent, (flags == COPI_DONT_SHIFT_LEFT) ? 0 : lh, key, data, flags);
	
	/* While assigning unallocated block to block numbers we need to insert 
	    new extent units - this may lead to new block allocation on twig and 
	    upper levels. Take these blocks from 5% of disk space. */
	grabbed2free(get_current_context()->grabbed_blocks - grabbed);
	    
	if (!result) {
		reiser4_extent *ext;

		if (coord_after.node != orig_znode)
			result = zload(coord_after.node);

		if (likely(!result)) {
			ext = extent_by_coord(&coord_after);

			assert("vs-987", znode_is_loaded(coord_after.node));
			assert("vs-988", !memcmp(ext, &orig_ext, sizeof (*ext)));

			*ext = *new_ext;
			znode_set_dirty(coord_after.node);

			if (coord_after.node != orig_znode)
				zrelse(coord_after.node);
		}
	}
	tap_done(&watch);
	return result;
}

/* find all units of extent item which require allocation. Allocate free blocks
   for them and replace those extents with new ones. As result of this item may
   "inflate", so, special precautions are taken to have it to inflate to right
   only, so items to the right of @item and part of item itself may get moved
   to right
*/
int
allocate_extent_item_in_place(coord_t * coord, lock_handle * lh, flush_position * flush_pos)
{
	int result = 0;
	unsigned i, num_units, orig_item_pos;
	reiser4_extent *ext;
	reiser4_block_nr first_allocated;
	reiser4_block_nr initial_width, allocated;
	unsigned long blocksize;
	reiser4_extent replace, paste;
	reiser4_item_data item;
	reiser4_key key, orig_key;
	znode *orig;
	struct inode *object;

	assert("vs-1019", item_is_extent(coord));
	assert("vs-1018", coord_is_existing_unit(coord));
	assert("zam-807", znode_is_write_locked(coord->node));

	blocksize = current_blocksize;

	ext = extent_by_coord(coord);
	num_units = coord_num_units(coord);

	orig_item_pos = coord->item_pos;
	item_key_by_coord(coord, &orig_key);
	object = NULL;

	for (i = coord->unit_pos; i < num_units; i++, ext++) {
		coord->unit_pos = i;
		coord->between = AT_UNIT;

		assert("vs-1021", coord->item_pos == orig_item_pos);
		assert("vs-1020", keyeq(item_key_by_coord(coord, &key), &orig_key));

		if ((result = extent_needs_allocation(ext, coord, flush_pos)) < 0) {
			break;
		}

		if (!result /* extent does not need allocation */ ) {
			continue;
		}

		assert("vs-439", state_of_extent(ext) == UNALLOCATED_EXTENT);

		/* try to get extent_width () free blocks starting from
		   *preceder */
		initial_width = extent_get_width(ext);

		/* inform block allocator that those blocks were in
		   UNALLOCATED stage */
		flush_pos_hint(flush_pos)->block_stage = BLOCK_UNALLOCATED;

		/*
		 * eflush<->extentallocation interactions.
		 *
		 * If node from unallocated extent was eflushed following bad
		 * things can happen:
		 *
		 *   . block reserved for this node in fake block space is
		 *   already used, and extent_allocate_blocks() will underflow
		 *   fake_allocated counter.
		 *
		 *   . emergency block is marked as used in bitmap, so it is
		 *   possible for extent_allocate_blocks() to just be unable
		 *   to find enough free space on the disk.
		 *
		 * Current solution is to unflush all eflushed jnodes. This
		 * solves both problems, but this is suboptimal, because:
		 *
		 *   . as a result extents jnodes are scanned trice.
		 *
		 *   . this can exhaust memory (nodes were eflushed, because
		 *   we were short on memory in the first place).
		 *
		 * Possible proper solutions:
		 *
		 *   . scan extent. If eflushed jnode is found, split extent
		 *   into three: extent before, single-block-extent, extent
		 *   after. This requires mechanism to block eflush for jnodes
		 *   of this extent during this extent allocation (easy to
		 *   implement through inode flag).
		 *
		 *   . allocate chunk of extent. Scan this chunk, for each
		 *   eflushed jnode, load it, clear JNODE_EFLUSH bit and
		 *   release. This requires handling of temporary underflow of
		 *   fake space.
		 *
		 */

		result = unflush(coord);
		if (result)
			break;

		result = extent_allocate_blocks(flush_pos_hint(flush_pos), 
						initial_width, 
						&first_allocated, &allocated);
		unflush_finish(coord, initial_width);

		if (result)
			break;

		assert("vs-440", allocated > 0);
		/* update flush_pos's preceder to last allocated block number */
		flush_pos_hint(flush_pos)->blk = first_allocated + allocated - 1;

		unit_key_by_coord(coord, &key);
		/* find all pages for which blocks were allocated and assign
		   block numbers to jnodes of those pages */
		if ((result = assign_jnode_blocknrs(&key, first_allocated, allocated, flush_pos))) {
			break;
		}

		set_extent(&replace, ALLOCATED_EXTENT, first_allocated, allocated);
		if (allocated == initial_width) {
			int ret;
			/* whole extent is allocated */
			*ext = replace;

			/* grabbing one block for this dirty znode */
			if ((ret = reiser4_grab_space_force(1, BA_RESERVED)) != 0)
				break;
			
			znode_set_dirty(coord->node);
			continue;
		}

		/* set @key to key of first byte of part of extent which left
		   unallocated */
		set_key_offset(&key, get_key_offset(&key) + allocated * blocksize);
		set_extent(&paste, UNALLOCATED_EXTENT, 0, initial_width - allocated);

		/* [u/initial_width] ->
		   [first_allocated/allocated][u/initial_width - allocated] */
		orig = coord->node;
		result = replace_extent(coord, lh, &key,
					init_new_extent(&item, &paste, 1), &replace, COPI_DONT_SHIFT_LEFT);
		if (result)
			break;

		/* coord->node may change in balancing. Nevertheless,
		   lh->node must still be set to node replace started
		   from */
		assert("vs-1026", orig == lh->node);
		coord->node = orig;
		coord->item_pos = orig_item_pos;

		/* number of units in extent item might change */
		num_units = coord_num_units(coord);
	}

	/* content of extent item may be optimize-able (it may have
	   mergeable extents)) */
	optimize_extent(coord);

	/* set coord after last unit in the item */
	assert("vs-679", item_is_extent(coord));
	coord->unit_pos = coord_last_unit_pos(coord);
	coord->between = AFTER_UNIT;

	return result;
}

/* Block offset of first block addressed by unit */
/* AUDIT shouldn't return value be of reiser4_block_nr type?
   Josh's answer: who knows?  This returns the same type of information as "struct page->index", which is currently an unsigned long. */
__u64 extent_unit_index(const coord_t * item)
{
	reiser4_key key;

	assert("vs-648", coord_is_existing_unit(item));
	unit_key_by_coord(item, &key);
	return get_key_offset(&key) >> current_blocksize_bits;
}

/* AUDIT shouldn't return value be of reiser4_block_nr type?
   Josh's answer: who knows?  Is a "number of blocks" the same type as "block offset"? */
__u64 extent_unit_width(const coord_t * item)
{
	assert("vs-649", coord_is_existing_unit(item));
	return width_by_coord(item);
}

/* Starting block location of this unit. */
reiser4_block_nr extent_unit_start(const coord_t * item)
{
	return extent_get_start(extent_by_coord(item));
}

static void
extent_assign_fake_blocknr(jnode * j)
{
	reiser4_block_nr fake_blocknr;

	assign_fake_blocknr(&fake_blocknr, 0 /*unformatted */ );
	jnode_set_block(j, &fake_blocknr);
}

/* insert extent item (containing one unallocated extent of width 1) to place
   set by @coord */
static int
insert_first_block(coord_t * coord, lock_handle * lh, jnode * j, const reiser4_key * key)
{
	int result;
	reiser4_extent ext;
	reiser4_item_data unit;

	/* make sure that we really write to first block */
	assert("vs-240", get_key_offset(key) == 0);
	assert("zam-809", znode_is_write_locked(coord->node));

	/* extent insertion starts at leaf level */
	assert("vs-719", znode_get_level(coord->node) == LEAF_LEVEL);

	set_extent(&ext, UNALLOCATED_EXTENT, 0, 1ull);
	result = insert_extent_by_coord(coord, init_new_extent(&unit, &ext, 1), key, lh);
	if (result) {
		/* FIXME-VITALY: this is grabbed at file_write time. */
		/* grabbed2free ((__u64)1); */
		return result;
	}

	jnode_set_mapped(j);
	jnode_set_created(j);
	extent_assign_fake_blocknr(j);

	/*reiser4_stat_file_add (pointers);
	   reiser4_stat_file_add (write_repeats); */

	/* this is to indicate that research must be performed to continue
	   write. This is because coord->node is leaf node whereas write
	   continues in twig level
	*/
	coord->node = 0;
	return 0;
}

/* @coord is set to the end of extent item. Append it with pointer to one
   block - either by expanding last unallocated extent or by appending a new
   one of width 1 */
static int
append_one_block(coord_t * coord, lock_handle * lh, jnode * j, reiser4_key * key)
{
	int result;
	reiser4_extent *ext, new_ext;
	reiser4_item_data unit;

	assert("vs-228", (coord->unit_pos == coord_last_unit_pos(coord) && coord->between == AFTER_UNIT));
	assert("vs-883", ( {
			  reiser4_key next; keyeq(key, last_key_in_extent(coord, &next));
			  }));

	assert("zam-810", znode_is_write_locked(coord->node));

	ext = extent_by_coord(coord);
	switch (state_of_extent(ext)) {
	case UNALLOCATED_EXTENT:
		set_extent(ext, UNALLOCATED_EXTENT, 0, extent_get_width(ext) + 1);
		znode_set_dirty(coord->node);
		break;

	case HOLE_EXTENT:
	case ALLOCATED_EXTENT:
		/* we have to append one extent because last extent either not
		   unallocated extent or is full */
		set_extent(&new_ext, UNALLOCATED_EXTENT, 0, 1ull);
		result = insert_into_item(coord, lh, key, init_new_extent(&unit, &new_ext, 1), 0 /* flags */ );
		if (result) {
			/* FIXME-VITALY: this is grabbed at file_write time. */
			/*grabbed2free ((__u64)1);*/
			return result;
		}
		break;
	}

	jnode_set_mapped(j);
	jnode_set_created(j);
	extent_assign_fake_blocknr(j);

	/*reiser4_stat_file_add (pointers); */
	return 0;
}

/* @coord is set to hole unit inside of extent item, replace hole unit with an
   unit for unallocated extent of the width 1, and perhaps a hole unit before
   the unallocated unit and perhaps a hole unit after the unallocated unit. */
static int
plug_hole(coord_t * coord, lock_handle * lh, reiser4_key * key)
{
	reiser4_extent *ext, new_exts[2],	/* extents which will be added after original
						 * hole one */
	 replace;		/* extent original hole extent will be replaced
				 * with */
	reiser4_block_nr width, pos_in_unit;
	reiser4_item_data item;
	int count;

	assert("vs-234", coord_is_existing_unit(coord));

	ext = extent_by_coord(coord);
	width = extent_get_width(ext);
	check_me("vs-1090", offset_is_in_extent(coord, get_key_offset(key), &pos_in_unit));

	if (width == 1) {
		set_extent(ext, UNALLOCATED_EXTENT, 0, 1ull);
		znode_set_dirty(coord->node);
		return 0;
	} else if (pos_in_unit == 0) {
		if (coord->unit_pos) {
			if (state_of_extent(ext - 1) == UNALLOCATED_EXTENT) {
				extent_set_width(ext - 1, extent_get_width(ext - 1) + 1);
				extent_set_width(ext, width - 1);
				znode_set_dirty(coord->node);
				return 0;
			}
		}
		/* extent for replace */
		set_extent(&replace, UNALLOCATED_EXTENT, 0, 1);
		/* extent to be inserted */
		set_extent(&new_exts[0], HOLE_EXTENT, 0, width - 1);
		count = 1;
	} else if (pos_in_unit == width - 1) {
		if (coord->unit_pos < extent_nr_units(coord) - 1) {
			if (state_of_extent(ext + 1) == UNALLOCATED_EXTENT) {
				extent_set_width(ext + 1, extent_get_width(ext + 1) + 1);
				extent_set_width(ext, width - 1);
				znode_set_dirty(coord->node);
				return 0;
			}
		}
		/* extent for replace */
		set_extent(&replace, HOLE_EXTENT, 0, width - 1);
		/* extent to be inserted */
		set_extent(&new_exts[0], UNALLOCATED_EXTENT, 0, 1ull);
		count = 1;
	} else {
		/* extent for replace */
		set_extent(&replace, HOLE_EXTENT, 0, pos_in_unit);
		/* extent to be inserted */
		set_extent(&new_exts[0], UNALLOCATED_EXTENT, 0, 1ull);
		set_extent(&new_exts[1], HOLE_EXTENT, 0, width - pos_in_unit - 1);
		count = 2;
	}

	/* insert_into_item will insert new units after the one @coord is set
	   to. So, update key correspondingly */
	unit_key_by_coord(coord, key);	/* FIXME-VS: how does it work without this? */
	set_key_offset(key, (get_key_offset(key) + extent_get_width(&replace) * current_blocksize));

	return replace_extent(coord, lh, key, init_new_extent(&item, new_exts, count), &replace, 0	/* flags */
	    );
}

/* pointer to block for @bh exists in extent item and it is addressed by
   @coord. If it is hole - make unallocated extent for it. */
/* Audited by: green(2002.06.13) */
static int
overwrite_one_block(coord_t * coord, lock_handle * lh, jnode * j, reiser4_key * key)
{
	int result;
	reiser4_extent *ext;
	reiser4_block_nr block;

	ext = extent_by_coord(coord);

	switch (state_of_extent(ext)) {
	case ALLOCATED_EXTENT:
		block = blocknr_by_coord_in_extent(coord, get_key_offset(key));
		jnode_set_mapped(j);
		jnode_set_block(j, &block);
		break;

	case UNALLOCATED_EXTENT:
		jnode_set_mapped(j);
		break;

	case HOLE_EXTENT:

		result = plug_hole(coord, lh, key);
		if (result)
			return result;

		jnode_set_mapped(j);
		jnode_set_created(j);
		extent_assign_fake_blocknr(j);

		/*reiser4_stat_file_add (pointers); */
		break;

	default:
		impossible("vs-238", "extent of unknown type found");
		return -EIO;
	}

	return 0;
}

static int
make_extent(struct inode *inode, coord_t * coord, lock_handle * lh, jnode * j)
{
	int result;
	znode *loaded;
	reiser4_key key;
	write_mode todo;

	assert("vs-960", znode_is_write_locked(coord->node));

	/* key of first byte of the page */
	inode_file_plugin(inode)->key_by_inode(inode, (loff_t) jnode_page(j)->index << PAGE_CACHE_SHIFT, &key);

	todo = how_to_write(coord, lh, &key);
	if (unlikely(todo < 0))
		return todo;

	/* zload is necessary because balancing may return coord->node moved to another possibly not loaded node */
	result = zload(coord->node);
	if (result)
		return result;
	loaded = coord->node;

	switch (todo) {
	case FIRST_ITEM:
		/* create first item of the file */
		result = insert_first_block(coord, lh, j, &key);
		break;

	case APPEND_ITEM:
		result = append_one_block(coord, lh, j, &key);
		break;

	case OVERWRITE_ITEM:
		result = overwrite_one_block(coord, lh, j, &key);
		break;
	case RESEARCH:
		/* @coord is not set to a place in a file we have to write to,
		   so, coord_by_key must be called to find that place */
		/*reiser4_stat_file_add (write_repeats); */
		result = -EAGAIN;
		break;
	}

	zrelse(loaded);
	return result;
}

/* if page is not completely overwritten - read it if it is not
   new or fill by zeros otherwise */
static int
prepare_page(struct inode *inode, struct page *page, loff_t file_off, unsigned from, unsigned count)
{
	char *data;
	jnode *j;

	if (PageUptodate(page))
		return 0;

	if (count == current_blocksize)
		return 0;

	j = jnode_by_page(page);

	if (jnode_created(j)) {
		/* new page does not get zeroed. Fill areas around write one by
		   0s */
		assert("vs-957", blocknr_is_fake(jnode_get_block(j)));

		data = kmap_atomic(page, KM_USER0);
		memset(data, 0, from);
		memset(data + from + count, 0, PAGE_CACHE_SIZE - from - count);
		flush_dcache_page(page);
		kunmap_atomic(data, KM_USER0);
		return 0;
	}

	/* page contains some data of this file */
	assert("vs-699", inode->i_size > page->index << PAGE_CACHE_SHIFT);

	if (from == 0 && file_off + count >= inode->i_size) {
		/* current end of file is in this page. write areas covers it
		   all. No need to read block. Zero page past new end of file,
		   though */
		data = kmap_atomic(page, KM_USER0);
		memset(data + from + count, 0, PAGE_CACHE_SIZE - from - count);
		kunmap_atomic(data, KM_USER0);
		return 0;
	}

	/* read block because its content is not completely overwritten */
	reiser4_stat_extent_add(unfm_block_reads);

	page_io(page, j, READ, GFP_NOIO);

	reiser4_lock_page(page);
	UNDER_SPIN_VOID(jnode, j, eflush_del(j, 1));

	if (!PageUptodate(page)) {
		warning("jmacd-61238", "prepare_page: page not up to date");
		return -EIO;
	}
	return 0;
}

/* drop longterm znode lock before calling balance_dirty_pages. balance_dirty_pages may cause transaction to close,
   therefore we have to update stat data if necessary */
static int extent_balance_dirty_pages(struct address_space *mapping, const flow_t *f, coord_t *coord, lock_handle *lh)
{
	int result;
	struct sealed_coord hint;

	set_hint(&hint, &f->key, coord);
	done_lh(lh);
	coord->node = 0;
	result = update_sd_if_necessary(mapping->host, f);
	if (result)
		return result;

	balance_dirty_pages(mapping);
	return hint_validate(&hint, &f->key, coord, lh);
}

/* write flow's data into file by pages */
static int
extent_write_flow(struct inode *inode, coord_t *coord, lock_handle *lh, flow_t * f)
{
	int result;
	loff_t file_off;
	unsigned page_off, to_page;
	char *data;
	struct page *page;
	jnode *j;

	assert("vs-885", current_blocksize == PAGE_CACHE_SIZE);
	assert("vs-700", f->user == 1);

	result = 0;

	if (DQUOT_ALLOC_SPACE_NODIRTY(inode, f->length))
		return -EDQUOT;

	/* write position */
	file_off = get_key_offset(&f->key);

	/* this can be != 0 only for the first of pages which will be
	   modified */
	page_off = (unsigned) (file_off & (PAGE_CACHE_SIZE - 1));

	do {
		/* number of bytes to be written to page */
		to_page = PAGE_CACHE_SIZE - page_off;
		if (to_page > f->length)
			to_page = f->length;

		page = grab_cache_page(inode->i_mapping, (unsigned long) (file_off >> PAGE_CACHE_SHIFT));
		if (!page) {
			result = -ENOMEM;
			goto exit1;
		}

		j = jnode_of_page(page);
		if (IS_ERR(j)) {
			result = PTR_ERR(j);
			goto exit2;
		}

		if (!jnode_mapped(j)) {
			trace_on(TRACE_EXTENTS, "MAKE: page %p, index %lu, count %d..", page, page->index, page_count(page));
			
			/* unlock page before doing anything with filesystem tree */
			reiser4_unlock_page(page);
			/* make sure that page has non-hole extent pointing to it */
			result = make_extent(inode, coord, lh, j);
			reiser4_lock_page(page);
			if (result) {
				trace_on(TRACE_EXTENTS, "FAILED: %d\n", result);
				goto exit3;
			}
			trace_on(TRACE_EXTENTS, "OK\n");
		}

		/* if page is not completely overwritten - read it if it is not
		   new or fill by zeros otherwise */
		result = prepare_page(inode, page, file_off, page_off, to_page);
		if (result)
			goto exit3;

		schedulable();

		/* copy user data into page */
		data = kmap(page);
		result = __copy_from_user(data + page_off, f->data, to_page);
		kunmap(page);
		if (unlikely(result)) {
			result = -EFAULT;
			goto exit3;
		}
		SetPageUptodate(page);

		result = try_capture_page(page, ZNODE_WRITE_LOCK, 0);
		if (result)
			goto exit3;
		jnode_set_dirty(j);
		jput(j);

		assert("vs-1072", PageDirty(page));
		reiser4_unlock_page(page);
		page_cache_release(page);

		page_off = 0;
		file_off += to_page;
		move_flow_forward(f, to_page);

		/* throttle the writer */
		result = extent_balance_dirty_pages(page->mapping, f, coord, lh);
		if (result) {
			reiser4_stat_extent_add(bdp_caused_repeats);
			break;
		}
		continue;

exit3:
		jput(j);
exit2:
		reiser4_unlock_page(page);
		page_cache_release(page);
exit1:
		break;

		/* hint is unset by make_page_extent when first extent of a
		   file was inserted: in that case we can not use coord anymore
		   because we are to continue on twig level but are still at
		   leaf level
		*/
	} while (f->length && coord->node);

	if (f->length)
		DQUOT_FREE_SPACE_NODIRTY(inode, f->length);

	return result;
}

/* extent's write method. It can be called in tree modes:
  
   1. real write - to write data from flow to a file (@page == 0 && @f->data != 0)
  
   2. expanding truncate (@page == 0 && @f->data == 0)
*/
int
extent_write(struct inode *inode, coord_t *coord, lock_handle *lh, flow_t * f)
{
	int result;

	if (f->data)
		/* real write */
		result = extent_write_flow(inode, coord, lh, f);
	else {
		/* expanding truncate. add_hole requires f->key to be set to
		   new end of file */
		set_key_offset(&f->key, get_key_offset(&f->key) + f->length);
		f->length = 0;
		result = add_hole(coord, lh, &f->key);
	}

	return result;
}

/*
   Local variables:
   c-indentation-style: "K&R"
   mode: linux-c
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   scroll-step: 1
   End:
*/
