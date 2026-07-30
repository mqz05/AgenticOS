/* Transactional linked-list API based on TxOS tx_list2.

   tx_list2_head owns the real list and a speculative-list log.
   Each stable object embeds a tx_list2_entry_ref. During a 
   transaction, add operations use a speculative tx_list2_entry,
   and delete operations mark the stable entry as deleted.
*/

#ifndef _LINUX_TX_LIST2_H
#define _LINUX_TX_LIST2_H

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/transaction.h>

#ifdef CONFIG_TRANSACTIONS

/* Per-entry state used to distinguish stable entries from speculative changes. */
enum tx_list2_state {
	TX_LIST2_NON_TX = 0,
	TX_LIST2_TRANSACTIONAL_ADD = 1,
	TX_LIST2_TRANSACTIONAL_DEL = 2,
};

enum tx_list2_mode {
	TX_LIST2_NO_TX = 0,
	TX_LIST2_R = 1,
	TX_LIST2_W = 2,
	TX_LIST2_EXCL = 3,
};

struct tx_list2_head;
struct tx_list2_entry_ref;

/* Physical list node. 
   Stable objects use the embedded entry inside tx_list2_entry_ref.
   Transactional adds allocate a temporary entry that points back to
   the stable ref through cursor. */
struct tx_list2_entry {
	struct list_head list;
	int transactional_state;
	struct transaction *transaction;
	struct tx_list2_entry_ref *cursor;
	struct tx_list2_head *parent;
	struct list_head spec;
	bool embedded;
};

/* Transactional list head.
   The normal list contains both stable and speculative entries.
   spec_list tracks entries that need commit/abort processing. */
struct tx_list2_head {
	struct list_head head;
	spinlock_t lock;
	int mode;
	struct list_head spec_list;
	struct transaction_object transaction_object;
};

/* Stable reference embedded in the object that belongs to the list.
   sentry points to this transaction's speculative add entry. */
struct tx_list2_entry_ref {
	struct tx_list2_entry entry;
	struct tx_list2_entry *sentry;
	struct transaction *transaction;
};

/* Iterator state. Iteration holds the list lock and skips entries that
   should not be visible to the current transaction. */
struct tx_list2_iterator {
	struct list_head *cur;
	struct list_head *next;
	struct tx_list2_head *head;
};

void INIT_TX_LIST2_HEAD(struct tx_list2_head *head);
void INIT_TX_LIST2_REF(struct tx_list2_entry_ref *ref);
int tx_list2_add(struct tx_list2_entry_ref *cursor, struct tx_list2_head *head);
int tx_list2_add_tail(struct tx_list2_entry_ref *cursor, struct tx_list2_head *head);
int tx_list2_del(struct tx_list2_entry_ref *cursor);
int tx_list2_move(struct tx_list2_entry_ref *cursor, struct tx_list2_head *head);
int tx_list2_empty(struct tx_list2_head *head);
bool tx_list2_unreferenced(struct tx_list2_entry_ref *ref);
int tx_list2_get_iterator(struct tx_list2_iterator *iter, struct tx_list2_head *head);
void tx_list2_put_iterator(struct tx_list2_iterator *iter);
int tx_list2_iter_next(struct tx_list2_iterator *iter);

#define tx_list2_iter_entry(iter, type, member) list_entry(list_entry((iter)->cur, struct tx_list2_entry, list)->cursor, type, member)

#define tx_list2_first_entry(head, type, member) list_entry(list_entry((head)->head.next, struct tx_list2_entry, list)->cursor, type, member)

/* Without CONFIG_TRANSACTIONS, tx_list2 collapses to normal list_head behavior
   so non-transactional code can use the same API. */
#else

#define tx_list2_head list_head
#define tx_list2_entry_ref list_head

struct tx_list2_iterator {
	struct list_head *head;
	struct list_head *cur;
};

#define INIT_TX_LIST2_HEAD(head) INIT_LIST_HEAD(head)
#define INIT_TX_LIST2_REF(ref) INIT_LIST_HEAD(ref)
#define tx_list2_add(ref, head) ({ list_add(ref, head); 0; })
#define tx_list2_add_tail(ref, head) ({ list_add_tail(ref, head); 0; })
#define tx_list2_del(ref) ({ list_del_init(ref); 0; })
#define tx_list2_move(ref, head) ({ list_move(ref, head); 0; })
#define tx_list2_empty(head) list_empty(head)
#define tx_list2_unreferenced(ref) list_empty(ref)

#define tx_list2_get_iterator(iter, list) ({(iter)->head = list; (iter)->cur = list; 0;})

#define tx_list2_put_iterator(iter) do { } while (0)

static inline int tx_list2_iter_next(struct tx_list2_iterator * iter) {
	iter->cur = iter->cur->next;
	return iter->cur != iter->head;
}

#define tx_list2_iter_entry(iter, type, member) list_entry((iter)->cur, type, member)
#define tx_list2_first_entry(head, type, member) list_first_entry(head, type, member)

#endif /* CONFIG_TRANSACTIONS */

#endif /* _LINUX_TX_LIST2_H */
