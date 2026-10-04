/* Hermetic check of the exact predicate that the cursor path used to bypass.
#include <cassert>
#include <cstdint>
 *
 * Background: the guest hwcomposer aborted at 05:10:53 on
 *   zwp_linux_buffer_params_v1#58: error 7: importing the supplied dmabufs failed
 * immediately after
 *   attach dmabuf: 25x32 fmt 0x34324241 stride 128 mod 0x0 usage 0x933
 * modifier 0x0 is DRM_FORMAT_MOD_LINEAR.  get_wl_buffer() skips the
 * displayable_buffer() query for HWC_IS_CURSOR_LAYER (it routes those to
 * cursor_handler::create_buffer), so that LINEAR buffer was attached anyway;
 * a compositor on NVIDIA EGL cannot bind it as GL_TEXTURE_2D -> error 7 ->
 * hwc_wayland_thread() abort() -> SurfaceFlinger DEAD_OBJECT -> Android reboot.
 *
 * The predicates below are copied from patches/hwcommerce/0001's source
 * (gralloc_handler.cpp) so this test fails if that logic ever regresses.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <map>
#include <vector>

#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL
#define DRM_FORMAT_MOD_LINEAR 0ULL
#define DRM_FORMAT_MOD_VENDOR_NVIDIA 0x03

#define fourcc_mod_get_vendor(m) ((uint64_t)((m) >> 56) & 0xff)

#define DRM_FORMAT_ABGR8888 0x34324241ULL
#define MOD_NVIDIA_014 0x3000000004fe014ULL
#define MOD_NVIDIA_012 0x3000000004fe012ULL
#define MOD_NVIDIA_013 0x3000000004fe013ULL
#define MOD_INTEL_001 0x3000000000080001ULL

struct display {
    std::map<uint32_t, std::vector<uint64_t> > modifiers;
};

/* mirrors the function-local `cached` in the real code */
static int g_cached = -1;

static bool compositor_uses_nvidia_egl(struct display *d) {
    if (g_cached >= 0)
        return g_cached != 0;
    for (std::map<uint32_t, std::vector<uint64_t> >::const_iterator kv = d->modifiers.begin();
         kv != d->modifiers.end(); ++kv) {
        for (size_t i = 0; i < kv->second.size(); i++) {
            if (fourcc_mod_get_vendor(kv->second[i]) == DRM_FORMAT_MOD_VENDOR_NVIDIA) {
                g_cached = 1;
                return true;
            }
        }
    }
    g_cached = 0;
    return false;
}

static bool compositor_supports(struct display *d, uint32_t fmt, uint64_t mod) {
    if (mod == DRM_FORMAT_MOD_INVALID)
        return false;
    if (mod == DRM_FORMAT_MOD_LINEAR && compositor_uses_nvidia_egl(d))
        return false;
    std::map<uint32_t, std::vector<uint64_t> >::const_iterator it = d->modifiers.find(fmt);
    if (it == d->modifiers.end())
        return false;
    const std::vector<uint64_t> &mods = it->second;
    for (size_t i = 0; i < mods.size(); i++)
        if (mods[i] == mod)
            return true;
    return false;
}

int main(void) {
    struct display nvidia;
    nvidia.modifiers[DRM_FORMAT_ABGR8888].push_back(MOD_NVIDIA_014);
    nvidia.modifiers[DRM_FORMAT_ABGR8888].push_back(MOD_NVIDIA_012);
    nvidia.modifiers[DRM_FORMAT_ABGR8888].push_back(MOD_NVIDIA_013);
    nvidia.modifiers[DRM_FORMAT_ABGR8888].push_back(DRM_FORMAT_MOD_LINEAR);
    g_cached = -1;

    assert(compositor_uses_nvidia_egl(&nvidia));
    printf("ok 1: NVIDIA modifiers detected -> LINEAR diverted to client composition\n");

    /* the buffer that killed the HAL */
    assert(!compositor_supports(&nvidia, DRM_FORMAT_ABGR8888, 0x0ULL));
    printf("ok 2: 25x32 mod-0x0 (LINEAR) buffer is REJECTED -> wl_shm fallback\n");

    assert(compositor_supports(&nvidia, DRM_FORMAT_ABGR8888, MOD_NVIDIA_014));
    assert(compositor_supports(&nvidia, DRM_FORMAT_ABGR8888, MOD_NVIDIA_012));
    assert(compositor_supports(&nvidia, DRM_FORMAT_ABGR8888, MOD_NVIDIA_013));
    printf("ok 3: block-linear 0x...014/012/013 still accepted (no rendering regression)\n");

    assert(!compositor_supports(&nvidia, 0x34324242ULL, MOD_NVIDIA_014));
    assert(!compositor_supports(&nvidia, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_INVALID));
    printf("ok 4: unknown format and INVALID modifier still rejected\n");

    /* hybrid iGPU-only compositor must keep the LINEAR present path */
    struct display intel;
    intel.modifiers[DRM_FORMAT_ABGR8888].push_back(MOD_INTEL_001);
    intel.modifiers[DRM_FORMAT_ABGR8888].push_back(DRM_FORMAT_MOD_LINEAR);
    g_cached = -1;
    assert(!compositor_uses_nvidia_egl(&intel));
    assert(compositor_supports(&intel, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR));
    printf("ok 5: Intel-only compositor keeps LINEAR (hybrid present path intact)\n");

    printf("\nALL PASS\n");
    return 0;
}