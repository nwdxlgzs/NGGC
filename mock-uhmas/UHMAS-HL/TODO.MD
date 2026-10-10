# UHMAS-HL TODO

## 0. 已锁定决策

- [x] 项目名使用 UHMAS-HL，HL 表示 HeapLock。
- [x] RAW_PLAN.MD 与 RAW_TODO.MD 保留为原 UHMAS 历史材料，不再作为当前项目待办。
- [x] 公共接口尽量保持 UHMAS 形状，内部实现按 HL 简化。
- [x] 并发模型收敛为 heap 大锁。
- [x] malloc/mmap 相遇或无法继续 carve 时视为内存耗尽。
- [x] 不继续推进 lock-free remote free、分片锁、读者 hazard gate 和复杂诊断矩阵。

## 1. 文档

- [x] 编写 UHMAS-HL 专用 PLAN.MD。
- [x] 编写 UHMAS-HL 专用 TODO.MD。
- [x] 后续代码删改完成后，补一份 README 或使用说明，说明 HL 与原 UHMAS 的差异。

## 2. ABI 与公共头文件

- [x] 保留 bootstrap 符号和 `uhm_api` 函数表。
- [x] 保留 `uhm_config`、`uhm_stats` 中已有 lockless 字段以兼容旧调用方。
- [x] 将 README/PLAN 说明补齐：lockless remote 字段在 HL 中为兼容字段，永远报告不支持。
- [x] 保留当前 ABI minor，表示 UHMAS-HL 仍兼容 UHMAS 公共接口前缀；不为内部弱化另开 ABI 形状。

## 3. HeapLock 实现收缩

- [x] 将 shard 数量收敛为 1，停止把多 shard 当作性能目标。
- [x] 将 lockless remote primitive/config capability 降级为不支持。
- [x] `UHM_CONFIG_LOCKLESS_REMOTE_FREE` 可被旧调用方传入，但运行时清除该能力。
- [x] 删除 lockless remote reader、hazard、tag-wrap gate 等遗留结构。
- [x] 删除 slab raw-lock/fallback 中为锁外发布服务的状态字段。
- [x] 删除 treap health repair 相关运行时状态、统计和测试；公共 flag 仅作兼容输入。

## 4. 碰撞与元数据

- [x] 控制元数据只能按低水位申请，碰撞即失败，不做额外抢救。
- [x] 低水位 malloc claim 和高水位 mmap claim 碰撞时返回内存耗尽。
- [x] 审计 mremap grow、FIXED mmap、rollback 路径，确保没有重新引入碰撞渗透。
- [x] 删除原 collision-control 白盒测试里的多 shard、control extent pressure 场景。

## 5. 源码组织

- [x] 将 `src/*.inc` 实现合并回 `src/uhm.c`。
- [x] CMake 库目标只编译 `src/uhm.c`。
- [x] 删除不再引用的 `src/*.inc` 零散实现文件。
- [x] 删除测试 include 拆分，保留一个小型独立 HL 接口测试 `.c` 文件。

## 6. 测试与验证

- [x] 配置并构建默认开发目标。
- [x] 运行基础测试。
- [x] 修正因 HL 简化导致的旧 UHMAS 白盒测试失败。
- [x] 最终保留一组小而稳定的 HL 验证门槛：ABI、platform VM、malloc/free、mmap/munmap/mprotect/mremap、stats/check、close。
- [x] 保留 UHMAS quick-compatible Release 速度测试：small malloc/free、多线程 small、large realloc/free、mmap/munmap、fixed mmap、mprotect、collision pressure 和 mixed release pressure，并保留系统 malloc/VM 对照。
