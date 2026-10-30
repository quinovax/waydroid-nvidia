/* VK_ANDROID_native_buffer (spec version 8) on top of NVIDIA's dma-buf / DRM-modifier /
 * sync_fd support. Semantics mirror Mesa 25.2.0 src/vulkan/runtime/vk_android.c
 * (gralloc usage mapping, AcquireImageANDROID fd ownership, release fence export);
 * the buffer handle is minigbm's struct cros_gralloc_handle (include/minigbm/). */
#include "nvhal.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <drm/drm_fourcc.h>
#include <hardware/gralloc_usage.h>
#include <cros_gralloc_handle.h>

/* ------------------------------------------------------------------ formats */

/* VkFormat of a swapchain image -> DRM fourccs minigbm may have allocated for it. The list
 * covers every format the Android 13 loader offers for a surface (swapchain.cpp:764-821). */
static const struct { VkFormat vk; uint32_t fourcc[2]; } anb_formats[] = {
    {VK_FORMAT_R8G8B8A8_UNORM, {DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888}},
    {VK_FORMAT_R8G8B8A8_SRGB, {DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888}},
    {VK_FORMAT_B8G8R8A8_UNORM, {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888}},
    {VK_FORMAT_B8G8R8A8_SRGB, {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888}},
    {VK_FORMAT_R5G6B5_UNORM_PACK16, {DRM_FORMAT_RGB565, DRM_FORMAT_RGB565}},
    {VK_FORMAT_R16G16B16A16_SFLOAT, {DRM_FORMAT_ABGR16161616F, DRM_FORMAT_XBGR16161616F}},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, {DRM_FORMAT_ABGR2101010, DRM_FORMAT_XBGR2101010}},
    {VK_FORMAT_R8_UNORM, {DRM_FORMAT_R8, DRM_FORMAT_R8}},
};

static int anb_format_index(VkFormat f) {
    for (size_t i = 0; i < sizeof anb_formats / sizeof *anb_formats; i++)
        if (anb_formats[i].vk == f) return (int)i;
    return -1;
}

bool nvhal_anb_format_known(VkFormat format) { return anb_format_index(format) >= 0; }

void nvhal_fourcc_str(uint32_t f, char out[5]) {
    for (int i = 0; i < 4; i++) {
        char c = (char)((f >> (8 * i)) & 0xff);
        out[i] = (c >= 32 && c < 127) ? c : '?';
    }
    out[4] = 0;
}

static struct nvhal_device *anb_device(VkDevice device, const char *fn) {
    struct nvhal_device *d = nvhal_device_get(device);
    if (!d) {
        NVHAL_ERR("%s(%p): not a device created through this HAL", fn, (void *)device);
        return NULL;
    }
    if (!d->anb_enabled) {
        NVHAL_ERR("%s(%p): VK_ANDROID_native_buffer was not enabled on this device", fn, (void *)device);
        return NULL;
    }
    return d;
}

/* ------------------------------------------------------------------ gralloc usage */

static const VkImageUsageFlags render_usage =
    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
static const VkImageUsageFlags texture_usage =
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;

static VkResult usage_check(const char *fn, VkFormat format, VkImageUsageFlags usage) {
    if (anb_format_index(format) < 0) {
        NVHAL_ERR("%s: VkFormat %d has no gralloc/DRM mapping in this HAL -> VK_ERROR_FORMAT_NOT_SUPPORTED",
                  fn, format);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (usage & ~(render_usage | texture_usage) || !(usage & (render_usage | texture_usage))) {
        NVHAL_ERR("%s: image usage 0x%x cannot be expressed as gralloc usage (allowed: 0x%x) -> "
                  "VK_ERROR_FORMAT_NOT_SUPPORTED", fn, usage, render_usage | texture_usage);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    return VK_SUCCESS;
}

VkResult VKAPI_CALL nvhal_GetSwapchainGrallocUsageANDROID(VkDevice device, VkFormat format,
                                                          VkImageUsageFlags imageUsage, int *grallocUsage) {
    if (!anb_device(device, "vkGetSwapchainGrallocUsageANDROID")) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = usage_check("vkGetSwapchainGrallocUsageANDROID", format, imageUsage);
    if (r != VK_SUCCESS) return r;
    int u = GRALLOC_USAGE_HW_COMPOSER;
    if (imageUsage & render_usage) u |= GRALLOC_USAGE_HW_RENDER;
    if (imageUsage & texture_usage) u |= GRALLOC_USAGE_HW_TEXTURE;
    *grallocUsage = u;
    NVHAL_TRACE("vkGetSwapchainGrallocUsageANDROID(format %d, usage 0x%x) -> 0x%x", format, imageUsage, u);
    return VK_SUCCESS;
}

VkResult VKAPI_CALL nvhal_GetSwapchainGrallocUsage2ANDROID(VkDevice device, VkFormat format,
                                                           VkImageUsageFlags imageUsage,
                                                           VkSwapchainImageUsageFlagsANDROID swapchainUsage,
                                                           uint64_t *consumer, uint64_t *producer) {
    if (!anb_device(device, "vkGetSwapchainGrallocUsage2ANDROID")) return VK_ERROR_INITIALIZATION_FAILED;
    if (swapchainUsage & VK_SWAPCHAIN_IMAGE_USAGE_SHARED_BIT_ANDROID) {
        /* This HAL reports sharedImage = VK_FALSE, so the loader should never ask. */
        NVHAL_ERR("vkGetSwapchainGrallocUsage2ANDROID: shared presentable image requested, but this HAL "
                  "does not support front-buffer rendering -> VK_ERROR_FORMAT_NOT_SUPPORTED");
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    VkResult r = usage_check("vkGetSwapchainGrallocUsage2ANDROID", format, imageUsage);
    if (r != VK_SUCCESS) return r;
    *consumer = GRALLOC1_CONSUMER_USAGE_HWCOMPOSER;
    *producer = 0;
    if (imageUsage & render_usage) *producer |= GRALLOC1_PRODUCER_USAGE_GPU_RENDER_TARGET;
    if (imageUsage & texture_usage) *consumer |= GRALLOC1_CONSUMER_USAGE_GPU_TEXTURE;
    /* VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR is not visible here: the loader ORs
     * BufferUsage::PROTECTED itself (swapchain.cpp:1351-1357). */
    NVHAL_TRACE("vkGetSwapchainGrallocUsage2ANDROID(format %d, usage 0x%x) -> consumer 0x%" PRIx64
                " producer 0x%" PRIx64, format, imageUsage, *consumer, *producer);
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ image import */

static void anb_track(struct nvhal_device *d, VkImage img, VkDeviceMemory mem, bool *ok) {
    pthread_mutex_lock(&nvhal_lock);
    *ok = true;
    if (d->anb_count == d->anb_cap) {
        uint32_t cap = d->anb_cap ? d->anb_cap * 2 : 8;
        struct nvhal_anb_image *a = realloc(d->anb_images, cap * sizeof *a);
        if (!a) *ok = false;
        else { d->anb_images = a; d->anb_cap = cap; }
    }
    if (*ok) {
        d->anb_images[d->anb_count].image = img;
        d->anb_images[d->anb_count].memory = mem;
        d->anb_count++;
    }
    pthread_mutex_unlock(&nvhal_lock);
}

static VkDeviceMemory anb_untrack(struct nvhal_device *d, VkImage img) {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    pthread_mutex_lock(&nvhal_lock);
    for (uint32_t i = 0; i < d->anb_count; i++)
        if (d->anb_images[i].image == img) {
            mem = d->anb_images[i].memory;
            d->anb_images[i] = d->anb_images[--d->anb_count];
            break;
        }
    pthread_mutex_unlock(&nvhal_lock);
    return mem;
}

VkResult nvhal_modifier_usage_ok(struct nvhal_device *d, const VkImageCreateInfo *ci, uint64_t modifier,
                                 VkImageUsageFlags usage) {
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = modifier,
        .sharingMode = ci->sharingMode,
        .queueFamilyIndexCount = ci->sharingMode == VK_SHARING_MODE_CONCURRENT ? ci->queueFamilyIndexCount : 0,
        .pQueueFamilyIndices = ci->sharingMode == VK_SHARING_MODE_CONCURRENT ? ci->pQueueFamilyIndices : NULL,
    };
    /* MUTABLE_FORMAT with DRM-modifier tiling needs the view-format list in the query too
     * (VUID-VkPhysicalDeviceImageFormatInfo2-tiling-02313) */
    VkImageFormatListCreateInfo fl;
    for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            fl = *(const VkImageFormatListCreateInfo *)s;
            fl.pNext = NULL;
            mod.pNext = &fl;
            break;
        }
    VkPhysicalDeviceExternalImageFormatInfo ext = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = &mod,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &ext,
        .format = ci->format,
        .type = ci->imageType,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage,
        .flags = ci->flags,
    };
    VkExternalImageFormatProperties eprops = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &eprops};
    VkResult r = d->inst->GetPhysicalDeviceImageFormatProperties2(d->physical_device, &info, &props);
    if (r == VK_SUCCESS &&
        !(eprops.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
        r = VK_ERROR_FORMAT_NOT_SUPPORTED;
    return r;
}

VkResult nvhal_cros_handle(const native_handle_t *h, const char *who, const struct cros_gralloc_handle **out) {
    if (!h) {
        NVHAL_ERR("%s: the buffer handle is NULL -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    if (h->version != (int)sizeof(native_handle_t) || h->numFds < 1 || h->numFds > DRV_MAX_FDS ||
        h->numInts < 0 || (size_t)h->numFds + (size_t)h->numInts != CROS_GRALLOC_HANDLE_NUM_INTS_TOTAL) {
        NVHAL_ERR("%s: handle %p is not a minigbm cros_gralloc_handle (version %d, numFds %d, "
                  "numInts %d; expected version %zu, 1..%d fds, fds+ints == %zu). Remedy: the guest must run "
                  "minigbm gralloc (ro.hardware.gralloc=minigbm_gbm_mesa) -> VK_ERROR_INVALID_EXTERNAL_HANDLE",
                  who, (const void *)h, h->version, h->numFds, h->numInts, sizeof(native_handle_t), DRV_MAX_FDS,
                  (size_t)CROS_GRALLOC_HANDLE_NUM_INTS_TOTAL);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    const struct cros_gralloc_handle *g = (const struct cros_gralloc_handle *)h;
    if (g->magic != CROS_GRALLOC_MAGIC) {
        NVHAL_ERR("%s: handle %p magic 0x%08x, expected 0x%08x (not a minigbm buffer). Remedy: "
                  "the guest must run minigbm gralloc -> VK_ERROR_INVALID_EXTERNAL_HANDLE",
                  who, (const void *)h, g->magic, CROS_GRALLOC_MAGIC);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    *out = g;
    return VK_SUCCESS;
}

VkResult nvhal_anb_import(struct nvhal_device *d, const VkImageCreateInfo *ci, const VkNativeBufferANDROID *nb,
                          const VkAllocationCallbacks *alloc, const char *who, VkImage *pImage, VkDeviceMemory *pMem) {
    VkDevice device = d->handle;
    const struct cros_gralloc_handle *g;
    VkResult hr = nvhal_cros_handle(nb->handle, who, &g);
    if (hr != VK_SUCCESS) return hr;
    char fcc[5];
    nvhal_fourcc_str(g->format, fcc);
    const uint64_t modifier = g->format_modifier;
    const uint32_t stride = g->strides[0], offset = g->offsets[0];
#define FACTS "fourcc %s (0x%08x), modifier 0x%016" PRIx64 ", plane0 stride %u B offset %u, %ux%u, " \
              "planes %u, droid_format %d, usage 0x%x, VkFormat %d, extent %ux%u"
#define FACTS_ARGS fcc, g->format, modifier, stride, offset, g->width, g->height, g->num_planes, \
                   g->droid_format, (unsigned)g->usage, ci->format, ci->extent.width, ci->extent.height
    if (g->num_planes != 1 || g->fds[0] < 0) {
        NVHAL_ERR("%s: only single-plane buffers are implemented (%s); " FACTS
                  " -> VK_ERROR_FORMAT_NOT_SUPPORTED", who, g->fds[0] < 0 ? "fds[0] < 0" : "multi-planar", FACTS_ARGS);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (modifier == DRM_FORMAT_MOD_INVALID) {
        NVHAL_ERR("%s: the buffer carries DRM_FORMAT_MOD_INVALID (implicit layout), which "
                  "cannot be imported with an explicit modifier; " FACTS " -> VK_ERROR_INVALID_EXTERNAL_HANDLE",
                  who, FACTS_ARGS);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    int fi = anb_format_index(ci->format);
    if (fi < 0 || (anb_formats[fi].fourcc[0] != g->format && anb_formats[fi].fourcc[1] != g->format)) {
        NVHAL_ERR("%s: VkFormat %d does not match the gralloc buffer's DRM format; " FACTS
                  " -> VK_ERROR_FORMAT_NOT_SUPPORTED", who, ci->format, FACTS_ARGS);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (ci->imageType != VK_IMAGE_TYPE_2D || ci->mipLevels != 1 || ci->arrayLayers != 1 ||
        ci->extent.depth != 1 || ci->extent.width > g->width || ci->extent.height > g->height) {
        NVHAL_ERR("%s: image (type %d, %u mips, %u layers, %ux%ux%u) does not fit the "
                  "buffer; " FACTS " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, ci->imageType, ci->mipLevels,
                  ci->arrayLayers, ci->extent.width, ci->extent.height, ci->extent.depth, FACTS_ARGS);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    /* Rebuild pNext without the ANB structs. Other structs must be copied to be dropped from
     * the (const) chain, so only the known ones are accepted. */
    VkImageFormatListCreateInfo fmt_list;
    VkImageStencilUsageCreateInfo stencil;
    const void *kept = NULL;
    for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext) {
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID:
        case VK_STRUCTURE_TYPE_SWAPCHAIN_IMAGE_CREATE_INFO_ANDROID:
            break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO:
            fmt_list = *(const VkImageFormatListCreateInfo *)s;
            fmt_list.pNext = kept;
            kept = &fmt_list;
            break;
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO:
            stencil = *(const VkImageStencilUsageCreateInfo *)s;
            stencil.pNext = kept;
            kept = &stencil;
            break;
        default:
            NVHAL_ERR("%s: unsupported structure (sType %d) chained next to "
                      "VkNativeBufferANDROID -> VK_ERROR_FEATURE_NOT_PRESENT", who, (int)s->sType);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    VkSubresourceLayout plane = {.offset = offset, .size = 0, .rowPitch = stride, .arrayPitch = 0, .depthPitch = 0};
    VkImageDrmFormatModifierExplicitCreateInfoEXT drm = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .pNext = kept,
        .drmFormatModifier = modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &plane,
    };
    VkExternalMemoryImageCreateInfo ext = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &drm,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo ici = *ci;
    ici.pNext = &ext;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;

    /* Ask the ICD before creating: a clear refusal beats a failed import. */
    VkResult r = nvhal_modifier_usage_ok(d, &ici, modifier, ici.usage);
    if (r != VK_SUCCESS && modifier == DRM_FORMAT_MOD_LINEAR && (ici.usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) &&
        nvhal_modifier_usage_ok(d, &ici, modifier, ici.usage & ~VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) == VK_SUCCESS) {
        NVHAL_WARN("%s: the ICD rejects INPUT_ATTACHMENT usage on a LINEAR dma-buf and "
                   "accepts the rest; importing without VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT (usage 0x%x -> 0x%x)",
                   who, ici.usage, ici.usage & ~VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
        ici.usage &= ~VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        r = VK_SUCCESS;
    }
    if (r != VK_SUCCESS) {
        NVHAL_ERR("%s: ICD vkGetPhysicalDeviceImageFormatProperties2(DRM modifier, DMA_BUF "
                  "import, usage 0x%x, flags 0x%x) -> %s; " FACTS, who, ici.usage, ici.flags, NVHAL_VKR(r),
                  FACTS_ARGS);
        return r;
    }

    VkImage img = VK_NULL_HANDLE;
    r = d->CreateImage(device, &ici, alloc, &img);
    if (r != VK_SUCCESS) {
        NVHAL_ERR("%s: ICD vkCreateImage(DRM modifier explicit) -> %s; " FACTS,
                  who, NVHAL_VKR(r), FACTS_ARGS);
        return r;
    }
    VkDeviceMemory mem = VK_NULL_HANDLE;
    int fd = fcntl(g->fds[0], F_DUPFD_CLOEXEC, 0);
    if (fd < 0) {
        int e = errno;
        NVHAL_ERR("%s: dup of dma-buf fd %d failed: %s", who, g->fds[0], strerror(e));
        r = e == EMFILE ? VK_ERROR_TOO_MANY_OBJECTS : VK_ERROR_OUT_OF_HOST_MEMORY;
        goto fail;
    }
    off_t buf_size = lseek(fd, 0, SEEK_END);
    if (buf_size < 0 || lseek(fd, 0, SEEK_SET) < 0) {
        NVHAL_ERR("%s: lseek on dma-buf fd %d failed (%s); its size cannot be checked; " FACTS,
                  who, g->fds[0], strerror(errno), FACTS_ARGS);
        r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
        goto fail;
    }
    VkMemoryFdPropertiesKHR fdp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    r = d->GetMemoryFdPropertiesKHR(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdp);
    if (r != VK_SUCCESS) {
        NVHAL_ERR("%s: ICD vkGetMemoryFdPropertiesKHR(DMA_BUF) -> %s; " FACTS,
                  who, NVHAL_VKR(r), FACTS_ARGS);
        goto fail;
    }
    VkMemoryDedicatedRequirements dreq = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 mreq = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dreq};
    VkImageMemoryRequirementsInfo2 mri = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = img};
    d->anb_GetImageMemoryRequirements2(device, &mri, &mreq);
    const VkDeviceSize need = mreq.memoryRequirements.size;
    if ((uint64_t)buf_size < need) {
        NVHAL_ERR("%s: dma-buf is %lld bytes but the image needs %llu; " FACTS
                  " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, (long long)buf_size, (unsigned long long)need, FACTS_ARGS);
        r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
        goto fail;
    }
    uint32_t bits = fdp.memoryTypeBits & mreq.memoryRequirements.memoryTypeBits;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < d->mem_props.memoryTypeCount && type == UINT32_MAX; i++)
        if ((bits & (1u << i)) && (d->mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    for (uint32_t i = 0; i < d->mem_props.memoryTypeCount && type == UINT32_MAX; i++)
        if (bits & (1u << i)) type = i;
    if (type == UINT32_MAX) {
        NVHAL_ERR("%s: no memory type is allowed both by the dma-buf (0x%x) and by the image "
                  "(0x%x); " FACTS " -> VK_ERROR_INVALID_EXTERNAL_HANDLE", who, fdp.memoryTypeBits,
                  mreq.memoryRequirements.memoryTypeBits, FACTS_ARGS);
        r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
        goto fail;
    }
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = img};
    VkImportMemoryFdInfoKHR imp = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &ded,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = fd,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = need, .memoryTypeIndex = type,
    };
    r = d->AllocateMemory(device, &mai, alloc, &mem);
    if (r != VK_SUCCESS) {
        NVHAL_ERR("%s: ICD vkAllocateMemory(import DMA_BUF, dedicated, %llu bytes, type %u) -> %s; "
                  FACTS, who, (unsigned long long)need, type, NVHAL_VKR(r), FACTS_ARGS);
        mem = VK_NULL_HANDLE;
        goto fail;
    }
    fd = -1; /* a successful import owns the fd */
    VkBindImageMemoryInfo bind = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, .image = img, .memory = mem};
    r = d->anb_BindImageMemory2(device, 1, &bind);
    if (r != VK_SUCCESS) {
        NVHAL_ERR("%s: ICD vkBindImageMemory2 -> %s; " FACTS, who, NVHAL_VKR(r), FACTS_ARGS);
        goto fail;
    }
    NVHAL_TRACE("%s: image %p <- dma-buf %lld B, memory type %u, %llu B dedicated; " FACTS,
                who, (void *)(uintptr_t)img, (long long)buf_size, type, (unsigned long long)need, FACTS_ARGS);
    *pImage = img;
    *pMem = mem;
    return VK_SUCCESS;
fail:
    d->DestroyImage(device, img, alloc);
    if (mem != VK_NULL_HANDLE) d->FreeMemory(device, mem, alloc);
    if (fd >= 0) close(fd);
    return r;
#undef FACTS
#undef FACTS_ARGS
}

static VkResult anb_create_image(struct nvhal_device *d, const VkImageCreateInfo *ci, const VkNativeBufferANDROID *nb,
                                 const VkAllocationCallbacks *alloc, VkImage *pImage) {
    VkImage img;
    VkDeviceMemory mem;
    VkResult r = nvhal_anb_import(d, ci, nb, alloc, "vkCreateImage(ANB)", &img, &mem);
    if (r != VK_SUCCESS) return r;
    bool ok;
    anb_track(d, img, mem, &ok);
    if (!ok) {
        NVHAL_ERR("vkCreateImage(ANB): out of host memory recording the image");
        d->DestroyImage(d->handle, img, alloc);
        d->FreeMemory(d->handle, mem, alloc);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *pImage = img;
    return VK_SUCCESS;
}

VkResult VKAPI_CALL nvhal_CreateImage(VkDevice device, const VkImageCreateInfo *ci,
                                      const VkAllocationCallbacks *alloc, VkImage *pImage) {
    struct nvhal_device *d = nvhal_device_get(device);
    if (!d) {
        NVHAL_ERR("vkCreateImage(%p): not a device created through this HAL", (void *)device);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkNativeBufferANDROID *nb = NULL;
    const VkImageSwapchainCreateInfoKHR *sc = NULL;
    for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext) {
        if ((int)s->sType == VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID && !nb) nb = (const void *)s;
        if (s->sType == VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR && !sc) sc = (const void *)s;
    }
    if (sc && !nb) {
        if (sc->swapchain != VK_NULL_HANDLE) return nvhal_sc_CreateImage(d, ci, sc, alloc, pImage);
        /* swapchain VK_NULL_HANDLE: the struct says nothing; VK_KHR_swapchain is not the ICD's here,
         * so it is removed rather than shown to the ICD */
        unsigned char storage[1024];
        VkImageCreateInfo t = *ci;
        if (!nvhal_chain_replace(ci->pNext, sc, NULL, NULL, storage, sizeof storage, "vkCreateImage", &t.pNext))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        return nvhal_CreateImage(device, &t, alloc, pImage);
    }
    if (!nb) {
        bool handled;
        VkResult r = nvhal_ahb_CreateImage(d, device, ci, alloc, pImage, &handled);
        if (handled) return r;
        if (nvhal_astc_CreateImage(d, ci, alloc, pImage, &r)) return r; /* ASTC: emulated or refused (R8) */
        r = d->CreateImage(device, ci, alloc, pImage);
        if (r == VK_SUCCESS && nvhal_image_wrapper_handle(NVHAL_ND_U64(*pImage)))
            NVHAL_ERR("vkCreateImage: the ICD returned VkImage %p, which equals a live wrapper handle; "
                      "translation is now ambiguous", NVHAL_LOGH(*pImage));
        return r;
    }
    if (!d->anb_enabled) {
        NVHAL_ERR("vkCreateImage: VkNativeBufferANDROID chained, but VK_ANDROID_native_buffer is not enabled "
                  "on this device -> VK_ERROR_EXTENSION_NOT_PRESENT");
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    return anb_create_image(d, ci, nb, alloc, pImage);
}

void VKAPI_CALL nvhal_DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *alloc) {
    struct nvhal_device *d = nvhal_device_get(device);
    if (!d) {
        NVHAL_ERR("vkDestroyImage(%p): not a device created through this HAL", (void *)device);
        return;
    }
    if (nvhal_ahb_DestroyImage(d, device, image, alloc)) return;
    if (nvhal_astc_DestroyImage(d, image, alloc)) return;
    VkDeviceMemory mem = (image != VK_NULL_HANDLE && d->anb_count) ? anb_untrack(d, image) : VK_NULL_HANDLE;
    d->DestroyImage(device, image, alloc);
    if (mem != VK_NULL_HANDLE) d->FreeMemory(device, mem, alloc);
}

/* vkBindImageMemory2[KHR]. Binds of SWAPCHAIN wrappers (vkCreateImage with VkImageSwapchainCreateInfoKHR)
 * arrive with VkBindImageMemorySwapchainInfoKHR, to which the Android loader has prepended the
 * VkNativeBufferANDROID of swapchain image imageIndex (swapchain.cpp:2025-2048: the native buffer's pNext
 * is the app's chain). They are realised here (the ICD image is created on the gralloc buffer, as for a
 * swapchain image, and bound to its imported memory) and never reach the ICD; the other binds go to the
 * ICD (AHB wrappers translated). All binds are checked before any is performed. */
static VkResult bind2(VkDevice device, uint32_t n, const VkBindImageMemoryInfo *infos, bool khr) {
    static const char who[] = "vkBindImageMemory2(swapchain image)";
    struct nvhal_device *d = nvhal_device_get(device);
    if (!d) {
        NVHAL_ERR("vkBindImageMemory2(%p): not a device created through this HAL", (void *)device);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint32_t nsc = 0;
    for (uint32_t i = 0; i < n; i++) {
        const VkNativeBufferANDROID *nb = NULL;
        const VkBindImageMemorySwapchainInfoKHR *sci = NULL;
        for (const VkBaseInStructure *s = infos[i].pNext; s; s = s->pNext) {
            if ((int)s->sType == VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID) nb = (const void *)s;
            else if (s->sType == VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR) sci = (const void *)s;
        }
        enum nvhal_wrapper_kind kind = nvhal_image_wrapper_kind(infos[i].image);
        if (kind != NVHAL_WRAP_SWAPCHAIN) {
            if (nb) {
                NVHAL_ERR("vkBindImageMemory2: pBindInfos[%u] chains VkNativeBufferANDROID for image %p, which was "
                          "not created with VkImageSwapchainCreateInfoKHR (%s); only such an image can be bound to "
                          "swapchain memory -> VK_ERROR_INVALID_EXTERNAL_HANDLE", i, NVHAL_LOGH(infos[i].image),
                          kind == NVHAL_WRAP_AHB ? "it is an AHB image" : "it is a plain ICD image");
                return VK_ERROR_INVALID_EXTERNAL_HANDLE;
            }
            if (sci) {
                NVHAL_ERR("vkBindImageMemory2: pBindInfos[%u] chains VkBindImageMemorySwapchainInfoKHR (imageIndex "
                          "%u) for image %p, which was not created with VkImageSwapchainCreateInfoKHR -> "
                          "VK_ERROR_INVALID_EXTERNAL_HANDLE", i, sci->imageIndex, NVHAL_LOGH(infos[i].image));
                return VK_ERROR_INVALID_EXTERNAL_HANDLE;
            }
            continue;
        }
        if (!nb || !sci || infos[i].memory != VK_NULL_HANDLE) {
            NVHAL_ERR("vkBindImageMemory2: pBindInfos[%u]: image %p was created with VkImageSwapchainCreateInfoKHR "
                      "and can only be bound to swapchain memory: VkBindImageMemorySwapchainInfoKHR %s, "
                      "VkNativeBufferANDROID %s (the Android loader adds it), memory %p (must be VK_NULL_HANDLE) "
                      "-> VK_ERROR_INVALID_EXTERNAL_HANDLE", i, NVHAL_LOGH(infos[i].image), sci ? "chained" : "MISSING",
                      nb ? "chained" : "MISSING", NVHAL_LOGH(infos[i].memory));
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        for (const VkBaseInStructure *s = infos[i].pNext; s; s = s->pNext) {
            if ((int)s->sType == VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID ||
                s->sType == VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR)
                continue;
            if (s->sType == VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO) {
                /* one physical device: device index 0 for the single instance is what the bind does */
                const VkBindImageMemoryDeviceGroupInfo *g = (const void *)s;
                bool trivial = g->splitInstanceBindRegionCount == 0 && g->deviceIndexCount <= 1 &&
                               (g->deviceIndexCount == 0 || g->pDeviceIndices[0] == 0);
                if (trivial) continue;
                NVHAL_ERR("vkBindImageMemory2: pBindInfos[%u]: VkBindImageMemoryDeviceGroupInfo (%u device "
                          "indices, %u split-instance regions) on a swapchain-bound image is not implemented -> "
                          "VK_ERROR_INVALID_EXTERNAL_HANDLE", i, g->deviceIndexCount, g->splitInstanceBindRegionCount);
                return VK_ERROR_INVALID_EXTERNAL_HANDLE;
            }
#ifdef VK_KHR_maintenance6
            if (s->sType == VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS_KHR) continue; /* written below */
#endif
            NVHAL_ERR("vkBindImageMemory2: pBindInfos[%u]: structure sType %d chained to the bind of a "
                      "swapchain-bound image is not implemented -> VK_ERROR_INVALID_EXTERNAL_HANDLE", i,
                      (int)s->sType);
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        nsc++;
    }

    /* the swapchain binds */
    VkResult r = VK_SUCCESS;
    for (uint32_t i = 0; i < n && nsc; i++) {
        if (nvhal_image_wrapper_kind(infos[i].image) != NVHAL_WRAP_SWAPCHAIN) continue;
        const VkNativeBufferANDROID *nb = NULL;
        for (const VkBaseInStructure *s = infos[i].pNext; s && !nb; s = s->pNext)
            if ((int)s->sType == VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID) nb = (const void *)s;
        VkResult ri = nvhal_sc_bind(d, infos[i].image, nb, who);
#ifdef VK_KHR_maintenance6
        for (const VkBaseInStructure *s = infos[i].pNext; s; s = s->pNext)
            if (s->sType == VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS_KHR) *((const VkBindMemoryStatusKHR *)s)->pResult = ri;
#endif
        if (ri != VK_SUCCESS && r == VK_SUCCESS) r = ri;
    }
    if (r != VK_SUCCESS) return r;
    if (nsc == n) return VK_SUCCESS;

    /* the rest: to the ICD */
    VkBindImageMemoryInfo rest_st[8], *rest = (VkBindImageMemoryInfo *)infos;
    uint32_t nr = n;
    if (nsc) {
        nr = 0;
        rest = n - nsc <= 8 ? rest_st : malloc((n - nsc) * sizeof *rest);
        if (!rest) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < n; i++)
            if (nvhal_image_wrapper_kind(infos[i].image) != NVHAL_WRAP_SWAPCHAIN) rest[nr++] = infos[i];
    }
    VkBindImageMemoryInfo st[8], *t, *ta = NULL;
    uint32_t na = nr;
    r = nvhal_ahb_translate_binds(nr, rest, st, 8, &t);
    if (r == VK_SUCCESS) r = nvhal_astc_expand_binds(nr, t, &ta, &na); /* + the ASTC block images */
    if (r == VK_SUCCESS) r = (khr ? d->BindImageMemory2KHR : d->BindImageMemory2)(device, na, ta);
    if (ta && ta != t) free(ta);
    if (t != rest && t != st) free(t);
    if (rest != infos && rest != rest_st) free(rest);
    return r;
}
VkResult VKAPI_CALL nvhal_BindImageMemory2(VkDevice device, uint32_t n, const VkBindImageMemoryInfo *infos) {
    return bind2(device, n, infos, false);
}
VkResult VKAPI_CALL nvhal_BindImageMemory2KHR(VkDevice device, uint32_t n, const VkBindImageMemoryInfo *infos) {
    return bind2(device, n, infos, true);
}

/* ------------------------------------------------------------------ acquire / release */

VkResult VKAPI_CALL nvhal_AcquireImageANDROID(VkDevice device, VkImage image, int nativeFenceFd,
                                              VkSemaphore semaphore, VkFence fence) {
    (void)image;
    /* The HAL owns nativeFenceFd on every path, including failure (swapchain.cpp:1608-1614).
     * -1 means "already signalled"; SYNC_FD import of -1 is valid and measured to behave so. */
    struct nvhal_device *d = anb_device(device, "vkAcquireImageANDROID");
    if (!d) {
        if (nativeFenceFd >= 0) close(nativeFenceFd);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    int sem_fd = -1, fence_fd = -1;
    if (nativeFenceFd >= 0) {
        if (semaphore != VK_NULL_HANDLE && fence != VK_NULL_HANDLE) {
            sem_fd = nativeFenceFd;
            fence_fd = fcntl(nativeFenceFd, F_DUPFD_CLOEXEC, 0);
            if (fence_fd < 0) {
                int e = errno;
                NVHAL_ERR("vkAcquireImageANDROID: dup of fence fd %d failed: %s", nativeFenceFd, strerror(e));
                close(nativeFenceFd);
                return e == EMFILE ? VK_ERROR_TOO_MANY_OBJECTS : VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        } else if (semaphore != VK_NULL_HANDLE) {
            sem_fd = nativeFenceFd;
        } else if (fence != VK_NULL_HANDLE) {
            fence_fd = nativeFenceFd;
        } else {
            close(nativeFenceFd);
        }
    }
    VkResult r = VK_SUCCESS;
    if (semaphore != VK_NULL_HANDLE) {
        VkImportSemaphoreFdInfoKHR info = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore = semaphore,
            .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
            .fd = sem_fd,
        };
        r = d->ImportSemaphoreFdKHR(device, &info);
        if (r == VK_SUCCESS) sem_fd = -1; /* ownership moved to the ICD */
        else NVHAL_ERR("vkAcquireImageANDROID: ICD vkImportSemaphoreFdKHR(SYNC_FD, TEMPORARY, fd %d) -> %s",
                       info.fd, NVHAL_VKR(r));
    }
    if (r == VK_SUCCESS && fence != VK_NULL_HANDLE) {
        VkImportFenceFdInfoKHR info = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR,
            .fence = fence,
            .flags = VK_FENCE_IMPORT_TEMPORARY_BIT,
            .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
            .fd = fence_fd,
        };
        r = d->ImportFenceFdKHR(device, &info);
        if (r == VK_SUCCESS) fence_fd = -1;
        else NVHAL_ERR("vkAcquireImageANDROID: ICD vkImportFenceFdKHR(SYNC_FD, TEMPORARY, fd %d) -> %s",
                       info.fd, NVHAL_VKR(r));
    }
    if (sem_fd >= 0) close(sem_fd);
    if (fence_fd >= 0) close(fence_fd);
    return r;
}

/* Export fences: created with VkExportFenceCreateInfo{SYNC_FD}. Exporting a SYNC_FD has copy
 * transference and resets the fence (measured on this ICD: status VK_NOT_READY right after
 * export, while the sync_file carries the pending signal), so an exported fence is free for
 * the next submit. Before reuse its status is checked: NOT_READY is the expected reset state;
 * SUCCESS (an ICD that does not reset on export) is reset explicitly. */
static VkResult pool_get(struct nvhal_device *d, VkFence *out) {
    VkFence f = VK_NULL_HANDLE;
    pthread_mutex_lock(&nvhal_lock);
    if (d->fence_count) f = d->fence_pool[--d->fence_count];
    pthread_mutex_unlock(&nvhal_lock);
    if (f == VK_NULL_HANDLE) {
        VkExportFenceCreateInfo exp = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = &exp};
        VkResult r = d->CreateFence(d->handle, &fci, NULL, &f);
        if (r != VK_SUCCESS) {
            NVHAL_ERR("vkQueueSignalReleaseImageANDROID: ICD vkCreateFence(export SYNC_FD) -> %s",
                      NVHAL_VKR(r));
            return r;
        }
        NVHAL_TRACE("vkQueueSignalReleaseImageANDROID: new export fence %p on device %p",
                    (void *)(uintptr_t)f, (void *)d->handle);
    } else {
        VkResult s = d->GetFenceStatus(d->handle, f);
        if (s == VK_SUCCESS) {
            VkResult r = d->ResetFences(d->handle, 1, &f);
            if (r != VK_SUCCESS) {
                NVHAL_ERR("vkQueueSignalReleaseImageANDROID: ICD vkResetFences -> %s", NVHAL_VKR(r));
                d->DestroyFence(d->handle, f, NULL);
                return r;
            }
        } else if (s != VK_NOT_READY) {
            NVHAL_ERR("vkQueueSignalReleaseImageANDROID: ICD vkGetFenceStatus(pooled fence) -> %s",
                      NVHAL_VKR(s));
            return s;
        }
    }
    *out = f;
    return VK_SUCCESS;
}

static void pool_put(struct nvhal_device *d, VkFence f) {
    bool ok = true;
    pthread_mutex_lock(&nvhal_lock);
    if (d->fence_count == d->fence_cap) {
        uint32_t cap = d->fence_cap ? d->fence_cap * 2 : 4;
        VkFence *p = realloc(d->fence_pool, cap * sizeof *p);
        if (p) { d->fence_pool = p; d->fence_cap = cap; }
        else ok = false;
    }
    if (ok) d->fence_pool[d->fence_count++] = f;
    pthread_mutex_unlock(&nvhal_lock);
    if (!ok) d->DestroyFence(d->handle, f, NULL);
}

VkResult VKAPI_CALL nvhal_QueueSignalReleaseImageANDROID(VkQueue queue, uint32_t waitCount,
                                                         const VkSemaphore *pWait, VkImage image,
                                                         int *pNativeFenceFd) {
    (void)image;
    *pNativeFenceFd = -1; /* a failing call must not hand the loader an fd (HAL-CONTRACT 5.5) */
    struct nvhal_device *d = nvhal_queue_device(queue);
    if (!d) {
        NVHAL_ERR("vkQueueSignalReleaseImageANDROID(%p): queue was not obtained through this HAL's "
                  "vkGetDeviceQueue", (void *)queue);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (d->gt) nvhal_gt_frame(d);
    nvhal_xst_frame(); /* no-op unless NVHAL_XLATE_STATS / debug.nvhal.xlate_stats is on */
    nvhal_exp_memstats_present(d); /* no-op unless debug.nvhal.exp.memstats is on: ends the preload phase */
    if (!d->anb_enabled) {
        NVHAL_ERR("vkQueueSignalReleaseImageANDROID: VK_ANDROID_native_buffer not enabled on device %p",
                  (void *)d->handle);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkPipelineStageFlags stages_small[16];
    VkPipelineStageFlags *stages = stages_small;
    if (waitCount > 16 && !(stages = malloc(waitCount * sizeof *stages))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < waitCount; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    VkFence f;
    VkResult r = pool_get(d, &f);
    if (r != VK_SUCCESS) goto out;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = waitCount,
        .pWaitSemaphores = pWait,
        .pWaitDstStageMask = stages,
    };
    r = d->QueueSubmit(queue, 1, &si, f);
    if (r != VK_SUCCESS) {
        NVHAL_ERR("vkQueueSignalReleaseImageANDROID: ICD vkQueueSubmit(%u wait semaphore(s), export fence) -> %s",
                  waitCount, NVHAL_VKR(r));
        pool_put(d, f); /* a failed submit leaves the fence unsignaled with nothing pending */
        goto out;
    }
    VkFenceGetFdInfoKHR gi = {
        .sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
        .fence = f,
        .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    int fd = -1;
    r = d->GetFenceFdKHR(d->handle, &gi, &fd);
    if (r != VK_SUCCESS) {
        NVHAL_ERR("vkQueueSignalReleaseImageANDROID: ICD vkGetFenceFdKHR(SYNC_FD) -> %s. The ICD cannot "
                  "export the release fence; no fallback", NVHAL_VKR(r));
        /* the signal is still pending on the fence: wait it out before the fence is reused */
        VkResult w = d->WaitForFences(d->handle, 1, &f, VK_TRUE, UINT64_MAX);
        if (w == VK_SUCCESS && d->ResetFences(d->handle, 1, &f) == VK_SUCCESS) pool_put(d, f);
        else NVHAL_ERR("vkQueueSignalReleaseImageANDROID: release fence %p lost (wait %s)",
                       (void *)(uintptr_t)f, NVHAL_VKR(w));
        goto out;
    }
    pool_put(d, f);
    *pNativeFenceFd = fd;
out:
    if (stages != stages_small) free(stages);
    return r;
}

void nvhal_anb_device_teardown(struct nvhal_device *d, const VkAllocationCallbacks *alloc) {
    if (d->anb_count)
        NVHAL_ERR("vkDestroyDevice: %u VK_ANDROID_native_buffer image(s) were not destroyed; freeing their "
                  "imported memory", d->anb_count);
    for (uint32_t i = 0; i < d->anb_count; i++) d->FreeMemory(d->handle, d->anb_images[i].memory, alloc);
    for (uint32_t i = 0; i < d->fence_count; i++) d->DestroyFence(d->handle, d->fence_pool[i], NULL);
    free(d->anb_images);
    free(d->fence_pool);
    d->anb_images = NULL;
    d->fence_pool = NULL;
    d->anb_count = d->anb_cap = d->fence_count = d->fence_cap = 0;
}
