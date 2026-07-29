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
	loff_t f_pos;
	bool restore;
};

/* Initialize the generic transaction object embedded in the file struct. */
void transaction_file_init(struct file * file) {
	transaction_object_init(&file->transaction_object, TRANSACTION_OBJECT_FILE);
}
EXPORT_SYMBOL_GPL(transaction_file_init);

/* Abort callback for file workset entries.
   Restore the file offset captured when the file was first touched inside the transaction. */
static int transaction_file_abort(struct txobj_thread_list_node * node) {
	struct transaction_file_shadow * shadow = node->shadow_obj;
	struct file *file = node->orig_obj;

	if (shadow->restore)
		file->f_pos = shadow->f_pos;

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

/* Add this file to the current transaction's workset and snapshot its original offset.
   If the current task is not in a live transaction, this is a no-op.
   If the file is already in the workset, the original snapshot is reused so abort
   restores the offset from the first transactional touch. */
int transaction_file_snapshot(struct file * file) {
	struct txobj_thread_list_node * node;
	struct transaction_file_shadow * shadow;
	struct transaction * transaction;
	enum transaction_state status;
	int ret;

	if (!file)
		return -EINVAL;

	transaction = current_transaction();
	if (!transaction)
		return 0;

	status = transaction_status(transaction);
	if (status == TRANSACTION_ABORTED || status == TRANSACTION_ABORTING)
		return -ECANCELED;
	if (status != TRANSACTION_ACTIVE)
		return 0;

	if (transaction_workset_find_object(transaction, &file->transaction_object))
		return 0;

	shadow = kmalloc(sizeof(*shadow), GFP_KERNEL);
	if (!shadow)
		return -ENOMEM;

	shadow->restore = false;
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

	/* File workset callbacks. Commit does not need to publish anything yet because f_pos is updated in place;
	   abort restores the saved offset. */
	node->lock = transaction_file_lock;
	node->unlock = transaction_file_unlock;
	node->abort = transaction_file_abort;
	node->release = transaction_file_release;
	get_file(file);
	ret = transaction_workset_add(transaction, node);
	if (ret)
		goto free_node;

	ret = transaction_object_acquire(transaction, node, TRANSACTION_ACCESS_READ_WRITE, NULL);
	if (!ret) {
		shadow->f_pos = file->f_pos;
		shadow->restore = true;
		return 0;
	}

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
