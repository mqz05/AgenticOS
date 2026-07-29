// SPDX-License-Identifier: GPL-2.0
// Common transactional-object infrastructure.

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/transaction.h>

void transaction_object_init(struct transaction_object *object, enum transaction_object_type type) {
	object->type = type;
	object->writer = NULL;
	INIT_LIST_HEAD(&object->readers);
	spin_lock_init(&object->lock);
	object->version = 0;
}
EXPORT_SYMBOL_GPL(transaction_object_init);

// Called with object->lock held.
static struct txobj_thread_list_node *object_find_reader(struct transaction_object *object, struct transaction *transaction) {
	struct txobj_thread_list_node *node;

	list_for_each_entry(node, &object->readers, object_list) {
		if (node->tx == transaction)
			return node;
	}

	return NULL;
}

static int transaction_object_lose(struct transaction *transaction, bool can_sleep, bool *should_sleep) {
	int ret;

	/*
	 * should_sleep only reports that waiting and retrying would be allowed.
	 * TODO: The current acquire API has no wait/retry loop, so the loser is aborted.
	 */
	if (should_sleep)
		*should_sleep = can_sleep;

	ret = abort_transaction(transaction);
	return ret ? ret : -ECANCELED;
}

/*
 * Acquire read or read/write ownership of an object. The workset node is also
 * the object reader-list entry (writers are included in the reader list)
 */
int transaction_object_acquire(struct transaction *transaction,
			       struct txobj_thread_list_node *node,
			       enum transaction_access_mode mode,
			       bool *should_sleep) {
	struct txobj_thread_list_node *existing;
	struct txobj_thread_list_node *reader;
	struct transaction_object *object;
	struct transaction *writer;
	enum transaction_state status;
	bool can_sleep;
	int ret = 0;

	if (should_sleep)
		*should_sleep = false;
	if (!transaction || !node || !node->tx_obj || node->tx != transaction)
		return -EINVAL;
	if (mode != TRANSACTION_ACCESS_READ && mode != TRANSACTION_ACCESS_READ_WRITE)
		return -EOPNOTSUPP;
	if (node->type != node->tx_obj->type)
		return -EINVAL;
	if (transaction_status(transaction) != TRANSACTION_ACTIVE)
		return live_transaction(transaction) ? -ECANCELED : -EBUSY;
	if (atomic_read_acquire(&transaction->finishing))
		return -EBUSY;

	object = node->tx_obj;
	spin_lock(&object->lock);

	existing = object_find_reader(object, transaction);
	if (existing && existing != node) {
		ret = -EEXIST;
		goto out;
	}
	if (!existing && !list_empty(&node->object_list)) {
		ret = -EBUSY;
		goto out;
	}
	if (existing && node->rw >= mode)
		goto publish;

	/*
	 * Check readers first because this minimizes aborts. Do not change any
	 * ownership until every conflicting reader has lost.
	 */
	if (mode == TRANSACTION_ACCESS_READ_WRITE) {
		list_for_each_entry(reader, &object->readers, object_list) {
			if (reader->tx == transaction)
				continue;

			can_sleep = false;
			if (transaction_contention_manager(reader->tx, transaction, &can_sleep)) {
				ret = transaction_object_lose(transaction, can_sleep, should_sleep);
				goto out;
			}
		}
	}

	writer = object->writer;
	if (writer && writer != transaction) {
		can_sleep = false;
		if (transaction_contention_manager(writer, transaction, &can_sleep)) {
			ret = transaction_object_lose(transaction, can_sleep, should_sleep);
			goto out;
		}
	}

	if (mode == TRANSACTION_ACCESS_READ_WRITE) {
		list_for_each_entry(reader, &object->readers, object_list) {
			if (reader->tx == transaction)
				continue;
			ret = abort_transaction(reader->tx);
			if (ret)
				goto lost_race;
		}
	}
	if (writer && writer != transaction) {
		ret = abort_transaction(writer);
		if (ret)
			goto lost_race;
	}

	if (mode == TRANSACTION_ACCESS_READ_WRITE) {
		struct txobj_thread_list_node *next;

		list_for_each_entry_safe(reader, next, &object->readers, object_list) {
			if (reader->tx != transaction)
				list_del_init(&reader->object_list);
		}
	}
	if (writer && writer != transaction)
		object->writer = NULL;

publish:
	/*
	 * Recheck ACTIVE while the transaction lock prevents abort from racing
	 * conditions with ownership. Lock order is object->lock and then
	 * transaction->lock everywhere ownership and transaction state meet.
	 */
	spin_lock(&transaction->lock);
	status = transaction_status(transaction);
	if (status != TRANSACTION_ACTIVE) {
		ret = status == TRANSACTION_ABORTED || status == TRANSACTION_ABORTING ? -ECANCELED : -EBUSY;
		spin_unlock(&transaction->lock);
		goto out;
	}
	if (atomic_read(&transaction->finishing)) {
		ret = -EBUSY;
		spin_unlock(&transaction->lock);
		goto out;
	}
	if (!existing) {
		list_add(&node->object_list, &object->readers);
		node->rw = mode;
	} else if (node->rw < mode) {
		node->rw = mode;
	}
	if (mode == TRANSACTION_ACCESS_READ_WRITE)
		object->writer = transaction;
	spin_unlock(&transaction->lock);
	goto out;

lost_race:
	/*
	 * A transaction that started committing after arbitration cannot be
	 * aborted. It wins the race, so the acquiring transaction must lose.
	 */
	ret = transaction_object_lose(transaction, false, should_sleep);
out:
	spin_unlock(&object->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(transaction_object_acquire);

// Called with node->tx_obj->lock held.
void transaction_object_remove_ownership_locked(struct txobj_thread_list_node *node) {
	lockdep_assert_held(&node->tx_obj->lock);

	if (node->tx_obj->writer == node->tx)
		node->tx_obj->writer = NULL;
	if (!list_empty(&node->object_list))
		list_del_init(&node->object_list);
}

// Remove a workset node's reader and writer references from its object.
void transaction_object_remove_ownership(struct txobj_thread_list_node *node) {
	if (!node || !node->tx_obj)
		return;

	spin_lock(&node->tx_obj->lock);
	transaction_object_remove_ownership_locked(node);
	spin_unlock(&node->tx_obj->lock);
}
EXPORT_SYMBOL_GPL(transaction_object_remove_ownership);
