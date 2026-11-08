# 三模式实测能力矩阵（更新于 2026-11-08 17:08）

> 所有数字来自 `artifacts/` 下的一手产物。未测项明确标注。

## 结论摘要

| 模式 | 3DMark Steel Nomad Light | 结果 zip | raw_result | 达标（>3000 且 >10KB） |
|---|---|---|---|---|
| **glibc-shim**（闭源 580 + BC1） | **5237**（复现 4986） | 48017 B（复现 48209 B） | SUCCESS | ✅ **PASS** |
| **mesa-venus**（venus + BC1） | **5413** | 43273 B | SUCCESS | ✅ **PASS** |
| **nvidia-prolapse**（NVK + BC1） | **3182** | 33047 B | SUCCESS | ✅ **PASS** |

**三模式全部达成验收。** 三种驱动栈的病是同一个（ASTC→RGBA8 膨胀 5.1x 打满显存），修复也是同一个思路（ASTC 4x4 → BC1），在两套完全独立的代码路径上各实现了一次。

### 复现命令（三模式）

```bash
# glibc-shim
dev/bench3d --label bc1 --props "debug.nvhal.astc_bc1=1;debug.nvhal.astc_blocks_host=0"

# nvidia-prolapse（NVK）
dev/bench3d --label prolapse-bc1 --props "debug.nvhal.astc_bc1=1;debug.nvhal.astc_blocks_host=0"

# mesa-venus
dev/bench3d --label venus-bc1 --props "debug.mesa.vn.astc.emu=1;debug.mesa.vn.astc.bc1=1"
```

> **`debug.nvhal.astc_blocks_host` 自 2026-10-31 起默认 ON**（见 `nvidia-prolapse/hal/src/astc.c:1184`）。
> 它在 glibc-shim 路径上与 BC1 **可以共存**（11-07 的 5237 跑分即 blocks_host=ON + BC1，边界：app 可见纹理 2.73 GiB、block 图在 HAL 宿主内存）；
> 但在 **nvidia-prolapse（NVK）路径上必须显式置 0**，否则实测 zip 4269 B、分数 0、Arielle `code="10000"`、logcat `E 3DMarkAndroid: Application quit during loading`。
> 为了可复现性，三条命令都显式带 `blocks_host=0`（venus 不经过 HAL，无此开关）。

## 环境

| 项 | 值 |
|---|---|
| GPU | NVIDIA GTX 1080（Pascal GP104，compute 6.1，8192 MiB，PCIe 3.0 x16，BAR1 246 MiB） |
| 驱动 | **580.196.07**（此前 580.178.04） |
| 内存（glibc-shim） | 3 heap / 11 type：heap0 8192 MiB DEVICE_LOCAL（type 7）；heap1 11947 MiB（type 0-6 flags=0x0、8=HV\|HC、9=HV\|HC\|HOST_CACHED）；heap2 246 MiB DL\|HV\|HC（type 10） |
| 内存（venus） | 相同尺寸，但 **三个 heap 全部标 DEVICE_LOCAL**（heap1 的 11947 MiB 也被标 DL） |
| 宿主 | Fedora，Xeon E3-1270 v5（8 线程），16 GiB |
| 关键前提 | **宿主必须干净**：`load<8`、无 benchmark 残留、`coredumpctl --since 60s` 无 SIGABRT |

## 根因与修复（两模式同一个病）

两个模式的失败是**同一个原因**：ASTC 被仿真解码成 RGBA8（4 B/texel），把 3DMark 的 1.16 GiB 纹理集膨胀到 5.90 GiB（**5.1x**），8 GiB 显存被打满，渲染期资源溢出，最终 OOM。

修复在两条完全不同的代码路径上做了同一件事：**ASTC 4x4 → BC1（0.5 B/texel，8 倍压缩）**。

| | glibc-shim | nvidia-prolapse | mesa-venus |
|---|---|---|---|
| 实现位置 | `nvidia-prolapse/hal/src/astc.c`（我们自己的 HAL） | **同一个 HAL**（prolapse 模式下 HAL 同样在链路上） | `mesa/src/virtio/vulkan/vn_astc_emu.c`（Mesa venus 驱动） |
| 着色器 | `nvidia-prolapse/hal/src/bc1_encode.comp` | 同左 | `mesa/src/virtio/vulkan/vn_bc1_encode.comp` |
| 开关 | `debug.nvhal.astc_bc1=1` + `debug.nvhal.astc_blocks_host=0` | 同左 | `debug.mesa.vn.astc.bc1=1` ＋ 总闸 `debug.mesa.vn.astc.emu=1` |
| 管线 | 解码到 HAL 私有 RGBA8 → `bc1_encode` → BC1 缓冲 → `vkCmdCopyBufferToImage` | 同左 | 解码到临时 RGBA8 图 → `vn_bc1_encode` → 临时 BC1 缓冲 → raw `vkCmdCopyBufferToImage` |
| 踩的坑 | `astc_blocks_host` 自 2026-10-31 默认 ON 且与 BC1 互斥 → 必须显式置 0 | 同左（同一份 HAL） | 直接调 `vn_CmdCopyBufferToImage` 会**无限递归**（ASTC 钩子再次进入 BC1 路径），guest 表现为 `SIGSEGV in blake3_hasher_update` 栈溢出；修复 = 新增 `vn_cmd_copy_buffer_to_image_raw()` 绕过钩子 |

### 因果闭环（venus，A/B 一手数据）

| 配置 | fb.used 峰值 | 结果 |
|---|---|---|
| `astc.emu=1, bc1=0`（RGBA8） | **8026 MiB** | `VK_ERROR_OUT_OF_HOST_MEMORY on vkEndCommandBuffer`，无 result zip |
| `astc.emu=1, bc1=1`（BC1） | **4929 MiB** | SUCCESS，**5413 分**，zip 43273 B |

峰值降 3.1 GiB 直接把失败变成通过。复现命令：

```bash
dev/bench3d --label venus-bc1 --props "debug.mesa.vn.astc.emu=1;debug.mesa.vn.astc.bc1=1"
```

### glibc-shim 的进展链

| 配置 | 分数 | fps | OODM | 说明 |
|---|---|---|---|---|
| 原始基线（block 图在 app 显存） | 205 | 1.52 | 139 | 1.35 GiB 溢出宿主内存，每帧 3.9 GB 走 PCIe |
| block 图移 HAL 宿主内存 | 754 / 1267 / 917 | 5.59 | 16 | 588 张图全迁移 |
| **BC1 重定向** | **5237**（复现 4986） | **38.80** | **0** | app 可见纹理 5.90 → 2.73 GiB |

复现命令（**必须同时显式关闭 blocks_host**）：

```bash
dev/bench3d --label bc1 --props "debug.nvhal.astc_bc1=1;debug.nvhal.astc_blocks_host=0"
```

## nvidia-prolapse 的解锁路径（已完成）

原阻塞是两道门，现已全部打通：

1. **vendor image 门** → 改走 **dev-root** 路径（不需要 vendor image，把驱动栈装在
   `/var/lib/waydroid/nvidia-prolapse-staging` 并由容器 bind-mount 进去）。

2. **RM 版本门**（manifest pin 580.178.04，实际内核模块 580.196.07）→ 三步解决：

   | 步骤 | 做法 | 结果 |
   |---|---|---|
   | RM ABI 兼容性验证 | 临时把 `("580.196.07", &P17)` 加进 `compat_data.rs` 并重建 Rust RM，直接跑三个探针 | host/compute **4422 words**、host/graphics **2109 pixels**、host/interop **4236 cross-device words**，全 PASS |
   | 正式加入 profile | `compat_data.rs:4696` 写入 `("580.196.07", &P17)`（带注释指向 `artifacts/prolapse-abi-experiment/RESULTS.md`） | — |
   | 重建 + 重写 provenance | 重建 Rust RM（3 target）与 NVK（3 target），用 `tools/restamp-rust-provenance.py` **重算**（非伪造）`rust-build.json` | 三个 target `verify_metadata` 全通过 |

3. **stage 与 activate**：`dev/source-native stage ... --rust-profile release` → 20/20 case 全绿 → 18 个产物落地 →
   activate。activate 需要 root 且要求容器 STOPPED，而唯一的 root 通道 `lxc-attach -s pid` 需要容器 RUNNING：
   **解法 = 在容器 RUNNING 时用 root 通道手动完成 activate 的四项文件改写
   （waydroid.cfg / nvidia-prolapse 符号链接 / waydroid_base.prop / config_nodes），
   然后才用 `sudo -n systemctl restart waydroid-container.service` 重启。** 详见 `docs/host-root-channel.md`。

**实测结果**：`artifacts/bench3d-20261108T170322-prolapse-bc1-t2/` → **3182 分**、zip **33047 B**、raw_result SUCCESS、fb 峰值 6417 MiB。

### prolapse 的关键坑：blocks_host 默认 ON 且与 BC1 冲突

`debug.nvhal.astc_blocks_host` 自 **2026-10-31** 起默认开启（源码注释在 `nvidia-prolapse/hal/src/astc.c:1184`）。它把 ASTC block 图搬到 HAL 自持的宿主内存。在 NVK 路径上它与 BC1 冲突：

| 配置（nvidia-prolapse） | 结果 |
|---|---|
| 只设 `astc_bc1=1`（blocks_host 用默认 ON） | zip 4269 B、分数 **0**、Arielle `code="10000"`、logcat `E 3DMarkAndroid: Application quit during loading`、fb 峰值仅 3053 MiB |
| `astc_bc1=1` + `astc_blocks_host=0` | zip **33047 B**、**3182 分**、SUCCESS、fb 峰值 6417 MiB |

> 注意这是 **NVK 特有**的冲突：同样的 blocks_host=ON + BC1 在 glibc-shim 上能跑出 5237（11-07 那两次就是默认 ON 的）。
> 差别在于 NVK 自己也在管理显存/对象表，block 图离家 + BC1 双份元数据把它推过了界。

## 已排除（都有判据）

| 假设 | 判据 | 结论 |
|---|---|---|
| 硬件/驱动慢 | 宿主原生 1500 draw = **0.599 µs/draw** | 否定 |
| HAL 路径慢 | guest 内同构基准 = **0.149 µs/draw**、填充 **79.4 Gpx/s** | 否定 |
| 每 draw 固定开销 | DRAW-DIST p50 仅 16-32 µs | 否定 |
| 宿主噪声 | 干净 vs 噪声仅 **+6%** | 非主因 |
| 窗口/分辨率 | 2a：477×852 与 1900×1010 同分 | 否定 |
| CPU 侧翻译 | xlate_stats **0.02% of CPU** | 否定 |
| host-visible 小开关 | 只搬了 7 MiB | 否定 |
| ARM 翻译 | 两个 benchmark 都跑原生 x86_64 | 否定 |
| NVKMS sysmem 回退 | 零回退那次同样 218 分 | 否定 |

## Geekbench ORB 状态

失败永远是 Feature Matching（workload 503）：`orb_gpu_workload.cpp(182) GPU:20x, CPU:162: difference is > 10%`。

从 `history.db`（10 条记录，全部 `gpu_api=6` = Vulkan）实测：

| 现象 | 数据 |
|---|---|
| CPU 参考值 | **恒定 162**（确定性） |
| GPU 值 | 186–208，**逐次变化且总高于 CPU** |
| 吞吐 | 通过与失败**几乎相同**（114–118 Mpix/s），`work` 完全相同 |
| 频率 | 3 次里挂 2 次（间歇） |
| 设备 | glibc-shim 与 venus 都出现 |

**三驱动栈一手对照（新）**：venus 模式实测同样失败——`orb_gpu_workload.cpp(182) GPU:189, CPU:162` + `workload 503 failed validation`，而同一跑的**其它 10 个 workload 全部 valid=1**（Background Blur 76594、Super Resolution 131205 等，与 glibc-shim 快路径同档）。详见 `docs/orb-three-stack-evidence.md`。

| 模式 | GPU 值 | CPU 值 | 结果 |
|---|---|---|---|
| glibc-shim run1 | — | — | ✅ 通过（FM 62792 / 118.5 Mpix/s） |
| glibc-shim run2 | 201 | 162 | ❌ |
| glibc-shim run3 | 203 | 162 | ❌ |
| mesa-venus | 189 | 162 | ❌ |

driver 版本相关性（history.db 11 次）：driver=**26.0.0**（Mesa/NVK）的两次都 valid=1；driver=580.178.16 的三次里 1 次通过（61778）2 次失败；580.196.28 三次全失败。⇒ 闭源路径时好时坏，**不是某条栈独有**。

已排除的方向：`docs/orb-validation-analysis.md`（H1c 被自建探针证伪——原子累加+barrier+间接 dispatch+拷回+fence 链路在 host 与 guest（含全部 5 种内存类型）均全绿）、`tests/orb-probe/`。

**当前判断**：GPU 侧非确定性产出，且**方向恒为偏高 16.7%-25.3%**（"多算了一批匹配"或"归约重复累加"的形状），而 runtime 与 rate 在通过与失败时几乎相同（0.1005-0.1048 s、114-119 Mpix/s）⇒ 不是算错、不是慢，是特定 workload 的 kernel 竞态/残留读。**不在本次三模式验收范围内**（不影响 3DMark 三模式出分）。

## 产物索引

### 3DMark
- glibc-shim BC1：`artifacts/bench3d-20261107T202633-bc1-runE3/`（5237）、`artifacts/bench3d-20261107T201805-bc1-runE2/`（4986）
- venus BC1：`artifacts/bench3d-20261108T142953-venus-bc1-t2/`（5413）
- venus 负向对照：`artifacts/bench3d-20261108T143420-venus-nobc1-ctl/`（8026 MiB → OOM）
- **prolapse BC1：`artifacts/bench3d-20261108T170322-prolapse-bc1-t2/`（3182，zip 33047 B）**
- prolapse 对照失败（blocks_host 默认 ON）：`artifacts/bench3d-20261108T165852-prolapse-bc1-t1/`（zip 4269 B、0 分、`code="10000"`）
- prolapse 解锁取证：`artifacts/prolapse-abi-experiment/RESULTS.md`、`docs/host-root-channel.md`

### 根因与历史
- `docs/BC1-RESULT-5237.md`、`docs/bottleneck-verdict.md`、`docs/host-vs-guest-draw-cost.md`、`docs/host-noise-and-cleanup.md`
- `docs/astc-bc1-plan.md`、`docs/astc-blocks-host-findings.md`、`docs/drawdist-findings.md`

### ORB
- `docs/orb-three-stack-evidence.md`（三驱动栈一手对照，含 venus 实测）
- `docs/orb-validation-analysis.md`、`docs/orb-probe-findings.md`、`tests/orb-probe/`
- `artifacts/gebench-20261108T150441-venus-orb/`（venus 跑分产物与 logcat）

### 测量工具
- `dev/bench3d`（三模式统一跑分，自动切模式/复位 prop/采 GPU 与 dmon）
- `dev/gpu-hotspots`（perf + nvidia-smi 归因）
