# UHMAS-HL 计划

## 1. 项目定位

UHMAS-HL 是 UHMAS 的弱化版。目标是在尽量保持 UHMAS 公共接口形状的前提下，删掉原项目为了高并发、低延迟和极端回滚能力引入的大量复杂模型。

HL 指 HeapLock：所有堆内元数据并发都由一把 heap 大锁收口。项目不追求 lock-free remote free、分片锁扩展、读者 hazard gate、拆锁 VM 事务和复杂诊断矩阵。

RAW_PLAN.MD 与 RAW_TODO.MD 是原 UHMAS 的历史计划和待办，只作为参考输入。UHMAS-HL 的真实路线以本文和 TODO.MD 为准。

## 2. 保留目标

- 保留 C11 ABI 和 opaque `uhm_heap` / `uhm_thread_ctx` 类型。
- 保留 bootstrap 符号：`uhm_abi_query`、`uhm_heap_storage_query`、`uhm_open`、`uhm_get_api`。
- 保留 `uhm_api` 函数表调用方式和头文件里的 static inline 便利包装。
- 保留平台 VM 回调模型，让调用方仍可接入 Windows VirtualAlloc、POSIX mmap 或自定义 VM 后端。
- 保留 malloc 派和 mmap 派共用一个 arena 的基本模型。
- 保留页级 owner 记录，保证 malloc、mmap、control 三类页不混用。
- 保留基础统计、基础一致性检查、基础测试和默认平台包装。

## 3. HL 简化边界

- 并发模型：一把 heap lock 是唯一设计目标。旧代码里遗留的 shard lock、reader gate、lockless remote reader 等结构只允许作为兼容壳或后续删除对象存在，不再作为设计目标扩展。
- lockless 能力：`UHM_CONFIG_LOCKLESS_REMOTE_FREE`、lockless remote capability 和对应统计字段保留 ABI 名字，但在 HL 中报告不支持，运行时只走普通 heap-lock 路径。
- 碰撞处理：malloc 从低地址增长，mmap 从高地址增长。两者相遇或控制元数据需要继续侵入中间空间时，直接返回内存耗尽，不再做碰撞渗透、额外仲裁或复杂抢救。
- 控制元数据：优先使用初始控制带和已有元数据池；必要时只能按低水位申请普通 control 页。若低水位申请会碰到 mmap 高水位，则直接失败，不再做额外抢救。
- 诊断体系：保留必要的 public stats、`uhm_check` 和轻量速度测试，删除或降级 benchmark-only、treap-health、细粒度压力归因和 release/no-diagnostics 门槛路线。
- 源码组织：实现回到单个 `src/uhm.c` 翻译单元。单文件可以接受，避免原项目为大规模模块化准备的 include-only 拆分。

## 4. 非目标

- 不实现 lock-free/low-lock remote free。
- 不实现多把 shard lock 的性能路线。
- 不实现 mmap 与 malloc 的碰撞渗透或复杂回滚抢救。
- 不继续推进多翻译单元模块化。
- 不把 32 位高性能 fast path 当作当前目标；只保留 ABI 和基本可移植性。
- 不维护原 UHMAS 的完整 benchmark gate、diagnostic threshold gate 和长压测矩阵；只保留可直接看速度的轻量 benchmark。

## 5. 当前已落地状态

1. 已建立 UHMAS-HL 专用 PLAN/TODO，冻结简化边界。
2. 已将 lockless remote 能力降级为 ABI 兼容但运行时不支持。
3. 已将 shard 数量收敛到 1，并删除依赖多 shard 的旧白盒测试和诊断矩阵。
4. 已将 malloc/mmap 水位线相遇统一视为内存耗尽。
5. 已合并实现到单个 `src/uhm.c`，CMake 只把 `src/uhm.c` 作为库源文件。
6. 已收缩测试，只保留 ABI、平台 VM、基础 malloc/free、基础 mmap/munmap/mprotect/mremap 和 close/check 覆盖。
7. 已补回 UHMAS quick-compatible Release benchmark，用同名 case、同级迭代规模和系统 malloc/VM 对照查看核心路径速度；不恢复原 UHMAS 诊断矩阵和回归 gate。
