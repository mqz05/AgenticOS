// SPDX-License-Identifier: GPL-2.0
// Per-task transaction checkpoint lifecycle.

#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
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

int transaction_checkpoint_capture(struct task_struct *task, struct pt_regs *regs) {
	struct transaction_checkpoint *checkpoint;
	struct mm_struct *mm;

	if (!task || !regs)
		return -EINVAL;
	if (!user_mode(regs) || !user_64bit_mode(regs))
		return -EOPNOTSUPP;

	checkpoint = READ_ONCE(task->transaction_checkpoint);
	if (!checkpoint)
		return -EINVAL;
	if (!READ_ONCE(task->transaction))
		return -EINVAL;
	if (READ_ONCE(checkpoint->valid))
		return -EALREADY;
	if (READ_ONCE(checkpoint->mm) || !list_empty(&checkpoint->undo_log))
		return -EBUSY;

	mm = get_task_mm(task);
	if (!mm)
		return -ESRCH;

	checkpoint->regs_checkpoint = *regs;
	WRITE_ONCE(checkpoint->mm, mm);
	WRITE_ONCE(checkpoint->need_autoretry, false);
	WRITE_ONCE(checkpoint->valid, true);
	return 0;
}
EXPORT_SYMBOL_GPL(transaction_checkpoint_capture);

static void transaction_checkpoint_discard_state(struct transaction_checkpoint *checkpoint) {
	struct mm_struct *mm;

	if (!checkpoint)
		return;

	WRITE_ONCE(checkpoint->valid, false);
	WRITE_ONCE(checkpoint->need_autoretry, false);
	mm = xchg(&checkpoint->mm, NULL);
	if (mm)
		mmput(mm);
}

void transaction_checkpoint_discard(struct task_struct *task) {
	if (!task)
		return;

	transaction_checkpoint_discard_state(READ_ONCE(task->transaction_checkpoint));
}
EXPORT_SYMBOL_GPL(transaction_checkpoint_discard);

void transaction_checkpoint_free(struct task_struct *task) {
	struct transaction_checkpoint *checkpoint;

	if (!task)
		return;

	checkpoint = xchg(&task->transaction_checkpoint, NULL);
	if (!checkpoint)
		return;

	WARN_ON_ONCE(!list_empty(&checkpoint->undo_log));
	transaction_checkpoint_discard_state(checkpoint);
	kfree(checkpoint);
}
EXPORT_SYMBOL_GPL(transaction_checkpoint_free);
