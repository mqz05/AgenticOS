// SPDX-License-Identifier: GPL-2.0
// Transaction checkpoint page-table preparation.

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/transaction.h>
#include <linux/transaction_checkpoint.h>
#include <linux/userfaultfd_k.h>

#include <asm/tlb.h>

// Select writable private mappings whose writes must be checkpointed.
static bool transaction_checkpoint_vma_eligible(struct vm_area_struct *vma) {
	vm_flags_t flags = READ_ONCE(vma->vm_flags);

	return flags & VM_WRITE && is_cow_mapping(flags);
}

// Reject mappings that the current page checkpoint path cannot restore.
static int transaction_checkpoint_validate_vmas(struct mm_struct *mm) {
	struct vm_area_struct *vma;

	VMA_ITERATOR(vmi, mm, 0);

	for_each_vma(vmi, vma) {
		if (!transaction_checkpoint_vma_eligible(vma))
			continue;
		// First-write checkpointing currently requires normal COW pages.
		if (vma->vm_flags & (VM_HUGETLB | VM_PFNMAP | VM_MIXEDMAP | VM_IO))
			return -EOPNOTSUPP;
		if (userfaultfd_armed(vma))
			return -EOPNOTSUPP;
	}

	return 0;
}

// Apply one protection mode to every eligible mapping.
static int checkpoint_change_protection(struct mm_struct *mm, unsigned long cp_flags) {
	struct vm_area_struct *vma;
	struct mmu_gather tlb;
	long changed;
	int ret = 0;

	VMA_ITERATOR(vmi, mm, 0);

	tlb_gather_mmu(&tlb, mm);
	for_each_vma(vmi, vma) {
		if (!transaction_checkpoint_vma_eligible(vma))
			continue;
		changed = change_protection(&tlb, vma, vma->vm_start, vma->vm_end, cp_flags);
		if (changed < 0) {
			ret = changed;
			break;
		}
	}
	tlb_finish_mmu(&tlb);
	return ret;
}

// Write-protect private writable mappings so their first write faults.
// xbegin will call this once first-write snapshot handling is available.
int transaction_checkpoint_prepare_mm(struct task_struct *task) {
	struct transaction_checkpoint *checkpoint;
	struct transaction *transaction;
	struct mm_struct *mm;
	int ret;

	if (!task)
		return -EINVAL;
	transaction = READ_ONCE(task->transaction);
	if (!transaction || transaction_status(transaction) != TRANSACTION_ACTIVE)
		return -ECANCELED;
	checkpoint = READ_ONCE(task->transaction_checkpoint);
	if (!checkpoint || !READ_ONCE(checkpoint->valid))
		return -EINVAL;
	if (READ_ONCE(checkpoint->mm_prepared))
		return -EALREADY;

	mm = READ_ONCE(checkpoint->mm);
	if (!mm || mm != READ_ONCE(task->mm))
		return -ESTALE;
	// Only the attached task and its checkpoint may hold mm_users references.
	if (atomic_read(&mm->mm_users) != 2)
		return -EOPNOTSUPP;

	ret = mmap_write_lock_killable(mm);
	if (ret)
		return ret;
	if (transaction_status(transaction) != TRANSACTION_ACTIVE) {
		ret = -ECANCELED;
		goto out;
	}
	ret = transaction_checkpoint_validate_vmas(mm);
	if (ret)
		goto out;

	// Serialize the write-protection pass against fast page pinning.
	raw_write_seqcount_begin(&mm->write_protect_seq);
	ret = checkpoint_change_protection(mm, 0);
	if (!ret && transaction_status(transaction) != TRANSACTION_ACTIVE)
		ret = -ECANCELED;
	if (ret)
		checkpoint_change_protection(mm, MM_CP_TRY_CHANGE_WRITABLE);
	raw_write_seqcount_end(&mm->write_protect_seq);
	if (!ret)
		WRITE_ONCE(checkpoint->mm_prepared, true);
out:
	mmap_write_unlock(mm);
	return ret;
}
EXPORT_SYMBOL_GPL(transaction_checkpoint_prepare_mm);

// Restore ordinary writable permissions after checkpoint processing.
void transaction_checkpoint_unprotect_mm(struct transaction_checkpoint *checkpoint) {
	struct mm_struct *mm;

	if (!checkpoint || !READ_ONCE(checkpoint->mm_prepared))
		return;
	mm = READ_ONCE(checkpoint->mm);
	if (!mm)
		return;

	// Restore only PTEs that the MM can safely make writable.
	mmap_write_lock(mm);
	checkpoint_change_protection(mm, MM_CP_TRY_CHANGE_WRITABLE);
	WRITE_ONCE(checkpoint->mm_prepared, false);
	mmap_write_unlock(mm);
}
EXPORT_SYMBOL_GPL(transaction_checkpoint_unprotect_mm);
