/* vulkan.nvidia.so internals. The contract this implements is the Android 13 loader<->HAL contract
 * (build/android13/HAL-CONTRACT.md, extracted from the Android 13 loader source). */
#ifndef NVHAL_H
#define NVHAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <vulkan/vulkan.h>
#include <hardware/hwvulkan.h>
#include <vulkan/vk_android_native_buffer.h>

/* Non-dispatchable handles (VkImage, VkDeviceMemory, VkCommandPool, VkPrivateDataSlot, VkSwapchainKHR, ...)
 * are pointers on 64-bit targets and uint64_t on 32-bit ones (VK_USE_64_BIT_PTR_DEFINES, vulkan_core.h).
 * A round trip through uintptr_t is lossless only for the former: on i686 it would cut an ICD handle to its
 * low 32 bits, so hash keys could alias and a wrapper lookup could hit the wrong object. These two convert
 * without truncation on both; dispatchable handles and HAL heap pointers keep the plain uintptr_t casts. */
#if VK_USE_64_BIT_PTR_DEFINES == 1
#define NVHAL_ND_U64(h) ((uint64_t)(uintptr_t)(h))
#define NVHAL_U64_ND(T, v) ((T)(uintptr_t)(v))
#define NVHAL_LOGH(h) ((void *)(h))
#else
#define NVHAL_ND_U64(h) ((uint64_t)(h))
#define NVHAL_U64_ND(T, v) ((T)(v))
#define NVHAL_LOGH(h) ((void *)(uintptr_t)(h)) /* "%p" in log lines only: the low 32 bits */
#endif

#define NVHAL_LOG_TAG "vulkan.nvidia"

/* ---- logging (log.c) ----
 * Errors always reach the log (logcat on Android, stderr on the host); with NVHAL_TRACE set
 * (non-empty, not "0") trace lines are emitted too and everything is mirrored to stderr. */
enum nvhal_level { NVHAL_LVL_ERROR, NVHAL_LVL_WARN, NVHAL_LVL_INFO };
extern int nvhal_trace;
void nvhal_log(enum nvhal_level lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Every VkResult the HAL prints goes through NVHAL_VKR(r) with "%s": the name (and its aliases)
 * when the build's registry/headers know it, and always the signed value, e.g.
 * "VK_ERROR_NOT_PERMITTED aka VK_ERROR_NOT_PERMITTED_EXT/VK_ERROR_NOT_PERMITTED_KHR (-1000174001)" or
 * "VkResult -1000999000 (unknown name)". The string lives in a temporary returned by value, valid
 * until the end of the full expression (C11 6.2.4p8), i.e. for the duration of the log call. */
struct nvhal_vkr_str { char s[192]; };
struct nvhal_vkr_str nvhal_vkr_fmt(VkResult r);
#define NVHAL_VKR(r) (nvhal_vkr_fmt(r).s)
const char *nvhal_result_name(VkResult r); /* generated (gen/nvhal_gen.c); NULL = unknown name */
void nvhal_log_init(void);
#define NVHAL_ERR(...) nvhal_log(NVHAL_LVL_ERROR, __VA_ARGS__)
#define NVHAL_WARN(...) nvhal_log(NVHAL_LVL_WARN, __VA_ARGS__)
#define NVHAL_TRACE(...) do { if (nvhal_trace) nvhal_log(NVHAL_LVL_INFO, __VA_ARGS__); } while (0)

/* ---- handle maps (map.c): every lookup/insert/remove happens under nvhal_lock ---- */
struct nvhal_map {
    struct nvhal_map_entry { const void *key; void *val; } *e;
    size_t n, cap;
};
extern pthread_mutex_t nvhal_lock;
void *nvhal_map_get(const struct nvhal_map *m, const void *key);
bool nvhal_map_put(struct nvhal_map *m, const void *key, void *val);
void *nvhal_map_del(struct nvhal_map *m, const void *key);
void nvhal_map_del_val(struct nvhal_map *m, const void *val);
void nvhal_map_free(struct nvhal_map *m);

/* ---- read-mostly hash tables (util.c): open addressing on a 64-bit handle, own rwlock.
 * Used for the per-process sets the hot paths consult (AHB image wrappers, AHB-backed memory,
 * command buffer -> device). key 0 is never stored (VK_NULL_HANDLE). */
struct nvhal_hash {
    pthread_rwlock_t lock;
    uint64_t *k;
    void **v;
    size_t cap, n, used; /* used = live + tombstones */
};
#define NVHAL_HASH_INIT {PTHREAD_RWLOCK_INITIALIZER, NULL, NULL, 0, 0, 0}
void *nvhal_hash_get(struct nvhal_hash *h, uint64_t key);           /* takes the read lock */
bool nvhal_hash_put(struct nvhal_hash *h, uint64_t key, void *val);  /* takes the write lock */
void *nvhal_hash_del(struct nvhal_hash *h, uint64_t key);           /* takes the write lock */
/* write-locked sweep: deletes every entry for which pred(val, arg) is true, calling
 * on_del(val) for it (may be NULL); returns the number deleted */
size_t nvhal_hash_sweep(struct nvhal_hash *h, bool (*pred)(void *val, void *arg), void *arg,
                        void (*on_del)(void *val));

/* ---- ICD (hal.c) ---- */
extern PFN_vkGetInstanceProcAddr nvhal_icd_gipa;

/* ---- compressed-format emulation (ASTC LDR, ETC2/EAC), per physical device (astc.c) ---- */
enum nvhal_astc_state { NVHAL_ASTC_EMULATE = 1, NVHAL_ASTC_NATIVE, NVHAL_ASTC_UNAVAILABLE };
struct nvhal_astc_pd {
    struct nvhal_astc_pd *next;
    VkPhysicalDevice pd;
    enum nvhal_astc_state state;   /* ASTC LDR */
    char why[256];                 /* NVHAL_ASTC_UNAVAILABLE: the missing prerequisite */
    VkFormatFeatureFlags feat[2];  /* optimal-tiling features reported for ASTC [UNORM, SRGB] */
    enum nvhal_astc_state etc2_state; /* ETC2/EAC, decided independently of ASTC */
    char etc2_why[256];
    VkFormatFeatureFlags etc2_feat[10]; /* per VkFormat - VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK */
    VkImageUsageFlags usage;       /* image usages the emulation accepts (both families) */
    VkImageCreateFlags flags;      /* create flags the emulation accepts (sparse binding per device features) */
    uint32_t api_version;          /* the physical device's */
    uint32_t nq;
    VkQueueFlags qflags[16];
};

/* ---- per-instance state, keyed by the ICD's VkInstance ---- */
struct nvhal_instance {
    VkInstance handle;
    uint32_t api_version; /* apiVersion the app asked for (1.0 when absent) */
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkEnumeratePhysicalDeviceGroups EnumeratePhysicalDeviceGroups;
    PFN_vkEnumeratePhysicalDeviceGroups EnumeratePhysicalDeviceGroupsKHR;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2KHR;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    /* core entry point for a >= 1.1 instance, the KHR one otherwise (the HAL then enables
     * VK_KHR_get_physical_device_properties2 itself, see hal_CreateInstance) */
    PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2;
    /* same choice (core for >= 1.1, else KHR), used by the AHB implementation */
    PFN_vkGetPhysicalDeviceFormatProperties2 GetPhysicalDeviceFormatProperties2;
    PFN_vkGetPhysicalDeviceExternalBufferProperties GetPhysicalDeviceExternalBufferProperties;
    /* exactly the names the app may call (intercepted for the AHB handle type) */
    PFN_vkGetPhysicalDeviceImageFormatProperties2 GPDIFP2_core, GPDIFP2_khr;
    PFN_vkGetPhysicalDeviceExternalBufferProperties GPDEBP_core, GPDEBP_khr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    /* the format/feature queries the ASTC emulation answers (astc.c); exactly the names the app may call */
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures;
    PFN_vkGetPhysicalDeviceFeatures2 GPDF2_core, GPDF2_khr;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
    PFN_vkGetPhysicalDeviceFormatProperties2 GPDFP2_core, GPDFP2_khr;
    PFN_vkGetPhysicalDeviceImageFormatProperties GetPhysicalDeviceImageFormatProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    struct nvhal_astc_pd *astc_pd; /* list, nvhal_lock; freed with the instance */
};

struct nvhal_anb_image { VkImage image; VkDeviceMemory memory; };

/* private data the HAL keeps itself, for objects the Android loader owns (imgxlate.c) */
struct nvhal_loader_pd { VkObjectType type; uint64_t handle, slot, data; };

/* ICD entry points of every command that takes a VkImage (directly or in a struct) or that the
 * AHB implementation intercepts (imgxlate.c, ahb.c). One field per name the app can call: an
 * alias (KHR/EXT) gets its own field, because on an older device only the alias exists.
 * tools/vkimage-coverage.py checks the intercept table in hal.c against vk.xml. */
#define NVHAL_XL_FNS_CORE(X) \
    X(PFN_vkBindImageMemory, BindImageMemory, "vkBindImageMemory") \
    X(PFN_vkGetImageMemoryRequirements, GetImageMemoryRequirements, "vkGetImageMemoryRequirements") \
    X(PFN_vkGetImageMemoryRequirements2, GetImageMemoryRequirements2, "vkGetImageMemoryRequirements2") \
    X(PFN_vkGetImageMemoryRequirements2, GetImageMemoryRequirements2KHR, "vkGetImageMemoryRequirements2KHR") \
    X(PFN_vkGetImageSparseMemoryRequirements, GetImageSparseMemoryRequirements, "vkGetImageSparseMemoryRequirements") \
    X(PFN_vkGetImageSparseMemoryRequirements2, GetImageSparseMemoryRequirements2, "vkGetImageSparseMemoryRequirements2") \
    X(PFN_vkGetImageSparseMemoryRequirements2, GetImageSparseMemoryRequirements2KHR, "vkGetImageSparseMemoryRequirements2KHR") \
    X(PFN_vkGetImageSubresourceLayout, GetImageSubresourceLayout, "vkGetImageSubresourceLayout") \
    X(PFN_vkGetImageSubresourceLayout2, GetImageSubresourceLayout2, "vkGetImageSubresourceLayout2") \
    X(PFN_vkGetImageSubresourceLayout2, GetImageSubresourceLayout2KHR, "vkGetImageSubresourceLayout2KHR") \
    X(PFN_vkGetImageSubresourceLayout2, GetImageSubresourceLayout2EXT, "vkGetImageSubresourceLayout2EXT") \
    X(PFN_vkGetImageDrmFormatModifierPropertiesEXT, GetImageDrmFormatModifierPropertiesEXT, "vkGetImageDrmFormatModifierPropertiesEXT") \
    X(PFN_vkCreateImageView, CreateImageView, "vkCreateImageView") \
    X(PFN_vkCmdBlitImage, CmdBlitImage, "vkCmdBlitImage") \
    X(PFN_vkCmdBlitImage2, CmdBlitImage2, "vkCmdBlitImage2") \
    X(PFN_vkCmdBlitImage2, CmdBlitImage2KHR, "vkCmdBlitImage2KHR") \
    X(PFN_vkCmdClearColorImage, CmdClearColorImage, "vkCmdClearColorImage") \
    X(PFN_vkCmdClearDepthStencilImage, CmdClearDepthStencilImage, "vkCmdClearDepthStencilImage") \
    X(PFN_vkCmdCopyBufferToImage, CmdCopyBufferToImage, "vkCmdCopyBufferToImage") \
    X(PFN_vkCmdCopyBufferToImage2, CmdCopyBufferToImage2, "vkCmdCopyBufferToImage2") \
    X(PFN_vkCmdCopyBufferToImage2, CmdCopyBufferToImage2KHR, "vkCmdCopyBufferToImage2KHR") \
    X(PFN_vkCmdCopyImage, CmdCopyImage, "vkCmdCopyImage") \
    X(PFN_vkCmdCopyImage2, CmdCopyImage2, "vkCmdCopyImage2") \
    X(PFN_vkCmdCopyImage2, CmdCopyImage2KHR, "vkCmdCopyImage2KHR") \
    X(PFN_vkCmdCopyImageToBuffer, CmdCopyImageToBuffer, "vkCmdCopyImageToBuffer") \
    X(PFN_vkCmdCopyImageToBuffer2, CmdCopyImageToBuffer2, "vkCmdCopyImageToBuffer2") \
    X(PFN_vkCmdCopyImageToBuffer2, CmdCopyImageToBuffer2KHR, "vkCmdCopyImageToBuffer2KHR") \
    X(PFN_vkCmdResolveImage, CmdResolveImage, "vkCmdResolveImage") \
    X(PFN_vkCmdResolveImage2, CmdResolveImage2, "vkCmdResolveImage2") \
    X(PFN_vkCmdResolveImage2, CmdResolveImage2KHR, "vkCmdResolveImage2KHR") \
    X(PFN_vkCmdPipelineBarrier, CmdPipelineBarrier, "vkCmdPipelineBarrier") \
    X(PFN_vkCmdPipelineBarrier2, CmdPipelineBarrier2, "vkCmdPipelineBarrier2") \
    X(PFN_vkCmdPipelineBarrier2, CmdPipelineBarrier2KHR, "vkCmdPipelineBarrier2KHR") \
    X(PFN_vkCmdWaitEvents, CmdWaitEvents, "vkCmdWaitEvents") \
    X(PFN_vkCmdWaitEvents2, CmdWaitEvents2, "vkCmdWaitEvents2") \
    X(PFN_vkCmdWaitEvents2, CmdWaitEvents2KHR, "vkCmdWaitEvents2KHR") \
    X(PFN_vkCmdSetEvent2, CmdSetEvent2, "vkCmdSetEvent2") \
    X(PFN_vkCmdSetEvent2, CmdSetEvent2KHR, "vkCmdSetEvent2KHR") \
    X(PFN_vkCopyImageToImage, CopyImageToImage, "vkCopyImageToImage") \
    X(PFN_vkCopyImageToImage, CopyImageToImageEXT, "vkCopyImageToImageEXT") \
    X(PFN_vkCopyImageToMemory, CopyImageToMemory, "vkCopyImageToMemory") \
    X(PFN_vkCopyImageToMemory, CopyImageToMemoryEXT, "vkCopyImageToMemoryEXT") \
    X(PFN_vkCopyMemoryToImage, CopyMemoryToImage, "vkCopyMemoryToImage") \
    X(PFN_vkCopyMemoryToImage, CopyMemoryToImageEXT, "vkCopyMemoryToImageEXT") \
    X(PFN_vkTransitionImageLayout, TransitionImageLayout, "vkTransitionImageLayout") \
    X(PFN_vkTransitionImageLayout, TransitionImageLayoutEXT, "vkTransitionImageLayoutEXT") \
    X(PFN_vkGetImageOpaqueCaptureDescriptorDataEXT, GetImageOpaqueCaptureDescriptorDataEXT, "vkGetImageOpaqueCaptureDescriptorDataEXT") \
    X(PFN_vkCmdCopyMemoryToImageIndirectNV, CmdCopyMemoryToImageIndirectNV, "vkCmdCopyMemoryToImageIndirectNV") \
    X(PFN_vkQueueBindSparse, QueueBindSparse, "vkQueueBindSparse") \
    X(PFN_vkSetDebugUtilsObjectNameEXT, SetDebugUtilsObjectNameEXT, "vkSetDebugUtilsObjectNameEXT") \
    X(PFN_vkSetDebugUtilsObjectTagEXT, SetDebugUtilsObjectTagEXT, "vkSetDebugUtilsObjectTagEXT") \
    X(PFN_vkDebugMarkerSetObjectNameEXT, DebugMarkerSetObjectNameEXT, "vkDebugMarkerSetObjectNameEXT") \
    X(PFN_vkDebugMarkerSetObjectTagEXT, DebugMarkerSetObjectTagEXT, "vkDebugMarkerSetObjectTagEXT") \
    X(PFN_vkSetPrivateData, SetPrivateData, "vkSetPrivateData") \
    X(PFN_vkSetPrivateData, SetPrivateDataEXT, "vkSetPrivateDataEXT") \
    X(PFN_vkGetPrivateData, GetPrivateData, "vkGetPrivateData") \
    X(PFN_vkGetPrivateData, GetPrivateDataEXT, "vkGetPrivateDataEXT") \
    X(PFN_vkDestroyPrivateDataSlot, DestroyPrivateDataSlot, "vkDestroyPrivateDataSlot") \
    X(PFN_vkDestroyPrivateDataSlot, DestroyPrivateDataSlotEXT, "vkDestroyPrivateDataSlotEXT") \
    X(PFN_vkGetDeviceImageMemoryRequirements, GetDeviceImageMemoryRequirements, "vkGetDeviceImageMemoryRequirements") \
    X(PFN_vkGetDeviceImageMemoryRequirements, GetDeviceImageMemoryRequirementsKHR, "vkGetDeviceImageMemoryRequirementsKHR") \
    X(PFN_vkGetDeviceImageSparseMemoryRequirements, GetDeviceImageSparseMemoryRequirements, "vkGetDeviceImageSparseMemoryRequirements") \
    X(PFN_vkGetDeviceImageSparseMemoryRequirements, GetDeviceImageSparseMemoryRequirementsKHR, "vkGetDeviceImageSparseMemoryRequirementsKHR") \
    X(PFN_vkGetDeviceImageSubresourceLayout, GetDeviceImageSubresourceLayout, "vkGetDeviceImageSubresourceLayout") \
    X(PFN_vkGetDeviceImageSubresourceLayout, GetDeviceImageSubresourceLayoutKHR, "vkGetDeviceImageSubresourceLayoutKHR") \
    X(PFN_vkGetDeviceBufferMemoryRequirements, GetDeviceBufferMemoryRequirements, "vkGetDeviceBufferMemoryRequirements") \
    X(PFN_vkGetDeviceBufferMemoryRequirements, GetDeviceBufferMemoryRequirementsKHR, "vkGetDeviceBufferMemoryRequirementsKHR") \
    X(PFN_vkCreateBuffer, CreateBuffer, "vkCreateBuffer") \
    X(PFN_vkCreateSamplerYcbcrConversion, CreateSamplerYcbcrConversion, "vkCreateSamplerYcbcrConversion") \
    X(PFN_vkCreateSamplerYcbcrConversion, CreateSamplerYcbcrConversionKHR, "vkCreateSamplerYcbcrConversionKHR") \
    X(PFN_vkFreeCommandBuffers, FreeCommandBuffers, "vkFreeCommandBuffers") \
    X(PFN_vkDestroyCommandPool, DestroyCommandPool, "vkDestroyCommandPool")
/* Newer than some header sets (NDK r30 ships VK_HEADER_VERSION 335): compiled only where the
 * header has them; where it does not, the extension is hidden (gen/nvhal_gen.h). */
#ifdef VK_KHR_device_address_commands
#define NVHAL_XL_FNS_DAC(X) \
    X(PFN_vkCmdCopyMemoryToImageKHR, CmdCopyMemoryToImageKHR, "vkCmdCopyMemoryToImageKHR") \
    X(PFN_vkCmdCopyImageToMemoryKHR, CmdCopyImageToMemoryKHR, "vkCmdCopyImageToMemoryKHR")
#else
#define NVHAL_XL_FNS_DAC(X)
#endif
#ifdef VK_KHR_copy_memory_indirect
#define NVHAL_XL_FNS_CMI(X) \
    X(PFN_vkCmdCopyMemoryToImageIndirectKHR, CmdCopyMemoryToImageIndirectKHR, "vkCmdCopyMemoryToImageIndirectKHR")
#else
#define NVHAL_XL_FNS_CMI(X)
#endif
#ifdef VK_EXT_descriptor_heap
#define NVHAL_XL_FNS_DH(X) \
    X(PFN_vkWriteResourceDescriptorsEXT, WriteResourceDescriptorsEXT, "vkWriteResourceDescriptorsEXT") \
    X(PFN_vkGetImageOpaqueCaptureDataEXT, GetImageOpaqueCaptureDataEXT, "vkGetImageOpaqueCaptureDataEXT")
#else
#define NVHAL_XL_FNS_DH(X)
#endif
#define NVHAL_XL_FNS(X) NVHAL_XL_FNS_CORE(X) NVHAL_XL_FNS_DAC(X) NVHAL_XL_FNS_CMI(X) NVHAL_XL_FNS_DH(X)

struct nvhal_xl_fns {
#define NVHAL_XL_FIELD(type, field, name) type field;
    NVHAL_XL_FNS(NVHAL_XL_FIELD)
#undef NVHAL_XL_FIELD
};

/* ICD entry points of the commands that set compute-bind-point / push-constant state, which the ASTC
 * decode disturbs and cmdstate.c shadows and restores; one field per name the app can call. */
#define NVHAL_CS_FNS_CORE(X) \
    X(PFN_vkBeginCommandBuffer, BeginCommandBuffer, "vkBeginCommandBuffer") \
    X(PFN_vkCreateCommandPool, CreateCommandPool, "vkCreateCommandPool") \
    X(PFN_vkCmdBindPipeline, CmdBindPipeline, "vkCmdBindPipeline") \
    X(PFN_vkCmdBindDescriptorSets, CmdBindDescriptorSets, "vkCmdBindDescriptorSets") \
    X(PFN_vkCmdBindDescriptorSets2, CmdBindDescriptorSets2, "vkCmdBindDescriptorSets2") \
    X(PFN_vkCmdBindDescriptorSets2, CmdBindDescriptorSets2KHR, "vkCmdBindDescriptorSets2KHR") \
    X(PFN_vkCmdPushConstants, CmdPushConstants, "vkCmdPushConstants") \
    X(PFN_vkCmdPushConstants2, CmdPushConstants2, "vkCmdPushConstants2") \
    X(PFN_vkCmdPushConstants2, CmdPushConstants2KHR, "vkCmdPushConstants2KHR") \
    X(PFN_vkCmdPushDescriptorSet, CmdPushDescriptorSet, "vkCmdPushDescriptorSet") \
    X(PFN_vkCmdPushDescriptorSet, CmdPushDescriptorSetKHR, "vkCmdPushDescriptorSetKHR") \
    X(PFN_vkCmdPushDescriptorSet2, CmdPushDescriptorSet2, "vkCmdPushDescriptorSet2") \
    X(PFN_vkCmdPushDescriptorSet2, CmdPushDescriptorSet2KHR, "vkCmdPushDescriptorSet2KHR") \
    X(PFN_vkCmdPushDescriptorSetWithTemplate, CmdPushDescriptorSetWithTemplate, "vkCmdPushDescriptorSetWithTemplate") \
    X(PFN_vkCmdPushDescriptorSetWithTemplate, CmdPushDescriptorSetWithTemplateKHR, "vkCmdPushDescriptorSetWithTemplateKHR") \
    X(PFN_vkCmdPushDescriptorSetWithTemplate2, CmdPushDescriptorSetWithTemplate2, "vkCmdPushDescriptorSetWithTemplate2") \
    X(PFN_vkCmdPushDescriptorSetWithTemplate2, CmdPushDescriptorSetWithTemplate2KHR, "vkCmdPushDescriptorSetWithTemplate2KHR") \
    X(PFN_vkCmdSetDescriptorBufferOffsetsEXT, CmdSetDescriptorBufferOffsetsEXT, "vkCmdSetDescriptorBufferOffsetsEXT") \
    X(PFN_vkCmdSetDescriptorBufferOffsets2EXT, CmdSetDescriptorBufferOffsets2EXT, "vkCmdSetDescriptorBufferOffsets2EXT") \
    X(PFN_vkCmdBindDescriptorBufferEmbeddedSamplersEXT, CmdBindDescriptorBufferEmbeddedSamplersEXT, \
      "vkCmdBindDescriptorBufferEmbeddedSamplersEXT") \
    X(PFN_vkCmdBindDescriptorBufferEmbeddedSamplers2EXT, CmdBindDescriptorBufferEmbeddedSamplers2EXT, \
      "vkCmdBindDescriptorBufferEmbeddedSamplers2EXT") \
    X(PFN_vkCmdBindShadersEXT, CmdBindShadersEXT, "vkCmdBindShadersEXT") \
    X(PFN_vkCmdBeginConditionalRenderingEXT, CmdBeginConditionalRenderingEXT, "vkCmdBeginConditionalRenderingEXT") \
    X(PFN_vkCmdEndConditionalRenderingEXT, CmdEndConditionalRenderingEXT, "vkCmdEndConditionalRenderingEXT")
#ifdef VK_EXT_descriptor_heap
#define NVHAL_CS_FNS_DH(X) \
    X(PFN_vkCmdBindSamplerHeapEXT, CmdBindSamplerHeapEXT, "vkCmdBindSamplerHeapEXT") \
    X(PFN_vkCmdBindResourceHeapEXT, CmdBindResourceHeapEXT, "vkCmdBindResourceHeapEXT") \
    X(PFN_vkCmdPushDataEXT, CmdPushDataEXT, "vkCmdPushDataEXT")
#else
#define NVHAL_CS_FNS_DH(X)
#endif
#define NVHAL_CS_FNS(X) NVHAL_CS_FNS_CORE(X) NVHAL_CS_FNS_DH(X)

/* ICD entry points only the ASTC decode uses (all Vulkan 1.0 core; required when emulating) */
#define NVHAL_ASTC_FNS(X) \
    X(PFN_vkCmdDispatch, CmdDispatch, "vkCmdDispatch") \
    X(PFN_vkDestroyImageView, DestroyImageView, "vkDestroyImageView") \
    X(PFN_vkCreateDescriptorSetLayout, CreateDescriptorSetLayout, "vkCreateDescriptorSetLayout") \
    X(PFN_vkDestroyDescriptorSetLayout, DestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout") \
    X(PFN_vkCreatePipelineLayout, CreatePipelineLayout, "vkCreatePipelineLayout") \
    X(PFN_vkDestroyPipelineLayout, DestroyPipelineLayout, "vkDestroyPipelineLayout") \
    X(PFN_vkCreateShaderModule, CreateShaderModule, "vkCreateShaderModule") \
    X(PFN_vkDestroyShaderModule, DestroyShaderModule, "vkDestroyShaderModule") \
    X(PFN_vkCreateComputePipelines, CreateComputePipelines, "vkCreateComputePipelines") \
    X(PFN_vkDestroyPipeline, DestroyPipeline, "vkDestroyPipeline") \
    X(PFN_vkDestroyBuffer, DestroyBuffer, "vkDestroyBuffer") \
    X(PFN_vkGetBufferMemoryRequirements, GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements") \
    X(PFN_vkBindBufferMemory, BindBufferMemory, "vkBindBufferMemory") \
    X(PFN_vkMapMemory, MapMemory, "vkMapMemory") \
    X(PFN_vkUnmapMemory, UnmapMemory, "vkUnmapMemory") \
    X(PFN_vkCreateBufferView, CreateBufferView, "vkCreateBufferView") \
    X(PFN_vkDestroyBufferView, DestroyBufferView, "vkDestroyBufferView") \
    X(PFN_vkCreateDescriptorPool, CreateDescriptorPool, "vkCreateDescriptorPool") \
    X(PFN_vkDestroyDescriptorPool, DestroyDescriptorPool, "vkDestroyDescriptorPool") \
    X(PFN_vkAllocateDescriptorSets, AllocateDescriptorSets, "vkAllocateDescriptorSets") \
    X(PFN_vkFreeDescriptorSets, FreeDescriptorSets, "vkFreeDescriptorSets") \
    X(PFN_vkUpdateDescriptorSets, UpdateDescriptorSets, "vkUpdateDescriptorSets")

struct nvhal_cs_fns {
#define NVHAL_XL_FIELD(type, field, name) type field;
    NVHAL_CS_FNS(NVHAL_XL_FIELD)
    NVHAL_ASTC_FNS(NVHAL_XL_FIELD)
#undef NVHAL_XL_FIELD
};
struct nvhal_astc_dev;

/* ---- per-device state, keyed by the ICD's VkDevice ---- */
struct nvhal_device {
    VkDevice handle;
    VkPhysicalDevice physical_device;
    struct nvhal_instance *inst;
    uint32_t api_version; /* min(instance apiVersion, physical device apiVersion) */
    bool anb_enabled;     /* the app (loader) enabled VK_ANDROID_native_buffer */
    bool ahb_enabled;     /* the app enabled VK_ANDROID_external_memory_android_hardware_buffer */
    /* NVIDIA driver defects with imported SYNC_FD payloads (device.c, acquire_wait_flags): vkAcquireImageANDROID
     * waits for the native fence on the CPU and imports "already signalled" when it is given a fence
     * (acquire_wait_fence) or a semaphore (acquire_wait_semaphore) */
    bool acquire_wait_fence, acquire_wait_semaphore;
    VkPhysicalDeviceMemoryProperties mem_props;

    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkGetDeviceQueue2 GetDeviceQueue2;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkBindImageMemory2 BindImageMemory2;    /* core name, as the app sees it */
    PFN_vkBindImageMemory2 BindImageMemory2KHR; /* KHR name, as the app sees it */

    /* used by the ANB implementation only (fetched when anb_enabled) */
    PFN_vkBindImageMemory2 anb_BindImageMemory2;                     /* core or KHR */
    PFN_vkGetImageMemoryRequirements2 anb_GetImageMemoryRequirements2; /* core or KHR */
    PFN_vkGetMemoryFdPropertiesKHR GetMemoryFdPropertiesKHR;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkImportSemaphoreFdKHR ImportSemaphoreFdKHR;
    PFN_vkImportFenceFdKHR ImportFenceFdKHR;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkGetFenceStatus GetFenceStatus;
    PFN_vkResetFences ResetFences;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkGetFenceFdKHR GetFenceFdKHR;

    /* every translated / AHB-intercepted ICD entry point (NULL where the ICD has none) */
    struct nvhal_xl_fns xl;
    /* compute-state commands and the ASTC decode's own ICD entry points (NULL where the ICD has none) */
    struct nvhal_cs_fns cs;
    /* ASTC LDR emulation (astc.c): non-NULL when this device emulates it (the ICD has no ASTC) */
    struct nvhal_astc_dev *astc;
    const struct nvhal_astc_pd *astc_pd;

    /* ANB images (image -> imported memory); guarded by nvhal_lock */
    struct nvhal_anb_image *anb_images;
    uint32_t anb_count, anb_cap;
    /* idle SYNC_FD-exportable fences for vkQueueSignalReleaseImageANDROID; nvhal_lock */
    VkFence *fence_pool;
    uint32_t fence_count, fence_cap;
    /* private data of VkSwapchainKHR / VkSurfaceKHR handles: those objects belong to the Android
     * loader, which passes vkSet/GetPrivateData on them straight to the driver; nvhal_lock */
    struct nvhal_loader_pd *loader_pd;
    uint32_t loader_pd_count, loader_pd_cap;
    /* opt-in GPU timing (gputime.c); NULL unless debug.nvhal.gputime / NVHAL_GPUTIME is set */
    struct nvhal_gt_dev *gt;
    /* EXPERIMENT (off by default; debug.nvhal.exp.hostvis_to_bar / NVHAL_EXP_HOSTVIS_TO_BAR): plain allocations of a
     * host-visible, host-uncached, non-device-local type are made from the type with the same flags + DEVICE_LOCAL
     * (resizable BAR). Index + 1; 0 = unchanged. A diagnostic A/B, not a fix (gputime.c). */
    uint8_t mem_remap[VK_MAX_MEMORY_TYPES];
};

/* lookups (hal.c / device.c); each takes nvhal_lock internally */
struct nvhal_instance *nvhal_instance_get(VkInstance instance);
struct nvhal_instance *nvhal_physdev_instance(VkPhysicalDevice pd);
struct nvhal_device *nvhal_device_get(VkDevice device);
struct nvhal_device *nvhal_queue_device(VkQueue queue);

/* Dispatchable-object word check (HAL-CONTRACT 1.6). parent is the object whose loader data
 * a re-returned child may already carry (instance for physical devices, device for queues and
 * command buffers), or NULL for freshly created top-level objects. */
void nvhal_dispatch_check(void *obj, const void *parent, const char *what);

/* extension helpers (hal.c) */
bool nvhal_ext_in(const VkExtensionProperties *list, uint32_t n, const char *name);
VkResult nvhal_fill_ext_props(const VkExtensionProperties *src, uint32_t n, uint32_t *pCount,
                              VkExtensionProperties *pProps);

/* device-level entry points (device.c) */
VkResult VKAPI_CALL nvhal_CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *,
                                       const VkAllocationCallbacks *, VkDevice *);
void VKAPI_CALL nvhal_DestroyDevice(VkDevice, const VkAllocationCallbacks *);
VkResult VKAPI_CALL nvhal_EnumerateDeviceExtensionProperties(VkPhysicalDevice, const char *,
                                                             uint32_t *, VkExtensionProperties *);
void VKAPI_CALL nvhal_GetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue *);
void VKAPI_CALL nvhal_GetDeviceQueue2(VkDevice, const VkDeviceQueueInfo2 *, VkQueue *);
VkResult VKAPI_CALL nvhal_AllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *,
                                                 VkCommandBuffer *);
void VKAPI_CALL nvhal_GetPhysicalDeviceProperties2(VkPhysicalDevice, VkPhysicalDeviceProperties2 *);
void VKAPI_CALL nvhal_GetPhysicalDeviceProperties2KHR(VkPhysicalDevice, VkPhysicalDeviceProperties2 *);

/* ANB (anb.c) */
VkResult VKAPI_CALL nvhal_CreateImage(VkDevice, const VkImageCreateInfo *,
                                      const VkAllocationCallbacks *, VkImage *);
void VKAPI_CALL nvhal_DestroyImage(VkDevice, VkImage, const VkAllocationCallbacks *);
VkResult VKAPI_CALL nvhal_BindImageMemory2(VkDevice, uint32_t, const VkBindImageMemoryInfo *);
VkResult VKAPI_CALL nvhal_BindImageMemory2KHR(VkDevice, uint32_t, const VkBindImageMemoryInfo *);
VkResult VKAPI_CALL nvhal_GetSwapchainGrallocUsageANDROID(VkDevice, VkFormat, VkImageUsageFlags, int *);
VkResult VKAPI_CALL nvhal_GetSwapchainGrallocUsage2ANDROID(VkDevice, VkFormat, VkImageUsageFlags,
                                                           VkSwapchainImageUsageFlagsANDROID,
                                                           uint64_t *, uint64_t *);
VkResult VKAPI_CALL nvhal_AcquireImageANDROID(VkDevice, VkImage, int, VkSemaphore, VkFence);
VkResult VKAPI_CALL nvhal_QueueSignalReleaseImageANDROID(VkQueue, uint32_t, const VkSemaphore *,
                                                         VkImage, int *);
/* called by nvhal_DestroyDevice before the ICD destroys the device */
void nvhal_anb_device_teardown(struct nvhal_device *dev, const VkAllocationCallbacks *alloc);
/* VkFormat -> DRM fourcc accepted for an ANB buffer of that format; 0 if unmapped */
bool nvhal_anb_format_known(VkFormat format);

/* The ANB import core (anb.c): create the ICD image of ci (tiling replaced by the buffer's explicit
 * DRM modifier; ci->pNext may carry VkNativeBufferANDROID / VkSwapchainImageCreateInfoANDROID, which
 * are dropped, VkImageFormatListCreateInfo and VkImageStencilUsageCreateInfo, which are kept; anything
 * else is refused), import the gralloc buffer's dma-buf as its dedicated memory and bind it. `who`
 * prefixes every log line. Nothing is tracked: the caller owns *img and *mem. */
VkResult nvhal_anb_import(struct nvhal_device *d, const VkImageCreateInfo *ci, const VkNativeBufferANDROID *nb,
                          const VkAllocationCallbacks *alloc, const char *who, VkImage *img, VkDeviceMemory *mem);

/* minigbm handle checks shared by ANB and AHB (anb.c): structure + magic, with a named remedy. */
struct cros_gralloc_handle;
VkResult nvhal_cros_handle(const native_handle_t *h, const char *who, const struct cros_gralloc_handle **out);
void nvhal_fourcc_str(uint32_t fourcc, char out[5]);
/* the usage check anb.c and ahb.c share: can the ICD import a dma-buf with this modifier for an
 * image of ci (format/type/flags/sharing; pNext may carry a format list) and usage? */
VkResult nvhal_modifier_usage_ok(struct nvhal_device *d, const VkImageCreateInfo *ci, uint64_t modifier,
                                 VkImageUsageFlags usage);

/* ---- AHB (ahb.c) ---- */
extern bool nvhal_ahb_available;          /* entry points found (libnativewindow / test table) */
void nvhal_ahb_load(void);                /* hal_open, under nvhal_lock */
void nvhal_ahb_report_unavailable(void);  /* once per process */
void nvhal_ahb_device_teardown(struct nvhal_device *d, const VkAllocationCallbacks *alloc);
/* HAL-owned wrapper images (ahb.c), two kinds whose ICD image is created later:
 *   AHB:       vkCreateImage with the AHB handle type, realised by vkAllocateMemory (the buffer);
 *   SWAPCHAIN: vkCreateImage with VkImageSwapchainCreateInfoKHR{swapchain != NULL} (the swapchain is
 *              the Android loader's object, the ICD must never see it), realised by
 *              vkBindImageMemory2 with VkBindImageMemorySwapchainInfoKHR, to which the loader adds the
 *              VkNativeBufferANDROID of swapchain image imageIndex (swapchain.cpp:1989-2085). */
enum nvhal_wrapper_kind { NVHAL_WRAP_NONE = 0, NVHAL_WRAP_AHB, NVHAL_WRAP_SWAPCHAIN };
enum nvhal_wrapper_kind nvhal_image_wrapper_kind(VkImage image);
VkResult nvhal_sc_CreateImage(struct nvhal_device *d, const VkImageCreateInfo *ci,
                              const VkImageSwapchainCreateInfoKHR *sc, const VkAllocationCallbacks *alloc,
                              VkImage *pImage);
/* realise a SWAPCHAIN wrapper on the gralloc buffer nb (bind2, anb.c) */
VkResult nvhal_sc_bind(struct nvhal_device *d, VkImage wrapper, const VkNativeBufferANDROID *nb, const char *who);
/* Wrapper images: VkImage -> real ICD VkImage. *hit is set when image was a wrapper; an
 * unrealised wrapper yields VK_NULL_HANDLE (and an error log naming `what`, unless what is NULL:
 * for the calls where an image without memory is valid). */
VkImage nvhal_image_xl(VkImage image, bool *hit, const char *what);
/* true when no wrapper exists in the process: translators skip all lookups */
bool nvhal_no_wrappers(void);
/* for vkCreateImageView of an external-format wrapper with format UNDEFINED */
VkFormat nvhal_image_real_format(VkImage wrapper);
/* object-handle commands (debug utils, debug marker, private data) on wrappers */
bool nvhal_image_wrapper_handle(uint64_t handle);

VkResult VKAPI_CALL nvhal_ahb_CreateImage(struct nvhal_device *d, VkDevice device, const VkImageCreateInfo *ci,
                                          const VkAllocationCallbacks *alloc, VkImage *pImage, bool *handled);
bool nvhal_ahb_DestroyImage(struct nvhal_device *d, VkDevice device, VkImage image, const VkAllocationCallbacks *alloc);
VkResult VKAPI_CALL nvhal_AllocateMemory(VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *,
                                         VkDeviceMemory *);
void VKAPI_CALL nvhal_FreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *);
VkResult VKAPI_CALL nvhal_BindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
/* translate the image of each bind (AHB wrappers -> real image, validated); out may alias a
 * stack buffer; returns VK_SUCCESS or the error to return */
VkResult nvhal_ahb_translate_binds(uint32_t n, const VkBindImageMemoryInfo *in, VkBindImageMemoryInfo *stack,
                                   uint32_t stack_n, VkBindImageMemoryInfo **out);
VkResult VKAPI_CALL nvhal_GetAndroidHardwareBufferPropertiesANDROID(VkDevice, const struct AHardwareBuffer *,
                                                                    VkAndroidHardwareBufferPropertiesANDROID *);
VkResult VKAPI_CALL nvhal_GetMemoryAndroidHardwareBufferANDROID(VkDevice,
                                                                const VkMemoryGetAndroidHardwareBufferInfoANDROID *,
                                                                struct AHardwareBuffer **);
VkResult VKAPI_CALL nvhal_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice,
                                                                  const VkPhysicalDeviceImageFormatInfo2 *,
                                                                  VkImageFormatProperties2 *);
VkResult VKAPI_CALL nvhal_GetPhysicalDeviceImageFormatProperties2KHR(VkPhysicalDevice,
                                                                     const VkPhysicalDeviceImageFormatInfo2 *,
                                                                     VkImageFormatProperties2 *);
void VKAPI_CALL nvhal_GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalBufferInfo *,
                                                                VkExternalBufferProperties *);
void VKAPI_CALL nvhal_GetPhysicalDeviceExternalBufferPropertiesKHR(VkPhysicalDevice,
                                                                   const VkPhysicalDeviceExternalBufferInfo *,
                                                                   VkExternalBufferProperties *);
VkResult VKAPI_CALL nvhal_CreateBuffer(VkDevice, const VkBufferCreateInfo *, const VkAllocationCallbacks *, VkBuffer *);
VkResult VKAPI_CALL nvhal_CreateSamplerYcbcrConversion(VkDevice, const VkSamplerYcbcrConversionCreateInfo *,
                                                       const VkAllocationCallbacks *, VkSamplerYcbcrConversion *);
VkResult VKAPI_CALL nvhal_CreateSamplerYcbcrConversionKHR(VkDevice, const VkSamplerYcbcrConversionCreateInfo *,
                                                          const VkAllocationCallbacks *, VkSamplerYcbcrConversion *);
/* memory requirements of an image without its buffer (ahb.c): unrealised wrappers (either kind) and
 * vkGetDeviceImage* queries with the AHB handle type */
void nvhal_ahb_image_requirements(struct nvhal_device *d, VkImage wrapper, VkMemoryRequirements2 *out);
void nvhal_ahb_ci_requirements(struct nvhal_device *d, const VkImageCreateInfo *ci, VkMemoryRequirements2 *out,
                               bool khr);
bool nvhal_ci_is_ahb(const VkImageCreateInfo *ci);
bool nvhal_buffer_ci_is_ahb(const VkBufferCreateInfo *ci);
VkResult nvhal_buffer_ci_translate(const VkBufferCreateInfo *ci, VkBufferCreateInfo *out, void *storage,
                                   size_t storage_size);
/* private data / debug names on unrealised wrappers (ahb.c) */
bool nvhal_ahb_set_private_data(uint64_t handle, VkPrivateDataSlot slot, uint64_t data);
bool nvhal_ahb_get_private_data(uint64_t handle, VkPrivateDataSlot slot, uint64_t *data);
bool nvhal_ahb_set_object_name(uint64_t handle, const char *name);

/* intercept kinds (hal.c's table and imgxlate.c's nvhal_xl_entries) */
/* NVHAL_IC_WRAP: commands that must translate wrapper images, intercepted on devices where a wrapper
 * can exist (AHB or ANB enabled); NVHAL_IC_AHB: AHB-only semantics, intercepted on AHB devices */
/* NVHAL_IC_ASTC: the compute-state shadow (cmdstate.c), intercepted on devices that emulate ASTC */
enum { NVHAL_IC_ANY = 0, NVHAL_IC_DEVICE = 1, NVHAL_IC_ANB = 2, NVHAL_IC_AHB = 3, NVHAL_IC_AHB_ONLY = 4,
       NVHAL_IC_WRAP = 5, NVHAL_IC_ASTC = 6 };
struct nvhal_xl_entry { const char *name; PFN_vkVoidFunction fn; int kind; };
extern const struct nvhal_xl_entry nvhal_xl_entries[];
extern const struct nvhal_xl_entry nvhal_cs_entries[];

/* ---- opt-in VkImage translation cost statistics (imgxlate.c) ---- */
/* One presented frame (called by anb.c on the present path). No-op unless NVHAL_XLATE_STATS (host) /
 * debug.nvhal.xlate_stats (guest) turned the counters on; the module comment in imgxlate.c says what the
 * report line contains and how the two classes of count are defined. */
void nvhal_xst_frame(void);

/* ---- loader-object private data (imgxlate.c): freed by vkDestroyDevice ---- */
void nvhal_loader_pd_teardown(struct nvhal_device *d);

/* ---- command buffers -> device (imgxlate.c) ---- */
struct nvhal_cmd_state;
struct nvhal_cmd {
    struct nvhal_device *dev;
    VkCommandPool pool;
    uint32_t family;              /* queue family of the pool (cmdstate.c), UINT32_MAX when unknown */
    struct nvhal_cmd_state *cs;   /* compute-state shadow (cmdstate.c), NULL until something is recorded */
    struct nvhal_gt_cb *gt;       /* GPU timing state (gputime.c), NULL unless timing is on */
};
void nvhal_cmd_track(struct nvhal_device *d, VkCommandPool pool, const VkCommandBuffer *cmds, uint32_t n);
void nvhal_cmd_device_forget(struct nvhal_device *d);
struct nvhal_device *nvhal_cmd_device(VkCommandBuffer cmd);
/* the tracking entry (per-thread cached lookup); NULL for a command buffer not allocated through the HAL */
struct nvhal_cmd *nvhal_cmd_get(VkCommandBuffer cmd);

/* ---- opt-in GPU timing (gputime.c); every function is a no-op path unless the device has d->gt ---- */
struct nvhal_gt_dev;
struct nvhal_gt_cb;
void nvhal_gt_device_init(struct nvhal_device *d);     /* vkCreateDevice: reads the property, sets d->gt */
void nvhal_gt_device_teardown(struct nvhal_device *d);
void nvhal_gt_cb_free(struct nvhal_gt_cb *c);           /* NULL ok */
void nvhal_gt_frame(struct nvhal_device *d);            /* one presented image (d->gt non-NULL) */
void nvhal_exp_mem_init(struct nvhal_device *d);        /* memory-type experiment: prints the type/heap table and
                                                         * fills d->mem_remap. debug.nvhal.exp.memtype_target
                                                         * (>=0: every type without DEVICE_LOCAL -> that index) and
                                                         * debug.nvhal.exp.hostvis_cached (1: HV|HC -> same-heap
                                                         * HV|HOST_CACHED) take priority over
                                                         * debug.nvhal.exp.hostvis_to_bar (1 = HV|!HC|!DL -> +DL,
                                                         * 2 = any !DL -> closest DEVICE_LOCAL) */
const VkMemoryAllocateInfo *nvhal_exp_mem_remap(struct nvhal_device *d, const VkMemoryAllocateInfo *ai,
                                                VkMemoryAllocateInfo *storage);
void nvhal_exp_mem_remap_note(struct nvhal_device *d, uint32_t app_type, uint64_t bytes, VkResult r); /* one remapped
                                                                        * allocation actually handed to the ICD */
void nvhal_exp_mem_teardown(struct nvhal_device *d);  /* vkDestroyDevice: the src->dst count/bytes summary (1 line) */
void nvhal_exp_memstats_alloc(uint32_t type, uint64_t bytes, VkResult r); /* allocation census: one per allocation
                                                                          * handed to the ICD (off = 1 load) */
void nvhal_exp_memstats_fail(uint32_t type, VkResult r);      /* a HAL-level reject, before the ICD is called */
void nvhal_exp_memstats_present(struct nvhal_device *d);      /* one presented image: ends the preload phase and
                                                               * prints "alloc census @present #<n> t=<s>", throttled
                                                               * to one line per window (default 5 s) */
void nvhal_exp_memstats_teardown(struct nvhal_device *d);     /* vkDestroyDevice: the same line, marked "@final" */
/* vkGetDeviceProcAddr: the timing wrapper of name chaining to fn, or fn itself */
PFN_vkVoidFunction nvhal_gt_wrap(struct nvhal_device *d, const char *name, PFN_vkVoidFunction fn);

/* ---- compute-state shadow (cmdstate.c) ---- */
void nvhal_cs_free(struct nvhal_cmd_state *cs);
uint32_t nvhal_pool_family(VkCommandPool pool);
void nvhal_pool_forget(VkCommandPool pool);
/* re-records the app's compute pipeline, compute descriptor-set bindings and push constants after the HAL
 * recorded its own dispatch into cb; logs an error naming what could not be restored */
void nvhal_cs_restore(struct nvhal_device *d, VkCommandBuffer cb, const char *who);
/* true while cb records inside vkCmdBeginConditionalRenderingEXT/End (a dispatch there obeys the app's predicate) */
bool nvhal_cs_conditional(VkCommandBuffer cb);

/* ---- compressed-format emulation, ASTC LDR and ETC2/EAC (astc.c; the nvhal_astc_ names cover both) ---- */
bool nvhal_astc_format(VkFormat format);  /* one of the 28 2D LDR ASTC or the 10 ETC2/EAC formats */
bool nvhal_no_astc(void);                 /* no emulated compressed image exists in the process */
const struct nvhal_astc_pd *nvhal_astc_pd_get(struct nvhal_instance *inst, VkPhysicalDevice pd);
void nvhal_astc_instance_teardown(struct nvhal_instance *inst);
void VKAPI_CALL nvhal_GetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures *);
void VKAPI_CALL nvhal_GetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
void VKAPI_CALL nvhal_GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
void VKAPI_CALL nvhal_GetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat, VkFormatProperties *);
void VKAPI_CALL nvhal_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice, VkFormat, VkFormatProperties2 *);
void VKAPI_CALL nvhal_GetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice, VkFormat, VkFormatProperties2 *);
VkResult VKAPI_CALL nvhal_GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling,
                                                                VkImageUsageFlags, VkImageCreateFlags,
                                                                VkImageFormatProperties *);
/* vkGetPhysicalDeviceImageFormatProperties2 for an emulated compressed format on an emulating physical device
 * (ahb.c's gpdifp2 calls it first); false: not such a format/not emulated, the caller continues */
bool nvhal_astc_gpdifp2(struct nvhal_instance *inst, VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info,
                        VkImageFormatProperties2 *p, PFN_vkGetPhysicalDeviceImageFormatProperties2 f, VkResult *r);
/* vkCreateDevice (device.c): decides emulation for the device and rewrites the enabled features
 * (textureCompressionASTC_LDR/ETC2 are the HAL's when emulated, never the ICD's; shaderStorageImageWriteWithoutFormat
 * is enabled for the decoders). storage must outlive the ICD call. *maint2: the device also needs VK_KHR_maintenance2. */
struct nvhal_astc_ci_storage { VkPhysicalDeviceFeatures feats; VkPhysicalDeviceFeatures2 f2; unsigned char chain[2048]; };
VkResult nvhal_astc_device_ci(struct nvhal_instance *inst, VkPhysicalDevice pd, uint32_t api, const VkDeviceCreateInfo *ci,
                              VkDeviceCreateInfo *out, struct nvhal_astc_ci_storage *st, bool *emulate, bool *maint2);
VkResult nvhal_astc_device_init(struct nvhal_device *d); /* after the ICD created the device */
void nvhal_astc_device_teardown(struct nvhal_device *d);
/* the image entry points; each returns false when the image/create info is not an emulated compressed one */
bool nvhal_astc_CreateImage(struct nvhal_device *d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *alloc,
                            VkImage *pImage, VkResult *r);
bool nvhal_astc_DestroyImage(struct nvhal_device *d, VkImage image, const VkAllocationCallbacks *alloc);
bool nvhal_astc_image_requirements(VkImage image, VkMemoryRequirements2 *out);
bool nvhal_astc_ci_requirements(struct nvhal_device *d, const VkImageCreateInfo *ci, VkMemoryRequirements2 *out, bool khr);
bool nvhal_astc_ci_refused(struct nvhal_device *d, const VkImageCreateInfo *ci, const char *who);
bool nvhal_astc_is_image(VkImage image);
VkResult nvhal_astc_bind1(struct nvhal_device *d, VkImage image, VkDeviceMemory mem, VkDeviceSize offset, bool *handled);
/* vkBindImageMemory2: *out = in, or a malloc'd array with the emulation's own binds added */
VkResult nvhal_astc_expand_binds(uint32_t n, const VkBindImageMemoryInfo *in, VkBindImageMemoryInfo **out, uint32_t *nout);
/* vkAllocateMemory / vkFreeMemory (ahb.c): while debug.nvhal.astc_blocks_host / NVHAL_ASTC_BLOCKS_HOST is on, remember
 * which memory type the app's memory got - a VkDeviceMemory does not tell, and the block-host summary line reports the
 * memoryTypeIndex histogram of the emulated images' bindings (astc.c). Off: one acquire load each. */
void nvhal_astc_mem_note(VkDeviceMemory mem, uint32_t type);
void nvhal_astc_mem_forget(VkDeviceMemory mem);
bool nvhal_astc_CreateImageView(struct nvhal_device *d, VkDevice device, const VkImageViewCreateInfo *ci,
                                const VkAllocationCallbacks *alloc, VkImageView *view, VkResult *r);
/* vkAllocateMemory: a dedicated allocation naming an emulated image is made non-dedicated (the image's memory
 * also backs the emulation's block images); 1: *out was rewritten (pNext copied into storage), 0: untouched,
 * -1: the chain could not be rewritten (logged) */
int nvhal_astc_alloc_info(const VkMemoryAllocateInfo *ai, VkMemoryAllocateInfo *out, void *storage, size_t storage_size);
/* transfer commands; each returns true when it recorded the command itself */
bool nvhal_astc_cmd_copy_buffer_to_image(struct nvhal_device *d, VkCommandBuffer cb, VkBuffer src, VkImage dst,
                                         VkImageLayout layout, uint32_t n, const VkBufferImageCopy *r);
bool nvhal_astc_cmd_copy_buffer_to_image2(struct nvhal_device *d, VkCommandBuffer cb, const VkCopyBufferToImageInfo2 *info);
bool nvhal_astc_cmd_copy_image_to_buffer(struct nvhal_device *d, VkCommandBuffer cb, VkImage src, VkImageLayout layout,
                                         VkBuffer dst, uint32_t n, const VkBufferImageCopy *r);
bool nvhal_astc_cmd_copy_image_to_buffer2(struct nvhal_device *d, VkCommandBuffer cb, const VkCopyImageToBufferInfo2 *info);
bool nvhal_astc_cmd_copy_image(struct nvhal_device *d, VkCommandBuffer cb, VkImage src, VkImageLayout srcL, VkImage dst,
                               VkImageLayout dstL, uint32_t n, const VkImageCopy *r);
bool nvhal_astc_cmd_copy_image2(struct nvhal_device *d, VkCommandBuffer cb, const VkCopyImageInfo2 *info);
/* a command the emulation does not implement names an emulated image: logs (R8) and returns true (drop it) */
bool nvhal_astc_refuse(VkImage image, const char *what);
/* image barriers: the emulation's block images follow the app image's layouts and queue ownership; returns in
 * or an array with their barriers appended (*owned: free it) */
const VkImageMemoryBarrier *nvhal_astc_barriers(const VkImageMemoryBarrier *in, uint32_t n, uint32_t *nout,
                                                VkImageMemoryBarrier **owned);
/* vkQueueBindSparse: opaque binds of an emulated image are split over its RGBA8 image and block images; *out = in
 * or a heap copy whose arrays are listed in *owned (free each, then the list; *nowned entries) */
VkResult nvhal_astc_bind_sparse(uint32_t n, const VkBindSparseInfo *in, VkBindSparseInfo **out, void ***owned,
                                uint32_t *nowned);
const VkImageMemoryBarrier2 *nvhal_astc_barriers2(const VkImageMemoryBarrier2 *in, uint32_t n, uint32_t *nout,
                                                  VkImageMemoryBarrier2 **owned);

/* generic pNext surgery (imgxlate.c): *out = the const chain `head` with node `drop` removed (NULL:
 * nothing removed) and the caller-owned chain `add` (may be NULL; its last pNext field is
 * *add_tail_next) prepended. Only the nodes before `drop` are copied, into `storage` (sizes from
 * the generated nvhal_stype_size); the rest of the app's chain is shared. false (logged with `who`)
 * when a node before `drop` has an sType unknown to this build or storage is too small. */
bool nvhal_chain_replace(const void *head, const void *drop, const void *add, const void **add_tail_next,
                         void *storage, size_t storage_size, const char *who, const void **out);
size_t nvhal_stype_size(VkStructureType s); /* generated; 0 = unknown to this build */
bool nvhal_image_ext_hidden(const char *name); /* generated */
bool nvhal_device_ext_known(const char *name); /* generated */
const char *nvhal_registry_version(void);    /* generated */

#endif
