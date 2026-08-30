# Troubleshooting

Most field failures so far have been environment/config issues that
`waydroid-nvidia-setup` now detects or auto-fixes — **re-running
`sudo waydroid-nvidia-setup` is the first move for almost everything.**
It refuses loudly (with the fix in the message) on a missing NVIDIA node,
`nvidia-drm.modeset=0`, or a pre-minigbm vendor image, and it auto-removes
stale gralloc overrides left in `waydroid.cfg` by old installs.

## Quick health check

```sh
sudo waydroid shell dumpsys SurfaceFlinger | grep GLES
# healthy: GLES: ... ANGLE (NVIDIA, Vulkan ... Venus (NVIDIA GeForce ...))
sudo waydroid shell getprop ro.hardware.gralloc
# healthy: minigbm_gbm_mesa
sudo waydroid shell getprop ro.product.cpu.abilist
# ARM32 translation needs x86 plus armeabi-v7a in this list
```

## One-command debug capture

Restarts the stack, lets it run/crash for 12 s, and collects everything a bug
report needs into one file:

```sh
sudo -v; ( nvidia-smi -L; systemctl --user restart wd-venus; waydroid session stop; (waydroid session start &); sleep 12; echo "=== wd-venus journal:"; journalctl --user -u wd-venus --since "-1 min" --no-pager; echo "=== full logcat:"; sudo waydroid shell -- logcat -d ) > wdnv-debug.txt 2>&1
```

Skim it for personal data (a fresh crash-looping boot normally contains
none), then attach it to an issue.

## Hybrid display verification

On a box with an Intel/AMD iGPU driving the panel and NVIDIA doing the
render, the first gralloc alloc dumps the DRM topology and the chosen
presentation path to the `wd-venus` journal. After `show-full-ui`:

```sh
journalctl --user -u wd-venus --since '2 min ago' --no-pager | grep vtest_gpu_alloc
```

Healthy hybrid:

```
vtest_gpu_alloc: === presentation topology (hybrid issue #2) ===
vtest_gpu_alloc:   card0 vendor=0x8086 (Intel) driver=i915 boot_vga=1 connected=yes [eDP-1]
vtest_gpu_alloc:   card1 vendor=0x10de (NVIDIA) driver=nvidia boot_vga=0 connected=no
vtest_gpu_alloc:   present=nvidia-linear  reason=non-NVIDIA GPU has the only connected display (iGPU compositor)
vtest_gpu_alloc: gpu 1920x1080 ARGB8888 path=nvidia-linear hostvis=0 modifier=0x0 ...
```

Healthy discrete NVIDIA (no iGPU compositor): `present=block-linear` and
`modifier=0x03...`. If a hybrid box chose `block-linear`, force LINEAR:

```sh
systemctl --user edit wd-venus.service
# [Service]
# Environment=WAYDROID_NVIDIA_PRESENT=linear
systemctl --user daemon-reload
systemctl --user restart wd-venus.service
```

Do **not** set `=linear` on a machine whose compositor is on NVIDIA — KWin
cannot bind LINEAR as `GL_TEXTURE_2D` and the window goes black.

The probe `tests/crossimport.c` (`gcc -O1 -o crossimport tests/crossimport.c -lEGL -lGLESv2 -lgbm -ldl`)
checks whether this iGPU can import NVIDIA LINEAR at all, without starting
Waydroid.

## Known failure modes

| Symptom | Cause | Fix |
|---|---|---|
| `waydroid session start` refuses: "Venus render server socket … not accepting connections" | patched session could not `systemctl --user restart wd-venus.service` (unit missing, or you ran `waydroid` under sudo so `--user` is root's manager) | Install the user unit, then `waydroid session start` as the desktop user. Do **not** `enable` wd-venus — leftover clients after a session bounce block the next SurfaceFlinger connect |
| `show-full-ui` is a black window, `waydroid status` says `IP address: UNKNOWN`, host `adb devices` is empty, `wd-venus` restart-loops with `No provider of eglCreateSyncKHR` | Android *did* start (`system_server`/`adbd` live) but SurfaceFlinger aborts in `vn_renderer_create_vtest` because the host renderer died in `virgl_egl_init` (libepoxy abort on NVIDIA proprietary). Venus does not need vrend/EGL. | `wd-venus.service` must pass `--no-virgl --venus`. Drop any user ExecStart override that omits `--no-virgl`, then `systemctl --user daemon-reload && systemctl --user restart wd-venus.service` and restart the session |
| SurfaceFlinger crash-loops with `Unable to generate SkSurface`, guest uses `allocator@2.0` / "Using fallback gralloc implementation" | Stale gralloc override in `waydroid.cfg` (e.g. the old software-rendering workaround `ro.hardware.gralloc=default`), or a pre-minigbm vendor image | Re-run `sudo waydroid-nvidia-setup` — it removes the override / refuses the old image and tells you |
| Setup refuses: vendor image too old | Migrated install carrying an ancient `vendor.img`; **`waydroid init -f` does NOT re-download the vendor** (it trusts a stored timestamp even if the file is deleted) | `sudo sed -i 's/^vendor_datetime.*/vendor_datetime = 0/' /var/lib/waydroid/waydroid.cfg && sudo rm -f /var/lib/waydroid/images/vendor.img && sudo waydroid init -f` — then re-run setup. If you use gapps, `waydroid init -f -s GAPPS` (plain `-f` silently resets the channel to VANILLA) |
| Setup refuses: `nvidia-drm.modeset` | modeset=0 disables **all** DMA-BUF support in the driver — nothing in this stack can work | Add `nvidia_drm.modeset=1` to kernel params or modprobe.d, reboot |
| Cursor invisible / screenshots black, everything else fine | No `/dev/udmabuf` access (CPU-mappable buffer path); wd-venus journal says exactly this | The package's udev rule grants it to the seated user — re-log-in once after install. Headless: `setfacl -m u:USER:rw /dev/udmabuf` |
| Images in `/etc/waydroid-extra/images` or `/usr/share/waydroid-extra/images` | waydroid silently prefers preinstalled images over downloads | Remove/move them if you want OTA images |
| ARM32-only app runs on LLVM/Lavapipe while the desktop is accelerated | The 32-bit `vendor/lib/hw/vulkan.virtio.so` is missing or is a renamed `vulkan.lvp.so` | Install a dual-ABI release and re-run `sudo waydroid-nvidia-setup`; it requires ELF32/`EM_386` for every `vendor/lib` payload |
| `waydroid` dies instantly with `ModuleNotFoundError: No module named 'dbus'` (or `gi`, `gbinder`) | `/usr/bin/waydroid` used `#!/usr/bin/env python3`, and a user-local python (Homebrew/linuxbrew, pyenv, conda, `~/.local`) earlier in `PATH` shadows the system interpreter that owns the bindings | Re-run the installer (it now pins the shebang to an absolute system python), or fix it in place: `sudo sed -i '1s\|^#!.*\|#!/usr/bin/python3\|' /usr/lib/waydroid/waydroid.py` — edit **only** that file, never `/usr/bin/waydroid` (see next row) |
| `ModuleNotFoundError: No module named 'tools'` | `/usr/bin/waydroid` is supposed to be a symlink to `/usr/lib/waydroid/waydroid.py`; that link is what makes `sys.path[0]` the waydroid dir. Editing it in place (`sed -i`, some editors) replaces the link with a plain copy under `/usr/bin`, where `tools/` does not exist | Restore the link: `sudo ln -sfn ../lib/waydroid/waydroid.py /usr/bin/waydroid` |
| Hybrid laptop: Waydroid window dies instantly / black, KWin/Mutter logs `INVALID_WL_BUFFER` or "unsupported modifier" | iGPU compositor cannot import NVIDIA **block-linear** dmabufs | Host `wd-venus` must log `present=nvidia-linear` and allocs with `modifier=0x0`. If it chose `block-linear`, force it: `systemctl --user edit wd-venus.service` → `[Service]` `Environment=WAYDROID_NVIDIA_PRESENT=linear`, then `daemon-reload` + restart `wd-venus` and the session. Collect `journalctl --user -u wd-venus --since '2 min ago' \| grep vtest_gpu_alloc` |
| Discrete NVIDIA: window black after setting `WAYDROID_NVIDIA_PRESENT=linear` | NVIDIA EGL cannot bind LINEAR as `GL_TEXTURE_2D` | Unset the override (or set `=block-linear`). LINEAR is only for iGPU compositors |
| `show-full-ui` black after bouncing the session; `ss -xlp` shows Recv-Q on `venus.sock`; no new `virgl_render_server` | leftover vtest clients on a still-running `wd-venus` (login-enabled unit, or session stop did not stop Venus) | Patched session **restarts** Venus on start and **stops** it on stop. `systemctl --user disable wd-venus.service` (no `--now` if a session is live). Next `waydroid session stop` then `start` is a clean renderer |

For a translated app, confirm its process maps the 32-bit stack (replace the
package name if needed):

```sh
sudo waydroid shell -- sh -c '
  pid=$(pidof com.tencent.qqmusic | cut -d" " -f1)
  grep -E "/vendor/lib/(egl|hw)/(libGLESv2_angle|vulkan\.virtio)\.so" /proc/$pid/maps'
```

While playing audio or animating the UI, `nvidia-smi pmon` should show work in
the host renderer and logcat should not name `llvmpipe` or `Lavapipe`.

## Is my GPU/driver combination OK?

Either the proprietary (`nvidia.ko` / `nvidia-dkms`) or open (`nvidia-open`) kernel module works. Pascal and newer GPUs. Driver 535+ required (610.x recommended) with `nvidia-drm.modeset=1`.

To test any machine in ~30 seconds without installing anything, build the
probe from this repo and run it — it checks the actual DMA-BUF import/export
paths the stack depends on:

```sh
gcc -O1 -o nvimportprobe tests/nvimportprobe.c -ldl
tests/run-probe.sh    # or ./nvimportprobe directly
```

Exit 0 = this machine can run the stack. Exit 3 = it can't, and the output
says why.
