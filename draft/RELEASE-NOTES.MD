# 首次公开快照：ABI 0.88

本次发布提供 NextGenerationGarbageCollector 当前研究原型及设计材料，内存底座为随仓库提供的 UHMAS-HL。API 和内部布局仍会演进。

当前能力包括非移动 mark-sweep、分 Region 的 young evacuation、年龄晋升、受限并发 copy、显式 roots/safepoints、Pin owner ledger、Weak/Soft/Phantom 引用、有预算的 Mutator Assist 和故障降级。

最近完成的正确性改进包括 worker pool 显式失败退出与 close 重试、重新停车等待修复、scanner 错误返回/协作取消及 Verify-only 重入诊断、canonical-owner 读屏障、原子共享诊断与引用创建 API 的逐调用错误返回。

MSVC x64/x86 Debug/Release 四组完整测试各 23/23，MinGW x64 Debug 无 `<threads.h>` 配置 19/19。生产 Release 中的测试闸门与可选高成本诊断由预处理移除。

完整并发 REMAP、并发 marking/压缩、root/handle 并发闭包、TSAN 实测和其他平台验证仍待完成。屏障移锁暂缓。当前发布不作全平台已验证、高吞吐基准或硬实时承诺。

源码快照使用匿名初始提交，公开文件不带开发仓库历史、构建日志或本地工具配置。授权状态见根 README 与实际许可证文件。
