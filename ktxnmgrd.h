/* Copyright 2001, 2002 by Hans Reiser, licensing governed by reiser4/README */

/* Transaction manager daemon. */

#ifndef __KTXNMGRD_H__
#define __KTXNMGRD_H__

#include "kcond.h"
#include "txnmgr.h"
#include "spin_macros.h"

#include <linux/fs.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <asm/atomic.h>
#include <linux/sched.h>	/* for struct task_struct */

struct ktxnmgrd_context {
	kcond_t startup;
	struct completion finish;
	kcond_t wait;
	spinlock_t guard;
	signed long timeout;
	struct task_struct *tsk;
	txn_mgrs_list_head queue;
	int started:1;
	int done:1;
	int rescan:1;
	__u32 duties;
	atomic_t pressure;
};

#define spin_ordering_pred_ktxnmgrd(ctx) (1)

SPIN_LOCK_FUNCTIONS(ktxnmgrd, struct ktxnmgrd_context, guard);

typedef enum {
	CANNOT_COMMIT,
	MEMORY_PRESSURE,
	LOW_MEMORY
} ktxnmgrd_wake;

extern void init_ktxnmgrd_context(ktxnmgrd_context * context);
extern int ktxnmgrd(void *context);

extern int ktxnmgrd_attach(ktxnmgrd_context * ctx, txn_mgr * mgr);
extern void ktxnmgrd_detach(txn_mgr * mgr);

extern void ktxnmgrd_kick(ktxnmgrd_context * ctx, ktxnmgrd_wake reason);
extern int ktxnmgr_writeback(struct super_block *s, struct writeback_control *);

/* __KTXNMGRD_H__ */
#endif

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
