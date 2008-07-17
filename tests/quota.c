/*
 * libhugetlbfs - Easy use of Linux hugepages
 * Copyright (C) 2005-2007 David Gibson & Adam Litke, IBM Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include <sys/types.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <hugetlbfs.h>
#include <sys/vfs.h>
#include "hugetests.h"

/*
 * Test Rationale:
 *
 * The number of global huge pages available to a mounted hugetlbfs filesystem
 * can be limited using a fs quota mechanism by setting the size attribute at
 * mount time.  Older kernels did not properly handle quota accounting in a
 * number of cases (eg. for MAP_PRIVATE pages, and wrt MAP_SHARED reservation.
 *
 * This test replays some scenarios on a privately mounted filesystem to check
 * for regressions in hugetlbfs quota accounting.
 */

extern int errno;

#define BUF_SZ 1024

/* Global test configuration */
static long hpage_size;
static char mountpoint[17];

/* map action flags */
#define ACTION_COW		0x0001
#define ACTION_TOUCH		0x0002

/* Testlet results */
#define GOOD 0
#define BAD_SIG  1
#define BAD_EXIT 2

char result_str[3][10] = { "pass", "killed", "fail" };

void cleanup(void)
{
	if (umount(mountpoint) == 0)
		rmdir(mountpoint);
}

/*
 * Debugging function:  Verify the counters in the hugetlbfs superblock that
 * are used to implement the filesystem quotas.
 */
void _verify_stat(int line, long tot, long free, long avail)
{
	struct statfs s;
	statfs(mountpoint, &s);

	if (s.f_blocks != tot || s.f_bfree != free || s.f_bavail != avail)
		FAIL("Bad quota counters at line %i: total: %li free: %li "
		       "avail: %li\n", line, s.f_blocks, s.f_bfree, s.f_bavail);
}
#define verify_stat(t, f, a) _verify_stat(__LINE__, t, f, a)

void get_quota_fs(unsigned long size)
{
	char size_str[20];

	snprintf(size_str, 20, "size=%luK", size/1024);

	sprintf(mountpoint, "/tmp/huge-XXXXXX");
	if (!mkdtemp(mountpoint))
		FAIL("Cannot create directory for mountpoint");

	if (mount("none", mountpoint, "hugetlbfs", 0, size_str)) {
		perror("mount");
		FAIL();
	}

	/*
	 * Set HUGETLB_PATH so future calls to hugetlbfs_unlinked_fd()
	 * will use this mountpoint.
	 */
	if (setenv("HUGETLB_PATH", mountpoint, 1))
		FAIL("Cannot set HUGETLB_PATH environment variable");

	verbose_printf("Using %s as temporary mount point.\n", mountpoint);
}

void map(unsigned long size, int mmap_flags, int action_flags)
{
	int fd;
	char *a, *b, *c;

	fd = hugetlbfs_unlinked_fd();
	if (!fd) {
		verbose_printf("hugetlbfs_unlinked_fd () failed");
		exit(1);
	}

	a = mmap(0, size, PROT_READ|PROT_WRITE, mmap_flags, fd, 0);
	if (a == MAP_FAILED) {
		verbose_printf("mmap failed\n");
		exit(1);
	}


	if (action_flags & ACTION_TOUCH)
		for (b = a; b < a + size; b += hpage_size)
			*(b) = 1;

	if (action_flags & ACTION_COW) {
		c = mmap(0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
		if ((*c) !=  1) {
			verbose_printf("Data mismatch when setting up COW");
			exit(1);
		}
		if (c == MAP_FAILED) {
			verbose_printf("Creating COW mapping failed");
			exit(1);
		}
		(*c) = 0;
		munmap(c, size);
	}

	munmap(a, size);
	close(fd);
}

void do_unexpected_result(int line, int expected, int actual)
{
	FAIL("Unexpected result on line %i: expected %s, actual %s",
		line, result_str[expected], result_str[actual]);
}

void _spawn(int l, int expected_result, unsigned long size, int mmap_flags,
							int action_flags)
{
	pid_t pid;
	int status;
	int actual_result;

	pid = fork();
	if (pid == 0) {
		map(size, mmap_flags, action_flags);
		exit(0);
	} else if (pid < 0) {
		FAIL("fork()");
	} else {
		waitpid(pid, &status, 0);
		if (WIFEXITED(status)) {
			if (WEXITSTATUS(status) == 0)
				actual_result = GOOD;
			else
				actual_result = BAD_EXIT;
		} else {
			actual_result = BAD_SIG;
		}

		if (actual_result != expected_result)
			do_unexpected_result(l, expected_result, actual_result);
	}
}
#define spawn(e,s,mf,af) _spawn(__LINE__, e, s, mf, af)

int main(int argc, char ** argv)
{
	int fd, private_resv;

	test_init(argc, argv);
	check_must_be_root();
	mountpoint[0] = '\0';
	hpage_size = check_hugepagesize();

	check_free_huge_pages(1);
	get_quota_fs(hpage_size);

	fd = hugetlbfs_unlinked_fd();
	if ((private_resv = kernel_has_private_reservations(fd)) == -1)
		FAIL("kernel_has_private_reservations() failed\n");
	close(fd);

	/*
	 * Check that unused quota is cleared when untouched mmaps are
	 * cleaned up.
	 */
	spawn(GOOD, hpage_size, MAP_PRIVATE, 0);
	verify_stat(1, 1, 1);
	spawn(GOOD, hpage_size, MAP_SHARED, 0);
	verify_stat(1, 1, 1);

	/*
	 * Check that simple page instantiation works within quota limits
	 * for private and shared mappings.
	 */
	spawn(GOOD, hpage_size, MAP_PRIVATE, ACTION_TOUCH);
	spawn(GOOD, hpage_size, MAP_SHARED, ACTION_TOUCH);

	/*
	 * Page instantiation should be refused if doing so puts the fs
	 * over quota.
	 */
	spawn(BAD_EXIT, 2 * hpage_size, MAP_SHARED, ACTION_TOUCH);

	/*
	 * If private mappings are reserved, the quota is checked up front
	 * (as is the case for shared mappings).
	 */
	if (private_resv)
		spawn(BAD_EXIT, 2 * hpage_size, MAP_PRIVATE, ACTION_TOUCH);
	else
		spawn(BAD_SIG, 2 * hpage_size, MAP_PRIVATE, ACTION_TOUCH);

	/*
	 * COW should not be allowed if doing so puts the fs over quota.
	 */
	spawn(BAD_SIG, hpage_size, MAP_SHARED, ACTION_TOUCH|ACTION_COW);
	spawn(BAD_SIG, hpage_size, MAP_PRIVATE, ACTION_TOUCH|ACTION_COW);

	/*
	 * Make sure that operations within the quota will succeed after
	 * some failures.
	 */
	spawn(GOOD, hpage_size, MAP_SHARED, ACTION_TOUCH);
	spawn(GOOD, hpage_size, MAP_PRIVATE, ACTION_TOUCH);

	PASS();
}
