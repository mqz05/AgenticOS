// SPDX-License-Identifier: GPL-2.0
/* Transactional VFS dentry versioning support. */

#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/transaction.h>

#define TRANSACTION_DENTRY_FLAGS (DCACHE_DISCONNECTED | DCACHE_CANT_MOUNT | \
				  DCACHE_NFSFS_RENAMED | DCACHE_NEED_AUTOMOUNT | \
				  DCACHE_ENTRY_TYPE | DCACHE_NOKEY_NAME)

struct transaction_dentry_iput {
	struct list_head list;
	struct inode *inode;
};

struct transaction_dentry_private {
	struct _dentry contents;
	struct inode *commit_iput;
	struct inode *abort_iput;
	struct list_head deferred_iputs;
	unsigned int deferred_dputs;
	bool published;
};

static struct transaction_dentry_private *transaction_dentry_private(struct _dentry *contents) {
	return container_of(contents, struct transaction_dentry_private, contents);
}

static void transaction_dentry_private_init(struct transaction_dentry_private *private) {
	private->commit_iput = NULL;
	private->abort_iput = NULL;
	INIT_LIST_HEAD(&private->deferred_iputs);
	private->deferred_dputs = 0;
	private->published = false;
}

static void transaction_dentry_contents_free(struct rcu_head *rcu) {
	struct _dentry *dentry = container_of(rcu, struct _dentry, d_rcu);

	kfree(dentry);
}

static void transaction_dentry_contents_put(struct _dentry *dentry) {
	if (!dentry || !refcount_dec_and_test(&dentry->tx_refcount))
		return;
	transaction_dentry_name_put(dentry);
	if (dentry->owns_inode)
		iput(dentry->d_inode);
	if (!dentry->embedded)
		call_rcu(&dentry->d_rcu, transaction_dentry_contents_free);
}

static void transaction_dentry_copy_name_from_stable(struct _dentry *contents,
						       struct dentry *dentry) {
	transaction_dentry_name_get(contents, dentry);
}

static void transaction_dentry_copy_from_stable(struct _dentry *contents, struct dentry *dentry) {
	contents->parent = dentry;
	contents->shadow = NULL;
	contents->d_flags = READ_ONCE(dentry->d_flags);
	contents->d_inode = READ_ONCE(dentry->d_inode);
	contents->d_parent = READ_ONCE(dentry->d_parent);
	transaction_dentry_copy_name_from_stable(contents, dentry);
	contents->d_time = dentry->d_time;
	contents->d_op = dentry->d_op;
	contents->owns_inode = false;
}

static void transaction_dentry_shadow_copy(struct _dentry *shadow,
					    struct _dentry *committed) {
	memcpy(shadow, committed, sizeof(*shadow));
	transaction_dentry_name_copy(shadow, committed);
	shadow->shadow = committed;
	refcount_inc(&committed->tx_refcount);
	refcount_set(&shadow->tx_refcount, 1);
	memset(&shadow->d_rcu, 0, sizeof(shadow->d_rcu));
	shadow->embedded = false;
	shadow->owns_inode = false;
}

static bool transaction_dentry_name_matches(struct _dentry *contents, struct dentry *dentry) {
	if (contents->d_name.hash_len != dentry->d_name.hash_len)
		return false;
	return !contents->d_name.len ||
	       !memcmp(contents->d_name.name, dentry->d_name.name, contents->d_name.len);
}

static bool transaction_dentry_matches_stable(struct _dentry *contents, struct dentry *dentry) {
	return !((contents->d_flags ^ READ_ONCE(dentry->d_flags)) & TRANSACTION_DENTRY_FLAGS) &&
	       contents->d_inode == READ_ONCE(dentry->d_inode) &&
	       contents->d_parent == READ_ONCE(dentry->d_parent) &&
	       transaction_dentry_name_matches(contents, dentry) &&
	       contents->d_time == dentry->d_time && contents->d_op == dentry->d_op;
}

static struct _dentry *transaction_dentry_committed_locked(struct dentry *dentry) {
	struct _dentry *contents;

	contents = rcu_dereference_protected(dentry->d_contents, lockdep_is_held(&dentry->d_lock));
	if (contents)
		return contents;
	contents = &dentry->d_committed;
	transaction_dentry_copy_from_stable(contents, dentry);
	refcount_set(&contents->tx_refcount, 1);
	contents->embedded = true;
	rcu_assign_pointer(dentry->d_contents, contents);
	return contents;
}

int transaction_dentry_replace_committed_locked(struct transaction_object *object) {
	struct dentry *dentry = container_of(object, struct dentry, transaction_object);
	struct _dentry *committed;
	struct _dentry *replacement;

	lockdep_assert_held(&dentry->d_lock);
	committed = transaction_dentry_committed_locked(dentry);
	replacement = kmalloc(sizeof(*replacement), GFP_ATOMIC);
	if (!replacement)
		return -ENOMEM;
	memcpy(replacement, committed, sizeof(*replacement));
	transaction_dentry_name_copy(replacement, committed);
	replacement->parent = dentry;
	replacement->shadow = NULL;
	refcount_set(&replacement->tx_refcount, 1);
	memset(&replacement->d_rcu, 0, sizeof(replacement->d_rcu));
	replacement->embedded = false;
	replacement->owns_inode = false;
	if (committed->d_inode && !committed->owns_inode && refcount_read(&committed->tx_refcount) > 1) {
		ihold(committed->d_inode);
		committed->owns_inode = true;
	}
	rcu_assign_pointer(dentry->d_contents, replacement);
	transaction_dentry_contents_put(committed);
	return 0;
}
EXPORT_SYMBOL_GPL(transaction_dentry_replace_committed_locked);

static void transaction_dentry_private_put(struct _dentry *shadow) {
	if (!shadow)
		return;
	transaction_dentry_contents_put(shadow->shadow);
	shadow->shadow = NULL;
	transaction_dentry_contents_put(shadow);
}

void transaction_dentry_init(struct dentry *dentry) {
	transaction_object_init(&dentry->transaction_object, TRANSACTION_OBJECT_DENTRY);
	dentry->transaction_object.replace_committed = transaction_dentry_replace_committed_locked;
	dentry->transaction_publish_active = false;
	RCU_INIT_POINTER(dentry->d_contents, NULL);
	memset(&dentry->d_committed, 0, sizeof(dentry->d_committed));
}
EXPORT_SYMBOL_GPL(transaction_dentry_init);

void transaction_dentry_destroy(struct dentry *dentry) {
	struct _dentry *contents;

	contents = rcu_dereference_protected(dentry->d_contents, 1);
	RCU_INIT_POINTER(dentry->d_contents, NULL);
	if (contents)
		transaction_dentry_contents_put(contents);
}
EXPORT_SYMBOL_GPL(transaction_dentry_destroy);

static int transaction_dentry_lock(struct txobj_thread_list_node *node, int blocking) {
	struct dentry *dentry = node->orig_obj;
	struct txobj_thread_list_node *previous;

	if (!blocking) {
		for (previous = node->ordered_lock_prev; previous; previous = previous->ordered_lock_prev) {
			if (previous->nonblocking_lock_acquired && previous->nonblocking_nest_lock) {
				spin_lock_nest_lock(&dentry->d_lock, previous->nonblocking_nest_lock);
				goto locked;
			}
		}
		spin_lock(&dentry->d_lock);
locked:
		raw_write_seqcount_begin(&dentry->d_seq);
		dentry->transaction_publish_active = true;
	}
	return 0;
}

static int transaction_dentry_unlock(struct txobj_thread_list_node *node, int blocking) {
	struct dentry *dentry = node->orig_obj;

	if (!blocking) {
		raw_write_seqcount_end(&dentry->d_seq);
		dentry->transaction_publish_active = false;
		spin_unlock(&dentry->d_lock);
	}
	return 0;
}

static void transaction_dentry_finish_metadata(struct _dentry *shadow, struct dentry *dentry) {
	struct _dentry *baseline = shadow->shadow;
	unsigned int flags = READ_ONCE(dentry->d_flags);
	unsigned int changed;

	changed = (shadow->d_flags ^ baseline->d_flags) & TRANSACTION_DENTRY_FLAGS;
	flags = (flags & ~changed) | (shadow->d_flags & changed);
	shadow->d_flags = flags;
	WRITE_ONCE(dentry->d_flags, flags);
	if (shadow->d_parent != baseline->d_parent) {
		shadow->d_parent->d_lockref.count++;
		if (baseline->d_parent != dentry) {
			WARN_ON_ONCE(baseline->d_parent->d_lockref.count <= 0);
			baseline->d_parent->d_lockref.count--;
		}
	}
	WARN_ON_ONCE(!dentry->transaction_publish_active);
	transaction_dentry_name_restore(dentry, shadow);
	transaction_dentry_publish_inode(dentry, baseline->d_inode, shadow->d_inode);
	WRITE_ONCE(dentry->d_inode, shadow->d_inode);
	WRITE_ONCE(dentry->d_parent, shadow->d_parent);
	dentry->d_time = shadow->d_time;
	dentry->d_op = shadow->d_op;
}

static int transaction_dentry_commit(struct txobj_thread_list_node *node) {
	struct _dentry *shadow = node->shadow_obj;
	struct transaction_dentry_private *private;
	struct dentry *dentry = node->orig_obj;
	struct _dentry *baseline;
	struct _dentry *old;

	if (!shadow)
		return 0;
	if (node->rw == TRANSACTION_ACCESS_READ)
		return 0;
	private = transaction_dentry_private(shadow);
	old = transaction_dentry_committed_locked(dentry);
	baseline = shadow->shadow;
	WARN_ON_ONCE(baseline != old);
	transaction_dentry_finish_metadata(shadow, dentry);
	shadow->shadow = NULL;
	refcount_inc(&shadow->tx_refcount);
	rcu_assign_pointer(dentry->d_contents, shadow);
	private->published = true;
	node->tx_obj->version++;
	transaction_dentry_contents_put(old);
	transaction_dentry_contents_put(baseline);
	return 0;
}

static int transaction_dentry_abort(struct txobj_thread_list_node *node) {
	struct _dentry *shadow = node->shadow_obj;

	if (!shadow)
		return 0;
	if (node->rw == TRANSACTION_ACCESS_READ)
		return 0;
	return 0;
}

static int transaction_dentry_release(struct txobj_thread_list_node *node, int early) {
	struct dentry *dentry = node->orig_obj;
	struct _dentry *shadow = node->shadow_obj;
	struct transaction_dentry_private *private = NULL;
	struct transaction_dentry_iput *deferred;
	struct transaction_dentry_iput *next;
	bool published = false;

	if (node->rw == TRANSACTION_ACCESS_READ) {
		transaction_dentry_contents_put(shadow);
		shadow = NULL;
	} else if (shadow) {
		private = transaction_dentry_private(shadow);
		published = private->published;
		if (published && private->commit_iput)
			transaction_dentry_put_committed_inode(dentry, private->commit_iput);
		else if (!published && private->abort_iput)
			iput(private->abort_iput);
	}
	if (private) {
		list_for_each_entry_safe(deferred, next, &private->deferred_iputs, list) {
			list_del(&deferred->list);
			iput(deferred->inode);
			kfree(deferred);
		}
		if (published) {
			while (private->deferred_dputs) {
				private->deferred_dputs--;
				dput(dentry);
			}
		}
	}
	if (node->rw == TRANSACTION_ACCESS_READ_WRITE && shadow && !published)
		transaction_dentry_private_put(shadow);
	else if (published)
		transaction_dentry_contents_put(shadow);
	node->shadow_obj = NULL;
	dput(dentry);
	return 0;
}

int transaction_dentry_record_inode_change(struct dentry *dentry, struct inode *old_inode,
					    struct inode *new_inode) {
	struct transaction_dentry_iput *new_deferred = NULL;
	struct transaction_dentry_iput *old_deferred = NULL;
	struct _dentry *shadow = transaction_dentry_shadow(dentry);
	struct transaction_dentry_private *private;
	struct inode *baseline;
	bool defer_new;
	bool defer_old;

	if (!shadow || IS_ERR(shadow))
		return shadow ? PTR_ERR(shadow) : 0;
	if (old_inode == new_inode)
		return 0;
	private = transaction_dentry_private(shadow);
	baseline = shadow->shadow->d_inode;
	defer_old = old_inode && old_inode != baseline && private->abort_iput == old_inode;
	defer_new = new_inode && new_inode == baseline;
	if (old_inode != baseline && old_inode && !defer_old)
		return -EUCLEAN;
	if (defer_old) {
		old_deferred = kmalloc(sizeof(*old_deferred), GFP_ATOMIC);
		if (!old_deferred)
			return -ENOMEM;
	}
	if (defer_new) {
		new_deferred = kmalloc(sizeof(*new_deferred), GFP_ATOMIC);
		if (!new_deferred) {
			kfree(old_deferred);
			return -ENOMEM;
		}
	}
	if (old_deferred) {
		old_deferred->inode = old_inode;
		list_add_tail(&old_deferred->list, &private->deferred_iputs);
		private->abort_iput = NULL;
	}
	if (new_deferred) {
		new_deferred->inode = new_inode;
		list_add_tail(&new_deferred->list, &private->deferred_iputs);
		private->abort_iput = NULL;
	} else {
		private->abort_iput = new_inode;
	}
	private->commit_iput = baseline && new_inode != baseline ? baseline : NULL;
	return 0;
}
EXPORT_SYMBOL_GPL(transaction_dentry_record_inode_change);

static void transaction_dentry_setup_node(struct txobj_thread_list_node *node) {
	struct dentry *dentry = node->orig_obj;

	node->lock = transaction_dentry_lock;
	node->unlock = transaction_dentry_unlock;
	node->commit = transaction_dentry_commit;
	node->abort = transaction_dentry_abort;
	node->release = transaction_dentry_release;
	node->nonblocking_lock_id = &dentry->d_lock;
	node->nonblocking_nest_lock = &dentry->d_lock;
}

static struct _dentry *transaction_dentry_acquire_version(struct dentry *dentry,
							   enum transaction_access_mode mode,
							   bool locked, gfp_t gfp) {
	struct _dentry *candidate = NULL;
	struct _dentry *committed;
	struct _dentry *old = NULL;
	struct _dentry *shadow = NULL;
	struct transaction_dentry_private *private = NULL;

	if (mode == TRANSACTION_ACCESS_READ_WRITE) {
		private = kmalloc(sizeof(*private), gfp);
		if (!private)
			goto fail;
		transaction_dentry_private_init(private);
		shadow = &private->contents;
	}

retry:
	if (!locked)
		spin_lock(&dentry->d_lock);
	committed = rcu_dereference_protected(dentry->d_contents,
					      lockdep_is_held(&dentry->d_lock));
	if (!committed) {
		committed = &dentry->d_committed;
		transaction_dentry_copy_from_stable(committed, dentry);
		refcount_set(&committed->tx_refcount, 1);
		committed->embedded = true;
		rcu_assign_pointer(dentry->d_contents, committed);
	} else if (!transaction_dentry_matches_stable(committed, dentry)) {
		// Ordinary updates make the stable dentry the next committed baseline.
		if (!candidate) {
			if (!locked) {
				spin_unlock(&dentry->d_lock);
				candidate = kmalloc(sizeof(*candidate), gfp);
				if (!candidate)
					goto fail;
				goto retry;
			}
			candidate = kmalloc(sizeof(*candidate), gfp);
			if (!candidate)
				goto fail_locked;
		}
		old = committed;
		committed = candidate;
		candidate = NULL;
		transaction_dentry_copy_from_stable(committed, dentry);
		refcount_set(&committed->tx_refcount, 1);
		committed->embedded = false;
		rcu_assign_pointer(dentry->d_contents, committed);
	}
	if (mode == TRANSACTION_ACCESS_READ) {
		refcount_inc(&committed->tx_refcount);
		shadow = committed;
	} else {
		transaction_dentry_shadow_copy(shadow, committed);
	}
	if (!locked)
		spin_unlock(&dentry->d_lock);
	kfree(candidate);
	transaction_dentry_contents_put(old);
	return shadow;

fail_locked:
	if (!locked)
		spin_unlock(&dentry->d_lock);
fail:
	kfree(candidate);
	kfree(private);
	return NULL;
}

static struct _dentry *__transaction_dentry_get(struct dentry *dentry,
						 enum transaction_access_mode mode,
						 bool locked) {
	struct txobj_thread_list_node *node;
	struct transaction *transaction;
	struct _dentry *shadow;
	enum transaction_state status;
	gfp_t gfp = locked ? GFP_ATOMIC : GFP_KERNEL;
	int ret;

	if (!dentry)
		return ERR_PTR(-EINVAL);
	if (mode != TRANSACTION_ACCESS_READ && mode != TRANSACTION_ACCESS_READ_WRITE)
		return ERR_PTR(-EOPNOTSUPP);
	transaction = current_transaction();
	if (!transaction)
		return ERR_PTR(-EINVAL);
	status = transaction_status(transaction);
	if (status == TRANSACTION_ABORTED || status == TRANSACTION_ABORTING)
		return ERR_PTR(-ECANCELED);
	if (status != TRANSACTION_ACTIVE)
		return ERR_PTR(-EBUSY);

	node = transaction_workset_find_object(transaction, &dentry->transaction_object);
	if (node) {
		if (node->rw < mode) {
			struct transaction_dentry_private *private;

			private = kmalloc(sizeof(*private), gfp);
			if (!private)
				return ERR_PTR(-ENOMEM);
			transaction_dentry_private_init(private);
			shadow = &private->contents;
			ret = transaction_object_acquire(transaction, node, mode, NULL);
			if (ret) {
				kfree(private);
				return ERR_PTR(ret);
			}
			if (!locked)
				spin_lock(&dentry->d_lock);
			transaction_dentry_shadow_copy(shadow, node->shadow_obj);
			if (!locked)
				spin_unlock(&dentry->d_lock);
			transaction_dentry_contents_put(node->shadow_obj);
			node->shadow_obj = shadow;
			node->rw = mode;
		}
		shadow = node->shadow_obj;
		return shadow;
	}

	node = transaction_workset_node_alloc(NULL, dentry, &dentry->transaction_object,
					      TRANSACTION_OBJECT_DENTRY, mode, gfp);
	if (!node)
		return ERR_PTR(-ENOMEM);
	transaction_dentry_setup_node(node);
	if (locked)
		dget_dlock(dentry);
	else
		dget(dentry);
	ret = transaction_workset_add(transaction, node);
	if (ret)
		goto free_node;
	ret = transaction_object_acquire(transaction, node, mode, NULL);
	if (ret) {
		if (transaction_status(transaction) != TRANSACTION_ACTIVE)
			return ERR_PTR(ret);
		transaction_workset_remove(transaction, node);
		goto free_node;
	}
	shadow = transaction_dentry_acquire_version(dentry, mode, locked, gfp);
	if (!shadow) {
		abort_transaction(transaction);
		return ERR_PTR(-ENOMEM);
	}
	node->shadow_obj = shadow;
	return shadow;

free_node:
	if (locked) {
		lockdep_assert_held(&dentry->d_lock);
		WARN_ON_ONCE(dentry->d_lockref.count <= 0);
		dentry->d_lockref.count--;
	} else {
		dput(dentry);
	}
	transaction_workset_node_free(node);
	return ERR_PTR(ret == -EEXIST ? -EBUSY : ret);
}

struct _dentry *transaction_dentry_get(struct dentry *dentry, enum transaction_access_mode mode) {
	return __transaction_dentry_get(dentry, mode, false);
}
EXPORT_SYMBOL_GPL(transaction_dentry_get);

static struct txobj_thread_list_node *transaction_dentry_workset_node(struct dentry *dentry) {
	struct transaction *transaction = current_transaction();

	if (!transaction || !dentry)
		return NULL;
	return transaction_workset_find_object(transaction, &dentry->transaction_object);
}

struct _dentry *transaction_dentry_visible(struct dentry *dentry) {
	struct txobj_thread_list_node *node = transaction_dentry_workset_node(dentry);

	return node ? node->shadow_obj : NULL;
}
EXPORT_SYMBOL_GPL(transaction_dentry_visible);

struct _dentry *transaction_dentry_shadow(struct dentry *dentry) {
	struct txobj_thread_list_node *node = transaction_dentry_workset_node(dentry);
	struct transaction *transaction = current_transaction();

	if (!node)
		return NULL;
	if (node->rw != TRANSACTION_ACCESS_READ_WRITE ||
	    transaction_status(transaction) != TRANSACTION_ACTIVE) {
		abort_transaction(transaction);
		return ERR_PTR(-ECANCELED);
	}
	return node->shadow_obj;
}
EXPORT_SYMBOL_GPL(transaction_dentry_shadow);

bool transaction_dentry_get_flags(const struct dentry *dentry, unsigned int *flags) {
	struct _dentry *contents = transaction_dentry_visible((struct dentry *)dentry);

	if (!contents)
		return false;
	*flags = (READ_ONCE(dentry->d_flags) & ~TRANSACTION_DENTRY_FLAGS) |
		 (contents->d_flags & TRANSACTION_DENTRY_FLAGS);
	return true;
}
EXPORT_SYMBOL_GPL(transaction_dentry_get_flags);

bool transaction_dentry_set_flags(struct dentry *dentry, unsigned int flags, unsigned int mask) {
	struct _dentry *shadow = transaction_dentry_shadow(dentry);
	unsigned int transaction_mask = mask & TRANSACTION_DENTRY_FLAGS;

	if (!shadow)
		return false;
	if (IS_ERR(shadow))
		return true;
	shadow->d_flags = (shadow->d_flags & ~transaction_mask) | (flags & transaction_mask);
	if (mask & ~TRANSACTION_DENTRY_FLAGS)
		set_mask_bits(&dentry->d_flags, mask & ~TRANSACTION_DENTRY_FLAGS,
			      flags & ~TRANSACTION_DENTRY_FLAGS);
	return true;
}
EXPORT_SYMBOL_GPL(transaction_dentry_set_flags);

int transaction_dentry_snapshot(struct dentry *dentry) {
	struct _dentry *shadow;
	struct transaction *winner;
	int ret;

	if (!dentry)
		return -EINVAL;
	if (!current_transaction()) {
retry:
		spin_lock(&dentry->d_lock);
		winner = transaction_check_asymmetric_conflict(&dentry->transaction_object,
							       TRANSACTION_ACCESS_READ_WRITE, true, &ret);
		spin_unlock(&dentry->d_lock);
		if (!winner)
			return ret;
		ret = transaction_wait_on_conflict(winner);
		if (!ret)
			goto retry;
		return ret;
	}
	shadow = __transaction_dentry_get(dentry, TRANSACTION_ACCESS_READ_WRITE, false);
	return IS_ERR(shadow) ? PTR_ERR(shadow) : 0;
}
EXPORT_SYMBOL_GPL(transaction_dentry_snapshot);

int transaction_dentry_snapshot_locked(struct dentry *dentry) {
	struct _dentry *shadow;
	struct transaction *transaction;
	struct transaction *winner;
	int ret;

	if (!dentry)
		return -EINVAL;
	transaction = current_transaction();
	if (!transaction) {
		winner = transaction_check_asymmetric_conflict(&dentry->transaction_object,
							       TRANSACTION_ACCESS_READ_WRITE, false, &ret);
		if (WARN_ON_ONCE(winner)) {
			transaction_put(winner);
			return -EUCLEAN;
		}
		return ret;
	}
	shadow = __transaction_dentry_get(dentry, TRANSACTION_ACCESS_READ_WRITE, true);
	if (!IS_ERR(shadow))
		return 0;
	abort_transaction(transaction);
	return PTR_ERR(shadow);
}
EXPORT_SYMBOL_GPL(transaction_dentry_snapshot_locked);

int transaction_dentry_snapshot_unlink(struct dentry *dentry) {
	struct _dentry *shadow;

	if (!dentry)
		return -EINVAL;
	if (!current_transaction())
		return transaction_dentry_snapshot(dentry);
	shadow = __transaction_dentry_get(dentry, TRANSACTION_ACCESS_READ_WRITE, false);
	return IS_ERR(shadow) ? PTR_ERR(shadow) : 0;
}
EXPORT_SYMBOL_GPL(transaction_dentry_snapshot_unlink);

bool transaction_dentry_defer_dput(struct dentry *dentry)
{
	struct _dentry *shadow = transaction_dentry_shadow(dentry);
	struct transaction_dentry_private *private;

	if (!shadow || IS_ERR(shadow))
		return false;
	private = transaction_dentry_private(shadow);
	private->deferred_dputs++;
	return true;
}
EXPORT_SYMBOL_GPL(transaction_dentry_defer_dput);
