/* Copyright 2001, 2002 by Hans Reiser, licensing governed by reiser4/README */

/* 
   Tree Access Pointers (taps).

   tap is data structure combining coord and lock handle (mostly). It is
   useful when one has to scan tree nodes (for example, in readdir, or flush),
   for tap functions allow to move tap in either direction transparently
   crossing unit/item/node borders.

   Tap doesn't provide automatic synchronization of its fields as it is
   supposed to be per-thread object.
*/

#include "forward.h"
#include "debug.h"
#include "coord.h"
#include "tree.h"
#include "context.h"
#include "tap.h"
#include "znode.h"
#include "tree_walk.h"

#if REISER4_DEBUG
static int tap_invariant(const tap_t * tap);
static void tap_check(const tap_t * tap);
#else
#define tap_check(tap) noop
#endif

/** load node tap is pointing to, if not loaded already */
int
tap_load(tap_t * tap)
{
	tap_check(tap);
	if (tap->loaded == 0) {
		int result;

		result = zload_ra(tap->lh->node, &tap->ra_info);
		if (result != 0)
			return result;
	}
	++tap->loaded;
	tap_check(tap);
	return 0;
}

/** release node tap is pointing to. Dual to tap_load() */
void
tap_relse(tap_t * tap)
{
	tap_check(tap);
	--tap->loaded;
	if (tap->loaded == 0)
		zrelse(tap->lh->node);
	tap_check(tap);
}

/** 
 * init tap to consist of @coord and @lh. Locks on nodes will be acquired with
 * @mode 
 */
void
tap_init(tap_t * tap, coord_t * coord, lock_handle * lh, znode_lock_mode mode)
{
	tap->coord = coord;
	tap->lh = lh;
	tap->mode = mode;
	tap->loaded = 0;
	tap_list_clean(tap);
}

/** add @tap to the per-thread list of all taps */
void
tap_monitor(tap_t * tap)
{
	assert("nikita-2623", tap != NULL);
	tap_check(tap);
	tap_list_push_front(taps_list(), tap);
	tap_check(tap);
}

/** finish with @tap */
void
tap_done(tap_t * tap)
{
	assert("nikita-2565", tap != NULL);
	assert("nikita-2566", tap->coord->node == tap->lh->node);
	tap_check(tap);
	done_lh(tap->lh);
	if (tap->loaded > 0)
		zrelse(tap->lh->node);
	tap->loaded = 0;
	tap_list_remove_clean(tap);
	tap->coord->node = NULL;
}

/** 
 * move @tap to the new node, locked with @target. Load @target, if @tap was
 * already loaded. 
 */
int
tap_move(tap_t * tap, lock_handle * target)
{
	int result = 0;

	assert("nikita-2567", tap != NULL);
	assert("nikita-2568", target != NULL);
	assert("nikita-2570", target->node != NULL);
	assert("nikita-2569", tap->coord->node == tap->lh->node);

	tap_check(tap);
	if (tap->loaded > 0)
		result = zload_ra(target->node, &tap->ra_info);

	if (result == 0) {
		if (tap->loaded > 0)
			zrelse(tap->coord->node);
		done_lh(tap->lh);
		copy_lh(tap->lh, target);
		tap->coord->node = target->node;
	}
	tap_check(tap);
	return result;
}

/** 
 * move @tap to @target. Acquire lock on @target, if @tap was already
 * loaded. 
 */
int
tap_to(tap_t * tap, znode * target)
{
	int result;

	assert("nikita-2624", tap != NULL);
	assert("nikita-2625", target != NULL);

	tap_check(tap);
	result = 0;
	if (tap->coord->node != target) {
		lock_handle here;

		init_lh(&here);
		result = longterm_lock_znode(&here, target, tap->mode, ZNODE_LOCK_HIPRI);
		if (result == 0) {
			result = tap_move(tap, &here);
			done_lh(&here);
		}
	}
	tap_check(tap);
	return result;
}

/** 
 * move @tap to given @target, loading and locking @target->node if
 * necessary 
 */
int
tap_to_coord(tap_t * tap, coord_t * target)
{
	int result;

	tap_check(tap);
	result = tap_to(tap, target->node);
	if (result == 0)
		coord_dup(tap->coord, target);
	tap_check(tap);
	return result;
}

/** return list of all taps */
tap_list_head *
taps_list()
{
	return &get_current_context()->taps;
}

/** helper function for go_{next,prev}_{item,unit,node}() */
int
go_dir_el(tap_t * tap, sideof dir, int units_p)
{
	coord_t dup;
	coord_t *coord;
	int result;

	int (*coord_dir) (coord_t *);
	int (*get_dir_neighbor) (lock_handle *, znode *, int, int);
	void (*coord_init) (coord_t *, const znode *);
	ON_DEBUG(int (*coord_check) (const coord_t *));

	assert("nikita-2556", tap != NULL);
	assert("nikita-2557", tap->coord != NULL);
	assert("nikita-2558", tap->lh != NULL);
	assert("nikita-2559", tap->coord->node != NULL);

	tap_check(tap);
	if (dir == LEFT_SIDE) {
		coord_dir = units_p ? coord_prev_unit : coord_prev_item;
		get_dir_neighbor = reiser4_get_left_neighbor;
		coord_init = coord_init_last_unit;
	} else {
		coord_dir = units_p ? coord_next_unit : coord_next_item;
		get_dir_neighbor = reiser4_get_right_neighbor;
		coord_init = coord_init_first_unit;
	}
	ON_DEBUG(coord_check = units_p ? coord_is_existing_unit : coord_is_existing_item);
	assert("nikita-2560", coord_check(tap->coord));

	coord = tap->coord;
	coord_dup(&dup, coord);
	if (coord_dir(&dup) != 0) {
		do {
			/* move to the left neighboring node */
			lock_handle dup;

			init_lh(&dup);
			result = get_dir_neighbor(&dup, coord->node, (int) tap->mode, GN_DO_READ);
			if (result == 0) {
				result = tap_move(tap, &dup);
				if (result == 0)
					coord_init(tap->coord, dup.node);
				done_lh(&dup);
			}
			/* skip empty nodes */
		} while ((result == 0) && node_is_empty(coord->node));
	} else {
		result = 0;
		coord_dup(coord, &dup);
	}
	assert("nikita-2564", ergo(!result, coord_check(tap->coord)));
	tap_check(tap);
	return result;
}

/** 
 * move @tap to the next unit, transparently crossing item and node
 * boundaries 
 */
int
go_next_unit(tap_t * tap)
{
	return go_dir_el(tap, RIGHT_SIDE, 1);
}

/** 
 * move @tap to the previous unit, transparently crossing item and node
 * boundaries
 */
int
go_prev_unit(tap_t * tap)
{
	return go_dir_el(tap, LEFT_SIDE, 1);
}

/** 
 * @shift times apply @actor to the @tap. This is used to move @tap to the
 * @shift units (or items, or nodes) in either direction.
 */
int
rewind_to(tap_t * tap, go_actor_t actor, int shift)
{
	int result;

	assert("nikita-2555", shift >= 0);
	assert("nikita-2562", tap->coord->node == tap->lh->node);

	tap_check(tap);
	result = tap_load(tap);
	if (result != 0)
		return result;

	for (; shift > 0; --shift) {
		result = actor(tap);
		assert("nikita-2563", tap->coord->node == tap->lh->node);
		if (result != 0)
			break;
	}
	tap_relse(tap);
	tap_check(tap);
	return result;
}

/** move @tap @shift units rightward */
int
rewind_right(tap_t * tap, int shift)
{
	return rewind_to(tap, go_next_unit, shift);
}

/** move @tap @shift units leftward */
int
rewind_left(tap_t * tap, int shift)
{
	return rewind_to(tap, go_prev_unit, shift);
}

#if REISER4_DEBUG_OUTPUT
/** debugging function: print @tap content in human readable form */
void print_tap(const char * prefix, const tap_t * tap)
{
	if (tap == NULL) {
		info("%s: null tap\n", prefix);
		return;
	}
	info("%s: loaded: %i, in-list: %i, node: %p, mode: %s\n", prefix,
	     tap->loaded, tap_list_is_clean(tap), tap->lh->node,
	     lock_mode_name(tap->mode));
	print_coord("\tcoord", tap->coord, 0);
}
#else
#define print_tap(prefix, tap) noop
#endif

#if REISER4_DEBUG
/** check [tap-sane] invariant */
static int tap_invariant(const tap_t * tap)
{
	/* [tap-sane] invariant */

	if (tap == NULL)
		return 1;
	/* tap->mode is one of 
	 * 
	 * {ZNODE_NO_LOCK, ZNODE_READ_LOCK, ZNODE_WRITE_LOCK}, and
	 */
	if (tap->mode != ZNODE_NO_LOCK && 
	    tap->mode != ZNODE_READ_LOCK && tap->mode != ZNODE_WRITE_LOCK)
		return 2;
	/* tap->coord != NULL, and */
	if (tap->coord == NULL)
		return 3;
	/* tap->lh != NULL, and */
	if (tap->lh == NULL)
		return 4;
	/* tap->loaded > 0 => znode_is_loaded(tap->coord->node), and */
	if (!ergo(tap->loaded, znode_is_loaded(tap->coord->node)))
		return 5;
	/* tap->coord->node == tap->lh->node */
	if (tap->coord->node != tap->lh->node)
		return 6;
	return 0;
}

/** debugging function: check internal @tap consistency */
static void tap_check(const tap_t * tap)
{
	int result;

	result = tap_invariant(tap);
	if (result != 0) {
		print_tap("broken", tap);
		reiser4_panic("nikita-2831", "tap broken: %i\n", result);
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
