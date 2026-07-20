// SPDX-License-Identifier: GPL-2.0
// Core system-transaction lifecycle.

#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/transaction.h>

// The timestamp orders transactions that contend for the same object.
static atomic64_t timestamp_counter = ATOMIC64_INIT(0);

// Sort the workset by the original object's kernel virtual address.
static int transaction_workset_compare(const struct skiplist_head *left,
                                       const struct skiplist_head *right) {
	const struct txobj_thread_list_node *left_node;
	const struct txobj_thread_list_node *right_node;
	unsigned long left_address;
	unsigned long right_address;

	left_node = skiplist_entry(left,
					       struct txobj_thread_list_node,
					       workset_list);
	right_node = skiplist_entry(right,
						struct txobj_thread_list_node,
						workset_list);
	left_address = (unsigned long)left_node->orig_obj;
	right_address = (unsigned long)right_node->orig_obj;

	if (left_address < right_address)
		return -1;
	if (left_address > right_address)
		return 1;

	return 0;
}

// Allocate and initialize the common fields of a workset entry.
struct txobj_thread_list_node *transaction_workset_node_alloc(void *shadow_obj,
                                                              void *orig_obj,
                                                              struct transaction_object *tx_obj,
                                                              enum transaction_object_type type,
                                                              enum transaction_access_mode rw,
                                                              gfp_t gfp) {
	struct txobj_thread_list_node *node;

	node = kzalloc(sizeof(*node), gfp);
	if (!node)
		return NULL;

	skiplist_init_node(&node->workset_list);
	INIT_LIST_HEAD(&node->object_list);
	node->type = type;
	node->shadow_obj = shadow_obj;
	node->orig_obj = orig_obj;
	node->tx_obj = tx_obj;
	node->rw = rw;

	return node;
}
EXPORT_SYMBOL_GPL(transaction_workset_node_alloc);

// Free a workset entry only after it has been detached from both lists.
void transaction_workset_node_free(struct txobj_thread_list_node *node) {
	if (!node)
		return;
	if (WARN_ON_ONCE(node->tx ||
			 skiplist_linked(&node->workset_list) ||
			 !list_empty(&node->object_list)))
		return;

	kfree(node);
}
EXPORT_SYMBOL_GPL(transaction_workset_node_free);

// Find an original object while the caller holds workset_lock.
static struct txobj_thread_list_node *transaction_workset_find_orig_locked(struct transaction *transaction,
                                                                           const void *orig_obj) {
	struct txobj_thread_list_node *node;
	unsigned long address = (unsigned long)orig_obj;

	skiplist_for_each_entry(node, &transaction->object_list,
					    workset_list) {
		unsigned long node_address = (unsigned long)node->orig_obj;

		if (node_address == address)
			return node;
		if (node_address > address)
			break;
	}

	return NULL;
}

// Find a transactional object while the caller holds workset_lock.
static struct txobj_thread_list_node *transaction_workset_find_object_locked(struct transaction *transaction,
                                                                             struct transaction_object *tx_obj) {
	struct txobj_thread_list_node *node;

	skiplist_for_each_entry(node, &transaction->object_list,
					    workset_list) {
		if (node->tx_obj == tx_obj)
			return node;
	}

	return NULL;
}

// Add one object to an active transaction's ordered workset.
int transaction_workset_add(struct transaction *transaction, struct txobj_thread_list_node *node) {
	int ret;

	if (!transaction || !node || !node->orig_obj || !node->tx_obj)
		return -EINVAL;
	if (node->tx || skiplist_linked(&node->workset_list) ||
	    !list_empty(&node->object_list))
		return -EBUSY;

	spin_lock(&transaction->workset_lock);
	if (transaction_status(transaction) != TRANSACTION_ACTIVE) {
		ret = -EBUSY;
		goto out;
	}
	if (transaction_workset_find_object_locked(transaction, node->tx_obj)) {
		ret = -EEXIST;
		goto out;
	}

	ret = skiplist_insert(&node->workset_list,
					  &transaction->object_list,
					  transaction_workset_compare);
	if (!ret)
		node->tx = transaction;
out:
	spin_unlock(&transaction->workset_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(transaction_workset_add);

// Look up a workset entry by its stable original object.
struct txobj_thread_list_node *transaction_workset_find_orig(struct transaction *transaction,
                                                             const void *orig_obj) {
	struct txobj_thread_list_node *node;

	if (!transaction || !orig_obj)
		return NULL;

	spin_lock(&transaction->workset_lock);
	node = transaction_workset_find_orig_locked(transaction, orig_obj);
	spin_unlock(&transaction->workset_lock);

	return node;
}
EXPORT_SYMBOL_GPL(transaction_workset_find_orig);

// Look up a workset entry given a transaction object.
struct txobj_thread_list_node *transaction_workset_find_object(struct transaction *transaction,
                                                               struct transaction_object *tx_obj) {
	struct txobj_thread_list_node *node;

	if (!transaction || !tx_obj)
		return NULL;

	spin_lock(&transaction->workset_lock);
	node = transaction_workset_find_object_locked(transaction, tx_obj);
	spin_unlock(&transaction->workset_lock);

	return node;
}
EXPORT_SYMBOL_GPL(transaction_workset_find_object);

// Detach a workset entry without releasing the entry or its shadow object.
struct txobj_thread_list_node *transaction_workset_remove(struct transaction *transaction,
                                                          struct txobj_thread_list_node *node) {
	struct txobj_thread_list_node *removed = NULL;

	if (!transaction || !node)
		return NULL;

	spin_lock(&transaction->workset_lock);
	if (node->tx != transaction ||
	    !skiplist_linked(&node->workset_list))
		goto out;

	skiplist_del(&node->workset_list,
				 &transaction->object_list);
	node->tx = NULL;
	removed = node;
out:
	spin_unlock(&transaction->workset_lock);

	return removed;
}
EXPORT_SYMBOL_GPL(transaction_workset_remove);

bool transaction_workset_empty(struct transaction *transaction) {
	bool empty;

	if (!transaction)
		return true;

	spin_lock(&transaction->workset_lock);
	empty = skiplist_empty(&transaction->object_list);
	spin_unlock(&transaction->workset_lock);

	return empty;
}
EXPORT_SYMBOL_GPL(transaction_workset_empty);

struct transaction *transaction_alloc(gfp_t gfp) {
	struct transaction *transaction;

	transaction = kzalloc(sizeof(*transaction), gfp);
	if (!transaction)
		return NULL;

	INIT_LIST_HEAD(&transaction->tasks);
	atomic_set(&transaction->task_count, 0);
	refcount_set(&transaction->ref_count, 1);
	atomic_set(&transaction->status, TRANSACTION_INACTIVE);
	transaction->unsupported_operation_action = UNSUPPORTED_ABORT;
	skiplist_init_head(&transaction->object_list);
	spin_lock_init(&transaction->workset_lock);
	init_waitqueue_head(&transaction->losers);
	init_waitqueue_head(&transaction->siblings);
	spin_lock_init(&transaction->lock);

	return transaction;
}
EXPORT_SYMBOL_GPL(transaction_alloc);

struct transaction *transaction_get(struct transaction *transaction) {
	if (transaction)
		refcount_inc(&transaction->ref_count);

	return transaction;
}
EXPORT_SYMBOL_GPL(transaction_get);

void transaction_put(struct transaction *transaction) {
	if (!transaction)
		return;

	if (refcount_dec_and_test(&transaction->ref_count)) {
		if (WARN_ON_ONCE(!transaction_workset_empty(transaction)))
			return;
		kfree(transaction);
	}
}
EXPORT_SYMBOL_GPL(transaction_put);

void transaction_task_init(struct task_struct *task) {
	WRITE_ONCE(task->transaction, NULL);
	INIT_LIST_HEAD(&task->transaction_entry);
}

int transaction_attach_task(struct transaction *transaction, struct task_struct *task) {
	int ret = 0;

	if (!transaction || !task)
		return -EINVAL;

	spin_lock(&transaction->lock);
	if (READ_ONCE(task->transaction)) {
		ret = -EBUSY;
		goto out;
	}
	if (atomic_read(&transaction->task_count)) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	transaction_get(transaction);
	list_add_tail(&task->transaction_entry, &transaction->tasks);
	atomic_inc(&transaction->task_count);
	WRITE_ONCE(task->transaction, transaction);
out:
	spin_unlock(&transaction->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(transaction_attach_task);

void transaction_detach_task(struct task_struct *task) {
	struct transaction *transaction;

	if (!task)
		return;

	transaction = READ_ONCE(task->transaction);
	if (!transaction)
		return;

	spin_lock(&transaction->lock);
	if (READ_ONCE(task->transaction) != transaction) {
		spin_unlock(&transaction->lock);
		return;
	}

	WRITE_ONCE(task->transaction, NULL);
	list_del_init(&task->transaction_entry);
	atomic_dec(&transaction->task_count);
	spin_unlock(&transaction->lock);
	transaction_put(transaction);
}
EXPORT_SYMBOL_GPL(transaction_detach_task);

struct transaction *current_transaction(void) {
	return READ_ONCE(current->transaction);
}
EXPORT_SYMBOL_GPL(current_transaction);

enum transaction_state transaction_status(const struct transaction *transaction) {
	return atomic_read(&transaction->status);
}
EXPORT_SYMBOL_GPL(transaction_status);

bool inactive_transaction(const struct transaction *transaction) {
	return transaction_status(transaction) == TRANSACTION_INACTIVE;
}
EXPORT_SYMBOL_GPL(inactive_transaction);

bool active_transaction(const struct transaction *transaction) {
	return transaction_status(transaction) != TRANSACTION_INACTIVE;
}
EXPORT_SYMBOL_GPL(active_transaction);

/*
 * A live transaction is active or has been marked aborted but has not yet
 * observed the abort and cleaned up.
 */
bool live_transaction(const struct transaction *transaction) {
	enum transaction_state status = transaction_status(transaction);

	return status == TRANSACTION_ACTIVE || status == TRANSACTION_ABORTED;
}
EXPORT_SYMBOL_GPL(live_transaction);

bool committing_transaction(const struct transaction *transaction) {
	return transaction_status(transaction) == TRANSACTION_COMMITTING;
}
EXPORT_SYMBOL_GPL(committing_transaction);

bool aborting_transaction(const struct transaction *transaction) {
	return transaction_status(transaction) == TRANSACTION_ABORTING;
}
EXPORT_SYMBOL_GPL(aborting_transaction);

int begin_transaction(struct transaction *transaction) {
	int ret = 0;

	if (!transaction)
		return -EINVAL;

	spin_lock(&transaction->lock);
	if (!inactive_transaction(transaction)) {
		ret = -EALREADY;
		goto out;
	}
	transaction->timestamp = atomic64_inc_return(&timestamp_counter);
	transaction->count = 0;
	transaction->abortWithErr = false;
	atomic_set(&transaction->status, TRANSACTION_ACTIVE);
out:
	spin_unlock(&transaction->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(begin_transaction);

/*
 * This function can be called by an adversary to abort a transaction. The
 * transaction owner performs cleanup after it observes TRANSACTION_ABORTED.
 */
int abort_transaction(struct transaction *transaction) {
	enum transaction_state status;

	if (!transaction)
		return -EINVAL;

	status = atomic_cmpxchg(&transaction->status, TRANSACTION_ACTIVE,
				TRANSACTION_ABORTED);
	switch (status) {
	case TRANSACTION_ACTIVE:
	case TRANSACTION_ABORTED:
	case TRANSACTION_ABORTING:
		return 0;
	case TRANSACTION_INACTIVE:
		return -EINVAL;
	case TRANSACTION_COMMITTING:
		return -EBUSY;
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(abort_transaction);

static void terminate_transaction(struct transaction *transaction) {
	transaction->autoretry = 0;
	transaction->abortWithErr = false;
	atomic_set(&transaction->status, TRANSACTION_INACTIVE);
	wake_up_all(&transaction->losers);
	wake_up_all(&transaction->siblings);
}

int end_transaction(struct transaction *transaction) {
	enum transaction_state status;

	if (!transaction)
		return -EINVAL;

	status = atomic_cmpxchg(&transaction->status, TRANSACTION_ACTIVE,
				TRANSACTION_COMMITTING);
	if (status == TRANSACTION_ACTIVE) {
		if (!transaction_workset_empty(transaction)) {
			atomic_set(&transaction->status, TRANSACTION_ACTIVE);
			return -EBUSY;
		}
		terminate_transaction(transaction);
		return 0;
	}

	if (status == TRANSACTION_ABORTED &&
	    atomic_cmpxchg(&transaction->status, TRANSACTION_ABORTED,
			   TRANSACTION_ABORTING) == TRANSACTION_ABORTED) {
		if (!transaction_workset_empty(transaction)) {
			atomic_set(&transaction->status, TRANSACTION_ABORTED);
			return -EBUSY;
		}
		transaction->count++;
		terminate_transaction(transaction);
		return -ECANCELED;
	}

	if (status == TRANSACTION_INACTIVE)
		return -EINVAL;

	return -EBUSY;
}
EXPORT_SYMBOL_GPL(end_transaction);

void transaction_task_exit(struct task_struct *task) {
	struct transaction *transaction;

	if (!task)
		return;

	transaction = READ_ONCE(task->transaction);
	if (!transaction)
		return;

	if (live_transaction(transaction)) {
		abort_transaction(transaction);
		end_transaction(transaction);
	}
	transaction_detach_task(task);
}

int transaction_task_fork(const struct task_struct *task) {
	struct transaction *transaction;

	transaction = READ_ONCE(task->transaction);
	if (transaction && live_transaction(transaction))
		return -EOPNOTSUPP;

	return 0;
}
