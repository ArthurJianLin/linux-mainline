# mm 子系统避坑库（已证伪 / 已撞车 / 已占领区 / 已核区域 / 观察项 / 已归档候选）

> 维护规则：每轮扫描结束，把新证伪候选、新已核区域、判重状态变化追加进本文件；挖掘其他子系统时按同样结构新建 `references/<子系统>-avoidance-list.md`。
> 完整证伪论证见各轮扫描报告（下载目录：`mm-逻辑BUG深度扫描/`、`mm第三轮扫描-性能与广域审查报告.html`）。
> 基准：Linux 7.3 正式版（torvalds c03c1a14）。

## 已撞车（上游已修，勿再挖）

- call_mmap_prepare() 文件引用泄漏 —— 上游已修
- mremap 扩张搬移错误路径泄漏 Committed_AS —— Lorenzo Stoakes b78ed86d8（2026-10-19，lore 20261019-other-mremap-charge-leak-v1，Cc stable）抢先，修法与我们逐字等价；我们的系列未发出即放弃（devel/mremap-committed-as 分支留档），selftest 亦按用户决定不再单独提交

## 已修复（自己已提交，勿再挖）

（暂无）

## 已证伪候选（16 条）

### 第一轮（7 条，2026-09-21，基于 v7.3-rc4）

| # | 候选 | 证伪理由 |
|---|---|---|
| 1 | shake_folio / hwpoison_filter 的 EXPORT_SYMBOL_GPL 可删 | tristate 模块必须导出 |
| 2 | hugetlb_wp() 锁不平衡 | API 契约：锁释放委托调用者 |
| 3 | filemap.c max_t(ssize_t,...) unsigned 下溢 | cast 后恰好起到 clamp 作用 |
| 4 | memfd_luo.c retrieve 引用计数错 | kho_restore_folio refcount=1 对称 |
| 5 | migrate_device_coherent_folio 锁未释放 | 委托 __migrate_device_finalize |
| 6 | MREMAP_DONTUNMAP locked_vm 记账泄漏 | 数学守恒 |
| 7 | do_brk_flags 不查 vma_supports_mlock | 条件恒真，省略合法 |

### 第二轮（8 条，2026-09-22，基于 v7.3-rc4）

已确认第 1 条：fork RWP marker pte_modify 绕过 COW —— vm_page_prot 本来就是 PAGE_COPY。
其余 7 条（含 mremap RWP marker pte_swp_* 相关等）完整证伪表见第二轮报告《mm逻辑BUG深度扫描报告.html》。

### 第三轮（1 条，2026-10-20，基于 7.3 正式版）

| # | 候选 | 证伪理由 |
|---|---|---|
| 16 | weighted_interleave_nid() 每次页缓存分配 O(nn_nodes) 重算总权重，可缓存进 wi_state | weight_total 依赖 per-policy nodemask（子集和 ≠ 全局和，不可全局缓存）；典型 CXL 部署仅 2 节点，O(nn) 仅 2 次加法，收益无法自证 |

## 已占领区（上游活跃重构中，勿挖，2026-10-20 第四轮判定）

- VMA 标志语义显式化 / VM_SPECIAL 消除 —— Lorenzo Stoakes 39-patch 系列（v6，20261013）：mm_managed/fixed_mapping/mm_backed/can_merge 谓词链与旧拒绝集逐位等价，26 成员差分审查全部 NFC，替代此前对 migrate_vma/khugepaged/madvise 等散点调用点的独立关注
- mremap/MREMAP_DONTUNMAP 全函数族错误路径 —— 72 小时内连落 4 修：Committed_AS（b78ed86d8）、DONTUNMAP delta（4150cd1c0）、locked_vm splitting（597e4b9 v3）、locked_vm self-merge（838bb60 v4），Lorenzo 系统性清扫中
- MAP_PRIVATE-/dev/zero 匿名化 —— Lorenzo 9-08 六补丁系列（char-mem.c 迁移 + file_is_dev_zero + core mm 统一转换）+ 合并后热修 d3ed3a4，半匿名特判全数清除

## 已核干净区域（勿重复审查）

### 第五轮新增（2026-10-20 晚，方法论：本地深挖 7.3 已合入新逻辑 + 有人跟踪则舍）

- mm/rmap TTU/uffd-wp 批量化重构（Dev Jain 10-patch，8 月合入）：全 NFC（16 逻辑点）：dispatcher 统一 set_ptes 恢复依赖 folio_unmap_pte_batch 对 anon+swapbacked 恒返 1 的硬保证；uffd marker 批量一致性链全通（x86/arm64 专用 uffd 位不在 FPB 排除表、swap_pte_batch pte_same 全值比较、get_and_clear_ptes 聚合 dirty/young）；兄弟路径 try_to_migrate_one 的 uffd/soft_dirty 搬运完整，恢复侧 migrate.c 对平
- userfaultfd RWP 全套（Kiryl Shutsemau 8-patch 新功能，Claude 辅助 + Mike R review）：PROTNONE+_PAGE_UFFD 访问陷阱机制完整闭环——WP/RWP 互斥注册、re-register -EBUSY、migration/swap 恢复均 pte_modify(PAGE_NONE) 保留 protnone 形态、SET_MODE 用 mmap_write_lock+遍历 vma_start_write 排空 per-VMA lock 在飞 fault + WRITE_ONCE 单翻转
- mm/migrate_device OOB 修复（Hui Su dc41e961a269）：唯二无边界写入已修（range/pfns 填充阶段），下游 1162/1238 遍历全有 i+j<npages 保护，兄弟 bug 无
- mm/hmm locked/unlocked API（Stanislav Kinsburskii 14-patch）：hmm_range_fault_unlocked_timeout 外层循环 9 疑点对平（deadline 丢锁重置、killable+timeout=0 无挂死、VM_FAULT_RETRY 的 HMM_FAULT_UNLOCKED 哨兵、丢锁从头重走、amdxdna msecs_to_jiffies 量纲正确（commit message 表述不精确但代码对））
- 关键判断：7.3 已合入新代码普遍「AI 辅助 + 多层 review」，纯 code-reading 审已合入系列的命中被结构性压缩；syzbot/KASAN/lockdep 类验证器路径占上游 fix 发现源 57%，但无 root 环境不可复制

### 第五轮深度跟踪地图（有人深耕则果断舍弃）

- migrate_device → Hui Su（零 PMD + OOB 两连修）；page_owner → Zhen Ni/Ye Liu；kmemleak → Breno Leitao；swap_ops → hch；madvise PMD lazyfree → Zi Yan（a8fe69c01）；vma/rmap 语义 → Lorenzo；damon → SJ Park + AI bot
- 7.3 新代码的 fix 冲刷尚未进主线（fx 指向 7.3 窗口提交的数=0）：修复潮全部滞留在 mm-unstable 评审队列（正是用户判定不碰的那批）；已合入部分是抢先验证器冲刷的窗口期，但三层审阅+AI 辅助使其质量普遍偏高

### 旧条目（第一至四轮）

- memfd_luo.c（KHO）：preserve/unpreserve/freeze/discard 全链路资源对称性（第三轮）；retrieve 引用计数（第一轮）
- 五大热路径性能（第三轮结论）：zap 堆栈、匿名缺页（do_anonymous_page/mTHP）、swap-in（swap_io_ctx 重构）、pcp 批 free（free_unref_folios）、weighted interleave —— 社区 6.8→7.3 批量化系列已覆盖到位，"低争议无 benchmark"空间近零
- VMA 谓词链差分（第四轮）：hugetlbfs 恒设 DONTEXPAND 不变式（inode.c:115）、__thp_vma_allowable_orders 全调用点、29c9a3ce6 的 17 个 VM_MAYWRITE 清除站点、912816426 的 vm_start/end 修改站点、mlock 竞态重写消费者、uffd/guard/migrate/madvise 调用点 —— 余 13 成员（usbmon/sg/hfi1/ALSA 驱动转换 + 测试）补齐后收尾

## 观察项（只记录，不抢跑提交）

- memcontrol.c:3661 slabobj_ext 记账批量化 —— 作者自留 TODO，新框架活跃迭代
- swap 批量分配的 cluster lock 合并 —— swap_io_ctx 新框架（7.2/7.3），作者跟进中
- memfd_luo.c:188 KHO preserve 的 folio 清零 —— 作者公开标注的改进邀请
- truncate 大 folio 分裂失败路径 —— Zhang Yi 系列活跃矿脉（v1 9-09 → v3 4-patch 系列），v1 已预审认可（安全丢弃区间协议 + tail_isolated 状态机 + 并发窗口封堵），v3 落地对缝后评估第五轮切入角度

## 已归档候选（第一轮 8 个 cleanup/doc；用户 2026-10-20 质量优先决定：归档不再提交，以下判重记录仅留档）

- P1: mm/page_alloc.c 无效 EXPORT_SYMBOL(__alloc_frozen_pages_noprof) —— mm-unstable 判重未撞车（2026-10-20 复验）
- P2: include/linux/pgtable.h clear_young_dirty_ptes 文档 @mm→@vma —— mm-unstable 判重未撞车（2026-10-20 复验）
- P3: mm/mremap.c vrm_stat_account 过时注释（mremap 修复已由上游 b78ed86d8 落地；本条随整组归档）
- P4: mm/shmem.c TMPFS_ATTR_W / TMPFS_ATTR_RW 死宏
- P5-P8: kernel-doc 修复（page_reporting.h、percpu、memory_hotplug、swapops.h）

## 窗口情报存档

- 2026-10-20（第四轮当日执行，较原 11 月计划提前）：7.4 窗口 mm-unstable 约 200 新提交入审——Lorenzo VMA 系列 39 patch + 新鲜修复 9 靶子 + 新靶点 3 个；分支考古：新修复住 mm-hotfixes-unstable 顶端、SHA 寿命以小时计（hotfixes 分支每日重建 + gc，引用一律记标题）；补拉三通道：kernel.org cgit 标题匹配（通窗 15–20 分钟）、lore Anubis PoW（atom 端点豁免、difficulty=4 亚秒可解）、GitHub 九月镜像 compare API（历史版本救回：truncate v1、/dev/zero 原系列；PAT 已吊销、匿名配额共享 IP 需 compare 批取）。
- 2026-10-20 晚（第五轮）：历史分析通道建成——GitCode torvalds 镜像 partial clone（`git clone --filter=tree:0 --shallow-since=2026-04-01 --bare`，44013 提交仅 34MB，pathspec 会触发 lazy fetch 改用 message 层过滤）；7.3 窗口 mm 新提交 572 条主题图谱与已跟踪区排除；低 review 覆盖筛选法（无 Reviewed/Acked + 逻辑类 → 502 条候选池待继续）。`@@@%H` 格式的 git log 切分陷阱：line[4:] 会吃掉 SHA 首字符，应取 line[3:]。

> AI生成