# HackiOS — userland

> **REDMI K90 移植分支：`k90-port`。** 本仓库基于 [OOSEX GANG 的原用户态仓库](https://github.com/oosexgang/hackios-xnu-userland)，保留其 Git 历史并维护本项目的用户态和构建修改。感谢原作者及贡献者开放 init、注入库、APFS 镜像工具和图形适配；本项目沿用这些实现开展 K90 移植。

[顶层工作区](https://github.com/iola1999/hackios-k90-port)保存计划、实测和子模块版本，[K90 内核仓库](https://github.com/iola1999/hackios-xnu-kernel/tree/k90-port)维护 XNU、loader 与固件相关修改，本仓库维护用户态及镜像工具。两个代码仓库均已建立与对应上游的 GitHub fork 关系，默认分支均为 `k90-port`。原许可证和版权声明保留；K90 的验证范围以顶层工作区文档为准。

K90 构建使用对应内核仓库的 `k90-port`，运行 `common/setup.sh` 时显式指定 `USERLAND_URL=https://github.com/iola1999/hackios-xnu-userland.git USERLAND_REF=k90-port`。userland 会作为 `.userland.git` 管理的 overlay 放入 kernel 工作树；下文的上游获取方式保留其原始来源。

The user-space half of **HackiOS**: Apple's open-source XNU kernel on Android phone hardware (POCO X5 Pro 5G) and in QEMU, running the unmodified iOS 16.5 user space. The kernel, loader, UEFI and the `common/` scripts are in [hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel) — **start there**.

> **No Apple files are in this repository.** `ios165/fetch.sh` downloads the iOS 16.5 IPSW from Apple's CDN to your machine and extracts what is needed there. HackiOS is an independent research project by OOSEX GANG and is not affiliated with, authorised or endorsed by Apple Inc.

## 1. Getting it

The two repositories are built as one tree. Clone the kernel repository and let it bring this one in:

```sh
git clone https://github.com/oosexgang/hackios-xnu-kernel.git hackios
cd hackios
common/setup.sh        # checks this repository out into the same directory (git dir .userland.git)
common/ugit status     # git for this repository inside that tree
```

## 2. What is here

| Path | What |
|---|---|
| `userland/build.sh` | builds the APFS images: `root.apfs` (our init, libraries, launchd jobs, firmware) and `system.apfs` (the selected part of iOS 16.5 and its cryptexes); every option is documented at the top |
| `userland/src/` | `init.c` (process 1), injected libraries (`inject.c` backboardd, `inject_sb.c` SpringBoard, `inject_media.c` mediaserverd, `inject_tweaks.c`), the CoreAudio output device (`audio_plugin.c`), the OpenGL ES bridge to Mesa (`eagl.c`), Metal driver work, tests |
| `userland/libmini/` | a tiny libc for the static programs |
| `userland/rc.*`, `com.redwood.*.plist` | job lists and launchd jobs for test scenarios (`RC_JOBS=rc.jobs.tour`, `rc.jobs.ssh`, ...) |
| `userland/mkjobs.py`, `jobinfo.py` | the stock iOS launchd jobs: which are loaded, who provides a Mach service |
| `userland/var/` | our default preferences for `/private/var` |
| `ios165/` | `fetch.sh` / `fetch-from-ipsw.py` (take iOS out of the IPSW), `select-system.py` (what goes into the image) |
| `tools/apfs/` | APFS on the computer: `mkapfs.py` (image writer), reader, checks, write tests |
| `tools/mk-mesa.sh`, `tools/mesa/` | Mesa 25.0.7 with our patches: freedreno on Darwin, the AIR (Metal IR) frontend |
| `tools/air/`, `tools/metal/`, `tools/metallib.py` | Metal shader (AIR) checks and analysis; notes in `docs/air.md` |
| `tools/mk-jb.sh`, `jb-preinstall.py` | Procursus bootstrap + Sileo for the package manager prefix |
| `tools/font_trim.py` | trims two oversized iOS fonts so the image fits |
| `prebuilt/userland/` | our programs and libraries, built — used when there is no cross toolchain |

## 3. Building the images

```sh
ios165/fetch.sh               # once
REST=1 userland/build.sh      # -> userland/root.apfs, userland/system.apfs
GL=1 REST=1 userland/build.sh # with OpenGL ES (Mesa) for the window server; on the phone with gpu=1
```

Without the cross toolchain (theos + iPhoneOS16.5.sdk, see the kernel repository) the build takes our programs from `prebuilt/userland`; with it, it compiles `userland/src`.

A job list (`RC_JOBS=file`) is run by our `init`, not by a shell: iOS 16.5's root has no shell, lines are split on blanks, there is no quoting and no variable assignment; only `>> FILE` and a trailing `&` are understood.

## 4. Licenses

Our code in this repository is released under the [MIT License](LICENSE). Mesa (MIT) is not in the repository: `tools/mk-mesa.sh` fetches release 25.0.7, `tools/mesa/*.patch` are our changes to it. apfsprogs (GPL-2.0) is not in the repository either: `tools/apfs/check-image.sh` fetches and builds only `apfsck`.

OOSEX GANG — [t.me/oosexgangdevs](https://t.me/oosexgangdevs)
