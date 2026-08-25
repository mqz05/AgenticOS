// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <linux/transaction.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef __NR_xbegin
#define __NR_xbegin 470
#endif
#ifndef __NR_xend
#define __NR_xend 471
#endif
#ifndef __NR_xabort
#define __NR_xabort 472
#endif

static __attribute__((always_inline)) inline long syscall_result(long ret)
{
	if ((unsigned long)ret >= (unsigned long)-4095) {
		errno = -ret;
		return -1;
	}
	return ret;
}

/*
 * TxOS execution restoration targets the instruction following the original
 * xbegin syscall.  Keep these syscall instructions inline so each call site
 * has its own saved IP; libc's generic syscall() wrapper deliberately shares
 * one instruction and requires userspace stack rollback to distinguish calls.
 */
static __attribute__((always_inline)) inline long xbegin(unsigned int flags,
							  int *status)
{
	register long ax __asm__("rax") = __NR_xbegin;
	register long di __asm__("rdi") = flags;
	register long si __asm__("rsi") = (long)status;

	__asm__ volatile("syscall"
			 : "+a"(ax)
			 : "D"(di), "S"(si)
			 : "rcx", "r11", "memory");
	return syscall_result(ax);
}

static __attribute__((always_inline)) inline long xend(void)
{
	register long ax __asm__("rax") = __NR_xend;

	__asm__ volatile("syscall"
			 : "+a"(ax)
			 :
			 : "rcx", "r11", "memory");
	return syscall_result(ax);
}

static __attribute__((always_inline)) inline long xabort(void)
{
	register long ax __asm__("rax") = __NR_xabort;

	__asm__ volatile("syscall"
			 : "+a"(ax)
			 :
			 : "rcx", "r11", "memory");
	return syscall_result(ax);
}

int main(void)
{
	static volatile unsigned int attempts;
	int status = -1;
	long ret;

	ksft_print_header();
	ksft_set_plan(9);

	errno = 0;
	ret = xbegin(1U << 31, NULL);
	ksft_test_result(ret == -1 && errno == EINVAL,
			 "xbegin rejects unknown flags\n");

	errno = 0;
	ret = xbegin(TX_ERROR_UNSUPPORTED | TX_LIVE_DANGEROUSLY, NULL);
	ksft_test_result(ret == -1 && errno == EINVAL,
			 "xbegin rejects conflicting unsupported-syscall policies\n");

	status = -1;
	ret = xbegin(TX_NOUSER_ROLLBACK | TX_NOAUTO_RETRY, &status);
	ksft_test_result(ret == 0 && status == TX_STATUS_ACTIVE,
			 "xbegin publishes active status\n");
	ret = xend();
	ksft_test_result(ret == 0 && status == TX_STATUS_INACTIVE,
			 "xend publishes inactive status\n");

	status = -1;
	ret = xbegin(TX_NOUSER_ROLLBACK | TX_NOAUTO_RETRY, &status);
	if (ret == 0)
		ret = xabort();
	ksft_test_result(ret == 0 && status == TX_STATUS_ABORTED,
			 "straight-line xabort publishes aborted status\n");

	status = -1;
	errno = 0;
	ret = xbegin(TX_NOAUTO_RETRY, &status);
	if (ret == 0) {
		xabort();
		ksft_test_result_fail("explicit abort restores xbegin with ECANCELED\n");
	} else {
		ksft_test_result(ret == -1 && errno == ECANCELED &&
				 status == TX_STATUS_ABORTED,
				 "explicit abort restores xbegin with ECANCELED\n");
	}

	status = -1;
	attempts = 0;
	ret = xbegin(TX_DEFAULTS, &status);
	if (ret == 0) {
		attempts++;
		xabort();
		ksft_test_result_fail("automatic retry restores xbegin execution\n");
	} else {
		ksft_test_result(ret == 1 && attempts == 1 &&
				 status == TX_STATUS_ACTIVE,
				 "automatic retry restores xbegin execution\n");
	}
	ksft_test_result(xend() == 0 && status == TX_STATUS_INACTIVE,
			 "retried transaction commits normally\n");

	status = -1;
	ret = xbegin(TX_NONDURABLE | TX_NOUSER_ROLLBACK |
		     TX_NOAUTO_RETRY | TX_ERROR_UNSUPPORTED, &status);
	ksft_test_result(ret == 0 && status == TX_STATUS_ACTIVE && xend() == 0,
			 "supported TxOS policy flags are accepted\n");

	ksft_finished();
}
