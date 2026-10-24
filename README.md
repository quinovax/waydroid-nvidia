# waydroid-nvidia

**GPU-accelerated Waydroid on the NVIDIA driver — container-native, no VM.
Works with the proprietary NVIDIA kernel module; the open kernel module
(`nvidia-open`) is optional and has no advantage for DMA-BUF on this stack.**

[![build](https://github.com/CinQwQeggs01/waydroid-nvidia/actions/workflows/build.yml/badge.svg)](https://github.com/CinQwQeggs01/waydroid-nvidia/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/CinQwQeggs01/waydroid-nvidia)](https://github.com/CinQwQeggs01/waydroid-nvidia/releases)

## Branches: which one to use

| Branch | What it is | Who it is for |
|---|---|---|
| `main` | The Venus stack, released and stable. Vulkan is proxied over a unix socket to a host-side renderer (Mesa Venus → `virgl_test_server`). | Anyone on a supported GPU who wants the shipped behaviour. |
| `dev` | The next-generation stack, in testing. Adds three driver modes (`nvidia-prolapse`, `glibc-shim`, `venus`) that run the NVIDIA userspace inside the container instead of proxying Vulkan, plus the build and packaging work needed for them. | Testers, and maintainers of pre-GSP cards. |

`dev` is where this fork's maintenance now happens. It is a superset of `main`: it keeps the Venus
mode (so `main`'s behaviour remains available as one of its three modes) and adds the other two.
`main` stays the released line; fixes that matter for the Venus path land on `dev` first and are
carried back to `main` with the next release.

### What `dev` is maintained for

- **Pre-GSP NVIDIA cards, GTX 10 and GTX 9 series first.** Pascal (GTX 10xx) and Maxwell 2
  (GTX 9xx) are the target: they are old enough that NVIDIA's own userspace assumes GSP firmware
  and newer interfaces the hardware does not have, so running it inside the container needs the
  compatibility work that `dev` carries. `nvidia-prolapse` is the mode built for them; `glibc-shim`
  is the fallback that loads the host's unmodified glibc-built NVIDIA libraries into the bionic
  guest. See the per-mode table on `dev`'s README for the current status of each card.
- **Build and packaging fixes, Fedora first.** A large share of `dev`'s commits are build-system
  work: locale-independent tool invocation (`readelf` output is translated under a non-English
  locale), host library-directory layout (`/usr/lib64` + `/usr/lib` on Fedora multilib versus
  `/usr/lib` + `/usr/lib32` on Debian and Arch), missing `-devel` packages, and the AppArmor
  difference in Fedora's `liblxc`. These are what make `packaging/native/build-native.sh` and the
  component checks run on a stock Fedora host instead of only in the pinned container image.
- **Upstream tracking.** `dev` merges every upstream `public-test` release (currently v0.2.0-rc7)
  and keeps this fork's own transport work on top, so the compatibility work above is always
  tested against current upstream rather than a frozen snapshot.
- **Upstream fixes, more aggressively than upstream itself.** Following a release is the floor,
  not the goal. Where this fork hits a problem upstream has not solved — or has solved only by
  working around it — the fix is taken to the root cause and offered back upstream as a pull
  request, together with the minimal reproducible check that proves it. The intent is to stop
  carrying private patches for anything upstream can accept, so this work outlives the fork.
  See [Upstream contributions](#upstream-contributions) below for what that has produced.

### Upstream contributions

Maintaining pre-GSP cards means running the NVIDIA userspace in a place the vendor never
intended, so most of what `dev` learns is a bug report upstream cannot reproduce on an RTX
card. Three kinds of work go back upstream, and the fork's own carry-list shrinks as they land:

- **Root-cause fixes instead of workarounds.** Upstream's `vtest_gpu_alloc` chose NVIDIA's own
  LINEAR memory for CPU-readable buffers. That path faults the GPU (Xid 69, `CLEAR_SURFACE`
  `0x19D0`) when SurfaceFlinger renders into it, so upstream's answer was an environment variable
  to opt out. This fork makes the safe path (kernel `udmabuf`) the default and NVIDIA LINEAR the
  opt-in, on the reasoning that the failure mode decides: a GPU fault that kills the compositor
  is strictly worse than a possibly-imperfect render.
- **Gaps the import census cannot see.** The glibc shim exports only the names the NVIDIA blobs
  import, which is correct until a *loader* resolves something by glibc name that no blob does —
  `libglvnd` wants `pthread_join@GLIBC_2.2.5`, and without it every EGL/Vulkan client aborts in
  `hal_open`. The fix is a `runtime=` row in `dispositions.tsv`, and the same class of gap is now
  checked rather than discovered by a crash.
- **Distribution portability.** Several upstream paths only work on the distribution they were
  written on: `readelf` field names are translated under a non-English locale and every checker
  matched the English form; host library directories are `/usr/lib64` + `/usr/lib` on Fedora
  multilib but `/usr/lib` + `/usr/lib32` on Debian and Arch; Fedora's `liblxc` is built without
  AppArmor and rejects the stock `lxc.apparmor.profile` key outright. These become conditional
  implementations that work on both layouts, not assumptions about one distribution.

Reproducible evidence comes before the patch in each case: a minimal EGL/Vulkan probe built with
the NDK and run inside the container with the shim's own trace enabled (this is how the missing
`pthread_join` was found — the loader named the symbol and the version it wanted), a gate that
fails on the old behaviour and passes on the new, or a before/after log from this GTX 1080.
Where a fix is small enough that upstream can take it directly, that is preferred over another
fork-only patch; the aim is that a future upstream release makes more of this branch redundant,
not less. The work above is currently described from this fork's side — the findings are proven
here and the fixes are in this branch, while the corresponding upstream pull requests are being
prepared rather than merged. Anything already accepted upstream is folded in through the
`public-test` merges above and noted in the release history.

### Trying `dev`

```
git clone -b dev https://github.com/quinovax/waydroid-nvidia
git clone -b dev ssh://git@ssh.github.com:443/quinovax/waydroid-nvidia.git   # if you have access
```

`dev` is not a drop-in replacement for a `main` install: it changes the container configuration
(`nvidia_driver_mode`), stages its own vendor tree under `/var/lib/waydroid`, and can switch back.
Read `docs/dev-workflow.md` and the branch's own README before switching, and keep a `main` install
around if you need the released behaviour.

## Highlights

- **Proprietary NVIDIA driver support** — `nvidia.ko` (closed-source) works alongside `nvidia-open`; no DMA-BUF difference between the two on this stack.
- **Rendering fixes** — Present fence leaks patched (UE4 `VK_ERROR_TOO_MANY_OBJECTS`), ASTC texture emulation for desktop NVIDIA, DRM modifier negotiation.
- **Guest customization** — `scripts/waydroid-guest-customize.sh` for Magisk/Zygisk/Shamiko, WebView GL override, mouse fix, device spoof.
- **Container-native** — LXC with host kernel, DMA-BUF pass-through, udmabuf zero-copy to compositor.

Stock Waydroid can't render on NVIDIA. This project proxies Vulkan (Mesa
Venus) over a unix socket to a host-side renderer that issues the real Vulkan
calls — Android x86 and x86_64 app processes render on your NVIDIA GPU,
CUDA/NVENC and full performance stay intact, everything remains a container:

```
Android app ── Vulkan ──▶ guest Mesa Venus ── unix socket ──▶ host renderer
                                                                   │
KWin ◀── hwcomposer ◀── gralloc imports ◀── NVIDIA dmabufs ◀── NVIDIA driver
```

Buffers are allocated host-side as NVIDIA block-linear dmabufs and imported
by the compositor directly — no CPU copies. GL runs through ANGLE, ASTC
textures are emulated in a compute shader (desktop NVIDIA lacks the hardware
Android expects), and frame sync uses timeline syncobjs with imported sync_fd
semaphores. ARM-only apps run via libhoudini translation.

Tested on Pascal (GTX 1080) with the proprietary driver.

## Requirements

- **NVIDIA kernel module** (`nvidia.ko` / `nvidia-dkms` or `nvidia-open` / `nvidia-open-dkms`) — either the proprietary or open module works. The proprietary module is the standard one shipped with `nvidia-utils`. Pascal and newer GPUs.
- Driver **535+** with `nvidia-drm.modeset=1` (610.x recommended).
- A Wayland session (tested on KWin / Plasma 6) and the usual Waydroid
  kernel bits (binder).
- Unsure about a machine? `tests/run-probe.sh` checks the exact
  buffer-sharing paths in ~30 s and names anything missing — see
  [`docs/troubleshooting.md`](docs/troubleshooting.md).

`waydroid-nvidia-setup` deploys the guest stack, writes the config, verifies
your environment (modeset, vendor image, render node) and removes stale
config left by older installs. Safe to re-run any time — it's also the first
thing to try when something misbehaves. Verify acceleration:

```sh
sudo waydroid shell dumpsys SurfaceFlinger | grep GLES
# GLES: ... ANGLE (NVIDIA, Vulkan ... Venus (NVIDIA GeForce ...))
sudo waydroid shell getprop ro.product.cpu.abilist
# x86_64,x86,... means Houdini can translate ARM32-only apps into the x86 path
```

**Hybrid GPUs** (laptop panel on Intel/AMD, NVIDIA as the render GPU): the
iGPU compositor cannot import NVIDIA **block-linear** dmabufs, which is why
the Waydroid window used to die instantly. The host allocator now detects
that topology and exports **NVIDIA LINEAR** presentation buffers instead
(Intel/AMD can import those as `GL_TEXTURE_2D`; see `tests/crossimport.c`).
Discrete NVIDIA (this is the compositor GPU) still uses block-linear.

Check which path was chosen:

```sh
journalctl --user -u wd-venus --since '2 min ago' --no-pager | grep vtest_gpu_alloc
# hybrid:   present=nvidia-linear  ... path=nvidia-linear modifier=0x0
# discrete: present=block-linear   ... path=block-linear  modifier=0x03...
```

Override if auto-detect is wrong (`linear` on an iGPU-compositor box,
`block-linear` when KWin/Mutter is on NVIDIA):

```sh
systemctl --user edit wd-venus.service
# [Service]
# Environment=WAYDROID_NVIDIA_PRESENT=linear
systemctl --user daemon-reload && systemctl --user restart wd-venus.service
```

Nesting Waydroid in gamescope pinned to NVIDIA is **not** the hybrid path —
it still crashes and can take the host session with it (gamescope#1590).

### Install from the latest release (recommended)

```sh
curl -fsSL https://raw.githubusercontent.com/CinQwQeggs01/waydroid-nvidia/main/packaging/install-from-release.sh | sudo bash
# or, from a checkout, without sudo:
#   ./pkexec-install.sh
```

This auto-detects your distro, installs dependencies, downloads the latest
release tarballs, verifies SHA256 checksums, patches waydroid, and installs
everything — host binaries, guest stack, systemd units, udev rules, and
SELinux policy (rpm family).

To install a specific release:

```sh
curl -fsSL https://raw.githubusercontent.com/CinQwQeggs01/waydroid-nvidia/main/packaging/install-from-release.sh | sudo bash -s -- --tag v0.1.1
```

### Install from a tag (build from source)

When a tag exists but no release has been published yet (pre-built tarballs
are uploaded manually and may lag behind tags), you can build from source:

```sh
git clone https://github.com/CinQwQeggs01/waydroid-nvidia.git
cd waydroid-nvidia
sudo ./packaging/install-from-release.sh --source --tag v0.1.0
```

This installs build dependencies, downloads the Android NDK, clones and
patches all upstream trees, builds mesa (guest Venus driver) and
virglrenderer (host renderer) from source, and installs everything.

**Note:** ANGLE, hwcomposer, and surfaceflinger are not built by
`--source`. The installer fetches the `guest-prebuilts` tarball from the
same tag when it exists, and otherwise from this repository's latest
release that ships them (currently `v0.1.1`). `waydroid-nvidia-setup`
requires those files — a stock install without them cannot pass payload
validation.

### After installing (both methods)

```sh
waydroid init -s GAPPS
# Fedora / other rpm-family waydroid 1.6 packages need explicit OTA URLs:
#   pkexec waydroid init -f -c https://ota.waydro.id/system -v https://ota.waydro.id/vendor
pkexec waydroid-nvidia-setup
pkexec systemctl enable --now waydroid-container.service
# re-login once, then:
waydroid session start
```

The patched session starts and stops `wd-venus.service` with itself. Do
**not** `systemctl --user enable` it: leftover vtest clients after a session
restart block the next SurfaceFlinger connect (listen Recv-Q on `venus.sock`).

**Manual install / other distros:** see
[`docs/install-manual.md`](docs/install-manual.md).

### Build from source (Fedora, advanced)


## Documentation

- [`scripts/waydroid-guest-customize.sh`](scripts/waydroid-guest-customize.sh) — optional container customization (Magisk/Zygisk/Shamiko, WebView GL, mouse fix, device spoof)

```sh
# Install all features
sudo ./scripts/waydroid-guest-customize.sh --all

# Or pick features
sudo ./scripts/waydroid-guest-customize.sh --magisk --mouse-fix
sudo ./scripts/waydroid-guest-customize.sh --webview-gl --settings-tweaks

# Custom device spoof
SPOOF_MODEL=SM-S908B SPOOF_BRAND=samsung sudo ./scripts/waydroid-guest-customize.sh --device-spoof
```

| Flag | Effect |
|------|--------|
| `--magisk` | Install Magisk 30.1-Waydroid + Zygisk + Shamiko (requires [wsu](https://github.com/mistrmochov/WaydroidSU)) |
| `--webview-gl` | Disable Vulkan draw functor in WebView, force GL path |
| `--mouse-fix` | Enable relative mouse motion for games (`fake_touch=1`) |
| `--device-spoof` | Spoof device identity (default: HUAWEI VOG-AL10, override with `SPOOF_MODEL`/`SPOOF_BRAND`/`SPOOF_DEVICE`) |
| `--settings-tweaks` | Hide dev settings, disable package verifier, set pointer speed |

- [`docs/troubleshooting.md`](docs/troubleshooting.md) — health checks, known
  failure modes, one-command debug capture, GPU probe kit
- [`docs/architecture.md`](docs/architecture.md) — how the stack works
- [`docs/transport-design.md`](docs/transport-design.md) — socket protocol
  extensions (fences, imports, GPU allocation)
- [`docs/building.md`](docs/building.md) — building from source, repo layout,
  CI/attestation
- [`docs/dev-workflow.md`](docs/dev-workflow.md) — dev environment setup and
  the edit → build → deploy → measure loop
- [`docs/install-manual.md`](docs/install-manual.md) — non-Arch installation

## Limitations & roadmap

Not yet supported: ETC2 texture emulation (ASTC is; affected games show
placeholder textures), ASTC readback (uploads/sampling work), RGBA_FP16
gralloc buffers. dma_buf mmap read bandwidth is below native (readback paths
only). Planned: self-contained guest image published as an OTA channel,
shared-memory ring transport, ETC2, input-to-photon measurement.

## Prior art & references

Anbox Cloud on NVIDIA (commercial existence proof of this shape) ·
waydroid#1883 / #564 / #1402 ·
[Mesa Venus](https://gitlab.freedesktop.org/mesa/mesa) ·
[virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer) · ANGLE.

## License

Original code in `src/`, `build/`, `dev/`, `tests/`, `docs/` is MIT (see
[`LICENSE`](LICENSE)). Files under `patches/` are derivative works of their
respective upstreams and carry those upstreams' licenses.
