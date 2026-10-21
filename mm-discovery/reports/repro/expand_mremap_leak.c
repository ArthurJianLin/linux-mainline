// SPDX-License-Identifier: GPL-2.0
/*
 * expand_mremap_leak.c - 复现 mm/mremap.c Committed_AS 泄漏 BUG
 *
 * BUG: move_vma() 中扩张搬移（delta > 0，非 MREMAP_DONTUNMAP）在
 * move_page_tables() 中途失败时，vrm_calc_charge() 已记入全局
 * vm_committed_as 的 delta 页电荷永远不会被 uncharge：
 * 错误恢复路径 unmap 新 VMA 前清除了 VMA_ACCOUNT_BIT（本意是保护
 * 旧 VMA 的账），导致这笔"裸账"（不挂任何 VMA）无人回收，
 * Committed_AS 永久膨胀，直到重启。
 *
 * 复现原理：
 *   1. mmap 一块 MAP_PRIVATE|PROT_WRITE 匿名 VMA（带 VM_ACCOUNT）并写脏，
 *      建立完整页表；
 *   2. 通过 /proc/self/make-it-fail 打开本进程 fail_page_alloc 注入窗口
 *      （需 root 先配置 debugfs：task-filter=1, min-order=0,
 *      ignore-gfp-wait=0, probability=100）；
 *   3. mremap(MREMAP_MAYMOVE|MREMAP_FIXED) 扩张搬移到新地址 ——
 *      move_page_tables() 为 dst 分配页表（order-0, GFP_PGTABLE_USER）
 *      时被注入失败 → 返回 ENOMEM，内核把数据回退到旧 VMA；
 *   4. munmap 旧 VMA，进程回到本轮开始前的状态。
 *
 * 判据：每轮结束时进程 VMA 布局与轮初完全相同，但全局 Committed_AS
 * 净增 delta（= 期望泄漏）。累计 N 轮 → Committed_AS 净增 N * delta。
 *
 * case 1（默认）：扩张搬移（new_len > old_len）—— 期望泄漏 delta/轮
 * case 2（对照）：等长搬移（new_len == old_len）—— 期望零泄漏
 *                 （证明"mremap 失败"本身不产生泄漏，泄漏只来自 delta 电荷）
 *
 * 编译：gcc -Wall -Wextra -O2 -o expand_mremap_leak expand_mremap_leak.c
 * 运行前：以 root 执行 ./setup-fi.sh 配置 fault injection
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define PAGE_SZ       4096UL
#define MB(x)         ((x) * 1024UL * 1024UL)
#define REGION_GAP    MB(8)          /* old VMA 与 dst 地址间的空隙 */

static long read_committed_as_kb(void)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[160];
	long val = -1;

	if (!f) {
		perror("open /proc/meminfo");
		exit(1);
	}
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "Committed_AS: %ld", &val) == 1)
			break;
	}
	fclose(f);
	if (val < 0) {
		fprintf(stderr, "FATAL: Committed_AS not found in /proc/meminfo\n");
		exit(1);
	}
	return val;
}

static void set_make_it_fail(int on)
{
	char buf[8];
	int fd = open("/proc/self/make-it-fail", O_WRONLY);
	int n;

	if (fd < 0) {
		fprintf(stderr,
			"FATAL: cannot open /proc/self/make-it-fail: %s\n"
			"       (kernel needs CONFIG_FAULT_INJECTION=y, and run "
			"setup-fi.sh as root first)\n", strerror(errno));
		exit(1);
	}
	n = snprintf(buf, sizeof(buf), "%d\n", on);
	if (write(fd, buf, n) != n) {
		fprintf(stderr, "FATAL: write make-it-fail failed: %s\n",
			strerror(errno));
		close(fd);
		exit(1);
	}
	close(fd);
}

/* 写脏每一页：建立完整页表（每页首字节写入即可） */
static void touch_pages(volatile char *base, size_t len)
{
	volatile char *p;

	for (p = base; p < base + len; p += PAGE_SZ)
		*p = (char)0x5a;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-r rounds] [-s old_MB] [-d delta_MB] [-c case]\n"
		"  -r rounds   number of iterations        (default 10)\n"
		"  -s old_MB   source VMA size in MB       (default 64)\n"
		"  -d delta_MB expansion delta in MB       (default 64)\n"
		"  -c case     1 = expanding move (leak)   (default)\n"
		"              2 = equal-length move (control, no leak)\n",
		prog);
	exit(1);
}

int main(int argc, char **argv)
{
	int rounds = 10;
	int old_mb = 64;
	int delta_mb = 64;
	int kase = 1;
	int opt;
	int i;
	int hits = 0, misses = 0;
	unsigned long old_len, new_len, delta_len;
	unsigned long region_len;
	char *region, *old_addr, *dst_addr;
	long start_kb, end_kb;
	long expect_total_kb, got_total_kb;

	while ((opt = getopt(argc, argv, "r:s:d:c:")) != -1) {
		switch (opt) {
		case 'r': rounds = atoi(optarg); break;
		case 's': old_mb = atoi(optarg); break;
		case 'd': delta_mb = atoi(optarg); break;
		case 'c': kase = atoi(optarg); break;
		default:  usage(argv[0]);
		}
	}
	if (rounds < 1 || old_mb < 1 || (kase != 1 && kase != 2)) {
		usage(argv[0]);
	}

	old_len = MB(old_mb);
	if (kase == 1) {
		delta_len = MB(delta_mb);
		new_len = old_len + delta_len;
	} else {
		/* 对照组：等长搬移，delta = 0，期望零泄漏 */
		delta_len = 0;
		new_len = old_len;
	}

	/* 预留一块连续地址空间（PROT_NONE 不记账），munmap 后留作棋盘 */
	region_len = old_len + REGION_GAP + new_len + MB(16);
	region = mmap(NULL, region_len, PROT_NONE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (region == MAP_FAILED) {
		perror("mmap reserve region");
		return 1;
	}
	if (munmap(region, region_len)) {
		perror("munmap reserve region");
		return 1;
	}
	old_addr = region;
	dst_addr = region + old_len + REGION_GAP;

	printf("=== mm/mremap Committed_AS leak reproducer ===\n");
	printf("case %d: old=%dMB new=%dMB delta=%dMB rounds=%d\n",
	       kase, old_mb, kase == 1 ? old_mb + delta_mb : old_mb,
	       kase == 1 ? delta_mb : 0, rounds);
	printf("old VMA @ %p, dst @ %p\n", old_addr, dst_addr);
	printf("expected leak per round: %lu KB\n\n", delta_len >> 10);

	start_kb = read_committed_as_kb();
	printf("Committed_AS baseline: %ld KB\n\n", start_kb);

	for (i = 1; i <= rounds; i++) {
		long before_kb, after_kb, round_leak_kb;
		void *res;
		int saved_errno;
		char *v;

		before_kb = read_committed_as_kb();

		/* 1. 建立源 VMA（MAP_PRIVATE + PROT_WRITE -> VM_ACCOUNT） */
		v = mmap(old_addr, old_len, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (v == MAP_FAILED) {
			perror("mmap old VMA");
			return 1;
		}
		if (madvise(v, old_len, MADV_NOHUGEPAGE)) {
			perror("madvise NOHUGEPAGE (non-fatal)");
		}
		touch_pages(v, old_len);

		/* 2. 打开本进程页分配失败注入窗口 */
		set_make_it_fail(1);

		/* 3. 扩张搬移：内核在 move_page_tables() 分配 dst 页表时
		 *    被注入 ENOMEM，错误恢复路径把数据回退到旧 VMA */
		res = mremap(old_addr, old_len, new_len,
			     MREMAP_MAYMOVE | MREMAP_FIXED, dst_addr);
		saved_errno = errno;

		/* 4. 关闭注入窗口（关键：窗口内只允许这一个系统调用） */
		set_make_it_fail(0);

		if (res == MAP_FAILED) {
			if (saved_errno != ENOMEM) {
				fprintf(stderr,
					"FATAL round %d: mremap failed with "
					"unexpected errno %d (%s)\n",
					i, saved_errno, strerror(saved_errno));
				return 1;
			}
			/* 失败路径：数据已回退到旧 VMA，unmap 它归零 */
			if (munmap(old_addr, old_len)) {
				perror("munmap old VMA");
				return 1;
			}
			after_kb = read_committed_as_kb();
			round_leak_kb = after_kb - before_kb;
			hits++;
			printf("round %2d: mremap failed with ENOMEM "
			       "(injected), round delta Committed_AS: "
			       "%+ld KB (expected %lu)\n",
			       i, round_leak_kb, delta_len >> 10);
		} else {
			/* 注入未命中（概率配置异常等）：搬移意外成功，
			 * 数据在 dst，unmap dst 恢复，不计泄漏 */
			if (munmap(dst_addr, new_len)) {
				perror("munmap dst VMA");
				return 1;
			}
			misses++;
			printf("round %2d: WARNING injection MISSED, "
			       "mremap succeeded; round excluded\n", i);
		}
	}

	end_kb = read_committed_as_kb();
	got_total_kb = end_kb - start_kb;
	expect_total_kb = (long)(delta_len >> 10) * rounds;

	printf("\n--- summary (case %d) ---\n", kase);
	printf("injected failures : %d / %d rounds\n", hits, rounds);
	printf("injection misses  : %d\n", misses);
	printf("Committed_AS      : %ld KB -> %ld KB\n", start_kb, end_kb);
	printf("net growth        : %+ld KB\n", got_total_kb);
	printf("expected leak     : %ld KB (%d rounds x %lu KB)\n",
	       expect_total_kb, rounds, delta_len >> 10);

	if (kase == 1) {
		if (misses == 0 && got_total_kb >= expect_total_kb * 3 / 4) {
			printf("\n*** LEAK CONFIRMED: Committed_AS permanently "
			       "grew by the charged delta, while the process "
			       "holds NO extra VMA. ***\n");
		} else if (misses > 0) {
			printf("\n*** INCONCLUSIVE: injection missed in %d "
			       "round(s); check setup-fi.sh settings. ***\n",
			       misses);
		} else {
			printf("\n*** NO LEAK observed (growth below 3/4 of "
			       "expected). Re-check kernel version/patch. ***\n");
		}
	} else {
		/* 对照组：等长搬移 delta=0，任何显著增长都说明环境有噪声 */
		if (labs(got_total_kb) < 64 * rounds) {
			printf("\n*** CONTROL OK: no leak for equal-length "
			       "moves (delta = 0), as expected. ***\n");
		} else {
			printf("\n*** CONTROL NOISY: unexpected growth, "
			       "check environment. ***\n");
		}
	}

	return 0;
}
