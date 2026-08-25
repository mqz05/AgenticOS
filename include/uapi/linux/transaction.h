/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_TRANSACTION_H
#define _UAPI_LINUX_TRANSACTION_H

/* xbegin() policy flags, retaining the original TxOS bit assignments. */
#define TX_DEFAULTS			0U
#define TX_NONDURABLE			(1U << 0)
#define TX_NOUSER_ROLLBACK		(1U << 1)
#define TX_NOAUTO_RETRY			(1U << 2)
#define TX_ERROR_UNSUPPORTED		(1U << 3)
#define TX_LIVE_DANGEROUSLY		(1U << 4)
#define TX_VALID_FLAGS			(TX_NONDURABLE | TX_NOUSER_ROLLBACK | \
					 TX_NOAUTO_RETRY | TX_ERROR_UNSUPPORTED | \
					 TX_LIVE_DANGEROUSLY)

/* Values published through the optional xbegin() status word. */
#define TX_STATUS_INACTIVE	0
#define TX_STATUS_ACTIVE	1
#define TX_STATUS_ABORTED	2
#define TX_STATUS_COMMITTING	3
#define TX_STATUS_ABORTING	4

#endif /* _UAPI_LINUX_TRANSACTION_H */
