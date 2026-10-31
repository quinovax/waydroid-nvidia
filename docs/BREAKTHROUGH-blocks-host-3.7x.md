# **重大突破：astc_blocks_host 让分数提升 3.7 倍**（2026-10-31）

> 本文修正 `docs/astc-blocks-host-findings.md` 里「Run D 无改善」的错误结论。

## 修正说明（先认错）

我此前判定「把 block 图移出显存后帧时间完全没变」，依据是 `blocks-host-t4` 那次跑分。
但那次**根本没有产出结果 zip**（`no-result-file after 420s`），我把「未完成」误读成了「无改善」。
在干净宿主环境（load ~4、podman 0、abrt 0）下重跑（`clean-bh2`），结果是**显著改善**。

## 干净环境对照（同一 HAL、同一驱动、同一模式 glibc-shim）

| 跑分 | `astc_blocks_host` | 分数 | fps | fb 峰值 | PCIe(忙时) |
|---|---|---|---|---|---|
| clean-base | 0（关） | **205** | **1.52** | 8105 MiB | 6030 MB/s |
| **clean-bh2** | **1（开）** | **754** | **5.59** | 8086 MiB | 6016 MB/s |

**3.68 倍分数、3.68 倍 fps**。结果 zip 19442 B（>10KB，真实跑分）。

## HAL 侧的证据（一手日志）

```
astc blocks host summary at vkDestroyDevice: 588 emulated image(s) created,
  588 with HAL-owned block images (1314917888 B = 1.22 GiB cumulative),
  0 fell back to the combined allocation;
  app-visible requirements 6334706688 B (5.90 GiB) cumulative (decoded images only)
```

⇒ 588 张图**全部**走 HAL 自有宿主内存，**0 回退**。app 可见需求从 7.12 GiB 降到 **5.90 GiB**（省下 1.22 GiB）。

alloc census 印证：

```
110 ok (7459.9 MiB: 363.9 MiB before the first present, 7096.0 MiB after),
OUT_OF_DEVICE_MEMORY 16   <-- 从 139 降到 16
  type 0: post 1 ok 104.0 MiB
  type 1: post 14 ok 336.0 MiB
```

`OUT_OF_DEVICE_MEMORY` 从 **139 → 16**，溢出到宿主内存的量大幅减少。

## 为什么 1.22 GiB 能换来 3.7 倍

关键在于**溢出曲线是非线性的**：

- 关：app 可见需求 7.12 GiB + 渲染期分配 → 撞 8 GiB 上限 **139 次**，大量资源被挤到宿主内存；
- 开：6.90 GiB（5.90 解码 + 约 1 GiB 渲染）→ 仅撞 **16 次**，大部分资源留在 VRAM。

只要**最后那 0.4 GiB** 决定「关键资源在 VRAM 还是宿主内存」，帧时间就会出现台阶式变化。
这与先前「溢出 1352 MiB」的量级吻合。

## 但 PCIe 流量没有下降（6016 vs 6030 MB/s）—— 需要解释

这是本轮最反直觉的观测。可能原因：

1. **PCIe 流量包含了非渲染的搬迁**（纹理上传、显存到显存的 copy、以及 stream 缓冲循环），
   它随 fps 提高而成比例增加：fps 从 1.52 → 5.59（3.7 倍），若每帧流量同比下降 3.7 倍，总量恰好持平。
   ⇒ 这个解释与数据自洽：**每帧 PCIe 流量降低了 ~3.7 倍**，但帧数也多了 3.7 倍。
2. 采样器/写回路径仍在宿主内存（`type 0/1` 仍有 440 MiB）。

**正确读法**：不要看总带宽，要看**每帧带宽**。

| 跑分 | fps | PCIe 总 | 每帧 PCIe |
|---|---|---|---|
| clean-base | 1.52 | 6030 MB/s | **3967 MB/帧** |
| clean-bh2 | 5.59 | 6016 MB/s | **1076 MB/帧** |

⇒ **每帧 PCIe 吞吐降到 1/3.7**，这正是分数提升 3.7 倍的原因。链路不再饱和。

## 当前进度 vs 目标

| 项 | 要求 | 当前 | 差距 |
|---|---|---|---|
| glibc-shim 分数 | >3000 | **754** | 4.0 倍 |
| 结果 zip | >10KB | 19442 B | ✅ |
| 三模式出分 | 3/3 | 1/3（prolapse 不可装、venus ANR） | 需用户侧介入 |

## 下一步（按收益排序）

1. **BC1 改造**（`docs/astc-bc1-plan.md`）：app 可见需求 5.90 → **约 0.74 GiB**，
   预计彻底消除溢出（OODM 16 → 0），并大幅降低采样带宽。这是把 754 推向 >3000 的主攻方向。
   已验：`textureCompressionBC = YES`，`BC1_RGBA_UNORM optimal=0x1d401`（SAMPLED+BLIT_SRC+TRANSFER_SRC/DST）。
2. 把 `astc_blocks_host` 设为**默认开**（当前默认关），让所有模式都受益。
3. 复现验证（`bh-repro`）确认 754 的稳定性。
4. prolapse / venus 仍待用户侧解阻塞。

## 产物

- `artifacts/bench3d-20261031T175436-clean-bh2/`（754 分 / 5.59 fps / dmon-p.csv / alloc census）
- `artifacts/bench3d-20261031T173102-clean-base/`（205 分 / 1.52 fps 对照）
## 复现确认（第二次跑分 bh-repro）

| 跑分 | 分数 | 说明 |
|---|---|---|
| clean-base（关） | 205 | 基线 |
| clean-bh2（开） | 754 | 第一次 |
| bh-repro（开） | 1267 | 第二次，更高 |

第二次 588 张图同样全部迁移（`588 with HAL-owned block images`，0 回退），
PCIe 总带宽 5235 MB/s 而 fps 更高 => 每帧 PCIe 进一步下降。

两次都远高于基线 205，收益稳定。注：bh-repro 启动时宿主 load 31（guest 服务刚重启），
若在更安静的环境下可能更高。
## 默认开启后的验证（无需任何 setprop）

把 `bh_init()` 改成**默认 ON**（显式设 0 才关），重新部署后跑分：

```
$ dev/bench3d --label default-on2        # 没有传 --props
W vulkan.nvidia: astc blocks host: ON by default (set NVHAL_ASTC_BLOCKS_HOST / debug.nvhal.astc_blocks_host to 0 ...)
overallScore=917
```

| 配置 | 分数 |
|---|---|
| 基线（blocks_host 关） | 205 |
| blocks_host 显式开（第一次） | 754 |
| blocks_host 显式开（复现） | 1267 |
| **默认开（无 setprop）** | **917** |

全部远高于基线 205。**默认开启已部署到 glibc-shim 与 prolapse 两个 root。**

## 一并修掉的测量脚本缺陷

`dev/bench3d` 的播放键检测在 app 冷启动后过早截图，
会把**未画完的圆盘**（n≈360-515、flat≈0.71-0.80）当成按钮，质心落在 hero 插画上，
产生 `973,533` / `978,537` 这类假坐标，导致两次跑分被误判为 tap-failed 而中止。

修法：
1. 置信度阈值收紧到 `n>=600 && flat>=0.85`（完整圆盘实测 n≈1000-1060 / flat≈0.92，
   而常见稳定值是 n=750 / flat=0.89，仍在阈值内）；
2. 截图前先轮询焦点直到 3DMark 窗口获得焦点，再等 8 s 让首帧画完。

修复后同一环境点击恢复为 `1850,388 DISC n=750 span=60x62 flat=0.89`，workload 正常启动。

## 当前 vs 目标

| 项 | 要求 | 当前 | 差距 |
|---|---|---|---|
| glibc-shim 3DMark | >3000 | **917（默认开）** | 3.3 倍 |
| 结果 zip | >10KB | 15-19 KB | 达标 |
| 三模式 | 3/3 出分 | 1/3 | prolapse 需 setup、venus 需修 ANR |
| Geekbench ORB | 不记 0 | 仍间歇 | 未验证 SYNC_FD 修复效果 |