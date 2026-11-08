# ORB (Feature Matching) 验证失败：三驱动栈对照实测

结论先行：**ORB 失败不是闭源 NVIDIA HAL 特有的，venus 上也一样失败。**
这是本项目该问题第一次拿到三种驱动栈的一手日志对照。

## 1. 一手日志证据

### venus 模式（mesa-venus，本次新测）
驱动：`ANGLE (NVIDIA, Vulkan 1.3.312 (NVIDIA NVIDIA GeForce GTX 1080 (0x00001...)`
运行时版本：`580.196.2`（metadata id=5018），内核 `Linux 6.18.60-1.cachylts1.fc45.x86_64`

```
11-08 15:16:22.485  5268  5940 I geekbench: [1108/151622:ERROR:orb_gpu_workload.cpp(182)] GPU:189, CPU:162: difference is > 10%
11-08 15:16:22.486  5268  5940 I geekbench: [1108/151622:ERROR:gpu_workload.cpp(190)] workload 503 failed validation
```

### glibc-shim 模式（闭源 HAL，历史三次）
- run1（10-25 20:24）：GPU Score 61778，**无失败警告**，Feature Matching 62792 / 118.5 Mpix/s ✅
- run2（10-25 20:31）：`GPU:201, CPU:162: difference is > 10%` → workload 503 failed validation，GPU Score 23050 ❌
- run3（10-25 20:36）：`GPU:203, CPU:162` → 同上，GPU Score 22788 ❌

## 2. 关键：CPU 参考恒为 162，GPU 值逐次抖动

| 模式/次 | GPU 值 | CPU 值 | 结论 |
|---|---|---|---|
| glibc-shim run2 | 201 | 162 | FAIL |
| glibc-shim run3 | 203 | 162 | FAIL |
| **venus（本次）** | **189** | **162** | **FAIL** |

CPU 侧三轮完全一致（162），说明 CPU 参考实现是确定的；GPU 值在 189-203 之间抖动，
相对 CPU 恒偏高 16.7%-25.3%。**GPU 侧产出非确定性，且方向恒为"多"**。

## 3. 决定性：**其它 10 个 workload 全部通过**

venus run（history.db benchmark 9，GPU 总分 21614 valid=0）：

| workload | score | valid | runtime (s) | rate |
|---|---|---|---|---|
| Background Blur | 76594 | 1 | 0.0506 | 197.8 |
| Face Tracking | 63697 | 1 | 0.1266 | 79.0 |
| Super Resolution | 131205 | 1 | 0.2707 | 242058.5 |
| Horizon Detection | 61856 | 1 | 0.0099 | 2384469204.2 |
| Photo Filter | 66703 | 1 | 0.0082 | 1094.6 |
| Video Filter | 46253 | 1 | 0.0066 | 12537950221.1 |
| RAW | 65167 | 1 | 0.0050 | 2634213636.5 |
| **Feature Matching** | **0** | **0** | **0.1048** | **114024718.1** |
| Path Tracer | 42869 | 1 | 0.0270 | 76784251.7 |
| Particle Physics | 38155 | 1 | 0.1462 | 6840.5 |
| Fluid Simulation | 36929 | 1 | 0.1838 | 544.2 |

**只有 Feature Matching 一个 workload 被判无效**，其余 10 项（覆盖图形、计算、路径追踪、
粒子、流体）全部 valid=1，且分数与 glibc-shim 快路径同档（Background Blur 76594 vs 78274）。

⇒ 排除"整个 GPU 通路有问题"、"内存一致性全局坏了"、"计算管线普遍错误"这类解释。

## 4. 吞吐相同、只有 valid 标志不同

Feature Matching 在通过和失败时的 **runtime 与 rate 几乎完全一致**：

| run | score | valid | runtime (s) | rate (pix/s) |
|---|---|---|---|---|
| 通过 ×2 | 62635 / 62792 | 1 | 0.1011 / 0.1008 | 118225195 / 118521508 |
| 失败 ×8 | 0 | 0 | 0.1005 - 0.1048 | 114024718 - 118922623 |

⇒ **不是算错、不是慢、不是崩溃**，是"结果判定所用的某份数据"不对或是残留。

## 5. 与驱动版本的相关性（history.db 全部 11 次）

| # | GPU score | valid | driver (id=5018) |
|---|---|---|---|
| 0 | 22890 | 0 | 580.178.16 |
| 1 | 22821 | 0 | 580.178.16 |
| 2 | **62619** | **1** | **26.0.0** |
| 3 | **19675** | **1** | **26.0.0** |
| 4 | 8244 | 0 | 580.178.16 |
| 5 | **61778** | **1** | 580.178.16 |
| 6 | 23050 | 0 | 580.178.16 |
| 7 | 22788 | 0 | 580.196.28 |
| 8 | 22786 | 0 | 580.196.28 |
| 9 | 21614 | 0 | 580.196.28（venus 本次） |
| 10 | 2753 | 0 | ?（本次另一条，见下） |

- driver=26.0.0 的两次（bench 2、3）**都是 valid=1**；这两次是 Mesa/NVK 路径
  （26.0.0 = Mesa 版本号），bench 3 的 GPU 19675 就是用户矩阵里"nvidia-prolapse ✅"那一次。
- 但 **bench 5（driver 580.178.16）也是 valid=1**（61778），所以"580.x 必失败"不成立。
- ⇒ 闭源 580 路径**时好时坏**（3 次通过 1 次、2 次失败），venus 本次失败。

## 6. 与 task-19 探针结论的关系（不矛盾）

tests/orb-probe 的最小链路（counter SSBO 原子累加 → barrier → vkCmdDispatchIndirect
→ copy 到 host-visible staging → fence → CPU 比对）在 **host 与 guest 都是全绿**，
negative control（no-args 3 / none 388-400）能挂，说明仪器灵敏。

⇒ 通用原子/屏障/间接派发链路在我们的 HAL/gralloc 层**是正确的**。
ORB 失败不在这一层，而在 Feature Matching 这个特定 workload 的**特有实现细节**上
（它自己缺屏障、用了特定的 storage image / subgroup / AHB 图像路径、
或对 GPU 结果的排序/归约顺序敏感）。

## 7. 剩余可疑面（按优先级）

1. **Feature Matching 自身的 GPU kernel 有竞态**：CPU 参考 162 恒定、GPU 恒偏高
   16.7%-25.3%，形状像"多算了一批匹配"或"归约时重复累加"。这对应它自己缺屏障，
   在快 GPU 上更容易暴露、在慢 GPU 上被掩盖（解释了 26.0.0 慢路径通过）。
2. **Geekbench 的 GPU 结果缓冲复用**：跨 workload 复用同一 AHB/storage buffer，
   同步不足导致读到上一次的残留。
3. **驱动侧调度时序**：任务提交顺序影响非原子写竞争的结果。

## 8. 已排除

- ❌ ARM 翻译（3DMark / Geekbench 都是原生 x86_64）
- ❌ 内存一致性全局损坏（探针 host/guest 双全绿，10/11 workload 通过）
- ❌ NVK 独有（venus 与闭源都失败）
- ❌ 闭源独有（venus 也失败；且 driver 26.0.0 的 NVK 那两次通过）
- ❌ 算错/太慢（runtime 与 rate 在通过与失败时几乎相同）
- ❌ Geekbench 缺设置项（shared_prefs 只有 `<map />` 空）
- ❌ 显存不足（Geekbench 峰值只需 ~1 GiB，远低于 8 GiB）

## 9. 复现命令

```bash
# venus 模式（需要 ASTC 仿真开启，否则 3DMark 都无法跑）
sudo ... # 已部署：debug.mesa.vn.astc.emu=1 与 debug.mesa.vn.astc.bc1=1
dev/gebench 1 venus-orb

# 取证
sudo -n /usr/bin/lxc-attach -P /var/lib/waydroid/lxc -n waydroid --clear-env \
  -v PATH=/system/bin -- /system/bin/sh -c \
  'logcat -d -b all | grep -aE "difference is >|failed validation"'
```

产物：`artifacts/gebench-20261108T150441-venus-orb/`（logcat-venus-orb.txt 含上述两行报错）
