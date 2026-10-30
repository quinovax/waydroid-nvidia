/* anb-roundtrip: host test of vulkan.nvidia.so's VK_ANDROID_native_buffer path on the real GPU.
 *
 * Loads the HAL like AOSP libvulkan (dlopen, dlsym HMI, open "vk0", GetInstanceProcAddr) and:
 *   1. checks the instance/device extension filtering and VkPhysicalDevicePresentationPropertiesANDROID;
 *   2. allocates an exportable 256x256 R8G8B8A8_UNORM DRM-modifier image (image A) straight
 *      through the HAL's pass-through, exports its dma-buf and plane layout, wraps them in a
 *      synthetic minigbm cros_gralloc_handle, and imports that with vkCreateImage +
 *      VkNativeBufferANDROID (image B, the swapchain-image path);
 *   3. clears B to a known colour and uploads a per-pixel pattern into B, reading both back
 *      through A (same memory): verifies the import shares the memory AND the tiled layout;
 *   4. vkGetSwapchainGrallocUsage[2]ANDROID, vkAcquireImageANDROID with fd -1 and with a real
 *      sync_fd, vkQueueSignalReleaseImageANDROID (returned fd must be a sync_file that poll()s
 *      signalled, and must still be unsignalled while the waited-on work is running);
 *   5. an image created with VkImageSwapchainCreateInfoKHR (image C, the HAL's deferred
 *      "swapchain-bound" wrapper) bound with vkBindImageMemory2 + VkBindImageMemorySwapchainInfoKHR +
 *      the VkNativeBufferANDROID the Android loader prepends: clear/upload via C, read back via A;
 *      its negative cases (memory != NULL, no native buffer, vkBindImageMemory, rebind);
 *   6. negative cases (bad handle, bind with ANB chained on a plain image) and fd-leak accounting.
 * Exit 0 only when every check passed. Every VkResult is printed.
 *
 * --allow-synchronous-submit accepts implementations that complete work before
 * returning from submit/release. GPU-written completion markers remain mandatory.
 * --bounded reduces repeated fd-ownership checks to two rounds of four cycles.
 * usage: anb-roundtrip HAL.so [--api 1.0|1.1] [--linear] [--allow-synchronous-submit] [--bounded] */
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/sync_file.h>
#include <drm/drm_fourcc.h>
#include <vulkan/vulkan.h>
#include <hardware/hwvulkan.h>
#include <vulkan/vk_android_native_buffer.h>
#include <cros_gralloc_handle.h>

static int failures;
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkDevice dev;
static int api10;

#define VKR(call) vkr(#call, (call))
static VkResult vkr(const char *what, VkResult r) {
    printf("  %-72.72s -> %d%s\n", what, r, r == VK_SUCCESS ? "" : " (!)");
    return r;
}
#define MUST(call) do { if (VKR(call) != VK_SUCCESS) { printf("FATAL\n"); exit(2); } } while (0)
static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void check(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  [%s] ", ok ? "PASS" : "FAIL");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok) failures++;
}

static void *ip(const char *name) {
    void *p = (void *)gipa(inst, name);
    if (!p) { printf("FATAL: GIPA(instance, %s) = NULL\n", name); exit(2); }
    return p;
}
static void *dp(const char *name) {
    void *p = (void *)gdpa(dev, name);
    if (!p) { printf("FATAL: GDPA(device, %s) = NULL\n", name); exit(2); }
    return p;
}
static const char *v11(const char *core, const char *khr) { return api10 ? khr : core; }

static int count_fds(void) {
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    for (struct dirent *e; d && (e = readdir(d));) n += e->d_name[0] != '.';
    if (d) closedir(d);
    return n - 1; /* the DIR's own fd */
}

/* fd snapshot: the targets of every open fd, to name what appeared between two points */
#define MAXFD 1024
static char *fdsnap[MAXFD];
static void fd_snapshot(void) {
    for (int i = 0; i < MAXFD; i++) { free(fdsnap[i]); fdsnap[i] = NULL; }
    for (int i = 0; i < MAXFD; i++) {
        char p[64], t[256];
        snprintf(p, sizeof p, "/proc/self/fd/%d", i);
        ssize_t l = readlink(p, t, sizeof t - 1);
        if (l > 0) { t[l] = 0; fdsnap[i] = strdup(t); }
    }
}
static void fd_diff(const char *what) {
    for (int i = 0; i < MAXFD; i++) {
        char p[64], t[256];
        snprintf(p, sizeof p, "/proc/self/fd/%d", i);
        ssize_t l = readlink(p, t, sizeof t - 1);
        if (l > 0) t[l] = 0;
        if (l > 0 && (!fdsnap[i] || strcmp(fdsnap[i], t))) printf("    %s: new fd %d -> %s\n", what, i, t);
        if (l <= 0 && fdsnap[i]) printf("    %s: closed fd %d (was %s)\n", what, i, fdsnap[i]);
    }
}

static int count_fds_matching(const char *prefix) {
    int n = 0;
    for (int i = 0; i < MAXFD; i++) {
        char p[64], t[256];
        snprintf(p, sizeof p, "/proc/self/fd/%d", i);
        ssize_t l = readlink(p, t, sizeof t - 1);
        if (l > 0) { t[l] = 0; n += !strncmp(t, prefix, strlen(prefix)); }
    }
    return n;
}

static int sync_file_info(int fd, struct sync_file_info *info) {
    memset(info, 0, sizeof *info);
    return ioctl(fd, SYNC_IOC_FILE_INFO, info);
}
static void fd_target(int fd, char *buf, size_t n) {
    char p[64];
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    ssize_t l = readlink(p, buf, n - 1);
    buf[l > 0 ? l : 0] = 0;
}
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* device functions */
static PFN_vkGetDeviceQueue p_GetDeviceQueue;
static PFN_vkCreateImage p_CreateImage;
static PFN_vkDestroyImage p_DestroyImage;
static PFN_vkAllocateMemory p_AllocateMemory;
static PFN_vkFreeMemory p_FreeMemory;
static PFN_vkBindImageMemory p_BindImageMemory;
static PFN_vkBindImageMemory2 p_BindImageMemory2;
static PFN_vkGetImageMemoryRequirements2 p_GetImageMemoryRequirements2;
static PFN_vkGetMemoryFdKHR p_GetMemoryFdKHR;
static PFN_vkGetImageDrmFormatModifierPropertiesEXT p_GetImageDrmFormatModifierPropertiesEXT;
static PFN_vkGetImageSubresourceLayout p_GetImageSubresourceLayout;
static PFN_vkCreateBuffer p_CreateBuffer;
static PFN_vkDestroyBuffer p_DestroyBuffer;
static PFN_vkGetBufferMemoryRequirements p_GetBufferMemoryRequirements;
static PFN_vkBindBufferMemory p_BindBufferMemory;
static PFN_vkMapMemory p_MapMemory;
static PFN_vkCreateCommandPool p_CreateCommandPool;
static PFN_vkDestroyCommandPool p_DestroyCommandPool;
static PFN_vkAllocateCommandBuffers p_AllocateCommandBuffers;
static PFN_vkBeginCommandBuffer p_BeginCommandBuffer;
static PFN_vkEndCommandBuffer p_EndCommandBuffer;
static PFN_vkResetCommandBuffer p_ResetCommandBuffer;
static PFN_vkCmdPipelineBarrier p_CmdPipelineBarrier;
static PFN_vkCmdClearColorImage p_CmdClearColorImage;
static PFN_vkCmdCopyImageToBuffer p_CmdCopyImageToBuffer;
static PFN_vkCmdCopyBufferToImage p_CmdCopyBufferToImage;
static PFN_vkCmdFillBuffer p_CmdFillBuffer;
static PFN_vkQueueSubmit p_QueueSubmit;
static PFN_vkQueueWaitIdle p_QueueWaitIdle;
static PFN_vkCreateFence p_CreateFence;
static PFN_vkDestroyFence p_DestroyFence;
static PFN_vkWaitForFences p_WaitForFences;
static PFN_vkResetFences p_ResetFences;
static PFN_vkGetFenceStatus p_GetFenceStatus;
static PFN_vkGetFenceFdKHR p_GetFenceFdKHR;
static PFN_vkCreateSemaphore p_CreateSemaphore;
static PFN_vkDestroySemaphore p_DestroySemaphore;
static PFN_vkDestroyDevice p_DestroyDevice;
static PFN_vkGetSwapchainGrallocUsageANDROID p_GetSwapchainGrallocUsageANDROID;
static PFN_vkGetSwapchainGrallocUsage2ANDROID p_GetSwapchainGrallocUsage2ANDROID;
static PFN_vkAcquireImageANDROID p_AcquireImageANDROID;
static PFN_vkQueueSignalReleaseImageANDROID p_QueueSignalReleaseImageANDROID;

static VkPhysicalDeviceMemoryProperties memprops;
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    printf("FATAL: no memory type in 0x%x with flags 0x%x\n", bits, want);
    exit(2);
}

static VkQueue queue;
static VkCommandPool pool;
static VkCommandBuffer cmd;

static void begin(void) {
    MUST(p_ResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    MUST(p_BeginCommandBuffer(cmd, &bi));
}
static void barrier(VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    p_CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL,
                         0, NULL, 1, &b);
}
static void submit_wait(const char *what, uint32_t nwait, const VkSemaphore *waits, uint32_t nsig,
                        const VkSemaphore *sigs, int with_cmd) {
    MUST(p_EndCommandBuffer(cmd));
    VkPipelineStageFlags st[4] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = nwait, .pWaitSemaphores = waits,
                       .pWaitDstStageMask = st, .commandBufferCount = with_cmd ? 1 : 0, .pCommandBuffers = &cmd,
                       .signalSemaphoreCount = nsig, .pSignalSemaphores = sigs};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f;
    MUST(p_CreateFence(dev, &fci, NULL, &f));
    printf("  %s:\n", what);
    MUST(p_QueueSubmit(queue, 1, &si, f));
    MUST(p_WaitForFences(dev, 1, &f, VK_TRUE, 10ull * 1000000000ull));
    p_DestroyFence(dev, f, NULL);
}

struct hostbuf { VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; };
static struct hostbuf hostbuf(VkDeviceSize size, VkBufferUsageFlags usage) {
    struct hostbuf h;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    MUST(p_CreateBuffer(dev, &bci, NULL, &h.buf));
    VkMemoryRequirements mr;
    p_GetBufferMemoryRequirements(dev, h.buf, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    MUST(p_AllocateMemory(dev, &mai, NULL, &h.mem));
    MUST(p_BindBufferMemory(dev, h.buf, h.mem, 0));
    MUST(p_MapMemory(dev, h.mem, 0, VK_WHOLE_SIZE, 0, (void **)&h.ptr));
    return h;
}

#define W 256u
#define H 256u

int main(int argc, char **argv) {
    const char *halpath = argc > 1 ? argv[1] : "out/host/vulkan.nvidia.so";
    int linear = 0;
    int allow_synchronous_submit = 0, bounded = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--api") && i + 1 < argc) api10 = !strcmp(argv[++i], "1.0");
        else if (!strcmp(argv[i], "--linear")) linear = 1;
        else if (!strcmp(argv[i], "--allow-synchronous-submit")) allow_synchronous_submit = 1;
        else if (!strcmp(argv[i], "--bounded")) bounded = 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("anb-roundtrip: HAL %s, instance apiVersion %s, modifiers %s\n", halpath, api10 ? "1.0" : "1.1",
           linear ? "LINEAR only" : "all the device offers");

    /* ---- load the HAL the way libvulkan does ---- */
    void *so = dlopen(halpath, RTLD_NOW | RTLD_LOCAL);
    if (!so) { printf("FATAL: dlopen %s: %s\n", halpath, dlerror()); return 2; }
    hwvulkan_module_t *hmi = dlsym(so, HAL_MODULE_INFO_SYM_AS_STR);
    if (!hmi || strcmp(hmi->common.id, HWVULKAN_HARDWARE_MODULE_ID)) { printf("FATAL: no HMI / bad id\n"); return 2; }
    hmi->common.dso = so;
    hwvulkan_device_t *hal = NULL;
    int rc = hmi->common.methods->open(&hmi->common, HWVULKAN_DEVICE_0, (hw_device_t **)&hal);
    check(rc == 0 && hal, "open(\"vk0\") -> %d", rc);
    if (rc) return 2;
    gipa = hal->GetInstanceProcAddr;
    check(hmi->common.methods->open(&hmi->common, "vk1", (hw_device_t **)&hal) != 0, "open(\"vk1\") is refused");
    check(gipa(NULL, "vkCreateInstance") == (PFN_vkVoidFunction)hal->CreateInstance,
          "GIPA(NULL, vkCreateInstance) == hwvulkan_device_t.CreateInstance");
    check(gipa(NULL, "vkEnumeratePhysicalDevices") == NULL, "GIPA(NULL, non-global) == NULL");

    /* ---- instance extensions ---- */
    uint32_t n = 0;
    MUST(hal->EnumerateInstanceExtensionProperties(NULL, &n, NULL));
    VkExtensionProperties *ie = calloc(n, sizeof *ie);
    MUST(hal->EnumerateInstanceExtensionProperties(NULL, &n, ie));
    int hidden_found = 0, has_pdp2 = 0;
    for (uint32_t i = 0; i < n; i++) {
        const char *e = ie[i].extensionName;
        hidden_found |= !strcmp(e, "VK_KHR_surface") || !strcmp(e, "VK_KHR_xcb_surface") ||
                        !strcmp(e, "VK_KHR_wayland_surface") || !strcmp(e, "VK_KHR_display") ||
                        !strcmp(e, "VK_EXT_acquire_drm_display") || !strcmp(e, "VK_EXT_swapchain_colorspace");
        has_pdp2 |= !strcmp(e, "VK_KHR_get_physical_device_properties2");
    }
    printf("  instance extensions exposed: %u:", n);
    for (uint32_t i = 0; i < n; i++) printf(" %s", ie[i].extensionName);
    printf("\n");
    check(!hidden_found && has_pdp2, "WSI instance extensions hidden, VK_KHR_get_physical_device_properties2 kept");
    {
        uint32_t two = 2;
        VkExtensionProperties p2[2];
        check(hal->EnumerateInstanceExtensionProperties(NULL, &two, p2) == VK_INCOMPLETE && two == 2,
              "short instance-extension array -> VK_INCOMPLETE");
    }

    /* ---- instance ---- */
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "anb-roundtrip",
                            .apiVersion = api10 ? VK_API_VERSION_1_0 : VK_API_VERSION_1_1};
    const char *iexts[] = {"VK_KHR_get_physical_device_properties2"};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai,
                                .enabledExtensionCount = api10 ? 1 : 0, .ppEnabledExtensionNames = iexts};
    {
        const char *bad[] = {"VK_KHR_xcb_surface"};
        VkInstanceCreateInfo b = ici;
        b.enabledExtensionCount = 1;
        b.ppEnabledExtensionNames = bad;
        VkInstance x;
        check(VKR(hal->CreateInstance(&b, NULL, &x)) == VK_ERROR_EXTENSION_NOT_PRESENT,
              "vkCreateInstance with a hidden WSI extension -> VK_ERROR_EXTENSION_NOT_PRESENT");
    }
    MUST(hal->CreateInstance(&ici, NULL, &inst));
    check(*(uintptr_t *)inst == HWVULKAN_DISPATCH_MAGIC, "VkInstance dispatch word == 0x%x", HWVULKAN_DISPATCH_MAGIC);
    gdpa = ip("vkGetDeviceProcAddr");
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = ip("vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = ip("vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 =
        ip(v11("vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceProperties2KHR"));
    PFN_vkGetPhysicalDeviceFormatProperties2 GetPhysicalDeviceFormatProperties2 =
        ip(v11("vkGetPhysicalDeviceFormatProperties2", "vkGetPhysicalDeviceFormatProperties2KHR"));
    PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2 =
        ip(v11("vkGetPhysicalDeviceImageFormatProperties2", "vkGetPhysicalDeviceImageFormatProperties2KHR"));
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties =
        ip("vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = ip("vkGetPhysicalDeviceMemoryProperties");
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = ip("vkEnumerateDeviceExtensionProperties");
    PFN_vkCreateDevice CreateDevice = ip("vkCreateDevice");
    PFN_vkDestroyInstance DestroyInstance = ip("vkDestroyInstance");

    uint32_t npd = 0;
    MUST(EnumeratePhysicalDevices(inst, &npd, NULL));
    VkPhysicalDevice *pds = calloc(npd, sizeof *pds);
    MUST(EnumeratePhysicalDevices(inst, &npd, pds));
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props;
    for (uint32_t i = 0; i < npd && !pd; i++) {
        GetPhysicalDeviceProperties(pds[i], &props);
        if (props.vendorID == 0x10de) pd = pds[i]; /* by vendor ID, never by node path */
    }
    if (!pd) { printf("FATAL: no NVIDIA (0x10de) physical device\n"); return 2; }
    printf("  device: %s api %u.%u.%u\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
    GetPhysicalDeviceMemoryProperties(pd, &memprops);
    /* NVIDIA driver defects with imported SYNC_FD payloads make the HAL's vkAcquireImageANDROID wait for the native
     * fence on the CPU (src/device.c, acquire_wait_flags): given a fence on branches 550 to 560, given a semaphore
     * on 580. The acquired objects are then already signalled when the call returns. */
    int acquire_wait_fence = 0, acquire_wait_semaphore = 0;
    if (props.apiVersion >= VK_API_VERSION_1_2) {
        VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
        GetPhysicalDeviceProperties2(pd, &p2);
        uint32_t branch = props.driverVersion >> 22;
        if (drv.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY) {
            acquire_wait_fence = branch < 565;
            acquire_wait_semaphore = branch == 580;
        }
    }
    if (acquire_wait_fence || acquire_wait_semaphore)
        printf("  NVIDIA driver branch %u: vkAcquireImageANDROID waits for the native fence when given a %s\n",
               props.driverVersion >> 22, acquire_wait_fence ? "fence" : "semaphore");

    /* ---- VkPhysicalDevicePresentationPropertiesANDROID ---- */
    {
        VkPhysicalDeviceIDProperties idp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDevicePresentationPropertiesANDROID pp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENTATION_PROPERTIES_ANDROID, .pNext = &idp, .sharedImage = 7};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pp};
        GetPhysicalDeviceProperties2(pd, &p2);
        int uuid_set = 0;
        for (unsigned i = 0; i < VK_UUID_SIZE; i++) uuid_set |= idp.deviceUUID[i];
        check(pp.sharedImage == VK_FALSE && p2.pNext == &pp && pp.pNext == &idp && uuid_set &&
              !strcmp(p2.properties.deviceName, props.deviceName),
              "PresentationPropertiesANDROID: sharedImage=%u, chain restored, structs after it still filled",
              pp.sharedImage);
    }

    /* ---- device extensions ---- */
    MUST(EnumerateDeviceExtensionProperties(pd, NULL, &n, NULL));
    VkExtensionProperties *de = calloc(n, sizeof *de);
    MUST(EnumerateDeviceExtensionProperties(pd, NULL, &n, de));
    int anb_spec = -1, swapchain = 0, dmabuf = 0, lowlat2 = 0, prio = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(de[i].extensionName, VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME)) anb_spec = (int)de[i].specVersion;
        swapchain |= !strcmp(de[i].extensionName, "VK_KHR_swapchain") ||
                     !strcmp(de[i].extensionName, "VK_KHR_present_wait") ||
                     !strcmp(de[i].extensionName, "VK_EXT_swapchain_maintenance1");
        lowlat2 |= !strcmp(de[i].extensionName, "VK_NV_low_latency2");
        prio |= !strcmp(de[i].extensionName, "VK_EXT_global_priority") ||
                !strcmp(de[i].extensionName, "VK_KHR_global_priority") ||
                !strcmp(de[i].extensionName, "VK_EXT_global_priority_query");
        dmabuf += !strcmp(de[i].extensionName, "VK_EXT_external_memory_dma_buf") ||
                  !strcmp(de[i].extensionName, "VK_EXT_image_drm_format_modifier") ||
                  !strcmp(de[i].extensionName, "VK_KHR_external_fence_fd") ||
                  !strcmp(de[i].extensionName, "VK_KHR_external_semaphore_fd") ||
                  !strcmp(de[i].extensionName, "VK_EXT_queue_family_foreign") ||
                  !strcmp(de[i].extensionName, "VK_KHR_external_memory_fd");
    }
    printf("  device extensions exposed: %u\n", n);
    check(anb_spec == 8 && !swapchain && !lowlat2 && !prio && dmabuf == 6,
          "device: VK_ANDROID_native_buffer spec %d, swapchain/present and global-priority extensions hidden, "
          "6 external-handle extensions kept", anb_spec);

    /* Which ICD is behind the HAL decides one expectation. Vulkan allows a SYNC_FD export of an
     * already-signalled payload to return -1 (vkGetFenceFdKHR: "If ... handleType is
     * VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT and the fence is signaled at the time vkGetFenceFdKHR is
     * called, pFd may return the value -1"), and Android allows the release to pass it on ("If the wait
     * semaphores (if any) already signaled, and queue is already idle, the driver can set *pNativeFenceFd to
     * -1", source.android.com/docs/core/graphics/implement-vulkan). The source NVK/RM ICD does that; the
     * proprietary ICD never does, and the stock HAL's D1 timeline start treats -1 as a failure, so stock
     * expectations stay strict. Whenever the awaited work or earlier queue work is still pending, a real
     * pending fd stays mandatory for both (the "behind busy GPU work" checks below). */
    int accept_signalled_minus1 = 0;
    {
        int drvprops = VK_API_VERSION_MINOR(props.apiVersion) >= 2;
        for (uint32_t i = 0; i < n; i++) drvprops |= !strcmp(de[i].extensionName, "VK_KHR_driver_properties");
        VkPhysicalDeviceDriverProperties dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        if (drvprops) {
            VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp};
            GetPhysicalDeviceProperties2(pd, &p2);
        }
        accept_signalled_minus1 = drvprops && dp.driverID == VK_DRIVER_ID_MESA_NVK;
        printf("  driver: id %d '%s' (%s): an already-signalled release may be fd -1: %s\n", (int)dp.driverID,
               dp.driverName, drvprops ? "VkPhysicalDeviceDriverProperties" : "no driver properties",
               accept_signalled_minus1 ? "yes (Mesa NVK)" : "no (strict)");
    }

    /* ---- device: ANB plus what this test itself calls (the HAL de-duplicates) ---- */
    uint32_t nq = 0;
    GetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq, sizeof *qf);
    GetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nq && qfi == UINT32_MAX; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) qfi = i;
    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfi,
                                   .queueCount = 1, .pQueuePriorities = &qprio};
    const char *dexts[] = {VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME, "VK_KHR_external_memory_fd",
                           "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier",
                           "VK_KHR_external_fence_fd", "VK_KHR_external_semaphore_fd",
                           /* needed by the test's own calls on a 1.0 device */
                           "VK_KHR_get_memory_requirements2", "VK_KHR_dedicated_allocation", "VK_KHR_bind_memory2",
                           "VK_KHR_external_memory", "VK_KHR_external_fence", "VK_KHR_external_semaphore",
                           "VK_KHR_image_format_list", "VK_KHR_maintenance1", "VK_KHR_sampler_ycbcr_conversion"};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = api10 ? 15 : 6,
                              .ppEnabledExtensionNames = dexts};
    {
        const char *bad[] = {"VK_KHR_swapchain"};
        VkDeviceCreateInfo b = dci;
        b.enabledExtensionCount = 1;
        b.ppEnabledExtensionNames = bad;
        VkDevice x;
        check(VKR(CreateDevice(pd, &b, NULL, &x)) == VK_ERROR_EXTENSION_NOT_PRESENT,
              "vkCreateDevice with VK_KHR_swapchain -> VK_ERROR_EXTENSION_NOT_PRESENT");
    }
    {
        /* the global-priority group is hidden (HIGH/REALTIME refused to non-root): its struct is refused,
         * never stripped, on a pre-1.4 device */
        VkDeviceQueueGlobalPriorityCreateInfoKHR gp = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR,
                                                       .globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR};
        VkDeviceQueueCreateInfo q = qci;
        q.pNext = &gp;
        VkDeviceCreateInfo b = dci;
        b.pQueueCreateInfos = &q;
        VkDevice x;
        check(VKR(CreateDevice(pd, &b, NULL, &x)) == VK_ERROR_EXTENSION_NOT_PRESENT,
              "vkCreateDevice with VkDeviceQueueGlobalPriorityCreateInfoKHR(HIGH) -> VK_ERROR_EXTENSION_NOT_PRESENT");
        const char *gpx[] = {"VK_KHR_global_priority"};
        b = dci;
        b.enabledExtensionCount = 1;
        b.ppEnabledExtensionNames = gpx;
        check(VKR(CreateDevice(pd, &b, NULL, &x)) == VK_ERROR_EXTENSION_NOT_PRESENT,
              "vkCreateDevice enabling VK_KHR_global_priority -> VK_ERROR_EXTENSION_NOT_PRESENT");
    }
    MUST(CreateDevice(pd, &dci, NULL, &dev));
    check(*(uintptr_t *)dev == HWVULKAN_DISPATCH_MAGIC, "VkDevice dispatch word == 0x%x", HWVULKAN_DISPATCH_MAGIC);

#define D(name) p_##name = dp("vk" #name)
    D(GetDeviceQueue); D(CreateImage); D(DestroyImage); D(AllocateMemory); D(FreeMemory); D(BindImageMemory);
    D(GetMemoryFdKHR); D(GetImageDrmFormatModifierPropertiesEXT); D(GetImageSubresourceLayout); D(CreateBuffer);
    D(DestroyBuffer); D(GetBufferMemoryRequirements); D(BindBufferMemory); D(MapMemory); D(CreateCommandPool);
    D(DestroyCommandPool); D(AllocateCommandBuffers); D(BeginCommandBuffer); D(EndCommandBuffer);
    D(ResetCommandBuffer); D(CmdPipelineBarrier); D(CmdClearColorImage); D(CmdCopyImageToBuffer);
    D(CmdCopyBufferToImage); D(CmdFillBuffer); D(QueueSubmit); D(QueueWaitIdle); D(CreateFence); D(DestroyFence); D(WaitForFences);
    D(ResetFences); D(GetFenceStatus); D(GetFenceFdKHR); D(CreateSemaphore); D(DestroySemaphore); D(DestroyDevice);
    D(GetSwapchainGrallocUsageANDROID); D(GetSwapchainGrallocUsage2ANDROID); D(AcquireImageANDROID);
    D(QueueSignalReleaseImageANDROID);
#undef D
    p_BindImageMemory2 = dp(v11("vkBindImageMemory2", "vkBindImageMemory2KHR"));
    p_GetImageMemoryRequirements2 = dp(v11("vkGetImageMemoryRequirements2", "vkGetImageMemoryRequirements2KHR"));
    check(gdpa(dev, "vkCreateDevice") == NULL, "GDPA(device, vkCreateDevice) == NULL (instance-level)");

    p_GetDeviceQueue(dev, qfi, 0, &queue);
    VkQueue q2;
    p_GetDeviceQueue(dev, qfi, 0, &q2);
    check(queue == q2 && *(uintptr_t *)queue == HWVULKAN_DISPATCH_MAGIC, "VkQueue dispatch word == magic");
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfi};
    MUST(p_CreateCommandPool(dev, &cpci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    MUST(p_AllocateCommandBuffers(dev, &cbai, &cmd));
    check(*(uintptr_t *)cmd == HWVULKAN_DISPATCH_MAGIC, "VkCommandBuffer dispatch word == magic");

    /* ---- gralloc usage ---- */
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    uint64_t cons = 0, prod = 0;
    VKR(p_GetSwapchainGrallocUsage2ANDROID(dev, VK_FORMAT_R8G8B8A8_UNORM, usage, 0, &cons, &prod));
    check(prod == (1ull << 9) && cons == ((1ull << 8) | (1ull << 11)),
          "GrallocUsage2: producer 0x%llx (GPU_RENDER_TARGET), consumer 0x%llx (GPU_TEXTURE|HWCOMPOSER)",
          (unsigned long long)prod, (unsigned long long)cons);
    int g0 = 0;
    VKR(p_GetSwapchainGrallocUsageANDROID(dev, VK_FORMAT_R8G8B8A8_UNORM, usage, &g0));
    check(g0 == (int)(0x100 | 0x200 | 0x800), "GrallocUsage (gralloc0): 0x%x (HW_TEXTURE|HW_RENDER|HW_COMPOSER)", g0);
    check(VKR(p_GetSwapchainGrallocUsage2ANDROID(dev, VK_FORMAT_R8G8B8A8_UNORM,
                                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &cons, &prod)) ==
              VK_ERROR_FORMAT_NOT_SUPPORTED, "GrallocUsage2 with depth-stencil usage -> VK_ERROR_FORMAT_NOT_SUPPORTED");
    check(VKR(p_GetSwapchainGrallocUsage2ANDROID(dev, VK_FORMAT_R8G8B8A8_UNORM, usage,
                                                 VK_SWAPCHAIN_IMAGE_USAGE_SHARED_BIT_ANDROID, &cons, &prod)) ==
              VK_ERROR_FORMAT_NOT_SUPPORTED, "GrallocUsage2 with SHARED_BIT -> VK_ERROR_FORMAT_NOT_SUPPORTED");

    /* ---- image A: exportable DRM-modifier image allocated through the HAL's pass-through ---- */
    VkDrmFormatModifierPropertiesListEXT ml = {.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 fp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &ml};
    GetPhysicalDeviceFormatProperties2(pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    VkDrmFormatModifierPropertiesEXT *mp = calloc(ml.drmFormatModifierCount, sizeof *mp);
    ml.pDrmFormatModifierProperties = mp;
    GetPhysicalDeviceFormatProperties2(pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    uint64_t *mods = calloc(ml.drmFormatModifierCount, sizeof *mods);
    uint32_t nmods = 0;
    for (uint32_t i = 0; i < ml.drmFormatModifierCount; i++) {
        if (linear && mp[i].drmFormatModifier != DRM_FORMAT_MOD_LINEAR) continue;
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
            .drmFormatModifier = mp[i].drmFormatModifier, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkPhysicalDeviceExternalImageFormatInfo ei = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .pNext = &mi, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkPhysicalDeviceImageFormatInfo2 ifi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &ei, .format = VK_FORMAT_R8G8B8A8_UNORM, .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, .usage = usage};
        VkExternalImageFormatProperties ep = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 ifp = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &ep};
        if (GetPhysicalDeviceImageFormatProperties2(pd, &ifi, &ifp) == VK_SUCCESS &&
            (ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
            mods[nmods++] = mp[i].drmFormatModifier;
    }
    printf("  R8G8B8A8_UNORM: %u modifiers reported, %u usable for export with usage 0x%x\n",
           ml.drmFormatModifierCount, nmods, usage);
    if (!nmods) { printf("FATAL: no modifier\n"); return 2; }

    VkImageDrmFormatModifierListCreateInfoEXT mlci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = nmods, .pDrmFormatModifiers = mods};
    VkExternalMemoryImageCreateInfo emi = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = &mlci,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo aci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi, .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                             .usage = usage,
                             /* PREINITIALIZED (valid for DRM-modifier tiling): A's first transition must
                              * keep the bytes written through B */
                             .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED};
    VkImage imgA;
    MUST(p_CreateImage(dev, &aci, NULL, &imgA));
    VkMemoryDedicatedRequirements dr = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 mr2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dr};
    VkImageMemoryRequirementsInfo2 mri = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = imgA};
    p_GetImageMemoryRequirements2(dev, &mri, &mr2);
    VkMemoryDedicatedAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = imgA};
    VkExportMemoryAllocateInfo exi = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &dai,
                                      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo amai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &exi,
        .allocationSize = mr2.memoryRequirements.size,
        .memoryTypeIndex = mem_type(mr2.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory memA;
    MUST(p_AllocateMemory(dev, &amai, NULL, &memA));
    MUST(p_BindImageMemory(dev, imgA, memA, 0));
    VkImageDrmFormatModifierPropertiesEXT imp = {.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
    MUST(p_GetImageDrmFormatModifierPropertiesEXT(dev, imgA, &imp));
    VkImageSubresource sub = {.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT};
    VkSubresourceLayout lay;
    p_GetImageSubresourceLayout(dev, imgA, &sub, &lay);
    VkMemoryGetFdInfoKHR gfi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = memA,
                                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int dmabuf_fd = -1;
    MUST(p_GetMemoryFdKHR(dev, &gfi, &dmabuf_fd));
    char tgt[128];
    fd_target(dmabuf_fd, tgt, sizeof tgt);
    off_t dsize = lseek(dmabuf_fd, 0, SEEK_END);
    lseek(dmabuf_fd, 0, SEEK_SET);
    printf("  image A: modifier 0x%016llx, plane0 offset %llu rowPitch %llu size %llu; dma-buf fd %d -> %s, %lld bytes; "
           "memreqs %llu (dedicated required %u)\n", (unsigned long long)imp.drmFormatModifier,
           (unsigned long long)lay.offset, (unsigned long long)lay.rowPitch, (unsigned long long)lay.size, dmabuf_fd,
           tgt, (long long)dsize, (unsigned long long)mr2.memoryRequirements.size, dr.requiresDedicatedAllocation);
    check(!strncmp(tgt, "/dmabuf:", 8), "exported fd is a dma-buf");

    /* ---- synthetic minigbm handle ---- */
    struct cros_gralloc_handle gh;
    memset(&gh, 0, sizeof gh);
    gh.base.version = sizeof(native_handle_t);
    gh.base.numFds = 1;
    gh.base.numInts = (int)CROS_GRALLOC_HANDLE_NUM_INTS_TOTAL - 1;
    for (int i = 0; i < DRV_MAX_FDS; i++) gh.fds[i] = -1;
    gh.fds[0] = dmabuf_fd;
    gh.strides[0] = (uint32_t)lay.rowPitch;
    gh.offsets[0] = (uint32_t)lay.offset;
    gh.sizes[0] = (uint32_t)lay.size;
    gh.width = W;
    gh.height = H;
    gh.format = DRM_FORMAT_ABGR8888;
    gh.format_modifier = imp.drmFormatModifier;
    gh.magic = CROS_GRALLOC_MAGIC;
    gh.pixel_stride = (uint32_t)(lay.rowPitch / 4);
    gh.droid_format = 1; /* HAL_PIXEL_FORMAT_RGBA_8888 */
    gh.usage = 0x900;
    gh.num_planes = 1;
    gh.total_size = (uint64_t)dsize;

    VkSwapchainImageCreateInfoANDROID sci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_IMAGE_CREATE_INFO_ANDROID};
    VkNativeBufferANDROID nb = {.sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID, .pNext = &sci,
                                .handle = (const native_handle_t *)(const void *)&gh, .stride = (int)(lay.rowPitch / 4), .format = 1,
                                .usage = g0, .usage2 = {.consumer = cons, .producer = prod}};
    /* exactly what swapchain.cpp:1386-1453 passes */
    VkImageCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &nb, .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    {
        VkImage x;
        gh.magic = 0x12345678;
        check(VKR(p_CreateImage(dev, &bci, NULL, &x)) == VK_ERROR_INVALID_EXTERNAL_HANDLE, "bad magic -> VK_ERROR_INVALID_EXTERNAL_HANDLE");
        gh.magic = CROS_GRALLOC_MAGIC;
        gh.base.numFds = 0;
        check(VKR(p_CreateImage(dev, &bci, NULL, &x)) == VK_ERROR_INVALID_EXTERNAL_HANDLE, "numFds 0 -> VK_ERROR_INVALID_EXTERNAL_HANDLE");
        gh.base.numFds = 1;
        gh.format = DRM_FORMAT_ARGB8888;
        check(VKR(p_CreateImage(dev, &bci, NULL, &x)) == VK_ERROR_FORMAT_NOT_SUPPORTED,
              "RGBA VkFormat on an ARGB8888 buffer -> VK_ERROR_FORMAT_NOT_SUPPORTED");
        gh.format = DRM_FORMAT_ABGR8888;
    }
    int dmabufs_before_import = count_fds_matching("/dmabuf:");
    VkImage imgB;
    check(VKR(p_CreateImage(dev, &bci, NULL, &imgB)) == VK_SUCCESS, "ANB import (vkCreateImage + VkNativeBufferANDROID)");
    if (failures) { printf("FATAL: import failed\n"); return 1; }

    /* a second import of the same buffer with INPUT_ATTACHMENT usage (loader-allowed swapchain
     * usage): the HAL must import it, stripping the bit only if the ICD refuses it (logged) */
    {
        VkImageCreateInfo ici2 = bci;
        ici2.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        VkImage x;
        VkResult r = VKR(p_CreateImage(dev, &ici2, NULL, &x));
        check(r == VK_SUCCESS, "second ANB import of the same buffer with INPUT_ATTACHMENT usage");
        if (r == VK_SUCCESS) p_DestroyImage(dev, x, NULL);
    }

    /* ---- pixels: clear B, read through A ---- */
    struct hostbuf rb = hostbuf((VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
    VkClearColorValue cc = {.float32 = {0.2f, 0.4f, 0.6f, 1.0f}};
    VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    memset(rb.ptr, 0, (size_t)W * H * 4);
    begin();
    barrier(imgB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    p_CmdClearColorImage(cmd, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rng);
    barrier(imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    submit_wait("clear image B (ANB import) to {0.2,0.4,0.6,1.0}", 0, NULL, 0, NULL, 1);
    begin();
    /* A's memory was written through B: PREINITIALIZED -> GENERAL keeps the bytes */
    barrier(imgA, VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_GENERAL);
    p_CmdCopyImageToBuffer(cmd, imgA, VK_IMAGE_LAYOUT_GENERAL, rb.buf, 1, &region);
    submit_wait("copy image A (original export, same memory) to a host buffer", 0, NULL, 0, NULL, 1);
    uint32_t good = 0;
    for (uint32_t i = 0; i < W * H; i++) {
        const uint8_t *p = rb.ptr + 4 * i;
        good += p[0] == 51 && p[1] == 102 && p[2] == 153 && p[3] == 255;
    }
    check(good == W * H, "clear via B, read via A: %u/%u texels == {51,102,153,255} (first {%u,%u,%u,%u})", good, W * H,
          rb.ptr[0], rb.ptr[1], rb.ptr[2], rb.ptr[3]);

    /* ---- pixels: per-texel pattern into B, read through A (validates the tiled layout) ---- */
    struct hostbuf up = hostbuf((VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint8_t *p = up.ptr + 4 * (y * W + x);
            p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = (uint8_t)(x ^ (y * 7)); p[3] = (uint8_t)(x + 3 * y);
        }
    memset(rb.ptr, 0, (size_t)W * H * 4);
    begin();
    barrier(imgB, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    p_CmdCopyBufferToImage(cmd, up.buf, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    VkSemaphoreCreateInfo semci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore rendered;
    MUST(p_CreateSemaphore(dev, &semci, NULL, &rendered));
    submit_wait("upload a per-texel pattern into image B (signals a semaphore)", 0, NULL, 1, &rendered, 1);
    begin();
    barrier(imgA, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    p_CmdCopyImageToBuffer(cmd, imgA, VK_IMAGE_LAYOUT_GENERAL, rb.buf, 1, &region);
    submit_wait("copy image A to a host buffer", 0, NULL, 0, NULL, 1);
    check(memcmp(rb.ptr, up.ptr, (size_t)W * H * 4) == 0, "pattern written via B reads back identically via A "
          "(%u x %u texels, modifier 0x%016llx)", W, H, (unsigned long long)imp.drmFormatModifier);

    /* ---- image C: vkCreateImage + VkImageSwapchainCreateInfoKHR, bound to swapchain image 0 ----
     * What dEQP-VK.wsi.*.swapchain.create.image_swapchain_create_info does (vktWsiSwapchainTests.cpp
     * 1367-1414); the Android loader passes vkCreateImage through unchanged (api_gen.cpp:1145) and, in
     * vkBindImageMemory2, prepends the VkNativeBufferANDROID of swapchain image imageIndex to the app's
     * chain (swapchain.cpp:2025-2048: native_buffer.pNext = bind_infos[idx].pNext). The swapchain
     * handle is the loader's: a fake value here, which the ICD must never see. */
    printf("  -- swapchain-bound image C --\n");
    {
        const VkSwapchainKHR fake_sc = (VkSwapchainKHR)(uintptr_t)0x5C;
        int dmabufs_before_c = count_fds_matching("/dmabuf:"), fds_before_c = count_fds();
        VkImageSwapchainCreateInfoKHR isci = {.sType = VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR,
                                              .swapchain = fake_sc};
        VkImageCreateInfo cci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &isci,
                                 .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {W, H, 1},
                                 .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        /* an unbound one is destroyed without ever reaching the ICD */
        VkImage c0 = VK_NULL_HANDLE;
        VkResult cr = VKR(p_CreateImage(dev, &cci, NULL, &c0));
        check(cr == VK_SUCCESS && c0, "vkCreateImage + VkImageSwapchainCreateInfoKHR{swapchain 0x5C} (never bound, "
              "destroyed unbound) -> %p", (void *)(uintptr_t)c0);
        p_DestroyImage(dev, c0, NULL);
        VkImage imgC = VK_NULL_HANDLE;
        cr = VKR(p_CreateImage(dev, &cci, NULL, &imgC));
        check(cr == VK_SUCCESS && imgC, "vkCreateImage + VkImageSwapchainCreateInfoKHR{swapchain 0x5C} -> image C %p",
              (void *)(uintptr_t)imgC);
        VkMemoryRequirements2 cmr = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        VkImageMemoryRequirementsInfo2 cmri = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = imgC};
        p_GetImageMemoryRequirements2(dev, &cmri, &cmr);
        check(cmr.memoryRequirements.size > 0 && cmr.memoryRequirements.memoryTypeBits,
              "vkGetImageMemoryRequirements2(C) before the bind: %llu B, alignment %llu, types 0x%x",
              (unsigned long long)cmr.memoryRequirements.size, (unsigned long long)cmr.memoryRequirements.alignment,
              cmr.memoryRequirements.memoryTypeBits);

        VkBindImageMemorySwapchainInfoKHR bsi = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR,
                                                 .swapchain = fake_sc, .imageIndex = 0};
        VkNativeBufferANDROID nbb = {.sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID, .pNext = &bsi,
                                     .handle = (const native_handle_t *)(const void *)&gh,
                                     .stride = (int)(lay.rowPitch / 4), .format = 1, .usage = g0,
                                     .usage2 = {.consumer = cons, .producer = prod}};
        /* negative: memory != NULL, no native buffer, the one-image bind */
        VkBindImageMemoryInfo bad = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, .pNext = &nbb, .image = imgC,
                                     .memory = rb.mem};
        check(VKR(p_BindImageMemory2(dev, 1, &bad)) == VK_ERROR_INVALID_EXTERNAL_HANDLE,
              "bind C with VkBindImageMemorySwapchainInfoKHR + VkNativeBufferANDROID but memory != NULL -> "
              "VK_ERROR_INVALID_EXTERNAL_HANDLE");
        bad.pNext = &bsi;
        bad.memory = VK_NULL_HANDLE;
        check(VKR(p_BindImageMemory2(dev, 1, &bad)) == VK_ERROR_INVALID_EXTERNAL_HANDLE,
              "bind C with VkBindImageMemorySwapchainInfoKHR but no VkNativeBufferANDROID -> "
              "VK_ERROR_INVALID_EXTERNAL_HANDLE");
        check(VKR(p_BindImageMemory(dev, imgC, rb.mem, 0)) == VK_ERROR_INVALID_EXTERNAL_HANDLE,
              "vkBindImageMemory(C, memory) -> VK_ERROR_INVALID_EXTERNAL_HANDLE");

        /* the bind, in one call with an ordinary image E (the HAL splits the array) */
        VkImageCreateInfo eci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {64, 64, 1}, .mipLevels = 1,
                                 .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VkImage imgE;
        MUST(p_CreateImage(dev, &eci, NULL, &imgE));
        VkMemoryRequirements2 emr = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        VkImageMemoryRequirementsInfo2 emri = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = imgE};
        p_GetImageMemoryRequirements2(dev, &emri, &emr);
        VkMemoryAllocateInfo emai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = emr.memoryRequirements.size,
            .memoryTypeIndex = mem_type(emr.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        VkDeviceMemory memE;
        MUST(p_AllocateMemory(dev, &emai, NULL, &memE));
        VkBindImageMemoryInfo binds[2] = {
            {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, .pNext = &nbb, .image = imgC,
             .memory = VK_NULL_HANDLE, .memoryOffset = 0},
            {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, .image = imgE, .memory = memE},
        };
        VkResult br = VKR(p_BindImageMemory2(dev, 2, binds));
        check(br == VK_SUCCESS, "vkBindImageMemory2{C: memory NULL, VkNativeBufferANDROID -> "
              "VkBindImageMemorySwapchainInfoKHR{0x5C, 0}; E: ordinary memory}");
        if (br != VK_SUCCESS) { printf("FATAL: swapchain bind failed\n"); return 1; }
        int dmabufs_c = count_fds_matching("/dmabuf:"), fds_c = count_fds();
        check(VKR(p_BindImageMemory2(dev, 1, binds)) == VK_ERROR_INVALID_EXTERNAL_HANDLE,
              "second swapchain bind of C -> VK_ERROR_INVALID_EXTERNAL_HANDLE");
        VkMemoryRequirements2 cmr2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        p_GetImageMemoryRequirements2(dev, &cmri, &cmr2);
        check(cmr2.memoryRequirements.size > 0, "vkGetImageMemoryRequirements2(C) after the bind (real image): %llu B",
              (unsigned long long)cmr2.memoryRequirements.size);

        /* clear C, read via A */
        VkClearColorValue cc2 = {.float32 = {0.8f, 0.6f, 0.4f, 0.2f}};
        memset(rb.ptr, 0, (size_t)W * H * 4);
        begin();
        barrier(imgC, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        p_CmdClearColorImage(cmd, imgC, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc2, 1, &rng);
        barrier(imgC, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        submit_wait("clear image C (swapchain-bound) to {0.8,0.6,0.4,0.2}", 0, NULL, 0, NULL, 1);
        begin();
        barrier(imgA, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        p_CmdCopyImageToBuffer(cmd, imgA, VK_IMAGE_LAYOUT_GENERAL, rb.buf, 1, &region);
        submit_wait("copy image A to a host buffer", 0, NULL, 0, NULL, 1);
        uint32_t cgood = 0;
        for (uint32_t i = 0; i < W * H; i++) {
            const uint8_t *px = rb.ptr + 4 * i;
            cgood += px[0] == 204 && px[1] == 153 && px[2] == 102 && px[3] == 51;
        }
        check(cgood == W * H, "clear via C, read via A: %u/%u texels == {204,153,102,51} (first {%u,%u,%u,%u})", cgood,
              W * H, rb.ptr[0], rb.ptr[1], rb.ptr[2], rb.ptr[3]);

        /* a second per-texel pattern via C, read via A */
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
                uint8_t *px = up.ptr + 4 * (y * W + x);
                px[0] = (uint8_t)(255 - x); px[1] = (uint8_t)(x * 3 + y); px[2] = (uint8_t)(y ^ 0x5C); px[3] = (uint8_t)(x * y);
            }
        memset(rb.ptr, 0, (size_t)W * H * 4);
        begin();
        barrier(imgC, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        p_CmdCopyBufferToImage(cmd, up.buf, imgC, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(imgC, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        submit_wait("upload a second per-texel pattern into image C", 0, NULL, 0, NULL, 1);
        begin();
        barrier(imgA, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        p_CmdCopyImageToBuffer(cmd, imgA, VK_IMAGE_LAYOUT_GENERAL, rb.buf, 1, &region);
        submit_wait("copy image A to a host buffer", 0, NULL, 0, NULL, 1);
        check(memcmp(rb.ptr, up.ptr, (size_t)W * H * 4) == 0, "pattern written via C reads back identically via A "
              "(%u x %u texels, modifier 0x%016llx)", W, H, (unsigned long long)imp.drmFormatModifier);

        p_DestroyImage(dev, imgC, NULL);
        p_DestroyImage(dev, imgE, NULL);
        p_FreeMemory(dev, memE, NULL);
        /* this ICD closes the imported fd itself (as for B above): the dma-buf count does not rise while
         * bound; a leaked dup would show in both counts afterwards */
        int dmabufs_after_c = count_fds_matching("/dmabuf:"), fds_after_c = count_fds();
        check(dmabufs_after_c == dmabufs_before_c && fds_after_c == fds_before_c,
              "fds around C: dma-buf %d before, %d while bound, %d after vkDestroyImage(C); all fds %d / %d / %d",
              dmabufs_before_c, dmabufs_c, dmabufs_after_c, fds_before_c, fds_c, fds_after_c);
    }

    /* ---- vkQueueSignalReleaseImageANDROID ---- */
    int rfd = -2;
    /* `rendered` was signalled by a submission whose fence this test already waited for, and the queue is
     * idle: the one case where Android permits -1. */
    VkResult rr = VKR(p_QueueSignalReleaseImageANDROID(queue, 1, &rendered, imgB, &rfd));
    check(rr == VK_SUCCESS && (rfd >= 0 || (accept_signalled_minus1 && rfd == -1)),
          "QueueSignalReleaseImageANDROID(1 already-signalled wait semaphore, idle queue) returned fd %d%s", rfd,
          rfd == -1 ? " (= already signalled; accepted for Mesa NVK only)" : "");
    struct sync_file_info sfi;
    if (rfd >= 0) {
        int sr = sync_file_info(rfd, &sfi);
        fd_target(rfd, tgt, sizeof tgt);
        struct pollfd pfd = {.fd = rfd, .events = POLLIN};
        int pr = poll(&pfd, 1, 5000);
        check(sr == 0 && pr == 1 && (pfd.revents & POLLIN), "release fd -> %s, SYNC_IOC_FILE_INFO=%d name='%s' "
              "status=%d num_fences=%u; poll(5 s) = %d revents 0x%x", tgt, sr, sfi.name, sfi.status, sfi.num_fences,
              pr, pfd.revents);
        close(rfd);
    }

    /* release while the waited-on work is genuinely pending: the fd must not signal early */
    VkImageCreateInfo bigci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                               .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {4096, 4096, 1}, .mipLevels = 1,
                               .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                               .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkImage big;
    MUST(p_CreateImage(dev, &bigci, NULL, &big));
    VkMemoryRequirements2 bmr = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    VkImageMemoryRequirementsInfo2 bmri = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = big};
    p_GetImageMemoryRequirements2(dev, &bmri, &bmr);
    VkMemoryAllocateInfo bmai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bmr.memoryRequirements.size,
        .memoryTypeIndex = mem_type(bmr.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory bigmem;
    MUST(p_AllocateMemory(dev, &bmai, NULL, &bigmem));
    VkBindImageMemoryInfo bbind = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, .image = big, .memory = bigmem};
    MUST(p_BindImageMemory2(dev, 1, &bbind));
    /* Size the busy work to ~150 ms on this GPU at its current clocks. A fixed count is
     * 51 GB of writes, which a narrow-bus card at idle clocks cannot finish inside the
     * 5 s SEMSURF fence lifetime (nvidia-drm-fence.c:43). Pending at the first poll is
     * what this check needs, not volume. */
    const int calib_clears = 8;
    begin();
    barrier(big, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (int i = 0; i < calib_clears; i++) {
        VkClearColorValue c = {.float32 = {0, i / (float)calib_clears, 0, 1}};
        p_CmdClearColorImage(cmd, big, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rng);
        barrier(big, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }
    double calib_t0 = now_ms();
    submit_wait("busy-work calibration", 0, NULL, 0, NULL, 1);
    double per_clear = (now_ms() - calib_t0) / calib_clears;
    int busy_clears = per_clear > 0 ? (int)(150.0 / per_clear) + 1 : 400;
    if (busy_clears < 8) busy_clears = 8;
    if (busy_clears > 400) busy_clears = 400;
    printf("    calibration: %.3f ms per 128 MiB clear -> %d clears\n", per_clear, busy_clears);
    VkCommandBuffer busy;
    struct hostbuf completion = hostbuf(4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const uint32_t completion_value = 0x71c05eed;
    MUST(p_AllocateCommandBuffers(dev, &cbai, &busy));
    {
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        MUST(p_BeginCommandBuffer(busy, &bi));
        VkCommandBuffer save = cmd;
        cmd = busy;
        barrier(big, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        for (int i = 0; i < busy_clears; i++) {
            VkClearColorValue c = {.float32 = {i / (float)busy_clears, 0, 0, 1}};
            p_CmdClearColorImage(busy, big, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rng);
            barrier(big, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }
        p_CmdFillBuffer(busy, completion.buf, 0, 4, completion_value);
        VkBufferMemoryBarrier completion_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = completion.buf, .offset = 0, .size = 4,
        };
        p_CmdPipelineBarrier(busy, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                            0, NULL, 1, &completion_barrier, 0, NULL);
        cmd = save;
        MUST(p_EndCommandBuffer(busy));
    }
    {
        VkSemaphore busy_done;
        MUST(p_CreateSemaphore(dev, &semci, NULL, &busy_done));
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &busy,
                           .signalSemaphoreCount = 1, .pSignalSemaphores = &busy_done};
        double t0 = now_ms();
        memset(completion.ptr, 0, 4);
        MUST(p_QueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        double submitted = now_ms();
        int pfd2 = -2;
        VKR(p_QueueSignalReleaseImageANDROID(queue, 1, &busy_done, imgB, &pfd2));
        struct pollfd pp = {.fd = pfd2, .events = POLLIN};
        int early = poll(&pp, 1, 0);
        double t1 = now_ms();
        int later = poll(&pp, 1, 10000);
        double t2 = now_ms();
        uint32_t marker;
        memcpy(&marker, completion.ptr, 4);
        check(pfd2 >= 0 && (early == 0 || (allow_synchronous_submit && early == 1)) && later == 1 &&
              (pp.revents & POLLIN) && marker == completion_value,
              "release behind busy GPU work (%d clears): fd %d, submit %.2f ms, first poll %d at %.2f ms, "
              "final poll %d at %.2f ms, GPU marker 0x%x (%s)", busy_clears, pfd2, submitted - t0, early, t1 - t0,
              later, t2 - t0, marker, allow_synchronous_submit ? "synchronous submit permitted" : "pending required");
        close(pfd2);
        MUST(p_QueueWaitIdle(queue));
        p_DestroySemaphore(dev, busy_done, NULL);
    }
    /* zero wait semaphores but a busy queue: the release covers every earlier submission on the queue, so the
     * queue is not idle and -1 would be an early signal (for any ICD) */
    {
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &busy};
        double t0 = now_ms();
        memset(completion.ptr, 0, 4);
        MUST(p_QueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        int pfd3 = -2;
        VKR(p_QueueSignalReleaseImageANDROID(queue, 0, NULL, imgB, &pfd3));
        uint32_t marker_at_release;
        memcpy(&marker_at_release, completion.ptr, 4);
        struct pollfd pp = {.fd = pfd3, .events = POLLIN};
        int early = pfd3 >= 0 ? poll(&pp, 1, 0) : -1;
        double t1 = now_ms();
        int later = pfd3 >= 0 ? poll(&pp, 1, 10000) : -1;
        double t2 = now_ms();
        uint32_t marker;
        memcpy(&marker, completion.ptr, 4);
        check(pfd3 >= 0 && (early == 0 || (allow_synchronous_submit && early == 1)) && later == 1 &&
              (pp.revents & POLLIN) && marker == completion_value,
              "release with 0 semaphores behind busy queue work (%d clears): fd %d, first poll %d at %.2f ms "
              "(GPU marker then 0x%x), final poll %d at %.2f ms, GPU marker 0x%x (%s)", busy_clears, pfd3, early,
              t1 - t0, marker_at_release, later, t2 - t0, marker,
              allow_synchronous_submit ? "synchronous submit permitted" : "pending required");
        if (pfd3 >= 0) close(pfd3);
        MUST(p_QueueWaitIdle(queue));
    }

    /* ---- the property the HAL's export-fence reuse rests on (measured on this ICD) ---- */
    {
        VkExportFenceCreateInfo efci = {.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
                                        .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = &efci};
        VkFence ef;
        MUST(p_CreateFence(dev, &fci, NULL, &ef));
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &busy};
        MUST(p_QueueSubmit(queue, 1, &si, ef));
        VkFenceGetFdInfoKHR fgi = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, .fence = ef,
                                   .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        int efd = -1;
        MUST(p_GetFenceFdKHR(dev, &fgi, &efd));
        VkResult s0 = p_GetFenceStatus(dev, ef);
        struct pollfd pp = {.fd = efd, .events = POLLIN};
        int pl = poll(&pp, 1, 10000);
        MUST(p_QueueWaitIdle(queue));
        VkResult s1 = p_GetFenceStatus(dev, ef);
        check(s0 == VK_NOT_READY && pl == 1 && s1 == VK_NOT_READY,
              "SYNC_FD fence export while pending: fence status %d right after export, sync_fd signalled (poll=%d), "
              "fence status after the work completed %d (export detached the signal: immediate reuse is safe)",
              s0, pl, s1);
        close(efd);
        p_DestroyFence(dev, ef, NULL);
    }

    /* zero semaphores, and pool reuse with fd accounting */
    /* Run 100 calls twice; the second run must not grow the fd count (the first may: the ICD
     * keeps one sync_file per fence/semaphore object it has exported from or imported into). */
    {
        int ok = 1, fdc[3], minus1 = 0;
        fdc[0] = count_fds();
        for (int run = 0; run < 2; run++) {
            fd_snapshot();
            for (int i = 0; i < (bounded ? 4 : 100); i++) {
                int f = -2;
                VkResult r = p_QueueSignalReleaseImageANDROID(queue, 0, NULL, imgB, &f);
                /* idle queue, nothing to wait for: -1 is "already signalled" where accepted */
                if (r == VK_SUCCESS && f == -1 && accept_signalled_minus1) { minus1++; continue; }
                struct pollfd pp = {.fd = f, .events = POLLIN};
                if (r != VK_SUCCESS || f < 0 || poll(&pp, 1, 5000) != 1) { ok = 0; printf("  iteration %d: %d fd %d\n", i, r, f); }
                if (f >= 0) close(f);
            }
            fd_diff(run ? "release run 2" : "release run 1");
            fdc[run + 1] = count_fds();
        }
        check(ok && fdc[2] == fdc[1], "2 x %d QueueSignalReleaseImageANDROID(0 semaphores, idle queue): every fd "
              "signalled (%d of them -1 = already signalled%s); fd count %d -> %d (run 1) -> %d (run 2)",
              bounded ? 4 : 100, minus1, accept_signalled_minus1 ? ", Mesa NVK" : ", none accepted", fdc[0], fdc[1],
              fdc[2]);
    }

    /* ---- vkAcquireImageANDROID ---- */
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence af;
    MUST(p_CreateFence(dev, &fci, NULL, &af));
    VkSemaphore as;
    MUST(p_CreateSemaphore(dev, &semci, NULL, &as));
    check(VKR(p_AcquireImageANDROID(dev, imgB, -1, as, af)) == VK_SUCCESS, "AcquireImageANDROID(fd -1, semaphore, fence)");
    check(VKR(p_WaitForFences(dev, 1, &af, VK_TRUE, 5000000000ull)) == VK_SUCCESS, "fence from fd -1 is signalled");
    begin();
    submit_wait("empty submit waiting on the semaphore acquired from fd -1", 1, &as, 0, NULL, 0);
    MUST(p_ResetFences(dev, 1, &af));

    /* a real sync_fd from a fence the GPU signals */
    {
        VkExportFenceCreateInfo efci = {.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
                                        .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        VkFenceCreateInfo xfci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = &efci};
        VkFence xf;
        MUST(p_CreateFence(dev, &xfci, NULL, &xf));
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &busy};
        double t0 = now_ms();
        memset(completion.ptr, 0, 4);
        MUST(p_QueueSubmit(queue, 1, &si, xf));
        double submitted = now_ms();
        VkFenceGetFdInfoKHR fgi = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, .fence = xf,
                                   .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        int sfd = -1;
        MUST(p_GetFenceFdKHR(dev, &fgi, &sfd));
        int si_ok = sync_file_info(sfd, &sfi) == 0;
        printf("  real sync_fd %d: sync_file '%s' status %d (pending GPU work)\n", sfd, sfi.name, sfi.status);
        check(VKR(p_AcquireImageANDROID(dev, imgB, sfd, as, af)) == VK_SUCCESS && si_ok,
              "AcquireImageANDROID(real pending sync_fd, semaphore, fence)");
        VkResult early = p_GetFenceStatus(dev, af);
        double t1 = now_ms();
        uint32_t early_marker;
        memcpy(&early_marker, completion.ptr, 4);
        VkResult wr = VKR(p_WaitForFences(dev, 1, &af, VK_TRUE, 10000000000ull));
        double t2 = now_ms();
        uint32_t marker;
        memcpy(&marker, completion.ptr, 4);
        if (acquire_wait_fence || acquire_wait_semaphore) /* returned after the GPU work: signalled, marker written */
            check(wr == VK_SUCCESS && early == VK_SUCCESS && early_marker == completion_value &&
                  marker == completion_value, "acquire fence: submit %.2f ms, status %d at %.2f ms (acquire waited "
                  "for the native fence), signalled at %.2f ms, GPU marker 0x%x at return", submitted - t0, early,
                  t1 - t0, t2 - t0, early_marker);
        else
            check(wr == VK_SUCCESS && (early == VK_NOT_READY || (allow_synchronous_submit && early == VK_SUCCESS)) &&
                  marker == completion_value, "acquire fence: submit %.2f ms, status %d at %.2f ms, "
                  "signalled at %.2f ms, GPU marker 0x%x (%s)", submitted - t0, early, t1 - t0, t2 - t0,
                  marker, allow_synchronous_submit ? "synchronous submit permitted" : "pending required");
        begin();
        submit_wait("empty submit waiting on the semaphore acquired from the sync_fd", 1, &as, 0, NULL, 0);
        MUST(p_ResetFences(dev, 1, &af));
        p_DestroyFence(dev, xf, NULL);
    }
    /* the Android frame flow: acquire from a pending (foreign) sync_fd, release waiting on that semaphore.
     * The awaited producer is pending, so the release fd must be a real pending sync_file, never -1 */
    {
        VkExportFenceCreateInfo efci = {.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
                                        .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        VkFenceCreateInfo xfci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = &efci};
        VkFence xf;
        MUST(p_CreateFence(dev, &xfci, NULL, &xf));
        VkSemaphore as2;
        MUST(p_CreateSemaphore(dev, &semci, NULL, &as2));
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &busy};
        double t0 = now_ms();
        memset(completion.ptr, 0, 4);
        MUST(p_QueueSubmit(queue, 1, &si, xf));
        VkFenceGetFdInfoKHR fgi = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, .fence = xf,
                                   .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        int sfd = -1;
        MUST(p_GetFenceFdKHR(dev, &fgi, &sfd));
        int src_pending = 0;
        if (sfd >= 0) {
            struct pollfd sp = {.fd = sfd, .events = POLLIN};
            src_pending = poll(&sp, 1, 0) == 0;
        }
        VkResult ar = VKR(p_AcquireImageANDROID(dev, imgB, sfd, as2, VK_NULL_HANDLE));
        int pfd4 = -2;
        VkResult rr4 = VKR(p_QueueSignalReleaseImageANDROID(queue, 1, &as2, imgB, &pfd4));
        struct pollfd pp = {.fd = pfd4, .events = POLLIN};
        int early = pfd4 >= 0 ? poll(&pp, 1, 0) : -1;
        double t1 = now_ms();
        int later = pfd4 >= 0 ? poll(&pp, 1, 10000) : -1;
        double t2 = now_ms();
        uint32_t marker;
        memcpy(&marker, completion.ptr, 4);
        check(ar == VK_SUCCESS && rr4 == VK_SUCCESS && sfd >= 0 && pfd4 >= 0 &&
              (early == 0 || ((allow_synchronous_submit || acquire_wait_semaphore) && early == 1)) && later == 1 &&
              (pp.revents & POLLIN) &&
              marker == completion_value && (src_pending || allow_synchronous_submit),
              "release waiting on a semaphore acquired from a pending sync_fd (source fd %d pending %d): release fd %d, "
              "first poll %d at %.2f ms, final poll %d at %.2f ms, GPU marker 0x%x (%s)", sfd, src_pending, pfd4, early,
              t1 - t0, later, t2 - t0, marker,
              allow_synchronous_submit ? "synchronous submit permitted"
              : acquire_wait_semaphore ? "acquire waited for the native fence" : "pending required");
        if (pfd4 >= 0) close(pfd4);
        MUST(p_QueueWaitIdle(queue));
        p_DestroySemaphore(dev, as2, NULL);
        p_DestroyFence(dev, xf, NULL);
    }
    /* fd ownership: the HAL must consume every fd, on success and with nothing to import into */
    {
        VkExportFenceCreateInfo efci = {.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
                                        .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        VkFenceCreateInfo xfci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = &efci};
        VkFence xf;
        MUST(p_CreateFence(dev, &xfci, NULL, &xf));
        int ok = 1, fdc[3];
        fdc[0] = count_fds();
        for (int run = 0; run < 2; run++) {
        fd_snapshot();
        for (int i = 0; i < (bounded ? 4 : 50); i++) {
            VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
            if (p_QueueSubmit(queue, 1, &si, xf) != VK_SUCCESS) ok = 0;
            VkFenceGetFdInfoKHR fgi = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, .fence = xf,
                                       .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
            int f1 = -1, f2 = -1;
            if (p_GetFenceFdKHR(dev, &fgi, &f1) != VK_SUCCESS) ok = 0;
            if (p_QueueSubmit(queue, 1, &si, xf) != VK_SUCCESS) ok = 0;
            if (p_GetFenceFdKHR(dev, &fgi, &f2) != VK_SUCCESS) ok = 0;
            if (p_AcquireImageANDROID(dev, imgB, f1, as, af) != VK_SUCCESS) ok = 0;   /* semaphore + fence: dup path */
            if (p_WaitForFences(dev, 1, &af, VK_TRUE, 5000000000ull) != VK_SUCCESS) ok = 0;
            p_ResetFences(dev, 1, &af);
            VkSubmitInfo w = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &as,
                              .pWaitDstStageMask = (VkPipelineStageFlags[]){VK_PIPELINE_STAGE_ALL_COMMANDS_BIT}};
            if (p_QueueSubmit(queue, 1, &w, VK_NULL_HANDLE) != VK_SUCCESS) ok = 0;
            if (p_AcquireImageANDROID(dev, imgB, f2, VK_NULL_HANDLE, VK_NULL_HANDLE) != VK_SUCCESS) ok = 0; /* close path */
        }
        MUST(p_QueueWaitIdle(queue));
        fd_diff(run ? "acquire run 2" : "acquire run 1");
        fdc[run + 1] = count_fds();
        }
        check(ok && fdc[2] == fdc[1], "2 x (%d AcquireImageANDROID(sem+fence) + %d (no objects)): fd count %d -> %d (run 1) "
              "-> %d (run 2)", bounded ? 4 : 50, bounded ? 4 : 50, fdc[0], fdc[1], fdc[2]);
        p_DestroyFence(dev, xf, NULL);
    }

    /* ---- bind with VkNativeBufferANDROID chained on a plain ICD image: explicit refusal ---- */
    {
        VkBindImageMemoryInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, .pNext = &nb, .image = big,
                                    .memory = bigmem};
        check(VKR(p_BindImageMemory2(dev, 1, &bi)) == VK_ERROR_INVALID_EXTERNAL_HANDLE,
              "vkBindImageMemory2 + VkNativeBufferANDROID on a plain ICD image -> VK_ERROR_INVALID_EXTERNAL_HANDLE");
    }

    /* ---- teardown: the imported memory (and its dup of the dma-buf fd) must go away ---- */
    int dmabufs_imported = count_fds_matching("/dmabuf:");
    p_DestroyImage(dev, imgB, NULL);
    check(count_fds_matching("/dmabuf:") == dmabufs_before_import, "dma-buf fds: %d before the ANB import, %d while "
          "imported, %d after vkDestroyImage", dmabufs_before_import, dmabufs_imported, count_fds_matching("/dmabuf:"));
    p_DestroyImage(dev, imgA, NULL);
    p_FreeMemory(dev, memA, NULL);
    close(dmabuf_fd);
    p_DestroyImage(dev, big, NULL);
    p_FreeMemory(dev, bigmem, NULL);
    p_DestroyBuffer(dev, completion.buf, NULL);
    p_FreeMemory(dev, completion.mem, NULL);
    p_DestroyBuffer(dev, rb.buf, NULL);
    p_FreeMemory(dev, rb.mem, NULL);
    p_DestroyBuffer(dev, up.buf, NULL);
    p_FreeMemory(dev, up.mem, NULL);
    p_DestroySemaphore(dev, rendered, NULL);
    p_DestroySemaphore(dev, as, NULL);
    p_DestroyFence(dev, af, NULL);
    p_DestroyCommandPool(dev, pool, NULL);
    fd_snapshot();
    p_DestroyDevice(dev, NULL);
    fd_diff("vkDestroyDevice");
    DestroyInstance(inst, NULL);
    check(hal->common.close(&hal->common) == 0, "hw_device_t.close");

    printf("anb-roundtrip: %d failure(s) -> %s\n", failures, failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}
