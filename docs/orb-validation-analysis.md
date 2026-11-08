# Geekbench 7 / Feature Matching (ORB) 验证失败分析

- 任务：task-5（owner orb-analyst），写作者仅限本文件；其余全部只读
- 日期：2026-10-25
- 结论性质标注：**[已证实]** = 有原始一手证据；**[强推断]** = 由多条一手证据推出；**[推测]** = 尚无直接证据

---

## 0. TL;DR

1. **[已证实] Geekbench 装着呢**，包名 `com.primatelabs.parkdale`（versionName 7.1.0，versionCode 703456）。
   任务书里"Geekbench 似乎没安装"的前提**是错的**（之前 `pm list packages -3 | grep geek` 匹配不到，是因为包名里没有 "geek"）。
   → **P3 实跑实验可行**，只是必须由 bench-operator（唯一 GPU 持有者）执行，见 §5。
2. **[已证实] 失败永远是同一个子项：Feature Matching**。另外 10 个子项在所有记录中都是 valid=1。
   guest 内 `history.db` 里三次 Flutter 之外的 GPU 记录，失败那两次的其余子项分数与成功那次几乎相同 →
   总分 22890 / 22821 vs 62619 的差距**完全来自 Feature Matching 被记 0**。
3. **[已证实] 三次历史 GPU 跑分都用 Vulkan**（`history.db` 里 `"gpu_api": 6` + 字面量 "Vulkan"；
   且 `libgeekbench.so` 里有 `vkCreateComputePipelines / vkCmdDispatch / vkCmdDispatchIndirect / vkCmdPipelineBarrier` 字面量）。
   **重要修正**：记忆文件里那句 "ANGLE (NVIDIA, Vulkan 1.4.312 …)" 来自 `dumpsys SurfaceFlinger` 的 GLES 行（本次实测逐字复现），
   那是**系统 GLES/显示**栈，不是 Geekbench 跑分用的 API。所以"ANGLE 共有层"不是根因候选。
4. **[强推断] 不是 NVK 独有。** 失败在**闭源 580 的 glibc-shim 路径上出现过至少两次**（10-18 22:59、10-24 19:51），
   同一模式同一栈在 10-24 21:40 又完全通过。即：同一配置 3 次里失败 2 次 → **间歇性**，且**跨驱动**。
5. **[推测] 最可信的方向**：三模式共享的那一层（guest gralloc/minigbm 分配 + 我们的 HAL dma-buf/AHB 转换 + 闭源内核模块 580.178.04）
   在 "GPU 原子写 → CPU 读回" 这条链上偶发不一致；NVK 侧另有独立的 barrier/indirect 风险点。
   目前**没有一条根因被证实**，需要 §5 的 E1/E2/E3。

---

## 1. 事实基线（全部一手）

### 1.1 Geekbench 安装与形态
| 项 | 值 | 来源 |
|---|---|---|
| 包名 | `com.primatelabs.parkdale` | guest `pm path` |
| 版本 | versionName 7.1.0 / versionCode 703456 | guest `dumpsys package` |
| codePath | `/data/app/~~szIgCXaNdOTKWAFwMmia6g==/com.primatelabs.parkdale-15E3pE6Ca_Q1LgtMjfBEEA==/` | guest |
| 原生库 | `lib/x86_64/libgeekbench.so`（148,098,472 B）、`libc++_shared.so`（9 MB）在 `split_config.x86_64.apk` | guest |
| workload 数据 | `split_workload_pack.apk` = 单个 `assets/geekbench-workload.plxr`（463,652,452 B），**不可 grep 到 shader** | guest |
| 历史库 | `/data/data/com.primatelabs.parkdale/files/history.db`（151,552 B） | guest |
| NVK shader cache | `/data/data/com.primatelabs.parkdale/code_cache/nvk_shader_cache/<2-hex>/...` | guest（nvidia-prolapse 跑过 NVK 的直接证据） |

### 1.2 失败信号与源码位置
- 字符串 `src/geekbench/workload/gpu/orb_gpu_workload.cpp`：**已证实**存在于 `libgeekbench.so`。
- 格式串 `{}: difference is > {}%`：已证实存在（与用户引用的 `orb_gpu_workload.cpp(182)` 一致）。
- 实测样本（用户提供）：`GPU:208, CPU:162: difference is > 10%` → workload 503 记 0 分。
  `(208-162)/162 = 28.4%`，**远大于 10% 阈值**，不是"擦边没过"。
- 只有 Feature Matching 会失败 → 10 个子项里唯一一条"GPU 结果要和 CPU 参考结果对齐"的判定。

### 1.3 GPU API（关键修正）
`libgeekbench.so` 里同时存在两套后端，说明它会按设备选择：
- Vulkan 计算：`vkCreateInstance / vkGetInstanceProcAddr / vkCreateComputePipelines / vkCmdDispatch / vkCmdDispatchIndirect / vkCmdPipelineBarrier`，依赖 `libvulkan.so`
- OpenCL：`clCreateContext / clCreateProgramWithSource / clEnqueueNDRangeKernel`，依赖 `libOpenCL.so`
- **没有**任何 `glDispatchCompute / glMemoryBarrier / EGL / GLES` 字面量 → 跑分**不走** GLES。

三次历史记录的判定：`history.db` 中 `"gpu_api": 6`（3 条记录共 5 次出现）+ "Vulkan" 字面量 10 次 → **[已证实] 这 3 次都是 Vulkan**。
注意 `vkCmdDispatchIndirect` 存在 → **ORB 很可能用间接 dispatch**（关键点压缩/直方图的典型写法），这是 NVK pre-Turing 的敏感路径。

**环境对照**（本次实测 `dumpsys SurfaceFlinger`）：
`GLES: Google Inc. (NVIDIA), ANGLE (NVIDIA, Vulkan 1.4.312 (NVIDIA NVIDIA GeForce GTX 1080 (0x00001B80)), NVIDIA-580.178.4.0), OpenGL ES 3.2 (ANGLE 2.1.28324 …)`
→ 这句与 memory 里 22890 那次的 "GPU 识别" **逐字相同**，证明当时记录的是 SurfaceFlinger 的 GLES 行。
→ 但也顺带证明：**glibc-shim 模式下 guest 的 Vulkan 驱动是闭源 580.178.4.0**（ANGLE 把它嵌进了 renderer 串）。
props：`ro.hardware.egl=angle`、`ro.hardware.vulkan=nvidia`、`ro.hardware.gralloc=minigbm_gbm_mesa`。

### 1.4 三次历史跑分（guest history.db 原始 JSON 解出）
| # | 日期(UTC) | URL | GPU Score | top-level valid | Feature Matching | 其余 10 项 |
|---|---|---|---|---|---|---|
| 1 | 2026-10-18 14:59:44 | browser.geekbench.com/v7/gpu/**302060** | 22890 | 0 | **score 0, valid 0** | 全 valid=1 |
| 2 | 2026-10-24 11:51:21 | .../v7/gpu/**321972** | 22821 | 0 | **score 0, valid 0** | 全 valid=1 |
| 3 | 2026-10-24 13:40:10 | .../v7/gpu/**322325** | 62619 | 1 | score 62635, valid 1 | 全 valid=1 |

逐项分数（证明"只挂一项"）：
- run1: Blur 74146 / FaceTrack 62965 / SuperRes 132156 / Horizon 63239 / PhotoF 70378 / VideoF 53311 / RAW 77005 / PathT 47162 / Particle 40435 / Fluid 42055
- run2: Blur 75761 / FaceTrack 65431 / SuperRes 139490 / Horizon 58933 / PhotoF 66913 / VideoF 53477 / RAW 76536 / PathT 47996 / Particle 39556 / Fluid 41260
- run3: Blur 78274 / FaceTrack 63905 / SuperRes 134135 / Horizon 62552 / PhotoF 67900 / VideoF 55533 / RAW 73950 / PathT 47676 / Particle 40253 / Fluid 41262

**[强推断] run1/run2 是闭源快路径（glibc-shim）而非 NVK**：NVK 的同类子项要慢 1.7-6.5×（见 2026-10-18.md:793-804 的 19675 那次：Blur 只有 17131），
而 run1/run2 的 Blur 是 74146/75761 —— 只有闭源 580 能给出这个量级。memory 也明确记载 run1（22890）是 "glibc-shim 模式，nvidia_native_gl=off"。
**[已证实] 10-24 同一天 2 小时内：19:51 失败 → 21:40 通过**（同模式同栈）→ 间歇性。

> **caveat（诚实）**：run1 的"GLES 走 ANGLE"是 memory 的说法，但 1.3 已证明跑分走 Vulkan；
> 两者不冲突（ANGLE 只是显示/GLES 栈），但**不要在后续文档里把 22890 记成 "ANGLE 跑分"**。

### 1.5 E1 实测（2026-10-25 20:xx，glibc-shim，bench-operator 执行；**一手 logcat**）
同一模式、不重建、不切模式，连续 3 次 Geekbench 7 GPU：

| run | 结果 | Feature Matching | GPU 原始行 / 总分 |
|---|---|---|---|
| 1 | **valid=1（过）** | 62792（118.5 Mpixels/sec） | logcat 里**连 `difference is` 都没有**；GPU Score 61778 |
| 2 | **valid=0（挂）** | 0（被清零） | `I geekbench: [1025/203606:ERROR:orb_gpu_workload.cpp(182)] GPU:201, CPU:162: difference is > 10%` + `gpu_workload.cpp(190) workload 503 failed validation`；结果页出现 "One or more workloads have failed validation…"；总分 23050 |
| 3 | **valid=0（挂）** | 0（被清零） | `GPU:203, CPU:162` + `workload 503 failed validation`；结果页同样警告；总分 22788 |

**这就把 §5-E1 的判定表打在了"竞态"那一行**（同一模式、同一栈，1 过 1 挂；10-24 同一天也是 11:51 挂 / 13:40 过）。

**新证据：失败样本的符号是恒定的。** 目前三次可得样本的 CPU 参考值**都是 162**（用户样本 208/162、E1 201/162），
GPU 值**永远偏高** 24-28%（208/162=+28.4%，201/162=+24.1%）。
真正的随机内存污染通常应该**双向**且幅度更大；"恒定偏高"更像**多算了一批匹配 / 多跑了一遍 pass**
（例如同一批 candidate 被重复计入、或某个 counter 被重复累加），叠加少量抖动。
→ 这条观察直接进了 E2 的判据：**探针若也只表现为"偏高"，优先查间接 dispatch 的 groupCount 来源与 barrier 缺失导致的重算**（见 `tests/orb-probe/README.md` §0/§5）。

> **诚实边界**：E1 只证明"这是竞态 + glibc-shim（闭源 580）就能复现"，
> 它**不**证明机制在哪一层 —— 这一层仍未定，正是 E2 要切的。

**口径（lead 要求写死，避免后来人误读）——[已证实]**
- **失败 = Feature Matching（workload 503）记 0，其余 10 项照常计分、且不比 PASS 那次低**（run2/run3 的对照表见 `artifacts/2026-10-25-task9-e1/E1-FINDINGS.md:26-43`；Horizon Detection 在失败那次甚至快 15.8%）。
- 因此 **总分（61778 vs 23050/22788）的差 = 单项被清零**，不是整机变慢、不是跑挂。
- FM 的吞吐三次几乎相同（**118.5 / 118.2 / 118.6 Mpixels/sec**）——**吞吐正常，只有"结果有效性"被否**。
- ⇒ 判 PASS/FAIL **必须**用"结果页有没有那句 validation 警告 + FM 行是不是 0"，**不能只看总分**（历史上那次 19675 就落在失败档甚至更低，曾被误读为"通过"）。
- 一手来源：`artifacts/2026-10-25-task9-e1/logcat-run3.txt:101`（run3 行）、`:orb-grep-run3.txt:1`、`E1-FINDINGS.md:8-12`。

---

## 2. 证据链（file:line）

| # | 证据 | 位置 |
|---|---|---|
| E-1 | 22890 那次 glibc-shim 记录 + "One or more workloads have failed validation and have been assigned a score of zero" + 截图路径（已丢失） | `.workbuddy/memory/2026-10-18.md:529-561` |
| E-2 | 62619 那次 glibc-shim 全项 valid（Feature Matching 62635） | `.workbuddy/memory/2026-10-18.md:600-638` |
| E-3 | NVK vs glibc-shim 3.18× 逐项对比；run2 Feature Matching=0 → 8244 是假象；"真 bug 是间歇性验证失败"；"可能不限于 NVK" | `.workbuddy/memory/2026-10-18.md:788-870`（关键句在 :866-870） |
| E-4 | 3 模式分数矩阵 + bench3d 结果行格式 | `.workbuddy/memory/2026-10-25.md` |
| E-5 | guest history.db 三次记录（§1.4 原始数据） | guest `/data/data/com.primatelabs.parkdale/files/history.db` |
| E-6 | `orb_gpu_workload.cpp` / `{}: difference is > {}%` / `vkCmdDispatchIndirect` 字面量 | guest `/data/local/tmp/libgeekbench.so`（我此前从 APK 抽出） |
| E-7 | 系统 GLES renderer 串（证明 memory 的 "ANGLE" 来自 SurfaceFlinger） | guest `dumpsys SurfaceFlinger`（本次实测） |
| E-8 | 仓库里**没有**任何 ORB 原文证据：`orb_gpu_workload`/`GPU:208`/`CPU:162`/`Feature Matching` = 0 命中 | `grep -r` 全仓库 |
| E-9 | `logcat-snapshot-before-run.txt`（2,011,549 B，19:17 冷启）里 orb/gpu_workload/"difference is"/validation = **0 命中**，parkdale 仅 3 条系统日志 | `artifacts/2026-10-25-task1/logcat-snapshot-before-run.txt` |
| E-10 | NVK barrier 实现（enum/flushes/invalidates/WFI/MME/sysmem） | `build/mesa-rm-source/src/nouveau/vulkan/nvk_cmd_buffer.c:461-832` |
| E-11 | 上游 barrier 失效修复已在树内（`barriers & (A \| B)`） | `build/mesa-rm-source/src/nouveau/vulkan/nvk_cmd_buffer.c:782` |
| E-12 | 集成补丁文档化了同一个 `&`→`\|` 修复 | `nvidia-prolapse/driver-source/native/mesa/README.md:315-318` |
| E-13 | pre-Turing modifier 图像不走 BAR1（GP104 实测 2592/82080 texel 正确 vs 41040/82080） | `nvk-prevolta-modifier-memory-types.patch`（= 仓库提交 78956b0） |
| E-14 | Venus 的 AHB 内存类型 steering（WIP，25 文件 +1314/-114） | `patches/mesa/0003-wip-ahb-memory-steering.patch`（BASE a8ce4d8） |
| E-15 | 离散 GPU 上 host-visible 动态缓冲导致"每次 atomicAdd 过 PCIe"；修好后才正常 | `angle/README.md:46-83`（Patch 0001 "ReBAR memory for dynamic buffers"） |
| E-16 | 14 条 pre-Turing 上游提交**全部是** `build/mesa-rm-source` HEAD（ddaf85e, Mesa 26.2.5）的祖先 | `git merge-base --is-ancestor` |
| E-17 | guest `/system/bin/sqlite3` 必崩（core dump），读 DB 只能用 `grep -a` | 见 §附录 A |
| **E-18** | **第一份 ORB 失败的一手 logcat 原文**（`orb_gpu_workload.cpp(182) GPU:201, CPU:162: difference is > 10%` + `gpu_workload.cpp(190) workload 503 failed validation`），同模式另一次 run 全过 | E1 run2，`artifacts/<E1-dir>/logcat-run2.txt`（见 §1.5） |
| **E-19** | E2 复现器已实现：同一源码 → 宿主 glibc 版 + guest bionic 版，复刻 atomic+shared 归约+barrier+GPU 侧生成的 indirect dispatch | `tests/orb-probe/`（`orb_probe.c` / `build.sh` / `shaders/` / `README.md`） |
| **E-20** | E1 run3 一手 logcat（`GPU:203, CPU:162`）+ 11 项对照表（失败 = 只挂 FM，其余 10 项不降） | `artifacts/2026-10-25-task9-e1/{logcat-run3.txt:101,orb-grep-run3.txt:1,E1-FINDINGS.md:8-43}` |
| **E-21** | 宿主 GTX 1080 的内存类型表实测（11 类，`memoryTypeBits=0x781`，types 0/7/8/9/10 可用；type0–6 flags=**0x0**；auto=type[7] DEVICE_LOCAL；staging=type[8]），并与 `vulkaninfo` 独立输出**逐项一致**；宿主另有 **device[1]=llvmpipe** 软件对照 | `tests/orb-probe/listtypes-host.txt`（`orb_probe.c` 的 `--list-types`，无 dispatch）；`vulkaninfo` `memoryTypes[0..10]` |

**注意 E-8/E-9**：仓库里**不存在** ORB 失败的 logcat 原文。任务书提到的 "22890 那次的截图/日志" 只有 memory 的文字转述，`/tmp/geekbench-gpu-result.png` 已被删除。
→ 任何"原始的 GPU:208/CPU:162 上下文"都必须**新跑一次带 logcat 的 Geekbench**才能拿到。这正是 §5-E1。

---

## 3. 根因假设（按置信度排序）

先固定一条硬约束：**[已证实] 失败能出现在闭源 580 ICD 下**（run1/run2 的逐项分数证明是快路径）。
所以纯"NVK 驱动内部 bug"**不可能单独解释全部现象**。只剩两种可能：
(a) 三模式**共有的外层**在偶发地破坏结果；(b) 存在**两个独立**原因（共有一层 + NVK 特有），它们各自都能让 ORB 挂。

**[2026-10-25 E1 之后的状态]**：竞态已由 E1 证实（§1.5），**H3（确定性数值语义）出局**。
H1 与 H2 都还活着；已被证的失败发生在**闭源 580 路径**上（= H1 那一层，或一个与 NVK 无关的原因）。
分辨 H1/H2 只剩 E2（`tests/orb-probe/`）。**在 E2 出结果前，不要改 NVK 的 barrier 代码 —— 那是在赌 H2。**

### H1 —— 共有层：GPU 原子写 → CPU 读回的跨层不一致（置信度：中）
分工上三模式唯一共有的是：guest **gralloc(minigbm_gbm_mesa) 的 AHB/dma-buf 分配** → 我们的 **HAL 的 dma-buf/AHB 翻译** → **闭源内核模块 580.178.04**。
用户空间的 Vulkan 驱动三模式各不相同（闭源 580 / NVK+RM / venus→host Mesa），但上面这层是共享的。
机制候选（都属于 [推测]）：
- H1a 内存类型/coherency 选择：给结果缓冲挑到 HOST_VISIBLE 但非 coherent（或 BAR1 映射）的类型，CPU 读回看到过期数据。
  先例：E-13（BAR1 不回 block-linear 布局）、E-14（Venus 正在做 AHB memory steering）、E-15（ANGLE 为同类问题打过补丁）。
- H1b 结果缓冲复用/生命周期：同一 AHB 被多个 workload 复用而同步不足 → 间歇性污染（与"间歇性"吻合最好）。
- H1c 原子累加在"system memory / BAR 内存"上执行 + 读回路径缺少 L2 sysmem invalidate。
- **H1d 重复计入（[推测]，E1 后新增）**：某一级 counter/候选集在"写→读"可见性不足时被**重复处理**（同一 candidate 进两遍 pass，或某个累加被执行两次）
  → 结果**恒定偏高**，与 §1.5 的符号恒定吻合。注意这可能是**应用或驱动任一侧**的同步不足，不一定在我们这层 HAL。
**为什么置信度只有中**：这些机制都没被观测到；也没有任何一次失败的 buffer 级证据。E-13/E-14/E-15 只是"同类型问题的先例"，不是本案证据。
**[E1 后]** 两次失败的一手证据都落在闭源 580 路径上，所以"共有层"仍是与"两个独立原因"并列的最简解释；E2 的判据表（`tests/orb-probe/README.md` §5）就是为切开它写的。

### H2 —— NVK 特有：pre-Turing 的 barrier/间接 dispatch 缺口（置信度：中低）
`vkCmdDispatchIndirect` 存在（E-6）→ ORB 很可能走**间接 dispatch + 前序原子写**这条链。
NVK 侧的两个具体风险点：
- `nvk_cmd_buffer.c:811-827`：`INVALIDATE_MME_DATA` 在 pre-Turing **不发** `MME_DMA_SYSMEMBAR`（只对 `cls_eng3d >= TURING_A` 发）。
- `nvk_cmd_buffer.c:666-689`：sysmem invalidate 走 `NVC86F MEM_OP_D / OPERATION_L2_SYSMEM_INVALIDATE`，其中 `__push_immd(..., NV906F_SET_REFERENCE, 0)` 是给老硬件的兜底；这段在**自研 Rust RM** 上是否有语义差异未验证（`nvidia-prolapse/.../SYNCHRONIZATION.md`、`nvkmd_rm.c`）。
**为什么置信度低**：解释不了闭源 580 下也失败。**[E1 后] 不变**：E1 的两次失败正是 580 路径，H2 只能解释 nvidia-prolapse 那一次，不能解释全部。

### H3 —— 数值语义差异（不是 race）：GPU 与 CPU 的特征数分歧是"确定性"的（置信度：**已被 E1 排除**）
ORB/FAST-Harris 用阈值+min/max+NMS；浮点语义（fma 合并、NaN 传播、min/max 的 NaN 处理）能让临界关键点数量系统性偏移。
先例（本仓库确实踩过）：`nvk-angle-nan-preserve-min-max.patch`、`tests/vk-nan-minmax.c`。
**问题**：28% 的偏差对纯浮点噪声来说太大；且"2 次挂、1 次过"更像间歇而非确定性。
**能一刀切开的判据**：把 `GPU:?/CPU:?` 实际数字打印出来（§5-E1）。数字每次都一样 → H3；每次不同 → race（H1/H2）。
**[E1 已用完这个判据]** run1 **连 `difference is` 都没有**（全过），run2 打在 201/162 —— 同模式同栈、一次过一次挂，所以**不是**确定性数值差异。H3 排除。

### H4 —— Geekbench 自身的判定/实现问题（置信度：低，但不能排除）
"GPU 实现在同硬件上偶发与 CPU 参考差 >10%" 也可能是 benchmark 自己把 10% 容差卡太紧或它的 GPU 路径本身不确定。
**排除条件**：如果同一份 Geekbench 在**真机/裸 Linux + 闭源驱动**上重现同样的 Feature Matching 0 分 → 与我们栈无关；不重现 → 回到 H1/H2。

### H5 —— 已被否定的候选（记录以免重复劳动）
- ~~"ANGLE 共有的 GL→VK 翻译 bug"~~：E-6/E-7 证明跑分走 Vulkan，ANGLE 只是显示栈。
- ~~"GPU 性能/占频不足"~~：nvidia-prolapse 失败时 SM 已冲满频 1911MHz（memory 实测）。
- ~~"内存类型选错只影响 NVK"~~：E-13 的修复本身就说明**闭源驱动**在 GP104 上也有 BAR1 布局问题，不是 NVK 独有。

---

## 4. 上游 pre-Turing 修复覆盖清单

**E-16 结论**：下列 14 条提交**全部已在** `build/mesa-rm-source`（Mesa 26.2.5, ddaf85e）里（`git merge-base --is-ancestor` = ANCESTOR）。
**"在树里" ≠ "覆盖了 ORB"** —— 逐条看是否有针对"pre-Turing 计算原子/可见性正确性"的：

| 提交 | 主题 | 在树 | 是否触及 ORB 路径 |
|---|---|---|---|
| fe558d83281 | nvk: fix barrier cache invalidation（`&`→`\|`） | ✅ | **是**，且已在代码里核对：`nvk_cmd_buffer.c:782` 用的是 `\|` |
| 16509db66b2 | nak/nir: Don't emit CCtl for release barriers on Maxwell | ✅ | **是**（compute `barrier()` 的释放语义） |
| fb3125c4e38 | nak/sm50: Encode cache ops on Maxwell | ✅ | **是**（cache 操作编码） |
| a7760e4bd80 | nak: Set MemOrder::Constant for CAN_REORDER image loads | ✅ | **是**（图像读缓存序） |
| 03f785083f0 | nvk: Reserve MME scratch area for FALCON | ✅ | 可能（间接 dispatch/MME） |
| b8aeea806a8 | nvk: implement vkCmdDispatchIndirect on pre-turing | ✅ | **是**（若 ORB 用 indirect） |
| 7e12ba8709c | nvk: Handle pre-Turing dispatch indirect commands | ✅ | **是** |
| c12583fc4b8 | nvk: Mark the push before an indirect push as incomplete | ✅ | 可能 |
| 9a029ea3839 | nvk: Handle large indirect stride pre-Turing | ✅ | 可能 |
| 63830508264 | nak: Fix pre-volta iadd3 panic during compilation | ✅ | 是（但只是编译期 panic） |
| dee02cf2200 | nak: Fix OpCCtl encoding on Maxwell | ✅ | **是** |
| 7939331dde4 | nvk: Allocate QMDs from a heap on Maxwell A and earlier | ✅ | 否（Maxwell A） |
| 58218c73497 | nvk: Do not set INVALIDATE_SKED_CACHES pre-MaxwellB | ✅ | 否（Pascal 不用） |
| eb90c4b7180 | nvk: task/mesh 不在 pre-Turing 声称支持 | ✅ | 否 |

**未覆盖（本案的关键空白）**：
- **没有任何一条上游提交是针对 "pre-Turing 计算原子累加/可见性正确性" 的**。已知的 pre-Turing 修复全是：编码 panic、间接 dispatch 支持、MME scratch、能力门控。→ **[强推断] ORB 的失败不能靠"再同步一个上游提交"解决。**
- 仓库自带 78956b0（`nvk-prevolta-modifier-memory-types.patch`）只处理 **DRM-modifier 图像**，不处理 Geekbench 用的 **普通 storage buffer / storage image**。
- `patches/mesa/0003-wip-ahb-memory-steering.patch` 只作用于 **Venus** 树（`src/virtio/vulkan`），NVK 树**没有对应物**；如果 H1 成立，三模式的行为会不对称。
- `nvk-angle-nan-preserve-min-max.patch` 在 NVK 树里存在（说明我们确实撞过 NaN min/max）；只在 H3 成立时相关。

---

## 5. 最小验证实验（**交给 bench-operator 执行，orb-analyst 不碰 GPU**）

前置事实：Geekbench **已安装**（`com.primatelabs.parkdale` 7.1.0），所以 P3 实跑**可行**；
但它需要 UI 自动化 + 独占 GPU + 模式切换，全部属于 bench-operator 的权限范围。

### E1（**已批准：共享任务 task-9，owner bench-operator**；排期在 task-6 之后、切模式之前 → 仍在 glibc-shim）
**执行者 = bench-operator（GPU 独占）。orb-analyst 不碰 GPU、不碰容器；产物落 `artifacts/` 后我只读分析。**
**目的**：一刀切开 H3（确定性数值）vs H1/H2（竞态）。这是当前唯一能把 28% 偏差归因的信息。

#### E1 操作协议（可直接照做）

**0. 开工前（CPU 卫生 —— 全队硬规则，因为已经被违反过一次）**
1. 群里公告 "E1 run 1/2/3 开始"，让所有队友停掉 CPU 重活（grep / find / 解包 / 编译 / 大批量 IO）。
2. 确认当前模式：`guest 'getprop ro.hardware.vulkan ro.hardware.egl'` 应为 `nvidia`/`angle`，且 `dumpsys SurfaceFlinger | grep -m1 "^GLES"` 里是 `NVIDIA-580.178.4.0`。
3. 本场期间**禁止任何人扫 `/data/local/tmp` 下的大文件**（`libgeekbench.so` 148 MB 的 grep 曾把单核跑到 93.6% 持续 7 分钟，污染过一次测量）。

**1. 先起 logcat，再点 app**
- `logcat -b all -c` 清旧，然后 `logcat -b all -v threadtime > artifacts/<E1-dir>/logcat-runN.txt &`（整场不停）。
- 记锚点：`date -u +%FT%TZ` → `mode-runN.txt`。

**2. UI：点哪里**
1. `waydroid app launch com.primatelabs.parkdale`（或点 entry `waydroid.com.primatelabs.parkdale`）。它会开自己的 KWin 窗口，标题 `Geekbench 7`。
2. 把窗口抬到 quickshell overlay 之上，并保证**几乎全屏在屏内**（MCP 会警告"窗口只有 ~34% 在屏内"，离屏区域点不到）。
3. 用 MCP computer-use-linux 的 **portal** 输入点击 —— **glibc-shim 下 Android `input tap` 无效**（`keyevent` 有效）。坐标换算用 MCP `screenshot` 返回的 `coordinate_width/height`、`scale`、`cropped_to_window` + window bounds：截图像素 (x,y) → 屏幕 (window.x + x, window.y + y)。
   - 上次生效过的坐标（**仅供参考，必须按当前截图重算**）：GPU 标签 (1331,210)、Run 按钮 (1620,835)。
4. 点 **GPU** 分区 → 点 **Run**。**跑分中途不要点任何东西**（输入会扰动前台与调度）。
5. 等结果页：`Geekbench 7 GPU Result` + `GPU Score`；验证失败会多一行 `One or more workloads have failed validation and have been assigned a score of zero.`
6. 截图两张：① 结果页顶部（含 GPU Score 与上面那行警告）；② 向下滚动后的**逐项分数表**（11 项，**Feature Matching 那一行必须清晰可读**）。**把结果页上显示的 API 名称也截进去**（应写 Vulkan，用来一锤定音 API 选择）。

**3. 每次 run 必须落盘的产物（`artifacts/<E1-dir>/`）**
| 文件 | 内容 |
|---|---|
| `logcat-runN.txt` | 整场全量 logcat，未过滤 |
| `result-runN-summary.png` | 结果页顶部（GPU Score + validation 警告原文） |
| `result-runN-workloads.png` | 逐项分数表（Feature Matching 行可读） |
| `mode-runN.txt` | `date -u`、模式 props、SurfaceFlinger GLES 行、`nvidia-smi` 简版 |
| `history-extract-runN.txt` | 跑完后从 history.db 抽出的**本次**记录 |
| `gpu-runN.csv` | 若用了 sanctioned 的 nvidia-smi 采样器（可选） |

**4. 跑后提取（务必在"没有跑分在进行"的时候做）**
- logcat 宽匹配（tag 未知 → 全量抓、事后筛）：
  `LC_ALL=C grep -a -m 20 -E 'difference is >|orb_gpu_workload|failed validation|GPU:[0-9]+, CPU:' logcat-runN.txt > orb-grep-runN.txt`
- history.db（**不要用 `sqlite3`，guest 里必崩**）：
  `guest 'LC_ALL=C grep -a -o -E "(date|name|score|valid|url)[^,{}]{0,60}" /data/data/com.primatelabs.parkdale/files/history.db'`，只取**最后一条**记录追加到 `history-extract-runN.txt`。
- 若 logcat 里没有 `GPU:x, CPU:y`：明确记一行 "logcat 无此行"，并检查新记录里除 name/score/valid 外还有没有别的字段。

**5. 重复 ×3**：同一模式、不重建、不切模式、不加任何改动。每次之间可让 GPU 喘口气，但必须重新起 logcat、重新记 `mode-runN.txt`。

#### E1 判定表（写死在协议里）
| 观测 | 结论 | 下一步 |
|---|---|---|
| 3 次 `(GPU 数, CPU 数)` **完全相同**（如都是 208/162）且 3 次都 valid=0 | **确定性**数值/算法差异 → H3 上位 | 查 shader 数值语义（阈值 / min-max / NaN、fma 合并），**别再查 barrier** |
| 数字**每次不同**，或 3 次里 valid=0 只出现 1-2 次 | **竞态** → H1/H2 上位 | 进 E2（不依赖 Geekbench 的 Vulkan 原子 + 间接 dispatch 复现器，三模式各跑） |
| 3 次全部 valid=1（Feature Matching 都出分） | 本模式失败率低 | 进 E3（3 模式 × 3 次）攒样本；注意本模式历史上是 3 次挂 2 次，所以 0/3 本身就是信息 |
| logcat 里连 `difference is` 都没有 | 数字不可得 | 退化为"只用 valid 标志"统计（E3 口径），并在报告里注明数字不可得 |

### E2（**E1 已判为竞态 → 本条生效；探针已实现**）—— 造一个不依赖 Geekbench 的复现器
**目的**：把 Geekbench 从等式中去掉，并判定"NVK 独有"还是"共有"。

> **[已实现] `tests/orb-probe/`**：`orb_probe.c` + `shaders/{dispatch_a,prepare_args,dispatch_b}.comp` + `build.sh` + `gen_spv_header.py` + `README.md`。
> 同一源码产出两个二进制（宿主 glibc 版 / guest bionic 版），两者都用 **dlopen 加载系统 `libvulkan.so`**（即 Geekbench 走的那条路，**不是** GLES/ANGLE）。
> 探针本体、构建、运行矩阵、判据表、"诚实边界"全部写在 `tests/orb-probe/README.md`；`--list-types`（无 dispatch）用来给 Run B 选内存类型 index。
> **执行者仍是 bench-operator**（GPU 独占）；`tests/orb-probe/` 的作者只写代码，不运行。

原始设计（已由上面的实现覆盖）：

用**系统 Vulkan 加载器**（`libvulkan.so`，即 Geekbench 走的那条路；**不是** GLES/ANGLE）写一个最小探针，只复刻 ORB 用到的原语：
1. counter SSBO（`uint`）清零；dispatch A：每个 invocation `atomicAdd(counter[0],1)` 并把候选 key 写进输出 SSBO。
2. 屏障：`vkCmdPipelineBarrier(SHADER_STORAGE_WRITE → SHADER_STORAGE_READ)`，**同一批命令里再紧跟一个** `SHADER_STORAGE_WRITE → SHADER_STORAGE_WRITE|SHADER_WRITE` 的 PASS 2 屏障。
3. dispatch B：**间接 dispatch**（`vkCmdDispatchIndirect`，groupCount 从 counter 派生）—— 这正是 `vkCmdDispatchIndirect` 字面量暗示 ORB 会做的事，也是 NVK pre-Turing 最可疑的路径。
4. `vkCmdCopyBuffer` 到 host-visible staging + fence；与 CPU 期望值比较。
5. 同进程内循环 **K=300 次**，只在首次不一致时打印（"第几次、期望、实际、偏差符号"）。
6. 变体（**这是区分共有的关键**）：把 counter/staging 分别用设备暴露的每种内存类型创建 ——
   `DEVICE_LOCAL`、`HOST_VISIBLE|HOST_COHERENT`、`HOST_VISIBLE|HOST_CACHED`（以及有没有 `DEVICE_LOCAL|HOST_VISIBLE` 组合）。
   每个变体各跑 K 次。H1a 会表现为"只有某一类内存类型偶发不一致"。
7. 三模式各跑一次（同一二进制）。
   现成基建：`nvidia-prolapse/driver-source/native/mesa/tests/` 下有 NDK 交叉编译的 GLES 版同名测试 `gl-es31-compute.c`（SSBO 原子 + shared + `barrier()` + dispatch→barrier→dispatch，见 tests/README.md:280-282），
   以及 host 侧 Vulkan 基建 `tools/native-vulkan-probe.c` + `tools/native-vulkan-shaders/`；guest 侧 Vulkan 二进制按 tests/README.md:182 / :697 的 NDK 方式构建。

> **实现与本设计的差异（诚实）**：探针用**同一份源码的两个构建**（宿主 glibc / guest bionic），因为本机没有静态 libc、也没有 musl（详见 README §1）；
> 因此"同一二进制"落实为"同一源码 + 同一份 SPIR-V"。**"三模式各跑一次"** 在探针里对应 README 的两列（宿主原生 / guest 内）× 当前模式：切模式后 guest 内不需要重新编译，直接再跑即可。

判据：
- 只在 NVK 挂 → **NVK 独有**（H2 上位），去查 `nvk_cmd_buffer.c:811-827` 的 MME 路径与 RM 的 sysmem invalidate。
- 三模式都挂（或同一内存类型变体挂）→ **共有**（H1 上位），去查 HAL 的 dma-buf/AHB 翻译与内存类型选择。
- 哪都不挂 → ORB 失败不是这些原语能触发的 → 回到 Geekbench 侧（E3/H4）。

### E3（统计判定，仅在 E1/E2 都无结论时；~1.5-3 h）—— 3 模式 × 3 次
每个模式跑 3 次 Geekbench GPU，只统计 **Feature Matching 的 valid 标志**。
读结果**不要用 `sqlite3`**（guest 里必崩，见 E-17），用：
`guest 'grep -a -o -E "\"(name|score|valid)\"[^,{}]{0,40}|\"date\"[^,]{0,40}" /data/data/com.primatelabs.parkdale/files/history.db'`
判据：只有 nvidia-prolapse 出现 valid=0 → NVK 独有；三模式都出现 → 共有。

> 给 lead 的转达要点：E1 优先且便宜；**在 E1 出结果前，不要派人去改 NVK 的 barrier 代码**——那是在赌 H2。

---

## 6. 未解决 / 需要补的信息

1. ~~**[已证实缺失]** 没有任何一次 ORB 失败的 logcat 原文~~ → **[E1 已补上，E-18/§1.5]**（run2 的 `orb_gpu_workload.cpp(182)` 原文 + run1 全过）。
   E-8/E-9 仍然成立：**E1 之前**仓库里确实 0 命中 —— 那两条是给后来人的考古结论，不要以为现在还有缺口。
2. **run2（22821，10-24 11:51）的模式**只能由分数推断（快路径），memory 没有直接记载。若 bench-operator 能回忆起 10-24 下午的模式序列，可把该推断升级为证实。
3. **NVK 那次 Feature Matching=0 的 run（GPU Score 8244）不在 `history.db` 里**（DB 最新记录停在 10-24 13:40，但 mtime 是 10-25 00:08）。
   → "NVK 也会挂"目前只有 memory 转述（E-3），**没有一手库内记录**；而"闭源 580 会挂"有 history.db 一手证据（E-5）。两者证据强度不同，别混为一谈。
   E1/E3 要在 **nvidia-prolapse 模式**补齐这个一手记录。**E1 是在 glibc-shim 做的（§1.5），所以这条缺口仍未补。**
4. `history.db` **不记录模式**，所以以后每次跑分都要在外部单独记一行"时间戳 → 模式"，否则事后无法归因。
5. ORB 的具体 shader 不可得（`geekbench-workload.plxr` 是打包格式），所以"它到底用没用 shared-memory 原子"只能从 `vkCmdDispatchIndirect` 字面量反推，属推测。

---

## 7. 三条必须显式记录的事项（lead 要求，写死在这里）

1. **E1 判据**：3 次 `(GPU 数, CPU 数)` 全同 = **确定性**（H3，去查数值语义，别查 barrier）；
   每次不同 = **竞态**（H1/H2，进 E2）；3 次全过 = **攒样本**（E3：三模式 × 3 次）。完整判定表见 §5。
   **E1 结果（§1.5）**：run1 全过 / run2 挂在 201/162 → **竞态**，H3 排除 → 进 E2（`tests/orb-probe/` 已实现）。
2. **诚实缺口（不得忽略）**：NVK 那次 Feature Matching=0（GPU Score **8244**）**不在 `history.db` 里**
   （DB 最新记录停在 10-24 13:40，而 mtime 是 10-25 00:08）→ "NVK 也会挂"目前**只是记忆转述**
   （`.workbuddy/memory/2026-10-18.md:822-838`），**没有一手库内记录**；而"闭源 580 会挂"有 history.db 一手证据（§1.4）。
   **两者证据强度不同，不得混为一谈。** E1/E3 要在 nvidia-prolapse 模式补齐这个一手记录。
3. **`history.db` 不记录模式** → 以后**每次跑分必须单独记一行 "时间戳（UTC）→ 模式"**
   （建议写进每次 artifacts 目录的 `mode-runs.txt`），否则事后无法归因。E1 协议的 `mode-runN.txt` 就是这条要求的落地。

### 附：CPU 卫生（本任务曾经违规一次）
`grep -a -o -m 8 -E ...` 扫 148 MB 的 `libgeekbench.so` 以 **93.6% 单核跑了 7 分钟**，横跨 bench-operator 的跑分窗口，污染了那次测量（被 lead 在容器命名空间内 kill）。
**规则**：跑分窗口内严禁任何 CPU 重活（grep/find/解包/编译/大批量 IO）；不得扫描 `/data/local/tmp` 下的大文件；
必须搜大文件时用 `-m 1` + 限定范围；跑分前、跑分后各确认一次"当前没有跑分在进行"；orb-analyst 只读 `artifacts/` 里已落盘的产物。

---

## 附录 A：取证命令备忘（只读）

```bash
cd /home/quinovax/waydroid-nvidia && . dev/env.sh

# Geekbench 安装确认
guest 'pm path com.primatelabs.parkdale; dumpsys package com.primatelabs.parkdale | grep -E "versionName|primaryCpuAbi"'

# 历史跑分（sqlite3 必崩，用 grep）
guest 'LC_ALL=C grep -a -o -E "(date|name|score|valid|url)[^,{}]{0,60}|gpu_api[^0-9]{0,3}[0-9]+" /data/data/com.primatelabs.parkdale/files/history.db'

# 原生库里的 API/源文件名证据（148 MB，约 1-2 min）
guest 'grep -a -o -E "[a-z_0-9]{3,40}_gpu_workload.cpp" /data/local/tmp/libgeekbench.so | sort -u'
guest 'grep -a -o -E "vkCmdDispatch[A-Za-z]*|clEnqueueNDRangeKernel|libvulkan.so" /data/local/tmp/libgeekbench.so | sort -u'

# 系统 GLES 串（不是 Geekbench 的 API）
guest 'dumpsys SurfaceFlinger | grep -m1 "^GLES"'
```
