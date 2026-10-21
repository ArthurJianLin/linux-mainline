#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# run.sh - 一键复现 mm/mremap Committed_AS 泄漏
#
# 用法（测试机/VM 上，root）:
#   ./setup-fi.sh                 # 先配置 fault injection
#   ./run.sh                      # 编译 + case1(泄漏) + case2(对照)
#
# 建议在测试 VM 中运行，避免影响生产负载。
set -e
cd "$(dirname "$0")"

echo "== build =="
gcc -Wall -Wextra -O2 -o expand_mremap_leak expand_mremap_leak.c
echo

echo "== case 1: expanding move, expect +delta leak per round =="
./expand_mremap_leak -r 10 -s 64 -d 64 -c 1
echo
echo "== case 2 (control): equal-length move, expect zero leak =="
./expand_mremap_leak -r 10 -s 64 -d 64 -c 2
echo
echo "== dmesg tail (injection evidence) =="
dmesg 2>/dev/null | grep -i 'FAULT_INJECTION' | tail -5 || true
echo
echo "== restore (optional) =="
echo "echo 0 > /sys/kernel/debug/fail_page_alloc/probability"
