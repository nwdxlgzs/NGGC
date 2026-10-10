# 项目摘要

## 仓库简介

NextGenerationGarbageCollector（NGGC）：以 C11 为核心、面向 32/64 位平台的精确追踪 GC 原型，采用可配置 Region、年龄分代、常驻写屏障与按阶段启用的读屏障。当前基于 UHMAS-HL 开发，持续验证搬迁、Pin、引用处理、故障降级和线程停车协议。

建议主题词：`garbage-collector`、`c11`、`memory-management`、`generational-gc`、`concurrency`。

## 设计选择

- 精确扫描由类型注册的同步 scanner 描述，显式 root slot 负责栈与宿主引用；不依赖语言专有对象布局。
- 对象大小、Pin 和 non-moving 属性不改变出生代别：新对象从 Young、age 0 开始，达到阈值才允许 OLD。
- header forwarding 与屏障负责搬迁期间的对象身份和字段解析；不用 page fault、signal 或 SEH 作为核心屏障。
- 线程以手动协作式 safepoint/park 进入稳定阶段，copy window 通过 resume/repark handshake 控制活动范围。
- 固定地址的 Pin 和追踪可达性分离；资源死亡通知通过 PhantomReference/PhantomQueue 协议表达，没有终接器。
- OOM、RemSet metadata overflow、worker 故障及搬迁后的不安全状态有明确降级或 fail-stop 出口。

## 实现状态

| 领域 | 当前实现 |
| --- | --- |
| 基础 collector | 非移动 mark-sweep，young evacuation，可选整区原地晋升 |
| 对象与分配 | 64-byte header、type table、分配发布、per-thread TLAB、可配置 Region、大对象 run/bin |
| 根与线程 | global/thread roots、线程注册、held safepoint、collector-owned mutator |
| 并发 | 持久 GC worker pool、受限 mutator 并发 copy；REMAP 仍要求 mutator parked |
| 屏障 | 常驻写屏障、按阶段/隔离窗口启用读屏障、canonical owner/referent 解析 |
| 引用 | Weak/Soft/PhantomReference、特殊 PhantomQueue、弱观察与事件驱动阻塞等待 |
| Pin | 嵌套计数与 owner ledger、跨 owner 拒绝、持 lease 注销门禁、pinned Young 原地保留 |
| 增量工作 | 有预算的 mark/sweep/reference packets、per-thread Assist debt、公平性与退避 |
| 策略 | SMART 等七种触发 profile、延迟反馈、metadata overflow 降级 |
| 诊断 | heap/phase/scanner verifier、故障注入、原子共享诊断与逐调用错误返回 |

PhantomQueue 的非空自保活仅适用于此特殊类型；普通队列没有该待遇。reaper 等待接口不等同于后台消费线程。soft referent 的保留/清理由当前策略控制，具体合同以公共头和长期架构为准。

## 后续方向

优先完成 root/handle/TLAB/barrier-buffer/RemSet/forwarding 的并发闭包，再推进完整并发 REMAP、并发 marking 与压缩。descriptor reservation、slot publication 和屏障移锁目前暂缓。

active read barrier 的 owner 查询仍可能线性遍历 heap blocks；部分大对象 bin 的查找最坏为线性；尚无硬实时或全平台性能承诺。跨进程共享域、NUMA、真实 UHMAS 迁移、后台 reaper、长期 pinned space 和 sanitizer 实测仍属于后续工作。
