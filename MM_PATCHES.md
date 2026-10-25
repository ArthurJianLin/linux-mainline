# MM patch series: sysctl correctness + lowmem_reserve batching

**Branch:** `mm/sysctl-and-lowmem-reserve-batch`  
**Remote:** https://github.com/ArthurJianLin/linux-mainline  
**Baseline:** `main` @ `ccb58b178` (Linux 7.3 / torvalds `c03c1a14507d`)

This document tracks three related mm issues. Patches **#2** and **#3** are
implemented as real code on this branch. Issue **#1** is already present on
`main` (document only).

---

## Commit map

| Commit (subject) | Issue |
|------------------|-------|
| `mm/compaction: validate-then-commit compact_memory sysctl` | #2 |
| `mm: batch lowmem_reserve refresh for managed page updates` | #3 |
| `MM_PATCHES.md: document sysctl and lowmem_reserve series` | docs |

Issue #1 code is already on baseline `main` (see below) — no extra commit.

---

## Issue #1 — `lowmem_reserve_ratio` sysctl (correctness)

### Problem (historical)

`lowmem_reserve_ratio_sysctl_handler()` previously:

1. Ignored the return value of `proc_dointvec_minmax()`.
2. Called `setup_per_zone_lowmem_reserve()` on **reads** as well as writes.
3. Parsed directly into the live `sysctl_lowmem_reserve_ratio[]` vector (partial
   multi-int writes could desync the array from `zone->lowmem_reserve[]`).
4. Used a post-parse `< 1 → 0` clamp instead of `.extra1 = SYSCTL_ZERO`.

### Fix / upstream status

**Already in this tree’s `main`.** The handler uses a temporary `ratio[]`,
commits only after a successful parse, refreshes only on write, and the
ctl_table has `.extra1 = SYSCTL_ZERO`. A v5 posting on lore had Acks
(Johannes Weiner / Vlastimil Babka). Do not re-post a duplicate.

---

## Issue #2 — `compact_memory` validate-then-commit (correctness)

### Problem

`sysctl_compaction_handler()` called `proc_dointvec()` on the live
`sysctl_compact_memory`, then checked `!= 1` and returned `-EINVAL`.

**Worst-case:** `echo 2 > /proc/sys/vm/compact_memory` → parse succeeds, live
value becomes `2`, validation fails with `-EINVAL`, but the illegal value
**remains stored**. Mode is `0200` (write-only).

### Fix

Same pattern as `overcommit_policy_handler`:

1. On write, parse into a temporary `int` via a copied `struct ctl_table`.
2. If parse fails, return the error.
3. If `compact != 1`, return `-EINVAL` without updating the live variable.
4. On success, set `sysctl_compact_memory = 1` and call `compact_nodes()`.
5. On read, return `-EPERM` (write-only sysctl).

### Files

- `mm/compaction.c` — `sysctl_compaction_handler`

---

## Issue #3 — batch `lowmem_reserve` refresh (performance)

### Problem

`adjust_managed_page_count()` updates counters and always calls
`setup_per_zone_lowmem_reserve()` (`O(nr_nodes × nr_zones²)` +
`calculate_totalreserve_pages()`). Batch paths that adjust one page at a time
paid that rebuild per page:

| Caller | Amplification |
|--------|----------------|
| Balloon inflate/deflate | Per page, often under `balloon_pages_lock` |
| `free_reserved_page()` in `__free_reserved_area` / CMA cleanup | Per page |
| Memory offline | Full adjust + `init_per_zone_wmark_min()` → **double** refresh |

### API

In `include/linux/mm.h` + `mm/page_alloc.c`:

- `adjust_managed_page_count_norefresh(page, count)` — counters only.
- `refresh_zone_lowmem_reserves()` — wraps static `setup_per_zone_lowmem_reserve()`.
- `adjust_managed_page_count()` — norefresh + refresh (preserves `EXPORT_SYMBOL` ABI).
- `free_reserved_pages_norefresh()` / `free_reserved_page_norefresh()` — free
  without refresh.
- `free_reserved_pages()` — norefresh + refresh (single-call ABI unchanged).

### Callers converted in this push

1. **`mm/balloon.c`** — enqueue/dequeue use norefresh under lock; refresh after unlock.
2. **`mm/memblock.c`** — `__free_reserved_area` batches with one refresh.
3. **`mm/cma.c`** — activation failure cleanup loop batched.
4. **`mm/memory_hotplug.c`** — offline path uses norefresh before
   `init_per_zone_wmark_min()`.

### Follow-ups

Hyper-V / Xen / virtio balloon and other per-page
`adjust_managed_page_count()` loops — convert opportunistically.

---

## 中文摘要

1. **#1 `lowmem_reserve_ratio`**：临时数组校验后提交、仅 write 时 setup；**本仓库 `main` 已合入**，本分支只文档说明。
2. **#2 `compact_memory`**：先解析到临时变量，仅当值为 `1` 才写入并触发 compaction。
3. **#3 managed 计数批量刷新**：norefresh/refresh API；balloon、free_reserved 批量路径、CMA 清理、热插拔 offline 避免每页全量重算。

---

## How to use this branch

```bash
git fetch origin mm/sysctl-and-lowmem-reserve-batch
git checkout mm/sysctl-and-lowmem-reserve-batch
```

Upstream posting tip: send #2 as a small standalone correctness fix; #3 as a
short performance series. Skip #1 if already merged upstream.
