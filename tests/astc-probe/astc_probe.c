#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static const char *vn(VkResult r){
  switch(r){
    case VK_SUCCESS: return "SUCCESS";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OODM";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "OOHM";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "FORMAT_NOT_SUPPORTED";
    case VK_ERROR_INITIALIZATION_FAILED: return "INIT_FAILED";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "FEATURE_NOT_PRESENT";
    default: { static char b[32]; snprintf(b,sizeof b,"VkResult(%d)",r); return b; }
  }
}

struct cs { const char *name; VkFormat fmt; };

int main(void){
  setvbuf(stdout, NULL, _IONBF, 0);
  fprintf(stderr, "STEP 1 create instance\n");
  VkInstance inst;
  VkApplicationInfo ai;
  memset(&ai,0,sizeof ai);
  ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  ai.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici;
  memset(&ici,0,sizeof ici);
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &ai;
  VkResult r = vkCreateInstance(&ici, NULL, &inst);
  fprintf(stderr, "STEP 2 instance:%s\n", vn(r));
  if (r != VK_SUCCESS) return 1;

  uint32_t n = 0;
  r = vkEnumeratePhysicalDevices(inst, &n, NULL);
  fprintf(stderr, "STEP 3 devices n=%u r=%s\n", n, vn(r));
  if (n == 0) return 1;
  VkPhysicalDevice pd[8];
  if (n > 8) n = 8;
  vkEnumeratePhysicalDevices(inst, &n, pd);

  struct cs cases[] = {
    {"ASTC_4x4_UNORM",  VK_FORMAT_ASTC_4x4_UNORM_BLOCK},
    {"ASTC_4x4_SRGB",   VK_FORMAT_ASTC_4x4_SRGB_BLOCK},
    {"ASTC_6x6_UNORM",  VK_FORMAT_ASTC_6x6_UNORM_BLOCK},
    {"ASTC_6x6_SRGB",   VK_FORMAT_ASTC_6x6_SRGB_BLOCK},
    {"ASTC_8x8_UNORM",  VK_FORMAT_ASTC_8x8_UNORM_BLOCK},
    {"ETC2_RGB8",       VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK},
    {"BC1_RGBA_UNORM",  VK_FORMAT_BC1_RGBA_UNORM_BLOCK},
    {"RGBA8_UNORM",     VK_FORMAT_R8G8B8A8_UNORM},
  };

  for (uint32_t i = 0; i < n; i++) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd[i], &p);
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(pd[i], &f);
    printf("\n[dev %u] %s\n", i, p.deviceName);
    printf("  astcLDR=%d etc2=%d bc=%d\n",
           f.textureCompressionASTC_LDR, f.textureCompressionETC2, f.textureCompressionBC);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &qn, NULL);
    VkQueueFamilyProperties qp[16];
    if (qn > 16) qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &qn, qp);
    int q = -1;
    for (uint32_t k = 0; k < qn; k++)
      if (qp[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) { q = (int)k; break; }
    if (q < 0) for (uint32_t k = 0; k < qn; k++)
      if (qp[k].queueFlags & VK_QUEUE_COMPUTE_BIT) { q = (int)k; break; }
    if (q < 0) { printf("  no usable queue\n"); continue; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci,0,sizeof qci);
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = (uint32_t)q;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures want;
    memset(&want,0,sizeof want);
    want.textureCompressionASTC_LDR = f.textureCompressionASTC_LDR;
    want.textureCompressionETC2 = f.textureCompressionETC2;
    want.textureCompressionBC = f.textureCompressionBC;
    want.shaderStorageImageWriteWithoutFormat = f.shaderStorageImageWriteWithoutFormat;
    VkDeviceCreateInfo dci;
    memset(&dci,0,sizeof dci);
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &want;
    VkDevice dev;
    r = vkCreateDevice(pd[i], &dci, NULL, &dev);
    printf("  vkCreateDevice: %s\n", vn(r));
    if (r != VK_SUCCESS) continue;

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd[i], &mp);

    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
      VkFormatProperties fp;
      vkGetPhysicalDeviceFormatProperties(pd[i], cases[c].fmt, &fp);
      VkImageCreateInfo ci;
      memset(&ci,0,sizeof ci);
      ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      ci.imageType = VK_IMAGE_TYPE_2D;
      ci.format = cases[c].fmt;
      ci.extent.width = 2560;
      ci.extent.height = 1440;
      ci.extent.depth = 1;
      ci.mipLevels = 1;
      ci.arrayLayers = 1;
      ci.samples = VK_SAMPLE_COUNT_1_BIT;
      ci.tiling = VK_IMAGE_TILING_OPTIMAL;
      ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      printf("  -- %-16s optFeat=0x%-8x sampled=%d\n", cases[c].name, fp.optimalTilingFeatures,
             !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT));
      VkImage img;
      r = vkCreateImage(dev, &ci, NULL, &img);
      if (r != VK_SUCCESS) { printf("     createImage: %s\n", vn(r)); continue; }
      VkMemoryRequirements mr;
      vkGetImageMemoryRequirements(dev, img, &mr);
      printf("     size=%llu B (%.2f MiB) align=%llu typeBits=0x%x\n",
             (unsigned long long)mr.size, mr.size / 1048576.0,
             (unsigned long long)mr.alignment, mr.memoryTypeBits);
      if (mr.size == 0) {
        printf("     *** ZERO SIZE ***\n");
        vkDestroyImage(dev, img, NULL);
        continue;
      }
      int chosen = -1;
      for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
        if ((mr.memoryTypeBits & (1u << t)) &&
            (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { chosen = (int)t; break; }
      if (chosen < 0)
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
          if (mr.memoryTypeBits & (1u << t)) { chosen = (int)t; break; }
      if (chosen < 0) { printf("     no memory type\n"); vkDestroyImage(dev, img, NULL); continue; }
      printf("     memoryType=%d flags=0x%x\n", chosen, mp.memoryTypes[chosen].propertyFlags);
      VkMemoryAllocateInfo mai;
      memset(&mai,0,sizeof mai);
      mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      mai.allocationSize = mr.size;
      mai.memoryTypeIndex = (uint32_t)chosen;
      VkDeviceMemory mem;
      r = vkAllocateMemory(dev, &mai, NULL, &mem);
      if (r != VK_SUCCESS) { printf("     allocateMemory: %s\n", vn(r)); vkDestroyImage(dev, img, NULL); continue; }
      r = vkBindImageMemory(dev, img, mem, 0);
      printf("     bind: %s\n", vn(r));
      vkFreeMemory(dev, mem, NULL);
      vkDestroyImage(dev, img, NULL);
    }
    vkDestroyDevice(dev, NULL);
  }
  fprintf(stderr, "STEP 9 done\n");
  vkDestroyInstance(inst, NULL);
  return 0;
}
