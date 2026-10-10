# UHMAS-HL

UHMAS-HL 是 UHMAS 的 HeapLock 弱化版。它尽量保留 UHMAS 的公共 C ABI、bootstrap 符号、`uhm_api` 函数表和平台 VM 回调模型，但内部实现不再追求原项目的高并发拆锁路线。

HL 指 HeapLock。当前实现把堆内元数据并发统一收口到一把 heap 大锁；`malloc` 从 arena 低端增长，`mmap` 从高端增长。两者相遇、固定映射无法放入、或元数据空间无法继续申请时，直接返回内存耗尽或对应错误，不再做碰撞渗透、复杂仲裁或回滚抢救。

## 与原 UHMAS 的差异

- 保留公共头文件和接口形状：`uhm_abi_query`、`uhm_heap_storage_query`、`uhm_open`、`uhm_get_api`、`uhm_api`。
- 保留 `uhm_heap` / `uhm_thread_ctx` opaque 类型，但 `thread_ctx` 只是兼容包装，不再承载 TLS cache、deferred queue 或 hazard reader。
- 保留 lockless remote 相关 ABI 字段和配置名字，但 HL 永远报告不支持，运行时也不会启用 lock-free remote free。
- 保留基础 stats、`uhm_check`、leak enumeration 和平台默认 heap 包装。
- 删除原 UHMAS 的 shard lock、reader gate、tagged freelist、treap health、benchmark gate、diagnostic threshold 和大规模白盒压力矩阵。
- 源码实现合并为单个 `src/uhm.c` 翻译单元。

## 构建

```powershell
cmake -S . -B build -DUHMAS_BUILD_PROFILE=dev
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure

cmake -S . -B build-release -DUHMAS_BUILD_PROFILE=release
cmake --build build-release --config Release --parallel
```

`dev` profile 默认构建小型 HL 测试；`release` profile 默认只构建库目标。

## 性能测试

HL 保留一套速度测试，不恢复原 UHMAS 的诊断矩阵和回归 gate，但默认输出对齐 UHMAS quick benchmark 的表格形状，方便横向比对同名 case：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_benchmarks.ps1
```

也可以手动构建并运行：

```powershell
cmake -S . -B build-release -DUHMAS_BUILD_PROFILE=release -DUHMAS_BUILD_BENCHMARKS=ON
cmake --build build-release --config Release --parallel
build-release\Release\uhmas_hl_benchmarks.exe
```

默认 quick 参数为 96MiB arena、24000 次 small、4 x 7000 次 multithread、900 次 large、500 次 mmap、250 次 fixed/mprotect、9000 次 mixed release。输出包含：

- `uhm small malloc/free`
- `system malloc small`
- `uhm small malloc/free multithread`
- `system malloc multithread`
- `uhm large malloc/realloc/free`
- `system malloc large/realloc/free`
- `uhm mmap/munmap large`
- `system mmap/VirtualAlloc large`
- `uhm mmap fixed/auto mix`
- `uhm mprotect split/merge cost`
- `uhm malloc/mmap collision pressure`
- `uhm release mixed long pressure`

可调参数包括 `--arena-mib`、`--small-iters`、`--thread-iters`、`--large-iters`、`--mmap-iters`、`--fixed-iters`、`--release-iters` 和 `--stress`。脚本侧仍接受旧参数名 `-MallocIterations`、`-VmIterations`、`-ProtectIterations` 和 `-MremapIterations`，其中 mremap 参数仅保留兼容，不再作为 HL quick case 输出。

注意：HL 的 UHM case 在 benchmark 内使用独立 heap 运行，计时不包含 heap open/close。这样避免 `collision pressure` 在简化水位模型下污染后续 case，同时保留 UHMAS quick 的 case 名、迭代规模和系统 malloc/VM 对照。

## 当前文件结构

- `include/uhm/uhm.h`：公共 ABI。
- `include/uhm/uhm_platform.h`：默认平台 VM 包装。
- `src/uhm.c`：HeapLock 实现。
- `tests/test_uhm_hl.c`：小型接口级验证。
- `tests/bench_uhm_hl.c`：UHMAS quick-compatible 速度测试。
- `scripts/run_benchmarks.ps1`：Release benchmark 包装脚本。
- `PLAN.MD` / `TODO.MD`：UHMAS-HL 当前计划和待办。
