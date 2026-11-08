# 3DMark 慢因定位：宿主 0.6 µs/draw vs guest 10–30 ms/draw（2026-10-31）

## 结论一句话

同一张 GTX 1080、同一版 NVIDIA 驱动，**宿主原生 Vulkan 一个 draw 只要 0.6 µs**；
而 guest 里经 Waydroid + 我们的 HAL + ANGLE，**一个 draw 要 10–30 ms**，相差约 **2 万倍**。
硬件与驱动都不是瓶颈，瓶颈在我们这层。

## 1. 宿主原生（对照组）

`/tmp/drawcost2.c`：1500 个 1-三角形 draw 覆盖 2560x1440，内嵌 SPIR-V，直连 580.196.07：

```
device NVIDIA GeForce GTX 1080
1500 draws of 1 triangle over 2560x1440, 5 frames: 4.50 ms total,
0.899 ms/frame, 0.599 us/draw
```

加上「每 64 draw 重绑一次 pipeline」→ **0.767 µs/draw**（仍微秒级）。
空 render pass（512x512，无 draw）：**0.114 ms/帧**。

## 2. guest 侧（被测）

DRAW-DIST（`debug.nvhal.gputime=2`，artifact `artifacts/bench3d-20261031T134403-drawdist-t3/`）：

| pass | draws/帧 | ms/帧 | µs/draw |
|---|---|---|---|
| 4096x4096 阴影（**无像素着色器**） | 209 | 2500 | **11960** |
| 2048x2048 阴影 | 118 | 2163 | **18334** |
| 512x512 | 72 | 738 | **10248** |
| 2560x1440 | 1 | 153 | **153220** |

**每 draw 成本与像素覆盖量无关**（512² 与 2560×1440 一样贵）⇒ 是固定开销，不是填充率。

## 3. 已排除

| 假设 | 判据 | 结果 |
|---|---|---|
| 显存压力 / block 图占位 | Run D：`astc_blocks_host=1`，OODM 139→0、宿主内存分配 1352 MiB→0、app 可见 7.06→5.84 GiB | **帧时间不变**（669–955 ms/帧） |
| 数据在宿主内存 | 同上：分配全落在 VRAM type 7 | 否 |
| 窗口/渲染分辨率设置 | 2a：477×852 与 1900×1010 同分 | 否 |
| CPU 侧 VkImage 翻译 | `xlate_stats` 实测 0.02% of CPU | 否 |
| 提交次数 | gputime：8.0 submits/帧（只占 busy 的 20%） | 否 |
| HAL 拦截 draw/submit/fence | `hal.c:430-478` 拦截表里没有这些 | HAL 未参与每次 draw |
| GPU 闲置 | gputime：busy ≈ timed regions ≈ 700–780 ms/帧 | GPU 真的在忙 |

## 4. 剩下的位置

`hal.c` 拦截表不含 `vkCmdDraw*` / `vkQueueSubmit` / `vkWaitForFences`，所以这些是**直接透传给 ICD** 的。
guest 与宿主之间只多了：

1. **ANGLE**（`ro.hardware.egl=angle`，GLES→Vulkan 转换层）
2. **gralloc/minigbm**（buffer 分配与锁）
3. **HWC/SurfaceFlinger**（合成）

以 10–30 ms/draw 的固定成本看，最像 ANGLE 每 draw 做一次「状态重算 + pipeline/descriptor 全量重建」。

## 5. 下一步（三个便宜判定）

1. **perf 抓 draw 期间 CPU 栈**：`pkexec-ng perf record -F 199 -a -- sleep 12`，然后 `--sort dso,symbol`，
   看落在 `libGLESv2_angle.so` / `libEGL_angle.so` 还是 `vulkan.nvidia.so`。
   （本轮已抓到 /tmp/perf-draw.data，但采样窗口落在 workload 之外的 python3/helium 上，需要重抓。）
2. **`--native-gl on` 对照**：`dev/driver-mode apply glibc-shim --native-gl on` 绕开 ANGLE 跑一次 3DMark。
   分数跳升 → ANGLE；不变 → HWC/gralloc。
3. **app 自身 CPU**：`appcpu.csv` 显示 3DMark 进程在 workload 期峰值 **5.1 核**（`/proc/<pid>/stat` 差分），
   说明有大量 CPU 活动；若这些 CPU 在 ANGLE 里，与「每 draw 毫秒级」自洽。

## 产物

- 宿主微基准源码：`/tmp/drawcost2.c`、`/tmp/drawcost3.c`（内嵌 SPIR-V，可直接复跑）
- guest 逐 draw：`artifacts/bench3d-20261031T134403-drawdist-t3/`
- Run D（显存否定）：`docs/astc-blocks-host-findings.md`、`artifacts/bench3d-20261031T132842-blocks-host-t4/`
- perf 采样：`/tmp/perf-draw.data`（窗口未覆盖 workload，待重抓）
