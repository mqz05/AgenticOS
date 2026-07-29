/* Transactional VFS inode rollback support. */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/transaction.h>

/* Per transaction snapshot of inode metadata. */
struct transaction_inode_shadow {
	umode_t i_mode;
	kuid_t i_uid;
	kgid_t i_gid;
	struct timespec64 i_atime;
	struct timespec64 i_mtime;
	struct timespec64 i_ctime;
};

/* Initialize generic transaction object embedded in struct inode. */
void transaction_inode_init(struct inode * inode) {
	transaction_object_init(&inode->transaction_object, TRANSACTION_OBJECT_INODE);
}
EXPORT_SYMBOL_GPL(transaction_inode_init);

/* Abort callback for inode workset entries.
   Restore inode metadata captured at entry of transaction. */
static int transaction_inode_abort(struct txobj_thread_list_node * node) {
	struct transaction_inode_shadow * shadow = node->shadow_obj;
	struct inode * inode = node->orig_obj;
 
	inode->i_mode = shadow->i_mode;
	inode->i_uid = shadow->i_uid;
	inode->i_gid = shadow->i_gid;
	inode_set_atime_to_ts(inode, shadow->i_atime);
	inode_set_mtime_to_ts(inode, shadow->i_mtime);
	inode_set_ctime_to_ts(inode, shadow->i_ctime);

	return 0;
}

/* Release callback for inode workset entries.
   The generic workset code owns the original inode, this adapter owns the shadow state
   and the inode reference that is acquired when the inode was added to the workset. */
static int transaction_inode_release(struct txobj_thread_list_node * node, int early) {
	struct inode * inode = node->orig_obj;

	kfree(node->shadow_obj);
	iput(inode);

	return 0;
}

/* Add this inode to current transaction's workset and snapshot its metadata.
   If current task is not part of a transaction, this is a no-op.
   If inode is already in the workset, the original snapshot is reused. */
int transaction_inode_snapshot(struct inode * inode) {
	struct transaction_inode_shadow * shadow;
	struct txobj_thread_list_node * node;
	struct transaction * transaction;
	enum transaction_state status;
	int ret;

	if (!inode)
		return -EINVAL;

	transaction = current_transaction();
	if (!transaction)
		return 0;

	status = transaction_status(transaction);
	if (status == TRANSACTION_ABORTED || status == TRANSACTION_ABORTING)
		return -ECANCELED;
	if (status != TRANSACTION_ACTIVE)
		return 0;

	if (transaction_workset_find_object(transaction, &inode->transaction_object))
		return 0;

	shadow = kmalloc(sizeof(*shadow), GFP_KERNEL);
	if (!shadow)
		return -ENOMEM;

	shadow->i_mode = inode->i_mode;
	shadow->i_uid = inode->i_uid;
	shadow->i_gid = inode->i_gid;
	shadow->i_atime = inode_get_atime(inode);
	shadow->i_mtime = inode_get_mtime(inode);
	shadow->i_ctime = inode_get_ctime(inode);

	node = transaction_workset_node_alloc(
		shadow,
		inode,
		&inode->transaction_object,
		TRANSACTION_OBJECT_INODE,
		TRANSACTION_ACCESS_READ_WRITE,
		GFP_KERNEL);
	if (!node) {
		kfree(shadow);
		return -ENOMEM;
	}

	node->abort = transaction_inode_abort;
	node->release = transaction_inode_release;
	ihold(inode);
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
	iput(inode);
	kfree(shadow);
	transaction_workset_node_free(node);

	return ret == -EEXIST ? 0 : ret;
}
EXPORT_SYMBOL_GPL(transaction_inode_snapshot);
