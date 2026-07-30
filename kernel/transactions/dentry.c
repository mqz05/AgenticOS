// SPDX-License-Identifier: GPL-2.0
/* Transactional dentry metadata rollback support. */

#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/transaction.h>

struct transaction_dentry_shadow {
	unsigned int d_flags;
	unsigned long d_time;
	void *d_fsdata;
	struct inode *d_inode;
	struct dentry *d_parent;
};

void transaction_dentry_init(struct dentry * dentry) {
	transaction_object_init(&dentry->transaction_object, TRANSACTION_OBJECT_DENTRY);
}
EXPORT_SYMBOL_GPL(transaction_dentry_init);

static int transaction_dentry_lock(struct txobj_thread_list_node * node, int blocking) {
	struct dentry *dentry = node->orig_obj;
	if (!blocking)
		spin_lock(&dentry->d_lock);
	return 0;
}

static int transaction_dentry_unlock(struct txobj_thread_list_node * node, int blocking) {
	struct dentry *dentry = node->orig_obj;
	if (!blocking)
		spin_unlock(&dentry->d_lock);
	return 0;
}

static int transaction_dentry_abort(struct txobj_thread_list_node * node) {
	struct transaction_dentry_shadow *shadow = node->shadow_obj;
	struct dentry *dentry = node->orig_obj;

	dentry->d_flags = shadow->d_flags;
	dentry->d_time = shadow->d_time;
	dentry->d_fsdata = shadow->d_fsdata;
	WRITE_ONCE(dentry->d_inode, shadow->d_inode);
	WRITE_ONCE(dentry->d_parent, shadow->d_parent);

	return 0;
}

static int transaction_dentry_release(struct txobj_thread_list_node * node, int early) {
	struct dentry *dentry = node->orig_obj;
	kfree(node->shadow_obj);
	dput(dentry);
	return 0;
}

int transaction_dentry_snapshot(struct dentry * dentry) {
	struct transaction_dentry_shadow *shadow;
	struct txobj_thread_list_node *node;
	struct transaction *transaction;
	enum transaction_state status;
	int ret;

	if (!dentry)
		return -EINVAL;

	transaction = current_transaction();
	if (!transaction)
		return 0;

	status = transaction_status(transaction);
	if (status == TRANSACTION_ABORTED || status == TRANSACTION_ABORTING)
		return -ECANCELED;
	if (status != TRANSACTION_ACTIVE)
		return 0;

	if (transaction_workset_find_object(transaction,
					    &dentry->transaction_object))
		return 0;

	shadow = kmalloc(sizeof(*shadow), GFP_KERNEL);
	if (!shadow)
		return -ENOMEM;

	spin_lock(&dentry->d_lock);
	shadow->d_flags = dentry->d_flags;
	shadow->d_time = dentry->d_time;
	shadow->d_fsdata = dentry->d_fsdata;
	shadow->d_inode = dentry->d_inode;
	shadow->d_parent = dentry->d_parent;
	spin_unlock(&dentry->d_lock);

	node = transaction_workset_node_alloc(
		shadow,
		dentry,
		&dentry->transaction_object,
		TRANSACTION_OBJECT_DENTRY,
		TRANSACTION_ACCESS_READ_WRITE,
		GFP_KERNEL);
	if (!node) {
		kfree(shadow);
		return -ENOMEM;
	}

	node->lock = transaction_dentry_lock;
	node->unlock = transaction_dentry_unlock;
	node->abort = transaction_dentry_abort;
	node->release = transaction_dentry_release;
	dget(dentry);
	ret = transaction_workset_add(transaction, node);
	if (ret)
		goto free_node;

	ret = transaction_object_acquire(transaction, node,
					 TRANSACTION_ACCESS_READ_WRITE, NULL);
	if (!ret)
		return 0;

	if (transaction_status(transaction) != TRANSACTION_ACTIVE)
		return ret;

	transaction_workset_remove(transaction, node);

free_node:
	dput(dentry);
	kfree(shadow);
	transaction_workset_node_free(node);

	return ret == -EEXIST ? 0 : ret;
}
EXPORT_SYMBOL_GPL(transaction_dentry_snapshot);
