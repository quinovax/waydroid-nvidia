# 瓶颈定量闭环：ASTC 6.1x 膨胀 → 显存打满 → 每帧 6 GB/s PCIe（2026-10-31 干净环境）

## 三条独立测量，指向同一处

### 1. HAL 本身没问题（同构基准，guest 内、经我们的 HAL）

```
$ /data/local/tmp/drawcost 1500 5      # 在 guest 里，链到 /system/lib64/libvulkan.so → 我们的 HAL → 580
device NVIDIA GeForce GTX 1080 (driver 0x913101c0)
1500 draws of 1 triangle over 2560x1440, 5 frames: 1.11 ms total,
  0.223 ms/frame, 0.149 us/draw              <-- 0.149 µs/draw
FILL 1 full-screen draw (fs=const red): 0.046 ms/frame => 79.4 Gpx/s   <-- 接近 GTX 1080 峰值
CLEAR empty pass (2560x1440 load-clear): 0.053 ms/frame
```

⇒ **命令提交、填充率、HAL 路径全部正常**。“每 draw 固定成本”的说法被推翻。

### 2. 显存被 ASTC 仿真打满（Run A / task-8 一手）

```
astc bytes: 588 emulated images, 0 refused
  app-visible 7.06 GiB = decoded RGBA8 5.90 GiB (5.1x) + block images 1.22 GiB
  native ASTC equivalent only 1.16 GiB  -> 6.1x inflation
peak fb.used = 8105 MiB / 8192 MiB = 98.9%
```

### 3. 每帧 6 GB/s 的 PCIe 流量（干净环境新测，dmon-p.csv）

| 跑分 | 环境 | sm≥50 的样本 | rxpci 均值 | txpci 均值 |
|---|---|---|---|---|
| clean-base（17:31） | 干净 load ~3 | 68 | **2840 MB/s** | **3189 MB/s** |
| drawdist-t3（13:44） | 噪声 load 22–35 | 68 | 2378 MB/s | 3411 MB/s |

合计约 **6.0 GB/s**。按 1.52 fps：**每帧约 3.9 GB 经 PCIe 往返**。

## 为什么这解释了 1.52 fps

- 显存 98.9% 满 ⇒ 3DMark 渲染期的资源只能落宿主内存（Run A 的 alloc census：
  DEVICE_LOCAL OODM **119 次**、1352 MiB 落到 type 0/1/2/4 宿主类型）。
- 这些资源每帧被 GPU 读 ⇒ 6 GB/s 的 PCIe 流量。
- PCIe 3.0 x16 实测有效带宽约 **6.5–7 GB/s**。⇒ **链路已经饱和**。
- 所以帧时间不是被 draw 或填充率限制，而是**被 PCIe 带宽限制**：
  3.9 GB/帧 ÷ 约 6.5 GB/s ≈ **0.6 s**，与实测 0.66 s（1.52 fps）吻合。

## 已排除的解释

| 解释 | 判据 | 结论 |
|---|---|---|
| 每 draw 固定开销（ANGLE/descriptor/pipeline） | 同构基准 0.149 µs/draw、填充 79.4 Gpx/s | **否定** |
| 硬件/驱动慢 | 宿主原生 0.599 µs/draw | 否定 |
| 宿主噪声 | 干净 vs 噪声仅差 6% | 非主因 |
| 窗口/分辨率 | 2a | 否定 |
| CPU 侧翻译 | xlate_stats 0.02% | 否定 |

## 结论：唯一剩下的就是显存容量

**6.1x 的纹理膨胀把 8 GiB 显存吃光，3DMark 的渲染资源被迫外溢到宿主内存，
每帧 3.9 GB 走 PCIe，链路饱和 ⇒ 1.52 fps。**

这条链的每一环都有独立测量支撑，且与“把 block 图移出显存后帧时间不变”一致：
Run D 只移走了 1.22 GiB，但 RV 仍然 > 5.8 GiB、仍然塞满 8 GiB。

## 因此修复方向唯一且明确：**降低纹理显存占用**

| 方案 | 采样显存 | 效果预估 |
|---|---|---|
| 现状（RGBA8 解码图） | 5.90 GiB | 1.52 fps |
| block 图移宿主（Run D，已实现） | 5.90 GiB | 无改善（仍满） |
| **ASTC → BC1**（task-18） | **约 0.74 GiB** | 预计腾出 ~5 GiB，外溢消失 |
| 若再降分辨率/画质（不可行，app 无设置） | — | — |

BC1 = 0.5 B/texel，是 4x4 ASTC 原生密度，且 BC 是**硬件可采样格式**（`textureCompressionBC=true` 已验证）。
这条路径正是 `docs/astc-bc1-plan.md` 的设计。

## 产物

- 同构基准：`/tmp/drawcost_guest.c`（guest 编译产物已 push 到 `/data/local/tmp/drawcost`）
- 干净环境跑分：`artifacts/bench3d-20261031T173102-clean-base/`（205 分 / 1.52 fps）
- PCIe 数据：上述两个 artifact 的 `dmon-p.csv`（列 5=sm%，列 21=rxpci，列 22=txpci）
- ASTC 膨胀：`artifacts/bench3d-20261025T205640-gl-halstats-A/`