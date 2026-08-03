// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>
#include <linux/anon_inodes.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/iversion.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/transaction.h>
#include <linux/tx_hlist.h>
#include <linux/tx_list2.h>

struct transaction_race_context {
	struct transaction *transaction;
	struct completion start;
	struct completion done;
	int abort_ret;
};

struct transaction_finish_race_context {
	struct transaction *transaction;
	struct completion blocking_lock;
	struct completion resume;
	struct completion done;
	int commit_count;
	int abort_count;
	int end_ret;
};

/* Shared test state used by workset callbacks to verify that commit,
   abort, and release handlers are called on the expected path. */
struct transaction_workset_test_context {
	int commit_count;
	int abort_count;
	int release_count;
};

struct transaction_list_test_item {
	int value;
	struct tx_list2_entry_ref link;
};

struct transaction_contention_test_context {
	struct transaction *transaction;
	struct task_struct *task;
};

enum transaction_finish_test_event {
	TRANSACTION_FINISH_BLOCKING_LOCK = 1,
	TRANSACTION_FINISH_NONBLOCKING_LOCK,
	TRANSACTION_FINISH_VALIDATE,
	TRANSACTION_FINISH_COMMIT,
	TRANSACTION_FINISH_NONBLOCKING_UNLOCK,
	TRANSACTION_FINISH_BLOCKING_UNLOCK,
	TRANSACTION_FINISH_RELEASE,
};

struct transaction_finish_test_log {
	int events[16];
	unsigned int count;
};

struct transaction_finish_test_context {
	struct transaction_finish_test_log *log;
	struct transaction_object *object;
	struct transaction *transaction;
	int id;
	int blocking_locks;
	int nonblocking_locks;
	bool validate_locked;
	bool validate_owned;
	bool commit_locked;
	bool commit_unowned;
	bool release_unlocked;
	bool release_detached;
};

static const struct file_operations transaction_test_file_operations = { };

static atomic_t transaction_callback_calls = ATOMIC_INIT(0);

static int transaction_test_callback(struct txobj_thread_list_node *node) {
	atomic_inc(&transaction_callback_calls);
	return 0;
}

static int transaction_test_lock_callback(struct txobj_thread_list_node *node, int blocking) {
	atomic_inc(&transaction_callback_calls);
	return 0;
}

static void transaction_list_test_item_init(struct transaction_list_test_item *item,
					    int value)
{
	item->value = value;
	INIT_TX_LIST2_REF(&item->link);
}

static int transaction_list_test_count(struct tx_list2_head *head)
{
	struct tx_list2_iterator iter;
	int count = 0;
	int ret;

	ret = tx_list2_get_iterator(&iter, head);
	if (ret)
		return ret;
	while (tx_list2_iter_next(&iter))
		count++;
	tx_list2_put_iterator(&iter);

	return count;
}

static int transaction_test_begin_current(struct transaction **transaction_out)
{
	struct transaction *transaction;
	int ret;

	transaction = transaction_alloc(GFP_KERNEL);
	if (!transaction)
		return -ENOMEM;

	ret = transaction_attach_task(transaction, current);
	if (ret)
		goto put_transaction;

	ret = begin_transaction(transaction);
	if (ret)
		goto detach_task;

	*transaction_out = transaction;
	return 0;

detach_task:
	transaction_detach_task(current);
put_transaction:
	transaction_put(transaction);
	return ret;
}

static void transaction_test_finish_current(struct transaction *transaction)
{
	transaction_detach_task(current);
	transaction_put(transaction);
}

static int transaction_finish_race_lock(struct txobj_thread_list_node *node, int blocking) {
	struct transaction_finish_race_context *context = node->shadow_obj;

	if (blocking) {
		complete(&context->blocking_lock);
		wait_for_completion(&context->resume);
	}
	return 0;
}

static int transaction_finish_race_commit(struct txobj_thread_list_node *node) {
	struct transaction_finish_race_context *context = node->shadow_obj;

	context->commit_count++;
	return 0;
}

static int transaction_finish_race_abort(struct txobj_thread_list_node *node) {
	struct transaction_finish_race_context *context = node->shadow_obj;

	context->abort_count++;
	return 0;
}

static int transaction_finish_thread(void *data) {
	struct transaction_finish_race_context *context = data;

	context->end_ret = end_transaction(context->transaction);
	complete(&context->done);
	return 0;
}

static int transaction_validation_fail(struct txobj_thread_list_node *node) {
	return -ESTALE;
}

static void transaction_finish_test_record(struct transaction_finish_test_context *context, int event) {
	context->log->events[context->log->count++] = context->id * 10 + event;
}

static int transaction_finish_test_lock(struct txobj_thread_list_node *node, int blocking) {
	struct transaction_finish_test_context *context = node->shadow_obj;

	if (blocking) {
		context->blocking_locks++;
		transaction_finish_test_record(context, TRANSACTION_FINISH_BLOCKING_LOCK);
	} else {
		context->nonblocking_locks++;
		transaction_finish_test_record(context, TRANSACTION_FINISH_NONBLOCKING_LOCK);
	}

	return 0;
}

static int transaction_finish_test_unlock(struct txobj_thread_list_node *node, int blocking) {
	struct transaction_finish_test_context *context = node->shadow_obj;

	if (blocking) {
		context->blocking_locks--;
		transaction_finish_test_record(context, TRANSACTION_FINISH_BLOCKING_UNLOCK);
	} else {
		context->nonblocking_locks--;
		transaction_finish_test_record(context, TRANSACTION_FINISH_NONBLOCKING_UNLOCK);
	}

	return 0;
}

static int transaction_finish_test_validate(struct txobj_thread_list_node *node) {
	struct transaction_finish_test_context *context = node->shadow_obj;

	context->validate_locked = spin_is_locked(&context->object->lock);
	context->validate_owned = context->object->writer == context->transaction && !list_empty(&node->object_list);
	transaction_finish_test_record(context, TRANSACTION_FINISH_VALIDATE);
	return 0;
}

static int transaction_finish_test_commit(struct txobj_thread_list_node *node) {
	struct transaction_finish_test_context *context = node->shadow_obj;

	context->commit_locked = spin_is_locked(&context->object->lock);
	context->commit_unowned = context->object->writer == NULL && list_empty(&node->object_list);
	transaction_finish_test_record(context, TRANSACTION_FINISH_COMMIT);
	return 0;
}

static int transaction_finish_test_release(struct txobj_thread_list_node *node, int early) {
	struct transaction_finish_test_context *context = node->shadow_obj;

	context->release_unlocked = !context->blocking_locks && !context->nonblocking_locks;
	context->release_detached = node->tx == NULL;
	transaction_finish_test_record(context, TRANSACTION_FINISH_RELEASE);
	return 0;
}

/* Workset callback used by tests to record that an object reached the commit path. */
static int transaction_workset_commit(struct txobj_thread_list_node * node) {
	struct transaction_workset_test_context * context = node->shadow_obj;
	context->commit_count++;
	return 0;
}

/* Workset callback used by tests to record that an object reached the abort path. */
static int transaction_workset_abort(struct txobj_thread_list_node * node) {
	struct transaction_workset_test_context *context = node->shadow_obj;
	context->abort_count++;
	return 0;
}

/* Workset callback used by tests to record that object-private state was released after commit or abort handling. */
static int transaction_workset_release(struct txobj_thread_list_node * node, int early) {
	struct transaction_workset_test_context *context = node->shadow_obj;
	context->release_count++;
	return 0;
}

static int transaction_abort_thread(void *data) {
	struct transaction_race_context *context = data;

	wait_for_completion(&context->start);
	context->abort_ret = abort_transaction(context->transaction);
	complete(&context->done);

	return 0;
}

static int transaction_contention_test_init(struct kunit *test,
					    struct transaction_contention_test_context *context,
					    int priority) {
	int ret;

	context->transaction = transaction_alloc(GFP_KERNEL);
	if (!context->transaction)
		return -ENOMEM;

	context->task = kunit_kzalloc(test, sizeof(*context->task), GFP_KERNEL);
	if (!context->task) {
		transaction_put(context->transaction);
		context->transaction = NULL;
		return -ENOMEM;
	}

	transaction_task_init(context->task);
	context->task->prio = priority;
	ret = transaction_attach_task(context->transaction, context->task);
	if (ret)
		goto put_transaction;

	ret = begin_transaction(context->transaction);
	if (ret)
		goto detach_task;

	return 0;

detach_task:
	transaction_detach_task(context->task);
put_transaction:
	transaction_put(context->transaction);
	context->transaction = NULL;
	return ret;
}

static void transaction_contention_cleanup(struct transaction_contention_test_context *context) {
	if (!context->transaction)
		return;

	if (!inactive_transaction(context->transaction)) {
		atomic_set(&context->transaction->status, TRANSACTION_ACTIVE);
		end_transaction(context->transaction);
	}
	transaction_detach_task(context->task);
	transaction_put(context->transaction);
}

static int transaction_object_test_acquire(struct transaction_contention_test_context *context, struct transaction_object *object,
					   void *orig_obj, enum transaction_access_mode mode, bool *should_sleep,
					   struct txobj_thread_list_node **node_out) {
	struct txobj_thread_list_node *node;
	int ret;

	node = transaction_workset_node_alloc(NULL, orig_obj, object, object->type, mode, GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	ret = transaction_workset_add(context->transaction, node);
	if (ret) {
		transaction_workset_node_free(node);
		return ret;
	}

	*node_out = node;
	return transaction_object_acquire(context->transaction, node, mode, should_sleep);
}

static void transaction_object_test_cleanup(struct transaction_contention_test_context *context, struct txobj_thread_list_node *node) {
	if (node) {
		transaction_object_remove_ownership(node);
		transaction_workset_remove(context->transaction, node);
		transaction_workset_node_free(node);
	}
	transaction_contention_cleanup(context);
}

static int transaction_contention_test_init_pair(struct kunit *test,
						 struct transaction_contention_test_context *a,
						 int priority_a,
						 struct transaction_contention_test_context *b,
						 int priority_b) {
	int ret;

	ret = transaction_contention_test_init(test, a, priority_a);
	if (ret)
		return ret;

	ret = transaction_contention_test_init(test, b, priority_b);
	if (ret)
		transaction_contention_cleanup(a);

	return ret;
}

static void transaction_contention_priority_test(struct kunit *test) {
	struct transaction_contention_test_context a = { };
	struct transaction_contention_test_context b = { };
	bool should_sleep = false;

	KUNIT_ASSERT_EQ(test,
		transaction_contention_test_init_pair(test, &a, 100, &b, 120),
		0);
	a.transaction->timestamp = 20;
	b.transaction->timestamp = 10;

	KUNIT_EXPECT_TRUE(test,
		transaction_contention_manager(a.transaction, b.transaction,
					       &should_sleep));
	KUNIT_EXPECT_TRUE(test, should_sleep);
	should_sleep = false;
	KUNIT_EXPECT_FALSE(test,
		transaction_contention_manager(b.transaction, a.transaction,
					       &should_sleep));
	KUNIT_EXPECT_TRUE(test, should_sleep);

	transaction_contention_cleanup(&b);
	transaction_contention_cleanup(&a);
}

static void transaction_contention_timestamp_test(struct kunit *test) {
	struct transaction_contention_test_context a = { };
	struct transaction_contention_test_context b = { };
	bool should_sleep = false;

	KUNIT_ASSERT_EQ(test,
		transaction_contention_test_init_pair(test, &a, 120, &b, 120),
		0);
	KUNIT_ASSERT_LT(test, a.transaction->timestamp,
			b.transaction->timestamp);

	KUNIT_EXPECT_TRUE(test,
		transaction_contention_manager(a.transaction, b.transaction,
					       &should_sleep));
	KUNIT_EXPECT_TRUE(test, should_sleep);
	should_sleep = false;
	KUNIT_EXPECT_FALSE(test,
		transaction_contention_manager(b.transaction, a.transaction,
					       &should_sleep));
	KUNIT_EXPECT_TRUE(test, should_sleep);

	atomic_set(&b.transaction->status, TRANSACTION_INACTIVE);
	should_sleep = true;
	KUNIT_EXPECT_TRUE(test,
		transaction_contention_manager(a.transaction, b.transaction,
					       &should_sleep));
	KUNIT_EXPECT_FALSE(test, should_sleep);

	transaction_contention_cleanup(&b);
	transaction_contention_cleanup(&a);
}

static void transaction_contention_aborted_test(struct kunit *test) {
	struct transaction_contention_test_context a = { };
	struct transaction_contention_test_context b = { };
	bool should_sleep = true;

	KUNIT_ASSERT_EQ(test,
		transaction_contention_test_init_pair(test, &a, 100, &b, 120),
		0);
	atomic_set(&a.transaction->status, TRANSACTION_ABORTED);

	KUNIT_EXPECT_FALSE(test,
		transaction_contention_manager(a.transaction, b.transaction,
					       &should_sleep));
	KUNIT_EXPECT_FALSE(test, should_sleep);
	should_sleep = true;
	KUNIT_EXPECT_TRUE(test,
		transaction_contention_manager(b.transaction, a.transaction,
					       &should_sleep));
	KUNIT_EXPECT_FALSE(test, should_sleep);

	atomic_set(&a.transaction->status, TRANSACTION_ABORTING);
	should_sleep = true;
	KUNIT_EXPECT_FALSE(test,
		transaction_contention_manager(a.transaction, b.transaction,
					       &should_sleep));
	KUNIT_EXPECT_FALSE(test, should_sleep);

	transaction_contention_cleanup(&b);
	transaction_contention_cleanup(&a);
}

static void transaction_contention_committing_test(struct kunit *test) {
	struct transaction_contention_test_context a = { };
	struct transaction_contention_test_context b = { };
	bool should_sleep = true;

	KUNIT_ASSERT_EQ(test,
		transaction_contention_test_init_pair(test, &a, 120, &b, 100),
		0);
	atomic_set(&a.transaction->status, TRANSACTION_COMMITTING);

	KUNIT_EXPECT_TRUE(test,
		transaction_contention_manager(a.transaction, b.transaction,
					       &should_sleep));
	KUNIT_EXPECT_FALSE(test, should_sleep);
	should_sleep = true;
	KUNIT_EXPECT_FALSE(test,
		transaction_contention_manager(b.transaction, a.transaction,
					       &should_sleep));
	KUNIT_EXPECT_FALSE(test, should_sleep);

	transaction_contention_cleanup(&b);
	transaction_contention_cleanup(&a);
}

static void transaction_object_readers_test(struct kunit *test) {
	struct transaction_contention_test_context a = { };
	struct transaction_contention_test_context b = { };
	struct txobj_thread_list_node *a_node = NULL;
	struct txobj_thread_list_node *b_node = NULL;
	struct transaction_object object;
	unsigned long original;

	transaction_object_init(&object, TRANSACTION_OBJECT_CUSTOM);
	KUNIT_ASSERT_EQ(test, transaction_contention_test_init_pair(test, &a, 100, &b, 120), 0);
	KUNIT_ASSERT_EQ(test, transaction_object_test_acquire(&a, &object, &original, TRANSACTION_ACCESS_READ, NULL, &a_node), 0);
	KUNIT_ASSERT_EQ(test, transaction_object_test_acquire(&b, &object, &original, TRANSACTION_ACCESS_READ, NULL, &b_node), 0);

	KUNIT_EXPECT_PTR_EQ(test, object.writer, NULL);
	KUNIT_EXPECT_FALSE(test, list_empty(&a_node->object_list));
	KUNIT_EXPECT_FALSE(test, list_empty(&b_node->object_list));
	KUNIT_EXPECT_EQ(test, transaction_status(a.transaction), TRANSACTION_ACTIVE);
	KUNIT_EXPECT_EQ(test, transaction_status(b.transaction), TRANSACTION_ACTIVE);

	transaction_object_remove_ownership(a_node);
	KUNIT_EXPECT_TRUE(test, list_empty(&a_node->object_list));
	KUNIT_EXPECT_FALSE(test, list_empty(&object.readers));
	transaction_object_remove_ownership(b_node);
	KUNIT_EXPECT_TRUE(test, list_empty(&object.readers));

	transaction_object_test_cleanup(&b, b_node);
	transaction_object_test_cleanup(&a, a_node);
}

static void transaction_object_reader_wins_test(struct kunit *test) {
	struct transaction_contention_test_context reader = { };
	struct transaction_contention_test_context writer = { };
	struct txobj_thread_list_node *reader_node = NULL;
	struct txobj_thread_list_node *writer_node = NULL;
	struct transaction_object object;
	unsigned long original;
	bool should_sleep = false;

	transaction_object_init(&object, TRANSACTION_OBJECT_CUSTOM);
	KUNIT_ASSERT_EQ(test, transaction_contention_test_init_pair(test, &reader, 100, &writer, 120), 0);
	KUNIT_ASSERT_EQ(test, transaction_object_test_acquire(&reader, &object, &original, TRANSACTION_ACCESS_READ, NULL, &reader_node), 0);
	KUNIT_EXPECT_EQ(test, transaction_object_test_acquire(&writer, &object, &original, TRANSACTION_ACCESS_READ_WRITE,
							     &should_sleep, &writer_node), -ECANCELED);

	KUNIT_EXPECT_TRUE(test, should_sleep);
	KUNIT_EXPECT_EQ(test, transaction_status(reader.transaction), TRANSACTION_ACTIVE);
	KUNIT_EXPECT_EQ(test, transaction_status(writer.transaction), TRANSACTION_ABORTED);
	KUNIT_EXPECT_PTR_EQ(test, object.writer, NULL);
	KUNIT_EXPECT_FALSE(test, list_empty(&reader_node->object_list));
	KUNIT_EXPECT_TRUE(test, list_empty(&writer_node->object_list));

	transaction_object_test_cleanup(&writer, writer_node);
	transaction_object_test_cleanup(&reader, reader_node);
}

static void transaction_object_writer_wins_test(struct kunit *test) {
	struct transaction_contention_test_context reader = { };
	struct transaction_contention_test_context writer = { };
	struct txobj_thread_list_node *reader_node = NULL;
	struct txobj_thread_list_node *writer_node = NULL;
	struct transaction_object object;
	unsigned long original;

	transaction_object_init(&object, TRANSACTION_OBJECT_CUSTOM);
	KUNIT_ASSERT_EQ(test, transaction_contention_test_init_pair(test, &reader, 120, &writer, 100), 0);
	KUNIT_ASSERT_EQ(test, transaction_object_test_acquire(&reader, &object, &original, TRANSACTION_ACCESS_READ, NULL, &reader_node), 0);
	KUNIT_ASSERT_EQ(test, transaction_object_test_acquire(&writer, &object, &original, TRANSACTION_ACCESS_READ_WRITE, NULL, &writer_node), 0);

	KUNIT_EXPECT_EQ(test, transaction_status(reader.transaction), TRANSACTION_ABORTED);
	KUNIT_EXPECT_EQ(test, transaction_status(writer.transaction), TRANSACTION_ACTIVE);
	KUNIT_EXPECT_PTR_EQ(test, object.writer, writer.transaction);
	KUNIT_EXPECT_TRUE(test, list_empty(&reader_node->object_list));
	KUNIT_EXPECT_FALSE(test, list_empty(&writer_node->object_list));

	transaction_object_test_cleanup(&writer, writer_node);
	transaction_object_test_cleanup(&reader, reader_node);
}

static void transaction_object_writer_conflict_test(struct kunit *test) {
	struct transaction_contention_test_context a = { };
	struct transaction_contention_test_context b = { };
	struct txobj_thread_list_node *a_node = NULL;
	struct txobj_thread_list_node *b_node = NULL;
	struct transaction_object object;
	unsigned long original;

	transaction_object_init(&object, TRANSACTION_OBJECT_CUSTOM);
	KUNIT_ASSERT_EQ(test, transaction_contention_test_init_pair(test, &a, 120, &b, 100), 0);
	KUNIT_ASSERT_EQ(test, transaction_object_test_acquire(&a, &object, &original, TRANSACTION_ACCESS_READ_WRITE, NULL, &a_node), 0);
	KUNIT_ASSERT_EQ(test, transaction_object_test_acquire(&b, &object, &original, TRANSACTION_ACCESS_READ_WRITE, NULL, &b_node), 0);

	KUNIT_EXPECT_EQ(test, transaction_status(a.transaction), TRANSACTION_ABORTED);
	KUNIT_EXPECT_PTR_EQ(test, object.writer, b.transaction);
	KUNIT_EXPECT_TRUE(test, list_empty(&a_node->object_list));
	KUNIT_EXPECT_FALSE(test, list_empty(&b_node->object_list));

	transaction_object_test_cleanup(&b, b_node);
	transaction_object_test_cleanup(&a, a_node);
}

static void transaction_file_owner_displaced_test(struct kunit *test) {
	struct transaction_contention_test_context owner = { };
	struct transaction_contention_test_context contender = { };
	struct txobj_thread_list_node *owner_node = NULL;
	struct txobj_thread_list_node *contender_node = NULL;
	struct transaction_object object;
	unsigned long original;
	bool should_sleep = false;

	transaction_object_init(&object, TRANSACTION_OBJECT_FILE);
	KUNIT_ASSERT_EQ(test, transaction_contention_test_init_pair(test, &owner, 120, &contender, 100), 0);
	KUNIT_ASSERT_EQ(test, transaction_object_test_acquire(&owner, &object, &original, TRANSACTION_ACCESS_READ_WRITE, NULL, &owner_node), 0);
	KUNIT_EXPECT_EQ(test, transaction_object_test_acquire(&contender, &object, &original, TRANSACTION_ACCESS_READ_WRITE,
							     &should_sleep, &contender_node), 0);

	KUNIT_EXPECT_FALSE(test, should_sleep);
	KUNIT_EXPECT_EQ(test, transaction_status(owner.transaction), TRANSACTION_ABORTED);
	KUNIT_EXPECT_EQ(test, transaction_status(contender.transaction), TRANSACTION_ACTIVE);
	KUNIT_EXPECT_PTR_EQ(test, object.writer, contender.transaction);
	KUNIT_EXPECT_TRUE(test, list_empty(&owner_node->object_list));
	KUNIT_EXPECT_FALSE(test, list_empty(&contender_node->object_list));

	transaction_object_test_cleanup(&contender, contender_node);
	transaction_object_test_cleanup(&owner, owner_node);
}

static void transaction_object_upgrade_test(struct kunit *test) {
	struct transaction_contention_test_context a = { };
	struct transaction_contention_test_context b = { };
	struct txobj_thread_list_node *a_node = NULL;
	struct txobj_thread_list_node *b_node = NULL;
	struct transaction_object object;
	unsigned long original;

	transaction_object_init(&object, TRANSACTION_OBJECT_CUSTOM);
	KUNIT_ASSERT_EQ(test, transaction_contention_test_init_pair(test, &a, 100, &b, 120), 0);
	KUNIT_ASSERT_EQ(test, transaction_object_test_acquire(&a, &object, &original, TRANSACTION_ACCESS_READ, NULL, &a_node), 0);
	KUNIT_ASSERT_EQ(test, transaction_object_test_acquire(&b, &object, &original, TRANSACTION_ACCESS_READ, NULL, &b_node), 0);
	KUNIT_ASSERT_EQ(test, transaction_object_acquire(a.transaction, a_node, TRANSACTION_ACCESS_READ_WRITE, NULL), 0);

	KUNIT_EXPECT_PTR_EQ(test, object.writer, a.transaction);
	KUNIT_EXPECT_EQ(test, a_node->rw, TRANSACTION_ACCESS_READ_WRITE);
	KUNIT_EXPECT_EQ(test, transaction_status(b.transaction), TRANSACTION_ABORTED);
	KUNIT_EXPECT_TRUE(test, list_empty(&b_node->object_list));

	transaction_object_test_cleanup(&b, b_node);
	transaction_object_test_cleanup(&a, a_node);
}

static void transaction_object_upgrade_loses_test(struct kunit *test) {
	struct transaction_contention_test_context a = { };
	struct transaction_contention_test_context b = { };
	struct txobj_thread_list_node *a_node = NULL;
	struct txobj_thread_list_node *b_node = NULL;
	struct transaction_object object;
	unsigned long original;
	bool should_sleep = false;

	transaction_object_init(&object, TRANSACTION_OBJECT_CUSTOM);
	KUNIT_ASSERT_EQ(test, transaction_contention_test_init_pair(test, &a, 120, &b, 100), 0);
	KUNIT_ASSERT_EQ(test, transaction_object_test_acquire(&a, &object, &original, TRANSACTION_ACCESS_READ, NULL, &a_node), 0);
	KUNIT_ASSERT_EQ(test, transaction_object_test_acquire(&b, &object, &original, TRANSACTION_ACCESS_READ, NULL, &b_node), 0);
	KUNIT_EXPECT_EQ(test, transaction_object_acquire(a.transaction, a_node, TRANSACTION_ACCESS_READ_WRITE, &should_sleep), -ECANCELED);

	KUNIT_EXPECT_TRUE(test, should_sleep);
	KUNIT_EXPECT_PTR_EQ(test, object.writer, NULL);
	KUNIT_EXPECT_EQ(test, a_node->rw, TRANSACTION_ACCESS_READ);
	KUNIT_EXPECT_EQ(test, transaction_status(a.transaction), TRANSACTION_ABORTED);
	KUNIT_EXPECT_FALSE(test, list_empty(&a_node->object_list));
	KUNIT_EXPECT_FALSE(test, list_empty(&b_node->object_list));

	transaction_object_test_cleanup(&b, b_node);
	transaction_object_test_cleanup(&a, a_node);
}

static void transaction_object_reuse_test(struct kunit *test) {
	struct transaction_contention_test_context a = { };
	struct transaction_contention_test_context b = { };
	struct txobj_thread_list_node *a_node = NULL;
	struct txobj_thread_list_node *b_node = NULL;
	struct transaction_object object;
	unsigned long original;

	transaction_object_init(&object, TRANSACTION_OBJECT_CUSTOM);
	KUNIT_ASSERT_EQ(test, transaction_contention_test_init_pair(test, &a, 100, &b, 120), 0);
	KUNIT_ASSERT_EQ(test, transaction_object_test_acquire(&a, &object, &original, TRANSACTION_ACCESS_READ_WRITE, NULL, &a_node), 0);
	transaction_object_remove_ownership(a_node);
	KUNIT_EXPECT_PTR_EQ(test, object.writer, NULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&object.readers));

	KUNIT_ASSERT_EQ(test, transaction_object_test_acquire(&b, &object, &original, TRANSACTION_ACCESS_READ_WRITE, NULL, &b_node), 0);
	KUNIT_EXPECT_PTR_EQ(test, object.writer, b.transaction);
	KUNIT_EXPECT_FALSE(test, list_empty(&b_node->object_list));

	transaction_object_test_cleanup(&b, b_node);
	transaction_object_test_cleanup(&a, a_node);
}

static void transaction_finish_order_test(struct kunit *test) {
	static const unsigned int insertion_order[] = { 1, 0 };
	static const int expected_events[] = {
		1, 11, 2, 12, 4, 14, 15, 5, 16, 6, 7, 17,
	};
	struct transaction_finish_test_context contexts[2];
	struct transaction_finish_test_log log = { };
	struct txobj_thread_list_node *nodes[2];
	struct transaction_object objects[2];
	unsigned long originals[2];
	struct transaction *transaction;
	unsigned int i;

	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);

	for (i = 0; i < ARRAY_SIZE(insertion_order); i++) {
		unsigned int index = insertion_order[i];

		transaction_object_init(&objects[index], TRANSACTION_OBJECT_CUSTOM);
		contexts[index] = (struct transaction_finish_test_context) {
			.log = &log,
			.object = &objects[index],
			.transaction = transaction,
			.id = index,
		};
		nodes[index] = transaction_workset_node_alloc(&contexts[index], &originals[index], &objects[index],
							     TRANSACTION_OBJECT_CUSTOM, TRANSACTION_ACCESS_READ_WRITE, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, nodes[index]);
		nodes[index]->lock = transaction_finish_test_lock;
		nodes[index]->unlock = transaction_finish_test_unlock;
		nodes[index]->validate = transaction_finish_test_validate;
		nodes[index]->commit = transaction_finish_test_commit;
		nodes[index]->release = transaction_finish_test_release;
		KUNIT_ASSERT_EQ(test, transaction_workset_add(transaction, nodes[index]), 0);
		KUNIT_ASSERT_EQ(test, transaction_object_acquire(transaction, nodes[index], TRANSACTION_ACCESS_READ_WRITE, NULL), 0);
	}

	KUNIT_ASSERT_EQ(test, end_transaction(transaction), 0);
	KUNIT_ASSERT_EQ(test, log.count, ARRAY_SIZE(expected_events));
	for (i = 0; i < ARRAY_SIZE(expected_events); i++)
		KUNIT_EXPECT_EQ(test, log.events[i], expected_events[i]);

	for (i = 0; i < ARRAY_SIZE(contexts); i++) {
		KUNIT_EXPECT_FALSE(test, contexts[i].validate_locked);
		KUNIT_EXPECT_FALSE(test, contexts[i].validate_owned);
		KUNIT_EXPECT_TRUE(test, contexts[i].commit_locked);
		KUNIT_EXPECT_TRUE(test, contexts[i].commit_unowned);
		KUNIT_EXPECT_TRUE(test, contexts[i].release_unlocked);
		KUNIT_EXPECT_TRUE(test, contexts[i].release_detached);
		KUNIT_EXPECT_PTR_EQ(test, objects[i].writer, NULL);
		KUNIT_EXPECT_TRUE(test, list_empty(&objects[i].readers));
	}
	transaction_put(transaction);
}

static void transaction_abort_before_final_commit_test(struct kunit *test) {
	struct transaction_finish_race_context context = { };
	struct txobj_thread_list_node *node;
	struct transaction_object object;
	struct task_struct *end_task;
	unsigned long original;

	transaction_object_init(&object, TRANSACTION_OBJECT_CUSTOM);
	context.transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, context.transaction);
	init_completion(&context.blocking_lock);
	init_completion(&context.resume);
	init_completion(&context.done);
	context.end_ret = -EINPROGRESS;
	KUNIT_ASSERT_EQ(test, begin_transaction(context.transaction), 0);
	node = transaction_workset_node_alloc(&context, &original, &object, TRANSACTION_OBJECT_CUSTOM,
					     TRANSACTION_ACCESS_READ_WRITE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, node);
	node->lock = transaction_finish_race_lock;
	node->commit = transaction_finish_race_commit;
	node->abort = transaction_finish_race_abort;
	KUNIT_ASSERT_EQ(test, transaction_workset_add(context.transaction, node), 0);
	KUNIT_ASSERT_EQ(test, transaction_object_acquire(context.transaction, node, TRANSACTION_ACCESS_READ_WRITE, NULL), 0);

	end_task = kthread_run(transaction_finish_thread, &context, "transaction-finish-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, end_task);
	wait_for_completion(&context.blocking_lock);
	KUNIT_EXPECT_EQ(test, transaction_status(context.transaction), TRANSACTION_ACTIVE);
	KUNIT_EXPECT_EQ(test, atomic_read(&context.transaction->finishing), 1);
	KUNIT_EXPECT_EQ(test, transaction_object_acquire(context.transaction, node, TRANSACTION_ACCESS_READ_WRITE, NULL), -EBUSY);
	KUNIT_EXPECT_EQ(test, abort_transaction(context.transaction), 0);
	complete(&context.resume);
	wait_for_completion(&context.done);
	kthread_stop(end_task);

	KUNIT_EXPECT_EQ(test, context.end_ret, -ECANCELED);
	KUNIT_EXPECT_EQ(test, context.commit_count, 0);
	KUNIT_EXPECT_EQ(test, context.abort_count, 1);
	KUNIT_EXPECT_TRUE(test, inactive_transaction(context.transaction));
	KUNIT_EXPECT_EQ(test, atomic_read(&context.transaction->finishing), 0);
	KUNIT_EXPECT_PTR_EQ(test, object.writer, NULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&object.readers));
	transaction_put(context.transaction);
}

static void __maybe_unused transaction_validation_failure_test(struct kunit *test) {
	struct transaction_workset_test_context context = { };
	struct txobj_thread_list_node *node;
	struct transaction_object object;
	struct transaction *transaction;
	unsigned long original;

	transaction_object_init(&object, TRANSACTION_OBJECT_CUSTOM);
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	node = transaction_workset_node_alloc(&context, &original, &object, TRANSACTION_OBJECT_CUSTOM,
					     TRANSACTION_ACCESS_READ_WRITE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, node);
	node->validate = transaction_validation_fail;
	node->commit = transaction_workset_commit;
	node->abort = transaction_workset_abort;
	node->release = transaction_workset_release;
	KUNIT_ASSERT_EQ(test, transaction_workset_add(transaction, node), 0);
	KUNIT_ASSERT_EQ(test, transaction_object_acquire(transaction, node, TRANSACTION_ACCESS_READ_WRITE, NULL), 0);

	KUNIT_EXPECT_EQ(test, end_transaction(transaction), -ESTALE);
	KUNIT_EXPECT_EQ(test, context.commit_count, 0);
	KUNIT_EXPECT_EQ(test, context.abort_count, 1);
	KUNIT_EXPECT_EQ(test, context.release_count, 1);
	KUNIT_EXPECT_EQ(test, transaction->count, 1U);
	KUNIT_EXPECT_TRUE(test, inactive_transaction(transaction));
	KUNIT_EXPECT_PTR_EQ(test, object.writer, NULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&object.readers));
	transaction_put(transaction);
}

static void transaction_initial_state_test(struct kunit *test) {
	struct transaction *transaction;

	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_EXPECT_TRUE(test, inactive_transaction(transaction));
	KUNIT_EXPECT_EQ(test, refcount_read(&transaction->ref_count), 1U);
	KUNIT_EXPECT_EQ(test, atomic_read(&transaction->task_count), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&transaction->finishing), 0);
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

/* Committing a transaction should call each entry's commit callback,
   release the entry's private state, and leave the workset empty. */
static void transaction_workset_commit_cleanup_test(struct kunit *test) {
	struct txobj_thread_list_node *node;
	struct transaction_workset_test_context context = { };
	struct transaction_object object;
	struct transaction *transaction;
	unsigned long original;

	transaction_object_init(&object, TRANSACTION_OBJECT_CUSTOM);
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	node = transaction_workset_node_alloc(
		&context, &original, &object, TRANSACTION_OBJECT_CUSTOM,
		TRANSACTION_ACCESS_READ_WRITE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, node);
	node->commit = transaction_workset_commit;
	node->abort = transaction_workset_abort;
	node->release = transaction_workset_release;
	KUNIT_ASSERT_EQ(test, transaction_workset_add(transaction, node), 0);

	KUNIT_EXPECT_EQ(test, end_transaction(transaction), 0);
	KUNIT_EXPECT_TRUE(test, inactive_transaction(transaction));
	KUNIT_EXPECT_TRUE(test, transaction_workset_empty(transaction));
	KUNIT_EXPECT_EQ(test, context.commit_count, 1);
	KUNIT_EXPECT_EQ(test, context.abort_count, 0);
	KUNIT_EXPECT_EQ(test, context.release_count, 1);
	transaction_put(transaction);
}

/* Aborting a transaction should call each entry's abort callback, release
   the entry's private state, and leave the workset empty. */
static void transaction_workset_abort_cleanup_test(struct kunit *test) {
	struct txobj_thread_list_node *node;
	struct transaction_workset_test_context context = { };
	struct transaction_object object;
	struct transaction *transaction;
	unsigned long original;

	transaction_object_init(&object, TRANSACTION_OBJECT_CUSTOM);
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	node = transaction_workset_node_alloc(
		&context, &original, &object, TRANSACTION_OBJECT_CUSTOM,
		TRANSACTION_ACCESS_READ_WRITE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, node);
	node->commit = transaction_workset_commit;
	node->abort = transaction_workset_abort;
	node->release = transaction_workset_release;
	KUNIT_ASSERT_EQ(test, transaction_workset_add(transaction, node), 0);
	KUNIT_ASSERT_EQ(test, abort_transaction(transaction), 0);

	KUNIT_EXPECT_EQ(test, end_transaction(transaction), -ECANCELED);
	KUNIT_EXPECT_TRUE(test, inactive_transaction(transaction));
	KUNIT_EXPECT_TRUE(test, transaction_workset_empty(transaction));
	KUNIT_EXPECT_EQ(test, context.commit_count, 0);
	KUNIT_EXPECT_EQ(test, context.abort_count, 1);
	KUNIT_EXPECT_EQ(test, context.release_count, 1);
	transaction_put(transaction);
}

static void transaction_list_add_commit_test(struct kunit *test) {
	struct transaction_list_test_item item;
	struct tx_list2_head head;
	struct transaction *transaction;

	INIT_TX_LIST2_HEAD(&head);
	transaction_list_test_item_init(&item, 1);
	KUNIT_EXPECT_EQ(test, transaction_list_test_count(&head), 0);

	KUNIT_ASSERT_EQ(test, transaction_test_begin_current(&transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_list2_add_tail(&item.link, &head), 0);
	KUNIT_EXPECT_EQ(test, transaction_list_test_count(&head), 1);
	KUNIT_EXPECT_FALSE(test, list_empty(&head.spec_list));
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, transaction_list_test_count(&head), 1);
	KUNIT_EXPECT_TRUE(test, list_empty(&head.spec_list));
	KUNIT_EXPECT_PTR_EQ(test, item.link.entry.parent, &head);
	KUNIT_EXPECT_TRUE(test, item.link.transaction == NULL);

	transaction_test_finish_current(transaction);
	tx_list2_del(&item.link);
}

static void transaction_list_add_abort_test(struct kunit *test) {
	struct transaction_list_test_item item;
	struct tx_list2_head head;
	struct transaction *transaction;

	INIT_TX_LIST2_HEAD(&head);
	transaction_list_test_item_init(&item, 1);

	KUNIT_ASSERT_EQ(test, transaction_test_begin_current(&transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_list2_add_tail(&item.link, &head), 0);
	KUNIT_EXPECT_EQ(test, transaction_list_test_count(&head), 1);
	KUNIT_EXPECT_EQ(test, abort_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), -ECANCELED);
	KUNIT_EXPECT_EQ(test, transaction_list_test_count(&head), 0);
	KUNIT_EXPECT_TRUE(test, list_empty(&head.spec_list));
	KUNIT_EXPECT_TRUE(test, tx_list2_unreferenced(&item.link));

	transaction_test_finish_current(transaction);
}

static void transaction_list_delete_commit_test(struct kunit *test) {
	struct transaction_list_test_item item;
	struct tx_list2_head head;
	struct transaction *transaction;

	INIT_TX_LIST2_HEAD(&head);
	transaction_list_test_item_init(&item, 1);
	KUNIT_ASSERT_EQ(test, tx_list2_add_tail(&item.link, &head), 0);
	KUNIT_EXPECT_EQ(test, transaction_list_test_count(&head), 1);

	KUNIT_ASSERT_EQ(test, transaction_test_begin_current(&transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_list2_del(&item.link), 0);
	KUNIT_EXPECT_EQ(test, transaction_list_test_count(&head), 0);
	KUNIT_EXPECT_FALSE(test, list_empty(&head.spec_list));
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, transaction_list_test_count(&head), 0);
	KUNIT_EXPECT_TRUE(test, list_empty(&head.spec_list));
	KUNIT_EXPECT_TRUE(test, tx_list2_unreferenced(&item.link));

	transaction_test_finish_current(transaction);
}

static void transaction_list_delete_abort_test(struct kunit *test) {
	struct transaction_list_test_item item;
	struct tx_list2_head head;
	struct transaction *transaction;

	INIT_TX_LIST2_HEAD(&head);
	transaction_list_test_item_init(&item, 1);
	KUNIT_ASSERT_EQ(test, tx_list2_add_tail(&item.link, &head), 0);

	KUNIT_ASSERT_EQ(test, transaction_test_begin_current(&transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_list2_del(&item.link), 0);
	KUNIT_EXPECT_EQ(test, transaction_list_test_count(&head), 0);
	KUNIT_EXPECT_EQ(test, abort_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), -ECANCELED);
	KUNIT_EXPECT_EQ(test, transaction_list_test_count(&head), 1);
	KUNIT_EXPECT_TRUE(test, list_empty(&head.spec_list));
	KUNIT_EXPECT_PTR_EQ(test, item.link.entry.parent, &head);

	transaction_test_finish_current(transaction);
	tx_list2_del(&item.link);
}

static void transaction_list_move_commit_test(struct kunit *test) {
	struct transaction_list_test_item item;
	struct tx_list2_head first;
	struct tx_list2_head second;
	struct transaction *transaction;

	INIT_TX_LIST2_HEAD(&first);
	INIT_TX_LIST2_HEAD(&second);
	transaction_list_test_item_init(&item, 1);
	KUNIT_ASSERT_EQ(test, tx_list2_add_tail(&item.link, &first), 0);

	KUNIT_ASSERT_EQ(test, transaction_test_begin_current(&transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_list2_move(&item.link, &second), 0);
	KUNIT_EXPECT_EQ(test, transaction_list_test_count(&first), 0);
	KUNIT_EXPECT_EQ(test, transaction_list_test_count(&second), 1);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, transaction_list_test_count(&first), 0);
	KUNIT_EXPECT_EQ(test, transaction_list_test_count(&second), 1);
	KUNIT_EXPECT_PTR_EQ(test, item.link.entry.parent, &second);

	transaction_test_finish_current(transaction);
	tx_list2_del(&item.link);
}

/* Aborting a transaction discards the transaction-local file offset. */
static void transaction_file_offset_abort_test(struct kunit *test) {
	struct transaction *transaction;
	struct file *file;

	file = anon_inode_getfile("[transaction-test]",
				  &transaction_test_file_operations, NULL, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	file->f_pos = 17;
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, current), 0);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	KUNIT_ASSERT_EQ(test, transaction_file_snapshot(file), 0);
	KUNIT_EXPECT_PTR_EQ(test, file->transaction_object.writer, transaction);
	KUNIT_EXPECT_FALSE(test, list_empty(&file->transaction_object.readers));
	KUNIT_ASSERT_EQ(test, transaction_file_set_pos(file, 29), 0);
	KUNIT_ASSERT_EQ(test, transaction_file_snapshot(file), 0);
	KUNIT_EXPECT_EQ(test, transaction_file_get_pos(file), 29);
	KUNIT_ASSERT_EQ(test, transaction_file_set_pos(file, 41), 0);
	KUNIT_EXPECT_EQ(test, transaction_file_get_pos(file), 41);
	KUNIT_EXPECT_EQ(test, abort_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, transaction_file_snapshot(file), -ECANCELED);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), -ECANCELED);
	KUNIT_EXPECT_EQ(test, file->f_pos, 17);
	KUNIT_EXPECT_PTR_EQ(test, file->transaction_object.writer, NULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&file->transaction_object.readers));
	transaction_detach_task(current);
	transaction_put(transaction);
	fput(file);
}

/* Committing a transaction publishes the transaction-local file offset. */
static void transaction_file_offset_commit_test(struct kunit *test) {
	struct transaction *transaction;
	struct file *file;

	file = anon_inode_getfile("[transaction-test]",
				  &transaction_test_file_operations, NULL, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	file->f_pos = 17;
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, current), 0);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	KUNIT_ASSERT_EQ(test, transaction_file_snapshot(file), 0);
	KUNIT_EXPECT_PTR_EQ(test, file->transaction_object.writer, transaction);
	KUNIT_EXPECT_FALSE(test, list_empty(&file->transaction_object.readers));
	KUNIT_ASSERT_EQ(test, transaction_file_set_pos(file, 41), 0);
	KUNIT_EXPECT_EQ(test, file->f_pos, 17);
	KUNIT_EXPECT_EQ(test, transaction_file_get_pos(file), 41);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, file->f_pos, 41);
	KUNIT_EXPECT_PTR_EQ(test, file->transaction_object.writer, NULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&file->transaction_object.readers));
	transaction_detach_task(current);
	transaction_put(transaction);
	fput(file);
}

static void transaction_inode_metadata_abort_test(struct kunit *test) {
	struct _inode *shadow_inode;
	struct transaction *transaction;
	struct inode *inode;
	struct file *file;

	file = anon_inode_create_getfile("[transaction-inode-test]",
					 &transaction_test_file_operations,
					 NULL, 0, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	inode = file_inode(file);
	inode->i_mode = S_IFREG | 0600;
	i_uid_write(inode, 1000);
	i_gid_write(inode, 1000);
	inode_set_atime(inode, 1, 2);
	inode_set_mtime(inode, 3, 4);
	inode_set_ctime(inode, 5, 6);
	inode_set_iversion(inode, 5);

	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, current), 0);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	KUNIT_ASSERT_EQ(test, transaction_inode_snapshot(inode), 0);
	shadow_inode = transaction_inode_visible(inode);
	KUNIT_ASSERT_NOT_NULL(test, shadow_inode);
	KUNIT_EXPECT_PTR_NE(test, shadow_inode, rcu_access_pointer(inode->i_contents));
	KUNIT_EXPECT_PTR_EQ(test, shadow_inode->shadow, rcu_access_pointer(inode->i_contents));
	KUNIT_EXPECT_PTR_EQ(test, inode->transaction_object.writer, transaction);
	KUNIT_EXPECT_FALSE(test, list_empty(&inode->transaction_object.readers));
	shadow_inode->i_mode = S_IFREG | 0644;
	i_uid_write(inode, 2000);
	i_gid_write(inode, 2000);
	set_nlink(inode, 3);
	i_size_write(inode, 42);
	inode_set_flags(inode, S_DEAD, S_DEAD);
	inode_set_atime(inode, 7, 8);
	inode_set_mtime(inode, 9, 10);
	inode_set_ctime(inode, 11, 12);
	inode_inc_iversion(inode);
	KUNIT_EXPECT_EQ(test, inode->i_mode, S_IFREG | 0600);
	KUNIT_EXPECT_EQ(test, inode->i_nlink, 1U);
	KUNIT_EXPECT_EQ(test, inode_get_nlink(inode), 3U);
	KUNIT_EXPECT_EQ(test, inode->i_size, 0LL);
	KUNIT_EXPECT_EQ(test, i_size_read(inode), 42LL);
	KUNIT_EXPECT_EQ(test, inode->i_flags & S_DEAD, 0U);
	KUNIT_EXPECT_EQ(test, inode_get_flags(inode) & S_DEAD, S_DEAD);
	KUNIT_EXPECT_EQ(test, i_uid_read(inode), 2000U);
	KUNIT_EXPECT_EQ(test, i_gid_read(inode), 2000U);
	KUNIT_EXPECT_EQ(test, inode_get_atime_sec(inode), 7LL);
	KUNIT_EXPECT_EQ(test, inode_get_mtime_sec(inode), 9LL);
	KUNIT_EXPECT_EQ(test, inode_get_ctime_sec(inode), 11LL);
	KUNIT_EXPECT_EQ(test, inode_peek_iversion_raw(inode), 12ULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&inode->i_version), 10LL);
	KUNIT_EXPECT_EQ(test, abort_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), -ECANCELED);
	KUNIT_EXPECT_EQ(test, inode->i_mode, S_IFREG | 0600);
	KUNIT_EXPECT_EQ(test, inode->i_nlink, 1U);
	KUNIT_EXPECT_EQ(test, inode->i_size, 0LL);
	KUNIT_EXPECT_EQ(test, inode->i_flags & S_DEAD, 0U);
	KUNIT_EXPECT_EQ(test, i_uid_read(inode), 1000U);
	KUNIT_EXPECT_EQ(test, i_gid_read(inode), 1000U);
	KUNIT_EXPECT_EQ(test, inode_get_atime_sec(inode), 1LL);
	KUNIT_EXPECT_EQ(test, inode_get_atime_nsec(inode), 2L);
	KUNIT_EXPECT_EQ(test, inode_get_mtime_sec(inode), 3LL);
	KUNIT_EXPECT_EQ(test, inode_get_mtime_nsec(inode), 4L);
	KUNIT_EXPECT_EQ(test, inode_get_ctime_sec(inode), 5LL);
	KUNIT_EXPECT_EQ(test, inode_get_ctime_nsec(inode), 6L);
	KUNIT_EXPECT_EQ(test, inode_peek_iversion_raw(inode), 10ULL);
	KUNIT_EXPECT_PTR_EQ(test, inode->transaction_object.writer, NULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&inode->transaction_object.readers));
	transaction_detach_task(current);
	transaction_put(transaction);
	fput(file);
}

static void transaction_inode_metadata_commit_test(struct kunit *test) {
	struct _inode *shadow_inode;
	struct transaction *transaction;
	struct inode *inode;
	struct file *file;

	file = anon_inode_create_getfile("[transaction-inode-test]",
					 &transaction_test_file_operations,
					 NULL, 0, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	inode = file_inode(file);
	inode->i_mode = S_IFREG | 0600;
	inode_set_iversion(inode, 5);
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, current), 0);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	KUNIT_ASSERT_EQ(test, transaction_inode_snapshot(inode), 0);
	shadow_inode = transaction_inode_visible(inode);
	KUNIT_ASSERT_NOT_NULL(test, shadow_inode);
	KUNIT_EXPECT_PTR_EQ(test, shadow_inode->shadow, rcu_access_pointer(inode->i_contents));
	KUNIT_EXPECT_PTR_EQ(test, inode->transaction_object.writer, transaction);
	KUNIT_EXPECT_FALSE(test, list_empty(&inode->transaction_object.readers));
	shadow_inode->i_mode = S_IFREG | 0644;
	set_nlink(inode, 3);
	i_size_write(inode, 42);
	inode_set_flags(inode, S_DEAD, S_DEAD);
	inode_inc_iversion(inode);
	KUNIT_EXPECT_EQ(test, inode->i_mode, S_IFREG | 0600);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, inode->i_mode, S_IFREG | 0644);
	KUNIT_EXPECT_EQ(test, inode->i_nlink, 3U);
	KUNIT_EXPECT_EQ(test, inode->i_size, 42LL);
	KUNIT_EXPECT_EQ(test, inode->i_flags & S_DEAD, S_DEAD);
	KUNIT_EXPECT_EQ(test, inode_peek_iversion_raw(inode), 12ULL);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(inode->i_contents), shadow_inode);
	KUNIT_EXPECT_PTR_EQ(test, shadow_inode->shadow, NULL);
	KUNIT_EXPECT_PTR_EQ(test, inode->transaction_object.writer, NULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&inode->transaction_object.readers));
	transaction_detach_task(current);
	transaction_put(transaction);
	fput(file);
}

static void transaction_inode_read_version_test(struct kunit *test) {
	struct _inode *contents;
	struct transaction *transaction;
	struct inode *inode;
	struct file *file;

	file = anon_inode_create_getfile("[transaction-inode-test]",
					 &transaction_test_file_operations,
					 NULL, 0, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	inode = file_inode(file);
	inode->i_mode = S_IFREG | 0600;
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, current), 0);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	contents = transaction_inode_get(inode, TRANSACTION_ACCESS_READ);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, contents);
	KUNIT_EXPECT_PTR_EQ(test, contents, rcu_access_pointer(inode->i_contents));
	KUNIT_EXPECT_PTR_EQ(test, contents->shadow, NULL);
	KUNIT_EXPECT_PTR_EQ(test, transaction_inode_visible(inode), contents);
	KUNIT_EXPECT_PTR_EQ(test, inode->transaction_object.writer, NULL);
	KUNIT_EXPECT_FALSE(test, list_empty(&inode->transaction_object.readers));
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), 0);
	KUNIT_EXPECT_TRUE(test, list_empty(&inode->transaction_object.readers));
	transaction_detach_task(current);
	transaction_put(transaction);
	fput(file);
}

static void transaction_inode_refresh_committed_test(struct kunit *test) {
	struct _inode *shadow_inode;
	struct transaction *transaction;
	struct inode *inode;
	struct file *file;

	file = anon_inode_create_getfile("[transaction-inode-test]",
					 &transaction_test_file_operations,
					 NULL, 0, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	inode = file_inode(file);
	inode->i_mode = S_IFREG | 0600;
	i_uid_write(inode, 1000);

	KUNIT_ASSERT_EQ(test, transaction_test_begin_current(&transaction), 0);
	KUNIT_ASSERT_EQ(test, transaction_inode_snapshot(inode), 0);
	shadow_inode = transaction_inode_visible(inode);
	KUNIT_ASSERT_NOT_NULL(test, shadow_inode);
	shadow_inode->i_mode = S_IFREG | 0644;
	KUNIT_ASSERT_EQ(test, end_transaction(transaction), 0);
	transaction_test_finish_current(transaction);

	/* Ordinary updates after a commit become the next committed baseline. */
	inode->i_mode = S_IFREG | 0600;
	i_uid_write(inode, 2000);
	KUNIT_ASSERT_EQ(test, transaction_test_begin_current(&transaction), 0);
	KUNIT_ASSERT_EQ(test, transaction_inode_snapshot(inode), 0);
	shadow_inode = transaction_inode_visible(inode);
	KUNIT_ASSERT_NOT_NULL(test, shadow_inode);
	KUNIT_EXPECT_EQ(test, shadow_inode->i_mode, S_IFREG | 0600);
	KUNIT_EXPECT_EQ(test, from_kuid(i_user_ns(inode), shadow_inode->i_uid), 2000U);
	KUNIT_ASSERT_EQ(test, end_transaction(transaction), 0);
	transaction_test_finish_current(transaction);
	KUNIT_EXPECT_EQ(test, inode->i_mode, S_IFREG | 0600);
	KUNIT_EXPECT_EQ(test, i_uid_read(inode), 2000U);
	fput(file);
}

static void transaction_dentry_metadata_abort_test(struct kunit *test) {
	struct transaction *transaction;
	struct dentry *dentry;
	struct file *file;
	unsigned int original_flags;
	unsigned long original_time;
	void *original_fsdata;

	file = anon_inode_getfile("[transaction-dentry-test]",
				  &transaction_test_file_operations, NULL, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	dentry = file->f_path.dentry;
	KUNIT_ASSERT_NOT_NULL(test, dentry);

	original_flags = dentry->d_flags;
	original_time = dentry->d_time;
	original_fsdata = dentry->d_fsdata;
	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, current), 0);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	KUNIT_ASSERT_EQ(test, transaction_dentry_snapshot(dentry), 0);
	KUNIT_EXPECT_PTR_EQ(test, dentry->transaction_object.writer,
			    transaction);
	KUNIT_EXPECT_FALSE(test,
			   list_empty(&dentry->transaction_object.readers));

	spin_lock(&dentry->d_lock);
	dentry->d_flags = original_flags ^ DCACHE_REFERENCED;
	dentry->d_time = original_time + 10;
	dentry->d_fsdata = test;
	spin_unlock(&dentry->d_lock);

	KUNIT_EXPECT_EQ(test, abort_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), -ECANCELED);
	KUNIT_EXPECT_EQ(test, dentry->d_flags, original_flags);
	KUNIT_EXPECT_EQ(test, dentry->d_time, original_time);
	KUNIT_EXPECT_PTR_EQ(test, dentry->d_fsdata, original_fsdata);
	KUNIT_EXPECT_PTR_EQ(test, dentry->transaction_object.writer, NULL);
	KUNIT_EXPECT_TRUE(test,
			  list_empty(&dentry->transaction_object.readers));
	transaction_detach_task(current);
	transaction_put(transaction);
	fput(file);
}

static void transaction_dentry_metadata_commit_test(struct kunit *test) {
	struct transaction *transaction;
	struct dentry *dentry;
	struct file *file;
	unsigned int updated_flags;
	unsigned long updated_time;

	file = anon_inode_getfile("[transaction-dentry-test]",
				  &transaction_test_file_operations, NULL, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	dentry = file->f_path.dentry;
	KUNIT_ASSERT_NOT_NULL(test, dentry);

	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, current), 0);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	KUNIT_ASSERT_EQ(test, transaction_dentry_snapshot(dentry), 0);
	KUNIT_EXPECT_PTR_EQ(test, dentry->transaction_object.writer,
			    transaction);
	KUNIT_EXPECT_FALSE(test,
			   list_empty(&dentry->transaction_object.readers));

	updated_flags = dentry->d_flags ^ DCACHE_REFERENCED;
	updated_time = dentry->d_time + 10;
	spin_lock(&dentry->d_lock);
	dentry->d_flags = updated_flags;
	dentry->d_time = updated_time;
	dentry->d_fsdata = test;
	spin_unlock(&dentry->d_lock);

	KUNIT_EXPECT_EQ(test, end_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, dentry->d_flags, updated_flags);
	KUNIT_EXPECT_EQ(test, dentry->d_time, updated_time);
	KUNIT_EXPECT_PTR_EQ(test, dentry->d_fsdata, test);
	KUNIT_EXPECT_PTR_EQ(test, dentry->transaction_object.writer, NULL);
	KUNIT_EXPECT_TRUE(test,
			  list_empty(&dentry->transaction_object.readers));
	transaction_detach_task(current);
	transaction_put(transaction);
	fput(file);
}

static void transaction_hlist_restore_test(struct kunit *test) {
	struct tx_hlist_node_snapshot snapshot;
	struct hlist_head head;
	struct hlist_node first;
	struct hlist_node second;

	INIT_HLIST_HEAD(&head);
	INIT_HLIST_NODE(&first);
	INIT_HLIST_NODE(&second);
	hlist_add_head(&first, &head);
	hlist_add_head(&second, &head);

	tx_hlist_snapshot(&second, &snapshot);
	hlist_del_init(&second);
	KUNIT_EXPECT_PTR_EQ(test, head.first, &first);

	tx_hlist_restore(&second, &snapshot);
	KUNIT_EXPECT_PTR_EQ(test, head.first, &second);
	KUNIT_EXPECT_PTR_EQ(test, second.next, &first);
	KUNIT_EXPECT_PTR_EQ(test, first.pprev, &second.next);
}

static void transaction_hlist_bl_restore_test(struct kunit *test) {
	struct tx_hlist_bl_node_snapshot snapshot;
	struct hlist_bl_head head;
	struct hlist_bl_node first;
	struct hlist_bl_node second;

	INIT_HLIST_BL_HEAD(&head);
	INIT_HLIST_BL_NODE(&first);
	INIT_HLIST_BL_NODE(&second);
	hlist_bl_lock(&head);
	hlist_bl_add_head(&first, &head);
	hlist_bl_add_head(&second, &head);

	tx_hlist_bl_snapshot(&second, &snapshot);
	hlist_bl_del_init(&second);
	KUNIT_EXPECT_PTR_EQ(test, hlist_bl_first(&head), &first);

	tx_hlist_bl_restore(&second, &snapshot);
	KUNIT_EXPECT_PTR_EQ(test, hlist_bl_first(&head), &second);
	KUNIT_EXPECT_PTR_EQ(test, second.next, &first);
	KUNIT_EXPECT_PTR_EQ(test, first.pprev, &second.next);
	hlist_bl_unlock(&head);
}

static void transaction_dentry_sibling_abort_test(struct kunit *test) {
	struct transaction *transaction;
	struct dentry *parent;
	struct dentry *child;
	struct file *file;
	struct qstr name = QSTR_INIT("txchild", 7);

	file = anon_inode_getfile("[transaction-dcache-test]",
				  &transaction_test_file_operations, NULL, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	parent = file->f_path.dentry;
	KUNIT_ASSERT_NOT_NULL(test, parent);

	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, current), 0);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	child = d_alloc(parent, &name);
	KUNIT_ASSERT_NOT_NULL(test, child);
	KUNIT_EXPECT_FALSE(test, hlist_unhashed(&child->d_sib));

	KUNIT_EXPECT_EQ(test, abort_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), -ECANCELED);
	KUNIT_EXPECT_TRUE(test, hlist_unhashed(&child->d_sib));
	KUNIT_EXPECT_PTR_EQ(test, child->d_parent, child);

	transaction_detach_task(current);
	transaction_put(transaction);
	dput(child);
	fput(file);
}

static void transaction_dentry_hash_abort_test(struct kunit *test) {
	struct transaction *transaction;
	struct dentry *parent;
	struct dentry *child;
	struct file *file;
	struct qstr name = QSTR_INIT("txhash", 6);

	file = anon_inode_getfile("[transaction-dcache-test]",
				  &transaction_test_file_operations, NULL, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	parent = file->f_path.dentry;
	KUNIT_ASSERT_NOT_NULL(test, parent);
	child = d_alloc(parent, &name);
	KUNIT_ASSERT_NOT_NULL(test, child);
	KUNIT_ASSERT_TRUE(test, d_unhashed(child));

	transaction = transaction_alloc(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transaction);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, current), 0);
	KUNIT_ASSERT_EQ(test, begin_transaction(transaction), 0);
	d_rehash(child);
	KUNIT_EXPECT_FALSE(test, d_unhashed(child));

	KUNIT_EXPECT_EQ(test, abort_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), -ECANCELED);
	KUNIT_EXPECT_TRUE(test, d_unhashed(child));

	transaction_detach_task(current);
	transaction_put(transaction);
	dput(child);
	fput(file);
}

static void transaction_syscall_commit_test(struct kunit *test) {
	KUNIT_ASSERT_PTR_EQ(test, current_transaction(), NULL);
	KUNIT_ASSERT_EQ(test, transaction_sys_xbegin(), 0L);
	KUNIT_EXPECT_TRUE(test, live_transaction(current_transaction()));
	KUNIT_EXPECT_EQ(test, transaction_sys_xbegin(), (long)-EALREADY);
	KUNIT_EXPECT_EQ(test, transaction_sys_xend(), 0L);
	KUNIT_EXPECT_PTR_EQ(test, current_transaction(), NULL);
	KUNIT_EXPECT_EQ(test, transaction_sys_xend(), (long)-EINVAL);
}

static void transaction_syscall_abort_test(struct kunit *test) {
	KUNIT_ASSERT_PTR_EQ(test, current_transaction(), NULL);
	KUNIT_ASSERT_EQ(test, transaction_sys_xbegin(), 0L);
	KUNIT_EXPECT_EQ(test, transaction_sys_xabort(), 0L);
	KUNIT_EXPECT_PTR_EQ(test, current_transaction(), NULL);
	KUNIT_EXPECT_EQ(test, transaction_sys_xabort(), (long)-EINVAL);
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
	KUNIT_CASE(transaction_workset_commit_cleanup_test),
	KUNIT_CASE(transaction_workset_abort_cleanup_test),
	KUNIT_CASE(transaction_list_add_commit_test),
	KUNIT_CASE(transaction_list_add_abort_test),
	KUNIT_CASE(transaction_list_delete_commit_test),
	KUNIT_CASE(transaction_list_delete_abort_test),
	KUNIT_CASE(transaction_list_move_commit_test),
	KUNIT_CASE(transaction_file_offset_abort_test),
	KUNIT_CASE(transaction_file_offset_commit_test),
	KUNIT_CASE(transaction_inode_metadata_abort_test),
	KUNIT_CASE(transaction_inode_metadata_commit_test),
	KUNIT_CASE(transaction_inode_read_version_test),
	KUNIT_CASE(transaction_inode_refresh_committed_test),
	KUNIT_CASE(transaction_dentry_metadata_abort_test),
	KUNIT_CASE(transaction_dentry_metadata_commit_test),
	KUNIT_CASE(transaction_hlist_restore_test),
	KUNIT_CASE(transaction_hlist_bl_restore_test),
	KUNIT_CASE(transaction_dentry_sibling_abort_test),
	KUNIT_CASE(transaction_dentry_hash_abort_test),
	KUNIT_CASE(transaction_syscall_commit_test),
	KUNIT_CASE(transaction_syscall_abort_test),
	KUNIT_CASE(transaction_live_fork_test),
	KUNIT_CASE(transaction_commit_abort_race_test),
	KUNIT_CASE(transaction_contention_priority_test),
	KUNIT_CASE(transaction_contention_timestamp_test),
	KUNIT_CASE(transaction_contention_aborted_test),
	KUNIT_CASE(transaction_contention_committing_test),
	KUNIT_CASE(transaction_object_readers_test),
	KUNIT_CASE(transaction_object_reader_wins_test),
	KUNIT_CASE(transaction_object_writer_wins_test),
	KUNIT_CASE(transaction_object_writer_conflict_test),
	KUNIT_CASE(transaction_file_owner_displaced_test),
	KUNIT_CASE(transaction_object_upgrade_test),
	KUNIT_CASE(transaction_object_upgrade_loses_test),
	KUNIT_CASE(transaction_object_reuse_test),
	KUNIT_CASE(transaction_finish_order_test),
	KUNIT_CASE(transaction_abort_before_final_commit_test),
	// TODO: Re-enable when transaction_finish_workset() runs optional validation callbacks.
	// KUNIT_CASE(transaction_validation_failure_test),
	{}
};

static struct kunit_suite transaction_test_suite = {
	.name = "transactions",
	.test_cases = transaction_test_cases,
};

kunit_test_suite(transaction_test_suite);

MODULE_DESCRIPTION("System transactions lifecycle tests");
MODULE_LICENSE("GPL");
