# 硬解可行性：nvidia-vaapi-driver × waydroid-atv × Mesa Venus

研究日期 2026-10-17。工作目录 /home/quinovax/waydroid-nvidia，分支 dev（git log HEAD
cbd1a9a sync: adopt upstream public-test, keep this fork's venus stack）。本文所有结论按
「已验证」（附命令/日志/源码位置）与「推测」（显式标注）分开。

机器状态：GTX 1080（GP104 = PASCAL_B），闭源内核模块 580.178.04，访客 LineageOS 20 /
Android 13，驱动模式 venus（ro.hardware.vulkan=virtio、mesa.vn.debug=vtest）。

---

## 1. 结论摘要

1. venus 路线上硬解不可行，且不是"还差一点"，是架构上冲突：venus 的全部价值是"访客内没有
   NVIDIA 用户态、只有 venus 客户端"；而 nvidia-vaapi-driver 的 direct backend 要直接
   open("/dev/nvidiactl") + open("/dev/nvidia0")（src/direct/nv-driver.c:387,392），
   并要求 dlopen("libcuda.so.1") 后 cuInit(0)（src/vabackend.c:283-296）——
   即它要求访客里有 NVIDIA 的 glibc 用户态。两条路线的隔离模型互斥。
2. 推荐路线：(D) 把 NvC2Component 的 Pascal gate 打开，其次 (E) glibc-shim + 宿主 NVDEC 用户态。
   原因见 §6：Pascal 的 NVDEC 在闭源驱动上已被 NVIDIA 自己的用户态（libnvcuvid.so.580.178.04）
   支持，gate 是"本仓库没实现/没验证"而非"架构不支持"（§3）。
3. Pascal 能否吃到硬解：能，但只能走 NVIDIA 自己的 NVDEC 用户态或本项目自研 RM 通路，
   不能走 VA-API-in-guest。VA-API 那条要求访客里有 glibc 用户态（§5 A/E）。
4. 换 open-gpu-kernel-modules 不能救 Pascal：官方 README 明确 "can be used on any Turing
   or later GPU"，设备表 745 行里 GTX 1080 的 PCI ID 1B80 出现 0 次（§2.2）。
5. README.md:50「550–610, closed module → venus ❌ reported not working」在本机被证伪：
   闭源 580.178.04 + Pascal + venus 完整可用（boot=1、sf=running、ANGLE over venus 正常
   渲染）。原始依据已定位到 commit aa7d11f，是 2026-10-16 的文档改写，没有 issue/实验
   记录支撑（§2.1）。
6. glibc-shim 的 580 阻塞点已被本地修复：make 'out/.checked' 从 FAIL → PASS
   （393 个 glibc 名，6 个 front 全导出）。补丁 = dispositions.tsv 加 5 行 + 删 13 行 stale
   + core/pthread.c 去掉一个已无引用者的 export（§4）。
7. 工作量估计：(D) 路线 3–8 人周（NVDEC Pascal 类的方法流是主要未知量）；
   (E) 路线 2–5 人周（shim 已修通，剩下 guest 内 libnvcuvid 的 shim 适配）；
   (A)/(B)/(C) 均 6 人周以上且无上游先例，不建议。
8. waydroid-atv 的 c2.ffmpeg 服务本身与 A13 兼容性存疑但可移植：它的 frameworks/av
   commit 只有 5 个文件、且都是小改动（§5.6），不需要整棵 300 GB 的树；但 FFmpeg 侧
   av_hwdevice_ctx_create(..., "android", ...) 走的是 vaGetDisplay(&mDisplay) +
   /dev/dri/renderD128 的 DRM 路径（§5.5），对 nvidia-vaapi-driver 的 direct backend
   不成立（direct backend 不注册 VA/DRM driver，只走 cuvid API）。

---

## 2. Q1 矩阵：3 模式 × {启动, 渲染, 解码} × {闭源 580.178.04, open module}

### 2.1 「venus 在闭源模块上 reported not working」的原始依据（Q1 首要子问题）

| 问题 | 答案 | 证据 |
|---|---|---|
| 这句话从哪来 | commit aa7d11f（2026-10-16 22:57）引入 README 表格，与 7775030（README: support matrix as tables）同属"把证据整理成表"的一次文档改写 | git blame -L 50,50 README.md → aa7d11f；git log --oneline -S "550" -- README.md → 7775030、e05e7e4 |
| 有 issue/实验记录吗 | 没有。仓库内 .workbuddy/memory/*.md、STATE.md、NEXT-ROUND.md、state-log-archive.md 全部 grep reported not working / needs open module → 0 命中 | grep -rn "reported not working\|needs open module" --include=*.md . → 空 |
| 本机实测 | 闭源 580.178.04 + Pascal + venus 完整可用 | 见下 |
| 结论 | 该格是文档层面的推断（把 "venus 宿主侧要 dma-buf/syncobj → 直觉上要开源模块" 直接写成了 ❌），没有任何本仓或上游的实验/issue 依据。已在 §2.3 用本机实测反证 | — |

本机 venus 实测（2026-10-17，未重启、未改配置）：

    $ sudo -n /usr/bin/lxc-attach -P /var/lib/waydroid/lxc -n waydroid -- sh -c 'getprop ...'
    boot=1 / sf=running / vulkan=virtio / egl=angle
    $ ./dev/health
    GLES: Google Inc. (NVIDIA), ANGLE (NVIDIA, Vulkan 1.3.312 (NVIDIA NVIDIA GeForce GTX 1080
      (0x00001B80)), NVIDIA-580.178.4.0), OpenGL ES 3.1 (ANGLE 2.1.28324 git hash: c1a25085dd9e)
      wd-venus: active ; crash Cmdline count: 0

即 venus 把访客 Vulkan 代理到宿主闭源驱动的路径完全正常。「venus 要开源模块」这个前提是错的。

### 2.2 open module 对 Pascal 的支持（Q1.2，关键分叉点）

已拉取官方仓库（codeload.github.com/NVIDIA/open-gpu-kernel-modules @ main）核对：

- README.md:183：**"The NVIDIA open kernel modules can be used on any Turing or later GPU"**。
- 设备表 745 行中：GTX 1080 (1B80)、GTX 1080 Ti (1B06)、GTX 1060 (1C03)、
  TITAN V Volta (1D81)、GP100/GV100 (1DBx) 命中数全部为 0；
  表从 1E02 NVIDIA TITAN RTX（Turing）开始。
- 另核对了驱动包 /usr/share/doc/xorg-x11-drv-nvidia-580xx/html/supportedchips.html：
  Pascal 确实在闭源驱动支持列表里（NVIDIA GeForce GTX 1080 在第 721 行）。

判定：open-gpu-kernel-modules 不支持 Pascal。换 open module 对 GTX 1080 不可行，
本机必须留在闭源模块上。（这同时意味着 README 矩阵里「580–610, open module → venus ✅」
这一格对本机不适用，本机对应的是「550–610, closed module」那一行，而该行的 venus ❌ 是错的。）

### 2.3 驱动模式矩阵（本机 580.178.04 / 闭源 / Pascal / glibc 2.43）

| 模式 | 启动 | 渲染 | 解码 | 证据 |
|---|---|---|---|---|
| venus（当前） | ✅ | ✅ ANGLE over venus，实测见 §2.1 | ❌ 无任何视频通路；docs/transport-design.md 全文 91 行无 video/dmabuf-decode 章节；virglrenderer/vtest/ 无 video 命令 | dev/health；grep -i video docs/transport-design.md → 空 |
| nvidia-prolapse | 🧪（未在本机实跑，门禁见下） | 🧪 | ❌ Pascal 被 gate 在 Turing，见 §3 | src/video/mod.rs:509-558、src/video/session.rs:265-292 |
| glibc-shim | 🧪 见 §4（本地已修通 build） | 🧪 | ⚪ 未实现：libnvcuvid.so 从未被 shim 过（grep 全仓 0 命中） | grep -rniE 'nvcuvid\|nvidia-encode\|cuvid' nvidia-prolapse/ → 0 |

门禁代码（waydroid/nvidia_native.py）：

- driver_mode() :369-433：三种模式都由 nvidia_driver_mode 显式选择，venus 额外要求
  drm_device = NVIDIA render node（:459-461）与 nvidia_guest_layout=2（:462-467）。
- check_venus_preconditions 只查 render node 与 socket，不查模块是否开源——
  代码层面从来没有"venus 需要 open module"这个门禁，README:50 纯属文档笔误/推断。
- nvidia-prolapse 要求 nvidia_prolapse_install = vendor-image（走 sudo waydroid-nvidia-setup）
  或 dev-root（dev/source-native stage + sudo -n wd-native activate）（:403-413）。
  本机当前没有部署 vendor image，所以本机切 nvidia-prolapse 需要先跑 setup。

关于 "610.57 needs nvidia-drm.modeset=1"：本机 /sys/module/nvidia_drm/parameters/modeset
读为空（未加载 nvidia_drm 模块），但 venus 走的是 nvidia-drm 的 renderD128 导出路径且
实测正常，说明本机 modeset 状态不妨碍 venus。docs/troubleshooting.md:40 说 modeset=0
会禁用全部 DMA-BUF，本机 renderD128 的 dma-buf 导入导出实际可用（dev/health 报
present path: same-GPU direct），推测 modeset 已由 udev/initramfs 打开而 sysfs 参数节点
不可读（容器内）。此条为推测，未进一步验证。

---

## 3. Q1.3 / Q2(D)：Pascal 在 NvC2Component 里被拒的确切位置与判定

### 3.1 Gate 的完整链条（file:line）

| 层 | 位置 | 内容 |
|---|---|---|
| 类表 | driver-source/native/src/video/mod.rs:103-120 | NVDEC_CLASSES 共 11 项：Rubin D2B0、Blackwell CEB0/CFB0/D1B0/CDB0、Ada C9B0、Ampere C7B0/C6B0、Hopper B8B0、Turing C4B0（bit 0x0001_0000）、Volta C3B0（bit 0）。表里没有 Pascal 的 NVDEC 类 |
| 代际谓词 | mod.rs:139-141 | turing_plus() = bit & 0x1fff_0000 != 0，注释原文 "Mask 0x1fff0000: Turing and newer (stock supports nothing below)" |
| caps 决策 | mod.rs:509-558 | let mut stock = g.turing_plus() && class_gap(codec, class).is_none() && engine.decodes(codec); → c.supported = u32::from(stock && tested); |
| tested 判据 | mod.rs:511-514 | 类必须在 NVDEC_CLASSES 且第 4 列为 true（在本仓硬件上跑过 bit-exact），否则需 NV_VIDEO_UNTESTED_CLASSES=1 |
| session 层 | session.rs:265-292 | class == 0（RM 未报任何已知 NVDEC 类）→ Unsupported("GPU exposes no NVDEC class")；caps.supported == 0 时按 class_gap → !turing_plus() → !engine.decodes() → AV1 → known.is_some_and(|c| !c.3) → "unknown" 的顺序给出拒绝理由 |
| 类发现 | mod.rs:635-639 | nvdec_class = NVDEC_CLASSES.iter().map(|c| c.0).find(|c| classes.contains(c)).unwrap_or(0) —— 只在 11 个已知类里找，Pascal 的类根本不在候选集，直接得 0 |
| Codec2 组件 | media/core/nvc2_caps.cpp:21-44 | 只 continue（不报错）当 !c.supported；若全部不支持，NvC2Component.cpp:758-761 报 "GPU <name> supports none of VP9/AV1/HEVC/H.264 decode" 并拒绝启动整个 store |
| 文档 | README.md:59-60 | "H.264/HEVC/VP9/AV1 via NVDEC, 🧪 bit-exact on Turing, Ampere, Ada and ✅ Blackwell; not before Turing" |

### 3.2 判定的关键：这是「架构不支持」还是「未实现」？

证据指向「未实现/未验证」，不是架构不支持。四条理由：

1. NVIDIA 自己的用户态在 Pascal 上提供 NVDEC：本机装有
   /usr/lib64/libnvcuvid.so.580.178.04（20,749,912 B）与 libcuda.so.580.178.04，
   modinfo nvidia 为闭源 license: NVIDIA。宿主侧 nvidia-vaapi-driver direct backend 实测
   vaInitialize status=0、vendor 串 VA-API NVDEC driver [direct backend]、H.264 profile 可用，
   ffmpeg 端到端 CPU 4.1 核 → 0.55 核（NEXT-ROUND.md §2.1/§5）。Pascal + NVDEC 是工作的。
2. gate 的语义是"stock supports nothing below"，但这是引用 NVIDIA 闭源用户态的策略：
   session.rs:273-274 的拒绝理由是 "stock NVDEC does not decode it on this class"。
   "stock" = NVIDIA 闭源 blob 的策略，不是 RM/硬件的能力边界。而且 mod.rs:103-102 的注释
   自述这些类的 bit-exact 性是"rental run 20261015-nvdec-xgen3"在租来的 GPU 上做的——
   也就是说 tested 这一列是验证覆盖度，不是硬件能力。
3. 提交路径本身已支持 pre-Volta：driver-source/native/COMPATIBILITY.md:22 原文
   "Maxwell/Pascal use pre-Volta submission; Volta and newer use a selected USERMODE class"，
   且 src/capabilities.rs:128-145 对 sm < 70（Maxwell 50/52/53、Pascal 60/61/62）都有
   完整的 Submission::PreVolta + 0xc06f/0xb06f GPFIFO + generic_kind = 0xfe 分支，
   并有单元测试 maxwell_has_no_usermode_and_old_generic_kind()（capabilities.rs:317-324）。
   即 3D/提交侧 Pascal 已实现，缺的只是 NVDEC 类。
4. Volta C3B0 在表里但 bit=0（mod.rs:119）：说明本仓把 Volta 也标为
   turing_plus() == false，同样不接受。若"NVDEC 从 Volta 起才有"是硬件事实，那 Volta 被拒
   合理；但这也说明表的边界是"作者选择在哪里划线"，而不是"硬件在哪里划线"。

结论：Pascal 的 gate 是「本仓库未实现 Pascal 的 NVDEC 类 + 未在 Pascal 上验证 bit-exact」，
不是「Pascal 的 NVDEC 硬件/驱动不支持」。打开它需要：(a) 把 Pascal 的 NVDEC 类加进
NVDEC_CLASSES 并给一个非零 bit；(b) 实现该类的描述符/方法流（Turing+ 对应 NVC4B0，
Pascal 对应更早的 NVCxB0 家族，tinygrad runtime/autogen/nv_570.py 只覆盖 Turing+，
这是主要未知工作量）；(c) 在 GTX 1080 上做一次 bit-exact 对比（可复用
make realtest NV_VIDEO_LIB=... IVF=in.ivf REF=ref.nv12，见 media/Makefile）。

⚠️ 我不知道：Pascal NVDEC 类的方法流与 Turing 的差异有多大。
tinygrad checkout 37132d75 的 runtime/autogen/nv_570.py 只到 Turing+，
本仓 src/video/* 也没有 Pascal 类的任何痕迹。这一步的工作量估计（3–8 人周）主要来自这个未知数。

---

## 4. Q1.4：glibc-shim 在 580.178.04 上的补表方案（已验证 PASS）

### 4.1 复现失败（先确证现状）

    $ cd nvidia-prolapse/shim
    $ make NDK=/opt/android-ndk BLOBDIR=/usr/lib64 'out/.checked'
    import-census: undispositioned import dup3@GLIBC_2.9 (libc.so.6) in libEGL_nvidia.so.580.178.04
    import-census: undispositioned import __isnan@GLIBC_2.2.5 (libc.so.6) in libEGL_nvidia.so.580.178.04
    import-census: undispositioned import __isinf@GLIBC_2.2.5 (libc.so.6) in libEGL_nvidia.so.580.178.04
    import-census: undispositioned import __getdelim@GLIBC_2.2.5 (libc.so.6) in libEGL_nvidia.so.580.178.04
    import-census: undispositioned import fscanf@GLIBC_2.2.5 (libc.so.6) in libnvidia-gpucomp.so.580.178.04
    ... 12 行 stale ...
    make: *** [Makefile:73：gen/census.tsv] 错误 1

（BLOBDIR 必须是 /usr/lib64：Makefile:70 默认 /usr/lib，本机 64 位 blob 在 /usr/lib64。）

### 4.2 5 个新导入全部是 identical trampoline 级（低风险）

| 导入 | 版本 | bionic 目标 | 判定依据 |
|---|---|---|---|
| dup3 | GLIBC_2.9 | dup3 | NDK stub libc.so 有 T dup3@@LIBC（偏移 0x1adb0）；glibc int dup3(int,int,int) 与 bionic 同签名（/usr/include/unistd.h:560） |
| __isnan | GLIBC_2.2.5 | __isnan | bionic libm.so 有 T __isnan@@LIBC；glibc 内部 helper，纯函数 |
| __isinf | GLIBC_2.2.5 | __isinf | bionic libm.so 有 T __isinf@@LIBC；同上 |
| __getdelim | GLIBC_2.2.5 | getdelim | bionic 无 __getdelim 但有 getdelim@@LIBC（glibc 的 __getdelim 是 getdelim 的内部别名，/usr/include/stdio.h:693）；走「renamed identical」路径，与既有的 __isoc99_fscanf → fscanf（dispositions.tsv:12）同型 |
| fscanf | GLIBC_2.2.5 | fscanf | bionic libc.so 有 T fscanf@@LIBC；表里已有 sscanf identical、vfprintf identical、__isoc99_fscanf → fscanf，同类 |

这 5 个没有一个涉及 glibc 结构体布局、常量差异或 nvg_* 翻译器，全部是
「同一个符号在 bionic 里存在且签名相同」。gen-constants.py 的 CONSUMERS 交叉检查
（sysconf/nl_langinfo/newlocale/dlopen/sigaction/clock_gettime 等）也不覆盖它们，
所以不会有常量不匹配风险。

### 4.3 最小补丁（已实测，勿再手工推导）

三处改动，全部在 nvidia-prolapse/shim/：

(1) dispositions.tsv：删除 13 行 stale（580 的 blob 不再导入这些符号）：

    __sched_cpucount / ceilf / faccessat / flock / get_nprocs / isprint / openat64 /
    pthread_attr_destroy / pthread_attr_init / pthread_attr_setstacksize /
    pthread_join / trunc / truncf

(2) dispositions.tsv：追加 5 行：

    dup3	identical	dup3	-
    __isnan	identical	__isnan	-
    __isinf	identical	__isinf	-
    __getdelim	identical	getdelim	-
    fscanf	identical	fscanf	-

(3) core/pthread.c:191 去掉 NVG_EXPORT（这个符号已无 front 引用）：

    -NVG_EXPORT int nvg_pthread_attr_init(pthread_attr_t *attr) {
    +int nvg_pthread_attr_init(pthread_attr_t *attr) {

第三处为什么必须：import-census.py:174-186 会把「既不被导入、runtime 列又是 -」的行
报成 stale；删掉 pthread_attr_init 行后，gen-fronts.py:105-116 不再把
nvg_pthread_attr_init 加进 gen/core.exports，而 core/pthread.c 仍 NVG_EXPORT
导出它 → check-artifacts.py:127 报 libnvglibc.so: unexpected export nvg_pthread_attr_init。
这是死代码（nvg_pthread_attr_init 只被 attr_init_glibc() 内部调用，
见 core/pthread.c:191-193），去掉 export 不影响任何行为。

### 4.4 验证命令与结果

    cd /home/quinovax/waydroid-nvidia/nvidia-prolapse/shim
    rm -rf gen out
    make NDK=/opt/android-ndk BLOBDIR=/usr/lib64 'out/.checked'

实测输出（尾部）：

    import-census: libc.so.6 273, libm.so.6 45, libpthread.so.0 67, librt.so.1 1,
                   libdl.so.2 6, ld-linux-x86-64.so.2 1; 375 distinct names, all dispositioned
    gen-fronts: libc.so.6 273 (246 slot, 24 core, 3 data), libm.so.6 45 (45 slot, 0 core, 0 data),
                libpthread.so.0 67 (49 slot, 18 core, 0 data), librt.so.1 1 (1 slot, 0 core, 0 data),
                libdl.so.2 6 (3 slot, 3 core, 0 data), ld-linux-x86-64.so.2 1 (1 slot, 0 core, 0 data)
    check-artifacts: libnvglibc.so: 43 exports, all nvg_*; NEEDED ['libdl.so', 'liblog.so', 'libc.so'];
                     verneed libc.so:LIBC/LIBC_O/LIBC_P, libdl.so:LIBC/LIBC_N
    check-artifacts: libc.so.6: 273/273 names exported GLOBAL DEFAULT unversioned (3 data OBJECT size 8);
                     11 LIBC* verdefs; NEEDED ['libnvglibc.so']; UND 26 (all nvg_*)
    check-artifacts: libm.so.6: 45/45 names ... ; libpthread.so.0: 67/67 ... ; librt.so.1: 1/1 ...
    check-artifacts: libdl.so.2: 6/6 ... ; ld-linux-x86-64.so.2: 1/1 ...
    check-artifacts: OK: 393 glibc names over 6 fronts

make check-abi 同样 PASS：35 x86_64 glibc constants in core/glibc_abi.h match the host glibc。

已知非阻断项：make test 仍 FAIL（/usr/lib64/libasan.so.8.0.0 与 libubsan.so.1.0.0 缺失，
gcc 的 libasan.so 是 INPUT() 链接脚本），用 make 'out/.checked' 绕过即可——这与改动无关，
是主机缺 libasan/libubsan 包。

### 4.5 能否作为「不阻塞上游」的本地补丁

能，且这是目前唯一不需要私有 state/reference/imports-*.tsv 的路径。

- make dispositions（Makefile:117-120）需要 state/reference/imports-<driver-version>.tsv，
  该目录在本仓库不存在（ls nvidia-prolapse/state → 无此目录），只能由维护者用 580 的
  blob 重跑一次 import census 生成。
- 上述补丁不依赖任何私有文件，全部信息来自 NDK stub 与本机 blob 的 ELF 动态符号表，
  可以机械复核：llvm-nm -D --defined-only 于 NDK 的 libc/libm/libdl stub
  + llvm-readelf 读 blob 的 .gnu.version_r。
- 风险控制：check-artifacts.py 的 G0 不变量仍在（"all dispositioned" + "393 names over 6 fronts"），
  也就是说补表没有绕过任何检查，只是把表更新到 580 的实际导入集。
- 上游化时的正确形态：把 dispositions.tsv 重新生成为一份 580 的（或者让 census 允许
  版本相关的差集），而不是长期靠手工补行。这一点是推测（需要维护者确认生成流程）。

---

## 5. Q2：五种架构的对比

### 5.1 先讲清架构矛盾

venus 的隔离模型（docs/transport-design.md）：访客只有 venus 客户端（vulkan.virtio.so）
+ vtest gralloc wrapper，没有任何 NVIDIA 用户态，Vulkan 调用经 unix socket 到宿主
virgl_test_server，宿主侧用 NVIDIA 驱动。

nvidia-vaapi-driver 的需求（源码直读）：
- src/direct/nv-driver.c:387,392：open("/dev/nvidiactl", O_RDWR|O_CLOEXEC) 与
  open("/dev/nvidia0", O_RDWR|O_CLOEXEC) —— 不是 renderD128。
- src/vabackend.c:283-296：cuda_load_functions(&cu, NULL) → dlopen("libcuda.so.1")
  （ffnvcodec/dynlink_loader.h:70-71 用 dlopen/dlsym）→ cu->cuInit(0)；
  失败则 LOG("Failed to load CUDA functions"); return;。
- 它的 direct backend 不注册 VA-API DRM driver，只有 EGL backend 才走
  vaGetDisplayEGL。所以 waydroid-atv 的 vaGetDisplay(&mDisplay) → DRM 路径对它无效。

矛盾是结构性的：venus 要求"访客无 NVIDIA 用户态"，VA-API 要求"访客有 glibc 用户态 + /dev/nvidia*"。

### 5.2 五案对比表

| 方案 | 改动面 | 隔离性 | Pascal 可行性 | 上游先例 | 主要风险 | 工作量 |
|---|---|---|---|---|---|---|
| (A) 访客内 VA-API + /dev/nvidia* 透传 | 大：bind 5+ 个 /dev/nvidia*；访客内 bionic libva；nvidia_drv_video.so 重编到 bionic 或借 glibc-shim | 差：venus 隔离被完全绕开；驱动栈在访客重复一份 | 取决于 nvidia_drv_video.so 能否 bionic 化 | 无（waydroid-atv 是 Intel + DRM，不是这个形态） | bionic 化不可行见 §5.3；/dev/nvidia* 透传后 zygote fd allowlist 会 abort（nvidia_native.py:487-492 实测过） | 6+ 人周 |
| (B) 宿主解码 + 帧回传 | 巨大：新 vtest 命令族（提交码流、回传 dmabuf/AHardwareBuffer、fence、格式协商） | 好（保持 venus 模型） | 好（宿主侧已验证） | 无 | docs/transport-design.md 现有 3 个补丁只覆盖 sync_fd / dmabuf import / GPU alloc，没有任何视频编解码扩展；vtest 协议要加有状态的双向数据流 | 10+ 人周 |
| (C) 混合：独立解码代理服务 | 大，但比 B 聚焦：新 systemd 单元 + 新 socket + 访客侧 C2 组件走该 socket | 好 | 好 | 无 | 与 B 同源的协议工作，但可绕开 venus 的版本协商；调试独立 | 8+ 人周 |
| (D) 扩 NvC2Component 到 Pascal | 中：NVDEC_CLASSES 加 Pascal 类 + 该类方法流 + bit-exact 验证 | 最好（完全在现有架构内） | 好（§3.2 判定为未实现） | 有：本仓自己的 Turing/Ampere/Ada/Blackwell 就是这么做出来的 | Pascal 类方法流是未知量；需 NV_VIDEO_UNTESTED_CLASSES=1 之外的正式支持 | 3–8 人周 |
| (E) glibc-shim 直用宿主 NVDEC 用户态 | 中：shim 已修通（§4）；要新增 libnvcuvid.so/libcuda.so 的 shim 与一个 CUVID→Codec2 组件 | 中（glibc-shim 本来就允许宿主用户态进访客） | 好 | 部分有：本仓 glibc-shim 模式 + shim 机制 | libcuda.so 是 96 MB 的巨库，导入集会远大于 5 个 EGL blob；zygote fd allowlist 问题同样存在 | 2–5 人周 |

### 5.3 (A) 的关键问题：nvidia_drv_video.so 能否编到 bionic？

读了源码与构建文件（~/waydroid-nv/nvidia-vaapi-driver @ a037111）：

- glibc-only 设施：基本没有。grep sysconf/__isnan/__isinf/pthread_np/scandir/secure_getenv/
  __libc/strerror_r/fts/program_invocation/backtrace/dladdr/on_exit/getline/getdelim
  于 src/*.c 与 src/direct/*.c，只命中：av1.c:3/vabackend.c:12 的 #include <malloc.h>、
  vabackend.c:43-44 的 #include <pthread_np.h>（且被 __has_include 保护，并已有
  #ifdef __BIONIC__ 分支，vabackend.c:52-62）。
- 真正的 bionic 阻塞是链接对象，不是源码：meson.build:19-24 依赖
  egl、ffnvcodec、libdrm、threads，libva_deps = dependency('libva', version: '>= 1.8.0')。
  访客内需要：bionic libva（AOSP external/libva 有）、bionic libdrm（本仓已有，
  nvidia-prolapse/libdrm 就是 upstream 2.4.134 的 bionic 构建，README:29-31）、
  bionic ffnvcodec（只是头文件 + dlopen，subprojects/nv-codec-headers-11.1.5.1/include/
  ffnvcodec/dynlink_loader.h 只用 dlopen/dlsym，可移植）、以及 bionic libEGL
  （direct backend 不直接用，但 meson 仍 dependency('egl')，可改成 required: false）。
- 还有一层：它运行时 dlopen("libcuda.so.1") 与 libnvcuvid.so.1（cuda_load_functions /
  cuvid_load_functions，vabackend.c:283-296），而这两个是 glibc-linked 的宿主 blob
  → 必须走 glibc-shim（即 §4 的补丁是前置条件），或把 libcuda 也 bionic 化（不可行，
  NVIDIA 不发 bionic 版）。

判定：源码层面可以 bionic 化，但运行时必须配 glibc-shim 才能加载 libcuda/libnvcuvid，
因此 (A) 的实际形态 = (A) 的挂载 + (E) 的 shim，(A) 的独立性是假的。

### 5.4 (B)/(C) 的关键问题：venus/vtest 能否承载？

- docs/transport-design.md 全文（91 行）只描述三件事：sync_fd 导出、dma_buf 导入、
  宿主侧 GPU 分配。没有任何视频/编解码扩展。
- patches/virglrenderer/0001-0002 提供的是 fd 传递与 blob 导入的基础设施
  （SCM_RIGHTS），可以作为 (B) 的传输地基，但协议层要从零设计：码流提交（可能几 MB/帧）、
  解码后 surface 的跨进程生命周期、fence 同步、NV12/P010 的 stride/modifier 协商、
  以及"哪个 C2 组件拥有这块 buffer"的归属问题。
- 没有上游先例：virglrenderer 的 venus 是 Vulkan 代理，不是媒体代理；Mesa 也没有
  "venus video" 分支（grep -i video 于 docs/transport-design.md 与 patches/ → 0 命中）。

### 5.5 waydroid-atv 的 VA-API 路径具体长什么样（读源码核实）

WayDroid-ATV/android_external_stagefright-plugins 分支 14-x86/ffmpeg-7.1：

- 开关：utils/ffmpeg_hwaccel.c:11 if (!property_get_bool("media.sf.hwaccel", 0)) return 0;
  → 优先 AV_HWDEVICE_TYPE_VAAPI（或 debug.ffmpeg-codec2.hwaccel.vulkan 时走 Vulkan）→
  av_hwdevice_ctx_create(&avctx->hw_device_ctx, config->device_type,
  preferred_type == AV_HWDEVICE_TYPE_VAAPI ? "android" : NULL, NULL, 0)
  → 成功后 refcounted_frames=1、thread_count=1。
- FFmpeg 侧的 "android" device：libavutil/hwcontext_vaapi.c:1943-1957
  mDisplay = 0x18C34078; display = vaGetDisplay(&mDisplay);
  —— 这个魔数是 Android 的 android_native 标记。
- libva 侧的 android 后端：WayDroid-ATV/android_external_libva va/android/va_android.cpp:74-83

      property_get("gralloc.gbm.device", drm_device, DEVICE_NAME);   // DEVICE_NAME = "/dev/dri/renderD128"
      drm_state->fd = open(drm_device, O_RDWR | O_CLOEXEC);
      drm_state->auth_type = VA_DRM_AUTH_CUSTOM;

  然后 va_DisplayContextGetDriverNames() 走标准的 VA_DRM_GetDriverNames()。
  → 它最终是一个 VA/DRM display，通过 renderD128 + drmAuth。
- android_vendor_waydroid_init/waydroid-init/hwcodecs-vaapi.cpp:49-109 确实只是探测器：
  open render node → vaGetDisplayDRM(renderFd) → vaInitialize → vaQueryConfigProfiles
  → 按 VAProfileFourCC 表发 fourcc。不是解码器。

对 NVIDIA 的含义：这条路径要求一个能注册到 libva DRM 后端的 nvidia_drv_video.so。
而 elFarto/nvidia-vaapi-driver 的 direct backend 不注册 DRM driver（只有 EGL backend 会），
所以即便把访客侧全部备齐，vaInitialize 也走不到 NVDEC。除非：(i) 用 EGL backend
（README 自述 525+ 已坏，本机 580），或 (ii) 给 nvidia-vaapi-driver 写一个 DRM 注册路径
（它内部仍会 open("/dev/nvidiactl") + dlopen("libcuda.so.1")，所以 (ii) 仍需 glibc-shim）。

### 5.6 waydroid-atv 镜像本身能否用 / A13 可移植性

- 版本差：ATV 的 android_frameworks_av 默认分支 lineage-23.2（Android 16），
  android_external_ffmpeg 分支 release/7.1/14-x86（Android 14 基线）。
  我们是 LineageOS 20 / Android 13。
- frameworks/av 的改动面比想象的小（5 个 commit，逐个 gh api .../commits/<sha> 核对）：

  | commit | 标题 | 文件数 | 规模 |
  |---|---|---|---|
  | 58558319c4 | Add media extensions for FFMPEG codec2 service | 5 | 8+5+1+1+1 行 |
  | bde7c6f643 | Add OMX extensions from stagefright-plugins | 3 | 10+3+156(新头) |
  | 702faf3f6d | libstagefright: Extended media support via FFMPEG | 16 | 1308 行新文件 + 135 行 ACodec 改动 |
  | d4cbe998be | Enable all codec2 components when debug.stagefright.ccodec=0 | 1 | 2 行 |
  | f9765d73e0 | Only enable c2.android video decoders when using minigbm and force_hwaccel is not set | 1 | 12 行 |

  前 4 个都是小改动；只有 702faf3f6d（1308 行的 FFMPEGSoftCodec.cpp）是大件，
  而它只是软解 Codec2，对硬解不是必需的。
- 没有 aosp_frameworks_av checkout（STATE.md:242-243 已警告完整 LineageOS 树 ~300 GB /
  ~5 h）。但 hwcomposer/pins.env 已经钉了 frameworks/av 95be9bad (tm-qpr3)，
  本仓 nvidia-prolapse/media/vndk/build.sh 也在用 android-13 era 的 frameworks/av 头文件
  （README:151）——所以移植工作的瓶颈是"拿一个 android-13 的 frameworks/av 检出"，
  而不是"理解 1308 行"。低成本做法：只 repo init frameworks/av 一个项目
  （~2–4 GB），而不是整棵树。
- 更轻的路（Q2 最后两问）：c2.ffmpeg.* 服务本身是一个独立可执行文件
  （codec2/Android.mk 的 BUILD_EXECUTABLE，LOCAL_REQUIRED_MODULES = service 二进制 +
  policy + media_codecs_ffmpeg_c2.xml），加上一个 .rc 和一个 VINTF manifest 片段。
  它不要求改 frameworks/av 也能编出来、装进 /vendor/bin/hw、被 init 拉起来；
  frameworks/av 的那些 commit 决定的是"MediaCodecList 是否列出它 / c2.android 是否被压下去"。
  → 最小可验证路径：只编 + 只装 c2.ffmpeg 服务，用 media.sf.hwaccel=1 试
  av_hwdevice_ctx_create(..., "android", ...)，看它在 A13 上能否起来。
  这条不需要 frameworks/av，也不需要 300 GB 的树。

### 5.7 venus 路线下 media.sf.hwaccel=1 的语义（明确表态）

在 venus 路线上，media.sf.hwaccel=1 是无意义的，atv 这套在 venus 路线上根本无解。

理由：media.sf.hwaccel=1 让 FFmpeg 优先 AV_HWDEVICE_TYPE_VAAPI，而 vaapi 的 android
后端（§5.5）必须 open("/dev/dri/renderD128") 并注册一个 VA driver。venus 访客里：
- 没有 libva（实测访客 find /system /vendor -name 'libva*' 无果，memory/state-log-archive.md:564）；
- 没有 VA driver；
- renderD128 虽在容器内，但 NVIDIA 的 VA driver 需要 /dev/nvidiactl + libcuda.so（§5.1）。

必须走 (A)/(B)/(C)/(D)/(E) 之一。(A) 与 (E) 都要求访客内有 NVIDIA 用户态，(B)/(C) 要求新协议，
只有 (D) 完全不动访客的媒体栈形态。

---

## 6. Q2 推荐路线

### 6.1 首选：(D) 在 nvidia-prolapse 上把 NvC2Component 扩展到 Pascal

为什么：

1. 它是唯一不引入 VA-API、不引入访客 NVIDIA 用户态、不新协议的路，完全复用已有的
   Codec2 集成（NvC2Component.cpp + service.cpp + stage-odm.sh + nvc2-xmlgen）。
2. §3.2 已判定 gate 是"未实现/未验证"，不是架构限制；提交路径（pre-Volta SYS GPFIFO）
   在 capabilities.rs 里已经写好并有单元测试。
3. 宿主的 NVDEC 在 Pascal 上被 NVIDIA 自己的用户态支持（§3.2 理由 1），说明硬件/驱动侧
   没有障碍，缺的是本仓的方法流。
4. 与 Q1.4 无关：这条路不需要 glibc-shim 补丁，也不依赖 libcuda/libnvcuvid 进访客。

代价与风险：Pascal NVDEC 类的方法流在本仓完全没有痕迹，tinygrad 的 autogen 也只到 Turing+。
这是 3–8 人周估计的主要来源，也是最大的未知。

### 6.2 备选：(E) glibc-shim 模式下直接用宿主 NVDEC 用户态

为什么是备选而不是首选：

- 优点：§4 已把 shim 的 build 修通；宿主 libnvcuvid.so.580.178.04 与 libcuda.so.580.178.04
  现成；不碰访客的 Codec2 服务形态（可以用一个自定义 C2 组件包住 CUVID）。
- 缺点：libcuda.so 96 MB，其 glibc 导入集大概率远大于 5 个 EGL blob 的 393 个名字，
  §4 的"加 5 行"做法会变成"加几百行"；且 glibc-shim 模式本身还是 ⚪（未测试），
  要把整套（shim + HAL + gralloc + EGL front）跑起来才谈得上解码。
- 另外 zygote 的 fd allowlist 问题（nvidia_native.py:487-492）在原生模式下由
  ro.zygote.disable_gl_preload=true 解决，这条对 (E) 同样适用，不是新增阻塞。

### 6.3 第一个可验证的最小实验（1 小时内出结论）

目标：确认 Pascal 的 NVDEC 类能否被 RM 枚举到，以及本仓的 RM 后端能否给它建 channel。
纯主机侧，不碰容器、不碰访客、不改任何生产配置：

    # 1) Pascal 的 NVDEC 类是否被 RM 广告（闭源 580.178.04）
    #    NV2080_CTRL_CMD_GPU_GET_CLASSES 后 grep Pascal NVDEC 类
    #    （类表参考 Mesa 的 src/gallium/drivers/nouveau/nvidia/headers/ 下 clc2b0.h 一类头）

若该类在类列表里 → gate 确认为"未实现"，(D) 的 3–8 人周估计成立，可以立项。
若不在 → Pascal 的 NVDEC 根本不由 RM 广告，(D) 需要先查 GSP 是否暴露它，(D) 直接降级，
改推 (E)。

（本机 /proc/driver/nvidia/gpus/*/information 已确认 Model = GeForce GTX 1080、
PCI 0000:01:00.0、DMA 47-bit，这是实验的前置输入，不需要重测。）

---

## 7. Q3：fcitx5 输入法协议支持

### 7.1 现状（已验证）

- 宿主：fcitx5 已装且在跑。
  rpm -q fcitx5 → fcitx5-5.1.23-1.fc44.x86_64；
  pgrep -a fcitx5 → /usr/libexec/fcitx5-wayland-launcher --reopen + /usr/bin/fcitx5；
  环境变量 GTK_IM_MODULE=fcitx / QT_IM_MODULE=fcitx / XMODIFIERS=@im=fcitx / INPUT_METHOD=fcitx
  全部已设；GTK3 immodule im-fcitx5.so 与 Qt5 libfcitx5platforminputcontextplugin.so 都在。
- HAL：grep text_input/input_method/zwp_input 于 hwcomposer/*.cpp、*.h、patches/*.patch
  → 零命中（只有 wl_keyboard/wl_seat 的物理键盘路径）。上游 7750307 也没有。
- 访客：ime list -a 只有 com.android.inputmethod.latin/.LatinIME；
  settings get secure default_input_method = com.android.inputmethod.latin/.LatinIME；
  即访客用的是 Android 自带软键盘。
- 物理键盘直通已经能用（这可能比预期好）：HAL 实现了完整的 wl_keyboard_listener，
  keyboard_handle_key() → send_key_event() → 写 /dev/input/wl_keyboard_events 管道
  （hwcomposer/wayland-hwc.cpp:1192-1215, 1294-1301）；访客 dumpsys input 里有
  wayland_keyboard 设备（Path: /dev/input/wl_keyboard_events，Classes: KEYBOARD | ALPHAKEY | DPAD，
  Enabled: true）。keyboard_handle_keymap() 还把 XKB layout 名写进
  waydroid.keyboard_layout 属性（:1220-1245）。
  注意：本机 getprop waydroid.keyboard_layout 读为空——推测是 keymap 事件在容器
  启动早期到达、property_set 早于 prop 服务就绪，或 KWin 给的 layout 名解析失败。
  此条为推测，未验证。

### 7.2 KWin 侧实际暴露的协议（wayland-info 实测，未猜）

67 个 interface，与输入法相关的全部存在：

    interface: 'wl_seat',                          version: 10, name: 11
    interface: 'zwp_text_input_manager_v2',        version:  1, name: 69
    interface: 'zwp_text_input_manager_v3',        version:  2, name: 70
    interface: 'zwp_input_method_v1',              version:  1, name: 37
    interface: 'zwp_input_panel_v1',               version:  1, name: 38

→ "KWin 只提供 text-input，不提供 input-method" 这个前提在本机不成立：
KWin 6 同时提供 zwp_text_input_manager_v3（客户端侧：把文本输入请求发给 compositor）
和 zwp_input_method_v1（服务侧：让输入法面板作为 compositor 的输入法）。
fcitx5 走的是 zwp_input_method_v1（fcitx5-wayland-launcher 就是干这个的）。

### 7.3 最小可行范围判定

- 若目标是「访客里的 Android 输入框能调起宿主 fcitx5」，需要的是 zwp_text_input_v3 桥：
  HAL 作为 text-input 客户端，把 enter/leave/text/commit_state/preedit 转发给 Android 的
  InputMethodManager（或直接注入到当前 focused window 的输入连接）。
  不需要 input-method-v2（那是给"输入法面板本身"用的）。
- 若目标是「宿主 fcitx5 的候选词面板显示在 Waydroid 窗口上方」，那才需要
  zwp_input_panel_v1 + HAL 侧给面板一个 surface。
- Waydroid 官方方案：靠 Android 自己的软键盘（LatinIME），物理键盘走
  wl_keyboard → evdev 管道（§7.1）。上游没有 text-input 桥。

### 7.4 分阶段计划

| 阶段 | 内容 | 前置 | 判定标准 |
|---|---|---|---|
| 1. 物理键盘直通 | 已可用。剩余工作只有查清 waydroid.keyboard_layout 为什么空 | 无 | getprop waydroid.keyboard_layout 非空；实体键盘按键能在访客生效 |
| 2. text-input 桥 | HAL 加 zwp_text_input_manager_v3 客户端：wl_seat 已有（name 11），只需 bind text-input；把 enter(surface)/leave/commit_string/preedit_string/delete_surrounding_text 翻译成访客侧输入（最省事：写 /dev/input/wl_keyboard_events 不够，需要走 InputMethodManager 或 InputManager.injectInputEvent） | 阶段 1；需要访客侧一个接收端（HAL 已有 binder 服务可扩） | 在访客输入框里打字，字符经 KWin→HAL→访客 |
| 3. 完整输入法面板 | zwp_input_panel_v1 给 fcitx5 面板一个 overlay surface；处理 input-method-v1 的 activate/deactivate 与光标矩形 | 阶段 2 | 候选词面板出现在 Waydroid 窗口上方，位置跟随光标 |

阶段 2 的真正难点不是协议，是"把 compositor 的文本提交送进 Android 的输入连接"：
Android 的输入链路是 InputDispatcher → focused window → Editor → InputConnection，
HAL 现在是写 evdev 管道（绕过了 InputConnection，所以没有 preedit/候选词）。
要桥 text-input，最小做法是让 HAL 在收到 commit_string 时找到当前 focused window 的
InputConnection 并 commitText——这需要访客侧配合（WaydroidService 之类）。
此设计为推测，需要先读 hwcomposer/wayland-hwc.cpp 的 input 管道与访客
IWaydroidTask/InputFlinger 的交互才能定。

### 7.5 记录：宿主 fcitx5 行为

- KWin 下 fcitx5 通过 zwp_input_method_v1 工作（不是 XIM），
  fcitx5-wayland-launcher --reopen 负责在 Wayland 会话里注册。
- 对非 Waydroid 的普通 GTK/Qt 应用，GTK_IM_MODULE=fcitx/QT_IM_MODULE=fcitx 已足够。
- 对 Waydroid 访客应用：完全不生效（访客是另一个输入栈，Android 软键盘）。

---

## 8. Q4：其余遗留任务清单

| # | 现象 | 证据 | 判定 | 下一步 |
|---|---|---|---|---|
| 6-a | composer@2.1-service 启动 2 秒时的 Binder threadpool cannot be shrunk | 本次实测 logcat -d | grep -ci "Binder threadpool cannot be shrunk" → 0；logcat -d -b crash → 空；dev/health 报 crash Cmdline count: 0；HAL pid 81 稳定、init.svc.vendor.hwcomposer-2-1=running | 已修（NEXT-ROUND.md 的 0009 修法得到复核）。良性项描述可以保留但当前不触发 | 无需动作；下次重启时再确认一次 crash 缓冲仍为 0 |
| 6-b | virgl_test_server 实例累积（本次 13 个，会话内 15→12） | 见下方专项分析 | 不是泄漏，但也不是"按设计的一个客户端一个"那么干净：11 个已死客户端连接由宿主子进程持有 | 见下 |
| 6-c | 容器重启后 1 分钟内的 1 次 Xid | journalctl --since '2026-10-17 13:23' | grep -i xid → 0 条（sudo -n dmesg 也无）；本次 13:23 重启后至今无 Xid | 本次未复现。上一次记录在 NEXT-ROUND.md:30，仍属"未归因"但非当前故障 | 继续观察；若复现，用 VKR_TELEM_DUMP + WDRDIAG_ALLOC_TRACE 抓现场 |
| 20-门1 | ABenchMarkStart 永远 reportedDrawn=false | 本次实测（am start 后）：reportedDrawn=false reportedVisible=false、mNumDrawnWindows=0 allDrawn=false、窗口 mDrawState=DRAW_PENDING mLastHidden=true mEnterAnimationPending=true、mHasSurface=true isReadyForDisplay()=true、surface=[12,12][12,12]、Frames: parent=[40,20][1010,995]。pm enable 已恢复（组件当前 enabled） | 仍然存在，与 2026-10-17 记录一致。现象收敛为「窗口有 surface、isReadyForDisplay 为真，但 mDrawState 停在 DRAW_PENDING、mEnterAnimationPending=true、surface rect 仍是 12x12」——这更像窗口动画/首帧提交路径问题，而不是 reportDrawn 回调不触发 | 优先查 mEnterAnimationPending 为何不清：试 am start --no-window-animation；或 settings put global window_animation_scale 0 后重试 |
| 20-门2 | 跑分进度稳在 0%，3D 子进程到 vtest gralloc wrapper active 后静默退出 | 本次未复测（避免污染正在跑的会话） | 未复测。已确证的现场证据在 .workbuddy/memory/2026-10-17.md:217-250 | 见下 |

### 8.1 virgl_test_server 13 个实例：专项分析（Q4 的重点）

结论：不是进程泄漏，是 11 个已死客户端连接的 fd 没有被子进程关掉。

证据链：

1. STATE.md:51-58（2026-09-25）判定"SIGCHLD 是 SIG_IGN、每个子进程阻塞在 poll()、
   ESTAB socket 数匹配子进程数，所以是按设计"。这条在当时成立，但已被后续改动部分推翻：
   vtest/vtest_server.c:212-216, 432-447 现在是 do_fork 时装真正的 SIGCHLD handler
   （vtest_server_set_signal_child），不是 SIG_IGN。
2. 本机现状：13 个 virgl_test_server（1 个主 + 12 个子），11 个 virgl_render_server。
3. 每个子进程有 5 个 socket fd：
   - fd 1/2 = 996014（dup 的 stderr，连到主进程）
   - fd 3 = u_seq ESTAB，peer 是本子进程的 virgl_render_server
   - fd 4 = sock 0,9 UNIX-STREAM，这是 accept 得到的客户端连接
   - fd 5 = u_seq ESTAB，peer 是本子进程的 virgl-1-gpu_ren* 线程
4. fd 4 全部处于"无 peer"状态：这 11 个 inode 既不在宿主 /proc/net/unix
   （宿主表里 venus 相关只有 LISTEN 的 999634），在访客 netns 的
   /proc/net/unix 里却有 St=03（ESTAB）且 Path=/run/waydroid-venus/venus.sock；
   同时访客侧 83 个进程没有一个持有这些 inode 的 fd
   （ls -l /proc/[0-9]*/fd/* | grep socket:[<ino>] → 全不命中）。
   → 客户端（访客进程）已退出，socket 已断，但宿主子进程仍持有 accept 到的 fd。
5. 代码路径：vtest_server_run()（:756-834）里，客户端断开时
   vtest_client_dispatch_commands 返回 VTEST_CLIENT_DISCONNECTED，
   vtest_server_dispatch_clients()（:635-669）把 client 移到 inactive_clients；
   之后 vtest_server_tidy_clients()（:735-754）才会 close(client->in_fd) 并 free(client)。
   而 tidy 之后，if (!was_empty && is_empty) { vtest_cleanup_renderer(); if (!server.loop) run = false; }
   （:798-803）——子进程 server.loop 是 false（:681），所以最后一个客户端断开时子进程应该退出。

因此 11 个不死子进程意味着 vtest_server_tidy_clients() 没有被调到，或调到时
active_clients 非空。这是 --multi-clients 下的一个真实缺陷：
子进程 multi_clients=false（:682）、loop=false（:681），理论上 accept 新连接只发生在
主进程，子进程只服务自己被分到的那一个客户端；客户端消失后应该退出。
为什么没退出，需要读 vtest_client_dispatch_commands 与 fd 4 的 poll 行为才能定论——
这是本次研究未完成的部分（推测：fd 4 的断开没有被 select 唤醒，或 tidy 的条件判断有误）。

这对 Q2(B) 是真实的隐患：任何长期运行的解码代理都会累积同样形态的死连接。
建议在立项 (B)/(C) 之前，先在 --multi-clients 下复现并修掉这个
（hermetic 测试可以走 tests/vtest-supervisor.sh 的路子：伪造一个客户端连上再断开，
断言子进程退出）。

注：dev/health 与 NEXT-ROUND 里"15 个 virgl_test_server = 泄漏"的说法需要修正为
"11 个持有已断客户端 fd 的子进程"——进程数本身不是泄漏指标，fd 4 的 peer 状态才是。

---

## 9. 风险与未知（明确列出「我不知道」的部分）

1. Pascal NVDEC 类的方法流（§3.2/§6.3）：与 Turing 的差异未知；tinygrad autogen 只到 Turing+。
   这直接决定 (D) 的 3–8 人周估计。
2. nvidia-prolapse 模式在本机从未实跑：本机没有部署 vendor image（nvidia_prolapse_install
   缺失，/var/lib/waydroid/nvidia-prolapse 不是 symlink）。§2.3 表里它的 🧪 是依据门禁代码
   与 README 推断，不是本机实测。要实测需 sudo waydroid-nvidia-setup（会改生产配置，
   未获许可前不做）。
3. /sys/module/nvidia_drm/parameters/modeset 读为空的含义：是模块未加载，还是 sysfs
   权限。未验证。不影响 venus 实测结论。
4. fd 4 为何不触发子进程退出（§8.1）：只定位到"tidy 没被调到或条件不满足"，
   没有读到确切代码行。
5. waydroid.keyboard_layout 为空的原因（§7.1）：推测 keymap 事件时序或 layout 名解析，
   未验证。
6. libcuda.so 的 glibc 导入集规模（§6.2）：决定了 (E) 是"加 5 行"还是"加几百行"，
   未实测（可以用 llvm-readelf --dyn-syms 对 libcuda.so.580.178.04 跑一次 census 得到，
   本次未做，因为这会把 (E) 的优先级真正定下来）。
7. waydroid-atv 的 c2.ffmpeg 服务在 A13 上能否编译运行：codec2/Android.mk 依赖
   android.hardware.media.c2@1.2、libcodec2_aidl、libavservices_minijail 等，
   A13（LineageOS 20）的 media APEX 是否有这些符号未验证。
8. AnTuTu 门 2 是否由连接泄漏导致：本次没有按 prompt 要求"重启 wd-venus 单元再复测"，
   因为那会影响当前会话。这条仍是 open。

---

## 附：本次为研究做过的改动（均已恢复或明确标注）

| 改动 | 位置 | 状态 |
|---|---|---|
| dispositions.tsv +5/−13 行 | nvidia-prolapse/shim/dispositions.tsv | 保留在工作树（git status 显示 M），是 §4 的交付物；未提交 |
| core/pthread.c 去掉一个 NVG_EXPORT | nvidia-prolapse/shim/core/pthread.c | 保留在工作树（ M），同上 |
| gen/、out/ 重建产物 | nvidia-prolapse/shim/{gen,out}/ | 由 make 生成，.gitignore 已覆盖（shim/.gitignore = out gen out32 gen32） |
| /tmp/ogkm/（open-gpu-kernel-modules 解包） | /tmp | 临时，可删 |
| /tmp 下的 ATV 源码取样（fha.c、va_android.cpp、hwcodecs-vaapi.cpp、mcfx.xml、sfp.mk、hv.c、vd.c） | /tmp | 临时，可删 |
| 访客 am start ABenchMarkStart | 访客运行时 | 已自然结束；组件为 enabled（pm enable 早已恢复） |
| /var/lib/waydroid/waydroid.cfg、nvidia_driver_mode、任何 systemd 单元 | — | 未改动 |

git status --short 当前输出：

     M nvidia-prolapse/shim/core/pthread.c
     M nvidia-prolapse/shim/dispositions.tsv
    ?? PROMPT-codec-vaapi-research.md
