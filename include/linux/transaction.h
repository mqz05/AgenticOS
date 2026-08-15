/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TRANSACTION_H
#define _LINUX_TRANSACTION_H

#include <linux/atomic.h>
#include <linux/gfp_types.h>
#include <linux/list.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#ifdef CONFIG_TRANSACTIONS
#include <linux/skiplist.h>
#endif

struct task_struct;
struct file;
struct inode;
struct _inode;
struct dentry;
struct _dentry;
struct iattr;
struct mnt_idmap;
struct transaction;

#ifdef CONFIG_TRANSACTIONS

/*
 * Common system-transaction vocabulary. Keep this header independent of VFS
 * types: object-specific shadow state belongs in the corresponding adapter.
 */
enum transaction_state {
	TRANSACTION_INACTIVE,
	TRANSACTION_ACTIVE,
	TRANSACTION_ABORTED,
	TRANSACTION_COMMITTING,
	TRANSACTION_ABORTING,
};

enum unsupported_behavior {
	UNSUPPORTED_ABORT,
	UNSUPPORTED_ERROR_CODE,
	UNSUPPORTED_LIVE_DANGEROUSLY,
};

/*
 * A transaction begins with one task. Sharing it with additional tasks is
 * deferred until task checkpoint and rollback support is available.
 */
struct transaction {
	// The tasks associated with this transaction (only one for now)
	struct list_head tasks;
	atomic_t task_count;

	// Used for lifetime management, including wait-queue users.
	refcount_t ref_count;

	// Transaction status word.
	atomic_t status;

	// Serializes end_transaction() while status remains abortable.
	atomic_t finishing;

	// Automatic retry policy and transaction timestamp.
	int autoretry;
	u64 timestamp;

	// Number of retries.
	unsigned int count;

	// Abort and return an error after an explicit abort.
	bool abortWithErr;

	// What to do on an unsupported operation.
	enum unsupported_behavior unsupported_operation_action;

	// object_list tracks ordinary transactional objects such as files and inodes
	struct skiplist_head object_list;
	// list_list tracks transactional list heads separately for finish ordering:
	// list entries are collected separately and merged into the finished workset
	// before normal objects are committed or aborted.
	struct skiplist_head list_list;
	spinlock_t workset_lock;

	// The wait queue for loser transactions.
	wait_queue_head_t losers;

	// The wait queue for commit.
	wait_queue_head_t siblings;

	// Protects the tasks list, state transitions, and ownership publication.
	spinlock_t lock;
};

enum transaction_access_mode {
	TRANSACTION_ACCESS_READ,
	TRANSACTION_ACCESS_READ_WRITE,
	TRANSACTION_ACCESS_EXCLUSIVE,
};

enum transaction_object_type {
	TRANSACTION_OBJECT_FILE,
	TRANSACTION_OBJECT_INODE,
	TRANSACTION_OBJECT_DENTRY,
	TRANSACTION_OBJECT_SUPERBLOCK,
	TRANSACTION_OBJECT_MOUNT,
	TRANSACTION_OBJECT_ADDRESS_SPACE,
	TRANSACTION_OBJECT_SOCKET,
	TRANSACTION_OBJECT_TASK,
	TRANSACTION_OBJECT_LIST_HEAD,
	TRANSACTION_OBJECT_HLIST_HEAD,
	TRANSACTION_OBJECT_CUSTOM,
};

/*
 * System transactions use object-based STM: shared kernel objects carry
 * conflict metadata, while a transaction owns private shadow copies. Keep
 * only synchronization and identity here so object lifetime remains owned by
 * the native kernel subsystem.
 */
struct transaction_object {
	enum transaction_object_type type;
	struct transaction *writer;
	struct list_head readers;
	spinlock_t lock;
	u64 version;
	// Preserve the committed version before an ordinary writer takes over.
	int (*replace_committed)(struct transaction_object *object);
};

// An entry in a transaction's working set.
struct txobj_thread_list_node {
	// This is how we connect to the workset.
	struct skiplist_head workset_list;

	// This is how we connect to the object's reader list.
	struct list_head object_list;
	struct transaction *tx;
	enum transaction_object_type type;
	void *shadow_obj;
	void *orig_obj;
	struct transaction_object *tx_obj;
	enum transaction_access_mode rw;
	// Previous workset node in final object-lock order.
	struct txobj_thread_list_node *ordered_lock_prev;
	// Shared native locks are acquired once across object and list adapters.
	void *blocking_lock_id;
	void *nonblocking_lock_id;
	spinlock_t *nonblocking_nest_lock;
	bool blocking_lock_acquired;
	bool nonblocking_lock_acquired;
	/*
	 * TODO: Optional validation may return an errno to abort before commit. The
	 * other callbacks are expected to succeed; nonzero returns warn.
	 */
	int (*validate)(struct txobj_thread_list_node *node);
	int (*lock)(struct txobj_thread_list_node *node, int blocking);
	int (*unlock)(struct txobj_thread_list_node *node, int blocking);
	int (*commit)(struct txobj_thread_list_node *node);
	int (*abort)(struct txobj_thread_list_node *node);
	int (*release)(struct txobj_thread_list_node *node, int early);
};

void transaction_object_init(struct transaction_object *object, enum transaction_object_type type);
int transaction_object_acquire(struct transaction *transaction,
			       struct txobj_thread_list_node *node,
			       enum transaction_access_mode mode,
			       bool *should_sleep);
struct transaction *transaction_check_asymmetric_conflict(struct transaction_object *object,
							   enum transaction_access_mode mode,
							   bool can_sleep, int *error);
int transaction_wait_on_conflict(struct transaction *winner);
void transaction_object_remove_ownership_locked(struct txobj_thread_list_node *node);
void transaction_object_remove_ownership(struct txobj_thread_list_node *node);

struct txobj_thread_list_node *transaction_workset_node_alloc(void *shadow_obj,
                                                              void *orig_obj,
                                                              struct transaction_object *tx_obj,
                                                              enum transaction_object_type type,
                                                              enum transaction_access_mode rw,
                                                              gfp_t gfp);
void transaction_workset_node_free(struct txobj_thread_list_node *node);
int transaction_workset_add(struct transaction *transaction, struct txobj_thread_list_node *node);
struct txobj_thread_list_node *transaction_workset_find_orig(struct transaction *transaction,
                                                             const void *orig_obj);
struct txobj_thread_list_node *transaction_workset_find_object(struct transaction *transaction,
                                                               struct transaction_object *tx_obj);
struct txobj_thread_list_node *transaction_workset_remove(struct transaction *transaction,
                                                          struct txobj_thread_list_node *node);
bool transaction_workset_empty(struct transaction *transaction);
int transaction_list_workset_add(struct transaction *transaction, struct txobj_thread_list_node *node);
struct txobj_thread_list_node *transaction_list_workset_find_object(struct transaction *transaction,
                                                                    struct transaction_object *tx_obj);
struct txobj_thread_list_node *transaction_list_workset_remove(struct transaction *transaction,
                                                               struct txobj_thread_list_node *node);

void transaction_file_init(struct file *file);
loff_t transaction_file_get_pos(struct file *file);
int transaction_file_snapshot(struct file *file);
int transaction_file_set_pos(struct file *file, loff_t pos);
void transaction_inode_init(struct inode *inode);
void transaction_inode_destroy(struct inode *inode);
struct _inode *transaction_inode_get(struct inode *inode, enum transaction_access_mode mode);
struct _inode *transaction_inode_visible(struct inode *inode);
struct _inode *transaction_inode_shadow(struct inode *inode);
bool transaction_inode_get_size(const struct inode *inode, loff_t *size);
bool transaction_inode_set_size(struct inode *inode, loff_t size);
bool transaction_inode_setattr_copy(struct mnt_idmap *idmap, struct inode *inode, const struct iattr *attr);
int transaction_inode_read(struct inode *inode);
int transaction_inode_snapshot(struct inode *inode);
int transaction_inode_replace_committed_locked(struct transaction_object *object);
void transaction_dentry_init(struct dentry *dentry);
void transaction_dentry_destroy(struct dentry *dentry);
void transaction_dentry_name_get(struct _dentry *contents, struct dentry *dentry);
void transaction_dentry_name_copy(struct _dentry *dest, const struct _dentry *source);
void transaction_dentry_name_put(struct _dentry *contents);
void transaction_dentry_name_restore(struct dentry *dentry, const struct _dentry *contents);
struct _dentry *transaction_dentry_get(struct dentry *dentry, enum transaction_access_mode mode);
struct _dentry *transaction_dentry_visible(struct dentry *dentry);
struct _dentry *transaction_dentry_shadow(struct dentry *dentry);
bool transaction_dentry_get_flags(const struct dentry *dentry, unsigned int *flags);
bool transaction_dentry_set_flags(struct dentry *dentry, unsigned int flags, unsigned int mask);
int transaction_dentry_snapshot(struct dentry *dentry);
int transaction_dentry_snapshot_locked(struct dentry *dentry);
int transaction_dentry_snapshot_unlink(struct dentry *dentry);
int transaction_dentry_record_inode_change(struct dentry *dentry, struct inode *old_inode, struct inode *new_inode);
void transaction_dentry_publish_inode(struct dentry *dentry, struct inode *old_inode, struct inode *new_inode);
void transaction_dentry_put_committed_inode(struct dentry *dentry, struct inode *inode);
int transaction_dentry_replace_committed_locked(struct transaction_object *object);

struct transaction *transaction_alloc(gfp_t gfp);
struct transaction *transaction_get(struct transaction *transaction);
void transaction_put(struct transaction *transaction);

enum transaction_state transaction_status(const struct transaction *transaction);
bool inactive_transaction(const struct transaction *transaction);
bool active_transaction(const struct transaction *transaction);
bool live_transaction(const struct transaction *transaction);
bool committing_transaction(const struct transaction *transaction);
bool aborting_transaction(const struct transaction *transaction);
bool transaction_contention_manager(struct transaction *a, struct transaction *b, bool *should_sleep);

int begin_transaction(struct transaction *transaction);
int abort_transaction(struct transaction *transaction);
int end_transaction(struct transaction *transaction);
long transaction_sys_xbegin(void);
long transaction_sys_xend(void);
long transaction_sys_xabort(void);

void transaction_task_init(struct task_struct *task);
int transaction_attach_task(struct transaction *transaction, struct task_struct *task);
void transaction_detach_task(struct task_struct *task);
void transaction_task_exit(struct task_struct *task);
int transaction_task_fork(const struct task_struct *task);
struct transaction *current_transaction(void);
#else
static inline void transaction_task_init(struct task_struct *task) { }
static inline void transaction_task_exit(struct task_struct *task) { }
static inline int transaction_task_fork(const struct task_struct *task) {
	return 0;
}
static inline struct transaction *current_transaction(void) {
	return NULL;
}
static inline void transaction_file_init(struct file *file) { }
static inline int transaction_file_snapshot(struct file *file) {
	return 0;
}
static inline void transaction_inode_init(struct inode *inode) { }
static inline void transaction_inode_destroy(struct inode *inode) { }
static inline struct _inode *transaction_inode_shadow(struct inode *inode) {
	return NULL;
}
static inline bool transaction_inode_get_size(const struct inode *inode, loff_t *size) {
	return false;
}
static inline bool transaction_inode_set_size(struct inode *inode, loff_t size) {
	return false;
}
static inline bool transaction_inode_setattr_copy(struct mnt_idmap *idmap, struct inode *inode,
						  						  const struct iattr *attr) {
	return false;
}
static inline int transaction_inode_read(struct inode *inode) {
	return 0;
}
static inline int transaction_inode_snapshot(struct inode *inode) {
	return 0;
}
static inline void transaction_dentry_init(struct dentry *dentry) { }
static inline void transaction_dentry_destroy(struct dentry *dentry) { }
static inline struct _dentry *transaction_dentry_visible(struct dentry *dentry) {
	return NULL;
}
static inline struct _dentry *transaction_dentry_shadow(struct dentry *dentry) {
	return NULL;
}
static inline bool transaction_dentry_get_flags(const struct dentry *dentry, unsigned int *flags) {
	return false;
}
static inline bool transaction_dentry_set_flags(struct dentry *dentry, unsigned int flags,
						 unsigned int mask) {
	return false;
}
static inline int transaction_dentry_snapshot(struct dentry *dentry) {
	return 0;
}
static inline int transaction_dentry_snapshot_locked(struct dentry *dentry) {
	return 0;
}
static inline int transaction_dentry_snapshot_unlink(struct dentry *dentry) {
	return 0;
}
#endif

#endif /* _LINUX_TRANSACTION_H */
