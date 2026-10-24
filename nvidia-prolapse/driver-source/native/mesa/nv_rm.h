/* SPDX-License-Identifier: MIT
 * Source-built NVK -> Rust RM ABI. All calls are thread-safe and run on
 * the calling thread (no worker). Sync ids are backend payload references;
 * context/memory ids are numeric handles validated by the backend.
 * Return 0 on success, a negative VkResult on failure, with a diagnostic on
 * stderr (GNU) or in logcat, tag "nvk-rm" (Android).
 *
 * Resource frees never wait for GPU work: pending work may be gated on a
 * foreign fence whose producer is the freeing thread. nv_rm_free and
 * nv_rm_va_free instead defer the real RM free/unmap (and the VA range's
 * return to the caller) until every queue's work submitted before the call
 * has retired; with nothing outstanding they act immediately.
 */
#ifndef NV_RM_H
#define NV_RM_H
#include <stddef.h>
#include <stdint.h>
struct NvRmDevice;
struct NvRmInfo {
   uint32_t vendor_id, device_id, chipset, sm;
   uint32_t pci_domain, pci_bus, pci_device, pci_function;
   uint32_t gpc_count, tpc_count, mp_per_tpc, max_warps_per_mp;
   uint32_t cls_copy, cls_eng2d, cls_eng3d, cls_m2mf, cls_compute;
   uint32_t generic_kind; /* NIL uncompressed color PTE kind: Fermi 0xfe, Turing+ 6 */
   uint64_t vram_size, bar_size;
   char name[64];
};
struct NvRmAllocation {
   uint64_t id, size;
   uintptr_t cpu_address; /* Reserved zero; CPU maps are acquired separately. */
};
struct NvRmExec {
   uint64_t address;
   uint32_t size_B, flags;
};
struct NvRmSyncPoint {
   uint64_t sync, value;
};
#define NV_RM_SYNC_TIMELINE (1u << 0)
#define NV_RM_SYNC_SHAREABLE (1u << 1)
#define NV_RM_SYNC_SHARED (1u << 2)
#define NV_RM_SYNC_WAIT_PENDING (1u << 0)
#define NV_RM_SYNC_WAIT_ANY (1u << 1)
/* Sync identities belong to this device. Payloads accepted by submission are
 * retained independently of identity reset/move/destruction. Mutation is
 * transactional: a failed create/reset/move/import leaves live state intact. */
int32_t nv_rm_sync_create(struct NvRmDevice*, uint32_t flags,
                          uint64_t initial_value, uint64_t* sync);
int32_t nv_rm_sync_destroy(struct NvRmDevice*, uint64_t sync);
/* Historical name kept for ABI stability: disposal is immediate on the caller.
 * Accepted submissions keep their own payload slot references until retired. */
int32_t nv_rm_sync_destroy_deferred(struct NvRmDevice*, uint64_t sync);
int32_t nv_rm_sync_reset(struct NvRmDevice*, uint64_t sync);
int32_t nv_rm_sync_move(struct NvRmDevice*, uint64_t dst, uint64_t src,
                        uint32_t shared_source);
/* Bind a deferred (submit-thread) binary signal: dst becomes the handle the
 * deferred batch signals, on src's current slot; src reports it pending.
 * *reserved = 0: src is not reservable and nothing changed. */
int32_t nv_rm_sync_reserve(struct NvRmDevice*, uint64_t dst, uint64_t src,
                           uint32_t* reserved);
int32_t nv_rm_sync_signal(struct NvRmDevice*, uint64_t sync, uint64_t value);
int32_t nv_rm_sync_get_value(struct NvRmDevice*, uint64_t sync, uint64_t* value);
/* Nonblocking status query; ready is 0 or 1. flags select pending/any. */
int32_t nv_rm_sync_status_many(struct NvRmDevice*,
                               const struct NvRmSyncPoint*, uint32_t count,
                               uint32_t flags, uint32_t* ready);
/* Returns 0 on completion, 2 on caller timeout, negative on failure. Infinite
 * waits use UINT64_MAX. Sleeps on the caller: one waiting thread polls the RM
 * event descriptor and wakes the others; the deadline is the caller's only. */
int32_t nv_rm_sync_wait_many(struct NvRmDevice *, const struct NvRmSyncPoint *,
                            uint32_t count, uint32_t flags, uint64_t timeout_ns);

/* Import borrows fd; export returns an owned fd. opaque selects OPAQUE_FD
 * instead of SYNC_FD. Only explicit export needs a kernel export fence. */
int32_t nv_rm_sync_import_fd(struct NvRmDevice*, uint64_t sync, int fd,
                             uint32_t opaque);
int32_t nv_rm_sync_export_fd(struct NvRmDevice*, uint64_t sync, uint32_t opaque,
                             int drm_fd, int* fd);
/* These calls return VK_NOT_READY with the oldest token on ring pressure; no
 * waits, pushes or signals are accepted in that case. Success retains all
 * payloads and returns the actual GPU completion token for this batch. */
int32_t nv_rm_submit_batch(struct NvRmDevice*, uint64_t context,
                           const struct NvRmExec*, uint32_t count,
                           const struct NvRmSyncPoint*, uint32_t wait_count,
                           const struct NvRmSyncPoint*, uint32_t signal_count,
                           uint64_t* token);
int32_t nv_rm_submit_sync(struct NvRmDevice*, uint64_t context,
                          const struct NvRmExec*, uint32_t count,
                          const struct NvRmSyncPoint*, uint32_t wait_count,
                          uint64_t* token);
int32_t nv_rm_context_signal(struct NvRmDevice*, uint64_t context,
                             const struct NvRmSyncPoint*, uint32_t count,
                             uint64_t* token);
#define NV_RM_EXEC_NO_PREFETCH (1u << 0)
#define NV_RM_EXEC_INCOMPLETE (1u << 1)
/* Engine bits intentionally match nvkmd_engines. */
#define NV_RM_ENGINE_COPY (1u << 0)
#define NV_RM_ENGINE_2D (1u << 1)
#define NV_RM_ENGINE_3D (1u << 2)
#define NV_RM_ENGINE_M2MF (1u << 3)
#define NV_RM_ENGINE_COMPUTE (1u << 4)
#define NV_RM_PLACEMENT_VRAM 0u
#define NV_RM_PLACEMENT_GART 1u
/* Device-local, never CPU-mapped or exported: RM is asked for compression and
 * big pages (NVOS32 COMPR ANY | PAGE_SIZE BIG), as the stock driver does. RM
 * decides whether compression is granted; the allocation's own VA keeps the
 * requested (uncompressed) kind. Size must be a 64 KiB multiple; the alignment
 * is raised to 64 KiB. */
#define NV_RM_PLACEMENT_VRAM_COMPRESSIBLE 2u
/* C owns [16 GiB, 1 TiB), Rust internal mappings must remain below 16 GiB. */
#define NV_RM_VA_START (UINT64_C(1) << 34)
#define NV_RM_VA_END (UINT64_C(1) << 40)
int32_t
nv_rm_open(uint32_t domain,
           uint32_t bus,
           uint32_t device,
           uint32_t function,
           struct NvRmDevice** out,
           struct NvRmInfo* info);
void
nv_rm_close(struct NvRmDevice* device);
/* pte_kind 0: plain memory (RM picks the layout). Non-zero: uncompressed
 * block-linear memory of that kind (the device's generic kind only, VRAM or
 * GART); RM must grant exactly that kind. tile_mode must be 0. */
int32_t
nv_rm_alloc(struct NvRmDevice* device,
            uint64_t size,
            uint64_t alignment,
            uint32_t placement,
            uint32_t pte_kind,
            uint32_t tile_mode,
            struct NvRmAllocation* out);
/* Separate internal/client map references share one lazy mapping. */
int32_t
nv_rm_map(struct NvRmDevice* device, uint64_t memory, uintptr_t* address);
int32_t
nv_rm_unmap(struct NvRmDevice* device, uint64_t memory);
/* Drops the allocation id now (CPU mapping must be released). The RM object
 * is freed after the work submitted before this call retires (see above). */
int32_t
nv_rm_free(struct NvRmDevice* device, uint64_t memory);
int32_t
nv_rm_bind(struct NvRmDevice* device,
           uint64_t memory,
           uint64_t memory_offset,
           uint64_t size,
           uint64_t address,
           uint32_t pte_kind,
           uint64_t* mapping);
/* Immediate RM unmap, no GPU wait: only for application-ordered binds, where
 * Vulkan forbids unbinding/rebinding memory still used by pending work. */
int32_t
nv_rm_unbind(struct NvRmDevice* device, uint64_t mapping);
/* Returns a VA range to the caller's allocator. Called at most once per
 * successful nv_rm_va_free, only after RM unmapped every listed mapping, on
 * whichever thread reclaims it (that call, a later backend call, or
 * nv_rm_close, which runs all outstanding releases before returning). Never
 * called if an unmap failed (device lost; the range stays reserved). The
 * backend may hold its resource lock while calling it: it may take only the
 * caller's VA-heap lock, which must never be held across backend calls. */
typedef void (*NvRmVaRelease)(void* data, uint64_t address, uint64_t size);
/* Frees a VA reservation [address, address + size) and all its mappings
 * (ids from nv_rm_bind, each inside the range). On success the backend owns
 * the mappings and calls release(data, address, size) once, deferred as
 * described above (immediately if count is 0 or nothing is outstanding).
 * On failure nothing was taken: the caller keeps the range reserved. */
int32_t
nv_rm_va_free(struct NvRmDevice* device, const uint64_t* mappings,
              uint32_t count, uint64_t address, uint64_t size,
              NvRmVaRelease release, void* data);
int32_t
nv_rm_context_create(struct NvRmDevice* device, uint32_t engines, uint64_t* context);
int32_t
nv_rm_context_destroy(struct NvRmDevice* device, uint64_t context);
/* All pushes (including an incomplete push and its following continuation)
 * execute on the persistent channel in order. Success means full GPU completion.
 */
int32_t
nv_rm_exec(struct NvRmDevice* device,
           uint64_t context,
           const struct NvRmExec* pushes,
           uint32_t count);
/* Async submission: 0 means queued, *token names GPU completion; 1
 * (VK_NOT_READY) means no work/waits were queued and *token names the oldest
 * batch to wait before retrying. Other results are negative errors.
 * Borrowed wait fds remain owned by the caller; accepted waits retain owned
 * descriptor duplicates and cancellable userspace gates, not kernel WAIT
 * callbacks. Empty batches are valid and issue a completion.
 */
int32_t
nv_rm_submit_waits(struct NvRmDevice* device, uint64_t context,
                   const struct NvRmExec* pushes, uint32_t count,
                   int drm_fd, const int* wait_fds, uint32_t wait_count,
                   uint64_t* token);
int32_t
nv_rm_submit(struct NvRmDevice* device, uint64_t context,
             const struct NvRmExec* pushes, uint32_t count, uint64_t* token);
int32_t
nv_rm_poll(struct NvRmDevice* device, uint64_t context, uint64_t token,
           uint32_t* complete);
/* Waits on the caller. UINT64_MAX is infinite, zero polls.
 * Returns 0 when complete, 2 (VK_TIMEOUT) at deadline, negative on error. */
int32_t
nv_rm_wait(struct NvRmDevice* device, uint64_t context, uint64_t token,
           uint64_t timeout_ns);
/* Export a real pending GPU fence; returned fd belongs to the caller. */
int32_t
nv_rm_context_fence(struct NvRmDevice* device, uint64_t context, uint64_t token,
                    int drm_fd, int* sync_fd);
/* Poll channel and retained incoming dependency errors after Vulkan waits.
 * Returns VK_SUCCESS or VK_ERROR_DEVICE_LOST, preserving producer failures. */
int32_t
nv_rm_check_status(struct NvRmDevice* device);

/* Nonblocking snapshot: 0 = live, 1 = lost (also for NULL). One atomic load.
 * Caller must keep the device alive for this call, as with other entrypoints. */
int32_t
nv_rm_is_lost(const struct NvRmDevice* device);
/* CPU-only software-loss publication: atomics, CPU waiter notification and an
 * eventfd wake. May take the waiters' condition-variable lock only; never waits
 * for GPU progress, takes submission/resource locks or issues RM calls. Caller
 * retains the device throughout the call. */
void
nv_rm_mark_lost(const struct NvRmDevice* device);
int32_t
nv_rm_timestamp(struct NvRmDevice* device, uint64_t* timestamp);
int32_t
nv_rm_export(struct NvRmDevice* device, uint64_t memory, int drm_fd, int* dma_buf_fd);
int32_t
nv_rm_import(struct NvRmDevice* device,
             int drm_fd,
             int dma_buf_fd,
             struct NvRmAllocation* out);
#endif
