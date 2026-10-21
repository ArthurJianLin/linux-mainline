// SPDX-License-Identifier: GPL-2.0
/*
 * Regression test for a Committed_AS accounting leak in mmap()'s
 * MAP_FIXED-over-accountable error path (mm/vma.c __mmap_region()).
 *
 * A MAP_FIXED private-writable mapping that replaces an existing
 * accountable (VM_ACCOUNT) mapping charges only the delta
 * (map->charged = pglen - old accounting) and then transfers the old
 * accounting to the new mapping (vms->nr_accounted = 0).  If the
 * operation then fails - e.g. the replacement VMA cannot be allocated -
 * the error path returns map->charged but the transferred old
 * accounting is lost: vms_clean_up_area() has already cleared the PTEs,
 * so vms_abort_munmap_vmas() cannot reattach the old mapping and
 * vms_complete_munmap_vmas() unaccounts vms->nr_accounted, which is now
 * zero.  Before the fix the old mapping's pages were never unaccounted:
 * the charge is not tied to any VMA, so it survived even process exit
 * and permanently inflated the global Committed_AS on every such failed
 * call.
 *
 * The failure is injected deterministically via failslab: with
 * cache-filter enabled the kernel only fails allocations from caches
 * that have the "fails" sysfs flag set, so opting in the vm_area_struct
 * cache makes exactly the vm_area_alloc() inside __mmap_new_vma() fail.
 * GFP_KERNEL allocations are made injectable by clearing
 * ignore-gfp-wait.
 *
 * Requires root, debugfs, CONFIG_FAULT_INJECTION, CONFIG_FAILSLAB and
 * CONFIG_FAULT_INJECTION_DEBUG_FS; skips otherwise.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../kselftest.h"

#define FAILSLAB_DIR	"/sys/kernel/debug/failslab"
#define SLAB_DIR	"/sys/kernel/slab"
#define VMA_CACHE	"vm_area_struct"
#define PAGE_SZ		4096UL
#define MB(x)		((x) * 1024UL * 1024UL)

#define ANCHOR_LEN	MB(8)
#define TEST_LEN	MB(64)
#define ROUNDS		3

/*
 * A single leaking round inflates Committed_AS by TEST_LEN (64 MB),
 * orders of magnitude above counter noise; a fixed kernel stays at
 * zero.  Anything above half a delta per summed round is a leak.
 */
#define LEAK_THRESHOLD_KB ((long)(TEST_LEN / 1024) * ROUNDS / 2)

/* failslab attributes to save, adjust and restore. */
static const char * const fi_attrs[] = {
	"probability",
	"interval",
	"times",
	"ignore-gfp-wait",
	"cache-filter",
	NULL,
};
static char fi_saved[5][32];
static bool fi_saved_valid;
static char vma_fails_old[8];
static bool vma_fails_saved;

static long committed_as_kb(void)
{
	char line[160];
	long val = -1;
	FILE *f;

	f = fopen("/proc/meminfo", "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "Committed_AS: %ld", &val) == 1)
			break;
	}
	fclose(f);
	return val;
}

static int fi_write(const char *attr, const char *val)
{
	char path[256];
	int fd, ret;

	ret = snprintf(path, sizeof(path), "%s/%s", FAILSLAB_DIR, attr);
	if (ret >= (int)sizeof(path))
		return -1;
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -errno;
	ret = write(fd, val, strlen(val));
	close(fd);
	if (ret < 0)
		return -errno;
	return 0;
}

static int fi_read(const char *attr, char *buf, size_t sz)
{
	char path[256];
	int fd, ret;

	ret = snprintf(path, sizeof(path), "%s/%s", FAILSLAB_DIR, attr);
	if (ret >= (int)sizeof(path))
		return -1;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;
	ret = read(fd, buf, sz - 1);
	close(fd);
	if (ret < 0)
		return -errno;
	buf[ret] = '\0';
	return 0;
}

static int slab_write(const char *attr, const char *val)
{
	char path[256];
	int fd, ret;

	ret = snprintf(path, sizeof(path), "%s/%s/%s",
		       SLAB_DIR, VMA_CACHE, attr);
	if (ret >= (int)sizeof(path))
		return -1;
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -errno;
	ret = write(fd, val, strlen(val));
	close(fd);
	if (ret < 0)
		return -errno;
	return 0;
}

static int slab_read(const char *attr, char *buf, size_t sz)
{
	char path[256];
	int fd, ret;

	ret = snprintf(path, sizeof(path), "%s/%s/%s",
		       SLAB_DIR, VMA_CACHE, attr);
	if (ret >= (int)sizeof(path))
		return -1;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;
	ret = read(fd, buf, sz - 1);
	close(fd);
	if (ret < 0)
		return -errno;
	buf[ret] = '\0';
	return 0;
}

static void fi_restore(void)
{
	int i;

	if (!fi_saved_valid)
		return;
	for (i = 0; fi_attrs[i]; i++)
		fi_write(fi_attrs[i], fi_saved[i]);
	if (vma_fails_saved)
		slab_write("fails", vma_fails_old);
	fi_saved_valid = false;
	vma_fails_saved = false;
}

static bool have_debugfs(void)
{
	struct stat st;

	if (access(FAILSLAB_DIR, F_OK) == 0)
		return true;
	/* Try to mount debugfs; we might run on a minimal VM. */
	if (access("/sys/kernel/debug", F_OK) == 0 &&
	    stat("/sys/kernel/debug", &st) == 0 && S_ISDIR(st.st_mode)) {
		if (mount("debugfs", "/sys/kernel/debug", "debugfs",
			  0, NULL) == 0)
			return access(FAILSLAB_DIR, F_OK) == 0;
	}
	return false;
}

static int setup_fault_injection(void)
{
	char buf[32];
	int i, ret;

	for (i = 0; fi_attrs[i]; i++) {
		ret = fi_read(fi_attrs[i], buf, sizeof(buf));
		if (ret < 0)
			return ret;
		memcpy(fi_saved[i], buf, sizeof(fi_saved[i]));
		fi_saved[i][sizeof(fi_saved[i]) - 1] = '\0';
	}
	fi_saved_valid = true;

	/* Fail the allocation selected by /proc/self/fail-nth. */
	ret = fi_write("probability", "100");
	if (ret < 0)
		return ret;
	ret = fi_write("interval", "1");
	if (ret < 0)
		return ret;
	ret = fi_write("times", "1");
	if (ret < 0)
		return ret;
	/* vm_area_alloc() uses GFP_KERNEL. */
	ret = fi_write("ignore-gfp-wait", "0");
	if (ret < 0)
		return ret;
	/* Only caches with ->fails set are affected. */
	ret = fi_write("cache-filter", "1");
	if (ret < 0)
		return ret;

	/* Opt the vm_area_cache in. */
	ret = slab_read("fails", buf, sizeof(buf));
	if (ret < 0)
		return ret;
	vma_fails_old[0] = buf[0] == '1' ? '1' : '0';
	vma_fails_old[1] = '\0';
	vma_fails_saved = true;
	return slab_write("fails", "1");
}

/*
 * Build the layout for a round:
 *   anchor L:  ANCHOR_LEN, PROT_READ            (not accountable)
 *   test:      TEST_LEN,   PROT_READ|WRITE anon (accountable)
 *   anchor R:  ANCHOR_LEN, PROT_READ
 *
 * The test VMA has different flags from both neighbours, so a
 * MAP_FIXED replacement cannot merge with them and must allocate a new
 * VMA, which is where the injected failure lands.
 */
static void *make_layout(void)
{
	void *a, *t, *r;

	a = mmap(NULL, ANCHOR_LEN, PROT_READ,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (a == MAP_FAILED)
		return NULL;
	t = mmap(NULL, TEST_LEN, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (t == MAP_FAILED) {
		munmap(a, ANCHOR_LEN);
		return NULL;
	}
	r = mmap(NULL, ANCHOR_LEN, PROT_READ,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (r == MAP_FAILED) {
		munmap(t, TEST_LEN);
		munmap(a, ANCHOR_LEN);
		return NULL;
	}

	/*
	 * Anchors stay mapped for the process lifetime; the test VMA is
	 * the return value.  A MAP_FIXED replacement of @t cannot merge
	 * with either anchor because the flags differ (PROT_READ vs
	 * PROT_READ|PROT_WRITE), so it has to allocate a new VMA.
	 */
	return t;
}

static int fail_nth_set(unsigned int nth)
{
	char buf[16];
	int fd, ret;

	/* Reset first, then arm the nth allocation of this task. */
	fd = open("/proc/self/fail-nth", O_WRONLY);
	if (fd < 0)
		return -errno;
	ret = write(fd, "0", 1);
	if (ret < 0) {
		close(fd);
		return -errno;
	}
	snprintf(buf, sizeof(buf), "%u", nth);
	ret = write(fd, buf, strlen(buf));
	close(fd);
	return ret < 0 ? -errno : 0;
}

/*
 * One failing round: MAP_FIXED a replacement over @test.  The nth
 * vm_area_struct allocation of this task is made to fail, so the
 * replacement VMA allocation inside __mmap_new_vma() fails with ENOMEM.
 * Returns the Committed_AS change in KB, -2 if the injection did not
 * land (the mapping succeeded, e.g. the VMA was merged; the caller
 * rebuilds the layout and retries with a different @nth), -1 on error.
 */
static long failing_round(void *test, unsigned int nth)
{
	long before, after;
	void *p;
	int err;

	before = committed_as_kb();

	if (fail_nth_set(nth) < 0) {
		ksft_print_msg("cannot arm fail-nth: %s\n", strerror(errno));
		return -1;
	}
	errno = 0;
	p = mmap(test, TEST_LEN, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	err = errno;
	fail_nth_set(0);

	after = committed_as_kb();

	if (p != MAP_FAILED) {
		/* Injection missed (e.g. VMA was merged). */
		munmap(p, TEST_LEN);
		return -2;
	}
	if (err != ENOMEM) {
		ksft_print_msg("unexpected mmap error %d (%s)\n",
			       err, strerror(err));
		return -1;
	}
	return after - before;
}

int main(void)
{
	long baseline, delta = 0, d;
	int rounds_done = 0, attempts = 0;
	void *test;

	ksft_print_header();

	if (geteuid() != 0) {
		ksft_print_msg("need root to write /sys (debugfs, slab)\n");
		ksft_exit_skip("requires root\n");
	}
	if (!have_debugfs())
		ksft_exit_skip("debugfs not available\n");
	if (access(FAILSLAB_DIR, F_OK) != 0)
		ksft_exit_skip("failslab fault injection not configured\n");

	if (setup_fault_injection() < 0) {
		fi_restore();
		ksft_exit_skip("fault-injection setup failed\n");
	}

	/*
	 * Control: a successful replacement of an accountable mapping
	 * must charge TEST_LEN.
	 */
	baseline = committed_as_kb();
	test = make_layout();
	if (!test) {
		fi_restore();
		ksft_exit_skip("cannot create test mapping\n");
	}
	{
		void *ok = mmap(test, TEST_LEN, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
				 -1, 0);
		if (ok == MAP_FAILED) {
			ksft_print_msg("control MAP_FIXED failed: %s\n",
				       strerror(errno));
			fi_restore();
			ksft_print_msg("control mapping failed\n");
			ksft_exit_fail();
		}
		if (committed_as_kb() - baseline < (long)(TEST_LEN / 1024)) {
			ksft_print_msg("Committed_AS did not grow for accountable mapping\n");
			munmap(ok, TEST_LEN);
			fi_restore();
			ksft_print_msg("accounting sanity check failed\n");
			ksft_exit_fail();
		}
		/* The success-path replacement now serves as round target. */
		test = ok;
	}

	/* Leak rounds: repeatedly MAP_FIXED-replace @test with a failure. */
	while (rounds_done < ROUNDS && attempts < 3 * ROUNDS) {
		unsigned int nth;

		/*
		 * Find the task-relative allocation index that hits the
		 * replacement VMA.  Each miss leaves a successfully merged
		 * mapping, so rebuild the layout and retry with the next nth.
		 */
		for (nth = 1; nth <= 64 && rounds_done < ROUNDS; nth++) {
			attempts++;
			d = failing_round(test, nth);
			if (d == -2) {
				munmap(test, TEST_LEN);
				test = make_layout();
				if (!test) {
					fi_restore();
					ksft_exit_skip("cannot rebuild layout\n");
				}
				continue;
			}
			if (d < 0) {
				fi_restore();
				ksft_print_msg("round failed\n");
				ksft_exit_fail();
			}
			delta += d;
			rounds_done++;
		}

		/* Rebuild the target for the next round. */
		munmap(test, TEST_LEN);
		test = make_layout();
		if (!test) {
			fi_restore();
			ksft_exit_skip("cannot rebuild layout\n");
		}
	}

	fi_restore();

	ksft_print_msg("Committed_AS delta over %d failing round(s): %ld KB\n",
		       rounds_done, delta);

	if (rounds_done < ROUNDS) {
		ksft_print_msg("could not obtain enough failing rounds\n");
		ksft_exit_skip("not enough failing rounds\n");
	}
	if (delta >= LEAK_THRESHOLD_KB) {
		ksft_print_msg("Committed_AS leaked %ld KB (>= %ld)\n",
			       delta, LEAK_THRESHOLD_KB);
		ksft_exit_fail();
	}

	ksft_exit_pass();
}
