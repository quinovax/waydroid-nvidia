# 本轮状态（2026-10-31 18:45）——目标未达成，但有实质突破

## 验收对照（诚实）

| 验收项 | 要求 | 实际 | 状态 |
|---|---|---|---|
| glibc-shim 3DMark | >3000 分 | **最佳 1267（9.39 fps）**；默认配置典型 898-917 | ❌ 差 2.4-3.3 倍 |
| 结果 zip | >10 KB | 19-23 KB | ✅ |
| nvidia-prolapse 出分 | — | 不可安装 | ❌ 阻塞（需 `sudo waydroid-nvidia-setup`） |
| mesa-venus 出分 | — | 进不到 workload（启动器 ANR） | ❌ 阻塞 |
| Geekbench ORB 不记 0 | — | 仍间歇（3 跑 2 挂） | ❌ 未消除 |
| dev/bench3d | 交付 | 已扩展并修好点击检测 | ✅ |
| 三模式对比表 | 交付 | `docs/three-mode-matrix.md`（已更新） | ✅ |
| 热点修复补丁 | 交付 | `astc_blocks_host` 默认开（3.7-6.2x）+ 上游 SYNC_FD | ✅ 有补丁，但未达标 |
| docs 能力矩阵 | 更新 | 21 篇 | ✅ |

## 本轮实测（全部干净宿主环境）

| 跑分 | `astc_blocks_host` | 分数 | fps | zip |
|---|---|---|---|---|
| clean-base | 关 | **205** | 1.52 | 15049 B |
| clean-bh2 | 开 | **754** | 5.59 | 19442 B |
| bh-repro | 开 | **1267** | **9.39** | 23074 B |
| default-on2 | **默认开** | **917** | 6.80 | 23370 B |
| stable-917 | 默认开 | **915** | 6.78 | 21038 B |
| post-exp-default | 默认开 | **898** | 6.66 | 19860 B |

**默认开相对基线提升 4.4-6.2 倍**（898-1267 vs 205），zip 全部 >10KB。
但距 >3000 仍需再提 2.4-3.3 倍。

## 本轮做了什么

1. **修正重大误判**：上一轮说「block 图移出显存无改善」是错的——那次跑分**没有产出 zip**，
   我把「未完成」读成了「无改善」。干净环境重测得到 754→1267。
2. **设为默认**（`bh_init()` 默认 ON，显式 0 才关），已部署两个 root、验证、提交（`6f065c7`）。
3. **修掉 `dev/bench3d` 的真实缺陷**：冷启动后过早截图会把未画完的圆盘（n≈360-515/flat≈0.71-0.80）
   当成按钮，质心落在 hero 插画上，产生 973,533 / 978,537 假坐标，毁掉两次有效测量。
   收紧到 `n>=600 && flat>=0.85`，并先轮询焦点、再等 8s。修复后稳定为 `1841,403 n=1059 flat=0.92`。
4. **decoded_host 边界实验**（`1e256e4`）：把解码图也放到宿主内存，显存降到 1312 MiB，
   但 workload 报 `Could not get memory for an image!`（历史类 A 错误），HAL 无报错 ⇒ ICD 拒绝。
   **证明纹理必须留在 VRAM** ⇒ 唯一可行方向是**缩小纹理**（BC1），而不是搬走。
5. 更新 `docs/three-mode-matrix.md`（`4d5b9fb`）。

## 为什么 205 → 900+（因果链已闭环）

ASTC 仿真 **6.1x** 膨胀（解码 RGBA8 5.90 GiB + block 1.22 GiB，原生仅 1.16 GiB）
→ 8 GiB 显存 **98.9%** 满 → 渲染期 `OUT_OF_DEVICE_MEMORY` **139 次**，1.35 GiB 溢出宿主内存
→ 每帧 **3.9 GB** 走 PCIe → 链路饱和（6.5-7 GB/s）→ 1.52 fps。

把 1.22 GiB block 图移出后：OODM **139 → 16**，每帧 PCIe 降到约 **1.1 GB**，链路不再饱和 → 5.6-9.4 fps。

## 已排除（都有判据）

| 假设 | 判据 |
|---|---|
| 硬件/驱动慢 | 宿主原生 1500 draw = 0.599 µs/draw |
| HAL 路径慢 | guest 内同构基准 = **0.149 µs/draw**、填充 **79.4 Gpx/s** |
| 每 draw 固定开销 | 同上 + DRAW-DIST p50 仅 16-32 µs |
| 纹理搬到宿主内存 | decoded_host 实验：ICD 拒绝（`Could not get memory for an image!`） |
| 宿主噪声 | 干净 vs 噪声仅 +6%（193→205） |
| CPU 侧翻译 | xlate_stats 0.02% of CPU |
| 窗口/分辨率 | 2a：477×852 与 1900×1010 同分 |

## 下一步（唯一剩余路径）

**BC1**（`docs/astc-bc1-plan.md`）：解码目标 RGBA8 → BC1，0.5 B/texel（RGBA8 是 4 B/texel）
⇒ 5.90 GiB → **约 0.74 GiB**，OODM 16 → 0，采样仍走 VRAM。

已验证前提：`textureCompressionBC = YES`、`BC1_RGBA_UNORM optimal=0x1d401`
（含 SAMPLED_IMAGE/BLIT_SRC/TRANSFER_SRC/DST）。

实施要点（可行性已确认，工作量较大）：
1. `efmt` 增加 BC1 目标格式（仅 4x4 ASTC，占实测 588 张中的 378 张）；
2. 解码链改为：`block 图 →(现有 astc_decoder.glsl)→ 临时 RGBA8 图 →(新 bc1_encode.comp)→ BC1 块 buffer → vkCmdCopyBufferToImage → BC1 图`；
3. BC 不能做 storage image，故必须经 buffer；readback 仍从 block 图逐位往返；
4. 开关 `debug.nvhal.astc_bc1` 默认关，失败回退 RGBA8。

## 风险

- blocks_host 各次跑分方差较大（754 / 1267 / 898-917），说明对宿主状态敏感；
  正式验收应取多次中位数并固定宿主卫生条件。
- BC1 有损（色带），3DMark 计分不看像素保真，但 Geekbench 的 ORB 可能受影响，需单独验证。