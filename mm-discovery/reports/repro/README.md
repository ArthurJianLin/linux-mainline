---
AIGC:
  ContentProducer: '001191110102MAD55U9H0F10002'
  ContentPropagator: '001191110102MAD55U9H0F10002'
  Label: '1'
  ProduceID: '92086f81-9c2f-4443-97b9-4ec27208e126'
  PropagateID: '92086f81-9c2f-4443-97b9-4ec27208e126'
  ReservedCode1: '05e81bd1-6b57-4426-a833-37ad3e23c040'
  ReservedCode2: '05e81bd1-6b57-4426-a833-37ad3e23c040'
---

# mm/mremap Committed_AS 泄漏复现工程

复现对象：`mm/mremap.c` `move_vma()` 扩张搬移错误路径泄漏 `vm_committed_as`
（引入于 Linux 2.6.12-rc2，v7.3-rc4 仍存在，akpm mm-unstable 未修复）。

## 原理（一分钟版）

1. 私有可写 VMA 做 `mremap` 扩张搬移（new_len > old_len）时，内核先对
   delta 页 charge 全局 `vm_committed_as`（`vrm_calc_charge()`）；
2. 用 fault injection 让 `move_page_tables()` 中途分配 dst 页表失败
   → `mremap` 返回 `ENOMEM`；
3. 内核错误恢复路径把数据回退旧 VMA，随后 unmap 新 VMA——但 unmap 前
   清除了新 VMA 的 `VMA_ACCOUNT_BIT`（为保护旧 VMA 的账）；
4. 结果：delta 的 charge 永远无人 uncharge。**进程状态回到原样，全局
   Committed_AS 却净增 delta**。该账不挂任何 VMA，进程退出也不清，
   直到重启。

## 文件

| 文件 | 说明 |
|---|---|
| `expand_mremap_leak.c` | 复现程序（case 1 泄漏 + case 2 等长对照） |
| `setup-fi.sh` | 配置 fail_page_alloc（root，一次） |
| `run.sh` | 编译 + 运行两个 case + dmesg 证据 |

## 内核要求

```
CONFIG_FAULT_INJECTION=y
CONFIG_FAIL_PAGE_ALLOC=y
CONFIG_FAULT_INJECTION_DEBUG_FS=y   # 且 debugfs 已挂载
```

检查：`zcat /proc/config.gz | grep -E 'FAULT_INJECTION|FAIL_PAGE_ALLOC'`
（或 `/boot/config-$(uname -r)`）。本机若未开启，请在测试 VM 上跑。

## 使用

```sh
# 1. 配置注入（root，只需一次）
sudo ./setup-fi.sh

# 2. 运行（普通用户即可）
./run.sh
```

## 判读

**case 1（扩张搬移）**：每轮 `mremap` 失败（注入的 ENOMEM）后，进程 VMA
布局与轮初完全一致（munmap 已回收源 VMA），但 `Committed_AS` 每轮净增
`delta`（默认 64MB）。10 轮累计净增约 640MB，程序输出
`*** LEAK CONFIRMED ***`。

**case 2（等长搬移对照）**：delta=0，无 charge 可泄，`Committed_AS` 不变，
输出 `*** CONTROL OK ***`。证明泄漏来自 delta 电荷而非"mremap 失败"本身。

**dmesg**：每轮应有一条 `FAULT_INJECTION: forcing a failure. name ...`，
栈顶在页表分配路径（`should_fail_alloc_page` → `__alloc_pages` →
`__pte_alloc_one_noprof` / `pud/pmd alloc`）。`setup-fi.sh` 把 verbose
设为 1；改 2 可看到完整调用栈确认打点位置。

## 常见问题

- **程序报 cannot open /proc/self/make-it-fail**：内核没开
  `CONFIG_FAULT_INJECTION`，换测试内核/VM。
- **round 打印 injection MISSED**：`setup-fi.sh` 没执行或被覆盖。重点核对
  `min-order=0`、`ignore-gfp-wait=0`（这两个默认值会放行页表分配）。
- **Committed_AS 波动大**：测试机上有其他负载在 mmap/munmap。可加大
  delta（`-d 256`）提高信噪比，或独占 VM 运行。
- **跑完恢复**：`echo 0 > /sys/kernel/debug/fail_page_alloc/probability`
  （task-filter=1 下留着也只影响主动开启 make-it-fail 的任务，可安全共存）。

## 关键源码位置（v7.3-rc4）

| 位置 | 作用 |
|---|---|
| `mm/mremap.c:984` `vrm_calc_charge()` | charge delta 页 |
| `mm/mremap.c:1280` | copy_vma 失败时的正确 uncharge（对照） |
| `mm/mremap.c:1292-1312` | move_page_tables 失败 → 错误恢复 |
| `mm/mremap.c:1179-1202` `unmap_source_vma()` | 清 VMA_ACCOUNT_BIT 后 unmap（泄漏点） |
| `mm/fail_page_alloc.c:26` `should_fail_alloc_page()` | 注入打点 |
| `lib/fault-inject.c:124` `should_fail_ex()` | fail-nth / make-it-fail 判定 |

> AI生成