#!/usr/bin/env python3
"""Build the existing Android HAL over source NVK into isolated outputs."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[4]
HAL = ROOT / "nvidia-prolapse/hal"
# The existing HAL uses Vulkan 1.4 headers supplied by its pinned NDK r30.
NDK = Path("/opt/android-ndk/toolchains/llvm/prebuilt/linux-x86_64")
sources = [f"src/{name}.c" for name in
           "hal device anb ahb imgxlate util astc cmdstate astc_spv bc1_spv etc2_spv gputime".split()]
sources += ["out/gen/nvhal_gen.c", "out/gen/nvhal_astc_luts.c"]
subprocess.run(["make", "out/gen/nvhal_gen.c", "out/gen/nvhal_astc_luts.c", "out/gen/astc_spv.h", "out/gen/etc2_spv.h"],
               cwd=HAL, check=True)
common = ["-std=c11", "-D_GNU_SOURCE", "-DVK_NO_PROTOTYPES", "-DVK_USE_PLATFORM_ANDROID_KHR",
          "-DNVHAL_SOURCE_RM", "-fPIC", "-fvisibility=hidden", "-O2", "-g",
          f"-ffile-prefix-map={HAL}=nvidia-prolapse/hal", "-Wall", "-Wextra", "-Werror",
          "-Isrc", "-Iinclude/android", "-Iinclude/minigbm", "-Iout/gen", "-shared",
          "-Wl,-z,defs", "-Wl,--no-undefined", "-Wl,-soname,vulkan.nvidia.so"]
for target, compiler, flags in [
    ("host", "cc", ["-Iinclude/host", "-ldl", "-lpthread"]),
    ("android", str(NDK / "bin/x86_64-linux-android33-clang"), ["-llog", "-ldl"]),
    ("android32", str(NDK / "bin/i686-linux-android33-clang"), ["-llog", "-ldl"]),
]:
    output = ROOT / "build/source-native-integration" / target / "vulkan.nvidia.so"
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([compiler, *common, *sources, *flags, "-o", str(output)], cwd=HAL, check=True)
    print(output, flush=True)
