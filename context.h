/* Copyright 2001, 2002 by Hans Reiser, licensing governed by reiser4/README */

/* Reiser4 context */

#if !defined( __REISER4_CONTEXT_H__ )
#define __REISER4_CONTEXT_H__

#include "forward.h"
#include "debug.h"
#include "spin_macros.h"
#include "dformat.h"
#include "tslist.h"
#include "plugin/node/node.h"
#include "plugin/plugin.h"
#include "jnode.h"
#include "znode.h"
#include "tap.h"

#include <linux/types.h>	/* for __u??  */
#include <linux/fs.h>		/* for struct super_block  */
#include <linux/spinlock.h>
#include <linux/sched.h>	/* for struct task_struct */

/* list of active lock stacks */
ON_DEBUG(TS_LIST_DECLARE(context);)

/* global context used during system call. Variable of this type is
   allocated on the stack at the beginning of the reiser4 part of the
   system call and pointer to it is stored in the
   current->fs_context. This allows us to avoid passing pointer to
   current transaction and current lockstack (both in one-to-one mapping
   with threads) all over the call chain.

   It's kind of like those global variables the prof used to tell you
   not to use in CS1, except thread specific.;-) Nikita, this was a
   good idea.
*/
struct reiser4_context {
	/* magic constant. For debugging */
	__u32 magic;

	/* current lock stack. See lock.[ch]. This is where list of all
	   locks taken by current thread is kept. This is also used in
	   deadlock detection. */
	lock_stack stack;

	/* current transcrash. */
	txn_handle *trans;
	txn_handle trans_in_ctx;

	/* super block we are working with.  To get the current tree
	   use &get_super_private (reiser4_get_current_sb ())->tree. */
	struct super_block *super;

	/* parent fs activation */
	struct fs_activation *outer;

	/* per-thread grabbed (for further allocation) blocks counter */
	reiser4_block_nr grabbed_blocks;

	/* per-thread tracing flags. Use reiser4_trace_flags enum to set
	   bits in it. */
	__u32 trace_flags;

	/* parent context */
	reiser4_context *parent;
	tap_list_head taps;

	/* grabbing space is enabled */
	int grab_enabled:1;
    	/* should be set when we are write dirty nodes to disk in jnode_flush or
	 * reiser4_write_logs() */
	int writeout_mode:1;
	    
#if REISER4_DEBUG
	/* thread ID */
	__u32 tid;

	/* A link of all active contexts. */
	context_list_link contexts_link;
	lock_counters_info locks;
	int nr_children;	/* number of child contexts */
	struct task_struct *task;	/* so we can easily find owner of the stack */
#endif
#if REISER4_DEBUG_NODE
	int disable_node_check;
#endif
	/* count non-trivial jnode_set_dirty() calls */
	__u64 nr_marked_dirty;
};

#if REISER4_DEBUG
TS_LIST_DEFINE(context, reiser4_context, contexts_link);
#endif

extern reiser4_context *get_context_by_lock_stack(lock_stack *);

/* Debugging helps. */
extern int init_context_mgr(void);
#if REISER4_DEBUG_OUTPUT
extern void print_context(const char *prefix, reiser4_context * ctx);
#else
#define print_context(p,c) noop
#endif

#if REISER4_DEBUG_OUTPUT && REISER4_DEBUG
extern void print_contexts(void);
#else
#define print_contexts() noop
#endif

/* Hans, is this too expensive? */
#define current_tree (&(get_super_private(reiser4_get_current_sb())->tree))
#define current_blocksize reiser4_get_current_sb()->s_blocksize
#define current_blocksize_bits reiser4_get_current_sb()->s_blocksize_bits

extern int init_context(reiser4_context * context, struct super_block *super);
extern void done_context(reiser4_context * context);

/* magic constant we store in reiser4_context allocated at the stack. Used to
   catch accesses to staled or uninitialized contexts. */
#define context_magic ((__u32) 0x4b1b5d0b)

static inline int
is_in_reiser4_context(void)
{
	return current->fs_context != NULL && ((__u32) current->fs_context->owner) == context_magic;
}

/* return context associated with given thread */
static inline reiser4_context *
get_context(const struct task_struct *tsk)
{
	if (tsk == NULL) {
		BUG();
	}

	return (reiser4_context *) tsk->fs_context;
}

/* return context associated with current thread */
static inline reiser4_context *
get_current_context(void)
{
	reiser4_context *context;

	context = get_context(current);
	if (context != NULL)
		return context->parent;
	else
		return NULL;
}

static inline int is_writeout_mode(void)
{
	return get_current_context()->writeout_mode;
}

static inline void writeout_mode_enable(void)
{
	get_current_context()->writeout_mode = 1;
}

static inline void writeout_mode_disable(void)
{
	get_current_context()->writeout_mode = 0;
}

static inline void grab_space_enable(void) 
{
	get_current_context()->grab_enabled = 1;
}

static inline void grab_space_disable(void) 
{
	get_current_context()->grab_enabled = 0;
}

static inline int is_grab_enabled(void)
{
	return get_current_context()->grab_enabled;
}

#define REISER4_TRACE_CONTEXT (0)

#if REISER4_TRACE_TREE && REISER4_TRACE_CONTEXT
extern int write_in_trace(const char *func, const char *mes);

#define log_entry(super, str)						\
({									\
	if (super != NULL && get_super_private(super) != NULL &&	\
	    get_super_private(super)->trace_file.buf != NULL)		\
		write_in_trace(__FUNCTION__, str);			\
})

#else
#define log_entry(super, str) noop
#endif

#define __REISER4_ENTRY(super, errret)				\
	reiser4_context __context;				\
	do {							\
                int __ret;					\
                __ret = init_context(&__context, (super));	\
		log_entry(super, ":in");			\
                if (__ret != 0) {				\
			return errret;				\
		}						\
        } while (0)

#define REISER4_ENTRY_PTR(super)  __REISER4_ENTRY(super, ERR_PTR(__ret))
#define REISER4_ENTRY(super)      __REISER4_ENTRY(super, __ret)

extern int reiser4_exit_context(reiser4_context * context);

#define REISER4_EXIT( ret_exp )				\
({							\
	typeof ( ret_exp ) __result = ( ret_exp );	\
        int __ret = reiser4_exit_context( &__context );	\
	return __result ? : __ret;			\
})

#define REISER4_EXIT_PTR( ret_exp )				\
({								\
	typeof ( ret_exp ) __result = ( ret_exp );		\
        int __ret = reiser4_exit_context( &__context );		\
	return IS_ERR (__result) ? __result : ERR_PTR (__ret);	\
})


/* __REISER4_CONTEXT_H__ */
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
