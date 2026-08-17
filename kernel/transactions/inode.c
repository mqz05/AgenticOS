// SPDX-License-Identifier: GPL-2.0
/* Transactional VFS inode versioning support. */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/mnt_idmapping.h>
#include <linux/pagemap.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/transaction.h>

struct transaction_inode_pagecache_chunk {
	struct list_head list;
	loff_t pos;
	size_t len;
	u8 data[];
};

static void transaction_inode_pagecache_init(struct _inode * inode) {
	INIT_LIST_HEAD(&inode->tx_pagecache);
}

static void transaction_inode_pagecache_free(struct _inode * inode) {
	struct transaction_inode_pagecache_chunk *chunk;
	struct transaction_inode_pagecache_chunk *next;

	if (!inode)
		return;

	list_for_each_entry_safe(chunk, next, &inode->tx_pagecache, list) {
		list_del(&chunk->list);
		kfree(chunk);
	}
}

static void transaction_inode_contents_free(struct rcu_head *rcu) {
	struct _inode *inode = container_of(rcu, struct _inode, i_rcu);

	kfree(inode);
}

static void transaction_inode_contents_put(struct _inode *inode) {
	if (!inode || !refcount_dec_and_test(&inode->tx_refcount))
		return;
	transaction_inode_pagecache_free(inode);
	if (!inode->embedded)
		call_rcu(&inode->i_rcu, transaction_inode_contents_free);
}

static void transaction_inode_copy_from_stable(struct _inode *contents, struct inode *inode) {
	contents->parent = inode;
	contents->shadow = NULL;
	contents->i_mode = inode->i_mode;
	contents->i_uid = inode->i_uid;
	contents->i_gid = inode->i_gid;
	contents->i_flags = inode->i_flags;
	contents->i_nlink = inode->i_nlink;
	contents->i_size = __i_size_read(inode);
	contents->i_blocks = inode->i_blocks;
	contents->i_bytes = inode->i_bytes;
	atomic64_set(&contents->i_version, atomic64_read(&inode->i_version));
	contents->i_atime_sec = inode->i_atime_sec;
	contents->i_mtime_sec = inode->i_mtime_sec;
	contents->i_ctime_sec = inode->i_ctime_sec;
	contents->i_atime_nsec = inode->i_atime_nsec;
	contents->i_mtime_nsec = inode->i_mtime_nsec;
	contents->i_ctime_nsec = inode->i_ctime_nsec;
	transaction_inode_pagecache_init(contents);
}

static struct _inode *transaction_inode_committed_locked(struct inode *inode) {
	struct _inode *contents;

	contents = rcu_dereference_protected(inode->i_contents, lockdep_is_held(&inode->i_lock));
	if (contents)
		return contents;

	contents = &inode->i_committed;
	transaction_inode_copy_from_stable(contents, inode);
	refcount_set(&contents->tx_refcount, 1);
	contents->embedded = true;
	rcu_assign_pointer(inode->i_contents, contents);
	return contents;
}

int transaction_inode_replace_committed_locked(struct transaction_object *object) {
	struct inode *inode = container_of(object, struct inode, transaction_object);
	struct _inode *committed;
	struct _inode *replacement;

	lockdep_assert_held(&inode->i_lock);
	committed = transaction_inode_committed_locked(inode);
	replacement = kmalloc(sizeof(*replacement), GFP_ATOMIC);
	if (!replacement)
		return -ENOMEM;
	memcpy(replacement, committed, sizeof(*replacement));
	replacement->parent = inode;
	replacement->shadow = NULL;
	refcount_set(&replacement->tx_refcount, 1);
	memset(&replacement->i_rcu, 0, sizeof(replacement->i_rcu));
	replacement->embedded = false;
	transaction_inode_pagecache_init(replacement);
	rcu_assign_pointer(inode->i_contents, replacement);
	transaction_inode_contents_put(committed);
	return 0;
}
EXPORT_SYMBOL_GPL(transaction_inode_replace_committed_locked);

static void transaction_inode_shadow_copy(struct _inode *shadow_inode,
					  struct _inode *committed) {
	memcpy(shadow_inode, committed, sizeof(*shadow_inode));
	shadow_inode->shadow = committed;
	refcount_inc(&committed->tx_refcount);
	refcount_set(&shadow_inode->tx_refcount, 1);
	memset(&shadow_inode->i_rcu, 0, sizeof(shadow_inode->i_rcu));
	shadow_inode->embedded = false;
	transaction_inode_pagecache_init(shadow_inode);
}

static bool transaction_inode_matches_stable(struct _inode *contents, struct inode *inode) {
	return contents->i_mode == inode->i_mode && uid_eq(contents->i_uid, inode->i_uid) &&
	       gid_eq(contents->i_gid, inode->i_gid) && contents->i_flags == inode->i_flags &&
	       contents->i_nlink == inode->i_nlink && contents->i_size == __i_size_read(inode) &&
	       contents->i_blocks == inode->i_blocks && contents->i_bytes == inode->i_bytes &&
	       atomic64_read(&contents->i_version) == atomic64_read(&inode->i_version) &&
	       contents->i_atime_sec == inode->i_atime_sec &&
	       contents->i_mtime_sec == inode->i_mtime_sec &&
	       contents->i_ctime_sec == inode->i_ctime_sec &&
	       contents->i_atime_nsec == inode->i_atime_nsec &&
	       contents->i_mtime_nsec == inode->i_mtime_nsec &&
	       contents->i_ctime_nsec == inode->i_ctime_nsec;
}

static void transaction_inode_private_put(struct _inode *shadow_inode) {
	if (!shadow_inode)
		return;
	transaction_inode_contents_put(shadow_inode->shadow);
	shadow_inode->shadow = NULL;
	transaction_inode_contents_put(shadow_inode);
}

static bool transaction_inode_pagecache_covered(struct _inode *shadow_inode,
						loff_t pos, size_t len)
{
	struct transaction_inode_pagecache_chunk *chunk;
	loff_t end = pos + len;

	list_for_each_entry(chunk, &shadow_inode->tx_pagecache, list) {
		if (chunk->pos <= pos && chunk->pos + chunk->len >= end)
			return true;
	}

	return false;
}

static int transaction_inode_pagecache_snapshot(struct _inode *shadow_inode,
						struct address_space *mapping,
						loff_t pos, size_t len)
{
	struct transaction_inode_pagecache_chunk *chunk;
	struct folio *folio;
	size_t offset;
	void *src;

	if (!len || transaction_inode_pagecache_covered(shadow_inode, pos, len))
		return 0;

	folio = filemap_get_folio(mapping, pos >> PAGE_SHIFT);
	if (IS_ERR(folio))
		return 0;

	folio_lock(folio);
	if (folio->mapping != mapping) {
		folio_unlock(folio);
		folio_put(folio);
		return 0;
	}

	chunk = kmalloc(struct_size(chunk, data, len), GFP_KERNEL);
	if (!chunk) {
		folio_unlock(folio);
		folio_put(folio);
		return -ENOMEM;
	}

	chunk->pos = pos;
	chunk->len = len;
	offset = pos - folio_pos(folio);
	src = kmap_local_folio(folio, offset);
	memcpy(chunk->data, src, len);
	kunmap_local(src);
	list_add_tail(&chunk->list, &shadow_inode->tx_pagecache);

	folio_unlock(folio);
	folio_put(folio);
	return 0;
}

static int transaction_inode_pagecache_snapshot_range(struct _inode *shadow_inode,
						      struct inode *inode,
						      loff_t start, loff_t end)
{
	struct address_space *mapping = inode->i_mapping;
	loff_t pos = start;
	int ret;

	if (!mapping || start >= end)
		return 0;

	while (pos < end) {
		size_t offset = pos & (PAGE_SIZE - 1);
		size_t len = min_t(loff_t, end - pos, PAGE_SIZE - offset);

		ret = transaction_inode_pagecache_snapshot(shadow_inode, mapping, pos, len);
		if (ret)
			return ret;
		pos += len;
	}

	return 0;
}

static int transaction_inode_pagecache_restore(struct inode *inode,
					       struct _inode *shadow_inode)
{
	struct transaction_inode_pagecache_chunk *chunk;
	struct address_space *mapping = inode->i_mapping;
	int ret = 0;

	if (!mapping)
		return 0;

	list_for_each_entry(chunk, &shadow_inode->tx_pagecache, list) {
		struct folio *folio;
		size_t done = 0;

		while (done < chunk->len) {
			loff_t pos = chunk->pos + done;
			size_t offset = pos & (PAGE_SIZE - 1);
			size_t len = min_t(size_t, chunk->len - done, PAGE_SIZE - offset);
			void *dst;

			folio = __filemap_get_folio(mapping, pos >> PAGE_SHIFT,
						    FGP_WRITEBEGIN, mapping_gfp_mask(mapping));
			if (IS_ERR(folio)) {
				ret = PTR_ERR(folio);
				break;
			}

			if (!folio_test_uptodate(folio)) {
				folio_zero_range(folio, 0, folio_size(folio));
				folio_mark_uptodate(folio);
			}

			dst = kmap_local_folio(folio, offset);
			memcpy(dst, chunk->data + done, len);
			kunmap_local(dst);
			flush_dcache_folio(folio);
			folio_mark_dirty(folio);
			folio_unlock(folio);
			folio_put(folio);
			done += len;
		}

		if (ret)
			return ret;
	}

	return 0;
}

/* Initialize the generic transaction object embedded in struct inode. */
void transaction_inode_init(struct inode *inode) {
	transaction_object_init(&inode->transaction_object, TRANSACTION_OBJECT_INODE);
	inode->transaction_object.replace_committed = transaction_inode_replace_committed_locked;
	RCU_INIT_POINTER(inode->i_contents, NULL);
	memset(&inode->i_committed, 0, sizeof(inode->i_committed));
}
EXPORT_SYMBOL_GPL(transaction_inode_init);

void transaction_inode_destroy(struct inode *inode) {
	struct _inode *contents;

	contents = rcu_dereference_protected(inode->i_contents, 1);
	RCU_INIT_POINTER(inode->i_contents, NULL);
	if (contents && !contents->embedded)
		transaction_inode_contents_put(contents);
}
EXPORT_SYMBOL_GPL(transaction_inode_destroy);

static int transaction_inode_lock(struct txobj_thread_list_node *node, int blocking) {
	struct inode *inode = node->orig_obj;
	struct txobj_thread_list_node *previous;

	for (previous = node->ordered_lock_prev; previous; previous = previous->ordered_lock_prev) {
		if (previous->type != TRANSACTION_OBJECT_INODE)
			continue;
		if (blocking && previous->blocking_lock_acquired) {
			struct inode *nest = previous->orig_obj;

			down_write_nest_lock(&inode->i_rwsem, &nest->i_rwsem);
			return 0;
		}
		if (!blocking && previous->nonblocking_lock_acquired && previous->nonblocking_nest_lock) {
			spin_lock_nest_lock(&inode->i_lock, previous->nonblocking_nest_lock);
			return 0;
		}
	}
	if (blocking)
		inode_lock(inode);
	else
		spin_lock(&inode->i_lock);
	return 0;
}

static int transaction_inode_unlock(struct txobj_thread_list_node *node, int blocking) {
	struct inode *inode = node->orig_obj;

	if (blocking)
		inode_unlock(inode);
	else
		spin_unlock(&inode->i_lock);
	return 0;
}

static void transaction_inode_copy_to_stable(struct inode *inode, struct _inode *contents) {
	unsigned int old_nlink = inode->i_nlink;

	inode->i_mode = contents->i_mode;
	inode->i_uid = contents->i_uid;
	inode->i_gid = contents->i_gid;
	inode->i_flags = contents->i_flags;
	if (old_nlink && !contents->i_nlink)
		atomic_long_inc(&inode->i_sb->s_remove_count);
	else if (!old_nlink && contents->i_nlink)
		atomic_long_dec(&inode->i_sb->s_remove_count);
	inode->__i_nlink = contents->i_nlink;
	__i_size_write(inode, contents->i_size);
	inode->i_blocks = contents->i_blocks;
	inode->i_bytes = contents->i_bytes;
	atomic64_set(&inode->i_version, atomic64_read(&contents->i_version));
	inode->i_atime_sec = contents->i_atime_sec;
	inode->i_mtime_sec = contents->i_mtime_sec;
	inode->i_ctime_sec = contents->i_ctime_sec;
	inode->i_atime_nsec = contents->i_atime_nsec;
	inode->i_mtime_nsec = contents->i_mtime_nsec;
	inode->i_ctime_nsec = contents->i_ctime_nsec;
}

/* Publish a private inode version while the ordered inode and object locks are held. */
static int transaction_inode_commit(struct txobj_thread_list_node *node) {
	struct _inode *shadow_inode = node->shadow_obj;
	struct inode *inode = node->orig_obj;
	struct _inode *baseline;
	struct _inode *old;

	if (!shadow_inode)
		return 0;
	if (node->rw == TRANSACTION_ACCESS_READ) {
		transaction_inode_contents_put(shadow_inode);
		node->shadow_obj = NULL;
		return 0;
	}

	old = transaction_inode_committed_locked(inode);
	baseline = shadow_inode->shadow;
	WARN_ON_ONCE(baseline != old);
	transaction_inode_copy_to_stable(inode, shadow_inode);
	transaction_inode_pagecache_free(shadow_inode);
	transaction_inode_pagecache_init(shadow_inode);
	shadow_inode->shadow = NULL;
	rcu_assign_pointer(inode->i_contents, shadow_inode);
	node->shadow_obj = NULL;
	node->tx_obj->version++;
	transaction_inode_contents_put(old);
	transaction_inode_contents_put(baseline);
	return 0;
}

/* Abort discards the private version. */
static int transaction_inode_abort(struct txobj_thread_list_node *node) {
	if (node->rw == TRANSACTION_ACCESS_READ) {
		transaction_inode_contents_put(node->shadow_obj);
	} else {
		transaction_inode_pagecache_restore(node->orig_obj, node->shadow_obj);
		transaction_inode_private_put(node->shadow_obj);
	}
	node->shadow_obj = NULL;
	return 0;
}

/* The generic workset owns the entry, this adapter owns the inode reference. */
static int transaction_inode_release(struct txobj_thread_list_node *node, int early) {
	struct inode *inode = node->orig_obj;

	if (node->rw == TRANSACTION_ACCESS_READ)
		transaction_inode_contents_put(node->shadow_obj);
	else
		transaction_inode_private_put(node->shadow_obj);
	node->shadow_obj = NULL;
	iput(inode);
	return 0;
}

static void transaction_inode_setup_node(struct txobj_thread_list_node *node) {
	struct inode *inode = node->orig_obj;

	node->lock = transaction_inode_lock;
	node->unlock = transaction_inode_unlock;
	node->commit = transaction_inode_commit;
	node->abort = transaction_inode_abort;
	node->release = transaction_inode_release;
	node->blocking_lock_id = &inode->i_rwsem;
	node->nonblocking_lock_id = &inode->i_lock;
	node->nonblocking_nest_lock = &inode->i_lock;
}

static struct _inode *transaction_inode_acquire_version(struct inode *inode,
							 enum transaction_access_mode mode) {
	struct _inode *candidate = NULL;
	struct _inode *committed;
	struct _inode *old = NULL;
	struct _inode *shadow_inode = NULL;

	if (mode == TRANSACTION_ACCESS_READ_WRITE) {
		shadow_inode = kmalloc(sizeof(*shadow_inode), GFP_KERNEL);
		if (!shadow_inode)
			return NULL;
	}

retry:
	spin_lock(&inode->i_lock);
	committed = rcu_dereference_protected(inode->i_contents,
					      lockdep_is_held(&inode->i_lock));
	if (!committed) {
		committed = &inode->i_committed;
		transaction_inode_copy_from_stable(committed, inode);
		refcount_set(&committed->tx_refcount, 1);
		committed->embedded = true;
		rcu_assign_pointer(inode->i_contents, committed);
	} else if (!transaction_inode_matches_stable(committed, inode)) {
		// Ordinary updates make the stable inode the next committed baseline.
		if (!candidate) {
			spin_unlock(&inode->i_lock);
			candidate = kmalloc(sizeof(*candidate), GFP_KERNEL);
			if (!candidate) {
				kfree(shadow_inode);
				return NULL;
			}
			goto retry;
		}
		old = committed;
		committed = candidate;
		candidate = NULL;
		transaction_inode_copy_from_stable(committed, inode);
		refcount_set(&committed->tx_refcount, 1);
		committed->embedded = false;
		rcu_assign_pointer(inode->i_contents, committed);
	}
	if (mode == TRANSACTION_ACCESS_READ) {
		refcount_inc(&committed->tx_refcount);
		shadow_inode = committed;
	} else {
		transaction_inode_shadow_copy(shadow_inode, committed);
	}
	spin_unlock(&inode->i_lock);
	kfree(candidate);
	transaction_inode_contents_put(old);
	return shadow_inode;
}

/* Acquire ownership before referencing or copying the committed contents. */
struct _inode *transaction_inode_get(struct inode *inode, enum transaction_access_mode mode) {
	struct txobj_thread_list_node *node;
	struct transaction *transaction;
	struct _inode *shadow_inode;
	enum transaction_state status;
	int ret;

	if (!inode)
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

	node = transaction_workset_find_object(transaction, &inode->transaction_object);
	if (node) {
		if (node->rw >= mode)
			return node->shadow_obj;

		shadow_inode = kmalloc(sizeof(*shadow_inode), GFP_KERNEL);
		if (!shadow_inode)
			return ERR_PTR(-ENOMEM);
		ret = transaction_object_acquire(transaction, node, mode, NULL);
		if (ret) {
			kfree(shadow_inode);
			return ERR_PTR(ret);
		}
		transaction_inode_shadow_copy(shadow_inode, node->shadow_obj);
		transaction_inode_contents_put(node->shadow_obj);
		node->shadow_obj = shadow_inode;
		node->rw = mode;
		return shadow_inode;
	}

	node = transaction_workset_node_alloc(NULL, inode, &inode->transaction_object,
					      TRANSACTION_OBJECT_INODE, mode, GFP_KERNEL);
	if (!node)
		return ERR_PTR(-ENOMEM);
	transaction_inode_setup_node(node);
	ihold(inode);
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

	shadow_inode = transaction_inode_acquire_version(inode, mode);
	if (!shadow_inode) {
		abort_transaction(transaction);
		return ERR_PTR(-ENOMEM);
	}
	node->shadow_obj = shadow_inode;
	return shadow_inode;

free_node:
	iput(inode);
	transaction_workset_node_free(node);
	return ERR_PTR(ret == -EEXIST ? -EBUSY : ret);
}
EXPORT_SYMBOL_GPL(transaction_inode_get);

static struct txobj_thread_list_node *transaction_inode_workset_node(struct inode *inode) {
	struct txobj_thread_list_node *node;
	struct transaction *transaction = current_transaction();

	if (!transaction || !inode)
		return NULL;
	node = transaction_workset_find_object(transaction, &inode->transaction_object);
	return node;
}

struct _inode *transaction_inode_visible(struct inode *inode) {
	struct txobj_thread_list_node *node = transaction_inode_workset_node(inode);

	return node ? node->shadow_obj : NULL;
}
EXPORT_SYMBOL_GPL(transaction_inode_visible);

/* Return only a writable private version. A read-only version must never be
 * modified in place because it is also the committed version. */
struct _inode *transaction_inode_shadow(struct inode *inode) {
	struct txobj_thread_list_node *node = transaction_inode_workset_node(inode);
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
EXPORT_SYMBOL_GPL(transaction_inode_shadow);

bool transaction_inode_get_size(const struct inode *inode, loff_t *size) {
	struct _inode *contents = transaction_inode_visible((struct inode *)inode);

	if (!contents)
		return false;
	*size = contents->i_size;
	return true;
}
EXPORT_SYMBOL_GPL(transaction_inode_get_size);

bool transaction_inode_set_size(struct inode *inode, loff_t size) {
	struct _inode *shadow_inode = transaction_inode_shadow(inode);

	if (!shadow_inode)
		return false;
	if (IS_ERR(shadow_inode))
		return true;
	shadow_inode->i_size = size;
	return true;
}
EXPORT_SYMBOL_GPL(transaction_inode_set_size);

int transaction_inode_snapshot_truncate(struct inode *inode, loff_t oldsize,
					loff_t newsize)
{
	struct _inode *shadow_inode;

	if (!inode || newsize >= oldsize)
		return 0;

	shadow_inode = transaction_inode_shadow(inode);
	if (!shadow_inode)
		return 0;
	if (IS_ERR(shadow_inode))
		return PTR_ERR(shadow_inode);

	return transaction_inode_pagecache_snapshot_range(shadow_inode, inode,
							  newsize, oldsize);
}
EXPORT_SYMBOL_GPL(transaction_inode_snapshot_truncate);

bool transaction_inode_setattr_copy(struct mnt_idmap *idmap, struct inode *inode,
				    const struct iattr *attr) {
	struct _inode *shadow_inode = transaction_inode_shadow(inode);
	unsigned int ia_valid = attr->ia_valid;

	if (!shadow_inode)
		return false;
	if (IS_ERR(shadow_inode))
		return true;
	if (ia_valid & ATTR_UID)
		shadow_inode->i_uid = from_vfsuid(idmap, i_user_ns(inode), attr->ia_vfsuid);
	if (ia_valid & ATTR_GID)
		shadow_inode->i_gid = from_vfsgid(idmap, i_user_ns(inode), attr->ia_vfsgid);
	if (ia_valid & ATTR_MODE) {
		umode_t mode = attr->ia_mode;
		vfsgid_t vfsgid = make_vfsgid(idmap, i_user_ns(inode), shadow_inode->i_gid);

		if (!in_group_or_capable(idmap, inode, vfsgid))
			mode &= ~S_ISGID;
		shadow_inode->i_mode = mode;
	}
	if (ia_valid & ATTR_ATIME) {
		shadow_inode->i_atime_sec = attr->ia_atime.tv_sec;
		shadow_inode->i_atime_nsec = attr->ia_atime.tv_nsec;
	}
	if (ia_valid & ATTR_MTIME) {
		shadow_inode->i_mtime_sec = attr->ia_mtime.tv_sec;
		shadow_inode->i_mtime_nsec = attr->ia_mtime.tv_nsec;
	}
	if (ia_valid & (ATTR_CTIME | ATTR_CTIME_SET)) {
		shadow_inode->i_ctime_sec = attr->ia_ctime.tv_sec;
		shadow_inode->i_ctime_nsec = attr->ia_ctime.tv_nsec;
	}
	return true;
}
EXPORT_SYMBOL_GPL(transaction_inode_setattr_copy);

int transaction_inode_read(struct inode *inode) {
	struct transaction *winner;
	struct _inode *contents;
	int ret;

	if (!inode)
		return -EINVAL;
	if (!current_transaction()) {
		for (;;) {
			spin_lock(&inode->i_lock);
			winner = transaction_check_asymmetric_conflict(&inode->transaction_object,
								       TRANSACTION_ACCESS_READ, true, &ret);
			spin_unlock(&inode->i_lock);
			if (!winner)
				return ret;
			ret = transaction_wait_on_conflict(winner);
			if (ret)
				return ret;
		}
	}
	contents = transaction_inode_get(inode, TRANSACTION_ACCESS_READ);
	return IS_ERR(contents) ? PTR_ERR(contents) : 0;
}
EXPORT_SYMBOL_GPL(transaction_inode_read);

/* Add this inode to the current transaction's workset for read/write access. */
int transaction_inode_snapshot(struct inode *inode) {
	struct transaction *winner;
	struct _inode *shadow_inode;
	int ret;

	if (!inode)
		return -EINVAL;
	if (!current_transaction()) {
		spin_lock(&inode->i_lock);
		winner = transaction_check_asymmetric_conflict(&inode->transaction_object,
								       TRANSACTION_ACCESS_READ_WRITE, false, &ret);
		spin_unlock(&inode->i_lock);
		if (WARN_ON_ONCE(winner)) {
			transaction_put(winner);
			return -EUCLEAN;
		}
		return ret;
	}
	shadow_inode = transaction_inode_get(inode, TRANSACTION_ACCESS_READ_WRITE);
	return IS_ERR(shadow_inode) ? PTR_ERR(shadow_inode) : 0;
}
EXPORT_SYMBOL_GPL(transaction_inode_snapshot);
