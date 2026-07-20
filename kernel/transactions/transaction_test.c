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

static atomic_t transaction_callback_calls = ATOMIC_INIT(0);

static int transaction_test_callback(struct txobj_thread_list_node *node) {
	atomic_inc(&transaction_callback_calls);
	return 0;
}

static int transaction_test_lock_callback(struct txobj_thread_list_node *node, int blocking) {
	atomic_inc(&transaction_callback_calls);
	return 0;
}

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
	KUNIT_EXPECT_TRUE(test, transaction_workset_empty(transaction));
	transaction_put(transaction);
}

static void transaction_object_initialization_test(struct kunit *test) {
	struct transaction_object object;

	transaction_object_init(&object, TRANSACTION_OBJECT_INODE);
	KUNIT_EXPECT_EQ(test, object.type, TRANSACTION_OBJECT_INODE);
	KUNIT_EXPECT_PTR_EQ(test, object.writer, NULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&object.readers));
	KUNIT_EXPECT_EQ(test, object.version, 0ULL);
	KUNIT_ASSERT_TRUE(test, spin_trylock(&object.lock));
	spin_unlock(&object.lock);
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

static void transaction_second_task_rejected_test(struct kunit *test) {
	struct transaction *transaction;
	struct task_struct *second_task;

	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	second_task = kunit_kzalloc(test, sizeof(*second_task), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, second_task);
	transaction_task_init(second_task);

	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, current), 0);
	KUNIT_EXPECT_EQ(test,
		transaction_attach_task(transaction, second_task),
		-EOPNOTSUPP);
	KUNIT_EXPECT_PTR_EQ(test, READ_ONCE(second_task->transaction), NULL);
	KUNIT_EXPECT_EQ(test, atomic_read(&transaction->task_count), 1);
	transaction_detach_task(current);
	transaction_put(transaction);
}

static void transaction_workset_lifecycle_test(struct kunit *test) {
	struct txobj_thread_list_node *node;
	struct txobj_thread_list_node *removed;
	struct transaction_object object;
	struct transaction *transaction;
	unsigned long original;
	unsigned long shadow;

	transaction_object_init(&object, TRANSACTION_OBJECT_CUSTOM);
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	node = transaction_workset_node_alloc(
		&shadow, &original, &object, TRANSACTION_OBJECT_CUSTOM,
		TRANSACTION_ACCESS_READ_WRITE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, node);
	KUNIT_EXPECT_PTR_EQ(test, node->tx, NULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&node->object_list));
	KUNIT_EXPECT_FALSE(test,
			  skiplist_linked(&node->workset_list));

	atomic_set(&transaction_callback_calls, 0);
	node->validate = transaction_test_callback;
	node->lock = transaction_test_lock_callback;
	node->unlock = transaction_test_lock_callback;
	node->commit = transaction_test_callback;
	node->abort = transaction_test_callback;
	node->release = transaction_test_lock_callback;
	KUNIT_ASSERT_EQ(test, transaction_workset_add(transaction, node), 0);
	KUNIT_EXPECT_PTR_EQ(test, node->tx, transaction);
	KUNIT_EXPECT_PTR_EQ(test,
		transaction_workset_find_orig(transaction, &original), node);
	KUNIT_EXPECT_PTR_EQ(test,
		transaction_workset_find_object(transaction, &object), node);
	KUNIT_EXPECT_FALSE(test, transaction_workset_empty(transaction));
	KUNIT_EXPECT_PTR_EQ(test, object.writer, NULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&object.readers));
	KUNIT_EXPECT_EQ(test, atomic_read(&transaction_callback_calls), 0);

	removed = transaction_workset_remove(transaction, node);
	KUNIT_EXPECT_PTR_EQ(test, removed, node);
	KUNIT_EXPECT_PTR_EQ(test, node->tx, NULL);
	KUNIT_EXPECT_FALSE(test,
			  skiplist_linked(&node->workset_list));
	KUNIT_EXPECT_PTR_EQ(test, node->orig_obj, &original);
	KUNIT_EXPECT_PTR_EQ(test, node->shadow_obj, &shadow);
	KUNIT_EXPECT_PTR_EQ(test, node->tx_obj, &object);
	KUNIT_EXPECT_EQ(test, node->rw, TRANSACTION_ACCESS_READ_WRITE);
	KUNIT_EXPECT_TRUE(test, node->commit == transaction_test_callback);
	KUNIT_EXPECT_TRUE(test, transaction_workset_empty(transaction));
	KUNIT_EXPECT_EQ(test, atomic_read(&transaction_callback_calls), 0);
	transaction_workset_node_free(node);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), 0);
	transaction_put(transaction);
}

static void transaction_workset_duplicate_test(struct kunit *test) {
	struct txobj_thread_list_node *same_object;
	struct txobj_thread_list_node *same_original;
	struct txobj_thread_list_node *first;
	struct transaction_object objects[2];
	struct transaction *transaction;
	unsigned long originals[2];

	transaction_object_init(&objects[0], TRANSACTION_OBJECT_CUSTOM);
	transaction_object_init(&objects[1], TRANSACTION_OBJECT_CUSTOM);
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	first = transaction_workset_node_alloc(
		NULL, &originals[0], &objects[0], TRANSACTION_OBJECT_CUSTOM,
		TRANSACTION_ACCESS_READ, GFP_KERNEL);
	same_original = transaction_workset_node_alloc(
		NULL, &originals[0], &objects[1], TRANSACTION_OBJECT_CUSTOM,
		TRANSACTION_ACCESS_READ, GFP_KERNEL);
	same_object = transaction_workset_node_alloc(
		NULL, &originals[1], &objects[0], TRANSACTION_OBJECT_CUSTOM,
		TRANSACTION_ACCESS_READ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, first);
	KUNIT_ASSERT_NOT_NULL(test, same_original);
	KUNIT_ASSERT_NOT_NULL(test, same_object);

	KUNIT_ASSERT_EQ(test, transaction_workset_add(transaction, first), 0);
	KUNIT_EXPECT_EQ(test,
		transaction_workset_add(transaction, same_original), -EEXIST);
	KUNIT_EXPECT_EQ(test,
		transaction_workset_add(transaction, same_object), -EEXIST);
	KUNIT_EXPECT_PTR_EQ(test, same_original->tx, NULL);
	KUNIT_EXPECT_PTR_EQ(test, same_object->tx, NULL);
	KUNIT_EXPECT_PTR_EQ(test,
		transaction_workset_remove(transaction, first), first);
	transaction_workset_node_free(first);
	transaction_workset_node_free(same_original);
	transaction_workset_node_free(same_object);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), 0);
	transaction_put(transaction);
}

static void transaction_workset_ordering_test(struct kunit *test) {
	static const unsigned int insertion_order[] = { 3, 0, 2, 1 };
	struct txobj_thread_list_node *nodes[ARRAY_SIZE(insertion_order)];
	struct txobj_thread_list_node *node;
	struct transaction_object objects[ARRAY_SIZE(insertion_order)];
	unsigned long originals[ARRAY_SIZE(insertion_order)];
	struct transaction *transaction;
	unsigned int index = 0;
	unsigned int i;

	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	for (i = 0; i < ARRAY_SIZE(insertion_order); i++) {
		unsigned int object_index = insertion_order[i];

		transaction_object_init(&objects[object_index],
					TRANSACTION_OBJECT_CUSTOM);
		nodes[object_index] = transaction_workset_node_alloc(
			NULL, &originals[object_index], &objects[object_index],
			TRANSACTION_OBJECT_CUSTOM, TRANSACTION_ACCESS_READ,
			GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, nodes[object_index]);
		KUNIT_ASSERT_EQ(test,
			transaction_workset_add(transaction,
						nodes[object_index]),
			0);
	}

	skiplist_for_each_entry(node, &transaction->object_list,
					    workset_list) {
		KUNIT_ASSERT_LT(test, index, ARRAY_SIZE(nodes));
		KUNIT_EXPECT_PTR_EQ(test, node->orig_obj, &originals[index]);
		index++;
	}
	KUNIT_EXPECT_EQ(test, index, ARRAY_SIZE(nodes));
	KUNIT_EXPECT_EQ(test,
		skiplist_validate(&transaction->object_list), 0);

	for (i = 0; i < ARRAY_SIZE(nodes); i++) {
		KUNIT_EXPECT_PTR_EQ(test,
			transaction_workset_remove(transaction, nodes[i]),
			nodes[i]);
		transaction_workset_node_free(nodes[i]);
	}
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), 0);
	transaction_put(transaction);
}

static void transaction_nonempty_workset_end_test(struct kunit *test) {
	struct txobj_thread_list_node *node;
	struct transaction_object object;
	struct transaction *transaction;
	unsigned long original;

	transaction_object_init(&object, TRANSACTION_OBJECT_CUSTOM);
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	node = transaction_workset_node_alloc(
		NULL, &original, &object, TRANSACTION_OBJECT_CUSTOM,
		TRANSACTION_ACCESS_READ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, node);
	KUNIT_ASSERT_EQ(test, transaction_workset_add(transaction, node), 0);

	KUNIT_EXPECT_EQ(test, end_transaction(transaction), -EBUSY);
	KUNIT_EXPECT_EQ(test, transaction_status(transaction), TRANSACTION_ACTIVE);
	KUNIT_EXPECT_PTR_EQ(test,
		transaction_workset_remove(transaction, node), node);
	transaction_workset_node_free(node);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), 0);
	KUNIT_EXPECT_TRUE(test, inactive_transaction(transaction));
	transaction_put(transaction);
}

static void transaction_aborted_nonempty_workset_end_test(struct kunit *test) {
	struct txobj_thread_list_node *node;
	struct transaction_object object;
	struct transaction *transaction;
	unsigned long original;

	transaction_object_init(&object, TRANSACTION_OBJECT_CUSTOM);
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	node = transaction_workset_node_alloc(
		NULL, &original, &object, TRANSACTION_OBJECT_CUSTOM,
		TRANSACTION_ACCESS_READ, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, node);
	KUNIT_ASSERT_EQ(test, transaction_workset_add(transaction, node), 0);
	KUNIT_ASSERT_EQ(test, abort_transaction(transaction), 0);

	KUNIT_EXPECT_EQ(test, end_transaction(transaction), -EBUSY);
	KUNIT_EXPECT_EQ(test, transaction_status(transaction),
			TRANSACTION_ABORTED);
	KUNIT_EXPECT_PTR_EQ(test,
		transaction_workset_remove(transaction, node), node);
	transaction_workset_node_free(node);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), -ECANCELED);
	KUNIT_EXPECT_TRUE(test, inactive_transaction(transaction));
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
	KUNIT_CASE(transaction_object_initialization_test),
	KUNIT_CASE(transaction_commit_test),
	KUNIT_CASE(transaction_abort_test),
	KUNIT_CASE(transaction_invalid_transition_test),
	KUNIT_CASE(transaction_reference_test),
	KUNIT_CASE(transaction_task_attachment_test),
	KUNIT_CASE(transaction_second_task_rejected_test),
	KUNIT_CASE(transaction_workset_lifecycle_test),
	KUNIT_CASE(transaction_workset_duplicate_test),
	KUNIT_CASE(transaction_workset_ordering_test),
	KUNIT_CASE(transaction_nonempty_workset_end_test),
	KUNIT_CASE(transaction_aborted_nonempty_workset_end_test),
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
