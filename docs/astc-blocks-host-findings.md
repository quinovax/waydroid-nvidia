# ASTC block 图迁移实验（task-17 / Run D）与结论 — 2026-10-31

## 改了什么

`nvidia-prolapse/hal/src/astc.c`，开关 `debug.nvhal.astc_blocks_host`（默认 0）：

- 开启后，每张被仿真的压缩图的 **block 图**（原始压缩位）不再绑在 app 自己的 VkDeviceMemory 里，
  而是由 HAL 单独分配一块宿主内存并绑定；**app 可见的 memory requirements 只算解码图**。
- barrier 镜像、upload→block→解码、copyImageToBuffer 从 block 图逐位回读，全部保持原语义。
- 关闭时行为逐字节不变（只在 vkCreateImage/AllocateMemory/DestroyDevice 各多一次 acquire load）。

## 关键 bug（已修，commit 71006f7）

第一版 `bh_type()` 只接受 HOST_VISIBLE 类型，结果 **588 张图 0 张迁移、0 张回退**——开关静默失效。
根因：block 图是 `R32G32B32A32_UINT` / `R32G32_UINT` 的 OPTIMAL 图像，NVIDIA ICD 给它的
`memoryTypeBits = 0x82`（只有 type 1 和 type 7）。type 1 是 heap 1 宿主内存，但 **flags = 0x0**，
没有 HOST_VISIBLE 位（驱动根本不打算让它被 map）。而 block 图只被 copy 和 dispatch 触碰、CPU 从不解引用，
所以补了第 4 档：**任意非 DEVICE_LOCAL（且非 LAZILY_ALLOCATED）的类型**。

## 实测（Run D，artifacts/bench3d-20261031T132842-blocks-host-t4）

| 项 | 基线（base-580196） | Run D（blocks_host=1） |
|---|---|---|
| 结果 | 193 分 / 1.43 fps / zip 22.9 KB | **未在 420 s 内完成**（帧率未变，workload 只是跑得久） |
| fb.used 峰值 | 8102 MiB | 7973 MiB |
| app 可见 ASTC 需求 | 7.06 GiB（解码 5.84 + block 1.22） | **5.84 GiB（只解码）** |
| **OUT_OF_DEVICE_MEMORY** | **139 次** | **0 次** |
| 宿主内存里的分配 | 1352 MiB（type 0/1/2/4） | **0**（7019.9 MiB 全在 type 7 VRAM） |
| GPU busy / 帧 | 763-955 ms | 669-955 ms（**无改善**） |

## 结论（否定性，很重要）

**「显存被 ASTC 解码图撑满 → 渲染期分配溢出到宿主内存 → 每帧经 PCIe 往返 → 1.2 fps」这条链被证伪。**
把 1.22 GiB block 图移出显存后，OODM 从 139 归零、宿主内存分配从 1352 MiB 归零——
**帧时间完全没有变化**。慢的不是数据位置。

于是先前观察到的 2.1 GB/s PCIe 流量是「结果」而不是「原因」：它是溢出发生后必然产生的现象，
移走溢出后 GPU 仍然每帧忙 780 ms。

## 下一步该测什么

剩下的候选全部落在「那个 2560x1440 / 1526 draw 的 render pass」本身（256 ms/帧，约 168 µs/draw）：

1. **DRAW-DIST**（`debug.nvhal.gputime=2`，已实现并已部署）：每个 render pass 的逐 draw
   min/p50/p90/max + 直方图。均匀 → 每 draw 本身就贵；长尾 → 少数 draw 特别贵。
2. 若均匀：再看是否被 1526 次 draw 的固定开销吃掉（每 draw 的同步/状态切换），
   用 `debug.nvhal.gputime=1` 的 submits/cmdbufs 计数与 host 侧 perf 交叉验证。
3. 若长尾：抓那几个 draw 的 pipeline/shader 哈希，比对 shader 缓存与编译。

## 产物

- Run D：`artifacts/bench3d-20261031T132842-blocks-host-t4/`（gpu.csv / dmon-t.csv / logcat.txt / nvhal/）
- 基线：`artifacts/bench3d-20261030T223056-base-580196/`（193 分，zip 22.9 KB）
- 首次（开关静默失效）那次：`artifacts/bench3d-20261030T224225-blocks-host-t1/`