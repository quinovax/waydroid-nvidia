# Memory-type table + memory-type intervention experiment (task-12, extended by task-14)

Purpose: without any GPU-side program, learn what `memoryTypeIndex 0/1/2/3` actually are on this
machine, and offer three ways to move the app's buffers between system memory, BAR1 and VRAM so a
Run B can test each mechanism one at a time.

Everything lives in `nvidia-prolapse/hal/src/gputime.c` (`nvhal_exp_mem_init` and the `er_*`
summary) plus a two-line allocation hook in `nvidia-prolapse/hal/src/ahb.c` and one teardown call
in `nvidia-prolapse/hal/src/device.c`; every switch defaults to off (the only cost when off is one
`getenv`/`__system_property_get` per process and one load per allocation).

## The four switches

All are read **once per process** when `vkCreateDevice` runs
(`nvidia-prolapse/hal/src/device.c:751` calls `nvhal_exp_mem_init()`). Set them in the guest
**before** starting the app, exactly like the other HAL switches. Priority when several are set:
**`exp.memtype_target` > `exp.hostvis_cached` > `exp.hostvis_to_bar`** (only one remap mechanism is
ever active, so a run cannot mix them).

| Guest property | Host env var | Meaning |
| --- | --- | --- |
| `debug.nvhal.exp.memtypes` | `NVHAL_EXP_MEMTYPES` | `1` = print the memory-type/heap table once (read-only, no remap) |
| `debug.nvhal.exp.memtype_target` | `NVHAL_EXP_MEMTYPE_TARGET` | `-1`/unset = off; `>=0` = send **every** type that lacks `DEVICE_LOCAL` to exactly that index (out-of-range values are warned about and ignored) |
| `debug.nvhal.exp.hostvis_cached` | `NVHAL_EXP_HOSTVIS_CACHED` | `0`/unset = off; `1` = send `HOST_VISIBLE|HOST_COHERENT` (not `HOST_CACHED`) types to the `HOST_VISIBLE|HOST_CACHED` type of the **same heap** |
| `debug.nvhal.exp.hostvis_to_bar` | `NVHAL_EXP_HOSTVIS_TO_BAR` | `0`/unset = off; `1` = original narrow remap; `2` (any value >1 is clamped to 2) = remap every type that lacks `DEVICE_LOCAL` to the closest `DEVICE_LOCAL` type |

Setting any of them also prints the table (so a remap run is self-documenting).

### The table line

```
EXPERIMENT memory table: memoryTypeCount=4 heapCount=2; type[0]=HOST_VISIBLE|HOST_COHERENT 0x6 heap=0;
type[1]=HOST_VISIBLE|HOST_CACHED 0xa heap=0; type[2]=DEVICE_LOCAL 0x1 heap=1;
type[3]=DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT 0x7 heap=1; heap[0]=none 0x0 32768 MiB;
heap[1]=DEVICE_LOCAL 0x1 256 MiB; total=33024 MiB
```

`total` is the sum of all heap sizes the ICD reports. One line, buffer-bounded at 2048 bytes
(tail is replaced by ` ...(truncated)` if a driver ever reports a pathological number of types).

### The remap rule

* level **1** (unchanged from before): a type is remapped only if it is `HOST_VISIBLE`, **not**
  `HOST_CACHED`, **not** `DEVICE_LOCAL`, and some other type has flags exactly `flags | DEVICE_LOCAL`.
* level **2**: every type without `DEVICE_LOCAL` is remapped to the *closest* type that has it —
  exact `flags | DEVICE_LOCAL` match first, otherwise the candidate sharing the most flag bits
  (`__builtin_popcount(g & f)`, lowest index wins ties). If no candidate exists the type is left alone.
* Types carrying `LAZILY_ALLOCATED` or `PROTECTED` are never remapped (those bits are semantic).
* Only **plain** allocations are remapped. `nvhal_exp_mem_remap()` (`gputime.c:1333`) walks the
  pNext chain and returns the app's own type unless every structure is one of
  `VkMemoryAllocateFlagsInfo`, `VkMemoryPriorityAllocateInfoEXT`, `VkMemoryDedicatedAllocateInfo`,
  `VkMemoryOpaqueCaptureAddressAllocateInfo` — i.e. imports/exports (AHB, dma-buf) are untouched.

Per-type mapping lines:

```
EXPERIMENT debug.nvhal.exp.hostvis_to_bar=2: memory type 0 (HOST_VISIBLE|HOST_COHERENT 0x6 heap 0) is
allocated from type 3 (DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT 0x7 heap 1, heap 256 MiB)
```

### The explicit target and the cached preference (task-14)

`exp.memtype_target=N` ignores flags entirely and points every non-`DEVICE_LOCAL` type at type `N`,
whatever `N` is — that is what makes the three Run B mechanisms separable:

* `N` = a plain `DEVICE_LOCAL` type -> only real VRAM (heap 1, ~246 MiB on this Pascal card);
* `N` = a `DEVICE_LOCAL|HOST_VISIBLE` type -> BAR1 (map-able VRAM window, same ~256 MiB ceiling);
* `N` = a host type (e.g. the `HOST_CACHED` one) -> cached system memory instead of uncached PCIe reads.

`exp.hostvis_cached=1` is the narrow version of the last one: it only touches map-able uncached
system types and keeps them map-able. It never crosses heaps, so the `DEVICE_LOCAL` heap cannot fill
up because of it. If the heap has no `HOST_VISIBLE|HOST_CACHED` type, that type keeps the app's
choice and one `EXPERIMENT ... has no HOST_VISIBLE|HOST_CACHED type in heap N` line says so.

Both new switches print the same per-type mapping line (now naming the switch that fired) and, at
`vkDestroyDevice`, one line with the **per-allocation** `src -> dst` count and bytes — only
allocations the ICD actually accepted are counted (`VkResult` other than `VK_SUCCESS` is dropped),
and AHB imports/exports never appear because the pNext whitelist keeps them out of the remap:

```
EXPERIMENT memory remap summary at vkDestroyDevice: type 0 -> type 2
[debug.nvhal.exp.memtype_target]: 812 alloc(s) 3072.0 MiB; type 1 -> type 2 [debug.nvhal.exp.memtype_target]: 4 alloc(s) 16.0 MiB
```

(shown wrapped here; the real line is one `NVHAL_WARN` record, bounded at 1024 bytes). The count is
per source type and the pairs are keyed by source type, so the numbers add up to the total number of
remapped allocations. Matching against the `alloc census` (`debug.nvhal.exp.memstats`) of the same
run gives "of all allocations, how many were redirected, and to where".

and a single summary line for level 2:

```
EXPERIMENT debug.nvhal.exp.hostvis_to_bar=2: 2 non-device-local type(s) now point at VRAM type(s),
covering 256 MiB of heap; on Pascal the device-local window is usually ~256 MiB, so allocations that
big fail with VK_ERROR_OUT_OF_DEVICE_MEMORY
```

## How to run (bench-operator; not done here)

```sh
# 1. put the task-12 artifacts in place (same .so files as the xlate/astc instruments)
bash nvidia-prolapse/hal/DEPLOY-STATS.sh deploy      # now also preflights debug.nvhal.exp.memtypes
bash dev/restart
# 2. in the guest, before launching 3DMark (properties are read once at vkCreateDevice)
suspend ... # n/a: from the host
sudo -n lxc-attach -P /var/lib/waydroid/lxc -n waydroid -- /system/bin/sh -c \
  'setprop debug.nvhal.exp.memtypes 1'
# 3. run the benchmark, then collect
logcat -s vulkan.nvidia | grep EXPERIMENT
```

**Run A (read-only, do this first):** `debug.nvhal.exp.memtypes 1` alone — it answers "what is
type 0/1/2/3 here" and "how big is each heap" without touching a single allocation.

**Run B (one mechanism per run, each with its own `dev/restart`):**

| mechanism | setprop (before the app starts) | what it proves |
| --- | --- | --- |
| pure VRAM | `debug.nvhal.exp.memtype_target <plain DEVICE_LOCAL type>` | whether the app's buffers fit/behave in real VRAM at all |
| BAR1 | `debug.nvhal.exp.memtype_target <DEVICE_LOCAL\|HOST_VISIBLE type>` | whether the map-able VRAM window (~246 MiB here) is the ceiling |
| cached sysmem | `debug.nvhal.exp.memtype_target <HOST_CACHED type>` or `debug.nvhal.exp.hostvis_cached 1` | cached vs uncached PCIe reads for the same buffers |

Every Run B ends with the `EXPERIMENT memory remap summary at vkDestroyDevice` line: if it is
missing or shows only a handful of allocations, the app's big buffers never went through
`vkAllocateMemory` with a remappable type (imports/export are excluded by design) and the run says
nothing about that mechanism. A 3DMark that dies with `VK_ERROR_OUT_OF_DEVICE_MEMORY` (or a much
worse score) is a valid result: it means the target type does not fit Pascal's ~256 MiB window.
Set only **one** of the four switches per run — the priority order above is a safety net, not a
feature to lean on.

## Mirror-test evidence (no GPU involved)

The seven new functions were `sed`-extracted **verbatim** from `src/gputime.c` into a host test
program with a stub `nvhal_log()`, compiled with the same flags as the HAL, and run against a table
shaped like the NVIDIA Linux ICD (type0=HV|HC 0x6 heap0, type1=HV|HOST_CACHED 0xa heap0,
type2=DL 0x1 heap1, type3=DL|HV|HC 0x7 heap1; heap0 32 GiB, heap1 256 MiB):

| switch | logs | `mem_remap` |
| --- | --- | --- |
| none | 0 lines | `[0 0 0 0]` (off = silent, default path untouched) |
| `exp.memtypes=1` | 1 (table only) | `[0 0 0 0]` |
| `hostvis_to_bar=1` | 2 (table + 1 mapping) | `[4 0 0 0]` → type 0 sends to type 3 |
| `hostvis_to_bar=2` | 4 (table + 2 mappings + summary) | `[4 4 0 0]` |
| `hostvis_to_bar=9` | 4 (clamped to 2) | `[4 4 0 0]` |
| `memtype_target=2` | 4 (table + 2 mappings + summary) | `[3 3 0 0]` → both host types send to type 2 |
| `memtype_target=0` | 3 (table + skip warning + summary) | `[0 1 0 0]` → the target itself is left alone |
| `memtype_target=9` | 1 (warning only, mode stays off) | `[0 0 0 0]` |
| `memtype_target=9` + `memtypes=1` | 2 (warning + table) | `[0 0 0 0]` |
| `hostvis_cached=1` | 3 (table + 1 mapping + summary) | `[2 0 0 0]` → type 0 sends to the same-heap HOST_CACHED type 1 |
| `hostvis_cached=1`, HOST_CACHED type in another heap | 2 (table + "no HOST_CACHED type" warning) | `[0 0 0 0]` |
| `memtype_target=3` + `hostvis_to_bar=2` | target wins, no `hostvis_to_bar` line | `[4 4 0 0]` |

`mem_remap[i]` is `dst_index + 1`, `0` = untouched. The same test also drives the summary path:
with `memtype_target=2`, two accepted 1 MiB allocations of source type 0, one rejected allocation, one
allocation of an un-remapped type and one out-of-range index produce exactly
`type 0 -> type 2 [debug.nvhal.exp.memtype_target]: 2 alloc(s) 2.0 MiB`, a second `teardown` prints
nothing, and a fresh allocation after a teardown starts the counter at 1 again.

**Bug the mirror test caught:** the first version of the summary kept the previous run's `n`/`bytes`
in a reused slot (`er_pairs` is static, `er_npairs` is reset by `nvhal_exp_mem_init`), so a second
device in the same process reported "3 alloc(s)" instead of "1". The fix zeroes `n`/`bytes` when a
pair slot is created; the regression is one of the 31 checks.

**Correction to the earlier hypothesis:** level 1 *does* match a plain `HOST_VISIBLE|HOST_COHERENT`
type (`0x6` has no `HOST_CACHED` bit — that is `0x8`, not `0x4`). So "we never saw an EXPERIMENT
line" is **not** explained by "type 0 is HV|HC". The plausible causes are: the switch was not set for
that process, or the buffer was allocated through an import (excluded by the pNext whitelist).
The read-only `exp.memtypes=1` table removes the guesswork.

## Build

```sh
cd nvidia-prolapse/hal
make VKXML=/home/quinovax/waydroid-nv/mesa/src/vulkan/registry/vk.xml \
     out/host/vulkan.nvidia.so out/android/vulkan.nvidia.so \
     out/android32/vulkan.nvidia.so out/host32/vulkan.nvidia.so
python3 nvidia-prolapse/driver-source/native/mesa/build-hal.py   # -DNVHAL_SOURCE_RM variant
```

(Only the second command produces the prolapse-root binary. Point `VKXML` at the mesa registry or
the generated intercept table fails on `vkCmdCopyImageToMemoryKHR`.)

Artifacts that belong to each root: `out/android*` dlopens `libEGL_nvidia.so.0` (glibc-shim);
`build/source-native-integration/android*` dlopens `libvulkan_nouveau.so` (prolapse).
