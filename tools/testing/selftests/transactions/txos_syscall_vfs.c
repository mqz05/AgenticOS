#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
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
	unlink("/tmp/txos-truncate-abort");
	unlink("/tmp/txos-truncate-commit");
	unlink("/tmp/txos-ftruncate-abort");
	unlink("/tmp/txos-ftruncate-commit");
	unlink("/tmp/txos-create-abort");
	unlink("/tmp/txos-unlink-abort");
	unlink("/tmp/txos-rename-old");
	unlink("/tmp/txos-rename-new");
	unlink("/tmp/txos-rename-commit-old");
	unlink("/tmp/txos-rename-commit-new");
	unlink("/tmp/txos-link-old");
	unlink("/tmp/txos-link-new");
	unlink("/tmp/txos-link-commit-old");
	unlink("/tmp/txos-link-commit-new");
	rmdir("/tmp/txos-mkdir-abort");
	rmdir("/tmp/txos-mkdir-commit");
	rmdir("/tmp/txos-rmdir-abort");
	rmdir("/tmp/txos-rmdir-commit");
}

static int create_empty_file(const char *path, mode_t mode)
{
	int fd;

	fd = open(path, O_CREAT | O_RDWR | O_TRUNC, mode);
	if (fd < 0)
		return -1;
	return close(fd);
}

static int create_file_with_data(const char *path, const char *data)
{
	ssize_t len = 0;
	ssize_t written;
	int fd;

	while (data[len])
		len++;

	fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
	if (fd < 0)
		return -1;

	written = write(fd, data, len);
	if (close(fd) != 0)
		return -1;

	return written == len ? 0 : -1;
}

static int file_size_is(const char *path, off_t size)
{
	struct stat st;

	return stat(path, &st) == 0 && st.st_size == size;
}

static int file_content_is(const char *path, const char *expected)
{
	char buf[64];
	struct stat st;
	size_t expected_len = strlen(expected);
	ssize_t len;
	int close_ret;
	int fd;
	int i;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		ksft_print_msg("%s: open failed errno=%d (%s)\n",
			       path, errno, strerror(errno));
		return 0;
	}

	len = read(fd, buf, sizeof(buf));
	close_ret = close(fd);
	if (close_ret != 0 || len < 0) {
		ksft_print_msg("%s: read len=%zd close=%d errno=%d (%s)\n",
			       path, len, close_ret, errno, strerror(errno));
		return 0;
	}

	if ((size_t)len == expected_len && memcmp(buf, expected, len) == 0)
		return 1;

	ksft_print_msg("%s: content mismatch read_len=%zd expected_len=%zu\n",
		       path, len, expected_len);
	ksft_print_msg("%s: actual bytes='%.*s' expected='%s'\n",
		       path, (int)len, buf, expected);
	ksft_print_msg("%s: actual hex=", path);
	for (i = 0; i < len; i++)
		printf("%02x", (unsigned char)buf[i]);
	printf(" expected hex=");
	for (i = 0; expected[i]; i++)
		printf("%02x", (unsigned char)expected[i]);
	printf("\n");
	if (stat(path, &st) == 0)
		ksft_print_msg("%s: stat size=%lld mode=%o nlink=%lu blocks=%lld\n",
			       path, (long long)st.st_size, st.st_mode & 0777,
			       (unsigned long)st.st_nlink,
			       (long long)st.st_blocks);
	else
		ksft_print_msg("%s: stat failed errno=%d (%s)\n",
			       path, errno, strerror(errno));
	return 0;
}

int main(void)
{
	const char *path = "/tmp/txos-selftest-file";
	struct stat st;
	int fd;

	ksft_print_header();
	ksft_set_plan(24);
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

	if (create_file_with_data("/tmp/txos-truncate-abort", "abcdef") != 0) {
		ksft_test_result_fail("truncate size rollback through xabort\n");
		goto out_unlink;
	}
	if (xbegin() != 0) {
		ksft_test_result_fail("truncate size rollback through xabort\n");
		goto out_unlink;
	}
	if (truncate("/tmp/txos-truncate-abort", 2) != 0) {
		xabort();
		ksft_test_result_fail("truncate size rollback through xabort\n");
		goto out_unlink;
	}
	if (xabort() != 0) {
		ksft_test_result_fail("truncate size rollback through xabort\n");
		goto out_unlink;
	}
	ksft_test_result(file_size_is("/tmp/txos-truncate-abort", 6),
			 "truncate size rollback through xabort\n");
	ksft_test_result(file_content_is("/tmp/txos-truncate-abort", "abcdef"),
			 "truncate page-cache rollback through xabort\n");

	if (create_file_with_data("/tmp/txos-truncate-commit", "abcdef") != 0) {
		ksft_test_result_fail("truncate size publish through xend\n");
		goto out_unlink;
	}
	if (xbegin() != 0) {
		ksft_test_result_fail("truncate size publish through xend\n");
		goto out_unlink;
	}
	if (truncate("/tmp/txos-truncate-commit", 2) != 0) {
		xabort();
		ksft_test_result_fail("truncate size publish through xend\n");
		goto out_unlink;
	}
	if (xend() != 0) {
		ksft_test_result_fail("truncate size publish through xend\n");
		goto out_unlink;
	}
	ksft_test_result(file_size_is("/tmp/txos-truncate-commit", 2),
			 "truncate size publish through xend\n");

	if (create_file_with_data("/tmp/txos-ftruncate-abort", "abcdef") != 0) {
		ksft_test_result_fail("ftruncate size rollback through xabort\n");
		goto out_unlink;
	}
	fd = open("/tmp/txos-ftruncate-abort", O_RDWR);
	if (fd < 0) {
		ksft_test_result_fail("ftruncate size rollback through xabort\n");
		goto out_unlink;
	}
	if (xbegin() != 0) {
		close(fd);
		ksft_test_result_fail("ftruncate size rollback through xabort\n");
		goto out_unlink;
	}
	if (ftruncate(fd, 2) != 0) {
		xabort();
		close(fd);
		ksft_test_result_fail("ftruncate size rollback through xabort\n");
		goto out_unlink;
	}
	if (xabort() != 0) {
		close(fd);
		ksft_test_result_fail("ftruncate size rollback through xabort\n");
		goto out_unlink;
	}
	close(fd);
	ksft_test_result(file_size_is("/tmp/txos-ftruncate-abort", 6),
			 "ftruncate size rollback through xabort\n");
	ksft_test_result(file_content_is("/tmp/txos-ftruncate-abort", "abcdef"),
			 "ftruncate page-cache rollback through xabort\n");

	if (create_file_with_data("/tmp/txos-ftruncate-commit", "abcdef") != 0) {
		ksft_test_result_fail("ftruncate size publish through xend\n");
		goto out_unlink;
	}
	fd = open("/tmp/txos-ftruncate-commit", O_RDWR);
	if (fd < 0) {
		ksft_test_result_fail("ftruncate size publish through xend\n");
		goto out_unlink;
	}
	if (xbegin() != 0) {
		close(fd);
		ksft_test_result_fail("ftruncate size publish through xend\n");
		goto out_unlink;
	}
	if (ftruncate(fd, 2) != 0) {
		xabort();
		close(fd);
		ksft_test_result_fail("ftruncate size publish through xend\n");
		goto out_unlink;
	}
	if (xend() != 0) {
		close(fd);
		ksft_test_result_fail("ftruncate size publish through xend\n");
		goto out_unlink;
	}
	close(fd);
	ksft_test_result(file_size_is("/tmp/txos-ftruncate-commit", 2),
			 "ftruncate size publish through xend\n");

	if (xbegin() != 0) {
		ksft_print_msg("create abort: xbegin failed errno=%d (%s)\n",
			       errno, strerror(errno));
		ksft_test_result_fail("create rollback through xabort\n");
		goto out_unlink;
	}
	if (create_empty_file("/tmp/txos-create-abort", 0600) != 0) {
		int create_errno = errno;

		xabort();
		ksft_print_msg("create abort: open(O_CREAT) failed errno=%d (%s)\n",
			       create_errno, strerror(create_errno));
		ksft_test_result_fail("create rollback through xabort\n");
		goto out_unlink;
	}
	if (xabort() != 0) {
		ksft_print_msg("create abort: xabort failed errno=%d (%s)\n",
			       errno, strerror(errno));
		ksft_test_result_fail("create rollback through xabort\n");
		goto out_unlink;
	}
	{
		struct stat create_st;
		int access_errno;
		int access_ret;
		int create_ok;
		int stat_errno;
		int stat_ret;

		errno = 0;
		access_ret = access("/tmp/txos-create-abort", F_OK);
		access_errno = errno;
		errno = 0;
		stat_ret = stat("/tmp/txos-create-abort", &create_st);
		stat_errno = errno;
		create_ok = access_ret == -1 && access_errno == ENOENT;
		if (!create_ok) {
			ksft_print_msg("create abort: access_ret=%d errno=%d (%s) stat_ret=%d errno=%d (%s)\n",
				       access_ret, access_errno,
				       strerror(access_errno), stat_ret,
				       stat_errno, strerror(stat_errno));
			if (stat_ret == 0)
				ksft_print_msg("create abort: mode=%o size=%lld nlink=%lu blocks=%lld\n",
					       create_st.st_mode & 0777,
					       (long long)create_st.st_size,
					       (unsigned long)create_st.st_nlink,
					       (long long)create_st.st_blocks);
		}
		ksft_test_result(create_ok,
				 "create rollback through xabort\n");
	}

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

	create_empty_file("/tmp/txos-link-old", 0600);
	if (xbegin() != 0) {
		ksft_test_result_fail("link rollback through xabort\n");
		goto out_unlink;
	}
	if (link("/tmp/txos-link-old", "/tmp/txos-link-new") != 0) {
		xabort();
		ksft_test_result_fail("link rollback through xabort\n");
		goto out_unlink;
	}
	if (xabort() != 0) {
		ksft_test_result_fail("link rollback through xabort\n");
		goto out_unlink;
	}
	ksft_test_result(access("/tmp/txos-link-old", F_OK) == 0 &&
			 access("/tmp/txos-link-new", F_OK) == -1 &&
			 errno == ENOENT,
			 "link rollback through xabort\n");

	create_empty_file("/tmp/txos-link-commit-old", 0600);
	if (xbegin() != 0) {
		ksft_test_result_fail("link publish through xend\n");
		goto out_unlink;
	}
	if (link("/tmp/txos-link-commit-old",
		 "/tmp/txos-link-commit-new") != 0) {
		xabort();
		ksft_test_result_fail("link publish through xend\n");
		goto out_unlink;
	}
	if (xend() != 0) {
		ksft_test_result_fail("link publish through xend\n");
		goto out_unlink;
	}
	ksft_test_result(access("/tmp/txos-link-commit-old", F_OK) == 0 &&
			 access("/tmp/txos-link-commit-new", F_OK) == 0,
			 "link publish through xend\n");

	if (xbegin() != 0) {
		ksft_test_result_fail("mkdir rollback through xabort\n");
		goto out_unlink;
	}
	if (mkdir("/tmp/txos-mkdir-abort", 0700) != 0) {
		xabort();
		ksft_test_result_fail("mkdir rollback through xabort\n");
		goto out_unlink;
	}
	if (xabort() != 0) {
		ksft_test_result_fail("mkdir rollback through xabort\n");
		goto out_unlink;
	}
	ksft_test_result(access("/tmp/txos-mkdir-abort", F_OK) == -1 &&
			 errno == ENOENT,
			 "mkdir rollback through xabort\n");

	if (xbegin() != 0) {
		ksft_test_result_fail("mkdir publish through xend\n");
		goto out_unlink;
	}
	if (mkdir("/tmp/txos-mkdir-commit", 0700) != 0) {
		xabort();
		ksft_test_result_fail("mkdir publish through xend\n");
		goto out_unlink;
	}
	if (xend() != 0) {
		ksft_test_result_fail("mkdir publish through xend\n");
		goto out_unlink;
	}
	ksft_test_result(access("/tmp/txos-mkdir-commit", F_OK) == 0,
			 "mkdir publish through xend\n");

	mkdir("/tmp/txos-rmdir-abort", 0700);
	if (xbegin() != 0) {
		ksft_test_result_fail("rmdir rollback through xabort\n");
		goto out_unlink;
	}
	if (rmdir("/tmp/txos-rmdir-abort") != 0) {
		xabort();
		ksft_test_result_fail("rmdir rollback through xabort\n");
		goto out_unlink;
	}
	if (xabort() != 0) {
		ksft_test_result_fail("rmdir rollback through xabort\n");
		goto out_unlink;
	}
	ksft_test_result(access("/tmp/txos-rmdir-abort", F_OK) == 0,
			 "rmdir rollback through xabort\n");

	mkdir("/tmp/txos-rmdir-commit", 0700);
	if (xbegin() != 0) {
		ksft_test_result_fail("rmdir publish through xend\n");
		goto out_unlink;
	}
	if (rmdir("/tmp/txos-rmdir-commit") != 0) {
		xabort();
		ksft_test_result_fail("rmdir publish through xend\n");
		goto out_unlink;
	}
	if (xend() != 0) {
		ksft_test_result_fail("rmdir publish through xend\n");
		goto out_unlink;
	}
	ksft_test_result(access("/tmp/txos-rmdir-commit", F_OK) == -1 &&
			 errno == ENOENT,
			 "rmdir publish through xend\n");

out_unlink:
	unlink(path);
	cleanup_namespace_fixtures();
out:
	ksft_finished();
}
