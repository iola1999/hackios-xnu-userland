# rebase-26 —— Mesa 26.x 移植层（试验产物，尚未接入 mk-mesa.sh）

应用顺序（在 mk-mesa.sh 的 tar 解包之后、原有 `tools/mesa/*.patch` 循环之后）：

1. 原有 `tools/mesa/*.patch` 照旧应用；在 26.2.5 上会有部分 hunk 失败/跳过，属预期，失败明细见 `docs/analysis/mesa-rebase-2026-10.md` 第 2 节。
2. `0001-fd6-indirect-buffer-wfm-a8xx.patch`（upstream 2ee0a5e6）与 `0002-fd6-query-barriers-gen8.patch`（upstream 779cdb8e）：两个 a8xx 修复在 26.3-branchpoint 才有，26.2.5 上 cherry-pick 干净；若改用 26.3.x 基准则这两个可以去掉。
3. `0003-restore-osmesa-26.x-drift-adaptations.patch`：回滚 upstream 的 OSMesa 删除（027ccd963b）、以 25.0.7 的已打补丁版本恢复 osmesa 前后端（内容已包含 osmesa-es-profile 与 freedreno-darwin 的 osmesa hunk，因此那两个旧补丁的这些 hunk 必须处于跳过状态），并带上全部 26.x 漂移适配（glapi 包含路径、driParseConfigFiles 新签名、postprocess 剥离、u_sync_provider darwin 门控、libglapi_bridge 强制链接以导出 gl* 等）。

`smoke-osmesa.c` / `smoke-osmesa-es.c` 是构建后冒烟测试，在 macOS 主机上编译运行（详见报告第 3 节）。
