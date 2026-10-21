# Patch 解构 · 第 1 期：605f6586ecf7 mm/vma: do not leak memory when .mmap_prepare swaps the file

> 栏目说明：逐补丁拆解 mm 子系统代表性 commit——机制背景、修复设计、错误路径全集、后续回响、可复用方法论。目标不是找补丁机会，是把上游高手的修法拆成可复用的审查思维。
> 素材来源：GitHub kernelorg-mirror（原补丁）+ 主线历史（修复回响 894f99eb535c）+ 本地 7.3 树现状核对。

---

## 0. 一句话摘要

**Lorenzo Stoakes 在 mmap 新框架中修「.mmap_prepare 回调更换文件后成功路径多 pin 一次引用」的泄漏（605f6586ecf7）；一个月后 Sechang Lim 用 syzkaller+failslab+kmemleak 抓到同一设计在错误路径的对称泄漏（894f99eb535c），由 Lorenzo 亲自 review 补上。** 两条 patch 合起来构成「成功路径资源转移 → 错误路径反向补偿」的完整教科书。

---

## 1. 机制背景：mmap_prepare 与 file swap

7.1/7.2 引入的新 mmap 框架（`vfs_mmap_prepare()` + `struct vm_area_desc`）允许驱动在建立 VMA **之前**改写映射属性（file/pgoff/vm_ops/page_prot）。最典型场景：

```
mmap(/dev/zero, MAP_SHARED)
  └→ mmap_zero_prepare()                 # /dev/zero 的 mmap_prepare 回调
        └→ shmem_zero_setup_desc()       # MAP_SHARED 需 shmem 匿名文件支撑
              ├─ alloc_file_pseudo()      # 新建 shmem 文件，refcount = 1
              └─ desc->vm_file = 新文件    # 描述符里替换 file
```

**引用语义要点**：新建的 shmem 文件 refcount=1，这个引用**就是给 VMA 的「所有权」**——VMA 建好后持有它，unmap 时 `fput` 归零。因此从文件创建到 VMA 建立之间**不能再 get_file**（那会变成 2，unmap 只还 1，泄漏 1）。

---

## 2. 原始 bug（605f 之前）：成功路径多 pin

旧代码 `__mmap_new_file_vma()` 无条件：

```c
vma->vm_file = get_file(map->file);   /* 无条件 +1 */
```

- 普通 file mmap：get_file 正确（fd 的引用不转移给 VMA，VMA 需要自己的引用）
- **prepare 换 file 后**：map->file 已指向新 shmem 文件（refcount=1）→ 再 get_file → 2 → unmap fput → 1 → **永久泄漏 1 个引用**（shmem 文件永不释放：匿名 inode、页表、mapping 全部悬留，且每次此类 mmap 都漏一份）

---

## 3. 605f 的修复：成功路径的「所有权不重复」

三处改动构成一个完整标志机制：

```c
struct mmap_state {
    ...
    bool file_doesnt_need_get :1;   /* ① 新标志：prepare 换过 file */
};

/* ② call_mmap_prepare 尾部：检测到 file 被换 */
if (desc->vm_file != map->file) {
    map->file_doesnt_need_get = true;
    map->file = desc->vm_file;
}

/* ③ __mmap_new_file_vma：条件化 get_file */
vma->vm_file = map->file;
if (!map->file_doesnt_need_get)
    get_file(map->file);
```

**精确性验证**：
- 换 file 才置标志（file 未换则行为与旧代码一致）✓
- swap 场景：vma->vm_file = 新文件（refcount 1）→ 不 get → unmap fput → 0 ✓ 引用守恒
- 普通场景：原逻辑 ✓
- **成功路径完整闭环**。

---

## 4. 605f 引入的新 bug：错误路径所有权无主（894f 修复回响）

### 4.1 错误路径全集推演

| 失败点 | 时间线 | swap 状态 | 新 file 引用处置 | 结果 |
|---|---|---|---|---|
| `__mmap_setup` | swap 前 | 未换 | 无需（原 file 由 sys_mmap fdput 释放） | ✓ |
| `call_mmap_prepare` 内部（action_prepare） | swap 后 | **已换** | **无主** | **泄漏（894f 修的洞）** |
| `__mmap_new_vma`（vm_area_alloc 失败） | swap 后 | **已换** | **无主** | **泄漏（syzbot 触发点）** |
| `mmap_file()`（f_op->mmap 失败） | vma 已建 | 已换 | `__mmap_new_file_vma` 内已有 fput(vma->vm_file) | ✓（605f 之前就正确） |

关键：**`__mmap_new_file_vma` 内部 mmap_file 失败早有 fput（vma->vm_file 已持有文件，错误时归回）**；605f 跳过 get_file 后，**vma 都没建出来**的失败路径（`vm_area_alloc` 失败，根本没进 __mmap_new_file_vma）里，swap 后的新文件（refcount=1）就永远无主了。

### 4.2 修复（894f99eb，2026-04-06）

```c
abort_munmap:
    /* .mmap_prepare 已换新 file，abort 时原 file 由 fdput 清理，新 file 必须手动清理 */
    if (map.file_doesnt_need_get)
        fput(map.file);
    vms_abort_munmap_vmas(&map.vms, &map.mas_detach);
    return error;
```

放在 `__mmap_region` 的三条失败路径汇聚点 `abort_munmap:`——同时覆盖 action_prepare 失败与 vma 分配失败两条泄漏路径 ✓。

**发现方式**（教科书级）：syzkaller + `failslab` 故障注入（vm_area_alloc 失败）+ kmemleak 报告——完整调用链 `mmap_zero_prepare → shmem_zero_setup_desc → __shmem_file_setup → alloc_file_pseudo` 直指泄漏对象。**验证器生态对 file 类引用泄漏的捕捉力极强**。

---

## 5. 深度审视（解构者的扩展检查）

- **vms_abort_munmap_vmas 是否重复 fput**：abort 时 vma 未入树、vm_file 未赋值（失败在 vma 建立前），vms 的 detach 列表为空 → 不会碰 map.file ✓ fput 是唯一所有权转移点
- **`mmap_action_complete` 失败路径**（`__mmap_region` 第 56-59 行 `if (error) return error;` 不经 abort_munmap）：此时 vma 已建、vm_file 已持有（refcount 1 正确）、swap 的 file 引用已在 vma 上 → 返回时 vma 由调用方统一回收（pre-existing 结构，不涉本系列泄漏）
- **MAP_PRIVATE /dev/zero（map_set_anon）是另一个世界**：它不是 file→file swap，是 file→NULL（`map_set_anon` 清 file/ops/pgoff）。此时走 is_anon 判定 → 不建 file vma → 原 file 由 fdput 释放，无文件所有权问题 ✓ 两套机制语义独立，勿混淆
- **7.3 树现状核对**：`file_doesnt_need_get`（47 行声明 / 2612 消费 / 2810 设置）与 `abort_unmap` 的 fput（2934）均在场，标志无漂移 ✓

---

## 6. 可复用教训

1. **成功/错误路径对称原则**：任何「成功路径资源所有权转移」的修复，必须同步推演「转移前失败、转移中失败、转移后失败」三档——605f 只推演了转移成功与转移中失败，漏了转移前失败（vma 分配）这一档。审查别人的 resource fix 时按此矩阵扫即可等效覆盖 syzkaller 的 kmemleak 捕获面。
2. **修复回响是最高质量教材**：一个 commit 被修（Fixes: 它）比任何文档都清楚它哪里想漏了——解构时**必须拉取 Fixes 链**。
3. **错误路径不恢复的 file 引用 vs fd 引用**：sys_mmap 的 fdput 只覆盖「原始 file」，一切 prepare 阶段的**替换产物**（新文件、新 inode、新 mapping）都在 mmap 内部责任区——错误汇聚点统一清理是正解（`abort_unmap:` + 标志判断）。
4. **Lorenzo 亲自 Ack 修复**：上游交叉验证闭环（修自己引入的 bug）——说明「已修复」区域（含其 Fixes 链）不是新矿，但**同一模式的其他 file-swap 消费者**（若存在未覆盖路径）值得一查。

---

## 附：材料清单

| 项 | 来源 |
|---|---|
| 605f6586ecf7 原 diff | kernelorg-mirror master（GitHub API, application/vnd.github.diff） |
| 894f99eb535 修复 diff | 本地 linux-hist（主线 2026-04-06 合入） |
| 7.3 现状核对 | /home/ctyun/桌面/linux-mainline/mm/vma.c（2590-2660、2790-2830、2930-2940） |
| 后续演进 c7f5ccfa5 | mm-unstable 10-19（本地 .temp/r4 存档） |

---

## 7. 后续演进（2026-10-21 补记）

**605f+894f 的标志机制已被上游整体废弃重写**：Lorenzo 于 10-13 发出的 VMA 语义系列第 4/39 号 c7f5ccfa5《fix mmap_prepare file handling, remove file_doesnt_need_get》（mm-unstable 10-19，Suren reviewed）：

- commit 自评「file_doesnt_need_get 命名糟糕且在若干情况下无法正确追踪」
- **新所有权模型**：驱动换文件时**必须自己 get_file**（替换文件 refcount=2：驱动 get 给 VMA + fd 原文件）；`map->file`（原始）与 `map->vm_file`（可被替换）双字段，`map_same_file()` 判定；归还单点化为 `map_put_vm_file()`（替换文件只在 merge/abort 处还）；`__mmap_new_file_vma` 错误路径只还自己 get 的原文件
- 顺带修掉 605f/894f 机制的一个**结构隐患**：mmap_file() 失败路径（还有 unmap_region）与 abort 汇聚点的两次 fput 对替换文件构成「连续双取」，只因唯一消费者 shmem 无 `.mmap`（恒走 `return 0` 分支）而不触发 UAF（本解构派生扫描在三线推演中独立确认了该隐患，与 c7f5 的修法方向一致）

**605f→894f→c7f5 三部曲的完整启示**：同一设计点被两次修复后仍被发现根本性缺陷而整体重写——「标志位追踪所有权」不如「创建时引用+单点归还」。补丁审查中凡见「bool 标志 + 条件 get/put」的资源对，都值得追问：转移矩阵三档失败是否都被同一代码点覆盖。

*解构栏目 · 2026-10-21*

> AI生成