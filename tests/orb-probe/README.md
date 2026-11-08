# tests/orb-probe — ORB 验证竞态的最小 Vulkan 探针

目的：用一个**完全确定、CPU 端有闭式期望值**的 compute 流程，复刻 Geekbench 7
Feature Matching（ORB）用到的机制（per-invocation atomic 累加 + shared 归约 +
barrier + GPU 侧生成的 indirect dispatch 参数），从而判断验证失败到底在哪一层。

**本目录的作者不运行这个程序。** GPU 归 bench-operator。这里只有源码、构建脚本和判据。

## 0. 它复刻了什么（一步一步都在同一个 command buffer 里）

每个 iteration：

1. `vkCmdFillBuffer` 清空所有 buffer（counters、args、keys 填 0xFFFFFFFF）；
2. TRANSFER_WRITE -> COMPUTE 的 barrier；
3. **dispatch A**（`--groups G` 个 workgroup，每组 64 线程）：
   - 每线程对 `count_global` 做一次 `atomicAdd(1)`；
   - 组内 shared 累加（`atomicAdd(s_cnt,1)` + `barrier()`），每组建制一次 `atomicAdd(count_shared, s_cnt)`；
   - 每线程写一个 key：`keys[gid] = gid ^ seed`（seed 固定 0x12345678），即"可见性"探针；
4. **ORB 假设里的两条 barrier**（`--barrier` 决定发哪些，见 §3）；
5. **prepare_args**：在 GPU 上从 `count_global` 算出 indirect 的 groupCount
   （`groups = (count + 63) / 64`）写进 args buffer —— 这就是"indirect 参数由 GPU 产生"的那条路；
6. **dispatch B**：`vkCmdDispatchIndirect(args)`（`--indirect off` 时改成直接
   `vkCmdDispatch(G)`，用来单独隔离 indirect 路径）；
   - `slot = atomicAdd(seq, 1)`，把 slot 写进 `slots[slot % n]`；
   - 读回 `keys[slot]` 校验可见性，读错就 `atomicAdd(errs, 1)`；
   - slot >= n（多跑）同样计入 `errs`；
7. COMPUTE/SHADER_WRITE -> TRANSFER/TRANSFER_READ 的 barrier，6 个 `vkCmdCopyBuffer`
   把结果拷进 host-visible staging，fence 等待，非 coherent 时 invalidate，然后逐项比对。

CPU 端期望值是闭式的：

| 字段 | 期望 |
| --- | --- |
| counter_global | `n`（n = groups * 64） |
| counter_shared | `n` |
| seq (dispatch B 的执行次数) | `n` |
| errs | 0 |
| args | `(n/64, 1, 1)` |
| keysA[i] | `i ^ 0x12345678` |
| keysB | 恰好是 0..n-1 的排列：无 0xFFFFFFFF 空洞，且 sum == n(n-1)/2 |

任何一项不符都会打印 `iter=<i> mismatches=<k> <detail>`，**每个字段都带
`exp=/act=/delta=%+lld` 的有符号差值**。Geekbench 的失败签名是"GPU 偏高 24-28%
（CPU 恒为 162，GPU 201/208）且符号恒定"，所以：

> delta 为正、且表现为"多跑了一遍/多算了一批" → 优先怀疑 indirect groupCount 来源、
> barrier 缺失导致的重算、计数被重复累加。delta 双向乱跳 → 更像内存/coherency 层。

只有第一个不一致的 iteration 会打印（`--print-all` 打开全部）。

## 1. 构建（纯编译，不碰 GPU）

```sh
cd tests/orb-probe
./build.sh                    # 两个二进制
HOST_ONLY=1 ./build.sh        # 只出宿主版（NDK 不在时的降级路径）
SPV_ONLY=1 ./build.sh         # 只编译三个 SPIR-V
ANDROID_NDK_ROOT=/opt/android-ndk ./build.sh
```

产物：

| 文件 | 说明 |
| --- | --- |
| `build/dispatch_a.spv`, `prepare_args.spv`, `dispatch_b.spv` | glslangValidator 出的 SPIR-V（vulkan1.1） |
| `build/shaders_spv.h` | python3 生成的嵌入数组（宿主没有 xxd，不要用 xxd） |
| `build/orb-probe-host` | 宿主 glibc 版，**dlopen** 找 libvulkan，链接期不需要 -lvulkan |
| `build/orb-probe-android` | x86_64 bionic 版，给 waydroid guest 用 |

**为什么是两个二进制而不是一个**（诚实说明）：这台机器上 `/usr/lib64/libc.a` 不存在、
也没有 musl 工具链，所以做不出"一个静态 glibc 二进制"；bionic 二进制又不能在宿主跑。
两个二进制来自**完全相同的源码和相同的 SPIR-V**（`--spv-dir build` 可以让某个二进制
改用磁盘上的 .spv，用来排除嵌入头不一致）。因此本探针的判据是
"**同一份源码、两套 loader/驱动栈**"，而不是字面意义上的同一个 ELF。
宿主/guest 的差异只来自 loader + ICD + HAL，这正是我们要区分的层。

工具链事实（已核实）：宿主有 `gcc`/`cc`/`clang`、`/usr/bin/glslangValidator`、
`python3`、`objcopy`；**没有 glslc、没有 xxd**。NDK r30 在 `/opt/android-ndk`，
`toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android33-clang` 可用。
宿主 loader：`/usr/lib64/libvulkan.so -> libvulkan.so.1.4.341`。

## 2. `--list-types`（不需要 dispatch，先给 lead 选 Run B 的 index）

```sh
./build/orb-probe-host --list-types
```

打印：loader 名、instance apiVersion、`VK_ICD_FILENAMES`/`VK_DRIVER_FILES`、
每个 device 的 name/type/apiVersion/driverVersion/vendor+device id/
driverID+driverName+driverInfo、queue family、每个 memory heap 的大小与 flags、
以及**内存类型表**：

```
memory types usable for our storage/transfer buffers (memoryTypeBits=0x...):
  type[0] heap=1 flags=DEVICE_LOCAL usable=yes
  type[1] heap=1 flags=DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT|HOST_CACHED usable=yes
  ...
staging type: -> type[1] flags=...
```

`usable=yes|no` 是用**真实的** `vkGetBufferMemoryRequirements`（对一个
STORAGE|TRANSFER_SRC|TRANSFER_DST buffer）算出来的，不是猜的。lead 要的
"Run B 干预开关指向哪个 index" 就看这张表里 `DEVICE_LOCAL|HOST_VISIBLE` 的那些行。
注意：这张表用的是**被选中的 device**（本机只有一个 GPU，device 0）。

`--list-types` 会创建 instance + device + 一个临时 buffer，但**不做任何 dispatch、
不提交任何 queue**（毫秒级）。即便如此，它作为 Vulkan 程序仍然算"跑 GPU 工具"，
由 bench-operator 在确认没有跑分窗口时执行。

## 3. 运行矩阵

`--barrier` 五个取值的含义（都作用在 dispatch A 之后、prepare_args 之前）：

| 值 | 发的 barrier（src=COMPUTE/SHADER_WRITE） | 用途 |
| --- | --- | --- |
| `full`（默认） | ->COMPUTE/SHADER_READ + ->COMPUTE/SHADER_WRITE + ->DRAW_INDIRECT/INDIRECT_COMMAND_READ | 正确的全同步，对照基线 |
| `no-pass2` | read + args | 缺"写-写"（ORB 里同一批 candidate 被多 pass 重写的那种） |
| `no-args` | read + pass2 | 缺 indirect 参数的可见性（args 被 GPU 读之前没同步） |
| `none` | 只留 init 和最后拷贝的 barrier | 完全没同步，看驱动是否会"碰巧对" |
| `memory` | 一条 ALL_COMMANDS 的 MEMORY_WRITE->MEMORY_READ|WRITE | 粗粒度但正确的替代写法 |

推荐顺序（每步都是独立一行，便于判定）：

```sh
# A. 宿主原生（580 ICD）：基线
./build/orb-probe-host --iterations 300 --mem-type auto --indirect on --barrier full --json
# B. 宿主：只改 barrier
./build/orb-probe-host --iterations 300 --barrier no-pass2 --json
./build/orb-probe-host --iterations 300 --barrier no-args  --json
./build/orb-probe-host --iterations 300 --barrier none     --json
./build/orb-probe-host --iterations 300 --barrier memory   --json
# C. 宿主：隔离 indirect
./build/orb-probe-host --iterations 300 --indirect off --json
# D. 宿主：遍历 ICD 暴露的全部可用内存类型
./build/orb-probe-host --iterations 300 --mem-type all --json
```

guest 侧（bench-operator 用 dev/env.sh 的 guest() 或 lxc-attach）：

```sh
sudo -n /usr/local/sbin/wd-deploy push tests/orb-probe/build/orb-probe-android orb-probe
# 然后 guest 内：
/data/local/tmp/orb-probe --list-types
/data/local/tmp/orb-probe --iterations 300 --mem-type auto --indirect on --barrier full --json
# 再按 §3 的 barrier 矩阵与 --mem-type N（lead 从 --list-types 选出的 index）各跑一遍
```

耗时量级：一次 variant = K 次 iteration × 约 2×n 个 invocation（默认 n=65536）。
默认 `--iterations 300 --groups 1024` 在 GTX 1080 上远低于 1 秒 GPU 时间，
整套矩阵（宿主 + guest + barrier 变体）分钟级以内，不会挤占跑分窗口太久 —— 但仍要
**串行**，不许和 3DMark/Geekbench 同时跑。

## 4. 要收集什么

每个 variant 一行 `RESULT`，加上（`--json`）一行 JSON；最后一次 `ORB_PROBE_DONE` 是总览。
建议 bench-operator 把完整 stdout 存成
`artifacts/<run-label>/orb-probe-host.txt` 与 `orb-probe-guest.txt`（含命令行 echo、
`--version` 式的 loader/device 头、全部 RESULT 行），必要时加 `--print-all` 留一份
详细版。exit code：0 = 全部一致，1 = 有 mismatch，2 = 建立失败（loader/device/内存类型）。

## 5. 判据表（写成给 lead 可直接引用的形式）

| 宿主原生 | guest | 结论（置信度） |
| --- | --- | --- |
| clean | dirty | 嫌疑在 guest 这一层：HAL/gralloc/AHB 翻译或 guest loader 的 ICD 交付（高） |
| dirty | dirty | 嫌疑在驱动/算法本身，两套栈都触发（高）；宿主那份对应闭源 580 路径 |
| clean | clean | 本机制**不能**解释 ORB 失败，或探针规模/时序不足以触发（这是重要的负结果，别忽略） |

| 观察 | 结论 |
| --- | --- |
| `full` clean，`none`/`no-pass2`/`no-args` dirty | 缺 barrier 就会出错 —— 应用侧同步缺陷（对照：Mesa 上游 fe558d83281 "& 写成了 |" 那类 bug 就是这个模式） |
| `full` 都 dirty | barrier 写对了也没用 → 硬件/驱动级的可见性缺陷，不是应用同步问题 |
| 只有 `--indirect on` dirty，`--indirect off` clean | indirect 参数产生/读取那条路（groupCount 来源）；对应 ORB"多算一批"的偏高签名 |
| 只有某个 `--mem-type N` dirty | 内存类型/coherency steering（HAL 的 AHB 内存类型选择） |
| keysA 错但 counter 对 | 纯可见性/缓存失效问题 |
| counter/seq 偏高（delta>0） | 多执行/重复累加，而不是读旧值 |

## 6. 诚实边界

* 这不是 Geekbench。它只复刻机制，不复刻 ORB 的真实数据、规模与 pass 结构。
* 探针的同步是**确定性的**：它检测"某个 barrier 缺失/某条路径不一致时会不会错"，
  而不是"在真实时序下偶发不错"。因此 **clean 不能证明 ORB 就没问题**；
  但**dirty 可以直接定位机制**，这是它的价值。
* Geekbench 的 ORB 失败是间歇的（E1：同一模式 run1 PASS、run2 FAIL）。探针若 clean，
  也可能是真实竞态窗口需要特定的交叠；可以把 `--iterations` 提到 3000 再试一遍
  （仍是秒级）。
* `--list-types` 在 guest 内还会顺带暴露一件事：guest 的 `/system/lib64/libvulkan.so`
  到底把哪个 ICD（`/vendor/lib64/hw/vulkan.nvidia.so`？virtio？）交给应用。
  如果 guest 侧 `--list-types` 直接报 "no Vulkan physical devices"，那本身就是一条
  重要证据（loader 层交付问题），请原样保留输出。

## 7. 纪律

* GPU 只有一块：**bench-operator 跑，其他人不跑**。
* 编译/构建是 CPU 活，同样不要在跑分窗口内做（本目录的 build.sh 只编译）。
* 不要为了看结果去 `am start`、重启容器或切模式。
