/* Device-level state: extension filtering, vkCreateDevice / vkDestroyDevice, the
 * dispatchable-object checks for queues and command buffers, and
 * VkPhysicalDevicePresentationPropertiesANDROID. */
#include "nvhal.h"

#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct nvhal_map devices; /* VkDevice -> struct nvhal_device* */
static struct nvhal_map queues;  /* VkQueue -> struct nvhal_device* */

struct nvhal_device *nvhal_device_get(VkDevice device) {
    pthread_mutex_lock(&nvhal_lock);
    struct nvhal_device *d = nvhal_map_get(&devices, device);
    pthread_mutex_unlock(&nvhal_lock);
    return d;
}

struct nvhal_device *nvhal_queue_device(VkQueue queue) {
    pthread_mutex_lock(&nvhal_lock);
    struct nvhal_device *d = nvhal_map_get(&queues, queue);
    pthread_mutex_unlock(&nvhal_lock);
    return d;
}

/* Device extensions that need a VkSurfaceKHR / VkSwapchainKHR / display the ICD never owns
 * here: the Android loader implements VK_KHR_swapchain on top of VK_ANDROID_native_buffer. */
static const char *const hidden_device_exts[] = {
    "VK_KHR_swapchain", "VK_KHR_display_swapchain", "VK_EXT_display_control",
    "VK_KHR_swapchain_mutable_format", "VK_EXT_swapchain_maintenance1",
    "VK_KHR_swapchain_maintenance1", "VK_KHR_present_id", "VK_KHR_present_id2",
    "VK_KHR_present_wait", "VK_KHR_present_wait2", "VK_KHR_present_mode_fifo_latest_ready",
    "VK_EXT_present_mode_fifo_latest_ready", "VK_EXT_present_timing", "VK_NV_present_metering",
    "VK_NV_internal_nvpresent", "VK_GOOGLE_display_timing", "VK_KHR_incremental_present",
    "VK_KHR_shared_presentable_image", "VK_EXT_hdr_metadata", "VK_EXT_full_screen_exclusive",
    "VK_NV_acquire_winrt_display",
    /* its commands take a VkSwapchainKHR (vkSetLatencySleepModeNV & co.) */
    "VK_NV_low_latency2",
};

/* Device extensions whose vkCreateDevice needs an NVIDIA library that is not part of the guest
 * closure. Measured with probe/nvexts (vkexts.c --each) in the guest, 2026-10-10, driver
 * 615.71.09: with the raw ICD, vkCreateDevice enabling each of these alone fails with
 * VK_ERROR_INITIALIZATION_FAILED (the other 284 of 291 succeed). The shim dlopen trace shows the
 * cause: eglcore dlopen("libnvidia-rtcore.so.<ver>") fails for the ray-tracing group and
 * dlopen("libcuda.so.1") for the CUDA group. dEQP (and HWUI) enable every advertised extension,
 * so advertising them makes every vkCreateDevice fail. Static list on purpose (no runtime dlopen
 * probe); probe/nvexts is the regression check. */
static const char *const hidden_device_exts_missing_lib[] = {
    /* needs libnvidia-rtcore.so.<driver version> */
    "VK_KHR_acceleration_structure", "VK_KHR_ray_query", "VK_KHR_ray_tracing_pipeline", "VK_NV_ray_tracing",
    /* needs libcuda.so.1 */
    "VK_NV_cuda_kernel_launch", "VK_NVX_binary_import", "VK_NV_optical_flow",
};
#define MISSING_LIB_REMEDY "ship libnvidia-rtcore.so.<driver version> (+ its libgcc_s front) for the " \
    "ray-tracing extensions and libcuda.so.1 for the CUDA ones in the guest closure " \
    "(/vendor_extra/lib64), then drop them from hidden_device_exts_missing_lib"

/* Device extensions whose central promise the ICD refuses to keep for an Android process.
 * Measured with probe/nvexts --loader --hwui-bisect in the guest, 2026-10-10, driver 615.71.09,
 * under uid 10200 (appsandbox) and uid 1000: HWUI's exact vkCreateDevice (every extension + its
 * features2 chain) succeeds with no VkDeviceQueueGlobalPriorityCreateInfoKHR and with LOW/MEDIUM,
 * and fails with VK_ERROR_NOT_PERMITTED (-1000174001) for HIGH and REALTIME; as root everything
 * succeeds. Android consumers treat the extension as a promise: SystemUI raises HWUI's context
 * priority to SurfaceFlinger's GPU priority (frameworks/base SystemUIApplication.java:124-129)
 * and HWUI aborts when the device cannot be created (VulkanManager.cpp:319), so every app dies.
 * Static list on purpose; nvexts --hwui-bisect is the regression check. */
static const char *const hidden_device_exts_priority[] = {
    "VK_EXT_global_priority", "VK_KHR_global_priority", "VK_EXT_global_priority_query",
};
#define PRIORITY_REMEDY "run the process with the privilege NVIDIA requires for high queue priority " \
    "(root/CAP_SYS_NICE) and drop the group (hidden_device_exts_priority)"

static bool device_ext_priority(const char *name) {
    for (size_t i = 0; i < sizeof hidden_device_exts_priority / sizeof *hidden_device_exts_priority; i++)
        if (!strcmp(name, hidden_device_exts_priority[i])) return true;
    return false;
}

static bool device_ext_missing_lib(const char *name) {
    for (size_t i = 0; i < sizeof hidden_device_exts_missing_lib / sizeof *hidden_device_exts_missing_lib; i++)
        if (!strcmp(name, hidden_device_exts_missing_lib[i])) return true;
    return false;
}

static bool device_ext_hidden(const char *name) {
    for (size_t i = 0; i < sizeof hidden_device_exts / sizeof *hidden_device_exts; i++)
        if (!strcmp(name, hidden_device_exts[i])) return true;
    return device_ext_missing_lib(name) || device_ext_priority(name) || nvhal_image_ext_hidden(name);
}

/* The ICD-advertised members of one hidden group, space-separated; "" when it advertises none. */
static size_t group_members(const VkExtensionProperties *icd, uint32_t n, bool (*in_group)(const char *),
                            char *buf, size_t size) {
    size_t o = 0;
    buf[0] = 0;
    for (uint32_t i = 0; i < n; i++)
        if (in_group(icd[i].extensionName) && o + VK_MAX_EXTENSION_NAME_SIZE + 2 < size)
            o += (size_t)snprintf(buf + o, size - o, "%s%s", o ? " " : "", icd[i].extensionName);
    return o;
}

/* One info line per process for each hidden group the ICD advertises. */
static void report_missing_lib_exts(const VkExtensionProperties *icd, uint32_t n) {
    static atomic_bool reported;
    if (atomic_load(&reported)) return;
    char buf[512];
    if (!group_members(icd, n, device_ext_missing_lib, buf, sizeof buf) || atomic_exchange(&reported, true)) return;
    nvhal_log(NVHAL_LVL_INFO, "hiding device extension(s) whose vkCreateDevice needs an NVIDIA library "
              "absent from the guest (measured: VK_ERROR_INITIALIZATION_FAILED): %s. Remedy: %s "
              "(reported once per process)", buf, MISSING_LIB_REMEDY);
}

static void report_priority_exts(const VkExtensionProperties *icd, uint32_t n) {
    static atomic_bool reported;
    if (atomic_load(&reported)) return;
    char buf[512]; /* group_members() needs room for a full VK_MAX_EXTENSION_NAME_SIZE name */
    if (!group_members(icd, n, device_ext_priority, buf, sizeof buf) || atomic_exchange(&reported, true)) return;
    nvhal_log(NVHAL_LVL_INFO, "hiding device extension(s) whose HIGH/REALTIME queue priority the ICD refuses to "
              "non-root processes (measured: vkCreateDevice -> VK_ERROR_NOT_PERMITTED (-1000174001), which "
              "HWUI aborts on): %s. Remedy: %s (reported once per process)", buf, PRIORITY_REMEDY);
}

/* What VK_ANDROID_native_buffer is built on (anb.c). */
static const char *const anb_required_exts[] = {
    "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier",
    "VK_KHR_external_semaphore_fd", "VK_KHR_external_fence_fd", "VK_EXT_queue_family_foreign",
    "VK_KHR_image_format_list",
};
/* What VK_ANDROID_external_memory_android_hardware_buffer is built on (ahb.c). Its own spec
 * dependencies are VK_KHR_sampler_ycbcr_conversion + VK_KHR_external_memory + VK_EXT_queue_family_foreign
 * + VK_KHR_dedicated_allocation (1.1 core but for queue_family_foreign); the dma-buf/modifier
 * ones carry the implementation. VK_KHR_format_feature_flags2 is only queried (modifier features
 * per format), which needs support, not enabling. */
static const char *const ahb_required_exts[] = {
    "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier",
    "VK_EXT_queue_family_foreign", "VK_KHR_image_format_list",
};
static const char *const ahb_supported_exts[] = {"VK_KHR_format_feature_flags2"};
/* ... plus, when the device's effective API version is 1.0, the extensions that 1.1 made
 * core and that the lists above depend on. */
static const char *const required_exts_1_0[] = {
    "VK_KHR_external_memory", "VK_KHR_external_semaphore", "VK_KHR_external_fence",
    "VK_KHR_bind_memory2", "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2",
    "VK_KHR_maintenance1", "VK_KHR_sampler_ycbcr_conversion",
};
#define N_OF(a) (sizeof(a) / sizeof *(a))
#define AHB_EXT VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME

/* Measured on the host (driver 615.71.09, probe/vkcaps + an LD_AUDIT deny-list): NVIDIA's ICD
 * advertises VK_EXT_external_memory_dma_buf only when dlopen("libdrm.so.2") succeeds in its own
 * namespace AND the NVIDIA DRM render node is present. Android has no libdrm.so.2 of its own;
 * nvidia-prolapse/libdrm builds one (README.md there). */
#ifdef __ANDROID__
#define DMA_BUF_PREREQS "libdrm.so.2 must be loadable from the ICD's namespace " \
    "(/vendor_extra/lib64/libdrm.so.2, deployed by dev/native-deploy) and the NVIDIA render node must be present"
#else
#define DMA_BUF_PREREQS "libdrm.so.2 must be loadable from the ICD's namespace " \
    "(the host's libdrm) and the NVIDIA render node must be present"
#endif

/* Extra explanation for a missing ANB/AHB prerequisite, "" when there is nothing to add. */
static const char *anb_missing_hint(const char *miss) {
    if (!strcmp(miss, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME))
#ifdef NVHAL_SOURCE_RM
        return "; source NVK requires the Rust RM dma-buf import/export backend and "
               "a matching source libdrm with access to the NVIDIA DRM render node";
#else
        return "; NVIDIA's ICD only advertises it when both measured prerequisites hold: " DMA_BUF_PREREQS
               " (nvidia-prolapse/libdrm/README.md)";
#endif
    return "";
}

static VkResult icd_device_exts(struct nvhal_instance *inst, VkPhysicalDevice pd,
                                VkExtensionProperties **out, uint32_t *n_out) {
    VkResult r;
    VkExtensionProperties *p = NULL;
    uint32_t n = 0;
    do {
        r = inst->EnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
        if (r != VK_SUCCESS) break;
        VkExtensionProperties *q = realloc(p, (n + 2) * sizeof *p); /* +2: room for ANB and AHB */
        if (!q) { r = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
        p = q;
        r = inst->EnumerateDeviceExtensionProperties(pd, NULL, &n, p);
    } while (r == VK_INCOMPLETE);
    if (r != VK_SUCCESS) {
        NVHAL_ERR("ICD vkEnumerateDeviceExtensionProperties -> %s", NVHAL_VKR(r));
        free(p);
        return r;
    }
    *out = p;
    *n_out = n;
    return VK_SUCCESS;
}

static const char *list_missing(const VkExtensionProperties *icd, uint32_t n, const char *const *req, size_t nreq) {
    for (size_t i = 0; i < nreq; i++)
        if (!nvhal_ext_in(icd, n, req[i])) return req[i];
    return NULL;
}
static const char *anb_missing(const VkExtensionProperties *icd, uint32_t n, bool legacy) {
    const char *m = list_missing(icd, n, anb_required_exts, N_OF(anb_required_exts));
    return m ? m : legacy ? list_missing(icd, n, required_exts_1_0, N_OF(required_exts_1_0)) : NULL;
}
static const char *ahb_missing(const VkExtensionProperties *icd, uint32_t n, bool legacy) {
    const char *m = list_missing(icd, n, ahb_required_exts, N_OF(ahb_required_exts));
    if (!m) m = list_missing(icd, n, ahb_supported_exts, N_OF(ahb_supported_exts));
    return m ? m : legacy ? list_missing(icd, n, required_exts_1_0, N_OF(required_exts_1_0)) : NULL;
}

/* ICD extensions the vk.xml this HAL was generated from does not know: their commands cannot be
 * checked for VkImage parameters (tools/vkimage-coverage.py), so they are hidden. One warning per
 * process names them. */
static void report_unknown_exts(const VkExtensionProperties *icd, uint32_t n) {
    static atomic_bool reported;
    if (atomic_load(&reported)) return;
    char buf[512];
    size_t o = 0;
    buf[0] = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!nvhal_device_ext_known(icd[i].extensionName) && o + VK_MAX_EXTENSION_NAME_SIZE + 2 < sizeof buf)
            o += (size_t)snprintf(buf + o, sizeof buf - o, "%s%s", o ? " " : "", icd[i].extensionName);
    if (!o || atomic_exchange(&reported, true)) return;
    NVHAL_WARN("hiding device extension(s) unknown to this HAL's registry (%s): %s. Their commands cannot be checked "
               "for VkImage parameters, which AHB images need translated. Remedy: rebuild the HAL against a vk.xml "
               "that defines them (reported once per process)", nvhal_registry_version(), buf);
}

static bool device_ext_exposed(const char *name) {
    return !device_ext_hidden(name) && nvhal_device_ext_known(name);
}

VkResult VKAPI_CALL nvhal_EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *pLayerName,
                                                             uint32_t *pCount, VkExtensionProperties *pProps) {
    struct nvhal_instance *inst = nvhal_physdev_instance(pd);
    if (!inst) {
        NVHAL_ERR("vkEnumerateDeviceExtensionProperties(%p): physical device was not enumerated "
                  "through this HAL", (void *)pd);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pLayerName) return inst->EnumerateDeviceExtensionProperties(pd, pLayerName, pCount, pProps);
    VkExtensionProperties *icd;
    uint32_t n;
    VkResult r = icd_device_exts(inst, pd, &icd, &n);
    if (r != VK_SUCCESS) return r;
    /* The loader and apps query this many times per process; one report is enough. */
    static atomic_bool anb_missing_reported, ahb_missing_reported;
    const char *miss = anb_missing(icd, n, false);
    if (miss && !atomic_exchange(&anb_missing_reported, true))
        NVHAL_ERR("the ICD lacks %s, which VK_ANDROID_native_buffer (VK_KHR_swapchain for apps) "
                  "needs; vkCreateDevice with it will fail%s (reported once per process)",
                  miss, anb_missing_hint(miss));
    const char *amiss = ahb_missing(icd, n, false);
    if (amiss && !atomic_exchange(&ahb_missing_reported, true))
        NVHAL_ERR("the ICD lacks %s, which " AHB_EXT " needs; the extension is not offered%s (reported once per process)",
                  amiss, anb_missing_hint(amiss));
    if (!nvhal_ahb_available) nvhal_ahb_report_unavailable();
    report_missing_lib_exts(icd, n);
    report_priority_exts(icd, n);
    report_unknown_exts(icd, n);
    uint32_t nv = 0;
    for (uint32_t i = 0; i < n; i++)
        if (device_ext_exposed(icd[i].extensionName)) icd[nv++] = icd[i];
    memset(&icd[nv], 0, sizeof icd[nv]);
    strcpy(icd[nv].extensionName, VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME);
    icd[nv].specVersion = VK_ANDROID_NATIVE_BUFFER_SPEC_VERSION;
    nv++;
    if (nvhal_ahb_available && !amiss) {
        memset(&icd[nv], 0, sizeof icd[nv]);
        strcpy(icd[nv].extensionName, AHB_EXT);
        icd[nv].specVersion = VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_SPEC_VERSION;
        nv++;
    }
    r = nvhal_fill_ext_props(icd, nv, pCount, pProps);
    free(icd);
    return r;
}

static uint32_t api_min(uint32_t a, uint32_t b) {
    uint32_t ma = VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(a), VK_API_VERSION_MINOR(a), 0);
    uint32_t mb = VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(b), VK_API_VERSION_MINOR(b), 0);
    return ma < mb ? ma : mb;
}

static bool add_name(const char **names, uint32_t *n, const char *name) {
    for (uint32_t i = 0; i < *n; i++)
        if (!strcmp(names[i], name)) return false;
    names[(*n)++] = name;
    return true;
}

/* Enable an extension the HAL itself needs. Never one of the hidden ones (a hidden extension's
 * vkCreateDevice may fail, e.g. the missing-library group). */
static bool add_prereq(const char **names, uint32_t *n, const char *name, const char *why) {
    if (device_ext_hidden(name)) {
        NVHAL_ERR("vkCreateDevice: %s needs %s, which this HAL hides; refusing to enable it behind the app's back",
                  why, name);
        return false;
    }
    if (add_name(names, n, name)) NVHAL_TRACE("vkCreateDevice: %s: also enabling %s", why, name);
    return true;
}

static const VkDeviceQueueGlobalPriorityCreateInfoKHR *queue_global_priority(const VkDeviceQueueCreateInfo *q) {
    for (const VkBaseInStructure *b = q->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR)
            return (const VkDeviceQueueGlobalPriorityCreateInfoKHR *)b;
    return NULL;
}

static const char *global_priority_name(VkQueueGlobalPriorityKHR p) {
    switch ((int)p) {
    case VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR: return "LOW";
    case VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR: return "MEDIUM";
    case VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR: return "HIGH";
    case VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR: return "REALTIME";
    default: return "invalid";
    }
}

/* What the ICD was asked for, for its refusal's log line: every queue create info (family, count,
 * flags, the global priority if chained, other chained sTypes) and every sType chained on
 * VkDeviceCreateInfo.pNext, in order. Truncated with "..." when it does not fit. */
static void describe_device_ci(const VkDeviceCreateInfo *ci, char *buf, size_t size) {
    size_t o = 0;
#define PUT(...) do { if (o < size) o += (size_t)snprintf(buf + o, size - o, __VA_ARGS__); } while (0)
    buf[0] = 0;
    PUT("%u queue create info(s):", ci->queueCreateInfoCount);
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
        const VkDeviceQueueCreateInfo *q = &ci->pQueueCreateInfos[i];
        PUT(" [family %u x%u", q->queueFamilyIndex, q->queueCount);
        if (q->flags) PUT(" flags 0x%x", q->flags);
        const VkDeviceQueueGlobalPriorityCreateInfoKHR *gp = queue_global_priority(q);
        if (gp) PUT(" VkDeviceQueueGlobalPriorityCreateInfoKHR %s (%d)", global_priority_name(gp->globalPriority),
                    (int)gp->globalPriority);
        else PUT(" no global priority");
        for (const VkBaseInStructure *b = q->pNext; b; b = b->pNext)
            if (b->sType != VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR) PUT(" +sType %d", (int)b->sType);
        PUT("]");
    }
    PUT("; VkDeviceCreateInfo.pNext sTypes:");
    unsigned n = 0;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext, n++) PUT(" %d", (int)b->sType);
    if (!n) PUT(" none");
    PUT("; pEnabledFeatures %s", ci->pEnabledFeatures ? "set" : "NULL");
#undef PUT
    if (o >= size && size > 4) memcpy(buf + size - 4, "...", 4);
}

/* ---- Queue-timeline start (AHB-CONTRACT.md §8; egl/FENCE-CONTRACT.md §3.5, §6a) ----
 *
 * NVIDIA 615.71.09 keeps one semaphore timeline {V, started} per VkQueue (measured: vk-first-wait mq-* modes, one
 * SEMSURF context per created queue). A GPU wait on an imported SYNC_FD advances V without setting `started`; the
 * first sync_fd export (vkGetFenceFdKHR / vkGetSemaphoreFdKHR) on a timeline whose V != 0 then takes the wait's own
 * value and signals with the imported fence, covering none of the work before it (defect D1). Invariant I: the first
 * fence creation on every timeline happens while V == 0. It sets `started` and forces a release after all earlier
 * commands; from then on every submission ends with a release and every export takes the latest one (§6a).
 *
 * The HAL establishes I inside vkCreateDevice, before the VkDevice exists for the app: one empty submit with an
 * exportable fence (or, without VK_KHR_external_fence_fd, an exportable semaphore) and its sync_fd export, on every
 * queue of every VkDeviceQueueCreateInfo. Nothing of the app can have waited yet, so V == 0 on each of them. Why here
 * and not in vkGetDeviceQueue{,2}: those return void (no error to report), may be called from any app thread at any
 * time, and the timelines are per queue but are created with the device (CTX_CREATE count = 3 + queues at
 * vkCreateDevice); measured: the start holds across threads (warm-thread), vkQueueWaitIdle/vkDeviceWaitIdle
 * (warm-qidle/-didle) and the vkGetDeviceQueue2(flags 0) handle (warm-q2, same VkQueue), and does NOT carry over
 * between queues of one family or between families (mq-warm-g0-use-g1, -g15, -c0: 24/24 early) -> every queue.
 *
 * Devices that enable neither VK_KHR_external_semaphore_fd nor VK_KHR_external_fence_fd (VK_ANDROID_native_buffer
 * enables both) can neither import a SYNC_FD wait nor export a sync_fd, so no fence of theirs is ever created or
 * observed through the defective path; they are not started.
 *
 * Every failure is loud and fails vkCreateDevice: an export of -1 would mean the driver skipped the fence creation
 * that sets `started`, so it is a failure too, never "already signalled, fine". */
#define START_TIMEOUT_MS 5000
#define START_TIMEOUT_STR "5000"
static double start_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}
static VkResult start_one_queue(struct nvhal_device *d, VkQueue q, uint32_t fam, uint32_t idx, bool use_fence,
                                const VkAllocationCallbacks *alloc, PFN_vkQueueSubmit submit, PFN_vkCreateFence cfence,
                                PFN_vkDestroyFence dfence, PFN_vkGetFenceFdKHR fence_fd, PFN_vkCreateSemaphore csem,
                                PFN_vkDestroySemaphore dsem, PFN_vkGetSemaphoreFdKHR sem_fd) {
    VkDevice dev = d->handle;
    VkFence f = VK_NULL_HANDLE;
    VkSemaphore s = VK_NULL_HANDLE;
    int fd = -1;
    VkResult r;
    const char *what;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (use_fence) {
        VkExportFenceCreateInfo e = {.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
                                     .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = &e};
        what = "vkCreateFence(exportable SYNC_FD)";
        if ((r = cfence(dev, &fci, alloc, &f)) != VK_SUCCESS) goto fail;
        what = "vkQueueSubmit(empty, fence)";
        if ((r = submit(q, 1, &si, f)) != VK_SUCCESS) goto fail;
        VkFenceGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, .fence = f,
                                  .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        what = "vkGetFenceFdKHR(SYNC_FD)";
        if ((r = fence_fd(dev, &gi, &fd)) != VK_SUCCESS) goto fail;
    } else {
        VkExportSemaphoreCreateInfo e = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                                         .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
        VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &e};
        what = "vkCreateSemaphore(exportable SYNC_FD)";
        if ((r = csem(dev, &sci, alloc, &s)) != VK_SUCCESS) goto fail;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &s;
        what = "vkQueueSubmit(empty, signal semaphore)";
        if ((r = submit(q, 1, &si, VK_NULL_HANDLE)) != VK_SUCCESS) goto fail;
        VkSemaphoreGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = s,
                                      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
        what = "vkGetSemaphoreFdKHR(SYNC_FD)";
        if ((r = sem_fd(dev, &gi, &fd)) != VK_SUCCESS) goto fail;
    }
    if (fd < 0) {
        what = use_fence ? "vkGetFenceFdKHR(SYNC_FD) returned fd -1 (no fence was created, so the timeline is not "
                           "started)"
                         : "vkGetSemaphoreFdKHR(SYNC_FD) returned fd -1 (no fence was created, so the timeline is not "
                           "started)";
        r = VK_ERROR_INITIALIZATION_FAILED;
        goto fail;
    }
    /* The fence/semaphore may only be destroyed once its submission has completed: wait for the exported fd. */
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int pr;
    double t0 = start_now_ms();
    for (;;) {
        int left = START_TIMEOUT_MS - (int)(start_now_ms() - t0);
        pr = poll(&p, 1, left > 0 ? left : 0);
        if (pr < 0 && errno == EINTR) continue;
        break;
    }
    if (pr != 1) {
        what = pr == 0 ? "the empty submission's sync_fd did not signal within " START_TIMEOUT_STR " ms"
                       : "poll() on the empty submission's sync_fd failed";
        r = VK_ERROR_INITIALIZATION_FAILED;
        goto fail;
    }
    close(fd);
    if (f) dfence(dev, f, alloc);
    if (s) dsem(dev, s, alloc);
    return VK_SUCCESS;
fail:
    NVHAL_ERR("vkCreateDevice: starting the semaphore timeline of queue (family %u, index %u) failed: %s -> %s. "
              "Without it the queue's first exported sync_fd after an imported-fence wait signals before the work it "
              "covers (NVIDIA defect D1, nvidia-prolapse/hal/AHB-CONTRACT.md §8); the device is not created",
              fam, idx, what, NVHAL_VKR(r));
    if (fd >= 0) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        poll(&p, 1, START_TIMEOUT_MS); /* let the submission finish before its fence/semaphore is destroyed */
        close(fd);
    }
    return r;
}

static bool name_in(const char *const *names, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(names[i], name)) return true;
    return false;
}

/* Returns VK_SUCCESS when every queue's timeline is started, or when the device needs none (see above). On failure
 * the caller destroys the device (vkDestroyDevice waits for the submissions made here). */
static VkResult start_queue_timelines(struct nvhal_device *d, const VkDeviceCreateInfo *ci, bool sem_ext, bool fence_ext,
                                      const VkAllocationCallbacks *alloc, uint32_t *n_started) {
    *n_started = 0;
#ifdef NVHAL_SOURCE_RM
    /* D1 is a proprietary NVIDIA userspace defect. Mesa's DRM syncobj path
     * may validly export an already-signalled sync_file as fd -1. */
    return VK_SUCCESS;
#endif
    if (!sem_ext && !fence_ext) return VK_SUCCESS;
    PFN_vkGetDeviceProcAddr gdpa = d->inst->GetDeviceProcAddr;
    VkDevice dev = d->handle;
    PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)gdpa(dev, "vkQueueSubmit");
    PFN_vkCreateFence cfence = (PFN_vkCreateFence)gdpa(dev, "vkCreateFence");
    PFN_vkDestroyFence dfence = (PFN_vkDestroyFence)gdpa(dev, "vkDestroyFence");
    PFN_vkGetFenceFdKHR fence_fd = fence_ext ? (PFN_vkGetFenceFdKHR)gdpa(dev, "vkGetFenceFdKHR") : NULL;
    PFN_vkCreateSemaphore csem = (PFN_vkCreateSemaphore)gdpa(dev, "vkCreateSemaphore");
    PFN_vkDestroySemaphore dsem = (PFN_vkDestroySemaphore)gdpa(dev, "vkDestroySemaphore");
    PFN_vkGetSemaphoreFdKHR sem_fd = !fence_ext ? (PFN_vkGetSemaphoreFdKHR)gdpa(dev, "vkGetSemaphoreFdKHR") : NULL;
    const char *missing = !submit ? "vkQueueSubmit" : fence_ext && (!cfence || !dfence || !fence_fd) ? "the fence entry points"
                        : !fence_ext && (!csem || !dsem || !sem_fd) ? "the semaphore entry points" : NULL;
    if (missing) {
        NVHAL_ERR("vkCreateDevice: the ICD returned NULL for %s needed to start the queues' semaphore timelines (D1, "
                  "AHB-CONTRACT.md §8) -> VK_ERROR_INITIALIZATION_FAILED", missing);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
        const VkDeviceQueueCreateInfo *q = &ci->pQueueCreateInfos[i];
        for (uint32_t k = 0; k < q->queueCount; k++) {
            VkQueue queue = VK_NULL_HANDLE;
            if (q->flags) { /* a queue created with flags is only reachable through vkGetDeviceQueue2 with them */
                if (!d->GetDeviceQueue2) {
                    NVHAL_ERR("vkCreateDevice: queue family %u was created with flags 0x%x but the ICD has no "
                              "vkGetDeviceQueue2 to fetch it for the timeline start -> VK_ERROR_INITIALIZATION_FAILED",
                              q->queueFamilyIndex, q->flags);
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                VkDeviceQueueInfo2 qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, .flags = q->flags,
                                         .queueFamilyIndex = q->queueFamilyIndex, .queueIndex = k};
                d->GetDeviceQueue2(dev, &qi, &queue);
            } else {
                d->GetDeviceQueue(dev, q->queueFamilyIndex, k, &queue);
            }
            if (queue == VK_NULL_HANDLE) {
                NVHAL_ERR("vkCreateDevice: the ICD returned no VkQueue for family %u index %u (flags 0x%x) of the device "
                          "it just created -> VK_ERROR_INITIALIZATION_FAILED", q->queueFamilyIndex, k, q->flags);
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            VkResult r = start_one_queue(d, queue, q->queueFamilyIndex, k, fence_ext, alloc, submit, cfence, dfence,
                                         fence_fd, csem, dsem, sem_fd);
            if (r != VK_SUCCESS) return r;
            (*n_started)++;
        }
    }
    return VK_SUCCESS;
}

VkResult VKAPI_CALL nvhal_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                       const VkAllocationCallbacks *alloc, VkDevice *pDevice) {
    struct nvhal_instance *inst = nvhal_physdev_instance(pd);
    if (!inst) {
        NVHAL_ERR("vkCreateDevice(%p): physical device was not enumerated through this HAL", (void *)pd);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkExtensionProperties *icd;
    uint32_t n_icd;
    VkResult r = icd_device_exts(inst, pd, &icd, &n_icd);
    if (r != VK_SUCCESS) return r;

    VkPhysicalDeviceProperties props;
    inst->GetPhysicalDeviceProperties(pd, &props);
    uint32_t api = api_min(inst->api_version, props.apiVersion);
    bool legacy = api < VK_API_VERSION_1_1;

    const char **names = malloc((ci->enabledExtensionCount + N_OF(anb_required_exts) + N_OF(ahb_required_exts) +
                                 N_OF(required_exts_1_0) + 2) * sizeof *names); /* + VK_KHR_maintenance2 (ASTC) */
    if (!names) { free(icd); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    uint32_t nn = 0;
    bool anb = false, ahb_ext = false;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        const char *e = ci->ppEnabledExtensionNames[i];
        if (!strcmp(e, VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME)) { anb = true; continue; }
        if (!strcmp(e, AHB_EXT)) { ahb_ext = true; continue; }
        if (!device_ext_exposed(e) || !nvhal_ext_in(icd, n_icd, e)) {
            const char *why = !nvhal_ext_in(icd, n_icd, e) ? anb_missing_hint(e)
                : device_ext_missing_lib(e) ? " (its vkCreateDevice needs an NVIDIA library absent from the guest; "
                                              "remedy: " MISSING_LIB_REMEDY ")"
                : device_ext_priority(e) ? " (NVIDIA refuses its HIGH/REALTIME priorities to non-root processes "
                                           "with VK_ERROR_NOT_PERMITTED; remedy: " PRIORITY_REMEDY ")"
                : nvhal_image_ext_hidden(e) ? " (its commands take VkImages this HAL does not translate for AHB "
                                              "images; tools/image-hidden-exts.txt)"
                : !nvhal_device_ext_known(e) ? " (unknown to this HAL's registry, so its VkImage parameters cannot "
                                               "be checked)"
                : " (it needs a surface/swapchain owned by the ICD; on Android the loader implements "
                  "VK_KHR_swapchain over VK_ANDROID_native_buffer)";
            NVHAL_ERR("vkCreateDevice: extension %s is not exposed by this driver%s -> VK_ERROR_EXTENSION_NOT_PRESENT",
                      e, why);
            free(names); free(icd);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        add_name(names, &nn, e);
    }
    bool ok = true;
    if (anb) {
        const char *miss = anb_missing(icd, n_icd, legacy);
        if (miss) {
            NVHAL_ERR("vkCreateDevice: VK_ANDROID_native_buffer requested, but the ICD lacks %s "
                      "(needed to import gralloc dma-bufs and sync_fds)%s -> VK_ERROR_EXTENSION_NOT_PRESENT",
                      miss, anb_missing_hint(miss));
            free(names); free(icd);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        for (size_t i = 0; i < N_OF(anb_required_exts); i++)
            ok &= add_prereq(names, &nn, anb_required_exts[i], "VK_ANDROID_native_buffer");
        if (legacy)
            for (size_t i = 0; i < N_OF(required_exts_1_0); i++)
                ok &= add_prereq(names, &nn, required_exts_1_0[i], "VK_ANDROID_native_buffer on a 1.0 device");
    }
    if (ahb_ext) {
        const char *miss = !nvhal_ahb_available ? "the AHardwareBuffer entry points" : ahb_missing(icd, n_icd, legacy);
        if (miss) {
            NVHAL_ERR("vkCreateDevice: " AHB_EXT " requested, but %s %s missing%s -> VK_ERROR_EXTENSION_NOT_PRESENT",
                      miss, nvhal_ahb_available ? "(ICD extension) is" : "are", anb_missing_hint(miss));
            free(names); free(icd);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        for (size_t i = 0; i < N_OF(ahb_required_exts); i++)
            ok &= add_prereq(names, &nn, ahb_required_exts[i], AHB_EXT);
        if (legacy)
            for (size_t i = 0; i < N_OF(required_exts_1_0); i++)
                ok &= add_prereq(names, &nn, required_exts_1_0[i], AHB_EXT " on a 1.0 device");
    }
    /* compressed-format emulation (astc.c: ASTC LDR, ETC2/EAC): decided per physical device; rewrites the enabled
     * features */
    struct nvhal_astc_ci_storage astc_st;
    VkDeviceCreateInfo aci;
    bool astc = false, maint2 = false;
    r = nvhal_astc_device_ci(inst, pd, api, ci, &aci, &astc_st, &astc, &maint2);
    if (r != VK_SUCCESS) {
        free(names);
        free(icd);
        return r;
    }
    if (maint2) {
        if (!nvhal_ext_in(icd, n_icd, "VK_KHR_maintenance2")) {
            NVHAL_ERR("vkCreateDevice: the compressed-format emulation on an API 1.0 device needs VK_KHR_maintenance2 "
                      "(image view usage, EXTENDED_USAGE), which the ICD lacks -> VK_ERROR_EXTENSION_NOT_PRESENT");
            ok = false;
        } else {
            ok &= add_prereq(names, &nn, "VK_KHR_maintenance2", "the compressed-format emulation on a 1.0 device");
        }
    }
    free(icd);
    if (!ok) {
        free(names);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    /* VkDeviceQueueGlobalPriorityCreateInfoKHR/EXT belongs to the hidden priority group, which no app
     * can have enabled (refused above): using an unenabled extension's struct is answered with
     * VK_ERROR_EXTENSION_NOT_PRESENT, never stripped silently. From Vulkan 1.4 the struct is core and
     * needs no extension; then the ICD's own answer (VK_ERROR_NOT_PERMITTED for HIGH/REALTIME as
     * non-root) is the spec-conformant one and is passed through, logged below. */
    if (api < VK_API_VERSION_1_4)
        for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
            const VkDeviceQueueGlobalPriorityCreateInfoKHR *gp = queue_global_priority(&ci->pQueueCreateInfos[i]);
            if (!gp) continue;
            NVHAL_ERR("vkCreateDevice: pQueueCreateInfos[%u] (family %u) chains VkDeviceQueueGlobalPriorityCreateInfoKHR "
                      "(priority %s (%d)) on an API %u.%u device, but VK_KHR_global_priority / VK_EXT_global_priority "
                      "are not exposed by this driver (NVIDIA refuses HIGH/REALTIME to non-root processes with "
                      "VK_ERROR_NOT_PERMITTED; remedy: " PRIORITY_REMEDY ") -> VK_ERROR_EXTENSION_NOT_PRESENT",
                      i, ci->pQueueCreateInfos[i].queueFamilyIndex, global_priority_name(gp->globalPriority),
                      (int)gp->globalPriority, VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api));
            free(names);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

    VkDeviceCreateInfo dci = aci;
    dci.enabledExtensionCount = nn;
    dci.ppEnabledExtensionNames = names;
    /* what the queue-timeline start needs to know (start_queue_timelines) */
    bool sync_sem_fd = name_in(names, nn, "VK_KHR_external_semaphore_fd");
    bool sync_fence_fd = name_in(names, nn, "VK_KHR_external_fence_fd");
    VkDevice dev = VK_NULL_HANDLE;
    r = inst->CreateDevice(pd, &dci, alloc, &dev);
    free(names);
    if (r != VK_SUCCESS) {
        char desc[1024];
        describe_device_ci(&dci, desc, sizeof desc);
        NVHAL_ERR("ICD vkCreateDevice(%u extensions%s%s, API %u.%u) -> %s; %s", nn, anb ? ", ANB prerequisites" : "",
                  ahb_ext ? ", AHB prerequisites" : "", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
                  NVHAL_VKR(r), desc);
        return r;
    }
    nvhal_dispatch_check(dev, NULL, "VkDevice");

    struct nvhal_device *d = calloc(1, sizeof *d);
    PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)inst->GetDeviceProcAddr(dev, "vkDestroyDevice");
    if (!d) {
        NVHAL_ERR("vkCreateDevice: out of host memory for the HAL's device state");
        if (destroy) destroy(dev, alloc);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    d->handle = dev;
    d->physical_device = pd;
    d->inst = inst;
    d->api_version = api;
    d->anb_enabled = anb;
    d->ahb_enabled = ahb_ext;
    inst->GetPhysicalDeviceMemoryProperties(pd, &d->mem_props);
    const char *missing = NULL;
#define DFN(field, name) do { d->field = (void *)inst->GetDeviceProcAddr(dev, name); } while (0)
#define DREQ(field, name) do { DFN(field, name); if (!d->field && !missing) missing = name; } while (0)
    DREQ(GetDeviceProcAddr, "vkGetDeviceProcAddr");
    DREQ(DestroyDevice, "vkDestroyDevice");
    DREQ(GetDeviceQueue, "vkGetDeviceQueue");
    DREQ(AllocateCommandBuffers, "vkAllocateCommandBuffers");
    DREQ(CreateImage, "vkCreateImage");
    DREQ(DestroyImage, "vkDestroyImage");
    DFN(GetDeviceQueue2, "vkGetDeviceQueue2");
    DFN(BindImageMemory2, "vkBindImageMemory2");
    DFN(BindImageMemory2KHR, "vkBindImageMemory2KHR");
    /* every translated entry point, on every device (NULL where the ICD has none) */
#define XL_FETCH(type, field, name) DFN(xl.field, name);
    NVHAL_XL_FNS(XL_FETCH)
#undef XL_FETCH
    /* the AHB-kind intercepts are also reachable through vkGetInstanceProcAddr on any device */
    DREQ(AllocateMemory, "vkAllocateMemory");
    DREQ(FreeMemory, "vkFreeMemory");
    DREQ(xl.BindImageMemory, "vkBindImageMemory");
    DREQ(xl.CreateBuffer, "vkCreateBuffer");
    DREQ(xl.CreateImageView, "vkCreateImageView");
    DREQ(xl.FreeCommandBuffers, "vkFreeCommandBuffers");
    DREQ(xl.DestroyCommandPool, "vkDestroyCommandPool");
    /* the compute-state commands and the ASTC decode's own entry points (NULL where the ICD has none) */
#define CS_FETCH(type, field, name) DFN(cs.field, name);
    NVHAL_CS_FNS(CS_FETCH)
    NVHAL_ASTC_FNS(CS_FETCH)
#undef CS_FETCH
    d->astc_pd = nvhal_astc_pd_get(inst, pd);
    if (anb || ahb_ext) {
        if (legacy) {
            DREQ(anb_BindImageMemory2, "vkBindImageMemory2KHR");
            DREQ(anb_GetImageMemoryRequirements2, "vkGetImageMemoryRequirements2KHR");
        } else {
            DREQ(anb_BindImageMemory2, "vkBindImageMemory2");
            DREQ(anb_GetImageMemoryRequirements2, "vkGetImageMemoryRequirements2");
        }
        DREQ(GetMemoryFdPropertiesKHR, "vkGetMemoryFdPropertiesKHR");
        if (!inst->GetPhysicalDeviceImageFormatProperties2 && !missing)
            missing = legacy ? "vkGetPhysicalDeviceImageFormatProperties2KHR"
                             : "vkGetPhysicalDeviceImageFormatProperties2";
    }
    if (anb) {
        DREQ(ImportSemaphoreFdKHR, "vkImportSemaphoreFdKHR");
        DREQ(ImportFenceFdKHR, "vkImportFenceFdKHR");
        DREQ(QueueSubmit, "vkQueueSubmit");
        DREQ(CreateFence, "vkCreateFence");
        DREQ(DestroyFence, "vkDestroyFence");
        DREQ(GetFenceStatus, "vkGetFenceStatus");
        DREQ(ResetFences, "vkResetFences");
        DREQ(WaitForFences, "vkWaitForFences");
        DREQ(GetFenceFdKHR, "vkGetFenceFdKHR");
    }
    if (ahb_ext) {
        if (!inst->GetPhysicalDeviceFormatProperties2 && !missing)
            missing = legacy ? "vkGetPhysicalDeviceFormatProperties2KHR" : "vkGetPhysicalDeviceFormatProperties2";
    }
#undef DREQ
#undef DFN
    ok = !missing;
    if (missing)
        NVHAL_ERR("vkCreateDevice: the ICD returned NULL for %s on a device it just created -> "
                  "VK_ERROR_INITIALIZATION_FAILED", missing);
    VkResult astc_r = VK_SUCCESS;
    if (ok && astc && (astc_r = nvhal_astc_device_init(d)) != VK_SUCCESS) ok = false;
    if (ok) nvhal_gt_device_init(d);
    if (ok) nvhal_exp_mem_init(d);
    /* D1: every queue's timeline is started before the app can use the device (AHB-CONTRACT.md §8) */
    VkResult start_r = VK_SUCCESS;
    uint32_t n_started = 0;
    double start_t0 = start_now_ms();
    if (ok && (start_r = start_queue_timelines(d, ci, sync_sem_fd, sync_fence_fd, alloc, &n_started)) != VK_SUCCESS)
        ok = false;
    double start_ms = start_now_ms() - start_t0;
    if (ok) {
        pthread_mutex_lock(&nvhal_lock);
        ok = nvhal_map_put(&devices, dev, d);
        pthread_mutex_unlock(&nvhal_lock);
        if (!ok) NVHAL_ERR("vkCreateDevice: out of host memory recording the device");
    }
    if (!ok) {
        nvhal_astc_device_teardown(d);
        nvhal_gt_device_teardown(d);
        nvhal_exp_memstats_teardown(d);
    nvhal_exp_mem_teardown(d);
        if (destroy) destroy(dev, alloc);
        free(d);
        return missing ? VK_ERROR_INITIALIZATION_FAILED : astc_r != VK_SUCCESS ? astc_r
             : start_r != VK_SUCCESS ? start_r : VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    NVHAL_TRACE("vkCreateDevice: VkDevice %p (%s), effective API %u.%u, %u extension(s) to the ICD, "
                "VK_ANDROID_native_buffer %s, " AHB_EXT " %s, ASTC LDR %s, ETC2/EAC %s, %u queue timeline(s) started (%s, "
                "%.3f ms)",
                (void *)dev, props.deviceName, VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), nn,
                anb ? "enabled" : "not requested", ahb_ext ? "enabled" : "not requested",
                d->astc_pd->state == NVHAL_ASTC_NATIVE ? "native" : d->astc_pd->state == NVHAL_ASTC_EMULATE ? "emulated"
                                                                                                          : "unavailable",
                d->astc_pd->etc2_state == NVHAL_ASTC_NATIVE    ? "native"
                : d->astc_pd->etc2_state == NVHAL_ASTC_EMULATE ? "emulated"
                                                               : "unavailable",
                n_started,
                sync_fence_fd ? "fence export" : sync_sem_fd ? "semaphore export" : "no sync_fd extension: none needed",
                start_ms);
    *pDevice = dev;
    return VK_SUCCESS;
}

void VKAPI_CALL nvhal_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc) {
    if (device == VK_NULL_HANDLE) return;
    pthread_mutex_lock(&nvhal_lock);
    struct nvhal_device *d = nvhal_map_del(&devices, device);
    if (d) nvhal_map_del_val(&queues, d);
    pthread_mutex_unlock(&nvhal_lock);
    if (!d) {
        NVHAL_ERR("vkDestroyDevice(%p): not a device created through this HAL; not destroyed", (void *)device);
        return;
    }
    nvhal_anb_device_teardown(d, alloc);
    nvhal_ahb_device_teardown(d, alloc);
    nvhal_astc_device_teardown(d);
    nvhal_loader_pd_teardown(d);
    nvhal_cmd_device_forget(d);
    nvhal_gt_device_teardown(d);
    nvhal_exp_memstats_teardown(d);
    nvhal_exp_mem_teardown(d);
    d->DestroyDevice(device, alloc);
    free(d);
}

static void record_queue(struct nvhal_device *d, VkQueue q) {
    nvhal_dispatch_check(q, d->handle, "VkQueue");
    pthread_mutex_lock(&nvhal_lock);
    if (!nvhal_map_put(&queues, q, d))
        NVHAL_ERR("out of host memory recording VkQueue %p; vkQueueSignalReleaseImageANDROID on it will fail",
                  (void *)q);
    pthread_mutex_unlock(&nvhal_lock);
}

void VKAPI_CALL nvhal_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *pQueue) {
    struct nvhal_device *d = nvhal_device_get(device);
    if (!d) {
        NVHAL_ERR("vkGetDeviceQueue(%p): unknown device", (void *)device);
        *pQueue = VK_NULL_HANDLE;
        return;
    }
    d->GetDeviceQueue(device, family, index, pQueue);
    if (*pQueue != VK_NULL_HANDLE) record_queue(d, *pQueue);
}

void VKAPI_CALL nvhal_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *pQueue) {
    struct nvhal_device *d = nvhal_device_get(device);
    if (!d || !d->GetDeviceQueue2) {
        NVHAL_ERR("vkGetDeviceQueue2(%p): %s", (void *)device, !d ? "unknown device" : "ICD has no vkGetDeviceQueue2");
        *pQueue = VK_NULL_HANDLE;
        return;
    }
    d->GetDeviceQueue2(device, info, pQueue);
    if (*pQueue != VK_NULL_HANDLE) record_queue(d, *pQueue);
}

VkResult VKAPI_CALL nvhal_AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *info,
                                                 VkCommandBuffer *pCmd) {
    struct nvhal_device *d = nvhal_device_get(device);
    if (!d) {
        NVHAL_ERR("vkAllocateCommandBuffers(%p): unknown device", (void *)device);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult r = d->AllocateCommandBuffers(device, info, pCmd);
    if (r == VK_SUCCESS) {
        for (uint32_t i = 0; i < info->commandBufferCount; i++)
            nvhal_dispatch_check(pCmd[i], device, "VkCommandBuffer");
        nvhal_cmd_track(d, info->commandPool, pCmd, info->commandBufferCount);
    }
    return r;
}

/* Hide VkPhysicalDevicePresentationPropertiesANDROID from the ICD and answer it ourselves:
 * no shared presentable images (HAL-CONTRACT 4.4). */
static void props2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p, bool khr) {
    struct nvhal_instance *inst = nvhal_physdev_instance(pd);
    if (!inst) {
        NVHAL_ERR("vkGetPhysicalDeviceProperties2(%p): physical device was not enumerated through this HAL",
                  (void *)pd);
        return;
    }
    PFN_vkGetPhysicalDeviceProperties2 f = khr ? inst->GetPhysicalDeviceProperties2KHR
                                               : inst->GetPhysicalDeviceProperties2;
    VkBaseOutStructure *prev = (VkBaseOutStructure *)p, *pres = NULL;
    for (VkBaseOutStructure *s = p->pNext; s; prev = s, s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENTATION_PROPERTIES_ANDROID) { pres = s; break; }
    if (!pres) { f(pd, p); return; }
    prev->pNext = pres->pNext;
    ((VkPhysicalDevicePresentationPropertiesANDROID *)pres)->sharedImage = VK_FALSE;
    f(pd, p);
    prev->pNext = pres;
}
void VKAPI_CALL nvhal_GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) {
    props2(pd, p, false);
}
void VKAPI_CALL nvhal_GetPhysicalDeviceProperties2KHR(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) {
    props2(pd, p, true);
}
