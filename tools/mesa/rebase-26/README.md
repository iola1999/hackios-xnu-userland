# rebase-26 —— Mesa 26.x 移植层（`REBASE=26 tools/mk-mesa.sh` 时应用）

应用顺序（在 mk-mesa.sh 的 tar 解包之后、原有 `tools/mesa/*.patch` 循环之后）：

1. 原有 `tools/mesa/*.patch` 照旧应用；在 26.2.5 上会有部分 hunk 失败/跳过，属预期，失败明细见 `docs/analysis/mesa-rebase-2026-10.md` 第 2 节。
2. `0001-fd6-indirect-buffer-wfm-a8xx.patch`（upstream 2ee0a5e6）与 `0002-fd6-query-barriers-gen8.patch`（upstream 779cdb8e）：两个 a8xx 修复在 26.3-branchpoint 才有，26.2.5 上 cherry-pick 干净；若改用 26.3.x 基准则这两个可以去掉。
3. `0003-restore-osmesa-26.x-drift-adaptations.patch`：回滚 upstream 的 OSMesa 删除（027ccd963b）、以 25.0.7 的已打补丁版本恢复 osmesa 前后端（内容已包含 osmesa-es-profile 与 freedreno-darwin 的 osmesa hunk，因此那两个旧补丁的这些 hunk 必须处于跳过状态），并带上全部 26.x 漂移适配（glapi 包含路径、driParseConfigFiles 新签名、postprocess 剥离、u_sync_provider darwin 门控、libglapi_bridge 强制链接以导出 gl* 等）。
4. `0004-turnip-msm-darwin.patch`：Turnip（Vulkan）在 iOS 上经 msm 后端和 `tools/mesa/redwood-drm` 访问内核的 `RedwoodGPU`，用 `TURNIP=1 REBASE=26 tools/mk-mesa.sh` 构建。Turnip 单独构建时定义 `HAVE_LIBDRM`，Vulkan 运行时才能经 `drmGetDevices2` 找到设备；Darwin 上为 Turnip 编译 `u_sync_provider.c`；缓冲区映射改走 `redwood_drm_map`；`cache_ops_aarch64.c` 在 Darwin 上从 sysctl `hw.cachelinesize` 取缓存行大小，因为 XNU 不允许 EL0 读 `CTR_EL0`。补丁在 `REBASE=26` 时总会应用，除 `cache_ops_aarch64.c` 外，改动只在构建 Turnip 时生效。
5. `0004-a830-no-early-preamble.patch`：A830 的设备表关掉 `has_early_preamble`。开着时，片段着色器经 bindless fb-read 描述符读帧缓冲的 gallium 绘制会让 GPU 停住（CP 已取完命令，HLSQ 一直忙）；关掉后同样的绘制都能完成。实测见 `docs/progress/2026-10-30-ios26-usable-desktop.md` §4.3。
6. `0005-linear-import-last-level-padding.patch`：线性导入的末级行数不是 4 的倍数时，`fd6_layout.c` 补齐后的布局可能超过 IOSurface 的分配（45x42 RGBA16Float，每行 384 字节，布局 16896 字节，分配 16384 字节）。各行都落在内存内的单级单层线性导入照样接受，资源标记 `sysmem_only`，绘制时不走 GMEM，不触及补齐的行。实测见 `docs/progress/2026-10-30-ios26-usable-desktop.md` §5。
7. `0007-ir3-retry-spilling-stage-without-preamble.patch`：用了私有内存的 stage 打印一行 `ir3[进程 pid]:`（私有内存字节数、`max_reg`、preamble 指令数，每进程前 64 行）；任务环境变量 `REDWOOD_IR3_SPILL_RETRY=1` 时，对有 preamble 的这类 stage 关掉 preamble 再编一次，私有内存更少、或私有内存相同而寄存器更少的一份胜出。不设变量时只多打印。`kernel-12377-rate1-rt1` 里这类 stage 只有 11 个，在 SpringBoard 与 iconservicesagent，backboardd 没有，重编一批没有跑。
8. `0008-u-trace-redwood-pass-times.patch`：任务环境变量 `REDWOOD_PASS_TIMES=1` 打开 Mesa 的 GPU 时间戳，按 pass 种类（画布尺寸、GMEM bin 数、颜色附件数、深度／模板格式）每 5 秒汇总一行 `pass times`；`fd_trace_read_ts` 去掉 `FD_BO_PREP_NOSYNC` 加 10 ms 睡眠的轮询，直接阻塞等待，这一处对所有打开 u_trace 的用法生效。读法见顶层 `docs/analysis/2026-11-02-120hz-after-gpu-clock.md` §6。
9. `0009-fd-resource-contents-undefined.patch`：新增 `fd_resource_contents_undefined()`，把资源标为内容无效，下一个画进它的 batch 不再把它读回 GMEM；上一个写它的 batch 照样写回。AIR 在 Metal pass 的第一次绘制前，对 `MTLLoadActionDontCare` 的颜色附件调用它。CoreAnimation 整屏 pass 的第二个 RGBA16Float 附件属于这种情况，此前每个 bin 都读回，见顶层 `docs/analysis/2026-11-02-120hz-after-gpu-clock.md` §6。

`rebase-26/` 下的补丁按文件名顺序应用。两个 `0004` 开头的补丁分别来自 Turnip 与 A830 两条工作，改的文件互不重叠。

`smoke-osmesa.c` / `smoke-osmesa-es.c` 是构建后冒烟测试，在 macOS 主机上编译运行（详见报告第 3 节）。
