/* VkImage translation for AHB wrapper images (ahb.c), for every command that can carry a VkImage.
 *
 * The list is not hand-maintained: tools/vkimage-coverage.py derives from vk.xml every command
 * with a VkImage parameter, a struct reaching a VkImage member (incl. pNext extension structs), an
 * untyped object handle, or a create info that can carry the AHB handle type, and fails the build
 * unless each one is in the intercept table of hal.c or belongs to a hidden extension.
 *
 * Rules: the app's memory is never written (translation happens on stack/heap copies); when no
 * wrapper exists in the process (nvhal_no_wrappers) nothing is looked up; when no image of a call
 * is a wrapper the app's pointers go to the ICD unchanged. A wrapper without memory has no ICD
 * image: a command naming it is invalid usage; it is dropped with an error log (never forwarded
 * with a NULL handle). */
#include "nvhal.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

/* ------------------------------------------------------------------ command buffer -> device */

/* Every vkCmd* the HAL intercepts looks its command buffer up here; with the ASTC emulation that includes
 * vkCmdBindPipeline/vkCmdBindDescriptorSets/vkCmdPushConstants (cmdstate.c), so the lookup is cached per thread
 * (a command buffer is recorded by one thread at a time). Any insert or delete bumps cmd_gen, which invalidates
 * every thread's cached entry before its memory can be reused. */
static struct nvhal_hash cmds = NVHAL_HASH_INIT;
static _Atomic uint64_t cmd_gen = 1;
static __thread struct { VkCommandBuffer cb; struct nvhal_cmd *e; uint64_t gen; } cmd_cache;

struct nvhal_cmd *nvhal_cmd_get(VkCommandBuffer cb) {
    uint64_t g = atomic_load_explicit(&cmd_gen, memory_order_acquire);
    if (cmd_cache.cb == cb && cmd_cache.gen == g) return cmd_cache.e;
    struct nvhal_cmd *e = nvhal_hash_get(&cmds, (uint64_t)(uintptr_t)cb);
    cmd_cache.cb = cb;
    cmd_cache.e = e;
    cmd_cache.gen = g;
    return e;
}

static void cmd_free(void *v) {
    struct nvhal_cmd *e = v;
    if (!e) return;
    nvhal_cs_free(e->cs);
    nvhal_gt_cb_free(e->gt);
    free(e);
}

void nvhal_cmd_track(struct nvhal_device *d, VkCommandPool pool, const VkCommandBuffer *cb, uint32_t n) {
    const uint32_t family = nvhal_pool_family(pool);
    for (uint32_t i = 0; i < n; i++) {
        struct nvhal_cmd *e = nvhal_hash_get(&cmds, (uint64_t)(uintptr_t)cb[i]);
        if (e) { /* a recycled handle (its pool was destroyed): the entry is reused */
            nvhal_cs_free(e->cs);
            nvhal_gt_cb_free(e->gt);
            *e = (struct nvhal_cmd){d, pool, family, NULL, NULL};
            atomic_fetch_add_explicit(&cmd_gen, 1, memory_order_acq_rel);
            continue;
        }
        e = malloc(sizeof *e);
        if (!e || !nvhal_hash_put(&cmds, (uint64_t)(uintptr_t)cb[i], e)) {
            free(e);
            NVHAL_ERR("vkAllocateCommandBuffers: out of host memory recording command buffer %p; image commands "
                      "recorded into it will be dropped", (void *)cb[i]);
            continue;
        }
        *e = (struct nvhal_cmd){d, pool, family, NULL, NULL};
        atomic_fetch_add_explicit(&cmd_gen, 1, memory_order_acq_rel);
    }
}

struct nvhal_device *nvhal_cmd_device(VkCommandBuffer cb) {
    struct nvhal_cmd *e = nvhal_cmd_get(cb);
    return e ? e->dev : NULL;
}

static bool cmd_of_pool(void *v, void *arg) { return ((struct nvhal_cmd *)v)->pool == *(const VkCommandPool *)arg; }
static bool cmd_of_dev(void *v, void *arg) { return ((struct nvhal_cmd *)v)->dev == arg; }

void nvhal_cmd_device_forget(struct nvhal_device *d) {
    atomic_fetch_add_explicit(&cmd_gen, 1, memory_order_acq_rel);
    nvhal_hash_sweep(&cmds, cmd_of_dev, d, cmd_free);
}

static VKAPI_ATTR void VKAPI_CALL xl_FreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t n,
                                                        const VkCommandBuffer *cb) {
    struct nvhal_device *d = nvhal_device_get(device);
    if (!d) {
        NVHAL_ERR("vkFreeCommandBuffers(%p): not a device created through this HAL", (void *)device);
        return;
    }
    atomic_fetch_add_explicit(&cmd_gen, 1, memory_order_acq_rel);
    for (uint32_t i = 0; i < n; i++)
        if (cb[i]) cmd_free(nvhal_hash_del(&cmds, (uint64_t)(uintptr_t)cb[i]));
    d->xl.FreeCommandBuffers(device, pool, n, cb);
}

static VKAPI_ATTR void VKAPI_CALL xl_DestroyCommandPool(VkDevice device, VkCommandPool pool,
                                                        const VkAllocationCallbacks *alloc) {
    struct nvhal_device *d = nvhal_device_get(device);
    if (!d) {
        NVHAL_ERR("vkDestroyCommandPool(%p): not a device created through this HAL", (void *)device);
        return;
    }
    if (pool) {
        atomic_fetch_add_explicit(&cmd_gen, 1, memory_order_acq_rel);
        nvhal_hash_sweep(&cmds, cmd_of_pool, &pool, cmd_free);
        nvhal_pool_forget(pool);
    }
    d->xl.DestroyCommandPool(device, pool, alloc);
}

/* ------------------------------------------------------------------ translation cost statistics
 *
 * Opt-in measurement of what VkImage translation costs per command and per frame: NVHAL_XLATE_STATS=1
 * on the host, the property debug.nvhal.xlate_stats in the guest (report window in seconds:
 * NVHAL_XLATE_STATS_WINDOW / debug.nvhal.xlate_stats.window, default 1). Off by default: xl1, xl_arr and
 * real_handle then pay one acquire atomic load and a not-taken branch -- no clock_gettime, no logging, no
 * counter write. The value picks the tier: 1 = counts and per-call timing, 2 = counts only (the counters
 * and the per-frame call counts without one clock read per call, so the reading carries no measurement
 * overhead of its own), any other positive value behaves as 1.
 *
 * Counters are per translator class ("img" = xl1, one image reference; "arr" = xl_arr, one image array;
 * "obj" = real_handle, one object handle) in two classes: "calls" (the entry was invoked) and "xlated"
 * (it named a wrapper, so the handle really had to be rewritten). One line per window carries the
 * per-frame figures and, in the timing tier, the share of the process CPU time the translation accounts
 * for. The ms come from two clock_gettime(CLOCK_MONOTONIC) reads around the translated body, so they
 * exclude the measurement itself while the "cpu" denominator includes it: the reported share slightly
 * under-estimates, never over-estimates, which is the safe direction for the go/no-go question.
 *
 * The array path is a container: its ns include the xl1 ns of its elements (those are counted in "img"),
 * so the translation total in the report is img + obj, never img + arr + obj. nvhal_xst_frame (called by
 * anb.c on the present path) counts frames and also drives the report when image commands are sparse. */
enum { XST_IMG, XST_ARR, XST_OBJ, XST_NK };
/* three flat arrays and not a struct of atomics: on i686 a struct holding _Atomic uint64_t has an
 * alignment GCC notes as ABI-relevant, and the build is -Werror clean without the noise */
static _Atomic uint64_t xst_n[XST_NK], xst_xlated[XST_NK], xst_ns[XST_NK];
static _Atomic uint64_t xst_ticks, xst_frames, xst_arr_el, xst_last_report;
static _Atomic int xst_state; /* 0 = not probed yet, 1 = off, 2 = counts + timing, 3 = counts only */
static pthread_once_t xst_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t xst_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t xst_window_ns = 1000000000ull;
static double xst_clk_ns; /* self-measured cost of one clock_gettime, timing tier only */

struct xst_snap {
    uint64_t n[XST_NK], xlated[XST_NK], ns[XST_NK], frames, el, cpu_ns, wall_ns;
};
static struct xst_snap xst_prev; /* only touched under xst_mu */

static uint64_t xst_clock(clockid_t id) {
    struct timespec t;
    clock_gettime(id, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static inline uint64_t xst_now(void) { return xst_clock(CLOCK_MONOTONIC); }

static void xst_report(uint64_t now);

/* one translator invocation: its counter class, whether it really translated, and its body's ns */
static void xst_note(int k, bool xlated, uint64_t t0, uint64_t t1) {
    atomic_fetch_add_explicit(&xst_n[k], 1, memory_order_relaxed);
    if (xlated) atomic_fetch_add_explicit(&xst_xlated[k], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&xst_ns[k], t1 - t0, memory_order_relaxed);
    /* throttled check: reporting is never on the hot path itself (trylock, one clock read) */
    if ((atomic_fetch_add_explicit(&xst_ticks, 1, memory_order_relaxed) & 1023u) == 0) xst_report(t1);
}

static void xst_init(void) {
    char v[96] = "", w[96] = "";
#ifdef __ANDROID__
    __system_property_get("debug.nvhal.xlate_stats", v);
    __system_property_get("debug.nvhal.xlate_stats.window", w);
#else
    const char *e = getenv("NVHAL_XLATE_STATS");
    if (e) snprintf(v, sizeof v, "%s", e);
    const char *we = getenv("NVHAL_XLATE_STATS_WINDOW");
    if (we) snprintf(w, sizeof w, "%s", we);
#endif
    int lvl = atoi(v);
    if (lvl <= 0) {
        atomic_store_explicit(&xst_state, 1, memory_order_release);
        return;
    }
    lvl = lvl == 2 ? 3 : 2; /* 2 = counts only; 1 or anything else positive = counts + per-call timing */
    double ws = w[0] ? atof(w) : 1.0;
    xst_window_ns = (uint64_t)((ws > 0.1 ? ws : 1.0) * 1e9);
    xst_prev.wall_ns = xst_now();
    xst_prev.cpu_ns = xst_clock(CLOCK_PROCESS_CPUTIME_ID);
    if (lvl == 2) { /* what this tier's own measurement costs, measured here in the guest, not assumed */
        uint64_t c0 = xst_now();
        for (int i = 0; i < 1000; i++) (void)xst_now();
        xst_clk_ns = (double)(xst_now() - c0) / 1000.0;
    }
    atomic_store_explicit(&xst_last_report, xst_prev.wall_ns, memory_order_relaxed);
    atomic_store_explicit(&xst_state, lvl, memory_order_release);
    NVHAL_WARN("xlate: statistics ON (tier %s, window %.2f s; NVHAL_XLATE_STATS / debug.nvhal.xlate_stats): one "
               "line per window with the per-frame call counts%s",
               lvl == 2 ? "counts+timing" : "counts only", xst_window_ns / 1e9,
               lvl == 2 ? ", the clock_gettime cost of the translators and its share of the process CPU time"
                        : " (no per-call clock reads)");
    if (lvl == 2) NVHAL_WARN("xlate: this tier pays two clock_gettime per translator entry: %.1f ns each here, "
                             "so the 'meas~' field of each line is its own per-frame overhead", xst_clk_ns);
}

/* the tier of this process: 0 = off, 2 = counts + timing, 3 = counts only */
static inline int xst_level(void) {
    /* acquire: a thread that sees the state written by xst_init must also see the window and the snapshot
     * xst_init filled in (the off value needs no other state, but publishing it costs nothing here) */
    int s = atomic_load_explicit(&xst_state, memory_order_acquire);
    if (__builtin_expect(s == 0, 0)) { /* first call in the process: read the env/property once */
        pthread_once(&xst_once, xst_init);
        s = atomic_load_explicit(&xst_state, memory_order_acquire);
    }
    return s <= 1 ? 0 : s;
}

static void xst_report(uint64_t now) {
    if (pthread_mutex_trylock(&xst_mu) != 0) return; /* another thread is reporting or just did */
    int lvl = atomic_load_explicit(&xst_state, memory_order_relaxed);
    struct xst_snap s;
    for (int i = 0; i < XST_NK; i++) {
        s.n[i] = atomic_load_explicit(&xst_n[i], memory_order_relaxed);
        s.xlated[i] = atomic_load_explicit(&xst_xlated[i], memory_order_relaxed);
        s.ns[i] = atomic_load_explicit(&xst_ns[i], memory_order_relaxed);
    }
    s.frames = atomic_load_explicit(&xst_frames, memory_order_relaxed);
    s.el = atomic_load_explicit(&xst_arr_el, memory_order_relaxed);
    s.cpu_ns = xst_clock(CLOCK_PROCESS_CPUTIME_ID);
    s.wall_ns = now;
    double win = (double)(s.wall_ns - xst_prev.wall_ns) / 1e9;
    if ((double)(s.wall_ns - xst_prev.wall_ns) < (double)xst_window_ns) { /* a second thread asked too early */
        pthread_mutex_unlock(&xst_mu);
        return;
    }
    uint64_t dn[XST_NK], dx[XST_NK], dns[XST_NK], dframes = s.frames - xst_prev.frames, del = s.el - xst_prev.el;
    for (int i = 0; i < XST_NK; i++) {
        dn[i] = s.n[i] - xst_prev.n[i];
        dx[i] = s.xlated[i] - xst_prev.xlated[i];
        dns[i] = s.ns[i] - xst_prev.ns[i];
    }
    double cpu = (double)(s.cpu_ns - xst_prev.cpu_ns);
    double total = (double)(dns[XST_IMG] + dns[XST_OBJ]);
    double frames = (double)dframes;
    if (lvl == 2) {
        NVHAL_WARN("xlate stats[timing]: %.2fs frames=%" PRIu64 " (%.1f fps) cpu=%.1fms | "
                   "img calls=%" PRIu64 " xlated=%" PRIu64 " (%.2f%%) %.3fms %.0fns/call | "
                   "arr calls=%" PRIu64 " copies=%" PRIu64 " els=%" PRIu64 " %.3fms | "
                   "obj calls=%" PRIu64 " xlated=%" PRIu64 " | "
                   "total %.3fms = %.2f%% of cpu, %.1fus/frame, %.1f calls/frame | clk=%.1fns meas~%.0fus/frame",
                   win, dframes, frames > 0 ? frames / win : 0.0, cpu / 1e6,
                   dn[XST_IMG], dx[XST_IMG],
                   dn[XST_IMG] ? 100.0 * (double)dx[XST_IMG] / (double)dn[XST_IMG] : 0.0,
                   (double)dns[XST_IMG] / 1e6, dn[XST_IMG] ? (double)dns[XST_IMG] / (double)dn[XST_IMG] : 0.0,
                   dn[XST_ARR], dx[XST_ARR], del, (double)dns[XST_ARR] / 1e6,
                   dn[XST_OBJ], dx[XST_OBJ],
                   total / 1e6, cpu > 0.0 ? 100.0 * total / cpu : 0.0,
                   frames > 0 ? total / frames / 1e3 : 0.0,
                   frames > 0 ? (double)(dn[XST_IMG] + dn[XST_OBJ]) / frames : 0.0,
                   xst_clk_ns,
                   frames > 0 ? (double)(dn[XST_IMG] + dn[XST_OBJ]) * 2.0 * xst_clk_ns / frames / 1e3 : 0.0);
    } else {
        NVHAL_WARN("xlate stats[counts]: %.2fs frames=%" PRIu64 " (%.1f fps) cpu=%.1fms | "
                   "img calls=%" PRIu64 " xlated=%" PRIu64 " (%.2f%%) | "
                   "arr calls=%" PRIu64 " copies=%" PRIu64 " els=%" PRIu64 " | "
                   "obj calls=%" PRIu64 " xlated=%" PRIu64 " | "
                   "%.1f calls/frame (no per-call timing in this tier)",
                   win, dframes, frames > 0 ? frames / win : 0.0, cpu / 1e6,
                   dn[XST_IMG], dx[XST_IMG],
                   dn[XST_IMG] ? 100.0 * (double)dx[XST_IMG] / (double)dn[XST_IMG] : 0.0,
                   dn[XST_ARR], dx[XST_ARR], del,
                   dn[XST_OBJ], dx[XST_OBJ],
                   frames > 0 ? (double)(dn[XST_IMG] + dn[XST_OBJ]) / frames : 0.0);
    }
    xst_prev = s;
    atomic_store_explicit(&xst_last_report, s.wall_ns, memory_order_relaxed);
    pthread_mutex_unlock(&xst_mu);
}

/* one presented frame (anb.c, the present path): counted, and the report deadline is checked here too so
 * that a run whose image commands stop still gets its last window. No-op unless the statistics are on. */
void nvhal_xst_frame(void) {
    if (__builtin_expect(xst_level() == 0, 1)) return;
    atomic_fetch_add_explicit(&xst_frames, 1, memory_order_relaxed);
    uint64_t now = xst_now();
    if (now - atomic_load_explicit(&xst_last_report, memory_order_relaxed) >= xst_window_ns) xst_report(now);
}

/* ------------------------------------------------------------------ helpers */

#define DEV(fn)                                                                                     \
    struct nvhal_device *d = nvhal_device_get(device);                                              \
    if (!d) {                                                                                       \
        NVHAL_ERR("%s(%p): not a device created through this HAL", fn, (void *)device);             \
        return DEV_FAIL;                                                                            \
    }
#define CMD(fn)                                                                                     \
    struct nvhal_device *d = nvhal_cmd_device(cb);                                                  \
    if (!d) {                                                                                       \
        NVHAL_ERR("%s(%p): command buffer was not allocated through this HAL; dropped", fn, (void *)cb); \
        return;                                                                                     \
    }

/* translate one image in place; *bad when it is a wrapper without memory. With the statistics on this is
 * the per-image-reference entry: one "img call" per invocation, one "xlated" when a wrapper was found. */
static inline bool xl1(VkImage *img, bool *bad, const char *what) {
    if (nvhal_no_wrappers() || *img == VK_NULL_HANDLE) return false;
    int st = xst_level();
    uint64_t t0 = st == 2 ? xst_now() : 0;
    bool hit = false;
    VkImage r = nvhal_image_xl(*img, &hit, what);
    if (hit) {
        if (r == VK_NULL_HANDLE) *bad = true;
        *img = r;
    }
    if (__builtin_expect(st != 0, 0)) xst_note(XST_IMG, hit, t0, st == 2 ? xst_now() : 0);
    return hit;
}

/* Array of n structs of `size` bytes with a VkImage at `off`: returns src when no element names a
 * wrapper, else a translated copy (in `stack` when it fits, else malloc'd: free with xl_free). */
static void *xl_arr(const void *src, uint32_t n, size_t size, size_t off, void *stack, size_t stack_bytes, bool *bad,
                    const char *what) {
    if (!src || !n || nvhal_no_wrappers()) return (void *)src;
    int st = xst_level();
    uint64_t t0 = st == 2 ? xst_now() : 0;
    uint32_t nxl = 0; /* elements translated; each of them is also counted by xl1 as one img call */
    unsigned char *dst = NULL;
    for (uint32_t i = 0; i < n; i++) {
        VkImage img;
        memcpy(&img, (const unsigned char *)src + i * size + off, sizeof img);
        if (!xl1(&img, bad, what)) continue;
        nxl++;
        if (!dst) {
            dst = (size_t)n * size <= stack_bytes ? stack : malloc((size_t)n * size);
            if (!dst) {
                NVHAL_ERR("%s: out of host memory translating %u structs", what, n);
                *bad = true;
                return (void *)src;
            }
            memcpy(dst, src, (size_t)n * size);
        }
        memcpy(dst + i * size + off, &img, sizeof img);
    }
    if (__builtin_expect(st != 0, 0)) {
        atomic_fetch_add_explicit(&xst_arr_el, n, memory_order_relaxed);
        xst_note(XST_ARR, nxl != 0, t0, st == 2 ? xst_now() : 0);
    }
    return dst ? dst : (void *)src;
}
static void xl_free(const void *p, const void *src, const void *stack) {
    if (p != src && p != stack) free((void *)p);
}
#define ARR(src, n, T, member, stackbuf, bad, what) \
    ((const T *)xl_arr((src), (n), sizeof(T), offsetof(T, member), (stackbuf), sizeof(stackbuf), (bad), (what)))

/* ------------------------------------------------------------------ pNext surgery */

bool nvhal_chain_replace(const void *head, const void *drop, const void *add, const void **add_tail_next,
                         void *storage, size_t storage_size, const char *who, const void **out) {
    const void *rest = drop ? ((const VkBaseInStructure *)drop)->pNext : head;
    const void *first = NULL;
    VkBaseOutStructure *last = NULL;
    size_t used = 0;
    if (drop)
        for (const VkBaseInStructure *s = head; s && s != (const VkBaseInStructure *)drop; s = s->pNext) {
            size_t sz = nvhal_stype_size(s->sType);
            if (!sz) {
                NVHAL_ERR("%s: structure sType %d is chained before the one the HAL must replace and is unknown to "
                          "this build (%s); the call cannot be translated", who, (int)s->sType, nvhal_registry_version());
                return false;
            }
            used = (used + 15) & ~(size_t)15;
            if (used + sz > storage_size) {
                NVHAL_ERR("%s: pNext chain too long to copy (%zu bytes of storage)", who, storage_size);
                return false;
            }
            VkBaseOutStructure *c = (void *)((unsigned char *)storage + used);
            memcpy(c, s, sz);
            used += sz;
            if (last) last->pNext = c;
            else first = c;
            last = c;
        }
    if (last) last->pNext = (VkBaseOutStructure *)rest;
    else first = rest;
    if (add) {
        *add_tail_next = first;
        *out = add;
    } else {
        *out = first;
    }
    return true;
}

/* ------------------------------------------------------------------ device-level image queries */

#define DEV_FAIL
static VKAPI_ATTR void VKAPI_CALL xl_GetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *r) {
    DEV("vkGetImageMemoryRequirements")
    if (!nvhal_no_astc()) {
        VkMemoryRequirements2 a2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        if (nvhal_astc_image_requirements(image, &a2)) {
            *r = a2.memoryRequirements;
            return;
        }
    }
    bool bad = false;
    VkImage i = image;
    if (xl1(&i, &bad, NULL) && bad) {
        VkMemoryRequirements2 r2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        nvhal_ahb_image_requirements(d, image, &r2);
        *r = r2.memoryRequirements;
        return;
    }
    d->xl.GetImageMemoryRequirements(device, i, r);
}

static void image_reqs2(VkDevice device, const VkImageMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *r, bool khr) {
    DEV("vkGetImageMemoryRequirements2")
    PFN_vkGetImageMemoryRequirements2 f = khr ? d->xl.GetImageMemoryRequirements2KHR : d->xl.GetImageMemoryRequirements2;
    if (nvhal_astc_image_requirements(info->image, r)) return; /* the RGBA8 image + its block images */
    VkImageMemoryRequirementsInfo2 t = *info;
    bool bad = false;
    if (!xl1(&t.image, &bad, NULL)) {
        f(device, info, r);
    } else if (bad) {
        nvhal_ahb_image_requirements(d, info->image, r);
    } else {
        f(device, &t, r);
    }
}
static VKAPI_ATTR void VKAPI_CALL xl_GetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *i,
                                                                 VkMemoryRequirements2 *r) {
    image_reqs2(device, i, r, false);
}
static VKAPI_ATTR void VKAPI_CALL xl_GetImageMemoryRequirements2KHR(VkDevice device,
                                                                    const VkImageMemoryRequirementsInfo2 *i,
                                                                    VkMemoryRequirements2 *r) {
    image_reqs2(device, i, r, true);
}

static VKAPI_ATTR void VKAPI_CALL xl_GetImageSparseMemoryRequirements(VkDevice device, VkImage image, uint32_t *n,
                                                                      VkSparseImageMemoryRequirements *r) {
    DEV("vkGetImageSparseMemoryRequirements")
    bool bad = false;
    if (xl1(&image, &bad, NULL) && bad) {
        *n = 0; /* AHB images are never sparse */
        return;
    }
    d->xl.GetImageSparseMemoryRequirements(device, image, n, r);
}

static void sparse_reqs2(VkDevice device, const VkImageSparseMemoryRequirementsInfo2 *info, uint32_t *n,
                         VkSparseImageMemoryRequirements2 *r, bool khr) {
    DEV("vkGetImageSparseMemoryRequirements2")
    PFN_vkGetImageSparseMemoryRequirements2 f =
        khr ? d->xl.GetImageSparseMemoryRequirements2KHR : d->xl.GetImageSparseMemoryRequirements2;
    VkImageSparseMemoryRequirementsInfo2 t = *info;
    bool bad = false;
    if (!xl1(&t.image, &bad, NULL)) f(device, info, n, r);
    else if (bad) *n = 0;
    else f(device, &t, n, r);
}
static VKAPI_ATTR void VKAPI_CALL xl_GetImageSparseMemoryRequirements2(VkDevice device,
                                                                       const VkImageSparseMemoryRequirementsInfo2 *i,
                                                                       uint32_t *n, VkSparseImageMemoryRequirements2 *r) {
    sparse_reqs2(device, i, n, r, false);
}
static VKAPI_ATTR void VKAPI_CALL xl_GetImageSparseMemoryRequirements2KHR(VkDevice device,
                                                                          const VkImageSparseMemoryRequirementsInfo2 *i,
                                                                          uint32_t *n, VkSparseImageMemoryRequirements2 *r) {
    sparse_reqs2(device, i, n, r, true);
}

/* A wrapper's ICD image has DRM-format-modifier tiling: plane layouts are addressed with
 * MEMORY_PLANE_i aspects, where the app (whose image is LINEAR) says COLOR. */
static VkImageAspectFlags plane_aspect(VkImageAspectFlags a) {
    return a == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT : a;
}

static VKAPI_ATTR void VKAPI_CALL xl_GetImageSubresourceLayout(VkDevice device, VkImage image,
                                                               const VkImageSubresource *sub, VkSubresourceLayout *l) {
    DEV("vkGetImageSubresourceLayout")
    bool bad = false;
    if (!xl1(&image, &bad, "vkGetImageSubresourceLayout")) {
        d->xl.GetImageSubresourceLayout(device, image, sub, l);
    } else if (bad) {
        memset(l, 0, sizeof *l);
    } else {
        VkImageSubresource s = *sub;
        s.aspectMask = plane_aspect(s.aspectMask);
        d->xl.GetImageSubresourceLayout(device, image, &s, l);
    }
}

static void subres2(VkDevice device, VkImage image, const VkImageSubresource2 *sub, VkSubresourceLayout2 *l,
                    PFN_vkGetImageSubresourceLayout2 f) {
    bool bad = false;
    if (!xl1(&image, &bad, "vkGetImageSubresourceLayout2")) {
        f(device, image, sub, l);
    } else if (bad) {
        memset(&l->subresourceLayout, 0, sizeof l->subresourceLayout);
    } else {
        VkImageSubresource2 s = *sub;
        s.imageSubresource.aspectMask = plane_aspect(s.imageSubresource.aspectMask);
        f(device, image, &s, l);
    }
}
static VKAPI_ATTR void VKAPI_CALL xl_GetImageSubresourceLayout2(VkDevice device, VkImage image,
                                                                const VkImageSubresource2 *s, VkSubresourceLayout2 *l) {
    DEV("vkGetImageSubresourceLayout2")
    subres2(device, image, s, l, d->xl.GetImageSubresourceLayout2);
}
static VKAPI_ATTR void VKAPI_CALL xl_GetImageSubresourceLayout2KHR(VkDevice device, VkImage image,
                                                                   const VkImageSubresource2 *s, VkSubresourceLayout2 *l) {
    DEV("vkGetImageSubresourceLayout2KHR")
    subres2(device, image, s, l, d->xl.GetImageSubresourceLayout2KHR);
}
static VKAPI_ATTR void VKAPI_CALL xl_GetImageSubresourceLayout2EXT(VkDevice device, VkImage image,
                                                                   const VkImageSubresource2 *s, VkSubresourceLayout2 *l) {
    DEV("vkGetImageSubresourceLayout2EXT")
    subres2(device, image, s, l, d->xl.GetImageSubresourceLayout2EXT);
}

#undef DEV_FAIL
#define DEV_FAIL VK_ERROR_INITIALIZATION_FAILED

static VKAPI_ATTR VkResult VKAPI_CALL xl_GetImageDrmFormatModifierPropertiesEXT(
    VkDevice device, VkImage image, VkImageDrmFormatModifierPropertiesEXT *p) {
    DEV("vkGetImageDrmFormatModifierPropertiesEXT")
    bool bad = false;
    if (xl1(&image, &bad, "vkGetImageDrmFormatModifierPropertiesEXT") && bad) return VK_ERROR_UNKNOWN;
    return d->xl.GetImageDrmFormatModifierPropertiesEXT(device, image, p);
}

static VKAPI_ATTR VkResult VKAPI_CALL xl_CreateImageView(VkDevice device, const VkImageViewCreateInfo *ci,
                                                         const VkAllocationCallbacks *alloc, VkImageView *view) {
    DEV("vkCreateImageView")
    VkResult ar;
    if (nvhal_astc_CreateImageView(d, device, ci, alloc, view, &ar)) return ar;
    VkImageViewCreateInfo t = *ci;
    bool bad = false;
    if (!xl1(&t.image, &bad, "vkCreateImageView")) return d->xl.CreateImageView(device, ci, alloc, view);
    if (bad) return VK_ERROR_UNKNOWN;
    if (t.format == VK_FORMAT_UNDEFINED) t.format = nvhal_image_real_format(ci->image); /* external format */
    return d->xl.CreateImageView(device, &t, alloc, view);
}

static VKAPI_ATTR VkResult VKAPI_CALL xl_GetImageOpaqueCaptureDescriptorDataEXT(
    VkDevice device, const VkImageCaptureDescriptorDataInfoEXT *info, void *data) {
    DEV("vkGetImageOpaqueCaptureDescriptorDataEXT")
    VkImageCaptureDescriptorDataInfoEXT t = *info;
    bool bad = false;
    if (!xl1(&t.image, &bad, "vkGetImageOpaqueCaptureDescriptorDataEXT"))
        return d->xl.GetImageOpaqueCaptureDescriptorDataEXT(device, info, data);
    if (bad) return VK_ERROR_UNKNOWN;
    return d->xl.GetImageOpaqueCaptureDescriptorDataEXT(device, &t, data);
}

/* ------------------------------------------------------------------ host image copy (1.4 / EXT) */

#define HOST_COPY(Name, Info, field1, field2)                                                               \
    static VkResult host_##Name(VkDevice device, const Info *info, PFN_vk##Name f) {                         \
        Info t = *info;                                                                                     \
        if (nvhal_astc_refuse(info->field1, "vk" #Name) || nvhal_astc_refuse(info->field2, "vk" #Name))      \
            return VK_ERROR_FORMAT_NOT_SUPPORTED; /* ASTC images have no HOST_TRANSFER usage */              \
        bool bad = false, hit = xl1(&t.field1, &bad, "vk" #Name);                                            \
        hit |= xl1(&t.field2, &bad, "vk" #Name);                                                             \
        if (bad) return VK_ERROR_UNKNOWN;                                                                   \
        return f(device, hit ? &t : info);                                                                  \
    }                                                                                                       \
    static VKAPI_ATTR VkResult VKAPI_CALL xl_##Name(VkDevice device, const Info *info) {                     \
        DEV("vk" #Name)                                                                                     \
        return host_##Name(device, info, d->xl.Name);                                                       \
    }                                                                                                       \
    static VKAPI_ATTR VkResult VKAPI_CALL xl_##Name##EXT(VkDevice device, const Info *info) {                \
        DEV("vk" #Name "EXT")                                                                               \
        return host_##Name(device, info, d->xl.Name##EXT);                                                  \
    }
HOST_COPY(CopyImageToImage, VkCopyImageToImageInfo, srcImage, dstImage)
HOST_COPY(CopyImageToMemory, VkCopyImageToMemoryInfo, srcImage, srcImage)
HOST_COPY(CopyMemoryToImage, VkCopyMemoryToImageInfo, dstImage, dstImage)

static VkResult transition(VkDevice device, uint32_t n, const VkHostImageLayoutTransitionInfo *tr,
                           PFN_vkTransitionImageLayout f) {
    for (uint32_t i = 0; i < n; i++)
        if (nvhal_astc_refuse(tr[i].image, "vkTransitionImageLayout")) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkHostImageLayoutTransitionInfo st[8];
    bool bad = false;
    const VkHostImageLayoutTransitionInfo *t = ARR(tr, n, VkHostImageLayoutTransitionInfo, image, st, &bad,
                                                   "vkTransitionImageLayout");
    VkResult r = bad ? VK_ERROR_UNKNOWN : f(device, n, t);
    xl_free(t, tr, st);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL xl_TransitionImageLayout(VkDevice device, uint32_t n,
                                                               const VkHostImageLayoutTransitionInfo *t) {
    DEV("vkTransitionImageLayout")
    return transition(device, n, t, d->xl.TransitionImageLayout);
}
static VKAPI_ATTR VkResult VKAPI_CALL xl_TransitionImageLayoutEXT(VkDevice device, uint32_t n,
                                                                  const VkHostImageLayoutTransitionInfo *t) {
    DEV("vkTransitionImageLayoutEXT")
    return transition(device, n, t, d->xl.TransitionImageLayoutEXT);
}

/* ------------------------------------------------------------------ object-handle commands */

static bool image_obj(VkObjectType t, uint64_t h) { return t == VK_OBJECT_TYPE_IMAGE && nvhal_image_wrapper_handle(h); }

static uint64_t real_handle(uint64_t h, bool *unrealised, const char *what) {
    (void)what; /* naming / private data before the image has memory is valid: no log */
    int st = xst_level();
    uint64_t t0 = st == 2 ? xst_now() : 0;
    bool hit = false;
    VkImage r = nvhal_image_xl(NVHAL_U64_ND(VkImage, h), &hit, NULL);
    *unrealised = hit && r == VK_NULL_HANDLE;
    if (__builtin_expect(st != 0, 0)) xst_note(XST_OBJ, hit, t0, st == 2 ? xst_now() : 0);
    return NVHAL_ND_U64(r);
}

/* before realisation the name/private data live on the wrapper (ahb.c) and are replayed */
static VKAPI_ATTR VkResult VKAPI_CALL xl_SetDebugUtilsObjectNameEXT(VkDevice device,
                                                                    const VkDebugUtilsObjectNameInfoEXT *info) {
    DEV("vkSetDebugUtilsObjectNameEXT")
    if (nvhal_no_wrappers() || !image_obj(info->objectType, info->objectHandle))
        return d->xl.SetDebugUtilsObjectNameEXT(device, info);
    VkDebugUtilsObjectNameInfoEXT t = *info;
    bool unreal;
    if (nvhal_ahb_set_object_name(info->objectHandle, info->pObjectName) &&
        !(t.objectHandle = real_handle(info->objectHandle, &unreal, "vkSetDebugUtilsObjectNameEXT"), unreal))
        return d->xl.SetDebugUtilsObjectNameEXT(device, &t);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL xl_SetDebugUtilsObjectTagEXT(VkDevice device,
                                                                   const VkDebugUtilsObjectTagInfoEXT *info) {
    DEV("vkSetDebugUtilsObjectTagEXT")
    if (nvhal_no_wrappers() || !image_obj(info->objectType, info->objectHandle))
        return d->xl.SetDebugUtilsObjectTagEXT(device, info);
    VkDebugUtilsObjectTagInfoEXT t = *info;
    bool unreal;
    t.objectHandle = real_handle(info->objectHandle, &unreal, "vkSetDebugUtilsObjectTagEXT");
    if (unreal) {
        NVHAL_WARN("vkSetDebugUtilsObjectTagEXT: AHB image without memory yet; the tag is not kept");
        return VK_SUCCESS;
    }
    return d->xl.SetDebugUtilsObjectTagEXT(device, &t);
}

static VKAPI_ATTR VkResult VKAPI_CALL xl_DebugMarkerSetObjectNameEXT(VkDevice device,
                                                                     const VkDebugMarkerObjectNameInfoEXT *info) {
    DEV("vkDebugMarkerSetObjectNameEXT")
    if (nvhal_no_wrappers() || info->objectType != VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT ||
        !nvhal_image_wrapper_handle(info->object))
        return d->xl.DebugMarkerSetObjectNameEXT(device, info);
    VkDebugMarkerObjectNameInfoEXT t = *info;
    bool unreal;
    t.object = real_handle(info->object, &unreal, "vkDebugMarkerSetObjectNameEXT");
    if (unreal) {
        nvhal_ahb_set_object_name(info->object, info->pObjectName);
        return VK_SUCCESS;
    }
    return d->xl.DebugMarkerSetObjectNameEXT(device, &t);
}

static VKAPI_ATTR VkResult VKAPI_CALL xl_DebugMarkerSetObjectTagEXT(VkDevice device,
                                                                    const VkDebugMarkerObjectTagInfoEXT *info) {
    DEV("vkDebugMarkerSetObjectTagEXT")
    if (nvhal_no_wrappers() || info->objectType != VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT ||
        !nvhal_image_wrapper_handle(info->object))
        return d->xl.DebugMarkerSetObjectTagEXT(device, info);
    VkDebugMarkerObjectTagInfoEXT t = *info;
    bool unreal;
    t.object = real_handle(info->object, &unreal, "vkDebugMarkerSetObjectTagEXT");
    if (unreal) {
        NVHAL_WARN("vkDebugMarkerSetObjectTagEXT: AHB image without memory yet; the tag is not kept");
        return VK_SUCCESS;
    }
    return d->xl.DebugMarkerSetObjectTagEXT(device, &t);
}

/* Private data of objects the Android loader owns. VkSwapchainKHR (and VkSurfaceKHR) are the loader's
 * (swapchain.cpp); it has no private-data handling (no PrivateData in libvulkan's swapchain.cpp, driver.cpp,
 * api.cpp), so vkSet/GetPrivateData on them reach the driver with a handle the ICD never issued, and NVIDIA's
 * ICD dereferences it (SIGSEGV in libnvidia-eglcore, dEQP-VK.wsi.android.swapchain.private_data.*). As Mesa
 * does on Android (src/vulkan/runtime/vk_object.c vk_object_base_private_data), the HAL keeps those values
 * itself, per device, keyed by (object type, handle, slot); the ICD never sees the handle. A never-set value
 * reads 0 (spec); a destroyed slot's values are dropped (vkDestroyPrivateDataSlot), so a recycled slot handle
 * starts at 0. The loader does not tell the driver when a swapchain dies: values of a destroyed swapchain stay
 * until the slot or the device is destroyed (a later swapchain at the same address would read them; Mesa
 * has the same property). */
static bool loader_obj(VkObjectType t) { return t == VK_OBJECT_TYPE_SWAPCHAIN_KHR || t == VK_OBJECT_TYPE_SURFACE_KHR; }

static VkResult loader_pd_set(struct nvhal_device *d, VkObjectType type, uint64_t h, VkPrivateDataSlot slot,
                              uint64_t data) {
    const uint64_t sl = NVHAL_ND_U64(slot);
    VkResult r = VK_SUCCESS;
    pthread_mutex_lock(&nvhal_lock);
    uint32_t i = 0;
    while (i < d->loader_pd_count &&
           !(d->loader_pd[i].type == type && d->loader_pd[i].handle == h && d->loader_pd[i].slot == sl))
        i++;
    if (i == d->loader_pd_count) {
        if (d->loader_pd_count == d->loader_pd_cap) {
            uint32_t cap = d->loader_pd_cap ? d->loader_pd_cap * 2 : 16;
            struct nvhal_loader_pd *p = realloc(d->loader_pd, cap * sizeof *p);
            if (p) { d->loader_pd = p; d->loader_pd_cap = cap; }
        }
        if (d->loader_pd_count < d->loader_pd_cap)
            d->loader_pd[d->loader_pd_count++] = (struct nvhal_loader_pd){type, h, sl, 0};
        else
            r = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (r == VK_SUCCESS) d->loader_pd[i].data = data;
    pthread_mutex_unlock(&nvhal_lock);
    if (r != VK_SUCCESS)
        NVHAL_ERR("vkSetPrivateData: out of host memory storing private data of loader object 0x%" PRIx64
                  " (object type %d) -> VK_ERROR_OUT_OF_HOST_MEMORY", h, (int)type);
    else
        NVHAL_TRACE("vkSetPrivateData: loader object 0x%" PRIx64 " (object type %d) slot 0x%" PRIx64 " = 0x%" PRIx64
                    " (kept by the HAL)", h, (int)type, sl, data);
    return r;
}

static uint64_t loader_pd_get(struct nvhal_device *d, VkObjectType type, uint64_t h, VkPrivateDataSlot slot) {
    const uint64_t sl = NVHAL_ND_U64(slot);
    uint64_t v = 0;
    pthread_mutex_lock(&nvhal_lock);
    for (uint32_t i = 0; i < d->loader_pd_count; i++)
        if (d->loader_pd[i].type == type && d->loader_pd[i].handle == h && d->loader_pd[i].slot == sl) {
            v = d->loader_pd[i].data;
            break;
        }
    pthread_mutex_unlock(&nvhal_lock);
    return v;
}

static void loader_pd_forget_slot(struct nvhal_device *d, VkPrivateDataSlot slot) {
    const uint64_t sl = NVHAL_ND_U64(slot);
    pthread_mutex_lock(&nvhal_lock);
    for (uint32_t i = 0; i < d->loader_pd_count;)
        if (d->loader_pd[i].slot == sl) d->loader_pd[i] = d->loader_pd[--d->loader_pd_count];
        else i++;
    pthread_mutex_unlock(&nvhal_lock);
}

void nvhal_loader_pd_teardown(struct nvhal_device *d) {
    pthread_mutex_lock(&nvhal_lock);
    free(d->loader_pd);
    d->loader_pd = NULL;
    d->loader_pd_count = d->loader_pd_cap = 0;
    pthread_mutex_unlock(&nvhal_lock);
}

static VkResult set_pd(struct nvhal_device *d, VkDevice device, VkObjectType type, uint64_t h, VkPrivateDataSlot slot,
                       uint64_t data, PFN_vkSetPrivateData f) {
    if (loader_obj(type)) return loader_pd_set(d, type, h, slot, data);
    if (nvhal_no_wrappers() || !image_obj(type, h)) return f(device, type, h, slot, data);
    nvhal_ahb_set_private_data(h, slot, data); /* kept for replay should the image be (re)realised */
    bool unreal;
    uint64_t r = real_handle(h, &unreal, "vkSetPrivateData");
    return unreal ? VK_SUCCESS : f(device, type, r, slot, data);
}
static VKAPI_ATTR VkResult VKAPI_CALL xl_SetPrivateData(VkDevice device, VkObjectType t, uint64_t h,
                                                        VkPrivateDataSlot s, uint64_t v) {
    DEV("vkSetPrivateData")
    return set_pd(d, device, t, h, s, v, d->xl.SetPrivateData);
}
static VKAPI_ATTR VkResult VKAPI_CALL xl_SetPrivateDataEXT(VkDevice device, VkObjectType t, uint64_t h,
                                                           VkPrivateDataSlot s, uint64_t v) {
    DEV("vkSetPrivateDataEXT")
    return set_pd(d, device, t, h, s, v, d->xl.SetPrivateDataEXT);
}

#undef DEV_FAIL
#define DEV_FAIL
static void get_pd(struct nvhal_device *d, VkDevice device, VkObjectType type, uint64_t h, VkPrivateDataSlot slot,
                   uint64_t *data, PFN_vkGetPrivateData f) {
    if (loader_obj(type)) {
        *data = loader_pd_get(d, type, h, slot);
        return;
    }
    if (nvhal_no_wrappers() || !image_obj(type, h)) {
        f(device, type, h, slot, data);
        return;
    }
    bool unreal;
    uint64_t r = real_handle(h, &unreal, "vkGetPrivateData");
    if (unreal) nvhal_ahb_get_private_data(h, slot, data);
    else f(device, type, r, slot, data);
}
static VKAPI_ATTR void VKAPI_CALL xl_GetPrivateData(VkDevice device, VkObjectType t, uint64_t h, VkPrivateDataSlot s,
                                                    uint64_t *v) {
    DEV("vkGetPrivateData")
    get_pd(d, device, t, h, s, v, d->xl.GetPrivateData);
}
static VKAPI_ATTR void VKAPI_CALL xl_GetPrivateDataEXT(VkDevice device, VkObjectType t, uint64_t h,
                                                       VkPrivateDataSlot s, uint64_t *v) {
    DEV("vkGetPrivateDataEXT")
    get_pd(d, device, t, h, s, v, d->xl.GetPrivateDataEXT);
}
static VKAPI_ATTR void VKAPI_CALL xl_DestroyPrivateDataSlot(VkDevice device, VkPrivateDataSlot slot,
                                                            const VkAllocationCallbacks *alloc) {
    DEV("vkDestroyPrivateDataSlot")
    if (slot != VK_NULL_HANDLE) loader_pd_forget_slot(d, slot);
    d->xl.DestroyPrivateDataSlot(device, slot, alloc);
}
static VKAPI_ATTR void VKAPI_CALL xl_DestroyPrivateDataSlotEXT(VkDevice device, VkPrivateDataSlot slot,
                                                               const VkAllocationCallbacks *alloc) {
    DEV("vkDestroyPrivateDataSlotEXT")
    if (slot != VK_NULL_HANDLE) loader_pd_forget_slot(d, slot);
    d->xl.DestroyPrivateDataSlotEXT(device, slot, alloc);
}

/* ------------------------------------------------------------------ create-info queries */

static void dev_image_reqs(VkDevice device, const VkDeviceImageMemoryRequirements *info, VkMemoryRequirements2 *r,
                           bool khr) {
    DEV("vkGetDeviceImageMemoryRequirements")
    if (nvhal_astc_ci_requirements(d, info->pCreateInfo, r, khr)) return;
    if (d->ahb_enabled && nvhal_ci_is_ahb(info->pCreateInfo)) {
        nvhal_ahb_ci_requirements(d, info->pCreateInfo, r, khr);
        return;
    }
    (khr ? d->xl.GetDeviceImageMemoryRequirementsKHR : d->xl.GetDeviceImageMemoryRequirements)(device, info, r);
}
static VKAPI_ATTR void VKAPI_CALL xl_GetDeviceImageMemoryRequirements(VkDevice device,
                                                                      const VkDeviceImageMemoryRequirements *i,
                                                                      VkMemoryRequirements2 *r) {
    dev_image_reqs(device, i, r, false);
}
static VKAPI_ATTR void VKAPI_CALL xl_GetDeviceImageMemoryRequirementsKHR(VkDevice device,
                                                                         const VkDeviceImageMemoryRequirements *i,
                                                                         VkMemoryRequirements2 *r) {
    dev_image_reqs(device, i, r, true);
}

static void dev_sparse_reqs(VkDevice device, const VkDeviceImageMemoryRequirements *info, uint32_t *n,
                            VkSparseImageMemoryRequirements2 *r, bool khr) {
    DEV("vkGetDeviceImageSparseMemoryRequirements")
    if (nvhal_astc_ci_refused(d, info->pCreateInfo, "vkGetDeviceImageSparseMemoryRequirements")) {
        *n = 0; /* emulated ASTC images are never sparse */
        return;
    }
    if (d->ahb_enabled && nvhal_ci_is_ahb(info->pCreateInfo)) {
        *n = 0; /* AHB images are never sparse (vkCreateImage refuses sparse flags with the AHB type) */
        return;
    }
    (khr ? d->xl.GetDeviceImageSparseMemoryRequirementsKHR : d->xl.GetDeviceImageSparseMemoryRequirements)(device, info,
                                                                                                           n, r);
}
static VKAPI_ATTR void VKAPI_CALL xl_GetDeviceImageSparseMemoryRequirements(VkDevice device,
                                                                            const VkDeviceImageMemoryRequirements *i,
                                                                            uint32_t *n,
                                                                            VkSparseImageMemoryRequirements2 *r) {
    dev_sparse_reqs(device, i, n, r, false);
}
static VKAPI_ATTR void VKAPI_CALL xl_GetDeviceImageSparseMemoryRequirementsKHR(VkDevice device,
                                                                               const VkDeviceImageMemoryRequirements *i,
                                                                               uint32_t *n,
                                                                               VkSparseImageMemoryRequirements2 *r) {
    dev_sparse_reqs(device, i, n, r, true);
}

static void dev_subres(VkDevice device, const VkDeviceImageSubresourceInfo *info, VkSubresourceLayout2 *l, bool khr) {
    DEV("vkGetDeviceImageSubresourceLayout")
    if (nvhal_astc_ci_refused(d, info->pCreateInfo, "vkGetDeviceImageSubresourceLayout")) {
        memset(&l->subresourceLayout, 0, sizeof l->subresourceLayout); /* OPTIMAL only: no host-visible layout */
        return;
    }
    if (d->ahb_enabled && nvhal_ci_is_ahb(info->pCreateInfo)) {
        NVHAL_ERR("vkGetDeviceImageSubresourceLayout: an AHB image's layout is the buffer's (stride/offset come with "
                  "the buffer); there is no layout before the buffer is known: zero layout returned");
        memset(&l->subresourceLayout, 0, sizeof l->subresourceLayout);
        return;
    }
    (khr ? d->xl.GetDeviceImageSubresourceLayoutKHR : d->xl.GetDeviceImageSubresourceLayout)(device, info, l);
}
static VKAPI_ATTR void VKAPI_CALL xl_GetDeviceImageSubresourceLayout(VkDevice device,
                                                                     const VkDeviceImageSubresourceInfo *i,
                                                                     VkSubresourceLayout2 *l) {
    dev_subres(device, i, l, false);
}
static VKAPI_ATTR void VKAPI_CALL xl_GetDeviceImageSubresourceLayoutKHR(VkDevice device,
                                                                        const VkDeviceImageSubresourceInfo *i,
                                                                        VkSubresourceLayout2 *l) {
    dev_subres(device, i, l, true);
}

static void dev_buffer_reqs(VkDevice device, const VkDeviceBufferMemoryRequirements *info, VkMemoryRequirements2 *r,
                            bool khr) {
    DEV("vkGetDeviceBufferMemoryRequirements")
    PFN_vkGetDeviceBufferMemoryRequirements f =
        khr ? d->xl.GetDeviceBufferMemoryRequirementsKHR : d->xl.GetDeviceBufferMemoryRequirements;
    if (!d->ahb_enabled || !nvhal_buffer_ci_is_ahb(info->pCreateInfo)) {
        f(device, info, r);
        return;
    }
    unsigned char storage[1024];
    VkBufferCreateInfo ci;
    if (nvhal_buffer_ci_translate(info->pCreateInfo, &ci, storage, sizeof storage) != VK_SUCCESS) {
        memset(&r->memoryRequirements, 0, sizeof r->memoryRequirements);
        return;
    }
    VkDeviceBufferMemoryRequirements t = *info;
    t.pCreateInfo = &ci;
    f(device, &t, r);
}
static VKAPI_ATTR void VKAPI_CALL xl_GetDeviceBufferMemoryRequirements(VkDevice device,
                                                                       const VkDeviceBufferMemoryRequirements *i,
                                                                       VkMemoryRequirements2 *r) {
    dev_buffer_reqs(device, i, r, false);
}
static VKAPI_ATTR void VKAPI_CALL xl_GetDeviceBufferMemoryRequirementsKHR(VkDevice device,
                                                                          const VkDeviceBufferMemoryRequirements *i,
                                                                          VkMemoryRequirements2 *r) {
    dev_buffer_reqs(device, i, r, true);
}

/* ------------------------------------------------------------------ transfer / clear commands */

static VKAPI_ATTR void VKAPI_CALL xl_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout srcL, VkImage dst,
                                                  VkImageLayout dstL, uint32_t n, const VkImageBlit *r, VkFilter f) {
    CMD("vkCmdBlitImage")
    bool bad = false;
    xl1(&src, &bad, "vkCmdBlitImage");
    xl1(&dst, &bad, "vkCmdBlitImage");
    if (!bad) d->xl.CmdBlitImage(cb, src, srcL, dst, dstL, n, r, f);
}
static VKAPI_ATTR void VKAPI_CALL xl_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout srcL, VkImage dst,
                                                  VkImageLayout dstL, uint32_t n, const VkImageCopy *r) {
    CMD("vkCmdCopyImage")
    bool bad = false;
    xl1(&src, &bad, "vkCmdCopyImage");
    xl1(&dst, &bad, "vkCmdCopyImage");
    if (bad || nvhal_astc_cmd_copy_image(d, cb, src, srcL, dst, dstL, n, r)) return;
    d->xl.CmdCopyImage(cb, src, srcL, dst, dstL, n, r);
}
static VKAPI_ATTR void VKAPI_CALL xl_CmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout srcL, VkImage dst,
                                                     VkImageLayout dstL, uint32_t n, const VkImageResolve *r) {
    CMD("vkCmdResolveImage")
    bool bad = false;
    xl1(&src, &bad, "vkCmdResolveImage");
    xl1(&dst, &bad, "vkCmdResolveImage");
    if (!bad) d->xl.CmdResolveImage(cb, src, srcL, dst, dstL, n, r);
}
static VKAPI_ATTR void VKAPI_CALL xl_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst,
                                                          VkImageLayout dstL, uint32_t n, const VkBufferImageCopy *r) {
    CMD("vkCmdCopyBufferToImage")
    bool bad = false;
    xl1(&dst, &bad, "vkCmdCopyBufferToImage");
    if (bad || nvhal_astc_cmd_copy_buffer_to_image(d, cb, src, dst, dstL, n, r)) return;
    d->xl.CmdCopyBufferToImage(cb, src, dst, dstL, n, r);
}
static VKAPI_ATTR void VKAPI_CALL xl_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout srcL,
                                                          VkBuffer dst, uint32_t n, const VkBufferImageCopy *r) {
    CMD("vkCmdCopyImageToBuffer")
    bool bad = false;
    xl1(&src, &bad, "vkCmdCopyImageToBuffer");
    if (bad || nvhal_astc_cmd_copy_image_to_buffer(d, cb, src, srcL, dst, n, r)) return;
    d->xl.CmdCopyImageToBuffer(cb, src, srcL, dst, n, r);
}
static VKAPI_ATTR void VKAPI_CALL xl_CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l,
                                                        const VkClearColorValue *c, uint32_t n,
                                                        const VkImageSubresourceRange *r) {
    CMD("vkCmdClearColorImage")
    bool bad = false;
    xl1(&img, &bad, "vkCmdClearColorImage");
    if (!bad) d->xl.CmdClearColorImage(cb, img, l, c, n, r);
}
static VKAPI_ATTR void VKAPI_CALL xl_CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout l,
                                                               const VkClearDepthStencilValue *c, uint32_t n,
                                                               const VkImageSubresourceRange *r) {
    CMD("vkCmdClearDepthStencilImage")
    bool bad = false;
    xl1(&img, &bad, "vkCmdClearDepthStencilImage");
    if (!bad) d->xl.CmdClearDepthStencilImage(cb, img, l, c, n, r);
}
static VKAPI_ATTR void VKAPI_CALL xl_CmdCopyMemoryToImageIndirectNV(VkCommandBuffer cb, VkDeviceAddress a, uint32_t n,
                                                                    uint32_t stride, VkImage dst, VkImageLayout l,
                                                                    const VkImageSubresourceLayers *s) {
    CMD("vkCmdCopyMemoryToImageIndirectNV")
    bool bad = false;
    xl1(&dst, &bad, "vkCmdCopyMemoryToImageIndirectNV");
    if (!bad && !nvhal_astc_refuse(dst, "vkCmdCopyMemoryToImageIndirectNV")) d->xl.CmdCopyMemoryToImageIndirectNV(cb, a, n, stride, dst, l, s);
}

/* the *2 commands: one info struct with one or two images; core and KHR names */
/* hook: the ASTC emulation records the command itself when it names an emulated image (astc.c) */
#define NO_ASTC_HOOK(d, cb, info) ((void)(d), (void)(cb), (void)(info), false)
#define CMD_INFO2(Name, Info, field1, field2, hook)                                                            \
    static void cmd_##Name(struct nvhal_device *d, VkCommandBuffer cb, const Info *info, PFN_vk##Name f) {     \
        Info t = *info;                                                                                       \
        bool bad = false, hit = xl1(&t.field1, &bad, "vk" #Name);                                              \
        hit |= xl1(&t.field2, &bad, "vk" #Name);                                                               \
        if (bad || hook(d, cb, hit ? &t : info)) return;                                                      \
        f(cb, hit ? &t : info);                                                                               \
    }                                                                                                         \
    static VKAPI_ATTR void VKAPI_CALL xl_##Name(VkCommandBuffer cb, const Info *info) {                        \
        CMD("vk" #Name)                                                                                       \
        cmd_##Name(d, cb, info, d->xl.Name);                                                                  \
    }                                                                                                         \
    static VKAPI_ATTR void VKAPI_CALL xl_##Name##KHR(VkCommandBuffer cb, const Info *info) {                   \
        CMD("vk" #Name "KHR")                                                                                 \
        cmd_##Name(d, cb, info, d->xl.Name##KHR);                                                             \
    }
CMD_INFO2(CmdBlitImage2, VkBlitImageInfo2, srcImage, dstImage, NO_ASTC_HOOK)
CMD_INFO2(CmdCopyImage2, VkCopyImageInfo2, srcImage, dstImage, nvhal_astc_cmd_copy_image2)
CMD_INFO2(CmdResolveImage2, VkResolveImageInfo2, srcImage, dstImage, NO_ASTC_HOOK)
CMD_INFO2(CmdCopyBufferToImage2, VkCopyBufferToImageInfo2, dstImage, dstImage, nvhal_astc_cmd_copy_buffer_to_image2)
CMD_INFO2(CmdCopyImageToBuffer2, VkCopyImageToBufferInfo2, srcImage, srcImage, nvhal_astc_cmd_copy_image_to_buffer2)

#ifdef VK_KHR_device_address_commands
static VKAPI_ATTR void VKAPI_CALL xl_CmdCopyMemoryToImageKHR(VkCommandBuffer cb, const VkCopyDeviceMemoryImageInfoKHR *i) {
    CMD("vkCmdCopyMemoryToImageKHR")
    VkCopyDeviceMemoryImageInfoKHR t = *i;
    bool bad = false, hit = xl1(&t.image, &bad, "vkCmdCopyMemoryToImageKHR");
    if (!bad && !nvhal_astc_refuse(i->image, "vkCmdCopyMemoryToImageKHR")) d->xl.CmdCopyMemoryToImageKHR(cb, hit ? &t : i);
}
static VKAPI_ATTR void VKAPI_CALL xl_CmdCopyImageToMemoryKHR(VkCommandBuffer cb, const VkCopyDeviceMemoryImageInfoKHR *i) {
    CMD("vkCmdCopyImageToMemoryKHR")
    VkCopyDeviceMemoryImageInfoKHR t = *i;
    bool bad = false, hit = xl1(&t.image, &bad, "vkCmdCopyImageToMemoryKHR");
    if (!bad && !nvhal_astc_refuse(i->image, "vkCmdCopyImageToMemoryKHR")) d->xl.CmdCopyImageToMemoryKHR(cb, hit ? &t : i);
}
#endif
#ifdef VK_KHR_copy_memory_indirect
static VKAPI_ATTR void VKAPI_CALL xl_CmdCopyMemoryToImageIndirectKHR(VkCommandBuffer cb,
                                                                     const VkCopyMemoryToImageIndirectInfoKHR *i) {
    CMD("vkCmdCopyMemoryToImageIndirectKHR")
    VkCopyMemoryToImageIndirectInfoKHR t = *i;
    bool bad = false, hit = xl1(&t.dstImage, &bad, "vkCmdCopyMemoryToImageIndirectKHR");
    if (!bad && !nvhal_astc_refuse(i->dstImage, "vkCmdCopyMemoryToImageIndirectKHR")) d->xl.CmdCopyMemoryToImageIndirectKHR(cb, hit ? &t : i);
}
#endif

/* ------------------------------------------------------------------ barriers */

static VKAPI_ATTR void VKAPI_CALL xl_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src,
                                                        VkPipelineStageFlags dst, VkDependencyFlags dep, uint32_t nm,
                                                        const VkMemoryBarrier *m, uint32_t nb,
                                                        const VkBufferMemoryBarrier *b, uint32_t ni,
                                                        const VkImageMemoryBarrier *im) {
    CMD("vkCmdPipelineBarrier")
    VkImageMemoryBarrier st[16];
    bool bad = false;
    const VkImageMemoryBarrier *t = ARR(im, ni, VkImageMemoryBarrier, image, st, &bad, "vkCmdPipelineBarrier");
    VkImageMemoryBarrier *owned;
    uint32_t na;
    const VkImageMemoryBarrier *ta = nvhal_astc_barriers(t, ni, &na, &owned);
    if (!bad) d->xl.CmdPipelineBarrier(cb, src, dst, dep, nm, m, nb, b, na, ta);
    free(owned);
    xl_free(t, im, st);
}

static VKAPI_ATTR void VKAPI_CALL xl_CmdWaitEvents(VkCommandBuffer cb, uint32_t ne, const VkEvent *ev,
                                                   VkPipelineStageFlags src, VkPipelineStageFlags dst, uint32_t nm,
                                                   const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b,
                                                   uint32_t ni, const VkImageMemoryBarrier *im) {
    CMD("vkCmdWaitEvents")
    VkImageMemoryBarrier st[16];
    bool bad = false;
    const VkImageMemoryBarrier *t = ARR(im, ni, VkImageMemoryBarrier, image, st, &bad, "vkCmdWaitEvents");
    VkImageMemoryBarrier *owned;
    uint32_t na;
    const VkImageMemoryBarrier *ta = nvhal_astc_barriers(t, ni, &na, &owned);
    if (!bad) d->xl.CmdWaitEvents(cb, ne, ev, src, dst, nm, m, nb, b, na, ta);
    free(owned);
    xl_free(t, im, st);
}

/* one VkDependencyInfo: *copy = info with its image barriers translated (wrappers) and the ASTC block images'
 * barriers appended; *arr (st/heap, xl_free) and *owned (free) are what to release */
static void xl_dep(const VkDependencyInfo *info, VkDependencyInfo *copy, VkImageMemoryBarrier2 *st, size_t st_bytes,
                   const VkImageMemoryBarrier2 **arr, VkImageMemoryBarrier2 **owned, bool *bad, const char *what) {
    *arr = xl_arr(info->pImageMemoryBarriers, info->imageMemoryBarrierCount, sizeof(VkImageMemoryBarrier2),
                  offsetof(VkImageMemoryBarrier2, image), st, st_bytes, bad, what);
    *copy = *info;
    copy->pImageMemoryBarriers = nvhal_astc_barriers2(*arr, info->imageMemoryBarrierCount,
                                                      &copy->imageMemoryBarrierCount, owned);
}

static void cmd_dep(VkCommandBuffer cb, const VkDependencyInfo *info, PFN_vkCmdPipelineBarrier2 f, const char *what) {
    VkImageMemoryBarrier2 st[16], *owned;
    const VkImageMemoryBarrier2 *arr;
    VkDependencyInfo t;
    bool bad = false;
    xl_dep(info, &t, st, sizeof st, &arr, &owned, &bad, what);
    if (!bad) f(cb, &t);
    free(owned);
    xl_free(arr, info->pImageMemoryBarriers, st);
}
static VKAPI_ATTR void VKAPI_CALL xl_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *i) {
    CMD("vkCmdPipelineBarrier2")
    cmd_dep(cb, i, d->xl.CmdPipelineBarrier2, "vkCmdPipelineBarrier2");
}
static VKAPI_ATTR void VKAPI_CALL xl_CmdPipelineBarrier2KHR(VkCommandBuffer cb, const VkDependencyInfo *i) {
    CMD("vkCmdPipelineBarrier2KHR")
    cmd_dep(cb, i, d->xl.CmdPipelineBarrier2KHR, "vkCmdPipelineBarrier2KHR");
}

static void set_event2(VkCommandBuffer cb, VkEvent e, const VkDependencyInfo *info, PFN_vkCmdSetEvent2 f) {
    VkImageMemoryBarrier2 st[16], *owned;
    const VkImageMemoryBarrier2 *arr;
    VkDependencyInfo t;
    bool bad = false;
    xl_dep(info, &t, st, sizeof st, &arr, &owned, &bad, "vkCmdSetEvent2");
    if (!bad) f(cb, e, &t);
    free(owned);
    xl_free(arr, info->pImageMemoryBarriers, st);
}
static VKAPI_ATTR void VKAPI_CALL xl_CmdSetEvent2(VkCommandBuffer cb, VkEvent e, const VkDependencyInfo *i) {
    CMD("vkCmdSetEvent2")
    set_event2(cb, e, i, d->xl.CmdSetEvent2);
}
static VKAPI_ATTR void VKAPI_CALL xl_CmdSetEvent2KHR(VkCommandBuffer cb, VkEvent e, const VkDependencyInfo *i) {
    CMD("vkCmdSetEvent2KHR")
    set_event2(cb, e, i, d->xl.CmdSetEvent2KHR);
}

static void wait_events2(VkCommandBuffer cb, uint32_t n, const VkEvent *ev, const VkDependencyInfo *deps,
                         PFN_vkCmdWaitEvents2 f) {
    if (nvhal_no_wrappers() && nvhal_no_astc()) {
        f(cb, n, ev, deps);
        return;
    }
    if (!nvhal_no_astc()) { /* each VkDependencyInfo translated + ASTC block images' barriers appended */
        VkDependencyInfo st_copy[4], *copy = n <= 4 ? st_copy : malloc(n * sizeof *copy);
        const VkImageMemoryBarrier2 **arrs = calloc(n ? n : 1, sizeof *arrs);
        VkImageMemoryBarrier2 **owned = calloc(n ? n : 1, sizeof *owned);
        bool bad = !copy || !arrs || !owned;
        if (bad) NVHAL_ERR("vkCmdWaitEvents2: out of host memory; dropped");
        for (uint32_t i = 0; i < n && !bad; i++)
            xl_dep(&deps[i], &copy[i], NULL, 0, &arrs[i], &owned[i], &bad, "vkCmdWaitEvents2");
        if (!bad) f(cb, n, ev, copy);
        for (uint32_t i = 0; arrs && owned && i < n; i++) {
            free(owned[i]);
            xl_free(arrs[i], deps[i].pImageMemoryBarriers, NULL);
        }
        free(arrs);
        free(owned);
        if (copy != st_copy) free(copy);
        return;
    }
    VkDependencyInfo *copy = NULL;
    const VkImageMemoryBarrier2 **arrs = NULL;
    bool bad = false;
    for (uint32_t i = 0; i < n && !bad; i++) {
        const VkImageMemoryBarrier2 *a =
            xl_arr(deps[i].pImageMemoryBarriers, deps[i].imageMemoryBarrierCount, sizeof(VkImageMemoryBarrier2),
                   offsetof(VkImageMemoryBarrier2, image), NULL, 0, &bad, "vkCmdWaitEvents2");
        if (a == deps[i].pImageMemoryBarriers) continue;
        if (!copy) {
            copy = malloc(n * sizeof *copy);
            arrs = calloc(n, sizeof *arrs);
            if (!copy || !arrs) {
                NVHAL_ERR("vkCmdWaitEvents2: out of host memory; dropped");
                bad = true;
                xl_free(a, deps[i].pImageMemoryBarriers, NULL);
                break;
            }
            memcpy(copy, deps, n * sizeof *copy);
        }
        arrs[i] = a;
        copy[i].pImageMemoryBarriers = a;
    }
    if (!bad) f(cb, n, ev, copy ? copy : deps);
    if (arrs)
        for (uint32_t i = 0; i < n; i++) free((void *)arrs[i]);
    free(arrs);
    free(copy);
}
static VKAPI_ATTR void VKAPI_CALL xl_CmdWaitEvents2(VkCommandBuffer cb, uint32_t n, const VkEvent *e,
                                                    const VkDependencyInfo *d2) {
    CMD("vkCmdWaitEvents2")
    wait_events2(cb, n, e, d2, d->xl.CmdWaitEvents2);
}
static VKAPI_ATTR void VKAPI_CALL xl_CmdWaitEvents2KHR(VkCommandBuffer cb, uint32_t n, const VkEvent *e,
                                                       const VkDependencyInfo *d2) {
    CMD("vkCmdWaitEvents2KHR")
    wait_events2(cb, n, e, d2, d->xl.CmdWaitEvents2KHR);
}

/* ------------------------------------------------------------------ sparse binding */

static VkResult bind_sparse_wrappers(struct nvhal_device *d, VkQueue queue, uint32_t n, const VkBindSparseInfo *bi,
                                     VkFence fence) {
    if (nvhal_no_wrappers()) return d->xl.QueueBindSparse(queue, n, bi, fence);
    /* AHB images are never sparse (vkCreateImage refuses sparse flags with the AHB handle type), so a
     * wrapper here is invalid usage; it is still translated rather than forwarded */
    VkBindSparseInfo *copy = NULL;
    void **owned = NULL;
    uint32_t n_owned = 0;
    bool bad = false;
    VkResult r = VK_SUCCESS;
    for (uint32_t i = 0; i < n && !bad; i++) {
        const VkSparseImageOpaqueMemoryBindInfo *o =
            xl_arr(bi[i].pImageOpaqueBinds, bi[i].imageOpaqueBindCount, sizeof *o,
                   offsetof(VkSparseImageOpaqueMemoryBindInfo, image), NULL, 0, &bad, "vkQueueBindSparse");
        const VkSparseImageMemoryBindInfo *m =
            xl_arr(bi[i].pImageBinds, bi[i].imageBindCount, sizeof *m, offsetof(VkSparseImageMemoryBindInfo, image), NULL,
                   0, &bad, "vkQueueBindSparse");
        if (o == bi[i].pImageOpaqueBinds && m == bi[i].pImageBinds) continue;
        if (!copy) {
            copy = malloc(n * sizeof *copy);
            owned = calloc(2 * (size_t)n, sizeof *owned);
            if (!copy || !owned) { bad = true; r = VK_ERROR_OUT_OF_HOST_MEMORY; }
            else memcpy(copy, bi, n * sizeof *copy);
        }
        if (o != bi[i].pImageOpaqueBinds) {
            if (owned) owned[n_owned++] = (void *)o;
            else free((void *)o);
            if (copy) copy[i].pImageOpaqueBinds = o;
        }
        if (m != bi[i].pImageBinds) {
            if (owned) owned[n_owned++] = (void *)m;
            else free((void *)m);
            if (copy) copy[i].pImageBinds = m;
        }
    }
    if (!bad) r = d->xl.QueueBindSparse(queue, n, copy ? copy : bi, fence);
    else if (r == VK_SUCCESS) r = VK_ERROR_UNKNOWN;
    for (uint32_t i = 0; i < n_owned; i++) free(owned[i]);
    free(owned);
    free(copy);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL xl_QueueBindSparse(VkQueue queue, uint32_t n, const VkBindSparseInfo *bi,
                                                         VkFence fence) {
    struct nvhal_device *d = nvhal_queue_device(queue);
    if (!d) {
        NVHAL_ERR("vkQueueBindSparse(%p): queue was not obtained through this HAL", (void *)queue);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    /* emulated ASTC images: opaque binds split over the RGBA8 image and the block images (astc.c) */
    VkBindSparseInfo *ab;
    void **own;
    uint32_t nown;
    VkResult r = nvhal_astc_bind_sparse(n, bi, &ab, &own, &nown);
    if (r != VK_SUCCESS) return r;
    r = bind_sparse_wrappers(d, queue, n, ab, fence);
    for (uint32_t i = 0; i < nown; i++) free(own[i]);
    free(own);
    return r;
}

/* ------------------------------------------------------------------ descriptor heap (VK_EXT_descriptor_heap) */

#undef DEV_FAIL
#define DEV_FAIL VK_ERROR_INITIALIZATION_FAILED
#ifdef VK_EXT_descriptor_heap
static bool image_descriptor_type(VkDescriptorType t) {
    /* VkResourceDescriptorDataEXT.pImage selection (vk.xml): SAMPLED_IMAGE, STORAGE_IMAGE,
     * INPUT_ATTACHMENT, BLOCK_MATCH_IMAGE_QCOM, SAMPLE_WEIGHT_IMAGE_QCOM */
    return t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT || t == VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM ||
           t == VK_DESCRIPTOR_TYPE_SAMPLE_WEIGHT_IMAGE_QCOM;
}

static VKAPI_ATTR VkResult VKAPI_CALL xl_WriteResourceDescriptorsEXT(VkDevice device, uint32_t n,
                                                                     const VkResourceDescriptorInfoEXT *res,
                                                                     const VkHostAddressRangeEXT *dst) {
    DEV("vkWriteResourceDescriptorsEXT")
    for (uint32_t i = 0; i < n && !nvhal_no_astc(); i++) /* the view would carry the ASTC format to the ICD */
        if (image_descriptor_type(res[i].type) && res[i].data.pImage && res[i].data.pImage->pView &&
            nvhal_astc_refuse(res[i].data.pImage->pView->image, "vkWriteResourceDescriptorsEXT"))
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (nvhal_no_wrappers()) return d->xl.WriteResourceDescriptorsEXT(device, n, res, dst);
    struct img_copy { VkImageDescriptorInfoEXT di; VkImageViewCreateInfo vi; } *ic = NULL;
    VkResourceDescriptorInfoEXT *copy = NULL;
    bool bad = false;
    for (uint32_t i = 0; i < n && !bad; i++) {
        if (!image_descriptor_type(res[i].type) || !res[i].data.pImage || !res[i].data.pImage->pView) continue;
        VkImage img = res[i].data.pImage->pView->image;
        if (!xl1(&img, &bad, "vkWriteResourceDescriptorsEXT")) continue;
        if (!copy) {
            copy = malloc(n * sizeof *copy);
            ic = malloc(n * sizeof *ic);
            if (!copy || !ic) { bad = true; break; }
            memcpy(copy, res, n * sizeof *copy);
        }
        ic[i].vi = *res[i].data.pImage->pView;
        ic[i].vi.image = img;
        if (ic[i].vi.format == VK_FORMAT_UNDEFINED) ic[i].vi.format = nvhal_image_real_format(res[i].data.pImage->pView->image);
        ic[i].di = *res[i].data.pImage;
        ic[i].di.pView = &ic[i].vi;
        copy[i].data.pImage = &ic[i].di;
    }
    VkResult r = bad ? VK_ERROR_UNKNOWN : d->xl.WriteResourceDescriptorsEXT(device, n, copy ? copy : res, dst);
    free(copy);
    free(ic);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL xl_GetImageOpaqueCaptureDataEXT(VkDevice device, uint32_t n, const VkImage *imgs,
                                                                      VkHostAddressRangeEXT *datas) {
    DEV("vkGetImageOpaqueCaptureDataEXT")
    VkImage st[16];
    bool bad = false;
    const VkImage *t = xl_arr(imgs, n, sizeof(VkImage), 0, st, sizeof st, &bad, "vkGetImageOpaqueCaptureDataEXT");
    VkResult r = bad ? VK_ERROR_UNKNOWN : d->xl.GetImageOpaqueCaptureDataEXT(device, n, t, datas);
    xl_free(t, imgs, st);
    return r;
}
#endif

/* ------------------------------------------------------------------ table for hal.c */

#define XF(x) ((PFN_vkVoidFunction)(x))
/* kind: NVHAL_IC_WRAP = handed out by vkGetDeviceProcAddr only on devices where a wrapper image can
 * exist: the AHB extension (AHB images) or VK_ANDROID_native_buffer (swapchain-bound images) enabled;
 * elsewhere the ICD's own entry point is returned;
 * NVHAL_IC_DEVICE = always (command buffer tracking must cover every device, so that the translators
 * also work when reached through vkGetInstanceProcAddr; private data on the loader's VkSwapchainKHR /
 * VkSurfaceKHR handles must never reach the ICD, whichever extensions are enabled). */
const struct nvhal_xl_entry nvhal_xl_entries[] = {
    {"vkGetImageMemoryRequirements", XF(xl_GetImageMemoryRequirements), NVHAL_IC_WRAP},
    {"vkGetImageMemoryRequirements2", XF(xl_GetImageMemoryRequirements2), NVHAL_IC_WRAP},
    {"vkGetImageMemoryRequirements2KHR", XF(xl_GetImageMemoryRequirements2KHR), NVHAL_IC_WRAP},
    {"vkGetImageSparseMemoryRequirements", XF(xl_GetImageSparseMemoryRequirements), NVHAL_IC_WRAP},
    {"vkGetImageSparseMemoryRequirements2", XF(xl_GetImageSparseMemoryRequirements2), NVHAL_IC_WRAP},
    {"vkGetImageSparseMemoryRequirements2KHR", XF(xl_GetImageSparseMemoryRequirements2KHR), NVHAL_IC_WRAP},
    {"vkGetImageSubresourceLayout", XF(xl_GetImageSubresourceLayout), NVHAL_IC_WRAP},
    {"vkGetImageSubresourceLayout2", XF(xl_GetImageSubresourceLayout2), NVHAL_IC_WRAP},
    {"vkGetImageSubresourceLayout2KHR", XF(xl_GetImageSubresourceLayout2KHR), NVHAL_IC_WRAP},
    {"vkGetImageSubresourceLayout2EXT", XF(xl_GetImageSubresourceLayout2EXT), NVHAL_IC_WRAP},
    {"vkGetImageDrmFormatModifierPropertiesEXT", XF(xl_GetImageDrmFormatModifierPropertiesEXT), NVHAL_IC_WRAP},
    {"vkCreateImageView", XF(xl_CreateImageView), NVHAL_IC_WRAP},
    {"vkGetImageOpaqueCaptureDescriptorDataEXT", XF(xl_GetImageOpaqueCaptureDescriptorDataEXT), NVHAL_IC_WRAP},
    {"vkCopyImageToImage", XF(xl_CopyImageToImage), NVHAL_IC_WRAP},
    {"vkCopyImageToImageEXT", XF(xl_CopyImageToImageEXT), NVHAL_IC_WRAP},
    {"vkCopyImageToMemory", XF(xl_CopyImageToMemory), NVHAL_IC_WRAP},
    {"vkCopyImageToMemoryEXT", XF(xl_CopyImageToMemoryEXT), NVHAL_IC_WRAP},
    {"vkCopyMemoryToImage", XF(xl_CopyMemoryToImage), NVHAL_IC_WRAP},
    {"vkCopyMemoryToImageEXT", XF(xl_CopyMemoryToImageEXT), NVHAL_IC_WRAP},
    {"vkTransitionImageLayout", XF(xl_TransitionImageLayout), NVHAL_IC_WRAP},
    {"vkTransitionImageLayoutEXT", XF(xl_TransitionImageLayoutEXT), NVHAL_IC_WRAP},
    {"vkSetDebugUtilsObjectNameEXT", XF(xl_SetDebugUtilsObjectNameEXT), NVHAL_IC_WRAP},
    {"vkSetDebugUtilsObjectTagEXT", XF(xl_SetDebugUtilsObjectTagEXT), NVHAL_IC_WRAP},
    {"vkDebugMarkerSetObjectNameEXT", XF(xl_DebugMarkerSetObjectNameEXT), NVHAL_IC_WRAP},
    {"vkDebugMarkerSetObjectTagEXT", XF(xl_DebugMarkerSetObjectTagEXT), NVHAL_IC_WRAP},
    {"vkSetPrivateData", XF(xl_SetPrivateData), NVHAL_IC_DEVICE},
    {"vkSetPrivateDataEXT", XF(xl_SetPrivateDataEXT), NVHAL_IC_DEVICE},
    {"vkGetPrivateData", XF(xl_GetPrivateData), NVHAL_IC_DEVICE},
    {"vkGetPrivateDataEXT", XF(xl_GetPrivateDataEXT), NVHAL_IC_DEVICE},
    {"vkDestroyPrivateDataSlot", XF(xl_DestroyPrivateDataSlot), NVHAL_IC_DEVICE},
    {"vkDestroyPrivateDataSlotEXT", XF(xl_DestroyPrivateDataSlotEXT), NVHAL_IC_DEVICE},
    {"vkGetDeviceImageMemoryRequirements", XF(xl_GetDeviceImageMemoryRequirements), NVHAL_IC_WRAP},
    {"vkGetDeviceImageMemoryRequirementsKHR", XF(xl_GetDeviceImageMemoryRequirementsKHR), NVHAL_IC_WRAP},
    {"vkGetDeviceImageSparseMemoryRequirements", XF(xl_GetDeviceImageSparseMemoryRequirements), NVHAL_IC_WRAP},
    {"vkGetDeviceImageSparseMemoryRequirementsKHR", XF(xl_GetDeviceImageSparseMemoryRequirementsKHR), NVHAL_IC_WRAP},
    {"vkGetDeviceImageSubresourceLayout", XF(xl_GetDeviceImageSubresourceLayout), NVHAL_IC_WRAP},
    {"vkGetDeviceImageSubresourceLayoutKHR", XF(xl_GetDeviceImageSubresourceLayoutKHR), NVHAL_IC_WRAP},
    {"vkGetDeviceBufferMemoryRequirements", XF(xl_GetDeviceBufferMemoryRequirements), NVHAL_IC_WRAP},
    {"vkGetDeviceBufferMemoryRequirementsKHR", XF(xl_GetDeviceBufferMemoryRequirementsKHR), NVHAL_IC_WRAP},
    {"vkCmdBlitImage", XF(xl_CmdBlitImage), NVHAL_IC_WRAP},
    {"vkCmdBlitImage2", XF(xl_CmdBlitImage2), NVHAL_IC_WRAP},
    {"vkCmdBlitImage2KHR", XF(xl_CmdBlitImage2KHR), NVHAL_IC_WRAP},
    {"vkCmdCopyImage", XF(xl_CmdCopyImage), NVHAL_IC_WRAP},
    {"vkCmdCopyImage2", XF(xl_CmdCopyImage2), NVHAL_IC_WRAP},
    {"vkCmdCopyImage2KHR", XF(xl_CmdCopyImage2KHR), NVHAL_IC_WRAP},
    {"vkCmdResolveImage", XF(xl_CmdResolveImage), NVHAL_IC_WRAP},
    {"vkCmdResolveImage2", XF(xl_CmdResolveImage2), NVHAL_IC_WRAP},
    {"vkCmdResolveImage2KHR", XF(xl_CmdResolveImage2KHR), NVHAL_IC_WRAP},
    {"vkCmdCopyBufferToImage", XF(xl_CmdCopyBufferToImage), NVHAL_IC_WRAP},
    {"vkCmdCopyBufferToImage2", XF(xl_CmdCopyBufferToImage2), NVHAL_IC_WRAP},
    {"vkCmdCopyBufferToImage2KHR", XF(xl_CmdCopyBufferToImage2KHR), NVHAL_IC_WRAP},
    {"vkCmdCopyImageToBuffer", XF(xl_CmdCopyImageToBuffer), NVHAL_IC_WRAP},
    {"vkCmdCopyImageToBuffer2", XF(xl_CmdCopyImageToBuffer2), NVHAL_IC_WRAP},
    {"vkCmdCopyImageToBuffer2KHR", XF(xl_CmdCopyImageToBuffer2KHR), NVHAL_IC_WRAP},
    {"vkCmdClearColorImage", XF(xl_CmdClearColorImage), NVHAL_IC_WRAP},
    {"vkCmdClearDepthStencilImage", XF(xl_CmdClearDepthStencilImage), NVHAL_IC_WRAP},
    {"vkCmdCopyMemoryToImageIndirectNV", XF(xl_CmdCopyMemoryToImageIndirectNV), NVHAL_IC_WRAP},
    {"vkCmdPipelineBarrier", XF(xl_CmdPipelineBarrier), NVHAL_IC_WRAP},
    {"vkCmdPipelineBarrier2", XF(xl_CmdPipelineBarrier2), NVHAL_IC_WRAP},
    {"vkCmdPipelineBarrier2KHR", XF(xl_CmdPipelineBarrier2KHR), NVHAL_IC_WRAP},
    {"vkCmdWaitEvents", XF(xl_CmdWaitEvents), NVHAL_IC_WRAP},
    {"vkCmdWaitEvents2", XF(xl_CmdWaitEvents2), NVHAL_IC_WRAP},
    {"vkCmdWaitEvents2KHR", XF(xl_CmdWaitEvents2KHR), NVHAL_IC_WRAP},
    {"vkCmdSetEvent2", XF(xl_CmdSetEvent2), NVHAL_IC_WRAP},
    {"vkCmdSetEvent2KHR", XF(xl_CmdSetEvent2KHR), NVHAL_IC_WRAP},
    {"vkQueueBindSparse", XF(xl_QueueBindSparse), NVHAL_IC_WRAP},
    {"vkFreeCommandBuffers", XF(xl_FreeCommandBuffers), NVHAL_IC_DEVICE},
    {"vkDestroyCommandPool", XF(xl_DestroyCommandPool), NVHAL_IC_DEVICE},
#ifdef VK_KHR_device_address_commands
    {"vkCmdCopyMemoryToImageKHR", XF(xl_CmdCopyMemoryToImageKHR), NVHAL_IC_WRAP},
    {"vkCmdCopyImageToMemoryKHR", XF(xl_CmdCopyImageToMemoryKHR), NVHAL_IC_WRAP},
#endif
#ifdef VK_KHR_copy_memory_indirect
    {"vkCmdCopyMemoryToImageIndirectKHR", XF(xl_CmdCopyMemoryToImageIndirectKHR), NVHAL_IC_WRAP},
#endif
#ifdef VK_EXT_descriptor_heap
    {"vkWriteResourceDescriptorsEXT", XF(xl_WriteResourceDescriptorsEXT), NVHAL_IC_WRAP},
    {"vkGetImageOpaqueCaptureDataEXT", XF(xl_GetImageOpaqueCaptureDataEXT), NVHAL_IC_WRAP},
#endif
    {NULL, NULL, 0},
};
