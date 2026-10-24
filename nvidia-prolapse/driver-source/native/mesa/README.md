# Full source NVK over the Rust RM backend

This directory preserves the build inputs and C adaptation layer for Mesa NVK
over the native Rust `/dev/nvidia*` backend. Mesa supplies the complete Vulkan
frontend, graphics command generation, image layouts, SPIR-V/NIR/NAK compiler,
and Android HAL entrypoints. The `nvkmd` adapter supplies allocation, GPU virtual
address binding, channels, execution and synchronization through `nv_rm.h`.

The source pin is **ddaf85e353442d9d7f192fdcf1823e652e3f2ee3** (tag mesa-26.2.5,
2026-10-14; moved from 0b97dcdd = mesa-25.2.0 on 2026-10-16). The isolated
`build/mesa-rm-source` worktree leaves the compiler-only worktree and original
Mesa checkout unchanged. This is separate from the earlier narrow experimental
Vulkan frontend: Rust builds use `--no-default-features` so it does not export
the prototype Vulkan entrypoints into the full NVK process.

## Rebuild

```sh
python nvidia-prolapse/driver-source/native/mesa/build.py --target both
```

`--target host`, `--target android` (x86_64), and `--target android32` (i686)
are available. Prepare the 32-bit platform library closure with
`python nvidia-prolapse/driver-source/native/mesa/prepare-runtime32.py` first;
this extracts unmodified files from the installed system image read-only into
`build/nvk-android32/runtime`. Android requires host
`mesa_clc` and `vtn_bindgen2`; the script builds the host first if these are
absent. `--download-deps` permits downloading missing host build tools. Ordinary
rebuilds are offline and use cached checksum-pinned Mesa Rust dependencies.

The script verifies the Mesa revision, applies `build-compat.patch` and
`rm-integration.patch` idempotently, copies canonical adapter files from this
directory, builds the correct Rust backend, regenerates Android dependency
metadata, and configures/builds Mesa and the independent artifact inspector.
It does not install, deploy, mount files, or start Waydroid.

Build dependencies reuse existing repository inputs:

- Rust 1.95.0 and its `x86_64-linux-android` / `i686-linux-android` targets.
- NDK r29, API 33; `build/hwcomposer-work/ndk/android-ndk-r29`.
- Meson 1.12.1, Ninja, GCC/C++, Python Mako, LLVM/Clang 23.1.1.
- The compiler-only build's locally built bindgen 0.72.1/cbindgen 0.29.2.
- Existing pinned AOSP platform headers under `build/hwcomposer-work/deps` and
  unmodified Android13 platform libraries extracted from the guest image.
- Source-built libdrm in `build/libdrm/build-android33-{x86_64,x86}`.
- Static libelf 0.8.13 for Android (`android_libelf.py`, upstream Mesa's Android CI recipe,
  md5 + sha256 pinned): Mesa >= 26 refuses to configure NVK without libelf (CUDA cubin
  parsing for VK_NVX_binary_import). Built once into `build/libelf-android/<triple>`.

Host-only LLVM helper dependencies are extracted into `build/nvk-host-deps`,
never installed system-wide or shipped into Android. `host-build-deps.json`
pins libclc 23.1.1-2 and SPIR-V LLVM Translator 23.1.2-1 package archives and their
SHA256 hashes, independently matched against the Arch sync database. Mesa CLC
and vtn_bindgen2 themselves build from the pinned Mesa source. LLVM only builds
NVK's internal OpenCL helpers; application SPIR-V uses source-built NAK at
runtime. The Android driver does not depend on LLVM, libclc, NVRTC, CUDA, or
proprietary NVIDIA userspace.

`build-compat.patch` adapts the host glibc C11 thread definitions (26.2.5 already
handles LLVM 22/23 in clc_helpers.cpp). All code generation remains in the pinned source.
`rm-integration.patch` and the canonical `nvkmd_rm.*` implement backend
selection and report only capabilities actually provided by the adapter.
On 26.2 it also moves nouveau's shared-memory derivation into
`nv_device_info_init_shared_mem_sizes` (nv_device_info.h) so the RM backends fill
`sm_smem_sizes_kB`/`max_smem_per_wg_kB` exactly as the nouveau winsys; keeps
`copy_sync_payloads` NULL unless the sync type is a DRM syncobj
(`vk_drm_syncobj_copy_payloads` cannot handle RM syncs); and keeps the
stock-driver compression policy (`kmd_info.has_rm_compression`, compressible kind on
the image VA) next to upstream's dedicated-allocation compression
(`has_compression`, off for RM).
`nvkmd_rm.c` allocates device-local memory in whole 64 KiB big pages (size and
alignment), as the stock driver does and as NVK itself did up to 25.2: Mesa 26's
nvk_AllocateMemory passes only bind_align_B (upstream 7a492e102ff2), which shrank
exported dma-bufs to 4 KiB granularity (failed the HAL ahb-yuv gate) and silently
took the compressible placement away from non-64 KiB-multiple allocations.
It also selects Mesa's THREADED_ON_DEMAND submit mode for native timelines and
returns submissions to the caller whenever the drained submit thread is not
needed, so the backend never receives a wait whose signal is unsubmitted
(SYNCHRONIZATION.md "Wait-before-signal and ring pressure"; reproducer
`tools/native-vulkan-ring-wbs.c`).
`pascal-a-clear.patch` (backend-independent NVK behaviour) brackets every
`vkCmdClearAttachments`/render-pass/image clear on PASCAL_A (GP100/GP10B) with
the undocumented 3D methods `0xF60 := 1` … `0xF80 := 1, 0xF60 := 0`, as the NVIDIA
580 driver does on that class only; `tests/pascal-a-clear.c` checks the words and
the class selection (Maxwell, PASCAL_B, Volta+ get nothing).
Multi-planar YCbCr (NV12 = G8_B8R8_2PLANE_420, P010 = G10X6_B10X6R10X6_2PLANE_420_3PACK16)
with DRM_FORMAT_MOD_LINEAR and explicit per-plane pitch/offset is upstream in 26.2
(!40473: 98185bfb, 29ffb845, multi-plane tiled shadows 1564995b/72e4b256). On top:
`nvk-upstream-ecfea8d9-ycbcr-modifier.patch` (a modifier list on a YCbCr image picks
LINEAR without asking nil) and `nvk-upstream-bfb90f05-memory-plane-aspects.patch`
(MR !31867: `VK_IMAGE_ASPECT_MEMORY_PLANE_i_BIT_EXT` selects plane i in
`vkGetImageSubresourceLayout`, `VkImagePlaneMemoryRequirementsInfo`,
`VkBindImagePlaneMemoryInfo`), both upstream-main commits not in 26.2.x, cherry-picked
unchanged; `nvk-explicit-plane-layouts.patch` turns unbindable client plane layouts
into loud vkCreateImage errors and makes a DISJOINT plane's explicit offset appear in
its subresource layout and memory requirement, consistent with the bind. LINEAR
imports need pitch and plane offset multiples of 32 B (256 B on Kepler, NIL
`new_linear`); the gralloc's NV12/P010 layout satisfies both NVK and stock. Since
several patches touch the same files, `build.py` unwinds the applied stack
newest-first and re-applies it in order. Tests: HAL `test/ahb-yuv.c`,
`tests/nvk-memory-plane-layout.c` (real GPU, NVK selected by driverID): NV12 and P010,
1920x1080 and 1280x720, packed and DISJOINT, every memory plane's offset and pitch equal
the explicit layout given at creation, MEMORY_PLANE_i and PLANE_i requirements agree,
binds through MEMORY_PLANE_i succeed.
`nvk-shader-upload-publish.patch` + `nvk-shader-prefetch-in-range.patch`: shader programs are
published the way the proprietary driver (libnvidia-eglcore 615.78.08) does. The shader heap is
never CPU-mapped, also with a full BAR: every upload is a copy-engine copy from system-memory
staging (`nvk_upload_queue`, LAUNCH_DMA NON_PIPELINED | FLUSH_ENABLE). `nvk_heap` counts uploads;
in the first submission on a queue after the count moved, the queue acquires the upload queue's
completion and then emits INVALIDATE_SHADER_CACHES_NO_WFI(INSTRUCTION) (3D class form, compute
form on compute-only queues) ahead of the command buffers, with no wait-for-idle. Before, heap
ranges of destroyed shaders were rewritten through a write-combined BAR1 mapping and reused at
once with no instruction-cache invalidate, so a draw could run a header or code cached for the
range's previous user (the rare Xid 13 "Shader Program Header 11/18 Error" under ANGLE decodes as
a vertex header read at a fragment program address; no deterministic reproducer exists). On
Ampere B and later programs start at 256 B and occupy whole 256 B blocks, so
SET_PIPELINE_PROGRAM_PREFETCH never covers the neighbouring program. Tests:
`tests/vk-shader-reuse.c` (recycled ranges, every pixel checked) and `native/tools/nvcap_publish.py`
on a push capture (every program address used only after its copy, the acquire and the
invalidate; prefetch inside the copied range).
`nvk-prevolta-modifier-memory-types.patch` (with the RM backend's `allocate_block_linear`):
memory for a block-linear DRM-modifier image carries the image's kind on the physical allocation
(NVOS32 FORMAT BLOCK_LINEAR, generic kind, no compression), as NVK does on nouveau
(`nvk_device_memory.c` → `nouveau_bo.c` `tile_flags`) and as the proprietary driver requests it.
Another process that imports the dma-buf maps it with the memory's own kind. Before Turing that
kind decides how the GPU reads the pages, so a pitch-kind allocation sampled as block-linear
faults (Xid 13 TEX LAYOUT in KWin on a GTX 1060/1080, closed 580). Before Turing, BAR1 also
doesn't return the modifier's byte layout to the CPU, so these images get no host-visible VRAM
memory type (the proprietary 580 driver offers none either: memoryTypeBits 0x82 on GP104);
system-memory types stay. Turing and later are unchanged (generic kind 0x06; BAR1 reads back
exactly on TU106 and GB202).
`nvk-3d-view-of-2d-image.patch`: a `VK_IMAGE_VIEW_TYPE_3D` view of a `VK_IMAGE_TYPE_2D` image is
invalid usage that NVIDIA's driver accepts (615.78.08 returns `VK_SUCCESS`); 3DMark Wild Life
Extreme creates one on its second scene load in one process. nil's `normalize_extent`
(`descriptor.rs:261`) asserts `image.dim == ImageDim::_3D`, and the panic aborts the process because
it cannot leave the `extern "C"` `nil_image_view_descriptor`. NVK now hands nil the selected layer as
a 3D image of depth 1 (same memory layout: one slice per level, `z_log2 = 0`), the mirror of its own
`image_3d_view_as_2d_array()`, and logs the invalid usage once. Only a single-sampled single layer
qualifies; any other 3D view of a non-3D image (multisampled, several layers, 1D) fails
`vkCreateImageView` with a logged reason and `VK_ERROR_UNKNOWN` (as `nvk_image.c` reports invalid image
parameters) instead of reaching nil's assert, which release builds keep. Upstream Mesa main (2026-10-23)
still asserts. Reproducer: `tests/nvk-3d-view-of-2d-image.c` (aborts before; after: the 3DMark view is
created and a 3D view of a 4x multisampled image returns an error; the stock ICDs accept both).
`nvk-angle-nan-preserve-min-max.patch`: driconf `nvk_force_nan_preserve_min_max` (RADV's
`radv_force_nan_preserve_min_max` mechanism -> spirv `workarounds.force_nan_preserve_min_max`),
set for engine `^ANGLE$` in `00-nvk-defaults.conf` and folded into the compiler flags (bit 15).
Since Mesa 26 `nir_opt_algebraic` folds `max(x, 0)` to `x` when `x` is provably non-negative
except for NaN and the FMax is not NaN-preserving, which GLSL.std.450 FMax from ANGLE never is;
Mesa's GLSL frontend keeps float min/max NaN-preserving and NVIDIA FMNMX/stock return the
non-NaN operand. GTA SA's vehicle specular `max(pow(x<0, 10.0), 0.0)` rendered car paint black
under ANGLE on 26.2 (`tests/vk-nan-minmax.c`, live A/B 2026-10-16).

Mesa util patches for the shader disk cache, applied to the NVK and the GL tree (upstream-style messages in each file):
`mesa-disk-cache-create-with-path.patch` (`disk_cache_create_with_path()`: a cache at an absolute,
per-application directory, enabled on every platform, driver_flags kept),
`mesa-disk-cache-keep-file-backend.patch` (`disk_cache_set_callbacks` leaves a cache with a working
file backend alone, so Android libEGL's blob-cache callbacks do not take it over) and
`mesa-disk-cache-evict-invalid-item.patch` (a multi-file item that fails validation is removed on load;
before, it blocked every later write of its key and the key missed forever).
`nvk-android-app-disk-cache.patch`: on Android NVK creates its disk cache with
`disk_cache_create_with_path()` at the application's `code_cache/nvk_shader_cache` (the sibling of
the TMPDIR that Android's framework sets in every application process, as for nvrm). Mesa's default
location is off on Android and Vulkan has no blob cache, so before this every process recompiled
every pipeline, ANGLE GLES applications on each launch. Processes without an application code cache
keep the in-memory cache only. The Android NVK build uses `-Dshader-cache=enabled -Dzlib=enabled` with
the NDK zlib. Measured 2026-10-17 (Minecraft 1.26.60.24, first hotbar switch, ANGLE on NVK, RTX 5090,
longest present interval): warm relaunch 1504 -> 130 ms; cold unchanged; cache 9.1 MB.

Dropped on 26.2: `p100-bisect.patch` (opt-in GP100 Xid 13 diagnostic; the bisection
ruled its suspects out and the cause was R19, RM depth kinds),
`mesa-gles3-immutable-levels.patch` (upstream 75a0cd6e2cdc),
`mesa-upstream-898fa317-trace-func.patch` (898fa317 is in 26.2).

## Artifacts and inspection

```
build/nvk-host/src/nouveau/vulkan/libvulkan_nouveau.so
build/nvk-android/src/nouveau/vulkan/libvulkan_nouveau.so
build/nvk-android32/src/nouveau/vulkan/libvulkan_nouveau.so
build/nv-rm-rust/debug/libnv_bionic_native.so
build/nv-rm-rust/x86_64-linux-android/debug/libnv_bionic_native.so
build/nv-rm-rust/i686-linux-android/debug/libnv_bionic_native.so
build/nvk-{host,android}/inspect
```

Both full NVK artifacts have a real `DT_NEEDED` dependency on the Rust backend.
These are development build artifacts with local absolute RUNPATHs. Packaging
must supply the Rust backend and source libdrm in the intended Android namespace
and remove development RUNPATHs; a successful local build is not deployment.

`inspect.c` loads the ICD directly through `vk_icdGetInstanceProcAddr`, creates
and destroys a Vulkan instance without requesting a GPU, and checks the Android
HAL `HMI` export. Actual host and bionic executions passed; Android exports
`HMI`, host does not. The reported API is Vulkan 1.4.318. This is an artifact
check and does not itself establish GPU correctness or conformance.

The Android inspector can run without starting the container using the
extracted bionic runtime. Its dependencies include `libdl_android.so` through
Android's `libvndksupport`; this was extracted read-only from the runtime APEX
into `build/nvk-android/runtime`, alongside a symlink for the source-built
`libdrm.so.2`. Required `LD_LIBRARY_PATH` directories are:

```
$repo/nvidia-prolapse/driver-source/native/runtime/lib64
$repo/build/nvk-android/runtime
$repo/build/hwcomposer-work/deps/image-rootfs/system/lib64
```

Use the absolute runtime/linker64 path, then the absolute inspector executable
path and absolute ICD path. The missing `/linkerconfig/ld.config.txt` warning
outside Android is retained in results. No guest file is modified by this test.

## Backend source interfaces

At the pin, `src/nouveau/vulkan/nvkmd/nvkmd.h` defines the narrow abstraction:

| Interface | Responsibilities |
|---|---|
| `nvkmd_pdev_ops`, line 123 | physical-device lifetime, VRAM use, DRM fd, device creation |
| `nvkmd_dev_ops`, line 160 | memory/VA allocation, imports, context creation, GPU timestamp |
| `nvkmd_mem_ops`, line 210 | CPU mapping, freeing, dma-buf export |
| `nvkmd_va_ops`, line 265 | exact VA binding/unbinding and release |
| `nvkmd_ctx_ops`, line 322 | wait, execute push arrays, bind, signal, flush, completion |

The original selector is `nvkmd/nvkmd.c:89`; original Nouveau backend sources
remain intact for comparison. `nvk_queue.c:250` submits address/size push arrays;
these are already GPU command streams, not Vulkan commands for Rust to translate.

The upstream Android frontend deserves separate runtime validation:
`nvk_android.c` uses CPU waits for incoming native fences; its release hook
waits the queue idle and returns `-1`, and does not independently consume the
passed wait-semaphore array. The deployment therefore uses our separately source-built `vulkan.nvidia.so`
HAL for ANB/AHB/sync semantics. Its actual bionic64/32 ANB tests and real
Android ANGLE window/AHB pixel gates passed on 2026-10-12; the upstream NVK
Android hooks are not the deployed synchronization implementation.

Source files retain
their upstream MIT/copyright notices. This document does not claim the Waydroid
replacement gate has passed; that requires actual rendering, external buffers,
synchronization, guest loading and visible Android frames.

## Existing Android HAL over source NVK

`python nvidia-prolapse/driver-source/native/mesa/build-hal.py` builds the public
HAL into `build/source-native-integration/{host,android,android32}/vulkan.nvidia.so`.
It uses the existing source-generated dispatch/ASTC assets, and the HAL's NDK
r30 toolchain at `/opt/android-ndk` (the older r29 Vulkan headers lack required
1.4 types). Existing `hal/out` driver artifacts are preserved.

`NVHAL_SOURCE_RM` explicitly selects `libvulkan_nouveau.so` and skips the
proprietary NVIDIA D1 first-fence workaround. Mesa's syncobj backend may validly
return fd -1 for an already-signalled exported sync_file. The HAL's existing
ANB/AHB and ASTC implementations remain in use: this pinned NVK does not provide
the Android external-memory hardware-buffer extension itself.

## Measured graphics and descriptor lifetime

Independent public `tools/native-vulkan-offscreen.c` rendered all 2,109 pixels
and guards correctly on host and bionic x86_64 after the Rust GART mapping was
made GPU-uncached. The bionic compute probe likewise checked all 4,422 words.
The first bionic teardown check observed fd4→5. A syscall trace identified the
remaining fd as liblog's process-global AF_UNIX socket targeting
`/dev/socket/logdw`; all RM and DRM descriptors closed. AOSP
`system/logging/liblog/logger_write.cpp:105` provides `__android_log_close`,
and that symbol exists in the actual platform library. Updated independent
probes call it after Vulkan teardown and before unloading the ICD, then retain
the strict descriptor equality check. The corrected probes passed on both Android x86_64 and i686: compute 4,422
words, graphics 2,109 pixels, guards intact, fd4→4 in each run.

All three NVK builds and all three HAL builds compile successfully. Android32
also passed actual bionic library loading and Vulkan instance creation, reporting
API 1.4.318 and the Android HMI export. All 30 independent public-RM-header C wire-layout checks match Rust on
Android32, and its compute/graphics GPU gates passed as described below.
On this host, the 32-bit bionic pthread implementation also requires a low PID;
run it in a PID namespace once authorized rather than mistaking that platform
limit for a GPU failure. `abi-check.c` is the independent C oracle for the
adapter structs; the Rust backend's separate public-RM-header oracle covers
the kernel ioctl wire layouts.

The adapter oracle measured the following on actual host/bionic processes:

| Struct | x86_64 size/alignment | Android i686 size/alignment |
|---|---|---|
| NvRmInfo | 152 / 8 | 152 / 4 |
| NvRmAllocation | 24 / 8 | 20 / 4 |
| NvRmExec | 16 / 8 | 16 / 4 |

Member offsets agree between ABIs: `NvRmInfo.vram_size=72`, `bar_size=80`,
`name=88`; `NvRmAllocation.cpu_address=16`; `NvRmExec.size_B=8`, `flags=12`.
The allocation struct's pointer field and trailing alignment explain its size
difference. Logs are `build/nvk-{host,android,android32}/adapter-abi.txt`.


The public ANB test has an explicit `--allow-synchronous-submit` mode for
implementations whose submit or sync-file export waits for GPU completion.
It still requires correct pixels, fd ownership, sync_file signalling and a
GPU-written completion marker after both release/acquire waits. Its default
mode retains the earlier pending-at-first-poll requirement. `--bounded` checks
two rounds of four fd ownership cycles instead of the default longer loops.
Host ANB passed this bounded mode; host AHB and ASTC gates also passed. Logs
are `hal-{anb,ahb,astc}-host.log` in this directory.
Actual bionic ANB and ASTC tests subsequently passed for both x86_64 and i686;
their logs are `hal-{anb,astc}-{android,android32}.log`.

`build-hal-tests.py` cross-builds the public ANB/ASTC tests for both Android
ABIs in the isolated integration output directories. The AHB host test uses
fake gralloc handles through a host-only test interface; Android intentionally
uses the real platform AHardwareBuffer implementation, so its final AHB gate
must run in the actual guest.

`build-gralloc.py --target {host,android,android32,all}` reproduces the existing
gralloc wrapper with explicit source NVK selection and source-RM layout rules.
It preserves NODELETE and builds separately from the stock `gralloc/out` files.

The integration patch also fixes `nvk_cmd_buffer.c`'s shader-cache invalidation
predicate: the shader-data and constant bits are `1<<3` and `1<<5`, so their
bitwise AND was always zero. OR allows either requested invalidation to emit
`INVALIDATE_SHADER_CACHES_NO_WFI`, preserving each independent payload bit.
All three drivers were rebuilt, and final bionic offscreen (2,109 pixels,
guards, fd4→4) plus ASTC reference regressions passed. Evidence is
`post-cache-{graphics,astc}.log`.

`verify-artifacts.py` records exact final NVK and patch hashes, DT_NEEDED data,
and actual host/bionic instance lifecycle checks in `build-verification.json`.
This read-only check runs Android32 inside a PID namespace and does not
enumerate GPUs. Neither these checks nor the standalone HAL fixtures establish
the final Waydroid boot/presentation gate.
