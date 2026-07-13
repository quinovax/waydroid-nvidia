# waydroid-nvidia

**GPU-accelerated Waydroid on the NVIDIA proprietary driver — container-native, no VM.**

Waydroid runs Android in an LXC container. On a machine whose displays hang off
an NVIDIA GPU, stock Waydroid can't use it for rendering. This project makes
Android render on the NVIDIA GPU by **proxying Vulkan (Mesa Venus) over a unix
socket** to a host-side renderer that issues the real Vulkan calls — keeping the
host's proprietary NVIDIA userspace (CUDA / NVENC / full performance) intact and
the whole thing inside a container.

## How it works

```
Android app ── Vulkan ──▶ guest Mesa Venus (bionic)
                              │ serialized Venus protocol, unix socket
                              ▼
                     host daemon (virglrenderer render server)
                              │ real Vulkan calls
                              ▼
                NVIDIA proprietary driver (Open KM) ──▶ GPU
                              │ rendered buffer as dmabuf (zero-copy)
                              ▼
              guest gralloc imports ──▶ hwcomposer ──▶ KWin
```

The container speaks the **Venus** wire protocol (Mesa's virtio-gpu Vulkan
driver) over the vtest unix-socket transport. A patched **virglrenderer** on the
host decodes it and replays the calls on the real NVIDIA driver. Buffers are
allocated **host-side** as NVIDIA block-linear `VkImage`s, exported as dmabufs,
and imported by a custom **gralloc** backend in the guest — so every buffer the
compositor sees is NVIDIA-native and binds without cross-vendor negotiation.

See [`docs/architecture.md`](docs/architecture.md) for the design and
[`docs/transport-design.md`](docs/transport-design.md) for the socket protocol
extensions.

## Capabilities

- **Full NVIDIA acceleration, zero VM.** Every pixel Android draws — Vulkan,
  GL (via ANGLE), UI, games — renders on the host NVIDIA GPU and displays
  through KWin as native NVIDIA dmabufs.
- **High refresh, low latency.** The guest runs a native high-refresh display
  (500 Hz supported; SurfaceFlinger's stock scheduler caps at ~333 Hz — this
  ships a one-line-tunable fix). All frame synchronization is GPU-side:
  timeline-syncobj fences shared between guest and host (zero per-frame
  socket roundtrips) and imported `sync_fd` semaphores (no CPU waits in
  SurfaceFlinger). Measured: a translated ARM game runs 500 fps flat at
  2 ms present-to-present on a 500 Hz monitor.
- **The compositor path is direct.** SurfaceFlinger composites nothing in
  steady state: app buffers attach straight to Wayland surfaces and KWin
  displays them — a fullscreen game's buffer travels app → KWin untouched.
  Layers the compositor can't take directly (software-rendered, solid-color)
  fall back to SurfaceFlinger transparently.
- **Vulkan-native Android games work on desktop GPUs.** Desktop NVIDIA has no
  ASTC texture hardware (Android mandates it); the guest Venus driver
  transparently decodes ASTC uploads with a compute shader, so Unity/Unreal
  Vulkan titles render correctly instead of magenta placeholders.
  (Opt-out: `VN_NO_ASTC_EMU=1`.)
- **Real games, verified:** Minecraft Bedrock (native x86_64), Subway
  Surfers, Arknights, Honkai: Star Rail — plus Google Play certification and
  ARM translation via libhoudini.
- **Survives updates.** Mounts and props are emitted by the integration
  generators, so `waydroid upgrade` keeps everything working. A desktop
  launcher entry health-checks and auto-recovers the whole stack on click.

## Repository layout

This repo is **patches + net-new source + build glue**, not vendored upstream
trees. A build clones pinned upstream and applies the patches (AUR-friendly).

```
patches/            per-component patch series; each dir has a BASE (pinned commit)
  mesa/               guest Venus driver
  virglrenderer/      host renderer
  minigbm/            gralloc (base pin; the change is the net-new backend below)
  hwcomposer/         display / refresh / windowing
  waydroid/           lxc.py mount generator + suspend handling
src/                net-new source dropped into the patched trees
  virglrenderer-vtest/  vtest_gpu_alloc.{c,h}   host NVIDIA allocator
  minigbm-vtest/        vtest_wrapper.c         gralloc backend over the socket
build/              standalone build glue (hwcomposer NDK, hidl-gen, mesa cross, ANGLE args)
build/lineage-20/   guest image / SurfaceFlinger build recipes (500 Hz patch)
patches/lineage-20/ frameworks/native patch series (vsync snap window prop)
dev/                dev-loop scripts (restart, status, logs)
tests/              C probes used to de-risk each step
docs/               architecture, transport design, dev workflow
packaging/host/     host helper binaries (wd-deploy, wd-launch desktop launcher)
packaging/aur/      PKGBUILD (planned)
```

## Building & deploying

Each `patches/<component>/BASE` pins the upstream commit and apply order. In outline:

1. **Host renderer** — clone virglrenderer at `patches/virglrenderer/BASE`, apply
   the series, copy `src/virglrenderer-vtest/*` into `vtest/`, `meson` + `ninja`.
   Runs as a systemd user unit serving the venus socket.
2. **Guest Mesa Venus** — clone mesa at `patches/mesa/BASE`, apply, cross-build
   for `android-x86_64` (NDK; cross file in `build/mesa/`).
3. **gralloc backend** — build `src/minigbm-vtest/vtest_wrapper.c` against
   minigbm; deploy as the `libgbm_mesa_wrapper` replacement.
4. **hwcomposer** — apply `patches/hwcomposer`, build standalone with
   `build/hwcomposer/build.sh` (no AOSP tree needed).
5. **waydroid** — apply `patches/waydroid`; the generators emit the bind-mounts
   and props so they survive `waydroid upgrade`.

## Roadmap

- Self-contained guest image (all components folded in, bind-mounts retired)
  published as an OTA channel + AUR package for one-command install.
- ETC2 texture emulation (same mechanism as ASTC; needed by some GLES3
  Vulkan ports).
- Shared-memory ring transport for Venus commands.
- Input-to-photon measurement and tuning.

## Limitations

- ETC2-compressed textures are not yet emulated (ASTC is); affected games
  show placeholder textures.
- Reading back ASTC texture data from the GPU (rare; some tools) is not
  supported — uploads and sampling are.
- RGBA_FP16 buffer combination not yet supported by the gralloc format table.
- dma_buf mmap read bandwidth is below native (affects readback paths, not the
  hot render path).

## Prior art & references

- Anbox Cloud on NVIDIA — commercial existence proof of this shape.
- Tracking issues: waydroid#1883 (NVIDIA accel), waydroid#564 (socket-proxy
  proposal), waydroid#1402 (Venus/virgl discussion).
- [Mesa Venus](https://gitlab.freedesktop.org/mesa/mesa) ·
  [virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer) · ANGLE.

## License

Original code in `src/`, `build/`, `dev/`, `tests/`, `docs/` is MIT (see
[`LICENSE`](LICENSE)). Files under `patches/` are derivative works of their
respective upstreams and carry those upstreams' licenses.
