# Compressed-format emulation: VRAM accounting (NVHAL_ASTC_BYTES)

The 196-point glibc-shim run peaked at **8104 MiB of 8192 MiB VRAM** with about 7.2 GiB in the app, and there was no
positive evidence anywhere that the app's textures went through the ASTC/ETC2 emulation at all. This switch turns
that question into numbers: for every image the emulation creates, what the app has to allocate, what the same image
in its native compressed format would have cost, and which formats/blocks were involved. Off by default, independent
of the other debug switches, and it counts on the `vkCreateImage` path only (never per frame).

## What is counted

An emulated compressed image is not one allocation:

* the app's handle **is** an ICD image of the *decoded* format (RGBA8 UNORM/SRGB, or R16[G16] for EAC R11/RG11) - 4 or
  2 bytes per texel, really allocated by the ICD (`astc.c` header, `build_cis()`);
* beside it the HAL keeps **one block image per mip level**, one texel per block, 16 B for a 128-bit ASTC block and
  8 B for a 64-bit ETC2/EAC block - inside the *same* allocation the app is given;
* the memory requirements the app sees cover both (`combine()`, `astc.c`).

| field | meaning |
| --- | --- |
| `cumulative N emulated` | emulated images created since process start (never net of destroyed ones) |
| `refused` | `vkCreateImage` of a compressed format that could not be emulated at all (not emulated on this device, a create-info the emulation refuses, or an ICD failure): a nonzero count means those textures never existed for the app |
| `alive` | emulated images alive right now - **this** is the figure to compare with the app's VRAM footprint |
| `ASTC: ...` / `ETC2/EAC: ...` | histogram, cumulative: images ever created per block size (4x4 … 12x12) / per ETC2-EAC decoder variant (RGB8, RGB8A1, RGBA8, R11, signed R11, RG11, signed RG11) |
| `app-visible live` | bytes the app is currently required to allocate for those images (`VkMemoryRequirements.size`, decoded + all block images, aligned) |
| `cumulative created` | the same, summed over every image ever created |
| `= decoded … + block images …` | the two parts, from the ICD's own image memory requirements |
| `native compressed` | layers x sum over mips of `ceil(w/bw) * ceil(h/bh) * block_bytes`: the bytes the same mip chain would take in the real ASTC/ETC2 format (16 B per ASTC block, 8 B per ETC2/EAC block) |
| `-> X.Xx inflation` | `cumulative created / native compressed`, and `decoded alone` for the part that is only the RGBA8/R16 substitution |

Everything except `alive` and `app-visible live` is cumulative: the `+N` figure is what that window added, while the
histograms and the byte totals keep growing across windows (so a nonzero bucket on a late window can come from an
image created much earlier). The cumulative line at `vkDestroyDevice` repeats the totals with `alive` at 0.

The block-image bytes are normally a little **above** the native figure (the ICD pads rows); that they are close is
the cross-check that the native model is right. A native ASTC 4x4 image is 1 B/texel and its decoded RGBA8 twin is
4 B/texel, so the floor is 4x; bigger blocks (8x8 = 0.25 B/texel) push the decoded-only ratio to 16x.

## Switch

| where | variable | values |
| --- | --- | --- |
| host | `NVHAL_ASTC_BYTES` | unset or 0 = **off** (default); any positive value = on |
| host | `NVHAL_ASTC_BYTES_WINDOW` | report window in seconds, default 1.0, floor 0.1 |
| guest | `debug.nvhal.astc_bytes` | same as the host variable |
| guest | `debug.nvhal.astc_bytes.window` | same as the host window variable |

Read once per process, at the first emulated `vkCreateImage` - set it before starting the application. When it is
on, a banner goes out at that point (`astc bytes: statistics ON (window …)`), which is also the fingerprint that the
patched build is the live one. When it is off, an emulated `vkCreateImage` pays one acquire load plus a not-taken
branch and `vkDestroyDevice` one acquire load; nothing else changes.

## Report

One line per window (skipped while nothing was created) and one cumulative line at `vkDestroyDevice`, both through
the normal HAL log (tag `vulkan.nvidia`, so `logcat -s vulkan.nvidia`), for example for 812 1024x1024 single-mip
textures, 512 of them ASTC 4x4 and 300 ASTC 8x8:

    astc bytes[window 1.00 s]: +12 emulated/refused image(s) (cumulative 812 emulated, 0 refused, 812 alive) | ASTC: 4x4 x512, 8x8 x300 | ETC2/EAC: - | app-visible live 4021288960 B (3.75 GiB), cumulative created 4021288960 B (3.75 GiB) = decoded 3405774848 B (3.17 GiB) + block images 615514112 B (587.0 MiB); native compressed 615514112 B (587.0 MiB) -> 6.5x inflation (decoded alone 5.5x)

    astc bytes[cumulative at vkDestroyDevice 0.00 s]: +812 emulated/refused image(s) (cumulative 812 emulated, 0 refused, 0 alive) | ASTC: 4x4 x512, 8x8 x300 | ETC2/EAC: - | app-visible live 0 B (0.0 MiB), cumulative created 4021288960 B (3.75 GiB) = ...

The cumulative line is printed even when nothing was ever emulated (`cumulative 0 emulated, 0 refused`): that **is**
the answer to "did this app use the emulation path", and it is worth having in the log either way.

## Reading it for the VRAM question

* `alive` + `app-visible live` are the numbers to put beside the run's VRAM peak (the app's own share, from
  `dev/gpu-hotspots`): if the app's footprint is ~7 GiB and the emulation only accounts for ~0.3 GiB, the peak is
  **not** explained by compressed-format emulation, whatever the histogram says.
* A non-empty `ASTC:` / `ETC2/EAC:` histogram is the positive evidence that the app does upload compressed assets;
  at 7.2 GiB of app memory the interesting case is a large `alive` byte count.
* `refused` > 0 means textures were dropped on the floor (the corresponding `NVHAL_ERR` lines name the reason) - a
  correctness finding, not a memory one.

## Build

Both variants, from `nvidia-prolapse/hal` (the generator needs the **mesa** registry: the system one lacks
`vkCmdCopyImageToMemoryKHR` / `vkCmdCopyMemoryToImageKHR` and fails the coverage gate):

    cd nvidia-prolapse/hal
    VKXML=/home/quinovax/waydroid-nv/mesa/src/vulkan/registry/vk.xml

    # (a) plain HAL (guest ICD libEGL_nvidia.so.0) - the glibc-shim root
    make VKXML=$VKXML out/host/vulkan.nvidia.so out/android/vulkan.nvidia.so \
                      out/android32/vulkan.nvidia.so out/host32/vulkan.nvidia.so

    # (b) source-RM HAL (-DNVHAL_SOURCE_RM, guest ICD libvulkan_nouveau.so) - the prolapse root
    python3 ../driver-source/native/mesa/build-hal.py

Artifacts: (a) `hal/out/{host,android,android32,host32}/vulkan.nvidia.so`;
(b) `build/source-native-integration/{host,android,android32}/vulkan.nvidia.so`. Both switches
(`debug.nvhal.xlate_stats`, `debug.nvhal.astc_bytes`) are in **both** variants, so one deployment and one container
restart serve task-4 and task-8.

## Deploy (bench-operator / task-7; not run by the HAL author)

Same recipe as `XLATE-STATS.md`: `pkexec install` to `<path>.new` then `pkexec mv -f` (never truncate a mapped
`.so` in place), one file per root:

    pkexec install -m 644 nvidia-prolapse/hal/out/android/vulkan.nvidia.so \
        /var/lib/waydroid/nvidia-glibc-shim-staging/vendor_extra/lib64/hw/vulkan.nvidia.so.new
    pkexec mv -f /var/lib/waydroid/nvidia-glibc-shim-staging/vendor_extra/lib64/hw/vulkan.nvidia.so.new \
        /var/lib/waydroid/nvidia-glibc-shim-staging/vendor_extra/lib64/hw/vulkan.nvidia.so
    # 32-bit, same tree: out/android32 -> .../vendor_extra/lib/hw/vulkan.nvidia.so

    pkexec install -m 644 build/source-native-integration/android/vulkan.nvidia.so \
        /var/lib/waydroid/nvidia-prolapse-staging/vendor_extra/lib64/hw/vulkan.nvidia.so.new
    pkexec mv -f /var/lib/waydroid/nvidia-prolapse-staging/vendor_extra/lib64/hw/vulkan.nvidia.so.new \
        /var/lib/waydroid/nvidia-prolapse-staging/vendor_extra/lib64/hw/vulkan.nvidia.so
    # 32-bit: build/source-native-integration/android32 -> .../vendor_extra/lib/hw/vulkan.nvidia.so

The mechanical version of this recipe is `DEPLOY-STATS.sh` (task-11): `verify` compares repo vs live sha256
read-only, `deploy` backs all four files up to `artifacts/hal-backup-<ts>/` (with `TARGETS.tsv` + a self-contained
`ROLLBACK.sh`), then installs `.new` -> verifies its sha256 -> `mv -f`, and finally re-checks sha256 + both switch
counts; `DRY_RUN=1 ... deploy` only prints what it would do. See `XLATE-STATS.md` for the full command list and the
post-deploy 3DMark regression gate.

Then `dev/restart`, and before starting the app in the guest:

    setprop debug.nvhal.xlate_stats 1     # task-4: per-frame translation cost
    setprop debug.nvhal.astc_bytes 1      # task-8: compressed-format VRAM
    logcat -s vulkan.nvidia | grep -E 'xlate: statistics ON|astc bytes: statistics ON|xlate stats\[|astc bytes\['

The prolapse root's `source-rm.json` artifact hashes go stale on a raw copy (only `dev/source-native adopt` checks
them); the glibc-shim root keeps no per-artifact hashes.

## Answer (i): are textureCompressionASTC_LDR / textureCompressionETC2 set for the app?

**Conditionally, yes** - and only by the HAL's own wrappers:

* `astc.c:334-337` `emulated_features()`: `f->textureCompressionASTC_LDR = VK_TRUE` only if
  `p->state == NVHAL_ASTC_EMULATE`, `f->textureCompressionETC2 = VK_TRUE` only if
  `p->etc2_state == NVHAL_ASTC_EMULATE` - i.e. per physical device and per family, and only after the capability
  probe (`probe()` `astc.c:221`) decided the ICD lacks the family and the emulation's own requirements hold; when
  the probe fails, `astc.c:286`/`293` log `NVHAL_ERR(… stays 0 …)` and the ICD's value (0 on desktop NVIDIA) stands.
* Called from `nvhal_GetPhysicalDeviceFeatures()` (`astc.c:339-348`, after the ICD's own query) and from
  `features2()` (`astc.c:350-355`) behind `nvhal_GetPhysicalDeviceFeatures2` / `…2KHR` (`astc.c:357-362`).
* The app reaches them through the instance dispatch table: `hal.c:446-448`.
* On device creation the HAL **clears** both bits before handing the create info to the ICD - `astc.c:579-580` for
  `pEnabledFeatures` and `astc.c:586-587` for the `VkPhysicalDeviceFeatures2` chain (the ICD has no such support;
  the HAL answers for it). It also forces `shaderStorageImageWriteWithoutFormat` on for the decoders' output images.

So "the app enabled textureCompressionASTC_LDR and it still worked" is consistent with emulation; it says nothing
about whether any ASTC image was ever created - that is what the new counters answer.

## Answer (ii): why is the `ASTC LDR … ETC2/EAC …` line in device.c absent from logcat?

Because it is a **TRACE** line and TRACE is unreachable in the guest:

* `device.c:773` is `NVHAL_TRACE("vkCreateDevice: VkDevice %p (%s), … ASTC LDR %s, ETC2/EAC %s, …")`
  (arguments at `device.c:778-785`).
* `NVHAL_TRACE` is defined at `nvhal.h:49` as `do { if (nvhal_trace) nvhal_log(NVHAL_LVL_INFO, …) } while (0)`.
* `nvhal_trace` (`util.c:12`) is set in exactly one place, `nvhal_log_init()` (`util.c:15-18`), from
  `getenv("NVHAL_TRACE")` - and that is called once per process from `vkCreateInstance` (`hal.c:633`).
* There is **no Android-property path** for it: the only `__system_property_get` calls in the HAL are
  `debug.nvhal.gputime`, `debug.nvhal.gputime.window`, `debug.nvhal.exp.hostvis_to_bar`, and the two new ones
  (`debug.nvhal.xlate_stats`, `debug.nvhal.astc_bytes`). Android properties are not environment variables, and
  nothing in the container exports `NVHAL_TRACE`, so `getenv` returns NULL and every TRACE line - this one
  included - is silently dropped in the guest. (It is a level gate, not a compile-time `#ifdef`.)
* `NVHAL_WARN`/`NVHAL_ERR` do reach logcat (`util.c:27-31`, `__android_log_write` at WARN/ERROR). So a count of 0
  occurrences of any "astc"-looking line proves nothing about ASTC usage; the new `astc bytes` lines are
  `NVHAL_WARN` and will appear whenever the switch is on.
