# NextGenerationGarbageCollector

NGGC 是一个以 C11 为核心、面向 32/64 位平台的精确追踪垃圾回收器原型。目前公共 ABI 为 **0.88**，内存操作底座使用仓库内的 `mock-uhmas/UHMAS-HL`。

长期目标是分 Region、分代、支持并发搬迁的低延迟 GC。当前已经实现非移动 mark-sweep、年轻代 evacuation 和受限的 mutator 并发复制窗口；完整并发标记、并发 REMAP 和并发压缩仍在开发。API 与内部布局尚未冻结。

## 当前能力

- 用户注册精确扫描函数，显式 global/thread roots，协作式 safepoint。
- 用户类型 ID 为 `1..65535`；`0` 是 GC 内部 `NGGC_TYPE_NONE`。
- 可配置 Region 尺寸，年龄达到阈值后才晋升 OLD；大对象的物理放置与逻辑代别分开。
- 常驻写屏障，搬迁或隔离窗口按需开启读屏障，header forwarding 与 canonical-owner 字段读取。
- Weak/Soft/PhantomReference、专用 PhantomQueue 与 reaper 观察/等待接口；不提供 finalizer。
- 嵌套 Pin lease、owner 归属检查及 pinned Young 的原地保留。**Pin 固定地址，不构成 root。**
- per-thread TLAB、可降级 RemSet/card buffer、有预算的 Mutator Assist。
- SMART 等可解释触发策略、OOM 降级、worker 故障接管及 post-forwarding fail-stop 管理。
- Debug verifier、独立故障注入测试，以及由宏控制的诊断入口。

## 构建与测试

需要 CMake 3.20+ 和支持 C11 原子的 C 编译器。MSVC 当前使用 `/experimental:c11atomics`；C11 `<threads.h>` 不可用时，相关路径采用同步或原子等待 fallback。

```sh
cmake -S . -B build
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

单配置生成器需在 configure 时加 `-DCMAKE_BUILD_TYPE=Debug`。

正式 Release 库可关闭测试目标：

```sh
cmake -S . -B build-release -DNGGC_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --config Release --parallel
```

默认 Release 通过预处理移除可选高成本诊断。独立测试目标会单独启用 Verify/Test Hooks；正式库仍保留必要的协议检查。ASAN/TSAN 宏开关不等同于编译器 sanitizer instrumentation 或实际完成 sanitizer 验证。

## 验证范围

2026-10-10：MSVC x64/x86 Debug/Release 四组完整 CTest 各 **23/23**，MinGW x64 Debug（无 `<threads.h>`）**19/19**。这是当前 Windows 环境的验证记录；Linux、macOS、其他架构与 TSAN 实测尚未完成。

## 文档

- [发布材料索引](draft/README.MD)：能力摘要、验证记录与匿名发布步骤。
- [长期架构](GC-ARCH-PLAN.MD)：协议、设计决策和演进过程。
- [工程计划](PLAN.MD) / [待办](TODO.MD)：当前实现与后续工作。
- [内存底座合同](BACKEND-CONTRACT.MD)：后续接入真实 UHMAS 的替换边界。
- [公共头](include/nggc/nggc.h)：当前 API、配置与统计结构。

## 授权

当前未附开源许可证。代码公开可见不代表授予复制、修改或再分发许可；正式授权以之后加入的许可证为准。
