# 匿名发布指南

## 发布范围

保留当前本地开发仓库与历史，发布使用独立的匿名仓库。旧历史有个人作者信息；直接从开发仓库 push、push `--mirror` 或上传其 `.git` 会带出这些信息。

候选源码包含根文档、CMake、公共头、实现、全部测试、UHMAS-HL 和本目录材料。发布工具从 Git 的 tracked + non-ignored untracked 文件建立清单，因此当前尚未提交的实现和测试也会进入快照。

排除开发 `.git`、所有 build/out 目录、本机 IDE 状态、日志、二进制产物与常见凭据文件。构建目录仍保留在本机，工具不修改或删除它们。公开快照不携带这些目录，也不继承 Git 全局身份、签名、hooks 或 remote。

## 生成候选

需要 Python 3.9+ 与 Git。仓库根目录执行：

```sh
python draft/tools/check_publication.py . --working-tree
python draft/tools/prepare_publication.py --release-date 2026-10-10
```

默认输出：

```text
out/publication/
  NextGenerationGarbageCollector/  # 独立匿名 Git 仓库，main 分支
  NextGenerationGarbageCollector-source.zip  # 源码，不含 .git
  SHA256SUMS.txt                  # 相对路径与 SHA-256
  publication-report.json        # 文件数、提交和扫描摘要
```

输出目录必须不存在；工具不会覆盖现有候选。重新生成时，用 `--output out/publication-next` 选择新目录。

匿名作者/提交者固定为 `NGGC Contributors` 与保留的示例地址 `nggc@example.invalid`，日期按发布日期统一为 UTC 零点。初始提交不引用旧 commit，也无旧 branch、tag、reflog 或 remote。源码压缩包固定文件时间与权限，不包含宿主绝对路径。

可以再次检查候选及 Git 历史：

```sh
python draft/tools/check_publication.py out/publication/NextGenerationGarbageCollector --private-repository .
git -C out/publication/NextGenerationGarbageCollector log --all --format=fuller
```

检查只报告规则及相对位置，不回显敏感内容。通过扫描不等于所有形式的个人信息均可被机器识别，发布前仍需浏览材料。

## 构建复核与推送

```sh
cmake -S out/publication/NextGenerationGarbageCollector -B out/publication-build -DCMAKE_BUILD_TYPE=Release -DNGGC_BUILD_TESTS=OFF
cmake --build out/publication-build --config Release --parallel
```

在远程平台创建空仓库后，进入候选仓库执行以下命令，将 `<REMOTE_URL>` 替换为实际地址：

```sh
cd out/publication/NextGenerationGarbageCollector
git remote add origin <REMOTE_URL>
git push -u origin main
```

之后公开提交也应保持匿名身份。远程账号、仓库 owner、登录/签名、issue 和 PR 操作的公开身份由托管平台决定，源码清理不能隐藏平台账号本身。

## 授权

当前未选择开源许可证；仅公开仓库不自动授权复制、修改或再分发。确定许可证后，在源码根添加 `LICENSE` 并重新生成候选。UHMAS-HL 随同发布，其授权归属与最终许可证范围也需一致。
