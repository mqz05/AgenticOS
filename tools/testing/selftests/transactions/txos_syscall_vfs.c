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

static void cleanup_namespace_fixtures(void)
{
	unlink("/tmp/txos-create-abort");
	unlink("/tmp/txos-unlink-abort");
	unlink("/tmp/txos-rename-old");
	unlink("/tmp/txos-rename-new");
	unlink("/tmp/txos-rename-commit-old");
	unlink("/tmp/txos-rename-commit-new");
}

static int create_empty_file(const char *path, mode_t mode)
{
	int fd;

	fd = open(path, O_CREAT | O_RDWR | O_TRUNC, mode);
	if (fd < 0)
		return -1;
	return close(fd);
}

int main(void)
{
	const char *path = "/tmp/txos-selftest-file";
	struct stat st;
	int fd;

	ksft_print_header();
	ksft_set_plan(12);
	cleanup_namespace_fixtures();

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

	if (xbegin() != 0) {
		ksft_test_result_fail("create rollback through xabort\n");
		goto out_unlink;
	}
	if (create_empty_file("/tmp/txos-create-abort", 0600) != 0) {
		xabort();
		ksft_test_result_fail("create rollback through xabort\n");
		goto out_unlink;
	}
	if (xabort() != 0) {
		ksft_test_result_fail("create rollback through xabort\n");
		goto out_unlink;
	}
	ksft_test_result(access("/tmp/txos-create-abort", F_OK) == -1 &&
			 errno == ENOENT,
			 "create rollback through xabort\n");

	create_empty_file("/tmp/txos-unlink-abort", 0600);
	if (xbegin() != 0) {
		ksft_test_result_fail("unlink rollback through xabort\n");
		goto out_unlink;
	}
	if (unlink("/tmp/txos-unlink-abort") != 0) {
		xabort();
		ksft_test_result_fail("unlink rollback through xabort\n");
		goto out_unlink;
	}
	if (xabort() != 0) {
		ksft_test_result_fail("unlink rollback through xabort\n");
		goto out_unlink;
	}
	ksft_test_result(access("/tmp/txos-unlink-abort", F_OK) == 0,
			 "unlink rollback through xabort\n");

	create_empty_file("/tmp/txos-rename-old", 0600);
	if (xbegin() != 0) {
		ksft_test_result_fail("rename rollback through xabort\n");
		goto out_unlink;
	}
	if (rename("/tmp/txos-rename-old", "/tmp/txos-rename-new") != 0) {
		xabort();
		ksft_test_result_fail("rename rollback through xabort\n");
		goto out_unlink;
	}
	if (xabort() != 0) {
		ksft_test_result_fail("rename rollback through xabort\n");
		goto out_unlink;
	}
	ksft_test_result(access("/tmp/txos-rename-old", F_OK) == 0 &&
			 access("/tmp/txos-rename-new", F_OK) == -1 &&
			 errno == ENOENT,
			 "rename rollback through xabort\n");

	create_empty_file("/tmp/txos-rename-commit-old", 0600);
	if (xbegin() != 0) {
		ksft_test_result_fail("rename publish through xend\n");
		goto out_unlink;
	}
	if (rename("/tmp/txos-rename-commit-old",
		   "/tmp/txos-rename-commit-new") != 0) {
		xabort();
		ksft_test_result_fail("rename publish through xend\n");
		goto out_unlink;
	}
	if (xend() != 0) {
		ksft_test_result_fail("rename publish through xend\n");
		goto out_unlink;
	}
	ksft_test_result(access("/tmp/txos-rename-commit-old", F_OK) == -1 &&
			 errno == ENOENT &&
			 access("/tmp/txos-rename-commit-new", F_OK) == 0,
			 "rename publish through xend\n");

out_unlink:
	unlink(path);
	cleanup_namespace_fixtures();
out:
	ksft_finished();
}
