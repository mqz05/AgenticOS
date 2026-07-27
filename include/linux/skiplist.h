/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SKIPLIST_H
#define _LINUX_SKIPLIST_H

#include <linux/container_of.h>
#include <linux/types.h>

#define SKIPLIST_MAX_LEVEL 32

struct skiplist_head {
	unsigned int level;
	struct skiplist_head *prev[SKIPLIST_MAX_LEVEL];
	struct skiplist_head *next[SKIPLIST_MAX_LEVEL];
};

typedef int (*skiplist_cmp_t)(const struct skiplist_head *left, const struct skiplist_head *right);

void skiplist_init_head(struct skiplist_head *head);
void skiplist_init_node(struct skiplist_head *node);
bool skiplist_empty(const struct skiplist_head *head);
bool skiplist_linked(const struct skiplist_head *node);
int skiplist_insert(struct skiplist_head *node, struct skiplist_head *head, skiplist_cmp_t compare);
void skiplist_insert_at(struct skiplist_head *node,
                        struct skiplist_head *head,
                        struct skiplist_head *after);
void skiplist_del(struct skiplist_head *node, struct skiplist_head *head);
void skiplist_splice_init(struct skiplist_head *list, struct skiplist_head *head);
int skiplist_validate(const struct skiplist_head *head);

static inline struct skiplist_head *skiplist_first(const struct skiplist_head *head) {
	if (skiplist_empty(head))
		return NULL;

	return head->next[0];
}

static inline struct skiplist_head *skiplist_next(const struct skiplist_head *head,
                                                  const struct skiplist_head *node) {
	if (node->next[0] == head)
		return NULL;

	return node->next[0];
}

static inline struct skiplist_head *skiplist_last(const struct skiplist_head *head) {
	if (skiplist_empty(head))
		return NULL;

	return head->prev[0];
}

static inline struct skiplist_head *skiplist_prev(const struct skiplist_head *head, const struct skiplist_head *node) {
	if (node->prev[0] == head)
		return NULL;

	return node->prev[0];
}

#define skiplist_entry(ptr, type, member) \
	container_of(ptr, type, member)

#define skiplist_entry_safe(ptr, type, member) ({	\
	type *__entry = NULL;					\
	if (ptr)						\
		__entry = skiplist_entry(ptr, type, member); \
	__entry;						\
})

#define skiplist_for_each_entry(pos, head, member)            \
	for (pos = skiplist_entry_safe(                         \
		     skiplist_first(head), typeof(*pos), member); \
	     pos;                                                            \
	     pos = skiplist_entry_safe(                          \
		     skiplist_next(head, &pos->member),            \
		     typeof(*pos), member))

#define skiplist_for_each_entry_reverse(pos, head, member)	\
	for (pos = skiplist_entry_safe(skiplist_last(head), typeof(*pos), member); \
	     pos; \
	     pos = skiplist_entry_safe(skiplist_prev(head, &pos->member), typeof(*pos), member))

#endif /* _LINUX_SKIPLIST_H */
