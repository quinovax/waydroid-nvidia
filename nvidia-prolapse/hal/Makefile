# vulkan.nvidia.so: the Android Vulkan HAL over NVIDIA's own Vulkan ICD.
# Two builds from the same sources:
#   out/host/vulkan.nvidia.so     glibc/gcc, dlopens /usr/lib/libEGL_nvidia.so.0 (or $NVHAL_ICD);
#                                 for iteration and real-pixel tests on the host GPU
#   out/android/vulkan.nvidia.so  NDK r30, API 33, x86_64; the guest's /vendor_extra/lib64/hw/
#   out/android32/vulkan.nvidia.so  NDK r30, API 33, i686; the guest's /vendor_extra/lib/hw/ (P6, 32-bit apps)
#   out/host32/vulkan.nvidia.so   gcc -m32 against lib32-glibc, dlopens /usr/lib32/libEGL_nvidia.so.0
# `make` builds all of them and runs `check` (exported-symbol contract). `make test` runs the host tests,
# `make test32` the same tests as 32-bit processes against the host's lib32 driver.
# out/gen/nvhal_gen.c is generated from vk.xml by tools/vkimage-coverage.py, which also FAILS the
# build when a command that can carry a VkImage is neither intercepted nor in a hidden extension.
# The ASTC emulation's decoder (src/astc.c) is built from Mesa's own sources at the pinned commit MESA_SHA,
# read from the git object store of $(MESA) (never from its working tree): astc_decoder.glsl -> SPIR-V with
# glslangValidator (Mesa's src/vulkan/runtime/meson.build command), texcompress_astc_luts.cpp -> LUT data
# (tools/astc-luts-gen.cpp). No binary is checked in.
# The ETC2/EAC decoder is src/etc2_decoder.comp, compiled the same way (out/gen/etc2_spv.h); its host test's reference
# is Mesa's CPU decoder src/mesa/main/texcompress_etc.c from the same pinned commit.

.DEFAULT_GOAL := all
NDK      ?= /opt/android-ndk
TC       := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64
NDK_CC   := $(TC)/bin/x86_64-linux-android33-clang
NDK_CC32 := $(TC)/bin/i686-linux-android33-clang
READELF  := $(TC)/bin/llvm-readelf
HOSTCC   ?= gcc
PYTHON   := python3 -I
VKXML    ?= /usr/share/vulkan/registry/vk.xml
HOST_VKINC ?= /usr/include/vulkan
NDK_VKINC  := $(TC)/sysroot/usr/include/vulkan
GEN_HDRS := --header-dir $(HOST_VKINC) --header-dir $(NDK_VKINC)

SRCS := src/hal.c src/device.c src/anb.c src/ahb.c src/imgxlate.c src/util.c src/astc.c src/cmdstate.c src/astc_spv.c \
        src/etc2_spv.c src/gputime.c
GEN  := out/gen/nvhal_gen.c
HDRS := src/nvhal.h src/nvhal_ahb.h src/nvhal_astc.h $(wildcard include/android/*/*.h) \
        include/minigbm/cros_gralloc_handle.h include/host/android/hardware_buffer.h

# ---- Mesa (ASTC decoder) ----
MESA     ?= ../../build/mesa-git
MESA_SHA := 0b97dcdd9424fce130054e1972a610b765644003
MESA_TAG := mesa-25.2.0
MESA_URL := https://gitlab.freedesktop.org/mesa/mesa.git
GLSLANG  ?= glslangValidator
SPIRV_VAL ?= spirv-val
HOSTCXX  ?= g++
ASTC_GEN := out/gen/nvhal_astc_luts.c out/gen/astc_spv.h out/gen/etc2_spv.h
MESA_FILES := src/compiler/glsl/astc_decoder.glsl src/util/texcompress_astc_luts.cpp src/util/texcompress_astc_luts.h \
              src/mesa/main/texcompress_astc.cpp src/mesa/main/texcompress_etc.c src/util/format/texcompress_etc_tmp.h
# the ETC2/EAC test's reference decoder (test/etc2-ref.c) and its inputs
ETC2_REF_DEPS := $(wildcard test/etc2-ref/*.h test/etc2-ref/util/*.h) out/gen/mesa/src/mesa/main/texcompress_etc.c \
                 out/gen/mesa/src/util/format/texcompress_etc_tmp.h
# a file of the pinned commit, from the object store (the checkout may be sparse or modified)
out/gen/mesa/%: | out/gen
	@mkdir -p $(dir $@)
	@git -C $(MESA) cat-file -e $(MESA_SHA):$* 2>/dev/null || { echo "Mesa $(MESA_TAG) ($(MESA_SHA)) is not in $(MESA)." \
	    "Remedy: git clone --filter=blob:none --no-checkout $(MESA_URL) $(MESA) && git -C $(MESA) fetch origin" \
	    "$(MESA_TAG) (or set MESA=<a clone that has $(MESA_SHA)>)" >&2; exit 1; }
	git -C $(MESA) show $(MESA_SHA):$* > $@.tmp && mv $@.tmp $@

# the decoder: Mesa's command line (src/vulkan/runtime/meson.build 'astc_spv.h'), validated, version recorded
out/gen/astc_spv.h: out/gen/mesa/src/compiler/glsl/astc_decoder.glsl
	@command -v $(GLSLANG) >/dev/null || { echo "$(GLSLANG) not found (glslang); it compiles Mesa's astc_decoder.glsl" >&2; exit 1; }
	$(GLSLANG) -V -S comp -o out/gen/astc.spv $<
	$(SPIRV_VAL) --target-env vulkan1.0 out/gen/astc.spv
	$(GLSLANG) -V -S comp -x -o $@ $<
	{ echo "Mesa $(MESA_TAG) $(MESA_SHA):src/compiler/glsl/astc_decoder.glsl"; $(GLSLANG) --version | head -1; } \
	    > out/gen/astc_spv.version

# the ETC2/EAC decoder (this tree's source), validated, version recorded
out/gen/etc2_spv.h: src/etc2_decoder.comp | out/gen
	@command -v $(GLSLANG) >/dev/null || { echo "$(GLSLANG) not found (glslang); it compiles src/etc2_decoder.comp" >&2; exit 1; }
	$(GLSLANG) -V -S comp -o out/gen/etc2.spv $<
	$(SPIRV_VAL) --target-env vulkan1.0 out/gen/etc2.spv
	$(GLSLANG) -V -S comp -x -o $@ $<
	$(GLSLANG) --version | head -1 > out/gen/etc2_spv.version

out/gen/astc-luts-gen: tools/astc-luts-gen.cpp out/gen/mesa/src/util/texcompress_astc_luts.cpp \
                       out/gen/mesa/src/util/texcompress_astc_luts.h | out/gen
	$(HOSTCXX) -std=c++17 -O1 -Wall -Iout/gen/mesa/src/util -o $@ tools/astc-luts-gen.cpp \
	    out/gen/mesa/src/util/texcompress_astc_luts.cpp
out/gen/nvhal_astc_luts.c: out/gen/astc-luts-gen
	./out/gen/astc-luts-gen $@ $(MESA_SHA)

# Path-independent output: the same sources give the same bytes (debug info included) from any
# checkout path, e.g. the repo and packaging/arch/src/ (the package build).
HERE       := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
PREFIX_MAP := -ffile-prefix-map=$(HERE)=nvidia-prolapse/hal
COMMON_CFLAGS := -std=c11 -D_GNU_SOURCE -DVK_NO_PROTOTYPES -DVK_USE_PLATFORM_ANDROID_KHR -fPIC -fvisibility=hidden \
                 -O2 -g $(PREFIX_MAP) -Wall -Wextra -Werror -Isrc -Iinclude/android -Iinclude/minigbm -Iout/gen
LDFLAGS_SO    := -shared -Wl,-z,defs -Wl,--no-undefined -Wl,-soname,vulkan.nvidia.so

.PHONY: all host android android32 host32 check coverage test test32 driver-check syncfd-check hal-fence-check clean
all: host android android32 host32 check
host: out/host/vulkan.nvidia.so out/host/anb-roundtrip out/host/ahb-roundtrip out/host/ext-duplicates \
      out/host/ext-duplicates-asan out/host/loader-pd out/host/astc-roundtrip out/host/etc2-roundtrip out/host/ahb-yuv
android: out/android/vulkan.nvidia.so out/android/ext-duplicates
android32: out/android32/vulkan.nvidia.so out/android32/ext-duplicates
host32: out/host32/vulkan.nvidia.so out/host32/anb-roundtrip out/host32/ahb-roundtrip out/host32/ext-duplicates \
        out/host32/ext-duplicates-asan out/host32/loader-pd out/host32/astc-roundtrip out/host32/etc2-roundtrip

out/host out/android out/android32 out/host32 out/gen:
	mkdir -p $@

# vk.xml -> sType sizes, hidden image extensions, known extensions; and the VkImage coverage gate
$(GEN): tools/vkimage-coverage.py tools/image-hidden-exts.txt src/hal.c src/imgxlate.c $(VKXML) | out/gen
	$(PYTHON) tools/vkimage-coverage.py --xml $(VKXML) --hal-c src/hal.c --hal-c src/imgxlate.c \
	    --hidden tools/image-hidden-exts.txt $(GEN_HDRS) --out $@
coverage: tools/vkimage-coverage.py
	$(PYTHON) tools/vkimage-coverage.py --xml $(VKXML) --hal-c src/hal.c --hal-c src/imgxlate.c \
	    --hidden tools/image-hidden-exts.txt $(GEN_HDRS) --out $(GEN) --report

# Host: system Vulkan headers (/usr/include/vulkan), no libvulkan link; the NDK's
# <android/hardware_buffer.h> types come from include/host (subset copy).
out/host/vulkan.nvidia.so: $(SRCS) $(GEN) $(ASTC_GEN) $(HDRS) | out/host
	$(HOSTCC) $(COMMON_CFLAGS) -Iinclude/host $(LDFLAGS_SO) -o $@ $(SRCS) $(GEN) out/gen/nvhal_astc_luts.c -ldl -lpthread

# Android: NDK sysroot Vulkan headers; liblog for logcat. libnativewindow is dlopened (ahb.c).
out/android/vulkan.nvidia.so: $(SRCS) $(GEN) $(ASTC_GEN) $(HDRS) | out/android
	$(NDK_CC) $(COMMON_CFLAGS) $(LDFLAGS_SO) -o $@ $(SRCS) $(GEN) out/gen/nvhal_astc_luts.c -llog -ldl

# 32-bit x86: the same sources. On LP32 every non-dispatchable handle is a uint64_t, not a pointer: the code
# converts them with NVHAL_ND_U64/NVHAL_U64_ND (nvhal.h), never through uintptr_t.
out/android32/vulkan.nvidia.so: $(SRCS) $(GEN) $(ASTC_GEN) $(HDRS) | out/android32
	$(NDK_CC32) $(COMMON_CFLAGS) $(LDFLAGS_SO) -o $@ $(SRCS) $(GEN) out/gen/nvhal_astc_luts.c -llog -ldl
out/android32/ext-duplicates: test/ext-duplicates.c $(wildcard include/android/*/*.h) | out/android32
	$(NDK_CC32) $(TEST_CFLAGS) -o $@ test/ext-duplicates.c -ldl

# Host 32-bit (gcc -m32, lib32-glibc): real pixels on the host GPU through the lib32 ICD.
out/host32/vulkan.nvidia.so: $(SRCS) $(GEN) $(ASTC_GEN) $(HDRS) | out/host32
	$(HOSTCC) -m32 $(COMMON_CFLAGS) -Iinclude/host $(LDFLAGS_SO) -o $@ $(SRCS) $(GEN) out/gen/nvhal_astc_luts.c -ldl -lpthread
out/host32/anb-roundtrip: test/anb-roundtrip.c include/minigbm/cros_gralloc_handle.h $(wildcard include/android/*/*.h) | out/host32
	$(HOSTCC) -m32 $(TEST_CFLAGS) -o $@ test/anb-roundtrip.c -ldl
out/host32/loader-pd: test/loader-pd.c $(wildcard include/android/*/*.h) | out/host32
	$(HOSTCC) -m32 $(TEST_CFLAGS) -o $@ test/loader-pd.c -ldl
out/host32/ahb-roundtrip: test/ahb-roundtrip.c src/nvhal_ahb.h include/host/android/hardware_buffer.h \
                          include/minigbm/cros_gralloc_handle.h | out/host32
	$(HOSTCC) -m32 $(TEST_CFLAGS) -rdynamic -o $@ test/ahb-roundtrip.c -ldl
out/host32/ext-duplicates: test/ext-duplicates.c $(wildcard include/android/*/*.h) | out/host32
	$(HOSTCC) -m32 $(TEST_CFLAGS) -o $@ test/ext-duplicates.c -ldl
out/host32/ext-duplicates-asan: test/ext-duplicates.c $(wildcard include/android/*/*.h) | out/host32
	$(HOSTCC) -m32 $(TEST_CFLAGS) -fsanitize=address -fno-omit-frame-pointer -o $@ test/ext-duplicates.c -ldl
out/host32/astc-ref.o: test/astc-ref.cpp $(wildcard test/astc-ref/*.h test/astc-ref/util/*.h) \
                       out/gen/mesa/src/mesa/main/texcompress_astc.cpp | out/host32
	$(HOSTCXX) -m32 -std=c++17 -O1 -g $(PREFIX_MAP) -Itest/astc-ref -Iout/gen/mesa/src/mesa/main -c -o $@ test/astc-ref.cpp
out/host32/astc-roundtrip: test/astc-roundtrip.c out/host32/astc-ref.o out/gen/astc_sample_spv.h \
                           $(wildcard include/android/*/*.h) | out/host32
	$(HOSTCC) -m32 $(TEST_CFLAGS) -Iout/gen -o $@ test/astc-roundtrip.c out/host32/astc-ref.o -ldl -lstdc++
out/host32/etc2-ref.o: test/etc2-ref.c $(ETC2_REF_DEPS) | out/host32
	$(HOSTCC) -m32 $(ETC2_REF_CFLAGS) -c -o $@ test/etc2-ref.c
out/host32/etc2-roundtrip: test/etc2-roundtrip.c out/host32/etc2-ref.o out/gen/etc2_sample_spv.h \
                           $(wildcard include/android/*/*.h) | out/host32
	$(HOSTCC) -m32 $(TEST_CFLAGS) -Iout/gen -o $@ test/etc2-roundtrip.c out/host32/etc2-ref.o -ldl

TEST_CFLAGS := -std=c11 -D_GNU_SOURCE -DVK_NO_PROTOTYPES -DVK_USE_PLATFORM_ANDROID_KHR -O1 -g $(PREFIX_MAP) -Wall -Wextra -Werror \
               -Isrc -Iinclude/host -Iinclude/android -Iinclude/minigbm
out/host/anb-roundtrip: test/anb-roundtrip.c include/minigbm/cros_gralloc_handle.h $(wildcard include/android/*/*.h) | out/host
	$(HOSTCC) $(TEST_CFLAGS) -o $@ test/anb-roundtrip.c -ldl

out/host/loader-pd: test/loader-pd.c $(wildcard include/android/*/*.h) | out/host
	$(HOSTCC) $(TEST_CFLAGS) -o $@ test/loader-pd.c -ldl

# sync_fd export after a first-op import-wait on a fresh queue (Vulkan analogue of egl/ fence-first-wait, D1)
out/host/syncfd-probe: test/syncfd-probe.c | out/host
	$(HOSTCC) $(TEST_CFLAGS) -o $@ test/syncfd-probe.c -ldl
out/host/vk-first-wait: test/vk-first-wait.c $(wildcard include/android/*/*.h) | out/host
	$(HOSTCC) $(TEST_CFLAGS) -o $@ test/vk-first-wait.c -ldl -lpthread

# ASTC emulation pixel test: the reference is Mesa's CPU ASTC decoder (pinned src/mesa/main/texcompress_astc.cpp,
# with the test-only stand-in headers of test/astc-ref/), the sampling shader is compiled from test/astc-sample.comp
out/gen/astc_sample_spv.h: test/astc-sample.comp | out/gen
	$(GLSLANG) -V -S comp -x -o $@ $<
out/host/astc-ref.o: test/astc-ref.cpp $(wildcard test/astc-ref/*.h test/astc-ref/util/*.h) \
                     out/gen/mesa/src/mesa/main/texcompress_astc.cpp | out/host
	$(HOSTCXX) -std=c++17 -O1 -g $(PREFIX_MAP) -Itest/astc-ref -Iout/gen/mesa/src/mesa/main -c -o $@ test/astc-ref.cpp
out/host/astc-roundtrip: test/astc-roundtrip.c out/host/astc-ref.o out/gen/astc_sample_spv.h \
                         $(wildcard include/android/*/*.h) | out/host
	$(HOSTCC) $(TEST_CFLAGS) -Iout/gen -o $@ test/astc-roundtrip.c out/host/astc-ref.o -ldl -lstdc++

# ETC2/EAC emulation pixel test: the reference is Mesa's CPU ETC2/EAC decoder (pinned src/mesa/main/texcompress_etc.c +
# src/util/format/texcompress_etc_tmp.h, with the test-only stand-in headers of test/etc2-ref/), the sampling shader is
# compiled from test/etc2-sample.comp
ETC2_REF_CFLAGS := -std=c11 -O1 -g $(PREFIX_MAP) -Wall -Wextra -Werror -Itest/etc2-ref -Iout/gen/mesa/src \
                   -Iout/gen/mesa/src/mesa/main
out/gen/etc2_sample_spv.h: test/etc2-sample.comp | out/gen
	$(GLSLANG) -V -S comp -x -o $@ $<
out/host/etc2-ref.o: test/etc2-ref.c $(ETC2_REF_DEPS) | out/host
	$(HOSTCC) $(ETC2_REF_CFLAGS) -c -o $@ test/etc2-ref.c
out/host/etc2-roundtrip: test/etc2-roundtrip.c out/host/etc2-ref.o out/gen/etc2_sample_spv.h \
                         $(wildcard include/android/*/*.h) | out/host
	$(HOSTCC) $(TEST_CFLAGS) -Iout/gen -o $@ test/etc2-roundtrip.c out/host/etc2-ref.o -ldl

# -rdynamic: the HAL finds the fake AHardwareBuffer table (nvhal_test_ahb_fns) with dlsym(RTLD_DEFAULT)
out/host/ahb-roundtrip: test/ahb-roundtrip.c src/nvhal_ahb.h include/host/android/hardware_buffer.h \
                        include/minigbm/cros_gralloc_handle.h | out/host
	$(HOSTCC) $(TEST_CFLAGS) -rdynamic -o $@ test/ahb-roundtrip.c -ldl

# NV12/P010 AHB import + Ycbcr sampling against a CPU reference (fake minigbm gbm_mesa NV12/P010 handles)
out/gen/ahb_yuv_spv.h: test/ahb-yuv.comp | out/gen
	$(GLSLANG) -V --target-env vulkan1.1 -o out/gen/ahb-yuv.spv $<
	$(SPIRV_VAL) --target-env vulkan1.1 out/gen/ahb-yuv.spv
	$(GLSLANG) -V --target-env vulkan1.1 --vn ahb_yuv_spv -o $@ $<
out/host/ahb-yuv: test/ahb-yuv.c out/gen/ahb_yuv_spv.h src/nvhal_ahb.h include/host/android/hardware_buffer.h \
                  include/minigbm/cros_gralloc_handle.h | out/host
	$(HOSTCC) $(TEST_CFLAGS) -Iout/gen -rdynamic -o $@ test/ahb-yuv.c -ldl -lm

# Duplicate ppEnabledExtensionNames (dEQP-VK.api.extension_duplicates.*): host build under ASan for the
# HAL and the raw ICD; Android build for the guest (--sphal / --loader libvulkan.so, with
# LIBC_DEBUG_MALLOC_OPTIONS="guard backtrace=8" as the ASan stand-in). The guest's AOSP loader is the
# component that overflows (reference/abi-contract.md §10); run it to re-check after an image update.
out/host/ext-duplicates: test/ext-duplicates.c $(wildcard include/android/*/*.h) | out/host
	$(HOSTCC) $(TEST_CFLAGS) -o $@ test/ext-duplicates.c -ldl
out/host/ext-duplicates-asan: test/ext-duplicates.c $(wildcard include/android/*/*.h) | out/host
	$(HOSTCC) $(TEST_CFLAGS) -fsanitize=address -fno-omit-frame-pointer -o $@ test/ext-duplicates.c -ldl
out/android/ext-duplicates: test/ext-duplicates.c $(wildcard include/android/*/*.h) | out/android
	$(NDK_CC) $(TEST_CFLAGS) -o $@ test/ext-duplicates.c -ldl

# The command-buffer lookup cache is dynamic (general/local-dynamic model) TLS: __tls_get_addr comes from
# bionic's libc (LIBC_Q) on Android and from ld-linux on glibc; no static TLS in either build.
check: out/android/vulkan.nvidia.so out/android32/vulkan.nvidia.so out/host/vulkan.nvidia.so \
       out/host32/vulkan.nvidia.so tools/check-exports.py
	$(PYTHON) tools/check-exports.py --readelf $(READELF) --allow-needed liblog.so,libdl.so,libc.so \
	    out/android/vulkan.nvidia.so
	$(PYTHON) tools/check-exports.py --readelf $(READELF) --allow-needed liblog.so,libdl.so,libc.so --hmi-size 128 \
	    out/android32/vulkan.nvidia.so
	$(PYTHON) tools/check-exports.py --readelf $(READELF) --allow-needed libc.so.6,ld-linux.so.2 --hmi-size 128 \
	    out/host32/vulkan.nvidia.so
	$(PYTHON) tools/check-exports.py --readelf $(READELF) --allow-needed libc.so.6,ld-linux-x86-64.so.2 \
	    out/host/vulkan.nvidia.so

# Real GPU, real pixels. Headless: no X11/Wayland involved.
# Exported sync_fds must cover the work before them (egl/FENCE-CONTRACT.md §3.5 is the defect D1, §6a the invariant).
# Every vk-first-wait mode (test/vk-first-wait.c MODES[]; --list-modes):
VKFW_MODES := sem-export fence-export hal-seq hal-seq-m1first fence-import-cpu no-import sem-export-warm fence-export-warm \
              hal-seq-warm mq-fresh-g1 mq-warm-g0-use-g1 mq-warm-g1-use-g1 mq-warm-g0-use-g15 mq-fresh-c0 mq-warm-g0-use-c0 \
              mq-warm-all-use-c0 mq-warm-all-use-g1 warm-qidle warm-didle warm-thread warm-q2 sem-warm sem-warm-sem-export
VKFW_ICD ?= /usr/lib/libEGL_nvidia.so.0
DRIVER_CHECK_ROUNDS ?= 10
HAL_FENCE_ROUNDS ?= 20
# driver-check: the RAW ICD, nothing of ours. A detector of the driver defect: it FAILS on 615.71.09 (24/24 early in
# sem-export, fence-export, hal-seq, mq-fresh-g1, mq-warm-g0-use-g1, mq-warm-g0-use-g15, mq-fresh-c0, mq-warm-g0-use-c0;
# 0 in the others) and passes only on a driver that keeps the contract.
driver-check: out/host/vk-first-wait
	@fail=0; for m in $(VKFW_MODES); do \
	    out=$$(DISPLAY= WAYLAND_DISPLAY= ./out/host/vk-first-wait --icd $(VKFW_ICD) --mode $$m --rounds $(DRIVER_CHECK_ROUNDS) \
	        --fills 60 --timestamps); rc=$$?; echo "$$out" | tail -1; [ $$rc -eq 0 ] || fail=1; done; exit $$fail
# syncfd-check: the RAW ICD, nothing of ours. The two driver defects with imported SYNC_FD payloads that make
# vkAcquireImageANDROID wait for the native fence (src/device.c, acquire_wait_flags); it also says whether the HAL's
# decision for this branch covers what it measured. Run it after every driver bump.
syncfd-check: out/host/syncfd-probe
	DISPLAY= WAYLAND_DISPLAY= ./out/host/syncfd-probe --icd $(VKFW_ICD)
# hal-fence-check: the same modes through our HAL, which starts every queue's timeline at vkCreateDevice
# (AHB-CONTRACT.md §8). Must be 0 early in every mode; the two semaphore-only runs exercise the HAL's semaphore start.
hal-fence-check: out/host/vulkan.nvidia.so out/host/vk-first-wait
	@fail=0; run() { out=$$(DISPLAY= WAYLAND_DISPLAY= ./out/host/vk-first-wait --hal out/host/vulkan.nvidia.so \
	        --rounds $(HAL_FENCE_ROUNDS) --fills 60 --timestamps "$$@"); rc=$$?; echo "$$out" | tail -1 | sed "s/\$$/ [$$*]/"; \
	        [ $$rc -eq 0 ] || fail=1; }; \
	    for m in $(VKFW_MODES); do run --mode $$m; done; \
	    run --mode sem-export --no-fence-ext --no-anb; run --mode sem-warm-sem-export --no-fence-ext --no-anb; exit $$fail

test: out/host/vulkan.nvidia.so out/host/anb-roundtrip out/host/ahb-roundtrip out/host/ext-duplicates-asan \
      out/host/loader-pd out/host/astc-roundtrip out/host/etc2-roundtrip out/host/ahb-yuv
	DISPLAY= WAYLAND_DISPLAY= ASAN_OPTIONS=detect_leaks=0 ./out/host/ext-duplicates-asan --hal out/host/vulkan.nvidia.so --dup 3
	DISPLAY= WAYLAND_DISPLAY= ASAN_OPTIONS=detect_leaks=0 ./out/host/ext-duplicates-asan --icd /usr/lib/libEGL_nvidia.so.0 --dup 3
	DISPLAY= WAYLAND_DISPLAY= ./out/host/anb-roundtrip out/host/vulkan.nvidia.so
	DISPLAY= WAYLAND_DISPLAY= ./out/host/anb-roundtrip out/host/vulkan.nvidia.so --api 1.0
	DISPLAY= WAYLAND_DISPLAY= ./out/host/anb-roundtrip out/host/vulkan.nvidia.so --linear
	DISPLAY= WAYLAND_DISPLAY= ./out/host/ahb-roundtrip out/host/vulkan.nvidia.so
	DISPLAY= WAYLAND_DISPLAY= ./out/host/ahb-roundtrip out/host/vulkan.nvidia.so --linear
	DISPLAY= WAYLAND_DISPLAY= ./out/host/ahb-roundtrip out/host/vulkan.nvidia.so --api 1.0
	DISPLAY= WAYLAND_DISPLAY= ./out/host/ahb-yuv out/host/vulkan.nvidia.so
	DISPLAY= WAYLAND_DISPLAY= ./out/host/loader-pd out/host/vulkan.nvidia.so
	DISPLAY= WAYLAND_DISPLAY= ./out/host/astc-roundtrip out/host/vulkan.nvidia.so
	DISPLAY= WAYLAND_DISPLAY= ./out/host/astc-roundtrip out/host/vulkan.nvidia.so --api 1.0
	DISPLAY= WAYLAND_DISPLAY= ./out/host/etc2-roundtrip out/host/vulkan.nvidia.so
	DISPLAY= WAYLAND_DISPLAY= ./out/host/etc2-roundtrip out/host/vulkan.nvidia.so --api 1.0

ICD32 ?= /usr/lib32/libEGL_nvidia.so.0
test32: out/host32/vulkan.nvidia.so out/host32/anb-roundtrip out/host32/ahb-roundtrip out/host32/ext-duplicates-asan \
        out/host32/loader-pd out/host32/astc-roundtrip out/host32/etc2-roundtrip
	DISPLAY= WAYLAND_DISPLAY= ASAN_OPTIONS=detect_leaks=0 ./out/host32/ext-duplicates-asan --hal out/host32/vulkan.nvidia.so --dup 3
	DISPLAY= WAYLAND_DISPLAY= ASAN_OPTIONS=detect_leaks=0 ./out/host32/ext-duplicates-asan --icd $(ICD32) --dup 3
	DISPLAY= WAYLAND_DISPLAY= ./out/host32/anb-roundtrip out/host32/vulkan.nvidia.so
	DISPLAY= WAYLAND_DISPLAY= ./out/host32/anb-roundtrip out/host32/vulkan.nvidia.so --api 1.0
	DISPLAY= WAYLAND_DISPLAY= ./out/host32/anb-roundtrip out/host32/vulkan.nvidia.so --linear
	DISPLAY= WAYLAND_DISPLAY= ./out/host32/ahb-roundtrip out/host32/vulkan.nvidia.so
	DISPLAY= WAYLAND_DISPLAY= ./out/host32/ahb-roundtrip out/host32/vulkan.nvidia.so --linear
	DISPLAY= WAYLAND_DISPLAY= ./out/host32/ahb-roundtrip out/host32/vulkan.nvidia.so --api 1.0
	DISPLAY= WAYLAND_DISPLAY= ./out/host32/loader-pd out/host32/vulkan.nvidia.so
	DISPLAY= WAYLAND_DISPLAY= ./out/host32/astc-roundtrip out/host32/vulkan.nvidia.so
	DISPLAY= WAYLAND_DISPLAY= ./out/host32/astc-roundtrip out/host32/vulkan.nvidia.so --api 1.0
	DISPLAY= WAYLAND_DISPLAY= ./out/host32/etc2-roundtrip out/host32/vulkan.nvidia.so
	DISPLAY= WAYLAND_DISPLAY= ./out/host32/etc2-roundtrip out/host32/vulkan.nvidia.so --api 1.0

clean:
	rm -rf out
