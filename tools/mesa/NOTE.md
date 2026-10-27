# 这份 `tools/mesa/` 只服务 iOS 16.5 那条线

iOS 26 的工作不要改这里。iOS 26 的权威副本是 `hackios-xnu-kernel/tools/mesa/`（同一个工作区里的另一个 submodule），`package.py` 与 `mk-mesa.sh` 取的都是那一份。

两份不合并：合并会动到 16.5 的构建路径，风险大于收益。改 iOS 26 的 `air_metal.c`、`air_metal.h` 或补丁时在 `hackios-xnu-kernel/` 里改，用 `common/ugit add -f <路径>` 提交（`add` 必须带路径）。

2026-10-28：曾有两个提交把这个目录放进 perf 的诊断 patch（`70ba33f`、`bbde1df`，`draw-perf.patch`），已删除（`dd58070`）。原因是 `tools/mesa/*.patch` 会被 16.5 的 `mk-mesa.sh` 用通配符自动收下，那个 patch 默认开着逐帧计时，会把测量开销带进 16.5 的库，而所有性能对比都以 16.5 为参照。删除后 `mesa_from_stamp` 的输入只剩 8 个 patch（`air-frontend`、`air-ir3`、`air-metal`、`freedreno-darwin`、`ir3-round-default`、`ir3-shift-split`、`ir3-ssbo-subdword`、`osmesa-es-profile`），戳为 `b24690de…`。
