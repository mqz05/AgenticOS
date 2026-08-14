/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TX_HLIST_H
#define _LINUX_TX_HLIST_H

#include <linux/errno.h>
#include <linux/list.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/transaction.h>
#include <linux/list_bl.h>
#include <linux/rculist_bl.h>

typedef void (*tx_hlist_ref_get_t)(void *owner);
typedef void (*tx_hlist_ref_put_t)(void *owner);
typedef void (*tx_hlist_lock_t)(void *owner);
typedef void (*tx_hlist_unlock_t)(void *owner);
typedef void (*tx_hlist_publish_t)(void *owner);

struct tx_hlist_head_callbacks {
	void *owner;
	// Head pins cover the workset lifetime; get must not sleep.
	tx_hlist_ref_get_t get;
	tx_hlist_ref_put_t put;
	// A shared blocking lock may cover publication across several heads.
	void *lock_id;
	tx_hlist_lock_t lock;
	tx_hlist_unlock_t unlock;
};

#ifdef CONFIG_TRANSACTIONS

enum tx_hlist_state {
	TX_HLIST_NON_TX,
	TX_HLIST_TRANSACTIONAL_ADD,
	TX_HLIST_TRANSACTIONAL_DEL,
};

enum tx_hlist_mode {
	TX_HLIST_NO_TX,
	TX_HLIST_R,
	TX_HLIST_W,
	TX_HLIST_EXCL,
};

struct tx_hlist_spec_entry;
struct tx_hlist_head_state;

// Transactional metadata attached to a stable native node.
// The containing object must serialize moves and remain alive while spec_count is nonzero.
struct tx_hlist_ref_state {
	struct tx_hlist_spec_entry *sentry;
	struct transaction *transaction;
	struct tx_hlist_head_state *parent;
	void *owner;
	bool bit_locked;
	spinlock_t lock;
	tx_hlist_ref_get_t get;
	tx_hlist_ref_put_t put;
	tx_hlist_publish_t publish_begin;
	tx_hlist_publish_t publish_end;
	// Outstanding logs keep the stable entry logically referenced.
	atomic_t spec_count;
};

// Per-head conflict state and speculative operation log.
struct tx_hlist_head_state {
	enum tx_hlist_mode mode;
	struct list_head spec_list;
	struct transaction_object transaction_object;
	void *owner;
	bool bit_locked;
	struct tx_hlist_head_callbacks callbacks;
	atomic_t worksets;
};

// A committed hlist node and its transactional reference state.
struct tx_hlist_entry_ref {
	struct hlist_node node;
	struct tx_hlist_ref_state state;
};

// A normal head; every native node must belong to a tx_hlist_entry_ref.
struct tx_hlist_head {
	struct hlist_head head;
	spinlock_t local_lock;
	spinlock_t *lock;
	struct tx_hlist_head_state state;
};

// Bitlocked links use RCU publication; moves require blocking and entry publication callbacks.
struct tx_hlist_bl_entry_ref {
	struct hlist_bl_node node;
	struct tx_hlist_ref_state state;
};

struct tx_hlist_bl_head {
	// Transaction metadata is held in a lazy sidecar to preserve native head size.
	struct hlist_bl_head head;
};

// Iterators merge this transaction's additions with committed native nodes.
struct tx_hlist_iterator {
	struct tx_hlist_head *head;
	struct list_head *spec_next;
	struct hlist_node *stable_next;
	struct tx_hlist_entry_ref *cursor;
	bool stable;
};

struct tx_hlist_bl_iterator {
	struct tx_hlist_bl_head *head;
	struct tx_hlist_head_state *state;
	struct list_head *spec_next;
	struct hlist_bl_node *stable_next;
	struct tx_hlist_bl_entry_ref *cursor;
	bool stable;
};

void tx_hlist_head_init(struct tx_hlist_head *head, spinlock_t *lock);
// Configure head and entry callbacks before exposing them to concurrent operations.
int tx_hlist_head_set_callbacks(struct tx_hlist_head *head,
				const struct tx_hlist_head_callbacks *callbacks);
void tx_hlist_entry_init(struct tx_hlist_entry_ref *ref);
// Lifetime callbacks must be matched, get must not sleep.
void tx_hlist_entry_init_with_lifetime(struct tx_hlist_entry_ref *ref, tx_hlist_ref_get_t get, tx_hlist_ref_put_t put);
// Publication callbacks update any container sequence protocol around native link changes.
void tx_hlist_entry_set_publish_callbacks(struct tx_hlist_entry_ref *ref, tx_hlist_publish_t begin, tx_hlist_publish_t end);
int tx_hlist_add_head(struct tx_hlist_entry_ref *ref, struct tx_hlist_head *head);
int tx_hlist_add_head_locked(struct tx_hlist_entry_ref *ref, struct tx_hlist_head *head);
int tx_hlist_del(struct tx_hlist_entry_ref *ref);
int tx_hlist_del_locked(struct tx_hlist_entry_ref *ref);
int tx_hlist_move(struct tx_hlist_entry_ref *ref, struct tx_hlist_head *head);
int tx_hlist_empty(struct tx_hlist_head *head);
bool tx_hlist_unreferenced(struct tx_hlist_entry_ref *ref);
int tx_hlist_get_iterator(struct tx_hlist_iterator *iter, struct tx_hlist_head *head);
void tx_hlist_put_iterator(struct tx_hlist_iterator *iter);
bool tx_hlist_iter_next(struct tx_hlist_iterator *iter);

void tx_hlist_bl_head_init(struct tx_hlist_bl_head *head);
int tx_hlist_bl_head_set_callbacks(struct tx_hlist_bl_head *head, const struct tx_hlist_head_callbacks *callbacks);
// Destroy is valid only after callers and entries stop referencing the empty head.
int tx_hlist_bl_head_destroy(struct tx_hlist_bl_head *head);
void tx_hlist_bl_entry_init(struct tx_hlist_bl_entry_ref *ref);
// Pins are released after all locks, put must provide RCU-safe destruction.
void tx_hlist_bl_entry_init_with_lifetime(struct tx_hlist_bl_entry_ref *ref, tx_hlist_ref_get_t get, tx_hlist_ref_put_t put);
void tx_hlist_bl_entry_set_publish_callbacks(struct tx_hlist_bl_entry_ref *ref, tx_hlist_publish_t begin, tx_hlist_publish_t end);
int tx_hlist_bl_add_head(struct tx_hlist_bl_entry_ref *ref, struct tx_hlist_bl_head *head);
int tx_hlist_bl_add_head_locked(struct tx_hlist_bl_entry_ref *ref, struct tx_hlist_bl_head *head);
int tx_hlist_bl_del(struct tx_hlist_bl_entry_ref *ref);
int tx_hlist_bl_del_locked(struct tx_hlist_bl_entry_ref *ref);
int tx_hlist_bl_move(struct tx_hlist_bl_entry_ref *ref, struct tx_hlist_bl_head *head);
// The caller holds the complete outer publication protocol; bucket locks are acquired here.
// Transactional moves reacquire the registered protocol when the workset finishes.
int tx_hlist_bl_move_under_protocol(struct tx_hlist_bl_entry_ref *ref, struct tx_hlist_bl_head *head);
int tx_hlist_bl_empty(struct tx_hlist_bl_head *head);
bool tx_hlist_bl_unreferenced(struct tx_hlist_bl_entry_ref *ref);
int tx_hlist_bl_get_iterator(struct tx_hlist_bl_iterator *iter, struct tx_hlist_bl_head *head);
void tx_hlist_bl_put_iterator(struct tx_hlist_bl_iterator *iter);
bool tx_hlist_bl_iter_next(struct tx_hlist_bl_iterator *iter);

#define tx_hlist_iter_entry(iter, type, member) container_of((iter)->cursor, type, member)
#define tx_hlist_bl_iter_entry(iter, type, member) container_of((iter)->cursor, type, member)

#else

struct tx_hlist_head;
struct tx_hlist_bl_head;

struct tx_hlist_entry_ref {
	struct hlist_node node;
	struct tx_hlist_head *parent;
};

struct tx_hlist_head {
	struct hlist_head head;
	spinlock_t local_lock;
	spinlock_t *lock;
};

struct tx_hlist_bl_entry_ref {
	struct hlist_bl_node node;
	struct tx_hlist_bl_head *parent;
};

struct tx_hlist_bl_head {
	struct hlist_bl_head head;
};

struct tx_hlist_iterator {
	struct tx_hlist_head *head;
	struct hlist_node *next;
	struct tx_hlist_entry_ref *cursor;
};

struct tx_hlist_bl_iterator {
	struct tx_hlist_bl_head *head;
	struct hlist_bl_node *next;
	struct tx_hlist_bl_entry_ref *cursor;
};

static inline void tx_hlist_head_init(struct tx_hlist_head *head, spinlock_t *lock) {
	INIT_HLIST_HEAD(&head->head);
	spin_lock_init(&head->local_lock);
	head->lock = lock ? lock : &head->local_lock;
}

static inline int tx_hlist_head_set_callbacks(struct tx_hlist_head *head,
					      const struct tx_hlist_head_callbacks *callbacks) {
	return callbacks && !(!callbacks->get != !callbacks->put) &&
	       !(!callbacks->lock != !callbacks->unlock) &&
	       (!!callbacks->lock == !!callbacks->lock_id) ? 0 : -EINVAL;
}

static inline void tx_hlist_entry_init(struct tx_hlist_entry_ref *ref) {
	INIT_HLIST_NODE(&ref->node);
	ref->parent = NULL;
}

static inline void tx_hlist_entry_init_with_lifetime(struct tx_hlist_entry_ref *ref,
					      tx_hlist_ref_get_t get, tx_hlist_ref_put_t put) {
	tx_hlist_entry_init(ref);
}

static inline void tx_hlist_entry_set_publish_callbacks(struct tx_hlist_entry_ref *ref,
						 tx_hlist_publish_t begin, tx_hlist_publish_t end) { }

static inline int tx_hlist_add_head_locked(struct tx_hlist_entry_ref *ref, struct tx_hlist_head *head) {
	if (!hlist_unhashed(&ref->node))
		return -EEXIST;
	hlist_add_head(&ref->node, &head->head);
	ref->parent = head;
	return 0;
}

static inline int tx_hlist_add_head(struct tx_hlist_entry_ref *ref, struct tx_hlist_head *head) {
	int ret;

	spin_lock(head->lock);
	ret = tx_hlist_add_head_locked(ref, head);
	spin_unlock(head->lock);
	return ret;
}

static inline int tx_hlist_del_locked(struct tx_hlist_entry_ref *ref) {
	if (!hlist_unhashed(&ref->node))
		hlist_del_init(&ref->node);
	ref->parent = NULL;
	return 0;
}

static inline int tx_hlist_del(struct tx_hlist_entry_ref *ref) {
	struct tx_hlist_head *head = ref->parent;
	int ret;

	if (!head)
		return 0;
	spin_lock(head->lock);
	ret = tx_hlist_del_locked(ref);
	spin_unlock(head->lock);
	return ret;
}

static inline int tx_hlist_move(struct tx_hlist_entry_ref *ref, struct tx_hlist_head *head) {
	struct tx_hlist_head *source = ref->parent;
	struct tx_hlist_head *first = source && (unsigned long)source < (unsigned long)head ? source : head;
	struct tx_hlist_head *second = source == head ? NULL : (first == head ? source : head);

	spin_lock(first->lock);
	if (second && second->lock != first->lock)
		spin_lock_nest_lock(second->lock, first->lock);
	if (!hlist_unhashed(&ref->node))
		hlist_del_init(&ref->node);
	hlist_add_head(&ref->node, &head->head);
	ref->parent = head;
	if (second && second->lock != first->lock)
		spin_unlock(second->lock);
	spin_unlock(first->lock);
	return 0;
}

static inline bool tx_hlist_unreferenced(struct tx_hlist_entry_ref *ref) {
	return hlist_unhashed(&ref->node);
}

static inline int tx_hlist_empty(struct tx_hlist_head *head) {
	int empty;

	spin_lock(head->lock);
	empty = hlist_empty(&head->head);
	spin_unlock(head->lock);
	return empty;
}

static inline int tx_hlist_get_iterator(struct tx_hlist_iterator *iter, struct tx_hlist_head *head) {
	spin_lock(head->lock);
	iter->head = head;
	iter->next = head->head.first;
	return 0;
}

static inline void tx_hlist_put_iterator(struct tx_hlist_iterator *iter) {
	spin_unlock(iter->head->lock);
}

static inline bool tx_hlist_iter_next(struct tx_hlist_iterator *iter) {
	if (!iter->next)
		return false;
	iter->cursor = hlist_entry(iter->next, struct tx_hlist_entry_ref, node);
	iter->next = iter->next->next;
	return true;
}

static inline void tx_hlist_bl_head_init(struct tx_hlist_bl_head *head) {
	INIT_HLIST_BL_HEAD(&head->head);
}

static inline int tx_hlist_bl_head_set_callbacks(struct tx_hlist_bl_head *head,
						 const struct tx_hlist_head_callbacks *callbacks) {
	return callbacks && !(!callbacks->get != !callbacks->put) &&
	       !(!callbacks->lock != !callbacks->unlock) &&
	       (!!callbacks->lock == !!callbacks->lock_id) ? 0 : -EINVAL;
}

static inline int tx_hlist_bl_head_destroy(struct tx_hlist_bl_head *head) {
	int ret;

	hlist_bl_lock(&head->head);
	ret = hlist_bl_empty(&head->head) ? 0 : -EBUSY;
	hlist_bl_unlock(&head->head);
	return ret;
}

static inline void tx_hlist_bl_entry_init(struct tx_hlist_bl_entry_ref *ref) {
	INIT_HLIST_BL_NODE(&ref->node);
	ref->parent = NULL;
}

static inline void tx_hlist_bl_entry_init_with_lifetime(struct tx_hlist_bl_entry_ref *ref,
						 tx_hlist_ref_get_t get, tx_hlist_ref_put_t put) {
	tx_hlist_bl_entry_init(ref);
}

static inline void tx_hlist_bl_entry_set_publish_callbacks(struct tx_hlist_bl_entry_ref *ref,
						    tx_hlist_publish_t begin, tx_hlist_publish_t end) { }

static inline int tx_hlist_bl_add_head_locked(struct tx_hlist_bl_entry_ref *ref,
					      struct tx_hlist_bl_head *head) {
	if (!hlist_bl_unhashed(&ref->node))
		return -EEXIST;
	hlist_bl_add_head_rcu(&ref->node, &head->head);
	ref->parent = head;
	return 0;
}

static inline int tx_hlist_bl_add_head(struct tx_hlist_bl_entry_ref *ref, struct tx_hlist_bl_head *head) {
	int ret;

	hlist_bl_lock(&head->head);
	ret = tx_hlist_bl_add_head_locked(ref, head);
	hlist_bl_unlock(&head->head);
	return ret;
}

static inline int tx_hlist_bl_del_locked(struct tx_hlist_bl_entry_ref *ref) {
	if (!hlist_bl_unhashed(&ref->node)) {
		__hlist_bl_del(&ref->node);
		WRITE_ONCE(ref->node.pprev, NULL);
	}
	ref->parent = NULL;
	return 0;
}

static inline int tx_hlist_bl_del(struct tx_hlist_bl_entry_ref *ref) {
	struct tx_hlist_bl_head *head = ref->parent;
	int ret;

	if (!head)
		return 0;
	hlist_bl_lock(&head->head);
	ret = tx_hlist_bl_del_locked(ref);
	hlist_bl_unlock(&head->head);
	return ret;
}

static inline int tx_hlist_bl_move(struct tx_hlist_bl_entry_ref *ref, struct tx_hlist_bl_head *head) {
	if (ref->parent)
		return -EOPNOTSUPP;
	return tx_hlist_bl_add_head(ref, head);
}

static inline int tx_hlist_bl_move_under_protocol(struct tx_hlist_bl_entry_ref *ref,
						   struct tx_hlist_bl_head *head) {
	struct tx_hlist_bl_head *source = ref->parent;
	struct tx_hlist_bl_head *first = source && (unsigned long)source < (unsigned long)head ? source : head;
	struct tx_hlist_bl_head *second = source == head ? NULL : (first == head ? source : head);

	hlist_bl_lock(&first->head);
	if (second)
		hlist_bl_lock(&second->head);
	tx_hlist_bl_del_locked(ref);
	hlist_bl_add_head_rcu(&ref->node, &head->head);
	ref->parent = head;
	if (second)
		hlist_bl_unlock(&second->head);
	hlist_bl_unlock(&first->head);
	return 0;
}

static inline bool tx_hlist_bl_unreferenced(struct tx_hlist_bl_entry_ref *ref) {
	return hlist_bl_unhashed(&ref->node);
}

static inline int tx_hlist_bl_empty(struct tx_hlist_bl_head *head) {
	int empty;

	hlist_bl_lock(&head->head);
	empty = hlist_bl_empty(&head->head);
	hlist_bl_unlock(&head->head);
	return empty;
}

static inline int tx_hlist_bl_get_iterator(struct tx_hlist_bl_iterator *iter, struct tx_hlist_bl_head *head) {
	hlist_bl_lock(&head->head);
	iter->head = head;
	iter->next = hlist_bl_first(&head->head);
	return 0;
}

static inline void tx_hlist_bl_put_iterator(struct tx_hlist_bl_iterator *iter) {
	hlist_bl_unlock(&iter->head->head);
}

static inline bool tx_hlist_bl_iter_next(struct tx_hlist_bl_iterator *iter) {
	if (!iter->next)
		return false;
	iter->cursor = hlist_bl_entry(iter->next, struct tx_hlist_bl_entry_ref, node);
	iter->next = iter->next->next;
	return true;
}

#define tx_hlist_iter_entry(iter, type, member) container_of((iter)->cursor, type, member)
#define tx_hlist_bl_iter_entry(iter, type, member) container_of((iter)->cursor, type, member)

#endif

// Temporary raw checkpoints retained until dentry relations use the speculative API.
struct tx_hlist_node_snapshot {
	struct hlist_node *next;
	struct hlist_node **pprev;
};

struct tx_hlist_bl_node_snapshot {
	struct hlist_bl_node *next;
	struct hlist_bl_node **pprev;
};

static inline void tx_hlist_snapshot(struct hlist_node *node, struct tx_hlist_node_snapshot *snapshot) {
	snapshot->next = node->next;
	snapshot->pprev = node->pprev;
}

static inline void tx_hlist_bl_snapshot(struct hlist_bl_node *node, struct tx_hlist_bl_node_snapshot *snapshot) {
	snapshot->next = node->next;
	snapshot->pprev = node->pprev;
}

static inline void tx_hlist_restore(struct hlist_node *node, const struct tx_hlist_node_snapshot *snapshot) {
	if (!hlist_unhashed(node))
		__hlist_del(node);

	node->next = snapshot->next;
	node->pprev = snapshot->pprev;
	if (snapshot->pprev) {
		WRITE_ONCE(*snapshot->pprev, node);
		if (snapshot->next)
			snapshot->next->pprev = &node->next;
	}
}

static inline void tx_hlist_bl_restore(struct hlist_bl_node *node, const struct tx_hlist_bl_node_snapshot *snapshot) {
	if (!hlist_bl_unhashed(node))
		__hlist_bl_del(node);

	node->next = snapshot->next;
	node->pprev = snapshot->pprev;
	if (snapshot->pprev) {
		WRITE_ONCE(*snapshot->pprev,
			   (struct hlist_bl_node *)((uintptr_t)node |
			   ((uintptr_t)*snapshot->pprev & LIST_BL_LOCKMASK)));
		if (snapshot->next)
			snapshot->next->pprev = &node->next;
	}
}

#endif
