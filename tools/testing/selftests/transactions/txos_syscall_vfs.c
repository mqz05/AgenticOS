// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
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

static long xbegin(void)
{
	return syscall(__NR_xbegin);
}

static long xend(void)
{
	return syscall(__NR_xend);
}

static long xabort(void)
{
	return syscall(__NR_xabort);
}

static int expect_syscall_error(long ret, int expected_errno)
{
	return ret == -1 && errno == expected_errno;
}

int main(void)
{
	const char *path = "/tmp/txos-selftest-file";
	struct stat st;
	int fd;

	ksft_print_header();
	ksft_set_plan(8);

	errno = 0;
	ksft_test_result(expect_syscall_error(xend(), EINVAL),
			 "xend without xbegin returns EINVAL\n");

	ksft_test_result(xbegin() == 0, "xbegin starts a transaction\n");

	errno = 0;
	ksft_test_result(expect_syscall_error(xbegin(), EALREADY),
			 "nested xbegin returns EALREADY\n");

	ksft_test_result(xend() == 0, "xend commits a transaction\n");

	ksft_test_result(xbegin() == 0, "xbegin before abort succeeds\n");
	ksft_test_result(xabort() == 0, "xabort aborts a transaction\n");

	errno = 0;
	ksft_test_result(expect_syscall_error(xend(), EINVAL),
			 "xend after abort returns EINVAL\n");

	unlink(path);
	fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
	if (fd < 0) {
		ksft_test_result_fail("chmod rollback through xabort\n");
		goto out;
	}
	close(fd);

	if (xbegin() != 0) {
		ksft_test_result_fail("chmod rollback through xabort\n");
		goto out_unlink;
	}
	if (chmod(path, 0644) != 0) {
		xabort();
		ksft_test_result_fail("chmod rollback through xabort\n");
		goto out_unlink;
	}
	if (xabort() != 0 || stat(path, &st) != 0) {
		ksft_test_result_fail("chmod rollback through xabort\n");
		goto out_unlink;
	}
	ksft_test_result((st.st_mode & 0777) == 0600,
			 "chmod rollback through xabort\n");

out_unlink:
	unlink(path);
out:
	ksft_finished();
}
