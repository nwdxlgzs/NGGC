# NextGenerationGarbageCollector TODO

## 0. 当前决策

- [x] 使用 `mock-uhmas/UHMAS-HL` 作为第一阶段内存操作底座。
- [x] 先从 UHMAS-HL 移植路径开发，后期再从 UHMAS-HL backend 切到真实 UHMAS。
- [x] 根项目使用 CMake。
- [x] 当前原型使用 C11。
- [x] `NGGC_TYPE_NONE == 0`，用户类型使用 `1..65535`。
- [x] 用户通过注册扫描函数描述对象引用布局。
- [x] 当前 ABI minor 为 `88`；ABI minor 80 的 fail-stop 管理面、ABI minor 81 的 Mutator Assist 增量、ABI minor 82 的触发器、ABI minor 83 的 discovered-list 协议、ABI minor 84 的 Pin 归属账本、ABI minor 85 的 bounded reference-processing packet、ABI minor 86 的可选整区原地晋升、ABI minor 87 的 PhantomQueue 阻塞等待通知和 ABI minor 88 的 scanner/worker-pool 故障闭环都不等同于完整并发搬迁恢复或完整 lock-free barrier。

## 1. 文档

- [x] 编写长期架构文档 `GC-ARCH-PLAN.MD`。
- [x] 编写当前工程计划 `PLAN.MD`。
- [x] 编写当前工程待办 `TODO.MD`。
- [x] 随代码推进持续同步 Phase 0/1 状态。
- [x] 后续补充 backend contract 文档。

## 2. 构建与项目结构

- [x] 添加根级 `CMakeLists.txt`。
- [x] 将 `mock-uhmas/UHMAS-HL` 接入为子目录依赖。
- [x] 添加 `nggc` 库目标。
- [x] 添加基础测试目标。
- [x] 增加 Debug/Release/profile 配置开关。
- [x] 增加 sanitizer/fault-injection/profiling 宏开关。

## 3. UHMAS-HL 内存底座

- [x] 使用 `uhm_platform_heap_open` 打开 UHMAS-HL heap。
- [x] 使用 UHMAS `mmap` 建立 GC arena。
- [x] 使用 UHMAS `munmap` 释放 GC arena。
- [x] 从 UHMAS stats 暴露 backing 信息。
- [x] 抽象 `nggc_backend_*` 接口，隔离 UHMAS-HL 细节。
- [x] 增加 backend OOM/failure 注入测试。
- [x] 将 backend 包装从 `src/nggc.c` 拆到独立源文件。
- [x] ABI minor 57：将 UHMAS-HL `check` 接入私有 `nggc_backend_check`，并让公共 `nggc_check` 在 GC verifier 通过后继续验证 backing heap；fault injection 覆盖 private/public 失败路径。
- [x] ABI minor 58：增加私有 `nggc_backend_decommit/recommit`，当前 UHMAS-HL backend 用 `munmap` + fixed no-replace `mmap` 模拟，fault injection 覆盖失败恢复；尚未接入 Region reclaim。
- [x] ABI minor 59：从 UHMAS-HL 暴露 backend page size、allocation granularity 和 decommitted bytes 到 `nggc_stats`，为后续页对齐 Region reclaim verifier 提供观测输入。
- [x] ABI minor 60：将 backend decommit/recommit 接入 GC tail Region reclaim；full collection 后仅对 verifier 可证明为空、无 TLAB/quarantine/collection-set/owner、backend page 对齐且位于 arena 尾部的整 Region 调用 decommit，后续 bump/TLAB/evacuation 目标写入前按需 recommit。

## 4. 对象模型与类型系统

- [x] 添加固定对象头。
- [x] 添加 `ALLOCATING -> LIVE` 发布状态。
- [x] 添加类型注册表。
- [x] 支持自动分配 type id。
- [x] 支持用户指定非零 type id。
- [x] 支持扫描函数。
- [x] ABI minor 88 增加可选 `nggc_scan_fn_ex`：scanner 可显式返回错误，并通过受控 `poll` 协作取消；保留旧 `nggc_scan_fn` ABI，描述符尾部扩展按 `size` 复制，Release/Verify 的 scanner、RemSet、mark、REMAP 和 `nggc_check` 路径均保留 callback 错误。
- [x] 补充 `GC_TYPE_NONE` 内部对象策略。
- [x] 补充类型冻结策略。
- [x] 补充类型注销策略。
- [x] 补充大对象和 pinned/non-movable 对象标志。

## 5. 分配器

- [x] 在 UHMAS-backed GC arena 内 bump allocate。
- [x] 支持定长对象分配。
- [x] 支持显式 payload size 分配。
- [x] 统计 live object count 和 live payload bytes。
- [x] 支持 free list 或 region reclaim。
- [x] 支持 free block 拆分。
- [x] 支持 free block 合并。
- [x] 支持 per-thread TLAB reserve/publish/retire/refill 与公共 thread-aware allocation；ABI minor 71 后 frozen-type 普通小对象 publish 使用 per-thread fast path，refill/retire 与未冻结类型表继续走 heap-lock slow path。
- [x] 增加 region/thread reserved-span shadow schema：owner、lease epoch、start/top/end、retire state，并验证 inactive 状态。
- [x] 实现 active reserved span 的 reserve/publish/retire slow-path 状态转换、尾部 sentinel 重建和整段耗尽。
- [x] 增加内部 `TLAB_RESERVED` sentinel state，并确保永不进入 free list/mark/scanner。
- [x] 抽象统一 block walker，并让全部 arena 线性遍历支持 LIVE/FREE/TLAB_RESERVED。
- [x] 统一 block parser v0：收敛 sweep、free-list rebuild、region/check 和 forwarded-slot 核心 walker。
- [x] 将 reference processing、type lookup、remap 与 arena verifier walker 全部迁移到统一 parser。
- [x] 迁移 remap、mark-epoch reset、assist sweep 与 type-live walker。
- [x] 迁移 soft/weak/phantom processing、reference verifier 和 RemSet rebuild walker。
- [x] 扩展统一 parser 验证 active `TLAB_RESERVED` span，加入 reserved region 守恒、stats 和合法 span 正向测试。
- [x] 实现 TLAB reserve/retire；第一版 slow path 持 heap lock。
- [x] 增加 thread allocation slow path，自动执行 reserve/publish/retire/refill。
- [x] 暴露 `nggc_thread_alloc` / `nggc_thread_alloc_size` 并增加 TLAB refill/retire/allocation stats。
- [x] 支持多个线程在不同 region 持有独立 TLAB，并为 safepoint pending 窗口提供 locked allocation fallback。
- [x] 增加可配置 TLAB refill size、默认值、边界校验和 stats 观测。
- [x] 让 thread-aware API 自动把 large/pinned/non-movable 对象路由到标准 slow path。
- [x] 增加 TLAB refill bytes、retired unused bytes 和 fallback allocation telemetry。
- [x] 实现 per-thread root stack，并接入标记、remap、forwarded-slot verifier 和线程注销约束。
- [x] 增加 per-thread root count/duplicate/alignment 故障注入和恢复测试。
- [x] ABI minor 71 将已验证的 TLAB reserve/publish/retire 演进为 per-thread fast path/refill slow path：只有 C11 owner、冻结类型表、OPEN gate 和 active TLAB 同时成立时，每对象路径才不取得 heap lock。
- [x] ABI minor 71 增加 32-bit gate/inflight 与 atomic fast-top 协议：exclusive observer 先关闭 gate、等待全部 inflight 清零，再 acquire 读取 top 并 flush；不依赖双宽 CAS 或共享 64-bit lock-free atomic。
- [x] ABI minor 71 分离 fast top 与 accounted top，保留 `TLAB_RESERVED` 尾部 sentinel 和 `ALLOCATING -> LIVE` release 发布顺序；Region/heap/assist/TLAB 统计由 locked flush 批量且只入账一次。
- [x] ABI minor 71 分离 `next_object_id` 高水位和 `allocation_count`，TLAB refill 在 heap lock 下租赁 ID 段；未使用 ID 空洞合法，fast path 不执行共享 64 位原子。
- [x] ABI minor 71 将 thread-aware 分配、collector-owned collection 和注销绑定到注册它的 C11 owner thread；foreign handle allocation 被拒绝并计数，foreign collect/unregister 被拒绝，避免 handle 生命周期或角色转换竞态。
- [x] ABI minor 71 增加 fast allocation/slow-path/flush/flushed-object/gate-close/wrong-owner telemetry，以及 fast-top/inflight/ID-lease/gate fault matrix；确定性 pause 测试证明 safepoint 等待 inflight publication 并仅 flush 一次。
- [x] ABI minor 71 在 24 KiB 非 2 的幂可配置 Region 上覆盖 4 个 C11 mutator 的并发 fast allocation、foreign owner 拒绝、safepoint gate 和 quiescence；Region 仍按 heap 配置，不退回固定 64 KiB 假设。
- [x] ABI minor 78 同批可靠性修复补齐 TLAB gate 的 pre-increment late enrollment：mutator 可在读到 `OPEN` 后、`inflight++` 前遇到 observer 关门；其二次 gate 检查前不修改对象，closer 若在 flush 时撞见迟到 inflight 必须重新 quiesce/flush，不能把内部 `BUSY` 泄漏给 stats、safepoint 或 allocation slow path。Verify 两阶段握手在 24 KiB Region 上确定性命中“零 inflight 扫描 -> 迟到登记 -> flush BUSY -> 重试成功”。
- [x] ABI minor 72 增加 `AUTO/SMART/FIXED` TLAB sizing：零初始化配置默认 SMART，旧式显式 `tlab_size` 在 AUTO 下保持 FIXED；SMART 支持按实际可配置 Region 验证的 initial/min/max，FIXED 禁止额外边界参数。
- [x] ABI minor 72 实现 per-thread target 与 retire-time 滞回：连续两次高利用率倍增、连续三次低利用率减半，只采样与当前 target 等长的 span，结果只作用于下一次 refill，不修改 active span。
- [x] ABI minor 72 增加 policy/bounds/current-target/sample/grow/shrink/hold telemetry、target/streak 常驻不变量和 adaptive-target 故障注入；24 KiB 非 2 次幂 Region 上确定性覆盖 `4 KiB -> 8 KiB -> 4 KiB`，512-byte Region 覆盖默认 min/initial/max 自动收缩。
- [x] ABI minor 73 实现 per-thread RemSet/card publication buffer：容量按 heap 配置，零值为 `min(64, card_count)`；同线程 card index 去重，容量满时只 flush 当前线程，全局表继续确定降级 `PRECISE_CARD -> COARSE_HEAP`。
- [x] ABI minor 73 将 card-buffer flush 接入 safepoint ack、mutator-concurrent EVACUATE repark、collector-owned 转换和 thread unregister；parked/collector-owned/reparking 线程必须无 pending card，stats 观察 pending 数但不主动 flush。
- [x] ABI minor 73 增加 buffered/direct/dedup/flush/overflow telemetry、全局 card 与 pending buffer 并集 rebuild coverage、常驻结构 verifier、越界 buffered-card 故障注入，以及 24 KiB 非 2 次幂 Region 多线程注销 flush 回归。
- [x] ABI minor 74 增加 C11 TSS per-heap barrier context：单一全局 key、每原生线程多 heap 链、唯一注册句柄 O(1) 命中、同 heap 多句柄显式歧义、注销后恢复唯一，以及无 `<threads.h>` registry-scan fallback。
- [x] ABI minor 74 将 TSS 定义为非权威性能缓存：创建/分配/设置失败不使注册失败，barrier 回退权威 registry；注销最后句柄发布 tombstone，线程退出 destructor 只释放 context 而不隐式注销 heap handle；增加 hit/fallback/ambiguous/update-failure/no-owner telemetry。
- [x] ABI minor 74 增加当前线程 TSS 链 verifier、强制 update failure 故障注入，以及 unique/ambiguous/unregister-recovery/multi-heap/no-owner/四 worker 命中测试。
- [x] ABI minor 75 在 C11 owner 路径把已预留 card 的数组写与 published-count release publication 移到 heap unlock 后；slot publication 前先发布一个有效 in-flight descriptor，使 global card、published prefix 和 descriptor 的并集始终覆盖新边。
- [x] ABI minor 75 增加 32-bit `atomic_uint` count/inflight 协议、`capacity <= UINT_MAX` 配置边界、effective/inflight stats 和常驻 snapshot verifier；safepoint/repark/collector-owned/unregister flush 必须观察 inflight 为零，无 `<threads.h>` fallback 继续锁内 append以保护句柄生命周期。
- [x] ABI minor 75 增加 unlock 后/数组发布前的确定性 pause、in-flight card fault 305、RemSet rebuild/full-check/safepoint closure 回归；24 KiB 非 2 次幂 Region 证明 descriptor 只依赖 card index，不引入固定 Region shift。
- [x] ABI minor 76 将 C11 未满 buffer 的线性 card 去重移到 heap unlock 后：descriptor 先覆盖 slot publication，owner扫描稳定 published prefix；重复时保持 count，非重复时才写数组并 release-publish count。满 buffer和无 `<threads.h>` fallback继续锁内去重。
- [x] ABI minor 76 增加 per-thread 32-bit deferred outcome、locked telemetry folding和 `remset_buffer_deferred_deduplicated_count`；snapshot验证 APPENDED/DEDUPLICATED 与 descriptor/count/prefix一致，fault 306拒绝语义不匹配 outcome。
- [x] ABI minor 76 扩展24 KiB非2次幂 Region确定性窗口：在 unlock 后扫描前分别暂停新 card和已存在 card，stats/rebuild/full-check/safepoint closure均接受，恢复后分别得到一次append或一次deferred dedup；容量1的满buffer重复写仍不产生无谓overflow flush。
- [x] ABI minor 77 将global card table改为逐项C11 `atomic_uchar`，C11 owner未满-buffer路径在descriptor/slot发布并unlock后才acquire检查global coverage；命中时以`COVERED`结束，不扫描prefix、不append，满buffer/coarse/threadless继续锁内慢路径。
- [x] ABI minor 77扩展deferred outcome与telemetry，stats报告atomic-card lock-free能力与deferred-covered次数，snapshot把atomic global card纳入descriptor effective coverage并拒绝非0/1值；fault 307和24 KiB非2次幂Region确定性窗口覆盖global-covered descriptor、safepoint、rebuild、full-check及生命周期。
- [x] ABI minor 78 将 C11 unique-owner 路径的 `remset_record/cross-region/OLD-to-YOUNG/buffer-record` 四类共享 64-bit 更新替换为 per-thread frozen classification descriptor、独立 32-bit inflight/outcome 与后续持锁折叠；direct/no-owner和threadless fallback继续锁内计数。
- [x] ABI minor 78 冻结合法分类状态 `RECORD`、`RECORD|CROSS`、`RECORD|CROSS|OLD_TO_YOUNG`，以 owner/value Region+generation 在 Release 中重算，null value 使用 `SIZE_MAX/NONE`；stats把未折叠事件作为有效快照而不修改共享基数，parked/collector-owned/reparking线程必须无 pending classification。
- [x] ABI minor 78 增加 pending/deferred-classification stats、fault 308和 outcome-published/inflight-not-cleared 双 stats 窗口；24 KiB非2次幂Region确定性覆盖 null/same-region、YOUNG跨Region、OLD-to-YOUNG、global-covered、safepoint flush和unregister，五套32/64-bit/threadless矩阵通过。
- [x] ABI minor 78 细化分类锁外化验证：将 Verify 暂停点放在 frozen Region/generation descriptor 的分类推导之前，另一线程在 outcome 尚未发布时取得 heap lock 完成 stats、RemSet rebuild 和 full check；确认分类推导只依赖冻结描述，不延长 heap-lock 临界区。reservation、slot publication 和对象/forwarding/Assist 生命周期校验仍明确留在锁内。
- [ ] 【按用户要求暂缓移锁】在前置并发闭包收敛后，把 ABI 78 的单生产者 descriptor reservation 和 slot publication 快路径移出 heap lock，并实现按 mark/remark epoch 的 buffer drain/交换与最终 Region RemSet；Region/generation 分类推导已移到冻结 descriptor 的锁外路径，当前 owner/forwarding/Assist 校验、coarse/满buffer overflow、descriptor reservation 和 slot publication仍持 heap lock。
- [ ] 将 stats、普通 slow allocation与 refill 的全 heap TLAB gate close 细化为按线程/按 span quiescence；ABI 73 不改变这一 TLAB observer 边界。
- [x] safepoint/thread unregister 在 ack/摘链前强制 flush 当前线程 RemSet buffer 并 retire TLAB；global/thread root registry 已落地，后续并发 root publication 仍需独立 epoch 协议。
- [x] 完成 TLAB overlap verifier：除 top 越界、陈旧 epoch、未 retire ack 和 reserved 尾部误入 free-list 外，Debug/Verify TLAB verifier 现在显式比较所有 active/retiring TLAB 区间并拒绝重叠 span。
- [x] 添加 region/thread orphan、top 越界和 lease epoch mismatch shadow 故障测试。
- [x] ABI minor 62 支持 Large Object Space v0：独立 `large_free_list`，small/large allocation 只搜索各自池；large block 拆分余量保留 large pool 分类，full rebuild 分池重建并按 Region generation/placement 边界限制合并，`nggc_check` 验证双链及统计守恒。
- [x] 增加 large pool telemetry 与回归测试：stats 暴露 large free block/bytes、large allocation/reuse 次数；测试证明 small allocation 不消耗 large free span、同尺寸 large allocation 原址复用，以及 small/large 混合连续堆尾仍可整体 decommit。
- [x] ABI minor 63 将 Large Object Space 演进为 Region 对齐的 humongous run：按 heap 实际 `region_size` 向上取整完整占用，Region 保存 START/CONTINUATION、run offset/length；回收后 metadata 保留，拆分只产生至少 2-Region 的可复用 run，tail decommit/reset 清零 metadata，verifier 可重建并拒绝 role/offset 损坏。
- [x] ABI minor 64 为 large free space 增加固定 32 档 logarithmic segregated bins：按 humongous run 的 Region 数分为 `2`、`3-4`、`5-8`、`9-16`……，用 32-bit nonempty mask 跳过空档；large allocation 从请求档向上搜索，split remainder 按新 run 长度重新分档，small allocator 仍不搜索 large pool。
- [x] ABI minor 64 增加 large-bin verifier、故障注入和 telemetry：完整 heap check 验证每个 FREE large header 的尺寸档、链表/位图一致性与总量守恒；stats 暴露档位容量、活跃档数、最大空闲 run Region 数以及 search/probe 次数；8 KiB 可配置 Region 测试覆盖多档并存、向上跳档、拆分重分档和单 Region 余量吸收。
- [x] ABI minor 65 支持非尾部空 humongous run 的整段 decommit/recommit：full collection 在 tail reclaim 之后，从 committed large-free bins 中选择当前策略下至少 8 个完整 Region 的页对齐冷 run，将整段页面归还 backend；`8 Regions` 是当前原型策略阈值，不是固定 API 合同。
- [x] ABI minor 65 为不可读的 interior run 增加 Region side metadata、独立 32 档 decommitted side bins 和统一 parser 伪块状态；decommitted 页内不保留权威 header，parser 和对象指针验证必须先查 side metadata，不能依赖 page fault、signal、SEH 或保护异常恢复。
- [x] ABI minor 65 的 large allocation 搜索顺序为 committed large-free bins、decommitted side bins、最后 bump；定址 recommit 可以只取请求前缀，至少 2-Region 的余量重写 START/CONTINUATION 并重新分档，单 Region 余量由请求吸收，movable-special 与 pinned-special placement 继续隔离。
- [x] ABI minor 65 增加 interior run/bin/Region/bytes/recommit telemetry、三源守恒 verifier、mask/continuation-offset 故障注入，以及 decommit/recommit backend 失败回滚、persistent pseudo-block 重扫、部分 recommit/余量复用和陈旧对象指针屏障拒绝测试。
- [x] ABI minor 66 将 interior decommit 最小 run 长度提升为 per-heap 配置：`interior_decommit_min_regions == 0` 使用默认 8，`NGGC_INTERIOR_DECOMMIT_DISABLED` 显式禁用，其他值至少为 2；stats 暴露最终生效值，测试覆盖默认、4-Region 自定义启用、阈值 5 保持 committed、禁用和非法阈值。
- [x] ABI minor 67 实现 tail-adjacent interior side-run 折叠：tail reclaim 把 high-water 收缩到 decommitted run 末端后，从 side bin 摘除连续 run、清除 placement/humongous metadata，并继续收缩到 run 起点直到固定点；页面保持 decommitted，Region/bytes 总量不重复记账。
- [x] ABI minor 67 增加 tail-collapse run/Region/bytes 累计统计和双相邻 run 回归；测试证明两个不同 bin 的连续 side run 都被折叠，完整 verifier 通过，随后 17-Region bump allocation 在原地址按普通 tail recommit 路径恢复空间。
- [x] ABI minor 69 将 committed/decommitted 两类 large bin 的档内链按 `(run bytes, address)` 严格排序，使从最小档开始的首个 size+placement 兼容节点成为确定性 best-fit；split/rebuild/decommit/recommit/collapse 全部保持顺序，verifier 与故障交换测试覆盖两类链。
- [x] ABI minor 69 将 `large_free_probe_count` 拆分为 committed/decommitted source，并增加 size-reject/placement-reject 计数；回归覆盖 5/8-Region 同档的 committed/decommitted 最小 run 选择，以及同尺寸 pinned 节点在 movable 请求前被跳过并准确计数。
- [ ] 根据 ABI minor 69 的 search/probe/source/reject telemetry 评估是否将档内 ordered list 深化为 tree 或按 placement 拆分的二级索引；当前已消除 first-fit 碎片选择，但查找和有序插入最坏仍为 O(k)，不应在没有数据时引入更重 metadata。
- [x] ABI minor 68 在 ABI minor 66 的显式阈值安全壳上增加 SMART v0：默认 `0` 模式以 committed Region 占比作为可移植 RSS proxy、large-free 比例作为回收压力，以 interior recommit 和 large allocation/reuse 增量识别热度；阈值在 2..64 Regions 内倍增/折半，并以两轮 full-collection cooldown 抑制抖动。
- [x] ABI minor 68 保证 MANUAL/DISABLED 优先于 SMART，并公开 policy/action/cooldown、basis-point 压力、最近 counter delta 与 evaluation/raise/lower/cooldown telemetry；回归覆盖 warmup、`8 -> 4` decommit、原址 recommit 后 `4 -> 8`、两轮保持和冷却后的再次 decommit。
- [x] ABI minor 70 为 SMART decommit 增加可选 memory-pressure callback：宿主可提供 RSS/limit 或直接 pressure basis points；有效样本替代 committed-footprint 主输入，callback 失败/非法样本在同一轮退回 ABI minor 68 的确定性 fallback，MANUAL/DISABLED 不调用。
- [x] ABI minor 70 为 Region decommit/recommit 增加可选单调时钟与每 Region 1/8 latency EWMA；无时钟时不使用 wall-clock fallback，时钟失败不阻断 VM 操作。只有显式慢操作阈值和本轮新样本共同成立时才提高 SMART threshold，回归与 verifier fault 覆盖该安全壳。
- [ ] 在真实 UHMAS backend 稳定后评估内建 Windows/Linux/BSD/macOS/嵌入式 pressure provider 与自动延迟阈值校准；当前 callback ABI 已允许宿主全平台接入，核心库继续保持无平台 RSS API 依赖。
- [ ] 根据 telemetry 评估可选低频 large compaction/emergency compaction；在收益不足时继续保持 non-moving humongous run 和确定 OOM/降级语义。
- [x] ABI minor 61 支持 pinned Region placement v0：SPECIAL Region 增加内部 sticky placement，free-list 和 bump allocation 不再混用 movable-large 与 pinned/non-movable 对象，tail decommit/recommit 会清空 placement。
- [x] ABI minor 79 完成第一阶段动态 Pin lease：`PINNABLE` 对象支持嵌套 `nggc_pin` / `nggc_unpin`，Pin 不构成 root；pinned Young 在 evacuation 中原地保留并继续增龄，unpin 后下一轮才允许搬迁/晋升；heap/Region verifier、统计和不可达 pinned 回收已接入。
- [ ] 完整 pinned region/space：增加独立 pin owner/token、超时/线程退出诊断、长期 pinned 空间分离整理、低碎片回收和并发 REMAP 闭包；ABI minor 79 的计数 lease 不覆盖这些能力。
- [x] 支持默认 64 KiB 非移动 region table、跨区对象字节分摊和 region heap-walk verifier。
- [x] ABI minor 53 增加 `nggc_config.region_size`：`0` 保持默认 64 KiB；Region 至少一个 512-byte card，必须满足 64-byte 对象对齐和 card 对齐，Region/card 数量与 metadata 分配需可表示，并验证 arena/TLAB/32-bit 边界。
- [x] Region 尺寸会驱动默认 TLAB 上限和 large-object threshold；覆盖小于默认 TLAB 的 Region、默认 Region、较大 Region，以及非法尺寸/尾部/溢出配置测试。
- [x] 增加 region lifecycle 与 live/free/large/pinned/non-movable 状态摘要，并预留 generation/collection-set 字段。
- [x] 普通 bump allocation 不跨 region，区尾发布为可复用 padding free block；ABI minor 63 后大对象只从 Region 边界开始并占用完整 humongous run。
- [x] 增加 per-region allocation high-water cursor 和单 allocator owner token。
- [x] 增加可撤销 heap allocator lease epoch，并接入 safepoint/phase owner 撤销与无 lease 持锁 bump fallback。
- [x] ABI minor 60 增加 tail Region decommit/recommit 状态：decommitted Region 不参与 allocator owner 发布，完整 heap verifier 校验 decommit 计数/字节数，arena close 会按仍 committed 的 Region 分段 unmap。
- [x] 分离对象逻辑 generation 与 Region 物理 placement：所有新对象均为 `YOUNG, age=0`；标准/TLAB 进入 YOUNG，large/pinned/non-moving 进入 SPECIAL，只有达到阈值后才允许逻辑 OLD，free-list 只复用兼容 placement。
- [x] 启用 collection-set 与非移动 young collection v0：young-only sweep，成功/abort 清理 collection-set。
- [x] 将 young mark 收紧为 roots + dirty collection-set 外 owner card + young closure；集合外对象 implicit-live，OLD 统计按对象逻辑代别计算，Debug/Verify 回收前重建 RemSet coverage。
- [x] 为 young 对象增加独立、饱和且不回绕的 age metadata；未达到阈值时绝不改变代别，正常 moving/SPECIAL 路径达到阈值才晋升 OLD，非移动降级可让对象以 YOUNG tenuring candidate 身份延迟晋升。
- [x] 实现 STW young evacuation、Survivor copy、达到 tenuring threshold 后的对象级晋升、strong-slot remap 和 capacity preflight fallback。
- [x] 实现 forwarding 提交点和确定 emergency completion：发布前故障无损 abort，发布后在同一 safepoint 内幂等补完 copy、REMAP、source release 和 free-list rebuild，并保留原始错误返回。
- [x] 将 header forwarding 升级为 C11 单指针原子协议：目标对象完整发布后执行 release-CAS，读屏障/REMAP 使用 acquire load；对象头携带 32 位 evacuation epoch，不依赖双字 CAS 或 64 位 lock-free atomic；CAS loser/error 会保留 observed forwarding 的提交分类，串行未发布 copy 立即回收。
- [x] 实现 STW 窗口内并行 evacuation copy：精确任务表、全部目标预留、最多四个 C11 worker、无 threads/创建失败同步回退、任务重试、CAS loser 回收队列、worker epoch/quiescence 和 from-space reclaim gate。对象目标代别只由逐对象年龄阈值决定。
- [x] 将每轮创建/join 的 evacuation worker 演进为每 heap 按需创建的持久 worker pool：最多三个后台线程通过 condition variable 等待带 epoch 的 job，跨 collection 复用，heap close 广播停机并统一 join；无 C11 threads 或创建失败时 coordinator 同步接管。
- [x] ABI minor 88 完成持久 GC worker pool 的 condition wait/broadcast/join 故障闭环：coordinator wait 失败仍等待已派发 worker quiesce，dispatch/close broadcast 失败保留可重试 pool，join 失败保留 live heap handle；失败后的 pool/job/context/counter 状态由 verifier 和独立回归覆盖。
- [x] ABI minor 88 补齐 worker 显式失败退出：per-slot 原子失败状态与 dispatch-epoch 结束记账覆盖派发前、执行前、执行后和完成记账后的退出；wait 每次返回都核对失败，broken pool 停止 copy/REMAP 派发，实际参与者归静后保留错误与安全完成语义；不启动移锁。
- [x] 将 evacuation pool 泛化为共享 GC worker pool，并在 parked safepoint 内并行 REMAP heap block；roots 串行修正，heap block 由原子 cursor CAS 唯一领取，RemSet/card finalize 在 worker 归静后串行完成，copy/REMAP 各自维护 epoch/quiescence/reclaim gate 与统计。
- [x] 实现第一阶段 barrier-mediated mutator-concurrent evacuation copy window：EVACUATE worker 复制期间临时释放 heap lock，payload lock 保护 source，`nggc_load/store` 在读屏障窗口内可安全协作；注册 mutator 仍保持 parked，故不等同于完整并发搬迁。
- [x] ABI minor 55 增加 registered-mutator EVACUATE 状态机骨架：`AT_SAFEPOINT -> CONCURRENT_RUNNING -> CONCURRENT_REPARKING -> AT_SAFEPOINT`，只在 copy window 放行 `nggc_load/store`，并以 parked/running/repark 计数证明重新停车后才能进入 REMAP；无注册线程时保留纯 barrier window。
- [x] ABI minor 56 增加 poll/park 驱动的协作 repark handshake：resume epoch 解停已 ack 的 registered mutator，copy worker quiesce 后 collector 请求 repark，mutator 通过 `nggc_safepoint` / `nggc_safepoint_park` 回到 `AT_SAFEPOINT`。
- [x] ABI minor 88 修复 repark 重复消费 resume epoch：`nggc_safepoint_park` 在持 heap lock 的状态快照中区分首次停车与同 epoch 重新停车，后者只等待 final release，避免 heap-lock 忙循环；保留 resume publication，防止尚未醒来的首次 waiter 漏掉放行通知。
- [ ] 实现完整 registered-mutator 并发搬迁/REMAP 协议：root/handle/barrier-buffer 闭包、分配/TLAB 协议、并发 RemSet 修正，以及失败时的 fail-stop/quarantine 降级。
- [x] 实现跨 safepoint delayed from-space reclaim、quarantine/reclaim epoch 和读屏障关闭证明：moving SWEEP 将 forwarding source 转为 `FORWARDED` 隔离壳，`IDLE` 保持读屏障；只有后续不同 safepoint 全员 parked，或无线程时下一 collection 的空集证明，才物理转为 FREE。
- [x] ABI minor 80 为 post-forwarding fail-stop 增加可诊断的 heap state/reason 查询与 `nggc_force_destroy` 最终销毁 API；失败时保留 safepoint/collector state，不释放 mutator，force-destroy 要求宿主先停止所有访问者且不提供恢复语义。并修复 C11 TSS 旧 heap 条目在地址复用时的实例隔离与当前线程清理。
- [x] 实现 young evacuation 目标空间精确 preflight；按 Survivor/OLD 分组规划、跳过 from-space，在 forwarding 发布前确定容量不足并降级非移动 sweep。
- [x] ABI minor 86 增加 `NGGC_CONFIG_YOUNG_IN_PLACE_PROMOTION` 可选整区原地晋升：仅在一个 Young Region 的全部标记存活对象逐对象达到 tenuring threshold 且无 pin/large/non-movable 时跳过复制；默认关闭，混合 collection 仍保留普通 Region evacuation，失败/未达龄保持原有降级语义。
- [x] 为内部 weak/soft/phantom runtime object 实现专用 evacuation/remap：weak/soft referent、phantom queue 的 head/tail、phantom reference 的 referent/queue/next 均在 REMAP 中更新；reaper 观察项按 `(mark_epoch, object_id)` 校验并在失效或地址复用时丢弃。`NONE + INTERNAL` forwarded block 由统一 parser 接受，普通 `NONE` 仍拒绝。

## 6. 验证与测试

- [x] 添加基础对象分配测试。
- [x] 添加扫描函数引用验证。
- [x] 添加 scanner owner payload 边界、slot 对齐和特殊引用 referent 越界枚举验证。
- [x] 添加 Debug/Verify scanner replay，覆盖顺序无关的稳定集合、重复 slot、不稳定集合和 scanner payload 修改。
- [x] 添加 root/thread registry verifier，覆盖 root storage/count/对齐/重复项、thread 链表/节点/计数，以及按 observed epoch 重建 pending safepoint。
- [x] 添加仅测试目标可见的 registry 故障注入 hook 和正反向测试。
- [x] 添加 reference-processing verifier，覆盖 soft/weak mark 结果、marked phantom queue 保活、入队 referent 清空、queue 归属/唯一成员/计数和 mark epoch。
- [x] 修复 marked PhantomReference 关联的空 PhantomQueue 未被保活、可能在 sweep 后留下悬空 queue 指针的问题。
- [x] 添加 soft -> weak -> phantom 顺序测试和 10 类 reference-processing 故障注入测试。
- [x] 添加 phase transition verifier，覆盖 phase/read-barrier 矩阵、transition parity、active safepoint 排斥、collector expected phase/epoch 和 auto-collect safepoint epoch。
- [x] 为 collect 与 assist reclaim 增加严格 `ROOT_SNAPSHOT -> MARK -> REFERENCE_PROCESS -> SWEEP` collector cycle，并实现失败 abort。
- [x] 添加 standalone phase、非法 collector 顺序、cycle abort 和 13 类 phase 状态故障测试。
- [x] 添加 moving collector 五点故障矩阵：首次 forwarding 前、部分 copy、全部 forwarding 后、部分 REMAP、部分 source release；验证 auto-safepoint release、对象图、年龄晋升、RemSet 和后续 collection。
- [x] 添加 forwarding CAS winner/same-epoch loser/wrong-epoch loser 测试、epoch 缺失/残留校验、IDLE live-forwarding 拒绝和真实 moving collection CAS/epoch 统计断言。
- [x] 添加 evacuation worker 正反向测试：64 个独立任务、同 heap 连续两轮复用持久线程、重复 source 任务的一胜一负与 loser 回收、任务重试同步接管、一次性 `thrd_create` 返回失败后的串行回退与下一轮恢复建池、worker/reclaim gate 清零，并验证对象只在第二次存活达到阈值后晋升 OLD。
- [x] ABI minor 88 添加 worker pool 故障矩阵：coordinator `cnd_wait` 失败、dispatch/close `cnd_broadcast` 失败、`thrd_join` 失败后二次 close，以及失败期间 heap、pool、job/context 和 worker counter 的 quiescence/重试验证。
- [x] ABI minor 88 增加七个 worker 失败窗口：idle wait，以及 copy/REMAP 各自的执行前、执行后、完成记账后失败；验证根与对象环修正、payload 保留、broken pool 后续同步接管、非零 worker 退出结果和二次 close 不重复 join。专项 CTest 设置 30 秒超时，防止协议回归悬挂。
- [ ] 为未显式报告的 worker 卡死/外部线程终止设计 watchdog、宿主诊断和 fail-stop 协议；损坏 mutex 导致无法重新获得 pool lock 时仍不具备通用可移植恢复能力。
- [x] 添加受限 mutator-concurrent copy window 正向测试：在 EVACUATE worker 启动前稳定打开窗口，验证 `nggc_load/store` 通过读/写屏障进入、统计递增、窗口结束后 phase/epoch/quiescence 清零。
- [x] 添加已注册 mutator 的 copy window 状态测试：验证 `parked -> running -> reparked` 计数转换、窗口内 safepoint poll 门禁和窗口结束后的 quiescence。
- [x] 添加四个原生 owner mutator 真正解停后的并发 copy 压力：24 KiB 非 2 次幂 Region、thread roots、source payload 锁竞争、forwarding 发布后的写入/读回、每轮 1024 次压力写、Pin 固定地址/跨区边、三轮年龄晋升和最终 parked parallel REMAP；确定性闸门证明 repark 线程实际进入 condition wait，测试 hook 在正式 Release 宏移除。
- [x] ABI minor 88 补齐 canonical-owner 读屏障：active barrier 中先定位 owner/field offset，source lock 取得后重新解析 forwarding，再读取 canonical payload，覆盖 canonical-only null/替换写入、load/store 与 copy worker 的确定性竞争、隔离期读取和大对象 continuation slot；非法 header/padding/未对齐 slot 在读取前拒绝，load 测试闸门在正式 Release 预处理移除。
- [x] ABI minor 88 补齐原子诊断与逐调用错误回传：`last_error` 使用 C11 `atomic_int` relaxed store/load；Weak/Soft/PhantomQueue/PhantomReference 创建 API 返回局部 helper result，失败输出为空，保留私有 pointer allocator。确定性暂停覆盖错误被其他线程替换的窗口，三线程压力验证诊断读写及 heap 守恒；仅恢复共享诊断回传的 mutation 实际失败，暂停 hook 在正式 Release 预处理移除，不移动既有锁。
- [ ] 为 active read-barrier 的 payload owner 查询建立对象起点索引或其他有界表示；当前复用线性 heap block walk，最坏 O(block count)，暂不宣称最终低延迟路径。普通 free block 可跨同代/同 placement 的 Region 合并，不能假设 Region.start 永远是 block 起点。
- [ ] 添加 mutator-concurrent REMAP 压力和 payload race 检查；现有专项只验证并发 copy 与全员 repark 后的并行 REMAP，不代表 REMAP 期间允许 mutator 运行，也未在本机运行 TSAN。
- [x] 添加 RemSet coarse-heap overflow fallback、独立 rebuild coverage verifier 和故障注入测试。
- [x] 将 rebuild verifier 收紧为 cross-region edge card coverage，并单独统计/验证 old-to-young 子集和漏卡故障。
- [x] ABI minor 73 增加 per-thread pending-card coverage：确定性覆盖 buffer 容量 1、同 card 去重、overflow 只刷当前线程、safepoint flush 后触发 coarse、buffer/global union rebuild 和非法 card index；C11 多线程覆盖 unregister 后 pending 归零。
- [x] ABI minor 74 增加 barrier-context 路由覆盖：四个 worker 的全部 store 命中各自 TSS；同 native thread/same heap 双句柄不随意选 owner；注销一个句柄后恢复唯一；同线程双 heap 相互隔离；无 owner 走 direct；TSS update fault 后注册和 buffered publication 保持正确。
- [x] collection-set 启用后增加 collection-set 外指向集合内的边覆盖对比：SPECIAL/pinned owner 的 dirty card 指向普通 Young target 时，young closure 必须保留并搬迁 target，同时未覆盖 Young garbage 被回收。
- [x] 添加跨 region 大对象、回收和 free-block 拆分复用测试。
- [x] 添加普通对象 region 边界、尾部 padding 发布和拆分复用测试。
- [x] 添加 region cursor/owner/lease verifier 故障注入与恢复测试。
- [x] 添加 stats 验证。
- [x] 添加 mark-sweep 回收测试。
- [x] 添加 free block 复用测试。
- [x] 添加固定 type id 重复注册测试。
- [x] 添加 OOM 测试。
- [x] 添加读/写屏障 smoke test。
- [x] 添加 card table / remset 统计测试。
- [x] 添加线程注册和 safepoint smoke test。
- [x] 添加 safepoint pending/ack/wait 测试。
- [x] 添加 GC phase begin/end smoke test。
- [x] 添加 collector-driven phase transition 测试。
- [x] 添加 reference-processing phase skeleton 测试。
- [x] 添加 phase-gated read barrier smoke test。
- [x] 添加 phase-gated read barrier header forwarding 测试。
- [x] 添加 REMAP phase strong slot fixup、forwarding 目标校验和残留 forwarded root/strong slot verifier 测试。
- [x] 添加类型注册边界测试。
- [x] 添加损坏对象头测试。
- [x] 添加多线程 smoke test。
- [x] 添加 UHMAS-HL backend 失败路径测试。
- [x] 添加 Mutator Assist debt 统计测试。
- [x] 添加 opt-in Mutator Assist reclaim 分配路径测试。
- [x] 添加 bounded Mutator Assist 专用测试：跨 packet backlog、MARK 写屏障、partial SWEEP 分配保活和 full collect 接管。
- [x] 添加 ABI minor 81 Mutator Assist 回归：per-thread debt、actor 优先偿还、动态 8/16/24/32 item 与 1/2/4/8 KiB budget、公平性上限、无进展退避统计和双线程压力。
- [x] 添加 ABI minor 82 回收触发器回归：SMART 自动触发、MANUAL no-op、非法策略/soft limit、LATENCY pause sample/EWMA，以及 clock failure 不阻断 collection。
- [x] 添加 `GC_TYPE_NONE` 内部对象策略测试。
- [x] 添加内部 WeakReference discover/clear 测试。
- [x] 添加公开 WeakReference API 测试。
- [x] 添加内部 SoftReference retain/clear 测试。
- [x] 添加公开 SoftReference API 和 soft policy 测试。
- [x] 添加内部 PhantomReference enqueue 和 PhantomQueue retention/drain 测试。
- [x] 添加公开 PhantomReference/PhantomQueue API 和手动 drain 测试。
- [x] 添加 PhantomQueue reaper 弱观察/轮询测试。
- [x] 添加 PhantomQueue reaper 有界等待测试。
- [x] Region 配置回归不只覆盖默认 64 KiB：现覆盖 512 B、8 KiB、24 KiB、128 KiB 和 1 MiB，并以 4 MiB arena 验证较大 Region 的普通/large threshold、humongous run、card 数和统计。

## 7. Collector

- [x] 添加 Phase 0 verifier-only `nggc_collect`，并已替换为非移动 mark-sweep。
- [x] 添加显式 root stack。
- [x] 添加 mark bit / mark epoch。
- [x] 实现非移动 mark-sweep。
- [x] 实现 reference-processing phase skeleton。
- [x] 实现内部 WeakReference 对象和 weak discover/clear 语义。
- [x] 设计公开 WeakReference API v0。
- [x] 实现内部 SoftReference 对象和 heap-pressure v0 soft policy 语义。
- [x] 设计公开 SoftReference API v0 和可配置 soft policy。
- [x] 实现内部 PhantomReference enqueue 语义。
- [x] 实现内部 PhantomQueue special object 和非空自保活语义。
- [x] 设计公开 PhantomReference/PhantomQueue API 和手动 drain v0。
- [x] 设计 PhantomQueue reaper 弱观察/轮询协议 v0。
- [x] 实现 PhantomQueue reaper 有界等待接口。
- [x] ABI minor 87 实现平台级阻塞唤醒接口 `nggc_phantom_reaper_wait_blocking`：等待只消费通知，不自动 collection 或替调用者 pop；C11 condition variable 使用事件 epoch 防丢唤醒，观察集清空返回 `NGGC_ERR_BUSY`，无 `<threads.h>` 保留可移植原子等待降级。
- [ ] 可选实现真正后台 PhantomQueue reaper worker；需要先定义用户回调/消费所有权、heap close join 和取消协议，当前不启动后台线程。
- [x] 实现 Mutator Assist 计债接口。
- [x] 将 Mutator Assist 计债接口接到 opt-in allocation-failure reclaim 原型。
- [x] 将 Mutator Assist 扩展为持久化 bounded mark/sweep work packets。
- [x] ABI minor 81 增加 per-thread assist debt、actor 优先偿还、公平性/退避和动态预算；当前仍是 allocation-failure 驱动的 bounded mark/sweep packet，reference processing packet 化已由 ABI minor 85 在现有 heap lock 内完成。
- [x] ABI minor 82 完成可解释回收触发器 v0：SMART/heap-limit/allocation-rate/latency-target/throughput/memory-saving/manual profile、显式自动触发开关、soft limit/quantum、action/reason/pending telemetry、分配慢路径接入和 registered-mutator advisory no-op 边界。
- [x] ABI minor 82 完成 collection pause latency feedback：单调 clock callback 的成功/abort/fail-stop 采样、1/8 EWMA、clock failure 降级、stats/check 和确定性测试；仍未实现 p95/p99 预测或后台 coordinator。
- [x] ABI minor 83 完成 collector-owned weak/soft/phantom discovered-list：独立 epoch、ACTIVE/DRAINING/IDLE 状态、intrusive list、软引用闭包 drain、已入队 PhantomReference 排除、abort/assist/next-epoch 清理、stats/check 和嵌套发现回归测试；在 ABI minor 85 之前 reference processing 仍保持原子化。
- [x] ABI minor 85 在 discovered-list 协议之上实现 bounded reference-processing packets：持久化 mark stack、Soft/Weak/Phantom 分阶段游标、PhantomQueue 成员游标、arena 扫描游标、Assist 接管和失败清理协议；当前仍在现有 heap lock 内，不开启并发 reference processing。
- [x] ABI minor 85 增加 partial reference-processing takeover 回归：在 Assist 停留于 `REFERENCE_PROCESS` 时由完整 collector 清理旧 cursor/discovered-list 并从新 mark epoch 重启，验证引用发现结果、`nggc_check` 中间态和最终状态守恒；不推进移锁或并发 reference processing。
- [x] ABI minor 86 增加整区原地晋升回归：阈值前对象仍复制并保持 Young，阈值后仅对全体存活对象均合格的 Region 原址晋升为 OLD，验证年龄、地址、统计和 heap verifier 守恒。
- [x] ABI minor 87 增加 PhantomQueue reaper 阻塞等待：以 heap-lock 保护的事件 epoch 配合现有 C11 condition variable，覆盖 enqueue、pop、watch/unwatch、collection 完成和 stale weak-watch 唤醒；无 `<threads.h>` 构建保留原子等待降级，不创建后台 reaper worker。
- [ ] 支持内存压缩。
- [ ] 新生代左右半区复制移动（类似Java的机制，或者实现更好的方法处理）。
- [ ] 暴露Header方便外部定义GC对象。
- [ ] 实现并发标记、并发清理、并发压缩。
- [x] 常驻写屏障，并在 phase/quarantine/受限复制窗口按需介入读屏障。
- [x] OLD 只能在对象年龄达到 `young_tenuring_threshold` 后晋升；任何分配路径都不得让新对象出生即 OLD。
- [x] Pin 机制测试：nested lease、busy phase/safepoint、unpinned/pinned Region flag、pinned Young 原地增龄、unpin 后复制晋升、不可达对象回收和统计守恒。
- [x] ABI minor 84 增加 Pin lease 归属账本：按 `(object, object_id, owner)` 聚合嵌套 lease，跨 owner unpin 拒绝，持 lease 的注册线程不能注销；`nggc_check`/stats 验证逐对象与全局守恒。仍不推进移锁、lock-free barrier、并发 REMAP 或 token/超时 API。

## 8. 屏障与并发

- [x] 设计 `NGGC_LOAD` / `NGGC_STORE` API 形状。
- [x] 添加常驻写屏障的 no-op/recording 骨架。
- [x] 添加读屏障入口的 no-op/validation 骨架。
- [x] 将 `NGGC_STORE` 接到最小 card table / remembered-set 记录。
- [x] 添加 phase-gated read barrier 骨架。
- [x] 给 phase-gated read barrier 接入 header forwarding 逻辑。
- [x] 给 phase-gated read barrier 补齐最小 strong slot fixup/remap pass、forwarding 目标校验和残留 forwarded root/strong slot verifier。
- [x] 实现 STW young evacuation 的 from-space verifier：成功 sweep 后 collection-set 内不得残留 live object，forwarding target 身份与布局必须匹配。
- [x] 扩展 collector expected-phase 状态机，区分 non-moving 与 young-evacuation cycle kind，并验证完整 moving phase 序列。
- [x] 将单 heap card table 包装为可降级 RemSet v0，支持 precise-card/coarse-heap 模式。
- [ ] 加入 region table 后演进为 region/young-old remembered set。
- [x] 添加线程注册。
- [x] 添加协作式 safepoint epoch/poll 骨架。
- [x] 添加 safepoint 等待/确认 handshake 状态机。
- [x] 将 ack 升级为 held safepoint：线程保持 parked，增加显式 release、pending/parked 守恒和 Release 门禁。
- [x] auto-collect 在成功/abort 路径释放 parked 线程，并暴露 active/parked/release stats。
- [x] 添加 partial release、重复 release、parked root/unregister 门禁、parked-count/active-state 故障测试。
- [x] 增加 `nggc_safepoint_park` 阻塞式 poll/wakeup：C11 condition variable 主路径、原子 release-epoch fallback，mutator 无需外部等待 flag。
- [x] 增加 `nggc_thread_collect` collector-owned mutator 模式，避免同步 collection 发起线程等待自身，并拒绝其他 collector 接管。
- [x] ABI minor 56：registered mutator 在 EVACUATE copy window 通过 resume epoch 解停，只允许 `nggc_load/store` 进入；copy worker quiesce 后由 collector 请求 repark，mutator 通过 `nggc_safepoint` / `nggc_safepoint_park` 协作回到 `AT_SAFEPOINT`。
- [ ] 将 ABI 56 的 copy-only window 扩展到 mutator-concurrent REMAP、root/handle/barrier-buffer 闭包和 RemSet flush；ABI minor 80 的 fail-stop 查询/销毁管理面已落地，但完整恢复、并发 REMAP 和 root/handle 闭包仍未完成。
- [x] 为 collector cycle begin、ROOT_SNAPSHOT OOM、reference processing、sweep abort 和 post-forwarding emergency completion 增加 auto-safepoint release 故障注入矩阵。
- [x] 冻结 scanner callback 对不同 owner 的并发可重入契约：只按类型/布局同步枚举 owner payload 内 strong slot，不依赖 referent 值，不缓存 visitor/context，不阻塞、不重入 GC API。
- [x] ABI minor 88 为 scanner 增加显式错误返回/协作取消协议，并补 worker pool condition wait/broadcast、`thrd_join` 失败的 release/close 故障矩阵；当前已覆盖一次性 `thrd_create` 返回失败注入、失败后的下一轮恢复建池、任务级重试和 coordinator 同步接管。
- [x] ABI minor 88 增加 Verify-only scanner 同线程 API 重入拒绝：C11 TLS 隔离不同 worker/heap 的回调，全部 54 个公共 API 在触碰参数、快路径或锁之前返回 `BUSY`/空值，外层扫描锁存 `CORRUPTION`，错误 poll context 在解引用前拒绝。覆盖 legacy/ex 全 API、跨 heap、MARK abort、后台 REMAP 应急完成与下一轮复用、两 heap 真正重叠扫描；正式 Release 预处理移除全部 guard/TLS/test hook。
- [ ] 为 scanner 增加超时/阻塞、跨线程滥用和缓存 visitor/context 的诊断，并补 worker 未报告的中途终止/阻塞、无 condition variable 平台原生 wait primitive 等更深故障模型；ABI 88 的显式失败退出已覆盖执行前/后与完成记账后的故障。
- [x] 添加 GC phase coordinator 骨架。
- [x] 将 GC phase coordinator 接入 `nggc_collect`。
- [x] 给 `nggc_collect` 增加可选自动 safepoint 请求/等待策略。

## 9. Git 维护

- [x] 建立初始提交。
- [ ] 每个稳定阶段提交一次。

## 10. 公开发布准备

- [x] 增加根 README 与 `draft/` 的项目摘要、验证记录、首次发布说明和匿名发布指南。
- [x] 增加发布文件/个人身份/常见凭据扫描及独立匿名快照工具，排除旧 `.git`、build/out、本地 IDE 状态和二进制产物；保留本地开发历史。
- [ ] 从匿名候选创建并推送远程仓库；后续提交继续保持公开身份策略。
- [ ] 确定许可证并核对 UHMAS-HL 的授权范围；当前仅公开代码，暂不添加许可证。
