# Offline Mesa transport regression

Run from the repository root after configuring the pinned host Mesa build:

```sh
python3 nvidia-prolapse/driver-source/native/mesa/tests/run-pdev-preflight.py
```

The harness compiles the canonical adapter and selected actual pinned Mesa
runtime sources. Fake RM/DRM calls check arguments; unexpected device access
and DRM syncobj create/wait/destroy operations abort. It requires no GPU and
does not establish runtime support on any driver.

Coverage includes selected NVIDIA node checks, partial initialization cleanup,
absent legacy syncobj capabilities, wait deadlines/ANY/ALL/pending, a simulated
6.251-second valid dependency, maintenance and submit-thread loss, ring retries,
sync reset/move, FD forwarding, and destruction without infinite pre-drain.
`submit-on-demand.inc` drives the patched `vk_queue.c` in THREADED_ON_DEMAND
mode with a minimal timeline sync: `driver_submit` asserts that every wait it
receives is already pending (fails under the former IMMEDIATE mode), a
same-thread wait-before-signal and the work behind it wait in the submit
thread until the host signal, order is kept, and a drained queue submits on
the caller again, and a binary signal of a deferred submission is reserved so
a binary wait submitted behind it does not block vkQueueSubmit.
VA teardown hands every binding to one `nv_rm_va_free` (no synchronous unbind):
a deferred release keeps the range reserved until the backend's callback runs,
a failed call publishes loss and retains the range, and an application rebind
over a live binding unbinds immediately.

Void sync destruction uses the historical `nv_rm_sync_destroy_deferred` ABI
(immediate caller-side disposal since the caller-side backend); the synchronous
entrypoint is a harness tripwire. Cleanup after loss is checked, and an injected
disposal failure must log a diagnostic, publish Mesa/Rust loss and wake the
eventfd through the installed nonblocking loss hook. Rust CPU and hardware tests
separately exercise slot lifetime across retire, detach-on-wait, reset, move
and import.

`submit-fusion.inc` exercises the actual adapter's combined push/wait/signal
call. Exact mock expectations cover empty, signal-only, wait-only and
0/1/1000/1001/2000-entry submissions, continuation boundaries, first/final
ring-pressure retries, failed waits, rejected batches, and an accepted prefix
followed by failure. Signals appear only on the final chunk and waits only on
the first accepted chunk. Invalid signal types, allocation failure, and an
oversized continuation chain reject before any submission. Bind-only contexts
continue to signal through the CPU path. This checks C-to-Rust call contracts,
not GPU execution or Rust ownership internals.

Loss notification checks invoke the actual Mesa device and queue setters through
the adapter's installed callback. They require loss publication before the wake,
exercise the callback under a submit mutex, and verify hook removal before the
backend is freed. Scripted waits require one blocking backend call, reject status
polling, preserve caller deadlines, and give loss precedence over racing success.
The threaded loss tests use real eventfd wakeups; no GPU or device nodes are used.

`sync-properties.inc` invokes the actual public Mesa fence/semaphore property
queries. It asserts these masks, rather than deriving them from a second model:

| Object | Requested handle | Compatible / export-from-imported | Features |
| --- | --- | --- | --- |
| Fence | OPAQUE_FD | OPAQUE_FD only | Import + export |
| Fence | SYNC_FD | SYNC_FD only | Import + export |
| Binary semaphore | OPAQUE_FD | OPAQUE_FD only | Import + export |
| Binary semaphore | SYNC_FD | SYNC_FD only | Import + export |
| Timeline semaphore | OPAQUE_FD | OPAQUE_FD only | Import + export |
| Timeline semaphore | SYNC_FD | None | None |

`sync-timeline-mode.c` compiles the pinned `vk_device.c` and exposes its static
selector only to this test. It verifies that the two adapter types provide
exactly one timeline implementation and select native timeline mode. Opaque
sharing and sync-file copy transfer use separate callback sets while retaining
the same RM completion operations. Cross-handle conversion is not advertised.

The runner stores its executable and objects under
`build/gpu-coverage/pdev-preflight/`. Actual GPU probe results are separate.

`pascal-a-clear.c` (build line in its header) compiles the patched
`nvk_pascal_a.h` from `build/mesa-rm-source` and checks the PASCAL_A clear
bracket words `0x800103d8` / `0x800103e0, 0x800003d8` and that only class
0xc097 gets them.

## gl-g0a-clear.c (nvrm gallium driver, gate G0a)

Build the driver (`build.py --target host --gl`, which needs an existing NVK
host build for mesa_clc), then:

    B=build/nvrm-host
    cc -std=c11 -Wall -Wextra -Werror -Ibuild/mesa-gl-source/include \
       nvidia-prolapse/driver-source/native/mesa/tests/gl-g0a-clear.c \
       -L$B/src/egl -lEGL -L$B/src/mesa/glapi/es2api -lGLESv2 \
       -Wl,-rpath,$PWD/$B/src/egl:$PWD/$B/src/mesa/glapi/es2api -o gl-g0a-clear
    LIBGL_DRIVERS_PATH=$PWD/$B/src/gallium/targets/dri ./gl-g0a-clear

The loader maps the nvidia-drm render node to nvrm by itself; no driver
override is needed. PASS = GL_VENDOR names nvrm and two glClear colours read
back exactly on every pixel.

## gl-g0b-triangle.c (nvrm gallium driver, gate G0b)

Same build line as gl-g0a-clear.c. The program clears a 64x64 RGBA8 FBO,
draws a triangle (glDrawArrays, VBO, per-vertex colour) and a quad
(glDrawElements, client-side GL_UNSIGNED_SHORT indices) with uniforms in
both stages, and writes the readback to a file; with a third argument it
compares against a reference image (mismatch count, coverage differences,
max channel difference). The reference comes from the same source linked
against the host system Mesa (glvnd) on llvmpipe — the --gl build has no
llvmpipe (LLVM disabled):

    cc -std=c11 gl-g0b-triangle.c -lEGL -lGLESv2 -o gl-g0b-triangle-sys
    __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
    LIBGL_ALWAYS_SOFTWARE=1 ./gl-g0b-triangle-sys --flat llvmpipe ref-flat.rgba
    LIBGL_DRIVERS_PATH=$PWD/$B/src/gallium/targets/dri \
    ./gl-g0b-triangle --flat nvrm out-flat.rgba ref-flat.rgba

`--flat` gives every vertex of a primitive the same colour: PASS = 0
mismatching pixels. Without it (colour gradients) the result depends on
varying-interpolation precision, which GL leaves to the implementation:
llvmpipe, NVIDIA's own GLES (10_nvidia.json vendor) and nvrm all differ
pairwise by at most 1 LSB on 80-160 pixels with identical coverage. The
gradient mode is a coverage + tolerance check, not an exactness check.
NVRM_DEBUG_SHADERS=1 prints st/mesa's NIR and NAK's assembly.

## gl-lod-switchover.c (min/mag switch-over point c)

Same build line as gl-g0a-clear.c; the system build (`-lEGL -lGLESv2`) runs
on llvmpipe (50_mesa.json) or NVIDIA's own GLES (10_nvidia.json). Vertex
shader texture2DLod/textureCubeLod at a black/white texel boundary, one point
per LOD: NEAREST prints 0 or 255, LINEAR ~127. Measured 10-15 on the 5090:
nvrm and stock NVIDIA print identical values, c = 0 for every filter pair
(NEAREST_MIPMAP_*/LINEAR at LOD 0.1-0.49 samples NEAREST level 0).
dEQP-GLES2's reference uses c = 0.5 for MAG LINEAR + MIN NEAREST_MIPMAP_*
only on a context that is not ES3-compatible (tcuTexture.cpp
sampleLevelArray2DOffset `es2`, non-seamless cube path), so the six
`texture.vertex.cube.filtering.nearest_mipmap_*_linear_*` cases fail on an
ES 2.0-only nvrm context and pass once nvrm exposes ES 3.0 (llvmpipe with
MESA_GLES_VERSION_OVERRIDE=2.0 fails the same six).

## dEQP-GLES2 on nvrm (gate G1)

dEQP comes from the pinned VK-GL-CTS clone in `build/deqp/VK-GL-CTS`
(vulkan-cts-1.4.6.2, f6a29701), built for the host with the EGL
surfaceless platform:

    cmake -S build/deqp/VK-GL-CTS -B build/deqp/build-host-gles2 -G Ninja \
          -DDEQP_TARGET=surfaceless -DCMAKE_BUILD_TYPE=Release
    ninja -C build/deqp/build-host-gles2 deqp-gles2

`deqp-gles2-run.py` runs a case list in batches and survives crashes (a
crash or watchdog timeout marks the running case and resumes after it);
it writes `results.json` (counts, per-case status, first error line):

    D=build/deqp/build-host-gles2/modules/gles2
    DEQP_CONFIG=rgba8888d24s8ms0 PROLAPSE_ROOT=$PWD \
      python3 nvidia-prolapse/driver-source/native/mesa/tests/deqp-gles2-run.py \
      $D/deqp-gles2 cases.txt out-dir nvrm

Modes: `nvrm` (our build), `llvmpipe` (host Mesa, glvnd 50_mesa.json).
NVIDIA's own EGL cannot be the comparator with this target: its
eglGetDisplay refuses the surfaceless platform. Case lists come from
`deqp-gles2 --deqp-runmode=txt-caselist` run with the nvrm environment
(pbuffer surfaces 256x256).

### dEQP-GLES3 (G1 GLES 3.0)

Same clone and build directory; the runner takes any dEQP binary:

    ninja -C build/deqp/build-host-gles2 deqp-gles3
    D=build/deqp/build-host-gles2/modules/gles3
    (cd $D && ./deqp-gles3 --deqp-runmode=txt-caselist ...)   # nvrm env, as above
    DEQP_CONFIG=rgba8888d24s8ms0 PROLAPSE_ROOT=$PWD \
      python3 nvidia-prolapse/driver-source/native/mesa/tests/deqp-gles2-run.py \
      $D/deqp-gles3 cases.txt out-dir nvrm

Keep case lists in dEQP's own order (grep the generated list, never sort
it): dEQP runs a case list in tree order and the runner's resume logic
assumes file order = execution order. `NVRM_GL_DIR_SUFFIX=-x` makes both
`build.py --gl` and the runner use build/mesa-gl-source-x / nvrm-host-x,
so parallel branches do not share a build tree.

## gl-g0b-triangle.c on Android bionic (G2 groundwork, host GPU)

`--es3` asks for an ES 3.0 context and fails unless GL_VERSION is "OpenGL ES 3.x". Built
with the NDK (`<triple>33-clang … -lEGL_nvrm -lGLESv2_nvrm`) against
`build/nvrm-android{,32}` (`build.py --gl --target android|android32`) and run through the
bionic linker; exact commands in ../ANDROID-GL.md. 2026-10-15 (5090): x86_64 and i686
`--es3 --flat` 0/4096 vs llvmpipe (hash 61e30af4 = G0), gradient ≤1 LSB on 160 px (same
bytes on both ABIs, the known interpolation difference), GL_VERSION "OpenGL ES 3.0 Mesa
25.2.0", 0 NVRM kernel lines.

GLES3 results at gl-fix3 7163c79 (5090, 4 contiguous shards of the tree-order
list + 2 GLES2 shards): dEQP-GLES3 functional 45,268 P / 14 F / 183 NS /
0 crash (was 45,160 / 104 / 197 at 2adff98; 108 Fail/NS -> Pass, 0
regressions), dEQP-GLES2 functional per-case identical. Results in
build/deqp/results-gl-gles3/gles{3,2}-functional-7163c79*.json.
Open: 6 preprocessor.conditional_inclusion (fail on llvmpipe too, GLSL
front end) and 8 texture_functions.texture{,proj}offset.*sampler3d_fragment:
only implicit-LOD + AOFFI + 3D fails; the bias, explicit-LOD, grad and
vertex (LOD 0) variants of the same 3D offsets pass, and forcing NAK's
TexDerivMode::Auto instead of DerivXY (SM120) does not change it. Next:
run the 8 cases on a pre-Blackwell GPU to split NAK/SM120 from nvrm.

## gl-skia-shaders.c (SurfaceFlinger RenderEngine shader features)

Skia-style GLSL ES programs as AOSP 13 RenderEngine emits them (sk_RTAdjust, sk_FragCoord flip,
samplerExternalOES in ESSL1/ESSL3 incl. textureSize/texelFetch on an EGLImage of a GL texture,
integer dither, derivatives + inversesqrt + discard, mip bias -0.475, LinearEffect sRGB/PQ +
tone map, blur loop, mediump rrect), each drawn into a 64x64 RGBA8 FBO, compared per channel
(<= 3 LSB) with llvmpipe; `external_size_lod` is compared with its analytic result because
llvmpipe (26.2.4) returns textureSize (64, 0) and a zero texel for external samplers.

    B=build/nvrm-host-sf
    cc -std=c11 tests/gl-skia-shaders.c -lEGL -lGLESv2 -lm -o skia-sys
    __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json LIBGL_ALWAYS_SOFTWARE=1 \
      ./skia-sys llvmpipe ref
    cc -std=c11 tests/gl-skia-shaders.c -L$B/src/egl -L$B/src/mesa/glapi/es2api -lEGL -lGLESv2 -lm \
       -Wl,-rpath,$B/src/egl:$B/src/mesa/glapi/es2api -o skia-nvrm
    LIBGL_DRIVERS_PATH=$B/src/gallium/targets/dri ./skia-nvrm nvrm out ref [case-substring]

10-15 before the fix: the three external cases aborted with NAK's "Unsupported texture
dimension: 6" (same abort as SurfaceFlinger's primeCache in the guest); all other cases passed.
After: 11/11 PASS. Also part of gl-android-smoke.py (bionic, both ABIs).

## gl-dmabuf.c (G2: dma-buf + native fences)

    B=build/nvrm-host-dmabuf
    cc -std=c11 -Wall -Wextra -Ibuild/mesa-gl-source-dmabuf/include -I/usr/include/libdrm \
       nvidia-prolapse/driver-source/native/mesa/tests/gl-dmabuf.c -L$B/src/egl -L$B/src/mesa/glapi/es2api \
       -lEGL -lGLESv2 -lvulkan -Wl,-rpath,$B/src/egl:$B/src/mesa/glapi/es2api -o gl-dmabuf
    LIBGL_DRIVERS_PATH=$B/src/gallium/targets/dri \
    VK_DRIVER_FILES=$B/src/nouveau/vulkan/nouveau_devenv_icd.x86_64.json VK_LOADER_LAYERS_DISABLE='~implicit~' \
      ./gl-dmabuf nvrm --vk

Checks (each prints ok/FAIL, exit 0 = all ok): EGL extensions; modifier lists; GL texture
export -> GL import read/sample/write-back exact; native fence fd signals + re-import +
eglWaitSyncKHR; GL export -> NVK VkImage import exact; NVK image (block-linear, then LINEAR)
-> EGLImage after a 400-clear Vulkan chain, SYNC_FD imported and eglWaitSyncKHR'd while still
pending, pixels = final Vulkan colour; minigbm window formats rendered by GL and read by NVK.
Without `--vk` (and with -DNVRM_NO_VULKAN) it is the bionic capability gate of gl-android-smoke.py.

## gl-yuv.c (NV12 / P010 EGLImage import, samplerExternalOES)

    B=build/nvrm-host-yuv
    cc -std=c11 -Wall -Wextra -Ibuild/mesa-gl-source-yuv/include -Ibuild/mesa-gl-source-yuv/include/drm-uapi \
       nvidia-prolapse/driver-source/native/mesa/tests/gl-yuv.c -L$B/src/egl -L$B/src/mesa/glapi/es2api \
       -lEGL -lGLESv2 -lm -ldl -Wl,-rpath,$B/src/egl:$B/src/mesa/glapi/es2api -o gl-yuv
    LIBGL_DRIVERS_PATH=$B/src/gallium/targets/dri NVRM_YUV_VK_ICD=$PWD/$B/src/nouveau/vulkan/libvulkan_nouveau.so \
      ./gl-yuv nvrm

NV12/P010 buffers in our minigbm 0004 layout from NVK (the ICD is dlopen'ed, no loader, so the
same source runs on bionic): raw exportable memory (gralloc's carrier) and a VK multi-planar LINEAR
image with explicit plane layouts; 854x480, 1920x1080 with 1088 rows, 640x362; BT601/709 (+2020 for
P010) x limited/full, NEAREST, plus one GL_LINEAR run per buffer and one with a separate plane-1 fd;
compared with a CPU H.273 reference (<= 2 LSB; P010 against Mesa's UNORM16 definition, the strict
10-bit difference is printed). Refusals: block-linear NV12, no modifier, YUV420, CbCr offset off
32 B, CbCr past the dma-buf, pitch 880. 10-15 on the 5090: GL YUV PASS (85 checks), max 1 LSB
(strict 10-bit H.273 for P010 limited: 3 LSB). Part of gl-android-smoke.py on both ABIs.

## gl-es31-mc.c (G3 app gate: Minecraft Bedrock's GLES 3.1 feature set)

Minecraft Bedrock (RenderDragon/bgfx, 26.60 beta) ships only `#version 310 es` shaders:
14,290 of them in assets/renderer/materials; fragment shaders use up to 5 std430 SSBO
blocks, textureGather, samplerCubeArray (`GL_EXT_texture_cube_map_array : require`) and
centroid varyings; 44 compute shaders use image load/store, SSBOs, atomicAdd. On nvrm
(ES 3.0) bgfx's init gets GL errors on glBindBuffer(GL_SHADER_STORAGE_BUFFER) and
glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY), MC never compiles a material and presents
bgfx clears only (black screen, no crash). This test checks the same features with its
own shaders: ES 3.1 context, limits, a fragment draw (5 SSBOs + gather + cube array +
centroid) read back from RGBA32F, a compute dispatch (imageStore + SSBO + atomicAdd).

    B=build/nvrm-host-sf; T=nvidia-prolapse/driver-source/native/mesa/tests/gl-es31-mc.c
    cc -std=c11 -Ibuild/mesa-gl-source/include $T -L$B/src/egl -L$B/src/mesa/glapi/es2api \
       -lEGL -lGLESv2 -lm -Wl,-rpath,$B/src/egl:$B/src/mesa/glapi/es2api -o es31-nvrm
    LIBGL_DRIVERS_PATH=$B/src/gallium/targets/dri ./es31-nvrm
    cc -std=c11 -Ibuild/mesa-gl-source/include $T -lEGL -lGLESv2 -lm -o es31-sys
    __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json LIBGL_ALWAYS_SOFTWARE=1 ./es31-sys
    __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json ./es31-sys

10-15: llvmpipe 26.2.4 (ES 3.2) PASS, stock NVIDIA 615.71.09 (ES 3.2) PASS,
nvrm 0ca9444 FAIL "no OpenGL ES 3.1 context" + "GL_EXT_texture_cube_map_array missing".

## gl-es31-compute.c, gl-es31-helper-loads.c (GLES 3.1 part A: compute, SSBOs, indirect)

gl-es31-compute: dispatch with SSBOs, shared memory + barrier(), a 32 KiB shared array,
SSBO atomics, atomic counters (lowered to SSBOs), a UBO past the default block,
gl_NumWorkGroups, dispatch -> barrier -> dispatch and dispatch -> barrier -> draw chains,
16 SSBOs + UBO binding 14 in one compute and one fragment shader (Mesa's ceilings),
texture sampling in compute; `indirect` argument runs glDispatchComputeIndirect instead.
Every value is checked against the CPU; the digest compares drivers. Build as gl-es31-mc.c;
nvrm still reports ES 3.0 until part B lands, so run it with MESA_GLES_VERSION_OVERRIDE=3.1.

Result: nvrm PASS (and PASS with `indirect`), digest = stock NVIDIA
615.71.09 (stock fails only the limit check: 14 UBO blocks), llvmpipe PASS (digest differs by
blend rounding of one colour channel in case 4).

gl-es31-helper-loads: dFdx/dFdy of an SSBO load on a triangle edge, 64x64 RGBA8 written to
$OUT. nvrm without NVK's SET_PRIV_REG sm_disp_ctrl bit-3 clear: 32 edge pixels differ from
stock and llvmpipe (which agree); with it: byte-identical to stock.
## gl-cs-sysvals.c (compute system values, gl_LocalInvocationIndex)

Each invocation of 16x16x1 (Minecraft's GLSL176 shape), 4x2x3, 64x1x1, 1x1x1 and 7x5x3
workgroups (2x3x2 groups) writes gl_LocalInvocationIndex/ID, gl_GlobalInvocationID,
gl_WorkGroupID, gl_NumWorkGroups and gl_WorkGroupSize to its own SSBO slot, checked on the
CPU; plus a 16x16 shared-memory reduction indexed by gl_LocalInvocationIndex. Build as
gl-es31-mc.c; PASS on nvrm and llvmpipe. Regression it guards (Mesa 26.2 rebase, MC startup
abort 10-16): NAK no longer lowers load_local_invocation_index (25.2's nir_options had
lower_cs_local_index_to_id), so nvrm lowers it before NAK like NVK (nvk_shader.c
csv_options.lower_local_invocation_index); without it the link panics in NAK from_nir.rs
"Unsupported intrinsic instruction: load_local_invocation_index". 10-16 (5090): PASS; dEQP-GLES31
functional.compute.* + functional.ssbo.atomic.* 243/243 Pass.

## gl-es31-b.c (nvrm GLES 3.1 part B: images, gather, stencil texturing, no-attachment FBOs, MS fetch)

Fragment-shader image store/load/atomics (2D, 2D-array layer, layered 3D,
3D slice, layered cube, imageSize, 16 and 32 image units in one shader),
glMemoryBarrier, framebuffer_no_attachment, textureGather (+Offset, shadow),
DEPTH_STENCIL_TEXTURE_MODE stencil sampling, texelFetch on sampler2DMS and
GL_SAMPLE_POSITION. PASS/FAIL lines carry CPU expectations; the value lines
are diffed against llvmpipe. Until compute (part A) exposes ES 3.1 by itself,
nvrm needs `MESA_GLES_VERSION_OVERRIDE=3.1`:

    B=build/nvrm-host-es31b
    cc -std=c11 -Ibuild/mesa-gl-source-es31b/include tests/gl-es31-b.c \
       -L$B/src/egl -lEGL -L$B/src/mesa/glapi/es2api -lGLESv2 \
       -Wl,-rpath,$PWD/$B/src/egl:$PWD/$B/src/mesa/glapi/es2api -o gl-es31-b-nvrm
    MESA_GLES_VERSION_OVERRIDE=3.1 LIBGL_DRIVERS_PATH=$PWD/$B/src/gallium/targets/dri ./gl-es31-b-nvrm > nvrm.txt
    cc -std=c11 -Ibuild/mesa-gl-source-es31b/include tests/gl-es31-b.c -lEGL -lGLESv2 -o gl-es31-b-sys
    __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json LIBGL_ALWAYS_SOFTWARE=1 ./gl-es31-b-sys > llvmpipe.txt
    diff <(grep '^[a-z0-9-]*\[' llvmpipe.txt) <(grep '^[a-z0-9-]*\[' nvrm.txt)

10-15 on the 5090: both 16/16 PASS, 932 value lines, 7 differ in the 6th
decimal only (llvmpipe's UNORM8->float is 1 ulp off k/255).

## gl-linear-zs.c, gl-zero-vb.c, gl-border-swizzle.c (nvrm vs NVK divergences D1/D2/D4)

Build lines as gl-dmabuf.c (gl-linear-zs needs `-lvulkan` and our NVK ICD for the exportable
LINEAR buffer) and gl-g0a-clear.c (the other two); system builds (`-lEGL -lGLESv2`) give the
llvmpipe references.

- `gl-linear-zs ref|dmabuf <outdir>` (D1): LINEAR dma-bufs (host-visible exportable VkBuffer,
  imported with the LINEAR modifier) as FBO colour, {64x64 pitch 256, 40x40 pitch 192} x
  {DEPTH_COMPONENT24 renderbuffer, none}, two batches per scene. nvrm renders linear only
  under NVK's rule (no Z/S, base and pitch %128), otherwise through a block-linear shadow
  copied in per batch and back on flush. 10-15 (5090): all 4 scenes byte-identical to
  llvmpipe, dma-buf memory == readback, gdb shows 6 copy-in/out pairs (3 shadowed scenes x 2
  batches), the aligned no-depth scene renders LAYOUT_PITCH directly. Main 48d2524's code
  (not run) aborts on the pitch-192 scenes and programs a pitch CT together with ZETA.
- `gl-zero-vb` (D2): an attribute at offset == buffer size, a zero-size buffer object, an
  index offset == index buffer size, user indices; exact pixels, 0 Xid.
- `gl-border-swizzle <out.rgba> [ref.rgba]` (D4): GL_CLAMP_TO_BORDER border colour under
  texture swizzles, L/A/LA, R/RG, sRGB (incl. AAAA on SRGB8_ALPHA8) and RGBA16F; <= 1 LSB vs
  llvmpipe. 10-15 (5090): 15/15 with the fix; without PIPE_QUIRK_TEXTURE_BORDER_COLOR_SWIZZLE_NV50
  9/15 differ, with the quirk but without the sRGB-view pairing the AAAA sRGB case differs.
  dEQP-GLES31 border_clamp (MESA_GLES_VERSION_OVERRIDE=3.1, temporary; nvrm exposes ES 3.0)
  passes 624/625 with and without the fix (1 NotSupported) - it has no swizzled cases.
- Pre-Turing rental 10-15 (runid 20261015-nvrm-gldiv, bundle build/gl-div-vast d124f660a0fd):
  GTX TITAN X (Maxwell, 0xb197, 580.173.02) and TITAN X Pascal (GP102, 0xc197, 570.172.08):
  gl-zero-vb PASS, G0a PASS, G0b flat 0/4096, dEQP-GLES2 rasterization/fbo/clipping/texture
  2341 cases identical to the gl-merge-pv rentals, 0 Xid. The P100 (PASCAL_A) never ran.

## gl-gs.c (geometry shaders: OES/EXT_geometry_shader, EXT_texture_cube_map_array)

ES 3.1 context; point -> quad expansion (gl_PrimitiveIDIn), layered rendering
(`invocations = 4`, gl_Layer into a 4-layer 2D-array FBO via glFramebufferTextureEXT),
lines/triangles/triangle-strip adjacency (vertex order per the EXT_geometry_shader
tables), samplerCubeArray under `#extension GL_EXT_texture_cube_map_array : require`.
Every case is checked analytically; `[dump-file]` writes the raw pixels for a byte diff.

    B=build/nvrm-host-gs; T=nvidia-prolapse/driver-source/native/mesa/tests/gl-gs.c
    cc -std=c11 -Ibuild/mesa-gl-source-gs/include $T -lEGL -lGLESv2 -lm -o gs-sys
    __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json LIBGL_ALWAYS_SOFTWARE=1 \
      ./gs-sys llvmpipe ref.bin
    cc -std=c11 -Ibuild/mesa-gl-source-gs/include $T -L$B/src/egl -L$B/src/mesa/glapi/es2api \
       -lEGL -lGLESv2 -lm -Wl,-rpath,$B/src/egl:$B/src/mesa/glapi/es2api -o gs-nvrm
    LIBGL_DRIVERS_PATH=$B/src/gallium/targets/dri ./gs-nvrm nvrm out.bin

Stock NVIDIA cannot run it (no surfaceless ES 3.1 context), llvmpipe is the comparator.

10-15 (5090, SM120, branch gl-gs): 6/6 PASS on nvrm and llvmpipe 26.2.4; raw dumps differ by
at most 1 LSB (alpha 0.5 -> 128 vs 127, 20,544 bytes). dEQP-GLES31 (rgba8888d24s8ms0, results
in build/deqp/results-gl-gs3): geometry_shading 198 P / 9 NS / 0 F of 207 (llvmpipe 206 / 1;
the 8 extra NS are *_2d_multisample_array = OES_texture_storage_multisample_2d_array, not
exposed; basic.output_256 is NS on both); cube-array groups 714 P / 17 NS / 0 F of 731 (NS:
sRGB RG8, texture buffers, android_extension_pack). Before the max_vertices = 0 clamp,
emit.*_emit_0_* raised Xid 13 "Shader Program Header 7 Error". 0 Xid on the final build.

## gl-polygon-mode.c (GL_NV_polygon_mode on ES; Minecraft per-frame error)

Minecraft Bedrock (libminecraftpe.so imports the plain name `glPolygonMode`, no
polygon_mode extension string) calls glPolygonMode(GL_FRONT_AND_BACK, GL_FILL) every
frame. Mesa's eglGetProcAddress returns the PolygonMode dispatch stub, which on ES had
no implementation -> "GL_INVALID_OPERATION in glPolygonMode(invalid call)" per frame.
ANGLE exposes GL_NV_polygon_mode + GL_ANGLE_polygon_mode. Fix:
`mesa-nv-polygon-mode.patch` (GL_NV_polygon_mode for ES2+, glPolygonModeNV aliasing the
PolygonMode slot, FRONT_AND_BACK only, POLYGON_MODE_NV / POLYGON_OFFSET_{POINT,LINE}_NV,
gated on the new `pipe_caps.polygon_mode_es`) + nvrm SET_FRONT/BACK_POLYGON_MODE as
nvk_flush_rs_state. Analytic checks (llvmpipe does not set the cap): both names resolve
to one stub, MC's call raises no error, FRONT/BACK -> INVALID_ENUM, FILL covers the
interior, LINE = 144 edge pixels and an empty interior, POINT = the 3 vertex pixels.
10-15 (5090): PASS.

    cc -std=c11 -I<mesa>/include gl-polygon-mode.c -L$B/src/egl -lEGL -L$B/src/mesa/glapi/es2api \
       -lGLESv2 -Wl,-rpath,$B/src/egl:$B/src/mesa/glapi/es2api -o gl-polygon-mode
    LIBGL_DRIVERS_PATH=$B/src/gallium/targets/dri ./gl-polygon-mode

## gl-shader-heap.c (growable shader arena; Minecraft in-world abort)

Loading a Minecraft world aborted with "nvrm: shader heap (4194304 B) exhausted": the
heap was one fixed 4 MiB GART BO. Now NVK's contiguous nvk_mem_arena: 4 GiB VA reserved
at screen creation (pre-Volta SET_PROGRAM_REGION base), memories of 64K, 64K, 128K, ...
bound on demand at base + committed, 2 KiB prefetch pad kept at the committed end,
uploads may straddle memories. The test links N programs with unfoldable 400-step
integer chains (xorshift + LCG; a pure LCG chain is reassociated by NIR into one IMAD),
draws each into its own pixel, checks exact CPU values, deletes half, relinks, checks
again. 10-15 (5090): N=400 at 00ff6c5 aborts "shader heap (4194304 B) exhausted";
with the arena PASS (0 wrong pixels before and after reclaim).

## Pre-Turing indirect draws/dispatches (ES 3.1 + geometry shaders on Maxwell, Pascal, Volta)

nvrm follows NVK's pre-Turing branches (nvk_cmd_draw.c:4273-4470, nvk_cmd_dispatch.c:459-588):
the Fermi-style MME cannot DMA, so the indirect commands are appended to the
CALL_MME_MACRO method as a NO_PREFETCH GPFIFO entry (nvk_cmd_buffer_push_indirect,
nvk_cmd_buffer.c:187-200; nvrm_push_indirect + the NvRmExec array in nvrm_flush). Below
PASCAL_B, instanced draws, direct ones included, loop instances in the MME
(nvk_mme_draw/_indexed). Host check without a GPU: the macros run in Mesa's MME simulator
(mme_sim_core) per class. On 0xb197, 0xc097, 0xc197, 0xb097 and 0xc397 they emit the
expected base vertex/instance, BEGIN FIRST/SUBSEQUENT or SET_INSTANCE_COUNT, and the
gl_NumWorkGroups/QMD stores, and skip the pad dwords.

On rented pre-Turing GPUs every class
reports OpenGL ES 3.1 + GL_EXT_geometry_shader + GL_EXT_texture_cube_map_array; gl-es31-compute
digest ac886ae3 (= the 5090) with direct and indirect dispatch; gl-es31-b, gl-gs and gl-es31-mc
PASS. Per-case dEQP results vs the 5090 are in the next paragraph.
Lists: round 1 = GLES31 draw_indirect / compute.indirect_dispatch /
geometry_shading / compute.basic (487) + GLES3 subsets (2,971). Round 2 = the 5090's
local lists: GLES31 compute/ssbo/image/atomic/draw_indirect/GS/... 5,522, GLES3
draw/instanced/primitive_restart/vertex_arrays/transform_feedback/fbo/clipping/
rasterization/multisample/draw_buffers_indexed/fragment_out 7,065. The 5090 local
results equal results-gl-gs3 case for case. Round 2 per GPU: 12,587 cases = 11,659 P /
922 NS / 6 QW, 0 per-case diffs vs the 5090 on GTX TITAN X (GM200, 580.173), Tesla P100
(GP100 PASCAL_A, 550.54), GTX 1070 (GP104, 580.173), TITAN V (580.173) and GTX 1660 Ti
(TU116, 595.99). Round 1 matched on GTX 1060, V100 and RTX 2060 too. Its 2 GM200/GP100
crashes (draw.instancing.*_grid_100x100: 10000 CPU-unrolled instances overflowed the push
buffer) are what led to the MME instance loop. 0 NVML Xid and 0 kernel NVRM lines on every
host that returned results. MAXWELL_A (GM107/GM108) is enabled by the same code and
simulator-checked, but no rental offer existed to run it on hardware.

## gl-mc-vis.c (clip control, depth clamp, copy_image; Minecraft visual bugs)

Build lines in the file header (nvrm build and the system llvmpipe build). Self-checking
against analytic expectations; llvmpipe (system Mesa 26.2.4) passes every check, which
validates the expectations. Optional argument = one section (`mc`, `clip`, `fragz`, `copy`).

- `mc`: bgfx's blit fallback without copy_image (a depth texture attached as the read FBO's
  COLOR_ATTACHMENT0 + glCopyTexSubImage2D) is INCOMPLETE_ATTACHMENT +
  GL_INVALID_FRAMEBUFFER_OPERATION on every Mesa driver (fbobject.c "bad format" ->
  teximage.c "invalid readbuffer") = Minecraft's ~1,034 errors / 75 s; the same copy through
  glCopyImageSubDataEXT is exact. The fix is exposing OES/EXT_copy_image, not the readbuffer.
- `clip`: 8 strips at clip z/w -1.5..1.5 into D32F and D24, {NEGATIVE_ONE,ZERO}_TO_ONE x depth
  clamp off/on x depth range (0,1)/(0.25,0.75)/(0.9,0.1): visibility and stored depth.
- `fragz`: gl_FragDepth 1.5/-0.5 stored clamped to [0,1] on float and unorm depth.
- `copy`: RGBA8 mip sub-rects, D32F/D24/D24S8/D32FS8 (depth + stencil sampled), 2D array ->
  3D -> cube face -> 2D, RGTC1/BC1 <-> RGBA16UI (both directions), render -> copy -> sample
  x64 without flushes, compute imageStore -> glMemoryBarrier -> copy, glCopyBufferSubData
  300,000 B (crosses the 128 KiB copy-engine line limit).

Results: RTX 5090 PASS (llvmpipe PASS = expectations).
dEQP-GLES31 copy_image (12,395, was NotSupported): nvrm 12,394 P + 1 runner batch-timeout case
that passes alone; llvmpipe 12,395 P. The viewclass_96_bits rgb32f<->rgb32i/ui cases first hit an
st/mesa assert (st chose R32G32B32X32_FLOAT for GL_RGB32F, 96-bit R32G32B32_SINT for GL_RGB32I) ->
nvrm offers RGB32 for buffers only (nvrm_is_format_supported). GLES3 clipping / rasterization /
fragment_ops / texture.specification / texture.mipmap / fbo / buffer.copy / read_pixels /
negative_api (7,922) and the GLES31 compute/ssbo/image/atomic/draw_indirect lists (3,522): 0
per-case changes vs results-main4 / results-gl-mc2. Rentals (bundle build/gl-mcvis-vast, run
20261015-nvrm-mcvis): GTX 1080 (GP104, 580.178.04) and RTX 2070 (TU106, 580.159.03): gl-mc-vis
PASS, gl-es31-mc PASS, every 4th copy_image case (3,099) + GLES3 subsets (1,390) = 5090 except one
runner batch-timeout case each; 0 NVML Xid, 0 kernel NVRM lines. Run with --deqp-surface-width/height
(the runner passes 256x256): without them dEQP's copy_image render path divides by zero (SIGFPE in
RandomizedRenderGrid) on every driver.

## gl-perf-frame.c (memory placement + map synchronisation, directional)

Per frame: N draws into a 1024x1024 FBO, each after a glBufferSubData of a 256 B UBO, with a
static VBO, a 64 KiB UBO table read by the VS and a 1 MiB SSBO + 1024² texture read per
fragment; glFinish per frame. Prints wall / process-CPU / waiting ms per frame and a checksum
of the last frame. `NVRM_DEBUG_STATS=1` prints the screen's submission and CPU-wait totals at
context destruction (run two frame counts and difference them); `NVRM_FORCE_SMALL_BAR=1` takes
the small-BAR paths (copy-engine shader upload, VRAM buffers behind GART staging) on a full-BAR
GPU. Build as gl-es31-mc.c. Directional only: another GPU client changes the numbers.

10-15 (5090, other GPU users possible), `./gl-perf-frame 3 20`: main 75f5efc (GART buffers,
flush + wait per busy map) 1771 ms/frame; gl-perf (VRAM placement, staging for busy discarding
writes) 5.2 ms/frame full BAR, 5.3 ms forced small BAR, same checksum 5365acee; stock 615 4.7 ms
(checksum differs: rounding). `30 200`: gl-perf 53 ms/frame, CPU 0.4 ms; stock 44.5 ms (CPU-spins
in glFinish); main 75f5efc > 9 s/frame (timed out).
With the asynchronous flush (gl-perf d152f6a+): `13 20` 5.0-5.2 ms/frame full BAR, 5.3-5.5 ms
forced small BAR, ~1 submission and 1 CPU wait per frame (the glFinish; was 20 waits per frame
at b15c627 before busy discarding writes went through staging); `20 100 pipelined` (glFlush per
frame) 25.6 ms/frame vs stock 22.4 ms: GPU-bound, the CPU waits only when the 4-slot batch ring
is full. Rentals (small BAR 256 MiB): RTX 2080 Ti `13 20` 20 ms/frame, GTX 1080 Ti 79 ms/frame
(b15c627; same checksum 5365acee everywhere).

Rentals 10-15 (bundle build/gl-perf-vast, all small BAR = 256 MiB BAR1, host tests on the
native and the forced small-BAR paths + dEQP-GLES31 compute/ssbo/synchronization/draw_indirect
+ dEQP-GLES3 buffer/ubo/vertex_arrays/fbo/texture): GTX 1080 Ti (GP102, 580.178.04) at da03a20
and GTX TITAN X (GM200, 580.173.02) at f86d90c: 14,528 cases each, 0 per-case differences vs
the 5090, 0 NVML Xid; RTX 2080 Ti (TU102, 580.178.04) at b15c627: host tests + 6,520 dEQP cases
pass, 0 Xid (instance lost before the texture group). Pre-existing, not from this branch:
the 88 dEQP-GLES3 rgb32i/rgb32ui texture cases (format.sized, specification.*rgb32i/ui*,
units.* that pick those formats) fault with Xid 13 on GTX 1080 Ti at main 75f5efc as well
(bisect bundle, both builds 88/88 Crash + 88 Xid); they were left out of the later lists.

## gl-perf-mc.c (Minecraft's per-frame GL pattern: what nvrm emits per draw)

Per frame: TexSubImage2D 64x64, pass A (DRAWS indexed draws into RGBA8 + D24S8, a
glUniformMatrix4fv + glUniform4f before each, texture + sampler bind every 3rd, UBO range
re-pointing every 16th, busy-VBO glBufferSubData every 100th), pass B (64 draws sampling A),
a compute dispatch writing an SSBO + glMemoryBarrier, pass C (16 draws reading the SSBO and
sampling B), glFinish. Prints wall/CPU ms per frame (directional) and an FNV-1a checksum of
A and C (depends on the frame count: compare equal counts). Build as gl-es31-mc.c; run
`./gl-perf-mc [frames] [draws]`. Counting: capture with NV_RM_CAPTURE (below) armed and the
`go` file created ~2 s after start (host glFinish exports no fence, so the capture is one
"frame": divide nvcap totals by draws / 1080), syscalls with `strace -f -c` at two frame
counts, differenced.

10-15, RTX 5090, GPU shared with a live Minecraft session (timings not comparable), 1000
draws: gl-prof bbc5be2 vs gl-pb: BIND_GROUP_CONSTANT_BUFFER 42.3 -> 0.12 per draw;
INVALIDATE_SHADER_CACHES_NO_WFI 1082 -> 14 per frame; texture header / sampler cache
invalidates 339 -> 11 per frame; LOAD_CONSTANT_BUFFER 0 -> 4.2 loads / 9.7 words per draw;
method writes 71.9 -> 35.8 per draw, push bytes 337 -> 202 per draw, kicks 2 -> 1 per frame;
ioctl 10.4 -> 0, mmap / munmap / openat 1.6 -> 0 per frame; checksum 2a846dc0 (30 frames)
and gl-perf-frame 5365acee unchanged, also with NVRM_FORCE_SMALL_BAR=1.

## Perf capture (Rust RM backend, NVK and nvrm; off by default)

`NV_RM_CAPTURE=<dir>` (host) or `setprop debug.nvrm.capture <dir>` + `debug.nvrm.capture.proc <cmdline substring>`
(guest) arms per-submission GPU timestamps (two host semaphore releases per kick) and a VA index in the matching
process; `echo 'ts frames=300' > <dir>/go` (or `push frames=30`, which also copies every pushbuffer) records the next
N presents to `<dir>/cap-<pid>-<dev>-<n>.bin`. Decode with `native/tools/nvcap.py CAP --driver nvk|nvrm [--json]`
(per-frame kicks, GPFIFO entries, bytes, draws, method histogram, WFIs, invalidates, CE/I2M, cbuf binds/loads,
GPU busy/idle) and `native/tools/nvcap_timeline.py CAP FRAME N` (per-kick CPU submit vs GPU start/end).
Host check 10-15: `gl-perf-frame 13 20` with capture on: checksum 5365acee unchanged, GPU 4.6 ms/kick vs
wall 4.69 ms/frame. Live Minecraft runs: outer build/gl-prof/runs/{angle1,angle2,nvrm1}.

P-A (branch gl-pa, 10-15): push memory is a pool of 256 KiB GART chunks chained into the open batch
as extra GPFIFO entries (NVK nvk_cmd_buffer_new_push); kicks only at flush / fence / CPU sync, or
after 8 MiB of commands (budget 16 MiB per context). Window shadow copies are recorded inline,
skipped for clears that do not write the shadowed colour buffer, for full clears and discards.
eglWaitSync no longer kicks (stock: wait at the next flush). GL built release; Mesa trace scopes
(upstream Mesa 898fa317, mesa-upstream-898fa317-trace-func.patch) no longer vsnprintf a name per draw when nothing traces. Host checks
(build -gl-pa, merged with gl-pb 5f13e30): gl-perf-frame 3 20 checksum 5365acee, 30 200 checksum
7285664f (= gl-prof), gl-mc-vis / gl-es31-mc / gl-shader-heap / gl-dmabuf PASS, gl-linear-zs
dmabuf byte-identical to llvmpipe on all 4 scenes. Live MC: outer build/gl-pa/runs/ (kicks.py,
seq.py, offcpu.py there).

## GTA SA live flicker check (`gta-flicker-live.py`, live Waydroid, 2026-10-16)

`python3 native/mesa/tests/gta-flicker-live.py OUT_DIR [--launch] [--frames N]` (host, needs `sudo -n lxc-attach`
and python3-pillow). `--launch` force-stops com.rockstargames.gtasa, starts GameActivity, taps "Skip Sign in" and
"Resume" (coordinates scaled from 2560x1412) and waits for a lit scene; without it the game must already be in
gameplay. It takes N guest `screencap` frames and counts DROPOUT frames (>= 90 % exactly-black pixels while the
burst median is < 50 %). Exit 0 PASS, 1 FAIL (lists the frames), 2 not measurable. Output: OUT_DIR/frameNN.png
(half size) + result.json. The signature it was written for: 7/12
frames black except the untextured HUD health bar, byte-identical to each other, i.e. every textured draw of the
final window pass returned 0 (composite quad black, textured HUD invisible). Not reproduced since (see the
gta-flicker branch commit message); keep running it after every nvrm/NVK deploy.

Capture analysis helpers for the push-mode capture hook: `native/tools/nvcap_seq.py CAP [submit...]` prints the
ordered method stream per submission; `native/tools/nvcap_draws.py SEQ.txt [submit]` turns that into a per-draw
table (colour target, Z test/func/write, blend, CT write, fragment texture-handle load) plus clears. Arm the hook
for a guest app with `setprop debug.nvrm.capture <dir owned by the app uid>`, `debug.nvrm.capture.proc <pkg>`,
`debug.nvrm.capture.mode push`, relaunch the app, then `echo "push frames=4" > <dir>/go`.

**Late arming (texpool-audit, 2026-10-16): no relaunch needed.** A process opened without a capture
configuration looks for it again from submit once per second (`capture::ARM_POLL`), so a game that is
showing an intermittent bug can be captured as it runs: `setprop debug.nvrm.capture.proc <pkg>`,
`setprop debug.nvrm.capture.mode push` FIRST, then `setprop debug.nvrm.capture <dir>` (every polling process
arms as soon as that one is set, with whatever filter is set then), wait ≥ 1 s (logcat: "capture armed late:
N contexts, M binds indexed"), then `echo "push frames=4" > <dir>/go`. The late arm adds the timestamp releases
to every live context and indexes the binds that exist at that moment. Clear `debug.nvrm.capture` afterwards
(processes that armed stay armed until they exit). Host test `gl-capture-late-arm.c` (build as gl-g0a-clear.c;
`./gl-capture-late-arm DIR`, it sets NV_RM_CAPTURE with setenv() mid-run): 10-16 PASS, capture started and
nvcap_seq.py decoded 13,595 submits with pushbuffer words.

Push mode also records the **texture header and sampler pools** each submission binds (REC_POOL: contents as the
CPU wrote them, read at submission, for pools in CPU-mapped memory, ≤ 1 MiB). `native/tools/nvcap_pools.py CAP
[--dump]` lists per submission the non-zero entry counts and every texture handle loaded into a 256-B handle
window (nvrm's driver cbuf) whose header or sampler entry is all zeros: for the GTA signature it tells "the CPU
wrote a bad table / handle" (BAD lines) from "the table was right and the GPU read something else" (no BAD).

## gl-texpool-rebind.c (texture pool ring under in-flight work, texpool-audit 2026-10-16)

Forces the conditions under which a whole pass could sample zeros, instead of waiting for them (GTA SA flicker
audit). Batch 1 = a slow full-screen draw (`iters` ALU loop, then one fetch of texture RED) into a 2048² FBO,
`glFlush`; batch 2 is submitted ~10 µs later while batch 1 runs (timings printed). PASS = every pixel of the slow
draw is RED in every round. Variants (`./gl-texpool-rebind ROUNDS ITERS [variant]`, build as gl-g0a-clear.c):
- (none): batch 2 samples GREEN: its first draw binds the next ring slot's pools (SET_TEX_HEADER_POOL /
  SAMPLER_POOL + header/sampler cache invalidates) before its framebuffer WAIT_FOR_IDLE, under batch 1's draw;
- `zero`: batch 2 is untextured (next slot's pool slot 0 never written = zeros, the GTA signature);
- `wrap`: 6 more GREEN batches, so the ring returns to batch 1's slot while it runs (the flush that rotates onto
  it must wait for batch 1 before slot 0 is rewritten);
- `ctx2`: batch 2 from a second shared context on the same screen channel (its own pools + handle cbuf);
- `free`: RED is deleted right after batch 1's submission and 32 new GREEN textures are created and drawn
  (view / resource / memory / VA released while sampled; pool slots reused).
10-16 (RTX 5090, branch texpool-audit on main 6e7bbd5, slow batch 31-32 ms at 40000 iters): all five variants
PASS, 0 wrong pixels of 4,194,304 per round (none/zero 6 rounds, wrap/ctx2/free 4 rounds); in `wrap` batch 2..8
took 32.4 ms to submit (the ring-slot wait for batch 1). 0 Xid.

## vk-nan-minmax.c (FMax/FMin/FClamp with a NaN operand under ANGLE; GTA SA black car paint)

    T=build/gta-carpaint/test; mkdir -p $T
    glslangValidator -V nvidia-prolapse/driver-source/native/mesa/tests/vk-nan-minmax.comp \
      --vn vk_nan_minmax_spv -o $T/vk-nan-minmax.spv.h
    cc -std=c11 -Wall -Wextra -Werror -I$T nvidia-prolapse/driver-source/native/mesa/tests/vk-nan-minmax.c \
      -lvulkan -lm -o $T/vk-nan-minmax
    E="VK_DRIVER_FILES=$PWD/build/nvk-host/native-rm-icd.json VK_LOADER_LAYERS_DISABLE=~implicit~ \
       DRIRC_CONFIGDIR=$PWD/build/mesa-rm-source/src/nouveau/vulkan"
    env $E $T/vk-nan-minmax ANGLE maxnum          # PASS = driconf entry active
    env $E $T/vk-nan-minmax nvk-nan-test report   # other engines keep upstream semantics
    VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json $T/vk-nan-minmax ANGLE maxnum   # stock reference

The host build reads driconf from files (expat), hence DRIRC_CONFIGDIR; Android builds embed
00-nvk-defaults.conf (`-Dexpat=disabled` -> driconf_static.h). Compute dispatch of
`max(pow(d,10),0)*2`, `clamp(pow(d,10),0,1)`, `min(-pow(d,10),0)` for d = -0.5, 0.5, -2, 0.
2026-10-16, RTX 5090: NVK 26.2.5 unpatched = NaN for max/min at d < 0 (clamp 0); stock 615.71.09 =
0 for every engine name; patched NVK with engine "ANGLE" = PASS, engine "nvk-nan-test" or "ANGLEx" =
NaN (upstream), `nvk_force_nan_preserve_min_max=true` in the environment = PASS for any engine.

## gl-shader-async.c (nvrm program cache + asynchronous compile, shader-f 2026-10-17)

Share-group test of nvrm's compile queue: per round (1) link + immediate delete (in round 0 the NAK
jobs are cold, so the delete drops queued jobs and waits for running ones), (2) context A links and
hands each program to a shared context B on another thread, which makes the first use (the wait),
draws and deletes it, (3) `glMaxShaderCompilerThreadsKHR` + `GL_COMPLETION_STATUS_KHR` polling, then
a draw per program. Input: a shbench-format directory, e.g. MC set A
(36 programs from Minecraft's ActorForwardPBR material; not distributed).

    B=build/nvrm-host-shader-f
    cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Ibuild/mesa-gl-source-shader-f/include \
       nvidia-prolapse/driver-source/native/mesa/tests/gl-shader-async.c \
       -L$B/src/egl -L$B/src/mesa/glapi/es2api -Wl,-rpath-link,$B/src/gallium/targets/dri \
       -lEGL -lGLESv2 -lpthread -o gl-shader-async
    LD_LIBRARY_PATH=$B/src/egl:$B/src/mesa/glapi/es2api:$B/src/gallium/targets/dri \
       MESA_SHADER_CACHE_DISABLE=true ./gl-shader-async DIR 2      # also NVRM_FORCE_SMALL_BAR=1,
                                                                   # and MESA_SHADER_CACHE_DIR=<dir> (warm)

PASS = every link succeeds, no GL error after any draw, all programs complete within 60 s.
TSan: build the driver with `-Dbuildtype=debugoptimized -Db_sanitize=thread -Db_lundef=false` and the
test with `-fsanitize=thread -rdynamic ... native/mesa/tests/c11tsan.c` (mandatory: libtsan does
not intercept glibc's C11 mtx/cnd that Mesa uses). 2026-10-17: no report in nvrm, util_queue, the
program cache or disk_cache.

## gl-narrow-args.c (GLboolean arguments with garbage above bit 7)

Calls `glColorMask`, `glColorMaski{,OES,EXT}` and `glDepthMask` through function pointers typed with
32-bit parameters (exported symbols and `eglGetProcAddress` results), passing what an arm64 Unity game
running under Android's ARM translation passed for `glColorMask(0, 0, 0, 1)`: r=0x0 g=0x10000 b=0x100
a=0x1. PASS = `GL_COLOR_WRITEMASK` / `GL_DEPTH_WRITEMASK` follow the low byte of every argument and a
masked clear and a masked draw only change alpha. Build as gl-g0a-clear.c. The x86-64 psABI leaves the
upper bits of narrow arguments unspecified; clang-built callees test whole registers, GCC-built ones the
low byte, so only a clang-built Mesa (the Android NDK build, or a host build with CC=clang) fails without
mesa-glapi-narrow-args.patch: before (clang host build) the exported and GetProcAddress `glColorMask`
calls give writemask 0111, after 0001.

## vk-shader-reuse.c (shader heap address reuse; shader publication order)

Each iteration creates a graphics pipeline from one of 8 vertex and 12 fragment shaders (exact
unrolled integer chains of different lengths, generated by `vk-shader-reuse-gen.py`, needs glslc)
through a fresh VkPipelineCache, draws one full-screen triangle into R32_UINT, waits, checks every
pixel against the CPU value and destroys the pipeline and the cache, so the next pipeline's headers
and code land on recycled heap ranges. Stale program headers show as Xid 13 "Shader Program Header"
(VK_ERROR_DEVICE_LOST), stale code as wrong pixels. The test tears everything down at the end
(device destruction included). Host:

    T=build/shader-reuse-test; mkdir -p $T
    python3 nvidia-prolapse/driver-source/native/mesa/tests/vk-shader-reuse-gen.py $T
    cc -std=c11 -O2 -Wall -Wextra -Werror -I$T \
      nvidia-prolapse/driver-source/native/mesa/tests/vk-shader-reuse.c -lvulkan -o $T/vk-shader-reuse
    VK_DRIVER_FILES=build/nvk-host/native-rm-icd.json $T/vk-shader-reuse 3000 1        # idle between frames
    VK_DRIVER_FILES=build/nvk-host/native-rm-icd.json $T/vk-shader-reuse 2000 5 8 256  # 8 frames in flight, 256x256

Arguments: ITERATIONS SEED FRAMES SIZE. With FRAMES > 1 a slot's pipeline is destroyed only after its
fence signalled while the other frames still execute, so ranges are recycled under a busy queue.
It guards the recycled-range path; it does not reproduce the rare Xid 13 SPH fault (no deterministic
reproducer exists).

Publication order (`nvk-shader-upload-publish.patch`, `nvk-shader-prefetch-in-range.patch`): capture
the stream and check it with `native/tools/nvcap_publish.py`:

    mkdir -p $T/cap && echo "push frames=1" > $T/cap/go
    NV_RM_CAPTURE=$T/cap VK_DRIVER_FILES=build/nvk-host/native-rm-icd.json $T/vk-shader-reuse 300 7 8 64
    python3 nvidia-prolapse/driver-source/native/tools/nvcap_publish.py $T/cap/cap-*.bin

2026-10-19, GB202: with both patches 902 submits, 343 copy-engine copies, 300 instruction
invalidates, 600 program-address uses, 0 errors (every use after its copy, the acquire and the
invalidate; 256 B aligned; prefetch inside the copied range). Without them (same pin): no copies, no
instruction invalidate, 0x80-aligned addresses, 8 of 8 uses flagged.

Android: build the same source with the NDK's `x86_64-linux-android33-clang ... -lvulkan` and run it
in the guest.
