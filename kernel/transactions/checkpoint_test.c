// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/transaction.h>
#include <linux/transaction_checkpoint.h>

#include <asm/segment.h>

static struct task_struct *transaction_checkpoint_test_task(struct kunit *test) {
	struct task_struct *task;

	task = kunit_kzalloc(test, sizeof(*task), GFP_KERNEL);
	if (!task)
		return NULL;
	transaction_task_init(task);
	spin_lock_init(&task->alloc_lock);
	return task;
}

static void transaction_checkpoint_test_put_page(void *data) {
	put_page(data);
}

static void transaction_checkpoint_task_init_test(struct kunit *test) {
	struct task_struct *task = transaction_checkpoint_test_task(test);

	KUNIT_ASSERT_NOT_NULL(test, task);
	KUNIT_EXPECT_PTR_EQ(test, READ_ONCE(task->transaction), NULL);
	KUNIT_EXPECT_PTR_EQ(test, READ_ONCE(task->transaction_checkpoint), NULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&task->transaction_entry));
}

static void transaction_checkpoint_attach_lifecycle_test(struct kunit *test) {
	struct transaction_checkpoint *checkpoint;
	struct transaction *transaction;
	struct task_struct *task;

	task = transaction_checkpoint_test_task(test);
	KUNIT_ASSERT_NOT_NULL(test, task);
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, task), 0);

	checkpoint = READ_ONCE(task->transaction_checkpoint);
	KUNIT_ASSERT_NOT_NULL(test, checkpoint);
	KUNIT_EXPECT_FALSE(test, checkpoint->valid);
	KUNIT_EXPECT_FALSE(test, checkpoint->need_autoretry);
	KUNIT_EXPECT_FALSE(test, checkpoint->mm_prepared);
	KUNIT_EXPECT_PTR_EQ(test, checkpoint->mm, NULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&checkpoint->undo_log));

	transaction_detach_task(task);
	KUNIT_EXPECT_PTR_EQ(test, READ_ONCE(task->transaction_checkpoint), NULL);
	transaction_put(transaction);
}

static void transaction_checkpoint_capture_test(struct kunit *test) {
	struct transaction_checkpoint *checkpoint;
	struct transaction *transaction;
	struct task_struct *task;
	struct mm_struct *mm;
	struct pt_regs regs = { };

	task = transaction_checkpoint_test_task(test);
	KUNIT_ASSERT_NOT_NULL(test, task);
	mm = kunit_kzalloc(test, sizeof(*mm), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, mm);
	atomic_set(&mm->mm_users, 1);
	task->mm = mm;
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, task), 0);

	KUNIT_EXPECT_EQ(test, transaction_checkpoint_capture(task, &regs), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, atomic_read(&mm->mm_users), 1);
	regs.cs = __USER_CS;
	regs.ip = 0x1234;
	regs.sp = 0x5678;
	regs.ax = 0x9abc;
	KUNIT_ASSERT_EQ(test, transaction_checkpoint_capture(task, &regs), 0);
	checkpoint = READ_ONCE(task->transaction_checkpoint);
	KUNIT_ASSERT_NOT_NULL(test, checkpoint);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(checkpoint->valid));
	KUNIT_EXPECT_PTR_EQ(test, READ_ONCE(checkpoint->mm), mm);
	KUNIT_EXPECT_EQ(test, checkpoint->regs_checkpoint.ip, regs.ip);
	KUNIT_EXPECT_EQ(test, checkpoint->regs_checkpoint.sp, regs.sp);
	KUNIT_EXPECT_EQ(test, checkpoint->regs_checkpoint.ax, regs.ax);
	KUNIT_EXPECT_EQ(test, atomic_read(&mm->mm_users), 2);
	KUNIT_EXPECT_EQ(test, transaction_checkpoint_capture(task, &regs), -EALREADY);

	transaction_checkpoint_discard(task);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(checkpoint->valid));
	KUNIT_EXPECT_FALSE(test, READ_ONCE(checkpoint->need_autoretry));
	KUNIT_EXPECT_PTR_EQ(test, READ_ONCE(checkpoint->mm), NULL);
	KUNIT_EXPECT_EQ(test, atomic_read(&mm->mm_users), 1);
	transaction_detach_task(task);
	transaction_put(transaction);
}

static void transaction_checkpoint_free_discards_capture_test(struct kunit *test) {
	struct transaction *transaction;
	struct task_struct *task;
	struct page *page_checkpoint;
	struct page *stable;
	struct mm_struct *mm;
	struct pt_regs regs = { };

	task = transaction_checkpoint_test_task(test);
	KUNIT_ASSERT_NOT_NULL(test, task);
	mm = kunit_kzalloc(test, sizeof(*mm), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, mm);
	atomic_set(&mm->mm_users, 1);
	task->mm = mm;
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, task), 0);

	regs.cs = __USER_CS;
	KUNIT_ASSERT_EQ(test, transaction_checkpoint_capture(task, &regs), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&mm->mm_users), 2);
	stable = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stable);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, transaction_checkpoint_test_put_page,
							stable), 0);
	page_checkpoint = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page_checkpoint);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, transaction_checkpoint_test_put_page,
							page_checkpoint), 0);
	KUNIT_ASSERT_EQ(test, transaction_checkpoint_log_page(task, stable, page_checkpoint, 0), 0);
	KUNIT_EXPECT_EQ(test, page_count(stable), 2);
	KUNIT_EXPECT_EQ(test, page_count(page_checkpoint), 2);
	transaction_detach_task(task);
	KUNIT_EXPECT_PTR_EQ(test, READ_ONCE(task->transaction_checkpoint), NULL);
	KUNIT_EXPECT_EQ(test, atomic_read(&mm->mm_users), 1);
	KUNIT_EXPECT_EQ(test, page_count(stable), 1);
	KUNIT_EXPECT_EQ(test, page_count(page_checkpoint), 1);
	transaction_put(transaction);
}

static void transaction_checkpoint_undo_log_test(struct kunit *test) {
	struct transaction_checkpoint *checkpoint;
	struct transaction *transaction;
	struct undo_log_rec *undo;
	struct task_struct *task;
	struct page *page_checkpoint;
	struct page *stable;
	struct mm_struct *mm;
	struct pt_regs regs = { };
	int ret;

	task = transaction_checkpoint_test_task(test);
	KUNIT_ASSERT_NOT_NULL(test, task);
	mm = kunit_kzalloc(test, sizeof(*mm), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, mm);
	atomic_set(&mm->mm_users, 1);
	task->mm = mm;
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, task), 0);
	regs.cs = __USER_CS;
	KUNIT_ASSERT_EQ(test, transaction_checkpoint_capture(task, &regs), 0);

	stable = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stable);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, transaction_checkpoint_test_put_page,
							stable), 0);
	page_checkpoint = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page_checkpoint);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, transaction_checkpoint_test_put_page,
							page_checkpoint), 0);

	ret = transaction_checkpoint_log_page(task, stable, stable, 0);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = transaction_checkpoint_log_page(task, stable, page_checkpoint, 1);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = transaction_checkpoint_log_page(task, stable, page_checkpoint, PAGE_SIZE);
	KUNIT_ASSERT_EQ(test, ret, 0);
	checkpoint = READ_ONCE(task->transaction_checkpoint);
	KUNIT_ASSERT_NOT_NULL(test, checkpoint);
	KUNIT_ASSERT_FALSE(test, list_empty(&checkpoint->undo_log));
	undo = list_first_entry(&checkpoint->undo_log, struct undo_log_rec, list);
	KUNIT_EXPECT_PTR_EQ(test, undo->stable, stable);
	KUNIT_EXPECT_PTR_EQ(test, undo->checkpoint, page_checkpoint);
	KUNIT_EXPECT_EQ(test, undo->addr, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, page_count(stable), 2);
	KUNIT_EXPECT_EQ(test, page_count(page_checkpoint), 2);
	ret = transaction_checkpoint_log_page(task, stable, page_checkpoint, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, ret, -EEXIST);
	KUNIT_EXPECT_EQ(test, page_count(stable), 2);
	KUNIT_EXPECT_EQ(test, page_count(page_checkpoint), 2);

	transaction_checkpoint_clear_undo(task);
	KUNIT_EXPECT_TRUE(test, list_empty(&checkpoint->undo_log));
	KUNIT_EXPECT_EQ(test, page_count(stable), 1);
	KUNIT_EXPECT_EQ(test, page_count(page_checkpoint), 1);
	transaction_detach_task(task);
	transaction_put(transaction);
}

static void transaction_checkpoint_prepare_mm_test(struct kunit *test) {
	struct transaction_checkpoint *checkpoint;
	struct transaction *transaction;
	struct task_struct *task;
	struct mm_struct *mm;
	struct pt_regs regs = { };

	task = transaction_checkpoint_test_task(test);
	KUNIT_ASSERT_NOT_NULL(test, task);
	mm = mm_alloc();
	KUNIT_ASSERT_NOT_NULL(test, mm);
	task->mm = mm;
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, task), 0);
	regs.cs = __USER_CS;
	KUNIT_ASSERT_EQ(test, transaction_checkpoint_capture(task, &regs), 0);
	checkpoint = READ_ONCE(task->transaction_checkpoint);
	KUNIT_ASSERT_NOT_NULL(test, checkpoint);

	KUNIT_EXPECT_FALSE(test, READ_ONCE(checkpoint->mm_prepared));
	mmget(mm);
	KUNIT_EXPECT_EQ(test, transaction_checkpoint_prepare_mm(task), -EOPNOTSUPP);
	mmput(mm);
	KUNIT_ASSERT_EQ(test, transaction_checkpoint_prepare_mm(task), 0);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(checkpoint->mm_prepared));
	KUNIT_EXPECT_EQ(test, transaction_checkpoint_prepare_mm(task), -EALREADY);
	transaction_checkpoint_discard(task);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(checkpoint->mm_prepared));
	KUNIT_EXPECT_FALSE(test, READ_ONCE(checkpoint->valid));
	KUNIT_EXPECT_PTR_EQ(test, READ_ONCE(checkpoint->mm), NULL);

	transaction_detach_task(task);
	task->mm = NULL;
	mmput(mm);
	transaction_put(transaction);
}

static void transaction_checkpoint_rejected_attach_cleanup_test(struct kunit *test) {
	struct transaction *transaction;
	struct task_struct *first_task;
	struct task_struct *second_task;

	first_task = transaction_checkpoint_test_task(test);
	KUNIT_ASSERT_NOT_NULL(test, first_task);
	second_task = transaction_checkpoint_test_task(test);
	KUNIT_ASSERT_NOT_NULL(test, second_task);
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, first_task), 0);
	KUNIT_EXPECT_EQ(test, transaction_attach_task(transaction, second_task), -EOPNOTSUPP);
	KUNIT_EXPECT_PTR_EQ(test, READ_ONCE(second_task->transaction_checkpoint), NULL);

	transaction_detach_task(first_task);
	transaction_put(transaction);
}

static void transaction_checkpoint_task_exit_cleanup_test(struct kunit *test) {
	struct transaction *transaction;
	struct task_struct *task;

	task = transaction_checkpoint_test_task(test);
	KUNIT_ASSERT_NOT_NULL(test, task);
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, task), 0);

	transaction_task_exit(task);
	KUNIT_EXPECT_PTR_EQ(test, READ_ONCE(task->transaction), NULL);
	KUNIT_EXPECT_PTR_EQ(test, READ_ONCE(task->transaction_checkpoint), NULL);
	KUNIT_EXPECT_EQ(test, atomic_read(&transaction->task_count), 0);
	transaction_put(transaction);
}

static struct kunit_case transaction_checkpoint_test_cases[] = {
	KUNIT_CASE(transaction_checkpoint_task_init_test),
	KUNIT_CASE(transaction_checkpoint_attach_lifecycle_test),
	KUNIT_CASE(transaction_checkpoint_capture_test),
	KUNIT_CASE(transaction_checkpoint_free_discards_capture_test),
	KUNIT_CASE(transaction_checkpoint_undo_log_test),
	KUNIT_CASE(transaction_checkpoint_prepare_mm_test),
	KUNIT_CASE(transaction_checkpoint_rejected_attach_cleanup_test),
	KUNIT_CASE(transaction_checkpoint_task_exit_cleanup_test),
	{ }
};

static struct kunit_suite transaction_checkpoint_test_suite = {
	.name = "transaction_checkpoint",
	.test_cases = transaction_checkpoint_test_cases,
};

kunit_test_suite(transaction_checkpoint_test_suite);

MODULE_LICENSE("GPL");
