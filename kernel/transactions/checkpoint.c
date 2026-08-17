// SPDX-License-Identifier: GPL-2.0
// Per-task transaction checkpoint lifecycle.

#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/transaction_checkpoint.h>

int transaction_checkpoint_alloc(struct task_struct *task) {
	struct transaction_checkpoint *checkpoint;

	if (!task)
		return -EINVAL;

	checkpoint = kzalloc(sizeof(*checkpoint), GFP_KERNEL);
	if (!checkpoint)
		return -ENOMEM;

	INIT_LIST_HEAD(&checkpoint->undo_log);
	if (cmpxchg(&task->transaction_checkpoint, NULL, checkpoint)) {
		kfree(checkpoint);
		return -EALREADY;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(transaction_checkpoint_alloc);

void transaction_checkpoint_free(struct task_struct *task) {
	struct transaction_checkpoint *checkpoint;

	if (!task)
		return;

	checkpoint = xchg(&task->transaction_checkpoint, NULL);
	if (!checkpoint)
		return;

	WARN_ON_ONCE(!list_empty(&checkpoint->undo_log));
	kfree(checkpoint);
}
EXPORT_SYMBOL_GPL(transaction_checkpoint_free);
