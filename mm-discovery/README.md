# mm 子系统 Patch 漏洞扫描与发现归档

内核 mm 子系统漏洞扫描/发现工作的完整归档，与各 patch 开发分支并行维护，便于多环境同步研究进度。

## 目录结构

```
mm-discovery/
├── reports/                      # 扫描报告与知识沉淀
│   ├── mm子系统Patch机会扫描报告.html      # 第一轮：patch 机会池建立
│   ├── mm逻辑BUG深度扫描报告.html          # 第二轮：逻辑类 bug 深扫
│   ├── mm第三轮扫描-性能与广域审查报告.html # 第三轮：性能 + 广域
│   ├── mm第四轮扫描-7.4窗口审查报告.html    # 第四轮：7.4 窗口差分 + bugfix 复审
│   ├── mremap Committed_AS泄漏问题详解与复现.html
│   ├── mm-avoidance-list.md               # 避坑清单（有人跟踪/无机会的已核条目）
│   └── repro/                             # mremap 泄漏复现工程（failslab 注入）
├── patch-deconstruct/                    # 补丁解构栏目（拆解上游代表性 commit）
│   └── 01-605f6586-mmap_prepare-swap-file.md
└── patches/                              # 已成型 patch 交付包
    ├── mremap-committed-as/              # 线① mremap 扩容失败记账泄漏
    └── map-fixed-accounting/             # 线② MAP_FIXED 替换失败记账泄漏
```

## 各 patch 开发分支（代码层）

- `devel/mremap-committed-as`：mremap 扩容失败 Committed_AS 泄漏修复（已推送）
- `devel/map-fixed-accounting`：MAP_FIXED 替换 ACCOUNT 旧映射失败泄漏修复（已推送）

## 说明

- 扫描方法论、窗口情报、判重结论见 `reports/mm-avoidance-list.md` 与各期报告
- 所有 patch 均 checkpatch 0 errors，selftest 编译验证；运行验证与发送待 root + fault-injection VM 环境
- 身份：Jianlin Shi <shijianlin11@foxmail.com>

> AI生成