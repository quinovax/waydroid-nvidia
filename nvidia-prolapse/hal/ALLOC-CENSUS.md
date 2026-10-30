# Allocation census (task-13a) and per-draw cost distribution (task-13b)

Both live in `src/gputime.c` and are **off by default**. They do not touch the translation path
(`imgxlate.c`), the ASTC accounting (`astc.c`) or any default behaviour.

## (a) `debug.nvhal.exp.memstats` — where the app's memory actually went

| platform | switch | values | default |
| --- | --- | --- | --- |
| guest (Android) | `debug.nvhal.exp.memstats` | unset/0 = off, >0 = on | off |
| host | `NVHAL_EXP_MEMSTATS` | same | off |
| guest (Android) | `debug.nvhal.exp.memstats.window` | seconds between `@present` lines, min 0.1 | 5.0 |
| host | `NVHAL_EXP_MEMSTATS_WINDOW` | same | 5.0 |

Read **once per process** (`pthread_once`, `ms_init()`) at the first allocation **or the first present**,
so both must be set before the app is started — `setprop` afterwards has no effect on a running process.
When it is on, the module prints one throttled line on the present path and one final line at
`vkDestroyDevice` (if the app ever gets there).

### What is counted

Every allocation the app asks for through this HAL's `vkAllocateMemory` (`nvhal_AllocateMemory`,
ahb.c:961). All three ICD call sites are funnelled through one place, `icd_alloc()` (ahb.c:930):

* ahb.c:990 — an emulated ASTC image's combined allocation
* ahb.c:994 — the plain pass-through (`/* not ours */`) and every AHB import/export
* ahb.c:951 — `import_dmabuf()`, the dedicated dma-buf import (used by both AHB branches)

plus the failures the HAL rejects itself before the ICD is ever called:

* ahb.c:1066 / 1073 / 1085 — AHB export of an unsupported object/format, or a failed
  `AHardwareBuffer_allocate` → OODM (policy, not necessarily an exhausted heap — the line says which)
* ahb.c:1178 — the HAL's own bookkeeping ran out → OOHM

Not counted (they never pass through those sites): the ASTC LUT (`astc.c:667`), a swapchain
realisation (`anb.c:382`) and the gputime query machinery (`gputime.c`) — all internal ICD
allocations that the app never requested.

### The two phases

`nvhal_exp_memstats_present()` is called on every presented image
(`nvhal_QueueSignalReleaseImageANDROID`, anb.c:732). Everything recorded before the first one is the
**preload** phase (the app unpacking its DLC into VRAM), everything after it the **render** phase. If the
process never presents, everything is preload and the line says `first present seen: no`.

### The line (shared body, two markers)

    alloc census @present #37 t=63.4 s: 5 ok (75.0 MiB: 72.0 MiB before the first present, 3.0 MiB after), OUT_OF_DEVICE_MEMORY 3, OUT_OF_HOST_MEMORY 0; type 0: pre 1 ok 64.0 MiB (OODM 2 OOHM 0), post 0 ok 0.0 MiB (OODM 0 OOHM 0); type 1: pre 0 ok 0.0 MiB (OODM 0 OOHM 0), post 1 ok 2.0 MiB (OODM 0 OOHM 0); type 2: pre 2 ok 8.0 MiB (OODM 0 OOHM 0), post 1 ok 1.0 MiB (OODM 0 OOHM 0); type 3: pre 0 ok 0.0 MiB (OODM 0 OOHM 0), post 0 ok 0.0 MiB (OODM 1 OOHM 0); first present seen: yes

    alloc census @final (vkDestroyDevice): 5 ok (75.0 MiB: ...); first present seen: yes

### The throttled `@present` line (task-16)

3DMark ends on a result screen and never destroys its `VkDevice`, so the old single teardown line never
appeared. The present path now prints the **same body** at most once per window:

* marker `alloc census @present #<n> t=<s>:` — `#<n>` counts presented images since the first probe
  (7 presents may produce line #1, #5, #7), `t` is seconds of the monotonic clock since the module's
  first probe (≈ app start; the allocations that matter happen during `vkCreateDevice`), so a line can
  be aligned with `dmon`/`gpu.csv` by the logcat timestamp and `t` together;
* the **first** present after the switch is on always prints (the gate starts at 0), then the gate is
  `last_print + window`; the window is 5.0 s by default and `debug.nvhal.exp.memstats.window` overrides
  it (clamped to ≥0.1 s so a typo cannot flood logcat). A line is emitted by **at most one thread**:
  the gate is a `__atomic_compare_exchange` on the last-print timestamp;
* printing **never resets** the census — successive lines grow (preload totals stay, render-phase
  allocations accumulate in `post`), which is exactly what makes them a timeline;
* the teardown line keeps working and is marked `@final`; a second teardown after counters were
  cleared reports zeros with `first present seen: no`.

* **ok** = the ICD returned `VK_SUCCESS`; bytes are the requested `allocationSize` (the ICD may round
  up), OODM/OOHM = the count of `VK_ERROR_OUT_OF_DEVICE_MEMORY`/`..._HOST_MEMORY` results.
  Any other result is not counted.
* One `type N:` group per memory type that saw any activity (silent when idle), with the pre/post
  split inside. `pre`/`post` for **ok** is count + MiB; for failures it is OODM/OOHM counts.
* The type numbers are the app's `memoryTypeIndex` values, i.e. the native NVIDIA enumeration
  (0 = HV|HC system memory, 2 = DEVICE_LOCAL, ...). `debug.nvhal.exp.memtypes 1` prints the table
  (see `MEMTYPE-EXPERIMENT.md`) so the numbers can be matched to flags.
* The counters are **reset by the teardown** (`ms_snapshot(..., clear=1)`), so a device recreated later
  starts a clean census; the `@present` marker keeps counting presents across the whole process.

### How to read it against the 8105/8192 MiB observation

* `type 2: pre ... (~7.2 GB)` → the preload really did fill VRAM; `post` allocations landing in
  type 0/1 (host memory) is then the PCIe path for the render phase.
* non-zero `OODM` in a device-local type is the direct "this type is exhausted" evidence.
* `post` bytes in a device-local type would instead mean there was still room.

## (b) per-draw timestamps and `DRAW-DIST`

**Answer to "does level 2 timestamp every draw?"** — Yes, but only for an inline render pass on a
*primary* command buffer at `debug.nvhal.gputime >= 2`: `after_draw()` (gputime.c:857) returns early at
gputime.c:868 for `!c->in_rp` (outside a render pass), `c->g->level < 2` and `c->rp_secondary`
(secondary command buffers only contribute their draw count, folded into the primary's render pass).
Each remaining draw gets one BOTTOM_OF_PIPE timestamp pair, so the per-draw data **was** already being
collected.

**What was missing:** the report only aggregated by key (render pass / pipeline) — `total, count, avg,
max`, top 45 by total time (`report()`, gputime.c:541). No min/median/p90 and no per-render-pass
breakdown, so "every draw costs ~168 us" and "a few draws are very expensive" looked the same.

`rpd_report()` (gputime.c:498, called from report() at gputime.c:606) now adds, per render pass
instance, lines like

    DRAW-DIST rp=00000000000000c3 n=1820/frame avg=168.4 us min=12.1 p50=160-176us p90=240-256us max=1934.0 us  hist[0-16us:4 16-32us:11 144-160us:610 160-176us:702 176-192us:420 240-256us:71 >65536us:1]

* keyed by the same aggregation key as the render pass row (one formula, `rp_key()`, gputime.c:386),
  printed as the `rp=<handle>` in hex so it matches the `GK_RP` line;
* `n` is per frame, `min/p50/p90/max` in us; p50/p90 are the bucket labels that contain the
  percentile, the histogram has 32 linear 16 us buckets up to 512 us plus 8 logarithmic ones
  (`:47-49` `RPD_HB`); at most 10 non-empty buckets are printed per line;
* up to 16 render passes per window (ordered by draw count, the rest are summarised as
  `DRAW-DIST: N further render pass(es)...`), and the table is reset with the window;
* a **uniform** histogram with p90 close to p50 means per-draw serialisation; a long right tail
  (like the `>65536us` bucket above) means one or a few draws dominate.

Both features need the level-2 timing, so `setprop debug.nvhal.gputime 2` (plus optionally
`debug.nvhal.gputime.window`) must be set before the app starts.

## Build (2026-10-25 21:07, all four targets + the source-RM variant)

    cd nvidia-prolapse/hal
    make VKXML=/home/quinovax/waydroid-nv/mesa/src/vulkan/registry/vk.xml \
         out/host/vulkan.nvidia.so out/android/vulkan.nvidia.so \
         out/android32/vulkan.nvidia.so out/host32/vulkan.nvidia.so      # exit 0
    python3 nvidia-prolapse/driver-source/native/mesa/build-hal.py         # exit 0 (-DNVHAL_SOURCE_RM)

`-Wall -Wextra -Werror` clean; `VKXML` must point at the mesa registry (the system one lacks
`vkCmdCopyImageToMemoryKHR`; see `XLATE-STATS.md`).

| artifact | bytes | sha256 | ICD it must be |
| --- | --- | --- | --- |
| hal/out/android/vulkan.nvidia.so | 2453040 | b2e11a308057710f65bccd6fc1951ec2a106974b2f712b9d70a78cdc2945ad2e | libEGL_nvidia.so.0 (glibc-shim) |
| hal/out/android32/vulkan.nvidia.so | 2246424 | f4012067525ff63466dc78166ce186242f7f38c40f10827c8cb8d9720218379f | libEGL_nvidia.so.0 |
| build/source-native-integration/android/vulkan.nvidia.so | 2441160 | c5c1d19d8832ab0a5b51c4bbfde389d321c66c14ebd96c0c5523612f5b4454b6 | libvulkan_nouveau.so (prolapse) |
| build/source-native-integration/android32/vulkan.nvidia.so | 2237440 | 6b0ba02e8f96d94f2378f3bed9e5da1f5e60803767a1413be0cc4f2ad0487157 | libvulkan_nouveau.so |
| hal/out/host/vulkan.nvidia.so | 2736880 | e1fa77eff574341548f05a69ac4bfe954041116d168083546bb90c281b9a84b2 | (host harness) |
| build/source-native-integration/host/vulkan.nvidia.so | 2720712 | d7eacb8f8886b06bd0b32469bb1c54f85845b867c7b22e0fcd8f05752c834b27 | |
| hal/out/host32/vulkan.nvidia.so | 2660916 | f9340f3f3ce0a19f7fff0f953afb9c670839e6a558ff1641c03bf16989a6f4e7 | |

`strings` acceptance (all four android/android32 artifacts, identical counts):
`debug.nvhal.xlate_stats` x3, `debug.nvhal.astc_bytes` x3, `debug.nvhal.exp.memtypes` x1,
`debug.nvhal.exp.memstats` x2, `debug.nvhal.exp.memstats.window` x1,
`debug.nvhal.exp.memtype_target` x3, `debug.nvhal.exp.hostvis_cached` x3,
`alloc census @present` x1, `alloc census @final` x1, `DRAW-DIST` x2, `EXPERIMENT memory table` x1.
ICD ownership is still `out/android*` -> `libEGL_nvidia.so.0` and
`build/source-native-integration/android*` -> `libvulkan_nouveau.so`.

The `@present`/`@final` markers and `debug.nvhal.exp.memstats.window` are what `DEPLOY-STATS.sh` looks
for (`EXPECT_MEMSTATS=3`, `EXPECT_PRESENT=1`); an older artifact fails its preflight with
`has no 'alloc census @present' marker: not the task-16 build`.

**One build lesson (task-16):** clang **rejects** `__atomic_*()` builtins whose argument is an
`_Atomic`-qualified object (error: address argument to atomic operation must be a pointer to integer ...),
while gcc accepts them - so the host build succeeded and only the four android/android32 targets failed.
The counters are therefore plain `uint64_t` accessed through `__atomic_*` (the form ahb.c already used).
Build `out/android` explicitly before trusting a `make` whose tail shows only the last target.

## Deploy + run (bench-operator)

`nvidia-prolapse/hal/DEPLOY-STATS.sh` carries all of this: it now also preflights
`debug.nvhal.exp.memstats` and expects exactly 2 occurrences.

    bash nvidia-prolapse/hal/DEPLOY-STATS.sh verify
    bash nvidia-prolapse/hal/DEPLOY-STATS.sh deploy          # backup + install .new + mv -f, prints rollback
    # then the sanctioned dev/restart (bench-operator), then in the guest before starting the app:
    setprop debug.nvhal.exp.memstats 1
    setprop debug.nvhal.exp.memtypes 1
    setprop debug.nvhal.gputime 2        # only for DRAW-DIST / per-draw timings
    # collect:
    logcat -s vulkan.nvidia | grep -E 'alloc census|DRAW-DIST|EXPERIMENT'

## Overhead

* off (default): one acquire load per allocation and one per presented image (`ms_on()`); no clock
  read, no lock, no allocation, no formatting. `rpd_add()` is not reached without level-2 timing, and
  level 2 was already paying for the timestamps.
* on: one mutex-protected counter update per allocation; per presented image one relaxed counter
  increment, one `clock_gettime(CLOCK_MONOTONIC)` (~20-40 ns) and one timestamp comparison, plus the
  body build — and only when the window has elapsed; `DRAW-DIST` adds one hashtable update per timed
  draw (already timed) and up to 16 lines per window.

## Verification (host, no GPU)

The census module and the bounded appender are extracted **verbatim** from `src/gputime.c` and compiled
into a host test with a log-capture stub and a **replaced `clock_gettime`** (a fake monotonic clock, so
the throttle is tested without sleeping):

    cd nvidia-prolapse/hal
    awk '/^static int exp_buf\(/{f=1} f{print} f&&/^\}$/{exit}' src/gputime.c > /tmp/ms2/exp_body.c
    awk '/----- allocation census/{f=1} f' src/gputime.c > /tmp/ms2/census_body.c
    gcc -std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -I/tmp/ms2 /tmp/ms2/test.c -o /tmp/ms2/ms_test -lpthread

22 checks, all four modes pass (`off`, `win5`, `win1`, `clamp`):

| mode | evidence |
| --- | --- |
| `off` (`NVHAL_EXP_MEMSTATS` unset) | alloc + fail + present + teardown produce **0** log lines |
| `win5` (default window) | banner says `window 5.00 s`; present #1 prints `@present #1 t=0.0 s`; presents #2-#4 up to t=4.9 add nothing; #5 at t=5.1 prints; #6 at 6.0 is throttled; #7 at 10.2 prints — **7 presents, 3 lines** |
| `win5` bodies | line 1 totals `3 ok (5.0 MiB: 5.0 MiB before…)` with `type 0: pre 2 ok 2.0 MiB (OODM 1 OOHM 0)`, `type 2: pre 0 ok 0.0 MiB (OODM 0 OOHM 1)`; after a 2 MiB post-present allocation line 3 reports `4 ok (7.0 MiB: 5.0 MiB before the first present, 2.0 MiB after)` — the census survives printing |
| `win5` teardown | `@final (vkDestroyDevice)` with the same body; a second teardown reports `0 ok (0.0 MiB…)` and `first present seen: no` |
| `win1` (`…window=1`) | banner `window 1.00 s`; presents at t=0.0/0.5/1.0 print #1 and #3 only |
| `clamp` (`…window=0.01`) | banner `window 0.10 s` (the ≥0.1 s clamp) |

The four modes were re-run against the final source text (22 `ok`, 0 failures), and the extracted file was
checked to contain the shipped declaration (`grep -c 'uint64_t ms_present_n'` = 1).
