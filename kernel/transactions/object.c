// SPDX-License-Identifier: GPL-2.0
/* Common transactional-object infrastructure. */

#include <linux/export.h>
#include <linux/transaction.h>

void transaction_object_init(struct transaction_object *object,
			     enum transaction_object_type type) {
	raw_spin_lock_init(&object->lock);
	object->type = type;
	object->version = 0;
}
EXPORT_SYMBOL_GPL(transaction_object_init);
