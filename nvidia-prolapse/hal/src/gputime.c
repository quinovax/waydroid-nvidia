/* Opt-in GPU timing (a diagnostic, off by default).
 *
 * Enabled per device at vkCreateDevice by the system property debug.nvhal.gputime (Android) or the
 * environment variable NVHAL_GPUTIME (host):
 *   1  timestamps around every render pass instance, dispatch, transfer command and vkCmdExecuteCommands
 *      recorded outside a render pass, in primary command buffers;
 *   2  additionally one timestamp after every draw / vkCmdClearAttachments inside an inline render pass
 *      (per-pipeline cost; each bottom-of-pipe timestamp may serialise the GPU a little, compare with 1).
 * Off (absent, empty or "0"): hal.c's vkGetDeviceProcAddr returns exactly what it returns without this file,
 * so nothing here is on any call path.
 *
 * How: vkGetDeviceProcAddr wraps the entry point the HAL would have returned (its own intercept or the ICD's)
 * for the commands below; the wrapper records the command through it and writes VK_PIPELINE_STAGE_BOTTOM_OF_PIPE
 * timestamps into a query pool owned by the command buffer, reset at the start of each recording. A region's
 * time is the difference between its timestamp and the previous one in the same command buffer (so barriers
 * and waits before a region count towards it). Results are read when the command buffer is begun again or
 * freed (it cannot be pending then), only for recordings that were submitted, and aggregated per pipeline /
 * render pass. Every debug.nvhal.gputime.window seconds (default 3) a report goes to logcat (tag vulkan.nvidia)
 * and to <dir>/gputime.txt, where <dir> is /data/data/<process name>/nvhal-gputime on Android
 * ($NVHAL_GPUTIME_DIR or /tmp/nvhal-gputime on the host). Every shader module's SPIR-V is written to
 * <dir>/<fnv1a64>.spv; the report names pipelines by the hashes of their stages. Frames are counted at
 * vkQueueSignalReleaseImageANDROID (one per presented image). */
#include "nvhal.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#define GT_QP_SIZE 4096u
#define GT_MAXPOOLS 8u
#define GT_TAB 8192u   /* aggregation slots per window (power of two) */
#define GT_TOP 45u

/* entry points the wrappers chain to: whatever the HAL's vkGetDeviceProcAddr resolved (one field per name) */
#define GT_FNS(X) \
    X(PFN_vkAllocateCommandBuffers, AllocateCommandBuffers, "vkAllocateCommandBuffers") \
    X(PFN_vkBeginCommandBuffer, BeginCommandBuffer, "vkBeginCommandBuffer") \
    X(PFN_vkEndCommandBuffer, EndCommandBuffer, "vkEndCommandBuffer") \
    X(PFN_vkQueueSubmit, QueueSubmit, "vkQueueSubmit") \
    X(PFN_vkQueueSubmit2, QueueSubmit2, "vkQueueSubmit2") \
    X(PFN_vkQueueSubmit2, QueueSubmit2KHR, "vkQueueSubmit2KHR") \
    X(PFN_vkCmdBindPipeline, CmdBindPipeline, "vkCmdBindPipeline") \
    X(PFN_vkCmdBeginRenderPass, CmdBeginRenderPass, "vkCmdBeginRenderPass") \
    X(PFN_vkCmdBeginRenderPass2, CmdBeginRenderPass2, "vkCmdBeginRenderPass2") \
    X(PFN_vkCmdBeginRenderPass2, CmdBeginRenderPass2KHR, "vkCmdBeginRenderPass2KHR") \
    X(PFN_vkCmdEndRenderPass, CmdEndRenderPass, "vkCmdEndRenderPass") \
    X(PFN_vkCmdEndRenderPass2, CmdEndRenderPass2, "vkCmdEndRenderPass2") \
    X(PFN_vkCmdEndRenderPass2, CmdEndRenderPass2KHR, "vkCmdEndRenderPass2KHR") \
    X(PFN_vkCmdBeginRendering, CmdBeginRendering, "vkCmdBeginRendering") \
    X(PFN_vkCmdBeginRendering, CmdBeginRenderingKHR, "vkCmdBeginRenderingKHR") \
    X(PFN_vkCmdEndRendering, CmdEndRendering, "vkCmdEndRendering") \
    X(PFN_vkCmdEndRendering, CmdEndRenderingKHR, "vkCmdEndRenderingKHR") \
    X(PFN_vkCmdDraw, CmdDraw, "vkCmdDraw") \
    X(PFN_vkCmdDrawIndexed, CmdDrawIndexed, "vkCmdDrawIndexed") \
    X(PFN_vkCmdDrawIndirect, CmdDrawIndirect, "vkCmdDrawIndirect") \
    X(PFN_vkCmdDrawIndexedIndirect, CmdDrawIndexedIndirect, "vkCmdDrawIndexedIndirect") \
    X(PFN_vkCmdDrawIndirectCount, CmdDrawIndirectCount, "vkCmdDrawIndirectCount") \
    X(PFN_vkCmdDrawIndirectCount, CmdDrawIndirectCountKHR, "vkCmdDrawIndirectCountKHR") \
    X(PFN_vkCmdDrawIndexedIndirectCount, CmdDrawIndexedIndirectCount, "vkCmdDrawIndexedIndirectCount") \
    X(PFN_vkCmdDrawIndexedIndirectCount, CmdDrawIndexedIndirectCountKHR, "vkCmdDrawIndexedIndirectCountKHR") \
    X(PFN_vkCmdDrawMultiEXT, CmdDrawMultiEXT, "vkCmdDrawMultiEXT") \
    X(PFN_vkCmdDrawMultiIndexedEXT, CmdDrawMultiIndexedEXT, "vkCmdDrawMultiIndexedEXT") \
    X(PFN_vkCmdClearAttachments, CmdClearAttachments, "vkCmdClearAttachments") \
    X(PFN_vkCmdDispatch, CmdDispatch, "vkCmdDispatch") \
    X(PFN_vkCmdDispatchBase, CmdDispatchBase, "vkCmdDispatchBase") \
    X(PFN_vkCmdDispatchBase, CmdDispatchBaseKHR, "vkCmdDispatchBaseKHR") \
    X(PFN_vkCmdDispatchIndirect, CmdDispatchIndirect, "vkCmdDispatchIndirect") \
    X(PFN_vkCmdCopyBuffer, CmdCopyBuffer, "vkCmdCopyBuffer") \
    X(PFN_vkCmdCopyImage, CmdCopyImage, "vkCmdCopyImage") \
    X(PFN_vkCmdCopyBufferToImage, CmdCopyBufferToImage, "vkCmdCopyBufferToImage") \
    X(PFN_vkCmdCopyImageToBuffer, CmdCopyImageToBuffer, "vkCmdCopyImageToBuffer") \
    X(PFN_vkCmdBlitImage, CmdBlitImage, "vkCmdBlitImage") \
    X(PFN_vkCmdResolveImage, CmdResolveImage, "vkCmdResolveImage") \
    X(PFN_vkCmdClearColorImage, CmdClearColorImage, "vkCmdClearColorImage") \
    X(PFN_vkCmdClearDepthStencilImage, CmdClearDepthStencilImage, "vkCmdClearDepthStencilImage") \
    X(PFN_vkCmdFillBuffer, CmdFillBuffer, "vkCmdFillBuffer") \
    X(PFN_vkCmdUpdateBuffer, CmdUpdateBuffer, "vkCmdUpdateBuffer") \
    X(PFN_vkCmdCopyBuffer2, CmdCopyBuffer2, "vkCmdCopyBuffer2") \
    X(PFN_vkCmdCopyBuffer2, CmdCopyBuffer2KHR, "vkCmdCopyBuffer2KHR") \
    X(PFN_vkCmdCopyImage2, CmdCopyImage2, "vkCmdCopyImage2") \
    X(PFN_vkCmdCopyImage2, CmdCopyImage2KHR, "vkCmdCopyImage2KHR") \
    X(PFN_vkCmdCopyBufferToImage2, CmdCopyBufferToImage2, "vkCmdCopyBufferToImage2") \
    X(PFN_vkCmdCopyBufferToImage2, CmdCopyBufferToImage2KHR, "vkCmdCopyBufferToImage2KHR") \
    X(PFN_vkCmdCopyImageToBuffer2, CmdCopyImageToBuffer2, "vkCmdCopyImageToBuffer2") \
    X(PFN_vkCmdCopyImageToBuffer2, CmdCopyImageToBuffer2KHR, "vkCmdCopyImageToBuffer2KHR") \
    X(PFN_vkCmdBlitImage2, CmdBlitImage2, "vkCmdBlitImage2") \
    X(PFN_vkCmdBlitImage2, CmdBlitImage2KHR, "vkCmdBlitImage2KHR") \
    X(PFN_vkCmdResolveImage2, CmdResolveImage2, "vkCmdResolveImage2") \
    X(PFN_vkCmdResolveImage2, CmdResolveImage2KHR, "vkCmdResolveImage2KHR") \
    X(PFN_vkCmdExecuteCommands, CmdExecuteCommands, "vkCmdExecuteCommands") \
    X(PFN_vkCreateShaderModule, CreateShaderModule, "vkCreateShaderModule") \
    X(PFN_vkDestroyShaderModule, DestroyShaderModule, "vkDestroyShaderModule") \
    X(PFN_vkCreateGraphicsPipelines, CreateGraphicsPipelines, "vkCreateGraphicsPipelines") \
    X(PFN_vkCreateComputePipelines, CreateComputePipelines, "vkCreateComputePipelines") \
    X(PFN_vkDestroyPipeline, DestroyPipeline, "vkDestroyPipeline") \
    X(PFN_vkAllocateMemory, AllocateMemory, "vkAllocateMemory") \
    X(PFN_vkFreeMemory, FreeMemory, "vkFreeMemory") \
    X(PFN_vkBindBufferMemory, BindBufferMemory, "vkBindBufferMemory") \
    X(PFN_vkBindBufferMemory2, BindBufferMemory2, "vkBindBufferMemory2") \
    X(PFN_vkBindBufferMemory2, BindBufferMemory2KHR, "vkBindBufferMemory2KHR") \
    X(PFN_vkUpdateDescriptorSets, UpdateDescriptorSets, "vkUpdateDescriptorSets") \
    X(PFN_vkUpdateDescriptorSetWithTemplate, UpdateDescriptorSetWithTemplate, "vkUpdateDescriptorSetWithTemplate") \
    X(PFN_vkUpdateDescriptorSetWithTemplate, UpdateDescriptorSetWithTemplateKHR, "vkUpdateDescriptorSetWithTemplateKHR") \
    X(PFN_vkCmdBindDescriptorSets, CmdBindDescriptorSets, "vkCmdBindDescriptorSets")

struct gt_next {
#define GT_FIELD(type, field, name) type field;
    GT_FNS(GT_FIELD)
#undef GT_FIELD
};

enum gt_kind { GK_RP, GK_DRAW, GK_CLEAR_ATT, GK_DISPATCH, GK_XFER, GK_EXEC, GK_N };
static const char *const gk_name[GK_N] = {"RPASS", "draw", "clratt", "DISPATCH", "XFER", "EXEC"};
enum gt_xop { GX_COPYBUF, GX_COPYIMG, GX_BUF2IMG, GX_IMG2BUF, GX_BLIT, GX_RESOLVE, GX_CLRCOLOR, GX_CLRDS, GX_FILL,
              GX_UPDATE, GX_N };
static const char *const gx_name[GX_N] = {"copyBuffer", "copyImage", "copyBufferToImage", "copyImageToBuffer",
                                          "blitImage", "resolveImage", "clearColorImage", "clearDepthStencil",
                                          "fillBuffer", "updateBuffer"};

#define GT_NOQ UINT32_MAX
struct gt_rec {
    uint32_t qs, qe;  /* timestamp indices: previous and own */
    uint8_t kind, op; /* op: transfer kind / attachment count */
    uint32_t w, h, z; /* render area / dispatch groups */
    uint32_t draws;   /* render pass: draws recorded in it (inline or via secondaries) */
    VkPipeline pipe;  /* draw/dispatch: bound pipeline; render pass: the first pipeline drawn with */
    uint64_t rp;      /* render pass handle (0: dynamic rendering) */
    uint64_t rpkey;   /* draw/clear: aggregation key of the render pass instance it was recorded in */
    uint8_t ssbo_n, ssbo_sys; /* dispatch: storage buffers in the bound compute sets, and those not in DEVICE_LOCAL memory */
    uint8_t ssbo_type;        /* dispatch: memory type index of the first non-DEVICE_LOCAL storage buffer */
};

struct nvhal_gt_dev;
struct nvhal_gt_cb {
    struct nvhal_gt_dev *g;
    bool primary, active, in_rp, rp_secondary, submitted;
    VkQueryPool qp[GT_MAXPOOLS];
    uint32_t npools, nq, last_q, want_q;
    bool overflow;
    struct gt_rec *rec;
    uint32_t nrec, caprec, rp_rec;
    VkPipeline gfx, cs;
    VkDescriptorSet cset[8]; /* bound compute descriptor sets */
    VkPipeline first_gfx; /* secondaries: first graphics pipeline drawn with, and draw count */
    uint32_t draws;
};

struct gt_stat {
    uint64_t key;
    uint8_t kind, op;
    uint32_t w, h, z;
    VkPipeline pipe;
    uint64_t rp;
    uint64_t ns, n, max_ns, draws;
    uint8_t ssbo_n, ssbo_sys, ssbo_type;
};

struct nvhal_gt_dev {
    struct nvhal_device *d;
    int level;
    double period; /* ns per tick */
    struct gt_next next;
    PFN_vkCreateQueryPool CreateQueryPool;
    PFN_vkDestroyQueryPool DestroyQueryPool;
    PFN_vkGetQueryPoolResults GetQueryPoolResults;
    PFN_vkCmdResetQueryPool CmdResetQueryPool;
    PFN_vkCmdWriteTimestamp CmdWriteTimestamp;
    pthread_mutex_t mu; /* everything below */
    uint64_t window_ns, win_t0, frames, submits, cbs, dropped, overflows;
    struct gt_stat *tab;
    uint32_t ntab;
    struct { uint64_t a, b; } *iv; /* busy intervals of the window (GPU ticks) */
    uint32_t niv, capiv;
    char dir[256];
};

/* ------------------------------------------------------------------ shader / pipeline identity */

struct gt_pipe { uint64_t h[4]; VkPipelineBindPoint bp; }; /* vs, fs, cs, other */
static struct nvhal_hash gt_modules = NVHAL_HASH_INIT;   /* VkShaderModule -> (uint64_t) hash */
static struct nvhal_hash gt_pipes = NVHAL_HASH_INIT;     /* VkPipeline -> struct gt_pipe* */
static char gt_dir[256];
static uint64_t gt_templ_updates; /* descriptor template updates seen (their storage buffers are not attributed) */

static uint64_t fnv1a(const void *p, size_t n) {
    const unsigned char *b = p;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h ? h : 1;
}

static void dump_spirv(uint64_t h, const void *code, size_t size) {
    if (!gt_dir[0]) return;
    char path[320];
    snprintf(path, sizeof path, "%s/%016" PRIx64 ".spv", gt_dir, h);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return; /* exists (same hash) or not writable (reported once at init) */
    const char *p = code;
    while (size) {
        ssize_t w = write(fd, p, size);
        if (w <= 0) break;
        p += w;
        size -= (size_t)w;
    }
    close(fd);
}

/* The 64-bit module hash lives in the hash's void * value: directly on LP64, boxed on LP32 (where a
 * pointer would keep only half of it and the dump file names would stop matching the reports). */
#if UINTPTR_MAX >= UINT64_MAX
#define GT_HASH_BOX(h) ((void *)(uintptr_t)(h))
#define GT_HASH_UNBOX(v) ((uint64_t)(uintptr_t)(v))
#define GT_HASH_FREE(v) ((void)(v))
#else
static void *gt_hash_box(uint64_t h) {
    uint64_t *p = malloc(sizeof(*p));
    if (p) *p = h;
    return p;
}
#define GT_HASH_BOX(h) gt_hash_box(h)
#define GT_HASH_UNBOX(v) ((v) ? *(const uint64_t *)(v) : 0)
#define GT_HASH_FREE(v) free(v)
#endif

static uint64_t stage_hash(const VkPipelineShaderStageCreateInfo *s) {
    if (s->module) return GT_HASH_UNBOX(nvhal_hash_get(&gt_modules, (uint64_t)s->module));
    for (const VkBaseInStructure *n = s->pNext; n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            const VkShaderModuleCreateInfo *m = (const void *)n;
            uint64_t h = fnv1a(m->pCode, m->codeSize);
            dump_spirv(h, m->pCode, m->codeSize);
            return h;
        }
    return 0;
}

static int stage_slot(VkShaderStageFlagBits s) {
    return s == VK_SHADER_STAGE_VERTEX_BIT ? 0 : s == VK_SHADER_STAGE_FRAGMENT_BIT ? 1
         : s == VK_SHADER_STAGE_COMPUTE_BIT ? 2 : 3;
}

static void pipe_record(VkPipeline p, VkPipelineBindPoint bp, uint32_t n, const VkPipelineShaderStageCreateInfo *st,
                        const void *pnext) {
    if (!p) return;
    struct gt_pipe *gp = calloc(1, sizeof *gp);
    if (!gp) return;
    gp->bp = bp;
    for (uint32_t i = 0; i < n; i++) gp->h[stage_slot(st[i].stage)] = stage_hash(&st[i]);
    for (const VkBaseInStructure *s = pnext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR) {
            const VkPipelineLibraryCreateInfoKHR *l = (const void *)s;
            for (uint32_t i = 0; i < l->libraryCount; i++) {
                const struct gt_pipe *lib = nvhal_hash_get(&gt_pipes, (uint64_t)l->pLibraries[i]);
                if (!lib) continue;
                for (int k = 0; k < 4; k++)
                    if (!gp->h[k]) gp->h[k] = lib->h[k];
            }
        }
    free(nvhal_hash_del(&gt_pipes, (uint64_t)p)); /* a recycled handle */
    if (!nvhal_hash_put(&gt_pipes, (uint64_t)p, gp)) free(gp);
}

static void pipe_label(VkPipeline p, char *out, size_t n) {
    const struct gt_pipe *gp = p ? nvhal_hash_get(&gt_pipes, (uint64_t)p) : NULL;
    if (!p) snprintf(out, n, "no-pipeline");
    else if (!gp) snprintf(out, n, "P%" PRIx64 " (unknown)", (uint64_t)p);
    else if (gp->bp == VK_PIPELINE_BIND_POINT_COMPUTE) snprintf(out, n, "P%" PRIx64 " cs=%016" PRIx64, (uint64_t)p, gp->h[2]);
    else
        snprintf(out, n, "P%" PRIx64 " vs=%016" PRIx64 " fs=%016" PRIx64 "%s", (uint64_t)p, gp->h[0], gp->h[1],
                 gp->h[3] ? " +gs/ts" : "");
}

/* ------------------------------------------------------------------ device setup */

static struct nvhal_gt_dev *gt_any; /* fallback for a command buffer the HAL does not know */

static int read_level(char *window, size_t wn) {
    char v[96] = "";
    window[0] = 0;
#ifdef __ANDROID__
    __system_property_get("debug.nvhal.gputime", v);
    char w[96] = "";
    __system_property_get("debug.nvhal.gputime.window", w);
    snprintf(window, wn, "%s", w);
#else
    const char *e = getenv("NVHAL_GPUTIME");
    if (e) snprintf(v, sizeof v, "%s", e);
    const char *w = getenv("NVHAL_GPUTIME_WINDOW");
    if (w) snprintf(window, wn, "%s", w);
#endif
    return atoi(v);
}

static void pick_dir(char *out, size_t n) {
#ifdef __ANDROID__
    char name[128] = "";
    FILE *f = fopen("/proc/self/cmdline", "re");
    if (f) {
        size_t r = fread(name, 1, sizeof name - 1, f);
        name[r] = 0;
        fclose(f);
    }
    char *colon = strchr(name, ':'); /* com.x:service -> com.x */
    if (colon) *colon = 0;
    if (name[0] && !strchr(name, '/')) snprintf(out, n, "/data/data/%s/nvhal-gputime", name);
    else snprintf(out, n, "/data/local/tmp/nvhal-gputime");
#else
    const char *e = getenv("NVHAL_GPUTIME_DIR");
    snprintf(out, n, "%s", e && *e ? e : "/tmp/nvhal-gputime");
#endif
    if (mkdir(out, 0755) != 0 && errno != EEXIST) {
        NVHAL_ERR("gputime: cannot create %s (%s); SPIR-V dumps and the report file are disabled (logcat only)", out,
                  strerror(errno));
        out[0] = 0;
    }
}

void nvhal_gt_device_init(struct nvhal_device *d) {
    char window[96];
    int level = read_level(window, sizeof window);
    if (level <= 0) return;
    struct nvhal_gt_dev *g = calloc(1, sizeof *g);
    struct gt_stat *tab = calloc(GT_TAB, sizeof *tab);
    if (!g || !tab) {
        free(g);
        free(tab);
        NVHAL_ERR("gputime: out of host memory; GPU timing stays off");
        return;
    }
    g->d = d;
    g->level = level;
    g->tab = tab;
    pthread_mutex_init(&g->mu, NULL);
    double ws = window[0] ? atof(window) : 3.0;
    g->window_ns = (uint64_t)((ws > 0.2 ? ws : 3.0) * 1e9);
    VkPhysicalDeviceProperties props;
    d->inst->GetPhysicalDeviceProperties(d->physical_device, &props);
    g->period = props.limits.timestampPeriod;
#define GTF(field, name) g->field = (void *)d->GetDeviceProcAddr(d->handle, name)
    GTF(CreateQueryPool, "vkCreateQueryPool");
    GTF(DestroyQueryPool, "vkDestroyQueryPool");
    GTF(GetQueryPoolResults, "vkGetQueryPoolResults");
    GTF(CmdResetQueryPool, "vkCmdResetQueryPool");
    GTF(CmdWriteTimestamp, "vkCmdWriteTimestamp");
#undef GTF
    if (!g->CreateQueryPool || !g->DestroyQueryPool || !g->GetQueryPoolResults || !g->CmdResetQueryPool ||
        !g->CmdWriteTimestamp || !props.limits.timestampComputeAndGraphics) {
        NVHAL_ERR("gputime: the ICD lacks timestamp queries on all queues; GPU timing stays off");
        free(tab);
        free(g);
        return;
    }
    if (!gt_dir[0]) pick_dir(gt_dir, sizeof gt_dir);
    snprintf(g->dir, sizeof g->dir, "%s", gt_dir);
    d->gt = g;
    if (!gt_any) gt_any = g;
    nvhal_log(NVHAL_LVL_WARN, "gputime: GPU timing ON (level %d, %.1f s windows, timestampPeriod %.3f ns); report and "
              "SPIR-V in %s", level, g->window_ns / 1e9, g->period, g->dir[0] ? g->dir : "(logcat only)");
}

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ------------------------------------------------------------------ aggregation and report */

static uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

/* the aggregation key of a render-pass record; the per-draw distribution and stat_add() must agree,
 * so both come through here */
static uint64_t rp_key(uint64_t rp, uint32_t w, uint32_t h, VkPipeline pipe, uint8_t op) {
    uint64_t key = mix(0, GK_RP);
    key = mix(key, rp);
    key = mix(key, w);
    key = mix(key, h);
    key = mix(key, (uint64_t)pipe);
    return mix(key, op);
}

static void stat_add(struct nvhal_gt_dev *g, const struct gt_rec *r, uint64_t ns) {
    uint64_t key = mix(0, r->kind);
    switch (r->kind) {
    case GK_RP: key = rp_key(r->rp, r->w, r->h, r->pipe, r->op); break;
    case GK_DRAW: case GK_CLEAR_ATT: case GK_DISPATCH: key = mix(key, (uint64_t)r->pipe); break;
    case GK_XFER: key = mix(key, r->op); break;
    default: break;
    }
    if (!key) key = 1;
    uint32_t i = (uint32_t)(key ^ (key >> 29)) & (GT_TAB - 1);
    for (uint32_t probe = 0; probe < GT_TAB; probe++, i = (i + 1) & (GT_TAB - 1)) {
        struct gt_stat *s = &g->tab[i];
        if (s->key == key) {
            s->ns += ns;
            s->n++;
            s->draws += r->draws;
            if (ns > s->max_ns) { s->max_ns = ns; s->w = r->w; s->h = r->h; s->z = r->z; }
            return;
        }
        if (!s->key) {
            if (g->ntab >= GT_TAB * 3 / 4) break;
            *s = (struct gt_stat){key, r->kind, r->op, r->w, r->h, r->z, r->pipe, r->rp, ns, 1, ns, r->draws,
                                  r->ssbo_n, r->ssbo_sys, r->ssbo_type};
            g->ntab++;
            return;
        }
    }
    g->dropped++;
}

static int cmp_stat(const void *a, const void *b) {
    const struct gt_stat *x = a, *y = b;
    return x->ns < y->ns ? 1 : x->ns > y->ns ? -1 : 0;
}
static int cmp_iv(const void *a, const void *b) {
    const uint64_t *x = a, *y = b;
    return x[0] < y[0] ? -1 : x[0] > y[0];
}

static void emit(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void emit(FILE *f, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    nvhal_log(NVHAL_LVL_INFO, "gputime: %s", buf);
    if (f) fprintf(f, "%s\n", buf);
}

/* ---- shape of the per-draw costs (gputime level 2) ----
 * after_draw() writes one bottom-of-pipe timestamp per draw inside an inline render pass; the stat table
 * below only aggregates those by pipeline (total / count / max). This keeps the actual shape per render
 * pass instance: min / p50 / p90 / max plus a histogram, so "every draw costs ~168 us" can be told apart
 * from "a few draws are very expensive". Reset with the rest of the window. */
#define RPD_TAB 64u
#define RPD_HB 40u /* 32 linear buckets of 16 us up to 512 us, then 8 logarithmic ones */
struct rpd_entry {
    uint64_t key, n, ns_sum, min_ns, max_ns;
    uint32_t hist[RPD_HB];
};
static struct rpd_entry rpd_tab[RPD_TAB];
static pthread_mutex_t rpd_mu = PTHREAD_MUTEX_INITIALIZER;

static uint32_t rpd_bucket(uint64_t ns) {
    uint64_t us = ns / 1000;
    if (us < 512) return (uint32_t)(us >> 4);
    if (us < 1024) return 32;
    if (us < 2048) return 33;
    if (us < 4096) return 34;
    if (us < 8192) return 35;
    if (us < 16384) return 36;
    if (us < 32768) return 37;
    if (us < 65536) return 38;
    return 39;
}
static void rpd_label(uint32_t b, char *o, size_t n) {
    static const unsigned lo[9] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 0};
    if (b < 32) snprintf(o, n, "%u-%uus", b * 16u, (b + 1u) * 16u);
    else if (b < 39) snprintf(o, n, "%u-%uus", lo[b - 32], lo[b - 31]);
    else snprintf(o, n, ">65536us");
}
static void rpd_add(uint64_t key, uint64_t ns) {
    if (!key) key = 1;
    pthread_mutex_lock(&rpd_mu);
    uint32_t i = (uint32_t)(key ^ (key >> 29)) & (RPD_TAB - 1);
    for (uint32_t probe = 0; probe < RPD_TAB; probe++, i = (i + 1) & (RPD_TAB - 1)) {
        struct rpd_entry *e = &rpd_tab[i];
        if (e->key != key && e->key) continue;
        if (!e->key) {
            memset(e, 0, sizeof *e);
            e->key = key;
            e->min_ns = UINT64_MAX;
        }
        e->n++;
        e->ns_sum += ns;
        if (ns < e->min_ns) e->min_ns = ns;
        if (ns > e->max_ns) e->max_ns = ns;
        e->hist[rpd_bucket(ns)]++;
        break;
    }
    pthread_mutex_unlock(&rpd_mu);
}
static void rpd_report(FILE *f, double fr) {
    struct rpd_entry v[16];
    uint32_t n = 0, dropped = 0;
    pthread_mutex_lock(&rpd_mu);
    for (uint32_t i = 0; i < RPD_TAB; i++) {
        if (!rpd_tab[i].key || !rpd_tab[i].n) continue;
        if (n < 16) v[n++] = rpd_tab[i];
        else dropped++;
    }
    memset(rpd_tab, 0, sizeof rpd_tab);
    pthread_mutex_unlock(&rpd_mu);
    if (!n) return;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = i + 1; j < n; j++)
            if (v[j].n > v[i].n) { struct rpd_entry t = v[i]; v[i] = v[j]; v[j] = t; }
    for (uint32_t i = 0; i < n; i++) {
        const struct rpd_entry *e = &v[i];
        uint64_t acc = 0, half = e->n / 2, p90 = (e->n * 9) / 10;
        char p50[28] = "?", p90s[28] = "?", hist[512] = "";
        uint32_t hn = 0;
        size_t hl = 0;
        for (uint32_t b = 0; b < RPD_HB; b++) {
            if (!e->hist[b]) continue;
            acc += e->hist[b];
            if (p50[0] == '?' && acc > half) rpd_label(b, p50, sizeof p50);
            if (p90s[0] == '?' && acc >= p90) rpd_label(b, p90s, sizeof p90s);
            if (hn < 10 && hl + 1 < sizeof hist) {
                char lab[28];
                int w;
                rpd_label(b, lab, sizeof lab);
                w = snprintf(hist + hl, sizeof hist - hl, "%s%s:%u", hn ? " " : "", lab, e->hist[b]);
                if (w > 0) hl += (size_t)w < sizeof hist - hl ? (size_t)w : sizeof hist - hl - 1;
                hn++;
            }
        }
        emit(f, "DRAW-DIST rp=%016" PRIx64 " n=%.0f/frame avg=%.1f us min=%.1f p50=%s p90=%s max=%.1f us  hist[%s]",
             e->key, (double)e->n / fr, (double)e->ns_sum / 1000.0 / (double)e->n, (double)e->min_ns / 1000.0, p50, p90s,
             (double)e->max_ns / 1000.0, hist);
    }
    if (dropped) emit(f, "DRAW-DIST: %u further render pass(es) with draws not shown", dropped);
}

/* under g->mu */
static void report(struct nvhal_gt_dev *g, uint64_t t) {
    double wall_ms = (t - g->win_t0) / 1e6;
    double fr = g->frames ? (double)g->frames : 1.0;
    /* GPU busy: union of the submitted command buffers' [first, last] timestamps */
    double busy_ms = 0, span_ms = 0;
    if (g->niv) {
        qsort(g->iv, g->niv, sizeof *g->iv, cmp_iv);
        uint64_t a = g->iv[0].a, b = g->iv[0].b, busy = 0, lo = a, hi = b;
        for (uint32_t i = 1; i < g->niv; i++) {
            if (g->iv[i].b > hi) hi = g->iv[i].b;
            if (g->iv[i].a > b) { busy += b - a; a = g->iv[i].a; b = g->iv[i].b; }
            else if (g->iv[i].b > b) b = g->iv[i].b;
        }
        busy += b - a;
        busy_ms = busy * g->period / 1e6;
        span_ms = (hi - lo) * g->period / 1e6;
    }
    struct gt_stat *v = malloc(GT_TAB * sizeof *v);
    uint32_t n = 0;
    double top_ms = 0, draw_ms = 0;
    for (uint32_t i = 0; i < GT_TAB; i++)
        if (g->tab[i].key) {
            if (v) v[n++] = g->tab[i];
            double ms = g->tab[i].ns * g->period / 1e6;
            if (g->tab[i].kind == GK_DRAW || g->tab[i].kind == GK_CLEAR_ATT) draw_ms += ms;
            else top_ms += ms;
        }
    FILE *f = NULL;
    if (g->dir[0]) {
        char path[320];
        snprintf(path, sizeof path, "%s/gputime.txt", g->dir);
        f = fopen(path, "ae");
    }
    emit(f, "=== window %.0f ms, %" PRIu64 " frames (%.1f fps), %.1f submits/frame, %.1f cmdbufs/frame; GPU per frame: "
         "busy %.3f ms (span %.1f ms/window), timed regions %.3f ms, draws(level 2) %.3f ms; dropped %" PRIu64
         " overflow %" PRIu64 " templ-updates %" PRIu64, wall_ms, g->frames, g->frames * 1000.0 / wall_ms, g->submits / fr, g->cbs / fr,
         busy_ms / fr, span_ms, top_ms / fr, draw_ms / fr, g->dropped, g->overflows, gt_templ_updates);
    if (v) {
        qsort(v, n, sizeof *v, cmp_stat);
        for (uint32_t i = 0; i < n && i < GT_TOP; i++) {
            const struct gt_stat *s = &v[i];
            char pl[160], det[256] = "";
            pipe_label(s->pipe, pl, sizeof pl);
            double ms = s->ns * g->period / 1e6;
            switch (s->kind) {
            case GK_RP:
                snprintf(det, sizeof det, "rp=%" PRIx64 " %ux%u att=%u draws/inst=%.1f first: %s", s->rp, s->w, s->h,
                         s->op, (double)s->draws / s->n, pl);
                break;
            case GK_DISPATCH:
                if (s->ssbo_sys)
                    snprintf(det, sizeof det, "groups(max-time inst) %ux%ux%u ssbo %u, NOT device-local %u (type %u) %s",
                             s->w, s->h, s->z, s->ssbo_n, s->ssbo_sys, s->ssbo_type, pl);
                else
                    snprintf(det, sizeof det, "groups(max-time inst) %ux%ux%u ssbo %u %s", s->w, s->h, s->z, s->ssbo_n, pl);
                break;
            case GK_XFER: snprintf(det, sizeof det, "%s", gx_name[s->op]); break;
            case GK_EXEC: snprintf(det, sizeof det, "vkCmdExecuteCommands"); break;
            default: snprintf(det, sizeof det, "%s", pl); break;
            }
            emit(f, "%-8s %8.3f ms/frame %7.1f/frame avg %8.1f us max %8.1f us  %s", gk_name[s->kind], ms / fr,
                 s->n / fr, ms * 1000.0 / s->n, s->max_ns * g->period / 1e3, det);
        }
        free(v);
    }
    rpd_report(f, fr);
    if (f) fclose(f);
    memset(g->tab, 0, GT_TAB * sizeof *g->tab);
    g->ntab = 0;
    g->niv = 0;
    g->frames = g->submits = g->cbs = g->dropped = g->overflows = 0;
    g->win_t0 = t;
}

void nvhal_gt_frame(struct nvhal_device *d) {
    struct nvhal_gt_dev *g = d->gt;
    pthread_mutex_lock(&g->mu);
    if (!g->win_t0) g->win_t0 = now_ns();
    g->frames++;
    pthread_mutex_unlock(&g->mu);
}

/* read the last submitted recording of c and fold it into the window */
static void harvest(struct nvhal_gt_cb *c) {
    struct nvhal_gt_dev *g = c->g;
    uint32_t nq = c->nq, nrec = c->nrec;
    bool sub = c->submitted;
    c->nq = c->nrec = 0;
    c->submitted = false;
    if (!sub || !nq || !nrec) return;
    uint64_t *res = malloc((size_t)nq * 2 * sizeof *res);
    if (!res) return;
    for (uint32_t p = 0; p * GT_QP_SIZE < nq && p < c->npools; p++) {
        uint32_t cnt = nq - p * GT_QP_SIZE;
        if (cnt > GT_QP_SIZE) cnt = GT_QP_SIZE;
        VkResult r = g->GetQueryPoolResults(g->d->handle, c->qp[p], 0, cnt, (size_t)cnt * 16, res + (size_t)p * GT_QP_SIZE * 2,
                                            16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (r != VK_SUCCESS && r != VK_NOT_READY) {
            for (uint32_t i = 0; i < cnt; i++) res[((size_t)p * GT_QP_SIZE + i) * 2 + 1] = 0;
        }
    }
#define AV(q) ((q) != GT_NOQ && (q) < nq && res[(size_t)(q) * 2 + 1])
#define TS(q) (res[(size_t)(q) * 2])
    uint64_t lo = UINT64_MAX, hi = 0;
    for (uint32_t q = 0; q < nq; q++)
        if (AV(q)) {
            if (TS(q) < lo) lo = TS(q);
            if (TS(q) > hi) hi = TS(q);
        }
    uint64_t t = now_ns();
    pthread_mutex_lock(&g->mu);
    if (!g->win_t0) g->win_t0 = t;
    g->cbs++;
    for (uint32_t i = 0; i < nrec; i++) {
        const struct gt_rec *r = &c->rec[i];
        if (!AV(r->qs) || !AV(r->qe) || TS(r->qe) < TS(r->qs)) continue;
        stat_add(g, r, TS(r->qe) - TS(r->qs));
        if (r->kind == GK_DRAW || r->kind == GK_CLEAR_ATT) rpd_add(r->rpkey, TS(r->qe) - TS(r->qs));
    }
    if (hi > lo) {
        if (g->niv == g->capiv) {
            uint32_t cap = g->capiv ? g->capiv * 2 : 1024;
            void *p = realloc(g->iv, cap * sizeof *g->iv);
            if (p) { g->iv = p; g->capiv = cap; }
        }
        if (g->niv < g->capiv) { g->iv[g->niv].a = lo; g->iv[g->niv].b = hi; g->niv++; }
    }
    if (g->frames && t - g->win_t0 >= g->window_ns) report(g, t);
    pthread_mutex_unlock(&g->mu);
#undef AV
#undef TS
    free(res);
}

void nvhal_gt_cb_free(struct nvhal_gt_cb *c) {
    if (!c) return;
    harvest(c);
    for (uint32_t i = 0; i < c->npools; i++) c->g->DestroyQueryPool(c->g->d->handle, c->qp[i], NULL);
    free(c->rec);
    free(c);
}

void nvhal_gt_device_teardown(struct nvhal_device *d) {
    struct nvhal_gt_dev *g = d->gt;
    if (!g) return;
    if (gt_any == g) gt_any = NULL;
    d->gt = NULL;
    free(g->tab);
    free(g->iv);
    pthread_mutex_destroy(&g->mu);
    free(g);
}

/* ------------------------------------------------------------------ memory placement of storage buffers */

/* value: memoryTypeIndex + 1 in bits 0-7, propertyFlags in bits 8-39 */
static struct nvhal_hash gt_mem = NVHAL_HASH_INIT;  /* VkDeviceMemory -> value */
static struct nvhal_hash gt_buf = NVHAL_HASH_INIT;  /* VkBuffer -> value */
struct gt_set { uint32_t n; struct { uint32_t binding; VkBuffer buf; } e[16]; };
static struct nvhal_hash gt_sets = NVHAL_HASH_INIT; /* VkDescriptorSet -> struct gt_set* (storage buffers only) */

static VKAPI_ATTR VkResult VKAPI_CALL gt_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *ai,
                                                        const VkAllocationCallbacks *alloc, VkDeviceMemory *out) {
    struct nvhal_device *d = nvhal_device_get(device);
    struct nvhal_gt_dev *g = d && d->gt ? d->gt : gt_any;
    VkResult r = g->next.AllocateMemory(device, ai, alloc, out);
    if (r == VK_SUCCESS && d && ai->memoryTypeIndex < d->mem_props.memoryTypeCount) {
        uint64_t v = (uint64_t)(ai->memoryTypeIndex + 1) |
                     ((uint64_t)d->mem_props.memoryTypes[ai->memoryTypeIndex].propertyFlags << 8);
        nvhal_hash_put(&gt_mem, (uint64_t)*out, (void *)(uintptr_t)v);
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL gt_FreeMemory(VkDevice device, VkDeviceMemory m, const VkAllocationCallbacks *alloc) {
    struct nvhal_device *d = nvhal_device_get(device);
    struct nvhal_gt_dev *g = d && d->gt ? d->gt : gt_any;
    if (m) nvhal_hash_del(&gt_mem, (uint64_t)m);
    g->next.FreeMemory(device, m, alloc);
}
static void buf_bound(VkBuffer b, VkDeviceMemory m) {
    void *v = nvhal_hash_get(&gt_mem, (uint64_t)m);
    if (v) nvhal_hash_put(&gt_buf, (uint64_t)b, v);
    else nvhal_hash_del(&gt_buf, (uint64_t)b);
}
static VKAPI_ATTR VkResult VKAPI_CALL gt_BindBufferMemory(VkDevice device, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) {
    struct nvhal_device *d = nvhal_device_get(device);
    struct nvhal_gt_dev *g = d && d->gt ? d->gt : gt_any;
    VkResult r = g->next.BindBufferMemory(device, b, m, o);
    if (r == VK_SUCCESS) buf_bound(b, m);
    return r;
}
static VkResult bind2(VkDevice device, uint32_t n, const VkBindBufferMemoryInfo *bi, bool khr) {
    struct nvhal_device *d = nvhal_device_get(device);
    struct nvhal_gt_dev *g = d && d->gt ? d->gt : gt_any;
    VkResult r = (khr ? g->next.BindBufferMemory2KHR : g->next.BindBufferMemory2)(device, n, bi);
    if (r == VK_SUCCESS)
        for (uint32_t i = 0; i < n; i++) buf_bound(bi[i].buffer, bi[i].memory);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL gt_BindBufferMemory2(VkDevice device, uint32_t n, const VkBindBufferMemoryInfo *bi) {
    return bind2(device, n, bi, false);
}
static VKAPI_ATTR VkResult VKAPI_CALL gt_BindBufferMemory2KHR(VkDevice device, uint32_t n,
                                                              const VkBindBufferMemoryInfo *bi) {
    return bind2(device, n, bi, true);
}
static VKAPI_ATTR void VKAPI_CALL gt_UpdateDescriptorSets(VkDevice device, uint32_t nw, const VkWriteDescriptorSet *w,
                                                          uint32_t nc, const VkCopyDescriptorSet *c) {
    struct nvhal_device *d = nvhal_device_get(device);
    struct nvhal_gt_dev *g = d && d->gt ? d->gt : gt_any;
    g->next.UpdateDescriptorSets(device, nw, w, nc, c);
    for (uint32_t i = 0; i < nw; i++) {
        if (w[i].descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
            w[i].descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) continue;
        struct gt_set *s = nvhal_hash_get(&gt_sets, (uint64_t)w[i].dstSet);
        if (!s) {
            s = calloc(1, sizeof *s);
            if (!s || !nvhal_hash_put(&gt_sets, (uint64_t)w[i].dstSet, s)) { free(s); continue; }
        }
        for (uint32_t k = 0; k < w[i].descriptorCount; k++) {
            uint32_t binding = w[i].dstBinding + w[i].dstArrayElement + k; /* approximate for arrays */
            uint32_t j = 0;
            while (j < s->n && s->e[j].binding != binding) j++;
            if (j == s->n) {
                if (s->n == 16) continue;
                s->n++;
            }
            s->e[j].binding = binding;
            s->e[j].buf = w[i].pBufferInfo[k].buffer;
        }
    }
}
static VKAPI_ATTR void VKAPI_CALL gt_UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set,
                                                                     VkDescriptorUpdateTemplate t, const void *data) {
    struct nvhal_device *d = nvhal_device_get(device);
    struct nvhal_gt_dev *g = d && d->gt ? d->gt : gt_any;
    gt_templ_updates++; /* not decoded: storage buffers written this way are not attributed */
    g->next.UpdateDescriptorSetWithTemplate(device, set, t, data);
}
static VKAPI_ATTR void VKAPI_CALL gt_UpdateDescriptorSetWithTemplateKHR(VkDevice device, VkDescriptorSet set,
                                                                        VkDescriptorUpdateTemplate t, const void *data) {
    struct nvhal_device *d = nvhal_device_get(device);
    struct nvhal_gt_dev *g = d && d->gt ? d->gt : gt_any;
    gt_templ_updates++;
    g->next.UpdateDescriptorSetWithTemplateKHR(device, set, t, data);
}
/* storage buffers in the compute sets bound to c */
static void ssbo_census(const struct nvhal_gt_cb *c, struct gt_rec *r) {
    for (int i = 0; i < 8; i++) {
        const struct gt_set *s = c->cset[i] ? nvhal_hash_get(&gt_sets, (uint64_t)c->cset[i]) : NULL;
        if (!s) continue;
        for (uint32_t j = 0; j < s->n; j++) {
            uint64_t v = (uint64_t)(uintptr_t)nvhal_hash_get(&gt_buf, (uint64_t)s->e[j].buf);
            if (r->ssbo_n < 255) r->ssbo_n++;
            if (v && !((v >> 8) & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                if (!r->ssbo_sys) r->ssbo_type = (uint8_t)((v & 0xff) - 1);
                if (r->ssbo_sys < 255) r->ssbo_sys++;
            }
        }
    }
}

/* ------------------------------------------------------------------ per command buffer */

static struct nvhal_gt_dev *gt_of(VkCommandBuffer cb, struct nvhal_gt_cb **c) {
    struct nvhal_cmd *e = nvhal_cmd_get(cb);
    *c = e ? e->gt : NULL;
    if (e && e->dev->gt) return e->dev->gt;
    return gt_any;
}

static uint32_t ts(struct nvhal_gt_cb *c, VkCommandBuffer cb) {
    if (!c || !c->active) return GT_NOQ;
    if (c->nq >= c->npools * GT_QP_SIZE) {
        c->overflow = true;
        return GT_NOQ;
    }
    uint32_t i = c->nq++;
    c->g->CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->qp[i / GT_QP_SIZE], i % GT_QP_SIZE);
    c->last_q = i;
    return i;
}

static struct gt_rec *push(struct nvhal_gt_cb *c) {
    if (c->nrec == c->caprec) {
        uint32_t cap = c->caprec ? c->caprec * 2 : 256;
        struct gt_rec *p = realloc(c->rec, cap * sizeof *p);
        if (!p) return NULL;
        c->rec = p;
        c->caprec = cap;
    }
    struct gt_rec *r = &c->rec[c->nrec++];
    memset(r, 0, sizeof *r);
    return r;
}

/* a region outside any render pass ends here */
static struct gt_rec *region(struct nvhal_gt_cb *c, VkCommandBuffer cb, uint8_t kind, uint8_t op, VkPipeline p,
                             uint32_t x, uint32_t y, uint32_t z) {
    if (!c || !c->active || c->in_rp) return NULL;
    uint32_t qs = c->last_q, qe = ts(c, cb);
    if (qe == GT_NOQ) return NULL;
    struct gt_rec *r = push(c);
    if (!r) return NULL;
    r->qs = qs;
    r->qe = qe;
    r->kind = kind;
    r->op = op;
    r->pipe = p;
    r->w = x;
    r->h = y;
    r->z = z;
    if (kind == GK_DISPATCH) ssbo_census(c, r);
    return r;
}

static void after_draw(struct nvhal_gt_cb *c, VkCommandBuffer cb, uint8_t kind) {
    if (!c) return;
    if (!c->primary) { /* a secondary: summarised into the primary's render pass at vkCmdExecuteCommands */
        if (!c->first_gfx) c->first_gfx = c->gfx;
        c->draws++;
        return;
    }
    if (!c->in_rp) return;
    struct gt_rec *rp = &c->rec[c->rp_rec];
    rp->draws++;
    if (!rp->pipe && kind == GK_DRAW) rp->pipe = c->gfx;
    if (c->g->level < 2 || c->rp_secondary) return;
    uint32_t qs = c->last_q, qe = ts(c, cb);
    if (qe == GT_NOQ) return;
    struct gt_rec *r = push(c);
    if (!r) return;
    r->qs = qs;
    r->qe = qe;
    r->kind = kind;
    r->pipe = kind == GK_DRAW ? c->gfx : VK_NULL_HANDLE;
    r->rpkey = rp_key(rp->rp, rp->w, rp->h, rp->pipe, rp->op);
}

static VKAPI_ATTR VkResult VKAPI_CALL gt_AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *ai,
                                                                VkCommandBuffer *out) {
    struct nvhal_device *d = nvhal_device_get(device);
    struct nvhal_gt_dev *g = d && d->gt ? d->gt : gt_any;
    VkResult r = g->next.AllocateCommandBuffers(device, ai, out);
    if (r != VK_SUCCESS || !d || !d->gt) return r;
    for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
        struct nvhal_cmd *e = nvhal_cmd_get(out[i]);
        if (!e) continue;
        nvhal_gt_cb_free(e->gt);
        e->gt = calloc(1, sizeof *e->gt);
        if (!e->gt) continue;
        e->gt->g = d->gt;
        e->gt->primary = ai->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        e->gt->want_q = GT_QP_SIZE;
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL gt_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    if (c) harvest(c);
    VkResult r = g->next.BeginCommandBuffer(cb, bi);
    if (r != VK_SUCCESS || !c) return r;
    c->in_rp = c->rp_secondary = c->overflow = false;
    c->gfx = c->cs = c->first_gfx = VK_NULL_HANDLE;
    memset(c->cset, 0, sizeof c->cset);
    c->draws = 0;
    c->last_q = GT_NOQ;
    c->active = c->primary;
    if (!c->active) return r;
    uint32_t want = (c->want_q + GT_QP_SIZE - 1) / GT_QP_SIZE;
    if (want > GT_MAXPOOLS) want = GT_MAXPOOLS;
    while (c->npools < want) {
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP,
                                    GT_QP_SIZE, 0};
        if (g->CreateQueryPool(g->d->handle, &qi, NULL, &c->qp[c->npools]) != VK_SUCCESS) break;
        c->npools++;
    }
    if (!c->npools) { c->active = false; return r; }
    for (uint32_t i = 0; i < c->npools; i++) g->CmdResetQueryPool(cb, c->qp[i], 0, GT_QP_SIZE);
    ts(c, cb);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL gt_EndCommandBuffer(VkCommandBuffer cb) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    if (c && c->active) {
        if (c->overflow) {
            c->want_q = c->npools * GT_QP_SIZE * 2;
            pthread_mutex_lock(&g->mu);
            g->overflows++;
            pthread_mutex_unlock(&g->mu);
        }
        else if (c->nq + 64 > c->want_q) c->want_q = c->nq + 64;
    }
    return g->next.EndCommandBuffer(cb);
}

static void mark_submitted(struct nvhal_gt_dev *g, VkCommandBuffer cb) {
    (void)g;
    struct nvhal_cmd *e = nvhal_cmd_get(cb);
    if (e && e->gt) e->gt->submitted = true;
}
static void count_submit(struct nvhal_gt_dev *g) {
    pthread_mutex_lock(&g->mu);
    g->submits++;
    pthread_mutex_unlock(&g->mu);
}

static VKAPI_ATTR VkResult VKAPI_CALL gt_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) {
    struct nvhal_device *d = nvhal_queue_device(q);
    struct nvhal_gt_dev *g = d && d->gt ? d->gt : gt_any;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j < s[i].commandBufferCount; j++) mark_submitted(g, s[i].pCommandBuffers[j]);
    count_submit(g);
    return g->next.QueueSubmit(q, n, s, f);
}
static VkResult submit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f, bool khr) {
    struct nvhal_device *d = nvhal_queue_device(q);
    struct nvhal_gt_dev *g = d && d->gt ? d->gt : gt_any;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j < s[i].commandBufferInfoCount; j++)
            mark_submitted(g, s[i].pCommandBufferInfos[j].commandBuffer);
    count_submit(g);
    return (khr ? g->next.QueueSubmit2KHR : g->next.QueueSubmit2)(q, n, s, f);
}
static VKAPI_ATTR VkResult VKAPI_CALL gt_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f) {
    return submit2(q, n, s, f, false);
}
static VKAPI_ATTR VkResult VKAPI_CALL gt_QueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f) {
    return submit2(q, n, s, f, true);
}

static VKAPI_ATTR void VKAPI_CALL gt_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    g->next.CmdBindPipeline(cb, bp, p);
    if (!c) return;
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) c->gfx = p;
    else if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) c->cs = p;
}

static VKAPI_ATTR void VKAPI_CALL gt_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l,
                                                           uint32_t first, uint32_t n, const VkDescriptorSet *sets,
                                                           uint32_t ndyn, const uint32_t *dyn) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    g->next.CmdBindDescriptorSets(cb, bp, l, first, n, sets, ndyn, dyn);
    if (!c || bp != VK_PIPELINE_BIND_POINT_COMPUTE) return;
    for (uint32_t i = 0; i < n && first + i < 8; i++) c->cset[first + i] = sets[i];
}

/* render pass instances: a timestamp before the begin (outside) and after the end */
static uint32_t rp_pre(struct nvhal_gt_cb *c, VkCommandBuffer cb) {
    if (!c || !c->active || c->in_rp) return GT_NOQ;
    return ts(c, cb);
}
static void rp_begin(struct nvhal_gt_cb *c, uint32_t qs, uint64_t rp, VkRect2D area, uint32_t natt, bool secondary) {
    if (!c || !c->active || qs == GT_NOQ) return;
    struct gt_rec *r = push(c);
    if (!r) return;
    r->kind = GK_RP;
    r->qs = qs;
    r->qe = GT_NOQ;
    r->rp = rp;
    r->w = area.extent.width;
    r->h = area.extent.height;
    r->op = (uint8_t)(natt > 255 ? 255 : natt);
    c->rp_rec = c->nrec - 1;
    c->in_rp = true;
    c->rp_secondary = secondary;
}
static void rp_end(struct nvhal_gt_cb *c, VkCommandBuffer cb) {
    if (!c || !c->active || !c->in_rp) return;
    c->in_rp = false;
    uint32_t qe = ts(c, cb);
    c->rec[c->rp_rec].qe = qe;
}

static VKAPI_ATTR void VKAPI_CALL gt_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi,
                                                        VkSubpassContents sc) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    uint32_t qs = rp_pre(c, cb);
    g->next.CmdBeginRenderPass(cb, bi, sc);
    rp_begin(c, qs, (uint64_t)bi->renderPass, bi->renderArea, 0 /* not tracked for VkRenderPass */,
             sc != VK_SUBPASS_CONTENTS_INLINE);
}
static void begin_rp2(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, const VkSubpassBeginInfo *si, bool khr) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    uint32_t qs = rp_pre(c, cb);
    (khr ? g->next.CmdBeginRenderPass2KHR : g->next.CmdBeginRenderPass2)(cb, bi, si);
    rp_begin(c, qs, (uint64_t)bi->renderPass, bi->renderArea, 0 /* not tracked for VkRenderPass */,
             si->contents != VK_SUBPASS_CONTENTS_INLINE);
}
static VKAPI_ATTR void VKAPI_CALL gt_CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi,
                                                         const VkSubpassBeginInfo *si) {
    begin_rp2(cb, bi, si, false);
}
static VKAPI_ATTR void VKAPI_CALL gt_CmdBeginRenderPass2KHR(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi,
                                                            const VkSubpassBeginInfo *si) {
    begin_rp2(cb, bi, si, true);
}
static VKAPI_ATTR void VKAPI_CALL gt_CmdEndRenderPass(VkCommandBuffer cb) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    g->next.CmdEndRenderPass(cb);
    rp_end(c, cb);
}
static VKAPI_ATTR void VKAPI_CALL gt_CmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo *ei) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    g->next.CmdEndRenderPass2(cb, ei);
    rp_end(c, cb);
}
static VKAPI_ATTR void VKAPI_CALL gt_CmdEndRenderPass2KHR(VkCommandBuffer cb, const VkSubpassEndInfo *ei) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    g->next.CmdEndRenderPass2KHR(cb, ei);
    rp_end(c, cb);
}
static void begin_rendering(VkCommandBuffer cb, const VkRenderingInfo *ri, bool khr) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    uint32_t qs = rp_pre(c, cb);
    (khr ? g->next.CmdBeginRenderingKHR : g->next.CmdBeginRendering)(cb, ri);
    rp_begin(c, qs, 0, ri->renderArea, ri->colorAttachmentCount + (ri->pDepthAttachment ? 1 : 0),
             (ri->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT) != 0);
}
static VKAPI_ATTR void VKAPI_CALL gt_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    begin_rendering(cb, ri, false);
}
static VKAPI_ATTR void VKAPI_CALL gt_CmdBeginRenderingKHR(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    begin_rendering(cb, ri, true);
}
static VKAPI_ATTR void VKAPI_CALL gt_CmdEndRendering(VkCommandBuffer cb) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    g->next.CmdEndRendering(cb);
    rp_end(c, cb);
}
static VKAPI_ATTR void VKAPI_CALL gt_CmdEndRenderingKHR(VkCommandBuffer cb) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    g->next.CmdEndRenderingKHR(cb);
    rp_end(c, cb);
}

#define GT_DRAW(field, kind, params, args) \
    static VKAPI_ATTR void VKAPI_CALL gt_##field params { \
        struct nvhal_gt_cb *c; \
        struct nvhal_gt_dev *g = gt_of(cb, &c); \
        g->next.field args; \
        after_draw(c, cb, kind); \
    }
GT_DRAW(CmdDraw, GK_DRAW, (VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t x, uint32_t y), (cb, a, b, x, y))
GT_DRAW(CmdDrawIndexed, GK_DRAW, (VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t x, int32_t o, uint32_t y),
        (cb, a, b, x, o, y))
GT_DRAW(CmdDrawIndirect, GK_DRAW, (VkCommandBuffer cb, VkBuffer bf, VkDeviceSize o, uint32_t n, uint32_t s),
        (cb, bf, o, n, s))
GT_DRAW(CmdDrawIndexedIndirect, GK_DRAW, (VkCommandBuffer cb, VkBuffer bf, VkDeviceSize o, uint32_t n, uint32_t s),
        (cb, bf, o, n, s))
GT_DRAW(CmdDrawIndirectCount, GK_DRAW, (VkCommandBuffer cb, VkBuffer bf, VkDeviceSize o, VkBuffer cbf, VkDeviceSize co,
        uint32_t m, uint32_t s), (cb, bf, o, cbf, co, m, s))
GT_DRAW(CmdDrawIndirectCountKHR, GK_DRAW, (VkCommandBuffer cb, VkBuffer bf, VkDeviceSize o, VkBuffer cbf,
        VkDeviceSize co, uint32_t m, uint32_t s), (cb, bf, o, cbf, co, m, s))
GT_DRAW(CmdDrawIndexedIndirectCount, GK_DRAW, (VkCommandBuffer cb, VkBuffer bf, VkDeviceSize o, VkBuffer cbf,
        VkDeviceSize co, uint32_t m, uint32_t s), (cb, bf, o, cbf, co, m, s))
GT_DRAW(CmdDrawIndexedIndirectCountKHR, GK_DRAW, (VkCommandBuffer cb, VkBuffer bf, VkDeviceSize o, VkBuffer cbf,
        VkDeviceSize co, uint32_t m, uint32_t s), (cb, bf, o, cbf, co, m, s))
GT_DRAW(CmdDrawMultiEXT, GK_DRAW, (VkCommandBuffer cb, uint32_t n, const VkMultiDrawInfoEXT *i, uint32_t ic,
        uint32_t fi, uint32_t st), (cb, n, i, ic, fi, st))
GT_DRAW(CmdDrawMultiIndexedEXT, GK_DRAW, (VkCommandBuffer cb, uint32_t n, const VkMultiDrawIndexedInfoEXT *i,
        uint32_t ic, uint32_t fi, uint32_t st, const int32_t *vo), (cb, n, i, ic, fi, st, vo))
GT_DRAW(CmdClearAttachments, GK_CLEAR_ATT, (VkCommandBuffer cb, uint32_t n, const VkClearAttachment *a, uint32_t rn,
        const VkClearRect *r), (cb, n, a, rn, r))
#undef GT_DRAW

static VKAPI_ATTR void VKAPI_CALL gt_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    g->next.CmdDispatch(cb, x, y, z);
    if (c) region(c, cb, GK_DISPATCH, 0, c->cs, x, y, z);
}
static VKAPI_ATTR void VKAPI_CALL gt_CmdDispatchBase(VkCommandBuffer cb, uint32_t bx, uint32_t by, uint32_t bz,
                                                     uint32_t x, uint32_t y, uint32_t z) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    g->next.CmdDispatchBase(cb, bx, by, bz, x, y, z);
    if (c) region(c, cb, GK_DISPATCH, 0, c->cs, x, y, z);
}
static VKAPI_ATTR void VKAPI_CALL gt_CmdDispatchBaseKHR(VkCommandBuffer cb, uint32_t bx, uint32_t by, uint32_t bz,
                                                        uint32_t x, uint32_t y, uint32_t z) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    g->next.CmdDispatchBaseKHR(cb, bx, by, bz, x, y, z);
    if (c) region(c, cb, GK_DISPATCH, 0, c->cs, x, y, z);
}
static VKAPI_ATTR void VKAPI_CALL gt_CmdDispatchIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    g->next.CmdDispatchIndirect(cb, b, o);
    if (c) region(c, cb, GK_DISPATCH, 0, c->cs, 0, 0, 0);
}

#define GT_XFER(field, op, params, args) \
    static VKAPI_ATTR void VKAPI_CALL gt_##field params { \
        struct nvhal_gt_cb *c; \
        struct nvhal_gt_dev *g = gt_of(cb, &c); \
        g->next.field args; \
        region(c, cb, GK_XFER, op, VK_NULL_HANDLE, 0, 0, 0); \
    }
GT_XFER(CmdCopyBuffer, GX_COPYBUF, (VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r),
        (cb, s, d, n, r))
GT_XFER(CmdCopyImage, GX_COPYIMG, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl,
        uint32_t n, const VkImageCopy *r), (cb, s, sl, d, dl, n, r))
GT_XFER(CmdCopyBufferToImage, GX_BUF2IMG, (VkCommandBuffer cb, VkBuffer s, VkImage d, VkImageLayout dl, uint32_t n,
        const VkBufferImageCopy *r), (cb, s, d, dl, n, r))
GT_XFER(CmdCopyImageToBuffer, GX_IMG2BUF, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkBuffer d, uint32_t n,
        const VkBufferImageCopy *r), (cb, s, sl, d, n, r))
GT_XFER(CmdBlitImage, GX_BLIT, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl,
        uint32_t n, const VkImageBlit *r, VkFilter f), (cb, s, sl, d, dl, n, r, f))
GT_XFER(CmdResolveImage, GX_RESOLVE, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl,
        uint32_t n, const VkImageResolve *r), (cb, s, sl, d, dl, n, r))
GT_XFER(CmdClearColorImage, GX_CLRCOLOR, (VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue *v,
        uint32_t n, const VkImageSubresourceRange *r), (cb, i, l, v, n, r))
GT_XFER(CmdClearDepthStencilImage, GX_CLRDS, (VkCommandBuffer cb, VkImage i, VkImageLayout l,
        const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r), (cb, i, l, v, n, r))
GT_XFER(CmdFillBuffer, GX_FILL, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize s, uint32_t v),
        (cb, b, o, s, v))
GT_XFER(CmdUpdateBuffer, GX_UPDATE, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize s, const void *p),
        (cb, b, o, s, p))
GT_XFER(CmdCopyBuffer2, GX_COPYBUF, (VkCommandBuffer cb, const VkCopyBufferInfo2 *i), (cb, i))
GT_XFER(CmdCopyBuffer2KHR, GX_COPYBUF, (VkCommandBuffer cb, const VkCopyBufferInfo2 *i), (cb, i))
GT_XFER(CmdCopyImage2, GX_COPYIMG, (VkCommandBuffer cb, const VkCopyImageInfo2 *i), (cb, i))
GT_XFER(CmdCopyImage2KHR, GX_COPYIMG, (VkCommandBuffer cb, const VkCopyImageInfo2 *i), (cb, i))
GT_XFER(CmdCopyBufferToImage2, GX_BUF2IMG, (VkCommandBuffer cb, const VkCopyBufferToImageInfo2 *i), (cb, i))
GT_XFER(CmdCopyBufferToImage2KHR, GX_BUF2IMG, (VkCommandBuffer cb, const VkCopyBufferToImageInfo2 *i), (cb, i))
GT_XFER(CmdCopyImageToBuffer2, GX_IMG2BUF, (VkCommandBuffer cb, const VkCopyImageToBufferInfo2 *i), (cb, i))
GT_XFER(CmdCopyImageToBuffer2KHR, GX_IMG2BUF, (VkCommandBuffer cb, const VkCopyImageToBufferInfo2 *i), (cb, i))
GT_XFER(CmdBlitImage2, GX_BLIT, (VkCommandBuffer cb, const VkBlitImageInfo2 *i), (cb, i))
GT_XFER(CmdBlitImage2KHR, GX_BLIT, (VkCommandBuffer cb, const VkBlitImageInfo2 *i), (cb, i))
GT_XFER(CmdResolveImage2, GX_RESOLVE, (VkCommandBuffer cb, const VkResolveImageInfo2 *i), (cb, i))
GT_XFER(CmdResolveImage2KHR, GX_RESOLVE, (VkCommandBuffer cb, const VkResolveImageInfo2 *i), (cb, i))
#undef GT_XFER

static VKAPI_ATTR void VKAPI_CALL gt_CmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *sec) {
    struct nvhal_gt_cb *c;
    struct nvhal_gt_dev *g = gt_of(cb, &c);
    g->next.CmdExecuteCommands(cb, n, sec);
    if (!c || !c->active) return;
    if (c->in_rp) {
        struct gt_rec *rp = &c->rec[c->rp_rec];
        for (uint32_t i = 0; i < n; i++) {
            struct nvhal_cmd *e = nvhal_cmd_get(sec[i]);
            if (!e || !e->gt) continue;
            rp->draws += e->gt->draws;
            if (!rp->pipe) rp->pipe = e->gt->first_gfx;
        }
        return;
    }
    region(c, cb, GK_EXEC, 0, VK_NULL_HANDLE, n, 0, 0);
}

/* ------------------------------------------------------------------ shaders and pipelines */

static VKAPI_ATTR VkResult VKAPI_CALL gt_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci,
                                                            const VkAllocationCallbacks *alloc, VkShaderModule *out) {
    struct nvhal_device *d = nvhal_device_get(device);
    struct nvhal_gt_dev *g = d && d->gt ? d->gt : gt_any;
    VkResult r = g->next.CreateShaderModule(device, ci, alloc, out);
    if (r == VK_SUCCESS) {
        uint64_t h = fnv1a(ci->pCode, ci->codeSize);
        dump_spirv(h, ci->pCode, ci->codeSize);
        nvhal_hash_put(&gt_modules, (uint64_t)*out, GT_HASH_BOX(h));
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL gt_DestroyShaderModule(VkDevice device, VkShaderModule m,
                                                         const VkAllocationCallbacks *alloc) {
    struct nvhal_device *d = nvhal_device_get(device);
    struct nvhal_gt_dev *g = d && d->gt ? d->gt : gt_any;
    if (m) GT_HASH_FREE(nvhal_hash_del(&gt_modules, (uint64_t)m));
    g->next.DestroyShaderModule(device, m, alloc);
}
static VKAPI_ATTR VkResult VKAPI_CALL gt_CreateGraphicsPipelines(VkDevice device, VkPipelineCache pc, uint32_t n,
                                                                 const VkGraphicsPipelineCreateInfo *ci,
                                                                 const VkAllocationCallbacks *alloc, VkPipeline *out) {
    struct nvhal_device *d = nvhal_device_get(device);
    struct nvhal_gt_dev *g = d && d->gt ? d->gt : gt_any;
    VkResult r = g->next.CreateGraphicsPipelines(device, pc, n, ci, alloc, out);
    if (r >= 0)
        for (uint32_t i = 0; i < n; i++)
            pipe_record(out[i], VK_PIPELINE_BIND_POINT_GRAPHICS, ci[i].stageCount, ci[i].pStages, ci[i].pNext);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL gt_CreateComputePipelines(VkDevice device, VkPipelineCache pc, uint32_t n,
                                                                const VkComputePipelineCreateInfo *ci,
                                                                const VkAllocationCallbacks *alloc, VkPipeline *out) {
    struct nvhal_device *d = nvhal_device_get(device);
    struct nvhal_gt_dev *g = d && d->gt ? d->gt : gt_any;
    VkResult r = g->next.CreateComputePipelines(device, pc, n, ci, alloc, out);
    if (r >= 0)
        for (uint32_t i = 0; i < n; i++) pipe_record(out[i], VK_PIPELINE_BIND_POINT_COMPUTE, 1, &ci[i].stage, ci[i].pNext);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL gt_DestroyPipeline(VkDevice device, VkPipeline p, const VkAllocationCallbacks *alloc) {
    struct nvhal_device *d = nvhal_device_get(device);
    struct nvhal_gt_dev *g = d && d->gt ? d->gt : gt_any;
    if (p) free(nvhal_hash_del(&gt_pipes, (uint64_t)p));
    g->next.DestroyPipeline(device, p, alloc);
}

/* ------------------------------------------------------------------ vkGetDeviceProcAddr hook */

PFN_vkVoidFunction nvhal_gt_wrap(struct nvhal_device *d, const char *name, PFN_vkVoidFunction fn) {
    struct nvhal_gt_dev *g = d->gt;
    if (!g || !fn) return fn;
#define GT_MATCH(type, field, nm) \
    if (!strcmp(name, nm)) { \
        g->next.field = (type)fn; \
        return (PFN_vkVoidFunction)gt_##field; \
    }
    GT_FNS(GT_MATCH)
#undef GT_MATCH
    return fn;
}

/* ------------------------------------------------------------------ EXPERIMENT: host-visible sysmem -> BAR */

/* Diagnostic only, never on unless a switch below is set when the device is created. A plain vkAllocateMemory (no
 * import/export) of the chosen type is made from another type; the app still sees the type it asked for.
 *
 *   debug.nvhal.exp.hostvis_to_bar (host NVHAL_EXP_HOSTVIS_TO_BAR)
 *     1  the original narrow rule: a type that is HOST_VISIBLE, not HOST_CACHED and not DEVICE_LOCAL is made from
 *        the type whose flags are the same plus DEVICE_LOCAL, i.e. the resizable-BAR window of VRAM.
 *     2  every type that lacks DEVICE_LOCAL is made from the closest DEVICE_LOCAL type (flags equal to the app's
 *        plus DEVICE_LOCAL first, otherwise the most property bits in common). Types whose semantics must not be
 *        changed - LAZILY_ALLOCATED and PROTECTED - are left alone in both modes. On Pascal the device-local
 *        window is usually ~256 MiB, so level 2 is expected to run into VK_ERROR_OUT_OF_DEVICE_MEMORY: that is a
 *        useful answer too, and the default path is untouched either way.
 *   debug.nvhal.exp.memtypes (host NVHAL_EXP_MEMTYPES)
 *     only print the memory type/heap table, remap nothing; the same table is printed when hostvis_to_bar is on.
 *
 * It exists to prove (A/B) that a GPU-heavy buffer placed in system memory by the GLES layer is the cost; the fix
 * belongs where the memory type is chosen (ANGLE). This file only fills d->mem_remap[]; the allocation hook that
 * reads it is nvhal_exp_mem_remap() below, called from ahb.c's vkAllocateMemory. */
/* The switch's own value, read once per device: 1 = the original narrow rule, 2 = every type that lacks
 * DEVICE_LOCAL. Anything > 2 is treated as 2. Returns 0 when the experiment is off. */
static int exp_level(char *out, size_t n) {
#ifdef __ANDROID__
    (void)n;
    __system_property_get("debug.nvhal.exp.hostvis_to_bar", out);
#else
    const char *e = getenv("NVHAL_EXP_HOSTVIS_TO_BAR");
    if (e) snprintf(out, n, "%s", e);
#endif
    int lvl = atoi(out);
    if (lvl > 2) lvl = 2;
    return lvl > 0 ? lvl : 0;
}

/* debug.nvhal.exp.memtypes (host NVHAL_EXP_MEMTYPES): print the memory table and remap nothing. */
static bool exp_table_wanted(void) {
    char v[96] = "";
#ifdef __ANDROID__
    __system_property_get("debug.nvhal.exp.memtypes", v);
#else
    const char *e = getenv("NVHAL_EXP_MEMTYPES");
    if (e) snprintf(v, sizeof v, "%s", e);
#endif
    return atoi(v) > 0;
}

/* debug.nvhal.exp.memtype_target (host NVHAL_EXP_MEMTYPE_TARGET), -1 = off: point every allocation whose type
 * lacks DEVICE_LOCAL at exactly this type index, so an A/B run can measure one type at a time (task-14). Takes
 * priority over hostvis_to_bar and hostvis_cached. */
static int exp_target(void) {
    char v[64] = "";
#ifdef __ANDROID__
    __system_property_get("debug.nvhal.exp.memtype_target", v);
#else
    const char *e = getenv("NVHAL_EXP_MEMTYPE_TARGET");
    if (e) snprintf(v, sizeof v, "%s", e);
#endif
    return v[0] ? atoi(v) : -1;
}

/* debug.nvhal.exp.hostvis_cached (host NVHAL_EXP_HOSTVIS_CACHED): 1 = move HOST_VISIBLE|HOST_COHERENT
 * allocations (map-able system memory the GPU reads over PCIe) to the HOST_CACHED type of the same heap. */
static bool exp_cached_wanted(void) {
    char v[64] = "";
#ifdef __ANDROID__
    __system_property_get("debug.nvhal.exp.hostvis_cached", v);
#else
    const char *e = getenv("NVHAL_EXP_HOSTVIS_CACHED");
    if (e) snprintf(v, sizeof v, "%s", e);
#endif
    return atoi(v) > 0;
}

/* The per-allocation src -> dst summary printed at vkDestroyDevice: one entry per remapped source type. */
#define ER_MAX 32u
struct er_pair {
    uint8_t src, dst, mode;
    uint64_t n, bytes;
};
static struct er_pair er_pairs[ER_MAX];
static uint32_t er_npairs;
static uint8_t er_mode;
static pthread_mutex_t er_mu = PTHREAD_MUTEX_INITIALIZER;

static const char *er_mode_name(uint8_t m) {
    return m == 1 ? "debug.nvhal.exp.memtype_target"
         : m == 2 ? "debug.nvhal.exp.hostvis_cached"
                  : "debug.nvhal.exp.hostvis_to_bar";
}

/* ahb.c calls this from the two vkAllocateMemory paths that can carry a remapped type. Off = one load. */
void nvhal_exp_mem_remap_note(struct nvhal_device *d, uint32_t app_type, uint64_t bytes, VkResult r) {
    if (r != VK_SUCCESS || app_type >= VK_MAX_MEMORY_TYPES) return;
    uint8_t m = d->mem_remap[app_type];
    if (!m) return;
    pthread_mutex_lock(&er_mu);
    struct er_pair *p = NULL;
    for (uint32_t i = 0; i < er_npairs; i++)
        if (er_pairs[i].src == app_type) {
            p = &er_pairs[i];
            break;
        }
    if (!p && er_npairs < ER_MAX) {
        p = &er_pairs[er_npairs++];
        p->src = (uint8_t)app_type;
        p->dst = (uint8_t)(m - 1u);
        p->mode = er_mode;
        p->n = 0;
        p->bytes = 0;
    }
    if (p) {
        p->n++;
        p->bytes += bytes;
    }
    pthread_mutex_unlock(&er_mu);
}

void nvhal_exp_mem_teardown(struct nvhal_device *d) {
    (void)d;
    pthread_mutex_lock(&er_mu);
    if (!er_npairs) {
        pthread_mutex_unlock(&er_mu);
        return;
    }
    char b[1024];
    int o = 0;
    for (uint32_t i = 0; i < er_npairs; i++) {
        int w = snprintf(b + o, sizeof b - (size_t)o, "%stype %u -> type %u [%s]: %llu alloc(s) %.1f MiB", i ? "; " : "",
                         er_pairs[i].src, er_pairs[i].dst, er_mode_name(er_pairs[i].mode),
                         (unsigned long long)er_pairs[i].n, (double)er_pairs[i].bytes / 1048576.0);
        if (w < 0 || (size_t)(o + w) >= sizeof b) {
            o = (int)sizeof b - 1;
            break;
        }
        o += w;
    }
    er_npairs = 0;
    pthread_mutex_unlock(&er_mu);
    nvhal_log(NVHAL_LVL_WARN, "EXPERIMENT memory remap summary at vkDestroyDevice: %s", b);
}

static void exp_flag_names(VkMemoryPropertyFlags f, char *out, size_t n) {
    static const struct {
        VkMemoryPropertyFlags bit;
        const char *name;
    } t[] = {
        {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "DEVICE_LOCAL"},
        {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "HOST_VISIBLE"},
        {VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "HOST_COHERENT"},
        {VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "HOST_CACHED"},
        {VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "LAZILY_ALLOCATED"},
        {VK_MEMORY_PROPERTY_PROTECTED_BIT, "PROTECTED"},
    };
    size_t o = 0;
    out[0] = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (f & t[i].bit)
            o += (size_t)snprintf(out + o, o < n ? n - o : 0, "%s%s", o ? "|" : "", t[i].name);
    if (!o && n) snprintf(out, n, "none");
}

/* bounded appender: once the buffer is full every later call is a no-op, so the line stays valid UTF-8/ASCII */
static int exp_buf(char *b, int n, size_t cap, const char *fmt, ...) {
    if (n < 0 || (size_t)n >= cap) return (int)cap;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(b + n, cap - (size_t)n, fmt, ap);
    va_end(ap);
    if (w < 0) return n;
    return n + w <= (int)cap ? n + w : (int)cap;
}

/* One line (logcat privileges a line over several): memoryTypeCount/heapCount, every type with its flags
 * spelled out and its heap, every heap with its size/flags, and the total the ICD reported. */
static void exp_print_mem_table(const struct nvhal_device *d) {
    const VkPhysicalDeviceMemoryProperties *mp = &d->mem_props;
    char b[2048];
    char nm[96];
    int n = 0;
    n = exp_buf(b, n, sizeof b, "memoryTypeCount=%u heapCount=%u;", mp->memoryTypeCount, mp->memoryHeapCount);
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
        exp_flag_names(mp->memoryTypes[i].propertyFlags, nm, sizeof nm);
        n = exp_buf(b, n, sizeof b, " type[%u]=%s 0x%x heap=%u;", i, nm, mp->memoryTypes[i].propertyFlags,
                    mp->memoryTypes[i].heapIndex);
    }
    for (uint32_t j = 0; j < mp->memoryHeapCount; j++) {
        exp_flag_names(mp->memoryHeaps[j].flags, nm, sizeof nm);
        n = exp_buf(b, n, sizeof b, " heap[%u]=%s 0x%x %llu MiB;", j, nm, mp->memoryHeaps[j].flags,
                    (unsigned long long)(mp->memoryHeaps[j].size >> 20));
    }
    uint64_t total = 0;
    for (uint32_t j = 0; j < mp->memoryHeapCount; j++) total += mp->memoryHeaps[j].size;
    n = exp_buf(b, n, sizeof b, " total=%llu MiB", (unsigned long long)(total >> 20));
    if ((size_t)n >= sizeof b) snprintf(b + sizeof b - 24, 24, " ...(truncated)");
    nvhal_log(NVHAL_LVL_WARN, "EXPERIMENT memory table: %s", b);
}

/* level 2 destination: the DEVICE_LOCAL type whose flags are the app's plus DEVICE_LOCAL if one exists, else the
 * DEVICE_LOCAL type sharing the most property bits with them (lowest index wins a tie). -1 = no candidate. */
static int exp_pick_dl(const VkPhysicalDeviceMemoryProperties *mp, VkMemoryPropertyFlags f) {
    const VkMemoryPropertyFlags DL = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    int best = -1;
    unsigned best_same = 0;
    for (uint32_t j = 0; j < mp->memoryTypeCount; j++) {
        VkMemoryPropertyFlags g = mp->memoryTypes[j].propertyFlags;
        if (!(g & DL)) continue;
        if (g == (f | DL)) return (int)j;
        unsigned same = (unsigned)__builtin_popcount((unsigned)(g & f));
        if (best < 0 || same > best_same) {
            best = (int)j;
            best_same = same;
        }
    }
    return best;
}

void nvhal_exp_mem_init(struct nvhal_device *d) {
    char v[96] = "";
    int lvl = exp_level(v, sizeof v);
    int target = exp_target();
    bool cached = exp_cached_wanted();
    const VkPhysicalDeviceMemoryProperties *mp = &d->mem_props;
    if (target >= (int)mp->memoryTypeCount) {
        nvhal_log(NVHAL_LVL_WARN, "EXPERIMENT debug.nvhal.exp.memtype_target=%d is not a memory type of this device "
                  "(memoryTypeCount=%u); ignoring it", target, mp->memoryTypeCount);
        target = -1;
    }
    /* priority: an explicit target beats the cached preference, which beats hostvis_to_bar */
    int mode = target >= 0 ? 1 : cached ? 2 : lvl ? 3 : 0;
    if (mode || exp_table_wanted()) exp_print_mem_table(d);
    if (!mode) return;
    er_mode = (uint8_t)mode;
    pthread_mutex_lock(&er_mu);
    er_npairs = 0;
    pthread_mutex_unlock(&er_mu);
    const VkMemoryPropertyFlags HV = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, HC = VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                CO = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                DL = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                SPECIAL = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT;
    bool seen[VK_MAX_MEMORY_HEAPS] = {false};
    uint32_t mapped = 0;
    uint64_t heap_bytes = 0;
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = mp->memoryTypes[i].propertyFlags;
        if (f & (DL | SPECIAL)) continue; /* already VRAM, or the type's semantics must not be changed */
        int j = -1;
        if (mode == 1) {
            j = target;
        } else if (mode == 2) {
            if (!(f & HV) || !(f & CO) || (f & HC)) continue;
            for (uint32_t k = 0; k < mp->memoryTypeCount; k++) {
                VkMemoryPropertyFlags g = mp->memoryTypes[k].propertyFlags;
                if ((g & (HV | HC)) == (HV | HC) && mp->memoryTypes[k].heapIndex == mp->memoryTypes[i].heapIndex) {
                    j = (int)k;
                    break;
                }
            }
        } else if (lvl == 1) {
            if (!(f & HV) || (f & HC)) continue;
            for (uint32_t k = 0; k < mp->memoryTypeCount; k++)
                if (mp->memoryTypes[k].propertyFlags == (f | DL)) {
                    j = (int)k;
                    break;
                }
        } else {
            j = exp_pick_dl(mp, f);
        }
        if (j < 0) {
            if (mode == 2)
                nvhal_log(NVHAL_LVL_WARN, "EXPERIMENT debug.nvhal.exp.hostvis_cached=1: memory type %u has no "
                          "HOST_VISIBLE|HOST_CACHED type in heap %u; it keeps the app's type", i,
                          mp->memoryTypes[i].heapIndex);
            continue;
        }
        if ((uint32_t)j == i) {
            nvhal_log(NVHAL_LVL_WARN, "EXPERIMENT %s=%d: memory type %u is the target itself and lacks DEVICE_LOCAL; it "
                      "keeps the app's type", er_mode_name((uint8_t)mode), mode == 1 ? target : 1, i);
            continue;
        }
        d->mem_remap[i] = (uint8_t)(j + 1);
        char fn[96], gn[96];
        exp_flag_names(f, fn, sizeof fn);
        exp_flag_names(mp->memoryTypes[j].propertyFlags, gn, sizeof gn);
        nvhal_log(NVHAL_LVL_WARN, "EXPERIMENT %s=%d: memory type %u (%s 0x%x heap %u) is allocated from type %u "
                  "(%s 0x%x heap %u, heap %llu MiB)", er_mode_name((uint8_t)mode), mode == 1 ? target : mode == 2 ? 1 : lvl,
                  i, fn, f, mp->memoryTypes[i].heapIndex, j, gn, mp->memoryTypes[j].propertyFlags,
                  mp->memoryTypes[j].heapIndex,
                  (unsigned long long)(mp->memoryHeaps[mp->memoryTypes[j].heapIndex].size >> 20));
        mapped++;
        uint32_t hi = mp->memoryTypes[j].heapIndex;
        if (hi < VK_MAX_MEMORY_HEAPS && !seen[hi]) {
            seen[hi] = true;
            heap_bytes += mp->memoryHeaps[hi].size;
        }
    }
    if (!mapped) return;
    if (mode == 3 && lvl == 2)
        nvhal_log(NVHAL_LVL_WARN, "EXPERIMENT debug.nvhal.exp.hostvis_to_bar=2: %u non-device-local type(s) now point at "
                  "VRAM type(s), covering %llu MiB of heap; on Pascal the device-local window is usually ~256 MiB, so "
                  "allocations that big fail with VK_ERROR_OUT_OF_DEVICE_MEMORY", mapped,
                  (unsigned long long)(heap_bytes >> 20));
    else if (mode == 1)
        nvhal_log(NVHAL_LVL_WARN, "EXPERIMENT debug.nvhal.exp.memtype_target=%d: %u non-device-local type(s) now point at "
                  "type %d, covering %llu MiB of heap; the per-allocation src->dst count/bytes summary follows at "
                  "vkDestroyDevice", target, mapped, target, (unsigned long long)(heap_bytes >> 20));
    else if (mode == 2)
        nvhal_log(NVHAL_LVL_WARN, "EXPERIMENT debug.nvhal.exp.hostvis_cached=1: %u host-visible type(s) now use a "
                  "HOST_CACHED type of the same heap (%llu MiB of heap); still map-able, GPU reads go through the cache",
                  mapped, (unsigned long long)(heap_bytes >> 20));
}

const VkMemoryAllocateInfo *nvhal_exp_mem_remap(struct nvhal_device *d, const VkMemoryAllocateInfo *ai,
                                                VkMemoryAllocateInfo *storage) {
    if (ai->memoryTypeIndex >= VK_MAX_MEMORY_TYPES || !d->mem_remap[ai->memoryTypeIndex]) return ai;
    for (const VkBaseInStructure *s = ai->pNext; s; s = s->pNext)
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO:
        case VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT:
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO:
        case VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO:
            break;
        default:
            return ai; /* imports, exports and anything else keep the app's type */
        }
    *storage = *ai;
    storage->memoryTypeIndex = d->mem_remap[ai->memoryTypeIndex] - 1u;
    return storage;
}

/* ------------------------------------------------------------------ allocation census
 * Opt-in census of what the app asks this HAL for through vkAllocateMemory: how many allocations succeeded
 * per memory type and how many bytes they were, how many failed with OUT_OF_DEVICE_MEMORY / OUT_OF_HOST_MEMORY
 * (the direct evidence for "the type is exhausted"), each split into "before the first present" and "after it"
 * so the preload phase (the DLC unpacking into VRAM) can be told apart from the render phase.
 * The same body is printed twice: on the present path, throttled to at most one line per window (default 5 s,
 * "alloc census @present #<n> t=<s>: ...") so a benchmark that never destroys its VkDevice still leaves a
 * timeline, and once at vkDestroyDevice marked "@final" when the app really does destroy it. Off (the default)
 * it costs one acquire load per allocation and one per present. */
#define MS_PHASES 2u
struct ms_slot {
    uint64_t ok_n, ok_bytes, fail_oodm, fail_oohm;
};
static struct {
    pthread_mutex_t mu;
    struct ms_slot s[MS_PHASES][VK_MAX_MEMORY_TYPES];
    int present_seen;
} ms_ = {.mu = PTHREAD_MUTEX_INITIALIZER};
/* flat statics, not fields inside a struct: on i686 a struct holding _Atomic uint64_t has an alignment GCC
 * notes as ABI-relevant, and this build is -Werror clean without the noise (same reasoning as imgxlate.c) */
/* plain uint64_t, not _Atomic: clang rejects __atomic_* builtins on an _Atomic-qualified object, and the
 * builtins themselves are what provide the atomicity (the same form the HAL already uses in ahb.c) */
static uint64_t ms_present_n;     /* presents since the first probe: the "#<n>" timeline marker */
static uint64_t ms_last_print_ns; /* CLOCK_MONOTONIC of the last @present line: the throttle gate */
static uint64_t ms_window_ns = 5000000000ull; /* 5 s default; debug.nvhal.exp.memstats.window overrides it */
static uint64_t ms_t0_ns;                     /* CLOCK_MONOTONIC at the first probe: "t=" is measured from here */

static int ms_state; /* 0: not probed yet, 1: off, 2: on */
static pthread_once_t ms_once = PTHREAD_ONCE_INIT;

static uint64_t ms_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void ms_init(void) {
    char v[64] = "";
    char w[64] = "";
#ifdef __ANDROID__
    __system_property_get("debug.nvhal.exp.memstats", v);
    __system_property_get("debug.nvhal.exp.memstats.window", w);
#else
    const char *e = getenv("NVHAL_EXP_MEMSTATS");
    if (e) snprintf(v, sizeof v, "%s", e);
    e = getenv("NVHAL_EXP_MEMSTATS_WINDOW");
    if (e) snprintf(w, sizeof w, "%s", e);
#endif
    double secs = atof(w);
    if (secs > 0) ms_window_ns = (uint64_t)((secs < 0.1 ? 0.1 : secs) * 1e9); /* never faster than 10 lines/s */
    ms_t0_ns = ms_now();
    int on = atoi(v) > 0;
    __atomic_store_n(&ms_state, on ? 2 : 1, __ATOMIC_RELEASE);
    if (on)
        nvhal_log(NVHAL_LVL_WARN, "alloc census (debug.nvhal.exp.memstats): statistics ON (window %.2f s: at most one "
                  "@present line per window, plus one @final line if the app destroys its VkDevice)",
                  (double)ms_window_ns / 1e9);
}
static bool ms_on(void) {
    int s = __atomic_load_n(&ms_state, __ATOMIC_ACQUIRE);
    if (__builtin_expect(s == 0, 0)) {
        pthread_once(&ms_once, ms_init);
        s = __atomic_load_n(&ms_state, __ATOMIC_ACQUIRE);
    }
    return s == 2;
}
/* the census itself, under the mutex; clear=1 empties it for a new device so its preload phase starts fresh */
static void ms_snapshot(struct ms_slot (*s)[VK_MAX_MEMORY_TYPES], int *seen, bool clear) {
    pthread_mutex_lock(&ms_.mu);
    memcpy(s, ms_.s, sizeof(struct ms_slot) * MS_PHASES * VK_MAX_MEMORY_TYPES);
    if (seen) *seen = ms_.present_seen;
    if (clear) {
        memset(ms_.s, 0, sizeof ms_.s);
        __atomic_store_n(&ms_.present_seen, 0, __ATOMIC_RELEASE); /* a new device starts a new preload phase */
    }
    pthread_mutex_unlock(&ms_.mu);
}

static void ms_mib(char *o, size_t n, uint64_t b) { snprintf(o, n, "%.1f MiB", (double)b / 1048576.0); }

/* the single body both lines share: totals, then one clause per memory type that saw any traffic */
static void ms_body(char *b, size_t cap, struct ms_slot s[MS_PHASES][VK_MAX_MEMORY_TYPES]) {
    uint64_t pre_n = 0, pre_b = 0, pre_d = 0, pre_h = 0, post_n = 0, post_b = 0, post_d = 0, post_h = 0;
    for (uint32_t t = 0; t < VK_MAX_MEMORY_TYPES; t++) {
        pre_n += s[0][t].ok_n;
        pre_b += s[0][t].ok_bytes;
        pre_d += s[0][t].fail_oodm;
        pre_h += s[0][t].fail_oohm;
        post_n += s[1][t].ok_n;
        post_b += s[1][t].ok_bytes;
        post_d += s[1][t].fail_oodm;
        post_h += s[1][t].fail_oohm;
    }
    char h[3][24];
    int n = 0;
    ms_mib(h[0], sizeof h[0], pre_b + post_b);
    ms_mib(h[1], sizeof h[1], pre_b);
    ms_mib(h[2], sizeof h[2], post_b);
    n = exp_buf(b, n, cap,
                "%llu ok (%s: %s before the first present, %s after), OUT_OF_DEVICE_MEMORY %llu, "
                "OUT_OF_HOST_MEMORY %llu;",
                (unsigned long long)(pre_n + post_n), h[0], h[1], h[2], (unsigned long long)(pre_d + post_d),
                (unsigned long long)(pre_h + post_h));
    for (uint32_t t = 0; t < VK_MAX_MEMORY_TYPES; t++) {
        const struct ms_slot *a = &s[0][t], *c = &s[1][t];
        if (!a->ok_n && !a->ok_bytes && !a->fail_oodm && !a->fail_oohm && !c->ok_n && !c->ok_bytes && !c->fail_oodm &&
            !c->fail_oohm)
            continue;
        ms_mib(h[1], sizeof h[1], a->ok_bytes);
        ms_mib(h[2], sizeof h[2], c->ok_bytes);
        n = exp_buf(b, n, cap,
                    " type %u: pre %llu ok %s (OODM %llu OOHM %llu), post %llu ok %s (OODM %llu OOHM %llu);", t,
                    (unsigned long long)a->ok_n, h[1], (unsigned long long)a->fail_oodm,
                    (unsigned long long)a->fail_oohm, (unsigned long long)c->ok_n, h[2],
                    (unsigned long long)c->fail_oodm, (unsigned long long)c->fail_oohm);
    }
}

/* one presented image (anb.c): ends the preload phase, bumps the marker and maybe prints the throttled line */
void nvhal_exp_memstats_present(struct nvhal_device *d) {
    (void)d;
    if (!ms_on()) return; /* off: one acquire load and nothing else */
    __atomic_store_n(&ms_.present_seen, 1, __ATOMIC_RELEASE);
    uint64_t n = 1 + __atomic_fetch_add(&ms_present_n, 1, __ATOMIC_RELAXED);
    uint64_t now = ms_now();
    uint64_t last = __atomic_load_n(&ms_last_print_ns, __ATOMIC_RELAXED);
    if (now - last < ms_window_ns) return; /* throttle: at most one line per window, without taking the lock */
    if (!__atomic_compare_exchange_n(&ms_last_print_ns, &last, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return; /* another thread already printed the line for this window */
    struct ms_slot s[MS_PHASES][VK_MAX_MEMORY_TYPES];
    int seen = 0;
    ms_snapshot(s, &seen, false);
    char b[1400];
    ms_body(b, sizeof b, s);
    nvhal_log(NVHAL_LVL_WARN, "alloc census @present #%llu t=%.1f s: %s first present seen: %s",
              (unsigned long long)n, (double)(now - ms_t0_ns) / 1e9, b, seen ? "yes" : "no");
}
void nvhal_exp_memstats_alloc(uint32_t type, uint64_t bytes, VkResult r) {
    if (!ms_on()) return;
    if (type >= VK_MAX_MEMORY_TYPES) return; /* the app's own bug; the ICD rejects it too */
    bool ok = r == VK_SUCCESS;
    if (!ok && r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_ERROR_OUT_OF_HOST_MEMORY) return;
    uint32_t ph = __atomic_load_n(&ms_.present_seen, __ATOMIC_ACQUIRE) ? 1u : 0u;
    pthread_mutex_lock(&ms_.mu);
    struct ms_slot *s = &ms_.s[ph][type];
    if (ok) {
        s->ok_n++;
        s->ok_bytes += bytes;
    } else if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        s->fail_oodm++;
    } else {
        s->fail_oohm++;
    }
    pthread_mutex_unlock(&ms_.mu);
}
/* a HAL-level rejection (ahb.c) or bookkeeping failure: the app sees this result, the ICD never hears about it */
void nvhal_exp_memstats_fail(uint32_t type, VkResult r) { nvhal_exp_memstats_alloc(type, 0, r); }

/* the app destroyed its VkDevice: print the same body once, marked final (3DMark often never gets here) */
void nvhal_exp_memstats_teardown(struct nvhal_device *d) {
    (void)d;
    if (!ms_on()) return;
    struct ms_slot s[MS_PHASES][VK_MAX_MEMORY_TYPES];
    int seen = 0;
    ms_snapshot(s, &seen, true);
    char b[1400];
    ms_body(b, sizeof b, s);
    nvhal_log(NVHAL_LVL_WARN, "alloc census @final (vkDestroyDevice): %s first present seen: %s", b,
              seen ? "yes" : "no");
}

