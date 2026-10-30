/* VK_ANDROID_external_memory_android_hardware_buffer (spec version 5) on top of NVIDIA's dma-buf +
 * explicit DRM format modifier import, for minigbm buffers (struct cros_gralloc_handle).
 *
 * The consumer contract (what ANGLE and Skia/HWUI call, in which order) is AHB-CONTRACT.md; Mesa
 * 25.2.0 src/vulkan/runtime/vk_android.c is the semantic reference.
 *
 * An AHB image's layout is only known once its buffer is: vkCreateImage with the AHB handle type
 * returns a HAL-owned wrapper handle (struct nvhal_image), and the ICD image is created
 * ("realised") at vkAllocateMemory time, from the buffer's fd / modifier / stride / offset, with
 * VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT. Every command that can carry a VkImage translates a
 * wrapper to its ICD image (imgxlate.c; tools/vkimage-coverage.py proves the list complete
 * against vk.xml). */
#include "nvhal.h"
#include "nvhal_ahb.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <drm/drm_fourcc.h>
#include <cros_gralloc_handle.h>

/* ------------------------------------------------------------------ entry points */

bool nvhal_ahb_available;
static struct nvhal_ahb_fns ahb;
static char ahb_why[512] = "not loaded";

void nvhal_ahb_load(void) {
    if (nvhal_ahb_available) return;
#ifdef __ANDROID__
    dlerror();
    void *h = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *e = dlerror();
        snprintf(ahb_why, sizeof ahb_why, "dlopen(libnativewindow.so) in the HAL's linker namespace failed: %s",
                 e ? e : "?");
        return;
    }
    static const char *const names[] = {"AHardwareBuffer_describe", "AHardwareBuffer_getNativeHandle",
                                        "AHardwareBuffer_acquire", "AHardwareBuffer_release",
                                        "AHardwareBuffer_allocate"};
    void *syms[5];
    for (int i = 0; i < 5; i++) {
        syms[i] = dlsym(h, names[i]);
        if (!syms[i]) {
            snprintf(ahb_why, sizeof ahb_why, "libnativewindow.so has no %s (LLNDK symbol expected since API 26)",
                     names[i]);
            return; /* the library handle stays open: harmless, it is loaded by libvulkan anyway */
        }
    }
    memcpy(&ahb.describe, &syms[0], sizeof(void *));
    memcpy(&ahb.get_native_handle, &syms[1], sizeof(void *));
    memcpy(&ahb.acquire, &syms[2], sizeof(void *));
    memcpy(&ahb.release, &syms[3], sizeof(void *));
    memcpy(&ahb.allocate, &syms[4], sizeof(void *));
#else
    const struct nvhal_ahb_fns *t = dlsym(RTLD_DEFAULT, NVHAL_TEST_AHB_FNS_SYMBOL);
    if (!t || !t->describe || !t->get_native_handle || !t->acquire || !t->release || !t->allocate) {
        snprintf(ahb_why, sizeof ahb_why, "host build: the process exports no complete `struct nvhal_ahb_fns %s` "
                 "(only test/ahb-roundtrip provides one)", NVHAL_TEST_AHB_FNS_SYMBOL);
        return;
    }
    ahb = *t;
#endif
    nvhal_ahb_available = true;
    NVHAL_TRACE("AHardwareBuffer entry points loaded; VK_ANDROID_external_memory_android_hardware_buffer offered");
}

void nvhal_ahb_report_unavailable(void) {
    static atomic_bool reported;
    if (atomic_exchange(&reported, true)) return;
#ifdef __ANDROID__
    NVHAL_ERR("VK_ANDROID_external_memory_android_hardware_buffer is not offered: %s. ANGLE then cannot import "
              "GraphicBuffers (EGL_NATIVE_BUFFER_ANDROID) and HWUI's Vulkan renderer cannot wrap window buffers. "
              "Remedy: libnativewindow.so (LLNDK) must be reachable from the sphal namespace (reported once per "
              "process)", ahb_why);
#else
    NVHAL_TRACE("VK_ANDROID_external_memory_android_hardware_buffer not offered: %s", ahb_why);
#endif
}

/* ------------------------------------------------------------------ formats */

/* AHardwareBuffer format -> VkFormat: the spec's "AHardwareBuffer Format Equivalence" table (as
 * Mesa vk_android.c vk_ahb_format_to_image_format), plus HAL_PIXEL_FORMAT_BGRA_8888 (= 5). */
static VkFormat ahb_to_vk(uint32_t f) {
    switch (f) {
    case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
    case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8_UNORM;
    case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case AHARDWAREBUFFER_FORMAT_D16_UNORM: return VK_FORMAT_D16_UNORM;
    case AHARDWAREBUFFER_FORMAT_D24_UNORM: return VK_FORMAT_X8_D24_UNORM_PACK32;
    case AHARDWAREBUFFER_FORMAT_D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
    case AHARDWAREBUFFER_FORMAT_D32_FLOAT: return VK_FORMAT_D32_SFLOAT;
    case AHARDWAREBUFFER_FORMAT_D32_FLOAT_S8_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case AHARDWAREBUFFER_FORMAT_S8_UINT: return VK_FORMAT_S8_UINT;
    case AHARDWAREBUFFER_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
    case NVHAL_AHB_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/* What the gralloc backend can allocate (nvidia-prolapse/gralloc/nvidia_gbm_wrapper.c nvg_formats): DRM fourcc of
 * the buffer -> VkFormat of its memory, and the AHB format an export allocates for a VkFormat. */
static const struct { uint32_t fourcc; VkFormat vk; uint32_t ahb; } gralloc_formats[] = {
    {DRM_FORMAT_ABGR8888, VK_FORMAT_R8G8B8A8_UNORM, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM},
    {DRM_FORMAT_XBGR8888, VK_FORMAT_R8G8B8A8_UNORM, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM},
    {DRM_FORMAT_ARGB8888, VK_FORMAT_B8G8R8A8_UNORM, NVHAL_AHB_FORMAT_B8G8R8A8_UNORM},
    {DRM_FORMAT_XRGB8888, VK_FORMAT_B8G8R8A8_UNORM, NVHAL_AHB_FORMAT_B8G8R8A8_UNORM},
    {DRM_FORMAT_RGB565, VK_FORMAT_R5G6B5_UNORM_PACK16, AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM},
    {DRM_FORMAT_ABGR2101010, VK_FORMAT_A2B10G10R10_UNORM_PACK32, AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM},
    {DRM_FORMAT_ABGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT, AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT},
    {DRM_FORMAT_R8, VK_FORMAT_R8_UNORM, AHARDWAREBUFFER_FORMAT_R8_UNORM},
};
#define N_OF(a) (sizeof(a) / sizeof *(a))

static VkFormat fourcc_to_vk(uint32_t fourcc) {
    for (size_t i = 0; i < N_OF(gralloc_formats); i++)
        if (gralloc_formats[i].fourcc == fourcc) return gralloc_formats[i].vk;
    return VK_FORMAT_UNDEFINED;
}

/* Bi-planar YUV buffers the gralloc lays out on its R8 carrier (minigbm gbm_mesa spoofed formats, LINEAR,
 * gralloc/minigbm/0004: byte pitch aligned to 256, CbCr plane at pitch * even(height), one dma-buf). They are
 * import-only external formats: vkGetAndroidHardwareBufferPropertiesANDROID reports format UNDEFINED and the
 * VkFormat below as externalFormat (Mesa vk_android.c get_ahb_buffer_format_properties2: NV12 ->
 * G8_B8R8_2PLANE_420_UNORM), and the image is realised with two explicit LINEAR plane layouts. */
static const struct { uint32_t fourcc; VkFormat vk; } yuv_formats[] = {
    {DRM_FORMAT_NV12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM},
    {DRM_FORMAT_P010, VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16},
};

static VkFormat yuv_fourcc_to_vk(uint32_t fourcc) {
    for (size_t i = 0; i < N_OF(yuv_formats); i++)
        if (yuv_formats[i].fourcc == fourcc) return yuv_formats[i].vk;
    return VK_FORMAT_UNDEFINED;
}

static bool yuv_vk(VkFormat f) {
    for (size_t i = 0; i < N_OF(yuv_formats); i++)
        if (yuv_formats[i].vk == f) return true;
    return false;
}

/* AHB format to allocate for an image of this VkFormat (0: the gralloc cannot allocate it) */
static uint32_t vk_to_ahb(VkFormat f) {
    for (size_t i = 0; i < N_OF(gralloc_formats); i++)
        if (gralloc_formats[i].vk == f) return gralloc_formats[i].ahb;
    return 0;
}

/* VkFormats this HAL hands out as externalFormat: every importable buffer format */
static bool external_format_ok(VkFormat f) { return vk_to_ahb(f) != 0 || yuv_vk(f); }

/* "AHardwareBuffer Usage Equivalence" (spec; Mesa vk_image_usage_to_ahb_usage) */
static uint64_t vk_to_ahb_usage(VkImageCreateFlags flags, VkImageUsageFlags usage) {
    uint64_t u = 0;
    if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
        u |= AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
        u |= AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT) u |= AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
    if (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) u |= AHARDWAREBUFFER_USAGE_GPU_CUBE_MAP;
    if (flags & VK_IMAGE_CREATE_PROTECTED_BIT) u |= AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT;
    if (!u) u = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    return u;
}

/* ------------------------------------------------------------------ buffer description */

struct gbuf {
    AHardwareBuffer_Desc desc;
    const struct cros_gralloc_handle *g;
    int fd;
    uint64_t size;   /* dma-buf size (lseek) */
    VkFormat format; /* VK_FORMAT_UNDEFINED for BLOB */
    bool blob;
    bool yuv;        /* NV12/P010: two planes in one dma-buf, LINEAR */
    uint32_t planes; /* memory planes of the layout (1, or 2 for yuv) */
    uint32_t strides[2], offsets[2];
    char fcc[5];
};
#define GB_FACTS "AHB %ux%u layers %u format 0x%x usage 0x%" PRIx64 " stride %u px; handle fourcc %s (0x%08x) " \
                 "modifier 0x%016" PRIx64 " plane0 stride %u B offset %u, planes %u, %llu B dma-buf"
#define GB_ARGS(b) (b)->desc.width, (b)->desc.height, (b)->desc.layers, (b)->desc.format, (b)->desc.usage, \
                   (b)->desc.stride, (b)->fcc, (b)->g->format, (b)->g->format_modifier, (b)->g->strides[0], \
                   (b)->g->offsets[0], (b)->g->num_planes, (unsigned long long)(b)->size

static VkResult gbuf_get(const AHardwareBuffer *buf, const char *who, struct gbuf *b) {
    memset(b, 0, sizeof *b);
    if (!buf) {
        NVHAL_ERR("%s: AHardwareBuffer is NULL -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    ahb.describe(buf, &b->desc);
    VkResult r = nvhal_cros_handle(ahb.get_native_handle(buf), who, &b->g);
    if (r != VK_SUCCESS) return r;
    nvhal_fourcc_str(b->g->format, b->fcc);
    b->fd = b->g->fds[0];
    off_t sz = b->fd >= 0 ? lseek(b->fd, 0, SEEK_END) : -1;
    if (sz < 0 || lseek(b->fd, 0, SEEK_SET) < 0) {
        NVHAL_ERR("%s: lseek on the buffer's dma-buf fd %d failed (%s); its size is unknown -> "
                  "VK_ERROR_INVALID_EXTERNAL_HANDLE", who, b->fd, strerror(errno));
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    b->size = (uint64_t)sz;
    b->blob = b->desc.format == AHARDWAREBUFFER_FORMAT_BLOB;
    if (b->blob) return VK_SUCCESS;
    if (b->g->num_planes != 1) {
        VkFormat yf = yuv_fourcc_to_vk(b->g->format);
        if (yf == VK_FORMAT_UNDEFINED || b->g->num_planes != 2) {
            NVHAL_ERR("%s: multi-planar buffer of DRM format %s with %u planes: only NV12 and P010 (2 planes) are "
                      "implemented; " GB_FACTS " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, b->fcc, b->g->num_planes,
                      GB_ARGS(b));
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        if (b->g->format_modifier != DRM_FORMAT_MOD_LINEAR) {
            NVHAL_ERR("%s: %s buffer with modifier 0x%016" PRIx64 ": multi-planar formats are imported LINEAR only "
                      "(the driver lists only DRM_FORMAT_MOD_LINEAR for them); " GB_FACTS
                      " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, b->fcc, b->g->format_modifier, GB_ARGS(b));
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        struct stat s0, s1;
        if (b->g->fds[1] < 0 || fstat(b->fd, &s0) != 0 || fstat(b->g->fds[1], &s1) != 0 || s0.st_dev != s1.st_dev ||
            s0.st_ino != s1.st_ino) {
            NVHAL_ERR("%s: %s plane 1 fd %d is not the plane 0 dma-buf (fd %d): planes in separate buffers "
                      "(disjoint) are not implemented; " GB_FACTS " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, b->fcc,
                      b->g->fds[1], b->fd, GB_ARGS(b));
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        const uint64_t rows[2] = {b->desc.height, (b->desc.height + 1u) / 2u};
        for (uint32_t p = 0; p < 2; p++) {
            if (!b->g->strides[p] || (uint64_t)b->g->offsets[p] + (uint64_t)b->g->strides[p] * rows[p] > b->size) {
                NVHAL_ERR("%s: %s plane %u (offset %u, stride %u B, %llu rows) does not fit the %llu B dma-buf; "
                          GB_FACTS " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, b->fcc, p, b->g->offsets[p],
                          b->g->strides[p], (unsigned long long)rows[p], (unsigned long long)b->size, GB_ARGS(b));
                return VK_ERROR_INVALID_EXTERNAL_HANDLE;
            }
            b->strides[p] = b->g->strides[p];
            b->offsets[p] = b->g->offsets[p];
        }
        b->yuv = true;
        b->planes = 2;
        b->format = yf;
        return VK_SUCCESS;
    }
    if (b->g->format_modifier == DRM_FORMAT_MOD_INVALID) {
        NVHAL_ERR("%s: the buffer carries DRM_FORMAT_MOD_INVALID (implicit layout), which cannot be imported with an "
                  "explicit modifier; " GB_FACTS " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, GB_ARGS(b));
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    VkFormat fv = fourcc_to_vk(b->g->format), av = ahb_to_vk(b->desc.format);
    if (fv == VK_FORMAT_UNDEFINED) {
        NVHAL_ERR("%s: the buffer's DRM format has no Vulkan equivalent in this HAL (YUV and other external-only "
                  "formats are not implemented); " GB_FACTS " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, GB_ARGS(b));
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    if (av != VK_FORMAT_UNDEFINED && av != fv) {
        NVHAL_ERR("%s: the AHB format (VkFormat %d by the equivalence table) disagrees with the memory layout the "
                  "gralloc chose (VkFormat %d); " GB_FACTS " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, av, fv, GB_ARGS(b));
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    b->format = fv;
    b->planes = 1;
    b->strides[0] = b->g->strides[0];
    b->offsets[0] = b->g->offsets[0];
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ wrapper images */

#define IMG_MAGIC 0x4e564148u /* "HAVN" */

struct pdata { uint64_t slot, data; };
struct nvhal_image {
    uint32_t magic;
    enum nvhal_wrapper_kind kind;
    struct nvhal_device *dev;
    pthread_mutex_t lock;
    /* written once under lock, read with __atomic; 8-aligned: a uint64_t on i686, where a struct
     * member is only 4-aligned and an 8-byte atomic must not straddle */
    VkImage real __attribute__((aligned(8)));
    VkDeviceMemory memory; /* AHB: the AHB memory the image was realised for (the app's);
                            * SWAPCHAIN: the imported gralloc memory (the HAL's, freed with the image) */
    uint64_t swapchain;    /* SWAPCHAIN: VkImageSwapchainCreateInfoKHR.swapchain (the loader's; logs only) */
    const VkAllocationCallbacks *alloc; /* SWAPCHAIN: vkCreateImage's, used for the ICD objects */
    VkImageCreateInfo ci;  /* the app's, pNext = NULL, format = real format */
    uint32_t *qfi;
    VkFormat *view_formats;
    uint32_t view_format_count;
    bool has_stencil_usage, external_format;
    VkImageUsageFlags stencil_usage;
    uint64_t modifier;
    /* object state set before realisation, replayed on the ICD image */
    char *name;
    bool name_marker;
    struct pdata *pd;
    uint32_t pd_n;
};

static struct nvhal_hash wrappers = NVHAL_HASH_INIT;
static atomic_uint wrapper_count;

bool nvhal_no_wrappers(void) { return atomic_load_explicit(&wrapper_count, memory_order_acquire) == 0; }

static struct nvhal_image *wrapper(VkImage image) {
    if (image == VK_NULL_HANDLE || nvhal_no_wrappers()) return NULL;
    return nvhal_hash_get(&wrappers, NVHAL_ND_U64(image));
}

static VkImage real_of(struct nvhal_image *w) { return __atomic_load_n(&w->real, __ATOMIC_ACQUIRE); }

VkImage nvhal_image_xl(VkImage image, bool *hit, const char *what) {
    struct nvhal_image *w = wrapper(image);
    if (!w) return image;
    *hit = true;
    VkImage r = real_of(w);
    if (r == VK_NULL_HANDLE && what) {
        if (w->kind == NVHAL_WRAP_SWAPCHAIN)
            NVHAL_ERR("%s: swapchain-bound image %p (VkImageSwapchainCreateInfoKHR) is not bound yet (it is realised "
                      "by vkBindImageMemory2 with VkBindImageMemorySwapchainInfoKHR); the call is dropped", what,
                      (void *)w);
        else
            NVHAL_ERR("%s: AHB image %p has no memory yet (it is realised by vkAllocateMemory with "
                      "VkImportAndroidHardwareBufferInfoANDROID / an AHB export); the call is dropped", what, (void *)w);
    }
    return r;
}

enum nvhal_wrapper_kind nvhal_image_wrapper_kind(VkImage image) {
    struct nvhal_image *w = wrapper(image);
    return w ? w->kind : NVHAL_WRAP_NONE;
}

VkFormat nvhal_image_real_format(VkImage image) {
    struct nvhal_image *w = wrapper(image);
    return w ? w->ci.format : VK_FORMAT_UNDEFINED;
}

bool nvhal_image_wrapper_handle(uint64_t handle) { return wrapper(NVHAL_U64_ND(VkImage, handle)) != NULL; }

static void wrapper_free(struct nvhal_image *w) {
    pthread_mutex_destroy(&w->lock);
    free(w->qfi);
    free(w->view_formats);
    free(w->name);
    free(w->pd);
    w->magic = 0;
    free(w);
}

/* ------------------------------------------------------------------ AHB-backed memory */

struct ahb_mem {
    struct nvhal_device *dev;
    AHardwareBuffer *buffer;  /* one reference held */
    struct nvhal_image *image; /* dedicated wrapper (cleared when the wrapper dies first) */
};
static struct nvhal_hash ahb_mems = NVHAL_HASH_INIT;

/* ------------------------------------------------------------------ create-info parsing */

static bool chain_find(const void *chain, VkStructureType t, const void **out) {
    for (const VkBaseInStructure *s = chain; s; s = s->pNext)
        if (s->sType == t) { *out = s; return true; }
    return false;
}

bool nvhal_ci_is_ahb(const VkImageCreateInfo *ci) {
    const VkExternalMemoryImageCreateInfo *e;
    return ci && chain_find(ci->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, (const void **)&e) &&
           (e->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID);
}

/* Fill w (unregistered) from an app create info with the AHB handle type. */
static VkResult parse_ci(const VkImageCreateInfo *ci, const char *who, struct nvhal_image *w) {
    memset(w, 0, sizeof *w);
    w->ci = *ci;
    w->ci.pNext = NULL;
    w->ci.pQueueFamilyIndices = NULL;
    VkFormat ext_format = VK_FORMAT_UNDEFINED;
    for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext) {
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO: {
            const VkExternalMemoryImageCreateInfo *e = (const void *)s;
            if (e->handleTypes != VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) {
                NVHAL_ERR("%s: handleTypes 0x%x mixes the AHB handle type with others; only "
                          "ANDROID_HARDWARE_BUFFER alone is implemented -> VK_ERROR_FEATURE_NOT_PRESENT", who,
                          e->handleTypes);
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            break;
        }
        case VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID: {
            const VkExternalFormatANDROID *x = (const void *)s;
            ext_format = (VkFormat)x->externalFormat;
            break;
        }
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: {
            const VkImageFormatListCreateInfo *l = (const void *)s;
            if (l->viewFormatCount) {
                w->view_formats = malloc(l->viewFormatCount * sizeof *w->view_formats);
                if (!w->view_formats) return VK_ERROR_OUT_OF_HOST_MEMORY;
                memcpy(w->view_formats, l->pViewFormats, l->viewFormatCount * sizeof *w->view_formats);
                w->view_format_count = l->viewFormatCount;
            }
            break;
        }
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO:
            w->has_stencil_usage = true;
            w->stencil_usage = ((const VkImageStencilUsageCreateInfo *)s)->stencilUsage;
            break;
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_IMAGE_CREATE_INFO_NV:
            break; /* the ICD image of an AHB is always a dedicated allocation */
        default:
            NVHAL_ERR("%s: structure sType %d chained to an AHB image is not implemented -> "
                      "VK_ERROR_FEATURE_NOT_PRESENT", who, (int)s->sType);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    if (ext_format != VK_FORMAT_UNDEFINED) {
        /* externalFormat values this HAL hands out are VkFormats (vkGetAndroidHardwareBufferPropertiesANDROID) */
        if (ci->format != VK_FORMAT_UNDEFINED || !external_format_ok(ext_format)) {
            NVHAL_ERR("%s: VkExternalFormatANDROID.externalFormat %" PRIu64 " with format %d: this HAL's external "
                      "formats are the VkFormat of the buffer and require format UNDEFINED -> "
                      "VK_ERROR_FORMAT_NOT_SUPPORTED", who, (uint64_t)ext_format, ci->format);
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        w->ci.format = ext_format;
        w->external_format = true;
    }
    if (!external_format_ok(w->ci.format)) {
        NVHAL_ERR("%s: VkFormat %d cannot back an AHB here (the gralloc allocates only RGBA8/RGBX8/BGRA8/RGB565/"
                  "RGB10A2/RGBA16F/R8, and NV12/P010 for import) -> VK_ERROR_FORMAT_NOT_SUPPORTED", who, w->ci.format);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (ci->tiling != VK_IMAGE_TILING_OPTIMAL && ci->tiling != VK_IMAGE_TILING_LINEAR) {
        NVHAL_ERR("%s: tiling %d with the AHB handle type; the buffer's layout decides, request OPTIMAL or LINEAR "
                  "-> VK_ERROR_FORMAT_NOT_SUPPORTED", who, ci->tiling);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (ci->imageType != VK_IMAGE_TYPE_2D || ci->extent.depth != 1 || ci->mipLevels != 1 || ci->arrayLayers != 1 ||
        ci->samples != VK_SAMPLE_COUNT_1_BIT ||
        (ci->flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                      VK_IMAGE_CREATE_SPARSE_ALIASED_BIT | VK_IMAGE_CREATE_DISJOINT_BIT))) {
        NVHAL_ERR("%s: AHB image type %d %ux%ux%u, %u mips, %u layers, %d samples, flags 0x%x: the gralloc allocates "
                  "single-level, single-layer, single-sample 2D buffers and sparse/disjoint AHB images are not "
                  "implemented -> VK_ERROR_FORMAT_NOT_SUPPORTED", who, ci->imageType, ci->extent.width,
                  ci->extent.height, ci->extent.depth, ci->mipLevels, ci->arrayLayers, ci->samples, ci->flags);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if ((ci->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) && !w->view_format_count) {
        NVHAL_ERR("%s: MUTABLE_FORMAT without a VkImageFormatListCreateInfo: the image is realised with "
                  "DRM-format-modifier tiling, which requires the list (VUID-VkImageCreateInfo-tiling-02353). "
                  "Remedy: chain the view formats -> VK_ERROR_FORMAT_NOT_SUPPORTED", who);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (ci->sharingMode == VK_SHARING_MODE_CONCURRENT && ci->queueFamilyIndexCount) {
        w->qfi = malloc(ci->queueFamilyIndexCount * sizeof *w->qfi);
        if (!w->qfi) return VK_ERROR_OUT_OF_HOST_MEMORY;
        memcpy(w->qfi, ci->pQueueFamilyIndices, ci->queueFamilyIndexCount * sizeof *w->qfi);
        w->ci.pQueueFamilyIndices = w->qfi;
    }
    return VK_SUCCESS;
}

/* The ICD create info of w for DRM-modifier tiling: either the explicit layout of a buffer, or
 * (explicit == NULL) a modifier list, for requirement probes. */
struct icd_ci {
    VkImageCreateInfo ci;
    VkImageFormatListCreateInfo fl;
    VkImageStencilUsageCreateInfo su;
    VkImageDrmFormatModifierExplicitCreateInfoEXT ex;
    VkImageDrmFormatModifierListCreateInfoEXT ml;
    VkExternalMemoryImageCreateInfo em;
    VkSubresourceLayout plane[2];
};

static void build_icd_ci(const struct nvhal_image *w, struct icd_ci *c, uint64_t modifier, uint32_t nplanes,
                         const uint32_t *strides, const uint32_t *offsets, const uint64_t *list, uint32_t list_n) {
    memset(c, 0, sizeof *c);
    c->ci = w->ci;
    c->ci.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    c->ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const void *chain = NULL;
    if (w->view_format_count) {
        c->fl = (VkImageFormatListCreateInfo){VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, chain,
                                              w->view_format_count, w->view_formats};
        chain = &c->fl;
    }
    if (w->has_stencil_usage) {
        c->su = (VkImageStencilUsageCreateInfo){VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO, chain,
                                                w->stencil_usage};
        chain = &c->su;
    }
    if (list) {
        c->ml = (VkImageDrmFormatModifierListCreateInfoEXT){
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT, chain, list_n, list};
        chain = &c->ml;
    } else {
        for (uint32_t p = 0; p < nplanes && p < 2; p++)
            c->plane[p] = (VkSubresourceLayout){.offset = offsets[p], .rowPitch = strides[p]};
        c->ex = (VkImageDrmFormatModifierExplicitCreateInfoEXT){
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, chain, modifier, nplanes, c->plane};
        chain = &c->ex;
    }
    c->em = (VkExternalMemoryImageCreateInfo){VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, chain,
                                              VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    c->ci.pNext = &c->em;
}

/* The app's create info of a SWAPCHAIN wrapper (tiling as given) with only its format list and
 * stencil usage chained: the input of nvhal_anb_import and of the requirement probe. */
static void sc_icd_ci(const struct nvhal_image *w, struct icd_ci *c) {
    memset(c, 0, sizeof *c);
    c->ci = w->ci;
    const void *chain = NULL;
    if (w->view_format_count) {
        c->fl = (VkImageFormatListCreateInfo){VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, chain,
                                              w->view_format_count, w->view_formats};
        chain = &c->fl;
    }
    if (w->has_stencil_usage) {
        c->su = (VkImageStencilUsageCreateInfo){VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO, chain,
                                                w->stencil_usage};
        chain = &c->su;
    }
    c->ci.pNext = chain;
}

/* Modifiers the gralloc backend would choose for an image of this format and tiling: the same
 * filter as nvidia-prolapse/gralloc/nvidia_gbm_wrapper.c nvg_alloc (single plane, LINEAR only for CPU buffers,
 * block-linear only for GPU buffers, SAMPLED|TRANSFER_SRC|TRANSFER_DST (+COLOR_ATTACHMENT for GPU)). */
static uint32_t gralloc_modifiers(struct nvhal_instance *inst, VkPhysicalDevice pd, VkFormat f, bool linear,
                                  uint64_t *mods, VkFormatFeatureFlags2 *feats, uint32_t cap) {
    VkDrmFormatModifierPropertiesList2EXT ml = {.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT};
    VkFormatProperties2 fp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &ml};
    inst->GetPhysicalDeviceFormatProperties2(pd, f, &fp);
    VkDrmFormatModifierProperties2EXT mp[64];
    if (ml.drmFormatModifierCount > 64) ml.drmFormatModifierCount = 64;
    ml.pDrmFormatModifierProperties = mp;
    inst->GetPhysicalDeviceFormatProperties2(pd, f, &fp);
    const VkFormatFeatureFlags2 need = yuv_vk(f) ? VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT
                                       : VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
                                         VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT |
                                         (linear ? 0 : VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT);
    uint32_t n = 0;
    /* the gralloc lays NV12/P010 out LINEAR, two planes in one buffer */
    const uint32_t planes = yuv_vk(f) ? 2 : 1;
    if (planes == 2) linear = true;
    for (uint32_t i = 0; i < ml.drmFormatModifierCount && n < cap; i++) {
        bool lin = mp[i].drmFormatModifier == DRM_FORMAT_MOD_LINEAR;
        if (mp[i].drmFormatModifier == DRM_FORMAT_MOD_INVALID || lin != linear ||
            mp[i].drmFormatModifierPlaneCount != planes ||
            (mp[i].drmFormatModifierTilingFeatures & need) != need)
            continue;
        if (feats) feats[n] = mp[i].drmFormatModifierTilingFeatures;
        mods[n++] = mp[i].drmFormatModifier;
    }
    return n;
}

/* features of one modifier of a format (0 when the ICD does not list it) */
static VkFormatFeatureFlags2 modifier_features(struct nvhal_instance *inst, VkPhysicalDevice pd, VkFormat f,
                                               uint64_t modifier) {
    VkDrmFormatModifierPropertiesList2EXT ml = {.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT};
    VkFormatProperties2 fp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &ml};
    inst->GetPhysicalDeviceFormatProperties2(pd, f, &fp);
    VkDrmFormatModifierProperties2EXT mp[64];
    if (ml.drmFormatModifierCount > 64) ml.drmFormatModifierCount = 64;
    ml.pDrmFormatModifierProperties = mp;
    inst->GetPhysicalDeviceFormatProperties2(pd, f, &fp);
    for (uint32_t i = 0; i < ml.drmFormatModifierCount; i++)
        if (mp[i].drmFormatModifier == modifier) return mp[i].drmFormatModifierTilingFeatures;
    return 0;
}

/* The usage the ICD image gets for a modifier: the app's, except INPUT_ATTACHMENT on LINEAR when
 * the ICD refuses exactly that (same rule and warning as anb.c; ANGLE always asks for it, HBS:89-92). */
static VkResult effective_usage(struct nvhal_device *d, const VkImageCreateInfo *ici, uint64_t modifier,
                                VkImageUsageFlags *usage, const char *who) {
    VkResult r = nvhal_modifier_usage_ok(d, ici, modifier, *usage);
    if (r != VK_SUCCESS && modifier == DRM_FORMAT_MOD_LINEAR && (*usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) &&
        nvhal_modifier_usage_ok(d, ici, modifier, *usage & ~VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) == VK_SUCCESS) {
        /* ANGLE asks for it on every CPU-buffer import: warn once, trace afterwards */
        static atomic_bool warned;
        if (!atomic_exchange(&warned, true) || nvhal_trace)
            nvhal_log(NVHAL_LVL_WARN, "%s: the ICD rejects INPUT_ATTACHMENT usage on a LINEAR dma-buf and accepts the "
                      "rest; the image is created without VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT (usage 0x%x -> 0x%x)%s",
                      who, *usage, *usage & ~VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
                      nvhal_trace ? "" : " (further occurrences only with NVHAL_TRACE)");
        *usage &= ~VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        r = VK_SUCCESS;
    }
    return r;
}

/* ------------------------------------------------------------------ realisation */

static void replay_object_state(struct nvhal_image *w, VkImage real) {
    struct nvhal_device *d = w->dev;
    if (w->name) {
        if (!w->name_marker && d->xl.SetDebugUtilsObjectNameEXT) {
            VkDebugUtilsObjectNameInfoEXT ni = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
                                                VK_OBJECT_TYPE_IMAGE, NVHAL_ND_U64(real), w->name};
            d->xl.SetDebugUtilsObjectNameEXT(d->handle, &ni);
        } else if (w->name_marker && d->xl.DebugMarkerSetObjectNameEXT) {
            VkDebugMarkerObjectNameInfoEXT ni = {VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT, NULL,
                                                 VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT, NVHAL_ND_U64(real),
                                                 w->name};
            d->xl.DebugMarkerSetObjectNameEXT(d->handle, &ni);
        }
    }
    PFN_vkSetPrivateData sp = d->xl.SetPrivateData ? d->xl.SetPrivateData : d->xl.SetPrivateDataEXT;
    for (uint32_t i = 0; i < w->pd_n && sp; i++)
        sp(d->handle, VK_OBJECT_TYPE_IMAGE, NVHAL_ND_U64(real), NVHAL_U64_ND(VkPrivateDataSlot, w->pd[i].slot),
           w->pd[i].data);
}

/* Create the ICD image of w for buffer b. Called with w->lock held. */
/* NV12/P010 (two planes in one LINEAR dma-buf): NVIDIA's driver (615, measured) binds each plane of such an image as a
 * mapping of its own, sized and aligned like that plane of a DISJOINT image (pitch * rows rounded up to 64 KiB, 64 KiB
 * alignment), whatever the explicit offsets. A plane offset off a page makes that mapping spill into the next one in
 * the driver's GPU VA: RM refuses the mapping node ("intermapRegisterDmaMapping: Failed to insert new mapping node",
 * kernel log), vkBindImageMemory still succeeds and the plane samples zeros. A plane whose sized range runs past the
 * dma-buf samples zeros as well. So the layout must be one the ICD can bind plane by plane: each offset a multiple of
 * the ICD's per-plane alignment, each plane's required size inside the dma-buf, the planes in order without overlap.
 * NVK reports exact sizes and 32-byte alignment, i.e. its own LINEAR import rule. The gralloc lays NV12/P010 out by
 * this rule (nvidia-prolapse/gralloc, minigbm 0004 + get_gbm_layout_ops). */
static VkResult yuv_layout_bindable(struct nvhal_device *d, struct nvhal_image *w, const struct gbuf *b,
                                    VkImageUsageFlags usage, const VkAllocationCallbacks *alloc, const char *who) {
    const VkFormatFeatureFlags2 mf = modifier_features(d->inst, d->physical_device, b->format, DRM_FORMAT_MOD_LINEAR);
    if (!(mf & VK_FORMAT_FEATURE_2_DISJOINT_BIT)) {
        NVHAL_ERR("%s: the ICD lists no DISJOINT feature for VkFormat %d LINEAR (features 0x%" PRIx64 "), so the "
                  "per-plane requirements of this %s layout cannot be checked; " GB_FACTS
                  " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, b->format, (uint64_t)mf, b->fcc, GB_ARGS(b));
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    const uint32_t zero[2] = {0, 0};
    struct icd_ci c;
    build_icd_ci(w, &c, DRM_FORMAT_MOD_LINEAR, b->planes, b->strides, zero, NULL, 0);
    c.ci.flags |= VK_IMAGE_CREATE_DISJOINT_BIT;
    c.ci.usage = usage;
    VkImage img = VK_NULL_HANDLE;
    VkResult r = d->CreateImage(d->handle, &c.ci, alloc, &img);
    if (r != VK_SUCCESS) {
        NVHAL_ERR("%s: ICD vkCreateImage(DISJOINT, LINEAR, strides %u/%u B) for the per-plane requirements -> %s; "
                  GB_FACTS " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, b->strides[0], b->strides[1], NVHAL_VKR(r),
                  GB_ARGS(b));
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    VkMemoryRequirements req[2];
    for (uint32_t p = 0; p < b->planes; p++) {
        VkImagePlaneMemoryRequirementsInfo pi = {VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO, NULL,
                                                 p ? VK_IMAGE_ASPECT_PLANE_1_BIT : VK_IMAGE_ASPECT_PLANE_0_BIT};
        VkImageMemoryRequirementsInfo2 mi = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, &pi, img};
        VkMemoryRequirements2 mr = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        d->anb_GetImageMemoryRequirements2(d->handle, &mi, &mr);
        req[p] = mr.memoryRequirements;
    }
    d->DestroyImage(d->handle, img, alloc);
    for (uint32_t p = 0; p < b->planes; p++) {
        const uint64_t off = b->offsets[p], end = off + req[p].size;
        const uint64_t prev_end = p ? (uint64_t)b->offsets[p - 1] + req[p - 1].size : 0;
        if (!req[p].alignment || off % req[p].alignment || end > b->size || off < prev_end) {
            NVHAL_ERR("%s: %s plane %u at offset %llu: the ICD binds it as %llu B aligned to %llu B (its per-plane "
                      "requirement for stride %u B), which needs the offset aligned, the range inside the %llu B "
                      "dma-buf and after plane %u's (ends at %llu); otherwise this driver samples zeros (NVIDIA: RM "
                      "rejects the plane's mapping); " GB_FACTS " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, b->fcc, p,
                      (unsigned long long)off, (unsigned long long)req[p].size,
                      (unsigned long long)req[p].alignment, b->strides[p], (unsigned long long)b->size, p ? p - 1 : 0,
                      (unsigned long long)prev_end, GB_ARGS(b));
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
    }
    return VK_SUCCESS;
}

static VkResult realise(struct nvhal_image *w, const struct gbuf *b, const VkAllocationCallbacks *alloc,
                        const char *who, VkImage *out) {
    struct nvhal_device *d = w->dev;
    const struct cros_gralloc_handle *g = b->g;
    if (b->blob || b->format != w->ci.format || w->ci.extent.width != b->desc.width ||
        w->ci.extent.height != b->desc.height || b->desc.layers != w->ci.arrayLayers) {
        NVHAL_ERR("%s: the AHB does not match the image (VkFormat %d, %ux%u, %u layer(s); format by the equivalence "
                  "table, identical width/height/layers by VUID-VkMemoryAllocateInfo-pNext-02388); " GB_FACTS
                  " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, w->ci.format, w->ci.extent.width, w->ci.extent.height,
                  w->ci.arrayLayers, GB_ARGS(b));
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    struct icd_ci c;
    build_icd_ci(w, &c, g->format_modifier, b->planes, b->strides, b->offsets, NULL, 0);
    VkResult r = effective_usage(d, &c.ci, g->format_modifier, &c.ci.usage, who);
    if (r != VK_SUCCESS) {
        NVHAL_ERR("%s: ICD vkGetPhysicalDeviceImageFormatProperties2(DRM modifier, DMA_BUF import, usage 0x%x, flags "
                  "0x%x) -> %s; " GB_FACTS, who, c.ci.usage, c.ci.flags, NVHAL_VKR(r), GB_ARGS(b));
        return r == VK_ERROR_FORMAT_NOT_SUPPORTED ? VK_ERROR_INVALID_EXTERNAL_HANDLE : r;
    }
    if (b->planes > 1) {
        r = yuv_layout_bindable(d, w, b, c.ci.usage, alloc, who);
        if (r != VK_SUCCESS) return r;
    }
    VkImage img = VK_NULL_HANDLE;
    r = d->CreateImage(d->handle, &c.ci, alloc, &img);
    if (r != VK_SUCCESS) {
        NVHAL_ERR("%s: ICD vkCreateImage(DRM modifier explicit, %u plane(s)) -> %s; " GB_FACTS, who, b->planes,
                  NVHAL_VKR(r), GB_ARGS(b));
        if (b->planes > 1)
            NVHAL_ERR("%s: plane 1 offset %u, stride %u B", who, b->offsets[1], b->strides[1]);
        /* the buffer's layout is what the driver rejected: for the AHB import that is an invalid handle */
        return r == VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT ? VK_ERROR_INVALID_EXTERNAL_HANDLE : r;
    }
    if (nvhal_hash_get(&wrappers, NVHAL_ND_U64(img)))
        NVHAL_ERR("%s: the ICD returned VkImage %p, which equals a live AHB wrapper handle; translation is now "
                  "ambiguous (this HAL assumes ICD handles and its heap pointers never collide)", who, NVHAL_LOGH(img));
    w->modifier = g->format_modifier;
    NVHAL_TRACE("%s: AHB image %p realised as ICD image %p (usage 0x%x flags 0x%x); " GB_FACTS, who, (void *)w,
                NVHAL_LOGH(img), c.ci.usage, c.ci.flags, GB_ARGS(b));
    *out = img;
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ vkCreateImage / vkDestroyImage */

VkResult VKAPI_CALL nvhal_ahb_CreateImage(struct nvhal_device *d, VkDevice device, const VkImageCreateInfo *ci,
                                          const VkAllocationCallbacks *alloc, VkImage *pImage, bool *handled) {
    (void)device;
    (void)alloc;
    const VkExternalFormatANDROID *xf = NULL;
    bool is_ahb = nvhal_ci_is_ahb(ci);
    chain_find(ci->pNext, VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID, (const void **)&xf);
    if (!is_ahb && !(xf && xf->externalFormat)) {
        *handled = false;
        return VK_SUCCESS;
    }
    *handled = true;
    if (!d->ahb_enabled) {
        NVHAL_ERR("vkCreateImage: the AHB handle type / VkExternalFormatANDROID is used, but "
                  "VK_ANDROID_external_memory_android_hardware_buffer is not enabled on this device -> "
                  "VK_ERROR_EXTENSION_NOT_PRESENT");
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    if (!is_ahb) {
        NVHAL_ERR("vkCreateImage: VkExternalFormatANDROID.externalFormat %" PRIu64 " without the AHB handle type in "
                  "VkExternalMemoryImageCreateInfo -> VK_ERROR_FORMAT_NOT_SUPPORTED", (uint64_t)xf->externalFormat);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    struct nvhal_image *w = malloc(sizeof *w);
    if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult r = parse_ci(ci, "vkCreateImage(AHB)", w);
    if (r != VK_SUCCESS) {
        free(w->qfi);
        free(w->view_formats);
        free(w);
        return r;
    }
    w->magic = IMG_MAGIC;
    w->kind = NVHAL_WRAP_AHB;
    w->dev = d;
    pthread_mutex_init(&w->lock, NULL);
    if (!nvhal_hash_put(&wrappers, (uint64_t)(uintptr_t)w, w)) {
        wrapper_free(w);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    atomic_fetch_add_explicit(&wrapper_count, 1, memory_order_release);
    NVHAL_TRACE("vkCreateImage(AHB): wrapper %p, VkFormat %d%s, %ux%u, usage 0x%x, flags 0x%x, tiling %d, %u view "
                "format(s); the ICD image is created when the buffer is known", (void *)w, w->ci.format,
                w->external_format ? " (external format)" : "", w->ci.extent.width, w->ci.extent.height, w->ci.usage,
                w->ci.flags, w->ci.tiling, w->view_format_count);
    *pImage = (VkImage)(uintptr_t)w;
    return VK_SUCCESS;
}

bool nvhal_ahb_DestroyImage(struct nvhal_device *d, VkDevice device, VkImage image, const VkAllocationCallbacks *alloc) {
    if (image == VK_NULL_HANDLE || nvhal_no_wrappers()) return false;
    struct nvhal_image *w = nvhal_hash_del(&wrappers, NVHAL_ND_U64(image));
    if (!w) return false;
    atomic_fetch_sub_explicit(&wrapper_count, 1, memory_order_release);
    if (w->kind == NVHAL_WRAP_AHB && w->memory != VK_NULL_HANDLE) {
        struct ahb_mem *m = nvhal_hash_get(&ahb_mems, NVHAL_ND_U64(w->memory));
        if (m && m->image == w) m->image = NULL;
    }
    VkImage real = real_of(w);
    if (real != VK_NULL_HANDLE) d->DestroyImage(device, real, alloc);
    /* the swapchain image's imported memory is the HAL's (as for an ANB image, anb_untrack) */
    if (w->kind == NVHAL_WRAP_SWAPCHAIN && w->memory != VK_NULL_HANDLE) d->FreeMemory(device, w->memory, alloc);
    NVHAL_TRACE("vkDestroyImage(%s): wrapper %p (ICD image %p) destroyed",
                w->kind == NVHAL_WRAP_SWAPCHAIN ? "swapchain-bound" : "AHB", (void *)w, NVHAL_LOGH(real));
    wrapper_free(w);
    return true;
}

/* ------------------------------------------------------------------ swapchain-bound images */

VkResult nvhal_sc_CreateImage(struct nvhal_device *d, const VkImageCreateInfo *ci,
                              const VkImageSwapchainCreateInfoKHR *sc, const VkAllocationCallbacks *alloc,
                              VkImage *pImage) {
    static const char who[] = "vkCreateImage(VkImageSwapchainCreateInfoKHR)";
    const uint64_t swapchain = NVHAL_ND_U64(sc->swapchain);
    if (!d->anb_enabled) {
        NVHAL_ERR("%s: swapchain 0x%" PRIx64 ": an image bound to swapchain memory is implemented over "
                  "VK_ANDROID_native_buffer, which is not enabled on this device (the Android loader enables it on "
                  "every device it creates swapchains for) -> VK_ERROR_EXTENSION_NOT_PRESENT", who, swapchain);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    if (!nvhal_anb_format_known(ci->format) || ci->imageType != VK_IMAGE_TYPE_2D || ci->extent.depth != 1 ||
        ci->mipLevels != 1 || ci->arrayLayers != 1 || ci->samples != VK_SAMPLE_COUNT_1_BIT ||
        (ci->flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                      VK_IMAGE_CREATE_SPARSE_ALIASED_BIT | VK_IMAGE_CREATE_DISJOINT_BIT))) {
        NVHAL_ERR("%s: swapchain 0x%" PRIx64 ", VkFormat %d, type %d %ux%ux%u, %u mips, %u layers, %d samples, flags "
                  "0x%x: a swapchain image here is a single-level, single-layer, single-sample 2D gralloc buffer of a "
                  "format this HAL maps to DRM (anb.c anb_formats), never sparse/disjoint -> "
                  "VK_ERROR_FORMAT_NOT_SUPPORTED", who, swapchain, ci->format, ci->imageType, ci->extent.width,
                  ci->extent.height, ci->extent.depth, ci->mipLevels, ci->arrayLayers, ci->samples, ci->flags);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    struct nvhal_image *w = calloc(1, sizeof *w);
    if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
    w->ci = *ci;
    w->ci.pNext = NULL;
    w->ci.pQueueFamilyIndices = NULL;
    VkResult r = VK_SUCCESS;
    for (const VkBaseInStructure *s = ci->pNext; s && r == VK_SUCCESS; s = s->pNext) {
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR:
            break; /* the loader's object: never shown to the ICD */
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: {
            const VkImageFormatListCreateInfo *l = (const void *)s;
            if (l->viewFormatCount && !w->view_formats) {
                w->view_formats = malloc(l->viewFormatCount * sizeof *w->view_formats);
                if (!w->view_formats) { r = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
                memcpy(w->view_formats, l->pViewFormats, l->viewFormatCount * sizeof *w->view_formats);
                w->view_format_count = l->viewFormatCount;
            }
            break;
        }
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO:
            w->has_stencil_usage = true;
            w->stencil_usage = ((const VkImageStencilUsageCreateInfo *)s)->stencilUsage;
            break;
        default:
            NVHAL_ERR("%s: unsupported structure (sType %d) chained next to VkImageSwapchainCreateInfoKHR -> "
                      "VK_ERROR_FEATURE_NOT_PRESENT", who, (int)s->sType);
            r = VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    if (r == VK_SUCCESS && ci->sharingMode == VK_SHARING_MODE_CONCURRENT && ci->queueFamilyIndexCount) {
        w->qfi = malloc(ci->queueFamilyIndexCount * sizeof *w->qfi);
        if (!w->qfi) r = VK_ERROR_OUT_OF_HOST_MEMORY;
        else {
            memcpy(w->qfi, ci->pQueueFamilyIndices, ci->queueFamilyIndexCount * sizeof *w->qfi);
            w->ci.pQueueFamilyIndices = w->qfi;
        }
    }
    if (r != VK_SUCCESS) {
        free(w->qfi);
        free(w->view_formats);
        free(w);
        return r;
    }
    w->magic = IMG_MAGIC;
    w->kind = NVHAL_WRAP_SWAPCHAIN;
    w->dev = d;
    w->swapchain = swapchain;
    w->alloc = alloc;
    pthread_mutex_init(&w->lock, NULL);
    if (!nvhal_hash_put(&wrappers, (uint64_t)(uintptr_t)w, w)) {
        wrapper_free(w);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    atomic_fetch_add_explicit(&wrapper_count, 1, memory_order_release);
    NVHAL_TRACE("%s: wrapper %p for swapchain 0x%" PRIx64 ", VkFormat %d, %ux%u, usage 0x%x, flags 0x%x, tiling %d, "
                "sharing %d (%u families), %u view format(s); the ICD image is created at the swapchain bind", who,
                (void *)w, swapchain, w->ci.format, w->ci.extent.width, w->ci.extent.height, w->ci.usage, w->ci.flags,
                w->ci.tiling, w->ci.sharingMode, w->ci.queueFamilyIndexCount, w->view_format_count);
    *pImage = (VkImage)(uintptr_t)w;
    return VK_SUCCESS;
}

VkResult nvhal_sc_bind(struct nvhal_device *d, VkImage image, const VkNativeBufferANDROID *nb, const char *who) {
    struct nvhal_image *w = wrapper(image);
    if (!w || w->kind != NVHAL_WRAP_SWAPCHAIN || w->dev != d) {
        NVHAL_ERR("%s: image %p is not a swapchain-bound image of device %p -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who,
                  NVHAL_LOGH(image), (void *)d->handle);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    pthread_mutex_lock(&w->lock);
    if (real_of(w) != VK_NULL_HANDLE) {
        pthread_mutex_unlock(&w->lock);
        NVHAL_ERR("%s: image %p (swapchain 0x%" PRIx64 ") is already bound (ICD image %p); an image is bound once "
                  "-> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, (void *)w, w->swapchain, NVHAL_LOGH(real_of(w)));
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    struct icd_ci c;
    sc_icd_ci(w, &c);
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult r = nvhal_anb_import(d, &c.ci, nb, w->alloc, who, &img, &mem);
    if (r != VK_SUCCESS) {
        pthread_mutex_unlock(&w->lock);
        return r;
    }
    if (nvhal_hash_get(&wrappers, NVHAL_ND_U64(img)))
        NVHAL_ERR("%s: the ICD returned VkImage %p, which equals a live wrapper handle; translation is now "
                  "ambiguous (this HAL assumes ICD handles and its heap pointers never collide)", who, NVHAL_LOGH(img));
    w->memory = mem;
    replay_object_state(w, img);
    __atomic_store_n(&w->real, img, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&w->lock);
    NVHAL_TRACE("%s: image %p (swapchain 0x%" PRIx64 ") realised as ICD image %p on gralloc handle %p, imported "
                "memory %p", who, (void *)w, w->swapchain, NVHAL_LOGH(img), (const void *)nb->handle, NVHAL_LOGH(mem));
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ memory */

/* Every allocation this HAL hands to the ICD goes through here, so the opt-in census
 * (nvhal_exp_memstats_alloc; off = one relaxed load) sees exactly the app's vkAllocateMemory calls. */
static VkResult icd_alloc(struct nvhal_device *d, VkDevice dev, const VkMemoryAllocateInfo *ai,
                          const VkAllocationCallbacks *alloc, VkDeviceMemory *out) {
    VkResult r = d->AllocateMemory(dev, ai, alloc, out);
    nvhal_exp_memstats_alloc(ai->memoryTypeIndex, ai->allocationSize, r);
    return r;
}

static VkResult import_dmabuf(struct nvhal_device *d, const struct gbuf *b, VkImage ded_image, VkBuffer ded_buffer,
                              uint32_t type, VkDeviceSize size, const void *extra, const VkAllocationCallbacks *alloc,
                              const char *who, VkDeviceMemory *out) {
    int fd = fcntl(b->fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) {
        int e = errno;
        NVHAL_ERR("%s: dup of dma-buf fd %d failed: %s", who, b->fd, strerror(e));
        return e == EMFILE ? VK_ERROR_TOO_MANY_OBJECTS : VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, extra, ded_image, ded_buffer};
    VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                   (ded_image || ded_buffer) ? (const void *)&ded : extra,
                                   VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd};
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, size, type};
    VkResult r = icd_alloc(d, d->handle, &mai, alloc, out);
    if (r != VK_SUCCESS) {
        close(fd);
        NVHAL_ERR("%s: ICD vkAllocateMemory(import DMA_BUF%s, %llu B, type %u) -> %s; " GB_FACTS, who,
                  ded_image ? ", dedicated image" : ded_buffer ? ", dedicated buffer" : "", (unsigned long long)size,
                  type, NVHAL_VKR(r), GB_ARGS(b));
    }
    return r; /* a successful import owns fd */
}

VkResult VKAPI_CALL nvhal_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *ai,
                                         const VkAllocationCallbacks *alloc, VkDeviceMemory *pMem) {
    struct nvhal_device *d = nvhal_device_get(device);
    if (!d) {
        NVHAL_ERR("vkAllocateMemory(%p): not a device created through this HAL", (void *)device);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkMemoryAllocateInfo exp_ai;
    uint32_t app_type = ai->memoryTypeIndex; /* the app's own type: the remap summary reports it as the source */
    ai = nvhal_exp_mem_remap(d, ai, &exp_ai); /* off by default: returns ai */
    const VkImportAndroidHardwareBufferInfoANDROID *imp = NULL;
    const VkExportMemoryAllocateInfo *exp = NULL;
    const VkMemoryDedicatedAllocateInfo *ded = NULL;
    const VkDedicatedAllocationMemoryAllocateInfoNV *dednv = NULL;
    for (const VkBaseInStructure *s = ai->pNext; s; s = s->pNext) {
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID: imp = (const void *)s; break;
        case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO: exp = (const void *)s; break;
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO: ded = (const void *)s; break;
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_MEMORY_ALLOCATE_INFO_NV: dednv = (const void *)s; break;
        default: break;
        }
    }
    bool exp_ahb = exp && (exp->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID);
    if ((ded || dednv) && !imp && !exp_ahb) {
        /* an emulated ASTC image's memory also backs its block images: not dedicated to the RGBA8 image */
        unsigned char storage[1024];
        VkMemoryAllocateInfo t;
        int a = nvhal_astc_alloc_info(ai, &t, storage, sizeof storage);
        if (a < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
        if (a > 0) {
            VkResult r = icd_alloc(d, device, &t, alloc, pMem);
            nvhal_exp_mem_remap_note(d, app_type, t.allocationSize, r);
            return r;
        }
    }
    struct nvhal_image *w = ded ? wrapper(ded->image) : NULL;
    if (!w && dednv) w = wrapper(dednv->image);
    if (!imp && !exp_ahb && !w) { /* not ours */
        VkResult r = icd_alloc(d, device, ai, alloc, pMem);
        nvhal_exp_mem_remap_note(d, app_type, ai->allocationSize, r);
        return r;
    }
    /* this entry point is also intercepted on ANB-only devices (swapchain-bound wrappers) */
    if ((imp || exp_ahb) && !d->ahb_enabled) {
        NVHAL_ERR("vkAllocateMemory: an AHB import/export, but VK_ANDROID_external_memory_android_hardware_buffer is "
                  "not enabled on this device -> VK_ERROR_INVALID_EXTERNAL_HANDLE");
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    if (w && w->kind == NVHAL_WRAP_SWAPCHAIN) {
        NVHAL_ERR("vkAllocateMemory: dedicated allocation for image %p, which was created with "
                  "VkImageSwapchainCreateInfoKHR and gets its memory from the swapchain (vkBindImageMemory2 + "
                  "VkBindImageMemorySwapchainInfoKHR) -> VK_ERROR_INVALID_EXTERNAL_HANDLE", (void *)w);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    const char *who = imp ? "vkAllocateMemory(AHB import)" : exp_ahb ? "vkAllocateMemory(AHB export)"
                                                                     : "vkAllocateMemory";
    if (!imp && !exp_ahb) {
        NVHAL_ERR("%s: dedicated allocation for AHB image %p without VkImportAndroidHardwareBufferInfoANDROID or an AHB "
                  "export: an AHB image can only be bound to its buffer's memory -> VK_ERROR_INVALID_EXTERNAL_HANDLE",
                  who, (void *)w);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    if (imp && exp_ahb) {
        NVHAL_ERR("%s: importing and exporting an AHB in one allocation -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    if (exp && (exp->handleTypes & ~VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) && exp_ahb) {
        NVHAL_ERR("%s: export handleTypes 0x%x mix AHB with other types; not implemented -> "
                  "VK_ERROR_INVALID_EXTERNAL_HANDLE", who, exp->handleTypes);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    VkBuffer ded_buffer = ded ? ded->buffer : dednv ? dednv->buffer : VK_NULL_HANDLE;
    /* rebuild the chain: only the structs the import can carry along */
    VkMemoryAllocateFlagsInfo flags_s;
    VkMemoryPriorityAllocateInfoEXT prio_s;
    VkMemoryOpaqueCaptureAddressAllocateInfo cap_s;
    const void *extra = NULL;
    for (const VkBaseInStructure *s = ai->pNext; s; s = s->pNext) {
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID:
        case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO:
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO:
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_MEMORY_ALLOCATE_INFO_NV:
            break;
        case VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO:
            flags_s = *(const VkMemoryAllocateFlagsInfo *)s;
            flags_s.pNext = extra;
            extra = &flags_s;
            break;
        case VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT:
            prio_s = *(const VkMemoryPriorityAllocateInfoEXT *)s;
            prio_s.pNext = extra;
            extra = &prio_s;
            break;
        case VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO:
            cap_s = *(const VkMemoryOpaqueCaptureAddressAllocateInfo *)s;
            cap_s.pNext = extra;
            extra = &cap_s;
            break;
        default:
            NVHAL_ERR("%s: structure sType %d next to the AHB import/export is not implemented -> "
                      "VK_ERROR_INVALID_EXTERNAL_HANDLE", who, (int)s->sType);
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
    }

    AHardwareBuffer *buf = NULL;
    bool owned = false; /* the allocation's reference (export) vs one we acquire (import) */
    if (exp_ahb) {
        if (!w) {
            NVHAL_ERR("%s: exporting an AHB without a dedicated AHB image (a BLOB for a VkBuffer, or no dedicated "
                      "object) is not implemented; no consumer in the guest uses it (AHB-CONTRACT.md §2) -> "
                      "VK_ERROR_OUT_OF_DEVICE_MEMORY", who);
            nvhal_exp_memstats_fail(ai->memoryTypeIndex, VK_ERROR_OUT_OF_DEVICE_MEMORY);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        if (!vk_to_ahb(w->ci.format)) {
            NVHAL_ERR("%s: exporting an AHB for VkFormat %d (YUV): the gralloc's NV12/P010 buffers are decoder output "
                      "imported into Vulkan; allocating them from Vulkan is not implemented -> "
                      "VK_ERROR_OUT_OF_DEVICE_MEMORY", who, w->ci.format);
            nvhal_exp_memstats_fail(ai->memoryTypeIndex, VK_ERROR_OUT_OF_DEVICE_MEMORY);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        AHardwareBuffer_Desc desc = {
            .width = w->ci.extent.width, .height = w->ci.extent.height, .layers = w->ci.arrayLayers,
            .format = vk_to_ahb(w->ci.format), .usage = vk_to_ahb_usage(w->ci.flags, w->ci.usage),
        };
        int rc = ahb.allocate(&desc, &buf);
        if (rc != 0 || !buf) {
            NVHAL_ERR("%s: AHardwareBuffer_allocate(%ux%u, %u layer(s), format 0x%x, usage 0x%" PRIx64 ") -> %d "
                      "(the gralloc log names the reason) -> VK_ERROR_OUT_OF_DEVICE_MEMORY", who, desc.width,
                      desc.height, desc.layers, desc.format, desc.usage, rc);
            nvhal_exp_memstats_fail(ai->memoryTypeIndex, VK_ERROR_OUT_OF_DEVICE_MEMORY);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        owned = true;
    } else {
        buf = imp->buffer;
    }
    struct gbuf b;
    VkResult r = gbuf_get(buf, who, &b);
    if (r != VK_SUCCESS) goto fail_buf;
    if (imp && ai->allocationSize != b.size) {
        NVHAL_ERR("%s: allocationSize %llu, but vkGetAndroidHardwareBufferPropertiesANDROID reports %llu for this "
                  "buffer (VUID-VkMemoryAllocateInfo-allocationSize-02383) -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who,
                  (unsigned long long)ai->allocationSize, (unsigned long long)b.size);
        r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
        goto fail_buf;
    }
    VkMemoryFdPropertiesKHR fdp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    r = d->GetMemoryFdPropertiesKHR(d->handle, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, b.fd, &fdp);
    if (r != VK_SUCCESS) {
        NVHAL_ERR("%s: ICD vkGetMemoryFdPropertiesKHR(DMA_BUF) -> %s; " GB_FACTS, who, NVHAL_VKR(r), GB_ARGS(&b));
        goto fail_buf;
    }

    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImage real = VK_NULL_HANDLE;
    if (w) {
        pthread_mutex_lock(&w->lock);
        if (real_of(w) != VK_NULL_HANDLE) {
            pthread_mutex_unlock(&w->lock);
            NVHAL_ERR("%s: AHB image %p already has memory -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, (void *)w);
            r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            goto fail_buf;
        }
        r = realise(w, &b, alloc, who, &real);
        if (r != VK_SUCCESS) {
            pthread_mutex_unlock(&w->lock);
            goto fail_buf;
        }
        VkMemoryRequirements2 mr = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        VkImageMemoryRequirementsInfo2 mri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, NULL, real};
        d->anb_GetImageMemoryRequirements2(d->handle, &mri, &mr);
        uint32_t bits = fdp.memoryTypeBits & mr.memoryRequirements.memoryTypeBits;
        if (b.size < mr.memoryRequirements.size || !(bits & (1u << ai->memoryTypeIndex))) {
            NVHAL_ERR("%s: the image needs %llu B in memory types 0x%x, the dma-buf has %llu B in types 0x%x, the app "
                      "asked for type %u -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who,
                      (unsigned long long)mr.memoryRequirements.size, mr.memoryRequirements.memoryTypeBits,
                      (unsigned long long)b.size, fdp.memoryTypeBits, ai->memoryTypeIndex);
            r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
        } else {
            r = import_dmabuf(d, &b, real, VK_NULL_HANDLE, ai->memoryTypeIndex, mr.memoryRequirements.size, extra,
                              alloc, who, &mem);
        }
        if (r != VK_SUCCESS) {
            d->DestroyImage(d->handle, real, alloc);
            pthread_mutex_unlock(&w->lock);
            goto fail_buf;
        }
        w->memory = mem;
        replay_object_state(w, real);
        __atomic_store_n(&w->real, real, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&w->lock);
    } else {
        if (!b.blob) {
            NVHAL_ERR("%s: an image-format AHB (format 0x%x) imported without VkMemoryDedicatedAllocateInfo.image: the "
                      "spec requires the dedicated image for non-BLOB buffers (VUID-VkMemoryAllocateInfo-pNext-02384) "
                      "-> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, b.desc.format);
            r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            goto fail_buf;
        }
        if (!(fdp.memoryTypeBits & (1u << ai->memoryTypeIndex))) {
            NVHAL_ERR("%s: memory type %u is not allowed for this dma-buf (types 0x%x) -> "
                      "VK_ERROR_INVALID_EXTERNAL_HANDLE", who, ai->memoryTypeIndex, fdp.memoryTypeBits);
            r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            goto fail_buf;
        }
        r = import_dmabuf(d, &b, VK_NULL_HANDLE, ded_buffer, ai->memoryTypeIndex, ai->allocationSize, extra, alloc, who,
                          &mem);
        if (r != VK_SUCCESS) goto fail_buf;
    }
    struct ahb_mem *m = malloc(sizeof *m);
    if (!m || !nvhal_hash_put(&ahb_mems, NVHAL_ND_U64(mem), m)) {
        free(m);
        NVHAL_ERR("%s: out of host memory recording the AHB memory", who);
        if (w) { /* unrealise */
            pthread_mutex_lock(&w->lock);
            __atomic_store_n(&w->real, VK_NULL_HANDLE, __ATOMIC_RELEASE);
            w->memory = VK_NULL_HANDLE;
            pthread_mutex_unlock(&w->lock);
            d->DestroyImage(d->handle, real, alloc);
        }
        d->FreeMemory(d->handle, mem, alloc);
        r = VK_ERROR_OUT_OF_HOST_MEMORY;
        nvhal_exp_memstats_fail(ai->memoryTypeIndex, r);
        goto fail_buf;
    }
    m->dev = d;
    m->buffer = buf;
    m->image = w;
    if (!owned) ahb.acquire(buf);
    NVHAL_TRACE("%s: memory %p <- %s%s, type %u (dma-buf types 0x%x), AHB %p; " GB_FACTS, who, NVHAL_LOGH(mem),
                w ? "dedicated ICD image " : ded_buffer ? "dedicated buffer" : "plain BLOB import",
                w ? "of the AHB image" : "", ai->memoryTypeIndex, fdp.memoryTypeBits, (void *)buf, GB_ARGS(&b));
    *pMem = mem;
    return VK_SUCCESS;
fail_buf:
    if (owned) ahb.release(buf);
    return r;
}

void VKAPI_CALL nvhal_FreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *alloc) {
    struct nvhal_device *d = nvhal_device_get(device);
    if (!d) {
        NVHAL_ERR("vkFreeMemory(%p): not a device created through this HAL", (void *)device);
        return;
    }
    struct ahb_mem *m = memory ? nvhal_hash_del(&ahb_mems, NVHAL_ND_U64(memory)) : NULL;
    d->FreeMemory(device, memory, alloc);
    if (m) {
        if (m->image) {
            pthread_mutex_lock(&m->image->lock);
            m->image->memory = VK_NULL_HANDLE;
            pthread_mutex_unlock(&m->image->lock);
        }
        ahb.release(m->buffer);
        NVHAL_TRACE("vkFreeMemory: AHB memory %p freed, AHB %p released", NVHAL_LOGH(memory), (void *)m->buffer);
        free(m);
    }
}

VkResult VKAPI_CALL nvhal_GetMemoryAndroidHardwareBufferANDROID(VkDevice device,
                                                                const VkMemoryGetAndroidHardwareBufferInfoANDROID *info,
                                                                struct AHardwareBuffer **pBuffer) {
    struct nvhal_device *d = nvhal_device_get(device);
    if (!d || !d->ahb_enabled) {
        NVHAL_ERR("vkGetMemoryAndroidHardwareBufferANDROID(%p): %s", (void *)device,
                  !d ? "not a device created through this HAL" : "the extension is not enabled on this device");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct ahb_mem *m = nvhal_hash_get(&ahb_mems, NVHAL_ND_U64(info->memory));
    if (!m) {
        NVHAL_ERR("vkGetMemoryAndroidHardwareBufferANDROID: memory %p was not imported from or exported as an AHB -> "
                  "VK_ERROR_INVALID_EXTERNAL_HANDLE", NVHAL_LOGH(info->memory));
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    ahb.acquire(m->buffer); /* the spec: a new reference, in addition to the memory's */
    *pBuffer = m->buffer;
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ bind */

VkResult nvhal_ahb_translate_binds(uint32_t n, const VkBindImageMemoryInfo *in, VkBindImageMemoryInfo *stack,
                                   uint32_t stack_n, VkBindImageMemoryInfo **out) {
    *out = (VkBindImageMemoryInfo *)in;
    if (nvhal_no_wrappers()) return VK_SUCCESS;
    VkBindImageMemoryInfo *c = NULL;
    for (uint32_t i = 0; i < n; i++) {
        struct nvhal_image *w = wrapper(in[i].image);
        if (!w) continue;
        if (w->kind == NVHAL_WRAP_SWAPCHAIN) {
            /* bind2 (anb.c) consumes every bind of these; what is left is vkBindImageMemory */
            NVHAL_ERR("vkBindImageMemory: image %p was created with VkImageSwapchainCreateInfoKHR; it can only be bound "
                      "with vkBindImageMemory2 + VkBindImageMemorySwapchainInfoKHR (memory %p given) -> "
                      "VK_ERROR_INVALID_EXTERNAL_HANDLE", (void *)w, NVHAL_LOGH(in[i].memory));
            if (c && c != stack) free(c);
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        VkImage real = real_of(w);
        if (real == VK_NULL_HANDLE || w->memory != in[i].memory || in[i].memoryOffset != 0) {
            NVHAL_ERR("vkBindImageMemory: AHB image %p bound to memory %p offset %llu, but it can only be bound at "
                      "offset 0 to the memory its buffer was imported into (%p) -> VK_ERROR_INVALID_EXTERNAL_HANDLE",
                      (void *)w, NVHAL_LOGH(in[i].memory), (unsigned long long)in[i].memoryOffset, NVHAL_LOGH(w->memory));
            if (c && c != stack) free(c);
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        if (!c) {
            c = n <= stack_n ? stack : malloc(n * sizeof *c);
            if (!c) return VK_ERROR_OUT_OF_HOST_MEMORY;
            memcpy(c, in, n * sizeof *c);
        }
        c[i].image = real;
    }
    if (c) *out = c;
    return VK_SUCCESS;
}

VkResult VKAPI_CALL nvhal_BindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset) {
    struct nvhal_device *d = nvhal_device_get(device);
    if (!d) {
        NVHAL_ERR("vkBindImageMemory(%p): not a device created through this HAL", (void *)device);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    bool astc;
    VkResult r = nvhal_astc_bind1(d, image, memory, offset, &astc); /* the RGBA8 image + its block images */
    if (astc) return r;
    VkBindImageMemoryInfo bi = {VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, NULL, image, memory, offset}, *t;
    r = nvhal_ahb_translate_binds(1, &bi, &bi, 1, &t);
    if (r != VK_SUCCESS) return r;
    return d->xl.BindImageMemory(device, t->image, memory, offset);
}

/* ------------------------------------------------------------------ properties */

static VkResult ahb_format_props(struct nvhal_device *d, const struct gbuf *b, VkFormat *format, uint64_t *external,
                                 VkFormatFeatureFlags2 *features, const char *who) {
    if (b->blob) {
        NVHAL_ERR("%s: a BLOB buffer has no image format (format properties chained) -> "
                  "VK_ERROR_INVALID_EXTERNAL_HANDLE", who);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    VkFormatFeatureFlags2 f = modifier_features(d->inst, d->physical_device, b->format, b->g->format_modifier);
    if (!f) {
        NVHAL_ERR("%s: the ICD lists modifier 0x%016" PRIx64 " for no use of VkFormat %d; " GB_FACTS
                  " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, b->g->format_modifier, b->format, GB_ARGS(b));
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    if (b->yuv) {
        /* external-only format (not in the spec's equivalence table; Skia requires UNDEFINED for it, GAU:115-160).
         * The spec requires SAMPLED_IMAGE plus MIDPOINT or COSITED chroma samples; the driver must report them
         * itself (Mesa adds MIDPOINT unconditionally, vk_android.c:850; this HAL does not invent features). */
        const VkFormatFeatureFlags2 chroma = VK_FORMAT_FEATURE_2_MIDPOINT_CHROMA_SAMPLES_BIT |
                                             VK_FORMAT_FEATURE_2_COSITED_CHROMA_SAMPLES_BIT;
        if (!(f & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) || !(f & chroma)) {
            NVHAL_ERR("%s: the ICD's LINEAR features for VkFormat %d are 0x%" PRIx64 ", without SAMPLED_IMAGE or a "
                      "chroma-sample location; " GB_FACTS " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, b->format,
                      (uint64_t)f, GB_ARGS(b));
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        *format = VK_FORMAT_UNDEFINED;
    } else {
        *format = b->format;
    }
    *external = (uint64_t)b->format; /* non-zero (spec; ANGLE asserts it, HBS:270); a VkFormat, like Mesa */
    *features = f;
    return VK_SUCCESS;
}

/* Suggested Ycbcr conversion of a YUV buffer, from its gralloc4 Dataspace metadata. minigbm keeps
 * CrosGralloc4Metadata at the start of the handle's reserved region (fd fds[num_planes], size
 * reserved_region_size; cros_gralloc/gralloc4/CrosGralloc4Mapper.cc getReservedRegionArea MAPPER4_METADATA):
 * char name[1024]; BlendMode blendMode (int32); Dataspace dataspace (int32) -> offset 1028. The mapping of the
 * standard and range fields is Mesa's (u_gralloc_imapper4_api.cpp mapper4_get_buffer_color_info), plus the
 * legacy HAL_DATASPACE_{JFIF,BT601_625,BT601_525,BT709} values (graphics-base-v1.0.h). Unknown/unset: BT.709
 * narrow (video). minigbm stores no ChromaSiting, so the chroma offsets stay MIDPOINT (Mesa's default). */
#define NVHAL_CROS_MD_DATASPACE_OFFSET 1028u
static void yuv_suggest(const struct gbuf *b, VkSamplerYcbcrModelConversion *model, VkSamplerYcbcrRange *range,
                        const char *who) {
    *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    *range = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
    const struct cros_gralloc_handle *g = b->g;
    int fd = g->num_planes < DRV_MAX_FDS ? g->fds[g->num_planes] : -1;
    int32_t ds = 0;
    if (fd >= 0 && g->reserved_region_size >= NVHAL_CROS_MD_DATASPACE_OFFSET + sizeof ds) {
        void *m = mmap(NULL, (size_t)g->reserved_region_size, PROT_READ, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) {
            NVHAL_WARN("%s: mmap of the gralloc metadata region (fd %d, %llu B) failed (%s); suggesting BT.709 "
                       "narrow", who, fd, (unsigned long long)g->reserved_region_size, strerror(errno));
            return;
        }
        memcpy(&ds, (const char *)m + NVHAL_CROS_MD_DATASPACE_OFFSET, sizeof ds);
        munmap(m, (size_t)g->reserved_region_size);
    }
    const uint32_t u = (uint32_t)ds, standard = u & (63u << 16), rng = u & (7u << 27);
    switch (u) { /* legacy dataspaces carry no standard/range fields */
    case 0x101: *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601; *range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL; break;
    case 0x102: case 0x103: *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601; break;
    case 0x104: break;
    default:
        switch (standard) {
        case 1u << 16: *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709; break;
        case 2u << 16: case 3u << 16: case 4u << 16: case 5u << 16:
            *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601; break;
        case 6u << 16: case 7u << 16: *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020; break;
        default: break;
        }
        if (rng == 1u << 27) *range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
        break;
    }
    NVHAL_TRACE("%s: dataspace 0x%x -> suggested model %d range %d", who, u, *model, *range);
}

static void ycbcr_identity(VkComponentMapping *c, VkSamplerYcbcrModelConversion *model, VkSamplerYcbcrRange *range,
                           VkChromaLocation *x, VkChromaLocation *y) {
    *c = (VkComponentMapping){VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                              VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
    *range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    *x = *y = VK_CHROMA_LOCATION_MIDPOINT;
}

VkResult VKAPI_CALL nvhal_GetAndroidHardwareBufferPropertiesANDROID(VkDevice device, const struct AHardwareBuffer *buffer,
                                                                    VkAndroidHardwareBufferPropertiesANDROID *p) {
    static const char who[] = "vkGetAndroidHardwareBufferPropertiesANDROID";
    struct nvhal_device *d = nvhal_device_get(device);
    if (!d || !d->ahb_enabled) {
        NVHAL_ERR("%s(%p): %s", who, (void *)device,
                  !d ? "not a device created through this HAL" : "the extension is not enabled on this device");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct gbuf b;
    VkResult r = gbuf_get(buffer, who, &b);
    if (r != VK_SUCCESS) return r;
    if (!(b.desc.usage & (AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                          AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER))) {
        NVHAL_ERR("%s: the buffer has no AHARDWAREBUFFER_USAGE_GPU_* usage (VUID-vkGetAndroidHardwareBufferPropertiesANDROID"
                  "-buffer-01884); " GB_FACTS " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, GB_ARGS(&b));
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    VkMemoryFdPropertiesKHR fdp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    r = d->GetMemoryFdPropertiesKHR(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, b.fd, &fdp);
    if (r != VK_SUCCESS) {
        NVHAL_ERR("%s: ICD vkGetMemoryFdPropertiesKHR(DMA_BUF) -> %s; " GB_FACTS, who, NVHAL_VKR(r), GB_ARGS(&b));
        return r == VK_ERROR_OUT_OF_HOST_MEMORY ? r : VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    uint64_t ext = 0;
    VkFormatFeatureFlags2 feat = 0;
    bool fmt_done = false;
    for (VkBaseOutStructure *s = p->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID ||
            s->sType == VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_2_ANDROID) {
            if (!fmt_done) {
                r = ahb_format_props(d, &b, &fmt, &ext, &feat, who);
                if (r != VK_SUCCESS) return r;
                fmt_done = true;
            }
        }
        if (s->sType == VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID) {
            VkAndroidHardwareBufferFormatPropertiesANDROID *f = (void *)s;
            f->format = fmt;
            f->externalFormat = ext;
            f->formatFeatures = (VkFormatFeatureFlags)(feat & 0x7fffffffu);
            ycbcr_identity(&f->samplerYcbcrConversionComponents, &f->suggestedYcbcrModel, &f->suggestedYcbcrRange,
                           &f->suggestedXChromaOffset, &f->suggestedYChromaOffset);
            if (b.yuv) yuv_suggest(&b, &f->suggestedYcbcrModel, &f->suggestedYcbcrRange, who);
        } else if (s->sType == VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_2_ANDROID) {
            VkAndroidHardwareBufferFormatProperties2ANDROID *f = (void *)s;
            f->format = fmt;
            f->externalFormat = ext;
            f->formatFeatures = feat;
            ycbcr_identity(&f->samplerYcbcrConversionComponents, &f->suggestedYcbcrModel, &f->suggestedYcbcrRange,
                           &f->suggestedXChromaOffset, &f->suggestedYChromaOffset);
            if (b.yuv) yuv_suggest(&b, &f->suggestedYcbcrModel, &f->suggestedYcbcrRange, who);
#ifdef VK_ANDROID_external_format_resolve
        } else if (s->sType == VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_RESOLVE_PROPERTIES_ANDROID) {
            /* VK_ANDROID_external_format_resolve is not offered: no color attachment format */
            ((VkAndroidHardwareBufferFormatResolvePropertiesANDROID *)s)->colorAttachmentFormat = VK_FORMAT_UNDEFINED;
#endif
        }
    }
    p->allocationSize = b.size;
    p->memoryTypeBits = fdp.memoryTypeBits;
    NVHAL_TRACE("%s: allocationSize %llu, memoryTypeBits 0x%x, VkFormat %d, externalFormat %" PRIu64 ", features2 "
                "0x%" PRIx64 "; " GB_FACTS, who, (unsigned long long)b.size, fdp.memoryTypeBits, fmt, ext,
                (uint64_t)feat, GB_ARGS(&b));
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ image format query */

static VkResult gpdifp2(VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *p,
                        bool khr) {
    static const char who[] = "vkGetPhysicalDeviceImageFormatProperties2(AHB)";
    struct nvhal_instance *inst = nvhal_physdev_instance(pd);
    if (!inst) {
        NVHAL_ERR("vkGetPhysicalDeviceImageFormatProperties2(%p): physical device was not enumerated through this HAL",
                  (void *)pd);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    PFN_vkGetPhysicalDeviceImageFormatProperties2 f = khr ? inst->GPDIFP2_khr : inst->GPDIFP2_core;
    const VkPhysicalDeviceExternalImageFormatInfo *ei = NULL;
    chain_find(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, (const void **)&ei);
    bool is_ahb = ei && ei->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    /* VkAndroidHardwareBufferUsageANDROID is ours: unlink it from the output chain for the ICD */
    VkBaseOutStructure *prev = (VkBaseOutStructure *)p, *us = NULL;
    for (VkBaseOutStructure *s = p->pNext; s; prev = s, s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID) { us = s; break; }
    if (us) prev->pNext = us->pNext;
    VkResult r;
    if (!is_ahb && nvhal_astc_gpdifp2(inst, pd, info, p, f, &r)) {
        /* answered by the ASTC emulation */
    } else if (!is_ahb) {
        r = f(pd, info, p);
    } else if (!nvhal_ahb_available) {
        nvhal_ahb_report_unavailable();
        r = VK_ERROR_FORMAT_NOT_SUPPORTED;
    } else {
        const VkPhysicalDeviceImageDrmFormatModifierInfoEXT *mi = NULL;
        chain_find(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, (const void **)&mi);
        uint64_t mods[32];
        uint32_t n = 0;
        if (mi || (info->tiling != VK_IMAGE_TILING_OPTIMAL && info->tiling != VK_IMAGE_TILING_LINEAR)) {
            NVHAL_TRACE("%s: tiling %d: the buffer's layout decides an AHB image's tiling; only OPTIMAL/LINEAR are "
                        "answered -> VK_ERROR_FORMAT_NOT_SUPPORTED", who, info->tiling);
        } else if (!vk_to_ahb(info->format)) {
            NVHAL_TRACE("%s: VkFormat %d has no AHB format the gralloc allocates -> VK_ERROR_FORMAT_NOT_SUPPORTED", who,
                        info->format);
        } else if (info->type != VK_IMAGE_TYPE_2D ||
                   (info->flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                                   VK_IMAGE_CREATE_SPARSE_ALIASED_BIT | VK_IMAGE_CREATE_DISJOINT_BIT))) {
            NVHAL_TRACE("%s: type %d flags 0x%x not implemented for AHB images -> VK_ERROR_FORMAT_NOT_SUPPORTED", who,
                        info->type, info->flags);
        } else {
            n = gralloc_modifiers(inst, pd, info->format, info->tiling == VK_IMAGE_TILING_LINEAR, mods, NULL, 32);
            if (!n)
                NVHAL_TRACE("%s: the ICD lists no %s modifier the gralloc would pick for VkFormat %d -> "
                            "VK_ERROR_FORMAT_NOT_SUPPORTED", who,
                            info->tiling == VK_IMAGE_TILING_LINEAR ? "LINEAR" : "block-linear", info->format);
        }
        r = n ? VK_SUCCESS : VK_ERROR_FORMAT_NOT_SUPPORTED;
        VkImageFormatProperties acc = {{UINT32_MAX, UINT32_MAX, UINT32_MAX}, UINT32_MAX, UINT32_MAX, ~0u, UINT64_MAX};
        unsigned char storage[1024];
        for (uint32_t i = 0; i < n && r == VK_SUCCESS; i++) {
            VkPhysicalDeviceImageDrmFormatModifierInfoEXT dm = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, NULL, mods[i], VK_SHARING_MODE_EXCLUSIVE,
                0, NULL};
            VkPhysicalDeviceExternalImageFormatInfo de = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &dm,
                                                          VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
            VkPhysicalDeviceImageFormatInfo2 q = *info;
            q.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            if (!nvhal_chain_replace(info->pNext, ei, &de, &dm.pNext, storage, sizeof storage, who, &q.pNext)) {
                r = VK_ERROR_FORMAT_NOT_SUPPORTED;
                break;
            }
            /* the same INPUT_ATTACHMENT-on-LINEAR rule as the realisation */
            r = f(pd, &q, p);
            if (r == VK_ERROR_FORMAT_NOT_SUPPORTED && mods[i] == DRM_FORMAT_MOD_LINEAR &&
                (q.usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) {
                q.usage &= ~VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
                r = f(pd, &q, p);
            }
            if (r != VK_SUCCESS) {
                NVHAL_TRACE("%s: VkFormat %d usage 0x%x flags 0x%x: modifier 0x%016" PRIx64 " -> %s", who, info->format,
                            info->usage, info->flags, mods[i], NVHAL_VKR(r));
                break;
            }
            VkImageFormatProperties *x = &p->imageFormatProperties;
            if (x->maxExtent.width < acc.maxExtent.width) acc.maxExtent.width = x->maxExtent.width;
            if (x->maxExtent.height < acc.maxExtent.height) acc.maxExtent.height = x->maxExtent.height;
            if (x->maxExtent.depth < acc.maxExtent.depth) acc.maxExtent.depth = x->maxExtent.depth;
            acc.sampleCounts &= x->sampleCounts;
            if (x->maxResourceSize < acc.maxResourceSize) acc.maxResourceSize = x->maxResourceSize;
        }
        if (r == VK_SUCCESS) {
            /* the gralloc allocates one level, one layer, one sample */
            acc.maxMipLevels = 1;
            acc.maxArrayLayers = 1;
            acc.sampleCounts &= VK_SAMPLE_COUNT_1_BIT;
            p->imageFormatProperties = acc;
            VkExternalImageFormatProperties *ep = NULL;
            for (VkBaseOutStructure *s = p->pNext; s; s = s->pNext)
                if (s->sType == VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES) ep = (void *)s;
            if (ep)
                ep->externalMemoryProperties = (VkExternalMemoryProperties){
                    VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT |
                        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT,
                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
            NVHAL_TRACE("%s: VkFormat %d tiling %d usage 0x%x flags 0x%x: supported over %u modifier(s), max %ux%u",
                        who, info->format, info->tiling, info->usage, info->flags, n, acc.maxExtent.width,
                        acc.maxExtent.height);
        } else {
            memset(&p->imageFormatProperties, 0, sizeof p->imageFormatProperties);
        }
    }
    if (us) {
        prev->pNext = us;
        ((VkAndroidHardwareBufferUsageANDROID *)us)->androidHardwareBufferUsage =
            r == VK_SUCCESS ? vk_to_ahb_usage(info->flags, info->usage) : 0;
    }
    return r;
}
VkResult VKAPI_CALL nvhal_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd,
                                                                  const VkPhysicalDeviceImageFormatInfo2 *i,
                                                                  VkImageFormatProperties2 *p) {
    return gpdifp2(pd, i, p, false);
}
VkResult VKAPI_CALL nvhal_GetPhysicalDeviceImageFormatProperties2KHR(VkPhysicalDevice pd,
                                                                     const VkPhysicalDeviceImageFormatInfo2 *i,
                                                                     VkImageFormatProperties2 *p) {
    return gpdifp2(pd, i, p, true);
}

static void gpdebp(VkPhysicalDevice pd, const VkPhysicalDeviceExternalBufferInfo *info, VkExternalBufferProperties *p,
                   bool khr) {
    struct nvhal_instance *inst = nvhal_physdev_instance(pd);
    if (!inst) {
        NVHAL_ERR("vkGetPhysicalDeviceExternalBufferProperties(%p): physical device was not enumerated through this "
                  "HAL", (void *)pd);
        memset(&p->externalMemoryProperties, 0, sizeof p->externalMemoryProperties);
        return;
    }
    PFN_vkGetPhysicalDeviceExternalBufferProperties f = khr ? inst->GPDEBP_khr : inst->GPDEBP_core;
    if (info->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) {
        f(pd, info, p);
        return;
    }
    VkPhysicalDeviceExternalBufferInfo q = *info;
    q.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    f(pd, &q, p);
    VkExternalMemoryProperties *e = &p->externalMemoryProperties;
    bool imp = nvhal_ahb_available && (e->externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
    /* import only: exporting a BLOB AHB is not implemented (AHB-CONTRACT.md §5) */
    e->externalMemoryFeatures = imp ? VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT : 0;
    e->compatibleHandleTypes = imp ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID : 0;
    e->exportFromImportedHandleTypes = imp ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID : 0;
    NVHAL_TRACE("vkGetPhysicalDeviceExternalBufferProperties(AHB, usage 0x%x) -> features 0x%x (from the ICD's DMA_BUF "
                "answer)", info->usage, e->externalMemoryFeatures);
}
void VKAPI_CALL nvhal_GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice pd,
                                                                const VkPhysicalDeviceExternalBufferInfo *i,
                                                                VkExternalBufferProperties *p) {
    gpdebp(pd, i, p, false);
}
void VKAPI_CALL nvhal_GetPhysicalDeviceExternalBufferPropertiesKHR(VkPhysicalDevice pd,
                                                                   const VkPhysicalDeviceExternalBufferInfo *i,
                                                                   VkExternalBufferProperties *p) {
    gpdebp(pd, i, p, true);
}

/* ------------------------------------------------------------------ buffers, Ycbcr conversions */

bool nvhal_buffer_ci_is_ahb(const VkBufferCreateInfo *ci) {
    const VkExternalMemoryBufferCreateInfo *e;
    return ci && chain_find(ci->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, (const void **)&e) &&
           (e->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID);
}

/* storage must hold a VkExternalMemoryBufferCreateInfo plus the structs chained before it */
VkResult nvhal_buffer_ci_translate(const VkBufferCreateInfo *ci, VkBufferCreateInfo *out, void *storage,
                                   size_t storage_size) {
    const VkExternalMemoryBufferCreateInfo *e = NULL;
    chain_find(ci->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, (const void **)&e);
    VkExternalMemoryBufferCreateInfo *n = storage;
    *n = *e;
    n->handleTypes = (e->handleTypes & ~VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) |
                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    *out = *ci;
    return nvhal_chain_replace(ci->pNext, e, n, &n->pNext, (char *)storage + sizeof *n, storage_size - sizeof *n,
                               "vkCreateBuffer(AHB)", &out->pNext)
               ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT;
}

VkResult VKAPI_CALL nvhal_CreateBuffer(VkDevice device, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *alloc,
                                       VkBuffer *pBuffer) {
    struct nvhal_device *d = nvhal_device_get(device);
    if (!d) {
        NVHAL_ERR("vkCreateBuffer(%p): not a device created through this HAL", (void *)device);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!nvhal_buffer_ci_is_ahb(ci)) return d->xl.CreateBuffer(device, ci, alloc, pBuffer);
    unsigned char storage[1024];
    VkBufferCreateInfo t;
    VkResult r = nvhal_buffer_ci_translate(ci, &t, storage, sizeof storage);
    if (r != VK_SUCCESS) return r;
    r = d->xl.CreateBuffer(device, &t, alloc, pBuffer);
    NVHAL_TRACE("vkCreateBuffer(AHB handle type -> DMA_BUF, %llu B, usage 0x%x) -> %s", (unsigned long long)ci->size,
                ci->usage, NVHAL_VKR(r));
    return r;
}

static VkResult create_ycbcr(VkDevice device, const VkSamplerYcbcrConversionCreateInfo *ci,
                             const VkAllocationCallbacks *alloc, VkSamplerYcbcrConversion *out, bool khr) {
    struct nvhal_device *d = nvhal_device_get(device);
    if (!d) {
        NVHAL_ERR("vkCreateSamplerYcbcrConversion(%p): not a device created through this HAL", (void *)device);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    PFN_vkCreateSamplerYcbcrConversion f = khr ? d->xl.CreateSamplerYcbcrConversionKHR : d->xl.CreateSamplerYcbcrConversion;
    const VkExternalFormatANDROID *xf = NULL;
    if (!chain_find(ci->pNext, VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID, (const void **)&xf)) return f(device, ci, alloc, out);
    VkSamplerYcbcrConversionCreateInfo t = *ci;
    if (xf->externalFormat) {
        if (ci->format != VK_FORMAT_UNDEFINED || !external_format_ok((VkFormat)xf->externalFormat)) {
            NVHAL_ERR("vkCreateSamplerYcbcrConversion: externalFormat %" PRIu64 " with format %d is not an external "
                      "format of this HAL -> VK_ERROR_OUT_OF_HOST_MEMORY (the only failure the command has)",
                      (uint64_t)xf->externalFormat, ci->format);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        t.format = (VkFormat)xf->externalFormat;
    }
    unsigned char storage[1024];
    if (!nvhal_chain_replace(ci->pNext, xf, NULL, NULL, storage, sizeof storage, "vkCreateSamplerYcbcrConversion",
                             &t.pNext))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult r = f(device, &t, alloc, out);
    NVHAL_TRACE("vkCreateSamplerYcbcrConversion: VkExternalFormatANDROID(%" PRIu64 ") -> format %d -> %s",
                (uint64_t)xf->externalFormat, t.format, NVHAL_VKR(r));
    return r;
}
VkResult VKAPI_CALL nvhal_CreateSamplerYcbcrConversion(VkDevice device, const VkSamplerYcbcrConversionCreateInfo *ci,
                                                       const VkAllocationCallbacks *alloc, VkSamplerYcbcrConversion *o) {
    return create_ycbcr(device, ci, alloc, o, false);
}
VkResult VKAPI_CALL nvhal_CreateSamplerYcbcrConversionKHR(VkDevice device, const VkSamplerYcbcrConversionCreateInfo *ci,
                                                          const VkAllocationCallbacks *alloc,
                                                          VkSamplerYcbcrConversion *o) {
    return create_ycbcr(device, ci, alloc, o, true);
}

/* ------------------------------------------------------------------ requirements without a buffer */

/* Requirements of a probe ICD image built from w with the modifiers the gralloc would choose. */
static void probe_requirements(struct nvhal_image *w, VkMemoryRequirements2 *out, const char *who) {
    struct nvhal_device *d = w->dev;
    uint64_t mods[32];
    uint32_t n = gralloc_modifiers(d->inst, d->physical_device, w->ci.format, w->ci.tiling == VK_IMAGE_TILING_LINEAR,
                                   mods, NULL, 32);
    memset(&out->memoryRequirements, 0, sizeof out->memoryRequirements);
    if (!n) {
        NVHAL_ERR("%s: no modifier for VkFormat %d; requirements are zero", who, w->ci.format);
        return;
    }
    struct icd_ci c;
    build_icd_ci(w, &c, 0, 0, NULL, NULL, mods, n);
    if (effective_usage(d, &c.ci, mods[0], &c.ci.usage, who) != VK_SUCCESS)
        NVHAL_WARN("%s: the ICD refuses usage 0x%x for modifier 0x%016" PRIx64 "; probing anyway", who, c.ci.usage, mods[0]);
    VkImage img = VK_NULL_HANDLE;
    VkResult r = d->CreateImage(d->handle, &c.ci, NULL, &img);
    if (r != VK_SUCCESS) {
        NVHAL_ERR("%s: ICD vkCreateImage(probe, %u modifiers) -> %s; requirements are zero", who, n, NVHAL_VKR(r));
        return;
    }
    VkImageMemoryRequirementsInfo2 mri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, NULL, img};
    d->anb_GetImageMemoryRequirements2(d->handle, &mri, out);
    d->DestroyImage(d->handle, img, NULL);
    for (VkBaseOutStructure *s = out->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            VkMemoryDedicatedRequirements *dr = (void *)s;
            dr->prefersDedicatedAllocation = dr->requiresDedicatedAllocation = VK_TRUE; /* AHB images always are */
        }
    NVHAL_WARN("%s: memory requirements of an AHB image before it has memory are not defined by the spec "
               "(VUID-VkImageMemoryRequirementsInfo2-image-01897: the image must be bound); answering from a probe "
               "image (%u gralloc modifiers): %llu B, alignment %llu, types 0x%x. The import uses "
               "vkGetAndroidHardwareBufferPropertiesANDROID's allocationSize", who, n,
               (unsigned long long)out->memoryRequirements.size, (unsigned long long)out->memoryRequirements.alignment,
               out->memoryRequirements.memoryTypeBits);
}

/* Requirements of an unbound swapchain-bound image. Approximation: the image will be created on the
 * gralloc buffer with that buffer's DRM modifier, which is unknown until the bind; the probe is an ICD
 * image of the app's create info as given (tiling OPTIMAL in every known caller, no external memory).
 * The bind does not use the answer: the memory comes from the swapchain, never from an allocation the
 * app sizes with it (memory must be VK_NULL_HANDLE). */
static void sc_probe_requirements(struct nvhal_image *w, VkMemoryRequirements2 *out, const char *who) {
    struct nvhal_device *d = w->dev;
    struct icd_ci c;
    sc_icd_ci(w, &c);
    memset(&out->memoryRequirements, 0, sizeof out->memoryRequirements);
    VkImage img = VK_NULL_HANDLE;
    VkResult r = d->CreateImage(d->handle, &c.ci, w->alloc, &img);
    if (r != VK_SUCCESS) {
        NVHAL_ERR("%s: ICD vkCreateImage(probe, tiling %d, usage 0x%x, flags 0x%x) -> %s; requirements are zero", who,
                  c.ci.tiling, c.ci.usage, c.ci.flags, NVHAL_VKR(r));
        return;
    }
    VkImageMemoryRequirementsInfo2 mri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, NULL, img};
    d->anb_GetImageMemoryRequirements2(d->handle, &mri, out);
    d->DestroyImage(d->handle, img, w->alloc);
    NVHAL_TRACE("%s: swapchain-bound image %p before its bind: answering from a probe image (tiling %d): %llu B, "
                "alignment %llu, types 0x%x", who, (void *)w, c.ci.tiling,
                (unsigned long long)out->memoryRequirements.size, (unsigned long long)out->memoryRequirements.alignment,
                out->memoryRequirements.memoryTypeBits);
}

void nvhal_ahb_image_requirements(struct nvhal_device *d, VkImage image, VkMemoryRequirements2 *out) {
    (void)d;
    struct nvhal_image *w = wrapper(image);
    if (!w) return;
    if (w->kind == NVHAL_WRAP_SWAPCHAIN)
        sc_probe_requirements(w, out, "vkGetImageMemoryRequirements(swapchain-bound image before its bind)");
    else
        probe_requirements(w, out, "vkGetImageMemoryRequirements(AHB image without memory)");
}

void nvhal_ahb_ci_requirements(struct nvhal_device *d, const VkImageCreateInfo *ci, VkMemoryRequirements2 *out,
                               bool khr) {
    (void)khr;
    struct nvhal_image w;
    memset(&out->memoryRequirements, 0, sizeof out->memoryRequirements);
    if (parse_ci(ci, "vkGetDeviceImageMemoryRequirements(AHB)", &w) == VK_SUCCESS) {
        w.dev = d;
        probe_requirements(&w, out, "vkGetDeviceImageMemoryRequirements(AHB)");
    }
    free(w.qfi);
    free(w.view_formats);
}

/* ------------------------------------------------------------------ object state before realisation */

bool nvhal_ahb_set_private_data(uint64_t handle, VkPrivateDataSlot slot, uint64_t data) {
    struct nvhal_image *w = wrapper(NVHAL_U64_ND(VkImage, handle));
    if (!w) return false;
    pthread_mutex_lock(&w->lock);
    bool done = false;
    for (uint32_t i = 0; i < w->pd_n; i++)
        if (w->pd[i].slot == NVHAL_ND_U64(slot)) { w->pd[i].data = data; done = true; }
    if (!done) {
        struct pdata *p = realloc(w->pd, (w->pd_n + 1) * sizeof *p);
        if (p) {
            w->pd = p;
            w->pd[w->pd_n++] = (struct pdata){NVHAL_ND_U64(slot), data};
        } else {
            NVHAL_ERR("vkSetPrivateData: out of host memory storing private data of AHB image %p", (void *)w);
        }
    }
    pthread_mutex_unlock(&w->lock);
    return true;
}

bool nvhal_ahb_get_private_data(uint64_t handle, VkPrivateDataSlot slot, uint64_t *data) {
    struct nvhal_image *w = wrapper(NVHAL_U64_ND(VkImage, handle));
    if (!w) return false;
    *data = 0;
    pthread_mutex_lock(&w->lock);
    for (uint32_t i = 0; i < w->pd_n; i++)
        if (w->pd[i].slot == NVHAL_ND_U64(slot)) *data = w->pd[i].data;
    pthread_mutex_unlock(&w->lock);
    return true;
}

bool nvhal_ahb_set_object_name(uint64_t handle, const char *name) {
    struct nvhal_image *w = wrapper(NVHAL_U64_ND(VkImage, handle));
    if (!w) return false;
    pthread_mutex_lock(&w->lock);
    free(w->name);
    w->name = name ? strdup(name) : NULL;
    pthread_mutex_unlock(&w->lock);
    return true;
}

/* ------------------------------------------------------------------ device lifetime */

static bool of_device(void *v, void *arg) {
    return ((struct nvhal_image *)v)->dev == arg;
}
/* a leaked swapchain-bound image: its imported memory is the HAL's (as nvhal_anb_device_teardown) */
static void wrapper_drop(void *v) {
    struct nvhal_image *w = v;
    if (w->kind != NVHAL_WRAP_SWAPCHAIN || w->memory == VK_NULL_HANDLE) return;
    VkImage real = real_of(w);
    if (real != VK_NULL_HANDLE) w->dev->DestroyImage(w->dev->handle, real, w->alloc);
    w->dev->FreeMemory(w->dev->handle, w->memory, w->alloc);
    w->memory = VK_NULL_HANDLE;
}
static bool mem_of_device(void *v, void *arg) { return ((struct ahb_mem *)v)->dev == arg; }
static void mem_release(void *v) {
    struct ahb_mem *m = v;
    ahb.release(m->buffer);
    free(m);
}

void nvhal_ahb_device_teardown(struct nvhal_device *d, const VkAllocationCallbacks *alloc) {
    (void)alloc;
    if (nvhal_no_wrappers() && !ahb_mems.n) return;
    /* leaked by the app: drop our records (the ICD frees its objects with the device) */
    size_t nw = nvhal_hash_sweep(&wrappers, of_device, d, wrapper_drop);
    if (nw) {
        atomic_fetch_sub_explicit(&wrapper_count, (unsigned)nw, memory_order_release);
        NVHAL_ERR("vkDestroyDevice: %zu AHB / swapchain-bound image(s) were not destroyed (their records are dropped, "
                  "not freed: the wrappers may still be referenced; a swapchain-bound image's imported memory is "
                  "freed)", nw);
    }
    size_t nm = nvhal_hash_sweep(&ahb_mems, mem_of_device, d, mem_release);
    if (nm) NVHAL_ERR("vkDestroyDevice: %zu AHB-backed memory object(s) were not freed; their AHBs are released", nm);
}
