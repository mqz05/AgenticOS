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
	unlink("/tmp/txos-symlink-abort");
	unlink("/tmp/txos-symlink-commit");
	unlink("/tmp/txos-replace-old");
	unlink("/tmp/txos-replace-new");
	unlink("/tmp/txos-replace-commit-old");
	unlink("/tmp/txos-replace-commit-new");
	unlink("/tmp/txos-nlink-old");
	unlink("/tmp/txos-nlink-new");
	unlink("/tmp/txos-nlink-commit-old");
	unlink("/tmp/txos-nlink-commit-new");
	unlink("/tmp/txos-failed-op");
	unlink("/tmp/txos-repeated");
	unlink("/tmp/txos-nested/child/file");
	rmdir("/tmp/txos-nested/child");
	rmdir("/tmp/txos-nested");
	rmdir("/tmp/txos-dir-old");
	rmdir("/tmp/txos-dir-new");
	rmdir("/tmp/txos-dir-commit-old");
	rmdir("/tmp/txos-dir-commit-new");
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

static void print_path_stat(const char *label, const char *path)
{
	struct stat st;

	if (lstat(path, &st) == 0)
		ksft_print_msg("%s: mode=%o size=%lld nlink=%lu ino=%lu\n",
			       label, st.st_mode & 07777, (long long)st.st_size,
			       (unsigned long)st.st_nlink, (unsigned long)st.st_ino);
	else
		ksft_print_msg("%s: lstat errno=%d (%s)\n",
			       label, errno, strerror(errno));
}

static int path_absent(const char *path)
{
	errno = 0;
	return access(path, F_OK) == -1 && errno == ENOENT;
}

static int path_nlink_is(const char *path, nlink_t expected)
{
	struct stat st;

	return stat(path, &st) == 0 && st.st_nlink == expected;
}

static void test_symlink_rollback(void)
{
	char target[32];
	long begin_ret;
	long end_ret = -1;
	ssize_t len;
	int begin_errno;
	int symlink_errno = 0;
	int end_errno = 0;
	int symlink_ret = -1;
	int ok;

	ok = xbegin() == 0 &&
	     symlink("txos-target", "/tmp/txos-symlink-abort") == 0 &&
	     xabort() == 0 && path_absent("/tmp/txos-symlink-abort");
	if (!ok)
		xabort();
	ksft_test_result(ok, "symlink creation rollback through xabort\n");

	errno = 0;
	begin_ret = xbegin();
	begin_errno = errno;
	if (begin_ret == 0) {
		errno = 0;
		symlink_ret = symlink("txos-target", "/tmp/txos-symlink-commit");
		symlink_errno = errno;
		if (symlink_ret == 0) {
			errno = 0;
			end_ret = xend();
			end_errno = errno;
		} else {
			xabort();
		}
	}
	ok = begin_ret == 0 && symlink_ret == 0 && end_ret == 0;
	if (!ok)
		ksft_print_msg("symlink commit stages: xbegin=%ld errno=%d symlink=%d errno=%d xend=%ld errno=%d\n",
			       begin_ret, begin_errno, symlink_ret, symlink_errno,
			       end_ret, end_errno);
	if (ok) {
		errno = 0;
		len = readlink("/tmp/txos-symlink-commit", target,
			       sizeof(target) - 1);
		if (len >= 0)
			target[len] = '\0';
		ok = len == (ssize_t)strlen("txos-target") &&
		     !strcmp(target, "txos-target");
		if (!ok)
			ksft_print_msg("symlink commit: readlink len=%zd errno=%d (%s) target='%s'\n",
				       len, errno, strerror(errno),
				       len >= 0 ? target : "<unavailable>");
	}
	if (!ok)
		xabort();
	ksft_test_result(ok, "symlink creation publish through xend\n");
}

static void test_rename_over_existing(void)
{
	int ok;

	create_file_with_data("/tmp/txos-replace-old", "old");
	create_file_with_data("/tmp/txos-replace-new", "new");
	ok = xbegin() == 0 &&
	     rename("/tmp/txos-replace-old", "/tmp/txos-replace-new") == 0 &&
	     xabort() == 0 &&
	     file_content_is("/tmp/txos-replace-old", "old") &&
	     file_content_is("/tmp/txos-replace-new", "new");
	if (!ok)
		xabort();
	ksft_test_result(ok, "rename-over-existing rollback through xabort\n");

	create_file_with_data("/tmp/txos-replace-commit-old", "old");
	create_file_with_data("/tmp/txos-replace-commit-new", "new");
	ok = xbegin() == 0 &&
	     rename("/tmp/txos-replace-commit-old",
		    "/tmp/txos-replace-commit-new") == 0 &&
	     xend() == 0 && path_absent("/tmp/txos-replace-commit-old") &&
	     file_content_is("/tmp/txos-replace-commit-new", "old");
	if (!ok)
		xabort();
	ksft_test_result(ok, "rename-over-existing publish through xend\n");
}

static void test_directory_rename(void)
{
	int ok;

	mkdir("/tmp/txos-dir-old", 0700);
	ok = xbegin() == 0 &&
	     rename("/tmp/txos-dir-old", "/tmp/txos-dir-new") == 0 &&
	     xabort() == 0 && access("/tmp/txos-dir-old", F_OK) == 0 &&
	     path_absent("/tmp/txos-dir-new");
	if (!ok)
		xabort();
	ksft_test_result(ok, "directory rename rollback through xabort\n");

	mkdir("/tmp/txos-dir-commit-old", 0700);
	ok = xbegin() == 0 &&
	     rename("/tmp/txos-dir-commit-old",
		    "/tmp/txos-dir-commit-new") == 0 &&
	     xend() == 0 && path_absent("/tmp/txos-dir-commit-old") &&
	     access("/tmp/txos-dir-commit-new", F_OK) == 0;
	if (!ok)
		xabort();
	ksft_test_result(ok, "directory rename publish through xend\n");
}

static void test_hardlink_counts(void)
{
	int ok;

	create_empty_file("/tmp/txos-nlink-old", 0600);
	ok = path_nlink_is("/tmp/txos-nlink-old", 1) && xbegin() == 0 &&
	     link("/tmp/txos-nlink-old", "/tmp/txos-nlink-new") == 0 &&
	     xabort() == 0 && path_nlink_is("/tmp/txos-nlink-old", 1) &&
	     path_absent("/tmp/txos-nlink-new");
	if (!ok)
		xabort();
	ksft_test_result(ok, "hard-link count rollback through xabort\n");

	create_empty_file("/tmp/txos-nlink-commit-old", 0600);
	ok = xbegin() == 0 &&
	     link("/tmp/txos-nlink-commit-old",
		  "/tmp/txos-nlink-commit-new") == 0 &&
	     xend() == 0 && path_nlink_is("/tmp/txos-nlink-commit-old", 2) &&
	     path_nlink_is("/tmp/txos-nlink-commit-new", 2);
	if (!ok)
		xabort();
	ksft_test_result(ok, "hard-link count publish through xend\n");
}

static void test_failed_operation_continuation(void)
{
	int ok;

	errno = 0;
	ok = xbegin() == 0 && unlink("/tmp/txos-does-not-exist") == -1 &&
	     errno == ENOENT &&
	     create_empty_file("/tmp/txos-failed-op", 0600) == 0 &&
	     xabort() == 0 && path_absent("/tmp/txos-failed-op");
	if (!ok)
		xabort();
	ksft_test_result(ok, "failed namespace operation leaves transaction usable\n");
}

static void test_repeated_operation_rollback(void)
{
	int ok;

	ok = xbegin() == 0 &&
	     create_empty_file("/tmp/txos-repeated", 0600) == 0 &&
	     unlink("/tmp/txos-repeated") == 0 && xabort() == 0 &&
	     path_absent("/tmp/txos-repeated");
	if (!ok)
		xabort();
	ksft_test_result(ok, "repeated create-unlink rollback through xabort\n");
}

static void test_nested_directory_rollback(void)
{
	long begin_ret;
	long abort_ret = -1;
	int child_errno = 0;
	int child_ret = -1;
	int file_errno = 0;
	int file_ret = -1;
	int parent_errno = 0;
	int parent_ret = -1;
	int ok;

	begin_ret = xbegin();
	if (begin_ret == 0) {
		errno = 0;
		parent_ret = mkdir("/tmp/txos-nested", 0700);
		parent_errno = errno;
	}
	if (parent_ret == 0) {
		errno = 0;
		child_ret = mkdir("/tmp/txos-nested/child", 0700);
		child_errno = errno;
	}
	if (child_ret == 0) {
		errno = 0;
		file_ret = create_empty_file("/tmp/txos-nested/child/file", 0600);
		file_errno = errno;
	}
	if (begin_ret == 0)
		abort_ret = xabort();
	ok = begin_ret == 0 && parent_ret == 0 && child_ret == 0 &&
	     file_ret == 0 && abort_ret == 0 && path_absent("/tmp/txos-nested");
	if (!ok)
		ksft_print_msg("nested stages: xbegin=%ld parent=%d errno=%d child=%d errno=%d file=%d errno=%d xabort=%ld\n",
			       begin_ret, parent_ret, parent_errno, child_ret,
			       child_errno, file_ret, file_errno, abort_ret);
	if (!ok)
		xabort();
	ksft_test_result(ok, "nested namespace creation rollback through xabort\n");
}

int main(void)
{
	const char *path = "/tmp/txos-selftest-file";
	struct stat st;
	int fd;

	ksft_print_header();
	ksft_set_plan(35);
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
	errno = 0;
	if (access("/tmp/txos-unlink-abort", F_OK) != 0) {
		int saved_errno = errno;

		ksft_print_msg("unlink abort: access errno=%d (%s)\n",
			       saved_errno, strerror(saved_errno));
		print_path_stat("unlink abort", "/tmp/txos-unlink-abort");
		ksft_test_result_fail("unlink rollback through xabort\n");
	} else {
		ksft_test_result_pass("unlink rollback through xabort\n");
	}

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
	errno = 0;
	if (access("/tmp/txos-rmdir-abort", F_OK) != 0) {
		int saved_errno = errno;

		ksft_print_msg("rmdir abort: access errno=%d (%s)\n",
			       saved_errno, strerror(saved_errno));
		print_path_stat("rmdir abort", "/tmp/txos-rmdir-abort");
		ksft_test_result_fail("rmdir rollback through xabort\n");
	} else {
		ksft_test_result_pass("rmdir rollback through xabort\n");
	}

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

	test_symlink_rollback();
	test_rename_over_existing();
	test_directory_rename();
	test_hardlink_counts();
	test_failed_operation_continuation();
	test_repeated_operation_rollback();
	test_nested_directory_rollback();

out_unlink:
	unlink(path);
	cleanup_namespace_fixtures();
out:
	ksft_finished();
}
