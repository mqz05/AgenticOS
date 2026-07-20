// SPDX-License-Identifier: GPL-2.0
// Common transactional-object infrastructure.

#include <linux/export.h>
#include <linux/transaction.h>

void transaction_object_init(struct transaction_object *object, enum transaction_object_type type) {
	object->type = type;
	object->writer = NULL;
	INIT_LIST_HEAD(&object->readers);
	spin_lock_init(&object->lock);
	object->version = 0;
}
EXPORT_SYMBOL_GPL(transaction_object_init);
