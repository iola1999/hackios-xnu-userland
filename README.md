# hackios-xnu-userland（REDMI K90 移植分支）

本仓库是 [HackiOS K90 移植](https://github.com/iola1999/hackios-redmi-k90-annibale)的用户态部分：init 与注入库、launchd 任务、APFS 镜像构建、从 IPSW 提取 iOS 文件的脚本、Mesa 补丁与 AIR（Metal IR）前端，以及 Adreno 830 固件。默认分支 `k90-port`。使用方法、当前进展和排查记录都在顶层工作区。

本仓库 fork 自 OOSEX GANG 的 [hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland)，保留其 Git 历史。

## 1. 获取

本仓库作为顶层工作区的子模块检出在 `hackios-xnu-userland/`，与 [K90 内核仓库](https://github.com/iola1999/hackios-xnu-kernel)并列：

```sh
git submodule update --init --recursive
```

## 2. 目录

| 路径 | 内容 |
|---|---|
| `userland/build.sh` | 构建用户态程序与 APFS 镜像，变量说明在文件开头。iOS 26：在 `userland/` 执行 `COMPILE_ONLY=1 IOS=../ios26 bash build.sh`（需要 Xcode），生成供顶层 `package.py` 打包辅助盘的 `root/`；系统卷使用 IPSW 中的原版 APFS 容器 |
| `userland/src/` | `init.c`（1 号进程）、注入 SpringBoard 等进程的库（`inject_sb26*.c` 等）、原生 IPA 管理器应用 `AnnibaleInstaller.app`（`installer_app.m`、`build_installer.sh`）与安装工具 `appinst`（`appinst.m`）、触摸与按键服务、OpenGL ES 到 Mesa 的桥接（`eagl.c`）、测试程序 |
| `userland/var/`、`var.skel.extra` | `/private/var` 的默认内容 |
| `ios26/` | `fetch.sh` 完整提取 iPhone 17 的 iOS 26.6.2 `os.dmg`、`systemos.dmg` 与 `appos.dmg`（IPSW 需另行下载），供 helper、launchd 任务生成和框架分析使用；`full-system.py` 生成元数据并校验提取状态 |
| `tools/mk-mesa.sh`、`tools/mesa/` | Mesa 加本项目补丁。K90 用 26.2.5 与 `REBASE=26`，补丁说明见 [tools/mesa/rebase-26/README.md](tools/mesa/rebase-26/README.md) |
| `tools/mesa/air/`、`tools/air/` | AIR 前端与它的检查。说明在 [docs/air.md](docs/air.md)（上游，英文），[docs/air-report.md](docs/air-report.md) 是 `tools/air/check.sh` 生成的报告 |
| `tools/apfs/` | 在电脑上写入和读取 APFS 镜像 |
| `firmware-gen8/` | Adreno 830 固件，见 [firmware-gen8/README.md](firmware-gen8/README.md) |

iOS 26 的 Metal 驱动在内核仓库的 `tools/userland-driver/`。根盘、Metal 驱动与 AIR 库的打包由顶层工作区的 `tools/k90auto/` 完成，见顶层 [k90auto 说明](https://github.com/iola1999/hackios-redmi-k90-annibale/blob/main/tools/k90auto/README.md)；Mesa 的 iOS 交叉构建见顶层 [Mesa 26.2.5 移植层](https://github.com/iola1999/hackios-redmi-k90-annibale/blob/main/docs/analysis/mesa-rebase-2026-10.md)。

## 3. 许可证

- 仓库不含 iOS 文件。系统盘在使用者自己的机器上由 `ios26/fetch.sh` 从 IPSW 取出。
- 本仓库的代码按 [MIT License](LICENSE) 发布。
- Mesa（MIT）不在仓库里：`tools/mk-mesa.sh` 下载固定版本，`tools/mesa/*.patch` 与 `tools/mesa/rebase-26/*.patch` 是对它的修改。
- apfsprogs（GPL-2.0）不在仓库里：`tools/apfs/check-image.sh` 下载它，只构建 `apfsck`。
- `firmware-gen8/` 的固件按 [LICENSE.qcom.txt](firmware-gen8/LICENSE.qcom.txt) 与 [NOTICE.qcom.txt](firmware-gen8/NOTICE.qcom.txt) 分发。
- HackiOS 是独立研究项目，与 Apple Inc. 没有关联，也没有得到其授权或认可。

上游作者：OOSEX GANG，动态见 [t.me/oosexgangdevs](https://t.me/oosexgangdevs)。
