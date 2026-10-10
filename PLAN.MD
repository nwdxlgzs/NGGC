# NextGenerationGarbageCollector Plan

本文档记录当前工程计划。更高层的长期架构见 `GC-ARCH-PLAN.MD`；本文聚焦当前代码如何先对着 `mock-uhmas/UHMAS-HL` 开发。

## 1. 当前路线

第一阶段把 UHMAS-HL 当作内存操作底座，而不是直接写平台 VM 代码。

```text
NGGC heap
  -> opens a UHMAS-HL platform heap
  -> maps one GC arena through UHMAS mmap
  -> performs GC object allocation inside that arena
  -> later migrates the same backend contract to real UHMAS
```

这样做的目的：

- 先固定 GC 自己的对象头、类型注册、扫描和验证路径。
- 复用 UHMAS-HL 的 arena、mmap、munmap、mprotect、统计和平台 VM 包装。
- 避免第一版就依赖真实 UHMAS 的复杂并发模型。
- 后期 UHMAS 开发差不多后，把 backend 从 UHMAS-HL 移植到 UHMAS。

## 2. 已落地的 Phase 0/1 骨架

- 根级 CMake 项目。
- `nggc` 静态库目标。
- `mock-uhmas/UHMAS-HL` 作为子目录依赖。
- Debug/Release/Profile 构建配置入口。
- `NGGC_ENABLE_VERIFY`、`NGGC_ENABLE_ASAN_HOOKS`、`NGGC_ENABLE_TSAN_HOOKS`、`NGGC_ENABLE_FAULT_INJECTION`、`NGGC_ENABLE_PROFILING`、`NGGC_ENABLE_TRACE_LOG` 宏开关。
- `include/nggc/nggc.h` 公共头。
- 当前 ABI minor 为 `88`。
- `src/nggc.c` 初始实现。
- `tests/test_nggc_basic.c` 基础测试。

当前实现能力：

- 打开/关闭 GC heap。
- 通过内部 `nggc_backend_*` 包装打开 UHMAS-HL platform heap。
- 通过 backend map/unmap 建立和释放 GC arena。
- backend 已拆到 `src/nggc_backend.c`，当前合同记录在 `BACKEND-CONTRACT.MD`。
- 支持 `uint16_t` 类型 ID，`0` 保留为 `NGGC_TYPE_NONE`。
- 支持私有 `GC_TYPE_NONE` 内部对象策略；公开分配仍拒绝 type 0，内部 type 0 对象必须带 `INTERNAL` 标志并作为叶子对象验证。
- 支持用户注册扫描函数。
- ABI minor 88 增加可选 `nggc_scan_fn_ex` 和 `nggc_scan_control`：scanner 可返回显式错误，并通过协作式 `poll` 传播取消；旧 `nggc_scan_fn` 保持兼容，描述符尾部扩展按 `size` 复制。Release/Verify 的 scanner、RemSet、mark、REMAP 和 `nggc_check` 路径均不覆盖 visitor/callback 错误。
- ABI minor 88 补齐 Verify-only scanner 同线程 API 重入诊断：C11 `_Thread_local` 记录当前同步扫描上下文，全部 54 个公共 API 在参数解引用、分配快路径和取锁之前拒绝重入，跨 heap 调用也适用。嵌套 API 返回 `BUSY` 或空值，外层扫描锁存并返回 `CORRUPTION`；worker 不写共享 `last_error`。正常 visitor/poll 继续有效，回调返回后清理上下文，不增加公共 ABI。
- ABI minor 88 将 heap-wide `last_error` 改为显式初始化的 C11 `atomic_int`，使用 relaxed store/load 消除并发诊断读写的数据竞争；诊断不发布 heap 状态，也不承诺每线程独立保存错误。Weak/Soft/PhantomQueue/PhantomReference 四个创建 API 直接返回本次内部 helper 的局部 `nggc_result`，失败输出为空，不再从可被其他线程覆盖的共享诊断重建结果；保留私有 pointer allocator 包装，不改变公共 ABI 或既有锁。
- `nggc_check` 会验证 scanner 上报的 slot 位于当前 owner payload 内、满足指针对齐，且 slot 值为 null 或合法 live object；因此普通 scanner 不能越界枚举 root、其他对象字段或 weak/soft/phantom special object 的内部 referent。
- Debug/Verify 构建支持 scanner replay：保存 payload 快照并执行两次 scanner，按 slot 集合比较而不强制枚举顺序，拒绝重复 slot、不稳定集合、引用值漂移和 scanner 对 payload 的修改；该高成本路径在默认 Release 构建中由 `NGGC_ENABLE_VERIFY` 代码级移除。
- 支持冻结类型表；冻结后拒绝新注册和注销，但已注册类型仍可分配。
- 支持类型注销：未冻结且没有 live object 使用该 type id 时可注销，注销后自动 type id 分配可复用该 id。
- 支持定长对象分配和显式大小对象分配。
- 对象头含 state、type_id、flags、forwarding、payload size、alloc size、object id。
- 对象 flags 已覆盖 internal、pinnable、non-movable 和 large object 原型标记。
- 分配协议已有 `ALLOCATING -> LIVE` 发布状态。
- 支持显式 root stack，root 以 slot 形式登记；slot 必须指针对齐，同一 slot 不能重复注册，pop 后会清理 registry 尾项。
- 支持显式线程注册 `nggc_thread_register` / `nggc_thread_unregister`。
- 支持持有式 safepoint epoch + pending/parked/release：`nggc_request_safepoint` 请求停车，`nggc_safepoint` 在 flush 当前线程 RemSet buffer、退休 TLAB 后让线程保持 `AT_SAFEPOINT`，`nggc_safepoint_wait` 等待全部 ack，`nggc_safepoint_release` 统一恢复线程。
- safepoint 活动期间，公开分配、global/thread root 修改、读写屏障、线程注册/注销均被低成本门禁；auto-collect 在成功和 abort 路径负责 release，手动请求由调用者显式 release。stats ABI minor 36 暴露 active、parked 和 release count。
- ABI minor 37 新增 `nggc_thread_collect` collector-owned mutator 模式：发起线程退休自己的 TLAB 并进入 `COLLECTOR_OWNED`，自身计入 parked 但不计入 pending；其余线程停车后只能由同一 owner 继续 collection，普通 collector 和其他线程不能接管。stats 暴露当前 collector-owned thread count。
- ABI minor 38 新增阻塞式 `nggc_safepoint_park` 和原子 `safepoint_release_epoch`：有 C11 `threads.h` 时使用 `cnd_wait/cnd_broadcast`，无该可选标准库时保留原子等待 fallback；release epoch 作为条件变量谓词，避免丢失唤醒。`nggc_close` 现在拒绝仍有注册线程或活动 safepoint。
- Verify+Test Hooks collector fault matrix 已覆盖 cycle begin 后失败、ROOT_SNAPSHOT metadata OOM、REFERENCE_PROCESS 失败和 SWEEP 失败；四条路径都必须 abort 到 `IDLE`、释放 auto-safepoint，并允许下一轮正常 collection。故障字段和分支不进入正式库。
- `nggc_check` 已接入 root/thread registry verifier：基础路径验证 root storage/count/capacity、slot 对齐和值；Debug/Verify 额外拒绝重复 root，检查 thread 链表无环、节点 magic/heap/state、registry 计数，并从逐线程 epoch/state 重建 pending/parked safepoint 数。专用破坏测试 hook 只编入独立测试目标，不进入正式库。
- 支持可选非阻塞 collect auto-safepoint：`NGGC_CONFIG_COLLECT_AUTO_SAFEPOINT`。
- 支持 GC phase skeleton：`nggc_phase_begin` / `nggc_phase_end` 可做 standalone 单阶段诊断；collector/assist 另有内部 cycle epoch 与 expected phase，严格执行 `ROOT_SNAPSHOT -> MARK -> REFERENCE_PROCESS -> SWEEP`，失败清理 active phase 后显式 abort cycle。
- 常驻低成本 phase verifier 会检查 active/idle 与 transition-count 奇偶、phase/read-barrier 矩阵、active phase 无 pending safepoint、auto-collect safepoint epoch 归属，以及 collector expected phase/epoch；active phase 或 collector cycle 中会拒绝新的 safepoint 请求。
- `nggc_collect` 已接入 reference-processing phase，为弱/软/虚引用处理预留明确阶段。
- 支持 WeakReference v0：内部 special object 使用 `NGGC_TYPE_NONE + INTERNAL`，公开 `nggc_weak_ref_create` / `nggc_weak_ref_get`；弱引用对象本身可被 root 保活，referent 不走普通 mark visitor，`REFERENCE_PROCESS` 中按本轮 mark epoch 清理不可达 referent。
- 支持 SoftReference v0：公开 `nggc_soft_ref_create` / `nggc_soft_ref_get`；软引用对象本身可被 root 保活，referent 由 soft policy 决定保留或清理；保留时会重新进入标记闭包。
- 支持 PhantomReference/PhantomQueue v0：公开 `nggc_phantom_queue_create` / `nggc_phantom_ref_create` / `nggc_phantom_queue_count` / `nggc_phantom_queue_pop`；marked phantom 会在 reference-processing 中保活其关联 queue，referent 死亡后 phantom 入队，非空 PhantomQueue 会作为内部 root 保活自己和队列内 phantom reference，用户可手动 drain 队列。
- Debug/Verify reference-processing verifier 会复查 soft/weak 非空 referent 已进入当前 mark epoch、marked phantom 的 queue/referent 已标记，以及 enqueued phantom 的 referent 已清空、queue 归属唯一、链表计数一致、queue/ref 均被本轮标记；处理顺序测试覆盖 soft-retained referent 不会被后续 weak 清理或 phantom 入队。
- 支持 PhantomQueue reaper 弱观察/轮询/有界等待 v0：`nggc_phantom_reaper_watch` / `unwatch` / `poll` / `wait` / `pop` 不保活空队列，通过 queue object id 清理失效观察项；`wait` 最多触发调用者指定次数的 collection，仍无 pending queue 时返回 `NGGC_ERR_BUSY`。ABI minor 87 增加 `nggc_phantom_reaper_wait_blocking`，由事件 epoch + C11 condition variable 等待 enqueue、pop、watch/unwatch 和 collection 状态变化，不自动 collection 或 pop；无 `<threads.h>` 时退回原子等待。ABI minor 88 增加 worker pool 故障注入与 close 重试闭环。
- stats 已暴露 weak reference discovered/cleared、soft reference discovered/retained/cleared、phantom discovered/enqueued、phantom queue promoted、phantom reaper watched/dropped 计数，以及当前 soft reference policy。
- ABI minor 84 增加对象级 Pin lease 归属账本：嵌套 lease 按对象、object ID 和 owner 聚合；当前注册线程拥有的 lease 必须由同一 owner 释放，跨 owner unpin 被拒绝，持 lease 的线程不能注销。常驻 `nggc_check` 验证每个对象的 lease 和 `pin_count` 守恒，stats 暴露 record、owner conflict 和 thread-exit busy 计数。
- 支持 phase-gated read barrier：`EVACUATE` / `REMAP` phase 会打开读屏障入口；active 阶段会验证 loaded object 并跟随 live object header 中的 forwarding 指针。
- 支持最小 REMAP strong slot fixup：结束 `REMAP` phase 时会扫描 root slot 和用户扫描函数上报的 strong slot，将指向 forwarded object 的 slot 修正到 forwarding 目标；`nggc_check` 会验证 live object forwarding 目标合法，并拒绝残留指向 forwarded object 的 root/strong slot。
- 支持 Mutator Assist 计债统计骨架；分配按字节增加 heap debt，并在有注册 owner 时同步增加 per-thread debt。回收工作先偿还当前 assist actor 的债务，再偿还其他已注册线程债务，剩余部分归入无 owner 的全局债务。
- ABI minor 81 将 opt-in `NGGC_CONFIG_MUTATOR_ASSIST_RECLAIM` 扩展为持久化、bounded、可公平调度的 work packet：根据最大 heap/actor debt 与本次分配请求，在 MARK 使用 8/16/24/32 items、SWEEP 使用 1024/2048/4096/8192 bytes 四档预算；同一线程连续最多 4 个 packet，存在其他线程债务时触发公平性上限。
- 每个注册线程维护 no-progress streak 和短退避窗口；heap 与线程 stats 暴露 packet/progress/no-progress、动态预算、公平性 cap、退避和 pending/max debt。所有 Assist 状态、budget、债务总和和线程生命周期都由常驻 verifier 校验，并继续兼容 32-bit，不要求 64-bit lock-free atomic。
- Assist 的 root snapshot 当前仍为原子步骤；reference processing 已由 ABI minor 85 切成可恢复 bounded packet，但完整 `nggc_collect` 仍会取消并接管未完成 Assist cycle。MARK 使用 incremental-update 写屏障，新分配对象按当前 mark epoch 发布；上述 per-thread/fairness/budget 逻辑当前仍在 heap lock 内，未宣称无锁 Assist。
- ABI minor 83 的 discovered-list 由 collector 在现有 heap lock 内独占，使用本轮 mark epoch 的 intrusive Soft/Weak/Phantom 三类列表；处理顺序为 Soft 闭包、Weak、PhantomReference、PhantomQueue。列表在成功 drain、collection abort、Assist reset 和下一轮 mark epoch 前清理；已入队 PhantomReference 不重复进入 discovered-list。ABI minor 85 在此基础上把 Assist reference processing 切成可恢复 bounded packet：持久化 mark stack、Soft/Weak/Phantom 阶段状态、PhantomQueue 成员游标和 arena queue-scan 游标跨 packet 保存；collector takeover 或 packet 错误统一清空 discovered-list 后重新开始。完整 collector 仍保留原子 reference-processing。
- ABI minor 85 的专用回归在 Assist 停留于 `REFERENCE_PROCESS` 的中间态调用完整 `nggc_collect`，验证旧 cursor/discovered-list 被清理、完整 collector 以新 mark epoch 重启、`nggc_check` 接受合法中间态且最终回到 `IDLE`；该测试仍完全依赖现有 heap lock。
- ABI minor 82 增加可解释回收触发器：`SMART`、heap-limit、allocation-rate、latency-target、throughput、memory-saving、manual 七种 profile；默认策略为 SMART，但自动入口必须显式设置 `NGGC_CONFIG_COLLECT_AUTO_TRIGGER`。策略只在分配慢路径/失败边界或显式 `nggc_collect_if_needed` 评估，TLAB fast path 不进入策略层；注册线程存在时自动路径保持 advisory no-op，不绕过 safepoint。
- 触发器使用 arena soft limit、Region-sized quantum、累计 allocation bytes、Assist debt、Young 可用性和 pause EWMA 做阈值/滞回决策，向既有 collector 请求 YOUNG/FULL action；stats 暴露 evaluation/request/collection/busy/failure、action/reason/pending 和 projected usage。`MANUAL` 永不自动收集，未到条件的显式评估成功返回 no-op。
- latency target 非零且提供单调 `clock_ns` 时，统一 collection cycle 在 owner 确认后至成功/abort/fail-stop 出口采样 pause，更新 last/sample/EWMA；callback 失败、回拨或溢出只累计 clock-failure，不改变 GC 结果，也无 wall-clock fallback。
- 支持 `nggc_load` / `nggc_store` 作为第一版读/写屏障入口。
- ABI minor 88 修正读屏障的 owner 路径：读屏障开启时，payload slot 先按 owner forwarding 和字段 offset 映射到 canonical slot，再读取/解析 referent；不能把旧 source shadow 作为当前值。并发 copy 中先取得 source payload lock，再重新解析 forwarding，必要时继续持 target payload lock；隔离窗口沿用 heap lock。外部 root/local slot 不做 owner 映射，未对齐地址以及 active barrier 中的 header、padding、非 LIVE/FORWARDED payload 在解引用前拒绝。
- `nggc_store` 已接入最小 card table / remembered-set 记录。
- RemSet 已支持 `PRECISE_CARD -> COARSE_HEAP` metadata overflow 降级；coarse 模式整堆 card 置脏并继续接受写屏障记录，不会因容量不足漏边。
- ABI minor 40 的写屏障会累计 cross-region 与 old-to-young 写入；Debug/Verify RemSet rebuild 通过完整 strong-slot heap walk 只要求跨 region 边的 owner card 被覆盖，并单独统计 old-to-young 子集。故障测试会清除真实 `OLD -> YOUNG` owner card 并要求 verifier 报错；precise-card overflow 后仍可降级 coarse-heap。
- ABI minor 73 增加 per-thread RemSet/card publication buffer。`nggc_config.remset_buffer_capacity == 0` 使用 `min(64, card_count)`，显式容量必须位于 `1..card_count`；同一线程内按 card index 去重，容量满时只 flush 当前线程，刷入全局表时继续使用既有 `PRECISE_CARD -> COARSE_HEAP` 确定降级。safepoint ack、并发 EVACUATE repark、collector-owned 转换和 thread unregister 都在发布 parked/摘链状态前 flush 当前线程；依赖 remembered closure 的 young collector 在 Release 路径显式要求全部注册线程 parked 且 pending card 为零；`nggc_get_stats` 只观察 pending buffer，不为统计主动刷卡。
- ABI minor 73 的 RemSet rebuild coverage 是“全局 dirty card 与所有合法 pending thread buffer 的并集”。常驻 `nggc_check` 验证每线程 buffer pointer/capacity/count、card index、线程内无重复，以及 parked/collector-owned/reparking 线程必须为空；Verify fault 304 会注入越界 buffered card。当前 `nggc_store` 的对象/forwarding/Assist 校验和 slot publication 仍持 heap lock，因此该增量只是 card publication buffering，不宣称写屏障已经 lock-free，也没有冻结最终 Region RemSet 表示。
- ABI minor 74 增加进程内单一 C11 TSS key 和每原生线程的 per-heap barrier-context 链。当前线程在某 heap 只有一个注册句柄时，`nggc_store` 可 O(1) 找到其 RemSet buffer；同一线程同一 heap 多句柄保持显式歧义并走 direct global path；同一线程可同时持有多个 heap 的独立 context。TSS 创建、分配或设置失败不会使线程注册失败，barrier 会退回权威 thread registry 扫描；无 C11 `<threads.h>` 的平台保持扫描 fallback。
- ABI minor 74 的 context 只缓存注册归属，不成为正确性来源。register/unregister 在持 heap lock 修改 registry 后同步重建调用线程的 `handle_count/unique_thread`；注销最后句柄时留下可复用 tombstone，避免 heap close 后悬挂解引用。线程退出时 TSS destructor 只释放私有 context，不擅自注销 heap thread handle，未显式 unregister 的 heap 仍按既有合同返回 busy。Debug/Verify 可核对当前调用线程的 context 链、唯一性与 registry 摘要；测试覆盖唯一命中、同 heap 歧义、注销恢复唯一、跨 heap 区分、无 owner direct 和强制更新失败 fallback。
- ABI minor 75 在 C11 owner 可证明的路径上，把 per-thread card array 写入和 published-count publication 移到 heap unlock 之后。heap lock 内仍完成 owner/value/slot/forwarding/Assist 校验、RemSet 分类、线程内去重、overflow flush、一个 card descriptor 预留和 slot publication；因此只是缩短临界区尾部，不是完整无锁写屏障。无 `<threads.h>` 平台无法证明 unlock 后唯一句柄不会被其他宿主线程注销，继续在锁内完成 append。
- ABI minor 75 的 remembered coverage 扩展为“global dirty card + 已发布 buffer prefix + 合法 in-flight descriptor”。descriptor 在 slot publication 前以 release 发布；mutator 解锁后依次写数组、release-store published count、release-store 清零 inflight。stats/check/rebuild 可在合法窗口中观察 descriptor，safepoint、repark、collector-owned 和 unregister 只有在 inflight 为零后才能 flush/发布 parked/摘链。Verify 确定性暂停和 fault 305 覆盖 unlock 后、数组发布前的 safepoint closure 与越界 descriptor 拒绝。
- ABI minor 76 将 C11 未满 per-thread buffer 的线性去重扫描也移到 heap unlock 后：descriptor 仍在 slot publication 前覆盖新边，owner 解锁后扫描稳定 published prefix，重复时不写数组/不增加 count，不重复时才 append。满 buffer 仍在锁内先扫描，避免重复写无谓触发 overflow flush；无 `<threads.h>` 平台继续锁内去重和 append。
- 锁外 append/dedup 的 0/1/2 outcome 使用 32-bit `atomic_uint` 回传，下一次 store、flush 或 stats 在 heap lock 下折叠到 64-bit 饱和 telemetry；snapshot 同时验证 outcome 与 descriptor/count/prefix 的语义对应。新增 deferred-dedup 统计和 fault 306，24 KiB 非 2 次幂 Region 的确定性窗口同时覆盖新 card 与已存在 card 两种 closure。
- ABI minor 77 将 global card table 元素升级为逐项 C11 `atomic_uchar`。C11 owner 的未满-buffer路径在锁内不再读取global card：descriptor先覆盖slot publication，解锁后先acquire读取card；已全局覆盖则回传`COVERED`且不扫描prefix、不append，未覆盖才继续ABI 76的dedup/append。coarse模式、满buffer overflow和无`<threads.h>` fallback仍是明确锁内慢路径。
- deferred outcome扩展为`NONE/APPENDED/DEDUPLICATED/COVERED`，仍只用32-bit `atomic_uint`并在heap lock下折叠64-bit telemetry。snapshot把global card、prefix和descriptor作为一个closure计算effective count，常驻verifier要求每个atomic card严格为0或1；fault 307与24 KiB非2次幂Region暂停窗口覆盖非法card值和global-covered descriptor生命周期。
- stats新增`remset_card_atomic_lock_free`和`remset_buffer_deferred_covered_count`：前者只报告当前heap byte atomic的性能能力（0/1），不成为正确性前提；后者只统计C11锁外global coverage命中，不与append/dedup计数混用。
- ABI minor 78 把 C11 unique-owner 写屏障的 `record/cross-region/OLD-to-YOUNG/buffer-record` 四类共享 64-bit telemetry 更新移出 heap 临界区。锁内冻结 owner/value 的 Region index 与逻辑 generation，并预留独立 classification descriptor；slot 成功发布并 unlock 后，owner 只以 32-bit `atomic_uint` 发布分类 outcome，下一次 store、flush、safepoint、unregister 或 stats 再在 heap lock 下验证并折叠。direct/no-owner 与无 `<threads.h>` fallback 继续锁内计数。
- 分类位只允许 `RECORD`、`RECORD|CROSS_REGION`、`RECORD|CROSS_REGION|OLD_TO_YOUNG`，强制 `OLD_TO_YOUNG => CROSS_REGION => RECORD`。null value 冻结为 `value_region=SIZE_MAX, value_generation=NONE`；其他 descriptor 必须能由 frozen Region/generation 唯一重算。独立 classification inflight 覆盖 coarse、global-covered、dedup 和 append 等所有成功写入，不借用只在部分路径存在的 card inflight。
- stats 新增 pending-classification thread 数与 deferred-classification 累计数。对 outcome 未发布或已发布但 inflight 尚未清零的窗口，stats 将 descriptor 作为有效但未折叠事件叠加到返回快照，不修改共享基数；折叠后 pending 归零而累计值保持不变。常驻 verifier 在 exchange 清零前验证 descriptor/outcome，fault 308 拒绝“枚举合法但语义不匹配”的分类位。
- ABI minor 78 的实现细化把 C11 owner 的分类推导放到 unlock 后：descriptor 只冻结可验证的数值字段，锁内 reservation 仍负责句柄、card/slot 和 forwarding/Assist 生命周期；解锁后先由唯一 owner 根据 descriptor 推导最终分类，再发布 32-bit outcome，最后清零 classification inflight。Verify 测试在推导前暂停，另一线程仍能取得 heap lock 完成 stats、RemSet rebuild 和 full check，证明推导不再延长 heap-lock 临界区；这不等价于完整 lock-free store，slot publication 与 descriptor reservation 仍在锁内。
- ABI minor 78 同批压力回归修复了既有 TLAB gate 的 pre-increment late-enrollment 竞态：fast allocator 首次 acquire 读到 `OPEN` 与 `tlab_fast_inflight++` 之间允许 observer 关门；allocator 在登记后必须二次检查 gate，失败即撤销登记且不触碰对象。closer 关闭 gate 后循环建立 quiescence，若 thread flush 在该有限迟到窗口观察到 `BUSY`，必须 yield 后重新扫描全部 inflight 并重做 flush，绝不向 stats、safepoint 或 allocation slow path 泄漏内部瞬态。Verify+Test Hooks 用两阶段 enrollment pause 和 quiescent-to-flush pause 确定性制造“先扫描 0、后迟到登记、flush 观察 BUSY”的窗口，并证明 observer 重试成功、allocation 走 slow path 成功、对象不丢失也不重复入账；正式 Release 只保留重试协议，测试握手由宏完全移除。
- 支持非移动 region table v0：Region 尺寸可配置且默认 64 KiB，提供地址到 region 映射、committed/live/free 字节和起始对象计数；跨 region 大对象按覆盖字节分摊。
- `nggc_check` 会通过完整 heap walk 重建 region 元数据，验证分配、回收、free-block 拆分/合并后的统计一致性。
- Region 状态摘要已覆盖 active/inactive、has-live/free/large/pinned/non-movable。ABI minor 39 建立了最初的 region placement 标签；ABI minor 46 已移除“标准分配直接 OLD”的旧规则：所有新对象在对象头中均以 `YOUNG, age=0` 发布，普通共享分配和 TLAB 使用物理 YOUNG 空间，large/pinned/non-moving 使用物理 SPECIAL 空间，只有达到年龄阈值的对象才能获得逻辑 OLD 代别。
- ABI minor 41 新增 `nggc_collect_young` / `nggc_thread_collect_young` 非移动 young collection v0：cycle 开始后把 committed YOUNG region 加入 collection-set，只 sweep collection-set 内未标记对象；成功或 abort 都清空 collection-set。
- ABI minor 42 将 young marking 收紧为代际输入；ABI minor 46 又把判定从 region 代别改成对象逻辑代别与 collection-set：集合外对象本轮 implicit-live，dirty 集合外 owner card 上的 strong slot 补充到 young closure。`young_mark_old_object_count` 只统计其中逻辑 OLD 对象，SPECIAL owner 指向普通 Young 的边同样会被扫描。
- ABI minor 43 已落地对象级 survivor age 基础：每次成功 young collection 后，collection-set 内仍存活的对象独立增加饱和年龄；年龄阈值是晋升 OLD 的硬性最低条件，未达到阈值绝不允许晋升。ABI minor 45 起，正常 moving 路径在 `next_age >= young_tenuring_threshold` 时复制到 OLD；若容量不足而在 forwarding 前降级为非移动回收，则对象可饱和停留为 YOUNG tenuring candidate，等待后续成功晋升。stats 暴露 survivor object、candidate object 和实际阈值。
- ABI minor 44 增加 young evacuation preflight：在发布任何 forwarding 前，以当前 arena cursor 为起点分别模拟 Survivor(YOUNG) 与 promotion(OLD) 目标布局，跳过 collection-set/from-space 和代别冲突区，并计入 region 封口。容量不足会计入 capacity fallback 后继续非移动 young sweep；stats 暴露 preflight/fallback 次数及最近一次可行计划的两类复制字节。
- ABI minor 45 启用 STW young evacuation v0：preflight 可行时切换 moving cycle，先复制未达阈值对象到 Survivor YOUNG region，再复制达到阈值对象到 OLD region；目标对象完整发布后源 header 保存 forwarding，REMAP 修正 global/thread roots 和用户 scanner 上报的 strong slots，随后 SWEEP 释放 from-space。搬迁源不计入死亡回收，stats 单独累计 evacuated/promoted object。
- moving REMAP 会以复制目标为 owner 重建 card：晋升对象若仍指向更年轻 Survivor，会记录新的 `OLD -> YOUNG` edge。测试用不同年龄的父子对象验证父晋升、子留 Survivor、下一轮仅通过 remembered edge 保活子对象。
- ABI minor 46 在对象头中分离逻辑 generation 与 Region 物理 placement。stats 同时暴露 young/old/special region count、young/old object count 和 SPECIAL 原地晋升计数；verifier 要求所有新生对象逻辑 YOUNG，普通 OLD 只能来自 evacuation，SPECIAL 对象可在达到同一阈值后原地变为逻辑 OLD，而 Region 仍保持 SPECIAL。
- ABI minor 47 冻结 STW evacuation 的故障提交点：第一个 source forwarding 发布前仍可无损 abort；发布后禁止回退，collector 必须在同一 safepoint 内幂等补完剩余 copy、REMAP、from-space SWEEP 和 free-list rebuild。API 保留原始故障返回值，但本轮按完成的 young collection 计数；stats 暴露 emergency completion/failure 次数。若 emergency completion 本身无法建立一致堆，原型进入 fail-stop：保留 safepoint/cycle/collection-set，不释放 mutator。
- ABI minor 48 将对象头 forwarding 改为 `_Atomic(void *)`，free-list 链接走 relaxed 访问，搬迁发布走 release-CAS，读屏障、REMAP 与 verifier 走 acquire load。每轮 moving cycle 分配非零 32 位 evacuation epoch；非空 forwarding 必须携带匹配 epoch，`IDLE` 禁止 live forwarding。CAS 失败即使发现错误 epoch 也会回传已观察 forwarding 并把事务归类为已提交；未成为 canonical target 的串行临时 copy 会立即退回 free-list。stats 暴露当前 epoch 和 CAS success/failure；专用测试覆盖 winner、同 epoch loser 与错误 epoch loser。
- ABI minor 49 将 young evacuation preflight 物化为精确任务表：每个任务冻结 source、destination 和由 `next_age >= young_tenuring_threshold` 唯一决定的目标代别，所有目标空间先在 heap lock 下完成预留与 Region 记账，再由 coordinator 与最多三个 C11 后台 worker 并行复制不相交 payload，并用既有单指针 CAS 竞争 canonical forwarding。不支持 C11 threads 或 worker 创建失败时由 coordinator 同步接管；CAS loser 目标进入回收队列，只有 worker epoch 匹配且 active worker 清零、quiescence 已发布后才能回收 loser 和隔离 from-space。当前仍是 parked safepoint 内、coordinator 持 heap lock 的并行复制，REMAP 和 source lifecycle 尚未与 mutator 并发。
- ABI minor 50 将短生命周期 copy worker 改为每 heap 按需创建的持久池：最多三个后台线程在独立 condition variable 上休眠，dispatch epoch 和唯一 job 指针防止跨轮误领任务，coordinator 仍作为第四个执行者参与复制。每轮只等待被派发 worker 全部完成，不再 `thrd_join`；线程在 `nggc_close` 时统一广播停机并 join。stats 新增 pool size 和累计 dispatch 数，created count 变成 heap 生命周期内实际建池线程数；phase verifier 同时核对 idle/in-flight job、slot owner/index、requested/completed 和 pool shutdown 状态。
- ABI minor 88 补齐 worker pool 故障闭环：coordinator `cnd_wait` 失败后仍等待已派发 worker 归静，dispatch/close 的 `cnd_broadcast` 失败保留 pool 和 heap 供重试，`thrd_join` 失败不销毁同步原语；失败 close 返回原始 `NGGC_ERR_BUSY`/`NGGC_ERR_BACKEND`，二次 close 可继续完成销毁。
- ABI minor 88 的 worker liveness 增量：每个 slot 用 C11 `atomic_uint failed` 发布显式失败退出，pool mutex 内按 `completed_dispatch_epoch` 对每次派发只结束记账一次。ensure/dispatch 拒绝已失败的 pool，coordinator 每次短时等待返回都核对失败 slot；执行前退出允许实际参与者少于派发人数，执行后退出清理 running 计数，完成记账后退出不重复计数。copy/REMAP 都在实际参与者归静后保留 backend 错误并执行现有 emergency completion。该机制只覆盖线程主动报告的失败，未报告的线程终止或卡死仍需后续 watchdog/宿主诊断。
- C11 测试在同一 heap 连续执行两轮 64-task Young evacuation，要求 pool size/created count 不变而 parallel cycle/dispatch count 增长；测试钩子还能让一次 `thrd_create` 调用返回失败，验证本轮 coordinator 无损接管、下一轮重新建池并恢复并行。无 C11 threads 的构建继续保持 pool/dispatch 为零并逐轮同步回退。
- ABI minor 51 将持久 evacuation pool 泛化为每 heap 共享 GC worker pool，并在 parked safepoint 内并行执行 moving REMAP。global/thread roots 由 coordinator 串行修正；heap block 通过原子 cursor CAS 唯一领取，后台 worker 只并行扫描不同 live object 的 strong slot；所有 worker 归静后，coordinator 再串行验证无残留 forwarding slot并重建 RemSet/card coverage。copy 与 REMAP 分别维护 dispatch/parallel 统计，但复用同一组最多三个后台线程。
- ABI minor 52 实现跨 safepoint delayed from-space reclaim。moving SWEEP 不再立即把已 forwarding source 放入 free list，而是转换为内部 `FORWARDED` 隔离壳；Region 独立统计 quarantined bytes/object count，`IDLE` 在隔离存在时继续开启读屏障。`nggc_load` 跟随目标并自愈陈旧 source slot；`nggc_store` 将陈旧 owner 的 payload offset 映射到 canonical target，同时 canonicalize value、记录目标 owner card并双写 source shadow/target。只有不同 safepoint epoch 已完整 parked，或无线程时下一 collection 入口形成空集证明，才把隔离壳物理转为 FREE、重建 free list 并关闭读屏障；任何新 collector/Assist cycle 都必须先完成该回收。
- ABI minor 55 在 ABI 54 copy window 上增加 registered-mutator 状态机骨架：EVACUATE 前将已 ack 的普通线程从 `AT_SAFEPOINT` 转为 `CONCURRENT_RUNNING`，copy 完成后经 `CONCURRENT_REPARKING` 回到 `AT_SAFEPOINT`，并用 parked/running/repark 计数证明 REMAP 前全员重新停车。窗口中仍只允许 `nggc_load/store`；常驻写屏障继续生效，读屏障在 EVACUATE 中跟随 header forwarding。
- ABI minor 56 将上述骨架升级为 poll/park 驱动的协作 repark handshake：resume epoch 会唤醒阻塞在 `nggc_safepoint_park` 的 registered mutator，让它们在 EVACUATE copy window 内执行 `nggc_load/store`；copy worker quiesce 后 collector 请求 repark，mutator 通过 `nggc_safepoint` / `nggc_safepoint_park` 回到 `AT_SAFEPOINT`，然后才能进入 REMAP。root mutation、TLAB/分配、thread-root/global-root 发布仍在窗口内拒绝；REMAP、SWEEP 和完整 root/handle/RemSet 闭包仍未 mutator-concurrent 化。
- ABI minor 88 的 repark 修复让已重新停车的线程只等待 final release，不再重复消费同一 resume epoch；首次 waiter 仍能观察保留的 resume publication。新专项以四个原生 owner、thread roots 和 24 KiB Region 强制制造 copy/store 交错，验证写入不丢、Pin 地址稳定、三轮年龄晋升、RemSet buffer 清空与 parked parallel REMAP。该修复不增加公共 ABI 字段，也不推进移锁。
- ABI minor 57 将 UHMAS-HL backing heap 的 `check` 接入私有 backend boundary：`nggc_check` 现在先完成 GC 自身 verifier，再要求 backend check 成功；失败会返回 `NGGC_ERR_BACKEND` 并记录 last error。stats 仍保持 advisory，不参与 collector 正确性。
- ABI minor 58 在 backend boundary 增加私有 `nggc_backend_decommit/recommit`，为后续 Region reclaim 预留内存归还/重新提交接口。当前 UHMAS-HL 没有 public 原生 decommit/recommit，因此实现用 `munmap` + fixed no-replace `mmap` 模拟；这只是 substrate capability，不表示 GC 已经会主动 decommit 空 Region。
- ABI minor 59 将 backend page size、allocation granularity 和 decommitted bytes 暴露到 `nggc_stats`。后续 Region reclaim 必须只对 backend 页粒度对齐、GC verifier 可证明空闲且不在 free-list/TLAB/quarantine 中的范围调用 decommit。
- ABI minor 60 启用 full collection 后的 tail Region reclaim v0：只回收 arena 高地址尾部连续整 Region，且每个 Region 必须 verifier 可证明为空、无 live/free/reserved/quarantine 对象、无 TLAB、无 collection-set、无 allocator owner，并且 Region 起止/跨度均满足 backend page size 对齐。回收后 Region metadata 进入 decommitted/inactive/NONE 状态，stats 暴露 decommitted Region 数/字节和 decommit/recommit/failure 计数；后续 bump、TLAB reserve 或 young evacuation 目标预留在写入前会按需 recommit。
- ABI minor 61 增加 SPECIAL Region placement 隔离 v0：Region metadata 区分 none/movable-special/pinned-special；普通 large SPECIAL 和 pinned/non-movable SPECIAL 的 free block 不能互相复用，bump 路径会跳过不兼容 sticky Region，verifier 会从 live object flags 重建并拒绝混用。Region 尺寸仍沿用 ABI minor 53 的 `nggc_config.region_size`，`0` 表示默认 64 KiB。
- ABI minor 62 增加 Large Object Space v0：回收后的 large block 保留 `LARGE` pool class 并进入独立 `large_free_list`，small/large allocation 不再交叉搜索；拆分余量继续属于 large pool，full rebuild 分别重建两条链并禁止跨不同 Region generation/placement 边界合并。tail reclaim 能识别由不同 pool class 组成的连续 free run，因此分池不会阻止整段尾部 decommit。
- `nggc_check` 会从 arena walk 分别重建 small/large free block 计数与字节数，验证两条链的节点类型、环上限、总量守恒和 large 子集守恒；stats 暴露 large free object/bytes 以及 large allocation/reuse telemetry。Region 回归测试证明 small allocation 不消耗 large span、large allocation 可原址复用，并继续覆盖 ABI 53 的 8 KiB/128 KiB 自定义 Region。
- ABI minor 63 增加真正的 Region-aligned humongous run：large object 的可见 payload size 不变，内部 `alloc_size` 按 heap 实际 `region_size` 向上取整到完整 run；首区标记 START，后续区标记 CONTINUATION，每区保存 run offset/length。large free block 不再互相 coalesce，split 只在余量至少还能形成 2-Region run 时发生，并重新划分两侧 metadata；否则整 run 复用，避免产生永远不能承载 large object 的单 Region 碎片。
- Region verifier 会从 LIVE/FREE large block 独立重建 humongous metadata，并验证对齐、完整 Region 覆盖、START/CONTINUATION 序列、offset/length、SPECIAL placement 和 decommit reset。stats 暴露总/live/free humongous run 数与覆盖 Region 数；8 KiB/24 KiB（非 2 的幂）/128 KiB 配置测试、4-Region free run 拆成两个 2-Region run、3-Region run 服务 2-Region 请求时吸收单 Region 余量，以及 role/offset 故障注入均已覆盖。
- ABI minor 64 将单一 large free 链替换为固定 32 档 logarithmic segregated bins。分档只依据 `alloc_size / region_size` 的 run Region 数：bin 0 为 2 Regions，bin 1 为 3-4，bin 2 为 5-8，随后逐级翻倍；32-bit nonempty mask 允许 allocator 从请求档向上跳过空档，不依赖 CPU bit-scan、编译器 builtin、Region 为 2 的幂或 64-bit lock-free atomic。拆分余量按新 run 长度重新分档，full rebuild 与 tail reclaim 后重建全部 heads/mask。
- `nggc_check` 会逐档验证 FREE large header 的 humongous geometry、正确档位、链表上限、位图/head 一致性和全局 large-free 守恒。stats 新增 bin capacity、active bin count、largest free run Regions 与 search/probe 计数；专项测试覆盖两个尺寸档同时非空、请求从空档向上搜索、拆分重分档、small/large 隔离，以及 mask/wrong-bin 故障恢复。ABI minor 69 又加入档内严格排序、确定性 best-fit、order 故障验证和 source/size/placement filter telemetry；链表最坏复杂度仍是 O(k)，后续由实测决定是否升级为 tree/二级索引。
- ABI minor 65 在 full collection 的 committed free-list rebuild 和 tail reclaim 之后，主动 decommit 非尾部、完整页对齐且 verifier 可证明为空的 humongous run。ABI 65 初版选择至少 8 Regions，以保留较小 committed hot run 的低延迟复用；ABI 66 将该阈值配置化，ABI 68 的 SMART v0 已接管未显式配置时的默认模式。
- interior decommit 不保留可读“锚点 header”。Region side metadata 在页面不可读期间保存 decommitted 标志、movable/pinned SPECIAL placement、START/CONTINUATION、run offset/length 和 START Region 的 side-list next；另一组 32 档 size-segregated side bins 用 `region_index + 1` 编码节点。统一 block parser 必须先查 Region side metadata，命中后返回 `header == NULL` 的整 run 伪块并跳过页面，任何 collector walker 都不得为伪块解引用 header。
- large allocation 的顺序固定为 committed large-free bins、decommitted side bins、最后 bump。side run 先按 placement 过滤，再通过 backend fixed-address recommit 获取请求前缀；若余量至少 2 Regions，则重写 remainder 的 START/CONTINUATION 并按新长度重新入 side bin，若只剩 1 Region则吸收整个 run。backend recommit 失败返回 `NGGC_ERR_BACKEND`，side bins、side metadata 和计数保持不变，可在原地址重试。
- backend decommit 失败仍是 advisory：本轮 collection 成功，原 committed free header 和统计不变，后续 collection 可重试。成功 decommit 前先完成全部 side metadata、计数、页几何和链表事务预检，成功后不再访问被归还页面。该协议不依赖 page fault、`mprotect`、signal 或 SEH；对象指针反查和屏障入口也先拒绝 decommitted Region。
- ABI minor 65 stats 区分 committed large-free active bins 与 decommitted side-bin/run/Region/bytes，并累计 interior decommit/recommit 次数；`largest_large_free_run_regions` 同时考虑两类可复用 run。`nggc_check` 从 arena 伪块 walk、side-bin walk 和 heap counters 三路核对，故障注入覆盖 mask 与 continuation offset 损坏，backend fault tests 覆盖 decommit/recommit 回滚。
- ABI minor 66 将 ABI 65 的 8-Region 策略常量升级为 `nggc_config.interior_decommit_min_regions`。ABI 68 后，配置为 `0` 选择初值为 8 的 SMART 模式，`NGGC_INTERIOR_DECOMMIT_DISABLED` 禁用 interior decommit，其他值必须至少为 2 且可由 32 位 Region 计数表示并冻结为 MANUAL；阈值大于当前 heap Region 数时自然无候选。`nggc_stats.interior_decommit_min_regions` 始终返回当前 effective threshold，禁用时为 0。
- ABI minor 67 让 tail reclaim 与 interior side-run reclaim 收敛到固定点。每次普通尾部回收后，如果 `arena_cursor` 恰好位于一个或多个 decommitted humongous run 的末端，collector 会逐个验证并从对应 side bin 摘除 START，清除整 run 的 placement/START/CONTINUATION metadata，把它们转换为普通 tail-decommitted Region，再把 high-water 收缩到 run 起点；若这又暴露前方 committed free tail，则继续 tail reclaim。
- tail collapse 不调用 backend recommit/decommit，也不改变 `decommitted_region_count`、`region_decommitted_bytes` 或 Region-oriented decommit 次数；它只把 active interior-run 统计转出并累计 collapse run/Region/bytes telemetry。之后 bump/TLAB/evacuation 可以通过既有 tail recommit 合同使用这些 Region，不再需要 side-bin allocator。
- ABI minor 68 增加 explainable SMART interior-decommit controller。首个 full collection 只建立稳定快照；后续以 committed Region 占比作为跨平台 RSS proxy、以 committed large-free/committed bytes 比例作为可归还压力，并以最近的 interior recommit、large allocation 和 large reuse 增量识别热 run。高回收压力使阈值按 `8 -> 4 -> 2` 逐级降低，recommit 或显著 large-path 热度使阈值倍增、最多到 64 Regions；每次真实升降后至少保持两个 full-collection evaluation，防止反复 decommit/recommit。
- SMART 只调整 coordinator 随后读取的 effective threshold，不改 free/side bins、Region metadata 或 phase；MANUAL/DISABLED 完全不运行控制器。stats 暴露 policy、last action、cooldown、两个 basis-point 压力输入、最近 counter delta 和 evaluation/raise/lower/cooldown 累计值。
- ABI minor 69 把 committed large-free bins 与 decommitted side bins 的档内链统一为 `(run bytes, start address)` 严格递增顺序。allocator 仍从最小可用 logarithmic bin 向上搜索，但每档首个同时满足 requested size 和 movable/pinned placement 的节点现在是确定性 best-fit；相同尺寸以地址稳定排序。split remainder、sweep link、free-list rebuild、interior decommit、partial recommit 和 tail-collapse 摘链都保持该顺序。
- ordered link 在提交指针修改前验证已有整链的范围、状态/side metadata、实际 bin、严格顺序和有界终止；`nggc_check` 独立验证 committed/decommitted 两类链的单调性。Verify 故障钩子交换同档前两个节点，必须报 corruption 并可恢复。stats 将总 allocation probe 拆成 committed/decommitted source，并累计 size reject 与 placement reject；5/8-Region 同档测试证明两类空间都选择 5-Region run，混合 pinned/movable 同尺寸测试证明跳过不兼容低地址节点。
- ABI minor 70 增加可选 `memory_pressure` callback：宿主可在 full-GC 安全点提供进程 RSS/limit 或已经归一化的 0..10000 平台压力。有效外部样本替代 committed-footprint proxy 作为主内存压力，large-free 压力仍独立生效；未配置、返回失败、flag/范围非法或样本不足时只累计 failure，并在同一轮退回 ABI 68 的 deterministic portable proxy，不使 collection 失败。callback 在 heap lock 内同步运行，禁止阻塞、保存指针或重入 NGGC。
- ABI minor 70 还允许宿主提供单调纳秒时钟，对成功的 Region decommit/recommit 按每 Region 归一化并维护 1/8 EWMA。默认不使用不适合测延迟的 wall clock；未提供时钟时完全跳过采时。EWMA 默认只做遥测，只有同时显式配置 `interior_decommit_slow_ns_per_region`，且本轮出现新的慢样本时，SMART 才把阈值提高一级以减少 VM churn；时钟失败不阻止原 VM 操作，只累计失败。verifier 覆盖 pressure source/validity 与 slow-sample 状态，确定性测试覆盖低外部压力、callback 失败/非法样本回退、慢 VM 提阈值和时钟失败不阻断 recommit。
- ABI minor 71 启用 frozen-type per-thread TLAB fast allocation：类型表冻结后，注册 owner 线程可在 heap-level gate 开放时只用 thread-local object-ID lease、C11 `atomic_uint` inflight 和 `_Atomic(uintptr_t)` fast top 发布普通小对象，每对象不取得 heap lock；无 C11 threads、类型表未冻结、foreign owner、无 active TLAB、gate 关闭、large/pinnable/non-movable 或 span 耗尽时进入受控拒绝或 locked refill/slow path。
- fast path 保留 `TLAB_RESERVED` 线性遍历合同：先建立新尾部 sentinel，再把旧 sentinel 初始化为 `ALLOCATING`，完成 header/body 后 release 发布 `LIVE`，最后 release 推进 `tlab_fast_top`。Region/heap/assist 统计延迟到 gate 关闭后，由持 heap lock 的 observer acquire 读取 fast top、遍历 `accounted_top..fast_top` 并一次性入账；`tlab_published_top` 现在明确表示 accounted top。
- safepoint、collection、phase、verifier/stats、标准分配 slow path、refill/retire 和 thread unregister 会先关闭 gate，等待所有 thread inflight 清零，再 flush/扫描/修改 TLAB。普通 `nggc_load/store` 不为每次屏障关闭全局 gate。类型描述符由 freeze 的 release/acquire publication 固定；`nggc_thread` 的分配、collector-owned collection 与注销都绑定注册它的 C11 owner thread，避免 handle 生命周期或角色转换与分配并发。
- 对象 ID 与成功分配计数已经分离：heap-lock slow path 维护 `next_object_id` 高水位，TLAB refill 为线程租赁按 span 上界计算的 ID 段；fast path 不执行共享 64 位原子。未用租赁 ID 形成的空洞合法且永不复用，`allocation_count` 只统计已发布并 flush 入账的真实对象。
- ABI minor 71 stats 增加 fast allocation/slow-path/flush/flushed-object/gate-close/wrong-owner 计数；Debug/Verify fault matrix 覆盖 fast top 越界、非零 inflight、ID lease 非法和 gate 非法状态。确定性 pause hook 证明 safepoint 在 gate 关闭后等待已经进入的 fast allocation 完成并只 flush 一次；正式 Release 库通过宏移除这些 hook。
- Region 尺寸继续由每个 heap 的 `nggc_config.region_size` 冻结配置：`0` 为默认 64 KiB，非零值不要求 2 的幂，但必须满足对象/card 对齐、arena/TLAB/metadata 和 32-bit 表示范围。512 B、8 KiB、24 KiB、128 KiB 和 1 MiB 已有覆盖；ABI minor 71 的多线程 fast allocation、ABI minor 74 的 TSS，以及 ABI minor 75/76/77/78 的 deferred append/dedup/global-coverage/classification 测试都使用 24 KiB 非 2 次幂 Region。
- ABI minor 72 增加 per-thread 自适应 TLAB sizing。`AUTO + tlab_size==0` 解析为 SMART，`AUTO + tlab_size!=0` 解析为 FIXED，保留 ABI 71 及以前显式 `tlab_size` 的固定粒度预期；显式 SMART 把 `tlab_size` 解释为 initial target，并接受 64-byte 对齐且满足 `min <= initial <= max <= region_size` 的 `tlab_min_size/tlab_max_size`。默认 SMART 初值为 `min(16 KiB, region_size)`、下限为 `min(4 KiB, initial)`、上限为实际 Region，因此 512-byte 到非 2 次幂 Region 都使用同一几何合同。
- SMART 只采样“实际 span 恰好等于当前线程 target”的 retire，排除 oversized one-object span；连续两次至少约 87.5% 利用率时 target 倍增，连续三次不超过 50% 利用率时 target 减半，并受 per-heap min/max 和 64-byte alignment 约束。target 只影响 retire 后紧接着的下一次 refill，绝不改写 active span 边界；FIXED 线程的 target/streak 永远保持配置值/零。
- ABI minor 72 stats 暴露 resolved policy、initial/min/max、当前注册线程 target 的 min/max，以及 sample/grow/shrink/hold 计数。常驻 verifier 校验 heap 几何、每线程 target/streak 和 FIXED 不变量；Verify fault matrix 增加 adaptive-target 损坏。24 KiB Region 上的确定性测试用 4/2/16 KiB initial/min/max 验证两次高利用率增长到 8 KiB、三次低利用率收缩回 4 KiB，并证明 frozen-type fast path、locked retire/refill 和动态目标兼容。
- 并行 REMAP 要求用户 scanner 可重入：同一 `scan`/`scan_ex` 函数可能同时处理不同对象，slot 集合必须只由 owner 类型/布局决定，不能依赖当前 referent 值；scanner 不能缓存 visitor/context、跨 owner 上报 slot、修改非引用 payload、阻塞或重入 NGGC API。legacy `scan` 没有错误返回通道，ABI minor 88 的 `scan_ex` 可直接返回错误并协作 poll；非法 slot 由 visitor/verifier 转为 collection failure。
- REMAP worker 使用独立 epoch、active/peak/quiescent 与 block/scanned-slot/rewritten-slot 统计。`SWEEP`、loser/from-space reclaim 和 cycle completion 同时要求 copy 与 REMAP worker epoch 匹配、active 为零且 quiescence 已发布；新增 verifier 故障覆盖 REMAP active worker 出现在错误 phase，以及未归静时错误进入 reclaim。
- 内部 weak/soft/phantom runtime object 可以出生在 YOUNG，并由 REMAP 专用路径更新 weak/soft referent、phantom queue 的 head/tail 以及 phantom reference 的 referent/queue/next；reaper 观察项按 `(mark_epoch, object_id)` 校验，失效或地址复用时丢弃。`NONE + INTERNAL` forwarded block 由统一 parser 接受，普通 `NONE` 仍拒绝。这不是预晋升。
- 普通对象 bump allocation 已限制在单个 region 内；区尾空间不足时发布为合法 free padding block。large object 会先推进到下一 Region 边界，再占用按实际 Region 尺寸取整的完整 humongous run；padding 仍可被普通 free-list 分配拆分复用。
- 每个 region 已记录独立 allocation high-water cursor 和 allocator owner token；当前只有全局 allocator 持有一个未封口 region，free-list reuse 不改变 high-water。
- allocator lease 带 epoch/acquire/revoke 统计；safepoint 或 GC phase 会撤销 owner。当前持 heap lock 的共享 bump 在 safepoint pending 时仍可无 lease 推进，但不会重新发布 owner；真正 TLAB 必须冻结后才能 ack。
- Verify test hooks 已覆盖 region allocation cursor 漂移、非法/重复 owner、active lease 与 pending safepoint 冲突，并验证故障后状态可恢复。
- Thread/TLAB 下一增量协议已冻结：active reserved span 由 region side metadata 描述，published top 之前保持可遍历，未使用尾部不得伪装成普通对象或进入全局 free list；safepoint ack 前必须 retire。
- Region/thread 已加入 reserved-span shadow metadata 与一致性 verifier；真实加锁 slow-path reserve/retire 已落地，合法 active span 可以通过完整 `nggc_check`，safepoint 和 thread unregister 会在确认/摘链前强制 retire。
- Active span 使用内部 `TLAB_RESERVED` sentinel，不复用 FREE/ALLOCATING；统一 parser 会校验 sentinel 与 region/thread owner、lease epoch、published top 和 end 完全一致。
- 统一 block parser 已覆盖全部 arena 线性 walker，并接受 LIVE/FREE/TLAB_RESERVED；reserved block 被 mark、reference processing、sweep、RemSet、scanner 和 free-list 自然跳过。
- Region 统计已加入 reserved/quarantined bytes，守恒式为 `live + free + reserved + quarantined == committed`；stats 暴露 reserved block/byte、quarantined region 和延迟回收 epoch/bytes/object 数。
- TLAB span 内对象 publish 已有两层路径：未冻结类型表和 refill/retire 使用 heap-lock slow path；ABI minor 71 在 frozen type table、owner 匹配和 gate 开放时使用不取得 heap lock 的 per-thread fast path。两条路径都支持连续发布、尾部 sentinel 重建和整段耗尽，large/pinnable/non-movable 对象继续路由标准 slow path。
- 线程分配 slow path 已能自动 reserve/publish，并在 span 空间不足时 retire/refill；ABI minor 72 默认使用自适应 per-thread target，初始值为 `min(16 KiB, region_size)`，超出 target 但不超过 Region 的普通对象按对象大小单独 reserve，且该 oversized span 不污染 sizing 样本。
- 公共 `nggc_thread_alloc` / `nggc_thread_alloc_size` 已暴露 thread-aware 分配语义；stats 报告 TLAB refill、retire 和成功 allocation 次数。
- 多线程 refill 会跳过已有 TLAB owner 的 region，通过发布区尾 free padding 推进到新区；safepoint pending 窗口禁止新 lease，thread allocation 临时退回普通持锁分配。
- `nggc_config.tlab_sizing_policy/tlab_size/tlab_min_size/tlab_max_size` 可配置 refill 策略。AUTO 对旧显式 `tlab_size` 保持 FIXED，对零初始化配置选择 SMART；所有 resolved target 必须 64-byte 对齐且不超过实际 Region，stats 同时回报初值、边界和注册线程当前目标。
- 公共 thread-aware API 对所有已注册类型可用：普通小对象走 TLAB，large/pinnable/non-movable 自动路由标准分配器且不打断当前 active TLAB。
- TLAB telemetry 包含累计 refill bytes、retired unused bytes、成功 fallback allocation count，以及 ABI minor 72 的 sizing sample/grow/shrink/hold；当前 SMART v0 直接按每次目标 span 的 retire 利用率做有界滞回，不依赖 Debug verifier 或平台时钟。
- per-thread root stack 已通过 `nggc_thread_root_push/pop` 落地，并接入 mark、assist、remap、forwarded-slot verifier；stats 的 root count 统计 global + thread roots。
- registry fault injection 已覆盖 thread-root count 越界、跨 slot 重复和未对齐 slot，并验证每次故障后的状态恢复。
- TLAB verifier 已拒绝“线程已 ack 当前 safepoint epoch 但仍持 active TLAB”，并通过完整 heap check 拒绝 reserved sentinel 被挂入 free-list。
- 第二批已迁移 remap、mark-epoch reset、assist sweep 和 type-live walk；剩余集中在 reference processing、RemSet rebuild verifier 与少量专用检查。
- 第三批已迁移 RemSet rebuild、soft/weak/phantom processing 和 reference verifier；arena 线性 walker 现已统一，剩余 magic 检查只属于 parser、单对象 API 与 free-list 链表节点。
- `nggc_check` 能遍历 arena，验证对象头、类型、大小和扫描函数报告的引用 slot；Debug/Verify 额外执行 scanner replay。
- `nggc_collect` 已经是非移动 mark-sweep：从 root slot 标记，未标记对象进入 free list。
- 分配器会优先复用 small free list；large 请求依次搜索 committed large-free bins、可定址 recommit 的 decommitted side bins，最后才推进 bump high-water。small block 支持尾部拆分和同 pool、同 Region generation/placement 边界内相邻合并，humongous run 拆分后重新分档且不跨 run 合并。
- stats 能报告 GC arena 和 UHMAS backing stats。

## 3. 当前简化边界

- global 和 per-thread root stack 已落地；当前 verifier 仍不能证明外部 C root slot 的实际生命周期，调用者必须在 slot 失效前 pop。
- mark-sweep 是 stop-the-world 原型，还没有并发标记。
- free list 已支持拆分、受 placement 约束的相邻合并、tail Region decommit/recommit，以及 ABI minor 65 的整段 interior humongous-hole decommit/recommit；partial Region 和普通小块内部空洞仍保持 committed/free-list 语义。
- 大对象已有 ABI minor 62 的独立 free space、ABI minor 63 的 Region-aligned humongous run/start-continuation metadata、ABI minor 64 的 32 档 committed bins、ABI minor 65 的 decommitted side bins/伪块 parser、ABI minor 66 的 per-heap decommit 阈值/禁用配置、ABI minor 67 的 tail-adjacent side-run 固定点折叠、ABI minor 68 的 SMART v0 自适应阈值/滞回、ABI minor 69 的档内 ordered best-fit/过滤 telemetry，以及 ABI minor 70 的外部压力/VM 延迟输入。ABI minor 79 已增加第一阶段动态 Pin lease、pinned Region 计数和 pinned Young evacuation 降级；ABI minor 80 已增加 post-forwarding fail-stop 查询与 force-destroy 出口；ABI minor 81 已增加 per-thread/fair bounded Mutator Assist。当前两类 bin 内仍是链表，查找与有序插入最坏为 O(k)；尚未实现 telemetry 驱动的 tree/二级索引、内建 OS pressure provider、自动延迟阈值校准、低频 large compaction、pin owner/token、超时 lease 或长期 pinned space 管理 API。
- card table 的最终表示仍是单 heap级atomic-card表，还没有Region remembered set；ABI minor 73已增加per-thread publication buffer，ABI minor 74让C11唯一-owner查找走TSS O(1)，ABI minor 75/76把未满buffer的append和线性去重移到heap unlock后，ABI minor 77又把同一路径的global-card coverage读取锁外化，ABI minor 78再把 C11 owner 的边分类共享 64-bit telemetry 改为锁外 32-bit outcome 与后续持锁折叠。`nggc_store`的对象/forwarding/Assist校验、coarse判断、满buffer去重/overflow、descriptor reservation和slot publication仍持heap lock；Region/generation分类推导已移到解锁后的冻结 descriptor 路径，buffer也尚未按mark/remark epoch分代。TLAB已有ABI minor 71 per-thread fast allocation与ABI minor 72自适应target；refill/retire、延迟统计flush和普通slow allocation仍会关闭heap-level fast gate并持heap lock。
- Pin 机制已落地第一阶段 ABI minor 79，并由 ABI minor 84 增加 lease 归属账本：`PINNABLE` 对象支持嵌套 `nggc_pin` / `nggc_unpin`，对象头记录动态 pin count；Pin 不构成 root，不可达对象仍回收。pinned Young 在 evacuation 中原地保留并继续增龄，达到阈值后等到 unpin 才允许后续搬迁/晋升；Region 与 heap 统计区分 pinnable 能力、实际 pinned object 和 pin lease。当前 Pin API 仍在 heap lock 下先完成 TLAB gate quiescence，GC/safepoint/Assist/quarantine 不稳定窗口返回 busy；已注册 owner 负责自己的 lease，跨 owner 释放和持 lease 注销会被拒绝；独立 token、超时 lease、长期 pinned space 和并发 REMAP 仍未实现。
- 弱引用已有公开 create/get v0；还没有更高层 typed wrapper、批量 drain 或语言绑定语义。
- 软引用已有公开 create/get v0 和可配置 heap-pressure/always-retain/always-clear 策略；还没有 SMART 自适应策略接入。
- 虚引用和虚引用队列已有公开 API、手动 drain v0、reaper 弱观察/轮询/有界等待 v0，以及 ABI minor 87 的事件驱动阻塞等待；仍没有真正后台 reaper 线程。后台线程需要另行定义消费回调、所有权、取消和 close join 协议。
- reference-processing 目前处理内部 weak discover/clear、soft retain/clear、marked phantom 到关联 queue 的保活，以及 phantom enqueue/queue promotion；ABI minor 85 已增加可恢复 bounded packet，但完整 collector 仍在现有 heap lock 内原子完成；未来并发 reference discovery 需要独立 discovered lists/epoch metadata。
- scanner replay 目前是持有 heap lock 的同步双次重放；ABI minor 51 已冻结 scanner 对不同对象的并发可重入契约，ABI minor 88 增加可选 `scan_ex` 显式错误返回、`control->poll` 协作取消和 Verify-only 同线程 API 重入拒绝，并保持 legacy scanner 兼容。超时/阻塞、跨线程滥用和缓存 visitor/context 的通用运行时检测仍未完成；回调不得缓存上下文或进入 GC API。
- 读屏障已有 phase-or-quarantine-gated active path、原子 header forwarding acquire-follow、32 位 evacuation epoch、精确目标预留、持久共享 worker pool、parked safepoint 内并行 young copy/REMAP，以及 ABI minor 52 跨 safepoint delayed from-space reclaim。ABI minor 54/55/56 又加入受限 barrier-mediated copy window、注册线程的 EVACUATE resume/repark 状态和 poll/park 驱动的协作重停车：窗口期间临时释放 heap lock，允许 `nggc_load/store` 进入并保护 payload slot；REMAP、SWEEP 和完整 root/handle/RemSet 闭包仍未并发化。
- Mutator Assist 已实现持久化 bounded mark/sweep work packet、per-thread debt、actor 优先偿还、动态预算、公平性 cap、无进展退避、进展统计和 dedicated test；当前仍由 allocation-failure 驱动，root snapshot/reference processing 尚未切片，也还没有并发 worker 或真正独立的按线程 work queue。Assist 状态和 debt 仍由 heap lock 保护。
- phase 目前已接入 `nggc_collect` / `nggc_thread_collect` 和持久共享 GC worker pool，copy/REMAP 可在 parked safepoint 内并行；还没有并发 marking worker。自动 safepoint 使用非阻塞 request/wait 协调，普通 mutator 可通过 `nggc_safepoint_park` 阻塞到 release。ABI minor 86 可选地对满足全体存活对象年龄条件的 Young Region 做原地晋升，不改变 phase 图或 Pin 语义。
- collector cycle 已覆盖非移动和 young moving 的 expected-phase 图；未来加入 concurrent mark、remark、plan-reclaim 时仍必须继续扩展该图，不能绕过 verifier 复用 standalone phase。
- collector coordinator 已增加显式 cycle kind：非移动 cycle 保持 `ROOT_SNAPSHOT -> MARK -> REFERENCE_PROCESS -> SWEEP`；young moving cycle 的内部合法图固定为 `ROOT_SNAPSHOT -> MARK -> REFERENCE_PROCESS -> EVACUATE -> REMAP -> SWEEP`。公开 young collection 在目标空间 preflight 可行时切换 moving kind；容量不足则在任何 forwarding 发布前退回非移动 sweep。
- 对象分配先使用单个 bump arena 和一把内部锁。
- 对象 payload 当前固定 64-byte 对齐；更细的 alignment 策略后续再做。

## 4. 后续迁移边界

NGGC 对 UHMAS-HL 的直接依赖必须逐步收敛到 backend 层。未来真实 UHMAS 可替换：

```text
nggc_backend_create
nggc_backend_destroy
nggc_backend_map
nggc_backend_unmap
nggc_backend_decommit
nggc_backend_recommit
nggc_backend_get_stats
nggc_backend_check
```

当前 backend 包装已拆到独立源文件，后期把 UHMAS-HL 实现替换为真实 UHMAS 实现时，应保持 `BACKEND-CONTRACT.MD` 的 NGGC-facing 合同稳定。ABI minor 57 已把 UHMAS-HL `check` 纳入该边界：`nggc_check` 的成功现在同时要求 GC verifier 和 backing heap verifier 通过。ABI minor 58 进一步预留 decommit/recommit 钩子；真实 UHMAS 可替换为原生页提交控制，当前 UHMAS-HL 仍是固定地址重映射模拟。ABI minor 59 暴露 backend 页几何，ABI minor 60 已把该几何接入 tail Region reclaim 和按需 recommit，ABI minor 65 又把完整页对齐的非尾部 humongous run 接入整段 decommit 与 fixed-address recommit。partial Region、普通小块 hole 和未页对齐范围仍保持 committed/free-list 语义。

## 5. 下一阶段方向

优先顺序：

1. 在 ABI 56 registered-mutator EVACUATE handshake 和 ABI 73 safepoint/repark card-buffer flush 基线上，继续实现 mutator-concurrent REMAP、root/handle/barrier-buffer epoch 闭包和并发 RemSet 修正；ABI minor 80 已提供 fail-stop quarantine 的状态查询与最终销毁出口，但当前阶段仍不能把受控 copy window 宣称为完整并发搬迁。
2. 在 ABI 81 bounded Mutator Assist、ABI 83 discovered-list、ABI 85 bounded reference-processing packet 和 ABI 86 整区原地晋升基线上，先以实测数据校准 debt 权重、packet 档位、退避和 Region 晋升收益；并发 worker、无锁 work queue 和并发 reference discovery 另行设计。
3. 扩展 ABI minor 80 的 fail-stop 诊断与宿主停机合同；仍不提供半完成 moving cycle 的恢复或继续运行语义。
4. 扩展 collector cycle 到 concurrent mark、remark 和并发 moving phases。
5. 按用户要求暂缓单生产者 descriptor reservation/slot publication 移锁。先收敛 root/handle/TLAB/barrier-buffer/RemSet/forwarding 的并发闭包，再在 ABI 75-78 基线上单独设计 mark/remark epoch、collector drain/交换与最终 Region RemSet，以及按线程/span 的 TLAB quiescence。后台 reaper、large-bin tree/二级索引、压力 provider 校准、large compaction、原生 decommit/recommit、NUMA 和 Region placement 仍作为后续评估项。

## 6. 本轮验证（2026-10-10）

ABI minor 88 worker 显式失败退出、repark 等待修复、真实 mutator 并发 copy、Verify-only scanner 重入诊断、canonical owner 读屏障与原子诊断/逐调用错误回传已重建并验证：

| 配置 | 构建目录 | 完整 CTest |
| --- | --- | --- |
| MSVC 64-bit Debug | `build-verify-current` | 23/23 |
| MSVC 64-bit Release | `build-release` | 23/23 |
| MSVC 32-bit Debug | `build-x86-current` | 23/23 |
| MSVC 32-bit Release | `build-x86-current` | 23/23 |
| MinGW 64-bit Debug，无 `<threads.h>` | `build-mingw-current` | 19/19 |

32-bit Debug/Release 顺序构建。32-bit Debug 在当前机器需要把 VS Build Tools 的 `debug_nonredist/x86/Microsoft.VC143.DebugCRT` 放入测试进程的 PATH，以提供 `vcruntime140_threadsd.dll`；该环境设置不改源码或 Release 配置。

worker pool 增量完成时的 `nggc_gc_pool_tests` 连续 30 次通过，本轮全量矩阵继续覆盖 coordinator wait/broadcast/join 与七个 worker 失败窗口。repark/canonical-owner 增量完成时，`nggc_concurrent_copy_tests` 在 MSVC x64/x86 Debug/Release 四个配置各连续 30 次通过；每次包含四个 native owner、三轮复制、Pin 和年龄晋升、source-lock 竞争与重新停车阻塞检查。构建目录中的临时 mutation 副本恢复旧等待谓词后，在“reparked mutators block instead of reusing resume epoch”断言处失败，证明专项能检出本次修复的缺陷。

canonical-owner 读回归先在旧实现上实际失败于“stale-owner load reads null from canonical field”，修复后五组全量矩阵通过。加严后的并发 copy 专项在四组 MSVC 配置各连续 30 次通过，交替让 load/store 等待真实 copy worker 的 source payload lock，并验证 canonical owner 写入 null/替换 referent 后的旧地址读取。同步 young 专项覆盖 512 B、24 KiB、128 KiB Region 下的大对象 start/continuation slot、外部 alias 自愈，以及 header/padding/未对齐 slot 的拒绝。

新增 `nggc_scanner_guard_tests` 在 MSVC x64/x86 Debug/Release 四个配置各连续 30 次通过，覆盖两种 scanner 的 54 个公共 API、跨 heap 拒绝、poll context、错误锁存、MARK abort/release、后台 REMAP 应急完成/下一轮复用和两 heap 重叠扫描；MinGW 无 `<threads.h>` 目标覆盖同步矩阵。公共头/实现入口审计为 54/54 guard。`build-verify-current/scanner-guard-mutation` 中只关闭公共 API guard 的临时副本在首个 API 重入 case 返回 `NGGC_OK` 并立即失败，证明专项能检出诊断缺失；原源码不作 mutation。

新增 `nggc_error_result_tests` 确定性暂停四种失败创建 API：helper 已返回 `BUSY` 并释放 heap lock 后，另一 native thread 通过无效 Pin 把共享诊断改为 `INVALID_ARGUMENT`，创建 API 仍须返回自己的 `BUSY` 和空输出。三线程另以每人 4000 次操作重叠两类错误发布与诊断读取，验证逐调用结果及 allocation/live/root 守恒；无线程构建覆盖同步失败矩阵。`build-verify-current/error-result-mutation` 保留原子诊断但恢复四个创建 API 从共享诊断取错误，实际失败于“reference creator preserves its own failure and null output”，说明原子性不能替代逐调用结果。

错误回传增量的最终源码在 MSVC x64/x86 Debug/Release 四个配置中，`nggc_error_result_tests` 各连续 30 次通过。源码审计确认 `last_error` 只经原子初始化/setter/getter访问，四个创建 API 无共享诊断回传；全部 54 个公共 API 的入口 guard 保持齐全。

MSVC 对正式配置的预处理结果确认不包含 scanner TLS/guard/phase hook、pool/copy 故障注入或错误回传暂停字段、闸门、等待计数、辅助函数和分支；原子诊断与局部 result helper 保留在正式代码。测试目标显式启用 Verify+Test Hooks，独立于正式库。`git diff --check` 通过。scanner 超时/阻塞/跨线程滥用/缓存上下文、未报告的 worker 终止/阻塞、完整并发 REMAP/root/handle 闭包与 TSAN 实测仍未完成，移锁继续暂缓。

## 7. 公开发布准备

根 README 和 `draft/` 整理项目摘要、能力边界、验证记录、首次发布说明与匿名发布步骤。发布脚本从 tracked + non-ignored untracked 文件生成独立快照，包含当前未提交的源码/测试，不继承开发仓库的历史、身份、配置、hooks 或构建产物；本地开发历史继续保留。

候选独立 Git 仓库、无 `.git` 的源码 ZIP、SHA-256 清单与匿名扫描报告生成到已忽略的 `out/publication/`。公开作者统一为项目贡献者占位身份，发布日期使用统一 UTC 时间；扫描报告不回显敏感文本或来源绝对路径。尚未添加许可证，未创建或推送远程仓库。
