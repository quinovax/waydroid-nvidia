/* See vtest_gpu_alloc.h. Vulkan is loaded lazily via dlopen so the vtest
 * server keeps working (minus this command) on hosts without a driver. */

#include "vtest_gpu_alloc.h"
#include "vtest_alloc_formats.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define ALLOC_ALIGN(v, a) (((v) + (a)-1) & ~((uint64_t)(a)-1))

/* Presentation path for hybrid GPUs (issue #2 / gamescope#1590).
 *
 * Discrete NVIDIA compositor: block-linear. NVIDIA EGL binds LINEAR only
 * as GL_TEXTURE_EXTERNAL_OES; KWin samples RGB as GL_TEXTURE_2D.
 *
 * iGPU compositor: NVIDIA LINEAR, allocated in SYSTEM memory first (issue
 * #7). Intel/AMD import LINEAR dma_bufs as GL_TEXTURE_2D
 * (tests/crossimport.c) but cannot sample dGPU VRAM directly — a VRAM
 * placement (the old behaviour) is read through BAR1 and shows up black on
 * RTD3 hybrid laptops. sysmem keeps NVIDIA as the exporter (guest renders
 * stay clean) while the iGPU imports memory it can actually read. VRAM is
 * only a fallback for hosts that refuse sysmem placements.
 *
 * udmabuf: explicit compatibility fallback. Kernel-allocated LINEAR memory
 * that every driver imports; NVIDIA renders into it through its dma-buf
 * import path, which some stacks get wrong (Xid-69-era corruption) — that
 * is what the NVIDIA-LINEAR paths above are for.
 *
 * Override: WAYDROID_NVIDIA_PRESENT=linear|block-linear|udmabuf
 */
enum present_mode {
   PRESENT_BLOCK_LINEAR = 0,
   PRESENT_NVIDIA_LINEAR = 1,
   PRESENT_UDMABUF = 2,
};

static enum present_mode g_present_mode = PRESENT_BLOCK_LINEAR;
static pthread_once_t g_present_once = PTHREAD_ONCE_INIT;
static unsigned g_alloc_log_n;

static const char *
pci_vendor_name(unsigned vendor)
{
   switch (vendor) {
   case 0x10de:
      return "NVIDIA";
   case 0x8086:
      return "Intel";
   case 0x1002:
      return "AMD";
   default:
      return "other";
   }
}

static bool
read_sysfs_u(const char *path, unsigned *out)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return false;
   unsigned v = 0;
   int n = fscanf(f, "%x", &v);
   fclose(f);
   if (n != 1)
      return false;
   *out = v;
   return true;
}

static bool
read_sysfs_line(const char *path, char *buf, size_t buflen)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return false;
   if (!fgets(buf, (int)buflen, f)) {
      fclose(f);
      return false;
   }
   fclose(f);
   size_t n = strlen(buf);
   while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
      buf[--n] = '\0';
   return n > 0;
}

static bool
is_drm_card(const char *name)
{
   if (strncmp(name, "card", 4) != 0)
      return false;
   const char *p = name + 4;
   if (*p < '0' || *p > '9')
      return false;
   for (; *p; p++) {
      if (*p < '0' || *p > '9')
         return false;
   }
   return true;
}

static void
present_init(void)
{
   bool nvidia_connected = false;
   bool other_connected = false;
   bool other_present = false;
   bool boot_vga_nvidia = false;
   bool boot_vga_other = false;

   fprintf(stderr, "vtest_gpu_alloc: === presentation topology (hybrid issue #2) ===\n");

   DIR *dir = opendir("/sys/class/drm");
   if (!dir) {
      fprintf(stderr, "vtest_gpu_alloc:   cannot open /sys/class/drm (%s)\n",
              strerror(errno));
   } else {
      struct dirent *ent;
      while ((ent = readdir(dir))) {
         if (!is_drm_card(ent->d_name))
            continue;

         char path[320];
         unsigned vendor = 0;
         snprintf(path, sizeof(path), "/sys/class/drm/%s/device/vendor",
                  ent->d_name);
         if (!read_sysfs_u(path, &vendor))
            continue;

         char driver[64] = "?";
         snprintf(path, sizeof(path), "/sys/class/drm/%s/device/driver",
                  ent->d_name);
         char link[256];
         ssize_t n = readlink(path, link, sizeof(link) - 1);
         if (n > 0) {
            link[n] = '\0';
            const char *slash = strrchr(link, '/');
            const char *src = slash ? slash + 1 : link;
            snprintf(driver, sizeof(driver), "%.*s", (int)sizeof(driver) - 1, src);
         }

         unsigned boot = 0;
         snprintf(path, sizeof(path), "/sys/class/drm/%s/device/boot_vga",
                  ent->d_name);
         read_sysfs_u(path, &boot);

         bool nvidia = (vendor == 0x10de);
         char conn[192] = "";
         bool connected = false;

         DIR *conns = opendir("/sys/class/drm");
         if (conns) {
            char prefix[64];
            if ((size_t)snprintf(prefix, sizeof(prefix), "%s-", ent->d_name) >=
                sizeof(prefix)) {
               closedir(conns);
               continue;
            }
            size_t plen = strlen(prefix);
            struct dirent *c;
            while ((c = readdir(conns))) {
               if (strncmp(c->d_name, prefix, plen) != 0)
                  continue;
               snprintf(path, sizeof(path), "/sys/class/drm/%s/status",
                        c->d_name);
               char st[32];
               if (!read_sysfs_line(path, st, sizeof(st)))
                  continue;
               if (strcmp(st, "connected") != 0)
                  continue;
               connected = true;
               size_t used = strlen(conn);
               snprintf(conn + used, sizeof(conn) - used, "%s%s",
                        used ? "," : "", c->d_name + plen);
            }
            closedir(conns);
         }

         fprintf(stderr,
                 "vtest_gpu_alloc:   %s vendor=0x%04x (%s) driver=%s boot_vga=%u connected=%s",
                 ent->d_name, vendor, pci_vendor_name(vendor), driver, boot,
                 connected ? "yes" : "no");
         if (connected)
            fprintf(stderr, " [%s]", conn);
         fprintf(stderr, "\n");

         if (nvidia) {
            if (connected)
               nvidia_connected = true;
            if (boot)
               boot_vga_nvidia = true;
         } else {
            other_present = true;
            if (connected)
               other_connected = true;
            if (boot)
               boot_vga_other = true;
         }
      }
      closedir(dir);
   }

   const char *env = getenv("WAYDROID_NVIDIA_PRESENT");
   const char *reason;
   enum present_mode mode = PRESENT_BLOCK_LINEAR;

   if (env && (!strcmp(env, "udmabuf") || !strcmp(env, "cpu-udmabuf"))) {
      mode = PRESENT_UDMABUF;
      reason = "WAYDROID_NVIDIA_PRESENT override (kernel udmabuf fallback)";
   } else if (env && (!strcmp(env, "linear") || !strcmp(env, "1") ||
                      !strcmp(env, "nvidia-linear"))) {
      mode = PRESENT_NVIDIA_LINEAR;
      reason = "WAYDROID_NVIDIA_PRESENT override";
   } else if (env && (!strcmp(env, "block-linear") || !strcmp(env, "0") ||
                      !strcmp(env, "block"))) {
      mode = PRESENT_BLOCK_LINEAR;
      reason = "WAYDROID_NVIDIA_PRESENT override";
   } else if (other_connected && !nvidia_connected) {
      mode = PRESENT_NVIDIA_LINEAR;
      reason = "non-NVIDIA GPU has the only connected display (iGPU compositor)";
   } else if (other_connected && nvidia_connected && boot_vga_other &&
              !boot_vga_nvidia) {
      mode = PRESENT_NVIDIA_LINEAR;
      reason = "both GPUs have displays; boot_vga is the iGPU";
   } else if (other_present && !nvidia_connected) {
      mode = PRESENT_NVIDIA_LINEAR;
      reason = "NVIDIA has no connected display; another GPU is present";
   } else if (other_present) {
      mode = PRESENT_BLOCK_LINEAR;
      reason = "NVIDIA has a connected display (compositor assumed on NVIDIA)";
   } else {
      mode = PRESENT_BLOCK_LINEAR;
      reason = "NVIDIA is the only GPU with a connected display";
   }

   g_present_mode = mode;
   fprintf(stderr, "vtest_gpu_alloc:   present=%s  reason=%s\n",
           mode == PRESENT_NVIDIA_LINEAR ? "nvidia-linear"
           : mode == PRESENT_UDMABUF     ? "udmabuf"
                                         : "block-linear",
           reason);
   fprintf(stderr,
           "vtest_gpu_alloc:   override: WAYDROID_NVIDIA_PRESENT=linear|block-linear|udmabuf\n");
   fprintf(stderr, "vtest_gpu_alloc: ===\n");
}

static void
log_alloc(const char *kind, uint32_t w, uint32_t h, uint32_t fmt,
          const char *path, const char *place, uint64_t modifier,
          uint32_t stride, uint64_t size, int ret)
{
   unsigned i = __sync_fetch_and_add(&g_alloc_log_n, 1);
   if (i >= 16 && (i % 128) != 0)
      return;
   if (ret) {
      fprintf(stderr,
              "vtest_gpu_alloc: %s %ux%u %s path=%s FAILED ret=%d (#%u)\n",
              kind, w, h, vtest_format_name(fmt), path, ret, i);
      return;
   }
   /* place= reports the memory class the driver actually picked (vram, bar1
    * or sysmem), not what was requested: the old hostvis= logged the request
    * and hid both the issue #7 BAR1 selection and the Xid-13 block-linear
    * sysmem regression behind a green-looking line. */
   fprintf(stderr,
           "vtest_gpu_alloc: %s %ux%u %s path=%s place=%s modifier=0x%llx stride=%u size=%llu (#%u)\n",
           kind, w, h, vtest_format_name(fmt), path, place,
           (unsigned long long)modifier, stride, (unsigned long long)size, i);
}

struct alloc_vk {
   void *lib;
   PFN_vkGetInstanceProcAddr get_proc;
   VkInstance instance;
   VkPhysicalDevice physical_device;
   VkDevice device;
   VkPhysicalDeviceMemoryProperties mem_props;

   PFN_vkCreateImage CreateImage;
   PFN_vkDestroyImage DestroyImage;
   PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
   PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout;
   PFN_vkAllocateMemory AllocateMemory;
   PFN_vkFreeMemory FreeMemory;
   PFN_vkBindImageMemory BindImageMemory;
   PFN_vkGetMemoryFdKHR GetMemoryFdKHR;
   PFN_vkGetPhysicalDeviceFormatProperties2 GetPhysicalDeviceFormatProperties2;
   PFN_vkGetImageDrmFormatModifierPropertiesEXT GetImageDrmFormatModifierPropertiesEXT;
};

static struct alloc_vk alloc_vk;
static pthread_mutex_t alloc_vk_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool alloc_vk_failed;
static struct timespec alloc_vk_last_fail;

static int
vtest_udmabuf_alloc(uint32_t width, uint32_t height, uint32_t drm_format,
                    uint32_t *out_stride, uint64_t *out_size, int *out_fd);

static VkFormat
drm_format_to_vk(uint32_t drm_format)
{
   switch (drm_format) {
   case VTEST_FORMAT_R8:
      return VK_FORMAT_R8_UNORM;
   case VTEST_FORMAT_RGB565:
      return VK_FORMAT_R5G6B5_UNORM_PACK16;
   case VTEST_FORMAT_XRGB8888:
   case VTEST_FORMAT_ARGB8888:
      return VK_FORMAT_B8G8R8A8_UNORM;
   case VTEST_FORMAT_XBGR8888:
   case VTEST_FORMAT_ABGR8888:
      return VK_FORMAT_R8G8B8A8_UNORM;
   case VTEST_FORMAT_ABGR2101010:
   case VTEST_FORMAT_XBGR2101010:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
   case VTEST_FORMAT_ABGR16161616F:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
   case VTEST_FORMAT_NV12:
   case VTEST_FORMAT_NV21:
   case VTEST_FORMAT_YVU420:
   case VTEST_FORMAT_YVU420_ANDROID:
   case VTEST_FORMAT_FLEX_YCBCR_420_888:
      /* minigbm's Android YV12 / flexible 420 fourccs. One NV12 VkImage is a
       * valid flexible-420 layout; YVU chroma order is a software-decoder
       * concern, not an allocation one (issue #16). */
      return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
   case VTEST_FORMAT_P010:
      return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
   default:
      return VK_FORMAT_UNDEFINED;
   }
}

static uint32_t
drm_format_bpp(uint32_t drm_format)
{
   switch (drm_format) {
   case VTEST_FORMAT_R8:
   case VTEST_FORMAT_NV12:
   case VTEST_FORMAT_NV21:
   case VTEST_FORMAT_YVU420:
   case VTEST_FORMAT_YVU420_ANDROID:
   case VTEST_FORMAT_FLEX_YCBCR_420_888:
      return 1;
   case VTEST_FORMAT_RGB565:
   case VTEST_FORMAT_P010:
      return 2;
   case VTEST_FORMAT_ABGR16161616F:
      return 8;
   default:
      return 4;
   }
}

static int
alloc_vk_init_locked(void)
{
   struct alloc_vk *vk = &alloc_vk;

   if (vk->device)
      return 0;
   if (alloc_vk_failed) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec - alloc_vk_last_fail.tv_sec < 5)
         return -ENODEV;
   }

   vk->lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!vk->lib) {
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }
   *(void **)&vk->get_proc = dlsym(vk->lib, "vkGetInstanceProcAddr");
   if (!vk->get_proc) {
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }

#define GET_GLOBAL(name) PFN_vk##name name = (PFN_vk##name)vk->get_proc(NULL, "vk" #name)
#define GET_INST(name) PFN_vk##name name = (PFN_vk##name)vk->get_proc(vk->instance, "vk" #name)

   GET_GLOBAL(CreateInstance);
   if (!CreateInstance) {
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }

   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "vtest-gpu-alloc",
      .apiVersion = VK_API_VERSION_1_1,
   };
   const VkInstanceCreateInfo inst_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   if (CreateInstance(&inst_info, NULL, &vk->instance) != VK_SUCCESS) {
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }

   GET_INST(EnumeratePhysicalDevices);
   GET_INST(GetPhysicalDeviceProperties);
   GET_INST(EnumerateDeviceExtensionProperties);
   GET_INST(GetPhysicalDeviceMemoryProperties);
   GET_INST(CreateDevice);
   GET_INST(GetDeviceProcAddr);

   uint32_t count = 0;
   if (EnumeratePhysicalDevices(vk->instance, &count, NULL) < 0 || !count) {
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }

   VkPhysicalDevice *devices = malloc(count * sizeof(*devices));
   if (!devices) {
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENOMEM;
   }
   if (EnumeratePhysicalDevices(vk->instance, &count, devices) != VK_SUCCESS) {
      free(devices);
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }

   /* prefer the device named by VTEST_ALLOC_GPU, else first discrete with
    * dma_buf export */
   const char *want = getenv("VTEST_ALLOC_GPU");
   VkPhysicalDevice best = VK_NULL_HANDLE;
   int best_score = -1;
   for (uint32_t i = 0; i < count; i++) {
      VkPhysicalDeviceProperties props;
      GetPhysicalDeviceProperties(devices[i], &props);

      bool has_dma_buf = false, has_modifier = false;
      uint32_t ext_count = 0;
      EnumerateDeviceExtensionProperties(devices[i], NULL, &ext_count, NULL);
      if (ext_count > 0) {
         VkExtensionProperties *exts = malloc(ext_count * sizeof(*exts));
         if (exts) {
            if (EnumerateDeviceExtensionProperties(devices[i], NULL, &ext_count, exts) == VK_SUCCESS) {
               for (uint32_t j = 0; j < ext_count; j++) {
                  if (!strcmp(exts[j].extensionName, "VK_EXT_external_memory_dma_buf"))
                     has_dma_buf = true;
                  if (!strcmp(exts[j].extensionName, "VK_EXT_image_drm_format_modifier"))
                     has_modifier = true;
               }
            }
            free(exts);
         }
      }
      if (!has_dma_buf || !has_modifier)
         continue;

      int score = 0;
      if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
         score += 2;
      if (want && strstr(props.deviceName, want))
         score += 10;
      if (score > best_score) {
         best_score = score;
         best = devices[i];
      }
   }
   free(devices);
   if (best == VK_NULL_HANDLE) {
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }
   vk->physical_device = best;
   GetPhysicalDeviceMemoryProperties(best, &vk->mem_props);

   {
      VkPhysicalDeviceProperties props;
      GetPhysicalDeviceProperties(best, &props);
      fprintf(stderr, "vtest_gpu_alloc: allocating on \"%s\"\n", props.deviceName);
   }

   const float prio = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   const char *dev_exts[] = {
      "VK_KHR_external_memory_fd",
      "VK_EXT_external_memory_dma_buf",
      "VK_EXT_image_drm_format_modifier",
   };
   const VkDeviceCreateInfo dev_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = sizeof(dev_exts) / sizeof(dev_exts[0]),
      .ppEnabledExtensionNames = dev_exts,
   };
   if (CreateDevice(best, &dev_info, NULL, &vk->device) != VK_SUCCESS) {
      vk->device = VK_NULL_HANDLE;
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }

#define GET_DEV(name) \
   vk->name = (PFN_vk##name)GetDeviceProcAddr(vk->device, "vk" #name)
   GET_DEV(CreateImage);
   GET_DEV(DestroyImage);
   GET_DEV(GetImageMemoryRequirements);
   GET_DEV(GetImageSubresourceLayout);
   GET_DEV(AllocateMemory);
   GET_DEV(FreeMemory);
   GET_DEV(BindImageMemory);
   GET_DEV(GetMemoryFdKHR);
   GET_DEV(GetImageDrmFormatModifierPropertiesEXT);
   vk->GetPhysicalDeviceFormatProperties2 =
      (PFN_vkGetPhysicalDeviceFormatProperties2)vk->get_proc(
         vk->instance, "vkGetPhysicalDeviceFormatProperties2");
#undef GET_DEV
#undef GET_INST
#undef GET_GLOBAL

   if (!vk->GetMemoryFdKHR || !vk->GetPhysicalDeviceFormatProperties2 ||
       !vk->GetImageDrmFormatModifierPropertiesEXT) {
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }

   alloc_vk_failed = false;
   return 0;
}

/* Memory class actually backing a successful alloc, for place= logging. */
static const char *
mem_place(VkMemoryPropertyFlags flags)
{
   if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
      return (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "bar1" : "vram";
   return "sysmem";
}

static int
vtest_gpu_alloc_image(uint32_t width, uint32_t height, uint32_t drm_format,
                      bool linear, bool host_visible, uint32_t *out_stride,
                      uint64_t *out_modifier, uint64_t *out_size, int *out_fd,
                      const char **out_place)
{
   const VkFormat format = drm_format_to_vk(drm_format);
   if (format == VK_FORMAT_UNDEFINED)
      return -EINVAL;
   pthread_mutex_lock(&alloc_vk_mutex);

   struct alloc_vk *vk = &alloc_vk;
   int ret = alloc_vk_init_locked();
   if (ret) {
      pthread_mutex_unlock(&alloc_vk_mutex);
      return ret;
   }

   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   VkDrmFormatModifierPropertiesEXT *props = NULL;
   uint64_t *mod_candidates = NULL;
   ret = -EINVAL;

   /* NVIDIA's EGL treats DRM_FORMAT_MOD_LINEAR dmabufs as external-only
    * (unbindable as GL_TEXTURE_2D), which KWin cannot display. Allocate
    * with the driver's real (block-linear) modifiers instead: enumerate
    * what the format supports and let the driver pick.
    */
   uint32_t mod_count = 0;
   if (linear) {
      mod_candidates = malloc(sizeof(*mod_candidates));
      if (!mod_candidates)
         goto out;
      /* CPU-mappable: explicit LINEAR so the guest can mmap and compute
       * pixel offsets; NVIDIA dma_bufs mmap fine from any memory type */
      mod_candidates[mod_count++] = 0; /* DRM_FORMAT_MOD_LINEAR */
   } else {
      VkDrmFormatModifierPropertiesListEXT mod_list = {
         .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
      };
      VkFormatProperties2 fmt_props = {
         .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
         .pNext = &mod_list,
      };
      vk->GetPhysicalDeviceFormatProperties2(vk->physical_device, format,
                                             &fmt_props);
      uint32_t mod_count_avail = mod_list.drmFormatModifierCount;
      if (mod_count_avail > 0) {
         props = malloc(mod_count_avail * sizeof(*props));
         mod_candidates = malloc(mod_count_avail * sizeof(*mod_candidates));
         if (props && mod_candidates) {
            mod_list.pDrmFormatModifierProperties = props;
            vk->GetPhysicalDeviceFormatProperties2(vk->physical_device, format,
                                                   &fmt_props);

            /* Multi-planar YUV is never a color attachment (issue #16). */
            const bool yuv = vtest_drm_format_is_yuv(drm_format);
            const VkFormatFeatureFlags need = yuv
               ? (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                  VK_FORMAT_FEATURE_TRANSFER_DST_BIT)
               : (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
            const uint32_t expected_planes = yuv ? 2 : 1;

            for (uint32_t i = 0; i < mod_list.drmFormatModifierCount; i++) {
               if (props[i].drmFormatModifierPlaneCount == expected_planes &&
                   (props[i].drmFormatModifierTilingFeatures & need) == need &&
                   props[i].drmFormatModifier != 0)
                  mod_candidates[mod_count++] = props[i].drmFormatModifier;
            }
         }
      }
   }
   if (!mod_count || !mod_candidates)
      goto out;

   const VkExternalMemoryImageCreateInfo ext_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkImageDrmFormatModifierListCreateInfoEXT mod_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
      .pNext = &ext_info,
      .drmFormatModifierCount = mod_count,
      .pDrmFormatModifiers = mod_candidates,
   };
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &mod_info,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = { width, height, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = vtest_drm_format_is_yuv(drm_format)
                  ? (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                  : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT),
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   {
      VkResult vr = vk->CreateImage(vk->device, &image_info, NULL, &image);
      if (vr != VK_SUCCESS) {
         fprintf(stderr,
                 "vtest_gpu_alloc: CreateImage %ux%u %s linear=%d failed vk=%d\n",
                 width, height, vtest_format_name(drm_format), (int)linear,
                 (int)vr);
         goto out;
      }
   }

   VkMemoryRequirements reqs;
   vk->GetImageMemoryRequirements(vk->device, image, &reqs);

   uint32_t mem_type = UINT32_MAX;
   const VkMemoryPropertyFlags want =
      host_visible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                   : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   for (uint32_t i = 0; i < vk->mem_props.memoryTypeCount; i++) {
      if ((reqs.memoryTypeBits & (1u << i)) &&
          (vk->mem_props.memoryTypes[i].propertyFlags & want) == want) {
         mem_type = i;
         break;
      }
   }
   if (mem_type == UINT32_MAX)
      mem_type = ffs(reqs.memoryTypeBits) - 1;
   if (reqs.memoryTypeBits == 0)
      goto out;

   /* Issue #7: a LINEAR dmabuf that lands in DEVICE_LOCAL (VRAM) cannot be
    * sampled by a cross-vendor iGPU compositor (AMD/Intel read it over BAR1
    * and get black on RTD3 hybrids). On the nvidia-linear hybrid path only,
    * prefer a pure system-memory type (HOST_VISIBLE|HOST_COHERENT without
    * DEVICE_LOCAL): NVIDIA still exports and still renders cleanly, and the
    * iGPU imports memory it can actually read.
    *
    * Scope note: this must NOT touch the block-linear discrete-compositor
    * path. NVIDIA block-linear layout metadata lives in VRAM; placing those
    * images in sysmem triggers Xid 13 TEX LAYOUT device losses under heavy
    * sampling (regression observed on GTX 1080 with AnTuTu UE4). BAR1 types
    * (DEVICE_LOCAL|HOST_VISIBLE) are also rejected here: they still read
    * over PCIe, which is exactly the failure mode being avoided. */
   if (linear && !host_visible) {
      for (uint32_t i = 0; i < vk->mem_props.memoryTypeCount; i++) {
         const VkMemoryPropertyFlags flags =
            vk->mem_props.memoryTypes[i].propertyFlags;
         if ((reqs.memoryTypeBits & (1u << i)) &&
             !(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
             (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mem_type = i;
            break;
         }
      }
   }

   const VkExportMemoryAllocateInfo export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkMemoryDedicatedAllocateInfo dedicated_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = &export_info,
      .image = image,
   };
   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &dedicated_info,
      .allocationSize = reqs.size,
      .memoryTypeIndex = mem_type,
   };
   {
      VkResult vr = vk->AllocateMemory(vk->device, &alloc_info, NULL, &memory);
      if (vr != VK_SUCCESS) {
         fprintf(stderr,
                 "vtest_gpu_alloc: AllocateMemory %ux%u %s hostvis=%d failed vk=%d\n",
                 width, height, vtest_format_name(drm_format), (int)host_visible,
                 (int)vr);
         memory = VK_NULL_HANDLE;
         goto out;
      }
   }
   if (vk->BindImageMemory(vk->device, image, memory, 0) != VK_SUCCESS)
      goto out;

   VkImageDrmFormatModifierPropertiesEXT chosen = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
   };
   if (vk->GetImageDrmFormatModifierPropertiesEXT(vk->device, image, &chosen) !=
       VK_SUCCESS)
      goto out;

   const VkImageSubresource subres = {
      .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
   };
   VkSubresourceLayout layout;
   vk->GetImageSubresourceLayout(vk->device, image, &subres, &layout);

   const VkMemoryGetFdInfoKHR fd_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
      .memory = memory,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   int fd = -1;
   if (vk->GetMemoryFdKHR(vk->device, &fd_info, &fd) != VK_SUCCESS || fd < 0)
      goto out;

   *out_stride = (uint32_t)layout.rowPitch;
   *out_modifier = chosen.drmFormatModifier;
   *out_size = reqs.size;
   *out_fd = fd;
   if (out_place)
      *out_place = mem_place(vk->mem_props.memoryTypes[mem_type].propertyFlags);
   ret = 0;

out:
   free(props);
   free(mod_candidates);
   if (image)
      vk->DestroyImage(vk->device, image, NULL);
   if (memory)
      vk->FreeMemory(vk->device, memory, NULL);
   pthread_mutex_unlock(&alloc_vk_mutex);
   return ret;
}

int
vtest_gpu_alloc_gpu(uint32_t width, uint32_t height, uint32_t drm_format,
                    uint32_t *out_stride, uint64_t *out_modifier,
                    uint64_t *out_size, int *out_fd)
{
   pthread_once(&g_present_once, present_init);

   if (g_present_mode == PRESENT_UDMABUF) {
      /* Explicit compatibility path: kernel udmabuf, no NVIDIA allocation. */
      return vtest_udmabuf_alloc(width, height, drm_format, out_stride,
                                 out_size, out_fd);
   }

   const bool linear = (g_present_mode == PRESENT_NVIDIA_LINEAR);
   const char *path = linear ? "nvidia-linear" : "block-linear";
   const char *place = "vram";
   int ret = vtest_gpu_alloc_image(width, height, drm_format, linear, false,
                                   out_stride, out_modifier, out_size, out_fd,
                                   &place);
   if (ret && linear) {
      /* LINEAR refused outright; retry the same tiling in the other heap.
       * The sysmem preference inside alloc_image already keeps this path out
       * of VRAM and BAR1 when a pure system-memory type exists. */
      ret = vtest_gpu_alloc_image(width, height, drm_format, true, true,
                                  out_stride, out_modifier, out_size, out_fd,
                                  &place);
   }
   log_alloc("gpu", width, height, drm_format, path, place,
             ret ? 0 : *out_modifier, ret ? 0 : *out_stride,
             ret ? 0 : *out_size, ret);
   return ret;
}

/* Kernel udmabuf over a shrink-sealed memfd — the vendor-neutral fallback
 * both present paths land on when NVIDIA allocation is unavailable or
 * explicitly overridden (WAYDROID_NVIDIA_PRESENT=udmabuf). */
static int
vtest_udmabuf_alloc(uint32_t width, uint32_t height, uint32_t drm_format,
                    uint32_t *out_stride, uint64_t *out_size, int *out_fd)
{
   uint32_t stride = 0;
   uint64_t size = 0;

   if (vtest_drm_format_is_yuv8_420(drm_format)) {
      stride = (uint32_t)ALLOC_ALIGN((uint64_t)width, 256);
      size = ALLOC_ALIGN((uint64_t)stride * height * 3 / 2, 4096);
   } else if (drm_format == VTEST_FORMAT_P010) {
      stride = (uint32_t)ALLOC_ALIGN((uint64_t)width * 2, 256);
      size = ALLOC_ALIGN((uint64_t)stride * height * 3 / 2, 4096);
   } else {
      const uint32_t bpp = drm_format_bpp(drm_format);
      stride = (uint32_t)ALLOC_ALIGN((uint64_t)width * bpp, 256);
      size = ALLOC_ALIGN((uint64_t)stride * height, 4096);
   }

   int memfd = memfd_create("vtest-gralloc", MFD_ALLOW_SEALING | MFD_CLOEXEC);
   if (memfd < 0)
      return -errno;
   if (ftruncate(memfd, (off_t)size) < 0) {
      int err = errno;
      close(memfd);
      return -err;
   }
   /* udmabuf requires the memfd to be shrink-sealed */
   if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) {
      int err = errno;
      fprintf(stderr, "vtest_gpu_alloc: F_SEAL_SHRINK failed (%s)\n", strerror(err));
      close(memfd);
      return -err;
   }

   int ufd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
   if (ufd < 0) {
      int err = errno;
      if (err == EACCES || err == EPERM)
         fprintf(stderr,
                 "vtest_gpu_alloc: cannot open /dev/udmabuf (%s). Install "
                 "70-waydroid-nvidia.rules (udev uaccess) and re-login, or "
                 "grant this user access to /dev/udmabuf.\n",
                 strerror(err));
      close(memfd);
      return -err;
   }
   struct udmabuf_create create = {
      .memfd = (uint32_t)memfd,
      .flags = UDMABUF_FLAGS_CLOEXEC,
      .offset = 0,
      .size = size,
   };
   int dmabuf = ioctl(ufd, UDMABUF_CREATE, &create);
   /* Capture errno before close(): a successful close does not clear it, but a
    * failing one overwrites it, so the returned code would describe the wrong
    * call -- and could even be 0, reporting success with *out_fd never set. */
   int ioctl_err = dmabuf < 0 ? errno : 0;
   close(ufd);
   close(memfd);
   if (dmabuf < 0)
      return -ioctl_err;

   *out_stride = stride;
   *out_size = size;
   *out_fd = dmabuf;
   log_alloc("cpu", width, height, drm_format, "udmabuf-linear", "sysmem", 0,
             stride, size, 0);
   return 0;
}

int
vtest_gpu_alloc_cpu(uint32_t width, uint32_t height, uint32_t drm_format,
                    uint32_t *out_stride, uint64_t *out_size, int *out_fd)
{
   /* NVIDIA renders corruptly into kernel-allocated (udmabuf) LINEAR memory
    * but cleanly into its own exported LINEAR memory (host probes: residual
    * -9/dips=141 vs -0.03/dips=0). Allocate CPU-mappable buffers as NVIDIA's
    * own LINEAR memory so guest renders into them are clean. Fall back to a
    * plain udmabuf when the format isn't renderable (non-whitelisted).
    * Upstream Shiro836#12. */
   pthread_once(&g_present_once, present_init);

   if (g_present_mode == PRESENT_UDMABUF)
      return vtest_udmabuf_alloc(width, height, drm_format, out_stride,
                                 out_size, out_fd);

   /* WAYDROID_NVIDIA_CPU_LINEAR=0 forces the udmabuf fallback so the
    * NVIDIA-LINEAR as render-target hypothesis (Xid 69 regression) can be
    * A/B tested without a rebuild. */
   const char *cpu_linear_env = getenv("WAYDROID_NVIDIA_CPU_LINEAR");
   bool use_nvidia_linear = !cpu_linear_env || cpu_linear_env[0] != '0';

   if (use_nvidia_linear) {
      uint64_t modifier = 0;
      const char *place = "sysmem";
      if (vtest_gpu_alloc_image(width, height, drm_format, true, true, out_stride,
                                &modifier, out_size, out_fd, &place) == 0) {
         log_alloc("cpu", width, height, drm_format, "nvidia-linear-hostvis",
                   place, modifier, *out_stride, *out_size, 0);
         return 0;
      }
   }

   /* Not an error when forced: WAYDROID_NVIDIA_CPU_LINEAR=0 pins this path
    * to udmabuf for the Xid-69 A/B test (config cpu-linear-off.conf). */
   if (cpu_linear_env)
      fprintf(stderr,
              "vtest_gpu_alloc: cpu NVIDIA LINEAR disabled for %ux%u %s "
              "(WAYDROID_NVIDIA_CPU_LINEAR=0), using udmabuf\n",
              width, height, vtest_format_name(drm_format));
   else
      fprintf(stderr,
              "vtest_gpu_alloc: cpu NVIDIA LINEAR failed for %ux%u %s, "
              "falling back to udmabuf\n",
              width, height, vtest_format_name(drm_format));

   return vtest_udmabuf_alloc(width, height, drm_format, out_stride,
                              out_size, out_fd);
}
