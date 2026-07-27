// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/skiplist.h>

struct skiplist_test_node {
	unsigned long address;
	struct skiplist_head list;
};

static int skiplist_test_compare(const struct skiplist_head *left,
                                 const struct skiplist_head *right) {
	const struct skiplist_test_node *left_node;
	const struct skiplist_test_node *right_node;

	left_node = skiplist_entry(
		left, struct skiplist_test_node, list);
	right_node = skiplist_entry(
		right, struct skiplist_test_node, list);
	if (left_node->address < right_node->address)
		return -1;
	if (left_node->address > right_node->address)
		return 1;

	return 0;
}

static void skiplist_test_node_init(struct skiplist_test_node *node, unsigned long address) {
	node->address = address;
	skiplist_init_node(&node->list);
}

static void skiplist_initialization_test(struct kunit *test) {
	struct skiplist_test_node node;
	struct skiplist_head head;
	unsigned int level;

	skiplist_init_head(&head);
	KUNIT_EXPECT_EQ(test, head.level, 1U);
	KUNIT_EXPECT_TRUE(test, skiplist_empty(&head));
	KUNIT_EXPECT_PTR_EQ(test, skiplist_first(&head), NULL);
	for (level = 0; level < SKIPLIST_MAX_LEVEL; level++) {
		KUNIT_EXPECT_PTR_EQ(test, head.next[level], &head);
		KUNIT_EXPECT_PTR_EQ(test, head.prev[level], &head);
	}

	skiplist_test_node_init(&node, 1);
	KUNIT_EXPECT_GE(test, node.list.level, 1U);
	KUNIT_EXPECT_LE(test, node.list.level,
			SKIPLIST_MAX_LEVEL);
	KUNIT_EXPECT_FALSE(test, skiplist_linked(&node.list));
	for (level = 0; level < SKIPLIST_MAX_LEVEL; level++) {
		KUNIT_EXPECT_PTR_EQ(test, node.list.next[level], &node.list);
		KUNIT_EXPECT_PTR_EQ(test, node.list.prev[level], &node.list);
	}
	KUNIT_EXPECT_EQ(test, skiplist_validate(&head), 0);
}

static void skiplist_single_node_test(struct kunit *test) {
	struct skiplist_test_node node;
	struct skiplist_head head;

	skiplist_init_head(&head);
	skiplist_test_node_init(&node, 1);
	KUNIT_ASSERT_EQ(test,
		skiplist_insert(&node.list, &head,
					skiplist_test_compare),
		0);
	KUNIT_EXPECT_FALSE(test, skiplist_empty(&head));
	KUNIT_EXPECT_TRUE(test, skiplist_linked(&node.list));
	KUNIT_EXPECT_PTR_EQ(test, skiplist_first(&head), &node.list);
	KUNIT_EXPECT_PTR_EQ(test, skiplist_last(&head), &node.list);
	KUNIT_EXPECT_PTR_EQ(test, skiplist_next(&head, &node.list), NULL);
	KUNIT_EXPECT_PTR_EQ(test, skiplist_prev(&head, &node.list), NULL);
	KUNIT_EXPECT_EQ(test, skiplist_validate(&head), 0);

	skiplist_del(&node.list, &head);
	KUNIT_EXPECT_TRUE(test, skiplist_empty(&head));
	KUNIT_EXPECT_FALSE(test, skiplist_linked(&node.list));
	KUNIT_EXPECT_EQ(test, head.level, 1U);
	KUNIT_EXPECT_EQ(test, skiplist_validate(&head), 0);
}

static void skiplist_expect_order(struct kunit *test,
                                  const unsigned long *input,
                                  const unsigned long *expected,
                                  size_t count) {
	struct skiplist_test_node *nodes;
	struct skiplist_test_node *node;
	struct skiplist_head head;
	size_t index = 0;
	size_t i;

	nodes = kunit_kcalloc(test, count, sizeof(*nodes), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, nodes);
	skiplist_init_head(&head);
	for (i = 0; i < count; i++) {
		skiplist_test_node_init(&nodes[i], input[i]);
		KUNIT_ASSERT_EQ(test,
			skiplist_insert(
				&nodes[i].list, &head,
				skiplist_test_compare),
			0);
	}

	skiplist_for_each_entry(node, &head, list) {
		KUNIT_ASSERT_LT(test, index, count);
		KUNIT_EXPECT_EQ(test, node->address, expected[index]);
		index++;
	}
	KUNIT_EXPECT_EQ(test, index, count);

	index = count;
	skiplist_for_each_entry_reverse(node, &head, list) {
		KUNIT_ASSERT_GT(test, index, (size_t)0);
		index--;
		KUNIT_EXPECT_EQ(test, node->address, expected[index]);
	}
	KUNIT_EXPECT_EQ(test, index, 0);
	KUNIT_EXPECT_EQ(test, skiplist_validate(&head), 0);

	for (i = 0; i < count; i++)
		skiplist_del(&nodes[i].list, &head);
	KUNIT_EXPECT_TRUE(test, skiplist_empty(&head));
}

static void skiplist_ascending_test(struct kunit *test) {
	static const unsigned long input[] = { 1, 2, 3, 4, 5, 6 };

	skiplist_expect_order(test, input, input, ARRAY_SIZE(input));
}

static void skiplist_descending_test(struct kunit *test) {
	static const unsigned long input[] = { 6, 5, 4, 3, 2, 1 };
	static const unsigned long expected[] = { 1, 2, 3, 4, 5, 6 };

	skiplist_expect_order(test, input, expected,
					  ARRAY_SIZE(input));
}

static void skiplist_random_order_test(struct kunit *test) {
	static const unsigned long input[] = {
		0x830, 0x20, 0x4a0, 0x110, 0x900, 0x70,
	};
	static const unsigned long expected[] = {
		0x20, 0x70, 0x110, 0x4a0, 0x830, 0x900,
	};

	skiplist_expect_order(test, input, expected,
					  ARRAY_SIZE(input));
}

static void skiplist_duplicate_test(struct kunit *test) {
	struct skiplist_test_node first;
	struct skiplist_test_node duplicate;
	struct skiplist_head head;

	skiplist_init_head(&head);
	skiplist_test_node_init(&first, 42);
	skiplist_test_node_init(&duplicate, 42);
	KUNIT_ASSERT_EQ(test,
		skiplist_insert(&first.list, &head,
					skiplist_test_compare),
		0);
	KUNIT_EXPECT_EQ(test,
		skiplist_insert(&duplicate.list, &head,
					skiplist_test_compare),
		-EEXIST);
	KUNIT_EXPECT_FALSE(test,
			  skiplist_linked(&duplicate.list));
	skiplist_del(&first.list, &head);
}

static void skiplist_splice_test(struct kunit *test) {
	struct skiplist_test_node *nodes;
	struct skiplist_test_node *node;
	struct skiplist_head destination;
	struct skiplist_head source;
	static const unsigned long expected[] = { 1, 2, 3 };
	size_t index = 0;

	nodes = kunit_kcalloc(test, ARRAY_SIZE(expected), sizeof(*nodes),
			      GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, nodes);
	skiplist_init_head(&source);
	skiplist_init_head(&destination);
	skiplist_test_node_init(&nodes[0], 1);
	skiplist_test_node_init(&nodes[1], 2);
	skiplist_test_node_init(&nodes[2], 3);
	KUNIT_ASSERT_EQ(test,
		skiplist_insert(&nodes[0].list, &source,
					skiplist_test_compare),
		0);
	KUNIT_ASSERT_EQ(test,
		skiplist_insert(&nodes[1].list, &source,
					skiplist_test_compare),
		0);
	KUNIT_ASSERT_EQ(test,
		skiplist_insert(&nodes[2].list, &destination,
					skiplist_test_compare),
		0);

	skiplist_splice_init(&source, &destination);
	KUNIT_EXPECT_TRUE(test, skiplist_empty(&source));
	skiplist_for_each_entry(node, &destination, list) {
		KUNIT_ASSERT_LT(test, index, ARRAY_SIZE(expected));
		KUNIT_EXPECT_EQ(test, node->address, expected[index]);
		index++;
	}
	KUNIT_EXPECT_EQ(test, index, ARRAY_SIZE(expected));
	KUNIT_EXPECT_EQ(test, skiplist_validate(&destination), 0);
	skiplist_del(&nodes[0].list, &destination);
	skiplist_del(&nodes[1].list, &destination);
	skiplist_del(&nodes[2].list, &destination);
}

static void skiplist_validation_test(struct kunit *test) {
	struct skiplist_test_node node;
	struct skiplist_head head;

	if (!IS_ENABLED(CONFIG_TRANSACTIONS_DEBUG)) {
		kunit_skip(test, "CONFIG_TRANSACTIONS_DEBUG is disabled");
		return;
	}

	skiplist_init_head(&head);
	skiplist_test_node_init(&node, 1);
	KUNIT_ASSERT_EQ(test,
		skiplist_insert(&node.list, &head,
					skiplist_test_compare),
		0);

	head.prev[0] = &head;
	KUNIT_EXPECT_EQ(test, skiplist_validate(&head), -EINVAL);
	head.prev[0] = &node.list;
	node.list.prev[0] = &node.list;
	KUNIT_EXPECT_EQ(test, skiplist_validate(&head), -EINVAL);
	node.list.prev[0] = &head;
	KUNIT_EXPECT_EQ(test, skiplist_validate(&head), 0);
	skiplist_del(&node.list, &head);
}

static struct kunit_case skiplist_test_cases[] = {
	KUNIT_CASE(skiplist_initialization_test),
	KUNIT_CASE(skiplist_single_node_test),
	KUNIT_CASE(skiplist_ascending_test),
	KUNIT_CASE(skiplist_descending_test),
	KUNIT_CASE(skiplist_random_order_test),
	KUNIT_CASE(skiplist_duplicate_test),
	KUNIT_CASE(skiplist_splice_test),
	KUNIT_CASE(skiplist_validation_test),
	{}
};

static struct kunit_suite skiplist_test_suite = {
	.name = "skiplist",
	.test_cases = skiplist_test_cases,
};

kunit_test_suite(skiplist_test_suite);

MODULE_DESCRIPTION("Skiplist tests");
MODULE_LICENSE("GPL");
