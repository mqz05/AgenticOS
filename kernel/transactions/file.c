// SPDX-License-Identifier: GPL-2.0
/*
 * Transactional VFS open-file-description support.  Offset and mutable
 * status flags live in the transaction shadow; stable identity fields are
 * captured so commit-time validation detects an invalidated description.
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/transaction.h>

/*
 * Modern counterpart of TxOS struct _file.  Lifetime and subsystem-private
 * state remain in struct file; semantically visible open-file-description
 * state is copied here and published atomically at commit.
 */
struct transaction_file_shadow {
	loff_t committed_pos;
	loff_t f_pos;
	fmode_t committed_mode;
	fmode_t f_mode;
	unsigned int committed_flags;
	unsigned int f_flags;
	unsigned int committed_iocb_flags;
	unsigned int f_iocb_flags;
	const struct file_operations *committed_op;
	struct address_space *committed_mapping;
	struct inode *committed_inode;
	const struct cred *committed_cred;
	struct vfsmount *committed_mnt;
	struct dentry *committed_dentry;
#ifdef CONFIG_SECURITY
	void *committed_security;
#endif
};

static unsigned int transaction_iocb_flags(unsigned int flags)
{
	unsigned int result = 0;

	if (flags & O_APPEND)
		result |= IOCB_APPEND;
	if (flags & O_DIRECT)
		result |= IOCB_DIRECT;
	if (flags & O_DSYNC)
		result |= IOCB_DSYNC;
	if (flags & __O_SYNC)
		result |= IOCB_SYNC;
	return result;
}

/* Initialize the generic transaction object embedded in the file struct. */
void transaction_file_init(struct file * file) {
	transaction_object_init(&file->transaction_object, TRANSACTION_OBJECT_FILE);
}
EXPORT_SYMBOL_GPL(transaction_file_init);

/* Publish transaction-local open-file-description state at commit. */
static int transaction_file_commit(struct txobj_thread_list_node * node) {
	struct transaction_file_shadow * shadow = node->shadow_obj;
	struct file *file = node->orig_obj;

	file->f_pos = shadow->f_pos;
	file->f_flags = shadow->f_flags;
	file->f_iocb_flags = shadow->f_iocb_flags;
	node->tx_obj->version++;

	return 0;
}

static int transaction_file_validate(struct txobj_thread_list_node *node)
{
	struct transaction_file_shadow *shadow = node->shadow_obj;
	struct file *file = node->orig_obj;
	int ret = transaction_object_validate(node);

	if (ret)
		return ret;
	if (!shadow || file->f_pos != shadow->committed_pos ||
	    file->f_mode != shadow->committed_mode ||
	    file->f_flags != shadow->committed_flags ||
	    file->f_iocb_flags != shadow->committed_iocb_flags ||
	    file->f_op != shadow->committed_op ||
	    file->f_mapping != shadow->committed_mapping ||
	    file->f_inode != shadow->committed_inode ||
	    file->f_cred != shadow->committed_cred ||
	    file->f_path.mnt != shadow->committed_mnt ||
	    file->f_path.dentry != shadow->committed_dentry
#ifdef CONFIG_SECURITY
	    || file->f_security != shadow->committed_security
#endif
	   )
		return -ESTALE;
	return 0;
}

/* Lock callback for file workset entries.
   f_pos_lock is a blocking lock and is acquired before generic object locks. */
static int transaction_file_lock(struct txobj_thread_list_node * node, int blocking) {
	struct file *file = node->orig_obj;

	if (blocking && file->f_mode & FMODE_ATOMIC_POS)
		mutex_lock(&file->f_pos_lock);
	else if (!blocking)
		spin_lock(&file->f_lock);

	return 0;
}

/* Unlock callback paired with transaction_file_lock(). */
static int transaction_file_unlock(struct txobj_thread_list_node * node, int blocking) {
	struct file *file = node->orig_obj;

	if (blocking && file->f_mode & FMODE_ATOMIC_POS)
		mutex_unlock(&file->f_pos_lock);
	else if (!blocking)
		spin_unlock(&file->f_lock);

	return 0;
}

/* Release callback for file workset entries.
   The generic workset code owns the entry itself; this adapter owns the shadow state stored in node->shadow_obj
   and the reference to the file stored in node->orig_obj. */
static int transaction_file_release(struct txobj_thread_list_node * node, int early) {
	struct file *file = node->orig_obj;

	kfree(node->shadow_obj);
	fput(file);

	return 0;
}

/* Return the file offset visible to the current transaction. */
loff_t transaction_file_get_pos(struct file * file) {
	struct txobj_thread_list_node *node;
	struct transaction_file_shadow *shadow;
	struct transaction *transaction;

	if (!file)
		return 0;

	transaction = current_transaction();
	if (!transaction)
		return file->f_pos;

	node = transaction_workset_find_object(transaction, &file->transaction_object);
	if (!node)
		return file->f_pos;

	shadow = node->shadow_obj;
	return shadow->f_pos;
}
EXPORT_SYMBOL_GPL(transaction_file_get_pos);

static struct transaction_file_shadow *transaction_file_shadow(struct file *file)
{
	struct txobj_thread_list_node *node;
	struct transaction *transaction = current_transaction();

	if (!transaction)
		return NULL;
	node = transaction_workset_find_object(transaction,
					       &file->transaction_object);
	return node ? node->shadow_obj : NULL;
}

static struct transaction_file_shadow *transaction_file_visible_shadow(struct file *file)
{
	struct transaction_file_shadow *shadow;

	shadow = transaction_file_shadow(file);
	if (!shadow && current_transaction()) {
		if (transaction_file_snapshot(file))
			return NULL;
		shadow = transaction_file_shadow(file);
	}
	return shadow;
}

unsigned int transaction_file_get_flags(struct file *file)
{
	struct transaction_file_shadow *shadow = transaction_file_visible_shadow(file);

	return shadow ? shadow->f_flags : READ_ONCE(file->f_flags);
}
EXPORT_SYMBOL_GPL(transaction_file_get_flags);

unsigned int transaction_file_get_iocb_flags(struct file *file)
{
	struct transaction_file_shadow *shadow = transaction_file_visible_shadow(file);

	return shadow ? shadow->f_iocb_flags : READ_ONCE(file->f_iocb_flags);
}
EXPORT_SYMBOL_GPL(transaction_file_get_iocb_flags);

fmode_t transaction_file_get_mode(struct file *file)
{
	struct transaction_file_shadow *shadow = transaction_file_visible_shadow(file);

	return shadow ? shadow->f_mode : READ_ONCE(file->f_mode);
}
EXPORT_SYMBOL_GPL(transaction_file_get_mode);

/*
 * Add this file to the workset and snapshot its open-file-description state.
 * If it is already present, reuse the existing shadow.
 */
int transaction_file_snapshot(struct file * file) {
	struct txobj_thread_list_node * node;
	struct transaction_file_shadow * shadow;
	struct transaction * transaction;
	struct transaction *winner;
	enum transaction_state status;
	int ret;

	if (!file)
		return -EINVAL;

	transaction = current_transaction();
	if (!transaction) {
		winner = transaction_check_asymmetric_conflict(&file->transaction_object,
								       TRANSACTION_ACCESS_READ_WRITE, false, &ret);
		if (WARN_ON_ONCE(winner)) {
			transaction_put(winner);
			return -EUCLEAN;
		}
		return ret;
	}

	status = transaction_status(transaction);
	if (status == TRANSACTION_ABORTED || status == TRANSACTION_ABORTING)
		return -ECANCELED;
	if (status != TRANSACTION_ACTIVE)
		return -EBUSY;

	if (transaction_workset_find_object(transaction, &file->transaction_object))
		return 0;

	shadow = kmalloc(sizeof(*shadow), GFP_KERNEL);
	if (!shadow)
		return -ENOMEM;

	spin_lock(&file->f_lock);
	shadow->committed_pos = file->f_pos;
	shadow->f_pos = file->f_pos;
	shadow->committed_mode = file->f_mode;
	shadow->f_mode = file->f_mode;
	shadow->committed_flags = file->f_flags;
	shadow->f_flags = file->f_flags;
	shadow->committed_iocb_flags = file->f_iocb_flags;
	shadow->f_iocb_flags = file->f_iocb_flags;
	shadow->committed_op = file->f_op;
	shadow->committed_mapping = file->f_mapping;
	shadow->committed_inode = file->f_inode;
	shadow->committed_cred = file->f_cred;
	shadow->committed_mnt = file->f_path.mnt;
	shadow->committed_dentry = file->f_path.dentry;
#ifdef CONFIG_SECURITY
	shadow->committed_security = file->f_security;
#endif
	spin_unlock(&file->f_lock);
	node = transaction_workset_node_alloc(
		shadow,
		file,
		&file->transaction_object,
		TRANSACTION_OBJECT_FILE,
		TRANSACTION_ACCESS_READ_WRITE,
		GFP_KERNEL
	);
	if (!node) {
		kfree(shadow);
		return -ENOMEM;
	}

	/* File workset callbacks. Runtime updates go to the shadow offset;
	   commit publishes that offset and abort discards it. */
	node->lock = transaction_file_lock;
	node->unlock = transaction_file_unlock;
	node->validate = transaction_file_validate;
	node->commit = transaction_file_commit;
	node->release = transaction_file_release;
	node->blocking_lock_id = file->f_mode & FMODE_ATOMIC_POS ? &file->f_pos_lock : NULL;
	node->nonblocking_lock_id = &file->f_lock;
	node->nonblocking_nest_lock = &file->f_lock;
	get_file(file);
	ret = transaction_workset_add(transaction, node);
	if (ret)
		goto free_node;

	ret = transaction_object_acquire(transaction, node, TRANSACTION_ACCESS_READ_WRITE, NULL);
	if (!ret)
		return 0;

	if (transaction_status(transaction) != TRANSACTION_ACTIVE)
		return ret;

	transaction_workset_remove(transaction, node);

free_node:
	fput(file);
	kfree(shadow);
	transaction_workset_node_free(node);

	return ret == -EEXIST ? 0 : ret;
}
EXPORT_SYMBOL_GPL(transaction_file_snapshot);

/* Update the file offset visible to the current transaction. */
int transaction_file_set_pos(struct file * file, loff_t pos) {
	struct txobj_thread_list_node *node;
	struct transaction_file_shadow *shadow;
	struct transaction *transaction;
	struct transaction *winner;
	int ret;

	if (!file)
		return -EINVAL;

	transaction = current_transaction();
	if (!transaction) {
		winner = transaction_check_asymmetric_conflict(&file->transaction_object,
								       TRANSACTION_ACCESS_READ_WRITE, false, &ret);
		if (WARN_ON_ONCE(winner)) {
			transaction_put(winner);
			return -EUCLEAN;
		}
		if (ret)
			return ret;
		file->f_pos = pos;
		return 0;
	}

	ret = transaction_file_snapshot(file);
	if (ret)
		return ret;

	node = transaction_workset_find_object(transaction, &file->transaction_object);
	if (!node) {
		file->f_pos = pos;
		return 0;
	}

	shadow = node->shadow_obj;
	shadow->f_pos = pos;
	return 0;
}
EXPORT_SYMBOL_GPL(transaction_file_set_pos);

int transaction_file_set_flags(struct file *file, unsigned int flags,
			       unsigned int mask)
{
	struct transaction_file_shadow *shadow;
	struct transaction *winner;
	int ret;

	if (!file)
		return -EINVAL;
	if (!current_transaction()) {
		winner = transaction_check_asymmetric_conflict(&file->transaction_object,
						       TRANSACTION_ACCESS_READ_WRITE,
						       false, &ret);
		if (WARN_ON_ONCE(winner)) {
			transaction_put(winner);
			return -EUCLEAN;
		}
		if (ret)
			return ret;
		spin_lock(&file->f_lock);
		file->f_flags = (file->f_flags & ~mask) | (flags & mask);
		file->f_iocb_flags = iocb_flags(file);
		spin_unlock(&file->f_lock);
		return 0;
	}

	ret = transaction_file_snapshot(file);
	if (ret)
		return ret;
	shadow = transaction_file_shadow(file);
	if (!shadow)
		return -EUCLEAN;
	shadow->f_flags = (shadow->f_flags & ~mask) | (flags & mask);
	shadow->f_iocb_flags = transaction_iocb_flags(shadow->f_flags);
	return 0;
}
EXPORT_SYMBOL_GPL(transaction_file_set_flags);
