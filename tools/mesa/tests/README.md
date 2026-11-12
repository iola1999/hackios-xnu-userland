# ir3 输出优化回归测试

## 1. 运行

`test-ir3-output-contracts.py` 从固定 SHA-256 的 Mesa 26.2.5 发布包解压源码，在新目录构建原生 `libfreedreno_ir3`、NIR 与测试程序。它先运行旧版，再应用 `0018-ir3-alias-relative-and-fsat.patch` 并重编同一测试。测试不访问 GPU。

依赖为 Python 3.12 或更新版本、Mako、PyYAML、packaging、Meson 1.12.1、C/C++ 编译器、Ninja、Bison 3.8、Flex、pkg-config 与 patch。macOS 自带的 Bison 版本过旧；使用 Homebrew Bison，并将其目录放在 `PATH` 前面。构建使用 `--wrap-mode=nofallback`，缺少依赖会直接报告，不下载 Meson 子项目。

从用户态仓库根目录执行，`out/ir3-output-contracts` 必须尚不存在：

```sh
python3 -m venv out/ir3-test-venv
out/ir3-test-venv/bin/pip install mako pyyaml packaging meson==1.12.1
PATH="$(brew --prefix bison)/bin:$PATH" CC=clang CXX=clang++ \
  out/ir3-test-venv/bin/python tools/mesa/tests/test-ir3-output-contracts.py \
  --archive ../hackios-xnu-kernel/xnu-deps/build/mesa/mesa-26.2.5.tar.xz \
  --work-dir out/ir3-output-contracts --jobs 4
```

脚本仅在新目录中追加测试目标，不改 `mk-mesa.sh` 使用的源码或构建目录。`baseline.log` 和 `fixed.log` 包含每项检查及生成的 A830 指令；`setup.log`、`baseline-build.log`、`fixed-build.log` 和 `patch.log` 保存构建输出。正常结束要求旧版测试返回失败，修复版返回成功。

## 2. 样本与检查

`ir3-output-contracts.c` 直接调用 `ir3_create_alias_rt()`，并通过 NIR finalize、`ir3_compile_shader_nir()` 与 ISA assembler 编译四个片元着色器。检查读取 IR 对象中的输出掩码、SSA 引用、寄存器、指令类型和饱和标记，反汇编文本仅供检查。

| 样本 | 旧版 26.2.5 | 应用补丁后 |
| --- | --- | --- |
| 相对常量 MOV 输出，偏移 12、地址值 4 | 生成 alias.rt，移除原输出，检查失败 | 无 alias.rt，原 SSA 输出、偏移和地址保留 |
| 绝对常量 c3.x 输出 | 生成 alias.rt，源寄存器保留 | 相同行为 |
| 立即数 0.5 输出 | 生成 alias.rt，值保留 | 相同行为 |
| `fsat(bitcast<float>(ixor(input.x, input.y)))`，单次使用 | `xor.b (sat)`，检查失败 | `xor.b` 后保留 `max.f (sat)` |
| `fsat(bitcast<float>(iadd(input.x, input.y)))`，单次使用 | `add.u (sat)`，检查失败 | `add.u` 后保留 `max.f (sat)` |
| XOR 结果另写一个输出，再执行 fsat | 保留 `max.f (sat)` | 相同行为 |
| 浮点加法后执行 fsat | `add.f (sat)` | 相同行为 |

2026-11-12 的 macOS arm64 原生运行包含 16 项检查：旧版 3 项失败，修复版 16 项通过。四个 NIR 样本均成功编译和汇编。生产准备树中的 `ir3_alias.c`、`instr-a3xx.h` 与发布包逐字节相同，补丁也通过该准备树上的 `patch --dry-run`。

## 3. 验证范围

宿主测试验证优化器的数据保留和 A830 指令选择，包含 ISA 汇编。它使用发布包加本补丁。另一次隔离构建从相同发布包应用本仓全部 Mesa 补丁，并复制 AIR 与 annibale-drm 源码，已完成 `libannibaleair.dylib` 的 arm64e 链接，目标最低版本 iOS 26.0，SDK 27.1。实际 GPU 像素仍待真机验证。

`tools/air/check.sh -q` 已执行，因缺少 `build/mesa-host/build/src/compiler/air/air2nir` 提前退出，未进入 AIR corpus 测试。四个输入 metallib 均存在。该入口的宿主构建脚本仍固定 Mesa 25.0.7，本轮未调用它。

后续真机 fsat 样本可用运行时整数输入产生 `uintBitsToFloat(bits ^ mask)`，覆盖 -0.5、0.5、1.5，预期输出分别为 0、0.5、1。目标使用浮点附件，防止 UNORM 写回的截断影响判断。相对常量样本需确认最终 ir3 含相对常量 MOV，并用不同数组元素及非零索引核对输出；普通 UBO 读取可能被编译成其他指令，无法覆盖本次 alias.rt 分支。

补丁移植自上游 [8511f446](https://gitlab.freedesktop.org/mesa/mesa/-/commit/8511f446715cc79ccd72bbd1c26471135d774dea) 与 [5a9db0be](https://gitlab.freedesktop.org/mesa/mesa/-/commit/5a9db0be64e090164930593c6cd13d08ad45123f)。`is_sat_compatible()` 移到两个浮点类别判断函数之后，满足 26.2.5 的声明顺序；条件与上游修复保持一致。
