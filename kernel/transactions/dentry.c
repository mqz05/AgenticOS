// SPDX-License-Identifier: GPL-2.0
/* Transactional VFS dentry versioning support. */

#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/transaction.h>
#include <linux/tx_hlist.h>

#define TRANSACTION_DENTRY_FLAGS (DCACHE_DISCONNECTED | DCACHE_CANT_MOUNT | \
				  				  DCACHE_NFSFS_RENAMED | DCACHE_NEED_AUTOMOUNT | \
				 				  DCACHE_ENTRY_TYPE | DCACHE_NOKEY_NAME)

struct transaction_dentry_relations {
	bool restore_unlink_pin;
	struct tx_hlist_bl_node_snapshot d_hash;
	struct tx_hlist_node_snapshot d_sib;
	struct tx_hlist_node_snapshot d_alias;
};

static void transaction_dentry_contents_free(struct rcu_head *rcu) {
	struct _dentry *dentry = container_of(rcu, struct _dentry, d_rcu);

	kfree(dentry);
}

static void transaction_dentry_contents_put(struct _dentry *dentry) {
	if (!dentry || !refcount_dec_and_test(&dentry->tx_refcount))
		return;
	transaction_dentry_name_put(dentry);
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
	contents->relations = NULL;
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
	shadow->relations = NULL;
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

static void transaction_dentry_capture_relations(struct _dentry *shadow, struct dentry *dentry) {
	struct transaction_dentry_relations *relations = shadow->relations;

	tx_hlist_bl_snapshot(&dentry->d_hash, &relations->d_hash);
	tx_hlist_snapshot(&dentry->d_sib, &relations->d_sib);
	tx_hlist_snapshot(&dentry->d_u.d_alias, &relations->d_alias);
}

static void transaction_dentry_private_put(struct _dentry *shadow) {
	if (!shadow)
		return;
	kfree(shadow->relations);
	shadow->relations = NULL;
	transaction_dentry_contents_put(shadow->shadow);
	shadow->shadow = NULL;
	transaction_dentry_contents_put(shadow);
}

void transaction_dentry_init(struct dentry *dentry) {
	transaction_object_init(&dentry->transaction_object, TRANSACTION_OBJECT_DENTRY);
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

	if (!blocking)
		spin_lock(&dentry->d_lock);
	return 0;
}

static int transaction_dentry_unlock(struct txobj_thread_list_node *node, int blocking) {
	struct dentry *dentry = node->orig_obj;

	if (!blocking)
		spin_unlock(&dentry->d_lock);
	return 0;
}

static void transaction_dentry_restore_relations(struct _dentry *shadow, struct dentry *dentry) {
	struct transaction_dentry_relations *relations = shadow->relations;
	struct _dentry *baseline = shadow->shadow;

	if (!relations)
		return;
	raw_write_seqcount_begin(&dentry->d_seq);
	transaction_dentry_name_restore(dentry, baseline);
	WRITE_ONCE(dentry->d_inode, baseline->d_inode);
	WRITE_ONCE(dentry->d_parent, baseline->d_parent);
	raw_write_seqcount_end(&dentry->d_seq);
	tx_hlist_bl_restore(&dentry->d_hash, &relations->d_hash);
	tx_hlist_restore(&dentry->d_sib, &relations->d_sib);
	tx_hlist_restore(&dentry->d_u.d_alias, &relations->d_alias);
	if (relations->restore_unlink_pin)
		dget_dlock(dentry);
}

static void transaction_dentry_finish_metadata(struct _dentry *shadow, struct dentry *dentry) {
	struct _dentry *baseline = shadow->shadow;
	unsigned int flags = READ_ONCE(dentry->d_flags);
	unsigned int changed;

	changed = (shadow->d_flags ^ baseline->d_flags) & TRANSACTION_DENTRY_FLAGS;
	flags = (flags & ~changed) | (shadow->d_flags & changed);
	shadow->d_flags = flags;
	WRITE_ONCE(dentry->d_flags, flags);
	if (shadow->d_time == baseline->d_time)
		shadow->d_time = dentry->d_time;
	else
		dentry->d_time = shadow->d_time;
	shadow->d_inode = READ_ONCE(dentry->d_inode);
	shadow->d_parent = READ_ONCE(dentry->d_parent);
	transaction_dentry_name_put(shadow);
	transaction_dentry_copy_name_from_stable(shadow, dentry);
	shadow->d_op = dentry->d_op;
}

static int transaction_dentry_commit(struct txobj_thread_list_node *node) {
	struct _dentry *shadow = node->shadow_obj;
	struct dentry *dentry = node->orig_obj;
	struct _dentry *baseline;
	struct _dentry *old;

	if (!shadow)
		return 0;
	if (node->rw == TRANSACTION_ACCESS_READ) {
		transaction_dentry_contents_put(shadow);
		node->shadow_obj = NULL;
		return 0;
	}
	old = transaction_dentry_committed_locked(dentry);
	baseline = shadow->shadow;
	WARN_ON_ONCE(baseline != old);
	transaction_dentry_finish_metadata(shadow, dentry);
	kfree(shadow->relations);
	shadow->relations = NULL;
	shadow->shadow = NULL;
	rcu_assign_pointer(dentry->d_contents, shadow);
	node->shadow_obj = NULL;
	node->tx_obj->version++;
	transaction_dentry_contents_put(old);
	transaction_dentry_contents_put(baseline);
	return 0;
}

static int transaction_dentry_abort(struct txobj_thread_list_node *node) {
	struct _dentry *shadow = node->shadow_obj;
	struct dentry *dentry = node->orig_obj;
	struct _dentry *baseline;
	unsigned int changed;
	unsigned int flags;

	if (!shadow)
		return 0;
	if (node->rw == TRANSACTION_ACCESS_READ) {
		transaction_dentry_contents_put(shadow);
		node->shadow_obj = NULL;
		return 0;
	}
	baseline = shadow->shadow;
	changed = (shadow->d_flags ^ baseline->d_flags) & TRANSACTION_DENTRY_FLAGS;
	flags = READ_ONCE(dentry->d_flags);
	flags = (flags & ~changed) | (baseline->d_flags & changed);
	WRITE_ONCE(dentry->d_flags, flags);
	dentry->d_time = baseline->d_time;
	transaction_dentry_restore_relations(shadow, dentry);
	transaction_dentry_private_put(shadow);
	node->shadow_obj = NULL;
	return 0;
}

static int transaction_dentry_release(struct txobj_thread_list_node *node, int early) {
	struct dentry *dentry = node->orig_obj;

	if (node->rw == TRANSACTION_ACCESS_READ)
		transaction_dentry_contents_put(node->shadow_obj);
	else
		transaction_dentry_private_put(node->shadow_obj);
	node->shadow_obj = NULL;
	dput(dentry);
	return 0;
}

static void transaction_dentry_setup_node(struct txobj_thread_list_node *node) {
	node->lock = transaction_dentry_lock;
	node->unlock = transaction_dentry_unlock;
	node->commit = transaction_dentry_commit;
	node->abort = transaction_dentry_abort;
	node->release = transaction_dentry_release;
}

static struct _dentry *transaction_dentry_acquire_version(struct dentry *dentry,
							   enum transaction_access_mode mode,
							   bool locked, gfp_t gfp) {
	struct transaction_dentry_relations *relations = NULL;
	struct _dentry *candidate = NULL;
	struct _dentry *committed;
	struct _dentry *old = NULL;
	struct _dentry *shadow = NULL;

	if (mode == TRANSACTION_ACCESS_READ_WRITE) {
		shadow = kmalloc(sizeof(*shadow), gfp);
		relations = kzalloc(sizeof(*relations), gfp);
		if (!shadow || !relations)
			goto fail;
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
		/* Ordinary updates make the stable dentry the next committed baseline. */
		/* TODO: Asymmetric conflicts must serialize this with active owners. */
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
		shadow->relations = relations;
		transaction_dentry_capture_relations(shadow, dentry);
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
	kfree(relations);
	kfree(shadow);
	return NULL;
}

static struct _dentry *__transaction_dentry_get(struct dentry *dentry,
						 enum transaction_access_mode mode,
						 bool locked, bool restore_unlink_pin) {
	struct transaction_dentry_relations *relations;
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
			struct transaction_dentry_relations *upgrade_relations;

			shadow = kmalloc(sizeof(*shadow), gfp);
			upgrade_relations = kzalloc(sizeof(*upgrade_relations), gfp);
			if (!shadow || !upgrade_relations) {
				kfree(upgrade_relations);
				kfree(shadow);
				return ERR_PTR(-ENOMEM);
			}
			ret = transaction_object_acquire(transaction, node, mode, NULL);
			if (ret) {
				kfree(upgrade_relations);
				kfree(shadow);
				return ERR_PTR(ret);
			}
			if (!locked)
				spin_lock(&dentry->d_lock);
			transaction_dentry_shadow_copy(shadow, node->shadow_obj);
			shadow->relations = upgrade_relations;
			transaction_dentry_capture_relations(shadow, dentry);
			if (!locked)
				spin_unlock(&dentry->d_lock);
			transaction_dentry_contents_put(node->shadow_obj);
			node->shadow_obj = shadow;
			node->rw = mode;
		}
		shadow = node->shadow_obj;
		if (restore_unlink_pin && node->rw == TRANSACTION_ACCESS_READ_WRITE) {
			relations = shadow->relations;
			relations->restore_unlink_pin = true;
		}
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
	if (restore_unlink_pin) {
		relations = shadow->relations;
		relations->restore_unlink_pin = true;
	}
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
	return __transaction_dentry_get(dentry, mode, false, false);
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

	if (!dentry)
		return -EINVAL;
	// TODO: Resolve ordinary conflicts after dcache relationships use transactional hlists.
	if (!current_transaction())
		return 0;
	shadow = __transaction_dentry_get(dentry, TRANSACTION_ACCESS_READ_WRITE, false, false);
	return IS_ERR(shadow) ? PTR_ERR(shadow) : 0;
}
EXPORT_SYMBOL_GPL(transaction_dentry_snapshot);

int transaction_dentry_snapshot_locked(struct dentry *dentry) {
	struct _dentry *shadow;
	struct transaction *transaction;

	if (!dentry)
		return -EINVAL;
	transaction = current_transaction();
	if (!transaction)
		return 0;
	shadow = __transaction_dentry_get(dentry, TRANSACTION_ACCESS_READ_WRITE, true, false);
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
		return 0;
	shadow = __transaction_dentry_get(dentry, TRANSACTION_ACCESS_READ_WRITE, false, true);
	return IS_ERR(shadow) ? PTR_ERR(shadow) : 0;
}
EXPORT_SYMBOL_GPL(transaction_dentry_snapshot_unlink);
