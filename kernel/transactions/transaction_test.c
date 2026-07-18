// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/transaction.h>

struct transaction_race_context {
	struct transaction *transaction;
	struct completion start;
	struct completion done;
	int abort_ret;
};

static int transaction_abort_thread(void *data) {
	struct transaction_race_context *context = data;

	wait_for_completion(&context->start);
	context->abort_ret = abort_transaction(context->transaction);
	complete(&context->done);

	return 0;
}

static void transaction_initial_state_test(struct kunit *test) {
	struct transaction *transaction;

	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_EXPECT_TRUE(test, inactive_transaction(transaction));
	KUNIT_EXPECT_EQ(test, refcount_read(&transaction->ref_count), 1U);
	KUNIT_EXPECT_EQ(test, atomic_read(&transaction->task_count), 0);
	KUNIT_EXPECT_TRUE(test, list_empty(&transaction->tasks));
	KUNIT_EXPECT_TRUE(test, list_empty(&transaction->object_list));
	transaction_put(transaction);
}

static void transaction_commit_test(struct kunit *test) {
	struct transaction *transaction;

	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	KUNIT_EXPECT_TRUE(test, live_transaction(transaction));
	KUNIT_EXPECT_GT(test, transaction->timestamp, 0ULL);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), 0);
	KUNIT_EXPECT_TRUE(test, inactive_transaction(transaction));
	transaction_put(transaction);
}

static void transaction_abort_test(struct kunit *test) {
	struct transaction *transaction;

	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, abort_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, transaction_status(transaction),
			TRANSACTION_ABORTED);
	KUNIT_EXPECT_EQ(test, abort_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), -ECANCELED);
	KUNIT_EXPECT_TRUE(test, inactive_transaction(transaction));
	KUNIT_EXPECT_EQ(test, transaction->count, 1U);
	transaction_put(transaction);
}

static void transaction_invalid_transition_test(struct kunit *test) {
	struct transaction *transaction;

	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), -EINVAL);
	KUNIT_EXPECT_EQ(test, abort_transaction(transaction), -EINVAL);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, begin_transaction(transaction), -EALREADY);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), 0);
	transaction_put(transaction);
}

static void transaction_reference_test(struct kunit *test) {
	struct transaction *transaction;

	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_EXPECT_PTR_EQ(test, transaction_get(transaction), transaction);
	KUNIT_EXPECT_EQ(test, refcount_read(&transaction->ref_count), 2U);
	transaction_put(transaction);
	KUNIT_EXPECT_EQ(test, refcount_read(&transaction->ref_count), 1U);
	transaction_put(transaction);
}

static void transaction_task_attachment_test(struct kunit *test) {
	struct transaction *transaction;

	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_PTR_EQ(test, current_transaction(), NULL);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, current), 0);
	KUNIT_EXPECT_PTR_EQ(test, current_transaction(), transaction);
	KUNIT_EXPECT_EQ(test, atomic_read(&transaction->task_count), 1);
	KUNIT_EXPECT_FALSE(test, list_empty(&transaction->tasks));
	KUNIT_EXPECT_EQ(test, refcount_read(&transaction->ref_count), 2U);
	KUNIT_EXPECT_EQ(test, transaction_attach_task(transaction, current),
			-EBUSY);
	transaction_detach_task(current);
	KUNIT_EXPECT_PTR_EQ(test, current_transaction(), NULL);
	KUNIT_EXPECT_EQ(test, atomic_read(&transaction->task_count), 0);
	KUNIT_EXPECT_TRUE(test, list_empty(&transaction->tasks));
	KUNIT_EXPECT_EQ(test, refcount_read(&transaction->ref_count), 1U);
	transaction_put(transaction);
}

static void transaction_live_fork_test(struct kunit *test) {
	struct transaction *transaction;

	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, current), 0);
	KUNIT_EXPECT_EQ(test, transaction_task_fork(current), 0);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, transaction_task_fork(current), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), 0);
	transaction_detach_task(current);
	transaction_put(transaction);
}

static void transaction_commit_abort_race_test(struct kunit *test) {
	struct transaction_race_context context;
	struct task_struct *abort_task;
	int end_ret;

	context.transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, context.transaction);
	init_completion(&context.start);
	init_completion(&context.done);
	context.abort_ret = -EINPROGRESS;
	KUNIT_ASSERT_EQ(test, begin_transaction(context.transaction), 0);

	abort_task = kthread_run(transaction_abort_thread, &context,
				 "transaction-abort-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, abort_task);
	complete(&context.start);
	end_ret = end_transaction(context.transaction);
	wait_for_completion(&context.done);
	kthread_stop(abort_task);

	KUNIT_EXPECT_TRUE(test, inactive_transaction(context.transaction));
	if (end_ret == -ECANCELED)
		KUNIT_EXPECT_EQ(test, context.abort_ret, 0);
	else
		KUNIT_EXPECT_EQ(test, end_ret, 0);
	KUNIT_EXPECT_TRUE(test, context.abort_ret == 0 ||
			  context.abort_ret == -EBUSY ||
			  context.abort_ret == -EINVAL);
	transaction_put(context.transaction);
}

static struct kunit_case transaction_test_cases[] = {
	KUNIT_CASE(transaction_initial_state_test),
	KUNIT_CASE(transaction_commit_test),
	KUNIT_CASE(transaction_abort_test),
	KUNIT_CASE(transaction_invalid_transition_test),
	KUNIT_CASE(transaction_reference_test),
	KUNIT_CASE(transaction_task_attachment_test),
	KUNIT_CASE(transaction_live_fork_test),
	KUNIT_CASE(transaction_commit_abort_race_test),
	{}
};

static struct kunit_suite transaction_test_suite = {
	.name = "transactions",
	.test_cases = transaction_test_cases,
};

kunit_test_suite(transaction_test_suite);

MODULE_DESCRIPTION("System transactions lifecycle tests");
MODULE_LICENSE("GPL");
