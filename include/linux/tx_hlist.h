#ifndef _LINUX_TX_HLIST_H
#define _LINUX_TX_HLIST_H

#include <linux/list.h>
#include <linux/list_bl.h>

/*
 * Lightweight hlist checkpoint helpers for transactional kernel objects.
 *
 * Linux 6.18 dcache links dentries through hlist_node and hlist_bl_node
 * fields, while the TxOS list2 code covers ordinary list_head lists. These
 * helpers snapshot one embedded hlist node and can later restore that node to
 * the same linked or unhashed state.
 */
struct tx_hlist_node_snapshot {
	struct hlist_node *next;
	struct hlist_node **pprev;
};

struct tx_hlist_bl_node_snapshot {
	struct hlist_bl_node *next;
	struct hlist_bl_node **pprev;
};

static inline void tx_hlist_snapshot(struct hlist_node *node, struct tx_hlist_node_snapshot *snapshot) {
	snapshot->next = node->next;
	snapshot->pprev = node->pprev;
}

static inline void tx_hlist_bl_snapshot(struct hlist_bl_node *node, struct tx_hlist_bl_node_snapshot *snapshot) {
	snapshot->next = node->next;
	snapshot->pprev = node->pprev;
}

static inline void tx_hlist_restore(struct hlist_node *node, const struct tx_hlist_node_snapshot *snapshot) {
	if (!hlist_unhashed(node))
		__hlist_del(node);

	node->next = snapshot->next;
	node->pprev = snapshot->pprev;
	if (snapshot->pprev) {
		WRITE_ONCE(*snapshot->pprev, node);
		if (snapshot->next)
			snapshot->next->pprev = &node->next;
	}
}

static inline void tx_hlist_bl_restore(struct hlist_bl_node *node, const struct tx_hlist_bl_node_snapshot *snapshot) {
	if (!hlist_bl_unhashed(node))
		__hlist_bl_del(node);

	node->next = snapshot->next;
	node->pprev = snapshot->pprev;
	if (snapshot->pprev) {
		WRITE_ONCE(*snapshot->pprev,
			   (struct hlist_bl_node *)
			   ((uintptr_t)node |
			    ((uintptr_t)*snapshot->pprev & LIST_BL_LOCKMASK)));
		if (snapshot->next)
			snapshot->next->pprev = &node->next;
	}
}

#endif /* _LINUX_TX_HLIST_H */
