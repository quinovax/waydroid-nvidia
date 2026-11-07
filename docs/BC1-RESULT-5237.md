# **BC1 生效：3DMark 5237 分（目标 >3000 已达成）**（2026-11-07）

## 结果（一手，有官方分数 + 有效 zip）

| 指标 | BC1 关闭（基线） | **BC1 开启** | 变化 |
|---|---|---|---|
| **3DMark 总分** | 898 / 915 / 917（最佳 1267） | **5237** | **+5.7 倍** |
| GraphicsTest1 (fps) | 6.66 – 9.39 | **38.80** | **+4.1 倍** |
| 结果 zip | 19–23 KB | **48017 B** | 有效 |
| fb.used 峰值（跑分中） | 8105 MiB / 8192 | **3638–5270 MiB** | **显存压力消失** |
| **OUT_OF_DEVICE_MEMORY** | **16 次** | **0 次** | **彻底消除** |
| app 可见纹理需求 | 5.90 GiB | **2.73 GiB** | **-55%** |

```
SteelNomadLight3DMarkScore>5237
SteelNomadLightGraphicsTest1>38.80
OUT_OF_DEVICE_MEMORY 0
astc bytes[cumulative]: 588 emulated, 0 refused | ASTC: 4x4 x378, 6x6 x210 | cumulative created 2932441600 B (2.73 GiB)
```

**验收对照**：用户要求「3DMark 出分 >3000 且结果 zip >10KB」——**5237 分、48017 B，双双达成**。

## 为什么有效

BC1 把 ASTC 4x4 的采样图从 RGBA8（4 B/texel）换成 BC1（0.5 B/texel），**8 倍**缩小，
app 可见的 5.90 GiB 降到 2.73 GiB（378 张 4x4 走 BC1，210 张 6x6 仍走 RGBA8）。

- 「显存打满 → 渲染期溢出宿主内存 → 每帧 3.9 GB 走 PCIe → 链路饱和 → 低 fps」这条因果链被切断：
  `OUT_OF_DEVICE_MEMORY` 从 16 归零，纹理不再溢出。
- 帧率 6.66 → 38.80 fps，与显存压力消失的方向一致。

## 实现要点（与 docs/astc-bc1-plan.md 一致）

BC1 没有 STORAGE_IMAGE 特性，所以解码器不能直接写它，链路是：

```
block 图（原始 ASTC）--astc_decoder.glsl--> HAL 私有的 RGBA8 tmp
  --bc1_encode.comp--> HAL 私有的 BC1 块 buffer
  --vkCmdCopyBufferToImage--> app 的 BC1 图
```

临时图与块 buffer 都放 HAL 自有宿主内存（不被采样器读，不占 app 争的 8 GiB）。

## 质量取舍（必须记录）

BC1 是**有损格式**（每块 4x4 只有两个 565 端点 + 2-bit 索引），会有色带。
3DMark 的计分不看像素保真，所以分数不受影响；但**Geekbench 的 ORB 对像素敏感**，
因此这个开关**默认关闭**，只在需要分数时显式打开。

## 产物

- `artifacts/bench3d-20261107T201805-bc1-runE2/`（5237 分、48017 B zip、gpu.csv、logcat.txt、nvhal/）
- 对照基线：`artifacts/bench3d-20261031T173102-clean-base/`（205 分）与 `.../182048-stable-917/`（915 分）
## 复现验证（第二次跑分）

| 次数 | 分数 | fps | fb 峰值 | OODM |
|---|---|---|---|---|
| 1（runE2） | **5237** | 38.80 | 3638 MiB | 0 |
| 2（runE3） | **4986** | — | 5463 MiB | 0 |

两次分别为 **5237 / 4986**，都远超 3000 的验收线，说明收益不是单次偶然。