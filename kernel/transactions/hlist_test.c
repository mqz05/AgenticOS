// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/preempt.h>
#include <linux/seqlock.h>
#include <linux/transaction.h>
#include <linux/tx_hlist.h>

struct tx_hlist_test_entry {
	int value;
	struct tx_hlist_entry_ref link;
};

struct tx_hlist_bl_test_entry {
	int value;
	struct tx_hlist_bl_entry_ref link;
};

struct tx_hlist_lifetime_entry {
	atomic_t references;
	struct tx_hlist_entry_ref link;
};

struct tx_hlist_callback_context {
	int gets;
	int puts;
	int locks;
	int unlocks;
};

struct tx_hlist_publish_entry {
	int begins;
	int ends;
	struct tx_hlist_bl_entry_ref link;
};

struct tx_hlist_rcu_context {
	struct completion started;
	seqcount_t sequence;
	atomic_t errors;
	atomic_t samples;
	wait_queue_head_t sampled;
	struct tx_hlist_bl_head *first;
	struct tx_hlist_bl_head *second;
};

struct tx_hlist_rcu_test_state {
	struct tx_hlist_rcu_context context;
	struct tx_hlist_publish_entry entry;
	struct tx_hlist_bl_head first;
	struct tx_hlist_bl_head second;
};

static void tx_hlist_head_get(void *owner) {
	((struct tx_hlist_callback_context *)owner)->gets++;
}

static void tx_hlist_head_put(void *owner) {
	((struct tx_hlist_callback_context *)owner)->puts++;
}

static void tx_hlist_head_lock(void *owner) {
	((struct tx_hlist_callback_context *)owner)->locks++;
}

static void tx_hlist_head_unlock(void *owner) {
	((struct tx_hlist_callback_context *)owner)->unlocks++;
}

static void tx_hlist_publish_begin(void *owner) {
	struct tx_hlist_bl_entry_ref *ref = owner;
	struct tx_hlist_publish_entry *entry = container_of(ref, struct tx_hlist_publish_entry, link);

	entry->begins++;
}

static void tx_hlist_publish_end(void *owner) {
	struct tx_hlist_bl_entry_ref *ref = owner;
	struct tx_hlist_publish_entry *entry = container_of(ref, struct tx_hlist_publish_entry, link);

	entry->ends++;
}

static void tx_hlist_rcu_write_begin(void *owner) {
	struct tx_hlist_rcu_context *context = owner;

	preempt_disable();
	write_seqcount_begin(&context->sequence);
}

static void tx_hlist_rcu_write_end(void *owner) {
	struct tx_hlist_rcu_context *context = owner;

	write_seqcount_end(&context->sequence);
	preempt_enable();
}

static int tx_hlist_rcu_reader(void *data) {
	struct tx_hlist_rcu_context *context = data;

	complete(&context->started);
	while (!kthread_should_stop()) {
		struct tx_hlist_bl_entry_ref *ref;
		struct hlist_bl_node *node;
		unsigned int sequence;
		int count;

		do {
			sequence = read_seqcount_begin(&context->sequence);
			count = 0;
			rcu_read_lock();
			hlist_bl_for_each_entry_rcu(ref, node, &context->first->head, node)
				count++;
			hlist_bl_for_each_entry_rcu(ref, node, &context->second->head, node)
				count++;
			rcu_read_unlock();
		} while (read_seqcount_retry(&context->sequence, sequence));
		if (count != 1)
			atomic_inc(&context->errors);
		atomic_inc(&context->samples);
		wake_up(&context->sampled);
		cond_resched();
	}
	return 0;
}

static void tx_hlist_stop_reader(void *data) {
	kthread_stop(data);
}

static void tx_hlist_rcu_state_cleanup(void *data) {
	struct tx_hlist_rcu_test_state *state = data;

	if (!tx_hlist_bl_unreferenced(&state->entry.link))
		WARN_ON(tx_hlist_bl_del(&state->entry.link));
	WARN_ON(tx_hlist_bl_head_destroy(&state->first));
	WARN_ON(tx_hlist_bl_head_destroy(&state->second));
}

static void tx_hlist_lifetime_get(void *owner) {
	struct tx_hlist_entry_ref *ref = owner;
	struct tx_hlist_lifetime_entry *entry;

	entry = container_of(ref, struct tx_hlist_lifetime_entry, link);
	atomic_inc(&entry->references);
}

static void tx_hlist_lifetime_put(void *owner) {
	struct tx_hlist_entry_ref *ref = owner;
	struct tx_hlist_lifetime_entry *entry;

	entry = container_of(ref, struct tx_hlist_lifetime_entry, link);
	atomic_dec(&entry->references);
}

static void tx_hlist_test_transaction_cleanup(void *data) {
	struct transaction *transaction = data;
	enum transaction_state status = transaction_status(transaction);

	if (status == TRANSACTION_ACTIVE) {
		abort_transaction(transaction);
		end_transaction(transaction);
	} else if (status == TRANSACTION_ABORTED) {
		end_transaction(transaction);
	}
	if (current_transaction() == transaction)
		transaction_detach_task(current);
	transaction_put(transaction);
}

static int tx_hlist_test_begin(struct kunit *test, struct transaction **result) {
	struct transaction *transaction;
	int ret;

	transaction = transaction_alloc(GFP_KERNEL);
	if (!transaction)
		return -ENOMEM;
	ret = transaction_attach_task(transaction, current);
	if (ret)
		goto put;
	ret = begin_transaction(transaction);
	if (ret)
		goto detach;
	ret = kunit_add_action_or_reset(test, tx_hlist_test_transaction_cleanup, transaction);
	if (ret)
		return ret;
	*result = transaction;
	return 0;

detach:
	transaction_detach_task(current);
put:
	transaction_put(transaction);
	return ret;
}

static int tx_hlist_test_commit(struct transaction *transaction) {
	int ret = end_transaction(transaction);

	if (ret)
		return ret;
	transaction_detach_task(current);
	return 0;
}

static int tx_hlist_test_abort(struct transaction *transaction) {
	int ret = abort_transaction(transaction);

	if (ret)
		return ret;
	ret = end_transaction(transaction);
	if (ret != -ECANCELED)
		return ret ? ret : -EUCLEAN;
	transaction_detach_task(current);
	return 0;
}

static int tx_hlist_count(struct kunit *test, struct tx_hlist_head *head) {
	struct tx_hlist_iterator iter;
	int count = 0;
	int ret;

	ret = tx_hlist_get_iterator(&iter, head);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		return ret;
	while (tx_hlist_iter_next(&iter))
		count++;
	tx_hlist_put_iterator(&iter);
	return count;
}

static int tx_hlist_bl_count(struct kunit *test, struct tx_hlist_bl_head *head) {
	struct tx_hlist_bl_iterator iter;
	int count = 0;
	int ret;

	ret = tx_hlist_bl_get_iterator(&iter, head);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		return ret;
	while (tx_hlist_bl_iter_next(&iter))
		count++;
	tx_hlist_bl_put_iterator(&iter);
	return count;
}

static void tx_hlist_init_test(struct kunit *test) {
	struct tx_hlist_test_entry entry;
	struct tx_hlist_head head;

	tx_hlist_head_init(&head, NULL);
	tx_hlist_entry_init(&entry.link);
	KUNIT_EXPECT_TRUE(test, hlist_empty(&head.head));
	KUNIT_EXPECT_TRUE(test, tx_hlist_unreferenced(&entry.link));
	KUNIT_EXPECT_EQ(test, head.state.mode, TX_HLIST_NO_TX);
	KUNIT_EXPECT_EQ(test, sizeof(struct tx_hlist_bl_head), sizeof(struct hlist_bl_head));
}

static void tx_hlist_nontransactional_test(struct kunit *test) {
	struct tx_hlist_test_entry first = { .value = 1 };
	struct tx_hlist_test_entry second = { .value = 2 };
	struct tx_hlist_iterator iter;
	struct tx_hlist_test_entry *entry;
	struct tx_hlist_head head;

	tx_hlist_head_init(&head, NULL);
	tx_hlist_entry_init(&first.link);
	tx_hlist_entry_init(&second.link);
	KUNIT_ASSERT_EQ(test, tx_hlist_add_head(&first.link, &head), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_add_head(&second.link, &head), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_get_iterator(&iter, &head), 0);
	KUNIT_ASSERT_TRUE(test, tx_hlist_iter_next(&iter));
	entry = tx_hlist_iter_entry(&iter, struct tx_hlist_test_entry, link);
	KUNIT_EXPECT_EQ(test, entry->value, 2);
	KUNIT_ASSERT_TRUE(test, tx_hlist_iter_next(&iter));
	entry = tx_hlist_iter_entry(&iter, struct tx_hlist_test_entry, link);
	KUNIT_EXPECT_EQ(test, entry->value, 1);
	KUNIT_EXPECT_FALSE(test, tx_hlist_iter_next(&iter));
	tx_hlist_put_iterator(&iter);
	KUNIT_ASSERT_EQ(test, tx_hlist_del(&second.link), 0);
	KUNIT_EXPECT_TRUE(test, tx_hlist_unreferenced(&second.link));
	KUNIT_EXPECT_EQ(test, tx_hlist_count(test, &head), 1);
}

static void tx_hlist_add_commit_test(struct kunit *test) {
	struct tx_hlist_test_entry first = { .value = 1 };
	struct tx_hlist_test_entry second = { .value = 2 };
	struct transaction *transaction;
	struct tx_hlist_head head;

	tx_hlist_head_init(&head, NULL);
	tx_hlist_entry_init(&first.link);
	tx_hlist_entry_init(&second.link);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_add_head(&first.link, &head), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_add_head(&second.link, &head), 0);
	KUNIT_EXPECT_TRUE(test, hlist_empty(&head.head));
	KUNIT_EXPECT_EQ(test, tx_hlist_count(test, &head), 2);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_commit(transaction), 0);
	KUNIT_EXPECT_PTR_EQ(test, head.head.first, &second.link.node);
	KUNIT_EXPECT_PTR_EQ(test, second.link.node.next, &first.link.node);
	KUNIT_EXPECT_TRUE(test, first.link.state.transaction == NULL);
	KUNIT_EXPECT_TRUE(test, second.link.state.transaction == NULL);
}

static void tx_hlist_add_abort_test(struct kunit *test) {
	struct tx_hlist_test_entry entry = { .value = 1 };
	struct transaction *transaction;
	struct tx_hlist_head head;

	tx_hlist_head_init(&head, NULL);
	tx_hlist_entry_init(&entry.link);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_add_head(&entry.link, &head), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_count(test, &head), 1);
	transaction_detach_task(current);
	KUNIT_EXPECT_EQ(test, tx_hlist_count(test, &head), 0);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, current), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_abort(transaction), 0);
	KUNIT_EXPECT_TRUE(test, hlist_empty(&head.head));
	KUNIT_EXPECT_TRUE(test, tx_hlist_unreferenced(&entry.link));
}

static void tx_hlist_delete_commit_test(struct kunit *test) {
	struct tx_hlist_test_entry entry = { .value = 1 };
	struct transaction *transaction;
	struct tx_hlist_head head;

	tx_hlist_head_init(&head, NULL);
	tx_hlist_entry_init(&entry.link);
	KUNIT_ASSERT_EQ(test, tx_hlist_add_head(&entry.link, &head), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_del(&entry.link), 0);
	KUNIT_EXPECT_FALSE(test, hlist_empty(&head.head));
	KUNIT_EXPECT_EQ(test, tx_hlist_count(test, &head), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_commit(transaction), 0);
	KUNIT_EXPECT_TRUE(test, hlist_empty(&head.head));
	KUNIT_EXPECT_TRUE(test, tx_hlist_unreferenced(&entry.link));
}

static void tx_hlist_add_delete_test(struct kunit *test) {
	struct tx_hlist_test_entry entry = { .value = 1 };
	struct transaction *transaction;
	struct tx_hlist_head head;

	tx_hlist_head_init(&head, NULL);
	tx_hlist_entry_init(&entry.link);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_add_head(&entry.link, &head), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_del(&entry.link), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_count(test, &head), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_commit(transaction), 0);
	KUNIT_EXPECT_TRUE(test, hlist_empty(&head.head));
	KUNIT_EXPECT_TRUE(test, tx_hlist_unreferenced(&entry.link));
}

static void tx_hlist_delete_abort_test(struct kunit *test) {
	struct tx_hlist_test_entry entry = { .value = 1 };
	struct transaction *transaction;
	struct tx_hlist_head head;

	tx_hlist_head_init(&head, NULL);
	tx_hlist_entry_init(&entry.link);
	KUNIT_ASSERT_EQ(test, tx_hlist_add_head(&entry.link, &head), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_del(&entry.link), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_count(test, &head), 0);
	transaction_detach_task(current);
	KUNIT_EXPECT_EQ(test, tx_hlist_count(test, &head), 1);
	KUNIT_ASSERT_EQ(test, transaction_attach_task(transaction, current), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_abort(transaction), 0);
	KUNIT_EXPECT_PTR_EQ(test, head.head.first, &entry.link.node);
	KUNIT_EXPECT_FALSE(test, tx_hlist_unreferenced(&entry.link));
}

static void tx_hlist_move_commit_test(struct kunit *test) {
	struct tx_hlist_test_entry entry = { .value = 1 };
	struct transaction *transaction;
	struct tx_hlist_head first;
	struct tx_hlist_head second;

	tx_hlist_head_init(&first, NULL);
	tx_hlist_head_init(&second, NULL);
	tx_hlist_entry_init(&entry.link);
	KUNIT_ASSERT_EQ(test, tx_hlist_add_head(&entry.link, &first), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_move(&entry.link, &second), 0);
	KUNIT_EXPECT_PTR_EQ(test, first.head.first, &entry.link.node);
	KUNIT_EXPECT_TRUE(test, hlist_empty(&second.head));
	KUNIT_EXPECT_EQ(test, tx_hlist_count(test, &first), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_count(test, &second), 1);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_commit(transaction), 0);
	KUNIT_EXPECT_TRUE(test, hlist_empty(&first.head));
	KUNIT_EXPECT_PTR_EQ(test, second.head.first, &entry.link.node);
}

static void tx_hlist_shared_lock_move_test(struct kunit *test) {
	struct tx_hlist_test_entry entry = { .value = 1 };
	struct transaction *transaction;
	struct tx_hlist_head first;
	struct tx_hlist_head second;
	spinlock_t lock;

	spin_lock_init(&lock);
	tx_hlist_head_init(&first, &lock);
	tx_hlist_head_init(&second, &lock);
	tx_hlist_entry_init(&entry.link);
	KUNIT_ASSERT_EQ(test, tx_hlist_add_head(&entry.link, &first), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_move(&entry.link, &second), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_commit(transaction), 0);
	KUNIT_EXPECT_TRUE(test, hlist_empty(&first.head));
	KUNIT_EXPECT_PTR_EQ(test, second.head.first, &entry.link.node);
}

static void tx_hlist_lifetime_test(struct kunit *test) {
	struct tx_hlist_lifetime_entry entry;
	struct transaction *transaction;
	struct tx_hlist_head head;

	atomic_set(&entry.references, 0);
	tx_hlist_head_init(&head, NULL);
	tx_hlist_entry_init_with_lifetime(&entry.link, tx_hlist_lifetime_get, tx_hlist_lifetime_put);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_add_head(&entry.link, &head), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&entry.references), 1);
	KUNIT_EXPECT_FALSE(test, tx_hlist_unreferenced(&entry.link));
	KUNIT_ASSERT_EQ(test, tx_hlist_del(&entry.link), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&entry.references), 1);
	KUNIT_EXPECT_FALSE(test, tx_hlist_unreferenced(&entry.link));
	KUNIT_ASSERT_EQ(test, tx_hlist_test_commit(transaction), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&entry.references), 0);
	KUNIT_EXPECT_TRUE(test, tx_hlist_unreferenced(&entry.link));
}

static void tx_hlist_aborted_mutation_test(struct kunit *test) {
	struct tx_hlist_test_entry entry = { .value = 1 };
	struct transaction *transaction;
	struct tx_hlist_head head;

	tx_hlist_head_init(&head, NULL);
	tx_hlist_entry_init(&entry.link);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, abort_transaction(transaction), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_add_head(&entry.link, &head), -ECANCELED);
	KUNIT_EXPECT_TRUE(test, hlist_empty(&head.head));
	KUNIT_EXPECT_TRUE(test, tx_hlist_unreferenced(&entry.link));
	KUNIT_ASSERT_EQ(test, end_transaction(transaction), -ECANCELED);
	transaction_detach_task(current);
}

static void tx_hlist_ordinary_takeover_test(struct kunit *test) {
	struct tx_hlist_test_entry speculative = { .value = 1 };
	struct tx_hlist_test_entry ordinary = { .value = 2 };
	struct transaction *transaction;
	struct tx_hlist_head head;

	tx_hlist_head_init(&head, NULL);
	tx_hlist_entry_init(&speculative.link);
	tx_hlist_entry_init(&ordinary.link);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_add_head(&speculative.link, &head), 0);
	transaction_detach_task(current);
	KUNIT_ASSERT_EQ(test, tx_hlist_add_head(&ordinary.link, &head), 0);
	KUNIT_EXPECT_EQ(test, transaction_status(transaction), TRANSACTION_ABORTED);
	KUNIT_EXPECT_PTR_EQ(test, head.head.first, &ordinary.link.node);
	KUNIT_EXPECT_TRUE(test, hlist_unhashed(&speculative.link.node));
	KUNIT_ASSERT_EQ(test, end_transaction(transaction), -ECANCELED);
	KUNIT_EXPECT_TRUE(test, tx_hlist_unreferenced(&speculative.link));
}

static void tx_hlist_bl_commit_abort_test(struct kunit *test) {
	struct tx_hlist_bl_test_entry committed = { .value = 1 };
	struct tx_hlist_bl_test_entry aborted = { .value = 2 };
	struct transaction *transaction;
	struct tx_hlist_bl_head head;

	tx_hlist_bl_head_init(&head);
	tx_hlist_bl_entry_init(&committed.link);
	tx_hlist_bl_entry_init(&aborted.link);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_add_head(&committed.link, &head), 0);
	KUNIT_EXPECT_TRUE(test, hlist_bl_empty(&head.head));
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_count(test, &head), 1);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_commit(transaction), 0);
	KUNIT_EXPECT_PTR_EQ(test, hlist_bl_first(&head.head), &committed.link.node);

	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_add_head(&aborted.link, &head), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_count(test, &head), 2);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_abort(transaction), 0);
	KUNIT_EXPECT_PTR_EQ(test, hlist_bl_first(&head.head), &committed.link.node);
	KUNIT_EXPECT_TRUE(test, tx_hlist_bl_unreferenced(&aborted.link));
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_del(&committed.link), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_head_destroy(&head), 0);
}

static void tx_hlist_bl_rcu_delete_test(struct kunit *test) {
	struct tx_hlist_bl_test_entry first = { .value = 1 };
	struct tx_hlist_bl_test_entry second = { .value = 2 };
	struct tx_hlist_bl_head head;

	tx_hlist_bl_head_init(&head);
	tx_hlist_bl_entry_init(&first.link);
	tx_hlist_bl_entry_init(&second.link);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_add_head(&first.link, &head), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_add_head(&second.link, &head), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_head_destroy(&head), -EBUSY);
	KUNIT_ASSERT_PTR_EQ(test, second.link.node.next, &first.link.node);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_del(&second.link), 0);
	KUNIT_EXPECT_PTR_EQ(test, second.link.node.next, &first.link.node);
	KUNIT_EXPECT_TRUE(test, hlist_bl_unhashed(&second.link.node));
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_del(&first.link), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_head_destroy(&head), 0);
}

static void tx_hlist_bl_callbacks_test(struct kunit *test) {
	struct tx_hlist_callback_context context = { };
	struct tx_hlist_publish_entry entry = { };
	struct tx_hlist_head_callbacks callbacks = {
		.owner = &context,
		.get = tx_hlist_head_get,
		.put = tx_hlist_head_put,
		.lock_id = &context,
		.lock = tx_hlist_head_lock,
		.unlock = tx_hlist_head_unlock,
	};
	struct transaction *transaction;
	struct tx_hlist_bl_head head;

	tx_hlist_bl_head_init(&head);
	tx_hlist_bl_entry_init(&entry.link);
	tx_hlist_bl_entry_set_publish_callbacks(&entry.link, tx_hlist_publish_begin, tx_hlist_publish_end);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_head_set_callbacks(&head, &callbacks), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_add_head(&entry.link, &head), 0);
	KUNIT_EXPECT_EQ(test, context.gets, 1);
	KUNIT_EXPECT_EQ(test, context.puts, 0);
	KUNIT_EXPECT_EQ(test, entry.begins, 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_commit(transaction), 0);
	KUNIT_EXPECT_EQ(test, context.locks, 1);
	KUNIT_EXPECT_EQ(test, context.unlocks, 1);
	KUNIT_EXPECT_EQ(test, context.puts, 1);
	KUNIT_EXPECT_EQ(test, entry.begins, 1);
	KUNIT_EXPECT_EQ(test, entry.ends, 1);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_del(&entry.link), 0);
	KUNIT_EXPECT_EQ(test, entry.begins, 2);
	KUNIT_EXPECT_EQ(test, entry.ends, 2);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_head_destroy(&head), 0);
}

static void tx_hlist_bl_shared_blocking_lock_test(struct kunit *test) {
	struct tx_hlist_callback_context context = { };
	struct tx_hlist_head_callbacks callbacks = {
		.owner = &context,
		.get = tx_hlist_head_get,
		.put = tx_hlist_head_put,
		.lock_id = &context,
		.lock = tx_hlist_head_lock,
		.unlock = tx_hlist_head_unlock,
	};
	struct tx_hlist_publish_entry entry = { };
	struct transaction *transaction;
	struct tx_hlist_bl_head first;
	struct tx_hlist_bl_head second;

	tx_hlist_bl_head_init(&first);
	tx_hlist_bl_head_init(&second);
	tx_hlist_bl_entry_init(&entry.link);
	tx_hlist_bl_entry_set_publish_callbacks(&entry.link, tx_hlist_publish_begin, tx_hlist_publish_end);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_head_set_callbacks(&first, &callbacks), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_head_set_callbacks(&second, &callbacks), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_add_head(&entry.link, &first), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_move(&entry.link, &second), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_commit(transaction), 0);
	KUNIT_EXPECT_EQ(test, context.gets, 2);
	KUNIT_EXPECT_EQ(test, context.puts, 2);
	KUNIT_EXPECT_EQ(test, context.locks, 1);
	KUNIT_EXPECT_EQ(test, context.unlocks, 1);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_del(&entry.link), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_head_destroy(&first), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_head_destroy(&second), 0);
}

static void tx_hlist_bl_move_protocol_test(struct kunit *test) {
	struct tx_hlist_bl_test_entry entry = { .value = 1 };
	struct transaction *transaction;
	struct tx_hlist_bl_head first;
	struct tx_hlist_bl_head second;

	tx_hlist_bl_head_init(&first);
	tx_hlist_bl_head_init(&second);
	tx_hlist_bl_entry_init(&entry.link);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_add_head(&entry.link, &first), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_move(&entry.link, &second), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_move(&entry.link, &first), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_move_under_protocol(&entry.link, &second), -EOPNOTSUPP);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_commit(transaction), 0);
	KUNIT_EXPECT_PTR_EQ(test, hlist_bl_first(&first.head), &entry.link.node);
	KUNIT_EXPECT_TRUE(test, hlist_bl_empty(&second.head));
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_move(&entry.link, &second), -EOPNOTSUPP);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_move_under_protocol(&entry.link, &second), 0);
	KUNIT_EXPECT_TRUE(test, hlist_bl_empty(&first.head));
	KUNIT_EXPECT_PTR_EQ(test, hlist_bl_first(&second.head), &entry.link.node);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_del(&entry.link), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_head_destroy(&first), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_head_destroy(&second), 0);
}

static void tx_hlist_bl_detached_workset_lifetime_test(struct kunit *test) {
	struct tx_hlist_bl_test_entry entry = { .value = 1 };
	struct transaction *transaction;
	struct tx_hlist_bl_head head;

	tx_hlist_bl_head_init(&head);
	tx_hlist_bl_entry_init(&entry.link);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_count(test, &head), 0);
	transaction_detach_task(current);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_add_head(&entry.link, &head), 0);
	KUNIT_EXPECT_EQ(test, transaction_status(transaction), TRANSACTION_ABORTED);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_del(&entry.link), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_head_destroy(&head), -EBUSY);
	KUNIT_EXPECT_EQ(test, end_transaction(transaction), -ECANCELED);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_head_destroy(&head), 0);
}

static void tx_hlist_bl_speculative_move_test(struct kunit *test) {
	struct tx_hlist_bl_test_entry entry = { .value = 1 };
	struct transaction *transaction;
	struct tx_hlist_bl_head first;
	struct tx_hlist_bl_head second;

	tx_hlist_bl_head_init(&first);
	tx_hlist_bl_head_init(&second);
	tx_hlist_bl_entry_init(&entry.link);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_add_head(&entry.link, &first), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_move(&entry.link, &second), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_count(test, &first), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_count(test, &second), 1);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_commit(transaction), 0);
	KUNIT_EXPECT_TRUE(test, hlist_bl_empty(&first.head));
	KUNIT_EXPECT_PTR_EQ(test, hlist_bl_first(&second.head), &entry.link.node);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_del(&entry.link), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_head_destroy(&first), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_head_destroy(&second), 0);
}

static void tx_hlist_callback_validation_test(struct kunit *test) {
	struct tx_hlist_head_callbacks callbacks = { };
	struct tx_hlist_bl_head head;

	tx_hlist_bl_head_init(&head);
	callbacks.lock_id = &head;
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_head_set_callbacks(&head, &callbacks), -EINVAL);
	callbacks.lock_id = NULL;
	callbacks.lock = tx_hlist_head_lock;
	callbacks.unlock = tx_hlist_head_unlock;
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_head_set_callbacks(&head, &callbacks), -EINVAL);
	KUNIT_EXPECT_EQ(test, tx_hlist_bl_head_destroy(&head), 0);
}

static void tx_hlist_operation_sequence_test(struct kunit *test) {
	struct tx_hlist_test_entry entry = { .value = 1 };
	struct transaction *transaction;
	struct tx_hlist_head first;
	struct tx_hlist_head second;
	struct tx_hlist_head third;

	tx_hlist_head_init(&first, NULL);
	tx_hlist_head_init(&second, NULL);
	tx_hlist_head_init(&third, NULL);
	tx_hlist_entry_init(&entry.link);
	KUNIT_ASSERT_EQ(test, tx_hlist_add_head(&entry.link, &first), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_del(&entry.link), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_add_head(&entry.link, &second), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_move(&entry.link, &third), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_count(test, &first), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_count(test, &second), 0);
	KUNIT_EXPECT_EQ(test, tx_hlist_count(test, &third), 1);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_abort(transaction), 0);
	KUNIT_EXPECT_PTR_EQ(test, first.head.first, &entry.link.node);
	KUNIT_EXPECT_TRUE(test, hlist_empty(&second.head));
	KUNIT_EXPECT_TRUE(test, hlist_empty(&third.head));

	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_del(&entry.link), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_add_head(&entry.link, &second), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_move(&entry.link, &third), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_commit(transaction), 0);
	KUNIT_EXPECT_TRUE(test, hlist_empty(&first.head));
	KUNIT_EXPECT_TRUE(test, hlist_empty(&second.head));
	KUNIT_EXPECT_PTR_EQ(test, third.head.first, &entry.link.node);
}

static void tx_hlist_bl_rcu_move_test(struct kunit *test) {
	struct tx_hlist_rcu_test_state *state;
	struct tx_hlist_head_callbacks callbacks = {
		.lock = tx_hlist_rcu_write_begin,
		.unlock = tx_hlist_rcu_write_end,
	};
	struct task_struct *reader;
	struct transaction *transaction;

	state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, state);
	init_completion(&state->context.started);
	seqcount_init(&state->context.sequence);
	atomic_set(&state->context.errors, 0);
	atomic_set(&state->context.samples, 0);
	init_waitqueue_head(&state->context.sampled);
	state->context.first = &state->first;
	state->context.second = &state->second;
	callbacks.owner = &state->context;
	callbacks.lock_id = &state->context.sequence;
	tx_hlist_bl_head_init(&state->first);
	tx_hlist_bl_head_init(&state->second);
	tx_hlist_bl_entry_init(&state->entry.link);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, tx_hlist_rcu_state_cleanup, state), 0);
	tx_hlist_bl_entry_set_publish_callbacks(&state->entry.link, tx_hlist_publish_begin, tx_hlist_publish_end);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_head_set_callbacks(&state->first, &callbacks), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_head_set_callbacks(&state->second, &callbacks), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_add_head(&state->entry.link, &state->first), 0);
	reader = kthread_run(tx_hlist_rcu_reader, &state->context, "tx_hlist_rcu_reader");
	KUNIT_ASSERT_FALSE(test, IS_ERR(reader));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, tx_hlist_stop_reader, reader), 0);
	wait_for_completion(&state->context.started);
	KUNIT_EXPECT_NE(test, wait_event_timeout(state->context.sampled,
			atomic_read(&state->context.samples) >= 10, HZ), 0L);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_begin(test, &transaction), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_move(&state->entry.link, &state->second), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_test_commit(transaction), 0);
	kunit_release_action(test, tx_hlist_stop_reader, reader);
	KUNIT_EXPECT_EQ(test, atomic_read(&state->context.errors), 0);
	KUNIT_EXPECT_GT(test, atomic_read(&state->context.samples), 0);
	KUNIT_EXPECT_TRUE(test, hlist_bl_empty(&state->first.head));
	KUNIT_EXPECT_PTR_EQ(test, hlist_bl_first(&state->second.head), &state->entry.link.node);
	kunit_release_action(test, tx_hlist_rcu_state_cleanup, state);
}

static void tx_hlist_bl_ordinary_rcu_move_test(struct kunit *test) {
	struct tx_hlist_rcu_test_state *state;
	struct tx_hlist_head_callbacks callbacks = {
		.lock = tx_hlist_rcu_write_begin,
		.unlock = tx_hlist_rcu_write_end,
	};
	struct task_struct *reader;
	unsigned int i;
	int samples_before;

	state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, state);
	init_completion(&state->context.started);
	seqcount_init(&state->context.sequence);
	atomic_set(&state->context.errors, 0);
	atomic_set(&state->context.samples, 0);
	init_waitqueue_head(&state->context.sampled);
	state->context.first = &state->first;
	state->context.second = &state->second;
	callbacks.owner = &state->context;
	callbacks.lock_id = &state->context.sequence;
	tx_hlist_bl_head_init(&state->first);
	tx_hlist_bl_head_init(&state->second);
	tx_hlist_bl_entry_init(&state->entry.link);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, tx_hlist_rcu_state_cleanup, state), 0);
	tx_hlist_bl_entry_set_publish_callbacks(&state->entry.link, tx_hlist_publish_begin, tx_hlist_publish_end);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_head_set_callbacks(&state->first, &callbacks), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_head_set_callbacks(&state->second, &callbacks), 0);
	KUNIT_ASSERT_EQ(test, tx_hlist_bl_add_head(&state->entry.link, &state->first), 0);
	reader = kthread_run(tx_hlist_rcu_reader, &state->context, "tx_hlist_rcu_reader");
	KUNIT_ASSERT_FALSE(test, IS_ERR(reader));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, tx_hlist_stop_reader, reader), 0);
	wait_for_completion(&state->context.started);
	KUNIT_EXPECT_NE(test, wait_event_timeout(state->context.sampled,
			atomic_read(&state->context.samples) >= 10, HZ), 0L);
	samples_before = atomic_read(&state->context.samples);
	for (i = 0; i < 257; i++)
		KUNIT_ASSERT_EQ(test, tx_hlist_bl_move(&state->entry.link,
						 i & 1 ? &state->first : &state->second), 0);
	KUNIT_EXPECT_NE(test, wait_event_timeout(state->context.sampled,
			atomic_read(&state->context.samples) > samples_before, HZ), 0L);
	kunit_release_action(test, tx_hlist_stop_reader, reader);
	KUNIT_EXPECT_EQ(test, atomic_read(&state->context.errors), 0);
	KUNIT_EXPECT_TRUE(test, hlist_bl_empty(&state->first.head));
	KUNIT_EXPECT_PTR_EQ(test, hlist_bl_first(&state->second.head), &state->entry.link.node);
	kunit_release_action(test, tx_hlist_rcu_state_cleanup, state);
}

static struct kunit_case tx_hlist_test_cases[] = {
	KUNIT_CASE(tx_hlist_init_test),
	KUNIT_CASE(tx_hlist_nontransactional_test),
	KUNIT_CASE(tx_hlist_add_commit_test),
	KUNIT_CASE(tx_hlist_add_abort_test),
	KUNIT_CASE(tx_hlist_delete_commit_test),
	KUNIT_CASE(tx_hlist_add_delete_test),
	KUNIT_CASE(tx_hlist_delete_abort_test),
	KUNIT_CASE(tx_hlist_move_commit_test),
	KUNIT_CASE(tx_hlist_shared_lock_move_test),
	KUNIT_CASE(tx_hlist_lifetime_test),
	KUNIT_CASE(tx_hlist_aborted_mutation_test),
	KUNIT_CASE(tx_hlist_ordinary_takeover_test),
	KUNIT_CASE(tx_hlist_bl_commit_abort_test),
	KUNIT_CASE(tx_hlist_bl_rcu_delete_test),
	KUNIT_CASE(tx_hlist_bl_callbacks_test),
	KUNIT_CASE(tx_hlist_bl_shared_blocking_lock_test),
	KUNIT_CASE(tx_hlist_bl_move_protocol_test),
	KUNIT_CASE(tx_hlist_bl_detached_workset_lifetime_test),
	KUNIT_CASE(tx_hlist_bl_speculative_move_test),
	KUNIT_CASE(tx_hlist_callback_validation_test),
	KUNIT_CASE(tx_hlist_operation_sequence_test),
	KUNIT_CASE(tx_hlist_bl_rcu_move_test),
	KUNIT_CASE(tx_hlist_bl_ordinary_rcu_move_test),
	{ }
};

static struct kunit_suite tx_hlist_test_suite = {
	.name = "transaction_hlist",
	.test_cases = tx_hlist_test_cases,
};

kunit_test_suite(tx_hlist_test_suite);

MODULE_DESCRIPTION("Transactional hlist tests");
MODULE_LICENSE("GPL");
