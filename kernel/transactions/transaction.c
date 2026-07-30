// SPDX-License-Identifier: GPL-2.0
// Core system-transaction lifecycle.

#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/sched/prio.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
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
static struct txobj_thread_list_node *transaction_workset_find_object_in_locked(struct skiplist_head *workset,
                                                                                struct transaction_object *tx_obj) {
	struct txobj_thread_list_node *node;

	skiplist_for_each_entry(node, workset,
					    workset_list) {
		if (node->tx_obj == tx_obj)
			return node;
	}

	return NULL;
}

static struct txobj_thread_list_node *transaction_workset_find_object_locked(struct transaction *transaction,
                                                                             struct transaction_object *tx_obj) {
	return transaction_workset_find_object_in_locked(&transaction->object_list, tx_obj);
}

static int transaction_workset_add_to(struct transaction *transaction,
				      struct txobj_thread_list_node *node,
				      struct skiplist_head *workset) {
	int ret;

	if (!transaction || !node || !node->orig_obj || !node->tx_obj)
		return -EINVAL;
	if (node->tx || skiplist_linked(&node->workset_list) ||
	    !list_empty(&node->object_list))
		return -EBUSY;

	spin_lock(&transaction->workset_lock);
	if (transaction_status(transaction) != TRANSACTION_ACTIVE ||
	    atomic_read_acquire(&transaction->finishing)) {
		ret = -EBUSY;
		goto out;
	}
	if (transaction_workset_find_object_in_locked(workset, node->tx_obj)) {
		ret = -EEXIST;
		goto out;
	}

	ret = skiplist_insert(&node->workset_list,
					  workset,
					  transaction_workset_compare);
	if (!ret)
		node->tx = transaction;
out:
	spin_unlock(&transaction->workset_lock);

	return ret;
}

// Add one object to an active transaction's ordered object workset.
int transaction_workset_add(struct transaction *transaction, struct txobj_thread_list_node *node) {
	return transaction_workset_add_to(transaction, node, &transaction->object_list);
}
EXPORT_SYMBOL_GPL(transaction_workset_add);

// Add one transactional list to the separate ordered list workset.
int transaction_list_workset_add(struct transaction *transaction, struct txobj_thread_list_node *node) {
	return transaction_workset_add_to(transaction, node, &transaction->list_list);
}
EXPORT_SYMBOL_GPL(transaction_list_workset_add);

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

struct txobj_thread_list_node *transaction_list_workset_find_object(struct transaction *transaction,
                                                                    struct transaction_object *tx_obj) {
	struct txobj_thread_list_node *node;

	if (!transaction || !tx_obj)
		return NULL;

	spin_lock(&transaction->workset_lock);
	node = transaction_workset_find_object_in_locked(&transaction->list_list, tx_obj);
	spin_unlock(&transaction->workset_lock);

	return node;
}
EXPORT_SYMBOL_GPL(transaction_list_workset_find_object);

static struct txobj_thread_list_node *transaction_workset_remove_from(struct transaction *transaction,
								      struct txobj_thread_list_node *node,
								      struct skiplist_head *workset) {
	struct txobj_thread_list_node *removed = NULL;

	if (!transaction || !node)
		return NULL;

	spin_lock(&transaction->workset_lock);
	if (node->tx != transaction ||
	    !skiplist_linked(&node->workset_list))
		goto out;

	skiplist_del(&node->workset_list,
				 workset);
	node->tx = NULL;
	removed = node;
out:
	spin_unlock(&transaction->workset_lock);

	return removed;
}

// Detach an object workset entry without releasing the entry or shadow object.
struct txobj_thread_list_node *transaction_workset_remove(struct transaction *transaction,
                                                          struct txobj_thread_list_node *node) {
	return transaction_workset_remove_from(transaction, node, &transaction->object_list);
}
EXPORT_SYMBOL_GPL(transaction_workset_remove);

// Detach a list workset entry without releasing the entry or shadow object.
struct txobj_thread_list_node *transaction_list_workset_remove(struct transaction *transaction,
                                                               struct txobj_thread_list_node *node) {
	return transaction_workset_remove_from(transaction, node, &transaction->list_list);
}
EXPORT_SYMBOL_GPL(transaction_list_workset_remove);

bool transaction_workset_empty(struct transaction *transaction) {
	bool empty;

	if (!transaction)
		return true;

	spin_lock(&transaction->workset_lock);
	empty = skiplist_empty(&transaction->object_list) &&
		skiplist_empty(&transaction->list_list);
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
	atomic_set(&transaction->finishing, 0);
	transaction->unsupported_operation_action = UNSUPPORTED_ABORT;
	skiplist_init_head(&transaction->object_list);
	skiplist_init_head(&transaction->list_list);
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

/* Drop a reference to a transaction and free it when the last reference
   goes away. Leftover workset entries warn because normal commit or abort
   handling must release them before the final reference is dropped. */
void transaction_put(struct transaction * transaction) {
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

// Return the best dynamic priority among the tasks in a transaction.
static int transaction_best_priority(struct transaction *transaction) {
	struct task_struct *task;
	int priority = MAX_PRIO;

	spin_lock(&transaction->lock);
	list_for_each_entry(task, &transaction->tasks, transaction_entry) {
		if (task->prio < priority)
			priority = task->prio;
	}
	spin_unlock(&transaction->lock);

	return priority;
}

/*
 * Contention manager function. Return true if a wins the conflict over b.
 * should_sleep indicates that two active transactions were arbitrated by
 * priority and timestamp, rather than by an abort or commit state. It does not
	 * sleep here, in a future acquisition the retry path may use it to wait for
 * the winner to finish before retrying.
 */
bool transaction_contention_manager(struct transaction *a, struct transaction *b, bool *should_sleep) {
	enum transaction_state status_a;
	enum transaction_state status_b;
	int priority_a;
	int priority_b;
	bool priority_winner;

	status_a = transaction_status(a);
	status_b = transaction_status(b);
	priority_a = transaction_best_priority(a);
	priority_b = transaction_best_priority(b);

	// Lower value means higher prio
	if (priority_a < priority_b)
		priority_winner = true;
	else if (priority_a > priority_b)
		priority_winner = false;
	else
		priority_winner = READ_ONCE(a->timestamp) < READ_ONCE(b->timestamp);

	if (should_sleep)
		*should_sleep = false;

	// Aborted transactions have to lose.
	if (status_a == TRANSACTION_ABORTED || status_a == TRANSACTION_ABORTING)
		return false;
	if (status_b == TRANSACTION_ABORTED || status_b == TRANSACTION_ABORTING)
		return true;

	// Committing transactions have to win.
	if (status_a == TRANSACTION_COMMITTING)
		return true;
	if (status_b == TRANSACTION_COMMITTING)
		return false;

	/*
	 * If both are active transactions, arbitrate based on dynamic priority
	 * first and use the timestamp to break ties.
	 */
	if (should_sleep && status_a == TRANSACTION_ACTIVE &&
	    status_b == TRANSACTION_ACTIVE)
		*should_sleep = true;

	return priority_winner;
}
EXPORT_SYMBOL_GPL(transaction_contention_manager);

int begin_transaction(struct transaction *transaction) {
	int ret = 0;

	if (!transaction)
		return -EINVAL;

	spin_lock(&transaction->lock);
	if (!inactive_transaction(transaction) || atomic_read(&transaction->finishing)) {
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
	int ret;

	if (!transaction)
		return -EINVAL;

	spin_lock(&transaction->lock);
	status = atomic_cmpxchg(&transaction->status, TRANSACTION_ACTIVE,
				TRANSACTION_ABORTED);
	switch (status) {
	case TRANSACTION_ACTIVE:
	case TRANSACTION_ABORTED:
	case TRANSACTION_ABORTING:
		ret = 0;
		break;
	case TRANSACTION_INACTIVE:
		ret = -EINVAL;
		break;
	case TRANSACTION_COMMITTING:
		ret = -EBUSY;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	spin_unlock(&transaction->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(abort_transaction);

/* Complete a transaction after either commit or abort cleanup.
   The transaction state returns to inactive, retry/abort flags are
   reset, and any waiters blocked on conflict or sibling completion
   are woken. */
static void terminate_transaction(struct transaction * transaction) {
	spin_lock(&transaction->lock);
	transaction->autoretry = 0;
	transaction->abortWithErr = false;
	atomic_set(&transaction->status, TRANSACTION_INACTIVE);
	atomic_set_release(&transaction->finishing, 0);
	spin_unlock(&transaction->lock);
	wake_up_all(&transaction->losers);
	wake_up_all(&transaction->siblings);
}

/*
 * Finish all objects in kernel address order. Blocking object locks are
 * acquired first, followed by non-blocking locks and transaction-object
 * locks. Ownership and callbacks remain protected until every object has
 * committed or aborted.
 */
static int transaction_finish_workset(struct transaction *transaction) {
	struct txobj_thread_list_node *first_node;
	struct txobj_thread_list_node *node;
	struct skiplist_head *first;
	struct skiplist_head workset;
	enum transaction_state status;
	bool commit = false;
	int validation_ret = 0;
	int ret;

	if (!transaction)
		return -EINVAL;

	skiplist_init_head(&workset);
	spin_lock(&transaction->workset_lock);
	skiplist_splice_init(&transaction->list_list, &workset);
	skiplist_splice_init(&transaction->object_list, &workset);
	spin_unlock(&transaction->workset_lock);

	// Acquire blocking locks in original-object address order.
	skiplist_for_each_entry(node, &workset, workset_list) {
		if (node->lock)
			WARN_ON_ONCE(node->lock(node, 1));
	}

	// Acquire non-blocking locks, then every transaction-object lock.
	first_node = skiplist_entry_safe(skiplist_first(&workset), struct txobj_thread_list_node, workset_list);
	skiplist_for_each_entry(node, &workset, workset_list) {
		if (node->lock)
			WARN_ON_ONCE(node->lock(node, 0));
		if (node == first_node)
			spin_lock(&node->tx_obj->lock);
		else
			spin_lock_nest_lock(&node->tx_obj->lock, &first_node->tx_obj->lock);
	}

	/*
	 * TODO: Optional validation that runs if CONFIG_TX_KSTM_ASSERTIONS was set in TxOS
	 * transactional objects should expose a validate() function to support this feature
	 */
	// if (transaction_status(transaction) == TRANSACTION_ACTIVE) {
	// 	skiplist_for_each_entry(node, &workset, workset_list) {
	// 		if (!node->validate)
	// 			continue;
	// 		callback_ret = node->validate(node);
	// 		if (callback_ret && !validation_ret)
	// 			validation_ret = callback_ret < 0 ? callback_ret : -EINVAL;
	// 	}
	// }

	/*
	 * All object locks are held, so choose the final outcome here. An abort that is
	 * already recorded wins, but after COMMITTING is set, abort can no longer win.
	 */
	spin_lock(&transaction->lock);
	status = transaction_status(transaction);

	if (status == TRANSACTION_ACTIVE && !validation_ret) {
		// Try to commit the active transaction.
		status = atomic_cmpxchg(&transaction->status, TRANSACTION_ACTIVE, TRANSACTION_COMMITTING);
		if (WARN_ON_ONCE(status != TRANSACTION_ACTIVE)) {
			atomic_set(&transaction->status, TRANSACTION_ABORTING);
			transaction->count++;
			ret = -ECANCELED;
		} else {
			commit = true;
			ret = 0;
		}
	} else if (status == TRANSACTION_ACTIVE || status == TRANSACTION_ABORTED) {
		// Abort after validation failure or an earlier abort.
		atomic_set(&transaction->status, TRANSACTION_ABORTING);
		transaction->count++;
		ret = validation_ret ? validation_ret : -ECANCELED;
	} else {
		// Handle an unexpected transaction state.
		WARN_ON_ONCE(1);
		atomic_set(&transaction->status, TRANSACTION_ABORTING);
		transaction->count++;
		ret = -EBUSY;
	}
	spin_unlock(&transaction->lock);

	// Remove ownership before publishing or rolling back object state.
	skiplist_for_each_entry(node, &workset, workset_list) {
		transaction_object_remove_ownership_locked(node);
		if (commit && node->commit)
			WARN_ON_ONCE(node->commit(node));
		else if (!commit && node->abort)
			WARN_ON_ONCE(node->abort(node));
	}

	// Release transaction-object and non-blocking locks in reverse order.
	skiplist_for_each_entry_reverse(node, &workset, workset_list) {
		spin_unlock(&node->tx_obj->lock);
		if (node->unlock)
			WARN_ON_ONCE(node->unlock(node, 0));
	}

	// Release blocking locks in reverse order.
	skiplist_for_each_entry_reverse(node, &workset, workset_list) {
		if (node->unlock)
			WARN_ON_ONCE(node->unlock(node, 1));
	}

	while ((first = skiplist_first(&workset))) {
		node = skiplist_entry(first, struct txobj_thread_list_node, workset_list);
		skiplist_del(&node->workset_list, &workset);
		node->tx = NULL;

		if (node->release)
			WARN_ON_ONCE(node->release(node, 0));
		transaction_workset_node_free(node);
	}

	return ret;
}

/*
 * Claim completion without making the transaction unabortable. The final
 * COMMITTING transition happens only after transaction_finish_workset() has
 * acquired every ordered object lock.
 */
int end_transaction(struct transaction * transaction) {
	enum transaction_state status;
	int ret;

	if (!transaction)
		return -EINVAL;

	spin_lock(&transaction->lock);
	status = transaction_status(transaction);
	if (atomic_read(&transaction->finishing))
		ret = -EBUSY;
	else if (status == TRANSACTION_INACTIVE)
		ret = -EINVAL;
	else if (status == TRANSACTION_ACTIVE || status == TRANSACTION_ABORTED) {
		atomic_set_release(&transaction->finishing, 1);
		ret = 0;
	} else {
		ret = -EBUSY;
	}
	spin_unlock(&transaction->lock);
	if (ret)
		return ret;

	ret = transaction_finish_workset(transaction);
	terminate_transaction(transaction);
	return ret;
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

/* Kernel-side implementation of xbegin() syscall - creates a new transaction
   and attaches the current process/thread to it. */
long transaction_sys_xbegin(void) {
	struct transaction * transaction;
	int ret;

	// Check whether current task is already part of a transaction
	if (current_transaction())
		return -EALREADY;

	transaction = transaction_alloc(GFP_KERNEL);
	if (!transaction)
		return -ENOMEM;

	// Attach current task to the newly created transaction
	ret = transaction_attach_task(transaction, current);
	if (ret) {
		transaction_put(transaction);
		return ret;
	}

	// Mark transaction as active and assign it a timestamp
	ret = begin_transaction(transaction);
	if (ret) {
		transaction_detach_task(current);
		transaction_put(transaction);
		return ret;
	}

	// Drop original allocation reference
	// task attachment keeps the transaction alive until xend(), xabort(), or task exit
	transaction_put(transaction);
	return 0;
}
EXPORT_SYMBOL_GPL(transaction_sys_xbegin);

/* Kernel-side implementation of xend() syscall - ends the current transaction
   for this process / thread and detaches it when cleanup completes. */
long transaction_sys_xend(void) {
	struct transaction *transaction;
	int ret;

	// Get the transaction currently associated with this task
	transaction = current_transaction();
	if (!transaction)
		return -EINVAL;

	// Try to commit / end the current transaction
	ret = end_transaction(transaction);
	// If the transaction ended cleanly, detach the current task
	// ret == 0 means the transaction committed successfully
	// ret == -ECANCELED means the transaction had already been marked aborted
	if (!ret || ret == -ECANCELED)
		transaction_detach_task(current);

	return ret;
}
EXPORT_SYMBOL_GPL(transaction_sys_xend);

/* Kernel-side implementation of xabort() syscall - explicitly aborts the current
   transaction for this process / thread, completes cleanup, and detaches it. */
long transaction_sys_xabort(void) {
	struct transaction *transaction;
	int ret;

	// Get the transaction currently associated with this task
	transaction = current_transaction();
	if (!transaction)
		return -EINVAL;

	// Mark current transaction as aborted
	ret = abort_transaction(transaction);
	if (ret)
		return ret;

	// abort_transaction() only marks the transaction aborted
	// end_transaction() performs the actual transition back to inactive and wakes up any waiting tasks
	ret = end_transaction(transaction);
	if (!ret || ret == -ECANCELED) {
		transaction_detach_task(current);
		return 0;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(transaction_sys_xabort);

// Syscall wrapper for xbegin()
SYSCALL_DEFINE0(xbegin) {
	return transaction_sys_xbegin();
}

// Syscall wrapper for xend()
SYSCALL_DEFINE0(xend) {
	return transaction_sys_xend();
}

// Syscall wrapper for xabort()
SYSCALL_DEFINE0(xabort) {
	return transaction_sys_xabort();
}
