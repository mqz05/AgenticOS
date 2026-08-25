/* TxOS-style transactional list implementation.
   The stable list may contain speculative entries while a transaction is live. 
   Visibility is controlled by tx_list2_iter_next(), and final state is resolved
   by the transaction workset callbacks:
   commit: publish speculative adds and remove committed deletes
   abort: remove speculative adds and restore deleted stable entries */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/transaction.h>
#include <linux/tx_list2.h>

static struct kmem_cache * tx_list2_cachep;

/* Allocate temporary speculative entries used for transactional adds. */
static struct tx_list2_entry *tx_list2_entry_alloc(gfp_t gfp) {
	if (tx_list2_cachep)
		return kmem_cache_alloc(tx_list2_cachep, gfp);
	return kmalloc(sizeof(struct tx_list2_entry), gfp);
}

static void tx_list2_entry_free(struct tx_list2_entry * entry) {
	if (!entry)
		return;
	if (tx_list2_cachep)
		kmem_cache_free(tx_list2_cachep, entry);
	else
		kfree(entry);
}

/* Initialize a transactional list head and its embedded transaction object. */
void INIT_TX_LIST2_HEAD(struct tx_list2_head * head) {
	INIT_LIST_HEAD(&head->head);
	spin_lock_init(&head->lock);
	head->mode = TX_LIST2_NO_TX;
	INIT_LIST_HEAD(&head->spec_list);
	transaction_object_init(&head->transaction_object, TRANSACTION_OBJECT_LIST_HEAD);
}
EXPORT_SYMBOL_GPL(INIT_TX_LIST2_HEAD);

static void INIT_TX_LIST2_ENTRY(struct tx_list2_entry * entry, struct tx_list2_entry_ref * cursor) {
	INIT_LIST_HEAD(&entry->list);
	entry->transactional_state = TX_LIST2_NON_TX;
	entry->transaction = NULL;
	entry->cursor = cursor;
	entry->parent = NULL;
	INIT_LIST_HEAD(&entry->spec);
	entry->embedded = false;
}

/* Initialize a stable list reference. */
void INIT_TX_LIST2_REF(struct tx_list2_entry_ref * ref) {
	INIT_TX_LIST2_ENTRY(&ref->entry, ref);
	ref->sentry = NULL;
	ref->transaction = NULL;
	ref->entry.embedded = true;
}
EXPORT_SYMBOL_GPL(INIT_TX_LIST2_REF);

/* Transaction workset lock callbacks. 
   The transaction engine calls these while finishing so list commit/abort
   runs with the list serialized. */
static int tx_list2_lock(struct txobj_thread_list_node * node, int blocking) {
	struct tx_list2_head *head = node->orig_obj;
	struct txobj_thread_list_node *previous;

	if (!blocking) {
		for (previous = node->ordered_lock_prev; previous; previous = previous->ordered_lock_prev) {
			if (previous->nonblocking_lock_acquired && previous->nonblocking_nest_lock) {
				spin_lock_nest_lock(&head->lock, previous->nonblocking_nest_lock);
				return 0;
			}
		}
		spin_lock(&head->lock);
	}
	return 0;
}

static int tx_list2_unlock(struct txobj_thread_list_node * node, int blocking) {
	struct tx_list2_head *head = node->orig_obj;
	if (!blocking)
		spin_unlock(&head->lock);
	return 0;
}

static void tx_list2_finish_mode(struct txobj_thread_list_node * node) {
	struct tx_list2_head *head = node->orig_obj;
	struct transaction_object *object = node->tx_obj;
	if (node->rw == TRANSACTION_ACCESS_READ_WRITE ||
	    (object->writer == NULL && list_empty(&object->readers)))
		head->mode = TX_LIST2_NO_TX;
}

/* Publish this transaction's speculative list changes.
   Transactional adds are converted into the stable embedded entry.
   Transactional deletes remove the stable entry from the real list. */
static int tx_list2_commit(struct txobj_thread_list_node * node) {
	struct transaction *transaction = node->tx;
	struct tx_list2_head *head = node->orig_obj;
	struct tx_list2_entry *entry;
	struct tx_list2_entry *next;
	bool changed = false;

restart:
	list_for_each_entry_safe(entry, next, &head->spec_list, spec) {
		if (entry->transaction != transaction)
			continue;

		if (entry->transactional_state == TX_LIST2_TRANSACTIONAL_ADD) {
			changed = true;
			struct tx_list2_entry *stable = &entry->cursor->entry;
			bool moved_deleted_stable = false;

			if (!list_empty(&stable->list)) {
				WARN_ON_ONCE(stable->transactional_state != TX_LIST2_TRANSACTIONAL_DEL);
				list_move(&stable->list, &entry->list);
				list_del_init(&stable->spec);
				moved_deleted_stable = true;
			} else {
				list_add(&stable->list, &entry->list);
			}

			list_del_init(&entry->list);
			stable->transactional_state = TX_LIST2_NON_TX;
			stable->transaction = NULL;
			stable->parent = entry->parent;
			entry->cursor->transaction = NULL;
			entry->cursor->sentry = NULL;
			list_del_init(&entry->spec);
			tx_list2_entry_free(entry);
			if (moved_deleted_stable)
				goto restart;
		} else if (entry->transactional_state == TX_LIST2_TRANSACTIONAL_DEL) {
			changed = true;
			list_del_init(&entry->list);
			entry->transactional_state = TX_LIST2_NON_TX;
			entry->transaction = NULL;
			entry->parent = NULL;
			if (entry->cursor->transaction == transaction) {
				entry->cursor->transaction = NULL;
				entry->cursor->sentry = NULL;
			}
			list_del_init(&entry->spec);
			if (!entry->embedded)
				tx_list2_entry_free(entry);
		} else {
			WARN_ON_ONCE(1);
		}
	}

	if (changed)
		node->tx_obj->version++;
	tx_list2_finish_mode(node);
	return 0;
}

/* Roll back this transaction's speculative list changes.
   Transactional adds are removed and freed. 
   Transactional deletes are unmarked, leaving the stable entry in the list. */
static int tx_list2_abort(struct txobj_thread_list_node * node) {
	struct transaction *transaction = node->tx;
	struct tx_list2_head *head = node->orig_obj;
	struct tx_list2_entry *entry;
	struct tx_list2_entry *next;

	list_for_each_entry_safe(entry, next, &head->spec_list, spec) {
		if (entry->transaction != transaction)
			continue;

		if (entry->transactional_state == TX_LIST2_TRANSACTIONAL_DEL) {
			entry->transactional_state = TX_LIST2_NON_TX;
			entry->transaction = NULL;
			if (entry->cursor->transaction == transaction) {
				entry->cursor->transaction = NULL;
				entry->cursor->sentry = NULL;
			}
			list_del_init(&entry->spec);
		} else if (entry->transactional_state == TX_LIST2_TRANSACTIONAL_ADD) {
			list_del_init(&entry->list);
			if (entry->cursor->transaction == transaction) {
				entry->cursor->transaction = NULL;
				entry->cursor->sentry = NULL;
			}
			list_del_init(&entry->spec);
			WARN_ON_ONCE(entry->embedded);
			tx_list2_entry_free(entry);
		} else {
			WARN_ON_ONCE(1);
		}
	}

	tx_list2_finish_mode(node);
	return 0;
}

static int tx_list2_validate(struct txobj_thread_list_node *node)
{
	struct tx_list2_head *head = node->orig_obj;
	int ret = transaction_object_validate(node);

	if (ret)
		return ret;
	if (head->mode == TX_LIST2_NO_TX)
		return -ESTALE;
	return 0;
}

/* Add this list head to the current transaction's list workset.
   This mirrors TxOS list_list behavior: list heads are tracked separately
   from ordinary transaction objects but still use transaction_object_acquire()
   for ownership and conflict handling. */
static int tx_list2_acquire(struct tx_list2_head *head,
			    enum transaction_access_mode list_access,
			    struct transaction **waiter) {
	struct txobj_thread_list_node *node;
	struct transaction *transaction;
	enum transaction_access_mode tx_access = TRANSACTION_ACCESS_READ;
	int next_mode;
	int ret;

	transaction = current_transaction();
	if (waiter)
		*waiter = NULL;
	if (!transaction) {
		if (waiter)
			*waiter = transaction_object_conflict_get(&head->transaction_object,
								  list_access);
		return waiter && *waiter ? -EAGAIN : 0;
	}
	if (transaction_status(transaction) != TRANSACTION_ACTIVE)
		return live_transaction(transaction) ? -ECANCELED : 0;

	if (list_access == TRANSACTION_ACCESS_READ) {
		next_mode = TX_LIST2_R;
		if (head->mode == TX_LIST2_W || head->mode == TX_LIST2_EXCL) {
			next_mode = TX_LIST2_EXCL;
			tx_access = TRANSACTION_ACCESS_READ_WRITE;
		}
	} else {
		next_mode = TX_LIST2_W;
		if (head->mode == TX_LIST2_R || head->mode == TX_LIST2_EXCL) {
			next_mode = TX_LIST2_EXCL;
			tx_access = TRANSACTION_ACCESS_READ_WRITE;
		}
	}

	node = transaction_list_workset_find_object(transaction, &head->transaction_object);
	if (node) {
		if (node->rw < tx_access) {
			ret = transaction_object_acquire(transaction, node, tx_access, NULL);
			if (ret)
				return ret;
			node->rw = tx_access;
		}
		head->mode = next_mode;
		return 0;
	}

	node = transaction_workset_node_alloc(head, head, &head->transaction_object, TRANSACTION_OBJECT_LIST_HEAD, tx_access, GFP_ATOMIC);
	if (!node)
		return -ENOMEM;

	node->lock = tx_list2_lock;
	node->unlock = tx_list2_unlock;
	node->validate = tx_list2_validate;
	node->commit = tx_list2_commit;
	node->abort = tx_list2_abort;
	node->nonblocking_lock_id = &head->lock;
	node->nonblocking_nest_lock = &head->lock;
	ret = transaction_list_workset_add(transaction, node);
	if (ret)
		goto free_node;

	ret = transaction_object_acquire(transaction, node, tx_access, NULL);
	if (!ret) {
		head->mode = next_mode;
		return 0;
	}

	if (transaction_status(transaction) != TRANSACTION_ACTIVE)
		return ret;

	transaction_list_workset_remove(transaction, node);

free_node:
	transaction_workset_node_free(node);
	return ret == -EEXIST ? 0 : ret;
}

/* Acquire write access to a list entry reference.
   If another transaction owns the same entry ref, use the transaction
   contention manager to decide which transaction must abort. */
static int tx_list2_acquire_entry_ref(struct tx_list2_head *head,
				      struct tx_list2_entry_ref *ref) {
	struct transaction *transaction = current_transaction();
	int ret;

	ret = tx_list2_acquire(head, TRANSACTION_ACCESS_READ_WRITE, NULL);
	if (ret || !transaction)
		return ret;

	if (ref->transaction && ref->transaction != transaction) {
		if (transaction_contention_manager(ref->transaction, transaction, NULL)) {
			abort_transaction(transaction);
			return -ECANCELED;
		}

		abort_transaction(ref->transaction);
		ref->transaction = transaction;
		ref->sentry = NULL;
		ref->entry.transactional_state = TX_LIST2_NON_TX;
		ref->entry.transaction = NULL;
	}

	return 0;
}

/* Add an entry while the caller holds head->lock.
   Inside a transaction, allocate a speculative entry and add it to both the 
   visible list and spec_list. Outside a transaction, add the stable entry directly. */
static int tx_list2_add_locked(struct tx_list2_entry_ref *cursor,
			       struct tx_list2_head *head, bool tail,
			       struct transaction **waiter) {
	struct transaction *transaction = current_transaction();
	struct tx_list2_entry *entry;
	int ret;

	if (transaction && transaction_status(transaction) == TRANSACTION_ACTIVE) {
		ret = tx_list2_acquire_entry_ref(head, cursor);
		if (ret)
			return ret;

		entry = tx_list2_entry_alloc(GFP_ATOMIC);
		if (!entry)
			return -ENOMEM;

		INIT_TX_LIST2_ENTRY(entry, cursor);
		entry->transaction = transaction;
		entry->transactional_state = TX_LIST2_TRANSACTIONAL_ADD;
		cursor->sentry = entry;
		cursor->transaction = transaction;
		list_add(&entry->spec, &head->spec_list);
	} else {
		ret = tx_list2_acquire(head, TRANSACTION_ACCESS_READ_WRITE, waiter);
		if (ret)
			return ret;
		entry = &cursor->entry;
		entry->transactional_state = TX_LIST2_NON_TX;
		entry->transaction = NULL;
	}

	if (tail)
		list_add_tail(&entry->list, &head->head);
	else
		list_add(&entry->list, &head->head);
	entry->parent = head;

	return 0;
}

int tx_list2_add(struct tx_list2_entry_ref * cursor, struct tx_list2_head * head) {
	struct transaction *waiter;
	int ret;

retry:
	waiter = NULL;
	spin_lock(&head->lock);
	ret = tx_list2_add_locked(cursor, head, false, &waiter);
	spin_unlock(&head->lock);
	if (waiter) {
		ret = transaction_wait_on_cleanup(waiter);
		if (!ret)
			goto retry;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(tx_list2_add);

int tx_list2_add_tail(struct tx_list2_entry_ref * cursor, struct tx_list2_head * head)
{
	struct transaction *waiter;
	int ret;

retry:
	waiter = NULL;
	spin_lock(&head->lock);
	ret = tx_list2_add_locked(cursor, head, true, &waiter);
	spin_unlock(&head->lock);
	if (waiter) {
		ret = transaction_wait_on_cleanup(waiter);
		if (!ret)
			goto retry;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(tx_list2_add_tail);

/* Delete an entry while the caller holds the list lock. 
   Inside a transaction, stable entries are marked TRANSACTIONAL_DEL so abort
   can restore visibility. 
   If deleting a speculative add from the same transaction, remove and free
   it immediately. */
static int tx_list2_del_locked(struct tx_list2_entry_ref *cursor,
			       struct transaction **waiter) {
	struct transaction *transaction = current_transaction();
	struct tx_list2_entry *entry;
	struct tx_list2_head *head;
	int ret;

	entry = cursor->transaction == transaction && cursor->sentry ? cursor->sentry : &cursor->entry;
	head = entry->parent;
	if (!head)
		return 0;

	if (transaction && transaction_status(transaction) == TRANSACTION_ACTIVE) {
		ret = tx_list2_acquire_entry_ref(head, cursor);
		if (ret)
			return ret;

		if (cursor->sentry)
			entry = cursor->sentry;

		cursor->sentry = NULL;
		cursor->transaction = transaction;
		if (entry->transactional_state == TX_LIST2_TRANSACTIONAL_ADD &&
		    entry->transaction == transaction) {
			list_del_init(&entry->list);
			list_del_init(&entry->spec);
			cursor->transaction = NULL;
			tx_list2_entry_free(entry);
			return 0;
		}

		entry->transactional_state = TX_LIST2_TRANSACTIONAL_DEL;
		entry->transaction = transaction;
		list_add(&entry->spec, &head->spec_list);
		return 0;
	}

	ret = tx_list2_acquire(head, TRANSACTION_ACCESS_READ_WRITE, waiter);
	if (ret)
		return ret;

	list_del_init(&entry->list);
	entry->transactional_state = TX_LIST2_NON_TX;
	entry->transaction = NULL;
	entry->parent = NULL;
	INIT_LIST_HEAD(&entry->spec);
	return 0;
}

int tx_list2_del(struct tx_list2_entry_ref * cursor) {
	struct tx_list2_entry *entry;
	struct tx_list2_head *head;
	struct transaction *waiter;
	int ret;

retry:
	waiter = NULL;
	entry = cursor->transaction == current_transaction() && cursor->sentry ?
		cursor->sentry : &cursor->entry;
	head = entry->parent;
	if (!head)
		return 0;

	spin_lock(&head->lock);
	ret = tx_list2_del_locked(cursor, &waiter);
	spin_unlock(&head->lock);
	if (waiter) {
		ret = transaction_wait_on_cleanup(waiter);
		if (!ret)
			goto retry;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(tx_list2_del);

int tx_list2_move(struct tx_list2_entry_ref * cursor, struct tx_list2_head * head) {
	int ret;

	ret = tx_list2_del(cursor);
	if (ret)
		return ret;

	return tx_list2_add(cursor, head);
}
EXPORT_SYMBOL_GPL(tx_list2_move);

int tx_list2_get_iterator(struct tx_list2_iterator * iter, struct tx_list2_head * head) {
	struct transaction *waiter;
	int ret;

retry:
	waiter = NULL;
	spin_lock(&head->lock);
	ret = tx_list2_acquire(head, TRANSACTION_ACCESS_READ, &waiter);
	if (ret) {
		spin_unlock(&head->lock);
		if (waiter) {
			ret = transaction_wait_on_cleanup(waiter);
			if (!ret)
				goto retry;
		}
		return ret;
	}

	iter->cur = &head->head;
	iter->next = iter->cur->next;
	iter->head = head;
	return 0;
}
EXPORT_SYMBOL_GPL(tx_list2_get_iterator);

void tx_list2_put_iterator(struct tx_list2_iterator * iter) {
	spin_unlock(&iter->head->lock);
}
EXPORT_SYMBOL_GPL(tx_list2_put_iterator);

/* Advance to the next entry visible to the current transaction.
   Rules:
   - stable entries are visible
   - this transaction's speculative adds are visible
   - another transaction's speculative deletes remain visible */
int tx_list2_iter_next(struct tx_list2_iterator * iter) {
	struct transaction *transaction = current_transaction();

	while (iter->next != &iter->head->head) {
		struct tx_list2_entry *entry;

		iter->cur = iter->next;
		iter->next = iter->cur->next;
		entry = list_entry(iter->cur, struct tx_list2_entry, list);
		if (entry->transactional_state == TX_LIST2_NON_TX ||
		    (entry->transactional_state == TX_LIST2_TRANSACTIONAL_ADD &&
		     entry->transaction == transaction) ||
		    (entry->transactional_state == TX_LIST2_TRANSACTIONAL_DEL &&
		     entry->transaction != transaction))
			return 1;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(tx_list2_iter_next);

int tx_list2_empty(struct tx_list2_head * head) {
	struct tx_list2_iterator iter;
	int ret;

	ret = tx_list2_get_iterator(&iter, head);
	if (ret)
		return ret;

	ret = !tx_list2_iter_next(&iter);
	tx_list2_put_iterator(&iter);
	return ret;
}
EXPORT_SYMBOL_GPL(tx_list2_empty);

bool tx_list2_unreferenced(struct tx_list2_entry_ref * ref) {
	return list_empty(&ref->entry.list) && ref->sentry == NULL;
}
EXPORT_SYMBOL_GPL(tx_list2_unreferenced);

static int __init tx_list2_init(void) {
	tx_list2_cachep = kmem_cache_create("tx_list2_entry", sizeof(struct tx_list2_entry), 0, SLAB_HWCACHE_ALIGN, NULL);
	return tx_list2_cachep ? 0 : -ENOMEM;
}
subsys_initcall(tx_list2_init);
