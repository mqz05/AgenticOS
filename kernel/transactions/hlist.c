// SPDX-License-Identifier: GPL-2.0
// Speculative transactional hlist infrastructure.

#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/transaction.h>
#include <linux/tx_hlist.h>
#include <linux/xarray.h>

static_assert(sizeof(struct tx_hlist_bl_head) == sizeof(struct hlist_bl_head));

struct tx_hlist_workset;

struct tx_hlist_spec_entry {
	struct list_head spec;
	enum tx_hlist_state state;
	struct transaction *transaction;
	struct tx_hlist_ref_state *cursor;
	struct tx_hlist_head_state *parent;
	struct tx_hlist_spec_entry *previous;
	struct tx_hlist_workset *workset;
	void *owner;
	tx_hlist_ref_put_t put;
};

struct tx_hlist_workset {
	struct tx_hlist_head_state *state;
	struct tx_hlist_head_callbacks callbacks;
	struct list_head retired;
	bool blocking_lock_acquired;
	bool head_pinned;
	bool lock_acquired;
};

static struct kmem_cache *tx_hlist_entry_cache;
static DEFINE_XARRAY(tx_hlist_bl_states);

static struct tx_hlist_spec_entry *tx_hlist_spec_alloc(void) {
	if (!tx_hlist_entry_cache)
		return NULL;
	return kmem_cache_alloc(tx_hlist_entry_cache, GFP_ATOMIC);
}

static void tx_hlist_spec_free(struct tx_hlist_spec_entry *entry) {
	kmem_cache_free(tx_hlist_entry_cache, entry);
}

static void tx_hlist_state_init(struct tx_hlist_head_state *state, void *owner, bool bit_locked) {
	state->mode = TX_HLIST_NO_TX;
	INIT_LIST_HEAD(&state->spec_list);
	transaction_object_init(&state->transaction_object, TRANSACTION_OBJECT_HLIST_HEAD);
	state->owner = owner;
	state->bit_locked = bit_locked;
	memset(&state->callbacks, 0, sizeof(state->callbacks));
	atomic_set(&state->worksets, 0);
}

// Bitlocked heads keep transaction metadata out of large native hash tables.
static struct tx_hlist_head_state *tx_hlist_bl_state(struct tx_hlist_bl_head *head, bool create) {
	struct tx_hlist_head_state *state;
	struct tx_hlist_head_state *new_state;
	void *old;

	state = xa_load(&tx_hlist_bl_states, (unsigned long)head);
	if (state || !create)
		return state;
	new_state = kmalloc(sizeof(*new_state), GFP_ATOMIC);
	if (!new_state)
		return ERR_PTR(-ENOMEM);
	tx_hlist_state_init(new_state, head, true);
	old = xa_cmpxchg(&tx_hlist_bl_states, (unsigned long)head, NULL, new_state, GFP_ATOMIC);
	if (xa_is_err(old)) {
		kfree(new_state);
		return old;
	}
	if (old) {
		kfree(new_state);
		return old;
	}
	return new_state;
}

static void tx_hlist_ref_init(struct tx_hlist_ref_state *state, void *owner, bool bit_locked) {
	state->sentry = NULL;
	state->transaction = NULL;
	state->parent = NULL;
	state->owner = owner;
	state->bit_locked = bit_locked;
	spin_lock_init(&state->lock);
	state->get = NULL;
	state->put = NULL;
	state->publish_begin = NULL;
	state->publish_end = NULL;
	atomic_set(&state->spec_count, 0);
}

void tx_hlist_head_init(struct tx_hlist_head *head, spinlock_t *lock) {
	INIT_HLIST_HEAD(&head->head);
	spin_lock_init(&head->local_lock);
	head->lock = lock ? lock : &head->local_lock;
	tx_hlist_state_init(&head->state, head, false);
}
EXPORT_SYMBOL_GPL(tx_hlist_head_init);

static int tx_hlist_state_set_callbacks(struct tx_hlist_head_state *state,
					const struct tx_hlist_head_callbacks *callbacks) {
	if (!callbacks || (!callbacks->get != !callbacks->put) || (!callbacks->lock != !callbacks->unlock))
		return -EINVAL;
	if (!!callbacks->lock != !!callbacks->lock_id)
		return -EINVAL;
	spin_lock(&state->transaction_object.lock);
	if (state->transaction_object.writer || !list_empty(&state->transaction_object.readers) ||
	    !list_empty(&state->spec_list) || atomic_read(&state->worksets)) {
		spin_unlock(&state->transaction_object.lock);
		return -EBUSY;
	}
	state->callbacks = *callbacks;
	spin_unlock(&state->transaction_object.lock);
	return 0;
}

int tx_hlist_head_set_callbacks(struct tx_hlist_head *head,
				const struct tx_hlist_head_callbacks *callbacks) {
	int ret;

	spin_lock(head->lock);
	ret = tx_hlist_state_set_callbacks(&head->state, callbacks);
	spin_unlock(head->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(tx_hlist_head_set_callbacks);

void tx_hlist_entry_init(struct tx_hlist_entry_ref *ref) {
	INIT_HLIST_NODE(&ref->node);
	tx_hlist_ref_init(&ref->state, ref, false);
}
EXPORT_SYMBOL_GPL(tx_hlist_entry_init);

void tx_hlist_entry_init_with_lifetime(struct tx_hlist_entry_ref *ref,
				       tx_hlist_ref_get_t get, tx_hlist_ref_put_t put) {
	tx_hlist_entry_init(ref);
	if (WARN_ON_ONCE(!get != !put))
		return;
	ref->state.get = get;
	ref->state.put = put;
}
EXPORT_SYMBOL_GPL(tx_hlist_entry_init_with_lifetime);

void tx_hlist_entry_set_publish_callbacks(struct tx_hlist_entry_ref *ref,
					  tx_hlist_publish_t begin, tx_hlist_publish_t end) {
	if (WARN_ON_ONCE(!begin != !end))
		return;
	ref->state.publish_begin = begin;
	ref->state.publish_end = end;
}
EXPORT_SYMBOL_GPL(tx_hlist_entry_set_publish_callbacks);

void tx_hlist_bl_head_init(struct tx_hlist_bl_head *head) {
	INIT_HLIST_BL_HEAD(&head->head);
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_head_init);

int tx_hlist_bl_head_set_callbacks(struct tx_hlist_bl_head *head,
				   const struct tx_hlist_head_callbacks *callbacks) {
	struct tx_hlist_head_state *state = tx_hlist_bl_state(head, true);
	int ret;

	if (IS_ERR(state))
		return PTR_ERR(state);
	hlist_bl_lock(&head->head);
	ret = tx_hlist_state_set_callbacks(state, callbacks);
	hlist_bl_unlock(&head->head);
	return ret;
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_head_set_callbacks);

int tx_hlist_bl_head_destroy(struct tx_hlist_bl_head *head) {
	struct tx_hlist_head_state *state = tx_hlist_bl_state(head, false);

	if (!state)
		return 0;
	hlist_bl_lock(&head->head);
	spin_lock(&state->transaction_object.lock);
	if (!hlist_bl_empty(&head->head) || state->transaction_object.writer ||
	    !list_empty(&state->transaction_object.readers) || !list_empty(&state->spec_list) ||
	    atomic_read(&state->worksets)) {
		spin_unlock(&state->transaction_object.lock);
		hlist_bl_unlock(&head->head);
		return -EBUSY;
	}
	if (xa_cmpxchg(&tx_hlist_bl_states, (unsigned long)head, state, NULL, GFP_ATOMIC) != state) {
		spin_unlock(&state->transaction_object.lock);
		hlist_bl_unlock(&head->head);
		return -EAGAIN;
	}
	spin_unlock(&state->transaction_object.lock);
	hlist_bl_unlock(&head->head);
	kfree(state);
	return 0;
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_head_destroy);

void tx_hlist_bl_entry_init(struct tx_hlist_bl_entry_ref *ref) {
	INIT_HLIST_BL_NODE(&ref->node);
	tx_hlist_ref_init(&ref->state, ref, true);
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_entry_init);

void tx_hlist_bl_entry_init_with_lifetime(struct tx_hlist_bl_entry_ref *ref,
					  tx_hlist_ref_get_t get, tx_hlist_ref_put_t put) {
	tx_hlist_bl_entry_init(ref);
	if (WARN_ON_ONCE(!get != !put))
		return;
	ref->state.get = get;
	ref->state.put = put;
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_entry_init_with_lifetime);

void tx_hlist_bl_entry_set_publish_callbacks(struct tx_hlist_bl_entry_ref *ref,
					     tx_hlist_publish_t begin, tx_hlist_publish_t end) {
	if (WARN_ON_ONCE(!begin != !end))
		return;
	ref->state.publish_begin = begin;
	ref->state.publish_end = end;
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_entry_set_publish_callbacks);

static struct tx_hlist_head_state *tx_hlist_node_state(struct txobj_thread_list_node *node) {
	return ((struct tx_hlist_workset *)node->shadow_obj)->state;
}

static spinlock_t *tx_hlist_node_spinlock(struct txobj_thread_list_node *node) {
	struct tx_hlist_head_state *state = tx_hlist_node_state(node);

	if (state->bit_locked)
		return NULL;
	return ((struct tx_hlist_head *)state->owner)->lock;
}

static struct tx_hlist_workset *tx_hlist_previous_workset(struct txobj_thread_list_node *node) {
	if (node->type != TRANSACTION_OBJECT_HLIST_HEAD)
		return NULL;
	return node->shadow_obj;
}

static bool tx_hlist_lock_seen(struct txobj_thread_list_node *node, void *lock_id, bool blocking) {
	struct txobj_thread_list_node *previous;

	for (previous = node->ordered_lock_prev; previous; previous = previous->ordered_lock_prev) {
		struct tx_hlist_workset *workset = tx_hlist_previous_workset(previous);

		if (!workset)
			continue;
		if (blocking) {
			if (workset->blocking_lock_acquired && workset->callbacks.lock_id == lock_id)
				return true;
		} else if (tx_hlist_node_spinlock(previous) == lock_id) {
			return true;
		}
	}
	return false;
}

static void tx_hlist_lock_state(struct tx_hlist_head_state *state) {
	if (state->bit_locked) {
		struct tx_hlist_bl_head *head = state->owner;

		hlist_bl_lock(&head->head);
	} else {
		struct tx_hlist_head *head = state->owner;

		spin_lock(head->lock);
	}
}

static void tx_hlist_unlock_state(struct tx_hlist_head_state *state) {
	if (state->bit_locked) {
		struct tx_hlist_bl_head *head = state->owner;

		hlist_bl_unlock(&head->head);
	} else {
		struct tx_hlist_head *head = state->owner;

		spin_unlock(head->lock);
	}
}

static void tx_hlist_assert_locked(struct tx_hlist_head_state *state) {
	if (state->bit_locked)
		WARN_ON_ONCE(!hlist_bl_is_locked(&((struct tx_hlist_bl_head *)state->owner)->head));
	else
		lockdep_assert_held(((struct tx_hlist_head *)state->owner)->lock);
}

static int tx_hlist_lock(struct txobj_thread_list_node *node, int blocking) {
	struct tx_hlist_workset *workset = node->shadow_obj;
	struct txobj_thread_list_node *previous;
	spinlock_t *lock;

	if (blocking) {
		struct tx_hlist_head_callbacks *callbacks = &workset->callbacks;

		workset->blocking_lock_acquired = false;
		if (!callbacks->lock || tx_hlist_lock_seen(node, callbacks->lock_id, true))
			return 0;
		callbacks->lock(callbacks->owner);
		workset->blocking_lock_acquired = true;
		return 0;
	}

	if (!workset->state)
		return 0;
	workset->lock_acquired = false;
	lock = tx_hlist_node_spinlock(node);
	if (lock && tx_hlist_lock_seen(node, lock, false))
		return 0;

	if (!lock) {
		tx_hlist_lock_state(workset->state);
	} else {
		for (previous = node->ordered_lock_prev; previous; previous = previous->ordered_lock_prev) {
			if (previous->type != TRANSACTION_OBJECT_HLIST_HEAD)
				continue;
			spinlock_t *nest_lock = tx_hlist_node_spinlock(previous);

			if (nest_lock) {
				spin_lock_nest_lock(lock, nest_lock);
				goto acquired;
			}
		}
		spin_lock(lock);
	}
acquired:
	workset->lock_acquired = true;
	return 0;
}

static int tx_hlist_unlock(struct txobj_thread_list_node *node, int blocking) {
	struct tx_hlist_workset *workset = node->shadow_obj;

	if (blocking && workset->blocking_lock_acquired) {
		workset->callbacks.unlock(workset->callbacks.owner);
		workset->blocking_lock_acquired = false;
	} else if (!blocking && workset->lock_acquired) {
		tx_hlist_unlock_state(workset->state);
		workset->lock_acquired = false;
	}
	return 0;
}

static bool tx_hlist_ref_unhashed(struct tx_hlist_ref_state *state) {
	if (state->bit_locked) {
		struct tx_hlist_bl_entry_ref *ref = state->owner;

		return hlist_bl_unhashed(&ref->node);
	}
	return hlist_unhashed(&((struct tx_hlist_entry_ref *)state->owner)->node);
}

static void tx_hlist_ref_del_init(struct tx_hlist_ref_state *state) {
	if (state->publish_begin)
		state->publish_begin(state->owner);
	if (state->bit_locked) {
		struct hlist_bl_node *node = &((struct tx_hlist_bl_entry_ref *)state->owner)->node;

		if (!hlist_bl_unhashed(node)) {
			__hlist_bl_del(node);
			WRITE_ONCE(node->pprev, NULL);
		}
	} else {
		hlist_del_init(&((struct tx_hlist_entry_ref *)state->owner)->node);
	}
	if (state->publish_end)
		state->publish_end(state->owner);
}

static void tx_hlist_ref_add_head(struct tx_hlist_ref_state *cursor, struct tx_hlist_head_state *parent) {
	if (cursor->publish_begin)
		cursor->publish_begin(cursor->owner);
	if (cursor->bit_locked) {
		struct tx_hlist_bl_entry_ref *ref = cursor->owner;
		struct tx_hlist_bl_head *head = parent->owner;

		hlist_bl_add_head_rcu(&ref->node, &head->head);
	} else {
		struct tx_hlist_entry_ref *ref = cursor->owner;
		struct tx_hlist_head *head = parent->owner;

		hlist_add_head(&ref->node, &head->head);
	}
	if (cursor->publish_end)
		cursor->publish_end(cursor->owner);
}

static bool tx_hlist_has_reader(struct transaction_object *object, struct transaction *transaction) {
	struct txobj_thread_list_node *reader;

	list_for_each_entry(reader, &object->readers, object_list) {
		if (reader->tx == transaction)
			return true;
	}
	return false;
}

// Rebuild the aggregate list2 mode from the owners that remain.
static void tx_hlist_refresh_mode(struct tx_hlist_head_state *state) {
	struct transaction_object *object = &state->transaction_object;
	struct tx_hlist_spec_entry *entry;

	lockdep_assert_held(&object->lock);
	if (object->writer) {
		state->mode = TX_HLIST_EXCL;
		return;
	}
	if (list_empty(&object->readers)) {
		state->mode = TX_HLIST_NO_TX;
		return;
	}

	state->mode = TX_HLIST_R;
	list_for_each_entry(entry, &state->spec_list, spec) {
		if (tx_hlist_has_reader(object, entry->transaction)) {
			state->mode = TX_HLIST_W;
			return;
		}
	}
}

static void tx_hlist_finish_mode(struct txobj_thread_list_node *node) {
	struct tx_hlist_head_state *state = tx_hlist_node_state(node);

	tx_hlist_refresh_mode(state);
}

static void tx_hlist_retire_spec(struct tx_hlist_spec_entry *entry) {
	list_move_tail(&entry->spec, &entry->workset->retired);
}

// Drop entry and head pins after transaction_finish_workset() releases every object lock.
static int tx_hlist_release(struct txobj_thread_list_node *node, int early) {
	struct tx_hlist_workset *workset = node->shadow_obj;
	struct tx_hlist_spec_entry *entry;
	struct tx_hlist_spec_entry *next;

	WARN_ON_ONCE(early);
	list_for_each_entry_safe(entry, next, &workset->retired, spec) {
		list_del_init(&entry->spec);
		spin_lock(&entry->cursor->lock);
		atomic_dec(&entry->cursor->spec_count);
		spin_unlock(&entry->cursor->lock);
		if (entry->put)
			entry->put(entry->owner);
		tx_hlist_spec_free(entry);
	}
	atomic_dec(&workset->state->worksets);
	if (workset->head_pinned)
		workset->callbacks.put(workset->callbacks.owner);
	kfree(workset);
	return 0;
}

// Apply only the latest operation for a ref; older move records are stale.
static int tx_hlist_commit(struct txobj_thread_list_node *node) {
	struct tx_hlist_head_state *state = tx_hlist_node_state(node);
	struct transaction *transaction = node->tx;
	struct tx_hlist_spec_entry *entry;
	struct tx_hlist_spec_entry *next;

	list_for_each_entry_safe_reverse(entry, next, &state->spec_list, spec) {
		struct tx_hlist_ref_state *cursor;

		if (entry->transaction != transaction)
			continue;
		cursor = entry->cursor;
		spin_lock(&cursor->lock);
		if (cursor->transaction == transaction && cursor->sentry == entry) {
			if (entry->state == TX_HLIST_TRANSACTIONAL_ADD) {
				if (!tx_hlist_ref_unhashed(cursor))
					tx_hlist_ref_del_init(cursor);
				tx_hlist_ref_add_head(cursor, entry->parent);
				cursor->parent = entry->parent;
			} else if (entry->state == TX_HLIST_TRANSACTIONAL_DEL) {
				if (!tx_hlist_ref_unhashed(cursor))
					tx_hlist_ref_del_init(cursor);
				cursor->parent = NULL;
			} else {
				WARN_ON_ONCE(1);
			}
			cursor->transaction = NULL;
			cursor->sentry = NULL;
		}
		tx_hlist_retire_spec(entry);
		spin_unlock(&cursor->lock);
	}
	tx_hlist_finish_mode(node);
	return 0;
}

// Discard speculative records without changing the committed native hlist.
static int tx_hlist_abort(struct txobj_thread_list_node *node) {
	struct tx_hlist_head_state *state = tx_hlist_node_state(node);
	struct transaction *transaction = node->tx;
	struct tx_hlist_spec_entry *entry;
	struct tx_hlist_spec_entry *next;

	list_for_each_entry_safe(entry, next, &state->spec_list, spec) {
		struct tx_hlist_ref_state *cursor;

		if (entry->transaction != transaction)
			continue;
		cursor = entry->cursor;
		spin_lock(&cursor->lock);
		if (cursor->transaction == transaction && cursor->sentry == entry) {
			cursor->transaction = NULL;
			cursor->sentry = NULL;
		}
		tx_hlist_retire_spec(entry);
		spin_unlock(&cursor->lock);
	}
	tx_hlist_finish_mode(node);
	return 0;
}

static int tx_hlist_acquire(struct tx_hlist_head_state *state, enum transaction_access_mode access,
			    struct tx_hlist_workset **result) {
	struct txobj_thread_list_node *node;
	struct transaction *transaction = current_transaction();
	struct tx_hlist_workset *workset;
	struct transaction *winner;
	enum transaction_access_mode tx_access = TRANSACTION_ACCESS_READ;
	enum tx_hlist_mode next_mode;
	int ret;

	if (result)
		*result = NULL;
	if (!transaction) {
		winner = transaction_check_asymmetric_conflict(&state->transaction_object, access, false, &ret);
		if (WARN_ON_ONCE(winner)) {
			transaction_put(winner);
			return -EUCLEAN;
		}
		spin_lock(&state->transaction_object.lock);
		tx_hlist_refresh_mode(state);
		spin_unlock(&state->transaction_object.lock);
		return ret;
	}
	if (transaction_status(transaction) != TRANSACTION_ACTIVE)
		return live_transaction(transaction) || aborting_transaction(transaction) ? -ECANCELED : -EBUSY;

	if (access == TRANSACTION_ACCESS_READ) {
		next_mode = TX_HLIST_R;
		if (state->mode == TX_HLIST_W || state->mode == TX_HLIST_EXCL) {
			next_mode = TX_HLIST_EXCL;
			tx_access = TRANSACTION_ACCESS_READ_WRITE;
		}
	} else {
		next_mode = TX_HLIST_W;
		if (state->mode == TX_HLIST_R || state->mode == TX_HLIST_EXCL) {
			next_mode = TX_HLIST_EXCL;
			tx_access = TRANSACTION_ACCESS_READ_WRITE;
		}
	}

	node = transaction_list_workset_find_object(transaction, &state->transaction_object);
	if (node) {
		if (node->rw < tx_access) {
			ret = transaction_object_acquire(transaction, node, tx_access, NULL);
			if (ret)
				return ret;
			node->rw = tx_access;
		}
		state->mode = next_mode;
		if (result)
			*result = node->shadow_obj;
		return 0;
	}

	workset = kmalloc(sizeof(*workset), GFP_ATOMIC);
	if (!workset)
		return -ENOMEM;
	workset->state = state;
	workset->callbacks = state->callbacks;
	atomic_inc(&state->worksets);
	INIT_LIST_HEAD(&workset->retired);
	workset->blocking_lock_acquired = false;
	workset->head_pinned = false;
	workset->lock_acquired = false;
	if (workset->callbacks.get) {
		workset->callbacks.get(workset->callbacks.owner);
		workset->head_pinned = true;
	}
	node = transaction_workset_node_alloc(workset, state->owner, &state->transaction_object,
					      TRANSACTION_OBJECT_HLIST_HEAD, tx_access, GFP_ATOMIC);
	if (!node) {
		atomic_dec(&state->worksets);
		if (workset->head_pinned)
			workset->callbacks.put(workset->callbacks.owner);
		kfree(workset);
		return -ENOMEM;
	}
	node->lock = tx_hlist_lock;
	node->unlock = tx_hlist_unlock;
	node->commit = tx_hlist_commit;
	node->abort = tx_hlist_abort;
	node->release = tx_hlist_release;
	ret = transaction_list_workset_add(transaction, node);
	if (ret)
		goto free_node;
	ret = transaction_object_acquire(transaction, node, tx_access, NULL);
	if (!ret) {
		state->mode = next_mode;
		if (result)
			*result = workset;
		return 0;
	}
	if (transaction_status(transaction) != TRANSACTION_ACTIVE)
		return ret;
	transaction_list_workset_remove(transaction, node);

free_node:
	transaction_workset_node_free(node);
	atomic_dec(&state->worksets);
	if (workset->head_pinned)
		workset->callbacks.put(workset->callbacks.owner);
	kfree(workset);
	return ret;
}

static int tx_hlist_contend_ref(struct tx_hlist_ref_state *cursor) {
	struct transaction *transaction = current_transaction();
	int ret;

	if (!cursor->transaction || cursor->transaction == transaction)
		return 0;
	if (transaction_contention_manager(cursor->transaction, transaction, NULL)) {
		abort_transaction(transaction);
		return -ECANCELED;
	}
	ret = abort_transaction(cursor->transaction);
	return ret;
}

// Clear an aborted speculative owner before an ordinary update reuses the ref.
static int tx_hlist_take_ref_ordinary(struct tx_hlist_ref_state *cursor) {
	int ret;

	if (!cursor->transaction)
		return 0;
	ret = abort_transaction(cursor->transaction);
	if (ret)
		return ret;
	cursor->transaction = NULL;
	cursor->sentry = NULL;
	return 0;
}

static struct tx_hlist_spec_entry *tx_hlist_publish_spec(struct tx_hlist_spec_entry *entry,
						  struct tx_hlist_ref_state *cursor,
						  struct tx_hlist_head_state *parent,
						  enum tx_hlist_state state,
						  struct tx_hlist_workset *workset) {
	if (!entry)
		return NULL;
	INIT_LIST_HEAD(&entry->spec);
	entry->state = state;
	entry->transaction = current_transaction();
	entry->cursor = cursor;
	entry->parent = parent;
	entry->previous = cursor->transaction == entry->transaction ? cursor->sentry : NULL;
	entry->workset = workset;
	entry->owner = cursor->owner;
	entry->put = cursor->put;
	if (cursor->get)
		cursor->get(cursor->owner);
	list_add(&entry->spec, &parent->spec_list);
	atomic_inc(&cursor->spec_count);
	cursor->transaction = entry->transaction;
	cursor->sentry = entry;
	return entry;
}

static int tx_hlist_add_locked_common(struct tx_hlist_ref_state *cursor,
				      struct tx_hlist_head_state *parent) {
	struct transaction *transaction = current_transaction();
	struct tx_hlist_workset *workset;
	struct tx_hlist_spec_entry *entry;
	int ret;

	if (cursor->bit_locked != parent->bit_locked)
		return -EINVAL;
	tx_hlist_assert_locked(parent);
	ret = tx_hlist_acquire(parent, TRANSACTION_ACCESS_READ_WRITE, &workset);
	if (ret)
		return ret;
	spin_lock(&cursor->lock);
	if (transaction) {
		if (transaction_status(transaction) != TRANSACTION_ACTIVE) {
			ret = -ECANCELED;
			goto out;
		}
		ret = tx_hlist_contend_ref(cursor);
		if (ret)
			goto out;
		if ((cursor->transaction == transaction && cursor->sentry &&
		     cursor->sentry->state == TX_HLIST_TRANSACTIONAL_ADD) ||
		    (!cursor->sentry && !tx_hlist_ref_unhashed(cursor))) {
			ret = -EEXIST;
			goto out;
		}
		entry = tx_hlist_spec_alloc();
		ret = tx_hlist_publish_spec(entry, cursor, parent, TX_HLIST_TRANSACTIONAL_ADD, workset) ? 0 : -ENOMEM;
		goto out;
	}

	ret = tx_hlist_take_ref_ordinary(cursor);
	if (ret)
		goto out;
	if (!tx_hlist_ref_unhashed(cursor)) {
		ret = -EEXIST;
		goto out;
	}
	tx_hlist_ref_add_head(cursor, parent);
	cursor->parent = parent;
out:
	spin_unlock(&cursor->lock);
	return ret;
}

static struct tx_hlist_head_state *tx_hlist_logical_parent_locked(struct tx_hlist_ref_state *cursor) {
	struct transaction *transaction = current_transaction();

	if (transaction && cursor->transaction == transaction && cursor->sentry &&
	    cursor->sentry->state == TX_HLIST_TRANSACTIONAL_ADD)
		return cursor->sentry->parent;
	return cursor->parent;
}

static struct tx_hlist_head_state *tx_hlist_logical_parent(struct tx_hlist_ref_state *cursor) {
	struct tx_hlist_head_state *parent;

	spin_lock(&cursor->lock);
	parent = tx_hlist_logical_parent_locked(cursor);
	spin_unlock(&cursor->lock);
	return parent;
}

static bool tx_hlist_same_lock(struct tx_hlist_head_state *first, struct tx_hlist_head_state *second) {
	if (first == second)
		return true;
	if (first->bit_locked)
		return false;
	return ((struct tx_hlist_head *)first->owner)->lock == ((struct tx_hlist_head *)second->owner)->lock;
}

static void tx_hlist_lock_pair(struct tx_hlist_head_state *first, struct tx_hlist_head_state *second) {
	tx_hlist_lock_state(first);
	if (!second || tx_hlist_same_lock(first, second))
		return;
	if (second->bit_locked)
		tx_hlist_lock_state(second);
	else
		spin_lock_nest_lock(((struct tx_hlist_head *)second->owner)->lock,
				    ((struct tx_hlist_head *)first->owner)->lock);
}

static void tx_hlist_unlock_pair(struct tx_hlist_head_state *first, struct tx_hlist_head_state *second) {
	if (second && !tx_hlist_same_lock(first, second))
		tx_hlist_unlock_state(second);
	tx_hlist_unlock_state(first);
}

static bool tx_hlist_move_protocol_valid(struct tx_hlist_ref_state *cursor,
					 struct tx_hlist_head_state *source,
					 struct tx_hlist_head_state *destination) {
	return source->callbacks.lock && destination->callbacks.lock &&
	       source->callbacks.lock_id == destination->callbacks.lock_id &&
	       cursor->publish_begin && cursor->publish_end;
}

static bool tx_hlist_move_needs_protocol(struct tx_hlist_ref_state *cursor,
					 struct tx_hlist_head_state *source) {
	bool needed;

	if (!cursor->bit_locked || !source)
		return false;
	spin_lock(&cursor->lock);
	needed = !tx_hlist_ref_unhashed(cursor);
	spin_unlock(&cursor->lock);
	return needed;
}

static int tx_hlist_move_protocol_snapshot(struct tx_hlist_head_state *source,
					   struct tx_hlist_head_callbacks *callbacks) {
	spin_lock(&source->transaction_object.lock);
	*callbacks = source->callbacks;
	spin_unlock(&source->transaction_object.lock);
	return callbacks->lock && callbacks->unlock && callbacks->lock_id ? 0 : -EOPNOTSUPP;
}

static bool tx_hlist_move_protocol_unchanged(struct tx_hlist_head_state *source,
					     struct tx_hlist_head_state *destination,
					     const struct tx_hlist_head_callbacks *callbacks) {
	return source->callbacks.owner == callbacks->owner && source->callbacks.lock_id == callbacks->lock_id &&
	       source->callbacks.lock == callbacks->lock && source->callbacks.unlock == callbacks->unlock &&
	       destination->callbacks.lock_id == callbacks->lock_id;
}

// Move commits as one logical operation and cannot leave a deletion after an error.
static int tx_hlist_move_common(struct tx_hlist_ref_state *cursor, struct tx_hlist_head_state *destination,
				bool protocol_held) {
	struct tx_hlist_spec_entry *first_entry = NULL;
	struct tx_hlist_spec_entry *second_entry = NULL;
	struct transaction *transaction = current_transaction();
	struct tx_hlist_workset *source_workset = NULL;
	struct tx_hlist_workset *destination_workset;
	struct tx_hlist_head_state *source;
	struct tx_hlist_head_state *first;
	struct tx_hlist_head_state *second;
	struct tx_hlist_spec_entry *entry;
	struct tx_hlist_head_callbacks protocol;
	bool protocol_acquired;
	int ret;

	if (cursor->bit_locked != destination->bit_locked)
		return -EINVAL;
	if (transaction) {
		first_entry = tx_hlist_spec_alloc();
		second_entry = tx_hlist_spec_alloc();
		if (!first_entry || !second_entry) {
			ret = -ENOMEM;
			goto free_entries;
		}
	}

retry:
	source = tx_hlist_logical_parent(cursor);
	protocol_acquired = false;
	if (!transaction && !protocol_held && tx_hlist_move_needs_protocol(cursor, source)) {
		ret = tx_hlist_move_protocol_snapshot(source, &protocol);
		if (ret)
			goto free_entries;
		protocol.lock(protocol.owner);
		protocol_acquired = true;
	}
	first = source && (unsigned long)source->owner < (unsigned long)destination->owner ? source : destination;
	second = first == destination ? source : destination;
	tx_hlist_lock_pair(first, second);
	if (tx_hlist_logical_parent(cursor) != source) {
		tx_hlist_unlock_pair(first, second);
		if (protocol_acquired)
			protocol.unlock(protocol.owner);
		goto retry;
	}
	// An RCU move needs one outer protocol and per-entry sequence publication.
	if (!protocol_held && tx_hlist_move_needs_protocol(cursor, source) &&
	    !tx_hlist_move_protocol_valid(cursor, source, destination)) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}
	if (protocol_acquired && !tx_hlist_move_protocol_unchanged(source, destination, &protocol)) {
		tx_hlist_unlock_pair(first, second);
		protocol.unlock(protocol.owner);
		goto retry;
	}

	if (source) {
		ret = tx_hlist_acquire(source, TRANSACTION_ACCESS_READ_WRITE, &source_workset);
		if (ret)
			goto unlock;
	}
	if (destination == source) {
		destination_workset = source_workset;
	} else {
		ret = tx_hlist_acquire(destination, TRANSACTION_ACCESS_READ_WRITE, &destination_workset);
		if (ret)
			goto unlock;
	}

	spin_lock(&cursor->lock);
	if (tx_hlist_logical_parent_locked(cursor) != source) {
		spin_unlock(&cursor->lock);
		tx_hlist_unlock_pair(first, second);
		if (protocol_acquired)
			protocol.unlock(protocol.owner);
		goto retry;
	}
	if (transaction) {
		if (transaction_status(transaction) != TRANSACTION_ACTIVE) {
			ret = -ECANCELED;
			goto unlock_cursor;
		}
		ret = tx_hlist_contend_ref(cursor);
		if (ret)
			goto unlock_cursor;

		entry = cursor->sentry;
		if (cursor->transaction == transaction && entry &&
		    entry->state == TX_HLIST_TRANSACTIONAL_ADD) {
			cursor->sentry = entry->previous;
			if (!cursor->sentry)
				cursor->transaction = NULL;
			tx_hlist_retire_spec(entry);
		} else if (source && !(cursor->transaction == transaction && entry &&
				       entry->state == TX_HLIST_TRANSACTIONAL_DEL)) {
			tx_hlist_publish_spec(first_entry, cursor, source, TX_HLIST_TRANSACTIONAL_DEL,
					      source_workset);
			first_entry = NULL;
		}
		tx_hlist_publish_spec(second_entry, cursor, destination, TX_HLIST_TRANSACTIONAL_ADD,
				      destination_workset);
		second_entry = NULL;
		ret = 0;
		goto unlock_cursor;
	}

	ret = tx_hlist_take_ref_ordinary(cursor);
	if (ret)
		goto unlock_cursor;
	if (!tx_hlist_ref_unhashed(cursor))
		tx_hlist_ref_del_init(cursor);
	tx_hlist_ref_add_head(cursor, destination);
	cursor->parent = destination;
	ret = 0;

unlock_cursor:
	spin_unlock(&cursor->lock);
unlock:
	tx_hlist_unlock_pair(first, second);
	if (protocol_acquired)
		protocol.unlock(protocol.owner);
free_entries:
	if (first_entry)
		tx_hlist_spec_free(first_entry);
	if (second_entry)
		tx_hlist_spec_free(second_entry);
	return ret;
}

static int tx_hlist_del_locked_common(struct tx_hlist_ref_state *cursor) {
	struct tx_hlist_head_state *parent = tx_hlist_logical_parent(cursor);
	struct transaction *transaction = current_transaction();
	struct tx_hlist_workset *workset;
	struct tx_hlist_spec_entry *entry;
	int ret;

	if (!parent)
		return 0;
	tx_hlist_assert_locked(parent);
	ret = tx_hlist_acquire(parent, TRANSACTION_ACCESS_READ_WRITE, &workset);
	if (ret)
		return ret;
	spin_lock(&cursor->lock);
	if (tx_hlist_logical_parent_locked(cursor) != parent) {
		ret = -EAGAIN;
		goto out;
	}
	if (transaction) {
		if (transaction_status(transaction) != TRANSACTION_ACTIVE) {
			ret = -ECANCELED;
			goto out;
		}
		ret = tx_hlist_contend_ref(cursor);
		if (ret)
			goto out;
		entry = cursor->sentry;
		if (cursor->transaction == transaction && entry &&
		    entry->state == TX_HLIST_TRANSACTIONAL_ADD) {
			cursor->sentry = entry->previous;
			if (!cursor->sentry)
				cursor->transaction = NULL;
			tx_hlist_retire_spec(entry);
			ret = 0;
			goto out;
		}
		if (cursor->transaction == transaction && entry &&
		    entry->state == TX_HLIST_TRANSACTIONAL_DEL) {
			ret = 0;
			goto out;
		}
		entry = tx_hlist_spec_alloc();
		ret = tx_hlist_publish_spec(entry, cursor, parent, TX_HLIST_TRANSACTIONAL_DEL, workset) ? 0 : -ENOMEM;
		goto out;
	}

	ret = tx_hlist_take_ref_ordinary(cursor);
	if (ret)
		goto out;
	if (!tx_hlist_ref_unhashed(cursor))
		tx_hlist_ref_del_init(cursor);
	cursor->parent = NULL;
out:
	spin_unlock(&cursor->lock);
	return ret;
}

int tx_hlist_add_head_locked(struct tx_hlist_entry_ref *ref, struct tx_hlist_head *head) {
	return tx_hlist_add_locked_common(&ref->state, &head->state);
}
EXPORT_SYMBOL_GPL(tx_hlist_add_head_locked);

int tx_hlist_add_head(struct tx_hlist_entry_ref *ref, struct tx_hlist_head *head) {
	int ret;

	spin_lock(head->lock);
	ret = tx_hlist_add_head_locked(ref, head);
	spin_unlock(head->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(tx_hlist_add_head);

int tx_hlist_del_locked(struct tx_hlist_entry_ref *ref) {
	return tx_hlist_del_locked_common(&ref->state);
}
EXPORT_SYMBOL_GPL(tx_hlist_del_locked);

int tx_hlist_del(struct tx_hlist_entry_ref *ref) {
	struct tx_hlist_head_state *parent;
	struct tx_hlist_head *head;
	int ret;

	do {
		parent = tx_hlist_logical_parent(&ref->state);
		if (!parent)
			return 0;
		head = parent->owner;
		spin_lock(head->lock);
		ret = tx_hlist_del_locked(ref);
		spin_unlock(head->lock);
	} while (ret == -EAGAIN);
	return ret;
}
EXPORT_SYMBOL_GPL(tx_hlist_del);

int tx_hlist_move(struct tx_hlist_entry_ref *ref, struct tx_hlist_head *head) {
	return tx_hlist_move_common(&ref->state, &head->state, false);
}
EXPORT_SYMBOL_GPL(tx_hlist_move);

bool tx_hlist_unreferenced(struct tx_hlist_entry_ref *ref) {
	return hlist_unhashed(&ref->node) && !atomic_read(&ref->state.spec_count);
}
EXPORT_SYMBOL_GPL(tx_hlist_unreferenced);

int tx_hlist_bl_add_head_locked(struct tx_hlist_bl_entry_ref *ref, struct tx_hlist_bl_head *head) {
	struct tx_hlist_head_state *state = tx_hlist_bl_state(head, true);

	if (IS_ERR(state))
		return PTR_ERR(state);
	return tx_hlist_add_locked_common(&ref->state, state);
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_add_head_locked);

int tx_hlist_bl_add_head(struct tx_hlist_bl_entry_ref *ref, struct tx_hlist_bl_head *head) {
	int ret;

	hlist_bl_lock(&head->head);
	ret = tx_hlist_bl_add_head_locked(ref, head);
	hlist_bl_unlock(&head->head);
	return ret;
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_add_head);

int tx_hlist_bl_del_locked(struct tx_hlist_bl_entry_ref *ref) {
	return tx_hlist_del_locked_common(&ref->state);
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_del_locked);

int tx_hlist_bl_del(struct tx_hlist_bl_entry_ref *ref) {
	struct tx_hlist_head_state *parent;
	struct tx_hlist_bl_head *head;
	int ret;

	do {
		parent = tx_hlist_logical_parent(&ref->state);
		if (!parent)
			return 0;
		head = parent->owner;
		hlist_bl_lock(&head->head);
		ret = tx_hlist_bl_del_locked(ref);
		hlist_bl_unlock(&head->head);
	} while (ret == -EAGAIN);
	return ret;
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_del);

int tx_hlist_bl_move(struct tx_hlist_bl_entry_ref *ref, struct tx_hlist_bl_head *head) {
	struct tx_hlist_head_state *state = tx_hlist_bl_state(head, true);

	if (IS_ERR(state))
		return PTR_ERR(state);
	return tx_hlist_move_common(&ref->state, state, false);
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_move);

int tx_hlist_bl_move_under_protocol(struct tx_hlist_bl_entry_ref *ref, struct tx_hlist_bl_head *head) {
	struct tx_hlist_head_state *state;

	state = tx_hlist_bl_state(head, true);
	if (IS_ERR(state))
		return PTR_ERR(state);
	return tx_hlist_move_common(&ref->state, state, !current_transaction());
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_move_under_protocol);

bool tx_hlist_bl_unreferenced(struct tx_hlist_bl_entry_ref *ref) {
	return hlist_bl_unhashed(&ref->node) && !atomic_read(&ref->state.spec_count);
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_unreferenced);

int tx_hlist_get_iterator(struct tx_hlist_iterator *iter, struct tx_hlist_head *head) {
	int ret;

	spin_lock(head->lock);
	ret = tx_hlist_acquire(&head->state, TRANSACTION_ACCESS_READ, NULL);
	if (ret) {
		spin_unlock(head->lock);
		return ret;
	}
	iter->head = head;
	iter->spec_next = head->state.spec_list.next;
	iter->stable_next = head->head.first;
	iter->cursor = NULL;
	iter->stable = false;
	return 0;
}
EXPORT_SYMBOL_GPL(tx_hlist_get_iterator);

void tx_hlist_put_iterator(struct tx_hlist_iterator *iter) {
	spin_unlock(iter->head->lock);
}
EXPORT_SYMBOL_GPL(tx_hlist_put_iterator);

bool tx_hlist_iter_next(struct tx_hlist_iterator *iter) {
	struct transaction *transaction = current_transaction();
	struct tx_hlist_spec_entry *entry;
	struct tx_hlist_entry_ref *ref;

	while (!iter->stable && iter->spec_next != &iter->head->state.spec_list) {
		bool visible;

		entry = list_entry(iter->spec_next, struct tx_hlist_spec_entry, spec);
		iter->spec_next = iter->spec_next->next;
		spin_lock(&entry->cursor->lock);
		visible = entry->state == TX_HLIST_TRANSACTIONAL_ADD && entry->transaction == transaction &&
			  entry->cursor->transaction == transaction && entry->cursor->sentry == entry;
		if (visible)
			iter->cursor = entry->cursor->owner;
		spin_unlock(&entry->cursor->lock);
		if (visible)
			return true;
	}
	iter->stable = true;
	while (iter->stable_next) {
		bool hidden;

		ref = hlist_entry(iter->stable_next, struct tx_hlist_entry_ref, node);
		iter->stable_next = iter->stable_next->next;
		spin_lock(&ref->state.lock);
		hidden = transaction && ref->state.transaction == transaction && ref->state.sentry;
		spin_unlock(&ref->state.lock);
		if (hidden)
			continue;
		iter->cursor = ref;
		return true;
	}
	return false;
}
EXPORT_SYMBOL_GPL(tx_hlist_iter_next);

int tx_hlist_empty(struct tx_hlist_head *head) {
	struct tx_hlist_iterator iter;
	int ret;

	ret = tx_hlist_get_iterator(&iter, head);
	if (ret)
		return ret;
	ret = !tx_hlist_iter_next(&iter);
	tx_hlist_put_iterator(&iter);
	return ret;
}
EXPORT_SYMBOL_GPL(tx_hlist_empty);

int tx_hlist_bl_get_iterator(struct tx_hlist_bl_iterator *iter, struct tx_hlist_bl_head *head) {
	struct tx_hlist_head_state *state;
	int ret;

	hlist_bl_lock(&head->head);
	state = tx_hlist_bl_state(head, true);
	if (IS_ERR(state)) {
		hlist_bl_unlock(&head->head);
		return PTR_ERR(state);
	}
	ret = tx_hlist_acquire(state, TRANSACTION_ACCESS_READ, NULL);
	if (ret) {
		hlist_bl_unlock(&head->head);
		return ret;
	}
	iter->head = head;
	iter->state = state;
	iter->spec_next = state->spec_list.next;
	iter->stable_next = hlist_bl_first(&head->head);
	iter->cursor = NULL;
	iter->stable = false;
	return 0;
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_get_iterator);

void tx_hlist_bl_put_iterator(struct tx_hlist_bl_iterator *iter) {
	hlist_bl_unlock(&iter->head->head);
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_put_iterator);

bool tx_hlist_bl_iter_next(struct tx_hlist_bl_iterator *iter) {
	struct transaction *transaction = current_transaction();
	struct tx_hlist_spec_entry *entry;
	struct tx_hlist_bl_entry_ref *ref;

	while (!iter->stable && iter->spec_next != &iter->state->spec_list) {
		bool visible;

		entry = list_entry(iter->spec_next, struct tx_hlist_spec_entry, spec);
		iter->spec_next = iter->spec_next->next;
		spin_lock(&entry->cursor->lock);
		visible = entry->state == TX_HLIST_TRANSACTIONAL_ADD && entry->transaction == transaction &&
			  entry->cursor->transaction == transaction && entry->cursor->sentry == entry;
		if (visible)
			iter->cursor = entry->cursor->owner;
		spin_unlock(&entry->cursor->lock);
		if (visible)
			return true;
	}
	iter->stable = true;
	while (iter->stable_next) {
		bool hidden;

		ref = hlist_bl_entry(iter->stable_next, struct tx_hlist_bl_entry_ref, node);
		iter->stable_next = iter->stable_next->next;
		spin_lock(&ref->state.lock);
		hidden = transaction && ref->state.transaction == transaction && ref->state.sentry;
		spin_unlock(&ref->state.lock);
		if (hidden)
			continue;
		iter->cursor = ref;
		return true;
	}
	return false;
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_iter_next);

int tx_hlist_bl_empty(struct tx_hlist_bl_head *head) {
	struct tx_hlist_bl_iterator iter;
	int ret;

	ret = tx_hlist_bl_get_iterator(&iter, head);
	if (ret)
		return ret;
	ret = !tx_hlist_bl_iter_next(&iter);
	tx_hlist_bl_put_iterator(&iter);
	return ret;
}
EXPORT_SYMBOL_GPL(tx_hlist_bl_empty);

static int __init tx_hlist_init(void) {
	tx_hlist_entry_cache = kmem_cache_create("tx_hlist_entry", sizeof(struct tx_hlist_spec_entry),
						 0, SLAB_HWCACHE_ALIGN, NULL);
	return tx_hlist_entry_cache ? 0 : -ENOMEM;
}
subsys_initcall(tx_hlist_init);
