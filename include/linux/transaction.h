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

	// Automatic retry policy and transaction timestamp.
	int autoretry;
	u64 timestamp;

	// Number of retries.
	unsigned int count;

	// Abort and return an error after an explicit abort.
	bool abortWithErr;

	// What to do on an unsupported operation.
	enum unsupported_behavior unsupported_operation_action;

	struct skiplist_head object_list;
	spinlock_t workset_lock;

	// The wait queue for loser transactions.
	wait_queue_head_t losers;

	// The wait queue for commit.
	wait_queue_head_t siblings;

	// Protects the tasks list.
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
	int (*validate)(struct txobj_thread_list_node *node);
	int (*lock)(struct txobj_thread_list_node *node, int blocking);
	int (*unlock)(struct txobj_thread_list_node *node, int blocking);
	int (*commit)(struct txobj_thread_list_node *node);
	int (*abort)(struct txobj_thread_list_node *node);
	int (*release)(struct txobj_thread_list_node *node, int early);
};

void transaction_object_init(struct transaction_object *object, enum transaction_object_type type);

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

struct transaction *transaction_alloc(gfp_t gfp);
struct transaction *transaction_get(struct transaction *transaction);
void transaction_put(struct transaction *transaction);

enum transaction_state transaction_status(const struct transaction *transaction);
bool inactive_transaction(const struct transaction *transaction);
bool active_transaction(const struct transaction *transaction);
bool live_transaction(const struct transaction *transaction);
bool committing_transaction(const struct transaction *transaction);
bool aborting_transaction(const struct transaction *transaction);

int begin_transaction(struct transaction *transaction);
int abort_transaction(struct transaction *transaction);
int end_transaction(struct transaction *transaction);

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
#endif

#endif /* _LINUX_TRANSACTION_H */
