/* VK_IMAGE_VIEW_TYPE_3D view of a 2D image.
 *
 * 3DMark Wild Life Extreme (Android, 2.7.5093) creates this view on its second
 * scene load in one process: the GPU particle system binds a 32x32
 * VK_FORMAT_R8G8B8A8_SRGB 2D image (6 mips, 1 layer, TRANSFER_DST|SAMPLED,
 * optimal tiling) to a 3D texture slot and asks for
 *   { viewType = VK_IMAGE_VIEW_TYPE_3D, format = R8G8B8A8_SRGB,
 *     identity swizzle, COLOR, levels 0..6, layers 0..1 }.
 * The first load created a real 3D image (128x128x128 R8G8B8A8_UNORM) for that
 * slot; the second load creates none and passes the 2D image instead.
 *
 * The Vulkan spec forbids this view (image/view type compatibility table in
 * "Image Views": a VK_IMAGE_TYPE_2D image allows 2D, 2D_ARRAY, CUBE and
 * CUBE_ARRAY views only). NVIDIA's own Vulkan driver accepts it. Mesa 26.2.5
 * nil (src/nouveau/nil/descriptor.rs:261, normalize_extent) asserts
 * `image.dim == ImageDim::_3D`; the panic cannot unwind out of the extern "C"
 * nil_image_view_descriptor and the process aborts.
 *
 * Case 2 asks for a 3D view of a 4x multisampled 2D image, which cannot be
 * described as a depth-1 3D image: the driver must not abort (NVK returns
 * VK_ERROR_UNKNOWN and logs why; any result is accepted).
 *
 * Exit status: 0 = case 1 created its view and neither case killed the
 * process; 1 = case 1's vkCreateImageView returned an error; the pre-fix NVK
 * aborts in case 1 (SIGABRT, "assertion failed: image.dim == ImageDim::_3D").
 *
 * Real GPU. The device is selected by vendorID 0x10de and driverID
 * (MESA_NVK, or NVIDIA_PROPRIETARY for the stock comparison), never by a node
 * path. Build/run (repository root):
 *   cc -std=c11 -Wall -Wextra -Werror -I<mesa>/include \
 *     nvidia-prolapse/driver-source/native/mesa/tests/nvk-3d-view-of-2d-image.c -ldl \
 *     -o nvk-3d-view-of-2d-image
 *   LD_LIBRARY_PATH=<rust lib dir> ./nvk-3d-view-of-2d-image <nvk-host>/src/nouveau/vulkan/libvulkan_nouveau.so
 *   ./nvk-3d-view-of-2d-image libGLX_nvidia.so.0     (stock comparison)
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

#define VKCHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
   fprintf(stderr, "FATAL %s:%d %s -> %d\n", __FILE__, __LINE__, #call, r_); exit(2); } } while (0)

static PFN_vkCreateImage p_CreateImage;
static PFN_vkDestroyImage p_DestroyImage;
static PFN_vkGetImageMemoryRequirements p_GetImageMemoryRequirements;
static PFN_vkAllocateMemory p_AllocateMemory;
static PFN_vkFreeMemory p_FreeMemory;
static PFN_vkBindImageMemory p_BindImageMemory;
static PFN_vkCreateImageView p_CreateImageView;
static PFN_vkDestroyImageView p_DestroyImageView;

/* create + bind the image, create the view on it, destroy everything; returns vkCreateImageView's result */
static VkResult
one_view(VkDevice dev, VkPhysicalDevice pd, PFN_vkGetPhysicalDeviceMemoryProperties gpdmp,
         const VkImageCreateInfo *ci, VkImageViewCreateInfo *vci)
{
   VkImage img;
   VKCHECK(p_CreateImage(dev, ci, NULL, &img));
   VkMemoryRequirements req;
   p_GetImageMemoryRequirements(dev, img, &req);
   VkPhysicalDeviceMemoryProperties mp;
   gpdmp(pd, &mp);
   uint32_t type = 0;
   while (type < mp.memoryTypeCount && !(req.memoryTypeBits & (1u << type)))
      type++;
   VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                .memoryTypeIndex = type };
   VkDeviceMemory mem;
   VKCHECK(p_AllocateMemory(dev, &mai, NULL, &mem));
   VKCHECK(p_BindImageMemory(dev, img, mem, 0));
   vci->image = img;
   VkImageView view = VK_NULL_HANDLE;
   VkResult r = p_CreateImageView(dev, vci, NULL, &view);
   if (r == VK_SUCCESS)
      p_DestroyImageView(dev, view, NULL);
   p_DestroyImage(dev, img, NULL);
   p_FreeMemory(dev, mem, NULL);
   return r;
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: %s path/to/vulkan-icd.so\n", argv[0]);
      return 2;
   }
   void *icd = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!icd) {
      fprintf(stderr, "FATAL dlopen %s: %s\n", argv[1], dlerror());
      return 2;
   }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(icd, "vk_icdGetInstanceProcAddr");
   if (!gipa) {
      fprintf(stderr, "FATAL %s has no vk_icdGetInstanceProcAddr\n", argv[1]);
      return 2;
   }
   PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
   VkInstance inst;
   VKCHECK(create_instance(&ici, NULL, &inst));
#define I(name) PFN_vk##name i_##name = (PFN_vk##name)gipa(inst, "vk" #name)
   I(EnumeratePhysicalDevices); I(GetPhysicalDeviceProperties2); I(GetPhysicalDeviceMemoryProperties);
   I(CreateDevice); I(GetDeviceProcAddr); I(DestroyInstance);
#undef I
   uint32_t n = 8;
   VkPhysicalDevice pdevs[8];
   VKCHECK(i_EnumeratePhysicalDevices(inst, &n, pdevs));
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   for (uint32_t i = 0; i < n && !pd; i++) {
      VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
      VkPhysicalDeviceProperties2 pp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
      i_GetPhysicalDeviceProperties2(pdevs[i], &pp);
      if (pp.properties.vendorID == 0x10de &&
          (drv.driverID == VK_DRIVER_ID_MESA_NVK || drv.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY)) {
         pd = pdevs[i];
         printf("device: %s (%s %s)\n", pp.properties.deviceName, drv.driverName, drv.driverInfo);
      }
   }
   if (!pd) {
      fprintf(stderr, "FATAL no NVIDIA device with driverID MESA_NVK or NVIDIA_PROPRIETARY\n");
      return 2;
   }
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
                                   .pQueuePriorities = &prio };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
   VkDevice dev;
   VKCHECK(i_CreateDevice(pd, &dci, NULL, &dev));
#define D(name) p_##name = (PFN_vk##name)i_GetDeviceProcAddr(dev, "vk" #name); \
   if (!p_##name) { fprintf(stderr, "FATAL no vk" #name "\n"); return 2; }
   D(CreateImage) D(DestroyImage) D(GetImageMemoryRequirements) D(AllocateMemory) D(FreeMemory)
   D(BindImageMemory) D(CreateImageView) D(DestroyImageView)
   PFN_vkDestroyDevice p_DestroyDevice = (PFN_vkDestroyDevice)i_GetDeviceProcAddr(dev, "vkDestroyDevice");
#undef D

   /* 1: the image and view 3DMark uses: 32x32 R8G8B8A8_SRGB, full mip chain, sampled; must succeed */
   VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_SRGB, .extent = { 32, 32, 1 }, .mipLevels = 6, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImageViewCreateInfo vci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = VK_IMAGE_VIEW_TYPE_3D,
      .format = VK_FORMAT_R8G8B8A8_SRGB,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 6, 0, 1 },
   };
   VkResult r1 = one_view(dev, pd, i_GetPhysicalDeviceMemoryProperties, &ci, &vci);
   printf("case 1: VIEW_TYPE_3D of a 2D 32x32 R8G8B8A8_SRGB image, 6 levels -> %d\n", r1);

   /* 2: a 3D view of a 4x multisampled 2D image cannot be a depth-1 3D image: any outcome but an abort is
    * accepted (NVK returns an error and logs the reason; the stock driver's answer is printed) */
   ci.mipLevels = 1;
   ci.samples = VK_SAMPLE_COUNT_4_BIT;
   ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
   vci.subresourceRange.levelCount = 1;
   VkResult r2 = one_view(dev, pd, i_GetPhysicalDeviceMemoryProperties, &ci, &vci);
   printf("case 2: VIEW_TYPE_3D of a 4x multisampled 2D 32x32 image -> %d (process alive)\n", r2);

   p_DestroyDevice(dev, NULL);
   i_DestroyInstance(inst, NULL);
   if (r1 != VK_SUCCESS) {
      printf("FAIL nvk-3d-view-of-2d-image: case 1 vkCreateImageView -> %d\n", r1);
      return 1;
   }
   printf("PASS nvk-3d-view-of-2d-image\n");
   return 0;
}
