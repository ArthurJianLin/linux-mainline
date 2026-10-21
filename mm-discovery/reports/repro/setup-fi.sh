#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# setup-fi.sh - 配置 fail_page_alloc fault injection（root 执行一次）
#
# 三个默认值必须显式覆盖，否则注入打不中 mremap 的页表分配：
#   min-order      默认 1  -> order-0 的 pte/pmd 表分配不注入，必须设 0
#   ignore-gfp-wait 默认 1 -> 带 __GFP_DIRECT_RECLAIM 的分配不注入，
#                             而 GFP_PGTABLE_USER 恰好带，必须设 0
#   task-filter    设 1    -> 只影响写了 /proc/self/make-it-fail 的任务
#                             （demo 程序自身），系统其他进程不受干扰
D=/sys/kernel/debug/fail_page_alloc

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: run as root" >&2
    exit 1
fi
if [ ! -d "$D" ]; then
    echo "ERROR: $D not found. Kernel needs:" >&2
    echo "  CONFIG_FAULT_INJECTION=y" >&2
    echo "  CONFIG_FAIL_PAGE_ALLOC=y" >&2
    echo "  CONFIG_FAULT_INJECTION_DEBUG_FS=y (and debugfs mounted)" >&2
    exit 1
fi

echo 100 > "$D/probability"      # 每次候选分配都注入
echo -1  > "$D/times"            # 无限次数
echo 1   > "$D/task-filter"      # 仅 make-it-fail=1 的任务受影响
echo 0   > "$D/ignore-gfp-wait"  # 允许带 DIRECT_RECLAIM 的 GFP（页表分配必需）
echo 0   > "$D/ignore-gfp-highmem"
echo 0   > "$D/min-order"        # 允许 order-0（页表是单页分配）
echo 1   > "$D/verbose"          # dmesg 打印注入事件（改 2 可看调用栈）

echo "fail_page_alloc configured:"
grep -H . "$D/probability" "$D/times" "$D/task-filter" \
    "$D/ignore-gfp-wait" "$D/min-order" 2>/dev/null
echo
echo "NOTE: with task-filter=1 this only affects tasks that write"
echo "      1 to /proc/self/make-it-fail (i.e. the demo itself)."
echo "      To disable injection entirely:  echo 0 > $D/probability"
