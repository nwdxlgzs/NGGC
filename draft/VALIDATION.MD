# 验证记录

日期：2026-10-10。代码公共 ABI：0.88。验证环境：Windows。

## 完整 CTest

| 编译器与位宽 | 配置 | 结果 |
| --- | --- | --- |
| MSVC x64 | Debug | 23/23 |
| MSVC x64 | Release | 23/23 |
| MSVC x86 | Debug | 23/23 |
| MSVC x86 | Release | 23/23 |
| MinGW x64，无 C11 `<threads.h>` | Debug | 19/19 |

部分多线程专项目标只在 `<threads.h>` 可用时构建，故无线程库配置的测试数较少。32-bit Debug 在验证机器上需要匹配的 MSVC x86 Debug CRT；安装位置由本机构建工具链确定，不应在公开材料中写入用户路径。

## 并发与故障回归

- 四原生 owner、三轮 young copy、source payload lock 竞争、resume/repark、Pin 地址稳定、年龄晋升与 parked parallel REMAP。
- canonical owner 独占更新字段后，旧 source slot 正确读回 null 或替换 referent；覆盖 512 B、24 KiB、128 KiB Region 的大对象 start/continuation slots。
- scanner legacy/ex 两种协议、全部 54 个公共 API 的 Verify-only 重入拒绝、跨 heap 调用、poll context、错误锁存、MARK abort 和后台 REMAP 应急完成。
- worker pool 的 coordinator wait/broadcast/join 失败与七个显式 worker 失败窗口，验证同步接管、参与者归静和 close 重试。
- 四种引用创建 API 在取得 `BUSY` 后，另一线程把共享诊断改为 `INVALID_ARGUMENT`，仍返回自己的 `BUSY` 与空输出；三线程各 4000 次错误发布/查询压力。

canonical-copy 与 scanner 增量完成时的专项，在 MSVC 四配置各连续通过 30 次；随后原子诊断/错误回传增量的最终源码完成上表全部测试，错误回传专项再在四配置各连续通过 30 次。

临时 mutation 副本恢复旧 repark 谓词、关闭 API 重入 guard 或恢复从共享诊断取失败码，均实际被对应回归检出。mutation 位于本机构建目录，不进入发布源码。

## 正式库与验证边界

正式 Release 预处理结果已检查：scanner TLS/guard、pool/copy 故障注入和错误回传暂停钩子被移除；原子诊断、局部 result helper 和必要协议检查保留。独立测试目标自行启用 Verify/Test Hooks。

已有宏入口不能证明已运行 ASAN/TSAN，也不能证明整个 collector 无数据竞争。本次未进行 TSAN 实测。Linux、macOS、其他 CPU 架构、mutator-concurrent REMAP、未报告的线程终止/阻塞和长期性能基准仍未完成。

公开快照工具另执行文件/身份检查，并生成 SHA-256 清单。该扫描发现常见邮箱、私人目录、已知本地身份与常见密钥格式；它不能自动识别所有自然语言个人信息或所有秘密。
