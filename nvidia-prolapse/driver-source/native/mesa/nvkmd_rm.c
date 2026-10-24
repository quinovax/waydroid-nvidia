/* SPDX-License-Identifier: MIT
 * Native NVIDIA RM transport for Mesa NVK. Vulkan, NAK, NIL, MME and resource
 * lifetime policy stay in Mesa; only kernel memory/context transport lives here.
 * The Rust backend runs every call on the calling thread through public RM
 * interfaces and never loads NVIDIA userspace.
 */
#include "nvkmd_rm.h"
#include "nv_rm.h"
#include "nvk_device.h"
#include "util/cache_ops.h"
#include "util/u_dynarray.h"
#include "util/os_time.h"
#include "util/vma.h"
#include "vk_device.h"
#include "vk_log.h"
#include "vk_object.h"
#include "vk_sync.h"
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>

struct nvkmd_rm_pdev {
   struct nvkmd_pdev base;
   struct NvRmInfo info;
   char render_path[PATH_MAX];
   int fd;
   struct vk_sync_type sync_type;
   struct vk_sync_type sync_file_type;
   const struct vk_sync_type* sync_types[3];
};
struct nvkmd_rm_dev {
   struct nvkmd_dev base;
   struct NvRmDevice* rm;
   struct vk_device* vk;
   int fd;
   simple_mtx_t va_mutex;
   struct util_vma_heap heap;
};
struct nvkmd_rm_mem {
   struct nvkmd_mem base;
   struct NvRmAllocation allocation;
};
struct rm_binding {
   struct list_head link;
   uint64_t offset, size, mapping;
};
struct nvkmd_rm_va {
   struct nvkmd_va base;
   struct list_head bindings;
};
struct nvkmd_rm_ctx {
   struct nvkmd_ctx base;
   struct vk_device* vk;
   uint64_t channel;
   struct util_dynarray pending;
   struct util_dynarray waits; /* identities borrowed until submission accepts payloads */
   uint64_t last_token;
};
NVKMD_DECL_SUBCLASS(pdev, rm);
NVKMD_DECL_SUBCLASS(dev, rm);
NVKMD_DECL_SUBCLASS(mem, rm);
NVKMD_DECL_SUBCLASS(va, rm);
NVKMD_DECL_SUBCLASS(ctx, rm);

static VkResult
rm_result(struct vk_object_base* log, int32_t status, const char* operation)
{
   if (status == 0)
      return VK_SUCCESS;
   return vk_errorf(log, (VkResult)status, "Native Rust RM %s failed", operation);
}
static VkResult
unsupported(struct vk_object_base* log, const char* what)
{
   return vk_errorf(log,
                    VK_ERROR_FEATURE_NOT_PRESENT,
                    "Native Rust RM transport does not implement %s",
                    what);
}
/* Void cleanup callbacks cannot return an error to Mesa. Once their logical
 * ownership is gone, failed RM cleanup must publish loss immediately; the Rust
 * owner retains any incomplete lease for teardown. This notification never
 * waits on the GPU or takes backend locks, even under Mesa mapping locks. */
static VkResult
rm_mem_lost(struct nvkmd_rm_dev *dev, const char *operation, int32_t status)
{
   fprintf(stderr, "Native Rust RM %s failed; device lost: %d\n", operation, status);
   if (dev->vk)
      return vk_device_set_lost(dev->vk, "Native Rust RM %s failed: %d",
                                operation, status);
   /* Physical-device probing has no logical Vulkan device to notify. */
   nv_rm_mark_lost(dev->rm);
   return VK_ERROR_DEVICE_LOST;
}
/* The Rust backend publishes loss (atomic snapshot) before any slow teardown.
 * Read that snapshot directly: it is one load, no lock and no RM call. */
static VkResult
rm_wait_status(struct vk_device *vk)
{
   if (vk_device_is_lost_no_report(vk))
      return VK_ERROR_DEVICE_LOST;
   struct nvk_device *nvk = container_of(vk, struct nvk_device, vk);
   struct nvkmd_rm_dev *dev = nvkmd_rm_dev(nvk->nvkmd);
   if (nv_rm_is_lost(dev->rm))
      return vk_device_set_lost(vk,
         "Native Rust RM maintenance reported a failed GPU dependency or channel");
   return VK_SUCCESS;
}

/* Ordinary synchronization owns RM payloads. DRM fences are constructed only
 * at an explicit external-handle boundary, never for an internal queue signal. */
struct nvkmd_rm_sync {
   struct vk_sync base;
   uint64_t id;
};
static const struct vk_sync_type rm_sync_type;

static struct nvkmd_rm_dev *
rm_sync_device(struct vk_device *vk)
{
   struct nvk_device *nvk = container_of(vk, struct nvk_device, vk);
   return nvkmd_rm_dev(nvk->nvkmd);
}

static struct nvkmd_rm_sync *
rm_sync(struct vk_sync *sync)
{
   assert(sync->type->init == rm_sync_type.init);
   return container_of(sync, struct nvkmd_rm_sync, base);
}

static VkResult
rm_sync_init(struct vk_device *vk, struct vk_sync *sync, uint64_t initial)
{
   STATIC_ASSERT(VK_SYNC_IS_TIMELINE == NV_RM_SYNC_TIMELINE);
   STATIC_ASSERT(VK_SYNC_IS_SHAREABLE == NV_RM_SYNC_SHAREABLE);
   STATIC_ASSERT(VK_SYNC_IS_SHARED == NV_RM_SYNC_SHARED);
   return rm_result(&vk->base,
      nv_rm_sync_create(rm_sync_device(vk)->rm, sync->flags, initial,
                         &rm_sync(sync)->id), "create sync payload");
}

static void
rm_sync_finish(struct vk_device *vk, struct vk_sync *sync)
{
   /* Drops this payload reference on the caller; accepted GPU submissions
    * keep their own slot references until they retire. */
   int32_t status = nv_rm_sync_destroy_deferred(rm_sync_device(vk)->rm,
                                               rm_sync(sync)->id);
   if (status != VK_SUCCESS) {
      fprintf(stderr, "Native Rust RM sync disposal failed: %d\n", status);
      /* The registered loss hook only publishes/wakes (void callback, may run
       * under Mesa locks). */
      vk_device_set_lost(vk, "Native Rust RM sync disposal failed: %d",
                         status);
   }
}

static VkResult
rm_sync_signal(struct vk_device *vk, struct vk_sync *sync, uint64_t value)
{
   return rm_result(&vk->base,
      nv_rm_sync_signal(rm_sync_device(vk)->rm, rm_sync(sync)->id, value),
      "signal sync payload");
}

static VkResult
rm_sync_get_value(struct vk_device *vk, struct vk_sync *sync, uint64_t *value)
{
   return rm_result(&vk->base,
      nv_rm_sync_get_value(rm_sync_device(vk)->rm, rm_sync(sync)->id, value),
      "query sync value");
}

static VkResult
rm_sync_reset(struct vk_device *vk, struct vk_sync *sync)
{
   return rm_result(&vk->base,
      nv_rm_sync_reset(rm_sync_device(vk)->rm, rm_sync(sync)->id),
      "reset sync payload");
}

static VkResult
rm_sync_move(struct vk_device *vk, struct vk_sync *dst, struct vk_sync *src)
{
   return rm_result(&vk->base,
      nv_rm_sync_move(rm_sync_device(vk)->rm, rm_sync(dst)->id, rm_sync(src)->id,
                      !!(src->flags & VK_SYNC_IS_SHARED)), "move sync payload");
}

static VkResult
rm_sync_reserve_signal(struct vk_device *vk, struct vk_sync *dst,
                       struct vk_sync *src, bool *reserved)
{
   uint32_t done = 0;
   VkResult result = rm_result(&vk->base,
      nv_rm_sync_reserve(rm_sync_device(vk)->rm, rm_sync(dst)->id,
                         rm_sync(src)->id, &done), "reserve binary signal");
   *reserved = result == VK_SUCCESS && done;
   return result;
}

/* The deadline belongs to the Vulkan caller. A pending host timeline signal
 * may legitimately take arbitrarily long; elapsed batch age is not loss.
 * The backend waits on this thread (one leader polls the RM event; the
 * others are woken by it), with no sleep-poll loop. */
static VkResult
rm_sync_wait_many(struct vk_device *vk, uint32_t count,
                  const struct vk_sync_wait *waits,
                  enum vk_sync_wait_flags flags, uint64_t deadline)
{
   STATIC_ASSERT(VK_SYNC_WAIT_PENDING == NV_RM_SYNC_WAIT_PENDING);
   STATIC_ASSERT(VK_SYNC_WAIT_ANY == NV_RM_SYNC_WAIT_ANY);
   if (rm_wait_status(vk) != VK_SUCCESS)
      return VK_ERROR_DEVICE_LOST;
   if (!count)
      return VK_SUCCESS;
   /* Fence/semaphore waits are small: no heap allocation on the hot path. */
   struct NvRmSyncPoint stack_points[16];
   struct NvRmSyncPoint *points = stack_points;
   if (count > ARRAY_SIZE(stack_points)) {
      points = calloc(count, sizeof(*points));
      if (!points)
         return vk_error(vk, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   for (uint32_t i = 0; i < count; i++)
      points[i] = (struct NvRmSyncPoint){ rm_sync(waits[i].sync)->id,
                                         waits[i].wait_value };
   uint64_t now = os_time_get_nano();
   uint64_t timeout_ns = deadline == UINT64_MAX ? UINT64_MAX :
                         (deadline > now ? deadline - now : 0);
   int ret = nv_rm_sync_wait_many(rm_sync_device(vk)->rm, points, count, flags,
                                  timeout_ns);
   if (points != stack_points)
      free(points);
   /* Loss may have been published while this caller slept, including at the
    * same instant as completion. Mirror it into Mesa before returning. */
   if (rm_wait_status(vk) != VK_SUCCESS)
      return VK_ERROR_DEVICE_LOST;
   return ret == 2 ? VK_TIMEOUT :
      rm_result(&vk->base, ret, "wait sync completion");
}

static VkResult
rm_sync_import_opaque_fd(struct vk_device *vk, struct vk_sync *sync, int fd)
{
   return rm_result(&vk->base,
      nv_rm_sync_import_fd(rm_sync_device(vk)->rm, rm_sync(sync)->id, fd, 1),
      "import opaque sync fd");
}

static VkResult
rm_sync_export_opaque_fd(struct vk_device *vk, struct vk_sync *sync, int *fd)
{
   struct nvkmd_rm_dev *dev = rm_sync_device(vk);
   return rm_result(&vk->base,
      nv_rm_sync_export_fd(dev->rm, rm_sync(sync)->id, 1, dev->fd, fd),
      "export opaque sync fd");
}

static VkResult
rm_sync_import_sync_file(struct vk_device *vk, struct vk_sync *sync, int fd)
{
   return rm_result(&vk->base,
      nv_rm_sync_import_fd(rm_sync_device(vk)->rm, rm_sync(sync)->id, fd, 0),
      "import sync file");
}

static VkResult
rm_sync_export_sync_file(struct vk_device *vk, struct vk_sync *sync, int *fd)
{
   struct nvkmd_rm_dev *dev = rm_sync_device(vk);
   return rm_result(&vk->base,
      nv_rm_sync_export_fd(dev->rm, rm_sync(sync)->id, 0, dev->fd, fd),
      "export sync file");
}

static const struct vk_sync_type rm_sync_type = {
   .size = sizeof(struct nvkmd_rm_sync),
   .features = VK_SYNC_FEATURE_BINARY | VK_SYNC_FEATURE_TIMELINE |
      VK_SYNC_FEATURE_GPU_WAIT | VK_SYNC_FEATURE_CPU_WAIT |
      VK_SYNC_FEATURE_CPU_RESET | VK_SYNC_FEATURE_CPU_SIGNAL |
      VK_SYNC_FEATURE_WAIT_PENDING | VK_SYNC_FEATURE_WAIT_ANY |
      VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL,
   .init = rm_sync_init,
   .finish = rm_sync_finish,
   .signal = rm_sync_signal,
   .get_value = rm_sync_get_value,
   .reset = rm_sync_reset,
   .move = rm_sync_move,
   .reserve_signal = rm_sync_reserve_signal,
   .wait_many = rm_sync_wait_many,
   .import_opaque_fd = rm_sync_import_opaque_fd,
   .export_opaque_fd = rm_sync_export_opaque_fd,
};

/* Opaque shared state and sync-file copies have different lifetime contracts.
 * Separate Mesa types expose only same-handle import/export compatibility.
 * Both use the same RM payload operations for ordinary completion. */
static const struct vk_sync_type rm_sync_file_type = {
   .size = sizeof(struct nvkmd_rm_sync),
   .features = VK_SYNC_FEATURE_BINARY |
      VK_SYNC_FEATURE_GPU_WAIT | VK_SYNC_FEATURE_CPU_WAIT |
      VK_SYNC_FEATURE_CPU_RESET | VK_SYNC_FEATURE_CPU_SIGNAL |
      VK_SYNC_FEATURE_WAIT_PENDING | VK_SYNC_FEATURE_WAIT_ANY |
      VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL,
   .init = rm_sync_init,
   .finish = rm_sync_finish,
   .signal = rm_sync_signal,
   .reset = rm_sync_reset,
   .move = rm_sync_move,
   .reserve_signal = rm_sync_reserve_signal,
   .wait_many = rm_sync_wait_many,
   .import_sync_file = rm_sync_import_sync_file,
   .export_sync_file = rm_sync_export_sync_file,
};

static void
rm_pdev_destroy(struct nvkmd_pdev* base)
{
   struct nvkmd_rm_pdev* pdev = nvkmd_rm_pdev(base);
   close(pdev->fd);
   free(pdev);
}
static uint64_t
rm_vram_used(struct nvkmd_pdev* base)
{
   (void)base; /* Not advertised: no fabricated budget query. */
   return 0;
}
static VkResult
rm_check_status(struct vk_device *vk)
{
   struct nvk_device *nvk = container_of(vk, struct nvk_device, vk);
   struct nvkmd_rm_dev *dev = nvkmd_rm_dev(nvk->nvkmd);
   if (nv_rm_check_status(dev->rm) == VK_SUCCESS)
      return VK_SUCCESS;
   return vk_device_set_lost(vk, "Native Rust RM reported a failed GPU dependency or channel");
}

/* Device loss may be published while Mesa holds a submit mutex. Rust publishes
 * its atomic snapshot and wakes caller waiters (condition variable + eventfd).
 * It never waits for GPU progress, takes backend submission/resource locks, or
 * issues RM calls here; channel teardown happens on later calls/close. */
static void
rm_notify_lost(struct vk_device *vk, void *data)
{
   (void)vk;
   nv_rm_mark_lost(data);
}

static VkResult
rm_dev_create(struct nvkmd_pdev* base, struct vk_object_base* log, struct nvkmd_dev** out)
{
   *out = NULL;
   struct nvkmd_rm_pdev* pdev = nvkmd_rm_pdev(base);
   struct nvkmd_rm_dev* dev = calloc(1, sizeof(*dev));
   if (!dev)
      return vk_error(log, VK_ERROR_OUT_OF_HOST_MEMORY);
   dev->fd = open(pdev->render_path, O_RDWR | O_CLOEXEC);
   if (dev->fd < 0) {
      free(dev);
      return vk_error(log, VK_ERROR_INITIALIZATION_FAILED);
   }
   struct NvRmInfo info;
   VkResult result = rm_result(log,
                               nv_rm_open(pdev->info.pci_domain,
                                          pdev->info.pci_bus,
                                          pdev->info.pci_device,
                                          pdev->info.pci_function,
                                          &dev->rm,
                                          &info),
                               "open device");
   if (result != VK_SUCCESS) {
      if (dev->rm)
         nv_rm_close(dev->rm);
      goto fail_fd;
   }
   dev->base.ops = &nvkmd_rm_dev_ops;
   dev->base.pdev = base;
   dev->base.va_start = NV_RM_VA_START;
   dev->base.va_end = NV_RM_VA_END;
   list_inithead(&dev->base.mems);
   simple_mtx_init(&dev->base.mems_mutex, mtx_plain);
   simple_mtx_init(&dev->va_mutex, mtx_plain);
   util_vma_heap_init(&dev->heap, NV_RM_VA_START, NV_RM_VA_END - NV_RM_VA_START);
   *out = &dev->base;
   /* Physical-device probing also opens a temporary KMD device; its log
    * object has no logical device or Vulkan status hook. */
   if (log && log->device) {
      dev->vk = log->device;
      dev->vk->check_status = rm_check_status;
      dev->vk->notify_lost_data = dev->rm;
      dev->vk->notify_lost = rm_notify_lost;
   }
   return VK_SUCCESS;

fail_fd:
   close(dev->fd);
   free(dev);
   return result;
}
const struct nvkmd_pdev_ops nvkmd_rm_pdev_ops = {
   .destroy = rm_pdev_destroy,
   .get_vram_used = rm_vram_used,
   .create_dev = rm_dev_create,
};

VkResult
nvkmd_rm_try_create_pdev(struct _drmDevice* drm,
                         struct vk_object_base* log,
                         enum nvk_debug debug,
                         struct nvkmd_pdev** out)
{
   *out = NULL;
   if (drm->bustype != DRM_BUS_PCI ||
       drm->deviceinfo.pci->vendor_id != NVIDIA_VENDOR_ID ||
       !(drm->available_nodes & (1 << DRM_NODE_RENDER)))
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   int fd = open(drm->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
   if (fd < 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   drmVersionPtr version = drmGetVersion(fd);
   bool native = version && version->name && strcmp(version->name, "nvidia-drm") == 0;
   if (version)
      drmFreeVersion(version);
   if (!native) {
      close(fd);
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }
   struct nvkmd_rm_pdev* pdev = calloc(1, sizeof(*pdev));
   if (!pdev) {
      close(fd);
      return vk_error(log, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   /* Rust open validates exact RM ABI and the semaphore-surface interface.
    * Our sync type owns timeline/binary payloads; DRM syncobj caps do not gate it. */
   VkResult result;
   struct stat st;
   if (fstat(fd, &st)) {
      result = vk_errorf(log, VK_ERROR_INITIALIZATION_FAILED,
                        "Native Rust RM render-node stat failed: %m");
      goto fail_pdev;
   }
   struct NvRmDevice* rm = NULL;
   result = rm_result(log,
                               nv_rm_open(drm->businfo.pci->domain,
                                          drm->businfo.pci->bus,
                                          drm->businfo.pci->dev,
                                          drm->businfo.pci->func,
                                          &rm,
                                          &pdev->info),
                               "query device");
   if (result != VK_SUCCESS) {
      if (rm)
         nv_rm_close(rm);
      goto fail_pdev;
   }
   nv_rm_close(rm);
   const struct NvRmInfo* info = &pdev->info;
   pdev->base.ops = &nvkmd_rm_pdev_ops;
   pdev->base.debug_flags = debug;
   pdev->base.bind_align_B = 4096;
   pdev->base.dev_info = (struct nv_device_info){
      .type = NV_DEVICE_TYPE_DIS,
      .device_id = info->device_id,
      .chipset = info->chipset,
      .sm = info->sm,
      .pci = { .domain = info->pci_domain,
               .bus = info->pci_bus,
               .dev = info->pci_device,
               .func = info->pci_function },
      .gpc_count = info->gpc_count,
      .tpc_count = info->tpc_count,
      .mp_per_tpc = info->mp_per_tpc,
      .max_warps_per_mp = info->max_warps_per_mp,
      .cls_copy = info->cls_copy,
      .cls_eng2d = info->cls_eng2d,
      .cls_eng3d = info->cls_eng3d,
      .cls_m2mf = info->cls_m2mf,
      .cls_compute = info->cls_compute,
      .vram_size_B = info->vram_size,
      .bar_size_B = info->bar_size,
   };
   snprintf(pdev->base.dev_info.device_name,
            sizeof(pdev->base.dev_info.device_name),
            "%s",
            info->name);
   snprintf(pdev->base.dev_info.chipset_name,
            sizeof(pdev->base.dev_info.chipset_name),
            "NV%03X",
            info->chipset);
   /* Derived exactly as the nouveau winsys does (nouveau_device.c,
    * nvkmd_nouveau_pdev.c): shared memory configurations from the SM
    * version, the non-coherent atom from the CPU cache line. */
   nv_device_info_init_shared_mem_sizes(&pdev->base.dev_info);
   pdev->base.dev_info.nc_atom_size_B = util_cache_granularity();
   pdev->base.kmd_info = (struct nvkmd_info){
      .has_dma_buf = true,
      .has_alloc_tiled = info->generic_kind == 0x06 || info->generic_kind == 0xfe,
      /* Stock-driver compression policy (nvk_image_init rm_compress, rm_mem_alloc_kind):
       * Turing+ generic compressible kind. NVK's dedicated-allocation
       * compression (has_compression) stays off. */
      .has_rm_compression = info->generic_kind == 0x06,
   };
   pdev->base.drm.render_dev = st.st_rdev;
   pdev->fd = fd;
   snprintf(
     pdev->render_path, sizeof(pdev->render_path), "%s", drm->nodes[DRM_NODE_RENDER]);
   pdev->sync_type = rm_sync_type;
   pdev->sync_file_type = rm_sync_file_type;
   pdev->sync_types[0] = &pdev->sync_type;
   pdev->sync_types[1] = &pdev->sync_file_type;
   pdev->base.sync_types = pdev->sync_types;
   *out = &pdev->base;
   return VK_SUCCESS;

fail_pdev:
   close(fd);
   free(pdev);
   return result;
}

static void
rm_dev_destroy(struct nvkmd_dev* base)
{
   struct nvkmd_rm_dev* dev = nvkmd_rm_dev(base);
   /* NVK joins its queues before destroying KMD. Vulkan externally synchronizes
    * DestroyDevice with API users, so no callback may still access this data. */
   if (dev->vk) {
      assert(dev->vk->notify_lost == rm_notify_lost);
      assert(dev->vk->notify_lost_data == dev->rm);
      dev->vk->notify_lost = NULL;
      dev->vk->notify_lost_data = NULL;
      dev->vk->check_status = NULL;
   }
   /* nv_rm_close runs every outstanding deferred VA release (rm_va_release)
    * before returning, so the heap must still be live here. */
   nv_rm_close(dev->rm);
   close(dev->fd);
   util_vma_heap_finish(&dev->heap);
   simple_mtx_destroy(&dev->va_mutex);
   simple_mtx_destroy(&dev->base.mems_mutex);
   free(dev);
}
static uint64_t
rm_timestamp(struct nvkmd_dev* base)
{
   uint64_t timestamp = 0;
   int32_t result = nv_rm_timestamp(nvkmd_rm_dev(base)->rm, &timestamp);
   if (result)
      fprintf(stderr, "Native Rust RM timestamp failed: %d\n", result);
   return timestamp;
}
static int
rm_drm_fd(struct nvkmd_dev* base)
{
   return nvkmd_rm_dev(base)->fd;
}
static VkResult
rm_va_alloc(struct nvkmd_dev* base,
            struct vk_object_base* log,
            enum nvkmd_va_flags flags,
            uint8_t kind,
            uint64_t size,
            uint64_t alignment,
            uint64_t fixed,
            struct nvkmd_va** out)
{
   if (flags & NVKMD_VA_SPARSE)
      return unsupported(log, "sparse residency");
   if (!size || size > NV_RM_VA_END - NV_RM_VA_START - 4095)
      return vk_error(log, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   size = align64(size, 4096);
   alignment = MAX2(alignment, 4096);
   if (!util_is_power_of_two_or_zero64(alignment))
      return unsupported(log, "unaligned VA");
   struct nvkmd_rm_dev* dev = nvkmd_rm_dev(base);
   struct nvkmd_rm_va* va = calloc(1, sizeof(*va));
   if (!va)
      return vk_error(log, VK_ERROR_OUT_OF_HOST_MEMORY);
   simple_mtx_lock(&dev->va_mutex);
   uint64_t address;
   if (flags & NVKMD_VA_ALLOC_FIXED) {
      bool valid = fixed >= NV_RM_VA_START && fixed < NV_RM_VA_END &&
                   size <= NV_RM_VA_END - fixed && !(fixed & (alignment - 1));
      address = valid && util_vma_heap_alloc_addr(&dev->heap, fixed, size) ? fixed : 0;
   } else {
      address = util_vma_heap_alloc(&dev->heap, size, alignment);
   }
   simple_mtx_unlock(&dev->va_mutex);
   if (!address) {
      free(va);
      return vk_error(log, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   va->base = (struct nvkmd_va){ .ops = &nvkmd_rm_va_ops,
                                 .dev = base,
                                 .flags = flags,
                                 .pte_kind = kind,
                                 .addr = address,
                                 .size_B = size };
   list_inithead(&va->bindings);
   *out = &va->base;
   return VK_SUCCESS;
}
static VkResult
rm_va_unbind(struct nvkmd_va* base,
             struct vk_object_base* log,
             uint64_t offset,
             uint64_t size)
{
   struct nvkmd_rm_va* va = nvkmd_rm_va(base);
   struct nvkmd_rm_dev* dev = nvkmd_rm_dev(base->dev);
   if (!size || offset > base->size_B || size > base->size_B - offset)
      return unsupported(log, "VA unbind range outside reservation");
   /* Non-sparse NVK image/resource binds cover whole existing mappings. Reject
    * partial overlapping updates until the RM adapter supports splitting them.
    * nv_rm_unbind is immediate: this path only serves application-ordered
    * binds (Vulkan forbids rebinding memory in use by pending work; sparse
    * queue binds are not advertised without has_sparse), never driver
    * teardown, which goes through rm_va_free's deferred nv_rm_va_free. */
   list_for_each_entry(struct rm_binding, b, &va->bindings, link)
   {
      if (b->offset < offset + size && offset < b->offset + b->size &&
          (b->offset < offset || b->offset + b->size > offset + size))
         return unsupported(log, "partial VA unbind");
   }
   list_for_each_entry_safe(struct rm_binding, b, &va->bindings, link)
   {
      if (b->offset >= offset && b->offset + b->size <= offset + size) {
         VkResult result = rm_result(log, nv_rm_unbind(dev->rm, b->mapping), "unbind VA");
         if (result != VK_SUCCESS)
            return result;
         list_del(&b->link);
         free(b);
      }
   }
   return VK_SUCCESS;
}
static VkResult
rm_va_bind(struct nvkmd_va* base,
           struct vk_object_base* log,
           uint64_t offset,
           struct nvkmd_mem* memory,
           uint64_t memory_offset,
           uint64_t size)
{
   struct nvkmd_rm_va* va = nvkmd_rm_va(base);
   struct nvkmd_rm_dev* dev = nvkmd_rm_dev(base->dev);
   struct nvkmd_rm_mem* mem = nvkmd_rm_mem(memory);
   if (memory_offset > memory->size_B || size > memory->size_B - memory_offset)
      return unsupported(log, "VA bind range outside allocation");
   VkResult result = rm_va_unbind(base, log, offset, size);
   if (result != VK_SUCCESS)
      return result;
   struct rm_binding* b = calloc(1, sizeof(*b));
   if (!b)
      return vk_error(log, VK_ERROR_OUT_OF_HOST_MEMORY);
   result = rm_result(log,
                      nv_rm_bind(dev->rm,
                                 mem->allocation.id,
                                 memory_offset,
                                 size,
                                 base->addr + offset,
                                 base->pte_kind,
                                 &b->mapping),
                      "bind VA");
   if (result != VK_SUCCESS) {
      free(b);
      return result;
   }
   b->offset = offset;
   b->size = size;
   list_addtail(&b->link, &va->bindings);
   return VK_SUCCESS;
}
/* Deferred VA release from the Rust backend (nv_rm.h NvRmVaRelease): runs
 * after RM unmapped the range, possibly on another thread or in nv_rm_close.
 * Lock order: backend resource lock -> va_mutex; va_mutex is never held
 * across a backend call (rm_va_alloc only guards util_vma_heap). */
static void
rm_va_release(void* data, uint64_t address, uint64_t size)
{
   struct nvkmd_rm_dev* dev = data;
   simple_mtx_lock(&dev->va_mutex);
   util_vma_heap_free(&dev->heap, address, size);
   simple_mtx_unlock(&dev->va_mutex);
}
/* NVK frees driver-internal memory that queued work may still use (e.g. the
 * previous SLM area, nvk_queue.c:65 / nvk_device.c:118). The backend defers
 * the unmaps and this range's return to the heap until that work retired;
 * it never waits here (the work may be gated on a fence this thread must
 * produce). */
static void
rm_va_free(struct nvkmd_va* base)
{
   struct nvkmd_rm_va* va = nvkmd_rm_va(base);
   struct nvkmd_rm_dev* dev = nvkmd_rm_dev(base->dev);
   uint64_t stack_ids[16];
   uint64_t* ids = stack_ids;
   uint32_t count = list_length(&va->bindings);
   if (count > ARRAY_SIZE(stack_ids))
      ids = calloc(count, sizeof(*ids));
   if (!ids) {
      /* Mappings stay owned by the backend until close; range retained. */
      rm_mem_lost(dev, "VA free bookkeeping; reservation retained",
                  VK_ERROR_OUT_OF_HOST_MEMORY);
   } else {
      uint32_t i = 0;
      list_for_each_entry(struct rm_binding, b, &va->bindings, link)
         ids[i++] = b->mapping;
      int32_t status = nv_rm_va_free(dev->rm, ids, count, base->addr,
                                     base->size_B, rm_va_release, dev);
      if (status != VK_SUCCESS)
         rm_mem_lost(dev, "VA free; reservation retained", status);
      if (ids != stack_ids)
         free(ids);
   }
   list_for_each_entry_safe(struct rm_binding, b, &va->bindings, link)
   {
      list_del(&b->link);
      free(b);
   }
   free(va);
}
const struct nvkmd_va_ops nvkmd_rm_va_ops = { .free = rm_va_free,
                                              .bind_mem = rm_va_bind,
                                              .unbind = rm_va_unbind };

static VkResult
rm_mem_alloc_kind(struct nvkmd_dev* base,
                  struct vk_object_base* log,
                  uint64_t size,
                  uint64_t alignment,
                  enum nvkmd_mem_flags flags,
                  uint8_t kind,
                  struct nvkmd_mem** out)
{
   struct nvkmd_rm_dev* dev = nvkmd_rm_dev(base);
   struct nvkmd_rm_mem* mem = calloc(1, sizeof(*mem));
   if (!mem)
      return vk_error(log, VK_ERROR_OUT_OF_HOST_MEMORY);
   uint32_t placement =
     (flags & NVKMD_MEM_GART) ? NV_RM_PLACEMENT_GART : NV_RM_PLACEMENT_VRAM;
   /* Device-local memory is allocated in whole 64 KiB big pages, as the stock
    * driver allocates device-local memory (BIG pages, seen in its RM alloc
    * requests) and as NVK did itself up to Mesa 25.2
    * (nvk_device_memory.c: NVKMD_MEM_LOCAL -> 64 KiB). Mesa 26 passes only
    * bind_align_B (upstream 7a492e102ff2, "page size is the KMD's choice"), so
    * the KMD keeps the policy: without it, exported dma-bufs shrink to 4 KiB
    * granularity and allocations that are not 64 KiB multiples silently lose
    * the compressible placement below. */
   if (placement == NV_RM_PLACEMENT_VRAM) {
      alignment = MAX2(alignment, 1u << 16);
      size = align64(size, 1u << 16);
   }
   /* Stock 615 requests compression + big pages for device-local memory that
    * is neither host-visible nor exportable; compressed views only exist
    * through image VAs with a compressible kind (rm_va_bind); the backend
    * raises the alignment to the 64 KiB big page. Turing+ only:
    * the generic compressible kind (0x8) is what those VAs carry. */
   if (kind == 0 && placement == NV_RM_PLACEMENT_VRAM &&
       !(flags & (NVKMD_MEM_CAN_MAP | NVKMD_MEM_SHARED)) &&
       nvkmd_rm_pdev(base->pdev)->info.generic_kind == 0x06 &&
       !(size & 0xffff))
      placement = NV_RM_PLACEMENT_VRAM_COMPRESSIBLE;
   /* Tiled (DRM modifier) memory records its block-linear kind on the
    * physical allocation, as NVK does on nouveau (nvk_device_memory.c
    * nvkmd_dev_alloc_tiled_mem -> nouveau_bo.c tile_flags = pte_kind << 8):
    * a dma-buf importer maps the memory with that kind, not with ours. */
   VkResult result =
     rm_result(log,
               nv_rm_alloc(dev->rm, size, alignment, placement, kind, 0, &mem->allocation),
               "allocate memory");
   if (result != VK_SUCCESS) {
      free(mem);
      return result;
   }
   /* Every CPU map of this backend is coherent (SYS memory CPU-cached and
    * snooped, VRAM through BAR1 write-combined), as Mesa 25.2's nvkmd assumed
    * for every map: NVKMD_MEM_COHERENT makes nvkmd_mem_sync_{to,from}_gpu
    * no-ops. */
   nvkmd_mem_init(base, &mem->base, &nvkmd_rm_mem_ops, flags | NVKMD_MEM_COHERENT,
                  mem->allocation.size, 4096);
   result = nvkmd_dev_alloc_va(
     base, log, 0, kind, mem->allocation.size, alignment, 0, &mem->base.va);
   if (result != VK_SUCCESS)
      goto fail_mem;
   result = nvkmd_va_bind_mem(mem->base.va, log, 0, &mem->base, 0, mem->allocation.size);
   if (result != VK_SUCCESS) {
      nvkmd_va_free(mem->base.va);
      goto fail_mem;
   }
   *out = &mem->base;
   return VK_SUCCESS;
fail_mem:
   {
      int32_t status = nv_rm_free(dev->rm, mem->allocation.id);
      if (status != VK_SUCCESS)
         result = rm_mem_lost(dev, "allocation rollback free", status);
   }
   simple_mtx_destroy(&mem->base.map_mutex);
   free(mem);
   return result;
}
static VkResult
rm_mem_alloc(struct nvkmd_dev* base,
             struct vk_object_base* log,
             uint64_t size,
             uint64_t alignment,
             enum nvkmd_mem_flags flags,
             struct nvkmd_mem** out)
{
   return rm_mem_alloc_kind(base, log, size, alignment, flags, 0, out);
}

static VkResult
rm_mem_tiled(struct nvkmd_dev* dev,
             struct vk_object_base* log,
             uint64_t size,
             uint64_t alignment,
             uint8_t kind,
             uint16_t tile,
             enum nvkmd_mem_flags flags,
             struct nvkmd_mem** out)
{
   /* NIL's uncompressed 2D modifier carries explicit geometry. The physical
    * memory carries the image's (generic, uncompressed) kind with FORMAT
    * BLOCK_LINEAR, and the allocation's VA the same kind. The block height
    * is in the modifier; RM has no per-memory tile mode, so none is stored. */
   if (kind == 0 && tile == 0)
      return rm_mem_alloc(dev, log, size, alignment, flags, out);
   const struct NvRmInfo* info = &nvkmd_rm_pdev(dev->pdev)->info;
   if (kind != info->generic_kind || (tile & ~0x70u) || (tile >> 4) > 5)
      return unsupported(log, "modifier allocation outside uncompressed 2D generic memory");
   /* Fermi..Volta (generic kind 0xfe): BAR1 does not return a block-linear
    * image's bytes in the modifier's layout, so memoryTypeBits of such images
    * carries no host-visible VRAM type (nvk-prevolta-modifier-memory-types
    * patch, nvk_get_image_memory_requirements). Reaching this with a
    * CPU-mappable VRAM request means that contract was broken upstream. */
   if (info->generic_kind == 0xfe && (flags & NVKMD_MEM_CAN_MAP) && !(flags & NVKMD_MEM_GART)) {
      assert(!"CPU-mappable block-linear VRAM before Turing");
      return vk_errorf(log, VK_ERROR_UNKNOWN,
                       "internal contract violated: CPU-mappable block-linear VRAM before Turing "
                       "(image memoryTypeBits must exclude host-visible VRAM types)");
   }
   return rm_mem_alloc_kind(dev, log, size, alignment, flags, kind, out);
}
static VkResult
rm_mem_import(struct nvkmd_dev* base,
              struct vk_object_base* log,
              int fd,
              struct nvkmd_mem** out)
{
   struct nvkmd_rm_dev* dev = nvkmd_rm_dev(base);
   struct nvkmd_rm_mem* mem = calloc(1, sizeof(*mem));
   if (!mem)
      return vk_error(log, VK_ERROR_OUT_OF_HOST_MEMORY);
   VkResult result = rm_result(
     log, nv_rm_import(dev->rm, dev->fd, fd, &mem->allocation), "import DMA-buf");
   if (result != VK_SUCCESS) {
      free(mem);
      return result;
   }
   nvkmd_mem_init(base,
                  &mem->base,
                  &nvkmd_rm_mem_ops,
                  NVKMD_MEM_LOCAL | NVKMD_MEM_SHARED | NVKMD_MEM_CAN_MAP |
                  NVKMD_MEM_COHERENT,
                  mem->allocation.size,
                  4096);
   result =
     nvkmd_dev_alloc_va(base, log, 0, 0, mem->allocation.size, 4096, 0, &mem->base.va);
   if (result != VK_SUCCESS)
      goto fail_mem;
   result = nvkmd_va_bind_mem(mem->base.va, log, 0, &mem->base, 0, mem->allocation.size);
   if (result != VK_SUCCESS) {
      nvkmd_va_free(mem->base.va);
      goto fail_mem;
   }
   *out = &mem->base;
   /* nvk_AllocateMemory consumes the application fd only after all import
    * initialization succeeds; this KMD helper always borrows its input fd. */
   return VK_SUCCESS;
fail_mem:
   {
      int32_t status = nv_rm_free(dev->rm, mem->allocation.id);
      if (status != VK_SUCCESS)
         result = rm_mem_lost(dev, "import rollback free", status);
   }
   simple_mtx_destroy(&mem->base.map_mutex);
   free(mem);
   return result;
}
static void
rm_mem_free(struct nvkmd_mem* base)
{
   struct nvkmd_rm_mem* mem = nvkmd_rm_mem(base);
   struct nvkmd_rm_dev* dev = nvkmd_rm_dev(base->dev);
   nvkmd_va_free(base->va);
   int32_t status = nv_rm_free(dev->rm, mem->allocation.id);
   if (status)
      rm_mem_lost(dev, "memory free", status);
   simple_mtx_destroy(&base->map_mutex);
   free(mem);
}
static VkResult
rm_mem_map(struct nvkmd_mem* base,
           struct vk_object_base* log,
           enum nvkmd_mem_map_flags flags,
           void* fixed,
           void** out)
{
   *out = NULL;
   if (fixed || (flags & NVKMD_MEM_MAP_FIXED))
      return unsupported(log, "fixed CPU mapping");
   struct nvkmd_rm_mem* mem = nvkmd_rm_mem(base);
   struct nvkmd_rm_dev* dev = nvkmd_rm_dev(base->dev);
   uintptr_t address = 0;
   int32_t status = nv_rm_map(dev->rm, mem->allocation.id, &address);
   if (status != VK_SUCCESS) {
      /* A rollback can poison Rust ownership while returning its original RM
       * error. The atomic loss snapshot disambiguates it without another RPC. */
      if (status == VK_ERROR_DEVICE_LOST || nv_rm_is_lost(dev->rm))
         return rm_mem_lost(dev, "CPU map", status);
      VkResult result = status == VK_ERROR_OUT_OF_HOST_MEMORY ||
                        status == VK_ERROR_OUT_OF_DEVICE_MEMORY ?
                           (VkResult)status : VK_ERROR_MEMORY_MAP_FAILED;
      return vk_errorf(log, result, "Native Rust RM CPU map failed: %d", status);
   }
   if (!address) {
      /* The successful bridge call acquired a logical lease even if its
       * address is invalid. Do not strand that reference on this error path. */
      status = nv_rm_unmap(dev->rm, mem->allocation.id);
      if (status != VK_SUCCESS)
         return rm_mem_lost(dev, "null CPU map rollback", status);
      return vk_errorf(log, VK_ERROR_MEMORY_MAP_FAILED,
                       "Native Rust RM CPU map returned a null address");
   }
   *out = (void*)address;
   return VK_SUCCESS;
}
static void
rm_mem_unmap(struct nvkmd_mem* base, enum nvkmd_mem_map_flags flags, void* map)
{
   (void)flags;
   (void)map;
   struct nvkmd_rm_mem* mem = nvkmd_rm_mem(base);
   struct nvkmd_rm_dev* dev = nvkmd_rm_dev(base->dev);
   int32_t status = nv_rm_unmap(dev->rm, mem->allocation.id);
   if (status)
      rm_mem_lost(dev, "CPU unmap", status);
}
static VkResult
rm_mem_overmap(struct nvkmd_mem* mem,
               struct vk_object_base* log,
               enum nvkmd_mem_map_flags flags,
               void* map)
{
   (void)mem;
   (void)flags;
   (void)map;
   return unsupported(log, "CPU overmap");
}
static VkResult
rm_mem_export(struct nvkmd_mem* base, struct vk_object_base* log, int* fd)
{
   struct nvkmd_rm_mem* mem = nvkmd_rm_mem(base);
   struct nvkmd_rm_dev* dev = nvkmd_rm_dev(base->dev);
   return rm_result(
     log, nv_rm_export(dev->rm, mem->allocation.id, dev->fd, fd), "export DMA-buf");
}
/* nvkmd only calls these for memory without NVKMD_MEM_COHERENT when the CPU
 * has no cache maintenance instructions; every rm allocation and import is
 * coherent (rm_mem_alloc_kind, rm_mem_import), so reaching them is a bug. */
static void
rm_mem_sync(struct nvkmd_mem* base, uint64_t offset, uint64_t range)
{
   fprintf(stderr, "nvkmd_rm: cache sync requested on coherent memory (%" PRIx64
           "+%" PRIx64 ", flags %#x)\n", offset, range, base->flags);
   abort();
}
static uint32_t
rm_mem_handle(struct nvkmd_mem* base)
{
   return nvkmd_rm_mem(base)->allocation.id;
}
const struct nvkmd_mem_ops nvkmd_rm_mem_ops = { .free = rm_mem_free,
                                                .map = rm_mem_map,
                                                .unmap = rm_mem_unmap,
                                                .overmap = rm_mem_overmap,
                                                .sync_to_gpu = rm_mem_sync,
                                                .sync_from_gpu = rm_mem_sync,
                                                .export_dma_buf = rm_mem_export,
                                                .log_handle = rm_mem_handle };

/* Accepted submissions retain payload references in Rust. The temporary C
 * list only borrows identities while Mesa owns its submit's sync objects. */
static void
rm_ctx_clear_waits(struct nvkmd_rm_ctx *ctx)
{
   util_dynarray_clear(&ctx->waits);
}

static VkResult
rm_ctx_submit(struct nvkmd_rm_ctx *ctx, struct vk_object_base *log,
              const struct NvRmExec *pushes, uint32_t count,
              const struct NvRmSyncPoint *signals, uint32_t signal_count)
{
   struct nvkmd_rm_dev *dev = nvkmd_rm_dev(ctx->base.dev);
   uint64_t token = 0;
   int32_t status;
   for (;;) {
      status = nv_rm_submit_batch(dev->rm, ctx->channel, pushes, count,
                                 util_dynarray_begin(&ctx->waits),
                                 util_dynarray_num_elements(&ctx->waits, struct NvRmSyncPoint),
                                 signals, signal_count,
                                 &token);
      if (status != VK_NOT_READY)
         break;
      /* Ring pressure: wait for the oldest batch on this thread; age alone
       * is not loss. This wait terminates: the device runs Mesa's
       * THREADED_ON_DEMAND submit mode, so every wait this backend receives
       * was VK_SYNC_WAIT_PENDING (its signal already submitted or
       * host-signalled) when submitted, and the batches in all rings form
       * a DAG in submission order. Only imported sync_files depend on
       * producers outside this device (SYNCHRONIZATION.md). */
      status = nv_rm_wait(dev->rm, ctx->channel, token, UINT64_MAX);
      if (status != VK_SUCCESS)
         break;
   }
   if (status != VK_SUCCESS)
      return rm_result(log, status,
                       "submit asynchronous work");
   rm_ctx_clear_waits(ctx);
   ctx->last_token = token;
   return VK_SUCCESS;
}

static VkResult
rm_ctx_flush_signals(struct nvkmd_ctx* base, struct vk_object_base* log,
                     const struct NvRmSyncPoint *signals, uint32_t signal_count)
{
   struct nvkmd_rm_ctx* ctx = nvkmd_rm_ctx(base);
   struct NvRmExec* pushes = util_dynarray_begin(&ctx->pending);
   uint32_t total = util_dynarray_num_elements(&ctx->pending, struct NvRmExec);
   VkResult result = VK_SUCCESS;
   /* Validate every continuation boundary before accepting any work. */
   for (uint32_t offset = 0; offset < total;) {
      uint32_t count = MIN2(total - offset, 1000);
      while (count && (pushes[offset + count - 1].flags & NV_RM_EXEC_INCOMPLETE))
         count--;
      if (!count)
         return unsupported(log, "push continuation chain exceeding 1000 entries");
      offset += count;
   }
   if (!total && (signal_count ||
       util_dynarray_num_elements(&ctx->waits, struct NvRmSyncPoint)))
      result = rm_ctx_submit(ctx, log, NULL, 0, signals, signal_count);
   uint32_t offset = 0;
   while (offset < total) {
      uint32_t count = MIN2(total - offset, 1000);
      while (pushes[offset + count - 1].flags & NV_RM_EXEC_INCOMPLETE)
         count--;
      bool final = offset + count == total;
      result = rm_ctx_submit(ctx, log, pushes + offset, count,
                             final ? signals : NULL, final ? signal_count : 0);
      if (result != VK_SUCCESS)
         break;
      offset += count;
   }
   /* A later chunk may fail after earlier chunks were accepted. Keep only the
    * unaccepted suffix; retries must never replay an accepted prefix. */
   if (offset) {
      memmove(pushes, pushes + offset, (total - offset) * sizeof(*pushes));
      ctx->pending.size = (total - offset) * sizeof(*pushes);
   }
   return result;
}

static VkResult
rm_ctx_flush(struct nvkmd_ctx* base, struct vk_object_base* log)
{
   return rm_ctx_flush_signals(base, log, NULL, 0);
}

static VkResult
rm_ctx_sync(struct nvkmd_ctx *base, struct vk_object_base *log)
{
   struct nvkmd_rm_ctx *ctx = nvkmd_rm_ctx(base);
   VkResult result = rm_ctx_flush(base, log);
   if (result != VK_SUCCESS || !ctx->last_token)
      return result;
   int32_t status = nv_rm_wait(nvkmd_rm_dev(base->dev)->rm, ctx->channel,
                               ctx->last_token, UINT64_MAX);
   return rm_result(log, status,
                    "wait for context completion");
}

static VkResult
rm_ctx_wait(struct nvkmd_ctx* base,
            struct vk_object_base* log,
            uint32_t count,
            const struct vk_sync_wait* waits)
{
   struct nvkmd_rm_ctx* ctx = nvkmd_rm_ctx(base);
   /* Consecutive waits precede the same future work and can share one GPU
    * acquire prefix. Flush only work that must remain before this wait. */
   VkResult result = VK_SUCCESS;
   if (util_dynarray_num_elements(&ctx->pending, struct NvRmExec))
      result = rm_ctx_flush(base, log);
   if (result != VK_SUCCESS)
      return result;
   /* Bind contexts perform CPU VA changes and have no GPU channel. Their
    * dependencies must complete before those changes; graphics/copy contexts
    * instead encode real GPU acquires in the following submission. */
   if (!ctx->channel)
      return vk_sync_wait_many(ctx->vk, count, waits,
                               VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
   for (uint32_t i = 0; i < count; i++) {
      if ((waits[i].sync->flags & VK_SYNC_IS_TIMELINE) && !waits[i].wait_value)
         continue;
      if (waits[i].sync->type->init != rm_sync_init)
         return unsupported(log, "non-RM synchronization payload");
      struct NvRmSyncPoint point = { rm_sync(waits[i].sync)->id,
                                     waits[i].wait_value };
      util_dynarray_append_typed(&ctx->waits, struct NvRmSyncPoint, point);
   }
   return VK_SUCCESS;
}
static VkResult
rm_ctx_exec(struct nvkmd_ctx* base,
            struct vk_object_base* log,
            uint32_t count,
            const struct nvkmd_ctx_exec* execs)
{
   struct nvkmd_rm_ctx* ctx = nvkmd_rm_ctx(base);
   for (uint32_t i = 0; i < count; i++) {
      if ((execs[i].addr & 3) || (execs[i].size_B & 3) || !execs[i].size_B ||
          execs[i].size_B >= (1u << 23))
         return unsupported(log, "invalid push range");
      struct NvRmExec p = { .address = execs[i].addr,
                            .size_B = execs[i].size_B,
                            .flags = (execs[i].no_prefetch ? NV_RM_EXEC_NO_PREFETCH : 0) |
                                     (execs[i].incomplete ? NV_RM_EXEC_INCOMPLETE : 0) };
      util_dynarray_append_typed(&ctx->pending, struct NvRmExec, p);
   }
   return VK_SUCCESS;
}
static VkResult
rm_ctx_bind(struct nvkmd_ctx* base,
            struct vk_object_base* log,
            uint32_t count,
            const struct nvkmd_ctx_bind* binds)
{
   VkResult result = rm_ctx_flush(base, log);
   if (result != VK_SUCCESS)
      return result;
   for (uint32_t i = 0; i < count; i++) {
      const struct nvkmd_ctx_bind* b = &binds[i];
      result = b->op == NVKMD_BIND_OP_BIND
                 ? nvkmd_va_bind_mem(
                     b->va, log, b->va_offset_B, b->mem, b->mem_offset_B, b->range_B)
                 : nvkmd_va_unbind(b->va, log, b->va_offset_B, b->range_B);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}
static VkResult
rm_ctx_signal(struct nvkmd_ctx* base,
              struct vk_object_base* log,
              uint32_t count,
              const struct vk_sync_signal* signals)
{
   struct nvkmd_rm_ctx* ctx = nvkmd_rm_ctx(base);
   VkResult result;
   if (!ctx->channel) {
      result = rm_ctx_flush(base, log);
      if (result != VK_SUCCESS)
         return result;
      for (uint32_t i = 0; i < count; i++) {
         result = vk_sync_signal(ctx->vk, signals[i].sync, signals[i].signal_value);
         if (result != VK_SUCCESS)
            return result;
      }
      return VK_SUCCESS;
   }
   if (!count)
      return rm_ctx_flush(base, log);
   struct NvRmSyncPoint *points = calloc(count, sizeof(*points));
   if (!points)
      return vk_error(log, VK_ERROR_OUT_OF_HOST_MEMORY);
   for (uint32_t i = 0; i < count; i++) {
      if (signals[i].sync->type->init != rm_sync_init) {
         free(points);
         return unsupported(log, "non-RM synchronization signal");
      }
      points[i] = (struct NvRmSyncPoint){ rm_sync(signals[i].sync)->id,
                                         signals[i].signal_value };
   }
   result = rm_ctx_flush_signals(base, log, points, count);
   free(points);
   return result;
}
static void
rm_ctx_destroy(struct nvkmd_ctx* base)
{
   struct nvkmd_rm_ctx* ctx = nvkmd_rm_ctx(base);
   /* Vulkan idle is explicit. Destruction discards unsubmitted work and uses
    * Rust's bounded quiesce path, never an infinite dependency wait here. */
   util_dynarray_clear(&ctx->pending);
   if (ctx->channel) {
      int32_t status = nv_rm_context_destroy(nvkmd_rm_dev(base->dev)->rm, ctx->channel);
      if (status != VK_SUCCESS)
         fprintf(stderr, "Native Rust RM bounded context destroy failed: %d\n", status);
   }
   rm_ctx_clear_waits(ctx);
   util_dynarray_fini(&ctx->waits);
   util_dynarray_fini(&ctx->pending);
   free(ctx);
}
const struct nvkmd_ctx_ops nvkmd_rm_ctx_ops = { .destroy = rm_ctx_destroy,
                                                .wait = rm_ctx_wait,
                                                .exec = rm_ctx_exec,
                                                .bind = rm_ctx_bind,
                                                .signal = rm_ctx_signal,
                                                .flush = rm_ctx_flush,
                                                .sync = rm_ctx_sync };
static VkResult
rm_ctx_create(struct nvkmd_dev* base,
              struct vk_object_base* log,
              enum nvkmd_engines engines,
              struct nvkmd_ctx** out)
{
   struct nvkmd_rm_ctx* ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
      return vk_error(log, VK_ERROR_OUT_OF_HOST_MEMORY);
   ctx->base = (struct nvkmd_ctx){ .ops = &nvkmd_rm_ctx_ops, .dev = base };
   ctx->vk = log->device;
   util_dynarray_init(&ctx->pending, NULL);
   util_dynarray_init(&ctx->waits, NULL);
   if (engines != NVKMD_ENGINE_BIND) {
      VkResult result =
        rm_result(log,
                  nv_rm_context_create(nvkmd_rm_dev(base)->rm, engines, &ctx->channel),
                  "create context");
      if (result != VK_SUCCESS) {
         util_dynarray_fini(&ctx->waits);
         util_dynarray_fini(&ctx->pending);
         free(ctx);
         return result;
      }
   }
   *out = &ctx->base;
   return VK_SUCCESS;
}
const struct nvkmd_dev_ops nvkmd_rm_dev_ops = { .destroy = rm_dev_destroy,
                                                .get_gpu_timestamp = rm_timestamp,
                                                .get_drm_fd = rm_drm_fd,
                                                .alloc_mem = rm_mem_alloc,
                                                .alloc_tiled_mem = rm_mem_tiled,
                                                .import_dma_buf = rm_mem_import,
                                                .alloc_va = rm_va_alloc,
                                                .create_ctx = rm_ctx_create };
