# 双附件 framebuffer fetch 探针

`fbfetch-mrt.m` 使用同一组 Metal render API 驱动 Apple 原生设备或指定的 Annibale 驱动。`fbfetch-mrt.metal` 提供 seed、DATA0/DATA1 pass-through、交换读取和故意将 DATA1 改读 DATA0 的 fragment。

## 1. 覆盖范围

两张 16×12 BGRA8Unorm 纹理采用不同的通道和空间图案，单采样，关闭 fixed blending，scissor 为 `3,2 9×7`。读取全图，包括 scissor 外应保持原值的像素；通道仅使用 0 和 255，逐字节比较，无容差。

以下三个变量组合成 8 个场景，每个场景检查两张纹理的 384 个像素：

- 读取 CPU 预填内容，或在同一 encoder 中先 draw 写入图案再读取。
- DATA0/DATA1 分别写回原附件，或交换写入两个附件。
- 纹理按正常顺序或反序绑定到 color attachments。

`--wrong-data1` 使用错误 fragment。相同预期必须在全部 8 个场景检测到失配并返回 1；编译或运行环境错误返回 2。正确版本返回 0。

探针设置 30 秒执行截止时间，覆盖设备初始化、编译与命令等待。超时通过 `SIGALRM` 输出 `fbfetch-mrt: 30-second deadline exceeded`，随后以 124 退出。124 只记录超时；像素失配使用返回码 1。

该探针覆盖两个 BGRA8 附件及 draw 间的读取顺序。其他格式、fixed blending、MSAA、memoryless 附件、TBDR 路径选择及实际系统 shader 仍需独立检查。Apple GPU 的像素结果用于核对测试语义，A830 的附件绑定与同步结果需要设备运行。

## 2. 宿主构建与原生参考

在用户态仓库根目录执行，输出目录必须尚不存在：

```sh
python3 tools/air/fbfetch-mrt-check.py --work-dir out/fbfetch-mrt --air2nir /path/to/air2nir
```

脚本编译 macOS 和 iOS 的独立 metallib，运行 Apple GPU 的正确及错误对照，编译并签名 arm64e iOS 探针。iOS shader 目标为 `air64-apple-ios16.5.0`、Metal 3.0；可执行文件最低系统版本为 iOS 26.0。所有命令、输入／产物摘要、日志、原生像素及 iOS bitcode 都写入输出目录。

可选 `--air2nir` 执行已有宿主工具，只读该工具及其依赖。检查五个函数的实际 decode，并确认 pass/swap 的 DATA0、DATA1 coherent 读取和各自输出写入。该检查覆盖 AIR 到 NIR，不运行 Adreno 指令或 Gallium framebuffer。省略该参数会明确记录未检查 AIR。

## 3. 设备运行

由负责设备的成员将 `iphoneos-probe` 打包为 `/annibale/bin/fbf`，将 `iphoneos.metallib` 打包为 `/annibale/share/fbf.metallib`，按项目设备手册运行。探针显式加载指定驱动，并检查 `rwa_device_name` 拒绝 softpipe。

正确版本的命令正文为 103 个 UTF-8 字节：

```sh
/annibale/bin/fbf --metallib /annibale/share/fbf.metallib --driver /annibale/lib/libannibalemetal.dylib
```

预期返回 0，并输出 `fbfetch-mrt: cases=8 failed=0 pixels=3072`。如需同轮验证错误对照，命令正文为 117 个 UTF-8 字节：

```sh
/annibale/bin/fbf --metallib /annibale/share/fbf.metallib --driver /annibale/lib/libannibalemetal.dylib --wrong-data1
```

预期返回 1，并输出 `fbfetch-mrt: cases=8 failed=8 pixels=3072`。该非零返回是错误对照的验收条件。探针默认只输出日志；`--output DIR` 可另存两张纹理各场景的原始 BGRA 数据，目录由调用者指定。
