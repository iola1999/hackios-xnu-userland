# hackios-xnu-userland（REDMI K90 移植分支）

本仓库是 [HackiOS K90 移植](https://github.com/iola1999/hackios-redmi-k90-annibale)的用户态部分：init 与注入库、launchd 任务、APFS 镜像构建、从 IPSW 选取 iOS 文件的脚本、Mesa 补丁与 AIR（Metal IR）前端，以及 Adreno 830 固件。默认分支 `k90-port`。使用方法、当前进展和排查记录都在顶层工作区。

本仓库 fork 自 OOSEX GANG 的 [hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland)，保留其 Git 历史。上游面向 iOS 16.5 的内容（`ios165/fetch.sh`、`prebuilt/userland/`、16.5 的测试场景）仍在仓库里，本分支不维护这部分；`ios26/fetch.sh` 仍调用 `ios165/` 里的选取脚本。

## 1. 获取

本仓库不单独使用，要检出到 [K90 内核仓库](https://github.com/iola1999/hackios-xnu-kernel)的工作目录里，两个仓库的脚本按同一棵目录树互相引用：

```sh
cd hackios-xnu-kernel
USERLAND_URL=https://github.com/iola1999/hackios-xnu-userland.git USERLAND_REF=k90-port common/setup.sh
common/ugit status        # 在这棵树里操作本仓库
```

开发在内核工作树的 `.userland.git` 里进行，提交再移植到本仓库，所以提交说明里会写「移植 `.userland.git` 某提交」。

## 2. 目录

| 路径 | 内容 |
|---|---|
| `userland/build.sh` | 构建 APFS 镜像，变量说明在文件开头。iOS 26 系统盘：`IOS=../ios26 bash build.sh`，得到 `system26.apfs` |
| `userland/src/` | `init.c`（1 号进程）、注入 SpringBoard 等进程的库（`inject_sb26*.c` 等）、触摸与按键服务、OpenGL ES 到 Mesa 的桥接（`eagl.c`）、测试程序 |
| `userland/var/`、`var.skel.extra` | `/private/var` 的默认内容 |
| `ios26/` | `fetch.sh` 从 iPhone 17 的 iOS 26.6.2 IPSW 取出文件（IPSW 需另行下载）；哪些文件进系统盘由 `ios165/select-system.py` 决定，`ios26/select-system.py` 是指向它的符号链接 |
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
