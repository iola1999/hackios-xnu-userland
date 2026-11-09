# Adreno 830 固件（gen80000）

K90 的 GPU 需要四个固件文件：`gen80000_gmu.bin`、`gen80000_sqe.fw`、`gen80000_aqe.fw` 和 `gen80000_zap.mbn`。厂商 `qcom,adreno-gpu-gen8-0-0` 配置使用同组文件，GMEM 为 12 MiB，来源见 [adreno-gpulist.h](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8750/blob/d50b305f7da9e14715a25120a4ac7b1a4b8b97c3/vendor/qcom/opensource/graphics-kernel/adreno-gpulist.h)。

- 本目录的 `gen80000_gmu.bin`、`gen80000_sqe.fw`、`gen80000_aqe.fw` 从 linux-firmware 提交 `5c57d4817ebded2730ef20b3f2dd0f844b16dd0e` 获取，URL、大小和 SHA-256 见 [gen80000-manifest.json](gen80000-manifest.json)。它们与手机 Android 里 `/vendor/firmware/` 的同名文件 SHA-256 相同。
- `gen80000_zap.mbn` 要用手机自己的，在 Android 的 `/odm/firmware/gen80000_zap.mbn`（12088 字节，本测试机 SHA-256 `297318258cb36e5cf6205332a424735c0f9c8f4ba3f9382a6b1d409431b136f7`）。linux-firmware 的 `qcom/sm8750/gen80000_zap.mbn` 是另一份文件，不放在本目录。

## 1. 放进根盘

内核从内存盘里根盘的 `/fw/` 读取这四个文件。`userland/build.sh` 构建根盘时把 `hackios-xnu-kernel/fw-gpu/` 里的文件复制到 `/fw/`。在顶层工作区执行：

```sh
mkdir -p hackios-xnu-kernel/fw-gpu
cp hackios-xnu-userland/firmware-gen8/gen80000_gmu.bin hackios-xnu-userland/firmware-gen8/gen80000_sqe.fw hackios-xnu-userland/firmware-gen8/gen80000_aqe.fw hackios-xnu-kernel/fw-gpu/
cp hackios-xnu-userland/firmware-gen8/LICENSE.qcom.txt hackios-xnu-userland/firmware-gen8/NOTICE.qcom.txt hackios-xnu-kernel/fw-gpu/
adb -s "$ANDROID_SERIAL" pull /odm/firmware/gen80000_zap.mbn hackios-xnu-kernel/fw-gpu/
```

`/odm/firmware/` 不需要 root 就能读。

## 2. 许可

`gen80000_*` 的许可和通知在 [LICENSE.qcom.txt](LICENSE.qcom.txt) 与 [NOTICE.qcom.txt](NOTICE.qcom.txt)。分发或打包固件时保留这两个文件，并遵守其中的条件。
