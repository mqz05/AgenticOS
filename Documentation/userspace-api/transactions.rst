.. SPDX-License-Identifier: GPL-2.0

===========================
TxOS system-call interface
===========================

The system-transaction interface consists of ``xbegin(flags, xsw)``,
``xend()``, and ``xabort()``.  The flag and status definitions are exported by
``<linux/transaction.h>``.

``xbegin`` starts a transaction for the calling task.  It returns zero for the
first attempt and a positive retry count after an automatic retry.  ``xsw``
may be NULL; otherwise it points to an integer status word updated by the
kernel.  Unknown flags and contradictory unsupported-operation policies are
rejected with ``EINVAL``.  Nested transactions are not supported.

``xend`` commits the transaction.  A commit-time validation or contention
failure rolls kernel state back.  With automatic retry enabled, the kernel
starts a fresh attempt and restores execution to the return from ``xbegin``.
With ``TX_NOAUTO_RETRY``, it restores that point with ``ECANCELED`` instead.

``xabort`` explicitly rolls kernel state back.  It follows the same retry
policy.  ``TX_NOUSER_ROLLBACK`` selects the straight-line mode used when the
caller does not want execution restored; in that mode ``xabort`` returns zero
normally.

The port currently restores the syscall-entry general-register frame,
including the user instruction pointer, stack pointer, and processor flags.
Until userspace stack rollback is added, callers that enable execution
restoration must issue ``xbegin`` from a dedicated inline syscall site rather
than libc's shared generic ``syscall()`` wrapper.
The later MM rollback milestone must add address-space and extended floating-
point state checkpointing before automatic retry provides complete rollback
of arbitrary userspace computation.  Callers must therefore treat automatic
retry as experimental and avoid relying on rollback of userspace memory or
floating-point/vector state at this checkpoint.

``TX_NONDURABLE`` and the unsupported-operation policy flags are accepted and
recorded in the transaction.  Filesystem durability integration and complete
unsupported-syscall enforcement are separate milestones.  A transaction is
currently restricted to one task; fork and shared multi-task transactions are
also handled by a later milestone.
