/*
 * orb-probe -- minimal Vulkan compute probe for the Geekbench 7
 * "Feature Matching" (ORB) validation race.
 *
 * WHAT IT REPLICATES (all of it is inside one command buffer per iteration):
 *   counter/candidate SSBOs cleared -> dispatch A (per-invocation atomicAdd,
 *   plus a shared-memory accumulator reduced by one atomic per workgroup, plus
 *   one candidate key written per invocation) -> the two barriers named in the
 *   ORB hypothesis (SHADER_STORAGE_WRITE -> SHADER_STORAGE_READ, and
 *   SHADER_STORAGE_WRITE -> SHADER_STORAGE_WRITE|SHADER_WRITE) -> a GPU-side
 *   derivation of the indirect dispatch arguments from the counter ->
 *   dispatch B via vkCmdDispatchIndirect -> vkCmdCopyBuffer into host-visible
 *   staging -> fence -> compare against an exact CPU expectation.
 *
 * WHY IT CAN DISCRIMINATE
 *   The CPU expectation is exact and closed-form, so every deviation prints as
 *   expected / actual / signed delta. Geekbench's ORB failure signature is a
 *   systematic GPU overcount (CPU reference 162 every single time; GPU
 *   201/208, i.e. +24..28%), so a probe iteration that reports
 *   actual > expected points at over-execution (indirect groupCount source, a
 *   re-run pass, a stale counter), not at random corruption.
 *   The same binary is meant to be run both on the host (proprietary ICD) and
 *   inside the guest (the HAL path), which separates "our layer" from
 *   "driver/algorithm".
 *
 * THE AUTHORS DO NOT RUN THIS PROGRAM. The GPU is owned by bench-operator.
 * Build it, hand it over, read the artifacts. See README.md.
 *
 * exit codes: 0 = every iteration matched, 1 = at least one mismatch,
 *             2 = setup error (loader/device/memory type unavailable).
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#include "shaders_spv.h"

#define ORB_WG 64u

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("orb-probe: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

#define VKCALL(expr) do { VkResult r_ = (expr); if (r_ != VK_SUCCESS) die("%s:%d: %s -> VkResult %d", __FILE__, __LINE__, #expr, (int)r_); } while (0)

/* ------------------------------------------------------------------ loader */

static PFN_vkGetInstanceProcAddr g_gipa;

static const char *loader_open(void)
{
    const char *env = getenv("ORB_PROBE_VULKAN");
    const char *names[4];
    unsigned n = 0;
    if (env) names[n++] = env;
    names[n++] = "libvulkan.so.1";
    names[n++] = "libvulkan.so";
    names[n] = NULL;

    for (unsigned i = 0; names[i]; i++) {
        void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (!h)
            continue;
        g_gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
        if (!g_gipa)
            die("%s has no vkGetInstanceProcAddr", names[i]);
        return names[i];
    }
    die("cannot dlopen a Vulkan loader (tried $ORB_PROBE_VULKAN, libvulkan.so.1, libvulkan.so): %s", dlerror());
    return NULL;
}

static PFN_vkVoidFunction gipa(VkInstance inst, const char *name)
{
    PFN_vkVoidFunction f = g_gipa(inst, name);
    if (!f)
        die("Vulkan loader has no entry point %s", name);
    return f;
}

#define VKF(inst, name) ((PFN_##name)gipa((inst), #name))

static uint32_t g_inst_api;

/* ----------------------------------------------------------------- options */

typedef struct {
    int list_types;
    int device_index;
    int iterations;
    uint32_t groups;
    const char *mem_type;
    int indirect;
    const char *barrier;
    const char *spv_dir;
    int json;
    int quiet;
    int print_all;
} opts_t;

static void usage(void)
{
    say("orb-probe -- minimal Vulkan compute probe for the Geekbench ORB validation race");
    say("");
    say("usage: orb-probe [options]");
    say("");
    say("  --list-types            enumerate devices/memory types and exit (no dispatch)");
    say("  --device N              physical device index (default 0)");
    say("  --iterations K          iterations per memory type (default 300)");
    say("  --groups G              dispatch-A workgroups (default 1024; N = G * %u)", ORB_WG);
    say("  --mem-type auto|all|N   device memory type for the GPU-side buffers");
    say("  --indirect on|off       dispatch B via vkCmdDispatchIndirect (default on)");
    say("  --barrier full|no-pass2|no-args|none|memory   (default full)");
    say("  --spv-dir DIR           load SPIR-V from DIR instead of the embedded blobs");
    say("  --json                  also print one JSON result object per variant");
    say("  --quiet                 suppress per-iteration output");
    say("  --print-all             print every mismatching iteration, not just the first");
    say("  --help");
    say("");
    say("exit: 0 = every iteration matched the CPU expectation, 1 = mismatch, 2 = setup error");
    say("NOTE: this program is not run by its authors; the GPU is owned by bench-operator.");
}

static void parse_args(int argc, char **argv, opts_t *o)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); exit(0); }
        else if (!strcmp(a, "--list-types")) o->list_types = 1;
        else if (!strcmp(a, "--device") && v) { o->device_index = atoi(v); i++; }
        else if (!strcmp(a, "--iterations") && v) { o->iterations = atoi(v); i++; }
        else if (!strcmp(a, "--groups") && v) { o->groups = (uint32_t)strtoul(v, NULL, 0); i++; }
        else if (!strcmp(a, "--mem-type") && v) { o->mem_type = v; i++; }
        else if (!strcmp(a, "--indirect") && v) { o->indirect = !!strcmp(v, "off"); i++; }
        else if (!strcmp(a, "--barrier") && v) { o->barrier = v; i++; }
        else if (!strcmp(a, "--spv-dir") && v) { o->spv_dir = v; i++; }
        else if (!strcmp(a, "--json")) o->json = 1;
        else if (!strcmp(a, "--quiet")) o->quiet = 1;
        else if (!strcmp(a, "--print-all")) o->print_all = 1;
        else die("unknown or incomplete option: %s (try --help)", a);
    }
    if (o->iterations <= 0) die("--iterations must be > 0");
    if (o->groups == 0 || o->groups > 262144u) die("--groups must be 1..262144");
    const char *ok[] = { "full", "no-pass2", "no-args", "none", "memory" };
    int found = 0;
    for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) if (!strcmp(o->barrier, ok[i])) found = 1;
    if (!found) die("--barrier must be full | no-pass2 | no-args | none | memory");
}

/* ------------------------------------------------------ memory type helpers */

typedef struct { const char *name; VkMemoryPropertyFlags bit; } flagmap_t;

static const flagmap_t g_flagmap[] = {
    { "DEVICE_LOCAL",     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT },
    { "HOST_VISIBLE",     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT },
    { "HOST_COHERENT",    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT },
    { "HOST_CACHED",      VK_MEMORY_PROPERTY_HOST_CACHED_BIT },
    { "LAZILY_ALLOCATED", VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT },
    { "PROTECTED",        VK_MEMORY_PROPERTY_PROTECTED_BIT },
    { "DEVICE_COHERENT",  VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD },
    { "DEVICE_UNCACHED",  VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD },
    { "RDMA_CAPABLE",     VK_MEMORY_PROPERTY_RDMA_CAPABLE_BIT_NV },
};

static void mem_flags_str(VkMemoryPropertyFlags f, char *out, size_t n)
{
    out[0] = 0;
    for (unsigned i = 0; i < sizeof(g_flagmap) / sizeof(g_flagmap[0]); i++) {
        if (!(f & g_flagmap[i].bit)) continue;
        if (out[0]) strncat(out, "|", n - strlen(out) - 1);
        strncat(out, g_flagmap[i].name, n - strlen(out) - 1);
    }
    if (!out[0]) snprintf(out, n, "0x%x", (unsigned)f);
}

/* ------------------------------------------------------------- vulkan state */

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;
    uint32_t type;
    void *map;
} gpubuf_t;

typedef struct {
    VkInstance inst;
    VkDevice dev;
    VkPhysicalDevice pdev;
    VkPhysicalDeviceMemoryProperties mp;
    VkQueue queue;
    uint32_t qfam;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkPipelineLayout playout;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool dpool;
    VkDescriptorSet dset;
    VkPipeline pipeA, pipeArgs, pipeB;
    uint32_t n, seed, groups;
    uint32_t staging_type;
    uint32_t staging_flags;
} ctx_t;

typedef struct {
    gpubuf_t counterA, counterB, errs, args, keysA, keysB, staging;
    VkDeviceSize keys_size;
    VkDeviceSize staging_size;
} bufs_t;

static void make_buffer(ctx_t *c, VkDeviceSize size, VkBufferUsageFlags usage,
                        uint32_t memtype, gpubuf_t *b, const char *what)
{
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCALL(VKF(c->inst, vkCreateBuffer)(c->dev, &bi, NULL, &b->buffer));

    VkMemoryRequirements req;
    VKF(c->inst, vkGetBufferMemoryRequirements)(c->dev, b->buffer, &req);
    if (!(req.memoryTypeBits & (1u << memtype)))
        die("memory type %u is not usable for %s (memoryTypeBits=0x%x)", memtype, what, req.memoryTypeBits);

    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memtype;
    VKCALL(VKF(c->inst, vkAllocateMemory)(c->dev, &ai, NULL, &b->memory));
    VKCALL(VKF(c->inst, vkBindBufferMemory)(c->dev, b->buffer, b->memory, 0));
    b->size = size;
    b->type = memtype;
    b->map = NULL;
}

static void destroy_buffer(ctx_t *c, gpubuf_t *b)
{
    if (b->map) { VKF(c->inst, vkUnmapMemory)(c->dev, b->memory); b->map = NULL; }
    if (b->buffer) { VKF(c->inst, vkDestroyBuffer)(c->dev, b->buffer, NULL); b->buffer = VK_NULL_HANDLE; }
    if (b->memory) { VKF(c->inst, vkFreeMemory)(c->dev, b->memory, NULL); b->memory = VK_NULL_HANDLE; }
}

/* --------------------------------------------------------------- shaders */

static VkShaderModule load_module(ctx_t *c, const char *name,
                                  const unsigned char *embedded, unsigned int embedded_len,
                                  const char *spv_dir)
{
    const unsigned char *data = embedded;
    unsigned int len = embedded_len;
    unsigned char *heap = NULL;

    if (spv_dir) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.spv", spv_dir, name);
        FILE *f = fopen(path, "rb");
        if (!f) die("cannot open %s", path);
        if (fseek(f, 0, SEEK_END) != 0) die("cannot seek %s", path);
        long sz = ftell(f);
        if (sz <= 0) die("%s is empty", path);
        if (fseek(f, 0, SEEK_SET) != 0) die("cannot seek %s", path);
        heap = malloc((size_t)sz);
        if (!heap) die("out of memory reading %s", path);
        if (fread(heap, 1, (size_t)sz, f) != (size_t)sz) die("short read on %s", path);
        fclose(f);
        data = heap;
        len = (unsigned int)sz;
    }

    VkShaderModuleCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = len;
    ci.pCode = (const uint32_t *)data;
    VkShaderModule m;
    VKCALL(VKF(c->inst, vkCreateShaderModule)(c->dev, &ci, NULL, &m));
    free(heap);
    return m;
}

static VkPipeline make_pipeline(ctx_t *c, VkShaderModule mod)
{
    VkComputePipelineCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = mod;
    ci.stage.pName = "main";
    ci.layout = c->playout;
    VkPipeline p;
    VKCALL(VKF(c->inst, vkCreateComputePipelines)(c->dev, VK_NULL_HANDLE, 1, &ci, NULL, &p));
    return p;
}

/* ------------------------------------------------------ recording / verify */

typedef struct { uint32_t n, seed, wg, pad; } push_t;

static void mem_barrier(ctx_t *c, VkPipelineStageFlags src, VkAccessFlags sa,
                        VkPipelineStageFlags dst, VkAccessFlags da)
{
    VkMemoryBarrier mb;
    memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = sa;
    mb.dstAccessMask = da;
    VKF(c->inst, vkCmdPipelineBarrier)(c->cmd, src, dst, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void copy_region(ctx_t *c, VkBuffer src, VkBuffer dst,
                        VkDeviceSize src_off, VkDeviceSize dst_off, VkDeviceSize size)
{
    VkBufferCopy r;
    memset(&r, 0, sizeof r);
    r.srcOffset = src_off;
    r.dstOffset = dst_off;
    r.size = size;
    VKF(c->inst, vkCmdCopyBuffer)(c->cmd, src, dst, 1, &r);
}

static void record(ctx_t *c, bufs_t *b, const opts_t *o)
{
    PFN_vkCmdFillBuffer        pFill  = VKF(c->inst, vkCmdFillBuffer);
    PFN_vkCmdDispatch          pDisp  = VKF(c->inst, vkCmdDispatch);
    PFN_vkCmdDispatchIndirect  pInd   = VKF(c->inst, vkCmdDispatchIndirect);
    PFN_vkCmdBindPipeline      pBind  = VKF(c->inst, vkCmdBindPipeline);
    PFN_vkCmdBindDescriptorSets pDset = VKF(c->inst, vkCmdBindDescriptorSets);
    PFN_vkCmdPushConstants     pPush  = VKF(c->inst, vkCmdPushConstants);

    VKF(c->inst, vkResetCommandBuffer)(c->cmd, 0);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCALL(VKF(c->inst, vkBeginCommandBuffer)(c->cmd, &bi));

    pFill(c->cmd, b->counterA.buffer, 0, 8, 0);
    pFill(c->cmd, b->counterB.buffer, 0, 4, 0);
    pFill(c->cmd, b->errs.buffer, 0, 4, 0);
    pFill(c->cmd, b->args.buffer, 0, 12, 0);
    pFill(c->cmd, b->keysA.buffer, 0, b->keys_size, 0xFFFFFFFFu);
    pFill(c->cmd, b->keysB.buffer, 0, b->keys_size, 0xFFFFFFFFu);

    mem_barrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    int v_full   = !strcmp(o->barrier, "full");
    int v_mem    = !strcmp(o->barrier, "memory");
    int b_read   = v_full || v_mem || !strcmp(o->barrier, "no-pass2") || !strcmp(o->barrier, "no-args");
    int b_pass2  = v_full || v_mem || !strcmp(o->barrier, "no-args");
    int b_args   = v_full || v_mem || !strcmp(o->barrier, "no-pass2");

    push_t pc;
    pc.n = c->n;
    pc.seed = c->seed;
    pc.wg = ORB_WG;
    pc.pad = 0;

    pBind(c->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipeA);
    pDset(c->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->playout, 0, 1, &c->dset, 0, NULL);
    pPush(c->cmd, c->playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    pDisp(c->cmd, c->groups, 1, 1);

    /* Barrier BEFORE prepare_args: publishes dispatch A's countA so that
     * prepare_args can read it. */
    if (v_mem) {
        mem_barrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    } else if (b_read) {
        mem_barrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }

    pBind(c->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipeArgs);
    pPush(c->cmd, c->playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    pDisp(c->cmd, 1, 1, 1);

    /* Barrier AFTER prepare_args: its args write must be visible to the indirect
     * dispatch.  This used to be emitted *before* prepare_args, i.e. before the
     * very write it was supposed to publish, so every indirect run read the
     * zero-filled args buffer and dispatched zero groups (host: 3 mismatches per
     * iteration, keysB never written).  Confirmed by --indirect off being clean. */
    if (v_mem) {
        mem_barrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    } else {
        if (b_pass2)
            mem_barrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        if (b_args && o->indirect)
            mem_barrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    }

    pBind(c->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipeB);
    pPush(c->cmd, c->playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    if (o->indirect)
        pInd(c->cmd, b->args.buffer, 0);
    else
        pDisp(c->cmd, c->groups, 1, 1);

    mem_barrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);

    copy_region(c, b->counterA.buffer, b->staging.buffer, 0, 0, 8);
    copy_region(c, b->counterB.buffer, b->staging.buffer, 0, 8, 4);
    copy_region(c, b->errs.buffer, b->staging.buffer, 0, 12, 4);
    copy_region(c, b->args.buffer, b->staging.buffer, 0, 16, 12);
    copy_region(c, b->keysA.buffer, b->staging.buffer, 0, 28, b->keys_size);
    copy_region(c, b->keysB.buffer, b->staging.buffer, 0, 28 + b->keys_size, b->keys_size);

    VKCALL(VKF(c->inst, vkEndCommandBuffer)(c->cmd));
}

typedef struct { int n; char text[768]; } mm_t;

static void mm(mm_t *m, const char *fmt, ...)
{
    char line[224];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    m->n++;
    if (m->n <= 6) {
        if (m->text[0]) strncat(m->text, " | ", sizeof m->text - strlen(m->text) - 1);
        strncat(m->text, line, sizeof m->text - strlen(m->text) - 1);
    }
}

static void verify(ctx_t *c, bufs_t *b, mm_t *m)
{
    const uint32_t *p = (const uint32_t *)b->staging.map;
    uint32_t n = c->n;
    uint32_t groups = n / ORB_WG;

    if (p[0] != n)
        mm(m, "counterA(global) exp=%u act=%u delta=%+lld", n, p[0], (long long)p[0] - (long long)n);
    if (p[1] != n)
        mm(m, "counterA(shared) exp=%u act=%u delta=%+lld", n, p[1], (long long)p[1] - (long long)n);
    if (p[2] != n)
        mm(m, "counterB(seq) exp=%u act=%u delta=%+lld", n, p[2], (long long)p[2] - (long long)n);
    if (p[3] != 0)
        mm(m, "errs(bad_visibility_or_overrun) exp=0 act=%u", p[3]);
    if (p[4] != groups || p[5] != 1 || p[6] != 1)
        mm(m, "indirect_args exp=(%u,1,1) act=(%u,%u,%u)", groups, p[4], p[5], p[6]);

    const uint32_t *kA = p + 7;
    const uint32_t *kB = p + 7 + n;

    uint32_t badA = 0;
    int firstA = -1;
    for (uint32_t i = 0; i < n; i++) {
        if (kA[i] != (i ^ c->seed)) {
            if (firstA < 0) firstA = (int)i;
            badA++;
        }
    }
    if (badA)
        mm(m, "keysA(pattern_or_visibility) bad=%u first=%d", badA, firstA);

    uint64_t sum = 0;
    uint32_t unwritten = 0;
    int firstU = -1;
    for (uint32_t i = 0; i < n; i++) {
        if (kB[i] == 0xFFFFFFFFu) {
            if (firstU < 0) firstU = (int)i;
            unwritten++;
        } else {
            sum += kB[i];
        }
    }
    uint64_t exp_sum = (uint64_t)n * (uint64_t)(n - 1) / 2ull;
    if (unwritten)
        mm(m, "keysB(slots_never_written) bad=%u first=%d", unwritten, firstU);
    if (sum != exp_sum)
        mm(m, "keysB.sum exp=%llu act=%llu delta=%+lld",
           (unsigned long long)exp_sum, (unsigned long long)sum, (long long)(sum - exp_sum));
}

/* --------------------------------------------------------------- one type */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int run_one_type(ctx_t *c, const opts_t *o, uint32_t memtype, const char *mtname)
{
    bufs_t b;
    memset(&b, 0, sizeof b);
    b.keys_size = (VkDeviceSize)c->n * 4;
    b.staging_size = 28 + 2 * b.keys_size;

    const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    make_buffer(c, 8, storage, memtype, &b.counterA, "counterA");
    make_buffer(c, 4, storage, memtype, &b.counterB, "counterB");
    make_buffer(c, 4, storage, memtype, &b.errs, "errs");
    make_buffer(c, 12, storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, memtype, &b.args, "args");
    make_buffer(c, b.keys_size, storage, memtype, &b.keysA, "keysA");
    make_buffer(c, b.keys_size, storage, memtype, &b.keysB, "keysB");
    make_buffer(c, b.staging_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, c->staging_type, &b.staging, "staging");
    VKCALL(VKF(c->inst, vkMapMemory)(c->dev, b.staging.memory, 0, VK_WHOLE_SIZE, 0, &b.staging.map));

    VkDescriptorBufferInfo info[6];
    VkBuffer bufs6[6];
    bufs6[0] = b.counterA.buffer;
    bufs6[1] = b.keysA.buffer;
    bufs6[2] = b.args.buffer;
    bufs6[3] = b.counterB.buffer;
    bufs6[4] = b.keysB.buffer;
    bufs6[5] = b.errs.buffer;

    VkWriteDescriptorSet wr[6];
    memset(wr, 0, sizeof wr);
    for (int i = 0; i < 6; i++) {
        info[i].buffer = bufs6[i];
        info[i].offset = 0;
        info[i].range = VK_WHOLE_SIZE;
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = c->dset;
        wr[i].dstBinding = (uint32_t)i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wr[i].pBufferInfo = &info[i];
    }
    VKF(c->inst, vkUpdateDescriptorSets)(c->dev, 6, wr, 0, NULL);

    int total_bad = 0;
    int first_bad = -1;
    uint64_t worst_abs = 0;
    double t0 = now_ms();

    for (int it = 0; it < o->iterations; it++) {
        record(c, &b, o);
        VKCALL(VKF(c->inst, vkResetFences)(c->dev, 1, &c->fence));
        VkSubmitInfo si;
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &c->cmd;
        VKCALL(VKF(c->inst, vkQueueSubmit)(c->queue, 1, &si, c->fence));
        VKCALL(VKF(c->inst, vkWaitForFences)(c->dev, 1, &c->fence, VK_TRUE, UINT64_MAX));

        if (!(c->staging_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            VkMappedMemoryRange r;
            memset(&r, 0, sizeof r);
            r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            r.memory = b.staging.memory;
            r.offset = 0;
            r.size = VK_WHOLE_SIZE;
            VKCALL(VKF(c->inst, vkInvalidateMappedMemoryRanges)(c->dev, 1, &r));
        }

        mm_t m;
        memset(&m, 0, sizeof m);
        verify(c, &b, &m);
        if (m.n) {
            total_bad += m.n;
            if (first_bad < 0) first_bad = it;
            if (!o->quiet && (first_bad == it || o->print_all))
                say("iter=%d mismatches=%d %s", it, m.n, m.text);
        }
    }

    double dt = now_ms() - t0;

    char first_s[32];
    if (first_bad < 0)
        snprintf(first_s, sizeof first_s, "none");
    else
        snprintf(first_s, sizeof first_s, "%d", first_bad);

    if (o->json)
        say("{\"probe\":\"orb-probe\",\"type\":%u,\"flags\":\"%s\",\"iterations\":%d,\"mismatches\":%d,"
            "\"first_mismatch\":%s,\"n\":%u,\"groups\":%u,\"indirect\":%s,\"barrier\":\"%s\",\"elapsed_ms\":%.1f}",
            memtype, mtname, o->iterations, total_bad,
            first_bad < 0 ? "null" : first_s,
            c->n, c->groups, o->indirect ? "true" : "false", o->barrier, dt);

    say("RESULT type=%u flags=%s iters=%d mismatches=%d first=%s n=%u groups=%u indirect=%s barrier=%s elapsed_ms=%.1f",
        memtype, mtname, o->iterations, total_bad,
        first_s, c->n, c->groups, o->indirect ? "on" : "off", o->barrier, dt);

    (void)worst_abs;
    destroy_buffer(c, &b.keysB);
    destroy_buffer(c, &b.keysA);
    destroy_buffer(c, &b.args);
    destroy_buffer(c, &b.errs);
    destroy_buffer(c, &b.counterB);
    destroy_buffer(c, &b.counterA);
    destroy_buffer(c, &b.staging);
    return total_bad;
}

/* ------------------------------------------------------------- list types */

static uint32_t storage_mem_bits(ctx_t *c)
{
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = 4096;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf;
    VKCALL(VKF(c->inst, vkCreateBuffer)(c->dev, &bi, NULL, &buf));
    VkMemoryRequirements req;
    VKF(c->inst, vkGetBufferMemoryRequirements)(c->dev, buf, &req);
    VKF(c->inst, vkDestroyBuffer)(c->dev, buf, NULL);
    return req.memoryTypeBits;
}

static void list_types(ctx_t *c, const char *ldr)
{
    say("loader               : %s", ldr);
    say("instance apiVersion  : %u.%u.%u", VK_API_VERSION_MAJOR(g_inst_api),
        VK_API_VERSION_MINOR(g_inst_api), VK_API_VERSION_PATCH(g_inst_api));
    const char *icd = getenv("VK_ICD_FILENAMES");
    if (!icd) icd = getenv("VK_DRIVER_FILES");
    say("ICD env              : %s", icd ? icd : "(unset: loader picks /usr/share/vulkan/icd.d)");

    uint32_t ndev = 0;
    VKCALL(VKF(c->inst, vkEnumeratePhysicalDevices)(c->inst, &ndev, NULL));
    if (!ndev) die("no Vulkan physical devices visible to this loader");
    VkPhysicalDevice *devs = calloc(ndev, sizeof *devs);
    VKCALL(VKF(c->inst, vkEnumeratePhysicalDevices)(c->inst, &ndev, devs));
    say("physical devices     : %u", ndev);

    PFN_vkGetPhysicalDeviceProperties2 gp2 =
        (PFN_vkGetPhysicalDeviceProperties2)g_gipa(c->inst, "vkGetPhysicalDeviceProperties2");

    for (uint32_t d = 0; d < ndev; d++) {
        VkPhysicalDevice pdev = devs[d];
        VkPhysicalDeviceProperties p;
        VKF(c->inst, vkGetPhysicalDeviceProperties)(pdev, &p);
        say("");
        say("device[%u] %s", d, p.deviceName);
        say("  deviceType      : %d", (int)p.deviceType);
        say("  apiVersion      : %u.%u.%u", VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        say("  driverVersion   : %u (0x%08x)", p.driverVersion, p.driverVersion);
        say("  vendorID/deviceID: 0x%04x / 0x%04x", p.vendorID, p.deviceID);

        if (gp2 && g_inst_api >= VK_API_VERSION_1_2) {
            VkPhysicalDeviceDriverProperties dp;
            memset(&dp, 0, sizeof dp);
            dp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            VkPhysicalDeviceProperties2 p2;
            memset(&p2, 0, sizeof p2);
            p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            p2.pNext = &dp;
            gp2(pdev, &p2);
            say("  driver          : driverID=%d driverName=\"%s\" driverInfo=\"%s\"",
                (int)dp.driverID, dp.driverName, dp.driverInfo);
        }

        uint32_t nqf = 0;
        VKF(c->inst, vkGetPhysicalDeviceQueueFamilyProperties)(pdev, &nqf, NULL);
        VkQueueFamilyProperties *qf = calloc(nqf, sizeof *qf);
        VKF(c->inst, vkGetPhysicalDeviceQueueFamilyProperties)(pdev, &nqf, qf);
        for (uint32_t i = 0; i < nqf; i++)
            say("  queueFamily[%u] : flags=0x%x count=%u", i, qf[i].queueFlags, qf[i].queueCount);
        free(qf);

        VkPhysicalDeviceMemoryProperties mp;
        VKF(c->inst, vkGetPhysicalDeviceMemoryProperties)(pdev, &mp);
        for (uint32_t i = 0; i < mp.memoryHeapCount; i++) {
            char f[128];
            mem_flags_str(mp.memoryHeaps[i].flags, f, sizeof f);
            say("  heap[%u]        : %llu MiB %s", i,
                (unsigned long long)(mp.memoryHeaps[i].size / (1024ull * 1024ull)), f);
        }

        /* memoryTypeBits for the buffers the probe actually allocates */
        ctx_t tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.inst = c->inst;
        tmp.dev = c->dev;
        uint32_t bits = storage_mem_bits(&tmp);
        say("  memory types usable for our storage/transfer buffers (memoryTypeBits=0x%x):", bits);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            char f[160];
            mem_flags_str(mp.memoryTypes[i].propertyFlags, f, sizeof f);
            say("    type[%u] heap=%u flags=%s usable=%s", i, mp.memoryTypes[i].heapIndex, f,
                (bits & (1u << i)) ? "yes" : "no");
        }

        uint32_t staging = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            if ((bits & (1u << i)) &&
                ((mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
                staging = i;
                break;
            }
        }
        if (staging == UINT32_MAX)
            for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
                if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                    staging = i;
                    break;
                }
        say("  staging type    : %s", staging == UINT32_MAX ? "NONE (probe cannot run)" : "see below");
        if (staging != UINT32_MAX) {
            char f[160];
            mem_flags_str(mp.memoryTypes[staging].propertyFlags, f, sizeof f);
            say("    -> type[%u] flags=%s heap=%u", staging, f, mp.memoryTypes[staging].heapIndex);
        }

        /* what --mem-type auto will pick for the GPU-side buffers (first usable DEVICE_LOCAL) */
        uint32_t auto_t = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                auto_t = i;
                break;
            }
        say("  auto storage type: %s",
            auto_t == UINT32_MAX ? "NONE (--mem-type auto would abort; use --mem-type all)" : "see below");
        if (auto_t != UINT32_MAX) {
            char f[160];
            mem_flags_str(mp.memoryTypes[auto_t].propertyFlags, f, sizeof f);
            say("    -> type[%u] flags=%s heap=%u   <- what --mem-type auto uses for GPU-side buffers",
                auto_t, f, mp.memoryTypes[auto_t].heapIndex);
        }
    }
    free(devs);
}

/* ------------------------------------------------------------------- main */

static void create_instance_and_device(opts_t *o, ctx_t *c)
{
    PFN_vkEnumerateInstanceVersion eiv =
        (PFN_vkEnumerateInstanceVersion)g_gipa(NULL, "vkEnumerateInstanceVersion");
    uint32_t api = VK_API_VERSION_1_0;
    if (eiv) eiv(&api);
    g_inst_api = api;

    VkApplicationInfo app;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "orb-probe";
    app.applicationVersion = 1;
    app.pEngineName = "orb-probe";
    app.engineVersion = 1;
    app.apiVersion = api > VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : api;

    VkInstanceCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance inst;
    VKCALL(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ci, NULL, &inst));
    c->inst = inst;

    uint32_t ndev = 0;
    VKCALL(VKF(inst, vkEnumeratePhysicalDevices)(inst, &ndev, NULL));
    if (!ndev) die("no Vulkan physical devices visible to this loader");
    VkPhysicalDevice *devs = calloc(ndev, sizeof *devs);
    VKCALL(VKF(inst, vkEnumeratePhysicalDevices)(inst, &ndev, devs));
    if (o->device_index < 0 || (uint32_t)o->device_index >= ndev) {
        for (uint32_t i = 0; i < ndev; i++) {
            VkPhysicalDeviceProperties p;
            VKF(inst, vkGetPhysicalDeviceProperties)(devs[i], &p);
            say("device[%u]: %s", i, p.deviceName);
        }
        die("--device %d out of range (%u devices)", o->device_index, ndev);
    }
    VkPhysicalDevice pdev = devs[(uint32_t)o->device_index];
    VkPhysicalDeviceProperties p;
    VKF(inst, vkGetPhysicalDeviceProperties)(pdev, &p);
    say("device: %s (apiVersion %u.%u.%u driverVersion %u)", p.deviceName,
        VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
        VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion);
    c->pdev = pdev;
    VKF(inst, vkGetPhysicalDeviceMemoryProperties)(pdev, &c->mp);

    uint32_t nqf = 0;
    VKF(inst, vkGetPhysicalDeviceQueueFamilyProperties)(pdev, &nqf, NULL);
    VkQueueFamilyProperties *qf = calloc(nqf, sizeof *qf);
    VKF(inst, vkGetPhysicalDeviceQueueFamilyProperties)(pdev, &nqf, qf);
    c->qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nqf; i++)
        if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && c->qfam == UINT32_MAX) c->qfam = i;
    free(qf);
    if (c->qfam == UINT32_MAX) die("no COMPUTE queue family");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof qci);
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = c->qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof dci);
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VKCALL(VKF(inst, vkCreateDevice)(pdev, &dci, NULL, &c->dev));
    VKF(inst, vkGetDeviceQueue)(c->dev, c->qfam, 0, &c->queue);
}

static void create_pipeline_objects(opts_t *o, ctx_t *c)
{
    VkDescriptorSetLayoutBinding binds[6];
    memset(binds, 0, sizeof binds);
    for (int i = 0; i < 6; i++) {
        binds[i].binding = (uint32_t)i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli;
    memset(&dli, 0, sizeof dli);
    dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dli.bindingCount = 6;
    dli.pBindings = binds;
    VKCALL(VKF(c->inst, vkCreateDescriptorSetLayout)(c->dev, &dli, NULL, &c->dsl));

    VkPushConstantRange pcr;
    memset(&pcr, 0, sizeof pcr);
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(push_t);
    VkPipelineLayoutCreateInfo pli;
    memset(&pli, 0, sizeof pli);
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &c->dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VKCALL(VKF(c->inst, vkCreatePipelineLayout)(c->dev, &pli, NULL, &c->playout));

    VkDescriptorPoolSize ps;
    memset(&ps, 0, sizeof ps);
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 6;
    VkDescriptorPoolCreateInfo dpi;
    memset(&dpi, 0, sizeof dpi);
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VKCALL(VKF(c->inst, vkCreateDescriptorPool)(c->dev, &dpi, NULL, &c->dpool));
    VkDescriptorSetAllocateInfo dsa;
    memset(&dsa, 0, sizeof dsa);
    dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsa.descriptorPool = c->dpool;
    dsa.descriptorSetCount = 1;
    dsa.pSetLayouts = &c->dsl;
    VKCALL(VKF(c->inst, vkAllocateDescriptorSets)(c->dev, &dsa, &c->dset));

    VkCommandPoolCreateInfo cpi;
    memset(&cpi, 0, sizeof cpi);
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = c->qfam;
    VKCALL(VKF(c->inst, vkCreateCommandPool)(c->dev, &cpi, NULL, &c->pool));
    VkCommandBufferAllocateInfo cbi;
    memset(&cbi, 0, sizeof cbi);
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = c->pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VKCALL(VKF(c->inst, vkAllocateCommandBuffers)(c->dev, &cbi, &c->cmd));

    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof fci);
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VKCALL(VKF(c->inst, vkCreateFence)(c->dev, &fci, NULL, &c->fence));

    VkShaderModule mA = load_module(c, "dispatch_a", orb_spv_dispatch_a, orb_spv_dispatch_a_len, o->spv_dir);
    VkShaderModule mP = load_module(c, "prepare_args", orb_spv_prepare_args, orb_spv_prepare_args_len, o->spv_dir);
    VkShaderModule mB = load_module(c, "dispatch_b", orb_spv_dispatch_b, orb_spv_dispatch_b_len, o->spv_dir);
    c->pipeA = make_pipeline(c, mA);
    c->pipeArgs = make_pipeline(c, mP);
    c->pipeB = make_pipeline(c, mB);
    VKF(c->inst, vkDestroyShaderModule)(c->dev, mA, NULL);
    VKF(c->inst, vkDestroyShaderModule)(c->dev, mP, NULL);
    VKF(c->inst, vkDestroyShaderModule)(c->dev, mB, NULL);
}

static uint32_t pick_staging_type(ctx_t *c, uint32_t bits)
{
    uint32_t best = UINT32_MAX;
    for (uint32_t i = 0; i < c->mp.memoryTypeCount; i++) {
        if (!(bits & (1u << i))) continue;
        VkMemoryPropertyFlags f = c->mp.memoryTypes[i].propertyFlags;
        if ((f & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            best = i;
            break;
        }
    }
    if (best == UINT32_MAX) {
        for (uint32_t i = 0; i < c->mp.memoryTypeCount; i++) {
            if (!(bits & (1u << i))) continue;
            if (c->mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) { best = i; break; }
        }
    }
    if (best == UINT32_MAX) die("no HOST_VISIBLE memory type usable for the staging buffer");
    return best;
}

int main(int argc, char **argv)
{
    opts_t o;
    memset(&o, 0, sizeof o);
    o.device_index = 0;
    o.iterations = 300;
    o.groups = 1024;
    o.mem_type = "auto";
    o.indirect = 1;
    o.barrier = "full";
    parse_args(argc, argv, &o);

    const char *ldr = loader_open();

    ctx_t c;
    memset(&c, 0, sizeof c);
    create_instance_and_device(&o, &c);

    if (o.list_types) {
        list_types(&c, ldr);
        return 0;
    }

    say("loader: %s (instance apiVersion %u.%u.%u)", ldr, VK_API_VERSION_MAJOR(g_inst_api),
        VK_API_VERSION_MINOR(g_inst_api), VK_API_VERSION_PATCH(g_inst_api));

    create_pipeline_objects(&o, &c);

    c.n = o.groups * ORB_WG;
    c.seed = 0x12345678u;
    c.groups = o.groups;

    uint32_t bits = storage_mem_bits(&c);
    c.staging_type = pick_staging_type(&c, bits);
    c.staging_flags = c.mp.memoryTypes[c.staging_type].propertyFlags;

    int total = 0;
    int variants = 0;

    if (!strcmp(o.mem_type, "all")) {
        for (uint32_t i = 0; i < c.mp.memoryTypeCount; i++) {
            if (!(bits & (1u << i))) continue;
            char f[160];
            mem_flags_str(c.mp.memoryTypes[i].propertyFlags, f, sizeof f);
            say("--- memtype %u %s (heap %u) ---", i, f, c.mp.memoryTypes[i].heapIndex);
            total += run_one_type(&c, &o, i, f);
            variants++;
        }
    } else {
        uint32_t idx = c.staging_type;
        if (strcmp(o.mem_type, "auto")) {
            idx = (uint32_t)strtoul(o.mem_type, NULL, 0);
            if (idx >= c.mp.memoryTypeCount) die("--mem-type %u out of range (%u types)", idx, c.mp.memoryTypeCount);
            if (!(bits & (1u << idx))) die("--mem-type %u is not usable for our buffers (bits=0x%x)", idx, bits);
        } else {
            int found = 0;
            for (uint32_t i = 0; i < c.mp.memoryTypeCount; i++)
                if ((bits & (1u << i)) && (c.mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    idx = i;
                    found = 1;
                    break;
                }
            if (!found) die("no usable DEVICE_LOCAL memory type; use --mem-type all");
        }
        char f[160];
        mem_flags_str(c.mp.memoryTypes[idx].propertyFlags, f, sizeof f);
        say("--- memtype %u %s (heap %u) ---", idx, f, c.mp.memoryTypes[idx].heapIndex);
        total += run_one_type(&c, &o, idx, f);
        variants++;
    }

    say("ORB_PROBE_DONE variants=%d mismatches_total=%d n=%u groups=%u indirect=%s barrier=%s staging_type=%u",
        variants, total, c.n, c.groups, o.indirect ? "on" : "off", o.barrier, c.staging_type);

    VKF(c.inst, vkDeviceWaitIdle)(c.dev);
    return total ? 1 : 0;
}
