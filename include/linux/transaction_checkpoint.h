/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TRANSACTION_CHECKPOINT_H
#define _LINUX_TRANSACTION_CHECKPOINT_H

#include <linux/list.h>
#include <linux/types.h>

#include <asm/ptrace.h>

struct mm_struct;
struct page;
struct task_struct;

// One private page copy retained until commit or rollback.
struct undo_log_rec {
	struct page *stable;
	struct page *checkpoint;
	unsigned long addr;
	struct list_head list;
};

// Per-task state retained across an aborted transaction attempt.
struct transaction_checkpoint {
	struct pt_regs regs_checkpoint;
	struct list_head undo_log;
	struct mm_struct *mm;
	bool valid;
	bool need_autoretry;
	bool mm_prepared;
};

int transaction_checkpoint_alloc(struct task_struct *task);
int transaction_checkpoint_capture(struct task_struct *task, struct pt_regs *regs);
// On success, the undo log holds one reference to each page.
int transaction_checkpoint_log_page(struct task_struct *task, struct page *stable,
				    struct page *checkpoint, unsigned long addr);
void transaction_checkpoint_clear_undo(struct task_struct *task);
int transaction_checkpoint_prepare_mm(struct task_struct *task);
void transaction_checkpoint_unprotect_mm(struct transaction_checkpoint *checkpoint);
void transaction_checkpoint_discard(struct task_struct *task);
void transaction_checkpoint_free(struct task_struct *task);

#endif /* _LINUX_TRANSACTION_CHECKPOINT_H */
