// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/transaction.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
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

static long xbegin(void)
{
	return syscall(__NR_xbegin, TX_NOUSER_ROLLBACK | TX_NOAUTO_RETRY, NULL);
}

static long xend(void)
{
	return syscall(__NR_xend);
}

static long xabort(void)
{
	return syscall(__NR_xabort);
}

static bool flags_are(int fd, int mask, int expected)
{
	int flags = fcntl(fd, F_GETFL);

	return flags >= 0 && (flags & mask) == expected;
}

struct shared_test_state {
	int phase;
	int before_commit;
	int after_commit;
};

static bool wait_for_phase(struct shared_test_state *state, int phase)
{
	unsigned int spins;

	for (spins = 0; spins < 100000000; spins++) {
		if (__atomic_load_n(&state->phase, __ATOMIC_ACQUIRE) >= phase)
			return true;
	}
	return false;
}

int main(void)
{
	const char *path = "/tmp/txos-file-adapter";
	int mask = O_APPEND | O_NONBLOCK;
	int flags;
	int duplicate;
	int file;
	int on;
	int status;
	pid_t child;
	struct shared_test_state *shared;
	bool tx_active = false;
	bool ok;

	ksft_print_header();
	ksft_set_plan(9);
	unlink(path);
	file = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (file < 0)
		ksft_exit_fail_msg("open failed: %d\n", errno);
	duplicate = dup(file);
	if (duplicate < 0)
		ksft_exit_fail_msg("dup failed: %d\n", errno);

	ksft_test_result(lseek(file, 7, SEEK_SET) == 7 &&
			 lseek(duplicate, 0, SEEK_CUR) == 7,
			 "duplicated descriptors share one file description\n");

	ok = xbegin() == 0 && lseek(file, 19, SEEK_SET) == 19 &&
	     lseek(duplicate, 0, SEEK_CUR) == 19 && xabort() == 0 &&
	     lseek(duplicate, 0, SEEK_CUR) == 7;
	ksft_test_result(ok, "shared file offset rolls back through xabort\n");

	ok = xbegin() == 0 && lseek(duplicate, 23, SEEK_SET) == 23 &&
	     xend() == 0 && lseek(file, 0, SEEK_CUR) == 23;
	ksft_test_result(ok, "shared file offset publishes through xend\n");

	flags = fcntl(file, F_GETFL);
	ok = flags >= 0 && xbegin() == 0 &&
	     fcntl(file, F_SETFL, flags | O_APPEND) == 0 &&
	     flags_are(duplicate, O_APPEND, O_APPEND) && xabort() == 0 &&
	     flags_are(file, O_APPEND, 0);
	ksft_test_result(ok, "shared file flags roll back through xabort\n");

	flags = fcntl(file, F_GETFL);
	ok = flags >= 0 && xbegin() == 0 &&
	     fcntl(duplicate, F_SETFL, flags | O_APPEND) == 0 &&
	     flags_are(file, O_APPEND, O_APPEND) && xend() == 0 &&
	     flags_are(file, O_APPEND, O_APPEND);
	ksft_test_result(ok, "shared file flags publish through xend\n");

	flags = fcntl(file, F_GETFL);
	if (flags >= 0)
		fcntl(file, F_SETFL, flags & ~mask);
	on = 1;
	ok = xbegin() == 0 && ioctl(file, FIONBIO, &on) == 0 &&
	     flags_are(duplicate, O_NONBLOCK, O_NONBLOCK) && xabort() == 0 &&
	     flags_are(file, O_NONBLOCK, 0);
	ksft_test_result(ok, "FIONBIO rolls back through xabort\n");

	on = 1;
	ok = xbegin() == 0 && ioctl(duplicate, FIONBIO, &on) == 0 &&
	     xend() == 0 && flags_are(file, O_NONBLOCK, O_NONBLOCK);
	ksft_test_result(ok, "FIONBIO publishes through xend\n");

	flags = fcntl(file, F_GETFL);
	ok = flags >= 0 && xbegin() == 0 &&
	     fcntl(file, F_SETFL, (flags & ~mask) | O_APPEND | O_NONBLOCK) == 0 &&
	     flags_are(duplicate, mask, O_APPEND | O_NONBLOCK) && xabort() == 0;
	ksft_test_result(ok, "repeated shared flag reads use the transaction shadow\n");

	/*
	 * The child is an ordinary accessor of the inherited open file
	 * description.  It must see committed state, never the parent's shadow.
	 * Anonymous shared memory is only test coordination and is established
	 * before xbegin; MM rollback is not exercised here.
	 */
	shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	ok = shared != MAP_FAILED;
	child = ok ? fork() : -1;
	if (child == 0) {
		if (!wait_for_phase(shared, 1))
			_exit(2);
		shared->before_commit = flags_are(file, O_APPEND, 0);
		__atomic_store_n(&shared->phase, 2, __ATOMIC_RELEASE);
		if (!wait_for_phase(shared, 3))
			_exit(3);
		shared->after_commit = flags_are(file, O_APPEND, O_APPEND);
		__atomic_store_n(&shared->phase, 4, __ATOMIC_RELEASE);
		_exit(0);
	}
	if (child < 0) {
		ok = false;
	} else {
		flags = fcntl(file, F_GETFL);
		if (flags < 0 || fcntl(file, F_SETFL, flags & ~O_APPEND) < 0 ||
		    xbegin() != 0) {
			ok = false;
		} else {
			tx_active = true;
			if (fcntl(file, F_SETFL, flags | O_APPEND) < 0)
				ok = false;
			__atomic_store_n(&shared->phase, 1, __ATOMIC_RELEASE);
			ok = wait_for_phase(shared, 2) && shared->before_commit && ok;
			if (ok) {
				tx_active = false;
				ok = xend() == 0;
			}
			__atomic_store_n(&shared->phase, 3, __ATOMIC_RELEASE);
			ok = wait_for_phase(shared, 4) && shared->after_commit && ok;
		}
		if (tx_active)
			xabort();
		__atomic_store_n(&shared->phase, 3, __ATOMIC_RELEASE);
		if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
		    WEXITSTATUS(status) != 0)
			ok = false;
	}
	ksft_test_result(ok,
			 "ordinary concurrent descriptor sees flags only after commit\n");
	if (shared != MAP_FAILED)
		munmap(shared, sizeof(*shared));

	close(duplicate);
	close(file);
	unlink(path);
	ksft_finished();
}
