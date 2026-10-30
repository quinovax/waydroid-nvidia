# VkImage translation statistics (NVHAL_XLATE_STATS)

The HAL rewrites every VkImage handle it hands to the application (a wrapper handle stands for a
gralloc-imported image). `imgxlate.c` can count and time that translation, per translator class and per
presented frame, so the go/no-go question "does translation cost more than 20% of the process CPU?" can be
answered from a normal 3DMark run instead of guessed. Off by default; the switch is read once per process.

## Switch

| where | variable | values |
| --- | --- | --- |
| host | `NVHAL_XLATE_STATS` | unset or 0 = **off** (default); 1 = **counts + timing**; 2 = **counts only** |
| host | `NVHAL_XLATE_STATS_WINDOW` | report window in seconds, default 1.0, floor 0.1 |
| guest | `debug.nvhal.xlate_stats` | same values as the host variable |
| guest | `debug.nvhal.xlate_stats.window` | same as the host window variable |

Any other positive value behaves as 1. The value is read **once per process**, at the first translator
call; later changes are ignored, so set the property *before* starting the application:

    setprop debug.nvhal.xlate_stats 1
    setprop debug.nvhal.xlate_stats.window 1
    am force-stop com.futuremark.dmandroid.application && am start ...

## What is counted

Three translator classes, two classes of count each:

* `img`  - `xl1()`: one image **reference** translated in place. A command with an image array produces
  one `img` call per array element.
* `arr`  - `xl_arr()`: one image **array**. A container: its ns include the `img` ns of its elements, and
  its "copies" count is the number of array calls that contained at least one wrapper.
* `obj`  - `real_handle()`: one object handle resolved for a query/name/private-data call.
* `calls`  - the entry was invoked with a non-null handle.
* `xlated` - the handle **was** a wrapper and really had to be rewritten (one hash lookup, a possible
  memory lookup, no allocation).

Totals: `total = img + obj` ms. `arr` is deliberately **not** added - it is a container whose time is
already inside `img`. What that leaves out is `arr`'s **own** cost (the stack-vs-malloc decision and the copy of
the handle array), which is real translation CPU spent once per array call. So read the translation cost as the
bracket `[total, total + arr_ms]` and, if the >20% verdict is close, decide from the two ends: if even
`total + arr_ms` stays below the threshold the answer is a clean no, and if `total` alone is above it the answer is
a clean yes. In the example above that is 12.400 vs 12.800 ms of 380 ms.

## Report

One line per window (default 1 s), printed through the normal HAL log (tag `vulkan.nvidia`, so
`logcat -s vulkan.nvidia`):

    xlate stats[timing]: 1.00s frames=120 (120.0 fps) cpu=380.0ms | img calls=48000 xlated=12000 (25.00%) 12.000ms 250ns/call | arr calls=600 copies=300 els=4800 0.400ms | obj calls=1200 xlated=1200 | total 12.400ms = 3.26% of cpu, 103.3us/frame, 410.0 calls/frame | clk=19.4ns meas~15.9us/frame

    xlate stats[counts]: 1.00s frames=120 (120.0 fps) cpu=380.0ms | img calls=48000 xlated=12000 (25.00%) | arr calls=600 copies=300 els=4800 | obj calls=1200 xlated=1200 | 410.0 calls/frame (no per-call timing in this tier)

* `frames` comes from the present path (`anb.c`, `nvhal_xst_frame`). `frames=0` means that path was not
  reached in this window, and then the per-frame columns are 0 - read the window totals instead.
* `total ... % of cpu` is the share of **this process's** CPU time (`CLOCK_PROCESS_CPUTIME_ID`) spent in the
  translators, which is the number the >20% question asks about.
  `clk` is the guest-measured cost of one `clock_gettime` and `meas~` the resulting measurement overhead
  per frame **of the timing tier itself** - if `meas~` is a noticeable share of `total`, rerun with
  `debug.nvhal.xlate_stats 2` (counts only) before believing a timing-tier percentage.

## Measurement overhead

| tier | per translator entry | measured here |
| --- | --- | --- |
| off | one acquire atomic load + a not-taken branch | 0.5 ns/op (host) |
| 2 (counts) | that, plus 3 relaxed fetch_add | 5.4 ns each = ~16 ns/call |
| 1 (timing) | that, plus two `clock_gettime(CLOCK_MONOTONIC)` | 38.5 ns/pair on the host; measured in the guest at init and printed as `clk=` |

The line's ms exclude the measurement (t0/t1 are taken outside the translated body) while the `cpu`
denominator includes it, so a timing-tier share is a slight **under**-estimate - it cannot manufacture a
>20% reading. The cheap cross-check is fps with the switch off vs on for the same scene.

## Build

Both variants are built from `nvidia-prolapse/hal`; the generator step needs the **mesa** registry, the
system one lacks `vkCmdCopyImageToMemoryKHR` / `vkCmdCopyMemoryToImageKHR` and fails the coverage gate:

    cd nvidia-prolapse/hal
    VKXML=/home/quinovax/waydroid-nv/mesa/src/vulkan/registry/vk.xml

    # (a) plain HAL (guest ICD libEGL_nvidia.so.0) - the glibc-shim root
    make VKXML=$VKXML out/host/vulkan.nvidia.so out/android/vulkan.nvidia.so \
                      out/android32/vulkan.nvidia.so out/host32/vulkan.nvidia.so

    # (b) source-RM HAL (-DNVHAL_SOURCE_RM, guest ICD libvulkan_nouveau.so) - the prolapse root
    python3 ../driver-source/native/mesa/build-hal.py

Artifacts: (a) `hal/out/{host,android,android32,host32}/vulkan.nvidia.so`;
(b) `build/source-native-integration/{host,android,android32}/vulkan.nvidia.so`.

## Deploy (bench-operator / task-7)

The HAL is reached through a directory mount, so a replaced file is picked up by the next process started
in the guest; replace it by **rename**, never in place (a running process may have the old inode mapped):

    # (a) glibc-shim root
    pkexec install -m 644 nvidia-prolapse/hal/out/android/vulkan.nvidia.so \
        /var/lib/waydroid/nvidia-glibc-shim-staging/vendor_extra/lib64/hw/vulkan.nvidia.so.new
    pkexec mv -f /var/lib/waydroid/nvidia-glibc-shim-staging/vendor_extra/lib64/hw/vulkan.nvidia.so.new \
        /var/lib/waydroid/nvidia-glibc-shim-staging/vendor_extra/lib64/hw/vulkan.nvidia.so
    # 32-bit clients, same tree: out/android32 -> .../vendor_extra/lib/hw/vulkan.nvidia.so

    # (b) prolapse root
    pkexec install -m 644 build/source-native-integration/android/vulkan.nvidia.so \
        /var/lib/waydroid/nvidia-prolapse-staging/vendor_extra/lib64/hw/vulkan.nvidia.so.new
    pkexec mv -f /var/lib/waydroid/nvidia-prolapse-staging/vendor_extra/lib64/hw/vulkan.nvidia.so.new \
        /var/lib/waydroid/nvidia-prolapse-staging/vendor_extra/lib64/hw/vulkan.nvidia.so
    # 32-bit: build/source-native-integration/android32 -> .../vendor_extra/lib/hw/vulkan.nvidia.so

The whole recipe is scripted in `DEPLOY-STATS.sh` (task-11): it backs up **all four** live files (sha256 +
size + mtime) into `artifacts/hal-backup-<ts>/<label>/vulkan.nvidia.so`, records them in `TARGETS.tsv`, drops a
self-contained `ROLLBACK.sh` next to them, then does the atomic replace and re-verifies sha256 and the switch
counts (expect 3 and 3):

    bash nvidia-prolapse/hal/DEPLOY-STATS.sh verify              # read-only compare, no pkexec, no writes
    DRY_RUN=1 bash nvidia-prolapse/hal/DEPLOY-STATS.sh deploy    # print the writes, change nothing
    bash nvidia-prolapse/hal/DEPLOY-STATS.sh deploy              # backup -> install .new -> mv -f -> verify
    bash nvidia-prolapse/hal/DEPLOY-STATS.sh rollback artifacts/hal-backup-<ts>   # restore all four files

Each deploy prints its own rollback command; nothing is replaced before the staged `.new` file's sha256 has been
checked against the repo artifact, and the backup is verified against the live file before the first write.

The sanctioned stagers do the same thing for the whole tree (`dev/native-deploy` for the glibc-shim root,
`dev/source-native stage NEWROOT` + `activate` for the prolapse root, which reruns its GPU gate). Note that
the prolapse root carries `source-rm.json`, whose `artifacts` hashes are compared against `build/` by
`dev/source-native adopt` (not by `check`, and not by the LXC generator), so a raw copy leaves that record
stale until the root is restaged. The glibc-shim root keeps no per-artifact hashes.

Then restart the stack with the sanctioned `dev/restart` (the app restart alone suffices for the HAL, a
stack restart is the clean slate), and confirm the patched build is the live one - it is the only build
that can print the first line:

    logcat -s vulkan.nvidia | grep -E 'xlate: statistics ON|xlate stats\['

**Regression gate after deploying** (task-11): before collecting the new numbers, do one 3DMark Steel Nomad Light
run and compare with the known-good glibc-shim run - score ~196, 1.4578 fps, result zip > 10 KB. A zip of 3.5-6 KB
means the workload never produced frames; a score materially below 196 without an explanation means the deployed
HAL is at fault, so restore the previous files immediately with the `rollback` command printed by the deploy run
(or `bash artifacts/hal-backup-<ts>/ROLLBACK.sh`), then re-run `dev/restart`.
