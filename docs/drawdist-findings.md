# 逐 draw 剖析结论：瓶颈是「每 draw 的固定启动成本」，不是填充率（2026-10-31）

## 决定性数据

`debug.nvhal.gputime=2`（inline render pass 内逐 draw 打 timestamp），steady workload 窗口：

```
=== window 2746 ms, 3 frames (1.1 fps), 8.0 submits/frame, 7.0 cmdbufs/frame
    busy 744.5 ms | timed regions 729.2 ms | draws(level 2) 585.2 ms   -> 79% 在 draw 里
```

### 各 pass 的「若每 draw 全屏覆盖」隐含填充率

| region | draws/frame | ms/frame | µs/draw | 隐含填充率 |
|---|---|---|---|---|
| 4096x4096 | 209 | 2500 | **11960** | 1.4 Gpx/s |
| 2048x2048 | 118 | 2163 | **18334** | 0.2 Gpx/s |
| 512x512 | 72 | 738 | **10248** | ~0 |
| 2560x1440 | 1 | 153 | **153220** | ~0 |
| 4096x4096 | 3 | 98 | 32554 | 0.5 Gpx/s |

**GTX 1080 的真实填充率约 100 Gpx/s**（26.6 Gtri/s、~100 Gpix/s）。
观察到的隐含填充率是 **0.2–1.4 Gpx/s**，即 **1/70 到 1/500**。

### 判读

- 单 draw 成本 **10–150 ms**，与 draw 的像素覆盖量**无关**（512x512 与 2560x1440 一样贵）。
- 若真是填充受限，512x512 的 draw 应该便宜 21 倍。它没有。
- ⇒ **每个 draw 都在付一个巨大的固定成本**（约 10–30 ms 量级），与画多少像素无关。
- 这个量级无法用「ZCULL 没生效」解释（ZCULL 失效最多让填充量回到全量，即多付 ~10 ms/帧，而不是每 draw 10–30 ms）。

## 因此候选收窄为两种

1. **draw 之间的隐式同步**：HAL 或驱动在每个 draw 前后插入栅栏/等待（我们的 HAL 没插 PipelineBarrier，但 ANGLE 层可能会，
   或者驱动 shim 的 fence 导出/导入在每 draw 生效）。
2. **每 draw 的 pipeline/状态切换病理**：若每个 draw 都触发一次 shader 重编译或 descriptor 重建，成本正好是毫秒级。

## 下一步（可判定）

1. **perf 抓 guest 进程的用户态栈**（`perf record -a` 已在宿主可用，guest 进程是宿主进程）：
   看 draw 期间 CPU 在 `vkCmdDraw` 里还是在驱动 shim 里自旋。若 CPU 在 HAL/ANGLE 里，就是 (1)。
2. **数 `vkCmdPipelineBarrier` 与 `vkQueueSubmit` 的次数**：gputime 已报 `8.0 submits/frame` 而 `draws/frame≈1500`，
   所以不是每 draw 一次 submit；但要确认 barrier 数（可在 ixlate/cmdstate 里加计数器）。
3. **对照宿主原生**：同一 workload 不可得，但可以用我们自己的微基准（大量小 draw，无贴图）在宿主直连 580 跑，
   量出「这台机器上一个空 draw 的成本」。若宿主也 ~10 ms/draw，则是驱动/硬件层；若宿主 ~2 µs，则差异在我们这层。

## 已排除（同批验证）

- 显存压力 / block 图占位（Run D：OODM 139→0，帧时间不变）
- 数据位置（宿主内存 vs VRAM）
- 窗口/分辨率设置
- CPU 侧 VkImage 翻译（0.02% of CPU）
- 每帧的提交次数（8 submits/frame，只占 busy 的 20%）
## 附：本轮实验的完整对照表

| 运行 | 开关 | 分数 | fps | app 可见 ASTC | OODM | 宿主内存分配 | GPU busy/帧 |
|---|---|---|---|---|---|---|---|
| base-580196 | 无 | **193** | 1.43 | 7.06 GiB | 139 | 1352 MiB | 763–955 ms |
| blocks-host-t1 | astc_blocks_host（首版，静默失效） | — | — | 7.12 GiB | 139 | 1352 MiB | — |
| blocks-host-t4 | astc_blocks_host（tier-4 修复后） | 未完成 | ~1.2 | **5.84 GiB** | **0** | **0** | **669–955 ms** |
| drawdist-t3 | gputime=2 | **190** | 1.41 | — | — | — | 709–745 ms |
| memtarget-7 | exp.memtype_target=7 | 0（workload 未跑起来，宿主类型被吸进 VRAM 后同步失败） | — | — | 0 | — | — |

**读法**：把 1.22 GiB block 图从显存移走、OODM 清零、宿主内存分配清零之后，
帧时间与分数**都没有改善**。所以慢与显存压力无关。
逐 draw 数据进一步给出：每 draw 付 10–30 ms 固定成本，与像素覆盖量无关。
