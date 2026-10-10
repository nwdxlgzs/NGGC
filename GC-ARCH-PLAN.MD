# Next Generation Garbage Collector Architecture Plan

本文档记录 NextGenerationGarbageCollector 的长期架构计划。它不是最终公开 API 说明，而是先固定会影响全局正确性的内部协议：对象模型、类型扫描、引用语义、GC 阶段状态机、屏障、安全点、分配发布、Mutator Assist、触发策略、降级路径和验证体系。

阅读方式：本文把“现在必须遵守的正确性契约”和“未来可以替换的实现策略”分开。对象头、发布顺序、引用处理顺序、屏障语义、safepoint 协议和 OOM 降级属于硬契约；region 形状、RemSet 内部结构、SMART 策略权重、UHMAS 深集成方式属于可演进实现。

版本说明：当前公共 ABI 已推进至 `88`；ABI minor 85 的 bounded reference-processing packet 详见第 49 节，ABI minor 86 的整区原地晋升详见第 50 节，ABI minor 87 的 PhantomQueue 事件驱动阻塞等待详见第 51 节，ABI minor 88 的 scanner 扩展与 worker pool 故障闭环详见第 52 节。

当前实现状态：公共 ABI 为 `88`。当前实现仍是原型，但可靠底座已经超过最初骨架：UHMAS-HL backend、单 arena、固定 64-byte 对象头、显式 global/thread root、线程注册与协作式 safepoint、非移动 mark-sweep、严格 phase verifier、常驻写屏障、phase-or-quarantine-gated 读屏障、weak/soft/phantom reference、bounded Mutator Assist、可降级 RemSet v0、默认 64 KiB 且可按 heap 配置的非移动 Region table、region allocator lease、页对齐 tail Region decommit/recommit、ABI minor 65 非尾部 humongous run 整段 decommit/fixed-address recommit、ABI minor 66 per-heap interior decommit 阈值、ABI minor 67 tail-adjacent side-run 固定点折叠、ABI minor 68 SMART v0 阈值/滞回控制器、ABI minor 69 档内 ordered best-fit 与过滤 telemetry、ABI minor 70 可选外部内存压力/VM 延迟输入、跨 safepoint delayed from-space reclaim、ABI minor 71 frozen-type per-thread TLAB fast allocation、ABI minor 72 per-thread 自适应 TLAB sizing、ABI minor 73 per-thread RemSet card buffer、ABI minor 74 C11 TSS per-heap barrier context、ABI minor 75 锁外 deferred card append、ABI minor 76 锁外未满-buffer线性去重、ABI minor 77 锁外global-card coverage、ABI minor 78 锁外32-bit RemSet分类事件发布、ABI minor 79 第一阶段动态 Pin lease、pinned Region 计数和 pinned evacuation 降级、ABI minor 80 fail-stop quarantine 管理面、ABI minor 81 per-thread/fair bounded Mutator Assist、ABI minor 82 可解释回收触发策略与延迟反馈、ABI minor 83 collector-owned discovered-list 协议、ABI minor 84 Pin lease 归属账本、ABI minor 85 bounded reference-processing packet、ABI minor 86 可选整区原地晋升、ABI minor 87 PhantomQueue 事件驱动阻塞等待和 ABI minor 88 scanner 扩展及 worker pool 故障闭环。ABI minor 54/55/56 又加入了受限 barrier-mediated mutator-concurrent evacuation window、registered-mutator EVACUATE resume/repark 状态和 poll/park 驱动协作重停车；完整并发 REMAP 尚未开启。后续真实 UHMAS、把 RemSet descriptor reservation/slot publication继续移出 heap lock、按 epoch remark、Region RemSet、并发 marking、完整并发搬迁和长期 pinned space 整理都必须在这些已验证契约上增量演进。

## 0. 当前硬决策

- 核心实现以 C11 可移植能力为上限，不依赖单一 OS、CPU 或编译器私有能力。
- 同时兼容 32-bit 和 64-bit；32-bit 不默认 pointer tagging，不假设 64-bit atomic lock-free。
- 类型 ID 使用 `uint16_t`，`NGGC_TYPE_NONE == 0`，用户类型为 `1..65535`。
- `NGGC_TYPE_NONE` 不是语言层面的 None/null，而是 GC 内部初始化、free block、自举对象或特殊占位用编号；普通公开分配默认拒绝 type 0。
- 所有新生托管对象都以逻辑 `YOUNG, age=0` 发布；分配入口、对象大小、pin 或 non-moving 属性都不能让对象出生即 OLD。只有对象年龄达到 tenuring threshold 后才允许逻辑晋升 OLD。
- large/pinned/non-moving 是物理放置与移动能力，不是年龄代别。对象可以位于 SPECIAL/Pinned/Large 空间并保持逻辑 YOUNG，达到阈值后原地成为逻辑 OLD，而物理空间角色不变。
- 用户通过注册扫描函数描述对象引用布局；GC 不硬编码某种语言对象模型。
- 不提供 finalizer/finalization 语义；死亡通知和资源清理由 weak/soft/phantom reference 体系承担。
- 写屏障常驻；读屏障只在并发搬迁、重映射或仍存在可被 mutator 持有的 from-space 隔离壳时开启。
- 不使用 page fault、memory protection、signal、SEH 作为核心屏障机制。
- 主路径使用协作式 safepoint/handshake；系统挂起线程只可作为诊断或极端 fallback。
- Forwarding 第一阶段放对象头，方便读屏障和并发搬迁协议落地。
- Remembered Set 在 UHMAS 项目稳定前只冻结职责和抽象接口，内部结构保留替换空间。
- Release 构建必须通过宏在代码级别移除 verifier、sanitizer hook、fault injection、trace 和高成本 profiling。
- 当前开发允许牺牲一定模块解耦度换取明确的扫描语义：类型由用户注册扫描函数描述，GC 只要求扫描函数遵守 slot 契约和 phase 契约。

## 1. 目标与非目标

### 1.1 核心目标

- 高并发：mutator 和 GC worker 尽量并发运行。
- 低延迟：长暂停拆分为 safepoint、remark、remap、assist 等小预算步骤。
- 高可靠：OOM、metadata failure、RemSet overflow、worker failure、pin 过多等失败路径必须可预测。
- 可移植：核心只依赖 C11、标准整数、标准原子和可替换平台层。
- 可调试：Debug/Chaos/Sanitize 模式能强验证对象图、屏障、phase 和降级路径。
- 可演进：非移动 mark-sweep 是可靠底座，后续逐步加入分代、region、并发标记和搬迁。

### 1.2 暂缓目标

- 暂不冻结公开 API。
- 暂不把 shared domain 纳入普通 tracing heap。
- 暂不承诺硬实时 GC；只吸收 real-time GC 的预算和切片思想。
- 暂不要求第一版实现完整并发压缩；先保证非移动回收、类型扫描、屏障入口和 safepoint 可靠。
- 暂不依赖 JIT/root map；C11 场景以显式 root、handle、注册扫描函数为基础。

## 2. 推荐总体形态

长期形态：

```text
regional heap
+ generational collection
+ concurrent marking
+ optional concurrent evacuation
+ always-on write barrier
+ phase-gated read barrier
+ precise type metadata
+ explicit roots / handles
+ strict verifier and fault injection
```

堆按 region 管理。小对象优先进入线程本地 TLAB/nursery；年轻代使用 copying/evacuation；老年代使用并发标记、局部 sweep、局部 evacuation 或 mixed collection；大对象和 pinned 对象单独管理，默认不频繁移动。

第一版可靠底座仍是非移动 mark-sweep。它承担三件事：

- 在复杂功能未完成时保证对象图正确回收。
- 作为 evacuation 失败、RemSet overflow、pin 过多、metadata failure 的降级路径。
- 为 verifier、类型扫描、root 协议、分配发布协议提供最小可测试闭环。

## 3. 先进 GC 算法谱系与取舍

本项目不复刻单个现有 GC，而是吸收成熟算法中的协议。

| 算法/家族 | 核心价值 | 本项目取舍 |
|---|---|---|
| Mark-Sweep | 简单、稳定、适合 pinned/large/non-moving 区域 | 基础回收器与降级路径 |
| Mark-Compact | 降低碎片，恢复连续空间 | STW full GC 或 emergency compaction 后备能力 |
| Copying/Semi-space | 年轻代回收快，分配简单 | 用于 nursery/young region evacuation |
| Generational GC | 利用对象朝生夕死规律 | 长期默认方向，必须配合 RemSet |
| Incremental GC | 拆分长暂停 | 用于 remark、sweep、remap、assist 的预算控制 |
| Concurrent Marking | 降低标记暂停 | old/full heap 标记主方向 |
| SATB | 易验证并发标记快照 | 默认 concurrent mark 写屏障语义 |
| Incremental Update | 记录新写入边 | 作为特定策略可选项 |
| Region-based GC | 局部回收、局部搬迁、可控暂停 | 堆布局主结构 |
| G1-style Mixed Collection | 按 region 收益选择回收集合 | 吸收 region scoring 和 mixed collection 思路 |
| Immix/Line Marking | 块内按 line 管理，低元数据且可局部整理 | old region 内部布局候选 |
| Shenandoah/ZGC/C4-style Relocation | 并发搬迁、读屏障修正引用 | 吸收 phase-gated load barrier + forwarding/remap |
| Brooks Pointer/Indirection | 通过间接指针简化移动 | 不作为默认对象模型，可作为实验后端 |
| RefCount/ARC | 回收及时，适合资源和 shared block | 不作为普通 heap 主算法；用于 shared domain/native wrapper 候选 |
| Cycle Collector | 弥补 refcount 循环 | 仅 shared/refcount 子系统需要时引入 |
| Real-time/Metronome | 严格时间片和 pause budget | 吸收预算、assist、phase slicing；不承诺硬实时 |
| Epoch/Hazard/RCU | 并发 metadata 回收 | 可用于 handle table、metadata、shared domain |

结论：长期目标是 regional generational concurrent tracing GC。非移动 mark-sweep 先落地，young evacuation 和局部 old evacuation 后续逐步开启。

## 4. 正确性优先级

设计优先级固定为：

```text
memory safety
  > object graph correctness
  > bounded pause
  > throughput
  > memory footprint
  > cleverness
```

硬不变量：

- GC 不能扫描半初始化对象。
- 从 root 可达的强引用对象不能被漏标。
- 搬迁后 mutator 不能长期持有 from-space pointer。
- weak/soft/phantom referent 不能被错误强保活。
- pinned/non-movable 对象不能被搬迁。
- RemSet/barrier buffer overflow 只能降级，不能破坏正确性。
- OOM 和 metadata failure 必须返回确定错误或进入确定 emergency mode。

## 5. 对象模型

### 5.1 当前对象头

当前原型保留 64-byte fixed header，payload 64-byte 对齐。实现中的头部字段包括：

```c
typedef struct nggc_object_header {
    uint32_t magic;
    atomic_uint state;
    uint16_t type_id;
    uint16_t size_class;
    uint32_t flags;
    void *forwarding;
    size_t payload_size;
    size_t alloc_size;
    uint64_t object_id;
    uint32_t mark_epoch;
} nggc_object_header;
```

当前状态：

```text
ALLOCATING  正在初始化，不能被 GC 扫描
LIVE        已发布，可被扫描和访问
FREE        已回收，可挂入 free list
```

长期会扩展或拆分为：

```text
MARKED
EVACUATING
FORWARDED
PINNED
NON_MOVABLE
LARGE
INTERNAL
```

### 5.2 Header 与 Side Table 分工

header 只放热路径需要的数据：

- magic/state/type_id。
- 快速 flags。
- forwarding。
- payload/alloc size，直到 region/size-class table 成熟。
- mark epoch 或 mark bits。

优先放 side table/region table 的数据：

- allocation site/backtrace。
- hash、lock、identity 等低频稳定状态。
- pin owner、pin lease 调试信息。
- RemSet、card summary、region scoring。
- verifier shadow state。
- soft/weak/phantom processing metadata。

### 5.3 `GC_TYPE_NONE` 策略

`GC_TYPE_NONE` 的含义：

- free block。
- GC 内部未绑定或自举对象。
- backend/metadata 初始化过程中的临时块。
- 未来特殊 runtime object 的内部占位。

约束：

- 公共 `nggc_alloc(..., NGGC_TYPE_NONE)` 继续拒绝。
- 内部 type 0 allocation 走私有 helper，并强制 `NGGC_OBJECT_FLAG_INTERNAL`。
- live internal type 0 对象作为 leaf object 参与 mark/check，不进入用户扫描函数。
- verifier 检查 `LIVE && type_id == NGGC_TYPE_NONE` 只出现在带 `INTERNAL` 标志的对象上。
- free block 必须满足 `state == FREE && type_id == NGGC_TYPE_NONE && payload_size == 0`。

### 5.4 类型注册与冻结

类型 ID：

```text
0        = NGGC_TYPE_NONE
1..65535 = user/runtime registered type
```

类型描述：

```c
typedef void (*nggc_visit_fn)(void *ctx, void **slot);
typedef void (*nggc_scan_fn)(void *object, nggc_visit_fn visit, void *ctx);

typedef struct nggc_type_desc {
    uint32_t size;
    uint16_t type_id;
    uint16_t flags;
    size_t object_size;
    size_t alignment;
    nggc_scan_fn scan;
    const char *name;
} nggc_type_desc;
```

ABI minor 88 在描述符尾部增加可选的 `nggc_scan_fn_ex scan_ex`。扩展 scanner 的返回值和 `nggc_scan_control.poll` 用于传播显式错误与协作取消；旧 `scan` 保持 ABI 兼容，短描述符只复制其声明的前缀，未知尾部字段不会被读取。`scan_ex` 与 legacy scanner 共用 visitor/slot 契约，control、visitor 和 ctx 只能在本次调用期间使用。

扫描函数契约：

- 只枚举对象内引用 slot。
- 必须通过 `visit(ctx, slot)` 上报。
- 同一个 `scan` 函数可能被多个 GC worker 同时用于不同对象，必须可重入；不得使用未同步的共享可变状态。
- slot 集合只能由当前 owner 的类型和 payload 布局决定，不能依赖 slot 当前 referent 值；visitor 可能在扫描过程中改写已经上报的 slot。
- `visit` 和 `ctx` 只在本次同步调用期间有效，scanner 不得缓存、延迟调用或转交其他线程。
- 不重入任何 GC API；当前 coordinator 通常持有 heap lock，ABI minor 54/55/56 的 EVACUATE copy window 例外地临时释放该锁，但只允许通过 `nggc_load/store` 的受控 barrier 入口。
- 不释放、不移动、不阻塞。
- `scan_ex` 可以在有限工作片段之间调用 `control->poll(ctx)`；非 `NGGC_OK` 结果立即结束本次扫描并由当前 collector 传播。该 poll 是协作式检查点，不提供强制抢占，也不允许 scanner 借此重入 GC。
- 默认不分配；若未来允许，需要单独定义重入规则。
- 不自行追踪 weak/soft/phantom referent。
- 不上报 raw native pointer。

类型表冻结：

- 冻结后拒绝新注册。
- 已注册类型仍可分配。
- 冻结用于减少并发 GC 期间 type table 同步复杂度。

类型注销：

- 只允许在类型表未冻结时执行。
- 只允许注销没有 live object 使用的 type id。
- 注销后自动 type id 分配可以复用该 id。
- 注销失败只能返回确定错误，例如 `NGGC_ERR_BUSY`、`NGGC_ERR_TYPE_NOT_FOUND` 或 `NGGC_ERR_FROZEN`。

## 6. Slot 与引用语义

### 6.1 Slot 分类

内部 slot 语义建议扩展为：

```text
STRONG_SLOT
WEAK_SLOT
SOFT_SLOT
PHANTOM_REFERENT_SLOT
PHANTOM_QUEUE_SLOT
HANDLE_SLOT
RAW_NON_GC_POINTER
INTERIOR_POINTER_SLOT, optional and restricted
```

第一版公开扫描函数只开放 strong slot；特殊引用对象由 GC 内置类型处理。普通容器不能在扫描时临时决定“跳过某个强字段”，否则同一对象图会随上下文改变语义。

后续如果开放非强 slot，必须显式改变扫描函数签名或增加专用 visitor，例如 `visit_weak(ctx, slot)`，不能让用户在普通 `visit(ctx, slot)` 之外通过私有约定表达弱语义。

### 6.2 强引用

强引用正常参与 tracing：

```text
strong reference -> referent is kept alive
```

### 6.3 弱引用

弱引用对象本身按普通对象追踪，但 weak referent 不强保活：

```text
weak reference object is traced normally
weak referent is discovered, not marked as strong edge
if referent is unreachable after strong marking, weak slot is cleared
```

弱引用不是“上级容器选择跳过追踪”的普通字段。推荐形态是 `WeakReference` special object；它的对象本身可以被 root、数组或普通对象强引用保活，但它内部的 referent 由 reference-processing phase 读取和清理。

当前原型已经有 `WeakReference` v0：使用 `NGGC_TYPE_NONE + INTERNAL`，以内部 `size_class` 区分 weak object；weak referent 不进入普通 mark visitor，在 `REFERENCE_PROCESS` 中发现并按本轮 `mark_epoch` 判断是否清零。公开 API 暂只暴露 `nggc_weak_ref_create` / `nggc_weak_ref_get`，返回/接收普通 GC object 指针；更高层 typed wrapper 或语言绑定语义后续再定。

### 6.4 软引用

软引用对象本身按普通对象追踪，soft referent 是否保留由策略决定：

```text
low memory pressure  -> may retain soft referent
high memory pressure -> may clear soft referent
emergency mode       -> clear aggressively
```

软引用是缓存语义，不是可靠资源生命周期工具。

软引用也应是 special object，而不是任意容器上的“可有可无字段”。它的保留策略由触发器和内存压力共同决定，且必须在 weak clearing 之前完成：被策略保留的 soft referent 会重新进入标记闭包；未保留的 soft referent 才继续参与后续 weak/phantom 判断。

当前原型已经有 `SoftReference` v0：使用 `NGGC_TYPE_NONE + INTERNAL`，以内部 `size_class` 区分 soft special object；公开 API 暂只暴露 `nggc_soft_ref_create` / `nggc_soft_ref_get`。reference-processing 先按 soft policy 处理 soft，再处理 weak。当前 policy 支持 heap-pressure、always-retain、always-clear 三档；heap-pressure 阈值可通过 retain percent 配置，默认 75%。SMART 后续只能调 policy 参数，不能绕过 reference-processing 顺序。

### 6.5 虚引用与虚引用队列

虚引用不强保活 referent，且用户不能通过虚引用取回 referent。

`PhantomQueue` 是 runtime special object，只服务于虚引用，不泛化给普通 Queue/List/Map：

```text
PhantomQueue.pending_count > 0
  -> queue is treated as an internal root

ordinary queue/list/map
  -> no special retention rule
```

推荐语义：

- `PhantomReference` 是特殊对象。
- `PhantomReference.referent` 是 phantom referent，不强保活。
- marked `PhantomReference` 在 reference-processing 完成前必须保活其关联 `PhantomQueue`，否则仍存活的 phantom 会留下悬空 queue 指针。
- referent 在 strong/soft/weak 处理后仍不可达时，相关 phantom reference 入队。
- `PhantomQueue` 对队列内 `PhantomReference` 有强引用。
- 非空队列由 GC runtime internal root 保活。
- 回收线程只通过弱句柄观察队列，drain 时临时提升为强持有。
- 队列为空、外部不可达且没有 marked phantom 仍关联它时，下一轮 GC 可回收队列。

因为没有 finalizer，虚引用不会引入对象复活语义。

ABI minor 87 增加 `nggc_phantom_reaper_wait_blocking`：它只等待被观察队列或观察集状态变化，不启动 collection，也不替调用者 pop。enqueue、pop、watch/unwatch、stale weak-watch 清理和 collection 收尾都会推进事件 epoch；C11 `threads.h` 构建用现有 condition variable 阻塞并以 epoch 谓词避免丢唤醒，没有 `<threads.h>` 时保留原子等待降级。该增量不是后台 reaper worker；真正后台消费仍需另行定义回调、消费所有权、取消和 close join 协议。

虚引用队列的“队列长度非零即内部保活”只属于 `PhantomQueue`。普通队列、用户容器、工作队列都没有这种待遇；否则 GC 会把容器库语义和生命周期语义耦合到一起。

当前原型已经有 `PhantomReference` / `PhantomQueue` v0：使用 `NGGC_TYPE_NONE + INTERNAL`，以内部 `size_class` 区分 phantom reference 和 phantom queue；公开 API 暂暴露 `nggc_phantom_queue_create` / `nggc_phantom_ref_create` / `nggc_phantom_queue_count` / `nggc_phantom_queue_pop`。reference-processing 在 weak/soft 之后发现 live phantom reference，先将其关联 queue 纳入本轮标记；当 referent 仍不可达时清空 referent 并把 phantom reference 入队。非空 `PhantomQueue` 会在同一轮以及后续 GC 中作为内部 root 保活自己和队列内 phantom reference。当前支持用户手动 drain，并提供 reaper 弱观察/轮询/有界等待 v0：reaper watch 不保活空队列，poll/pop/wait 时用 queue object id 验证观察项仍指向同一个 live `PhantomQueue`，失效观察项会被清理；wait 最多触发调用者指定次数的 collection，仍无 pending queue 时返回 `NGGC_ERR_BUSY`。真正后台线程和平台级唤醒协议后续单独设计。

### 6.6 Reference Processing 顺序

引用处理必须是独立 phase，不混在普通 mark visitor 中：

```text
1. trace strong graph
2. discover weak/soft/phantom reference objects
3. evaluate soft policy
4. optionally mark retained soft referents
5. clear weak refs whose referents remain unreachable
6. enqueue phantom refs whose referents remain unreachable and non-resurrectable
7. promote non-empty PhantomQueue to internal roots
8. verify special refs did not accidentally re-mark referents
```

处理时要区分两种“存活”：

```text
reference object liveness
  -> 这个 Weak/Soft/PhantomReference 对象本身是否被强图保活

referent reachability
  -> 它指向的目标是否被 strong/retained-soft 图保活
```

reference object 不存活时，引用对象自身会被 sweep，不需要清理 referent。reference object 存活但 referent 不可达时，才执行 weak clear、soft clear 或 phantom enqueue。

## 7. 堆布局

### 7.1 推荐空间划分

```text
young / nursery regions
old regions
large object regions
pinned regions
metadata / side table
reference processing queues
phantom queues
shared domain, separate and optional
```

### 7.2 Region 元数据草案

```c
typedef struct gc_region_meta {
    void *base;
    size_t size;
    size_t used;
    uint32_t generation;
    uint32_t flags;
    uint32_t live_bytes_estimate;
    uint32_t quarantined_bytes;
    uint32_t pinned_bytes_estimate;
    uint32_t remembered_bytes_estimate;
    uint64_t allocation_epoch;
    uint64_t mark_epoch;
} gc_region_meta_t;
```

region flags：

```text
YOUNG
OLD
LARGE_OBJECT
PINNED
EVACUATING
COLLECTING
NON_MOVING
HAS_DIRTY_CARDS
HAS_QUARANTINE
```

### 7.3 小对象

- fast path 使用 TLAB bump allocation。
- TLAB refill 进入 slow path。
- 无 thread handle 的共享分配入口也必须产生 `YOUNG, age=0` 对象；它只是在共享 slow path 分配，不能视为预晋升 API。
- young GC 优先处理 nursery。
- 每个 young 对象独立记录年龄；每成功存活一次 young collection 才增加一次年龄。
- 只有对象年龄达到 tenuring threshold 后才晋升到 old，未达到阈值的对象必须留在 young survivor 空间。
- region 年龄不能替代对象年龄：同一 region 内对象的出生轮次可能不同，整区按统一年龄晋升会导致新对象提前进入 old。

### 7.4 大对象

- large threshold 随 heap 的实际 Region 尺寸派生；默认 Region 下为 64 KiB。payload 达到该尺寸或完整对象超过 Region 时标记 large。
- 大对象 region 单独管理。
- ABI minor 62 的第一阶段采用独立 large free list：small allocation 不搜索 large pool，large allocation 也不搜索 small pool；回收和拆分后的 large block 保留 pool class，避免污染小对象分配路径。
- ABI minor 63 要求 large object 从 Region 边界开始，完整 `alloc_size` 按当前 heap 的实际 Region 尺寸向上取整；START/CONTINUATION metadata 使 live/free humongous run 都可由 verifier 重建。
- ABI minor 64 将回收后的 run 按 Region 数组织到 32 个 logarithmic bins，并以 32-bit nonempty mask 跳过空档；分档使用 heap 的实际 Region 尺寸，不要求 Region 为 2 的幂。
- ABI minor 65 把默认策略下至少 8 Regions 的页对齐、非尾部空 run 整段 decommit，并以 Region side metadata 和第二组 32 档 side bins 保持可遍历、可定址 recommit；不可读页面中不保留权威 header。ABI minor 66 允许 per-heap 覆盖或禁用该阈值，ABI minor 68 在未覆盖时运行可解释 SMART v0，ABI minor 69 让 committed/decommitted 两类 bin 都提供确定性 ordered best-fit，ABI minor 70 为 SMART 增加可选宿主 pressure 与单调 VM 时钟输入。
- 大对象出生时仍是逻辑 YOUNG；独立 large/SPECIAL region 只表示不走普通 Survivor copying，不能直接赋予 OLD。
- 默认 non-moving 或低频后台移动。
- 含引用的大对象必须有精确扫描描述。
- 大对象指向 young/other region 仍需 RemSet。

### 7.5 Pinned 对象

- `PINNABLE` 是对象可以取得动态 pin lease 的能力标志；对象不会因为具有该标志而出生即 OLD 或出生即进入 pinned SPECIAL。
- 当前 API 为 `nggc_pin` / `nggc_unpin`，对象头保存 `atomic_uint pin_count`，支持嵌套 lease；heap stats 同时维护实际 pinned object 数、lease 数和错误/忙拒绝计数。
- pin 不构成 root。不可达对象即使仍有 pin lease，也必须在标记后回收，并在回收时清除对象头、Region 和 heap 级 pin 计数。
- pin 操作在 heap lock 下执行，并先关闭/flush TLAB fast gate；safepoint、phase、collector cycle、Mutator Assist、并发搬迁和 from-space quarantine 等非稳定窗口返回 `NGGC_ERR_BUSY`。
- 未 Pin 的 `PINNABLE` Young 对象仍可搬迁；取得至少一个 lease 后禁止 evacuation，继续原地增龄。达到 tenuring threshold 后仍作为 Young candidate 保留，最终 unpin 后的下一轮 collection 才允许正常搬迁和晋升。
- pinned/non-movable 对象出生时仍是逻辑 YOUNG；达到同一对象年龄阈值后可原地逻辑晋升，pin 状态和 Region 角色不因此改变。
- `NON_MOVABLE` 是物理放置属性，出生进入 `PINNED_SPECIAL` 且始终不搬迁；它与动态 `PINNABLE` lease 计数分开。Region 的 `HAS_PINNED` 同时表示实际动态 pinned 对象或 non-movable 对象，未 Pin 的普通 `PINNABLE` Young 不会误报该标志。
- forwarding source 和 target 必须为零 pin；Pinned 对象只能留在原地并参与 mark/sweep，不得进入 evacuation set。长期 pinned space、owner/超时 lease 诊断和低碎片整理仍是后续工作。
- C/FFI 优先使用 stable handle；裸指针只能在 pin lease 或 safepoint 规则允许范围内使用。

## 8. 分配协议

### 8.1 Fast Path

目标路径：

```text
thread local TLAB has enough space
  -> reserve bytes by bump pointer
  -> write header as ALLOCATING
  -> initialize payload
  -> publish header as LIVE with release store
  -> return payload pointer
```

关键规则：

- `ALLOCATING` 对象不能被 GC 扫描。
- 对象发布前，payload 必须已经完成初始化或按策略清零。
- `LIVE` 发布使用 release；扫描/读取对象状态使用 acquire。
- 分配过程中如果触发 GC，必须保证未发布对象只由当前线程私有持有。
- 如果对象在发布前需要进入 root，必须通过专门的 allocation handle，而不是裸 root。

发布前对象的所有权：

```text
ALLOCATING object
  owned by allocating thread
  not in root stack
  not in mark queue
  not in RemSet
  not visible through public handle

LIVE object
  visible to roots, handles, scanner and barriers
```

对象初始化策略必须二选一：

- 分配器清零 payload，再返回给用户初始化。
- 分配器不清零 payload，但对象在 `LIVE` 发布前必须由构造路径完整初始化，并且 verifier 能在 Debug 模式验证未初始化扫描风险。

第一版建议默认清零，后续再用类型 flag 或分配 flag 打开非清零 fast path。

### 8.2 Slow Path

slow path 负责：

- TLAB refill。
- free list/region reclaim。
- 大对象分配。
- pinned/non-movable region 选择。
- 触发 GC 或 mutator assist。
- OOM/emergency mode。

当前原型：

- 单 UHMAS-backed arena。
- bump allocation。
- free list reuse。
- free block split。
- adjacent free block coalescing。
- 分配后 `ALLOCATING -> LIVE`。
- 对象头和 payload 当前按 64-byte 对齐；未来可用 size class/region metadata 缩小头部热字段和改善空间利用率。

### 8.3 分配失败顺序

建议顺序：

```text
try TLAB
  -> try local free list / region
  -> try global free list / region
  -> run bounded mutator assist
  -> request young/full collection
  -> try emergency reclaim
  -> OOM hook or deterministic OOM
```

## 9. 屏障协议

### 9.1 常驻写屏障

写屏障始终通过 `NGGC_STORE` 或等价 runtime helper：

```c
void gc_store_ref(void *owner, void **slot, void *new_value) {
    void *old_value = *slot;

    if (gc_phase_needs_satb()) {
        gc_satb_enqueue(old_value);
    }

    *slot = new_value;

    if (gc_cross_generation_or_region(owner, new_value)) {
        gc_card_mark(owner, slot);
        gc_remset_record(owner, slot, new_value);
    }
}
```

职责：

- card marking。
- old->young / cross-region RemSet。
- concurrent mark 的 SATB 或 incremental-update 记录。
- debug 模式验证 owner/slot/value 合法性。

当前原型已记录最小 card/remset 统计，后续演进为 per-thread buffer + region summary。

### 9.2 阶段性读屏障

读屏障通过 `NGGC_LOAD` 或等价 helper，在 `EVACUATE`/`REMAP` 等阶段或 delayed-reclaim quarantine 存在期间启用：

```c
void *gc_load_ref(void **slot) {
    void *obj = atomic_or_plain_load(slot);

    if (!gc_read_barrier_active()) {
        return obj;
    }

    if (!gc_in_evacuation_region(obj)) {
        return obj;
    }

    void *new_obj = gc_forward_or_evacuate(obj);
    if (new_obj != obj) {
        gc_try_update_slot(slot, obj, new_obj);
    }
    return new_obj;
}
```

必须处理：

- `NULL`。
- 已 forwarded 对象。
- 多线程同时复制同一对象。
- pinned/non-movable 对象。
- slot CAS 失败。
- 从 forwarding acquire 读取后看到完整的新对象 body。

### 9.3 不使用页错误屏障

page-fault/mprotect/signal 类屏障不作为核心方案。原因：

- 跨平台不可控。
- 内核路径和 TLB shootdown 成本高。
- 延迟抖动明显。
- 与用户信号/异常机制冲突。
- 不符合 C11 可移植核心目标。

## 10. GC 阶段状态机

### 10.1 阶段集合

当前公开 phase：

```text
IDLE
ROOT_SNAPSHOT
MARK
REFERENCE_PROCESS
SWEEP
EVACUATE
REMAP
```

长期内部状态机建议扩展为：

```text
IDLE
REQUEST_SAFEPOINT
ROOT_SNAPSHOT
INIT_MARK
CONCURRENT_MARK
REMARK
REFERENCE_PROCESS
PLAN_RECLAIM
SWEEP
EVACUATE
REMAP
DECOMMIT
COMPLETE
EMERGENCY
OOM
```

phase 切换规则：

- 只能由 coordinator 推进 phase。
- mutator 只能通过 safepoint、barrier buffer flush、assist work 或 runtime API 报告状态，不能直接改全局 phase。
- phase 进入和退出都要递增或检查 epoch，避免旧 ack、旧 barrier buffer 或旧 root snapshot 被复用。
- phase 失败必须走显式 fail transition，例如 `EVACUATE -> EMERGENCY -> MARK/SWEEP`，不能静默跳回 `IDLE`。
- 读屏障 active bit 只允许在需要 from-space 修正的 phase 或正常 delayed-reclaim quarantine 中开启；退出 moving phase 后若隔离壳仍存在，`IDLE` 也必须保持 active，直到不同 safepoint 的回收证明完成。

当前原型把 standalone phase 和 collector cycle 分开：公开 `nggc_phase_begin/end` 仍可单独验证某一阶段；collector/assist 持有独立 cycle epoch、cycle kind 和 expected phase。低成本 verifier 始终检查 active/idle 与 transition parity、读屏障矩阵、pending safepoint、auto-collect safepoint epoch、cycle kind 和 expected phase。原型尚无 `EMERGENCY` 状态，因此失败路径会先结束可结束的 active phase，再显式 abort cycle 回到一致的 `IDLE`；未来加入并发搬迁后要把不可回滚阶段的 abort 替换成本文定义的降级图。

moving 状态机基础现已落地。collector cycle 带显式 kind：`NON_MOVING` 继续使用四阶段序列；`YOUNG_EVACUATION` 只能执行 `ROOT_SNAPSHOT -> MARK -> REFERENCE_PROCESS -> EVACUATE -> REMAP -> SWEEP`。verifier 拒绝 active cycle 没有合法 kind、non-moving cycle 进入 moving phase，以及 moving phase 乱序。公开 young collection 已在目标空间 preflight 可行时选择 moving kind；容量不足会在任何 forwarding 发布前确定并降级到非移动 young sweep。

### 10.2 推荐主流程

非移动 collection：

```text
IDLE
  -> REQUEST_SAFEPOINT
  -> ROOT_SNAPSHOT
  -> INIT_MARK
  -> CONCURRENT_MARK, optional
  -> REMARK
  -> REFERENCE_PROCESS
  -> SWEEP
  -> COMPLETE
  -> IDLE
```

移动 collection：

```text
IDLE
  -> REQUEST_SAFEPOINT
  -> ROOT_SNAPSHOT
  -> INIT_MARK / CONCURRENT_MARK
  -> REMARK
  -> REFERENCE_PROCESS
  -> PLAN_RECLAIM
  -> EVACUATE
  -> REMAP
  -> SWEEP / DECOMMIT
  -> COMPLETE
  -> IDLE
```

降级流程：

```text
any concurrent/moving phase fails
  -> disable evacuation for affected regions
  -> REQUEST_SAFEPOINT
  -> full mark-sweep
  -> emergency cleanup if needed
  -> deterministic OOM if still failed
```

### 10.3 Phase 不变量

`IDLE`：

- 读屏障关闭。
- 写屏障仍开启。
- 没有 from-space pointer 需要修正。

`ROOT_SNAPSHOT`：

- 必要线程完成 safepoint handshake 或发布 root。
- TLAB 边界可被 GC 识别。
- barrier buffer 已 flush 或被纳入 snapshot。

`MARK/CONCURRENT_MARK`：

- tri-color invariant 必须成立。
- 写屏障维护 SATB 或 update 语义。
- mark stack overflow 必须降级到 overflow-safe marking 或 full scan。

`REFERENCE_PROCESS`：

- strong graph 已确定。
- weak/soft/phantom 按固定顺序处理。
- PhantomQueue internal root 只对非空队列生效。
- 不允许普通扫描函数在此阶段重新解释对象布局。
- weak/soft/phantom 处理错误只能中止本轮 collection 或降级，不能把特殊 referent 临时当 strong edge。

`EVACUATE`：

- 读屏障开启。
- forwarding 协议有效。
- pinned/non-movable 对象不搬迁。

`REMAP`：

- 读屏障开启。
- roots、handles、dirty cards、RemSet 被扫描修正。
- verifier 确认无 from-space pointer 后才可关闭读屏障。

`SWEEP`：

- 未标记 live 对象转 free。
- free block 可合并。
- metadata/stats 必须同步更新。

## 11. 协作式安全点与线程状态

主路径使用手动 safepoint/handshake。

建议线程状态：

```text
RUNNING
SAFEPOINT_REQUESTED
AT_SAFEPOINT
BLOCKED_NO_GC_ROOTS
BLOCKED_WITH_PUBLISHED_ROOTS
IN_NATIVE
UNREGISTERED
```

safepoint 放置点：

- 分配 slow path。
- TLAB refill。
- runtime API 边界。
- 循环回边。
- 阻塞前后。
- native/FFI 进入和退出。
- 显式 `GC_SAFEPOINT()`。

phase 切换：

```text
1. coordinator 发布 epoch/phase。
2. mutator 在 poll 点观察请求。
3. mutator flush local barrier/remset buffers。
4. mutator 发布 roots、TLAB 状态和线程状态。
5. coordinator 等待必要线程 ack。
6. phase transition 完成。
```

系统挂起线程不作为主路径，因为它无法可靠证明 root、TLAB、barrier buffer 和未知锁状态一致。

## 12. Mutator Assist

Mutator Assist 的目的不是惩罚分配线程，而是在 GC 落后时把少量可中断工作摊还给造成压力的 mutator。

### 12.1 债务模型

推荐指标：

```text
global_assist_debt
thread_assist_debt
allocated_bytes_since_last_assist
gc_backlog_bytes
mark_backlog_items
dirty_card_backlog
evacuation_backlog_bytes
```

分配时增加 debt：

```text
thread_debt += allocated_bytes * assist_ratio
global_debt += allocated_bytes * global_pressure_ratio
```

GC worker 完成工作时减少 global debt；mutator 完成 assist 时减少自己的 debt。

当前原型已落地统计骨架和 opt-in bounded work packet：分配会按字节增加 heap debt，并在有注册 owner 时增加对应 thread debt；collection/assist 按扫描工作量偿还 debt，优先偿还当前 actor，再处理其他注册线程，最后把无 owner 剩余归入 heap debt。assist 的 mark stack、状态和 sweep cursor 跨分配失败持久化；ABI minor 81 根据最大 heap/actor debt 与当前分配请求，在 MARK 使用 8/16/24/32 items、SWEEP 使用 1024/2048/4096/8192 bytes 四档预算。root snapshot 和 reference processing 当前仍为原子步骤。

### 12.2 Assist 预算

assist 必须有硬上限：

```text
max_assist_time_ns
max_mark_items
max_dirty_cards
max_sweep_bytes
max_evacuation_bytes
```

如果超过预算仍追不上，只能反馈给触发策略或进入更强 GC 模式，不能无限延长当前分配路径。

ABI minor 81 的实现把动态预算限制为四个固定档位，避免策略调节产生不可验证的任意上限：`(8 items, 1024 bytes)`、`(16, 2048)`、`(24, 4096)` 和 `(32, 8192)`。债务达到约 4 KiB、16 KiB、64 KiB 时逐级提高档位；公平性 cap 会把连续 packet 数限制为 4，在其他注册线程仍有债务时强制当前 packet 回到最低档。所有预算都使用 `size_t`/32-bit 控制字段，正确性不依赖 64-bit lock-free atomic。

### 12.3 Assist 工作优先级

从低风险到高风险：

```text
flush local buffers
  -> scan bounded dirty cards
  -> mark bounded objects
  -> sweep bounded free-list chunk
  -> evacuate bounded object, only when read barrier and forwarding are stable
```

搬迁类 assist 最晚启用。

### 12.4 Assist 退避与公平性

- 单个 mutator 的 assist 不能无限扩大；达到预算后返回 slow path 决策。
- 长期高分配线程可以承担更多 assist，但不能饿死低分配线程的 safepoint 响应。
- 如果所有 mutator assist 仍追不上 allocation rate，策略层必须提前启动更强 collection，而不是把延迟藏在分配路径里。
- assist work 只能执行当前 phase 明确允许的任务；例如 `MARK` 期间不能执行 remap，`SWEEP` 期间不能临时搬迁对象。

当前 ABI minor 81 已实现第一版可验证公平性：每个注册线程维护 no-progress streak；连续两次无进展后进入两个 packet 的短退避，退避只跳过本次 allocation-failure assist，不改变债务或 collector phase。进展会清零 streak/backoff。heap 与 thread stats 分别暴露 packet、progress、no-progress、dynamic-budget、fairness-cap、backoff 和 pending/max debt。该逻辑仍在 heap lock 内，属于调度与可观测性增量，不是并发 worker 或无锁队列。

## 13. 回收触发策略

支持多策略：

```text
GC_TRIGGER_MANUAL
GC_TRIGGER_HEAP_LIMIT
GC_TRIGGER_ALLOCATION_RATE
GC_TRIGGER_LATENCY_TARGET
GC_TRIGGER_THROUGHPUT
GC_TRIGGER_MEMORY_SAVING
GC_TRIGGER_SMART
```

默认 `SMART`，但必须可解释、可替换。

策略选择原则：

- 手动策略必须完全可用，便于测试和嵌入式场景。
- heap-limit 策略必须简单确定，作为 SMART 失效时的 fallback。
- latency/throughput/memory-saving 策略是预设 profile，本质是 SMART 权重不同。
- SMART 不允许直接改底层数据结构，只能向 coordinator 请求合法 phase 或调整软参数。

### 13.1 SMART 输入

- heap used / soft limit。
- allocation rate。
- young survival rate。
- old growth rate。
- large object pressure。
- pinned pressure。
- RemSet/dirty card pressure。
- recent pause p50/p95/p99。
- GC worker throughput。
- mutator assist debt。
- fragmentation。
- system memory pressure。
- user latency/throughput preference。

### 13.2 SMART v0 评分

```text
heap_pressure        = used_bytes / soft_limit
allocation_pressure  = allocation_rate / expected_reclaim_rate
young_pressure       = young_used / young_capacity
old_pressure         = old_growth_rate / old_budget
large_pressure       = large_used / large_capacity
pinned_pressure      = pinned_bytes / movable_bytes
remset_pressure      = dirty_cards_or_remset_bytes / heap_bytes
latency_pressure     = recent_pause_p95 / target_pause
assist_pressure      = global_assist_debt / debt_budget
fragmentation        = free_bytes_unusable / free_bytes_total
```

决策示例：

```text
young_pressure high and remset acceptable
  -> young collection

old_pressure or heap_pressure high
  -> concurrent mark

fragmentation high and pinned_pressure low
  -> select evacuation

pinned_pressure high
  -> avoid evacuation, prefer mark-sweep

memory critical
  -> clear soft references, emergency mode, full GC
```

策略层只能输出命令，不能直接修改对象头、线程状态或 RemSet。

### 13.3 SMART v1 控制器形态

SMART 不需要一开始就机器学习；更可靠的路线是“可解释评分 + 滞回 + 影子评估”：

```text
observe metrics
  -> normalize pressure scores
  -> apply profile weights
  -> choose candidate action
  -> apply hysteresis / cooldown
  -> ask coordinator to execute
  -> compare predicted reclaim/pause with actual result
  -> update moving averages
```

候选动作：

```text
do nothing
request young collection
request concurrent mark
request mixed collection
request full mark-sweep
request evacuation for selected regions
increase/decrease assist ratio
clear soft references more aggressively
enter emergency mode
```

保护栏：

- 不因短时 allocation spike 频繁 full GC。
- 不在 pinned pressure 高时选择大规模搬迁。
- 不在 RemSet 不可信时执行依赖精确 RemSet 的 young/mixed collection。
- 不在最近 pause 超标后继续扩大 STW 工作。
- 预测错误要降低策略自信度并回退到 heap-limit/full mark-sweep 这种确定路径。

## 14. Remembered Set 预留设计

RemSet 在 UHMAS 项目稳定前不冻结内部结构，只冻结职责、barrier 入口和降级语义。也就是说，未来可以把全局 card table 换成 region-local bitmap、slot log、Bloom summary、UHMAS side metadata 或混合结构，但不能改变“写屏障记录跨代/跨 region 引用，collection 能从记录恢复闭包”的契约。

RemSet 职责：

- old->young 引用追踪。
- cross-region 引用追踪。
- dirty card 记录。
- per-thread barrier buffer。
- region remembered set 查询。
- region 清理和重建。

预留接口方向：

```c
void gc_remset_record_write(void *obj, void **slot, void *value);
void gc_remset_flush_thread(void *thread);
void gc_remset_scan_region(void *region);
void gc_remset_clear_region(void *region);
```

第一版结构：

```text
global card table
  one byte or bit per card

per-thread dirty buffer
  store barrier appends dirty card / slot summary

region summary
  region -> maybe references young / evacuation set
```

Overflow 降级：

```text
per-thread buffer full
  -> flush to global

global remset pressure high
  -> mark whole region dirty

region remset overflow
  -> scan whole source region

metadata allocation fails
  -> disable evacuation, fall back to mark-sweep/full scan
```

card size 建议从 256B、512B、1KB 基准测试。

正确性要求：

- 漏记 cross-region/old->young 引用是 correctness bug。
- 多记 dirty card 只是性能问题。
- RemSet 不可信时必须能切换到 whole-region scan 或 full-heap scan。
- 写屏障更新 slot 和记录 RemSet 的顺序要与所选并发标记语义一致；SATB 需要先记录 old value，incremental update 需要记录 new edge。

## 15. 搬迁与 Forwarding

第一阶段 forwarding 放在对象头中。原因是读屏障能用一次 header 访问完成判断和转发，不需要先查 side table；这对 C11 portable 原型最简单。未来如果对象头需要压缩，才考虑 region-local forwarding table 或 Brooks-style indirection。

### 15.1 协议

```text
1. region selected into evacuation set
2. read barrier sees object in evacuating region
3. mutator or worker copies object to target region
4. copied object body is fully initialized
5. release-CAS old.header.forwarding from NULL to new_object
6. CAS winner owns the copy
7. CAS loser discards its copy and uses existing forwarding
8. read barrier returns new object
9. remap/fixup removes old references
```

### 15.2 发布顺序

```text
copy header skeleton
copy body
fix local metadata
publish new object as LIVE
release-CAS old.forwarding
set old state FORWARDED
```

读屏障读取 forwarding 使用 acquire。

约束：

- old object 的 forwarding 从 `NULL` 到 `new_object` 只能成功一次。
- CAS 失败的一方不能泄漏临时 copy；必须安全丢弃或放回 allocation buffer。
- forwarded old object 不能再被普通扫描当作可移动 live object 处理。
- 32-bit 平台不假设双字 CAS；对象状态和 forwarding 的组合协议必须能用单指针 CAS 或锁降级表达。
- debug 模式必须能检查“同一 old object 只有一个 canonical new object”。

### 15.3 Remap

remap/fixup 扫描：

- roots。
- handles。
- dirty cards。
- evacuation set 的反向 RemSet。
- old->young / cross-region RemSet。
- 必要时全 region fallback scan。

只有 verifier 确认无 from-space pointer 后，才能关闭读屏障并回收 from-space。

## 16. C11 内存模型原则

- phase/epoch 发布使用 release，读取使用 acquire。
- `ALLOCATING -> LIVE` 发布必须保证 payload 初始化先行可见。
- forwarding 发布必须保证新对象 body 先行可见。
- mark/state 竞争更新使用 atomic RMW/CAS。
- thread-local barrier buffer 写入可 relaxed，flush 到全局队列要有 release/acquire 边界。
- 跨线程发布对象引用必须通过 release store 或等价同步。
- 不默认使用 `memory_order_seq_cst`；除非调试模式或确有全局顺序需求。
- 不使用未对齐 atomic。
- 不假设 32-bit 平台上 64-bit atomic lock-free。

## 17. 32-bit / 64-bit 与平台层

- 对象大小、偏移、arena 尺寸用 `size_t` 或明确宽度整数。
- 指针转整数只用 `uintptr_t`，仅用于比较、哈希、地址查表等受控场景。
- 不默认高位 pointer tagging。
- alignment 通过 C11 对齐规则和平台层确认。
- region table 查询不能依赖巨大稀疏地址空间；32-bit 下需要紧凑表或分层表。
- card table、heap limit、大对象阈值要为 32-bit 单独设上限。

核心只依赖：

```text
stdatomic.h
stdint.h
stddef.h
stdalign.h
threads.h optional
```

平台层可以提供 VM reserve/commit/decommit、high precision clock、thread naming、NUMA、huge page、prefetch、sanitizer hook、crash dump，但这些都必须可关闭且不影响核心正确性。

## 18. OOM 与降级策略

### 18.1 降级阶梯

```text
concurrent GC falls behind
  -> increase mutator assist

evacuation fails
  -> mark affected region non-moving, use mark-sweep

remset overflow
  -> scan whole source region / full heap fallback

memory pressure continues
  -> STW full mark-sweep

full mark still not enough
  -> clear soft refs, sweep/decommit empty regions

still not enough
  -> OOM hook or deterministic OOM

metadata allocation fails
  -> fail-fast or predefined emergency mode
```

Emergency mode 的目标是“保正确性、少承诺性能”：

```text
disable optional evacuation
disable non-essential profiling
force whole-region/full-heap scan where metadata is untrusted
clear soft references aggressively
prefer non-moving mark-sweep
decommit empty regions only after verifier-safe point
```

Emergency mode 不能做的事：

- 忽略 write barrier 记录。
- 跳过 root 或 handle 扫描。
- 把 weak/soft/phantom referent 临时当 strong edge 保活。
- 在 metadata 已知损坏时继续执行依赖该 metadata 的移动回收。

### 18.2 必须覆盖的失败场景

- mark stack overflow。
- RemSet overflow。
- barrier buffer overflow。
- TLAB refill 失败。
- large object 分配失败。
- evacuation target 分配失败。
- pinned 对象过多导致无法压缩。
- GC worker 创建失败。
- 类型注册表满或冻结。
- `GC_TYPE_NONE` 意外进入普通扫描。
- shared domain metadata 损坏。
- verifier 发现对象图不一致。

### 18.3 OOM Hook

OOM hook 限制：

```text
before_oom:
  receives immutable snapshot stats
  may request soft-reference clearing
  may request one full collection retry
  may not allocate from the same heap unless explicitly documented

after_oom:
  deterministic error returned to caller
```

hook 失败或违反约束时，GC 必须 fail-fast 或返回 OOM，不能进入未定义状态。

### 18.4 确定性返回

公开分配 API 未来必须能区分：

```text
invalid argument
type table closed/full
heap closed
corruption detected
backend failure
out of memory after retry
unsupported operation in current phase
```

Debug/Chaos 模式可以 fail-fast；Release 默认应返回确定错误码或空指针并保存 last error，不允许半初始化对象泄漏到 heap。

## 19. 调试、验证、故障注入与性能监测

已有/预留宏：

```c
#if NGGC_ENABLE_VERIFY
#endif

#if NGGC_ENABLE_ASAN_HOOKS
#endif

#if NGGC_ENABLE_TSAN_HOOKS
#endif

#if NGGC_ENABLE_FAULT_INJECTION
#endif

#if NGGC_ENABLE_PROFILING
#endif

#if NGGC_ENABLE_TRACE_LOG
#endif
```

Release 要求：

- 不留 verifier 热路径分支。
- 不留 fault injection 代码。
- 不留高成本 profiling。
- 不因诊断设施影响正式性能。
- 通过 `#if` 在代码级别移除，而不是只在运行时关闭。

### 19.1 Verifier

逐步加入：

- heap verifier。
- region verifier。
- object header verifier。
- type descriptor verifier。
- root stack verifier。
- thread/TLAB verifier。
- RemSet/card verifier。
- forwarding verifier。
- weak/soft/phantom verifier。
- PhantomQueue verifier。
- pin/unpin balance checker。
- barrier correctness checker。
- phase transition checker。
- from-space pointer checker。

### 19.2 Sanitizer Hook

- 分配后 unpoison。
- sweep 后 poison。
- evacuation 后检测旧对象访问。
- TSAN 下标注同步边界或减少误报。

### 19.3 Fault Injection

- 第 N 次分配失败。
- backend create/map/unmap/stats 失败。
- mark stack push 失败。
- RemSet buffer 分配失败。
- evacuation target 分配失败。
- worker 创建失败。
- 随机 safepoint 压力。
- 随机 pin 泄漏模拟。
- 随机 barrier buffer overflow。

### 19.4 性能统计

- allocation rate。
- TLAB refill count。
- GC cycle time。
- per-phase time。
- pause distribution。
- mutator assist time。
- mark/sweep/evacuation throughput。
- dirty card count。
- RemSet bytes。
- pinned ratio。
- soft/weak clear count。
- phantom enqueue count。
- read/write barrier count。

### 19.5 构建矩阵

```text
Release:
  no verifier, no fault injection, no profiling unless explicitly enabled

RelWithStats:
  low-cost counters only

DebugVerify:
  heap/phase/barrier verifier

DebugSanitize:
  ASAN/TSAN hooks, poison/unpoison, extra synchronization annotations

DebugChaos:
  fault injection, randomized safepoints, randomized allocation failures
```

### 19.6 验证等级

```text
level 0: no verifier, Release 默认
level 1: cheap header/type/root checks
level 2: full heap walk, scan callback validation
level 3: phase invariant + barrier/remset validation
level 4: chaos mode, randomized failures and safepoint pressure
```

性能监测也分层：

- Release 只保留低成本累计计数或完全关闭。
- Profile 可以保留 phase 时间、allocation rate、barrier count。
- Debug/Chaos 可以保留昂贵 trace、对象级事件和 shadow metadata。

## 20. 多线程内存池管理

推荐结构：

```text
per-thread:
  TLAB
  local root stack
  local barrier buffer
  local remembered buffer
  allocation stats
  assist debt

global:
  region table
  type table
  old generation
  large object space
  pinned space
  phase coordinator
  marking queues
  worker pool
```

原则：

- fast path 线程本地。
- slow path 才进入全局同步。
- marking queue 支持 work stealing。
- region 可临时归属某线程以减少锁竞争。
- 线程注册/注销必须 flush buffer、retire TLAB、处理 root 和 pin。

线程注销协议：

```text
poll pending safepoint
publish or discard local roots
flush local barrier/remset buffers
return or retire TLAB
release pin leases or report leak in debug
detach from thread registry
advance epoch if coordinator is waiting
```

## 21. 多进程共享域

shared domain 暂不作为普通 tracing heap：

```text
process-local GC heap:
  tracing, generational, regional, optional moving

shared domain:
  immutable block
  refcounted block
  handle / offset pointer graph
  no raw process-local pointer
```

第一版只允许：

- 不含进程本地裸指针的 immutable block。
- 显式 refcounted block。
- offset pointer。
- stable shared handle。
- process-local GC 把 shared handle 视为外部资源。

禁止：

- shared block 直接指向 process-local GC object。
- process-local GC object 被其他进程裸指针引用。
- 跨进程 tracing root。
- 未处理 robust 语义的共享锁。

## 22. API 边界原则

公开 API 暂不冻结，但内部协议隐含这些能力：

- heap open/close。
- type register/freeze。
- thread register/unregister。
- root push/pop 或 handle。
- allocation。
- `NGGC_LOAD` / `NGGC_STORE`。
- pin/unpin。
- native/blocking enter/leave。
- safepoint。
- collect/check/stats。
- strategy config。
- OOM hook。
- verifier entry。

API 设计必须服务内部一致性，不能让用户绕过屏障、root、type scan 或 phase 约束。

## 23. 当前原型对齐

已落地：

- CMake C11 项目。
- UHMAS-HL backend 隔离在 `nggc_backend_*`，并有 backend contract。
- `NGGC_ABI_MINOR == 84`。
- 诊断宏矩阵入口。
- `uint16_t` type id，`NGGC_TYPE_NONE == 0`。
- 私有 `GC_TYPE_NONE` 内部对象分配 helper；公开分配仍拒绝 type 0。
- 用户注册扫描函数。
- 基础 scanner slot 校验：slot 必须位于当前 owner payload 内、满足指针对齐，且值为 null 或合法 live object；禁止借普通 scanner 越界枚举其他对象或特殊引用 referent。
- Debug/Verify scanner replay：保存 payload 快照，双次执行 scanner，按顺序无关的 slot 集合校验重复项、集合稳定性、引用值稳定性和 scanner 副作用；默认 Release 通过 `NGGC_ENABLE_VERIFY` 移除重放代码。
- 类型表冻结和安全类型注销。
- 固定 64-byte 对象头。
- object flags：internal、pinnable、non-movable、large。
- `ALLOCATING -> LIVE` 发布。
- 单 arena bump allocation。
- free list reuse、split、coalesce。
- 显式 heap-level root stack。
- root/thread registry verifier：基础 root storage/count/capacity、slot 对齐和值校验；Debug/Verify 重复 root、thread 链表环、节点 magic/owner/state、registry 计数和 pending safepoint epoch 重建校验。
- reference-processing verifier：复查 marked soft/weak referent 的 mark epoch、marked phantom 到 queue/referent 的闭包，以及 enqueued phantom 的 referent 清空、queue owner、唯一成员、链表计数和本轮 mark 状态。
- phase transition verifier 与 collector cycle：校验 phase/read-barrier/transition parity/safepoint epoch，并强制当前非移动 collector 的四阶段 expected-phase 顺序和失败 abort。
- 非移动 mark-sweep。
- mark epoch。
- `NGGC_LOAD` / `NGGC_STORE`。
- 最小 card table / RemSet 统计。
- Mutator Assist debt 统计；分配计债，成功 collection 按扫描工作量偿还 debt。
- opt-in bounded Mutator Assist：`NGGC_CONFIG_MUTATOR_ASSIST_RECLAIM` 允许分配失败路径持续推进持久 MARK/SWEEP packet，并导出 state/backlog/progress/work 统计。
- 线程注册/注销。
- safepoint epoch/poll/ack/wait。
- `NGGC_CONFIG_COLLECT_AUTO_SAFEPOINT`。
- phase skeleton：root snapshot、mark、reference process、sweep、evacuate、remap。
- reference-processing phase 和统计字段；当前已处理内部 weak/soft/phantom reference。
- WeakReference v0：`NGGC_TYPE_NONE + INTERNAL + size_class` 区分 weak special object，公开 `nggc_weak_ref_create` / `nggc_weak_ref_get`，reference-processing phase 能发现 live weak reference 并清理不可达 referent。
- SoftReference v0：`NGGC_TYPE_NONE + INTERNAL + size_class` 区分 soft special object，公开 `nggc_soft_ref_create` / `nggc_soft_ref_get`，reference-processing phase 先按可配置 soft policy 保留或清理 soft referent。
- PhantomReference/PhantomQueue v0：`NGGC_TYPE_NONE + INTERNAL + size_class` 区分 phantom reference 和 phantom queue，公开 queue/ref create、count、pop 手动 drain API，并提供 reaper weak-watch/poll/wait/pop v0；reference-processing phase 能入队不可达 referent 对应的 phantom reference，并保活非空 queue。
- weak reference discovered/cleared、soft reference discovered/retained/cleared、phantom reference discovered/enqueued 和 phantom queue promoted 统计字段。
- ABI minor 83 discovered-list v0：collector-owned 的 Soft/Weak/Phantom intrusive list，各自携带本轮 mark epoch；`ACTIVE -> DRAINING -> IDLE` 状态转换由现有 heap lock 保护，处理次序为 Soft 闭包、Weak、PhantomReference、PhantomQueue。列表在成功处理、collection abort、Assist reset 和下轮 mark epoch 前清理；已入队 PhantomReference 不重复发现，并导出 pending list stats 与常驻一致性校验。
- EVACUATE/REMAP 阶段读屏障 active 统计，并在 active load 时跟随 live object header forwarding。
- REMAP phase 结束时的最小 strong slot fixup：扫描 root slot 和用户 scan callback 上报的 strong slot，将 forwarded slot 修正到 forwarding 目标；`nggc_check` 验证 live forwarding 目标合法，并拒绝残留指向 forwarded object 的 root/strong slot。
- backend fault injection tests。

仍待实现或继续深化：

- 深化 ABI minor 71/72 per-thread TLAB：自适应 span sizing 已落地；ABI minor 73/74 已增加per-thread barrier/RemSet buffer、局部flush与TSS owner lookup，ABI minor 75/76/77已把未满buffer的预留card append、线性去重和global-card coverage读取移出heap lock，ABI minor 78又把C11 owner分类共享64-bit telemetry和Region/generation分类推导改为锁外32-bit事件。后续继续拆出reservation/slot publication，并减少stats/普通slow allocation对heap-level fast gate的干扰。refill/retire与sizing决策仍由持锁slow path管理。
- concurrent marking worker。
- bounded Mutator Assist 的 per-thread debt、公平性/退避、动态预算已由 ABI minor 81 落地；ABI minor 85 又为 Assist 增加了 bounded reference-processing packet，但并发 worker 或无锁 work queue 仍待后续。
- 可选后台 PhantomQueue reaper worker 或平台级阻塞唤醒接口。
- 根据 ABI minor 64/65/67/68/69/70 的 search/probe/source/reject、committed/decommitted run、recommit、tail-collapse、外部 pressure 与 VM latency telemetry，决定是否把 ABI 69 ordered list 深化为 tree/placement 二级索引，并增加内建平台 provider、延迟阈值校准与可选低频 compaction；确定性 best-fit、tail-adjacent hole 折叠、阈值滞回和可插拔输入安全壳已落地。
- 完整 pinned region/space、pin owner/token、超时/线程退出诊断和长期 pinned space 整理；ABI minor 79 的动态计数 lease 已落地，但不覆盖这些能力。
- region/young-old 精细 RemSet；当前 whole-heap precise-card/coarse-heap v0 已落地。
- young/old 对象年龄语义、STW young evacuation、post-forwarding emergency completion、单指针 forwarding CAS、32 位 evacuation epoch、ABI minor 49 精确任务表、ABI minor 50 持久 worker pool、ABI minor 51 parked-safepoint 并行 REMAP、ABI minor 52 跨 safepoint delayed reclaim、ABI minor 54 受限 barrier-mediated copy window、ABI minor 55 registered-mutator EVACUATE resume/repark 状态骨架和 ABI minor 56 poll/park 驱动协作 repark handshake 已落地；继续实现并发 REMAP、root/handle/RemSet 闭包和完整并发搬迁。
- SMART trigger。
- sanitizer hook、fault injection 扩展、profiling。

## 24. 原型路线

### Phase 0: 可靠底座

- type table。
- object header。
- explicit roots。
- scan callbacks。
- non-moving mark-sweep。
- verifier。
- backend isolation。

### Phase 1: 分配器与基本并发骨架

- free list split/coalesce。
- thread register。
- safepoint handshake。
- phase coordinator。
- object flags。
- type table freeze。

### Phase 2: 分代与 RemSet

- region table。
- TLAB。
- young/old generation。
- card table + per-thread dirty buffer。
- minor GC。

### Phase 3: 并发标记与 Assist

- concurrent marking worker。
- SATB barrier。
- bounded remark。
- mutator assist debt。
- worker pool。

### Phase 4: 引用处理

- Internal WeakReference prototype。
- Public WeakReference API。
- Internal SoftReference prototype。
- Public SoftReference API and configurable soft policy。
- Internal PhantomReference/PhantomQueue prototype。
- Public PhantomReference/PhantomQueue API and manual drain。
- PhantomQueue weak-watch reaper protocol。
- PhantomQueue background worker or blocking wait protocol。
- reference-processing verifier。

### Phase 5: 大对象、Pin、OOM

- large object space。
- pinned region。
- pin/unpin lease。
- emergency mode；ABI minor 80 增加 fail-stop 状态查询与 force-destroy 管理面。
- OOM hook。
- fault injection 扩展。

### Phase 6: 搬迁与读屏障

- evacuation set。
- forwarding CAS。
- phase-gated read barrier。
- remap/fixup。
- from-space pointer verifier。

### Phase 7: 智能策略与 UHMAS 深集成

- SMART trigger。
- adaptive young size。
- adaptive assist。
- region scoring。
- UHMAS-backed RemSet/region metadata。
- decommit/recommit/NUMA/huge page 优化。

## 25. 补充设计闭环

本节把前面讨论过但容易在实现时散掉的细节再收束一次。它不是公开 API 冻结，而是给后续代码落地时用的内部检查表。

### 25.1 系统分层

建议把 GC 拆成四层，每层只向上一层暴露稳定语义：

```text
backend / UHMAS layer
  reserve/commit/decommit/map/unmap/stats/fault injection

heap layout layer
  arena/region/TLAB/free-list/large/pinned/shared-domain metadata

collector protocol layer
  phase coordinator/safepoint/barrier/mark/sweep/evacuate/remap/reference processing

policy layer
  trigger/SMART/soft-reference policy/OOM hook/profile tuning
```

约束：

- backend 只能提供内存操作和平台能力，不能决定对象图语义。
- heap layout 可以换 region 形状、card table 或 UHMAS side metadata，但不能绕过 barrier 和 phase。
- collector protocol 层负责正确性，policy 层只能提出合法请求。
- public API 最终只应该调用这些稳定协议，不能让用户直接修改对象头、phase、RemSet 或 root registry。

### 25.2 GC Header 演进规则

当前固定 64-byte header 是原型期的正确选择。后续压缩 header 时要按热度拆分：

```text
hot header:
  magic/debug optional
  state
  type_id
  flags
  mark_epoch/mark_bits
  forwarding

warm side metadata:
  payload_size / alloc_size if size class is enough
  object_id
  pin lease
  allocation site
  verifier shadow state

cold region metadata:
  region id
  generation
  remembered summary
  evacuation score
  live bytes estimate
```

必须保留的语义：

- `type_id == NGGC_TYPE_NONE` 只代表 GC 内部 NONE，不代表语言层 null/none。
- 用户类型空间是 `1..65535`，耗尽时返回确定错误。
- public allocation 默认拒绝 type 0。
- forwarding 第一阶段在 header；只有读屏障和搬迁协议稳定后，才考虑 side forwarding table。
- 32-bit 不假设 header 字段能用双字 CAS 一次性发布，复杂状态必须拆成单指针 CAS、锁或 phase 降级。

### 25.3 类型扫描闭包

用户注册扫描函数是这个项目的语言无关核心，但扫描函数只能描述“布局”，不能描述“生命周期策略”。

扫描函数硬契约：

- 只通过 visitor 上报 GC object slot。
- 第一阶段 visitor 只代表 strong edge。
- 不在扫描函数里判断弱、软、虚引用语义。
- 不访问未发布对象，不缓存 visitor，不跨 phase 保存 ctx。
- 不执行阻塞、递归 collection、任意分配或用户回调。
- DebugVerify 模式下可以随机重放扫描函数，检查同一对象布局是否稳定。

特殊引用必须由 GC 内置对象或未来专用 visitor 表达。这样牺牲一点解耦度，但能换来引用处理顺序和 verifier 的可证明性。

### 25.4 分配与发布补充协议

分配路径要同时解决性能和“GC 不能看到半初始化对象”：

```text
reserve memory
  -> write header as ALLOCATING
  -> zero or initialize payload
  -> optionally attach allocation handle
  -> release publish state LIVE
  -> return payload
```

补充规则：

- TLAB 内对象在 `LIVE` 前只属于当前 mutator。
- 如果构造期间可能触发 GC，必须使用 allocation handle 保住半成品，不能直接塞进普通 root。
- 非清零分配只能在类型描述证明所有引用 slot 都会初始化后启用。
- slow path 可以触发 safepoint、assist、collection 或 OOM hook；fast path 不应进入全局锁。
- 分配失败后重试次数必须有限，不能在 OOM 边缘无限 GC。

### 25.5 并发标记协议

默认并发标记建议采用 SATB 语义，因为它更适合先做可验证实现：

```text
initial mark / root snapshot
  -> concurrent mark with SATB old-value barrier
  -> remark drains SATB buffers and mark queues
  -> reference processing
  -> reclaim plan
```

关键点：

- SATB 和 incremental-update 不能在同一 cycle 中混用，除非有明确 bridge phase。
- 写屏障必须在覆盖 slot 前记录 old value。
- mark queue overflow 不能丢对象；要切换到 overflow-safe marking、whole-region rescan 或 full mark。
- remark 必须有 pause budget；超预算时要么继续并发 mark，要么进入 emergency full mark，不能静默截断。
- Mutator Assist 在 `MARK` 期间只能做 bounded mark work，不能提前 sweep 或 remap。

### 25.6 Sweep 与 Reclaim 补充协议

非移动 mark-sweep 是可靠底座，也是所有高级策略的降级路径。

Sweep 要按可切片模型设计：

```text
sweep_cursor(region, offset)
  -> inspect bounded bytes
  -> reclaim dead object
  -> coalesce local free blocks when safe
  -> publish free-list chunk
  -> update stats
```

要求：

- 每个 sweep slice 有最大字节数和最大时间预算。
- free block 合并不能破坏正在被 allocator 观察的 free list。
- 被 pin、large、internal special object 使用的区域要有独立 reclaim 规则。
- sweep 统计必须能解释 `reclaimed_objects`、`reclaimed_bytes` 和 assist repay。
- 当前 opt-in assist reclaim 是过渡形态；最终要把 mark、reference process、sweep 都拆成可恢复的 work packet。

### 25.7 分代与 Region 启用顺序

推荐启用顺序：

```text
single arena mark-sweep
  -> region table without moving
  -> TLAB + region-local allocation
  -> card table / coarse RemSet
  -> young collection
  -> mixed collection
  -> optional old evacuation
```

这样做的原因是 RemSet 正确性比搬迁性能更基础。只要 old->young 或 cross-region edge 漏记，minor/mixed collection 就会错误回收对象。

Region metadata 至少要支持：

- generation。
- allocation kind: young/old/large/pinned/metadata。
- live bytes estimate。
- pinned bytes estimate。
- remembered summary。
- evacuation candidate flag。
- reclaim/sweep cursor。
- fallback dirty/full-scan flag。

### 25.8 RemSet 替换边界

UHMAS 项目完成前，RemSet 只冻结语义，不冻结数据结构。

必须稳定的记录单位是：

```text
source object or source region
slot address or card address
target object or target region
write epoch / phase epoch
```

可以替换的实现包括：

- global card table。
- region-local card bitmap。
- per-thread slot log。
- coarse region summary。
- Bloom/filter summary。
- UHMAS side metadata。
- dirty whole-region fallback bit。

任何实现都必须满足：

- 漏记是 correctness bug。
- 多记是性能问题。
- overflow 必须升级为 coarse dirty 或 full scan。
- phase 切换时要能 flush per-thread buffer。
- verifier 能从完整 heap walk 重建 RemSet，并与实际记录对比。

### 25.9 Pin 与大对象策略

Pin 和 large object 都会破坏“随时可搬”的假设，因此要从一开始当作一等空间管理。

Pin 建议使用 lease：

```text
pin(object, reason, max_duration_or_epoch)
  -> object/region marked non-moving
  -> pinned pressure increases
  -> unpin releases lease
```

规则：

- pinned 对象可以被标记和 sweep 判断，但不能进入 evacuation set。
- pin 过多时 SMART 要降低搬迁倾向，优先 mark-sweep。
- Debug 模式检查 pin/unpin 平衡、超期 lease 和线程退出时遗留 pin。
- public FFI 优先提供 stable handle；裸指针 pin 只作为受限能力。

大对象建议单独空间：

- 大对象阈值按平台和 region size 配置。
- 默认不复制，除非明确进入 compaction/emergency compaction。
- 含引用的大对象仍必须精确扫描。
- 大对象的跨 region 引用必须进 RemSet。
- 大对象释放后优先 decommit 或加入 large free tree，避免污染小对象 free list。

当前 ABI minor 62 已完成可验证的 list-based v0：free header 的 `LARGE` 位在 FREE 状态下只表示 pool class，不表示 live object；large 回收块和其拆分余量只进入 `large_free_list`，small/large allocator 互不借用。full rebuild 分池重建链表，且不会跨不同 Region generation/placement 边界合并；ABI minor 61 的 movable-special/pinned-special sticky placement 仍是第二层复用约束。连续堆尾即使由 small/large 多个 free block 组成，也会作为一个 free run 做整 Region decommit。

ABI minor 63 已补齐 Region 对齐 run 与 START/CONTINUATION metadata。large object 保留真实 payload size，但内部占用向上取整到完整 Region；回收后 run metadata 保留，复用拆分时重新划分两侧 run。为避免制造不可复用的单 Region large fragment，只有剩余空间至少为 2 个完整 Region 时才拆分，否则请求对象接管整个旧 run。

ABI minor 64 将历史单链升级为固定 32 档 logarithmic segregated bins。档位按 run Region 数计算：`2`、`3-4`、`5-8`、`9-16`……，使用 32-bit nonempty mask 从请求档向更大档跳过空档。实现不依赖 compiler builtin、CPU bit-scan 或 64-bit lock-free atomic；split remainder 必须按新 run 长度重新入档，full rebuild 和 tail reclaim 后重建全部 heads/mask。small allocator 仍完全不搜索这些 bins。

ABI minor 65 增加 non-tail interior humongous reclaim。full collection 先 sweep/rebuild committed bins，再执行 tail reclaim，最后从 committed large-free bins 中选择至少 8 个完整 Region、backend page 对齐且 side metadata 可验证的 run 整段 decommit。这个 8-Region 下限只是当前“保留小 hot run、归还大 cold run”的原型策略，不是永久 API；ABI minor 68 已加入 run 热度/分配压力滞回，ABI minor 70 又允许宿主提供 RSS/limit、平台 pressure 和经显式阈值校准的 VM 延迟输入。

ABI minor 66 把该下限提升为 per-heap 策略输入。该版本中 `nggc_config.interior_decommit_min_regions == 0` 使用默认 8，`NGGC_INTERIOR_DECOMMIT_DISABLED` 显式关闭 interior reclaim，其他值必须至少为 2 且能由 32 位 Region 计数表示；大于实际 `region_count` 的合法阈值等价于本 heap 无候选。ABI minor 68 将 `0` 的含义演进为“初始 effective threshold 为 8 的 SMART”，非零合法值为冻结的 MANUAL，禁用仍优先。`nggc_stats.interior_decommit_min_regions` 暴露当前 effective threshold，禁用时为 0。

ABI minor 67 处理 side run 最终抵达 heap 尾部的状态归并。tail reclaim 与 side-run collapse 形成有界固定点循环：先回收 committed free tail；若新的 `arena_cursor` 落在合法 interior run 末端，则按 run metadata 定位 START、从对应 side bin 摘链、清除整个 run 的 placement/role/offset/length/next，并把 cursor 降到 START；若新的高地址边界又暴露 committed free tail或另一个相邻 side run，则继续迭代。每次迭代必须严格降低 cursor，迭代数受 Region 数上限约束。

collapse 只是 metadata 分类转换，不进行 VM 调用：页面在转换前后都处于 decommitted，`decommitted_region_count` 和 bytes 保持不变，active interior run/Region/bytes 递减，累计 collapse run/Region/bytes 增长。清除后的 Region 满足普通 tail-decommitted invariant，因此下一次 bump/TLAB/evacuation 写入通过既有 backend recommit 路径恢复；不再从 decommitted side bin 搜索。两个相邻、位于不同尺寸档的 side run 回归证明 fixed-point collapse、mask/head 更新、high-water 收缩、完整 verifier 和跨两段旧 run 的原址 bump recommit。

ABI minor 68 的 SMART v0 只在 full collection 的普通 tail reclaim 完成后、interior candidate walk 开始前运行。第一轮 evaluation 仅采样，不改变 8-Region 初值；随后每轮从 heap lock 下的一致快照计算两个 0..10000 basis-point 输入：`(region_count - decommitted_region_count) / region_count` 是不依赖 OS API 的 committed-footprint/RSS proxy，`large_free_bytes / (gc_arena_size - region_decommitted_bytes)` 是当前 committed 空闲大对象比例。控制器还对 `interior_run_recommit_count`、`large_allocation_count` 和 `large_reuse_count` 做单调 counter delta，定址 recommit 是最强的 run-hot 信号，同时至少 4 次 large allocation 且至少 2 次 reuse 也视为 large path 热。

决策是有界且可解释的：committed proxy 至少 75% 或 large-free 比例至少 25% 时，effective threshold 折半但不低于 2；出现 recommit/large-path 热度时阈值倍增，并以 `max(default 8, min(region_count, 64))` 为上限。每次真实升降后安装两个后续 full-collection evaluation 的 cooldown；cooldown 内继续采样并更新 delta，但不因内存压力反向调整，新的热度仍可优先继续提高阈值。到达上限时的热信号同样刷新 cooldown，防止下一轮立即降低。该算法只改变随后由 coordinator 验证并消费的阈值，不直接改 header、phase、free bin、side metadata 或 backend 页面。

MANUAL 和 DISABLED heap 不执行 SMART evaluation，因而手动值和显式关闭具有绝对优先级。verifier 检查 policy/effective-threshold 组合、SMART 初始化/evaluation 对应关系、action 枚举、cooldown 上界和 basis-point 范围。stats 暴露 policy、last action、cooldown remaining、两个压力输入、最近三种 delta，以及 evaluation/raise/lower/cooldown-hold 累计数。

ABI minor 70 在该安全壳内增加 `nggc_memory_pressure_fn`。宿主可报告 resident bytes + memory limit，由核心用不溢出的 basis-point 比值归一化；也可直接报告平台已经计算好的 0..10000 pressure。callback 只在 SMART full-GC evaluation、safepoint 和 heap lock 内同步调用，必须有界、非阻塞、不可保存 sample 指针且不可重入 NGGC。有效外部样本替代 committed-footprint proxy 作为主内存压力，但 committed proxy 仍计算并公开，large-free ratio 也仍是独立回收信号。未配置 callback、callback 返回非 OK、未知 flag、越界 pressure 或缺少可计算输入都不使 collection 失败；控制器清空本轮 external-valid/source telemetry、累计 failure，并立即退回 ABI 68 的 deterministic portable proxy。

同一 ABI 允许配置可选 `nggc_clock_ns_fn`，在 heap lock 内包围所有成功的 tail/interior decommit、tail/side-run recommit。时钟必须单调；C11 核心不会拿 `TIME_UTC` wall clock 冒充 latency clock，未配置时完全不采时。有效 duration 按操作覆盖 Region 数归一化，decommit/recommit 分别维护权重为 1/8 的 EWMA 和 sample count；时钟读取失败或倒退只增加 clock-failure，不改变 backend 操作结果。EWMA 默认只是解释性 telemetry；只有配置非零 `interior_decommit_slow_ns_per_region`，且相对上次 SMART snapshot 出现新样本并达到阈值时，才设置 `slow_vm_observed` 并像 recommit churn 一样提高 effective run threshold。这样不同平台不会因默认时钟精度或 VM 成本差异产生未经授权的策略漂移。

ABI 70 回归用低 external pressure 压住原本足以触发的 portable committed pressure，再让 callback 失败，证明同一 heap 能恢复 portable lowering/decommit；另一组确定性时钟测试证明慢 decommit 样本使阈值从 8 提高到 16，而时钟失败不阻止原址 recommit。Verify fault 额外破坏 pressure source/valid 组合和 slow-threshold 组合，`nggc_check` 必须报 corruption 并恢复。内建 Windows/Linux/BSD/macOS/嵌入式 provider 与自动慢阈值校准尚未提供，应在真实 UHMAS backend 稳定后通过同一 callback 合同接入。

ABI minor 69 在 logarithmic bin 之内建立严格顺序合同。committed free header 链与 decommitted START Region side 链都按 `(run bytes, start address)` 升序；尺寸是第一关键字，因此 allocator 从请求档向更大档遍历时，遇到的第一个 size 足够且 placement 匹配的节点就是全局最小可复用 run。地址第二关键字让相同尺寸的结果稳定、可重放，但绝不跨过 movable-special/pinned-special 隔离。若较小或同尺寸低地址节点 placement 不匹配，搜索继续到下一节点，并把该 probe 归入 placement reject。

所有会生成或改变 run 的路径都必须保存顺序：sweep/free link、full free-list rebuild、large split remainder、interior decommit、fixed-address partial recommit remainder，以及 side-run unlink/collapse。普通 link 在修改 head/next 前完整验证已有链的地址范围、committed/side 状态、实际 bin、严格单调性与 Region-count 有界终止；rebuild 使用局部 heads 构造有序结果后一次发布。`nggc_check` 独立重走每个 committed/decommitted bin，拒绝尺寸逆序以及同尺寸地址非递增；Verify 故障钩子交换前两个节点，证明两类链都能发现顺序腐化并恢复。

ABI 69 telemetry 保留总 `large_free_probe_count`，并新增 committed/decommitted probe source、size reject 和 placement reject 累计数。5 与 8 Regions 落在同一 `5-8` 档的回归同时覆盖 committed 和 decommitted 空间：即使较大的 run 在更高地址，5-Region 请求也必须选择原 5-Region 地址且只 probe 一个候选。另一回归把同尺寸 pinned run 放在 movable run 之前，movable 请求必须记录一次 placement reject、两次 committed probe，最终选择 movable run。当前 ordered list 让碎片选择确定，但查找与插入仍最坏 O(k)；只有 telemetry 证明同档链过长或 placement reject 占比持续过高时，才应引入 tree 或按 placement 分裂的二级索引。

decommitted 页绝不保存权威可读 header，也不会留一个 committed 锚页。页面不可读期间，Region side metadata 保存 `decommitted`、movable/pinned SPECIAL placement、START/CONTINUATION、run offset/length，以及仅在 START Region 使用的 side-list next；第二组 32 档 size-segregated bins 以 `region_index + 1` 编码节点。统一 block parser 在触碰 arena 内存前先查询 Region metadata，命中 START 时返回覆盖整 run、`header == NULL` 的伪块；mark、sweep、REMAP、free-list rebuild、stats 和 verifier 都必须显式跳过伪块 header。对象指针反查和屏障入口同样先拒绝 decommitted Region，整个协议不借助 page fault、`mprotect`、signal 或 SEH 恢复。

large allocation 的搜索顺序为 committed bins、decommitted side bins、bump。side candidate 继续满足 ABI minor 61 的 movable-special/pinned-special 隔离；backend fixed-address recommit 成功后可只消费请求前缀，至少 2-Region 的 remainder 重写 START/CONTINUATION 并重新分档，1-Region remainder 则由请求整体吸收。recommit 失败返回 `NGGC_ERR_BACKEND` 且不改变 side list/metadata/counters；decommit 失败保持原 committed free run，collection 仍成功并允许下轮重试。

当前仍不是最终 humongous allocator：committed 与 decommitted 每个 bin 内是有序单链，确定性 best-fit 已落地，但同档查找、placement 过滤与有序插入仍可能 O(k)；还没有 tree/二级索引、内建 OS pressure provider、自动延迟阈值校准或后台 compaction。ABI minor 67 已完成与 `arena_cursor` 相邻的 interior side run 折叠，ABI minor 68 已完成 portable SMART v0 阈值滞回，ABI minor 69 已完成档内排序与 source/reject telemetry，ABI minor 70 已完成宿主 pressure/monotonic-clock ABI、失败回退和延迟 EWMA。`nggc_stats` 现在同时报告两类 active bins、effective threshold/SMART decision、portable/external pressure source、VM latency、interior run/Region/bytes、累计 decommit/recommit/tail-collapse、largest reusable run 和 search/probe/filter；三源 verifier 从 arena 伪块、side bins 与 heap counters 核对守恒。

### 25.10 虚引用队列与 Reaper 协议

`PhantomQueue` 是特殊对象，不是普通队列能力。

建议生命周期：

```text
queue externally reachable
  -> normal strong liveness

queue unreachable but pending_count > 0
  -> internal root keeps queue and queued PhantomReference alive

reaper observes queue weakly
  -> temporarily promotes queue while draining
  -> clears drained entries

queue unreachable and pending_count == 0
  -> collectible
```

约束：

- reaper 持有的是弱观察入口，不应让所有 queue 永久存活。
- drain 过程要有 safepoint/phase 规则，不能和 reference-processing 同时破坏队列链。
- 用户不能通过 phantom reference 取回 referent。
- queue 自保活只由 `pending_count > 0` 和 internal root 触发，普通容器没有这个规则。

### 25.11 Mutator Assist 终态

终态 Assist 应该是“还债式小任务执行器”，不是“分配失败时顺手做一次完整 GC”。

目标接口形态：

```text
assist_enter(reason, requested_bytes)
  -> compute local budget
  -> select legal work for current phase
  -> execute bounded packet
  -> repay debt
  -> return progress / no-progress / must-collect / oom
```

work packet 类型：

- flush barrier/remset buffer。
- scan bounded root or card batch。
- mark N objects。
- process bounded reference batch。
- sweep bounded bytes。
- remap bounded slots。
- evacuate bounded bytes，最后启用。

公平性：

- 每次 assist 都有时间、对象数、字节数上限。
- 同一线程连续 assist 要退避，避免把暂停藏进 allocator。
- 低分配线程不能因为高分配线程欠债而长期无法进入 safepoint。
- 如果 assist 没有进展，策略层必须切换 phase 或触发更强 collection。

### 25.12 SMART 策略安全壳

SMART 可以很聪明，但必须被关在安全壳里：

```text
metrics snapshot
  -> score pressures
  -> choose candidate command
  -> apply hysteresis/cooldown
  -> coordinator validates command
  -> execute
  -> compare predicted vs actual
  -> update confidence
```

SMART 允许调：

- collection kind。
- soft reference 清理强度。
- young size / region selection。
- assist ratio。
- worker count。
- evacuation aggressiveness。
- decommit aggressiveness。

SMART 不允许直接做：

- 改对象头。
- 改 phase。
- 忽略 verifier。
- 清空 RemSet。
- 把 weak/soft/phantom referent 当 strong edge。
- 在 metadata 不可信时选择依赖该 metadata 的优化路径。

### 25.13 多线程与多进程边界

多线程同一 heap 内：

- 每个线程必须注册后才能参与分配或持有 GC root。
- 未注册线程只能通过 stable handle 或外部 API 间接访问。
- 线程退出必须 flush barrier buffer、retire TLAB、释放或报告 pin。
- GC worker 和 mutator 共享 phase epoch，但 worker 不应持有用户锁。

多进程共享域：

- 不进入普通 tracing closure。
- 使用 offset pointer、shared handle、immutable block 或 refcounted block。
- process-local object 不能被其他进程裸指针引用。
- shared block 不能直接指向 process-local GC object。
- 如果未来加入 shared-domain cycle collector，它必须是独立子系统，不污染本地 tracing heap。

### 25.14 验证优先级

下一阶段最值钱的 verifier：

```text
header/state verifier
  -> scan callback replay verifier [current prototype complete]
  -> root/thread registry verifier [current prototype complete]
  -> reference-processing verifier [current prototype complete]
  -> RemSet rebuild verifier
  -> phase transition verifier [current prototype complete]
  -> forwarding/from-space verifier
```

每加一个高级能力，都要同时加至少一个反向测试：

- 弱/软/虚引用不能误保活。
- RemSet overflow 后不能漏标。
- pin 过多时不能搬迁 pinned object。
- assist 超预算时必须返回。
- OOM hook 不能泄漏半初始化对象。
- 读屏障关闭前不能残留 from-space pointer。

## 26. 架构决议摘要

- 先做可靠可验证的 tracing GC，再做激进并发搬迁。
- backend、heap layout、collector protocol、policy 分层；policy 只能发请求，不能直接改对象图或 phase。
- 常驻写屏障是系统骨架的一部分，不是可选优化。
- 读屏障必须 phase-gated，避免普通读取长期付费。
- 用户扫描函数定义布局，但引用语义由 GC 明确建模。
- 扫描函数第一阶段只表达 strong edge；weak/soft/phantom 必须走 special object 或未来专用 visitor。
- weak/soft/phantom 都不是普通 strong edge 的变体，而是 reference-processing 的输入。
- PhantomQueue 只服务 phantom reference，非空自保活不是普通队列特权。
- 手动 safepoint 是主路径；系统挂起只用于诊断/fallback。
- `ALLOCATING -> LIVE`、forwarding release/acquire、phase release/acquire 是 C11 正确性的关键点。
- Region 和 RemSet 要先保证可验证正确性，再开启 young/mixed/evacuation。
- RemSet 可以换实现，但职责、overflow 降级和 barrier 接口不能丢。
- Mutator Assist 必须有债务模型、预算上限和可恢复 work packet。
- SMART 策略必须可解释、可替换、只通过 coordinator 发出合法请求。
- OOM、降级、verifier、fault injection 是一等设计，不是后期补丁。

## 27. 落地决议矩阵

本节把当前讨论收束成实现时可逐项核对的矩阵。左侧是不能被后续优化破坏的契约，右侧是允许替换或延迟实现的部分。

| 主题 | 已冻结契约 | 可演进实现 |
|---|---|---|
| C 语言边界 | 核心以 C11 为上限；不能依赖特定 OS 信号、页保护或专有线程挂起能力 | 平台层可以提供更快的线程、时间、等待和提交/释放内存实现 |
| 位宽 | 32-bit 与 64-bit 都要工作；32-bit 不默认 pointer tagging | 64-bit 可在后续通过配置启用压缩指针、tagged handle 或 side metadata 优化 |
| 类型系统 | `uint16_t` 类型 ID；`NGGC_TYPE_NONE == 0` 为 GC 内部 NONE；公开普通对象 type 只能为 `1..65535` | 类型注册表存储结构、冻结时机和调试元数据可以演进 |
| 扫描函数 | 用户注册扫描函数只枚举 strong slot；weak/soft/phantom 不靠普通扫描函数私约定表达 | 后续可增加 typed visitor、slot descriptor、自动生成 scanner 或语言绑定 scanner |
| 对象头 | header 必须足够支撑 state、type、flags、mark、forwarding 和发布协议 | 字段可搬到 side table 或 region table，但热路径语义不能丢 |
| 强引用 | strong slot 被扫描即强保活 | slot 形态可从裸地址演进为 handle、compressed slot 或 descriptor slot |
| 弱引用 | weak reference object 自身可被强保活，referent 不强保活，不可达后清理 | API wrapper、批处理方式和清理队列可以调整 |
| 软引用 | soft reference object 自身可被强保活，referent 是否保留由 policy 决定，soft 处理早于 weak | SMART 可调整 soft policy 参数，但不能绕过顺序 |
| 虚引用 | phantom reference 不允许取回 referent；referent 死亡后入队；非空 PhantomQueue 自保活只属于 PhantomQueue | 后台 reaper、阻塞等待和唤醒机制可后续添加 |
| 终接器 | 不提供 finalizer/finalization | 资源通知走 phantom queue 或上层语言自己的显式协议 |
| 写屏障 | 常驻入口，所有托管 slot 写入必须经过 store barrier | barrier 内部可以从直接记录 card 演进为 per-thread buffer、region summary 或 UHMAS side metadata |
| 读屏障 | phase-gated，只在 evacuation/remap/load-barrier 必需阶段开启 | 具体可用 forwarding pointer、handle indirection、brooks-like 实验后端或 remap table |
| 安全点 | 主路径是协作式 safepoint/handshake；系统挂起只做诊断或极端 fallback | 安全点放置密度、等待策略、超时诊断可以调 |
| 分配 | 未发布对象不能被 GC 扫描；`ALLOCATING -> LIVE` 必须 release publish | TLAB、region allocator、free-list、large allocator 可以逐步替换 |
| Mutator Assist | 计债、预算上限、可恢复 work packet 是终态要求 | ABI minor 81 已落地 per-thread debt、actor 优先偿还、四档动态预算、公平性 cap、无进展退避和 stats；reference packet、并发 worker 与无锁 work queue 仍可演进 |
| 触发策略 | policy 只能向 coordinator 提交合法请求，不能直接改对象图、phase 或 header | SMART 权重、机器学习式预测、用户手动策略都可替换 |
| RemSet | 漏记是 correctness bug；overflow 必须降级为 coarse dirty 或 full scan | 全局 card table、region-local bitmap、slot log、Bloom summary 都可替换 |
| 搬迁 | forwarding 发布和读屏障修正必须有 release/acquire 边界；pinned/non-movable 不搬迁 | 先 young evacuation，再局部 old evacuation，最后再考虑更激进并发搬迁 |
| OOM | 失败路径必须确定返回或进入确定 emergency mode；不能泄漏半初始化对象 | hook、重试次数、soft 清理强度和 decommit 策略可配置 |
| 调试验证 | Debug/Chaos/Sanitize/Profile 能检查 header、scan、phase、barrier、reference、RemSet、forwarding | Release 通过宏移除高成本验证和故障注入 |

## 28. 最小可实现闭环

为了避免设计一次铺得太大，后续每个阶段都应能形成一个完整、可测试、可降级的小闭环。

### 28.1 对象发布闭环

最小协议：

```text
reserve raw memory
  -> initialize header as ALLOCATING
  -> initialize payload and internal metadata
  -> optionally clear payload
  -> release-store state LIVE
  -> publish through root, handle or managed slot
```

约束：

- `ALLOCATING` 对象不能进入 root set、RemSet、reference-processing queue 或 scan callback。
- 分配失败时必须撤销 reserve 或把 block 恢复为可回收状态，不能留下半对象。
- 如果 payload 非零初始化，必须由类型 flag 或分配 flag 明确声明，默认策略仍建议清零。
- `LIVE` 发布后，mutator 对托管 slot 的写入必须走写屏障。
- Debug verifier 要能扫描 heap，证明没有外部可达的 `ALLOCATING` 对象。

### 28.2 类型扫描闭环

推荐第一版 scanner 契约：

```text
scan(type_id, object_payload, payload_size, visitor)
  -> visitor(slot_address) for every strong managed slot
```

扫描函数必须遵守：

- 只报告 strong slot。
- 不报告 weak/soft/phantom referent。
- 不分配 GC 对象。
- 不进入会长期阻塞的用户锁。
- 不修改对象图，除非未来显式开放 moving fixup visitor。
- 对同一对象状态应确定性枚举相同 slot 集合。
- 可读取对象自己的不可变布局元数据，但不能依赖未同步的全局用户状态。

扫描函数失败策略：

- 普通扫描函数第一阶段建议设计为 `void`，由注册时保证正确性。
- 如果未来支持可失败 scanner，失败只能中止本轮 collection 或进入 full conservative fallback，不能继续使用不完整闭包。
- verifier 模式要提供 scan replay：完整 heap walk 重新执行 scanner，检查 slot 是否落在合法对象、root 或 null 范围。

### 28.3 引用处理闭环

引用语义可以压缩成下表：

| 引用形态 | 引用对象自身是否可被普通强引用保活 | referent 是否由普通扫描强保活 | 处理时机 | 结果 |
|---|---:|---:|---|---|
| strong slot | 不适用 | 是 | mark | referent 存活 |
| weak reference | 是 | 否 | soft 之后、phantom 之前 | referent 不可达则清空 |
| soft reference | 是 | 由 policy 决定 | weak 之前 | 保留则重新标记 referent，否则清空 |
| phantom reference | 是 | 否 | weak/soft 后仍不可达 | 清空 referent 并入 PhantomQueue |
| PhantomQueue | 是 | 队列内 phantom reference 由非空队列内部保活 | reference process / reaper drain | `pending_count == 0` 且外部不可达后可回收 |

特别说明：

- “弱引用正常追踪存活”只能理解为 weak reference object 自身正常追踪，不是 weak referent 正常追踪。
- “软引用不追踪也无所谓存活”应修正为 soft reference object 自身要正常追踪，soft referent 是否追踪由 soft policy 决定。
- “虚引用队列通过追踪和引用计数同时保活”在本设计中落为 `pending_count > 0` 的内部 root 规则，不把它推广到普通队列。
- reaper 观察 PhantomQueue 必须是弱观察，watch 本身不能让空队列存活。

### 28.4 阶段状态机闭环

阶段入口统一由 coordinator 推进，mutator 只能通过 safepoint、barrier flush、assist 或 runtime API 间接协作。

```text
IDLE
  -> REQUEST_SAFEPOINT
  -> ROOT_SNAPSHOT
  -> MARK
  -> REFERENCE_PROCESS
  -> REMARK
  -> SWEEP
  -> optional EVACUATE
  -> optional REMAP
  -> RELEASE
  -> IDLE
```

最小不变量：

- phase epoch 单调递增，旧 ack、旧 root snapshot、旧 barrier buffer 不能复用到新 phase。
- `MARK` 之前必须完成必要线程的 safepoint handshake 或得到可验证 root snapshot。
- `REFERENCE_PROCESS` 只能在 strong/retained-soft 闭包稳定后执行。
- `SWEEP` 不能释放仍可能被未 flush barrier buffer 指向的对象。
- `EVACUATE` 开始前必须启用需要的读屏障或 handle remap 机制。
- `REMAP` 完成并验证后才能关闭读屏障。
- 任意 phase 失败必须进入确定降级路径：retry、full mark-sweep、emergency mode 或 deterministic OOM。

### 28.5 并发与内存序闭环

第一版只要求少数关键边界使用 C11 原子内存序，其他地方宁可保守。

必须保守处理的边界：

- 对象发布：payload/header 初始化在前，`state = LIVE` release-store 在后；扫描侧 acquire-load。
- forwarding 发布：复制完成在前，forwarding pointer release-store 在后；读屏障 acquire-load。
- phase 发布：coordinator 写 phase/epoch 使用 release，mutator poll 使用 acquire。
- 全局 work queue：push release，pop acquire。
- thread-local barrier buffer：本地写入可 relaxed，flush 到全局或 phase snapshot 时需要 release/acquire 边界。

不建议第一阶段做的事：

- 在 32-bit 上假设 64-bit 原子天然 lock-free。
- 用未定义行为风格的 pointer punning 存 tag。
- 用 signal/page fault 来隐式修复读写。
- 在没有 verifier 的情况下打开并发搬迁。

### 28.6 分配、Assist 与触发策略闭环

分配 slow path 推荐顺序：

```text
try local fast path
  -> refill local cache/TLAB
  -> bounded assist if debt or pressure is high
  -> request/schedule collection
  -> optional synchronous emergency collection
  -> OOM hook
  -> deterministic OOM
```

Mutator Assist 必须满足：

- assist 的工作量由 debt、请求字节数、当前 phase 和全局压力共同决定。
- 每次 assist 有对象数、字节数或时间预算上限。
- 没有合法 work packet 时要快速返回，让 coordinator 切换 phase 或触发更强 collection。
- assist 不能在 `MARK` 期间提前 sweep，也不能在读屏障未就绪时执行 evacuation。
- assist 统计必须能回答：谁欠债、还了多少、还债是否有进展、是否触发退避。

SMART 策略必须满足：

- 所有输入来自指标快照，不直接读取半更新内部结构。
- 选择结果先经过 hysteresis/cooldown，再交给 coordinator 验证。
- 预测失败要降低 confidence，必要时退回预设策略。
- 手动策略优先级高于 SMART，但仍不能违反 phase 和引用处理契约。

### 28.7 Region、RemSet、Pin、大对象闭环

在 UHMAS 真正稳定前，先保留抽象接口，不把数据结构写死。

最低元数据需求：

```text
region id
generation
allocation kind: young / old / large / pinned / metadata
live bytes estimate
pinned bytes estimate
remembered summary
sweep cursor
evacuation candidate
coarse dirty fallback
```

规则：

- young collection 启用前，必须有 old-to-young 或 cross-region RemSet。
- RemSet overflow 必须升级为 coarse dirty 或 full scan，不能丢弃记录后继续 minor collection。
- pinned object/region 可 mark/sweep，但不能进 evacuation set。
- large object 默认独立空间，避免污染小对象 free list。
- large/pinned 压力是 SMART 的输入，不能只看总堆占用。

### 28.8 验收顺序

建议后续按这个验收顺序推进，而不是一次打开所有高级能力：

1. Header/state/type verifier。
2. Scanner replay verifier。
3. Weak/soft/phantom reference-processing verifier。
4. Safepoint handshake 和 phase transition verifier。
5. Barrier buffer flush 和 RemSet rebuild verifier。
6. Bounded sweep/assist verifier。
7. Forwarding uniqueness 和 read-barrier verifier。
8. Region/generation invariant verifier。
9. Pin lease 和 large object space verifier。
10. OOM/fault-injection/chaos matrix。

每一步的完成标准：

- 有正向测试证明功能可用。
- 有反向测试证明关键错误会被发现或被确定降级。
- Release 构建不保留高成本验证路径。
- `PLAN.MD`、`TODO.MD` 和本文件同步更新。

## 29. 当前最优下一步

结合当前原型状态，后续最值得优先做的不是马上上完整并发搬迁，而是把已有骨架变成可验证的小切片：

1. scanner replay、root/thread registry、reference-processing 和 phase transition verifier 已落地。
2. bounded Mutator Assist mark/sweep work packet、屏障规则、统计和专用测试已落地。
3. RemSet v0 已支持 precise-card/coarse-heap overflow fallback；rebuild verifier 已收紧为 cross-region owner-card coverage，并单独统计 old-to-young 子集。
4. STW young evacuation/from-space verifier、forwarding 确定完成、单指针 CAS、evacuation/worker epoch、精确目标预留、共享持久 worker pool、并行 REMAP、worker quiescence、loser 回收和跨 safepoint delayed reclaim 已落地；ABI minor 54 又落地受限 barrier-mediated copy window，下一步补 registered mutator 解停、并发 root/REMAP 和完整 moving 协议。
5. 后台 PhantomQueue reaper worker 可作为平台层增强，但必须保持 weak-watch 不保活空队列。

这一顺序的理由是：先把“能证明不会错”的基础打实，再逐步追求更低暂停和更高吞吐。下一代 GC 的核心竞争力不只是算法激进，而是每个激进路径都有可验证的降级出口。

## 30. 第二轮整理补充：实施契约清单

本节是实现时的短清单，目的是把“下一代 GC 大概长什么样”收敛成可落地的工程契约。前文给出完整设计理由；本节只记录后续写代码时最容易踩错的边界。

### 30.1 总体北极星

目标形态不是单纯“某个现有 GC 的 C 版”，而是：

```text
C11 portable runtime
  + precise registered scanner
  + non-moving mark-sweep safety base
  + regional/generational layout
  + always-on write barrier
  + phase-gated read barrier
  + cooperative safepoint
  + bounded assist
  + verifiable reference processing
  + deterministic fallback
```

第一性原则：

- 正确性协议先于性能策略。
- 任何并发、分代、搬迁、SMART 策略都必须能降级回非移动 mark-sweep。
- Release 版热路径不能携带 Debug/Chaos/Sanitize/Profile 的高成本逻辑。
- 用户扫描函数只描述布局，不决定生命周期语义。
- weak/soft/phantom 不允许退化成“用户扫描函数私下跳过某些字段”。

### 30.2 功能冻结与未冻结边界

已经冻结的设计：

- C11 为核心上限，32-bit/64-bit 都必须支持。
- 不依赖 page fault、mprotect、signal、SEH 或专有线程挂起作为主屏障。
- `uint16_t` type id，`NGGC_TYPE_NONE == 0` 只属于 GC 内部 NONE。
- public allocation 拒绝 type 0；内部 type 0 必须带 `INTERNAL` 语义。
- 常驻写屏障，phase-or-quarantine-gated 读屏障。
- 主路径使用协作式 safepoint/handshake。
- 无 finalizer；资源通知走 weak/soft/phantom reference 体系，尤其是 phantom queue。
- `PhantomQueue` 的非空内部保活规则只属于 `PhantomQueue`，不能泛化给普通队列。
- RemSet 漏记是 correctness bug；overflow 只能 coarse dirty/full scan 降级。
- pinned/non-movable 对象不能进入 evacuation set。
- OOM、metadata failure、worker failure、RemSet overflow 必须确定返回或确定降级。
- 每个 heap 通过 `nggc_config.region_size` 选择 Region 尺寸，`0` 固定表示默认 64 KiB；合法非零尺寸不要求为 2 的幂，但必须满足对象/card 对齐、表示范围和 backend 几何约束。

暂不冻结的实现：

- public API 的最终边界。
- Region/card/TLAB/large threshold 的具体默认值与策略调优；`region_size` 可配置和 `0 == 64 KiB default` 的语义已经冻结。
- RemSet 内部结构。
- SMART 策略权重和模型。
- forwarding 是否长期留在 header，还是后续迁移到 side table。
- 后台 PhantomQueue reaper 的具体线程/唤醒实现。
- shared domain 是否加入 cycle collector。

### 30.3 Phase 与屏障矩阵

实现时可以按下表检查每个 phase 的屏障要求：

| Phase | 写屏障 | 读屏障 | 必须验证的事 |
|---|---|---|---|
| `IDLE` | 开启 | 关闭 | 无待修正 from-space pointer |
| `ROOT_SNAPSHOT` | 开启 | 关闭 | root、thread、barrier buffer snapshot 可验证 |
| `MARK` / `CONCURRENT_MARK` | 开启，按 SATB 或 IU 语义记录 | 通常关闭 | tri-color invariant 或 snapshot invariant 成立 |
| `REFERENCE_PROCESS` | 开启 | 关闭 | soft -> weak -> phantom 顺序不可乱 |
| `SWEEP` | 开启 | 关闭 | 不释放仍可能被未 flush buffer 指向的对象 |
| `EVACUATE` | 开启 | 开启 | forwarding 发布 acquire/release 正确，pinned 不搬 |
| `REMAP` | 开启 | 开启 | root、handle、strong slot、RemSet/card 被修正 |
| `RELEASE` / `COMPLETE` | 开启 | 关闭前需验证 | 关闭读屏障前无残留 forwarded/from-space strong slot |

阶段失败的默认处理：

```text
metadata not trustworthy
  -> stop using optimized metadata
  -> mark affected region coarse dirty
  -> full scan or full mark-sweep

evacuation cannot guarantee remap
  -> keep read barrier active or abandon evacuation
  -> full remap verifier
  -> from-space reclaim only after verification

assist cannot make progress
  -> coordinator changes phase or schedules stronger collection
```

### 30.4 用户扫描函数红线

用户扫描函数是可扩展点，也是最大风险点。后续所有 scanner 相关 API 都必须守住这些红线：

- scanner 不得报告 weak/soft/phantom referent。
- scanner 不得在同一对象状态下返回不稳定 slot 集合。
- scanner 不得保存 visitor/ctx 到 phase 之后使用。
- scanner 不得进入可能长期阻塞的用户锁。
- scanner 默认不得分配 GC 对象；若未来开放，必须有重入协议。
- scanner 不得自行执行 collection、safepoint wait、pin/unpin 或引用处理。
- scanner 上报的 slot 必须是托管对象 slot、root slot 或明确允许的 handle slot。

Debug/Verify 模式必须支持 scanner replay：从 heap walk 重新执行 scanner，确认 slot 合法、对象边界合法、未把特殊引用 referent 当强边。

当前原型已经实现这一层：普通检查始终要求 scanner slot 位于当前 owner payload 内且满足指针对齐；Debug/Verify 保存对象 payload 快照并双次重放 scanner，以顺序无关集合比较拒绝重复 slot、集合漂移、引用值漂移和 payload 修改。当前公开 scanner 只能报告 owner 内 strong slot；root/handle 枚举要等未来专用 registry/visitor，不能借 scanner 越界实现。重放仍是同步验证工具，不解决 scanner 缓存 visitor、长期阻塞或递归进入 GC API；这些行为继续属于硬性禁止项。

### 30.5 搬迁开启前置条件

真正开启 moving GC 前，必须同时满足：

- forwarding 发布协议有 release/acquire 边界。
- read barrier 能跟随 forwarding，并能处理 CAS 失败。
- remap pass 能修正 root、handle、普通 strong slot、dirty card 和 RemSet 指向。
- verifier 能拒绝仍指向 forwarded/from-space 对象的 strong slot。
- pinned/non-movable/large object 明确排除或走专门路径。
- evacuation 失败能回滚或转 mark-sweep，不泄漏临时 copy。
- 32-bit 路径不依赖双字 CAS 或 64-bit lock-free 原子。

当前原型已有 phase-or-quarantine-gated read barrier、STW young evacuation、原子 header forwarding、32 位 evacuation epoch、精确目标预留、共享持久 worker pool、parked safepoint 内并行 copy/REMAP、RemSet 重建、worker/from-space verifier、post-forwarding emergency completion 和跨 safepoint delayed reclaim。ABI minor 54/55/56 又加入受限 barrier-mediated copy window、registered-mutator 的 EVACUATE 状态骨架和 poll/park 驱动协作重停车：copy 期间暂时释放 heap lock，`nggc_load/store` 通过对象 payload lock 协作；已 ack 的注册线程可经 resume epoch 进入 copy window，并在 collector 请求后通过 safepoint poll/park 重新满足 parked 证明。root、分配、TLAB、REMAP 和 SWEEP 仍受 safepoint 门禁，因此下一步仍是并发 root/handle/RemSet 闭包与 mutator-concurrent REMAP，而不是把该窗口误称为完整并发搬迁。

### 30.6 SMART 策略的可解释输入

SMART 默认策略可以很复杂，但输入必须是稳定快照，输出必须是 coordinator 可验证命令。

建议输入：

- allocation rate。
- promotion rate。
- live bytes、garbage estimate、fragmentation estimate。
- young/old/large/pinned pressure。
- RemSet/card dirty pressure。
- assist debt 与 assist progress。
- soft reference retained/cleared 比例。
- pause budget 命中率。
- 上一轮预测误差。

建议输出：

- no-op。
- young collection。
- old/full mark-sweep。
- mixed collection。
- increase/decrease assist ratio。
- clear soft refs more aggressively。
- enable/disable evacuation for selected regions。
- decommit idle regions。

SMART 绝不能直接改对象头、phase、root、RemSet 或引用处理顺序。它只能提出请求，由 coordinator 按硬契约裁决。

### 30.7 验证矩阵

每个高级能力合入时至少要有一个正向测试和一个反向测试：

| 能力 | 正向测试 | 反向测试 |
|---|---|---|
| 类型注册/注销 | 可注册、冻结、注销未使用类型 | live object 使用的 type 注销返回 busy |
| scanner | strong slot 能保活对象 | 非法 slot、未注册 type、内部 type 误用被 `check` 发现 |
| weak reference | weak object 自身可存活，referent 不可达后清空 | weak referent 不能误保活 |
| soft reference | policy retain 时重新标记 referent | emergency/always-clear 时不能保留 |
| phantom queue | referent 死亡后入队，非空 queue 自保活 | reaper weak-watch 不能保活空 queue |
| RemSet | old->young/cross-region edge 能被 minor/mixed collection 看到 | overflow 后必须 coarse dirty/full scan，不能漏标 |
| safepoint | 注册线程 ack 后进入 phase | 超时或未注册线程不能破坏 root snapshot |
| assist | bounded packet 能还债并返回进展 | 超预算或无合法 work 时不能假装成功 |
| forwarding | read barrier 跟随 forwarding，remap 修正 slot | 关闭读屏障前残留 forwarded/from-space strong slot 必须报错 |
| OOM/fault injection | 失败返回确定错误或进入 emergency path | 半初始化对象不能泄漏到 root/heap scan |

### 30.8 当前路线的最小验收口径

后续提交可以按这个顺序推进：

1. 基础 verifier 已完成 scanner replay、root/thread registry、reference-processing、phase transition 和 RemSet v0 rebuild coverage；forwarding/from-space 随 region/evacuation 模型落地。
2. bounded Mutator Assist work packet 已完成；root snapshot/reference processing 暂保持原子化。
3. RemSet v0 precise-card/coarse-heap fallback 已完成。
4. region table、region-local cursor、TLAB、对象 generation/Region placement 分离、跨区 RemSet coverage 和 young collection 已落地。
5. young marking 已收紧到 roots + dirty collection-set 外 owner card + young closure，STW Survivor copy/对象级晋升/remap 已开启。
6. ABI minor 54/55/56 已把当前 STW 并行 copy 演进为受限 barrier-mediated copy window，并加入 registered-mutator EVACUATE resume/repark 状态和 poll/park 驱动协作重停车；后续仍需实现并发 root/handle/RemSet 闭包、mutator-concurrent REMAP 和完整失败降级，再开启 mixed collection 和 SMART 深调度。

只要某一步无法证明不会破坏对象图，就先不要把它放进默认策略。先有可验证闭环，再有激进优化。

## 31. 当前实现基线与下一增量协议

本节把“长期终态”收敛成当前原型下一步可以直接实现和验收的协议。它不改变前文长期方向；若本节与较早的原型描述冲突，以本节对下一增量的约束为准。

### 31.1 当前已验证基线

当前原型已经具备：

- UHMAS-HL backend 边界和单 arena 非移动 mark-sweep。
- 固定 64-byte header、`NGGC_TYPE_NONE`、用户类型注册/冻结/注销和 scanner replay verifier。
- 显式 root、线程注册、协作式 safepoint，以及严格的 `ROOT_SNAPSHOT -> MARK -> REFERENCE_PROCESS -> SWEEP` phase cycle verifier。
- weak/soft/phantom reference 与仅属于 `PhantomQueue` 的非空队列自保活规则。
- 常驻写屏障入口、phase-or-quarantine-gated 读屏障、可降级 RemSet v0、forwarding/remap/delayed-reclaim 骨架和相关 verifier。
- free-list 拆分/合并、OOM 返回、统计、故障注入构建入口和多配置测试矩阵。

这意味着后续高级功能必须在上述闭环上增量演进，不能绕过 scanner、phase、root、reference-processing 或 barrier 契约另建快速路径。

### 31.2 Bounded Mutator Assist 状态机

当前实现使用独立于完整 collector cycle 的持久 assist 状态：

```text
ASSIST_IDLE
  -> ASSIST_MARK
  -> ASSIST_SWEEP
  -> ASSIST_IDLE
```

状态保存在 heap，而不是分配调用栈中。分配失败时至多执行一个 bounded packet；若执行后仍无可用块，本次分配确定返回 OOM，调用者后续重试可以继续同一个 assist cycle。这样单次分配延迟有上限，同时工作进度不会丢失。

ABI minor 81 之前的第一版预算固定为实现常量；它保留为历史 baseline：

- MARK packet 最多处理 8 个 mark-stack item。
- SWEEP packet 最多检查约 1024 bytes 的 arena 范围。
- root snapshot 和 reference processing 在这一增量仍是原子步骤，不伪装成 bounded packet。
- ABI minor 81 已将 MARK/SWEEP 扩展为四档有界预算；未来只能在保持可验证上限的前提下校准档位阈值，不能把单个 packet 扩成无界工作。

完整 `nggc_collect` 开始时取消尚未完成的 assist cycle，清理其 mark stack/cursor，并从新的 mark epoch 启动完整 cycle。assist 不得长期占用 `collector_cycle_active`，每个 packet 只进入并退出自己合法的 phase，避免跨分配调用阻塞 safepoint。

### 31.3 Assist MARK 协议

进入 `ASSIST_MARK` 时：

1. 获取合法 root snapshot。
2. 推进新的 mark epoch。
3. 将 roots、内部保活 roots 和特殊引用对象本身加入 mark stack。
4. 每个 packet 只弹出并扫描预算内对象，剩余 stack 保存在 heap。
5. stack 为空后执行原子的 reference processing；若它重新产生标记工作，继续 `ASSIST_MARK`，否则进入 `ASSIST_SWEEP`。

在 assist MARK 活跃期间，常驻写屏障执行 incremental-update 保活：若 owner/root 已在当前 epoch 标记，而新写入 value 尚未标记，则将 value 标记并推入持久 mark stack。root push 也遵守同样规则。直接绕过 `NGGC_STORE` 修改托管 slot 或 root slot 不在支持契约内。

assist 期间新分配并成功发布的对象直接带当前 `mark_epoch`，视为本轮存活；否则它可能在尚未完成的 sweep 中被错误回收。

### 31.4 Assist SWEEP 协议

`ASSIST_SWEEP` 保存 arena cursor、目标回收字节和累计回收字节。每个 packet 从 cursor 继续，按对象边界检查有限字节：

- 当前 epoch 未标记的 LIVE 对象转为 free block。
- 当前 epoch 已标记对象保持存活。
- packet 可以做局部相邻 free block 合并，但不得回头无界遍历整个 free list。
- 到达 arena 末尾后完成 cycle，清空临时状态并回到 `ASSIST_IDLE`。

partial sweep 可以与后续分配交错：从 free block 复用或 bump 新建的对象必须按当前 epoch 标记。SWEEP 期间若写屏障试图把一个已存在、当前 epoch 未标记的对象重新挂入存活图，第一版应拒绝并报告状态错误，而不是尝试“复活”已可能接近 sweep cursor 的对象。

### 31.5 Assist 统计与进展判定

至少暴露：

- assist state、mark backlog 和 sweep cursor 对应的工作量统计。
- packet count、cycle count、progress count、no-progress count。
- mark items processed、sweep bytes processed。
- debt、repaid bytes 和 repay count。

“执行过 packet”不等于“取得进展”。只有 mark backlog 减少、状态推进、cursor 前进、回收字节增加或 cycle 完成才记为 progress。连续 no-progress 必须反馈给触发器；SMART 可以升级为完整 collection、激进 soft clearing 或 emergency mode，但不能让分配线程无限 assist。

### 31.6 下一增量验收项

bounded assist 已新增并通过以下验收：

1. 单次 allocation retry 不足以完成回收，需要多次重试才能推进完整 cycle。
2. mark backlog 跨 packet 保留，且每个 packet 不超过对象预算。
3. MARK 期间通过写屏障新挂入的对象不会被漏标。
4. partial SWEEP 期间新分配对象在 sweep 完成后仍存活。
5. sweep cursor 跨 packet 保留，且每个 packet 的检查字节有界。
6. reference processing 顺序仍为 soft -> weak -> phantom，PhantomQueue 特殊自保活不被 assist 改写。
7. 完整 collection 能确定取消并接管未完成 assist cycle。
8. cycle 最终回到 `ASSIST_IDLE`，失败路径不残留 mark stack、cursor 或 active phase。
9. Debug、Release、Profile、Release+Verify、Win32 和可用的 GCC/C11 构建矩阵继续通过。

RemSet v0 已完成：默认使用 whole-heap precise card table；达到 dirty metadata 上限后切换 `COARSE_HEAP`，将所有 card 置脏并累计 overflow。ABI minor 40 的写屏障累计 cross-region 和 old-to-young 写入；独立 rebuild verifier 通过完整 strong-slot heap walk枚举当前跨区边，要求 owner slot 所在 card 已脏，并单独统计 old-to-young 子集。故障测试构造真实 `OLD -> YOUNG` 边，清除 owner card 后必须报 corruption。UHMAS region 形态稳定前仍不冻结 card 粒度、稀疏表、位图或哈希结构。

### 31.7 RemSet v0 实施状态

当前 RemSet 对屏障暴露稳定职责，内部有两种状态：

```text
PRECISE_CARD
  -> dirty metadata limit reached
COARSE_HEAP
```

- `PRECISE_CARD` 只置脏实际写入 slot 所在 card。
- overflow 时一次性将整个 card table 置脏并进入 `COARSE_HEAP`。
- `COARSE_HEAP` 后续记录保持成功，不能因 metadata 饱和让写屏障失败或漏边。
- stats 暴露 mode、dirty limit、overflow count 和 rebuild count。
- verifier 可从 scanner 枚举的非空 strong slot 重建覆盖需求，并拒绝缺失 card。
- scanner replay 等故障测试可能故意绕过写屏障，因此 rebuild coverage verifier 当前作为独立高成本检查，不无条件并入通用 `nggc_check`。
- generation-aware rebuild 已增加 owner/target region 并验证 cross-region/old-to-young；collection-set 启用后的“集合外指向集合内”边过滤已有回归覆盖：SPECIAL/pinned owner 位于 collection-set 外，通过 dirty card 指向普通 Young target 时，young closure 必须保留并搬迁 target，同时未覆盖 Young garbage 被回收。

### 31.8 非移动 Region Table v0

当前 arena 已覆盖可配置 Region table。它先解决地址归属和可验证统计，不提前开启分代或搬迁：

ABI minor 53 增加 `nggc_config.region_size`；`0` 选择 64 KiB。非零值必须至少为一个 512-byte card、满足 64-byte 对象对齐和 card 对齐、可被 `UINT32_MAX` 表示，且 `gc_arena_size` 必须满足对象对齐。Region/card 数量、metadata 数组大小和加法/取整必须在 `size_t` 与 32-bit 地址空间上可证明；显式 TLAB 必须满足 header/64-byte 对齐且不超过实际 Region。默认 TLAB 为 16 KiB，在更小 Region 上收缩到该 Region。所有 region-local 判断都必须读取 heap 的实际尺寸，不能残留编译期 64 KiB 假设。

ABI minor 53 的 `nggc_open` 在 backend 映射前完成上述 geometry validation；非法配置返回 `NGGC_ERR_INVALID_ARGUMENT`，不会部分创建 heap。Region table 使用向上取整覆盖末尾 partial Region，但每个 Region 的 `[start, end)` 必须由 arena 边界精确确定。card table 仍保持 512-byte 固定粒度，Region 要求为 card 的整数倍以保持 Region/card metadata 可解释。`nggc_stats.region_size` 和 `tlab_size` 返回冻结后的实际值。

- 每个 region 保存 `[start, end)`、committed bytes、live bytes、free bytes、quarantined bytes，以及以该区为起点的 live/quarantined object count。
- 每区维护 active/inactive state，以及 has-live/free/large/pinned/non-movable/quarantine flags；属性计数按对象覆盖区增减，flags 只由计数刷新。
- bump allocation 首次覆盖某区时增加 committed bytes 和 active region count。
- free-list reuse 将覆盖字节从 free 转为 live，不重复增加 committed。
- 普通 sweep 将死亡对象覆盖字节从 live 转为 free；moving sweep 将 forwarding source 从 live 转入 quarantined，后续安全点才从 quarantined 转为 free。
- 对象可以跨 region；大对象按实际覆盖字节分摊，object count 归属 header 所在起始 region。
- free block 拆分或相邻合并不改变 `committed = live + free + reserved + quarantined`，因此无需改变 region 字节归属。
- `nggc_check` 从 arena object walk 重建每区统计并逐项比较，拒绝 region 边界、active count 或字节守恒损坏。

ABI minor 53 刚合入时尚未加入、但后续 minor 已逐步补齐的项目：

- young/old generation。
- eden/survivor/old/pinned/large region state。
- region-local allocation cursor 或 TLAB ownership。
- collection set、live estimate、evacuation candidate 和 reclaim epoch。
- decommit/unmap 后 inactive region 回退；ABI minor 60 已覆盖 tail Region，ABI minor 65 已覆盖完整 interior humongous run，partial Region 仍保持 committed。

这些字段应在启用 young collection 前加入，但不能破坏当前对象跨区、非移动 mark-sweep 和 UHMAS-HL backend 边界。

ABI minor 39 建立了第一版 region placement 标签，但其中“标准/large/pinned/non-moving 直接放入 OLD”的做法已被 ABI minor 46 废止。当前对象头独立保存逻辑 generation 与 `young_age`：所有新对象统一以 `YOUNG, age=0` 发布；普通共享分配与 TLAB 使用物理 YOUNG region，large/pinned/non-moving 使用物理 SPECIAL region。共享 free-list 只复用与目标 placement 兼容的 block，任何普通分配都不能借用 OLD block 形成预晋升。

ABI minor 61 将 SPECIAL placement 从泛化标签收窄为可验证的内部 Region 角色：`NONE`、`MOVABLE_SPECIAL`、`PINNED_SPECIAL`。large 但可移动的 SPECIAL 对象只能进入 movable-special Region；`NON_MOVABLE` 对象进入 pinned-special Region；普通 `PINNABLE` 对象仍进入物理 YOUNG，动态 Pin 时由 evacuation 策略原地保留。free-only Region 会保留 sticky placement，直到 tail decommit/reset 清空 metadata；因此回收后的 pinned free block 不会被普通 large 对象复用，反向同理。

ABI minor 79 完成第一阶段动态 Pin lease：`PINNABLE` 对象支持嵌套 `nggc_pin` / `nggc_unpin`，对象头以固定 64-byte header 中的 `pin_count` 记录深度；Pin 不提供可达性，且不创建 root。首次 Pin 和最终 unpin 更新覆盖 Region 的实际 pinned-object 计数，`NGGC_REGION_FLAG_HAS_PINNED` 保持对 dynamic pinned 与 `NON_MOVABLE` 的物理兼容语义。young evacuation、forwarding、from-space quarantine、free/TLAB 状态以及 `nggc_check` 都验证 pin 为零或符合 pinned-young 原地保留规则。Pin 在不稳定 GC/safepoint/Assist/quarantine 窗口返回 busy，先完成 TLAB fast gate quiescence；回收不可达 pinned 对象时同步清除 lease 和全部统计。该版本仍不宣称完整长期 pinned space、pin owner/token、超时回收或并发 REMAP。

ABI minor 62 在该 placement 约束上增加 Large Object Space v0。FREE header 只允许 `flags == 0` 或 `flags == LARGE`：前者属于普通 free list，后者属于 large free list。small/large allocation 只搜索对应链；large split 的 remainder 继续保留 `LARGE`，full rebuild 分别重建双链并只合并 pool class 相同、跨 Region 边界时 generation/placement 也相同的相邻块。`nggc_check` 从 arena walk 重建 large-free 子集，再分别验证双链节点、环上限、总 free 守恒和 large 子集守恒。stats 增加 large free object/bytes 与 large allocation/reuse 计数。

ABI minor 63 增加 Region-aligned humongous run。Region metadata 新增 `NONE/START/CONTINUATION` role、run offset 和 run length；LIVE/FREE large block 都必须从 Region 起点开始、覆盖至少两个完整 Region且 `alloc_size` 为 Region 整数倍。回收只改变 live/free 字节与对象属性，保留 run metadata；large free-list rebuild 不合并相邻 run；拆分会分别重写前后 run metadata，且不产生少于两个 Region 的 remainder。tail decommit/reset 会把 role/offset/length 清零。

`nggc_check` 不信任现有 run metadata，而是从 arena block walk 重建每个 large block 覆盖的 START/CONTINUATION 序列，再逐 Region 比较。Debug/Verify 故障测试会破坏 role 和 continuation offset 并要求检查失败；正向测试用 4-Region free run 拆分为两个 2-Region run并逐个复用，还验证 3-Region run 面对 2-Region 请求时整 run 接管、不遗留单 Region large fragment。stats 报告总/live/free run 数和 humongous Region 数。

ABI minor 64 在 humongous geometry 合同上增加 size-segregated large free index。固定 32 档覆盖最多 `UINT32_MAX` 个 Region：bin 0 对应 2 Regions，bin 1 对应 3-4，bin 2 对应 5-8，后续逐级翻倍。allocator 从请求档向上扫描 `uint32_t` nonempty mask，只 probe 非空档中的候选；每个候选仍需满足实际尺寸和 ABI minor 61 placement。split 时旧节点先从原档摘除，至少 2-Region 的 remainder 根据新 run 长度重新入档；整 run 复用时仅在该档 head 为空后清除 mask bit。

full free-list rebuild 从 arena block walk 重新生成全部 32 个 heads 与 mask。`nggc_check` 逐档验证节点状态、LARGE pool class、START/CONTINUATION run、实际分档、链表上限、mask/head 一致性，并将所有档的 count/bytes 与 arena 重建值和 heap 统计比较。Debug/Verify 注入会清除真实非空档 bit 或把 header 额外放入错误档，检查必须报 corruption，恢复后再次通过。stats 暴露档位容量、活跃档数、最大空闲 run Region 数、search count 与 probe count；8 KiB Region 回归覆盖两个不同档同时非空、向上跳档、拆分重分档和单 Region 余量整 run 吸收。

ABI minor 65 在 committed bins 之外增加 decommitted side bins。full collection 按 effective threshold 选择每区和整 run 都满足 backend page 对齐的空 humongous run 执行整段 decommit；ABI 65 初版 threshold 固定为 8 Regions，ABI 66 后初值仍为 8 但可 per-heap 覆盖或禁用，ABI 68 后未覆盖模式允许 SMART 在安全范围内调整 effective 值。side metadata 在页面不可读时成为唯一权威结构。parser 先查 side metadata并返回无 header 的整 run 伪块，allocator 可 fixed-address recommit 请求前缀并重新分类 remainder。`nggc_check` 分别从伪块 walk、side-bin walk 和 heap counters 重建 run/Region/bytes/mask，故障注入覆盖 mask 与 continuation offset；backend fault injection 证明 decommit/recommit 失败都不留下半事务。

ABI minor 66 允许每个 heap 独立配置上述最小 run 长度或显式关闭 interior decommit。collector 始终读取 heap 冻结后的 effective threshold，而不是编译期常量；threshold 不改变 committed/decommitted bin 分类、placement 约束、页对齐、失败回滚或 parser 伪块合同。默认、4-Region 自定义启用、5-Region 阈值下 4-Region run 保持 committed、显式禁用和非法 1-Region 阈值均有回归覆盖。

ABI minor 67 让 tail reclaim 继续穿过已经 decommitted 的 interior side runs。只要 side run 精确结束于 high-water，就将其从 side index 转为普通 tail metadata 并下降 cursor；连续 run 和新暴露的 committed free tail会在同一 full collection 中继续处理，直到 cursor 不再下降。转换不重复调用 backend，也不改变总 decommitted Region/bytes，只改变 interior 分类并累计 collapse telemetry。

ABI minor 68 让默认阈值成为有状态 SMART v0：首轮 warmup 后，committed-footprint 或 large-free 压力可把阈值从 8 逐级降至 4/2，recommit 或显著 large allocation/reuse 热度可把阈值逐级提高到最多 64，每次变化后保持两个 evaluation 的 cooldown。测试固定构造 4-Region interior run，证明 warmup 保持 committed、压力下降阈值并 decommit、原址 recommit 后升阈值、两轮内不重退提交，以及 cooldown 结束后再次回收；同一矩阵也证明 MANUAL/DISABLED evaluation count 始终为零。

ABI minor 69 对 committed 与 decommitted 两类 size bin 增加相同的 `(run bytes, address)` 档内排序，使 first compatible 具有确定性 best-fit 语义。full rebuild 和所有增删/split 路径维持顺序，verifier 与 order-swap fault 覆盖严格单调性；stats 把 allocation probes 分解为 committed/decommitted source 与 size/placement reject。5/8-Region 同档及 pinned-before-movable 回归证明最小空间选择与 placement 隔离。

ABI minor 70 为 SMART 增加两个可选、宿主提供且不绑定 OS 的输入。memory-pressure callback 可返回 RSS/limit 或直接 basis points，有效样本覆盖 portable committed-pressure 主输入，失败/非法样本退回 ABI 68；monotonic clock callback 对成功 Region VM 操作生成每 Region decommit/recommit 1/8 EWMA，无时钟时不使用 wall-clock 代替。延迟只在显式慢阈值和本轮新样本同时成立时参与 raising。stats、确定性回归和 pressure-source/slow-state verifier fault 覆盖输入来源、失败计数、样本 delta、EWMA 与降级语义。

Region 尺寸继续由 ABI minor 53 的 `nggc_config.region_size` 冻结：`0` 为默认 64 KiB，合法自定义值会同时驱动 large threshold、TLAB 上限、humongous 几何和两类分档。ABI minor 63/64/65 都使用通用整数除法、取整和 Region-count 分类，不要求 Region 必须是 2 的幂；8 KiB、24 KiB（非 2 的幂）和 128 KiB 配置都验证为 2-Region large run，并继续满足 32-bit `size_t`/地址空间边界验证。

ABI minor 46 的物理 placement 标签为 `NONE/YOUNG/OLD/SPECIAL`。verifier 要求：未提交 region 为 `NONE`；活动 TLAB 只能属于物理 YOUNG；large/pinned/non-moving 对象只能位于 SPECIAL；普通物理 YOUNG 对象的逻辑代别必须为 YOUNG；普通物理 OLD 对象的逻辑代别必须为 OLD。stats 同时报告 young/old/special region 数和 young/old object 数，避免再用 region 数量代替对象年龄。

ABI minor 41 已加入非移动 young collection v0。`nggc_collect_young` 和 collector-owned `nggc_thread_collect_young` 在 cycle 开始后选择全部 committed young region；SWEEP 只回收 collection-set 内未标记对象，old garbage 保留到 full GC。成功和所有 abort 路径都会清空 collection-set。

ABI minor 42 把 ROOT_SNAPSHOT/MARK 收紧为代际输入；ABI minor 46 将其从“看 region 是否 OLD”改成“看对象逻辑代别和是否位于 collection-set”。所有 collection-set 外 live object 本轮 implicit-live，但不压入 mark stack；global/thread roots 直接发现集合内 Young；collector 扫描集合外普通对象 scanner 上报的 dirty slot，随后只递归 young closure。这样 SPECIAL Young/Old owner 指向普通 Young 的边也不会漏掉。soft/weak/phantom 仍沿用完整 reference-processing 顺序。

对象级 survivor/tenuring 是硬语义：对象出生时必为 `YOUNG, age=0`，每成功经历一次适用的 young collection 才增加年龄；只有 `object_age >= young_tenuring_threshold` 时才允许晋升 OLD。该阈值是 OLD 的硬性最低年龄，不是必须立刻晋升的最后期限：容量不足或非移动降级可以延迟晋升，但任何策略、对象属性或物理 placement 都不能提前晋升。未达到阈值的普通可移动对象复制到 Survivor region；large/pinned/non-moving 对象在 SPECIAL 空间原地增龄并在达到同一阈值后原地逻辑晋升。年龄使用对象头中的饱和小整数或等价 side metadata，不能回绕。

region 只记录物理空间角色和集合属性，例如 Eden、Survivor、Old、Special/Pinned/Large、collection-set、evacuation state；对象头或等价 side metadata 才记录逻辑 YOUNG/OLD。SPECIAL region 中的对象原地晋升后 Region 仍是 SPECIAL。整区晋升只能用于“该区所有存活对象均已逐对象证明达到阈值”的优化，不能作为默认 tenuring 协议。young collection abort、evacuation failure 或未完成发布不得增加对象年龄。

ABI minor 43 已实现对象级年龄基础；ABI minor 46 又增加对象逻辑 generation。新对象为 `YOUNG, age=0`，free/TLAB sentinel 为 `NONE, age=0`。非移动 fallback 后，普通 collection-set survivor 年龄递增并可停在 tenuring candidate；移动成功时达到阈值的对象复制到物理 OLD。SPECIAL 对象在成功 young collection 后原地增龄，达到阈值时只改变对象逻辑 generation 并清零 young age，Region placement 不变。verifier 拒绝任何出生即 OLD、OLD 携带非零 young age或对象/placement 不匹配。

ABI minor 44 加入 evacuation capacity preflight。规划器冻结本轮 `source_end`，只枚举 collection-set 中已标记存活对象，按下一年龄分成 Survivor(YOUNG) 和 promotion(OLD) 两组，再从当前 arena cursor 模拟目标布局。模拟会跳过所有 from-space region、代别冲突 region，并精确计入对象跨不过 region 尾部时的封口损耗。只有两组都能完整布局才算可行；容量不足发生在任何 forwarding 发布之前，因此当前实现会计数后退回既有非移动 young sweep。测试覆盖正常 Survivor/promotion 字节规划和两区满堆的确定 capacity fallback，后者必须保留原 young survivor。

ABI minor 45 在该 preflight 上启用 STW young evacuation v0。可行计划把 collector cycle 提升为 moving kind，按 Survivor 后 promotion 的固定分组复制；两组同时存在时必须先封口 Survivor region，禁止普通 YOUNG/OLD 对象混入同一 region。目标对象复制 payload、类型、object id、mark epoch 和正确的对象逻辑 generation；年龄未达阈值则保留 YOUNG 并递增，达到阈值才写入 OLD 并清零 young age。目标以 release state 发布后，源 header 才保存 forwarding。

`REMAP` 会修正 global root、thread root 和 scanner 枚举的全部 strong slot。扫描复制目标时同时重放 RemSet 记录，因此已晋升 OLD 的对象若指向较年轻 Survivor，会给目标 owner card 补上 `OLD -> YOUNG` coverage。ABI minor 52 起，随后 SWEEP 将未标记源对象按死亡立即回收，将已 forwarding 的源对象从 `LIVE` 转为 `FORWARDED` 隔离壳；它不计入全局 live，也不进入 free list。from-space verifier 要求 collection-set 在 sweep 后不再包含 live object，并核对 forwarding target 的 type、size、flags、object id、mark epoch 和逻辑 generation。

测试先用真实 young collection 将普通共享分配对象按阈值晋升为 OLD，再构造 mixed-age 父子图；覆盖 payload 保持、root 与对象内部 slot remap、父对象晋升、较年轻子对象留 Survivor，以及下一轮仅凭 REMAP 重建的 remembered edge 保活并晋升子对象。large/pinned 测试则证明对象先保持逻辑 YOUNG，达到阈值后在 SPECIAL region 原地变为逻辑 OLD，`old_region_count` 不会因此增加。ABI minor 49 已让复制任务在 parked safepoint 内并行运行，ABI minor 51 又让 heap strong-slot REMAP 复用同一 worker pool；coordinator 仍持 heap lock且 mutator 全部 parked，因此尚未与 mutator 并发。

ABI minor 46 还修正了 RemSet 判定：old-to-young 子集按 owner/target 的对象逻辑 generation 计算，而不是按 Region 标签猜测。故障测试中的 OLD owner 也必须先经真实年龄晋升，随后清除其实际 owner card 并要求 rebuild verifier 报错。ABI minor 79 已补齐内部 weak/soft/phantom runtime object 的专用 evacuation slot remap；这些对象仍按自身年龄保持 YOUNG/晋升语义，不会因为是内部对象而预晋升为 OLD。

ABI minor 47 将第一次 source forwarding 发布定义为 STW moving collection 的不可逆提交点。提交前的容量或注入故障仍走普通 abort，不能增加对象年龄，也不能留下复制目标；提交后禁止清空 collection-set 后直接 abort。copy 会识别并验证已经发布的 forwarding，REMAP 对已修正 slot 幂等，SWEEP 对已隔离 source 幂等，因此 collector 可以依据当前 `EVACUATE/REMAP/SWEEP` phase 在同一 parked safepoint 内补完剩余工作。ABI minor 52 后，补完成功会结束 cycle、更新 collection/年龄/回收统计并释放 mutator，但若产生 `FORWARDED` 壳则读屏障继续开启；API 仍返回触发恢复的原始错误码，避免静默吞掉 worker/scanner 类故障。如果 emergency completion 本身也失败，则不再执行伪回滚：堆保留当前 safepoint、cycle、phase 和 collection-set 进入 fail-stop，禁止 mutator 观察半搬迁状态；后续需专门的 heap quarantine/force-destroy 管理面，这与正常 delayed-reclaim 隔离态不同。

Verify+Test Hooks 覆盖五个 moving 故障点：首次 forwarding 前、第一次 forwarding 后的部分 copy、全部 forwarding 发布后、第一次 slot 修正后的部分 REMAP、第一次 source release 后的部分 SWEEP。每个 post-forwarding 故障都要求 emergency completion 计数增加、failure 计数保持零、root 和对象内部 edge 指向新对象、from-space 变 free、父对象按阈值晋升、较年轻子对象保留 YOUNG，并且下一次 young collection 仍能只凭重建的 `OLD -> YOUNG` remembered edge 保活并晋升子对象。

ABI minor 48 将 header forwarding 改为 C11 `_Atomic(void *)`。目标 body 与 `LIVE` state 先 release 发布，source header 再用单指针 release-CAS 从 `NULL` 发布到目标；读屏障、REMAP 和 verifier 使用 acquire load。free block 仍复用同一字段作为 free-list next，但只在 `FREE` state 下以 relaxed 原子访问。该协议在 32-bit 和 64-bit 上都只要求原生指针宽度 CAS；实现不假设双字 CAS，也不要求 64 位 atomic lock-free。

每次进入 moving cycle 都先验证 live heap forwarding quiescent，再分配非零 32 位 evacuation epoch。发布者先写 source `forwarding_epoch`，随后 CAS forwarding；读方看到空 forwarding 时允许 `EVACUATE` 中短暂存在“epoch 已写、CAS 未完成”的发布窗口，看到非空 forwarding 后则必须验证 epoch 与当前 cycle 一致、目标为完整 live object且目标自身未 forwarding。CAS 失败即使最终因 epoch 不匹配返回 corruption，也必须把 observed forwarding 回传给协调器，使失败路径按 post-commit 处理；未被 source 采用的串行临时 copy 立即转换为 free block。`IDLE` 严禁 live forwarding。stats 暴露当前 epoch 和 CAS success/failure；测试覆盖 CAS winner、同 epoch loser、错误 epoch loser、缺失/残留 epoch、IDLE 残留以及真实 moving collection 中 CAS success 与 evacuated object 数量一致。

ABI minor 49 把 evacuation preflight 的模拟布局物化为精确任务表。每项任务冻结 source header、唯一 destination、alloc size 隐含边界和 target generation；target generation 必须逐对象按 `next_age >= young_tenuring_threshold` 计算，large、pin、non-moving、目标空间压力或 worker 调度都没有提前赋予 OLD 的权限。阈值未到的目标只能进入物理 YOUNG Survivor，达到阈值的普通可移动对象才进入物理 OLD；容量不足只能在 forwarding 前降级非移动 young sweep或延迟晋升。

coordinator 在启动 worker 前一次性发布所有 padding、Region generation、destination header 和 Region 记账，因此 worker 不参与目标分配，也不获取 heap lock。任务状态使用 C11 atomic 在 `PENDING/RUNNING/RETRY/COMPLETE/FINALIZED` 间转换；coordinator 与最多三个后台线程共同领取不相交任务，复制 payload、release 发布 destination `LIVE`，再通过 ABI minor 48 的单指针 CAS 发布 source forwarding。不提供 C11 threads、线程创建失败或任务要求重试时，coordinator 同步接管剩余任务。ABI minor 54/55/56 后，coordinator 在 EVACUATE worker 运行期间会临时释放 heap lock；worker 复制 source payload 时持有 source header 的 payload lock，`nggc_load/store` 可以在该窗口进入并保护引用槽。已 ack 的 registered mutator 在窗口内由 `AT_SAFEPOINT` 转为 `CONCURRENT_RUNNING` 并经 resume epoch 解停，worker 完成后由 collector 请求 repark，mutator 通过 safepoint poll/park 回到 `AT_SAFEPOINT`；只有 parked/running/repark 计数一致且 running 清零，才能进入 REMAP。root、分配、TLAB、REMAP 和 SWEEP 仍由 safepoint 门禁保护，因此这是受限 barrier-mediated copy window，不是完整 registered-mutator concurrent evacuation。

每轮 moving cycle 同时建立 evacuation epoch 和 worker epoch。active worker 使用原子计数，peak、entered/completed 与 quiescence 由 coordinator 在本轮 pool job 完成后核对；REMAP 和 SWEEP 只有在 worker epoch 匹配、active 为零、quiescence 已发布且 from-space reclaim gate 非零时才能继续。重复 source 任务允许多个临时 copy 竞争，但只有 canonical CAS winner 被保留；loser destination 先进入 coordinator-owned reclaim queue，等全部 worker quiescent 后再转 free block。尚未发布任何 forwarding 的失败必须撤销全部预留、重建 free-list 并重放 Region verifier；若无法证明回滚完整，则保持 safepoint 进入 fail-stop。任一 forwarding 已发布后继续沿用 ABI minor 47 emergency completion，禁止伪回滚。

Verify+Test Hooks 已覆盖 64 个独立 copy task、重复任务的一次 CAS 成功/一次失败与 loser 回收、任务级重试、一次性 `thrd_create` 返回失败，以及 active worker 出现在错误 phase、REMAP 缺失 reclaim gate 两类状态破坏。所有 action case 都先验证第一次成功存活后仍为 YOUNG，再验证第二次达到配置阈值后才晋升 OLD。stats 暴露 worker epoch、active/peak/quiescent、parallel cycle、pool size、created/create-failure/dispatch/fallback、task 和 loser-reclaimed 计数；正式 Release 不包含测试 action hook。

ABI minor 50 将每轮 `thrd_create/thrd_join` 改为每 heap 按需创建的持久 evacuation worker pool。最多三个后台 slot 各自保存 heap owner、稳定 index 和已观察 dispatch epoch；空闲线程在独立 condition variable 上休眠。coordinator 在池锁下发布唯一 job、requested worker 数和递增非零 dispatch epoch，广播后自身作为第四个执行者处理同一原子任务游标，最后等待全部被派发 slot 增加 completed count。job 指针只有在 requested/completed 完全相等后才清空，因此栈上的 work context 不会被下一轮或已迟到 worker 观察。

线程池在第一次确实需要后台并行时才扩容，不为单任务 collection 预建线程；已建线程跨 Young collection 复用，`nggc_close` 在确认没有活动 collector/safepoint 后广播 shutdown 并统一 join。一次创建失败只降低本轮并发度，不把 heap 标为损坏；coordinator 接管全部未领取任务，下一轮仍可继续扩容。phase verifier 对 live heap 核对 pool size 上限、created slot 的 owner/index、idle job 清零、in-flight requested/completed、dispatch epoch 和 shutdown 状态。C11 测试要求同一 heap 两轮复制时 created/pool size 不增长而 dispatch/parallel cycle 增长，并验证一次创建失败后的下一轮恢复；threadless 构建要求 pool/dispatch 永远为零。

ABI minor 51 将该持久 evacuation pool 泛化为共享 GC worker pool。pool job 只包含同步 `run(context)`，copy 和 REMAP 复用同一组最多三个后台线程，但分别统计 dispatch、parallel cycle 和 fallback。job/context 只活到 coordinator 等待全部 requested worker 完成；pool 在 completed 与 requested 相等前不得清空 job 或开始下一轮 dispatch。

moving REMAP 仍在同一个 parked safepoint 内执行。global roots 和 thread roots 由 coordinator 串行修正；heap 不建立动态 slot 任务表，而是用 `_Atomic(uintptr_t) next_cursor` 让 worker 对 block 起始地址执行 CAS 唯一领取。每个 live object 只由一个 worker 调用 scanner 并修正 strong slot，因此正确 scanner 不会产生 slot 写冲突。scanner 必须遵守前述并发可重入、owner payload 边界、布局决定 slot 集合和同步 visitor 生命周期契约。RemSet/card 更新及完整 forwarded-slot verifier 在所有 worker 归静后由 coordinator 串行重放，避免 card metadata 和统计发生数据竞争。

REMAP 有独立的 `remap_worker_epoch`、active/peak/quiescent、parallel/dispatch/fallback、block/scanned-slot/rewritten-slot 统计。进入 SWEEP、回收 CAS loser、隔离 from-space 和完成 moving cycle都必须同时证明 copy 与 REMAP 的 epoch 匹配当前 evacuation epoch、active worker 为零且 quiescence 已发布。故障注入新增“REMAP active worker 出现在非法 phase”和“未发布 REMAP quiescence 却进入 reclaim”两类破坏。当前 REMAP/SWEEP 仍只是 collector-internal parallelism：mutator 在这些阶段全部 parked，coordinator 仍持 heap lock；ABI minor 52 只让 forwarding 壳跨 safepoint 存活，ABI minor 54/55/56 仅让 EVACUATE copy window 临时释放 heap lock、开放 `nggc_load/store`，并加入 registered-mutator 的 poll/park 协作重停车，尚未完成并发 REMAP 或完整 root/handle/RemSet 闭包。

ABI minor 52 将 from-space 的逻辑失效与物理复用拆开。moving SWEEP 对已 forwarding source 执行 `LIVE -> FORWARDED`，保留 header identity、payload 和 forwarding epoch，Region 记账从 live 转入 quarantined；未 forwarding 的死亡对象仍可立即转 FREE。`FORWARDED` 既不是 live object，也不是 free block，不能进入 mark/scanner/free-list，但 `nggc_load` 可 acquire-follow target 并自愈陈旧 slot，`nggc_store` 可按 source payload offset 找到 canonical owner slot、canonicalize value、记录 canonical RemSet 并双写 shadow/target。对象查询 API 对隔离 source 返回 `TYPE_NONE/size 0/flags 0`。

隔离态建立时记录 evacuation epoch、产生隔离的 safepoint epoch、source high-water、对象数和字节数。退出 moving cycle 后允许 `IDLE + read_barrier_active`，但禁止开始新的 collector/Mutator Assist cycle；只有不同 safepoint epoch 已证明 `pending=0 && parked=registered`，或者 registered mutator 集合为空且已进入后续 collection，才能把全部 `FORWARDED` 壳转为 FREE、重建 free list并关闭读屏障。原 safepoint 的 release 只能发布隔离态，不能在同一 epoch 立即复用 source。phase/Region/heap verifier 和故障注入覆盖隔离态缺少读屏障、evacuation epoch 不匹配以及缺少后续 safepoint 回收证明。

Region-local bump v0 已进一步落地：

- `alloc_size <= region_size` 的普通对象不得跨 region 边界。
- 当前 region 剩余空间不足时，尾部按 64-byte 对齐发布为 `FREE + TYPE_NONE` padding block，并加入统一 free list。
- padding 即使只有一个 header 大小也保持合法 heap-walk 结构；过小而无法服务普通对象的块只表现为空间碎片，不破坏正确性。
- 后续较小分配可以从 padding block 拆分复用。
- `alloc_size > region_size` 的对象自动标记 large；ABI minor 63 后会先对齐到下一 Region 起点，再把内部 `alloc_size` 向上取整为至少 2 个完整 Region 的 humongous run，并写入 START/CONTINUATION metadata；ABI minor 64 后 committed free run 按 Region 数进入 logarithmic bin，ABI minor 65 后足够大的冷 run 可转入同尺寸分类的 decommitted side bin。
- verifier 拒绝任何未标记 large 却跨 region 的 live object。
- stats 暴露累计 padding block count 和 padding created bytes，用于评估 region 边界浪费。

这仍是单个 heap cursor 驱动的 region-local 边界，不是最终 per-thread/per-region allocator。下一步要把“选择 region”和“在 region 内 bump”拆开，并为 region 增加 owner/cursor lease，之后才适合接 TLAB 和 generation。

当前又完成了 allocator ownership 的第一层拆分：

- 每个 region 保存独立 `allocation_cursor`，表示该区曾经由顺序 bump 发布到的 high-water。
- 已封口 region 的 cursor 等于 region end；未来 region 的 cursor 等于 region start。
- 当前未封口 region 的 `allocation_owner` 指向 heap allocator，其他 region owner 必须为空。
- padding 或普通/large bump 推进 cursor 时，只更新跨过的 region 和当前 owner，不按每次分配遍历整张 region table。
- free-list reuse 不倒退或推进 allocation cursor，因为它复用的是已发布地址范围。
- verifier 从 arena global high-water 重建每区 cursor/owner，并要求 owner 数量最多为一；stats 暴露当前 owned region count。

这还不是并发 lease：owner token 目前只是内部 heap 身份。下一步应引入可撤销 lease epoch、region selection 和 thread/TLAB owner，且 safepoint 必须能收回或冻结所有 lease。

Heap allocator lease v0 已落地：

- lease 使用单调 epoch，并统计 acquire/revoke 次数。
- safepoint request、collector phase begin 会撤销当前 region owner。
- pending 线程归零且 phase/cycle idle 后，下一次需要 bump 的分配重新获取 lease并发布 owner。
- free-list reuse 不需要 lease，因为它不扩大已发布 high-water。
- 当前共享 bump 始终持有 heap lock，因此 safepoint pending 时允许无 lease bump 作为兼容 fallback，但 owner 保持为空；这不等价于允许未来 TLAB 在 safepoint 后继续分配。
- verifier 要求 active lease 不得与 pending safepoint、active phase 或 collector cycle 并存，并校验 lease epoch 非零。

未来 thread/TLAB lease 必须更严格：线程只有在发布 TLAB top、撤销 region owner 并停止私有 bump 后才能 ack safepoint；GC 不得扫描尚未发布的 TLAB 尾部。

Region allocation verifier 现已具有负向证据：测试目标可以注入 cursor 偏移、伪造 owner、重复 owner，以及 active lease 与 pending safepoint 并存；每种损坏都必须返回 corruption，并在恢复 shadow 状态后重新通过完整 `nggc_check`。这些 hook 仅编入 Verify+Test Hooks 目标，不进入正式库。

### 31.9 Thread/TLAB Lease 落地协议

TLAB 不能简单表示为一个巨大的 `ALLOCATING` 对象，也不能直接作为普通 `FREE` block 挂入全局 free list：前者会隐藏 TLAB 内已经发布的多个对象边界，后者会允许其他线程抢走当前线程仍在使用的尾部。

下一实现采用 region side metadata 描述 reserved span：

```text
region lease
  owner_thread
  lease_epoch
  reserved_start
  published_top
  reserved_end
  state = ACTIVE | RETIRING | RETIRED
```

硬约束：

- `[reserved_start, published_top)` 必须始终是可由普通 heap walk 解析的对象或 free/padding block 序列。
- `[published_top, reserved_end)` 不进入普通 object walk，由 active TLAB side metadata 明确遮蔽。
- mutator 每次分配先在私有尾部写 `ALLOCATING` header 和 payload，再 release 发布 `LIVE`，最后 release 推进 `published_top`。
- GC 只能扫描不超过 acquire-load `published_top` 的范围。
- refill 前必须先 retire 旧 TLAB，不能让同一线程同时持有两个 active span。
- region lease epoch 与 thread observed epoch 必须匹配；陈旧线程不能继续写旧 span。

Safepoint ack 顺序：

```text
stop private bump
  -> publish final top
  -> convert unused tail to FREE padding or side-table retired span
  -> clear thread TLAB pointers
  -> revoke region owner/lease
  -> flush barrier buffers and roots
  -> ack safepoint epoch
```

线程注销也必须执行相同 retire 流程。若 retire 失败，线程不得从 registry 消失，collector 也不得把该 region 放入 collection set。

TLAB refill/retire 继续持 heap lock，对象 fast path 则按 ABI minor 71 的 gate/inflight/top 协议执行；ABI minor 72 的 per-thread target/streak 也只在该持锁 retire/refill 边界读取和修改。Release 构建不保留 shadow verifier；常驻正确性检查和 Debug/Verify 重建必须拒绝非法 sizing policy/bounds、越界或未对齐 target、FIXED target 漂移、同时非零的高低利用率 streak，以及重叠 lease、top 越界、非零 inflight 下观察、未 retire 就 ack、ID lease 重叠和 TLAB 尾部进入普通 free list。

当前代码已落地 reserved-span shadow schema、真实 reserve/publish/retire/refill、ABI minor 71 fast publication 和 ABI minor 72 adaptive target：region 与 thread 同时保存 owner、lease epoch、start/accounted-top/end 和 state，thread 另有原子 fast top、inflight、下一 refill target 与高低利用率 streak；统一 parser 接受 `TLAB_RESERVED`，并在 gate 关闭、inflight 归零和 fast objects flush 后要求 sentinel、双边 metadata 与 accounted top 完全一致。verifier 已能拒绝 region/thread 单边 orphan、top/end 逆序、epoch 不一致、fast top 越界、非法 inflight/gate、ID lease 和 adaptive target 损坏。

Active span 的结构表示固定为内部 `TLAB_RESERVED` sentinel state，而不是 `FREE` 或普通 `ALLOCATING`：

- sentinel header 位于 `reserved_start`，记录整个尚未展开 span 的结构长度。
- `TLAB_RESERVED` 只用于线性 heap walk 跳转，不能进入 mark、reference processing、free list、RemSet 或用户 scanner。
- sentinel 必须与 region/thread side metadata 的 start/end/owner/epoch 完全一致。
- mutator 从 span 前端发布对象时，先把对象区间改写为正常 `ALLOCATING -> LIVE`，再在剩余尾部重建新的 sentinel，最后发布 `published_top`。
- 若尾部不足一个 header，则并入最后一个对象的 `alloc_size`，不能留下无法解析的碎片。
- retire 将剩余 sentinel 原子地转为普通 `FREE + TYPE_NONE` block 并挂入 free list；清空双边 metadata 后才算完成。
- full GC、free-list rebuild、coalesce 和 type walk 遇到 active sentinel 必须按 metadata 跳过或拒绝启动，绝不能把它当垃圾回收。

引入该状态时必须集中增加 `nggc_block_next_locked`/等价结构 walker，逐步替换当前各处手写的 `cursor + alloc_size` 与 LIVE/FREE 二态判断，避免遗漏某条 heap walk 路径。

统一 block parser v0 已实现 header magic、最小长度、alignment、范围溢出、arena 上界和 LIVE/FREE/TLAB_RESERVED state 校验，并接入全部 arena 线性 walker。对于 reserved sentinel，parser 额外验证内部零字段形态以及 region/thread owner、lease epoch、published top、reserved end 和 state。现有损坏 header 测试继续覆盖 parser 失败路径。

第二批收敛已完成 remap、mark epoch wrap reset、bounded assist sweep 和 type-live lookup。当前剩余的结构性旧循环主要是 soft/weak/phantom processing、reference-processing verifier、RemSet rebuild verifier，以及少量仅做对象 API/header 检查的非遍历函数。

第三批已完成 RemSet rebuild、soft/weak/phantom processing 和 reference-processing verifier。当前 `header->magic` 的直接检查只剩统一 parser 本身、从单个对象反查 header 的公开查询，以及 free-list 链表节点验证；arena 线性 walker 已全部经由 block view。随后 parser 已扩展接受 `TLAB_RESERVED`，region 守恒式升级为 `live + free + reserved == committed`，stats ABI minor 31 增加 reserved block/byte 观测。下一步可以集中实现 reserve/publish/retire，而无需逐个修改 collector phase。

## 32. 第三轮整理：终态蓝图与近期施工边界

### 32.1 一句话终态

本项目的目标不是发明一个只能在单一平台跑分的 GC，而是形成一个以 C11 为可移植核心、以 region 为管理单位、以分代并发 tracing 为主路径、以非移动 mark-sweep 为可靠降级底座的组合式回收器：常驻写屏障维护并发标记和跨区信息，读屏障只在搬迁/重映射阶段开启，协作式 safepoint 负责获得可证明的一致状态，所有高级能力都必须能在资源不足或元数据失效时退回到确定路径。

长期组合形态：

```text
regional heap
 per-thread TLAB / young allocation
 generational young evacuation
 concurrent old marking
 selective mixed evacuation
 non-moving large/pinned regions
 always-on write barrier
 phase-gated read barrier
 registered precise scanners
 bounded mutator assist
 adaptive but constrained policy
 strict verifier / fault injection / telemetry
 deterministic degradation and OOM
```

### 32.2 不再讨论、直接视为契约的事项

- 核心最高使用 C11；32-bit 不依赖 pointer tagging、双字 CAS 或 64-bit lock-free atomic。
- 用户类型 ID 为 `1..65535`，`NGGC_TYPE_NONE == 0` 仅供 GC 内部结构状态使用。
- 用户注册 scanner 只枚举 strong slot；weak/soft/phantom 由 GC special object 和 reference-processing phase 统一解释。
- `PhantomQueue` 是虚引用专用特殊类型；只有非空 `PhantomQueue` 拥有内部自保活规则，普通队列绝无此待遇。
- 不提供 finalizer；资源回收通知走 phantom queue/reaper 协议，不允许对象复活语义污染 tracing。
- 写屏障常驻；读屏障只在需要跟随 forwarding 或 remap 的阶段开启；不使用 page fault、`mprotect`、signal 或 SEH 作为核心屏障。
- safepoint 主路径是线程主动 poll/handshake；系统强制挂起只允许用于诊断和无法继续运行的极端 fallback。
- forwarding 初期位于 header；pinned/non-movable 对象永不进入 evacuation set。
- RemSet 的职责、barrier 输入和 overflow 降级语义冻结，内部表示等待 UHMAS 稳定后再定型。
- Release 必须在预处理阶段移除昂贵 verifier、fault injection、trace 和 profiling 代码，而不只是运行时关闭。

### 32.3 当前实现的可信基线

当前可以作为后续工作的不变量基线：

- 固定 64-byte header 与 `ALLOCATING -> LIVE` 发布协议。
- 类型注册、冻结、注销约束和 scanner replay verifier。
- root/thread registry、协作式 safepoint、collector phase 状态机与失败 abort。
- 非移动 mark-sweep、free block split/coalesce 和确定 OOM 返回。
- soft -> weak -> phantom 的固定处理顺序，以及 PhantomQueue 非空内部保活和弱 reaper watch。
- bounded mark/sweep Mutator Assist work packet。
- precise-card/coarse-heap RemSet v0 与独立 rebuild verifier。
- phase-gated forwarding follow、最小 remap 和残留 forwarded slot verifier。
- ABI minor 53 可配置 Region table、跨区属性统计、普通对象不跨 region、allocator owner/lease epoch；默认 Region 仍为 64 KiB。
- ABI minor 60 tail Region reclaim、ABI minor 65 interior humongous run side metadata/伪块 parser/整段 decommit/fixed-address partial recommit/三源守恒 verifier、ABI minor 66 per-heap threshold/禁用配置、ABI minor 67 tail-adjacent fixed-point collapse、ABI minor 68 SMART v0 threshold/cooldown、ABI minor 69 ordered best-fit/filter telemetry，以及 ABI minor 70 external-pressure/VM-latency callback 与 fallback。
- region/thread TLAB shadow metadata，以及 orphan、top 越界、epoch mismatch 故障验证。
- arena 线性遍历已经统一到 block parser；collector 不再各自猜测 block 边界。

这里的“可信”只表示已有实现和测试闭环。ABI minor 71 已启用不取得 heap lock 的 per-thread TLAB 对象 fast path，ABI minor 72 已启用只改变下一 refill 的有界自适应 target，ABI minor 73/74 已启用持 heap lock 的 per-thread card buffer 与常见 TSS owner lookup；但与 mutator 并发的 REMAP、old/mixed evacuation、并发 marking、Region RemSet 和无 heap-lock barrier append 仍未启用。

### 32.4 已完成基础增量：让活动 TLAB span 成为合法堆结构

本基础增量先建立结构正确性；其后的 ABI minor 71 已在相同 sentinel/lease 合同上开放 TLAB fast allocation，完整并发协议见第 35 节。

1. [x] 新增内部状态 `NGGC_OBJECT_TLAB_RESERVED`。
2. [x] 扩展统一 block parser，使其接受 reserved sentinel，并验证 region/thread 两侧的 owner、lease epoch、start、published top、end 和 active/retiring state。
3. [x] reserved block 参与 committed-byte 守恒，但不计入 live/free，不进入 mark、scanner、reference processing、RemSet、sweep 或 free list。
4. [x] region 守恒式改为 `live_bytes + free_bytes + reserved_bytes == committed_bytes`。
5. [x] `nggc_check` 与 stats 增加 reserved block/byte 观测，ABI minor 升至 `31`。
6. [x] 增加合法活动 reserved span 的正向测试，并继续保留 orphan、越界和陈旧 epoch 的反向测试。
7. [x] 真实加锁 reserve/publish/retire 已实现，safepoint 和 thread unregister 会强制退休；ABI minor 71 又加入每对象不取得 heap lock 的 per-thread fast allocation。

reserved span 的结构规则：

```text
[published LIVE blocks][TLAB_RESERVED sentinel ........]
 ^ reserved_start       ^ published_top              ^ reserved_end
```

- sentinel 必须覆盖完整未发布尾部。
- 发布对象时，先构造 `ALLOCATING` header/body，再 release 发布为 `LIVE`，然后建立剩余尾部 sentinel 并推进 published top；具体写入顺序必须由 verifier 能证明 heap walker 永远看到一种合法结构。
- 尾部不足一个 header 时并入最后一个已发布对象的 `alloc_size`。
- retire 把剩余 sentinel 转成 `FREE + TYPE_NONE`，纳入 coalesce/free-list 后，再清空双边 lease metadata。
- safepoint ack、thread unregister、GC phase 接管 allocator ownership 之前必须完成 retire 或由 collector 以可证明方式接管该 span。

当前 reserve/retire v0 实施状态：

- `nggc_internal_tlab_reserve` 在 heap lock 下取得 allocator lease，必要时发布 region tail padding，然后在单个 region 内建立 `TLAB_RESERVED` sentinel。
- reserve 同步推进 arena cursor、region allocation cursor、committed/reserved bytes 和 region/thread 双边 lease metadata。
- `nggc_internal_tlab_retire` 把未发布尾部从 reserved 转成 `FREE + TYPE_NONE`，挂入 free list，并清空双边 metadata。
- `nggc_safepoint` 在 ack 前退休调用线程的 TLAB；`nggc_thread_unregister` 在摘链和释放 thread record 前退休 TLAB。
- verifier 测试覆盖显式 retire、safepoint 自动 retire、unregister 自动 retire，以及每次转换后的完整 heap check。
- `nggc_internal_tlab_alloc_size` 能从 sentinel 前端发布普通小对象，把 consumed bytes 从 reserved 转为 live，并在剩余尾部重建 sentinel；整段正好耗尽时允许 active TLAB 保持零 reserved bytes，随后 retire 只清理 lease metadata。
- large、pinnable 和 non-movable 对象被 TLAB publish 明确拒绝，继续保留给 large/pinned/non-moving slow path。
- 测试覆盖连续发布两个对象、剩余 sentinel、整段耗尽、显式 retire，以及部分发布后的 safepoint/unregister 自动 retire。
- `nggc_internal_thread_alloc_size` 已组合 reserve/publish/retire：无 active TLAB 时自动 refill，剩余空间不足时先退休尾部再申请新 span；默认 span 为 16 KiB。
- 自动 refill 测试连续发布 140 个对象跨越 span 边界，并验证 safepoint 退休后下一次线程分配可以重新 refill。
- ABI minor 32 公开 `nggc_thread_alloc` / `nggc_thread_alloc_size`，调用语义稳定为“已注册 thread + 普通小对象”；ABI minor 71 已在不改变调用者形状的前提下把 frozen-type、owner-matched 小对象替换为 fast path，其余情况仍受控回到 slow path。
- stats 新增 `tlab_refill_count`、`tlab_retire_count` 和 `tlab_allocation_count`，用于后续评估 refill 频率、span 浪费与 fast-path 命中率。
- 当 arena cursor 所在 region 已被另一活动 TLAB 占用时，refill 会把 cursor 后的区尾发布为 free padding，推进到下一个 region 后再建立新 lease；不会复用或覆盖旧 span。
- safepoint epoch 尚有 pending thread 时 allocator lease 保持撤销；已经 ack 的线程若继续调用 thread allocation，会临时走普通 heap-lock allocation，直到 handshake 完成后下一次分配再 refill TLAB。
- C11 多线程测试已经让 4 个线程使用公共 thread-aware API 并发分配，验证独立 region TLAB、barrier、safepoint ack、unregister retire 和最终全量回收。
- ABI minor 33 增加 `nggc_config.tlab_size` 和 `nggc_stats.tlab_size`：`0` 选择默认 16 KiB，若 ABI minor 53 的 Region 更小则收缩到实际 Region；非零值必须按 64-byte 对齐、至少容纳一个 header 且不超过实际 Region。
- heap-level sizing policy 与边界在 open 时冻结；ABI minor 72 SMART 已可在 retire 时提出下一 refill target，但不得修改已经授予的 active span 边界。
- `nggc_thread_alloc*` 是统一分配入口而不是“TLAB-only API”：普通小对象使用 TLAB；超过 region、达到 large threshold、pinnable 或 non-movable 的对象直接调用标准分配器，已有 active TLAB 保持原样。
- 路由测试同时保留 active TLAB，并通过 thread-aware API 分配跨 region large object 与 pinnable object，确认 region 属性和完整 heap walk 一致。
- ABI minor 34 增加 `tlab_refill_bytes`、`tlab_retired_unused_bytes` 和 `tlab_fallback_allocation_count`。计数器采用饱和语义，fallback 只统计成功发布的标准分配对象。
- ABI minor 72 SMART v0 使用目标 span 的 retire `used/unused` 判断大小：两次高利用率增长、三次低利用率收缩；oversized span、large/pinned/non-movable fallback 不参与样本。refill frequency 和 fallback 比例保留给后续更高阶策略，不在 v0 中引入不可解释反馈。
- ABI minor 35 增加 `nggc_thread_root_push` / `nggc_thread_root_pop`。每个已注册 thread 拥有独立 LIFO root stack，slot 不得与 global 或其他 thread root 重复。
- thread roots 与 global roots 一起参与 mark、bounded assist root snapshot、remap 和 residual-forwarding 检查；`nggc_stats.root_count` 报告两者总数。
- thread unregister 要求 root stack 已清空，否则返回 `NGGC_ERR_BUSY`；heap close 会释放仍注册线程的 root storage，但调用者仍应在 C slot 生命周期结束前主动 pop/unregister。
- Debug/Verify registry fault matrix 已增加 thread-root count 超 capacity、重复 slot 和未对齐 slot 三类破坏；hook 在检查后恢复原值，并要求随后的完整 `nggc_check` 成功。
- TLAB/safepoint 交叉不变量进一步冻结：handshake pending 时，已经观察到当前 safepoint epoch 的线程不得仍持 active TLAB；ack 必须发生在 retire 之后。
- TLAB fault matrix 已通过完整 `nggc_check` 验证 reserved sentinel 不能出现在 free-list。ABI minor 61 后，Debug/Verify TLAB verifier 还会显式比较所有 active/retiring TLAB 的 `[reserved_start, reserved_end)` 区间并拒绝重叠 span；测试钩子构造“镜像自洽但区间重叠”的未来多 span 腐败形态，避免只依赖当前一 region 一 TLAB 的结构保证。
- ABI minor 71 后，refill/retire、fallback、统计 flush 和 exclusive observer 仍持 heap lock；满足 frozen-type/owner/gate 条件的每对象 publish 不取得 heap lock。该路径是 heap-lock-free fast path，不承诺所有 C11 atomic 在所有架构上都由硬件 lock-free 实现。

### 32.5 TLAB 之后的启用顺序

```text
reserved sentinel correctness
  -> reserve/publish/retire
  -> safepoint and unregister forced retirement
  -> per-thread fast allocation
  -> object generation / region placement ownership
  -> cross-region/old-to-young RemSet tightening
  -> young collection
  -> promotion and survivor policy
  -> old concurrent mark
  -> selective evacuation and remap
  -> SMART mixed scheduling
```

不能跳序的原因是：没有可验证的 TLAB 退休就无法取得可靠 root/heap 边界；没有单代 region ownership 和可信 RemSet 就不能证明 minor collection 闭包；没有 forwarding、from-space 和 remap verifier 就不能开启真正并发搬迁。

### 32.6 高级功能的失败边界

| 高级能力 | 失败或不可信时的确定动作 |
|---|---|
| TLAB lease/span | retire 或撤销 lease，回到加锁分配 |
| RemSet/card buffer | 标记 coarse dirty，必要时 full-heap scan |
| concurrent mark backlog | 增加 bounded assist，随后提前 remark/full collection |
| evacuation target allocation，尚未发布 forwarding | 放弃该 collection set，region 转 non-moving mark-sweep |
| evacuation 已发布任一 forwarding | 禁止回滚；保持 safepoint 和读屏障，确定补完 copy、REMAP、source release 与 rebuild |
| pin pressure 过高 | 禁止扩大 evacuation，优先 sweep/full mark-sweep |
| soft-reference pressure | 按策略逐级增强清理，emergency 时激进清理 |
| GC worker 创建/运行失败 | 降低并发度，允许单 worker 或 mutator/collector 同步接管 |
| metadata allocation failure | 使用预留 emergency metadata；否则 fail-fast/deterministic OOM |
| SMART 预测失准 | 降低策略置信度，回退 heap-limit/full mark-sweep 策略 |
| verifier 发现不变量破坏 | Debug/Verify 立即失败，不允许带病继续回收 |

### 32.7 文档同步纪律

- `GC-ARCH-PLAN.MD` 记录长期契约、理由、阶段依赖和当前实现基线。
- `PLAN.MD` 只描述近期工程实施顺序和已完成里程碑。
- `TODO.MD` 只保留可勾选工作项，完成状态必须与代码和测试一致。
- 任何提交若改变 object state、header、stats ABI、phase、barrier、reference processing、region 守恒式或降级语义，必须同步检查三份文档。
- 文档不得把“字段已预留”“验证骨架已存在”写成“功能已启用”；分代、TLAB fast path、young collection 和 concurrent evacuation 必须各自经过正向、反向、故障恢复和多平台测试后才能改为已完成。

## 33. 第四轮整理：并发闭环与真实 Safepoint

本章补齐从“线程确认了 safepoint 请求”到“collector 获得可证明的独占观察窗口”之间的缺口。当前原型已经实现 held safepoint v0：请求 epoch、退休 TLAB、ack 后保持 `AT_SAFEPOINT`，并由显式 release 恢复；collector 不再仅凭历史 ack 宣称 mutator 已停止。`nggc_thread_collect` 已实现 collector-owned mutator，`nggc_safepoint_park` 已实现普通 mutator 的阻塞停车和 release 唤醒。

### 33.1 两类停顿语义必须分开

```text
HANDSHAKE
  线程发布某类局部状态后可以继续运行；collector 只能依赖已发布快照。

PARKED_SAFEPOINT
  线程发布全部约定状态后保持停车；在 coordinator 明确 release 前不得执行 mutator 行为。
```

可由 handshake 完成的工作包括配置刷新、统计采样、barrier buffer 轮换和不要求对象图冻结的 epoch 切换。必须使用 parked safepoint 的工作包括精确 root snapshot、最终 remark、需要稳定线程栈/handle 的检查、STW sweep/compact、evacuation plan 提交和全局 remap 收尾。

### 33.2 线程状态机

推荐内部状态收敛为：

```text
UNREGISTERED
  -> RUNNING
  -> SAFEPOINT_REQUESTED
  -> PARKING
  -> PARKED
  -> RELEASING
  -> RUNNING

RUNNING <-> BLOCKED_WITH_PUBLISHED_ROOTS
RUNNING <-> BLOCKED_NO_GC_ROOTS
RUNNING <-> IN_NATIVE
```

状态含义：

- `SAFEPOINT_REQUESTED`：线程尚未观察或尚未完成本地发布，不能计入 parked 数量。
- `PARKING`：禁止取得新 allocator lease，正在退休 TLAB、flush barrier/RemSet buffer 并发布 roots。
- `PARKED`：已 ack 当前 hold epoch；不得分配、修改 root registry、执行普通 barrier 或进入会触碰 GC heap 的 runtime API。
- `RELEASING`：观察到 release epoch，重新装载 phase/barrier 配置并恢复线程本地缓存。
- `BLOCKED_WITH_PUBLISHED_ROOTS` 可由 coordinator 计作已停车，但必须验证其 root/TLAB/buffer 已按当前 epoch 发布。
- `BLOCKED_NO_GC_ROOTS` 只有在线程保证不持有 GC pointer 且不访问 GC heap 时才能免于 root 扫描。

线程的 `observed_epoch`、`acked_epoch` 和 `parked_epoch` 必须分别存在或具有等价可验证表示，不能用一个字段同时表达“看见请求”“完成发布”和“仍处于停车状态”。

### 33.3 请求、停车与释放协议

```text
coordinator:
  allocate next hold_epoch
  publish safepoint request (release)
  revoke global/region allocator leases
  wait until required_thread_count == parked_thread_count
  verify every parked thread belongs to hold_epoch
  enter STW phase

mutator poll:
  observe request (acquire)
  stop private allocation
  retire active TLAB and publish final top
  flush SATB/card/RemSet/assist-local buffers
  publish thread roots and native state
  set parked_epoch and PARKED (release)
  wait until release_epoch >= parked_epoch
  reload barrier/phase state (acquire)
  return to RUNNING

coordinator exit or abort:
  finish/abort active phase into a consistent state
  publish release_epoch (release)
  wake parked threads
  verify parked_thread_count returns to zero
```

任何成功、OOM、metadata failure、scanner failure、worker failure、用户中止和 verifier failure 路径都必须有确定的 release 动作。Debug/Verify 构建应把“带着 parked 线程返回 API”视为严重错误。

### 33.4 Collector 调用模型

不能直接把现有 `nggc_safepoint()` 改成无限等待释放，因为当前同步测试和嵌入式调用可能由同一个已注册线程随后调用 collector，这会自锁。实施前必须明确区分两种角色：

- 专用 coordinator/GC worker：不作为普通 mutator 参与停车，可以请求并持有 parked safepoint。
- 发起同步 collection 的 mutator：通过 `nggc_thread_collect` 将自身转换为 `COLLECTOR_OWNED`，发布 roots、退休 TLAB 后执行 collection；它计入 parked 守恒，但不计入等待自己的 pending 集合。

公开 API v0 已拆出 `request`、`poll_and_park`、`wait_all_parked`、`release` 四个动作的等价语义。`nggc_safepoint()` 只完成逻辑 park 并返回，适合自定义调度器；`nggc_safepoint_park()` 会阻塞到 release；`nggc_thread_collect` 额外提供 owner 身份绑定。

### 33.5 停车期间的 API 门禁

线程处于 `PARKING/PARKED` 时：

- 禁止 `nggc_thread_alloc*`、普通 `nggc_alloc*` 和 TLAB refill。
- 禁止 push/pop global 或 thread root；root snapshot 必须保持稳定。
- 禁止用户主动调用写屏障或读屏障访问 heap。
- 只允许 release wait、诊断查询和 coordinator 明确列入白名单的内部动作。
- unregister 默认返回 `NGGC_ERR_BUSY`；只有 coordinator 在 release/abort 路径中执行受控摘链时才可例外。

门禁必须在 Release 保留低成本检查，因为这是正确性协议，不属于可宏移除的昂贵 verifier。

### 33.6 分配与 GC 的统一所有权规则

分配器、collector 和线程停车共享一条所有权原则：任何线程只有在持有当前 epoch 的有效 lease 时才能修改私有分配区；任何 collector 只有在相应 lease 已撤销、TLAB 已退休或显式接管后才能解释该区域。

```text
RUNNING + valid TLAB lease
  -> private publish permitted

safepoint pending / PARKING / PARKED
  -> no new lease, no private publish

collector owns region/collection set
  -> mutator allocation and mutation forbidden for that ownership epoch
```

ABI minor 71 TLAB fast path只移除每对象 heap lock，不改变 `tail sentinel -> ALLOCATING -> initialize -> release LIVE -> release fast_top` 的发布顺序，也不改变 safepoint 前“关闭 gate、等待 inflight、flush、retire”的要求。共享 gate/inflight 使用 32 位以内 C11 atomic，对象 ID 通过持锁 refill 租赁，fast path 不要求共享 64 位原子；`uintptr_t` top 即使不是硬件 lock-free，也依赖标准 C11 atomic 语义而不是 pointer tagging 或撕裂读写。

### 33.7 Root、扫描函数与并发阶段边界

- global/thread root registry 的结构修改只允许在 `RUNNING` 且未进入 root snapshot 冻结窗口时发生。
- scanner 只枚举 strong slot；weak/soft/phantom 继续由 special object 和 reference-processing phase 统一处理。
- concurrent mark 可扫描已经发布为 `LIVE` 的对象；不得扫描 `ALLOCATING` 或 `TLAB_RESERVED` 尾部。
- scanner 不得缓存 visitor/context，不得重入 GC，不得阻塞等待 parked mutator，也不得自行解释 forwarding。
- moving phase 中 scanner 上报的是 slot 地址；读取、修正和 CAS/锁定写回由 GC visitor 按 read-barrier/forwarding 协议完成。

### 33.8 SMART 策略与强制安全壳

SMART 只能选择“何时启动、预算多少、优先哪些 region、soft reference 清理强度和 assist 比例”，不能修改正确性状态机。以下条件具有最高优先级并覆盖预测结果：

- 可用空间低于 evacuation reserve：禁止扩大 moving collection set。
- RemSet 不可信或 overflow：升级 coarse dirty/full scan。
- parked safepoint 超时：记录未响应线程；默认不扫描不一致堆，不强行继续 moving GC。
- metadata reserve 低于下限：停止创建可选元数据，转 non-moving full mark-sweep。
- 连续预测失准或 pause 超预算：降低 SMART 置信度，回退明确阈值策略。
- 分配即将失败：允许 bounded assist、激进 soft clear、full mark-sweep 和 emergency reserve，最后确定性 OOM。

策略决策必须输出 reason code 和关键输入快照，以便 Profile/Debug 解释；Release 可保留固定大小的低成本计数器，但完整 trace 继续由宏移除。

### 33.9 下一阶段验证清单

真实 park/release 协议落地前后必须覆盖：

1. pending count、parked count 与逐线程状态一致。
2. ack 当前 epoch 的线程仍为 `RUNNING` 时 verifier 失败。
3. `PARKED` 线程持有 active TLAB、allocator lease 或未 flush buffer 时 verifier 失败。
4. stale parked/release epoch、重复 ack、重复 release 和跨 cycle ack 被拒绝。
5. 分配、root mutation、barrier 和 unregister 在停车期间被门禁。
6. collection 成功、OOM、scanner failure、worker failure 和显式 abort 均释放全部线程。
7. collector-owned mutator 模式不会等待自身，专用 coordinator 模式不会被误计为 mutator。
8. 多线程压力测试证明 release 后线程能够重新获取 lease/refill TLAB，且旧 epoch 不能复用。
9. MSVC x64/Win32、GCC/MinGW 的 Debug、Release、Verify/Profile 组合继续通过。

### 33.10 Held Safepoint v0 实施状态

- ABI minor 36 新增 `nggc_safepoint_release`，stats 新增 `safepoint_active`、`safepoint_parked_threads` 和 `safepoint_release_count`。
- `pending + parked == registered` 在 safepoint active 时成为常驻 phase invariant；inactive 时 pending/parked 必须同时为零。
- 线程 ack 前退休 TLAB，ack 后保持 `AT_SAFEPOINT`；重复 poll 不会重复增加 parked count。
- partial release 返回 `NGGC_ERR_BUSY`，重复 release 返回确定错误；注册、注销、分配、root mutation 和 barrier 在 hold 窗口被拒绝。
- auto-collect 只有在全部注册线程仍 parked 时进入 collector cycle，并在成功或 abort 后统一 release；手动 hold 跨 collection 保持有效，直到调用者显式 release。
- verifier 会从逐线程 epoch/state 重建 pending/parked 数；phase fault matrix 已覆盖非法 active 标志和 parked count。
- 多线程测试证明 coordinator release 前 mutator 不进入分配，release 后能够重新 refill TLAB 并继续运行。
- ABI minor 37 新增 `nggc_thread_collect` 和 `safepoint_collector_owned_threads`；owner 身份绑定到 hold epoch，普通 `nggc_collect` 和其他 thread handle 均不能接管未完成 cycle。
- C11 多线程测试由已注册 owner 发起 collection，四个 worker 分别 ack；owner 在 pending 清零后完成 collection 并统一 release，随后 worker 继续 TLAB 分配。
- ABI minor 38 新增 `nggc_safepoint_park` 和 `safepoint_release_epoch`。支持 C11 threads 的平台使用 condition variable，release 在同一等待锁下发布 epoch 并 broadcast；不提供可选 C11 threads 的平台编译原子等待 fallback。
- 多线程测试中的 worker 已直接阻塞在 `nggc_safepoint_park`，不再使用测试侧 release flag；这覆盖了 park、广播唤醒、owner release 和恢复后 TLAB refill 的完整链。
- heap close 在仍有注册线程或活动 safepoint 时返回 `NGGC_ERR_BUSY`，避免销毁仍被 waiter 使用的 condition/heap 状态。
- Verify+Test Hooks 包含普通 abort 的四个一次性 collector fault，以及 moving cycle 的五点提交/恢复矩阵。提交前故障要求 phase/cycle 回到 `IDLE` 且对象不增龄；提交后故障要求 emergency completion 后回到 `IDLE`、auto hold 完整 release、对象图与 RemSet 有效，并且后续正常 collection 成功。

ABI minor 88 已完成 scanner callback 错误/协作取消协议、Verify-only 同线程 API 重入诊断，以及 worker pool 运行路径的 condition wait/broadcast、`thrd_join` 失败注入和 close 重试矩阵。一次性 `thrd_create` 返回失败、失败后的下一轮恢复建池和持久 GC worker pool 已落地。核心 abort-release、对象 generation/Region placement 分离、跨代 RemSet coverage、可配置 Region、STW young evacuation、post-forwarding emergency completion、forwarding CAS/epoch、精确目标预留、STW 内持久池并行 copy/REMAP、ABI minor 54/55/56 受限 copy window、注册线程 resume/repark 状态和 poll/park 协作重停车、跨 safepoint delayed reclaim 已经闭环；仍待 scanner 超时/阻塞、跨线程滥用与缓存上下文诊断、未报告的 worker 异常退出模型、并发 root/handle/RemSet 闭包、mutator-concurrent REMAP 和真正并发 marking。

## 34. ABI minor 54/55/56：受限 Mutator-Concurrent Copy Window 与协作重停车

ABI minor 54 是从 parked-safepoint 内部并行到真正 mutator-concurrent moving 的中间增量；ABI minor 55 在此基础上增加 registered-mutator 的 EVACUATE resume/repark 状态骨架；ABI minor 56 将它升级为由 resume epoch 唤醒、由 mutator poll/park 协作完成的 repark handshake。它只放开已经能证明安全的 EVACUATE copy 子阶段，不改变 root、分配、TLAB、REMAP、SWEEP 或 collector ownership。

### 34.1 已实现协议

```text
parked safepoint
  -> preflight and reserve every destination under heap lock
  -> publish destination headers and region accounting
  -> set mutator_concurrent_active + evacuation epoch
  -> AT_SAFEPOINT -> CONCURRENT_RUNNING for acknowledged mutators
  -> publish safepoint_resume_epoch and wake parked mutators
  -> release heap lock for EVACUATE copy window
  -> mutators may execute nggc_load/store only
  -> worker locks source payload and copies body
  -> destination LIVE release-store
  -> source forwarding release-CAS
  -> worker quiescence
  -> request cooperative repark
  -> mutator safepoint poll/park transitions back to AT_SAFEPOINT
  -> reacquire heap lock
  -> prove parked/running/repark counts
  -> finalize / REMAP / SWEEP under existing parked protocol
```

对象头新增的 `payload_lock` 是 C11 `atomic_flag`。copy worker 在读取 source payload 时持有它；并发窗口中的 `nggc_load` 先锁定 slot 所属 source payload，取得锁后重新解析 owner forwarding、按字段 offset 映射 canonical slot，必要时继续锁定目标 payload，再读取字段及解析 referent。`nggc_store` 锁定 owner/value 相关 payload，再执行常驻写屏障、canonicalization 和 slot 写回。heap lock 仍保护 phase、region、RemSet 和 collector 元数据；payload lock 只保护对象体与复制之间的交叉访问，不替代 heap lock，也不使裸 payload 写入变得安全。

读屏障只在 `EVACUATE`/`REMAP` 或 from-space quarantine 条件满足时开启。并发窗口允许的 mutator 操作严格限于 `nggc_load/store`：普通分配、root mutation、thread registry、TLAB 操作和其他会改变 collector ownership 的 API 仍返回 `NGGC_ERR_BUSY` 或保持 parked。窗口结束前必须验证 worker epoch、active worker、task quiescence 和 forwarding 提交状态。

### 34.2 仍未完成的完整协议

当前状态机已经允许已 ack 的 registered mutator 在 EVACUATE copy window 内通过 `nggc_safepoint_park` 解停，并在 collector 请求后通过 `nggc_safepoint` / `nggc_safepoint_park` 协作重新停车。但这不是完整的 application-thread resume contract；窗口外 `safepoint_active` 仍要求所有 registered mutator parked。因此下列能力不能宣称已经完成：

- 并发 root/handle 发布、TLAB retire/refill、分配门禁和 barrier buffer flush；
- mutator-concurrent REMAP、RemSet 更新与 from-space reclaim；
- scanner、reference processing 和用户 API 在完整 moving cycle 中的并发重入协议；
- copy/REMAP 失败后的自动恢复、quarantine 观测与部分恢复协议；ABI minor 80 已提供 fail-stop 状态查询和最终 force-destroy，但尚未提供在 fail-stop 中继续运行或恢复收集的能力。

后续实现必须先加入 registered-mutator 状态机和操作 epoch，再把 `REMAP` 从 parked-only 阶段拆成可增量验证的工作包。任何阶段若不能证明所有 root、strong slot、RemSet 和 barrier buffer 已纳入闭包，必须保留读屏障并降级到 full scan/mark-sweep，不能直接关闭屏障或复用 from-space。

### 34.3 统计、验证与构建边界

`nggc_stats` 增加 `mutator_concurrent_active`、`mutator_concurrent_epoch`、窗口次数、load/store 次数、rejected 次数，以及 `mutator_concurrent_running_threads`、`mutator_concurrent_repark_pending_threads`、`mutator_concurrent_repark_requested`、resume/repark 累计计数。Verify+Test Hooks 增加窗口闸门和 phase fault，用于确定性检查注册线程在 EVACUATE 期间的解停、屏障 API 可进入、root/分配/TLAB 门禁拒绝、repark 请求后的协作停车，以及窗口退出后的 phase/epoch/quiescence 清零；这些测试 hook 在 Release 代码级移除。Region 配置仍保持 ABI minor 53 的语义：`region_size == 0` 使用默认 64 KiB，非零值必须满足 card、对象对齐、表示范围和 32-bit 边界约束。

验收最低要求：Debug/Verify 的 phase verifier、Release/Profile 全量 CTest、Win32 交叉构建，以及无 C11 threads 时的同步回退均通过。Win32 Debug 测试运行需要目标机提供对应版本的 x86 MSVC Debug CRT；这属于测试运行时前置条件，不是 GC 对 32-bit 原子协议的额外假设。

### 34.4 ABI minor 88：repark 等待谓词与真实复制交错验证

`safepoint_resume_epoch` 表示本 hold epoch 曾经放行 copy window，不等同于该 epoch 的最终 release。原来的 park wait 同时接受 resume/release，导致已回到 `AT_SAFEPOINT` 的线程重复消费仍匹配的 resume epoch，在最终释放前反复获取 heap lock。修复在持 heap lock 检查线程状态时冻结 `allow_resume`：首次停车仍等待 resume 或 release；同 epoch 已放行后重新停车只等待 release。保留已发布的 resume epoch，避免初次 waiter 尚未醒来时因清零而漏掉放行；等待条件仍在 condition lock 内 acquire 重查，虚假唤醒重新检查谓词。

独立 `nggc_concurrent_copy_tests` 使用四个各自在原生线程注册、分配和注销的 mutator，类型表冻结，Region 为 24 KiB，普通对象由 thread roots 保活。Verify+Test Hooks 在三个位置暂停 collector：持 source payload lock 且复制前、forwarding 发布并释放 payload lock 后、全部线程重新停车但 REMAP 尚未开始。测试侧直接观察写屏障已到达被占用的 source lock，再放行复制；写屏障必须重新解析 forwarding 并把写入提交到 destination，随后读回和最终图检查均保留该写入。

每轮另外执行四线程合计 1024 次 barrier store 和对应 load，允许 referent 在两个 load 之间发布 forwarding，因此即时比较按 source/canonical 身份等价判断；最终 parked REMAP 后则严格要求所有 thread root 和引用槽为 canonical。三轮 collection 检查全部 payload 字节、存活年龄达到阈值才 OLD、Pin lease/地址稳定、pinned owner 的跨区引用修正和 worker/RemSet 归静。最后先以后续 collection 完成 delayed quarantine，再修改 Pin lease。

重新停车闸门通过仅测试构建中的 condition-wait 计数，证明四个已解停线程再次进入阻塞等待，而非只验证逻辑 parked 计数。所有闸门、竞争观测字段、等待计数和辅助 API 在正式 Release 预处理时移除；等待谓词修复保留于正式构建。该验证覆盖并发 copy 与随后全员 parked 的并行 REMAP，完整 mutator-concurrent REMAP、并发 root/handle/TLAB publication 和 TSAN 实测仍是后续工作。移锁继续暂缓。

## 35. ABI minor 71：Frozen-Type Per-Thread TLAB Fast Allocation

ABI minor 71 在既有 `TLAB_RESERVED`、双边 lease metadata 和加锁 reserve/publish/retire 正确性基线上，加入真正不取得 heap lock 的每线程小对象分配路径。这里的“fast”准确表示每对象路径不获取 NGGC heap lock；它不声称 C11 实现在所有 CPU 上都会把每一种原子类型编译成单条硬件无锁指令，也不改变 refill、retire、fallback、统计 flush 和 collector observer 仍需串行化的事实。

### 35.1 启用条件与非目标

fast path 只有同时满足以下条件才可进入：

- 构建提供 C11 `<threads.h>`，注册时可以把 `nggc_thread` 绑定到 `thrd_current()`；
- 类型表已经通过 `nggc_type_table_freeze()` 永久冻结并完成 release publication；
- 调用者是注册该 `nggc_thread` 的 C11 owner thread；
- heap-level `tlab_fast_gate` 为 `OPEN`；
- thread 状态为 `RUNNING`，且持有一个合法 active TLAB；
- 对象类型已注册、尺寸合法，且不是 large、pinnable 或 non-movable；
- heap 不在 safepoint、collector cycle、mutator-concurrent moving、from-space 接管或 Mutator Assist 排他状态。

不提供 C11 threads 的构建保持原来的持锁 thread-allocation 路径。类型表未冻结时也继续持锁读取类型描述符，不能为了 fast path 与注册/注销并发。foreign owner 的分配、collector-owned collection 和注销返回 `NGGC_ERR_INVALID_ARGUMENT`；safepoint coordinator API 是否代表 mutator owner 是另一层角色合同，不借此 owner 规则强制等同。

Region 尺寸不是 fast path 常量。每个 heap 在 open 时从 `nggc_config.region_size` 冻结实际尺寸：`0` 使用 64 KiB 默认值；显式尺寸不要求为 2 的幂，但至少为 512 bytes，满足 64-byte 对象对齐和 512-byte card geometry，并通过 arena、TLAB、metadata、`uintptr_t`、`size_t` 和 32-bit Region 计数边界校验。默认 TLAB 为 `min(16 KiB, region_size)`，显式 TLAB 必须不超过实际 Region。

### 35.2 状态与字段语义

heap 侧关键状态：

- `type_table_published`：冻结完成后的 release/acquire publication bit；
- `tlab_fast_gate`：`CLOSED/OPEN` 两态的 C11 `atomic_uint`；
- `next_object_id`：只在 heap lock 下推进的全局 ID 高水位；
- `allocation_count`：成功发布并已纳入 heap/Region 统计的真实对象数。

thread 侧关键状态：

- `owner_thread + owner_thread_bound`：注册该 handle 的 C11 thread identity；
- `tlab_fast_inflight`：已经穿过第一道 gate 检查、尚未完成或退出的 fast operation 数；
- `tlab_fast_top`：mutator 以 release store 发布的真实对象尾端；
- `tlab_published_top`：heap lock 下已经 flush 入 Region/heap counters 的 accounted top；
- `tlab_object_id_next + tlab_object_id_remaining`：本次 span 尚未使用的 thread-local ID lease。

Region 的 `tlab_published_top` 与 thread 普通字段保持一致，只表示 accounted top。collector、verifier 和普通线性 heap observer 不得直接假定它总等于 mutator 的 fast top；它们必须先关闭 gate、等待 inflight 清零并执行 flush，之后两者才重新收敛。

### 35.3 Mutator 发布协议

单个 TLAB 只有注册 owner 可以分配，因此同一 span 不需要为 top 做竞争 CAS。fast path 的顺序固定为：

```text
acquire type_table_published and gate == OPEN
  -> inflight++ (acq_rel)
  -> acquire recheck type publication and gate
  -> validate stable thread/TLAB/type state
  -> compute aligned allocation and remaining tail
  -> consume one thread-local object ID
  -> initialize new TLAB_RESERVED tail sentinel, if any
  -> rewrite old sentinel as ALLOCATING object
  -> initialize header, forwarding state, payload lock and payload bytes
  -> release-store object state = LIVE
  -> release-store tlab_fast_top = object_end
  -> inflight-- (release)
```

先建立新尾部 sentinel，再改写旧 sentinel，保证 owner 完成发布后 arena 仍是：

```text
[accounted LIVE objects][not-yet-accounted LIVE objects][TLAB_RESERVED tail]
 ^ reserved_start       ^ accounted_top             ^ fast_top          ^ reserved_end
```

若剩余尾部不足一个固定 header，则把这点字节并入最后一个对象的 `alloc_size`，不会留下无法解析的小碎片。对象只有在 header/body 初始化完毕后才以 release store 进入 `LIVE`；observer 不扫描 `ALLOCATING`，也不会在 gate/inflight 证明之前越过 accounted top。

### 35.4 Gate、Quiescence 与 Deferred Flush

任何需要一致 heap/TLAB 快照或会修改 allocator/collector 所有权的入口都执行同一前缀：

```text
hold heap lock
  -> atomic exchange gate OPEN -> CLOSED (acq_rel)
  -> visit registered thread list
  -> wait until every tlab_fast_inflight == 0 (acquire)
  -> acquire-load each tlab_fast_top
  -> walk [tlab_published_top, tlab_fast_top)
  -> validate and batch-account every LIVE object
  -> perform stats/check/refill/retire/phase/collection operation
```

当前使用该前缀的关键入口包括 safepoint request、collection preparation、thread-owned collection、phase begin、`nggc_check`、`nggc_get_stats`、标准/large/internal allocation slow path、显式 TLAB reserve/alloc/retire、thread allocation refill/slow path 和 owner thread unregister。普通 `nggc_load` / `nggc_store` 不会为每一次屏障关闭全局 fast gate，因此常驻写屏障与 phase-gated 读屏障仍可和其他 mutator 的 TLAB fast allocation 并行。

flush 对每个尚未入账对象验证：block 为 `LIVE`、type 已注册、flags 与冻结描述符一致、对象为 `YOUNG age=0`、无 forwarding、非 large/pinnable/non-movable、payload/alloc size 和 object ID 均合法。全部预检通过后才批量更新 Region reserved/live bytes、Region/heap live object、heap payload bytes、allocation count、Mutator Assist debt 和 TLAB telemetry，并把两侧 accounted top 推进到 fast top。

gate 只能在类型表已冻结且 heap 回到安全的 `IDLE` 条件后以 release store 重新开放。safepoint release 必须先成功发布 release epoch并唤醒 waiter，随后才可 reopen。若一致性检查发现 corruption，保守地保持 gate 关闭是安全降级；错误堆不能为了恢复性能盲目重新开放 fast publication。

### 35.5 C11 内存模型与 32-bit 边界

类型冻结在完整写入所有 descriptor 后 release-store `type_table_published = 1`；fast path 的 acquire load使后续 descriptor 读取有 happens-before 保证。gate close 使用 acq_rel exchange，observer 对 inflight 的 acquire load与 mutator 最后的 release decrement 配对；随后对 fast top 和 object `LIVE` 的 acquire 读取观察完整发布。

协议不使用双字 CAS、128-bit atomic、共享 64-bit fetch-add 或依赖 64-bit lock-free 的 `(top, epoch)` 组合。共享 gate/inflight 只需要很小的 `atomic_uint` 状态；地址 top 使用标准 `_Atomic(uintptr_t)`。即使某个平台通过 C11 runtime 实现该原子类型，正确性仍由 C11 语义保证，性能能力可以在后续 backend/platform profile 中判定并选择持锁 fallback。

该协议不使用 page fault、`mprotect`、signal、SEH 或故意访问不可读页来探测 publication。decommitted humongous run 继续由 Region side metadata 在解引用前识别，和 TLAB gate 是两套显式状态机。

### 35.6 Object-ID Lease 与统计语义

TLAB refill 在 heap lock 下按 `span_size / NGGC_OBJECT_HEADER_BYTES` 租赁一个保守上界 ID 段。因为任何合法对象至少占一个 header，该段足以覆盖 span 中可能发布的对象数。fast path只推进 thread-local lease；retire 丢弃未使用后缀，形成的 ID 空洞合法、不会回收或复用。

因此两个计数不得混淆：

- `next_object_id` 是所有普通分配和 TLAB lease 的全局高水位，允许大于真实分配次数；
- `allocation_count` 只在普通对象成功发布，或 fast objects 被 locked flush 确认后增加。

`nggc_get_stats` 本身会关闭 gate 并 flush，因此它返回的是一致快照，不是任意时刻无干扰读取。fast allocation 到下一次 flush 之间，Region/heap live counters 和 Mutator Assist debt 允许延迟；延迟上界受每线程 active TLAB span 限制，refill 必然经过 slow path 并先 flush。

### 35.7 降级与失败语义

- 类型表未冻结：直接使用原持锁路径，不开放 descriptor 并发读取。
- 无 active TLAB、span 不足：关闭 gate、flush、retire/refill 后重试 locked publish。
- large/pinnable/non-movable：路由标准分配器，不破坏已有 active TLAB。
- gate 已关闭或 heap 不在允许状态：fast attempt 失败，slow path按 safepoint/phase 门禁返回 `BUSY` 或在合法状态处理。
- foreign thread handle：分配、collector-owned collection 与注销均拒绝；不能让另一个 OS thread 在 owner 可能进入 fast/slow allocation 时释放 handle或改变 collector 角色。
- ID lease 耗尽或全局 ID 高水位溢出：确定返回 OOM，不复用旧 ID。
- flush 发现非法 block/top/type/lease/counter：返回 corruption 并保持保守门禁；Release 正确性不依赖 Debug verifier 代为修复。

### 35.8 Telemetry、验证与 Release 边界

ABI minor 71 的 `nggc_stats` 新增：

- `tlab_fast_allocation_count`；
- `tlab_fast_slow_path_count`；
- `tlab_fast_flush_count`；
- `tlab_fast_flushed_object_count`；
- `tlab_fast_gate_close_count`；
- `tlab_fast_wrong_owner_count`。

Debug/Verify TLAB fault matrix新增 fast top bounds、non-zero inflight、object-ID lease 和非法 gate 状态。确定性 test hook 可以让 mutator 在 `inflight++` 后暂停，随后由另一线程请求 safepoint；测试必须先观察 gate 已关闭，再释放 mutator，并证明 request 只在 publication 完成、inflight 归零和对象 flush 后返回。最终 `allocation_count == 2`、fast/flushed object 均为 1、active TLAB 为 0，证明该对象既未丢失也未重复入账。

多线程回归使用 24 KiB 非 2 的幂 Region 和 4 KiB/默认 TLAB，4 个 owner thread 并发分配，验证 per-thread span、foreign handle 拒绝、gate close、safepoint retire 和完整 heap walk。当前 Region suite 覆盖 512 B、8 KiB、24 KiB、64 KiB 默认、128 KiB 和 1 MiB 配置。

pause/fault 字段、分支和 hook 只存在于 `NGGC_ENABLE_VERIFY && NGGC_ENABLE_TEST_HOOKS` 测试目标，正式 Release 库在预处理阶段代码级移除。gate、inflight、owner、publication、flush 和 retire 检查属于运行时正确性协议，不能随 verifier 一起删除。

### 35.9 后续深化顺序

ABI minor 71 已解决“每对象都拿 heap lock”；其后的第一项 sizing 深化已由 ABI minor 72 完成，但还没有消除所有全局干扰。下一批按以下顺序推进：

1. 把全局 flush 细化为按 thread/span 的定向 flush，并评估只读 stats 的低干扰快照；任何优化仍需保留 safepoint 的全局 quiescence 证明。
2. per-thread card publication buffer、safepoint/repark flush与overflow coarse降级已由ABI minor 73完成，常见owner lookup已由ABI minor 74的TSS per-heap context完成，未满buffer的预留card append、线性去重和global-card coverage读取已由ABI minor 75/76/77移到heap unlock后，分类共享64-bit telemetry和Region/generation分类推导已由ABI minor 78改为锁外32-bit事件；后续继续拆出reservation/slot publication，并增加按epoch的remark flush和Region RemSet。
3. 将 Mutator Assist debt 细分到线程并提供公平预算，避免单个 refill/flush 承担无界工作。
4. 在 root/handle/barrier-buffer 闭包完成后，才把 ABI minor 56 copy-only window 扩展到 concurrent REMAP 和完整并发 moving；TLAB fast path 和自适应 sizing 本身都不能替代这条闭包证明。

## 36. ABI minor 72：Per-Thread Adaptive TLAB Sizing

ABI minor 72 在 ABI minor 71 的 fast publication 上增加一个受约束、可解释、无需时钟的 SMART sizing v0。它只决定每个注册线程下一次 refill 的目标长度，不改变 `TLAB_RESERVED` sentinel、object-ID lease、fast gate/inflight/top、对象 release publication、safepoint retire 或 Region 所有权协议。调整发生在 heap lock 已持有、fast gate 已关闭、该线程 inflight 已归零并完成 flush 的 retire 路径，因此没有新增 mutator/collector 数据竞争。

### 36.1 配置兼容与 Region 几何

公共配置新增：

- `tlab_sizing_policy`：`AUTO(0)`、`SMART(1)` 或 `FIXED(2)`；
- `tlab_size`：FIXED 的固定目标，或 SMART 的 initial target；
- `tlab_min_size` / `tlab_max_size`：SMART 的闭区间边界。

解析规则固定为：

| 输入 | resolved policy | 语义 |
| --- | --- | --- |
| `AUTO + tlab_size == 0` | SMART | 零初始化配置默认获得自适应策略 |
| `AUTO + tlab_size != 0` | FIXED | 保持 ABI 71 及以前显式 `tlab_size` 的固定粒度预期 |
| 显式 SMART | SMART | `tlab_size` 是初值，0 使用默认初值 |
| 显式 FIXED | FIXED | `tlab_size` 是固定值，0 使用默认值；min/max 必须为 0 |

默认 initial 为 `min(16 KiB, region_size)`，SMART 默认 min 为 `min(4 KiB, initial)`，默认 max 为 `region_size`。resolved 几何必须满足：

```text
64-byte aligned
header_size <= min <= initial <= max <= region_size
```

FIXED 在 heap 内规范化为 `min == initial == max`。Region 继续是 per-heap 可配置值，不要求 2 的幂；只要满足 ABI minor 53 的 512-byte card geometry、64-byte object alignment 和表示范围，512 bytes、24 KiB 等尺寸都合法。TLAB target 同样不要求为 2 的幂，减半时必须显式向下对齐到 64 bytes 后再夹到 min，不能假定整数除法仍保持对象对齐。

### 36.2 Per-Thread 状态与活动 span 不变量

heap 保存 resolved policy、initial/min/max 和四个饱和计数器：sample、grow、shrink、hold。每个注册线程保存：

- `tlab_target_size`：下一 refill 的目标；注册时取 heap initial；
- `tlab_high_utilization_streak`：高利用率连续样本，合法值为 0 或 1；
- `tlab_low_utilization_streak`：低利用率连续样本，合法值为 0、1 或 2。

高低 streak 不得同时非零。FIXED 线程要求 target 恒等于 heap initial 且两个 streak 恒为零。SMART target 始终位于 min/max 内并满足 64-byte alignment。

已经授予的活动 span 仍以 `[reserved_start, reserved_end)` 为唯一边界。target 改变不会伸缩、拆分、迁移或重新解释该 span，也不会改变 fast mutator 正在读取的 top/end。若一个对象本身大于 target 但不超过 Region，slow path 可以为它建立 `reserve_size = alloc_size` 的 oversized span；该 span 的 retire 不作为 sizing 样本，避免单个异常对象把普通分配目标永久放大或缩小。

### 36.3 Retire-Time 滞回算法

只有 `actual_span_size == thread->tlab_target_size` 时才采样。设：

```text
used   = published_top - reserved_start
unused = reserved_end - published_top
span   = reserved_end - reserved_start
```

分类与动作：

1. `unused <= span / 8`，即利用率至少约 87.5%：清零 low streak；连续第二个高样本将 target 倍增并夹到 max。
2. `used <= span / 2`，即利用率不超过 50%：清零 high streak；连续第三个低样本将 target 减半、向下对齐 64 bytes并夹到 min。
3. 中间区间：清零两个 streak并保持 target。
4. 已在 max 上收到高样本，或已在 min 上收到低样本：保持 target并清零对应 streak，避免计数无限增长。

每个有效样本恰好归类为 grow、shrink 或 hold；阈值前的 streak 样本计为 hold。增长使用“先与 `max / 2` 比较再乘 2”的形式避免 `size_t` 溢出，利用率判断使用除法和剩余量比较，不计算 `used * 8`。所有计数器采用饱和语义。

关键 refill 顺序为：

```text
active span allocation reports OOM
  -> close gate / flush
  -> retire old span
  -> observe utilization and possibly update thread target
  -> recompute reserve_size from the new target
  -> reserve the immediately following span
```

不得在 retire 前缓存 reserve size。确定性测试曾专门覆盖 `4 KiB -> 8 KiB` 的增长点；若沿用旧快照，新 target 会错误地延迟一轮生效。

### 36.4 Fast Path、Safepoint 与统计边界

ABI minor 71 fast path 不写 sizing 字段。它只推进 fast top、消耗 thread-local object ID，并按原协议 release 发布对象。任何 retire 前都先关闭 gate、等待 inflight 为零并 flush accounted top，因此 sizing 看到的是本 span 的最终、完整利用率。

显式 retire、自动 refill、safepoint ack 前 retire 和 owner unregister retire 都可产生 SMART 样本；但 inactive thread 的重复 retire 不产生样本。safepoint 本身不会为了策略而延长活动 span，也不会在 mutator 未归静时读取 target/streak。

`nggc_stats` 新增：

- resolved `tlab_sizing_policy`；
- `tlab_size` initial、`tlab_min_size`、`tlab_max_size`；
- 当前所有注册线程 target 的 `tlab_target_min_size/tlab_target_max_size`，无线程时二者为 0；
- `tlab_sizing_sample_count/grow_count/shrink_count/hold_count`。

stats 延续 ABI minor 71 的一致快照语义：先关闭 gate并 flush，再读取线程 target。它不会为了读统计而 retire active TLAB，因此不会伪造利用率样本。

### 36.5 验证、故障注入与覆盖

常驻 TLAB verifier 校验 resolved heap policy/几何和每线程 target/streak；这些是 Release 正确性检查，不能依赖 Debug 重建代替。Verify+Test Hooks 的 `NGGC_TEST_TLAB_FAULT_ADAPTIVE_TARGET` 会把线程 target 破坏为 0，要求完整 `nggc_check` 返回 corruption，并在恢复 snapshot 后再次通过。snapshot 同时保存 target 与两个 streak，故障恢复不得遗留策略状态。

确定性正向测试使用 24 KiB 非 2 次幂 Region、4 KiB initial、2 KiB min、16 KiB max 和 512-byte payload（含 64-byte header 后每对象占 576 bytes）：

- 两个 4 KiB span 各容纳 7 个对象并剩余 64 bytes，形成连续两个高样本，target 增长到 8 KiB；
- 三个 8 KiB span 各只发布一个对象，形成连续三个低样本，target 收缩回 4 KiB；
- 最终精确得到 `sample=5, grow=1, shrink=1, hold=3`，且无 active TLAB；
- 类型表在测试中冻结，证明 adaptive locked refill/retire 与 ABI minor 71 frozen-type fast allocation兼容。

配置反向覆盖 unknown policy、未对齐 min、max 超过 Region、`min > initial`、`initial > max` 和 FIXED 携带 min/max；Region suite 另以 512-byte Region 验证默认 SMART initial/min/max 全部安全收缩到 Region。正式 Release 构建仍在预处理阶段移除 fault hook，策略状态机和几何检查保留。

### 36.6 当前非目标与下一步

SMART v0 故意只使用目标 span 的 retire 利用率，不把 wall clock、CPU 数、RSS、NUMA、fast/slow 命中率或 fallback 类型混入一个难以解释的总分。现有 refill/fallback/unused telemetry 可供后续策略实验，但任何替换算法都必须保持 min/max、滞回、active-span 不变和确定性降级合同。

ABI minor 72 没有解决 heap-level gate 的全局关闭成本。其后的 ABI minor 73 已完成第一版 per-thread card publication buffer 和 safepoint/repark 定向 flush，ABI minor 74 又消除了 C11 唯一-owner 常见路径的 registry 扫描；ABI minor 75/76 进一步把未满buffer的已预留card append和线性去重移到heap unlock后，ABI minor 77又把同一路径的global-card coverage读取锁外化，ABI minor 78把C11 owner分类共享64-bit telemetry和Region/generation分类推导改为锁外32-bit事件，ABI minor 81 已完成 per-thread Mutator Assist debt、公平性/退避和动态预算。满buffer overflow、descriptor reservation、slot publication、按epoch remark flush、Region RemSet、按thread/span的TLAB observer仍未全部完成。这些闭包完成之前，不能借自适应TLAB、card buffering、TSS lookup、deferred append/dedup/coverage/classification或Mutator Assist telemetry宣称concurrent REMAP或完整并发moving已经安全。

## 37. ABI minor 73：Per-Thread Remembered-Set Card Buffers

ABI minor 73 把 `nggc_store` 的 card-table publication 从“每次写都立即修改共享字节表”改成“优先记录到当前注册线程的 card-index buffer，再在边界点批量提交”。本增量的目标是建立后续低锁写屏障所需的私有缓冲、生命周期 flush 和可验证闭包；它没有同时删除 `nggc_store` 的 heap lock，也没有把单 heap card table替换成最终 Region RemSet。

### 37.1 配置、Region 几何与存储

公共 `nggc_config` 新增：

```text
remset_buffer_capacity == 0
    -> min(64, card_count)

remset_buffer_capacity != 0
    -> 1 <= capacity <= card_count
```

容量单位是 `size_t` card index 条目，不是字节。每个成功注册的线程分配固定数组，并保存独立的 `count/capacity`；注册失败路径、正常 unregister 和 heap 销毁路径都必须释放该数组。容量在 heap open 时冻结，显式值还必须满足 `capacity * sizeof(size_t)` 可表示。

Region 尺寸继续由 `nggc_config.region_size` 按 heap 配置：`0` 为默认 64 KiB，显式尺寸不要求为 2 的幂，但至少为一个 512-byte card，并满足对象、card、arena、TLAB 和 32-bit metadata 边界。buffer 只保存由 `(slot - arena_base) / card_size` 得到的 card index，因此不需要 Region 为 2 的幂，也不把 Region 尺寸编码进条目。Region suite 继续覆盖 512 bytes、8 KiB、24 KiB 和 128 KiB，其中 512-byte Region/8 KiB arena 还证明默认容量会按只有 16 张 card 的 heap 收缩为 16；多线程路径使用 24 KiB Region。

### 37.2 写屏障记录路径

当前 `nggc_store` 仍在 heap lock 内完成 owner/value/slot 合法性、forwarding canonicalization、并发 moving payload lock、Mutator Assist incremental mark 和 slot publication。在这些步骤中，RemSet 记录拆成三层：

1. prepare：验证 owner slot 和 value，计算 owner card，累计总 record、cross-region 与 old-to-young telemetry；
2. thread-buffer path：把合法 card 优先放入当前线程数组；
3. global commit：把 card 写入共享 card table，并沿用既有 precise/coarse 降级。

有 C11 `<threads.h>` 时，只有通过 `thrd_equal(owner_thread, thrd_current())` 唯一匹配的 `RUNNING` 或 `CONCURRENT_RUNNING` 注册线程才能接收本次记录。没有 C11 threads 时，只有一个 eligible 注册线程时才使用它；多个候选、未注册调用者或无法唯一识别 owner 时走 direct global fallback。collector/remap 内部重建调用始终走 direct path，避免把 collector 工作错误归入任意 mutator。

thread-buffer path 的顺序固定为：

```text
classify and count the write
  -> coarse already active or global card already dirty: covered
  -> linear deduplicate within this thread buffer
  -> buffer full: flush this thread only
  -> if flush caused coarse/global coverage: covered
  -> append the new card index
```

线性去重的上界由配置容量限制，默认最多 64 项；这是一版可解释的小缓冲策略，不是最终哈希或分段日志。不同线程可以暂时缓存同一 card，后续任一线程先刷成 global dirty 后，另一个线程的冗余项仍可安全排空。buffer overflow 不扫描或 flush 其他线程。

global commit 保留 ABI minor 40 的安全壳：precise 模式中新 card 到来时，若当前 dirty 数已达到 `remset_max_dirty_cards`，先把整张 card table 置脏、将 dirty count 设为 card count并切到 `COARSE_HEAP`；否则只置脏目标 card。进入 coarse 后，后续 direct 或 buffered record 都视为已覆盖，不能因 metadata 容量不足漏边。

### 37.3 Flush 与 safepoint 闭包

单线程 flush 在 heap lock 下依次提交当前数组中的 card，全部成功后把 count 清零，并累计一次 flush 与排空条目数。以下边界必须在发布状态之前执行 flush：

- 普通 safepoint ack：先 flush 当前 card buffer，再 retire 当前 TLAB，最后减少 pending、增加 parked 并发布 `AT_SAFEPOINT`；
- mutator-concurrent EVACUATE repark：收到 repark request 后，先 flush，再从 `CONCURRENT_RUNNING` 计入 parked；
- collector-owned collection：owner 在成为 `COLLECTOR_OWNED`、计入 parked 前先 flush，再 retire TLAB；
- thread unregister：在线程从 registry 摘链和释放 buffer 前先 flush，再完成 TLAB quiescence/retire。

`nggc_safepoint_release_locked` 要求每个 `AT_SAFEPOINT`/`COLLECTOR_OWNED` 线程的 buffer count 都为零。依赖 remembered closure 的 young collector 在 Release 路径还会显式要求全部 registered mutator parked，并重新验证 pending card 总数为零后才消费全局 card table；full non-moving collector扫描完整对象图，不依赖 RemSet 闭包。上述顺序不是依赖 Debug verifier 恰好发现遗漏。

`nggc_get_stats` 延续 ABI minor 71 的 TLAB 一致快照语义，仍会关闭并 flush TLAB fast gate；但它不会为了观测 RemSet 而 flush card buffer。这样 pending card/thread 数和 overflow 行为可被测量，读取统计也不会改变 precise/coarse 时序。ABI minor 73 没有把普通 TLAB observer 改成按线程 gate，该成本属于后续独立优化。

### 37.4 Coverage verifier 与故障注入

RemSet rebuild 对跨 Region strong edge 的覆盖定义从单一全局表扩展为：

```text
covered(card) = global_card_dirty(card)
             OR card_pending_in_any_valid_registered_thread_buffer(card)
```

因此 mutator 正常运行、card 尚未 flush 时，Debug/Verify full strong-slot walk 仍能验证 remembered edge 没有丢失。rebuild 开始前先验证所有 buffer 的结构，不能把越界或重复条目当成有效覆盖。

常驻 `nggc_check` 在 Debug 与 Release 都检查：

- heap effective capacity 非零、不超过 card count并可分配；
- registry 节点的 buffer pointer、per-thread capacity 与 heap capacity 一致；
- `count <= capacity`；
- 每个 pending card `< card_count`；
- 同一线程 buffer 内没有重复 card；
- parked、collector-owned 或 reparking 线程没有 pending card；
- 遍历线程数、pending card/thread 求和都可表示并与 registry 计数相容。

Verify+Test Hooks 的 `NGGC_TEST_REMSET_FAULT_BUFFER_CARD` 临时把真实 pending entry 改成 `card_count`，要求完整 `nggc_check` 返回 corruption，恢复后 heap 继续可用。确定性 RemSet 测试把容量和 global dirty limit 都设为 1，依次验证：首卡只存在于 pending buffer、union rebuild 接受；重复写同卡只增加 dedup；第二张 card 触发一次当前线程 overflow flush并保留新 pending；safepoint ack 刷第二张 card并触发 coarse；最终 `record=3, dedup=1, overflow_flush=1, flush=2, flushed_cards=2`。C11 多线程测试验证每个 worker 的 store 都进入线程 buffer，unregister 后 pending card/thread 均为零。

### 37.5 Telemetry

`nggc_stats` 新增 effective 配置和当前快照：

- `remset_buffer_capacity`；
- `remset_buffered_card_count`；
- `remset_buffered_thread_count`。

累计饱和计数器包括：

- `remset_buffer_record_count`；
- `remset_buffer_deduplicated_count`；
- `remset_buffer_flush_count`；
- `remset_buffer_flushed_card_count`；
- `remset_buffer_overflow_flush_count`；
- `remset_direct_record_count`。

`remset_record_count` 仍表示所有 owner 写入的总数；buffer/direct 计数描述 publication 路由，cross-region/old-to-young 计数描述边类型，三组不能互相替代。

### 37.6 当前非目标与后续顺序

ABI minor 73 自身不能表述为“无锁写屏障”：当时每次公共 `nggc_store` 仍获取heap lock，当前线程识别还需要扫描registry。其后的ABI minor 74已消除C11唯一-owner常见路径的扫描，ABI minor 75/76把已预留card的array/count publication和prefix dedup移到锁外，ABI minor 77又锁外化global-card coverage读取，ABI minor 78把分类telemetry和Region/generation分类推导改为锁外32-bit outcome；对象校验、forwarding、Assist、coarse/满buffer slow path、reservation和slot publication仍在heap-lock临界区。buffer没有epoch、SATB payload、NUMA分片或后台并发drain；全局表示仍是单heap `atomic_uchar` card table，不是UHMAS最终Region RemSet。

后续深化顺序为：

1. [ABI minor 74 已完成] 建立稳定的 thread-local per-heap barrier context，避免唯一-owner 常见路径每次写扫描 registry；
2. [ABI minor 75 已完成第一段] 把已预留 card 的数组写/count publication 从 heap lock 中移出；后续仍需把分类、去重和单生产者 reservation fast path移出共享临界区；
3. 为 concurrent mark/remark 定义 epoch、buffer 交换和 Mutator Assist/SATB 或 incremental-update 闭包；
4. 把单 heap card table替换为可按 Region 枚举、可 coarse 降级的最终 RemSet，具体表示等待 UHMAS 项目约束稳定；
5. 完成 root/handle/TLAB/barrier-buffer 闭包后，再开放 mutator-concurrent REMAP 和完整并发 moving。

## 38. ABI minor 74：C11 TSS Per-Heap Barrier Context

ABI minor 74 完成上一节后续顺序的第一步：在有 C11 `<threads.h>` 的平台上，常见写屏障不再为了寻找当前线程的 RemSet buffer 而遍历整个 heap thread registry。这个增量只优化 owner lookup；`nggc_store` 仍取得 heap lock，owner/value/forwarding/Mutator Assist 校验、card append 和 slot publication 的同步边界均未改变。因此它不是“无锁写屏障”，也没有提前声称并发 REMAP 闭包已经完成。

### 38.1 为什么是单一 TSS key 加 per-heap 链

一个原生线程可以同时注册多个 `nggc_heap`，也可以在同一个 heap 上持有多个 `nggc_thread` 句柄。只保存一个裸 `nggc_thread *` 无法表达前者；为每个 heap 分配独立 `tss_t` key 又会把 heap 生命周期耦合到平台 TSS key 配额，并使 close/key destructor 的先后关系变得危险。

当前实现使用一个 translation-unit/process-library 生命周期的 C11 TSS key。每个原生线程的 TSS value 是私有链：

```text
barrier_tss_entry {
    magic
    heap_identity            // monotonic heap-instance identity, never dereferenced
    unique_thread
    handle_count
    next
}
```

每个该线程曾注册过的 heap 最多有一个 entry。`handle_count == 1` 时 `unique_thread` 指向唯一句柄；`handle_count == 0` 时 entry 是不含句柄指针的 tombstone；`handle_count > 1` 时 `unique_thread == NULL`，表示同 heap 多句柄歧义。链是当前原生线程私有的，不由其他 mutator 或 GC worker并发修改。

TSS key 通过 C11 `call_once` 建立，初始化结果使用 32-bit `atomic_uint` 以 release/acquire 发布。这里不要求 64-bit atomic lock-free、双宽 CAS、128-bit atomic、pointer tagging 或 OS TLS 私有 API。若标准库没有 `<threads.h>`，整个 TSS 类型、key、destructor 和 lookup 代码在预处理阶段移除。

### 38.2 注册、注销、线程退出与 heap close

heap thread registry 仍是权威状态。`nggc_thread_register` 在 heap lock 下完成节点发布和 `registered_thread_count` 更新后，重建调用原生线程在该 heap 上的 context 摘要；`nggc_thread_unregister` 先完成 RemSet buffer flush、TLAB quiescence/retire和 registry 摘链，再在释放 thread 对象前重建摘要。因而缓存绝不会在正常注销后继续合法地暴露将被释放的唯一句柄。

若注销后该原生线程在 heap 上还剩一个句柄，entry 恢复为 unique；仍有多个则保持 ambiguous；最后一个句柄注销后变成 `count=0, unique=NULL` tombstone。tombstone 不保留可解引用的 heap 指针，而是保留注册时转换得到的 `uintptr_t` 地址 token，只用于以后整数地址匹配和 entry 复用；因此 heap close 后既不会读取已释放 heap，也不会在严格 C11 下读取已结束对象生命周期的悬挂 pointer value。当前不在每次最后注销时调用 `tss_set` 重写链头，也就不需要为“删除最后 entry 时 tss_set 失败”建立复杂回滚；代价是长生命周期线程会为曾接触过的 heap 暂留一个小 entry，直到线程退出。

TSS destructor 只释放当前原生线程的私有 entry 链，不能在未知 heap 生命周期、没有 heap lock 和可能处于线程退出上下文时隐式 unregister。应用仍必须显式 `nggc_thread_unregister`；若线程直接退出，权威 registry 中的 handle 按既有合同保留，`nggc_close` 返回 `NGGC_ERR_BUSY`，而不是由 destructor 猜测并修改 safepoint/root/TLAB 状态。

### 38.3 写屏障查找与 fail-closed 路由

在 `nggc_store` 已持 heap lock并需要记录 owner card 时，路由顺序为：

```text
C11 TSS ready and matching entry count == 1
  and cached thread magic/heap/state/native owner valid
    -> O(1) TSS hit，写入该线程 card buffer

matching entry count > 1
    -> explicit ambiguous，直接写全局 RemSet

missing/tombstoned/stale/ineligible entry，或 TSS unavailable
    -> 扫描权威 heap thread registry
       -> 唯一 eligible owner：写入其 buffer
       -> 多个 eligible owner：ambiguous direct
       -> 无 eligible owner：no-owner direct
```

同一 native thread/same heap 多句柄时不能任意挑一个 buffer，否则注销、safepoint flush 和 telemetry 会依赖 registry 链顺序；因此歧义是明确的 direct-global 语义。一个 native thread/different heaps 则在 TSS 链中分别查找，各自可以保持唯一并命中自己的 buffer。

缓存创建是性能优化而不是注册成功条件。`tss_create` 失败、entry `calloc` 失败或 `tss_set` 失败都只增加 update-failure telemetry；线程注册仍成功，后续 barrier 从 registry 得到相同正确结果。若更新已有 entry 的测试故障被注入，entry 先失效为 tombstone，再 fallback，不能保留一个可能把多句柄误判成 unique 的旧缓存。无 C11 threads 的构建始终使用 ABI minor 73 的 registry 扫描语义：只有唯一 eligible 注册句柄时才能使用 buffer，否则 direct。

### 38.4 C11 内存模型和当前同步边界

全局 TSS 初始化状态由 release store/acquire load 发布，保证观察到 READY 的线程也观察到已经创建的 key。entry 链仅由所属原生线程创建和修改；在 ABI minor 74 当时，heap registry、thread state、RemSet buffer和新增累计计数器仍在 heap lock 下访问。缓存中的 `nggc_thread *` 不承担跨线程 publication，也不替代 registry 节点的 magic/heap/state/owner 校验。ABI minor 75 对 buffer publication 的后续变化见第 39 节。

这个增量没有把 buffer append 移出 heap lock，因而还不需要为 producer/drainer 定义独立 head/tail 原子、epoch exchange 或 release sequence。下一步若让 mutator 在无 heap lock 路径 append，必须重新冻结：单生产者/collector drain 所有权、safepoint/remark epoch、buffer 满时 assist/flush、线程注销与 GC drain 的线性化点，以及 32-bit 平台可实现的索引原子宽度。ABI minor 74 的 TSS 只能复用为 context 入口，不能被误当成这些协议已经存在。

### 38.5 Region 尺寸兼容

barrier context 不缓存 Region shift、mask 或固定 Region 常量，只缓存 thread handle；card index 仍按 heap 的 `card_size` 和 arena 地址计算。`region_size` 继续是 per-heap 冻结配置：零值为 64 KiB，显式值不要求 2 的幂。ABI minor 74 的多线程回归继续使用 24 KiB Region；Region suite 现覆盖 512 B、8 KiB、24 KiB、128 KiB 和 1 MiB，1 MiB case 使用 4 MiB arena验证普通/large threshold、2-Region humongous run、card count 和统计。TSS lookup 不得引入位移假设破坏这些非 2 次幂或较大 Region。

### 38.6 Verifier、故障注入和 telemetry

Debug/Verify thread-registry verifier额外检查当前调用原生线程可见的 TSS 链：entry magic、链无环、同一 heap entry 唯一、`count/unique` 组合合法；若 entry 非 tombstone，则必须与权威 registry 对当前 native owner 的 `handle_count/unique_thread` 摘要完全一致。其他原生线程的 TSS 无法由当前线程可移植枚举，因此 verifier 不伪称能够全局读取所有 OS TLS；跨线程正确性继续由 heap registry、thread node、buffer 和 safepoint verifier 证明。

Verify+Test Hooks 可强制下一次 context update 失败。确定性测试先在缺少 entry 时注入失败，要求线程注册仍返回成功，第一次 store 产生一次 registry fallback、仍进入唯一线程的 card buffer，direct count 保持零，完整 `nggc_check` 通过；随后在已有 unique entry 的情况下让第二个 same-heap handle 更新失败，要求旧 unique cache 先失效为 tombstone、本次 store 经扫描识别 ambiguous 并 direct，注销第二句柄后又恢复 unique hit。普通 C11 测试还覆盖：

- 四个 worker 的全部 store 均为 TSS hit，fallback/ambiguous/update-failure/no-owner 为零；
- same native thread/same heap 双句柄产生 ambiguous direct，注销一个后下一次 store 恢复 unique hit；
- 同一 native thread 在两个同时存活 heap 各自命中；
- 最后句柄注销后调用 barrier 产生一次 fallback scan 和 no-owner direct；
- unregister/destructor/heap close 之间没有悬挂 context 解引用。

公开累计饱和计数器新增：

- `remset_barrier_tss_hit_count`；
- `remset_barrier_tss_fallback_scan_count`；
- `remset_barrier_tss_ambiguous_count`；
- `remset_barrier_tss_update_failure_count`；
- `remset_barrier_no_owner_direct_count`。

这些计数描述 owner lookup 路由；`remset_buffer_*` 描述 card publication buffer 行为，`remset_direct_record_count` 描述最终 direct publication，cross-region/old-to-young 仍描述边类型，不能互相替代。

### 38.7 下一增量边界

ABI minor 74 已消除有 C11 threads 且 owner 唯一时的 registry O(thread-count) 查找，但写屏障的主要临界区仍存在。后续优先级是：

1. [ABI minor 75 已完成第一段] 把已预留 card 的数组/count publication 变成不持 heap lock 的可移植单生产者尾路径，同时保留 direct/coarse fail-closed 路由；完整 reservation fast path仍待后续；
2. 冻结 collector drain、safepoint/repark、thread unregister 和 buffer overflow 的线性化与 epoch 协议；
3. 为 concurrent marking 选择并验证 SATB 或 incremental-update 的 remark closure，与 Mutator Assist packet 协调；
4. 在 UHMAS 约束稳定后把单heap atomic-card table演进为Region RemSet，并保留可验证coarse/full-scan降级；
5. 只有 root/handle/TLAB/barrier-buffer/RemSet 全部形成 epoch 闭包后，才允许 mutator-concurrent REMAP 和 from-space 复用。

## 39. ABI minor 75：Deferred Per-Thread Card Publication

ABI minor 75 在 ABI minor 73 的 per-thread card buffer 与 ABI minor 74 的 TSS owner context 上，完成一次刻意受限的临界区拆分：有 C11 native-thread identity、且当前原生线程在目标 heap 上只有一个合法注册句柄时，`nggc_store` 不再在 heap lock 内执行最终的 card-array 写入和 published-count 更新。它先在锁内建立一个不可丢失的单 card publication descriptor，发布对象 slot，解锁后再完成数组/count publication。

这个边界必须准确命名。ABI minor 75 不是完整无锁写屏障，也不是最终单生产者日志：`nggc_store` 仍在 heap lock 内完成对象、slot、value 和 forwarding 校验，Mutator Assist incremental-update 工作，RemSet 分类和 telemetry，TSS/registry owner 选择，global-card 与 thread-buffer 去重，buffer overflow flush，一个 descriptor 的 reservation，以及对象 slot publication。移出锁的只有已经不会失败的数组写、count release-store 和 inflight 清零。

### 39.1 每线程状态与表示边界

每个 `nggc_thread` 的 RemSet publication 状态为：

```text
remset_card_buffer[size_t capacity]
atomic_uint published_count
atomic_uint publication_inflight       // only 0 or 1
size_t publication_index
size_t publication_card
```

`published_count` 与 `publication_inflight` 使用 C11 `atomic_uint`，不是共享 64-bit counter。heap open 除 ABI minor 73 的 `1 <= capacity <= card_count` 和乘法可表示约束外，新增 `capacity <= UINT_MAX`；因此 `index + 1` 总能转换为 `unsigned`，协议不依赖 64-bit lock-free atomic、双宽 CAS 或 128-bit atomic。实现也不要求 `atomic_uint` lock-free：平台可以用其 C11 合法实现，正确性不观察 lock-freedom。

descriptor 的 `index/card` 是普通字段。它们只在 mutator持 heap lock且该线程没有既有 inflight publication 时写入，collector/stats/verifier也只在持 heap lock时读取；`publication_inflight` 的 release/acquire负责把 descriptor 从 mutator发布给随后取得 heap lock的其他线程。一个注册线程同时最多有一个 descriptor，避免为 `(index, card, epoch)` 引入宽原子事务。

### 39.2 写屏障线性化与发布顺序

C11 owner 路径的固定顺序是：

```text
heap lock held
  -> validate owner / slot / value
  -> canonicalize forwarding
  -> perform bounded Mutator Assist barrier work
  -> classify RemSet edge and update telemetry
  -> test global coverage and deduplicate published prefix
  -> if full, flush only this thread under the heap lock
  -> descriptor.index = published_count
  -> descriptor.card = owner_card
  -> release-store publication_inflight = 1
  -> publish canonical object slot
  -> release heap lock

heap lock not held
  -> buffer[descriptor.index] = descriptor.card
  -> release-store published_count = descriptor.index + 1
  -> release-store publication_inflight = 0
```

正确性的关键线性化点不是数组写，而是 slot publication 之前的有效 descriptor publication。只要新 strong edge 对 young/跨 Region collection 有意义，在该 edge 对其他 collector 可见之前，其 owner card 已经至少由以下三者之一覆盖：

```text
global dirty card
OR published per-thread buffer prefix
OR valid reserved in-flight descriptor
```

因此解锁后暂停、被抢占或 safepoint request 到来时，不存在“对象边已发布，但 global、buffer 和 descriptor 都没有记录”的窗口。数组写本身不会分配内存、扩容或调用 backend；所有可能触发 flush/coarse 降级的工作已在锁内完成。若宿主长期不再调度该 mutator，协作式 safepoint会继续把它计为 pending，而不是绕过 descriptor 强行回收线程或 from-space。

### 39.3 Reader snapshot 与合法中间态

所有 heap-lock reader都通过同一个 acquire snapshot 读取 publication 状态，不能直接把 atomic count 当成完整 closure。读取顺序固定为先 acquire-load `publication_inflight`，再 acquire-load `published_count`。若先看到最终 `inflight == 0`，该 acquire 与 mutator最后的 release clear同步，并传递此前 buffer write/count publication；若先看到 `inflight == 1`，snapshot允许以下两种中间态：

```text
reserved, array/count not published:
  inflight == 1
  published_count == publication_index
  effective_count = published_count + 1
  descriptor.card supplies coverage

array/count published, final clear not observed:
  inflight == 1
  published_count == publication_index + 1
  buffer[publication_index] == publication_card
  effective_count = published_count
```

除此之外的 count/index/card 组合均为 corruption。`effective_count` 是逻辑待处理 card 数；第一种状态中它比物理 published prefix 多一，第二种状态中 descriptor 已被 prefix吸收，不能重复计数。snapshot还要求 inflight thread 仍处于 `RUNNING` 或 `CONCURRENT_RUNNING`，descriptor index 在 capacity 内、card 在 card table 内。

普通 buffer prefix 由 count 的 release/acquire发布，reader只读取 `[0, published_count)`；第一种中间态不能读取尚未发布的 `buffer[index]`，而是读取 descriptor。第二种中间态只有在 acquire count已经观察到新值后才核对该数组项。这样普通 `size_t` 数组元素不需要逐项原子化，也不会与 mutator的未发布写形成数据竞争。

### 39.4 Flush、safepoint、注销和线程生命周期

`nggc_remset_flush_thread_locked` 只接受：

```text
publication_inflight == 0
effective_count == published_count
```

因此普通 safepoint ack、EVACUATE repark、collector-owned 转换和 thread unregister都不能在 deferred publication 尚未结束时清 count、提交 global card、发布 parked 状态或释放 buffer。正常 owner 调用是同步的：`nggc_store` 在返回调用者之前总会完成 deferred tail，随后同一原生线程才可能调用 park/unregister。若另一个宿主线程错误地拿该 handle请求 safepoint，flush会 fail-closed 返回 corruption/busy语义，而不会与 owner的数组写竞争。

有 C11 `<threads.h>` 时，`nggc_thread_register` 记录 `thrd_current()`，`nggc_store` 只有 native owner相等且句柄唯一时才选择 deferred buffer；`nggc_thread_unregister` 同样要求由该 owner线程调用。于是当前 `nggc_store` 解锁到 deferred tail返回之间，另一个原生线程不能合法摘链并释放该 `nggc_thread`。

没有 `<threads.h>` 时，registry fallback无法证明“当前只有一个 eligible handle”就等价于“调用它的原生线程拥有该 handle”。另一个宿主线程理论上可能在 unlock 后注销该唯一句柄。为避免 unlock 后解引用已释放 thread，threadless build保留 ABI minor 73 的锁内：

```text
buffer[count] = card
release-store published_count = count + 1
```

该 fallback不产生 deferred-publication telemetry，但 global/direct/coarse、去重、overflow、flush 和 remembered closure语义保持一致。这是安全降级，不是用平台页异常或系统挂起模拟 owner identity。

### 39.5 Verifier、统计与故障注入

`nggc_stats.remset_buffered_card_count` 和 `remset_buffered_thread_count` 现在报告 effective snapshot，因此包含第一种中间态中的 reserved descriptor。新增当前快照字段：

- `remset_buffer_inflight_card_count`；
- `remset_buffer_inflight_thread_count`。

每个线程最多一个 inflight descriptor，所以两者在当前协议中逐线程同增，但分开保留以免未来 batch descriptor 时破坏 stats ABI语义。新增累计饱和计数 `remset_buffer_deferred_publish_count`，只在 C11 owner路径真正建立一次锁外 publication时增加；同 card dedup、global card 已覆盖、coarse mode、ambiguous/no-owner direct和 threadless锁内 append都不增加它。

常驻 `nggc_check` 与 RemSet rebuild snapshot会检查：

- capacity 同 heap配置一致、非零、`<= card_count`、`<= UINT_MAX`；
- inflight只能为 0/1，published/effective count可表示且不超过 capacity；
- published prefix中所有 card有效且线程内无重复；
- reserved descriptor有效，且在尚未进入 prefix时不与 prefix重复；
- parked、collector-owned、reparking线程的 published/effective/inflight全为零；
- effective buffered card/thread和 inflight card/thread累计不溢出；
- RemSet rebuild对每条需要记忆的 strong edge接受 global、published prefix或reserved descriptor覆盖。

Verify+Test Hooks 增加一个只编入 verify测试目标的暂停点，位置精确在“heap lock已释放、descriptor已覆盖、数组/count尚未发布”。确定性 C11 测试在 24 KiB非 2 次幂 Region上证明：

1. 主线程可在 worker暂停时成功取得 stats，确认 heap lock确已释放；
2. stats报告一个 effective buffered card、一个 inflight card、零 global dirty card；
3. RemSet rebuild和完整 `nggc_check` 接受合法 descriptor coverage；
4. fault 305把 descriptor card临时改为 `card_count` 时，完整 check必须返回 corruption；
5. 暂停期间发起 safepoint后，worker仍是一个 pending thread，descriptor仍在 closure中；
6. 释放暂停后，worker先完成 array/count/inflight publication，再 park时flush到 global card，最终 parked线程不含 published或inflight card；
7. safepoint release后 worker可正常 unregister，证明 descriptor生命周期没有泄漏到 registry摘链之后。

暂停分支、entered/release原子和 fault 305都位于 `NGGC_ENABLE_VERIFY && NGGC_ENABLE_TEST_HOOKS`，正式 Release库在预处理阶段移除；Release正确性依赖常驻 snapshot、flush门禁和 owner生命周期合同，不依赖测试钩子。

### 39.6 Region 尺寸兼容

Region 尺寸继续由每个 heap的 `nggc_config.region_size` 冻结：

```text
region_size == 0
  -> 64 KiB default

region_size != 0
  -> explicit size; power-of-two is not required
```

显式尺寸仍须至少一个 512-byte card，并满足 64-byte对象对齐、card整除、`UINT32_MAX` metadata表示、arena/TLAB和32-bit地址空间边界。deferred descriptor只保存由 `(slot - arena_base) / card_size` 计算的 card index，不保存 Region shift/mask，也不把 Region大小编码进原子状态。因此 ABI minor 75不会把原先可配置 Region退化为固定 64 KiB或2次幂假设。

现有 Region suite覆盖 512 B、8 KiB、24 KiB、128 KiB和1 MiB；本增量的确定性并发窗口使用24 KiB Region。`region_size`为零的默认语义与显式非零配置都属于已冻结 heap级合同，未来 Region RemSet只能改变内部索引结构，不能删除该配置能力。

### 39.7 仍未完成的低锁屏障边界

ABI minor 75只去掉了 card append尾部的几次内存访问。下一阶段仍需按正确性依赖顺序推进：

1. 将 owner已知时的 RemSet分类、published-prefix去重和单生产者 reservation从全 heap lock拆到可验证的 per-thread快路径；overflow仍需明确 assist、交换或受控 slow path。
2. 给 card/SATB或incremental-update日志增加 mark/remark epoch，冻结 collector drain、buffer exchange、线程退出和 epoch rollover线性化点。
3. 让 concurrent mark worker、Mutator Assist packet和写屏障日志形成同一 remark closure，并为 metadata OOM保留 coarse/full-scan降级。
4. 在 UHMAS约束稳定后，把单heap atomic-card table替换为可按Region枚举、可分层coarse降级、支持young/old方向过滤的最终RemSet；具体container仍留待UHMAS项目回填。
5. 完成 root/handle/TLAB/barrier-buffer epoch闭包后，才扩展 mutator-concurrent REMAP与from-space复用。

在这些步骤完成前，公开描述必须保持“常驻写屏障 + 部分锁外 card publication”，不能写成“完整 lock-free barrier”或“完整并发 moving GC”。

## 40. ABI minor 76：Deferred Prefix Deduplication

ABI minor 76 在第39节的descriptor-first closure上继续缩短写屏障临界区：有C11 native owner且per-thread buffer尚未满时，锁内不再对published prefix执行最多`capacity`项的线性去重。锁内只建立descriptor并发布对象slot；owner解锁后扫描稳定prefix，若card已经存在则完成一次deferred dedup，若不存在才执行ABI minor 75的数组/count publication。

这个增量依然不是完整无锁写屏障。owner/value/slot/forwarding校验、Mutator Assist、RemSet边分类、global/coarse coverage判断、TSS/registry owner选择、descriptor reservation和slot publication仍持heap lock。buffer恰好已满时也仍在锁内扫描：满buffer没有可用的`descriptor.index == count < capacity`槽位，且重复card不应为了走锁外扫描而先flush整buffer。

### 40.1 未满与满buffer的分流

C11 owner路径在global/coarse coverage尚未满足后按以下规则分流：

```text
published_count < capacity
  -> 不在heap lock内扫描prefix
  -> descriptor.index = published_count
  -> descriptor.card = owner_card
  -> release-store inflight = 1
  -> publish object slot
  -> unlock
  -> owner扫描buffer[0..index)
       -> 命中：outcome = DEDUPLICATED，count不变
       -> 未命中：写buffer[index]、release-publish count，再发布APPENDED outcome
  -> release-store inflight = 0

published_count == capacity
  -> 在heap lock内扫描完整prefix
       -> 命中：直接locked dedup，不flush
       -> 未命中：overflow flush当前线程，再预留index 0 descriptor
```

没有C11 `<threads.h>` 时无法证明unlock后的handle生命周期，仍在heap lock内完成去重与append。threadless fallback不产生deferred append或deferred dedup telemetry；它与C11路径保持相同remembered coverage、overflow和coarse降级结果。

### 40.2 Descriptor在重复路径中的coverage

ABI minor 75把reserved descriptor视为一个额外effective card，因此当时要求descriptor尚未进入prefix时不得与prefix重复。ABI minor 76放宽这一结构规则：descriptor在slot publication前仍是合法coverage，但snapshot会扫描稳定published prefix并决定它是否新增逻辑card：

```text
descriptor.card not in prefix
  -> effective_count = published_count + 1

descriptor.card already in prefix
  -> effective_count = published_count
```

这不会漏边。重复路径中旧prefix本来就覆盖card；新descriptor只为“解锁后尚未完成去重判断”的窗口提供显式事务状态。非重复路径中descriptor本身继续覆盖尚未进入数组的新card。因而任一时刻仍满足：

```text
edge published
  => global dirty
     OR published prefix coverage
     OR valid descriptor coverage
```

RemSet rebuild、`nggc_check`、stats和safepoint request可以在owner暂停于锁外扫描之前观察任一情况。snapshot只读取`[0, published_count)`，该prefix由此前count release/acquire发布且在当前descriptor完成前不会被flush或改写，所以owner和reader并发扫描同一普通`size_t`数组只发生只读共享，不形成数据竞争。

### 40.3 32-bit deferred outcome协议

锁外路径不能直接增加heap内的64-bit累计计数器，否则会引入共享64-bit atomic要求或数据竞争。每线程新增一个32-bit `atomic_uint deferred_outcome`：

```text
0 = NONE
1 = APPENDED
2 = DEDUPLICATED
3 = CORRUPT（只用于fail-closed内部状态）
```

owner完成锁外判断后，append路径先写数组并release-publish count，再release-publish APPENDED outcome；这使APPENDED严格表示数组/count发布已经发出，不会让stats把尚未完成的append提前折叠。dedup路径不写数组、不改变count，直接release-publish DEDUPLICATED outcome。两条路径最后都release-clear inflight。下一次该线程进入store/flush，或者任意线程调用stats时，heap lock持有者以acquire/exchange把outcome折叠到现有64-bit饱和telemetry：

```text
APPENDED
  -> remset_buffer_deferred_publish_count += 1

DEDUPLICATED
  -> remset_buffer_deduplicated_count += 1
  -> remset_buffer_deferred_deduplicated_count += 1
```

outcome是最多一个待折叠事件，不是无限累计counter。同步`nggc_store`在返回前已经完成append或dedup；在该owner发起下一次store、park或unregister前，locked入口会先折叠旧outcome，所以不会被下一次descriptor覆盖。stats在读取累计值前折叠所有注册线程outcome，保证公开snapshot不漏掉已经完成的事件。`nggc_check`本身不为了验证而修改telemetry；它允许结构一致的pending outcome。

协议仍只依赖32-bit `atomic_uint`、普通`size_t` card数组和heap lock，不依赖共享64-bit lock-free atomic、双宽CAS或128-bit atomic。

### 40.4 Snapshot状态矩阵

reader继续先acquire-load inflight，再读取count和outcome。除ABI minor 75的reserved/append中间态外，新增重复状态：

```text
inflight == 1
count == descriptor.index
descriptor.card exists in prefix
outcome == NONE or DEDUPLICATED
effective_count = count
```

append状态必须满足：

```text
inflight == 1, count == index
  -> descriptor.card不在prefix
  -> outcome只能是NONE或APPENDED

inflight == 1, count == index + 1
  -> buffer[index] == descriptor.card
  -> outcome只能是NONE或APPENDED
```

虽然append owner按“count后outcome”发布，snapshot仍先读取count、后读取outcome；它可能先采到旧count，再采到新的APPENDED，因此第一行仍允许APPENDED。该状态不读取尚未计入count的数组槽，coverage继续由descriptor提供；后续reader通过outcome acquire或最终inflight clear会观察到已发布count。

owner最终clear inflight后，尚未折叠的outcome必须与descriptor和prefix保持可重建对应：

```text
APPENDED:
  count == index + 1
  buffer[index] == descriptor.card

DEDUPLICATED:
  count == index
  descriptor.card存在于buffer[0..count)
```

枚举值在范围内但语义不匹配同样是corruption。这样Release verifier能拒绝把APPENDED误标为DEDUPLICATED、错误count、错误descriptor index或不匹配数组项，而不只是拒绝`outcome > 2`。

### 40.5 Safepoint、flush与生命周期

flush、普通safepoint ack、EVACUATE repark、collector-owned转换和unregister仍要求`inflight == 0`。若outcome已经完成但尚未折叠，locked flush先验证descriptor/outcome语义，再折叠telemetry，之后才提交published prefix到global card table、清count和发布parked/摘链状态。

owner生命周期证明与ABI minor 75不变：只有注册该handle的C11 native thread可合法使用deferred路径并unregister；`nggc_store`在返回前完成锁外扫描和最终inflight clear。无native owner证明的平台不在unlock后解引用thread对象。

### 40.6 Telemetry、故障注入与确定性测试

公开stats新增：

- `remset_buffer_deferred_deduplicated_count`：实际在C11锁外prefix扫描中命中的累计次数。

`remset_buffer_deduplicated_count`仍表示locked与deferred两种去重总数；两者之差是locked dedup，包括满buffer重复写。`remset_buffer_deferred_publish_count`现在只在outcome确认APPENDED后增加，不再在锁内descriptor reservation时提前增加，因此暂停于锁外扫描前的stats会报告inflight descriptor，但不会把尚未决定append还是dedup的事务计为已append。

fault 306在C11 owner构建中选择一个具有有效`index/card/count`关系的合法静止descriptor，把outcome临时改成与其append/dedup语义相反的合法枚举值，要求完整`nggc_check`和会折叠telemetry的`nggc_get_stats`都在清零outcome前因descriptor/count/prefix语义不匹配返回corruption；恢复后heap继续可用。threadless fallback不创建deferred descriptor，因此不运行该fault。这比只注入越界枚举或依赖`SIZE_MAX`哨兵更能证明常驻状态矩阵。

24 KiB非2次幂Region上的确定性pause测试现在运行两次：

1. 空prefix上的新card：暂停时`effective=1, inflight=1, deferred_publish=0`；恢复后append一次，park flush一张card。
2. 已预填同card的非满prefix：暂停时仍`effective=1, inflight=1`，不会把descriptor重复计数；恢复后count保持1，最终`deferred_publish=1, dedup=1, deferred_dedup=1`，park仍只flush一张card。

两次窗口都验证stats可取得heap lock、RemSet rebuild/full-check接受closure、fault 305拒绝无效descriptor、safepoint保持worker pending，恢复后park/unregister成功。另有容量1测试证明满buffer重复写走locked dedup且不会产生overflow flush或deferred-dedup计数。正式Release仍在预处理阶段移除pause/fault hooks，常驻snapshot/outcome/flush协议保留。

### 40.7 Region兼容与下一步

outcome和dedup都只处理card index；不缓存Region shift/mask。`nggc_config.region_size == 0`仍表示默认64 KiB，显式Region不要求2次幂；现有512 B、8 KiB、24 KiB、128 KiB和1 MiB suite继续适用。ABI minor 76的并发测试沿用24 KiB Region，证明锁外prefix扫描没有引入固定Region几何。

下一步仍按以下边界推进：

1. 把global/coarse coverage读取、RemSet边分类和descriptor reservation逐步从全heap lock拆到per-thread context；满buffer overflow保留明确slow path。
2. 为card/SATB或incremental-update日志增加mark/remark epoch和collector drain/exchange协议。
3. 在真实UHMAS约束稳定后演进为最终Region RemSet，并保持coarse/full-scan降级。
4. root/handle/TLAB/barrier-buffer epoch闭包完成前，不开放mutator-concurrent REMAP和from-space立即复用。

因此ABI minor 76的准确表述是“常驻写屏障中的未满buffer append与线性去重已锁外化”，不是“整个写屏障无锁”。

## 41. ABI minor 77：Deferred Global-Card Coverage

ABI minor 77继续沿ABI 75/76的descriptor-first closure缩短C11 owner写屏障临界区：未满per-thread buffer的常见路径不再在heap lock内读取global card。锁内先建立descriptor、发布对象slot，owner解锁后才acquire读取目标card；如果global card已经为dirty，则descriptor以`COVERED`结果结束，不再扫描prefix，也不向thread buffer追加重复card。

这个增量仍不是完整无锁写屏障。对象/slot/value/forwarding校验、Mutator Assist、跨Region与OLD-to-YOUNG分类telemetry、TSS owner选择、coarse-mode判断、descriptor reservation和slot publication仍持heap lock。满buffer没有可预留槽位，因此global coverage、prefix dedup与overflow flush仍保留在locked slow path；没有C11 native-owner证明的平台也继续锁内完成global check、dedup和append。

### 41.1 Atomic card表示

global card table从普通`unsigned char[]`改为逐项`atomic_uchar[]`：

```text
0 = CLEAN
1 = DIRTY
其他值 = CORRUPTION
```

每个元素在heap open期间用`atomic_init`初始化。metadata分配与溢出校验按`sizeof(atomic_uchar)`计算，不能假设该C11原子类型在所有实现上物理恰好占一个byte。协议也不把`ATOMIC_CHAR_LOCK_FREE`当作正确性前提；主流平台通常提供lock-free byte atomic，其他合法C11实现仍可使用其标准原子实现。它不依赖共享64-bit lock-free atomic、双宽CAS、128-bit atomic、页错误、signal或系统挂起。

当前global card只在heap lock持有时由collector/flush路径修改，普通状态单调从`0 -> 1`；进入`COARSE_HEAP`时在锁内逐项release-store为1。C11 owner锁外只做acquire-load，不直接修改card或`dirty_card_count`。因此card元素不存在普通数组读写数据竞争，而`dirty_card_count`、mode切换和coarse降级仍由heap lock串行化。公开stats用`remset_card_atomic_lock_free`报告当前heap的`atomic_uchar`是否lock-free（严格0或1）；这是性能能力观测，不改变正确性合同。

当前实现不会在collection后把card重新清零。未来按mark/remark epoch交换或清空card table时，必须先证明所有旧epoch descriptor、`COVERED` outcome和collector drain都已闭包，不能直接在ABI 77协议下并发`1 -> 0`。

### 41.2 写屏障分流与发布顺序

C11 unique-owner且buffer未满时：

```text
heap lock held
  -> validate owner/value/slot and canonical forwarding
  -> perform Mutator Assist and edge classification telemetry
  -> fold previous deferred outcome
  -> if mode == COARSE_HEAP: covered, return locked path
  -> do not read target global card
  -> descriptor.index = published_count
  -> descriptor.card = owner_card
  -> release-store inflight = 1
  -> publish canonical object slot
  -> unlock

heap lock not held
  -> acquire-load global_card[descriptor.card]
       2..255: fail closed as corruption
       1: release-publish outcome=COVERED; count unchanged
       0: scan stable prefix
            hit: release-publish outcome=DEDUPLICATED; count unchanged
            miss: write buffer[index]
                  release-publish count=index+1
                  release-publish outcome=APPENDED
  -> release-store inflight = 0
```

descriptor仍在slot publication之前形成remembered coverage，所以global check被移动到unlock后不会产生漏边窗口。即使另一个locked flush在owner解锁后才把同一card置脏，owner看到1会走`COVERED`；如果owner较早看到0并append，随后global又变dirty，只会形成安全冗余，不会丢失closure。

满buffer路径必须先在锁内读取global card：dirty则直接结束；clean才扫描完整prefix，重复则locked dedup，不重复才flush当前线程。flush可能触发coarse降级或把目标card本身置脏，所以slow path在预留index 0 descriptor之前再次检查mode/card。threadless fallback同样只在锁内读取atomic card并完成append，避免unlock后handle生命周期无法证明。

### 41.3 COVERED outcome与telemetry折叠

per-thread 32-bit outcome扩展为：

```text
0 = NONE
1 = APPENDED
2 = DEDUPLICATED
3 = COVERED
4 = CORRUPT（内部fail-closed状态，不是合法snapshot）
```

`COVERED`表示本次未满-buffer事务在锁外观察到global card已经为1，因此没有prefix scan和array/count publication。下一次store、flush/safepoint/unregister或stats在heap lock下先通过统一snapshot验证outcome与descriptor/count/prefix/global-card语义，再把它折叠为公开饱和计数：

```text
remset_buffer_deferred_covered_count += 1
```

它不增加`remset_buffer_deferred_publish_count`或dedup计数。与ABI 76相同，outcome只是每线程最多一个待折叠事件，不是共享64-bit原子counter；同步`nggc_store`返回前一定完成outcome发布和inflight clear。stats可以在outcome已发布但inflight尚未clear时先验证并折叠事件，这仍安全，因为global card保持dirty且descriptor在heap lock下稳定。合法枚举但语义错误的outcome会在exchange清零之前返回corruption，stats不能吞掉损坏状态。

### 41.4 Snapshot closure与状态矩阵

snapshot继续先acquire-load inflight/count/outcome，并对descriptor card执行atomic acquire-load。每个card必须严格为0或1。对`inflight == 1 && count == index`：

```text
descriptor.card在prefix中
  -> effective_count = count
  -> outcome可为NONE或DEDUPLICATED

global_card == 1
  -> effective_count = count
  -> outcome可为NONE或COVERED
  -> 若owner已经决定append/dedup，也允许相应APPENDED/DEDUPLICATED中间态

prefix未命中且global_card == 0
  -> effective_count = count + 1
  -> descriptor提供唯一coverage
  -> outcome可为NONE，或因snapshot先读旧count而看到APPENDED
```

语义约束优先于枚举范围：`COVERED`必须对应global card 1，`DEDUPLICATED`必须对应prefix命中，`APPENDED`不得对应prefix重复。对`inflight == 1 && count == index + 1`，`buffer[index]`必须等于descriptor card，outcome只能是NONE或APPENDED；物理published prefix已经包含该card，即使global也dirty，effective count仍按实际count报告。

inflight clear后尚未折叠的静止状态为：

```text
APPENDED:
  count == index + 1
  buffer[index] == descriptor.card

DEDUPLICATED:
  count == index
  descriptor.card存在于prefix

COVERED:
  count == index
  global_card[descriptor.card] == 1
```

outcome已被stats/locked入口折叠为NONE后，静止descriptor只是历史调试信息，不再参与effective count。flush仍要求inflight为0；若outcome待折叠则先验证上述关系并计入telemetry，之后才提交真实published prefix。

### 41.5 Verifier、故障注入与确定性窗口

常驻`nggc_check`现在逐项atomic acquire-load global card，拒绝任何非0/1值，并继续核对dirty count、coarse全脏不变量和global/prefix/descriptor联合closure。Verify fault 307把一个card临时写为合法原子类型范围内但非法协议值2，要求完整check返回corruption并恢复；这验证了atomic本身不等于语义有效。

24 KiB非2次幂Region上的第三个确定性publication窗口先由no-owner direct barrier建立一个真实跨Region dirty card，再让C11 owner对同一slot写入并暂停在unlock后global check之前。暂停snapshot必须观察：

```text
dirty_card_count == 1
inflight_thread/card == 1
effective buffered card/thread == 0
deferred_covered == 0
```

此时global card已经吸收descriptor的effective coverage，RemSet rebuild与完整check都必须成功。释放窗口后owner发布`COVERED`、不扫描/append，随后safepoint park不产生空buffer flush计数；stats折叠出一次deferred-covered，worker可正常release/unregister。MSVC C11路径和MinGW threadless路径都运行fault 307；threadless路径不伪造deferred-covered事务。

### 41.6 Region兼容与下一步

atomic card索引仍由`(slot - arena_base) / card_size`计算，global check和outcome不缓存Region shift/mask。`nggc_config.region_size == 0`继续表示默认64 KiB；显式值不要求2的幂，512 B、8 KiB、24 KiB、128 KiB和1 MiB suite继续覆盖分配、large threshold、humongous run、TLAB及card几何。ABI 77新增窗口使用24 KiB Region和跨Regionlarge value，明确验证该优化不依赖固定64 KiB Region。

下一增量仍应保持可验证拆分：

1. 把跨Region/OLD-to-YOUNG分类结果改为可延迟折叠的per-thread事件，减少锁内共享64-bit telemetry更新；
2. 为descriptor reservation设计per-thread owner-only fast gate与collector quiescence，使reservation和最终slot publication可以在受控条件下脱离heap lock；
3. 为card/mark日志引入epoch、collector drain/exchange和remark closure，之后才能安全支持card清零复用；
4. 在真实UHMAS约束稳定后演进为Region RemSet，并保留atomic-card/coarse/full-scan降级；
5. root/handle/TLAB/barrier-buffer epoch闭包完成前，不开放mutator-concurrent REMAP和from-space立即复用。

因此ABI minor 77的准确表述是“未满buffer的global-card coverage检查已锁外化”，不是“global card由mutator无锁写入”，也不是“整个写屏障已经lock-free”。

## 42. ABI minor 78：Deferred RemSet Classification Telemetry

ABI minor 78继续缩短C11 unique-owner写屏障在heap lock内对共享状态的修改，但本轮只处理边分类telemetry，不把对象校验、descriptor reservation或slot publication锁外化。分类计算也已从锁内移到解锁后的冻结descriptor路径。此前每次owner写屏障都会在锁内直接更新以下共享64-bit字段：

```text
remset_record_count
remset_cross_region_record_count
remset_old_to_young_record_count
remset_buffer_record_count
```

现在C11 owner路径在锁内冻结一份可验证的分类descriptor，slot成功发布并unlock后只通过32-bit`atomic_uint`回传分类结果；共享64-bit饱和计数由下一次持锁入口验证后折叠。direct/no-owner路径和没有C11`<threads.h>`的平台仍在锁内同步计数，因此正确性不依赖32/64位平台上的共享64-bit lock-free atomic。

### 42.1 分类位与冻结descriptor

分类位不是任意bitset，只允许三个完整状态：

```text
RECORD
RECORD | CROSS_REGION
RECORD | CROSS_REGION | OLD_TO_YOUNG
```

硬性蕴含关系为：

```text
OLD_TO_YOUNG => CROSS_REGION => RECORD
```

每线程descriptor冻结：

```text
owner_region
value_region
owner_generation
value_generation
classification_inflight     // atomic_uint, 0/1
classification_outcome      // atomic_uint, 上述合法位集合或NONE
```

owner必须是合法live object，因此`owner_region < region_count`且逻辑generation只能是YOUNG或OLD。非null value同样必须冻结合法Region与YOUNG/OLD generation；null value使用唯一编码：

```text
value_region = SIZE_MAX
value_generation = NONE
expected flags = RECORD
```

Release常驻verifier不相信producer给出的flags，而是从冻结字段重新推导：owner/value Region不同才是`CROSS_REGION`；只有跨Region且owner逻辑OLD、value逻辑YOUNG才是`OLD_TO_YOUNG`。该推导发生在slot成功发布并unlock之后，只读取descriptor和不可变的Region表大小，不重新解引用对象。物理SPECIAL placement不会把新对象预晋升，分类仍以对象头的逻辑generation为准，也继续遵守“达到年龄阈值才允许OLD”的硬合同。

### 42.2 独立inflight与状态机

classification不能复用card publication inflight。一次成功写入即使处于coarse模式、已被global card覆盖、在线程prefix中去重，或者走满buffer锁内慢路径，也仍然产生一条`RECORD`分类事件；这些路径不一定有待append的card descriptor。因此每线程使用独立classification inflight，并允许card事务与classification事务处于不同阶段。

合法状态机为：

```text
IDLE
  inflight = 0
  outcome = NONE
  descriptor = SIZE_MAX/SIZE_MAX/NONE/NONE

RESERVED
  inflight = 1
  outcome = NONE
  descriptor可重算一个合法flags

OUTCOME_PUBLISHED
  inflight = 1
  outcome = expected flags
  descriptor保持不变

FOLDABLE
  inflight = 0
  outcome = expected flags
  descriptor保持不变

FOLDED/IDLE
  持锁observer先验证再exchange outcome为NONE
  将事件加入共享64-bit计数
  descriptor恢复sentinel
```

producer先release-publish outcome，最后release-store`inflight = 0`。observer先acquire-load inflight；若看到0，就同时取得此前outcome与descriptor可见性。若observer在两个store之间看到`inflight == 1`和非零outcome，该事件仍作为合法pending状态被验证和统计，但不能提前清空descriptor。每个同步`nggc_store`返回前完成outcome与inflight发布，因此同一owner线程下一次store不会覆盖尚未完成的descriptor；每线程最多存在一条有效pending classification。

### 42.3 写屏障顺序与折叠入口

C11 unique-owner成功路径的顺序为：

```text
heap lock held:
  fold previous card outcome
  fold previous classification outcome
  validate owner/value/slot/forwarding and Mutator Assist rules
  freeze owner/value Region and logical generation
  reserve classification descriptor + inflight
  reserve card descriptor when that card path needs one
  publish canonical object slot
  unlock

heap lock not held:
  complete ABI 77 card global-check/dedup/append outcome when pending
  derive classification flags from the frozen descriptor
  release-publish classification outcome
  release-clear classification inflight
```

classification事件只在`nggc_remset_record_write_impl`已成功且对象slot随后实际写入的路径预留。锁外card publication若检测到协议损坏，wrapper仍尽力完成classification生命周期，因为slot已经发布；返回值保留card错误，而常驻verifier会继续拒绝损坏的card状态。direct/no-owner路径先成功提交global card，再锁内增加direct与分类计数；threadless fallback在锁内完成card与分类，不制造无法证明句柄生命周期的unlock后事务。

完成outcome可由下一次同线程store、buffer flush、safepoint/repark、collector-owned转换、thread unregister或`nggc_get_stats`折叠。flush必须先验证card snapshot与classification snapshot，再分别折叠outcome，之后才允许提交prefix、发布parked状态或摘除thread节点。`AT_SAFEPOINT`、`COLLECTOR_OWNED`和`CONCURRENT_REPARKING`线程必须同时满足card与classification均无inflight/outcome/pending数据。

### 42.4 有效stats快照与不重复计数

ABI 78公开：

```text
remset_classification_pending_thread_count
remset_buffer_deferred_classification_count
```

`nggc_get_stats`取得heap lock后先折叠所有已经`inflight == 0`的合法outcome，再运行完整snapshot verifier。对仍处于RESERVED或OUTCOME_PUBLISHED的事件，stats不会修改heap共享基数，而是把descriptor推导出的增量只叠加到本次返回值：

```text
effective remset_record_count                  += 1
effective remset_buffer_record_count           += 1
effective remset_cross_region_record_count     += CROSS flag
effective remset_old_to_young_record_count     += OLD_TO_YOUNG flag
effective deferred_classification_count        += 1
pending classification thread count            += 1
```

因此slot已发布但outcome尚未完成时不会漏计；outcome已发布但inflight尚未清零时，连续两次stats也只报告同一条有效事件；稍后持锁折叠会把增量移入heap基数并把pending归零，公开累计值保持不变。折叠使用饱和加法，且在`atomic_exchange`清零之前验证枚举范围、蕴含关系和descriptor重算结果，不能用“先清零再检查”的顺序吞掉语义损坏。

### 42.5 Verifier、fault 308与确定性窗口

常驻`nggc_check`在所有构建中检查classification状态机、descriptor sentinel、Region范围、generation合法性、outcome语义，以及parked生命周期约束；这不是只存在于Debug的正确性补丁。Verify+Test Hooks fault 308选择一个静止running线程，临时建立语义应为`RECORD`的null-value descriptor，却注入枚举本身合法的`RECORD|CROSS_REGION` outcome。`nggc_check`和`nggc_get_stats`都必须在exchange前返回corruption，恢复descriptor后heap继续可用。

24 KiB非2次幂Region的确定性回归覆盖：

1. 同RegionYOUNG引用只产生`RECORD`；
2. null写入使用`SIZE_MAX/NONE`并只产生`RECORD`；
3. classification推导前由另一线程取得heap lock完成stats、RemSet rebuild和full check；
4. outcome已release-publish但classification inflight尚未clear时，连续两次stats不漏计、不重复计；
5. YOUNG跨Region且global card已覆盖时产生`RECORD|CROSS_REGION`，card结果为`COVERED`且不append；
6. owner达到tenuring threshold后成为逻辑OLD，再指向新生YOUNG时唯一产生`RECORD|CROSS_REGION|OLD_TO_YOUNG`；
7. 所有窗口都经过full check、RemSet rebuild（适用时）、safepoint flush、release与unregister，最终pending为0。

最终矩阵为MSVC x64 Debug 13/13、x64 Release 13/13、x86 Release 13/13、x64 Release+Verify 13/13，以及MinGW threadless 12/12。确定性pause字段与分支只存在于Verify+Test Hooks目标，正式Release通过宏代码级移除；descriptor、snapshot和生命周期验证保留在Release。

### 42.6 Region兼容、准确边界与下一步

descriptor保存通用`size_t` Region index，不保存shift/mask，不假设Region为2的幂；Region仍由每个heap的`nggc_config.region_size`冻结，`0`表示默认64 KiB，显式值至少512 B并满足64-byte对象对齐与512-byte card对齐。512 B、8 KiB、24 KiB、128 KiB和1 MiB suite继续有效，ABI 78并发窗口明确使用24 KiB。32-bit构建只依赖指针宽度`size_t`与32-bit原子控制字段，不引入共享64-bit lock-free要求。

下一增量应继续保持分层闭包：

1. 为card/classification descriptor reservation建立per-thread owner-only fast gate与collector quiescence证明；
2. 在锁外化slot publication前冻结handle/object lifetime、payload并发和forwarding重校验协议；
3. 为card/mark日志加入epoch、collector drain/exchange、overflow assist和remark closure，之后才允许安全清零或复用card；
4. 在真实UHMAS约束稳定后，把单heap atomic-card表演进为Region RemSet，并保留coarse/full-scan降级；
5. root/handle/TLAB/barrier-buffer epoch全部闭包前，不开放mutator-concurrent REMAP或from-space立即复用。

因此ABI minor 78的准确表述是“C11 unique-owner写屏障的RemSet分类共享64-bit telemetry已改为锁外32-bit事件发布与后续持锁折叠，分类推导已基于冻结descriptor移出heap lock”。descriptor reservation、canonical slot publication、coarse/满buffer慢路径以及对象/forwarding/Assist校验仍持heap lock；它不是完整lock-free写屏障，也不是最终Region RemSet。

### 42.7 同批可靠性修复：TLAB pre-increment late enrollment

ABI minor 78 的最终并发压力回归还暴露了 ABI minor 71 TLAB fast gate 中一个既有的有限迟到窗口。fast allocator 的必要顺序是：

```text
acquire-load gate == OPEN
atomic inflight++
acquire-load gate == OPEN
validate active span and publish object
atomic inflight--
```

第一次 gate 读取与 `inflight++` 不是一个原子事务。observer 可以在两者之间执行 `OPEN -> CLOSED`，扫描到全部 inflight 为零并开始 flush；随后迟到 mutator 才登记 inflight。这个 mutator 在第二次 gate 检查失败前不会修改 header、payload、sentinel 或 fast top，因此有两种安全交错：

```text
observer在迟到登记前完成该thread flush
  -> 安全；mutator随后登记、看到CLOSED、撤销登记并进入slow path

observer的thread flush恰好观察到迟到inflight
  -> flush返回内部BUSY；closer等待后重新扫描全部thread并重做flush
```

第二种交错中的 `BUSY` 是 quiescence 协议内部的瞬态，不是公共 API 的失败结果。`nggc_tlab_fast_close_and_flush_locked` 因此使用闭环：关闭 gate，扫描所有 thread inflight；有活动发布就 yield/retry；扫描为零后逐线程 flush；任一 flush 返回 `BUSY` 就放弃本轮剩余 flush，重新从全体 inflight 扫描开始。只有完整一轮全部 flush 成功才返回 `OK`。非 `BUSY` 的 flush 错误仍按 corruption fail-closed。

这不要求在每次分配上增加共享全局 enrollment counter，也不要求 64-bit lock-free atomic。现有 per-thread 32-bit `atomic_uint` inflight 足够；安全性来自“二次 gate 检查之前绝不发布对象”和“closer 对 flush-BUSY 重建全局 quiescence”。closer 持有 heap lock，gate 保持 CLOSED，迟到者只能有限撤销，不能无限产生新的有效 fast publication。

Verify+Test Hooks 增加一个两阶段 enrollment pause 和一个 quiescent-to-flush pause，确定性执行：

```text
mutator读到OPEN，在inflight++前暂停
stats observer关闭gate，扫描到inflight == 0，在flush前暂停
mutator继续，inflight++后再次暂停
observer进入flush并确定观察到BUSY
mutator继续，二次检查看到CLOSED并inflight--
observer重新quiesce、flush并成功返回
mutator的allocation slow path成功完成
```

回归使用每 heap 显式 `region_size = 24 KiB`，最终要求两次分配都只发布一次、fast allocation计数为零、slow-path计数覆盖两次fallback、active TLAB归零且完整heap verifier通过。该测试连续100轮通过；原始x64 Release线程压力连续300轮通过。所有 pause/stage/observed 字段和分支仍只编入 `NGGC_ENABLE_VERIFY && NGGC_ENABLE_TEST_HOOKS` 目标，正式Release在预处理阶段完全移除测试握手；closer重试本身是Release常驻正确性协议。

## 43. ABI minor 79：动态 Pin Lease 与 Pinned Evacuation 降级

ABI minor 79 完成第一阶段的对象级 Pin 机制。它只固定 Pin 的正确性边界，不把尚未完成的并发 REMAP、pin owner/token 或长期 pinned space 整理提前宣称为完成。

### 43.1 对象语义

- `NGGC_TYPE_FLAG_PINNABLE` 只表示对象允许取得动态 Pin lease。普通 `PINNABLE` 对象仍以 `YOUNG, age=0` 出生，默认进入可移动 Young 空间。
- `nggc_pin(heap, object)` 与 `nggc_unpin(heap, object)` 使用对象头内的 `atomic_uint pin_count` 实现嵌套 lease；`nggc_object_pin_count` 只对 live object 返回当前深度。
- Pin 是移动约束，不是可达性约束，不增加 global/thread root，也不改变 mark 结果。不可达 pinned object 仍由 mark-sweep 回收，回收时清除 object/header、Region 和 heap 级计数。
- `NON_MOVABLE` 仍是出生即 `PINNED_SPECIAL` 的物理属性，始终不搬迁；它与动态 Pin count 分离。类型同时带有 `PINNABLE` 和 `NON_MOVABLE` 时可保持已有 pinned-special placement，动态 lease 仍单独计数。

### 43.2 Region 与 heap 统计

Region 同时维护 `pinnable_object_count` 和实际 `pinned_object_count`。后者按对象覆盖的每个 Region 计数，支持跨 Region 对象；首次 Pin 执行 0->1 增加，最终 unpin 执行 1->0 减少，嵌套 lease 不重复增加 Region object count。

`NGGC_REGION_FLAG_HAS_PINNED` 和 `pinned_region_count` 继续保持兼容的物理语义：Region 中有动态 pinned object 或 `NON_MOVABLE` object 时置位。只有具备 `PINNABLE` 能力但尚未 Pin 的 Young object 不会置位。heap stats 另外暴露：

```text
pinned_object_count
pin_lease_count
pin_acquire_count
pin_release_count
pin_busy_count
pin_unpin_error_count
```

`nggc_check` 从 live object walk 重建 heap/Region pinned object 和 lease 数，并检查所有 pinned object 都具备 `PINNABLE` 标志；free、TLAB_RESERVED、FORWARDED、decommitted 和 humongous free metadata 不得残留 pin count。

### 43.3 Pin、分配和 GC phase 交互

Pin/unpin 目前在 heap lock 下完成，并复用 TLAB fast gate quiescence：入口先关闭并 flush 已发布的 TLAB fast allocation，操作结束后才重新打开 gate。以下状态不允许修改 pin count，返回 `NGGC_ERR_BUSY` 并累计 busy telemetry：

```text
safepoint active/pending
standalone phase active
collector cycle active
Mutator Assist active
young evacuation/copy or forwarding publication
from-space quarantine/read barrier active
```

对象失效、旧 forwarding source、非 live object 或 lease 下溢返回非法参数；不具备 `PINNABLE` 能力返回 unsupported。当前 API 使用对象头嵌套计数，并由 ABI minor 84 的 lease ledger 记录已知 owner；独立 token/owner object、超时 lease、调试泄漏报告和线程异常退出自动处理仍留在后续协议。

### 43.4 Evacuation 与 forwarding 协议

young planning、目标空间 preflight、copy reservation、forwarding validation、quarantine 和 sweep 都把 `pin_count != 0` 的对象排除在 evacuation 之外。Pinned Young object 留在原地址并在适用的 young collection 中增龄；达到阈值后仍保留为 Young tenuring candidate，直到解除最后一个 lease。解除后，下一轮 collection 才可正常复制并按对象年龄晋升 OLD。

forwarding source/target 必须为零 Pin。已经发布 forwarding 的 source 不能被重新 Pin，from-space quarantine 中不能出现 pinned object；这使 read barrier 不需要在 forwarding 目标上合并 lease 状态，也避免 source/target 同时持有移动禁止状态。

### 43.5 回收、降级与验证

Pin 过多、目标空间不足、RemSet 尚未闭包或并发 moving 尚未满足 handshake 时，collector 使用已有 non-moving mark/sweep、原地 pinned Young 保留或 from-space quarantine 降级，不依赖 page fault、memory protection、signal 或 SEH。Pin 本身不会阻止不可达对象回收，也不会制造 OOM 保活假象。

新测试覆盖 nested Pin、busy phase/safepoint、unpinned-vs-pinned Region flag、pinned Young 原地增龄、threshold candidate、unpin 后复制晋升、不可达 pinned object 回收、owner 注销门禁、跨 owner 冲突、统计守恒和完整 `nggc_check`。后续增量应在并发工作闭包完成后再考虑独立 token、按线程 pin 诊断、长期 pinned space 分离/整理和 concurrent REMAP。

## 44. ABI minor 80：Fail-Stop Quarantine 管理面与 TSS 实例隔离

ABI minor 80 为 post-forwarding emergency completion 建立最小、可诊断的 fail-stop 管理面。它不尝试从半完成 moving cycle 回滚，也不把 fail-stop heap 当作可继续服务的普通 heap；目标是保留故障现场、拒绝继续暴露不一致状态，并提供宿主明确销毁的出口。

### 44.1 Fail-stop 生命周期

- heap 增加 `RUNNING` 与 `FAIL_STOP` 生命周期状态。emergency completion 自身失败时，在仍持有 heap lock 的状态下保存第一次失败原因并切换到 `FAIL_STOP`；当前 safepoint、collector phase、collection-set、forwarding 和 quarantine 元数据原样保留。
- `nggc_get_heap_state` 可读取状态和保存的 `nggc_result` 原因。普通分配、收集、phase、root、thread、barrier 及其他要求 running heap 的 API 在 fail-stop 下不再执行；普通 `nggc_close` 保持 busy，避免绕过注册线程和 safepoint 约束释放半完成 heap。
- `nggc_force_destroy` 只接受 `FAIL_STOP` heap。宿主必须先停止所有仍可能访问该 heap 的 mutator、collector worker 和外部 root 访问者；该 API 不负责系统级线程挂起、不负责修正外部指针，也不提供恢复/继续收集语义。确认停止后它绕过普通 close gate，释放 worker、arena、registry 和其余 heap 资源。
- fail-stop 与正常 `FORWARDED` from-space delayed reclaim 是两种不同状态：后者在验证过的后续 safepoint 中可回收并恢复 running，前者只能查询诊断后销毁。`FAIL_STOP` 不是 OOM 降级路径，也不是 page fault、signal、SEH 或锁迁移方案。

### 44.2 TSS 缓存生命周期与地址复用

- C11 barrier TSS entry 不再用 heap 地址作为实例身份，而使用进程内单调递增的 64-bit heap identity。这样即使旧 heap 的地址被新 heap 复用，已停止线程留下的旧 TSS entry 也不会命中新 heap。
- 当前线程销毁 heap 时先尝试以 `tss_set` 成功为提交点摘除自己的 entry，只有摘除成功后才释放节点；其他已停止线程的私有 entry 不从外部线程强行修改，待其线程析构释放，并由 heap identity 保证失配安全。
- identity 分配只依赖 C11 原子 CAS，不假设 64-bit 原子 lock-free；无 C11 threads 构建保留原有无 TSS registry fallback。该修复是生命周期正确性边界，不代表 barrier 已经 lock-free。

### 44.3 验证边界

phase verifier 新增 emergency completion 失败、状态/原因查询、普通 API 拒绝、普通 close 门禁和 force-destroy 流程，并覆盖 force-destroy 后新 heap 的 forwarding CAS/check 回归。当前 x64 Debug、x64 Release、Verify Debug、x86 Release 已各通过 14/14，MinGW 无 C11 threads 已通过 13/13；Profile 构建和更广泛的 sanitizer/故障注入压力仍需继续验证。

ABI minor 80 不推进移锁、lock-free barrier、registered-mutator concurrent REMAP、root/handle/barrier-buffer epoch 闭包或长期 pinned space。上述并发工作仍按 ABI minor 73-78 的 reservation、slot publication、TLAB gate 和 RemSet 未完成项继续拆分。

## 45. ABI minor 81：Per-Thread Mutator Assist、公平性与动态预算

ABI minor 81 完成 Mutator Assist v1 的调度增量。它仍然是 opt-in、allocation-failure 驱动的 bounded mark/sweep assist：没有后台 assist worker，没有独立的 lock-free work queue，也没有把 root snapshot 或 reference processing 拆成可并发 packet。所有债务、packet 状态和计数器仍在 heap lock 下维护，因而本 ABI 不改变当前写屏障、TLAB 或 registered-mutator REMAP 的并发边界。

### 45.1 债务与 owner 归属

分配成功后按实际 allocation bytes 增加 heap-level `mutator_assist_debt_bytes`；如果分配调用带有当前 heap 的合法注册线程，则同时增加该线程的 `mutator_assist_debt_bytes`。没有可证明 owner 的普通分配只增加 heap debt。两者使用 `size_t`，累计遥测使用饱和 `uint64_t`，不依赖共享 64-bit lock-free atomic。

Assist packet 完成 sweep 工作后按以下顺序偿还：

```text
current assist actor debt
  -> other registered thread debt
  -> unowned remainder in heap debt
```

偿还总量不超过 heap debt；常驻 verifier 重新累加所有注册线程 debt，并要求它不超过 heap debt，差额表示无 owner 的分配债务。actor 只在调用时仍注册且属于该 heap 时享有优先偿还；线程注销前已有 debt 会自然转为 heap-level remainder，不留下悬挂的 thread owner。

### 45.2 可恢复 packet 与内部阶段

内部 Assist 状态只有 `IDLE`、`MARK`、`SWEEP`，阶段入口仍使用公共 phase verifier 保护：

```text
IDLE
  -> ROOT_SNAPSHOT -> MARK
  -> bounded MARK packets
  -> REFERENCE_PROCESS
  -> bounded SWEEP packets
  -> rebuild free list -> IDLE
```

首次 packet 在 `ROOT_SNAPSHOT` 建立当前 mark epoch 和持久 mark stack，之后每次分配失败最多执行一个 bounded packet。MARK packet 只消费 mark stack；引用闭包为空后才进入 REFERENCE_PROCESS 和 SWEEP。SWEEP packet 以持久 arena cursor 扫描 block，处理不超过本轮 byte budget；cursor 到达 arena high-water 后重建 free list、完成 collection 并清空 Assist 临时状态。任何完整 `nggc_collect` 都会在接管前取消并清理未完成 Assist cycle。

MARK 使用 incremental-update 写屏障：Assist MARK 期间新 root 或新写入的合法边会进入当前 mark epoch；SWEEP 期间指向未标记对象的写入按既有安全规则拒绝或走调用者可观察的 busy 路径，不能让 packet 在已开始清理后重新打开任意 mark 闭包。

### 45.3 四档动态预算

为了让上限可验证且不受任意策略值影响，ABI minor 81 固定四档 packet budget：

| debt pressure | MARK budget | SWEEP budget |
|---|---:|---:|
| baseline | 8 items | 1024 bytes |
| debt >= 4 KiB | 16 items | 2048 bytes |
| debt >= 16 KiB | 24 items | 4096 bytes |
| debt >= 64 KiB | 32 items | 8192 bytes |

pressure 取 heap debt、当前 actor debt 和本次失败分配请求三者的最大值。每次实际运行的 packet 将当前 budget 写入 stats；动态档位切换、MARK item 数和 SWEEP bytes 都受 `size_t`/32-bit 可表示范围约束，验证器拒绝超出最大档位或 active 状态下 budget 为零的 heap。`mutator_assist_dynamic_budget_count` 只累计实际偏离 baseline 的 packet 预算选择。

### 45.4 公平性与无进展退避

heap 记录最后一个 assist actor 和其连续 packet 数。若同一注册线程已连续执行 4 个 packet，且另一个注册线程仍有非零 debt，则下一次 packet 强制回到 baseline budget 并清零连续计数，累计 `mutator_assist_fairness_cap_count`。这只限制单个 actor 的 packet 连续占用，不会伪造或清除其他线程的债务。

每个注册线程还记录 no-progress streak 和 backoff remaining：连续 2 个没有 mark/sweep/root/reference 实际进展的 assist 尝试后，跳过接下来的 2 个 allocation-failure assist 尝试；跳过次数累计到 heap 的 backoff stats。任何实际进展都会清零该线程的 streak/backoff。退避不减少债务、不改变 phase、不阻塞 safepoint；分配路径随后按普通 slow-path/collection trigger 决策处理。

公开 stats 至少保持以下可诊断关系：`pending_thread_count` 是当前非零 thread debt 的数量，`max_thread_debt_bytes` 是注册线程 debt 最大值，heap/thread packet、progress 和 no-progress 计数分别反映总量与注册 actor 子集，fairness cap、dynamic budget 和 backoff 计数只在对应事件发生时增加。`nggc_check` 同时检查 thread debt 和 heap debt 守恒、active/idle budget、last actor 注册状态、streak/backoff 范围和 sweep cursor。

### 45.5 验证边界与后续工作

ABI minor 81 的回归覆盖跨 packet mark backlog、MARK 写屏障、partial SWEEP 保活、full collection 接管，以及双注册线程生成债务后由另一 actor 连续 assist 的动态预算、公平性 cap、actor progress 和 heap check。该增量保持非 2 次幂 Region、32-bit 可表示控制字段和无 `<threads.h>` fallback 的既有合同。

仍明确留待后续的工作包括：

1. 基于 ABI minor 85 的 bounded reference-processing packet，以实测数据校准 reference/mark packet 档位和 Assist debt；
2. 把 mark/card/Assist 日志接入 concurrent mark、remark、epoch exchange 和 collector drain，并据实测校准 debt ratio 与四档阈值；
3. 只有完成 root、handle、TLAB、barrier-buffer、RemSet 和 forwarding 的完整 epoch 闭包后，才考虑并发 worker/lock-free work queue、mutator-concurrent REMAP 和 from-space 立即复用；
4. descriptor reservation、canonical slot publication、完整 barrier 移锁、按线程/span 的 TLAB quiescence 仍遵守 ABI 73-78 的未完成 TODO，本 ABI 81 不提前推进这些任务。

## 46. ABI minor 82：可解释回收触发器与暂停反馈

ABI minor 82 将“何时请求回收”从分配失败的隐式副作用提升为可观察、可替换的策略层。它没有改变 collector 的 phase 图、对象图、引用强度或 safepoint 所有权，也没有开始 descriptor reservation、canonical slot publication、RemSet epoch 闭包或其他移锁任务。

### 46.1 配置与策略

`nggc_config` 新增以下冻结配置：

- `gc_trigger_policy`：`SMART`、`HEAP_LIMIT`、`ALLOCATION_RATE`、`LATENCY_TARGET`、`THROUGHPUT`、`MEMORY_SAVING` 或 `MANUAL`；零值为 `SMART`。
- `gc_trigger_soft_limit`：零值为 arena 的 75%，非零值必须不超过 arena 大小。
- `gc_trigger_quantum`：零值为当前 heap 的 `region_size`，用于分配速率、滞回和 assist debt 的最小触发粒度。
- `gc_trigger_latency_target_ns`：非零时必须同时提供已有的单调 `clock_ns` callback，零值表示不启用暂停反馈。

自动触发必须显式设置 `NGGC_CONFIG_COLLECT_AUTO_TRIGGER`；仅配置策略不会让每次分配进入策略评估。显式 `nggc_collect_if_needed` 可以独立使用策略评估。`MANUAL` 永不自动收集，未达到条件的评估以成功 no-op 返回；调用者仍可直接使用 `nggc_collect` 或 `nggc_collect_young`。

### 46.2 SMART v0 决策边界

当前实现是可解释的阈值/滞回控制器，不是机器学习预测器。决策在 heap lock 内只读取冻结的 heap 使用量、请求大小、累计分配量、上次 collection 分界、Mutator Assist debt、是否存在可收集 Young 对象和可选 pause EWMA，然后输出 `NONE`、`YOUNG` 或 `FULL` action 及原因：

```text
projected = arena_used + requested_bytes
pressure  = projected >= policy_threshold(soft_limit)
rate      = allocation_delta >= quantum || requested_bytes >= quantum
assist    = assist_debt >= 4 * quantum
critical  = projected >= arena_size
```

`HEAP_LIMIT`、`THROUGHPUT` 和 `MEMORY_SAVING` 使用不同的确定阈值 profile；`ALLOCATION_RATE` 在压力或速率到达时请求回收；`SMART` 在压力或 assist debt 到达时请求回收，并在严重压力时升级为 FULL。存在 live Young 时，低风险策略可请求 YOUNG；memory-saving 或 critical 情况请求 FULL。最近一次 collection 后至少跨过一个 quantum 才再次触发，critical/pending/assist debt 是例外。

策略只能提交请求，不能直接修改对象头、phase、线程状态、forwarding 或 RemSet。策略执行仍复用现有 `nggc_collect` / `nggc_collect_young` coordinator，因此所有已有 preflight、pin、forwarding、emergency completion、fail-stop 和 OOM 降级继续生效。

### 46.3 分配路径与 registered mutator 边界

自动策略只挂在公开分配的 slow path、TLAB fast allocation 失败回退边界和内部 special-object 分配入口；不会进入 TLAB fast path 的热循环。评估先关闭并 flush 当前 heap-level TLAB fast gate，决策在锁内完成，实际 collection 在解锁后执行，避免递归持锁。

当前实现对 `registered_thread_count != 0` 的自动评估保持 advisory no-op，不强行替代宿主的 safepoint handshake。宿主需要显式停车所有 mutator 后调用 collection，或由 collector-owned thread 使用 `nggc_thread_collect`；显式 `nggc_collect_if_needed` 在无法完成现有 handshake 时返回已有 `NGGC_ERR_BUSY` 语义。触发器不会绕过 root、handle、TLAB、barrier-buffer 或 RemSet 的生命周期合同。

### 46.4 暂停采样与降级

当 `gc_trigger_latency_target_ns != 0` 时，collector 在已确认 collector owner 后调用单调 `clock_ns` 记录 collection pause 起点，并在 collection 成功、普通 abort 或 post-forwarding fail-stop 出口记录终点。采样更新：

- `gc_trigger_last_pause_ns`：最近一次有效 collection 尝试的耗时；
- `gc_trigger_pause_ewma_ns`：alpha 为 1/8 的整数 EWMA；
- `gc_trigger_pause_sample_count`：有效样本数；
- `gc_trigger_clock_failure_count`：callback 失败、时间回拨或溢出样本数。

没有 callback、target 为零、callback 失败或时间回拨都不阻断 collection，也不使用 wall clock fallback。latency policy 在没有有效反馈时仍按确定性 profile 工作。Region VM 的已有 clock telemetry 与本触发器 pause telemetry 独立统计。

### 46.5 验证与未完成工作

Debug/Verify 回归覆盖 SMART 自动触发、MANUAL no-op、非法策略/soft limit、LATENCY pause sample/EWMA 以及 clock failure 不阻断 collection。Release 通过宏移除 verifier、故障注入和高成本诊断路径；pause callback 只有配置 target 非零时才调用。

仍待后续：基于真实 pause 分布的 p95/p99、分配速率和存活率校准，后台 coordinator、concurrent mark/remark 触发，以及 pressure provider/Region RemSet 的深度集成。任何这些工作都必须先完成 root/handle/TLAB/barrier-buffer/RemSet/forwarding epoch 闭包，不能借 ABI 83 把当前实现称为完整并发 GC 或 lock-free barrier。

## 47. ABI minor 83：collector-owned discovered-list v0

ABI minor 83 只解决引用发现结果的内部组织和生命周期，不改变四种引用的语义，也不把 reference processing 变成并发阶段。Weak/Soft/PhantomReference 都是特殊对象；PhantomQueue 仍是唯一具有非空自保活待遇的特殊队列，普通容器或其他队列不继承该规则。

### 47.1 所有权与状态

discovered-list 由 collector 独占，所有读写都在现有 heap lock 内完成。每轮 mark epoch 开始前清理上一轮 intrusive link，并进入 `ACTIVE`；标记阶段发现已经 mark 的 Soft/Weak/PhantomReference 时，按类型追加到三个独立列表。reference processing 进入 `DRAINING`，完成后必须回到 `IDLE`，epoch 和各对象的 discovered metadata 同时清零。

列表节点复用特殊引用对象自己的 payload 字段，不增加外部节点分配。每个节点记录 next 和本轮 discovered epoch；head、tail、count 必须一致，节点必须是当前 arena 中具有匹配 object id、type kind 和 mark epoch 的 live 对象。列表为空并不意味着对象可达性改变，它只表示本轮发现工作已经被消费。

### 47.2 处理闭包

处理顺序固定为 Soft、Weak、PhantomReference，最后扫描有 pending item 的 PhantomQueue：

1. Soft 根据既有 policy 保留或清除 referent。保留 referent 会重新进入 mark stack；新标记产生的 Soft 节点和普通强边必须继续循环，直到 Soft list 与 mark stack 同时为空。
2. Weak 只观察本轮 mark 结果，未标记 referent 被清零。
3. PhantomReference 只观察本轮 mark 结果，未标记 referent 被清零并追加到自己的 PhantomQueue；关联 queue 被强制标记。已经 `enqueued` 的 PhantomReference 已在队列中被保活，不再次加入 discovered-list。
4. 非空 PhantomQueue 继续保活自身和队列内 PhantomReference；空 PhantomQueue 没有额外保活权。

处理失败时 collection 必须走已有 abort/fail-stop 路径，并清理列表。Assist reset、下一轮 mark epoch 和正常成功 drain 也必须清理列表，因此 forwarding、sweep、reclaim 前不会留下指向将移动或释放对象的 discovered link。

### 47.3 可观测性、验证和边界

stats 暴露 discovery state、epoch 和 Soft/Weak/Phantom pending count；`nggc_check` 在常驻路径验证状态枚举、epoch、链表拓扑、节点身份以及 IDLE 状态下无残留 metadata。Debug/Verify 额外复查引用对象和队列的 mark 闭包。测试覆盖嵌套 Soft discovery、Soft -> Weak/Phantom 顺序、队列非空自保、列表清空和最终回收。

本 ABI 仍不提供并发引用列表、无锁发布、跨线程 epoch exchange 或最终 Region RemSet 表示。ABI minor 85 的 bounded packet 仍完全依赖现有 heap lock；上述并发工作必须等 root/handle/TLAB/barrier-buffer/RemSet/forwarding 的并发闭包完成后再单独设计；本 ABI 不启动任何 descriptor/slot publication 移锁任务。

## 48. ABI minor 84：Pin Lease 归属账本

ABI minor 84 在 ABI minor 79 的动态 Pin 基础上增加最小的 owner/lifetime ledger，但继续沿用现有 heap lock。它解决的是“谁取得的 Pin 必须由谁释放，以及线程退出前不得遗留 lease”的生命周期一致性问题，不开启移锁、lock-free barrier、并发 REMAP 或长期 pinned space。

### 48.1 记录与归属

- 每个 lease record 以 `(object header, object_id, owner)` 为键；同一键的嵌套 Pin 只增加 record count，不重复分配 record。`object_id` 用于防止地址复用后把旧 lease 误认成新对象。
- 当前线程只有一个可证明的注册 handle 时，记录 owner 为该 `nggc_thread`；没有注册 handle 时使用匿名 owner `NULL`。同一线程同一 heap 存在多个 handle 时不猜测 owner，Pin/unpin 返回 `NGGC_ERR_BUSY`。
- `nggc_unpin` 只允许当前 owner 对应的 record 释放。对象仍有其他 owner 的 lease 时，跨 owner 释放返回 `NGGC_ERR_INVALID_ARGUMENT` 并累计冲突统计；它不会降低对象 pin count。
- `nggc_thread_unregister` 在摘除 thread record 前汇总其 lease count。非零时返回 `NGGC_ERR_BUSY`，保留线程和全部 lease 不变；释放完 lease 后才允许注销。线程退出不会隐式替用户释放 Pin。

### 48.2 守恒与回收

常驻 verifier 对 lease 链执行有界遍历、owner 注册性、对象身份、唯一键和计数检查；对每个对象汇总所有 record count，并要求等于对象头 `pin_count`，再要求所有对象总和等于 heap `pin_lease_count` 与统计快照。回收 live object 时先删除它的全部 lease records，再清除对象头与 pinned 计数；因此不可达 pinned object 仍可回收，不会被 ledger 伪造为 root。

stats 新增 `pin_lease_record_count`、`pin_owner_conflict_count` 和 `pin_thread_exit_busy_count`。这些字段是诊断数据，不成为 collector 正确性的替代证明；`nggc_check` 仍是完整一致性入口。

### 48.3 验证边界与后续

回归覆盖 nested owner lease、持 lease 注销拒绝、释放后的正常注销、跨 C11 worker owner unpin 冲突、冲突统计和完整 heap check。无 `<threads.h>` 构建保留匿名 owner 与同一 heap lock 语义，不依赖平台线程 API。

后续仍保留独立 token/lease handle、超时与泄漏诊断、线程异常退出管理、长期 pinned space 分离/整理、pin-aware mixed compaction，以及完成 root/handle/TLAB/barrier-buffer/RemSet/forwarding epoch 闭包后的 concurrent REMAP。上述并发工作完成前，不得把本账本解释为 lock-free Pin 或完整并发 GC。

## 49. ABI minor 85：bounded reference-processing packet

ABI minor 85 只把 Mutator Assist 的 reference-processing 从原子步骤切成可恢复的 bounded packet；它不引入并发 reference discovery、并发 worker、无锁 work queue，也不改变 Soft/Weak/Phantom 的语义和处理顺序。完整 `nggc_collect` 仍使用原子 reference-processing 路径。

### 49.1 跨 packet 状态

Assist 在 `MARK` 完成后进入独立的 `REFERENCE_PROCESS` 状态。该状态由现有 heap lock 保护，reference discovered-list 保持 `DRAINING`，并持久化以下恢复信息：

- mark stack：Soft 保留 referent、PhantomQueue 和增量写屏障产生的闭包继续使用同一个 heap mark stack；
- reference kind：固定按 Soft、Weak、PhantomReference、PhantomQueue 四段推进；发现新条目时允许回到更高优先级段以闭合跨 packet 发布；
- PhantomQueue member cursor：当前 queue、object id、下一个成员和剩余成员数，保证队列成员不会重复或遗漏；
- PhantomQueue arena cursor：只扫描当前 packet 的有限 arena block，arena 扩大时在后续 packet 扩展 scan end；
- stats：packet 次数、处理 item 总数、无进展 packet 次数和 pending work 指示器均可观测。

一个 packet 的预算沿用 Assist 的动态 mark-item budget。列表条目、mark-stack item、队列成员和 arena block 各消耗一个 packet item；PhantomQueue 的 packet 路径只做 O(1) 首尾/身份检查，完整链表验证仍由 Verify 和完成态检查承担。

### 49.2 完成、接管和失败

只有 Soft 闭包、Weak list、PhantomReference list、PhantomQueue scan/member cursor 和持久化 mark stack 全部清空后，才调用 discovered-list finish 并进入 `SWEEP`。packet 之间允许 mutator 通过常驻写屏障发布新强边；新标记的特殊引用会按优先级重新排队，不会在队列扫描结束时被误判为已完成。

完整 collector 开始时先执行已有 Assist reset，清理 mark stack、discovered-list 和所有 reference cursor，再从新的 mark epoch 重新处理引用。packet 内部错误、对象身份不匹配、队列链损坏或 metadata OOM 同样走 Assist reset；不会把半完成的 reference-processing 状态带入 Sweep 或下一轮。`nggc_check` 在 `REFERENCE_PROCESS` 中接受尚未消费的 referent mark 结果，但仍验证 list 拓扑、队列身份、object id 和计数守恒。

专用 takeover 回归会在 Assist 暴露 `REFERENCE_PROCESS` 且仍有 pending work 时立即调用完整 collector，检查旧 packet 状态被清理、引用重新发现并最终回到 `IDLE`；该路径不改变锁模型，也不宣称并发 reference processing。

该协议仍建立在现有 heap lock 和协作式 safepoint 上，故它是低暂停的工作切片原型，不是并发 GC 的证明。并发 reference discovery、epoch exchange、worker-owned packet、无锁发布和最终 Region RemSet 仍需等待 root/handle/TLAB/barrier-buffer/RemSet/forwarding 的并发闭包。

## 50. ABI minor 86：可选 Young Region 整区原地晋升

ABI minor 86 增加 `NGGC_CONFIG_YOUNG_IN_PLACE_PROMOTION`，把“复制到 OLD”优化为一个严格受限的整区原地晋升候选。它只减少已经满足晋升条件的 Young Region 的复制和 forwarding，不改变对象年龄规则、Pin 规则、引用语义、写屏障或 phase 图。该选项默认关闭，关闭时 ABI minor 45 的普通 Young evacuation 路径完全不变。

### 50.1 候选判定

候选在 Young collection 的 marking/reference processing 完成后、任何 evacuation forwarding 发布前，以当前 heap lock 和当前 mark epoch 一次性计算。每个 Region 单独判定，允许同一轮 collection 混合存在原地晋升 Region 与普通 evacuation Region。候选必须同时满足：

- Region 属于本轮 collection-set，物理 generation 是 `YOUNG`，已提交且至少有一个存活对象；没有活动 TLAB 或其他未完成的 Region reservation。
- Region 内每个 live block 都在当前 mark epoch 内，逻辑 generation 仍为 `YOUNG`，且 `next_age` 达到 `young_tenuring_threshold`。因此“整区晋升”不会绕过对象级年龄阈值，也不会让新生对象直接进入 OLD。
- 所有存活对象的 Pin count 都为零，并且没有 `LARGE` 或 `NON_MOVABLE` 标志。单个 pinned/non-movable/large 或未达龄对象会使整个 Region 放弃原地候选；该 Region 的对象回到普通 evacuation 或既有非移动降级路径。
- 所有候选对象没有既有 forwarding。候选计算失败、计划元数据分配失败或提交前发现状态不一致时，不发布部分原地晋升结果；collection 按现有错误/降级协议处理。

候选表只保存每个 Region 的标志，不把 Region 地址编码进对象头，也不要求 Region 是 2 的幂。`region_size` 继续是 per-heap 配置值，仍受 card、对象对齐、地址空间和 metadata 可表示范围约束。

### 50.2 提交与混合 collection

规划器跳过候选 Region 的 evacuation task 和目标空间预留，仅为其他 Region 生成 Survivor/OLD 复制任务。普通 Region 仍执行原有 worker/coordinator copy、header forwarding、REMAP、from-space quarantine 和 delayed reclaim；因此候选 Region 不会被当成 from-space，也不会制造需要读屏障跟随的伪 forwarding。

所有普通 sweep/evacuation 操作完成后，提交函数再次验证候选 Region 的 mark epoch、逻辑 generation、Pin count、对象标志、forwarding 和 TLAB 状态。验证通过后逐个 live object 设置逻辑 generation 为 `OLD`、清零 young age，再把 Region generation 设置为 `OLD` 并移出 collection-set。死亡 block 仍由本轮 Young sweep 回收；存活对象的地址保持不变，引用无需因该 Region 本身发生重写。

只有实际完成的对象才计入 `young_promoted_object_count` 和 `young_in_place_promoted_object_count`。原地晋升不增加 evacuated-object、evacuation-task 或 forwarding-CAS 统计；普通 Region 的统计继续按原协议累计。提交后的 free-list rebuild、Region metadata rebuild、RemSet coverage 和 verifier 仍照常运行。

### 50.3 失败、Pin 与验证边界

候选资格不是 Pin 的保活权。不可达 pinned object 仍会被 sweep 回收；仍有 lease 的可达 pinned object 只会阻止其 Region 原地晋升或移动，并按既有 pinned Young 规则继续增龄，unpin 后才可能在后续 collection 移动或晋升。整区候选不会把特殊 PhantomQueue 的队列自保活规则扩展给普通容器，也不会改变 Weak/Soft/Phantom 的处理顺序。

若候选发现或提交在 forwarding 发布前失败，collection 保持可回退边界；若同一轮已有普通 Region 发布 forwarding，则继续遵守 ABI minor 47 的已提交事务和 emergency completion 协议，不尝试回滚已发布 forwarding。若提交后的 verifier 或 free-list rebuild 失败，沿既有错误/fail-stop 路径处理，不能留下“部分 Region 已晋升、部分对象仍按 Young 解释”的状态。

Debug/Verify 检查候选 Region 的全体存活对象、年龄阈值、Pin/flag/forwarding、TLAB inactive、Region generation 和对象统计守恒。专项回归覆盖阈值前仍复制、阈值后原址晋升、混合 collection、候选 Region 含死对象、pinned/large/non-movable/未达龄对象拒绝候选，以及自定义非 2 的幂 Region。Release 通过宏移除高成本验证和故障注入代码；该增量仍使用单一 heap lock 与协作式 safepoint。

ABI minor 86 不推进移锁、无锁屏障、并发 REMAP、并发 reference processing、最终 Region RemSet 或长期 pinned space 整理。后续只有在 root/handle/TLAB/barrier-buffer/RemSet/forwarding 的并发闭包完成并有收益数据后，才评估 Region 级 promotion bitmap、候选收益评分、并发候选计算或与 mixed collection 的更深融合。

## 51. ABI minor 87：PhantomQueue 事件驱动阻塞等待

ABI minor 87 补齐的是 reaper 的等待机制，而不是后台 GC/reaper worker。`nggc_phantom_reaper_wait` 的既有有界语义保持不变：它最多按调用者要求启动指定次数的 collection；新增 `nggc_phantom_reaper_wait_blocking` 不启动 collection，也不替调用者消费队列，只等待 watched `PhantomQueue` 出现 pending item 或 watched set 发生变化。

### 51.1 事件协议

- `phantom_reaper_event_epoch` 是 heap 内部的 `atomic_uint_fast64_t`，初始为零；它只在 heap lock 内推进并以 release 发布。计数回绕跳过零，不把事件 epoch 当作对象身份或存活证明。
- PhantomReference 入队、PhantomQueue pop、watch/unwatch、stale weak-watch 删除，以及 collection 成功/失败收尾都会推进 epoch。通知是 best-effort 的唤醒优化，队列和观察表的正确性仍完全由 heap lock 与 object id 校验保证。
- waiter 先在 heap lock 下 poll 并读取 watched count/epoch，释放 heap lock 后才取得 condition-variable lock；取得后以 acquire 重新检查 epoch，再进入条件等待。这个顺序避免 heap lock 与等待锁互相等待，epoch 谓词覆盖“检查到入睡”之间的丢通知窗口。
- C11 `threads.h` 可用时复用已初始化的 condition variable。它与 safepoint waiter 共享唤醒设施，但不共享状态谓词；虚假唤醒只会重新 poll。没有 `<threads.h>` 的构建保留原子等待降级，不假设平台存在私有阻塞 API。

### 51.2 所有权和生命周期

阻塞 waiter 不注册为 mutator，不拥有 PhantomQueue，也不成为 root；它只能返回 queue 指针和 pending snapshot，实际 `pop` 仍由调用者执行。空队列仍没有额外保活权，非空队列继续只按 PhantomQueue 专属规则自保活。观察表失效或被 unwatch 后，waiter 被唤醒并在 watched set 为空时返回 `NGGC_ERR_BUSY`。

当前 heap close 仍要求宿主保证没有线程访问 heap；本 ABI 没有把匿名 reaper waiter 纳入 close 引用计数，也没有引入取消 token、超时 API 或后台线程 join。真正后台 reaper worker 必须先定义消费回调、消费权转移、错误传播、取消/关闭握手和 queue lifetime contract，不能仅由当前通知接口推导出来。

### 51.3 验证边界

回归覆盖条件等待尚未收到 enqueue 时保持阻塞、unwatch 触发 `BUSY`、collection enqueue 后返回正确 queue/pending、随后 pop 与 stale queue 清理，以及 heap verifier 和 reaper 统计守恒。该增量仍使用单一 heap lock、协作式 safepoint 和常驻写屏障；不推进 descriptor reservation/slot publication 移锁、lock-free barrier、并发 REMAP、并发 reference processing 或最终 Region RemSet。

## 52. ABI minor 88：scanner 扩展与 GC worker pool 故障闭环

ABI minor 88 处理两类会直接影响可靠性的边界：用户 scanner 的显式失败/协作取消，以及持久 GC worker pool 的条件变量和线程 join 失败。它们都保持现有 heap lock、协作 safepoint 和 coordinator ownership，不把当前 worker pool 宣称为 lock-free runtime。

### 52.1 `scan_ex` 协议

- `nggc_type_desc` 尾部增加可选 `nggc_scan_fn_ex`；旧 `nggc_scan_fn` 继续有效，描述符只复制调用者声明的 `size` 覆盖范围，因此旧描述符不会读取新字段。
- `scan_ex` 与 legacy scanner 使用同一 visitor、slot 边界、对齐、对象身份和不可缓存 visitor/context 契约。`nggc_scan_control` 只在当前同步调用内有效。
- scanner 可以在有限工作片段之间调用 `control->poll(ctx)`。poll 或 scanner 返回非 `NGGC_OK` 时，当前扫描立即结束，错误由 mark、RemSet、REMAP、reference processing 和 `nggc_check` 保留并向上层传播；这只是协作取消点，不是强制抢占。
- Verify-only 同线程 API 重入诊断已完成，见 52.4；长时间阻塞、超时、跨线程滥用、缓存上下文和执行预算诊断仍未完成。Release 通过宏移除高成本 replay/诊断，不移除对象图正确性所需的 slot 校验。

### 52.2 worker pool 状态与故障处理

持久 pool 的硬状态为：

```text
IDLE       job == NULL, requested == completed == running == 0
DISPATCHED 唯一 job/context + 非零 dispatch epoch，等待 worker 完成
BROKEN     不再接收新 job，只允许 coordinator fallback 或 close
SHUTDOWN   不接收新 job，等待全部 join；失败 close 保留该状态供重试
```

- coordinator 的 condition wait 失败不会提前清理栈上的 job/context；它会继续以短时等待/轮询方式等待已派发 worker 归静，再将原始 backend 错误返回。
- 每个 worker slot 用 32-bit `atomic_uint failed` 独立发布终结性的显式失败；mutex 内的 `running` 与 `completed_dispatch_epoch` 描述该 slot 当前 job 的运行和结束记账。正常结束与 coordinator 对失败 slot 的补记共用同一 epoch，因此执行完成后才失败也不会增加第二次 completed。失败发布之后 worker 不再访问 job/context；成功获取 pool lock 的 coordinator 能以 acquire 读取该终结状态。
- ensure/dispatch 刷新失败状态，失败 pool 不再接收 job。若失败发生在派发之后，coordinator 在每次短时 wait 返回后核对失败 slot，仅对尚未结束记账的选中 slot 补记并清理 running。job/context 只在所有选中 slot 已正常结束或发布失败之后清空。copy/REMAP 同时核对实际参与者的 entered/completed 与 active 为零；失败情况下允许部分选中 worker 尚未进入 job，返回 backend 错误并沿用既有 emergency completion。
- pool wait 另行输出 `drained` 证明，只有 `completed == requested && running == 0` 且 job/人数一致才成立。copy/REMAP 必须同时满足该证明和实际参与者归静；不能把普通 backend 错误返回当成 worker 已结束的证据。
- dispatch 或 close 的 condition broadcast 失败会保留 pool、worker slot、同步原语和 heap handle。当前 collection 可由 coordinator 接管，close 之后可重试；不能在第一次失败时销毁仍可能被 worker 使用的对象。
- join 失败只保留尚未成功 join 的 slot；成功返回但 worker 返回非零时，该 slot 视为已经 join，再返回 backend 错误，避免二次 join。所有同步原语只在全部 join 成功后销毁。
- 失败 close 不把 heap 提前标记为 closed；`nggc_get_heap_state` 仍报告 running，下一次 `nggc_close` 继续 teardown。pool verifier 接受 shutdown/retry 临时态，但 active job、worker counter、slot owner/index 和 job/context 一致性仍是硬错误。
- `cnd_wait` fault 分别覆盖 coordinator 与 idle worker；worker 阶段故障固定选择 slot 0，另有 copy/REMAP 各自的执行前、执行后和完成记账后注入。执行后失败不广播通知，验证短时 wait 能独立观察原子失败状态；coordinator wait 注入时暂缓 worker 完成发布，使该等待点确定可达。这些分支与控制字段只存在于 Verify+Test Hooks 目标，正式 Release 在预处理阶段移除。
- 显式失败通道不能检测未报告的外部线程终止或无限阻塞，也不能把损坏 mutex 变成可移植的 robust mutex。worker 启动/返回路径的锁失败会尽力发布失败，但 coordinator 无法重新获得 pool lock 时仍不具备通用恢复证明；watchdog、宿主诊断与相应 fail-stop 协议留待后续。无线程平台继续 coordinator 同步路径。

### 52.3 验证边界与后续

独立 Verify+Test Hooks 回归覆盖 coordinator wait failure、dispatch/close broadcast failure、join failure 后 retry close，以及七个 worker 失败窗口：idle wait、copy/REMAP 的执行前/执行后/完成记账后失败。测试验证对象环、root/strong slot 修正与 payload 保留，检查 `IDLE` phase、active worker 为零、pool job/counter 清空、broken pool 后续同步接管、heap handle 可查询和 `nggc_check` 守恒。非零 worker 退出结果的第一次 close 返回 backend 错误，二次 close 跳过已经 join 的 slot 并完成销毁。专项 CTest 带 30 秒超时；Release 构建不包含这些故障注入分支。

本 ABI 明确不推进 descriptor reservation、slot publication、RemSet 最终表示、TLAB 按线程/span quiescence、完整并发 REMAP、并发 marking、后台 PhantomQueue reaper 或任何移锁任务。下一阶段先处理未报告的 worker 终止/阻塞、scanner 诊断和 root/handle/barrier-buffer/RemSet/forwarding 的并发闭包，再根据实测收益评估更深的并发化。

### 52.4 Verify-only scanner 同线程 API 重入诊断

scanner 既可能在 coordinator 持有 heap lock 时执行，也可能在 coordinator 持锁期间由 REMAP worker 执行。因此重入检查必须位于 API 入口，不能先等待 heap lock 再判定 callback 状态。

- `NGGC_ENABLE_VERIFY` 下使用 C11 `_Thread_local` 指针，指向当前 `nggc_invoke_scan` 栈上的 `(heap, result)` 上下文。不同 native thread 和不同 heap 的正常扫描保持独立；legacy/ex scanner 共用该包装层。嵌套 scanner 调用也锁存协议错误。
- 全部 54 个公共 API 在任何参数解引用、输出初始化、分配快路径和取锁之前检查。当前线程正在扫描时，result API 返回 `NGGC_ERR_BUSY`，pointer API 返回空指针，对象元数据查询返回零或 `NGGC_TYPE_NONE`；输出参数保持原值。对其他 heap 的调用同样拒绝，防止跨 heap 锁顺序死锁。
- 被拒绝的 API 只在当前线程的 guard 中锁存 `NGGC_ERR_CORRUPTION`，不从 worker 写共享 `heap->last_error`。即使 legacy callback 忽略失败，或 ex callback 返回另一个错误，包装层仍向外传播 corruption。callback 正常返回后先清理 TLS，再由既有 collector 错误路径处理。
- visitor 和 `control->poll(control->ctx)` 仍是允许的同步操作。Verify poll 先比较 context 是否为当前线程的 active guard，再解引用；错误/null/其他 context 返回 corruption，有 active guard 时同步锁存该错误。正常 poll 返回已锁存错误或既有 heap 状态结果。
- forwarding 前的错误走正常 abort/release；后台 REMAP 中的错误使用既有 post-forwarding emergency completion。所有实际 worker 归静后完成 canonical graph，再返回 corruption，不因重入而提前释放 parked mutator。
- 正式库在 `NGGC_ENABLE_VERIFY=0` 时预处理移除 TLS、guard、检查分支和 scanner phase 测试辅助函数；没有新增公共字段或函数，ABI minor 仍为 88。专项目标单独强制 Verify+Test Hooks，Release 配置中的专项不代表正式库启用诊断。

独立专项覆盖 legacy/ex scanner 的全部公共 API、跨 heap 调用、错误 poll context、错误优先级、MARK 失败后的 safepoint release、真正后台 REMAP 失败后的对象图/worker quiescence/应急收尾和下一轮 worker 复用，以及两个 heap 在不同 native owner 上同步重叠 MARK 扫描。

该 guard 只诊断当前 native thread 的同步协议违例，不能检测 callback 派生线程中的 NGGC 调用、任意缓存 visitor/context、无限阻塞或 `longjmp` 等非局部退出；stack 地址复用也使它不能成为通用 stale-context token。以上行为仍违反扫描契约，其完整诊断、执行预算和 watchdog/fail-stop 设计留待后续。descriptor reservation、slot publication 和 barrier 移锁继续暂缓。

### 52.5 canonical-owner 读屏障闭包

source shadow 不是 forwarding 发布后的权威对象体。通过 destination owner 写入字段时，只有目标 payload 更新；若读屏障只解析字段中的 referent，却仍从旧 source 字段读取，就会漏掉新值，尤其是新字段变为 null 时无法通过 referent forwarding 补救。

`nggc_load` 在 read barrier active 时先判定 slot 是外部 root/local，还是本 heap 的已发布 payload。对 LIVE/FORWARDED owner 按 source 内字段 offset 查找 canonical owner 的同一 slot，然后读取该 canonical slot，再执行原有 referent acquire-follow。需要自愈时修正的也是实际读取的 canonical slot；外部 alias 则仍原位自愈。source shadow 可以继续保持历史值，调用者不能通过裸字段读取绕过协议。

并发 copy 时的锁序为 heap lock、source payload lock、必要时 target payload lock，按反序释放。copy worker 只持 source payload lock；mutator 必须在该锁取得后重新解析 forwarding，因为 worker 可以在 mutator 等待期间完成复制并发布目标。source/target 锁覆盖实际字段读取，referent 本身仍按既有 forwarding/epoch 规则解释。隔离存在而没有 copy worker 时，heap lock 串行化字段读写，不额外取得 payload lock。读屏障关闭的路径继续直接访问 caller slot。

slot 必须按指针对齐。active barrier 对 arena 内的 header、padding、FREE/ALLOCATING/TLAB_RESERVED/decommitted 空间、未发布尾部和跨 payload 边界地址，在读取前返回错误；arena 外的有效 C root/local slot 仍由调用者负责其可访问性和生命周期。owner 查询复用统一 block parser，因此支持 humongous continuation slot 和跨 Region 合并的普通 free block；当前最坏仍为 O(heap block count)，对象起点索引/有界 lookup 留待后续，不借此宣称最终低延迟实现。

旧实现加上 canonical-only null 写入的回归后实际失败；修复后同一回归及替换 referent 通过。并发专项交替让 load/store 到达 copy worker 持有的 source lock，验证等待、forwarding 后重读和 canonical-only 字段读回，再完成既有 repark/parallel REMAP/Pin/年龄晋升守恒。大对象 start/continuation slot 和 header/padding/未对齐地址覆盖 512 B、24 KiB、128 KiB Region。新增 load 观测字段/辅助函数仅存在于 Verify+Test Hooks，正式 Release 预处理移除；公共 ABI 仍为 88，未移动任何既有锁。

### 52.6 原子诊断与逐调用错误回传

heap-wide `last_error` 的读写存在于 heap lock 外，包括参数拒绝路径和诊断查询。普通字段会使不同 native thread 的错误发布/查询形成 C11 数据竞争；即使把字段改为原子，另一个线程仍可在某 API 内部失败后、该 API 返回前替换诊断，所以共享诊断不能代表某一次调用的结果。

- `last_error` 使用 `atomic_int`，在 `nggc_open` 中显式 `atomic_init(..., NGGC_OK)`。统一 setter/getter 使用 `memory_order_relaxed` store/load；错误码是独立诊断值，不承载对象、phase 或线程状态的 publication，后者继续使用各自既有锁与原子协议。此改动不要求共享 64-bit lock-free atomic。
- `nggc_get_last_error` 只返回一次 heap-wide 原子诊断快照。并发操作可替换它，成功 API 也可能保留之前的错误；没有 per-thread error promise。需要判断具体调用时，使用该 API 返回的 `nggc_result`。
- WeakReference、SoftReference、PhantomQueue 和 PhantomReference 的内部 allocator 拆为返回 `nggc_result` 的四个静态 helper。helper 先清空自己的输出，失败返回本次局部错误，成功才填对象；公开 create API 直接返回 helper result。既有私有 pointer allocator 继续包装 helper，保持现有调用方式。公开参数拒绝和 Verify scanner 重入拒绝的合同保持原样。
- 没有增加公共 API 或字段，ABI minor 仍为 88。对象分配/初始化、heap registry 和屏障锁都沿用现有同步协议；descriptor reservation、slot publication 与 barrier 移锁继续暂缓。

独立 Verify+Test Hooks 目标为四种创建 API 制造同一个确定性交错：活动 safepoint 使 allocator 返回 `BUSY`，creator 在 helper 返回并释放 heap lock 后暂停，另一 native thread 通过无效 Pin 发布 `INVALID_ARGUMENT`，随后恢复 creator。每个 creator 必须返回自己的 `BUSY`，失败输出为空。暂停字段、函数和分支在正式 Release 预处理移除。另有三 native thread 每人 4000 次操作，重叠 `INVALID_ARGUMENT`/`CORRUPTION` 发布与诊断读取，并验证逐调用结果、allocation/live/root 计数及收尾检查；无 `<threads.h>` 目标覆盖同步拒绝矩阵。

构建目录中的 mutation 副本保留原子诊断，仅恢复四个创建 API 从 `nggc_get_last_error` 取失败返回值，实际在首个确定性交错断言失败；原源码未作 mutation。该验证证明逐调用结果不能依赖共享诊断。C11 原子访问审计和回归压力只覆盖这里的字段与交错，不构成整个 collector 无数据竞争的证明；TSAN 实测及完整并发 REMAP/root/handle 闭包仍待后续。
