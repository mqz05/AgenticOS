// SPDX-License-Identifier: GPL-2.0
// Per-task transaction checkpoint lifecycle.

#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/transaction_checkpoint.h>

static struct kmem_cache *undo_log_cachep;

static int __init transaction_checkpoint_init(void) {
	undo_log_cachep = KMEM_CACHE(undo_log_rec, SLAB_HWCACHE_ALIGN);
	return undo_log_cachep ? 0 : -ENOMEM;
}
subsys_initcall(transaction_checkpoint_init);

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

// Find an address already retained by this checkpoint.
static struct undo_log_rec *checkpoint_find_undo(struct transaction_checkpoint *checkpoint,
						 unsigned long addr) {
	struct undo_log_rec *undo;

	list_for_each_entry(undo, &checkpoint->undo_log, list) {
		if (undo->addr == addr)
			return undo;
	}

	return NULL;
}

// Retain the stable page and its private checkpoint copy.
int transaction_checkpoint_log_page(struct task_struct *task, struct page *stable,
				    struct page *page_checkpoint, unsigned long addr) {
	struct transaction_checkpoint *checkpoint;
	struct undo_log_rec *undo;

	if (!task || !stable || !page_checkpoint || stable == page_checkpoint ||
	    offset_in_page(addr))
		return -EINVAL;

	checkpoint = READ_ONCE(task->transaction_checkpoint);
	if (!checkpoint || !READ_ONCE(checkpoint->valid) || !READ_ONCE(checkpoint->mm))
		return -EINVAL;
	if (READ_ONCE(checkpoint->mm) != READ_ONCE(task->mm))
		return -ESTALE;
	if (checkpoint_find_undo(checkpoint, addr))
		return -EEXIST;

	if (unlikely(!undo_log_cachep))
		return -ENOMEM;
	undo = kmem_cache_alloc(undo_log_cachep, GFP_KERNEL);
	if (!undo)
		return -ENOMEM;

	get_page(stable);
	get_page(page_checkpoint);
	undo->stable = stable;
	undo->checkpoint = page_checkpoint;
	undo->addr = addr;
	INIT_LIST_HEAD(&undo->list);
	list_add(&undo->list, &checkpoint->undo_log);
	return 0;
}
EXPORT_SYMBOL_GPL(transaction_checkpoint_log_page);

static void transaction_checkpoint_clear_undo_state(struct transaction_checkpoint *checkpoint) {
	struct undo_log_rec *undo, *next;

	if (!checkpoint)
		return;

	list_for_each_entry_safe(undo, next, &checkpoint->undo_log, list) {
		list_del(&undo->list);
		put_page(undo->checkpoint);
		put_page(undo->stable);
		kmem_cache_free(undo_log_cachep, undo);
	}
}

void transaction_checkpoint_clear_undo(struct task_struct *task) {
	if (!task)
		return;

	transaction_checkpoint_clear_undo_state(READ_ONCE(task->transaction_checkpoint));
}
EXPORT_SYMBOL_GPL(transaction_checkpoint_clear_undo);

static void transaction_checkpoint_discard_state(struct transaction_checkpoint *checkpoint) {
	struct mm_struct *mm;

	if (!checkpoint)
		return;

	WRITE_ONCE(checkpoint->valid, false);
	WRITE_ONCE(checkpoint->need_autoretry, false);
	transaction_checkpoint_clear_undo_state(checkpoint);
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

	transaction_checkpoint_discard_state(checkpoint);
	kfree(checkpoint);
}
EXPORT_SYMBOL_GPL(transaction_checkpoint_free);
