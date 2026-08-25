// SPDX-License-Identifier: GPL-2.0
/* Transactional VFS file-object support.
   This currently handles per-open-file offset rollback. More file-local
   state can be added here later without changing the generic workset layer. */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/transaction.h>

/* Transaction-local snapshot of file state. For now we only preserve f_pos,
   which is the current offset for read/write/lseek on this open file. */
struct transaction_file_shadow {
	loff_t committed_pos;
	loff_t f_pos;
};

/* Initialize the generic transaction object embedded in the file struct. */
void transaction_file_init(struct file * file) {
	transaction_object_init(&file->transaction_object, TRANSACTION_OBJECT_FILE);
}
EXPORT_SYMBOL_GPL(transaction_file_init);

/* Commit callback for file workset entries.
   Publish the transaction-local file offset once the transaction commits. */
static int transaction_file_commit(struct txobj_thread_list_node * node) {
	struct transaction_file_shadow * shadow = node->shadow_obj;
	struct file *file = node->orig_obj;

	file->f_pos = shadow->f_pos;
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
	if (!shadow || file->f_pos != shadow->committed_pos)
		return -ESTALE;
	return 0;
}

/* Lock callback for file workset entries.
   f_pos_lock is a blocking lock and is acquired before generic object locks. */
static int transaction_file_lock(struct txobj_thread_list_node * node, int blocking) {
	struct file *file = node->orig_obj;

	if (blocking && file->f_mode & FMODE_ATOMIC_POS)
		mutex_lock(&file->f_pos_lock);

	return 0;
}

/* Unlock callback paired with transaction_file_lock(). */
static int transaction_file_unlock(struct txobj_thread_list_node * node, int blocking) {
	struct file *file = node->orig_obj;

	if (blocking && file->f_mode & FMODE_ATOMIC_POS)
		mutex_unlock(&file->f_pos_lock);

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

/* Add this file to the current transaction's workset and snapshot its current offset.
   Ordinary accesses resolve asymmetric ownership before using the stable offset.
   If the file is already in the workset, the existing shadow offset is reused. */
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

	shadow->committed_pos = file->f_pos;
	shadow->f_pos = file->f_pos;
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
