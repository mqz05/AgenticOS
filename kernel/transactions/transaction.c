// SPDX-License-Identifier: GPL-2.0
/* Core system-transaction lifecycle. */

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/transaction.h>

/* The timestamp orders transactions that contend for the same object. */
static atomic64_t timestamp_counter = ATOMIC64_INIT(0);

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
	INIT_LIST_HEAD(&transaction->object_list);  // TODO: replace with skip list
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
	if (transaction && refcount_dec_and_test(&transaction->ref_count))
		kfree(transaction);
}
EXPORT_SYMBOL_GPL(transaction_put);

void transaction_task_init(struct task_struct *task) {
	WRITE_ONCE(task->transaction, NULL);
	INIT_LIST_HEAD(&task->transaction_entry);
}

int transaction_attach_task(struct transaction *transaction,
			    struct task_struct *task) {
	int ret = 0;

	if (!transaction || !task)
		return -EINVAL;

	spin_lock(&transaction->lock);
	if (READ_ONCE(task->transaction)) {
		ret = -EBUSY;
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
		terminate_transaction(transaction);
		return 0;
	}

	if (status == TRANSACTION_ABORTED &&
	    atomic_cmpxchg(&transaction->status, TRANSACTION_ABORTED,
			   TRANSACTION_ABORTING) == TRANSACTION_ABORTED) {
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
