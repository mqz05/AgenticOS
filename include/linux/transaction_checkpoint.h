/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TRANSACTION_CHECKPOINT_H
#define _LINUX_TRANSACTION_CHECKPOINT_H

#include <linux/list.h>
#include <linux/types.h>

#include <asm/ptrace.h>

struct mm_struct;
struct task_struct;

/* Per-task state retained across an aborted transaction attempt. */
struct transaction_checkpoint {
	struct pt_regs regs_checkpoint;
	struct list_head undo_log;
	struct mm_struct *mm;
	bool valid;
	bool need_autoretry;
};

int transaction_checkpoint_alloc(struct task_struct *task);
void transaction_checkpoint_free(struct task_struct *task);

#endif /* _LINUX_TRANSACTION_CHECKPOINT_H */
