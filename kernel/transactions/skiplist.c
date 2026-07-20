// SPDX-License-Identifier: GPL-2.0
// Probabilistic ordered skiplist support.

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/random.h>
#include <linux/skiplist.h>

// Initialize an empty sentinel head at the minimum level.
void skiplist_init_head(struct skiplist_head *head) {
	unsigned int level;

	head->level = 1;
	for (level = 0; level < SKIPLIST_MAX_LEVEL; level++) {
		head->prev[level] = head;
		head->next[level] = head;
	}
}
EXPORT_SYMBOL_GPL(skiplist_init_head);

// Choose a random height and initialize an unlinked skiplist node.
void skiplist_init_node(struct skiplist_head *node) {
	u32 random = get_random_u32();
	unsigned int level;

	node->level = 1;
	while (node->level < SKIPLIST_MAX_LEVEL &&
	       !(random & 1)) {
		node->level++;
		random >>= 1;
	}

	for (level = 0; level < SKIPLIST_MAX_LEVEL; level++) {
		node->prev[level] = node;
		node->next[level] = node;
	}
}
EXPORT_SYMBOL_GPL(skiplist_init_node);

bool skiplist_empty(const struct skiplist_head *head) {
	return head->next[0] == head;
}
EXPORT_SYMBOL_GPL(skiplist_empty);

bool skiplist_linked(const struct skiplist_head *node) {
	return node->next[0] != node;
}
EXPORT_SYMBOL_GPL(skiplist_linked);

// Insert a node in comparator order, rejecting an equal key.
int skiplist_insert(struct skiplist_head *node,
                    struct skiplist_head *head,
                    skiplist_cmp_t compare) {
	struct skiplist_head *cursor = head;
	struct skiplist_head *next;
	int comparison;
	int level;

	if (!node || !head || !compare)
		return -EINVAL;
	if (!node->level || node->level > SKIPLIST_MAX_LEVEL ||
	    !head->level || head->level > SKIPLIST_MAX_LEVEL)
		return -EINVAL;
	if (skiplist_linked(node))
		return -EBUSY;

	for (level = head->level - 1; level >= 0; level--) {
		for (;;) {
			next = cursor->next[level];
			if (next == head)
				break;

			comparison = compare(next, node);
			if (!comparison)
				return -EEXIST;
			if (comparison > 0)
				break;
			cursor = next;
		}
	}

	skiplist_insert_at(node, head, cursor);
	return 0;
}
EXPORT_SYMBOL_GPL(skiplist_insert);

// Link a node after a known level-zero predecessor.
void skiplist_insert_at(struct skiplist_head *node,
                        struct skiplist_head *head,
                        struct skiplist_head *after) {
	struct skiplist_head *cursor = after;
	unsigned int level = 0;

	while (level < node->level) {
		if (cursor == head && head->level <= level) {
			head->level = level + 1;
			head->next[level] = node;
			head->prev[level] = node;
			node->next[level] = head;
			node->prev[level] = head;
			level++;
		} else if (level < cursor->level) {
			cursor->next[level]->prev[level] = node;
			node->next[level] = cursor->next[level];
			cursor->next[level] = node;
			node->prev[level] = cursor;
			level++;
		} else {
			cursor = cursor->prev[level - 1];
		}
	}
}
EXPORT_SYMBOL_GPL(skiplist_insert_at);

// Unlink a node and remove now-unused upper levels from the head.
void skiplist_del(struct skiplist_head *node, struct skiplist_head *head) {
	unsigned int level;

	if (!skiplist_linked(node))
		return;

	for (level = 0; level < node->level; level++) {
		node->next[level]->prev[level] = node->prev[level];
		node->prev[level]->next[level] = node->next[level];
		node->next[level] = node;
		node->prev[level] = node;
	}

	while (head->level > 1 && head->next[head->level - 1] == head)
		head->level--;
}
EXPORT_SYMBOL_GPL(skiplist_del);

// Prepend all nodes from list to head, then reinitialize list.
void skiplist_splice_init(struct skiplist_head *list, struct skiplist_head *head) {
	struct skiplist_head *first;
	struct skiplist_head *last;
	struct skiplist_head *at;
	unsigned int level;

	if (skiplist_empty(list))
		return;

	for (level = 0; level < list->level; level++) {
		first = list->next[level];
		if (first == list)
			continue;

		last = list->prev[level];
		if (level >= head->level) {
			head->next[level] = head;
			head->prev[level] = head;
			head->level = level + 1;
		}

		at = head->next[level];
		head->next[level] = first;
		first->prev[level] = head;
		last->next[level] = at;
		at->prev[level] = last;
	}

	skiplist_init_head(list);
}
EXPORT_SYMBOL_GPL(skiplist_splice_init);

// Verify reciprocal links and node heights when transaction debugging is on.
int skiplist_validate(const struct skiplist_head *head) {
#ifdef CONFIG_TRANSACTIONS_DEBUG
	const struct skiplist_head *node = head;
	unsigned int level;

	if (!head->level || head->level > SKIPLIST_MAX_LEVEL)
		return -EINVAL;

	do {
		if (!node->level || node->level > head->level)
			return -EINVAL;

		for (level = 0; level < node->level; level++) {
			if (!node->next[level] || !node->prev[level])
				return -EINVAL;
			if (node->next[level]->level <= level ||
			    node->prev[level]->level <= level)
				return -EINVAL;
			if (node->next[level]->prev[level] != node)
				return -EINVAL;
			if (node->prev[level]->next[level] != node)
				return -EINVAL;
		}
		node = node->next[0];
	} while (node != head);
#endif

	return 0;
}
EXPORT_SYMBOL_GPL(skiplist_validate);
