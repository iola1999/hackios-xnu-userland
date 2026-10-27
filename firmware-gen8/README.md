# Adreno gen8 固件

## 1. K90 / SM8750

K90 的 Android 启动日志加载 `gen80000_gmu.bin`、`gen80000_sqe.fw`、`gen80000_aqe.fw` 和 `gen80000_zap.mbn`。厂商 `qcom,adreno-gpu-gen8-0-0` 配置使用同组文件，GMEM 为 12 MiB，来源见 [adreno-gpulist.h](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8750/blob/d50b305f7da9e14715a25120a4ac7b1a4b8b97c3/vendor/qcom/opensource/graphics-kernel/adreno-gpulist.h)。

本目录的 `gen80000_*` 从 linux-firmware 提交 `5c57d4817ebded2730ef20b3f2dd0f844b16dd0e` 获取；具体 URL、大小和 SHA-256 见 [gen80000-manifest.json](gen80000-manifest.json)。这些文件来自公开固件库，与本机 Android 固件的二进制版本是否一致尚未核对。K90 的实验根盘把四个文件放入 `/fw/`，内核当前读取 `gen80000_gmu.bin`。

## 2. 许可与打包

现有 `userland/build.sh` 从其仓库根目录的 `fw-gpu/` 复制固件。顶层工作区可按以下命令准备 kernel 构建目录，随后运行原有用户态构建；阶段实验根盘也必须包含相同文件。

```sh
mkdir -p hackios-xnu-kernel/fw-gpu
cp hackios-xnu-userland/firmware-gen8/gen80000_gmu.bin hackios-xnu-userland/firmware-gen8/gen80000_sqe.fw hackios-xnu-userland/firmware-gen8/gen80000_aqe.fw hackios-xnu-userland/firmware-gen8/gen80000_zap.mbn hackios-xnu-kernel/fw-gpu/
cp hackios-xnu-userland/firmware-gen8/LICENSE.qcom.txt hackios-xnu-userland/firmware-gen8/NOTICE.qcom.txt hackios-xnu-kernel/fw-gpu/
```


`gen80000_*` 的许可和通知分别保存在 [LICENSE.qcom.txt](LICENSE.qcom.txt) 与 [NOTICE.qcom.txt](NOTICE.qcom.txt)。分发或打包固件时保留这两个文件，并遵守其中的适用条件。

目录中既有 `gen80200_*` 来自较早的离线准备。K90 当前实验按第 1 节选择 `gen80000_*`；其他 GPU 的文件选择仍需根据对应设备配置核对。
