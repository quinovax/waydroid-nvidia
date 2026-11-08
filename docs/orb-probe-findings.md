# ORB 探针（tests/orb-probe）实测结论 — 2026-10-25 21:30

## TL;DR

- **探针先在宿主机上用一次「自己的 bug」教了我们一课**：初版把 args 屏障放在 prepare_args **之前**（即它所服务的写之前），于是 vkCmdDispatchIndirect 读到清零后的 args、dispatch 0 个 workgroup。表现是每次迭代恒定 3 个 mismatch、keysB 一个槽都没写（act=0，方向是**少**不是多）。修好（屏障移到 prepare_args 之后）后宿主机 **全绿**。
- **修好后的探针在同一台机器、同一驱动的 guest 里也全绿**：glibc-shim 模式下 100 次迭代全部通过，1000 次迭代 0 mismatch，5 种 memory type（0/7/8/9/10）逐一 0 mismatch。探针确实走的是我们的 HAL（进程 maps 里有 /vendor_extra/lib64/hw/vulkan.nvidia.so，2450360 B = task-14 那版）。
- ⇒ **『GPU 原子累加 + barrier + 间接 dispatch + 拷回 host-visible + fence + CPU 比对』这条最小链路在我们的 HAL/gralloc 层是正确的**。ORB（Feature Matching）的失败**不是**这条通用链路的 coherency/竞态问题（H1c 被证伪，H1a/H1b 被显著削弱）。
- 剩余可疑面收窄到：Geekbench Feature Matching **自己的工作负载特性**（storage image / sampler / subgroup / AHB 图像路径 / 它自己缺屏障 + 我们驱动的调度时序）。探针 v2 若继续，应改用 **AHB/gralloc 导入的 storage buffer + image**，而不是普通 buffer。

## 仪器自检（负向对照，证明探针有灵敏度）

| 变体 | 语义 | host | guest |
|---|---|---|---|
| --barrier full | 应有屏障 | 0 / 100 iter | 0 / 100 iter |
| --barrier no-pass2 | 去掉 A→B 写屏障 | 0 | 0（NVIDIA L2 兜住） |
| --barrier no-args | 去掉 args 屏障 | 3（仅第 0 次迭代） | 3（仅第 0 次迭代） |
| --barrier none | 无任何屏障 | 400 | 388 |
| --barrier memory | 全内存屏障 | 0 | 0 |
| --indirect off | 不用间接 dispatch | 0 | 0 |
| --mem-type all | 5 种内存类型 | 全部 0 | 全部 0 |
| --iterations 1000 --barrier full | 长跑 | 0 | 0 |

负向对照能挂（3 / 388-400），正向全绿 ⇒ 探针不是「永远说没事」的坏仪器。

## 产物

- 修复前 host 矩阵（含不一致的原始输出）：artifacts/orb-probe-20261025T131135/
- 修复后 host 矩阵：artifacts/orb-probe-fix-20261025T132851/orb-probe-host-rerun.txt
- guest 矩阵：本次会话 guest 侧内联执行，结果与 host 一致
- 二进制：host tests/orb-probe/build/orb-probe-host 43176 B md5 10aa6090f1e0c5f7a3a69409e60b73b3；guest build/orb-probe-android 38856 B md5 936cdf7b6653cd6bcb8ae67796c5d96f

## 修改

tests/orb-probe/orb_probe.c：把 b_args 的 COMPUTE_SHADER/SHADER_WRITE → DRAW_INDIRECT/INDIRECT_COMMAND_READ 屏障从 prepare_args **之前**移到**之后**（并加注释说明踩坑），b_pass2 同步后移。

## 下一步（ORB 方向）

1. 探针 v2：把 storage buffer/image 换成 **gralloc/AHB 导入**（走 HAL 的 ahb.c 包装路径），并加 sampler 采样 —— 这是唯一还没被覆盖的层。
2. 或者直接查 Geekbench 那个 workload：libgeekbench.so 里的 Feature Matching 若用 storage image + 它自己的归约，缺屏障会让「多算」概率随调度变化（与 E1 的间歇性一致）。
3. 三条记录纪律不变：每次跑分记 UTC↔模式；结果页截图抓 validation 行；跑分窗口内不做 CPU 重活。