#!/usr/bin/env python3
"""Build full pinned Mesa NVK for the host and Android (no deployment)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
# A driver-source worktree outside the outer checkout names the outer root
# explicitly (shared build/ trees); the in-tree default is unchanged.
ROOT = Path(os.environ["PROLAPSE_ROOT"]).resolve() if "PROLAPSE_ROOT" in os.environ else HERE.parents[3]
sys.path.insert(0, str(ROOT / "tools"))
import native_rust_profile as profiles
import cc_launcher  # NATIVE_CC_LAUNCHER (ccache) in front of the C/C++ compilers
from fetch import fetch, retry_run  # pinned downloads: mirrors, retries, verified before kept
import android_libelf

PIN = "ddaf85e353442d9d7f192fdcf1823e652e3f2ee3"  # tag mesa-26.2.5 (2026-10-14)
SOURCE = ROOT / "build/mesa-rm-source"
TOOLS = ROOT / "build/nak-tools/bin"
DEPS = ROOT / "build/nvk-host-deps"
NDK = ROOT / "build/hwcomposer-work/ndk/android-ndk-r29/toolchains/llvm/prebuilt/linux-x86_64"
ANDROID = ROOT / "build/hwcomposer-work/deps"


def ninja_load():
    """`-l N` when NINJA_LOAD is set (packaging/native/build-native.sh builds targets concurrently: each ninja
    keeps -j but stops starting jobs while the load average is above N)."""
    return ["-l", os.environ["NINJA_LOAD"]] if os.environ.get("NINJA_LOAD") else []

def run(command, **kwargs):
    subprocess.run([str(x) for x in command], check=True, **kwargs)

def setup_run(command, args, **kwargs):
    """meson setup; with --download-deps it may fetch wraps (crates for NAK), so retry like other downloads."""
    if getattr(args, "download_deps", False):
        retry_run(command, **kwargs)
    else:
        run(command, **kwargs)

def apply(patch):
    reverse = subprocess.run(["git", "-C", str(SOURCE), "apply", "--reverse", "--check", str(patch)], capture_output=True)
    if reverse.returncode:
        run(["git", "-C", SOURCE, "apply", "--check", patch])
        run(["git", "-C", SOURCE, "apply", patch])

def host_dependencies(download):
    """Host-only tools: verify archive hashes before local extraction."""
    archives = DEPS / "downloads"
    archives.mkdir(parents=True, exist_ok=True)
    prefix = DEPS / "root"
    prefix.mkdir(exist_ok=True)
    items = json.loads((HERE / "host-build-deps.json").read_text())
    # extracted once per pin set: concurrent target builds run host tools (mesa_clc) from this prefix, so
    # it is never rewritten under them
    stamp = prefix / ".extracted"
    want = "".join(sorted(f"{i['filename']} {i['sha256']}\n" for i in items))
    if stamp.is_file() and stamp.read_text() == want:
        return
    for item in items:
        path = archives / item["filename"]
        if not path.exists():
            if not download:
                raise SystemExit(f"Missing {path}; use --download-deps for local build tools")
            # the Arch Linux Archive keeps every package version; a live mirror only while it is current
            name = path.name.rsplit("-", 3)[0]
            fetch([f"https://archive.archlinux.org/packages/{name[0]}/{name}/{path.name}",
                   f"https://geo.mirror.pkgbuild.com/extra/os/x86_64/{path.name}"], path, item["sha256"])
        if hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]:
            raise SystemExit(f"Checksum mismatch: {path}")
        # Limit extraction to usr/: package manager metadata is not needed.
        run(["bsdtar", "-xf", path, "-C", prefix, "usr"])
    for directory in ("lib/pkgconfig", "share/pkgconfig"):
        for pc in (prefix / "usr" / directory).glob("*.pc"):
            text = pc.read_text()
            text = text.replace("prefix=/usr", f"prefix={prefix}/usr")
            text = text.replace("datarootdir=/usr", f"datarootdir={prefix}/usr")
            text = text.replace("datadir=/usr", f"datadir={prefix}/usr")
            pc.write_text(text)
    stamp.write_text(want)

def android_cross_file(bits=64, out=None, download=False):
    """Meson cross file + pkg-config dir for an Android ABI. NVK (out=None)
    writes android{,32}.ini next to this script and build/nvk-<target>-pkgconfig;
    the GL build passes its own directory so it never rewrites NVK inputs."""
    target = "android" if bits == 64 else "android32"
    triple = "x86_64-linux-android" if bits == 64 else "i686-linux-android"
    cpu = "x86_64" if bits == 64 else "x86"
    drm_cpu = "x86_64" if bits == 64 else "x86"
    pkg = ROOT / f"build/nvk-{target}-pkgconfig" if out is None else out / "pkgconfig"
    pkg.mkdir(parents=True, exist_ok=True)
    runtime = ANDROID / "image-rootfs/system/lib64" if bits == 64 else ROOT / "build/nvk-android32/runtime"
    headers = {
        "cutils": ["aosp_system_core/libcutils/include", "aosp_system_core/libsystem/include", "aosp_system_core/libprocessgroup/include"],
        "hardware": ["aosp_libhardware/include", "aosp_system_core/libsystem/include", "aosp_frameworks_native/vulkan/include", "aosp_frameworks_native/libs/nativebase/include", "aosp_frameworks_native/libs/nativewindow/include", "aosp_frameworks_native/libs/arect/include"],
        "log": ["android_system_logging/liblog/include"],
        "sync": ["aosp_system_core/libsync/include"],
        "nativewindow": ["aosp_frameworks_native/libs/nativewindow/include", "aosp_frameworks_native/libs/nativebase/include", "aosp_frameworks_native/libs/arect/include"],
    }
    for name, dirs in headers.items():
        for directory in dirs:
            if not (ANDROID / directory).is_dir():
                raise SystemExit(f"Missing existing pinned platform headers: {ANDROID / directory}")
        (pkg / f"{name}.pc").write_text(f"Name: {name}\nDescription: Android13 platform {name}\nVersion: 33\nLibs: -L{runtime} -l{name}\nCflags: " + " ".join(f"-I{ANDROID / d}" for d in dirs) + "\n")
    # Mesa >= 26: NVK requires libelf (static, upstream Android CI recipe).
    android_libelf.install_pc(android_libelf.build(ROOT, NDK / "bin", triple, download), pkg)
    drm = ROOT / "build/libdrm"
    (pkg / "libdrm.pc").write_text(f"Name: libdrm\nDescription: Source-built Android libdrm\nVersion: 2.4.134\nLibs: -L{drm}/build-android33-{drm_cpu} -ldrm\nCflags: -I{drm}/drm -I{drm}/drm/include/drm\n")
    cross = HERE / f"{target}.ini" if out is None else out / f"{target}.ini"
    cross.write_text(f"""[binaries]
c = {cc_launcher.cross_binary(f'{NDK}/bin/{triple}33-clang')}
cpp = {cc_launcher.cross_binary(f'{NDK}/bin/{triple}33-clang++')}
ar = '{NDK}/bin/llvm-ar'
strip = '{NDK}/bin/llvm-strip'
rust = ['rustc', '--target={triple}']
pkg-config = 'pkg-config'
[properties]
needs_exe_wrapper = true
pkg_config_libdir = '{pkg}'
[built-in options]
c_args = ['-fPIC']
cpp_args = ['-fPIC']
cpp_link_args = ['-static-libstdc++', '-Wl,-rpath-link,{runtime}']
[host_machine]
system = 'android'
cpu_family = '{cpu}'
cpu = '{cpu}'
endian = 'little'
""")
    return cross

# NVRM_GL_DIR_SUFFIX lets parallel branches build without sharing a tree
# (e.g. "-gles3" -> build/mesa-gl-source-gles3, build/nvrm-host-gles3).
_GL_SUFFIX = os.environ.get("NVRM_GL_DIR_SUFFIX", "")
GL_SOURCE = ROOT / f"build/mesa-gl-source{_GL_SUFFIX}"
GL_RUST = ROOT / f"build/nvrm-rust{_GL_SUFFIX}"
NVK_PATCHES = ("build-compat.patch", "rm-integration.patch", "pascal-a-clear.patch",
               "nvk-upstream-ecfea8d9-ycbcr-modifier.patch",
               "nvk-upstream-bfb90f05-memory-plane-aspects.patch",
               "nvk-explicit-plane-layouts.patch", "nvk-angle-nan-preserve-min-max.patch",
               "mesa-disk-cache-create-with-path.patch", "mesa-disk-cache-keep-file-backend.patch",
               "mesa-disk-cache-evict-invalid-item.patch", "nvk-android-app-disk-cache.patch",
               "nvk-shader-upload-publish.patch", "nvk-shader-prefetch-in-range.patch",
               "nvk-prevolta-modifier-memory-types.patch", "nvk-3d-view-of-2d-image.patch")

GL_TRIPLES = {"android": "x86_64-linux-android", "android32": "i686-linux-android"}
# Android: distinct library names so the image's own Mesa (libEGL_mesa.so,
# libGLESv*_mesa.so, libgallium_dri.so in /vendor/lib*/egl and /vendor/lib*)
# is never shadowed: ro.hardware.egl=nvrm selects these.
GL_ANDROID_SUFFIX = "_nvrm"
GL_ANDROID_ARTIFACTS = (f"src/egl/libEGL{GL_ANDROID_SUFFIX}.so",
                        f"src/mesa/glapi/es1api/libGLESv1_CM{GL_ANDROID_SUFFIX}.so",
                        f"src/mesa/glapi/es2api/libGLESv2{GL_ANDROID_SUFFIX}.so",
                        f"src/gallium/targets/dri/libgallium_dri{GL_ANDROID_SUFFIX}.so")

NVK_PATCH_LIST = [
    HERE / "build-compat.patch",
    HERE / "rm-integration.patch",
    # Generic NVK hardware behaviour (any backend): PASCAL_A clear bracket.
    HERE / "pascal-a-clear.patch",
    # NV12/P010 dma-buf import: 26.2 has upstream !40473 (multi-planar LINEAR
    # modifiers, plane offsets, multi-plane tiled shadows); these two
    # upstream main fixes are not in 26.2.x: ecfea8d9 (modifier list on a
    # YCbCr image picks LINEAR without nil) and bfb90f05 (MEMORY_PLANE_i ->
    # plane i), cherry-picked unchanged.
    HERE / "nvk-upstream-ecfea8d9-ycbcr-modifier.patch",
    HERE / "nvk-upstream-bfb90f05-memory-plane-aspects.patch",
    # Client plane layouts that cannot be bound fail vkCreateImage loudly
    # (upstream asserts).
    HERE / "nvk-explicit-plane-layouts.patch",
    # GLES through ANGLE keeps NaN-preserving FMin/FMax/FClamp (Mesa 26
    # folds max(NaN, 0) to NaN; GTA SA car paint black), driconf by engine.
    HERE / "nvk-angle-nan-preserve-min-max.patch",
    # Shader disk cache in an explicit per-application directory
    # (create_with_path): nvrm's program cache and, on Android, NVK's
    # pipeline cache in the application's code_cache; never redirected
    # into the EGL blob cache (keep-file-backend); self-healing after a
    # damaged item (evict-invalid).
    HERE / "mesa-disk-cache-create-with-path.patch",
    HERE / "mesa-disk-cache-keep-file-backend.patch",
    HERE / "mesa-disk-cache-evict-invalid-item.patch",
    HERE / "nvk-android-app-disk-cache.patch",
    # Shader programs published as the proprietary driver does: copy-engine
    # uploads into an unmapped heap, an upload count, and an instruction-
    # cache invalidate after the upload wait in the first submission after
    # the count moved; programs in whole 256 B prefetch blocks (Ampere B+).
    HERE / "nvk-shader-upload-publish.patch",
    HERE / "nvk-shader-prefetch-in-range.patch",
    # Fermi..Volta: block-linear DRM-modifier images get no host-visible
    # VRAM (BAR1) memory type, as the proprietary driver offers none; BAR1
    # does not return the modifier's byte layout there.
    HERE / "nvk-prevolta-modifier-memory-types.patch",
    # A 3D view of a 2D image (invalid usage NVIDIA's driver accepts) is
    # described as a depth-1 3D image instead of tripping nil's assert.
    HERE / "nvk-3d-view-of-2d-image.patch",
]

def prepared_stamp(source, inputs):
    """Content hash of what a source preparation applies (patches, copied files): written into the worktree
    after --prepare / a full run, checked by --skip-prepare (concurrent target builds share one worktree)."""
    h = hashlib.sha256(PIN.encode())
    for path in sorted(Path(p) for p in inputs):
        for f in (sorted(x for x in path.rglob("*") if x.is_file()) if path.is_dir() else [path]):
            h.update(str(f.relative_to(HERE)).encode() + b"\0" + f.read_bytes())
    return Path(source) / ".nv-prepared", h.hexdigest()

def check_prepared(source, inputs, what):
    stamp, digest = prepared_stamp(source, inputs)
    if not stamp.is_file() or stamp.read_text().strip() != digest:
        raise SystemExit(f"{source} is not prepared for this checkout ({what}); run build.py "
                         f"{'--gl ' if source == GL_SOURCE else ''}--prepare-only first")

def mark_prepared(source, inputs):
    stamp, digest = prepared_stamp(source, inputs)
    stamp.write_text(digest + "\n")

GL_PREP_INPUTS = [HERE / n for n in NVK_PATCHES + ("gallium-nvrm.patch", "mesa-oes-texture-stencil8.patch",
                                                  "mesa-nv-polygon-mode.patch", "nak-panic-message.patch",
                                                  "android-egl-suffix.patch", "nv_rm.h", "nvkmd_rm.h", "nvkmd_rm.c",
                                                  "gallium-nvrm")]
NVK_PREP_INPUTS = NVK_PATCH_LIST + [HERE / n for n in ("nv_rm.h", "nvkmd_rm.h", "nvkmd_rm.c")]

def write_ndk_zlib_pc(pkgconfig_dir):
    """zlib.pc for the NDK sysroot zlib (an NDK stable library, libz.so in
    every image's /system/lib*); libzstd is not in the NDK."""
    ndk_zlib = (NDK / "sysroot/usr/include/zlib.h").read_text()
    zlib_version = next(line.split('"')[1].split("-")[0] for line in ndk_zlib.splitlines()
                        if line.startswith("#define ZLIB_VERSION "))
    (pkgconfig_dir / "zlib.pc").write_text(
        f"Name: zlib\nDescription: NDK sysroot zlib\nVersion: {zlib_version}\nLibs: -lz\nCflags:\n")

def gl_build_dir(target):
    return ROOT / f"build/nvrm-{target}{_GL_SUFFIX}"

def prepare_gl_source(args):
    """Reset the GL Mesa worktree to the pin and apply the full stack (the
    worktree is ours alone). Returns the build environment. --skip-prepare: only
    check that this checkout's stack was applied (a concurrent build prepared it)."""
    for tool in ("bindgen", "cbindgen"):
        if not (TOOLS / tool).exists():
            raise SystemExit(f"Missing {tool}; run ../compiler/build.py --download-deps first")
    if getattr(args, "skip_prepare", False):
        check_prepared(GL_SOURCE, GL_PREP_INPUTS, "GL")
        return gl_env()
    stamp = GL_SOURCE / ".nv-prepared"
    if stamp.exists():
        stamp.unlink()
    if not GL_SOURCE.exists():
        run(["git", "-C", ROOT / "build/mesa-git", "worktree", "add", "--detach", GL_SOURCE, PIN])
        run(["git", "-C", GL_SOURCE, "sparse-checkout", "disable"])
    actual = subprocess.check_output(["git", "-C", str(GL_SOURCE), "rev-parse", "HEAD"], text=True).strip()
    if actual != PIN:
        raise SystemExit(f"Mesa GL worktree revision mismatch: {actual}")
    run(["git", "-C", GL_SOURCE, "reset", "-q", "--hard", PIN])
    run(["git", "-C", GL_SOURCE, "clean", "-qfdx", "-e", "subprojects/packagecache"])
    # GL-only Mesa core fixes (st/mesa, main/) on top of the NVK stack;
    # android-egl-suffix.patch only changes Android library naming.
    for name in NVK_PATCHES + ("gallium-nvrm.patch", "mesa-oes-texture-stencil8.patch",
                               "mesa-nv-polygon-mode.patch", "nak-panic-message.patch",
                               "android-egl-suffix.patch", "mesa-glapi-narrow-args.patch"):
        run(["git", "-C", GL_SOURCE, "apply", HERE / name])
    rm_source = GL_SOURCE / "src/nouveau/vulkan/nvkmd/rm"
    rm_source.mkdir(parents=True, exist_ok=True)
    for name in ("nv_rm.h", "nvkmd_rm.h", "nvkmd_rm.c"):
        shutil.copy2(HERE / name, rm_source / name)
    shutil.copytree(HERE / "gallium-nvrm", GL_SOURCE / "src/gallium/drivers/nvrm", dirs_exist_ok=True)
    cache = GL_SOURCE / "subprojects/packagecache"
    cache.mkdir(exist_ok=True)
    for archive in (ROOT / "build/mesa-nak-source/subprojects/packagecache").glob("*.tar.gz"):
        if not (cache / archive.name).exists():
            shutil.copy2(archive, cache / archive.name)
    host_dependencies(args.download_deps)
    mark_prepared(GL_SOURCE, GL_PREP_INPUTS)
    return gl_env()

def gl_env():
    env = dict(os.environ)
    host_clc = ROOT / "build/nvk-host/src/compiler/clc"
    host_vtn = ROOT / "build/nvk-host/src/compiler/spirv"
    if not (host_clc / "mesa_clc").exists():
        raise SystemExit("Missing host mesa_clc: build the NVK host target first (build.py --target host)")
    env["PATH"] = ":".join(map(str, (host_clc, host_vtn, TOOLS))) + ":" + env["PATH"]
    env["PKG_CONFIG_PATH"] = f"{DEPS}/root/usr/lib/pkgconfig:{DEPS}/root/usr/share/pkgconfig"
    env["LD_LIBRARY_PATH"] = f"{DEPS}/root/usr/lib"
    cc_launcher.host_env(env)
    return env

def build_gl(args):
    """nvrm gallium driver: separate Mesa worktree, Cargo target and build dirs.

    Same pin and the same NVK patch stack as the NVK build (NAK, nil and
    nv_rm.h come from there), plus gallium-nvrm.patch (meson/loader hookup)
    and the gallium-nvrm/ overlay. Nothing under build/mesa-rm-source,
    build/nv-rm-rust, build/nvk-* or android{,32}.ini is read-modified or
    written. Host -> build/nvrm-host; android/android32 -> build/nvrm-android{,32}."""
    env = prepare_gl_source(args)
    if args.prepare_only:
        print(f"Prepared {GL_SOURCE}")
        return
    targets = ("android", "android32") if args.target == "both" else (args.target,)
    for target in targets:
        if target == "host":
            build_gl_host(args, env)
        else:
            build_gl_android(args, env, target)

def build_gl_host(args, env):
    GL_BUILD = gl_build_dir("host")
    rust_command = ["cargo", "build", "--offline", "--no-default-features", "--target-dir", GL_RUST,
                    "--manifest-path", HERE.parent / "Cargo.toml"]
    if args.rust_profile == "release":
        rust_command.append("--release")
    run(rust_command, env=env)
    rust_lib = GL_RUST / ("debug" if args.rust_profile == "dev" else "release")
    linker_args = [f"-L{rust_lib}", f"-Wl,-rpath,{rust_lib}"]
    options = ["-Dvulkan-drivers=nouveau", "-Dgallium-drivers=nvrm", "-Dplatforms=",
               "-Degl=enabled", "-Dgles1=disabled", "-Dgles2=enabled", "-Dopengl=true",
               "-Dglx=disabled", "-Dgbm=disabled", "-Dglvnd=disabled", "-Dllvm=disabled", "-Dmesa-clc=system",
               "-Dbuild-tests=false", "-Dvideo-codecs=", "-Dvalgrind=disabled",
               "-Dlibunwind=disabled", "-Dbuildtype=release",
               "-Dc_link_args=" + json.dumps(linker_args), "-Dcpp_link_args=" + json.dumps(linker_args)]
    setup = cc_launcher.setup_command(GL_BUILD, GL_SOURCE)
    setup_run(setup + options + ["--wrap-mode=" + ("default" if args.download_deps else "nodownload")], args, env=env)
    cc_launcher.record(GL_BUILD)
    run(["ninja", "-C", GL_BUILD, "-j", args.j, *ninja_load()], env=env)
    for name in ("src/egl/libEGL.so.1", "src/mesa/glapi/es2api/libGLESv2.so.2"):
        if not (GL_BUILD / name).exists():
            raise SystemExit(f"Missing {GL_BUILD / name} after the build")
    print(f"Built nvrm host GL (Rust {args.rust_profile}): {GL_BUILD}")

def build_gl_android(args, env, target):
    """platform_android EGL + GLES1/2 with u_gralloc (cros backend via the
    image's gralloc0 module) for one bionic ABI, NDK r29, SDK 33, same
    sysroot/pkg-config recipe as the NVK Android build."""
    build = gl_build_dir(target)
    bits = 32 if target == "android32" else 64
    triple = GL_TRIPLES[target]
    runtime = ROOT / "build/nvk-android32/runtime" if bits == 32 else ANDROID / "image-rootfs/system/lib64"
    target_env = dict(env)
    # The guest has ONE libnv_bionic_native.so per ABI (/vendor_extra), shared
    # with NVK, so nvrm links NVK's own Rust build output (build/nv-rm-rust,
    # profiles.rust_library) instead of a second cargo build: identity by
    # construction. Refused unless NVK's recorded build is current for the
    # outer checkout (profiles.verify_metadata) and was built against the same
    # nv_rm.h ABI as this checkout's nvrm.
    try:
        nvk_record = profiles.verify_metadata(ROOT, target, args.rust_profile)
    except RuntimeError as error:
        raise SystemExit(f"nvrm {target}: NVK's Rust backend is not usable: {error}")
    abi_key = "nvidia-prolapse/driver-source/native/mesa/nv_rm.h"
    abi_here = hashlib.sha256((HERE / "nv_rm.h").read_bytes()).hexdigest()
    if nvk_record["source_sha256"].get(abi_key) != abi_here:
        raise SystemExit(f"nvrm {target}: {HERE / 'nv_rm.h'} differs from the nv_rm.h NVK's Rust "
                         f"backend was built with ({ROOT / abi_key}); build both from one checkout")
    rust_lib = profiles.rust_library(ROOT, target, args.rust_profile).parent
    if not (rust_lib / "libnv_bionic_native.so").is_file():
        raise SystemExit(f"Missing {rust_lib / 'libnv_bionic_native.so'}")
    cross_dir = ROOT / f"build/nvrm-{target}-cross{_GL_SUFFIX}"
    cross = android_cross_file(bits, out=cross_dir, download=args.download_deps)
    # nvrm's program cache (Mesa disk_cache, nvrm_shader_cache.c) needs a
    # compressor: the NDK sysroot ships zlib (an NDK stable library, libz.so in
    # every image's /system/lib*); libzstd is not in the NDK.
    write_ndk_zlib_pc(cross_dir / "pkgconfig")
    linker_args = [f"-L{rust_lib}"]
    options = ["--cross-file", cross,
               "-Dvulkan-drivers=nouveau", "-Dgallium-drivers=nvrm",
               "-Dplatforms=android", "-Dplatform-sdk-version=33", "-Dandroid-stub=false",
               "-Dandroid-libbacktrace=disabled",
               "-Degl=enabled", "-Dgles1=enabled", "-Dgles2=enabled", "-Dopengl=true",
               f"-Degl-lib-suffix={GL_ANDROID_SUFFIX}", f"-Dgles-lib-suffix={GL_ANDROID_SUFFIX}",
               "-Dglx=disabled", "-Dgbm=disabled", "-Dglvnd=disabled", "-Dllvm=disabled",
               "-Dmesa-clc=system", "-Dexpat=disabled", "-Dzstd=disabled", "-Dzlib=enabled",
               "-Dshader-cache=enabled", "-Dbuild-tests=false", "-Dvideo-codecs=",
               "-Dvalgrind=disabled", "-Dlibunwind=disabled", "-Dbuildtype=release",
               "-Dc_link_args=" + json.dumps(linker_args),
               "-Dcpp_link_args=" + json.dumps(linker_args + ["-static-libstdc++", f"-Wl,-rpath-link,{runtime}"])]
    target_env.pop("PKG_CONFIG_PATH", None)
    target_env["BINDGEN_EXTRA_CLANG_ARGS"] = f"--sysroot={NDK}/sysroot -isystem {NDK}/sysroot/usr/include/{triple} -D__ANDROID_API__=33"
    setup = cc_launcher.setup_command(build, GL_SOURCE)
    setup_run(setup + options + ["--wrap-mode=" + ("default" if args.download_deps else "nodownload")], args, env=target_env)
    cc_launcher.record(build)
    run(["ninja", "-C", build, *GL_ANDROID_ARTIFACTS, "-j", args.j, *ninja_load()], env=target_env)
    for name in GL_ANDROID_ARTIFACTS:
        if not (build / name).is_file():
            raise SystemExit(f"Missing {build / name} after the build")
    # Build record for staging: the guest has ONE libnv_bionic_native.so per ABI
    # (vendor_extra), shared with NVK, so the stager must prove that the file it
    # stages is the one nvrm was linked against (sha256), and never mix.
    def sha(path):
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()
    head = subprocess.run(["git", "-C", str(HERE), "rev-parse", "HEAD"], capture_output=True, text=True)
    dirty = subprocess.run(["git", "-C", str(HERE), "status", "--porcelain", "--untracked-files=no"],
                           capture_output=True, text=True).stdout.strip() != ""
    record = {"format": 1, "target": target, "triple": triple, "rust_profile": args.rust_profile,
              "mesa_pin": PIN, "driver_source_head": head.stdout.strip(), "driver_source_dirty": dirty,
              "egl_suffix": GL_ANDROID_SUFFIX,
              "artifacts": {Path(name).name: {"path": str(build / name), "sha256": sha(build / name)}
                            for name in GL_ANDROID_ARTIFACTS},
              "rust": {"path": str(rust_lib / "libnv_bionic_native.so"),
                       "sha256": sha(rust_lib / "libnv_bionic_native.so")},
              "nvk_rust_build": str(profiles.metadata_path(ROOT, target))}
    if record["rust"]["sha256"] != nvk_record["rust"]["sha256"]:
        raise SystemExit(f"nvrm {target}: linked Rust backend changed during the build")
    (build / "nvrm-build.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"Built nvrm {target} GL (Rust {args.rust_profile}): {build}")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("host", "android", "android32", "both"), default="both")
    parser.add_argument("--rust-profile", choices=profiles.PROFILES, default="dev",
                        help="Rust Cargo profile (dev keeps the existing debug build)")
    parser.add_argument("--download-deps", action="store_true")
    parser.add_argument("-j", type=int, default=12)
    parser.add_argument("--gl", action="store_true",
                        help="build the nvrm gallium GL/GLES driver from its own Mesa worktree into "
                             "build/nvrm-<target> (--target both = android + android32); NVK trees "
                             "are not touched")
    prep = parser.add_mutually_exclusive_group()
    prep.add_argument("--prepare-only", action="store_true",
                      help="apply the patch stack to the Mesa worktree (NVK, or nvrm's with --gl) and stop")
    prep.add_argument("--skip-prepare", action="store_true",
                      help="build a target from a worktree an earlier --prepare-only/full run prepared for this "
                           "checkout (lets several targets build concurrently from one worktree)")
    args = parser.parse_args()
    if args.gl:
        return build_gl(args)
    sources_before = profiles.source_snapshot(ROOT)
    for tool in ("bindgen", "cbindgen"):
        if not (TOOLS / tool).exists():
            parser.error(f"Missing {tool}; run ../compiler/build.py --download-deps first")
    if not SOURCE.exists():
        run(["git", "-C", ROOT / "build/mesa-git", "worktree", "add", "--detach", SOURCE, PIN])
        run(["git", "-C", SOURCE, "sparse-checkout", "disable"])
    actual = subprocess.check_output(["git", "-C", str(SOURCE), "rev-parse", "HEAD"], text=True).strip()
    if actual != PIN:
        parser.error(f"Mesa revision mismatch: {actual}")
    if args.skip_prepare:
        check_prepared(SOURCE, NVK_PREP_INPUTS, "NVK")
    else:
        prepare_nvk_source(args.download_deps)
        if args.prepare_only:
            print(f"Prepared {SOURCE}")
            return
    rm_source = SOURCE / "src/nouveau/vulkan/nvkmd/rm"
    host_clc = ROOT / "build/nvk-host/src/compiler/clc"
    host_vtn = ROOT / "build/nvk-host/src/compiler/spirv"
    env = dict(os.environ)
    env["PATH"] = ":".join(map(str, (host_clc, host_vtn, TOOLS))) + ":" + env["PATH"]
    env["PKG_CONFIG_PATH"] = f"{DEPS}/root/usr/lib/pkgconfig:{DEPS}/root/usr/share/pkgconfig"
    env["LD_LIBRARY_PATH"] = f"{DEPS}/root/usr/lib"
    cc_launcher.host_env(env)
    common = ["-Dvulkan-drivers=nouveau", "-Dgallium-drivers=", "-Dglx=disabled", "-Degl=disabled", "-Dgbm=disabled", "-Dopengl=false", "-Dgles1=disabled", "-Dgles2=disabled", "-Dbuild-tests=false", "-Dvideo-codecs=", "-Dvalgrind=disabled", "-Dlibunwind=disabled", "-Dbuildtype=release"]
    # Android consumes source-built host mesa_clc and vtn_bindgen2.
    targets = ["host", "android"] if args.target == "both" else [args.target]
    if args.target.startswith("android") and not (host_clc / "mesa_clc").exists():
        targets.insert(0, "host")
    for target in targets:
        build = ROOT / f"build/nvk-{target}"
        options = list(common)
        target_env = dict(env)
        rust_build = ROOT / "build/nv-rm-rust"
        rust_command = ["cargo", "build", "--offline", "--no-default-features", "--target-dir", rust_build,
                        "--manifest-path", HERE.parent / "Cargo.toml"]
        if args.rust_profile == "release":
            rust_command.append("--release")
        rust_lib = profiles.rust_library(ROOT, target, args.rust_profile).parent
        bits = 32 if target == "android32" else 64
        triple = "i686-linux-android" if bits == 32 else "x86_64-linux-android"
        runtime = ROOT / "build/nvk-android32/runtime" if bits == 32 else ANDROID / "image-rootfs/system/lib64"
        if target.startswith("android"):
            rust_command += ["--target", triple]
            target_env["CARGO_TARGET_" + triple.upper().replace("-", "_") + "_LINKER"] = str(NDK / f"bin/{triple}33-clang")
        run(rust_command, env=target_env)
        linker_args = [f"-L{rust_lib}", f"-Wl,-rpath,{rust_lib}"]
        options += ["-Dc_link_args=" + json.dumps(linker_args)]
        if target == "host":
            options += ["-Dplatforms=", "-Dinstall-mesa-clc=true", "-Dcpp_link_args=" + json.dumps(linker_args)]
        else:
            # Persistent pipeline cache (nvk-android-app-disk-cache.patch) with
            # the NDK's zlib as compressor, as the nvrm build.
            cross = android_cross_file(bits, download=args.download_deps)
            write_ndk_zlib_pc(ROOT / f"build/nvk-{target}-pkgconfig")
            options += ["--cross-file", cross, "-Dplatforms=android", "-Dplatform-sdk-version=33", "-Dandroid-libbacktrace=disabled", "-Dllvm=disabled", "-Dmesa-clc=system", "-Dexpat=disabled", "-Dzstd=disabled", "-Dzlib=enabled", "-Dshader-cache=enabled"]
            options += ["-Dcpp_link_args=" + json.dumps(linker_args + ["-static-libstdc++", f"-Wl,-rpath-link,{runtime}"])]
            target_env.pop("PKG_CONFIG_PATH", None)
            target_env["BINDGEN_EXTRA_CLANG_ARGS"] = f"--sysroot={NDK}/sysroot -isystem {NDK}/sysroot/usr/include/{triple} -D__ANDROID_API__=33"
        setup = cc_launcher.setup_command(build, SOURCE)
        setup += options + ["--wrap-mode=" + ("default" if args.download_deps else "nodownload")]
        setup_run(setup, args, env=target_env)
        cc_launcher.record(build)
        artifacts = ["src/nouveau/vulkan/libvulkan_nouveau.so"]
        if target == "host":
            artifacts += ["src/compiler/clc/mesa_clc", "src/compiler/spirv/vtn_bindgen2"]
        run(["ninja", "-C", build, *artifacts, "-j", args.j, *ninja_load()], env=target_env)
        compiler = "cc" if target == "host" else NDK / f"bin/{triple}33-clang"
        run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", HERE / "inspect.c", "-ldl", "-o", build / "inspect"])
        run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", HERE / "abi-check.c", "-o", build / "abi-check"])
        for name in ("nv_rm.h", "nvkmd_rm.h", "nvkmd_rm.c"):
            if hashlib.sha256((rm_source / name).read_bytes()).digest() != hashlib.sha256((HERE / name).read_bytes()).digest():
                raise SystemExit(f"Compiled Mesa bridge copy differs from canonical {name}; rebuild")
        profiles.write_metadata(ROOT, target, args.rust_profile, rust_command,
                                subprocess.check_output(["rustc", "--version"], text=True).strip(),
                                {key: target_env[key] for key in ("RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS")
                                 if key in target_env}, sources_before)
        print(f"Built full {target} NVK (Rust {args.rust_profile}): {build}/src/nouveau/vulkan/libvulkan_nouveau.so")

def prepare_nvk_source(download):
    stamp = SOURCE / ".nv-prepared"
    if stamp.exists():
        stamp.unlink()
    patches = NVK_PATCH_LIST
    # Patches touching the same file only reverse-check on top of the stack:
    # unwind the applied ones newest first, then apply the whole stack in order.
    for patch in reversed(patches):
        if subprocess.run(["git", "-C", str(SOURCE), "apply", "--reverse", "--check", str(patch)],
                          capture_output=True).returncode == 0:
            run(["git", "-C", SOURCE, "apply", "--reverse", patch])
    for patch in patches:
        apply(patch)
    rm_source = SOURCE / "src/nouveau/vulkan/nvkmd/rm"
    rm_source.mkdir(parents=True, exist_ok=True)
    for name in ("nv_rm.h", "nvkmd_rm.h", "nvkmd_rm.c"):
        shutil.copy2(HERE / name, rm_source / name)
    cache = SOURCE / "subprojects/packagecache"
    cache.mkdir(exist_ok=True)
    for archive in (ROOT / "build/mesa-nak-source/subprojects/packagecache").glob("*.tar.gz"):
        if not (cache / archive.name).exists():
            shutil.copy2(archive, cache / archive.name)
    host_dependencies(download)
    mark_prepared(SOURCE, NVK_PREP_INPUTS)


if __name__ == "__main__":
    main()
