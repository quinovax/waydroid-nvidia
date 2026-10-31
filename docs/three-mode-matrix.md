# 三模式实测能力矩阵（更新于 2026-10-31 18:20）

> 所有数字来自 `artifacts/` 下的一手产物。未测项明确标注。

## 环境

| 项 | 值 |
|---|---|
| GPU | NVIDIA GTX 1080（Pascal GP104，compute 6.1，8192 MiB，PCIe 3.0 x16，BAR1 246 MiB） |
| 驱动 | **580.196.07**（此前 580.178.04；升级后需重新 native-deploy + driver-mode apply） |
| 内存 | 3 heap / 11 type：heap0 8192 MiB DEVICE_LOCAL（type 7）；heap1 11947 MiB（type 0-6 flags=0x0、8=HV|HC、9=HV|HC|HOST_CACHED）；heap2 246 MiB DL|HV|HC（type 10） |
| 宿主 | Fedora，Xeon E3-1270 v5（8 线程），16 GiB |
| 关键前提 | **宿主必须干净**：`load<8`、`podman ps -q` 为 0、`coredumpctl --since 60s` 无 SIGABRT |

## 矩阵

| 模式 | 3DMark Steel Nomad Light | 结果 zip | Geekbench 7 GPU | 判定 |
|---|---|---|---|---|
| **glibc-shim** | **205（基线）→ 754 → 1267 → 917（默认）** | 15-19 KB | PASS 61778-62619；FAIL 22788-23050（FM 记 0） | 能出分，最好的默认配置 **917**，距 >3000 差 3.3 倍 |
| nvidia-prolapse | 不可运行 | — | 19675（历史，可能落在 ORB 失败档） | **阻塞**：需 `sudo waydroid-nvidia-setup` |
| mesa-venus | 进不到 workload | — | ~10000（用户提供，未复测） | **阻塞**：3DMark 启动器在 ConstraintLayout solver 里 ANR |

## glibc-shim 的进展链（本轮的关键成果）

| 配置 | 分数 | fps | OODM | 说明 |
|---|---|---|---|---|
| 基线（block 图在 app 显存里） | 205 | 1.52 | 139 | 1.35 GiB 溢出宿主内存，每帧 3.9 GB 走 PCIe |
| block 图移 HAL 宿主内存（显式开） | **754 / 1267** | 5.59 | 16 | 588 张图全迁移，0 回退 |
| **同上，且设为默认** | **917**（无 setprop） | — | — | 已部署到两个 root |

根因链（`docs/bottleneck-verdict.md`）：ASTC 仿真 **6.1x** 膨胀（解码 RGBA8 5.90 GiB + block 1.22 GiB vs 原生 1.16 GiB）
→ 显存 **98.9%** 满 → 渲染期资源溢出到宿主内存 → 每帧 3.9 GB PCIe → 链路饱和（6.5-7 GB/s）→ 1.52 fps。
把 block 图移出后每帧 PCIe 降到约 1.1 GB，链路不再饱和。

## 已排除（都有判据）

| 假设 | 判据 | 结论 |
|---|---|---|
| 硬件/驱动慢 | 宿主原生 1500 draw = **0.599 µs/draw** | 否定 |
| HAL 路径慢 | guest 内同构基准 = **0.149 µs/draw**、填充 **79.4 Gpx/s** | 否定 |
| 每 draw 固定开销（ANGLE/descriptor） | 同上 + DRAW-DIST p50 仅 16-32 µs | 否定 |
| 宿主噪声 | 干净 vs 噪声仅 **+6%**（1.43→1.52） | 非主因（但必须清理） |
| 窗口/分辨率 | 2a：477×852 与 1900×1010 同分 | 否定 |
| CPU 侧翻译 | xlate_stats **0.02% of CPU** | 否定 |

## 下一步（按收益）

1. **ASTC → BC1**（`docs/astc-bc1-plan.md`）：app 可见需求 5.90 → **约 0.74 GiB**，
   预期 OODM 16 → 0，并大幅降低采样带宽。已验证 `textureCompressionBC=YES`、
   `BC1_RGBA_UNORM optimal=0x1d401`（SAMPLED+BLIT_SRC+TRANSFER_SRC/DST）。
2. 跑分脚本已修好点击检测（置信度 n>=600 且 flat>=0.85）。
3. prolapse / venus 待用户侧解阻塞。

## 产物索引

- 突破：`docs/BREAKTHROUGH-blocks-host-3.7x.md`、`artifacts/bench3d-20261031T175436-clean-bh2/`（754 分）
- 基线：`artifacts/bench3d-20261031T173102-clean-base/`（205 分 / 1.52 fps）
- 默认开：`artifacts/bench3d-20261031T181403-default-on2/`（917 分，无 setprop）
- 根因：`docs/bottleneck-verdict.md`、`docs/host-vs-guest-draw-cost.md`、`docs/host-noise-and-cleanup.md`
- ORB：`docs/orb-validation-analysis.md`、`docs/orb-probe-findings.md`、`tests/orb-probe/`
- venus：`docs/mode-venus-findings.md`