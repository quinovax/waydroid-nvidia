/* Compressed-format emulation: ASTC LDR (risk R8) and ETC2/EAC. Desktop NVIDIA has neither, and its ICD even
 * accepts vkCreateImage for an ASTC format and then reports memory requirements of size 0 (measured on 615.71.09).
 * Android titles create ASTC images whether or not textureCompressionASTC_LDR is advertised (Unity: VMA fails, the
 * game traps); Android's Vulkan baseline (VP_ANDROID_baseline_2021) requires textureCompressionETC2, and Chrome's
 * Skia compositor needs ETC2_R8G8B8_UNORM for its ETC1 tab thumbnails. NVK has no ETC2 on desktop GPUs either
 * (nvk_physical_device.c: textureCompressionETC2 = false).
 *
 * Design: Mesa's common emulation (src/vulkan/runtime/vk_texcompress_astc.c and vk_texcompress_etc2.c, used by
 * anv/radv) and the baseline's vn_astc_emu (patches/mesa/0003, Venus), re-hosted in a pass-through HAL:
 *  - the app's compressed image IS an ICD image of the decoded format (Mesa's vk_texcompress_astc_emulation_format
 *    and vk_texcompress_etc2_emulation_format: RGBA8 UNORM/SRGB for ASTC and ETC2 colour, R16[G16] UNORM/SNORM for
 *    EAC R11[G11]) with STORAGE usage and MUTABLE_FORMAT|EXTENDED_USAGE added. The app's handle is that image, so
 *    sampling, descriptors, blits from it and barriers need no translation; views get the decoded format and a
 *    VkImageViewUsageCreateInfo with the app's own usage;
 *  - beside it the HAL keeps the compressed data: one "block image" per mip level (one texel per block, every
 *    layer: R32G32B32A32_UINT for 128-bit blocks, R32G32_UINT for 64-bit ones), bound into the app's memory behind
 *    the decoded image; the memory requirements the app sees cover all of them. NVHAL_ASTC_BLOCKS_HOST /
 *    debug.nvhal.astc_blocks_host (task-17) moves the block images to one HAL-owned HOST_VISIBLE allocation per
 *    image, so the app's requirements cover the decoded image alone and the card's VRAM only holds those; every other
 *    rule here - barriers, upload/decode, readback, mip/layer layout, refusals - is unchanged, sparse images and any
 *    allocation/bind failure keep the combined path;
 *  - uploads (vkCmdCopyBufferToImage[2], vkCmdCopyImage[2] into it) copy the blocks into the block image and then
 *    run the decoder in the app's command buffer from the block image into an unsigned-integer storage view of the
 *    decoded image, between barriers: Mesa's astc_decoder.glsl (unorm8 mode) for ASTC, src/etc2_decoder.comp for
 *    ETC2/EAC; the app's compute state is then restored (cmdstate.c);
 *  - readbacks (vkCmdCopyImageToBuffer[2], vkCmdCopyImage[2] from it) copy the blocks, so compressed data
 *    round-trips bit-exactly; copies between images of the same decode class copy blocks and decoded texels;
 *  - image barriers naming the app's image are mirrored onto its block images (layouts, queue ownership).
 * Everything outside that is refused loudly, never handed to the ICD: LINEAR tiling, 1D/3D, sparse residency,
 * aliasing, protected, disjoint, multisampled, external memory, block-texel views, storage/attachment/host-transfer
 * usage, HDR/3D ASTC, device-address and indirect copies, uploads on queues without compute. ASTC and ETC2 are
 * decided independently per physical device: each is emulated only where the ICD lacks it and the probe passes. */
#include "nvhal.h"
#include "nvhal_astc.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#define ASTC_FIRST VK_FORMAT_ASTC_4x4_UNORM_BLOCK
#define ASTC_LAST VK_FORMAT_ASTC_12x12_SRGB_BLOCK
#define ETC2_FIRST VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK
#define ETC2_LAST VK_FORMAT_EAC_R11G11_SNORM_BLOCK
#define MAX_MIPS 16
#define R8_TAG "(ASTC gap, R8)"
#define ETC2_TAG "(ETC2 gap)"

/* VkFormat order: 4x4, 5x4, 5x5, 6x5, 6x6, 8x5, 8x6, 8x8, 10x5, 10x6, 10x8, 10x10, 12x10, 12x12, UNORM then
 * SRGB for each; the same order as vk_texcompress_astc.c get_partition_table_index() */
static const uint8_t block_dims[NVHAL_ASTC_NUM_PARTITION_TABLES][2] = {
    {4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

/* ETC2/EAC decoder variants (src/etc2_decoder.comp specialization KIND, SIGNED): RGB8, RGB8A1, RGBA8, R11, signed
 * R11, RG11, signed RG11 */
#define ETC2_VARIANTS 7
static const uint32_t etc2_spec[ETC2_VARIANTS][2] = {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {3, 1}, {4, 0}, {4, 1}};
static const char *const etc2_variant_name[ETC2_VARIANTS] = {"RGB8", "RGB8A1", "RGBA8", "R11", "signed R11", "RG11",
                                                             "signed RG11"};

enum { FAM_ASTC, FAM_ETC2 };
/* an emulated format */
struct efmt {
    uint8_t fam;
    uint8_t bw, bh;
    uint8_t cls;     /* decode class: ASTC: the block-size table (0..13); ETC2: 14 + the decoder variant. Views and
                        texel copies stay within one class (UNORM/SRGB twins decode to the same bytes, EAC UNORM/SNORM
                        do not) */
    uint8_t variant; /* ETC2: decoder variant */
    bool twin;       /* the SRGB (ASTC, ETC2 colour) or SNORM (EAC) member of its pair */
    VkFormat decoded; /* the app's image (vk_texcompress_*_emulation_format) */
    VkFormat store;   /* the decoder's unsigned-integer storage view of it */
    VkFormat block;   /* the block images: one texel per block */
    VkFormat bc1;     /* ASTC 4x4 only: the BC1 image the app gets instead of RGBA8 (VK_FORMAT_UNDEFINED
                         when this format keeps the decoded image) */
};

static bool efmt_get(VkFormat f, struct efmt *e) {
    if (f >= ASTC_FIRST && f <= ASTC_LAST) {
        const unsigned t = (unsigned)(f - ASTC_FIRST) / 2;
        const bool srgb = ((unsigned)(f - ASTC_FIRST) & 1) != 0;
        *e = (struct efmt){FAM_ASTC, block_dims[t][0], block_dims[t][1], (uint8_t)t, 0, srgb,
                           srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT,
                           VK_FORMAT_R32G32B32A32_UINT,
                           /* BC1 is a 4x4 format, so only ASTC 4x4 can map 1:1 */
                           t == 0 ? (srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK)
                                  : VK_FORMAT_UNDEFINED};
        return true;
    }
    if (f >= ETC2_FIRST && f <= ETC2_LAST) {
        /* vk_texcompress_etc2.c vk_texcompress_etc2_emulation_format() */
        static const struct { uint8_t variant; VkFormat decoded, store, block; } t[10] = {
            {0, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R32G32_UINT},
            {0, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R32G32_UINT},
            {1, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R32G32_UINT},
            {1, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R32G32_UINT},
            {2, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R32G32B32A32_UINT},
            {2, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R32G32B32A32_UINT},
            {3, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R32G32_UINT},
            {4, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R32G32_UINT},
            {5, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R32G32B32A32_UINT},
            {6, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R32G32B32A32_UINT},
        };
        const unsigned i = (unsigned)(f - ETC2_FIRST);
        *e = (struct efmt){FAM_ETC2, 4, 4, (uint8_t)(NVHAL_ASTC_NUM_PARTITION_TABLES + t[i].variant), t[i].variant,
                           (i & 1) != 0, t[i].decoded, t[i].store, t[i].block, VK_FORMAT_UNDEFINED};
        return true;
    }
    return false;
}

bool nvhal_astc_format(VkFormat f) { struct efmt e; return efmt_get(f, &e); }
static const char *gap(const struct efmt *e) { return e->fam == FAM_ASTC ? R8_TAG : ETC2_TAG; }
static const char *fam_name(const struct efmt *e) { return e->fam == FAM_ASTC ? "ASTC" : "ETC2/EAC"; }
static uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
static uint32_t mip_dim(uint32_t d, uint32_t m) { return (d >> m) ? (d >> m) : 1; }

/* What textureCompressionASTC_LDR / textureCompressionETC2 make mandatory for every format of the family (spec
 * "Required Format Support"), plus the transfer bits; the decoded format's own features bound it. */
#define ASTC_FEATURES (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | \
                       VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
                       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT)
#define ASTC_FEATURES_REQUIRED (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | \
                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
#define ASTC_USAGE (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
#define ASTC_FLAGS (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | \
                    VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)
/* dEQP-VK.api.info.image_format_properties requires SPARSE_BINDING (+ SPARSE_ALIASED) for a sampled compressed
 * 2D optimal format on a device with sparseBinding (sparseResidencyAliased); isRequiredImageParameterCombination()
 * exempts only SPARSE_RESIDENCY. Opaque binds are split over the parts (nvhal_astc_bind_sparse). */
#define SPARSE_FLAGS (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT)
#define RGBA_EXTRA_FLAGS (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)
#define BLOCK_USAGE (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
#define PUSH_SIZE 20u      /* astc_decoder.glsl: ivec2 texel_blk_start, ivec2 texel_end, bool is_3Dimage */
#define ETC2_PUSH_SIZE 16u /* etc2_decoder.comp: ivec2 start, ivec2 end */

/* ------------------------------------------------------------------ physical device */

#define NEED(cond, ...) do { if (!(cond)) { snprintf(why, sizeof p->why, __VA_ARGS__); return false; } } while (0)

/* ASTC: the decoder's prerequisites (Mesa's astc_decoder.glsl and its LUTs) */
static bool probe_astc(struct nvhal_instance *inst, struct nvhal_astc_pd *p, const VkPhysicalDeviceFeatures *f,
                       const VkPhysicalDeviceLimits *l, char *why) {
    VkPhysicalDevice pd = p->pd;
    VkFormatProperties unorm, srgb, u8, u128;
    inst->GetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_UNORM, &unorm);
    inst->GetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_SRGB, &srgb);
    inst->GetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_UINT, &u8);
    inst->GetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R32G32B32A32_UINT, &u128);
    NEED(u8.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
         "R8G8B8A8_UINT has no optimal-tiling STORAGE_IMAGE feature (0x%x); the decoder writes through it",
         u8.optimalTilingFeatures);
    const VkFormatFeatureFlags bf = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    NEED((u128.optimalTilingFeatures & bf) == bf,
         "R32G32B32A32_UINT optimal-tiling features 0x%x lack SAMPLED_IMAGE|TRANSFER_SRC|TRANSFER_DST (block images)",
         u128.optimalTilingFeatures);
    for (unsigned i = 0; i < NVHAL_ASTC_NUM_LUTS; i++) {
        VkFormatProperties lf;
        inst->GetPhysicalDeviceFormatProperties(pd, nvhal_astc_luts[i].format, &lf);
        NEED(lf.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT,
             "VkFormat %d has no UNIFORM_TEXEL_BUFFER feature (decoder LUT %u)", nvhal_astc_luts[i].format, i);
    }
    NEED(f->shaderStorageImageWriteWithoutFormat,
         "shaderStorageImageWriteWithoutFormat is unsupported (the decoder's output image has no format qualifier)");
    NEED(l->maxComputeWorkGroupInvocations >= 12 * 12 * 4 && l->maxComputeWorkGroupSize[0] >= 12 &&
             l->maxComputeWorkGroupSize[1] >= 12 && l->maxComputeWorkGroupSize[2] >= 4,
         "compute workgroup limits (%u invocations, %ux%ux%u) below the 12x12x4 the decoder needs",
         l->maxComputeWorkGroupInvocations, l->maxComputeWorkGroupSize[0], l->maxComputeWorkGroupSize[1],
         l->maxComputeWorkGroupSize[2]);
    NEED(l->maxTexelBufferElements >= 12 * 32 * 12 * 32, "maxTexelBufferElements %u below the 12x12 partition table",
         l->maxTexelBufferElements);
    p->feat[0] = unorm.optimalTilingFeatures & ASTC_FEATURES;
    p->feat[1] = srgb.optimalTilingFeatures & ASTC_FEATURES;
    NEED((p->feat[0] & ASTC_FEATURES_REQUIRED) == ASTC_FEATURES_REQUIRED &&
             (p->feat[1] & ASTC_FEATURES_REQUIRED) == ASTC_FEATURES_REQUIRED,
         "R8G8B8A8_UNORM/SRGB optimal features 0x%x/0x%x lack SAMPLED_IMAGE|BLIT_SRC|SAMPLED_IMAGE_FILTER_LINEAR, which "
         "textureCompressionASTC_LDR makes mandatory", unorm.optimalTilingFeatures, srgb.optimalTilingFeatures);
    return true;
}

/* ETC2/EAC: the decoder's prerequisites (src/etc2_decoder.comp) and the decoded formats' mandatory features */
static bool probe_etc2(struct nvhal_instance *inst, struct nvhal_astc_pd *p, const VkPhysicalDeviceFeatures *f,
                       const VkPhysicalDeviceLimits *l, char *why) {
    VkPhysicalDevice pd = p->pd;
    static const VkFormat stores[3] = {VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT};
    for (unsigned i = 0; i < 3; i++) {
        VkFormatProperties fp;
        inst->GetPhysicalDeviceFormatProperties(pd, stores[i], &fp);
        NEED(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
             "VkFormat %d has no optimal-tiling STORAGE_IMAGE feature (0x%x); the decoder writes through it", stores[i],
             fp.optimalTilingFeatures);
    }
    static const VkFormat blocks[2] = {VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32A32_UINT};
    const VkFormatFeatureFlags bf = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (unsigned i = 0; i < 2; i++) {
        VkFormatProperties fp;
        inst->GetPhysicalDeviceFormatProperties(pd, blocks[i], &fp);
        NEED((fp.optimalTilingFeatures & bf) == bf,
             "VkFormat %d optimal-tiling features 0x%x lack SAMPLED_IMAGE|TRANSFER_SRC|TRANSFER_DST (block images)",
             blocks[i], fp.optimalTilingFeatures);
    }
    NEED(f->shaderStorageImageWriteWithoutFormat,
         "shaderStorageImageWriteWithoutFormat is unsupported (the decoder's output image has no format qualifier)");
    NEED(l->maxComputeWorkGroupInvocations >= 64 && l->maxComputeWorkGroupSize[0] >= 8 &&
             l->maxComputeWorkGroupSize[1] >= 8,
         "compute workgroup limits (%u invocations, %ux%u) below the 8x8 the decoder needs",
         l->maxComputeWorkGroupInvocations, l->maxComputeWorkGroupSize[0], l->maxComputeWorkGroupSize[1]);
    for (VkFormat fm = ETC2_FIRST; fm <= ETC2_LAST; fm++) {
        struct efmt e;
        efmt_get(fm, &e);
        VkFormatProperties fp;
        inst->GetPhysicalDeviceFormatProperties(pd, e.decoded, &fp);
        p->etc2_feat[fm - ETC2_FIRST] = fp.optimalTilingFeatures & ASTC_FEATURES;
        NEED((p->etc2_feat[fm - ETC2_FIRST] & ASTC_FEATURES_REQUIRED) == ASTC_FEATURES_REQUIRED,
             "VkFormat %d (decoded format of VkFormat %d) optimal features 0x%x lack SAMPLED_IMAGE|BLIT_SRC|"
             "SAMPLED_IMAGE_FILTER_LINEAR, which textureCompressionETC2 makes mandatory", e.decoded, fm,
             fp.optimalTilingFeatures);
    }
    return true;
}
#undef NEED

static void probe(struct nvhal_instance *inst, struct nvhal_astc_pd *p) {
    VkPhysicalDevice pd = p->pd;
    VkPhysicalDeviceFeatures f;
    inst->GetPhysicalDeviceFeatures(pd, &f);
    VkPhysicalDeviceProperties props;
    inst->GetPhysicalDeviceProperties(pd, &props);
    p->api_version = props.apiVersion;
    uint32_t nq = 0;
    inst->GetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qf[16];
    if (nq > 16) nq = 16;
    inst->GetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    bool compute = false;
    p->nq = nq;
    for (uint32_t i = 0; i < nq; i++) {
        p->qflags[i] = qf[i].queueFlags;
        compute |= (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
    }
    p->usage = ASTC_USAGE;
    p->flags = ASTC_FLAGS | (f.sparseBinding ? VK_IMAGE_CREATE_SPARSE_BINDING_BIT : 0) |
               (f.sparseBinding && f.sparseResidencyAliased ? VK_IMAGE_CREATE_SPARSE_ALIASED_BIT : 0);
    p->why[0] = p->etc2_why[0] = 0;
    if (f.textureCompressionASTC_LDR)
        p->state = NVHAL_ASTC_NATIVE;
    else if (!compute)
        p->state = NVHAL_ASTC_UNAVAILABLE, snprintf(p->why, sizeof p->why, "no queue family has VK_QUEUE_COMPUTE_BIT");
    else
        p->state = probe_astc(inst, p, &f, &props.limits, p->why) ? NVHAL_ASTC_EMULATE : NVHAL_ASTC_UNAVAILABLE;
    if (f.textureCompressionETC2)
        p->etc2_state = NVHAL_ASTC_NATIVE;
    else if (!compute)
        p->etc2_state = NVHAL_ASTC_UNAVAILABLE,
        snprintf(p->etc2_why, sizeof p->etc2_why, "no queue family has VK_QUEUE_COMPUTE_BIT");
    else
        p->etc2_state = probe_etc2(inst, p, &f, &props.limits, p->etc2_why) ? NVHAL_ASTC_EMULATE : NVHAL_ASTC_UNAVAILABLE;
}

const struct nvhal_astc_pd *nvhal_astc_pd_get(struct nvhal_instance *inst, VkPhysicalDevice pd) {
    static const struct nvhal_astc_pd oom = {.state = NVHAL_ASTC_UNAVAILABLE, .why = "out of host memory"};
    pthread_mutex_lock(&nvhal_lock);
    for (struct nvhal_astc_pd *p = inst->astc_pd; p; p = p->next)
        if (p->pd == pd) {
            pthread_mutex_unlock(&nvhal_lock);
            return p;
        }
    pthread_mutex_unlock(&nvhal_lock);
    struct nvhal_astc_pd *n = calloc(1, sizeof *n);
    if (!n) {
        NVHAL_ERR("ASTC emulation: out of host memory probing physical device %p", (void *)pd);
        return &oom;
    }
    n->pd = pd;
    probe(inst, n); /* ICD calls: outside the lock */
    pthread_mutex_lock(&nvhal_lock);
    for (struct nvhal_astc_pd *p = inst->astc_pd; p; p = p->next)
        if (p->pd == pd) {
            pthread_mutex_unlock(&nvhal_lock);
            free(n);
            return p;
        }
    n->next = inst->astc_pd;
    inst->astc_pd = n;
    pthread_mutex_unlock(&nvhal_lock);
    static atomic_bool reported;
    if (n->state == NVHAL_ASTC_UNAVAILABLE && !atomic_exchange(&reported, true))
        NVHAL_ERR("ASTC LDR emulation is unavailable on physical device %p: %s. textureCompressionASTC_LDR stays 0 and "
                  "every ASTC image is refused " R8_TAG " (reported once per process)", (void *)pd, n->why);
    else if (n->state == NVHAL_ASTC_EMULATE)
        NVHAL_TRACE("ASTC LDR emulation active on physical device %p (ASTC features UNORM 0x%x SRGB 0x%x)", (void *)pd,
                    n->feat[0], n->feat[1]);
    static atomic_bool etc2_reported;
    if (n->etc2_state == NVHAL_ASTC_UNAVAILABLE && !atomic_exchange(&etc2_reported, true))
        NVHAL_ERR("ETC2/EAC emulation is unavailable on physical device %p: %s. textureCompressionETC2 stays 0 and every "
                  "ETC2/EAC image is refused " ETC2_TAG " (reported once per process)", (void *)pd, n->etc2_why);
    else if (n->etc2_state == NVHAL_ASTC_EMULATE)
        NVHAL_TRACE("ETC2/EAC emulation active on physical device %p", (void *)pd);
    return n;
}

void nvhal_astc_instance_teardown(struct nvhal_instance *inst) {
    for (struct nvhal_astc_pd *p = inst->astc_pd, *nx; p; p = nx) {
        nx = p->next;
        free(p);
    }
    inst->astc_pd = NULL;
}

static enum nvhal_astc_state fam_state(const struct nvhal_astc_pd *p, const struct efmt *e) {
    return e->fam == FAM_ASTC ? p->state : p->etc2_state;
}

/* the physical device's state when it emulates format f's family, else NULL; *instp gets the instance (NULL: unknown
 * physical device). f = VK_FORMAT_UNDEFINED: the state whatever is emulated. */
static const struct nvhal_astc_pd *pd_emulating(VkPhysicalDevice pd, VkFormat f, struct nvhal_instance **instp,
                                                const char *who) {
    struct nvhal_instance *inst = nvhal_physdev_instance(pd);
    *instp = inst;
    if (!inst) {
        NVHAL_ERR("%s(%p): physical device was not enumerated through this HAL", who, (void *)pd);
        return NULL;
    }
    const struct nvhal_astc_pd *p = nvhal_astc_pd_get(inst, pd);
    struct efmt e;
    if (f == VK_FORMAT_UNDEFINED) return p;
    return efmt_get(f, &e) && fam_state(p, &e) == NVHAL_ASTC_EMULATE ? p : NULL;
}

static VkFormatFeatureFlags fmt_features(const struct nvhal_astc_pd *p, VkFormat f) {
    struct efmt e;
    efmt_get(f, &e);
    return e.fam == FAM_ASTC ? p->feat[e.twin] : p->etc2_feat[f - ETC2_FIRST];
}

static void emulated_features(const struct nvhal_astc_pd *p, VkPhysicalDeviceFeatures *f) {
    if (p->state == NVHAL_ASTC_EMULATE) f->textureCompressionASTC_LDR = VK_TRUE;
    if (p->etc2_state == NVHAL_ASTC_EMULATE) f->textureCompressionETC2 = VK_TRUE;
}

void VKAPI_CALL nvhal_GetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) {
    struct nvhal_instance *inst;
    const struct nvhal_astc_pd *p = pd_emulating(pd, VK_FORMAT_UNDEFINED, &inst, "vkGetPhysicalDeviceFeatures");
    if (!inst) {
        memset(f, 0, sizeof *f);
        return;
    }
    inst->GetPhysicalDeviceFeatures(pd, f);
    emulated_features(p, f);
}

static void features2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f, bool khr) {
    struct nvhal_instance *inst;
    const struct nvhal_astc_pd *p = pd_emulating(pd, VK_FORMAT_UNDEFINED, &inst, "vkGetPhysicalDeviceFeatures2");
    if (!inst) return;
    (khr ? inst->GPDF2_khr : inst->GPDF2_core)(pd, f);
    emulated_features(p, &f->features);
}
void VKAPI_CALL nvhal_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) {
    features2(pd, f, false);
}
void VKAPI_CALL nvhal_GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) {
    features2(pd, f, true);
}

void VKAPI_CALL nvhal_GetPhysicalDeviceFormatProperties(VkPhysicalDevice pd, VkFormat format, VkFormatProperties *fp) {
    struct nvhal_instance *inst;
    const struct nvhal_astc_pd *p = NULL;
    if (nvhal_astc_format(format)) {
        p = pd_emulating(pd, format, &inst, "vkGetPhysicalDeviceFormatProperties");
    } else if (!(inst = nvhal_physdev_instance(pd))) {
        NVHAL_ERR("vkGetPhysicalDeviceFormatProperties(%p): physical device was not enumerated through this HAL",
                  (void *)pd);
    }
    if (!inst) {
        memset(fp, 0, sizeof *fp);
        return;
    }
    if (!p) {
        inst->GetPhysicalDeviceFormatProperties(pd, format, fp);
        return;
    }
    *fp = (VkFormatProperties){0, fmt_features(p, format), 0};
}

static void format_props2(VkPhysicalDevice pd, VkFormat format, VkFormatProperties2 *fp, bool khr) {
    struct nvhal_instance *inst;
    const struct nvhal_astc_pd *p = NULL;
    if (nvhal_astc_format(format)) {
        p = pd_emulating(pd, format, &inst, "vkGetPhysicalDeviceFormatProperties2");
    } else if (!(inst = nvhal_physdev_instance(pd))) {
        NVHAL_ERR("vkGetPhysicalDeviceFormatProperties2(%p): physical device was not enumerated through this HAL",
                  (void *)pd);
    }
    if (!inst) return;
    (khr ? inst->GPDFP2_khr : inst->GPDFP2_core)(pd, format, fp); /* chained structs: the ICD's answer (none) */
    if (!p) return;
    VkFormatFeatureFlags feat = fmt_features(p, format);
    fp->formatProperties = (VkFormatProperties){0, feat, 0};
    for (VkBaseOutStructure *s = fp->pNext; s; s = s->pNext) {
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3: {
            VkFormatProperties3 *f3 = (VkFormatProperties3 *)s;
            f3->linearTilingFeatures = 0;
            f3->optimalTilingFeatures = feat; /* the legacy bits have the same values in VkFormatFeatureFlags2 */
            f3->bufferFeatures = 0;
            break;
        }
        case VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT:
            ((VkDrmFormatModifierPropertiesListEXT *)s)->drmFormatModifierCount = 0;
            break;
        case VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT:
            ((VkDrmFormatModifierPropertiesList2EXT *)s)->drmFormatModifierCount = 0;
            break;
        default:
            break;
        }
    }
}
void VKAPI_CALL nvhal_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat f, VkFormatProperties2 *p) {
    format_props2(pd, f, p, false);
}
void VKAPI_CALL nvhal_GetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice pd, VkFormat f, VkFormatProperties2 *p) {
    format_props2(pd, f, p, true);
}

/* what the emulation implements; *why names the first parameter outside it */
static VkResult ifp_check(const struct nvhal_astc_pd *p, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage,
                          VkImageCreateFlags flags, const char **why) {
    *why = type != VK_IMAGE_TYPE_2D ? "only 2D images of an emulated compressed format are emulated"
         : tiling != VK_IMAGE_TILING_OPTIMAL ? "only OPTIMAL tiling is emulated (the texels live decoded in an image "
                                               "of the decoded format; a LINEAR/modifier compressed memory layout "
                                               "cannot be provided)"
         : (usage & ~p->usage) ? "usage beyond SAMPLED|TRANSFER_SRC|TRANSFER_DST"
         : (flags & ~p->flags) ? "create flags beyond MUTABLE_FORMAT|CUBE_COMPATIBLE|EXTENDED_USAGE|SPARSE_BINDING|"
                                 "SPARSE_ALIASED (sparse residency, alias, protected, disjoint and block-texel-view "
                                 "images are not emulated)"
         : (flags & VK_IMAGE_CREATE_SPARSE_ALIASED_BIT) && !(flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT)
             ? "SPARSE_ALIASED without SPARSE_BINDING"
         : NULL;
    return *why ? VK_ERROR_FORMAT_NOT_SUPPORTED : VK_SUCCESS;
}

VkResult VKAPI_CALL nvhal_GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice pd, VkFormat format, VkImageType type,
                                                                VkImageTiling tiling, VkImageUsageFlags usage,
                                                                VkImageCreateFlags flags, VkImageFormatProperties *out) {
    struct nvhal_instance *inst;
    const struct nvhal_astc_pd *p = NULL;
    if (nvhal_astc_format(format)) {
        p = pd_emulating(pd, format, &inst, "vkGetPhysicalDeviceImageFormatProperties");
    } else if (!(inst = nvhal_physdev_instance(pd))) {
        NVHAL_ERR("vkGetPhysicalDeviceImageFormatProperties(%p): physical device was not enumerated through this HAL",
                  (void *)pd);
    }
    if (!inst) return VK_ERROR_INITIALIZATION_FAILED;
    if (!p) return inst->GetPhysicalDeviceImageFormatProperties(pd, format, type, tiling, usage, flags, out);
    struct efmt e;
    efmt_get(format, &e);
    const char *why;
    VkResult r = ifp_check(p, type, tiling, usage, flags, &why);
    if (r == VK_SUCCESS)
        r = inst->GetPhysicalDeviceImageFormatProperties(pd, e.decoded, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                         usage | VK_IMAGE_USAGE_STORAGE_BIT, flags | RGBA_EXTRA_FLAGS, out);
    if (r == VK_SUCCESS) {
        out->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    } else {
        memset(out, 0, sizeof *out);
        NVHAL_TRACE("vkGetPhysicalDeviceImageFormatProperties(%s VkFormat %d, type %d, tiling %d, usage 0x%x, flags "
                    "0x%x) -> %s%s%s", fam_name(&e), format, type, tiling, usage, flags, NVHAL_VKR(r), why ? ": " : "",
                    why ? why : "");
    }
    return r;
}

bool nvhal_astc_gpdifp2(struct nvhal_instance *inst, VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info,
                        VkImageFormatProperties2 *p, PFN_vkGetPhysicalDeviceImageFormatProperties2 f, VkResult *r) {
    struct efmt e;
    if (!efmt_get(info->format, &e)) return false;
    const struct nvhal_astc_pd *ap = nvhal_astc_pd_get(inst, pd);
    if (fam_state(ap, &e) != NVHAL_ASTC_EMULATE) return false;
    const char *why;
    char buf[96];
    *r = ifp_check(ap, info->type, info->tiling, info->usage, info->flags, &why);
    for (const VkBaseInStructure *s = info->pNext; s && *r == VK_SUCCESS; s = s->pNext) {
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO:
            if (((const VkPhysicalDeviceExternalImageFormatInfo *)s)->handleType) {
                why = "external memory is not emulated for compressed-format images";
                *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
            break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: {
            const VkImageFormatListCreateInfo *fl = (const void *)s;
            for (uint32_t i = 0; i < fl->viewFormatCount; i++) {
                struct efmt v;
                if (!efmt_get(fl->pViewFormats[i], &v) || v.cls != e.cls) {
                    why = "a view format outside the image's decode class (block-texel views and EAC UNORM/SNORM "
                          "reinterpretation are not emulated)";
                    *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
                }
            }
            break;
        }
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO:
            break;
        default:
            snprintf(buf, sizeof buf, "input structure sType %d is not answered for an emulated compressed format",
                     (int)s->sType);
            why = buf;
            *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
            break;
        }
    }
    if (*r == VK_SUCCESS) {
        /* what vkCreateImage will ask the ICD for; the app's output chain is answered for that image */
        VkPhysicalDeviceImageFormatInfo2 q = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, NULL,
                                              e.decoded, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                              info->usage | VK_IMAGE_USAGE_STORAGE_BIT, info->flags | RGBA_EXTRA_FLAGS};
        *r = f(pd, &q, p);
        why = NULL;
    }
    if (*r == VK_SUCCESS) {
        p->imageFormatProperties.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
        for (VkBaseOutStructure *s = p->pNext; s; s = s->pNext)
            if (s->sType == VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES)
                memset(&((VkExternalImageFormatProperties *)s)->externalMemoryProperties, 0,
                       sizeof(VkExternalMemoryProperties));
    } else {
        memset(&p->imageFormatProperties, 0, sizeof p->imageFormatProperties);
        NVHAL_TRACE("vkGetPhysicalDeviceImageFormatProperties2(%s VkFormat %d, type %d, tiling %d, usage 0x%x, flags "
                    "0x%x) -> %s%s%s", fam_name(&e), info->format, info->type, info->tiling, info->usage, info->flags, NVHAL_VKR(*r),
                    why ? ": " : "", why ? why : "");
    }
    return true;
}

/* ------------------------------------------------------------------ device */

struct nvhal_astc_dev {
    pthread_mutex_t lock;
    int init; /* 0 not tried, 1 ready, -1 failed (logged) */
    VkDeviceSize texel_align;
    VkBuffer lut_buf;
    VkDeviceMemory lut_mem;
    VkBufferView lut_view[NVHAL_ASTC_NUM_LUTS], part_view[NVHAL_ASTC_NUM_PARTITION_TABLES];
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkShaderModule sm;
    VkPipeline pipe[NVHAL_ASTC_NUM_PARTITION_TABLES];
    /* ASTC->BC1 encoder (src/bc1_encode.comp); created lazily, see bc1_encoder() */
    int bc1_init; /* 0 not tried, 1 ready, -1 failed */
    VkDescriptorSetLayout bc1_dsl;
    VkPipelineLayout bc1_pl;
    VkShaderModule bc1_sm;
    VkPipeline bc1_pipe;
    VkDescriptorPool bc1_pool;
    bool pipe_failed[NVHAL_ASTC_NUM_PARTITION_TABLES];
    VkDescriptorPool *pools;
    uint32_t npools;
    /* the ETC2/EAC decoder (src/etc2_decoder.comp): no LUTs, set 0 = storage view + sampled block view */
    int etc2_init; /* 0 not tried, 1 ready, -1 failed (logged) */
    VkDescriptorSetLayout etc2_dsl;
    VkPipelineLayout etc2_pl;
    VkShaderModule etc2_sm;
    VkPipeline etc2_pipe[ETC2_VARIANTS];
    bool etc2_pipe_failed[ETC2_VARIANTS];
    VkDescriptorPool *etc2_pools;
    uint32_t etc2_npools;
};

VkResult nvhal_astc_device_ci(struct nvhal_instance *inst, VkPhysicalDevice pd, uint32_t api, const VkDeviceCreateInfo *ci,
                              VkDeviceCreateInfo *out, struct nvhal_astc_ci_storage *st, bool *emulate, bool *maint2) {
    *out = *ci;
    *emulate = *maint2 = false;
    const struct nvhal_astc_pd *p = nvhal_astc_pd_get(inst, pd);
    const bool astc = p->state == NVHAL_ASTC_EMULATE, etc2 = p->etc2_state == NVHAL_ASTC_EMULATE;
    if (!astc && !etc2) return VK_SUCCESS;
    *emulate = true;
    *maint2 = api < VK_API_VERSION_1_1; /* VkImageViewUsageCreateInfo + EXTENDED_USAGE on a 1.0 device */
    const VkPhysicalDeviceFeatures2 *f2 = NULL;
    for (const VkBaseInStructure *s = ci->pNext; s && !f2; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) f2 = (const void *)s;
    if (ci->pEnabledFeatures || !f2) {
        if (ci->pEnabledFeatures) st->feats = *ci->pEnabledFeatures;
        else memset(&st->feats, 0, sizeof st->feats);
        if ((astc && st->feats.textureCompressionASTC_LDR) || (etc2 && st->feats.textureCompressionETC2))
            NVHAL_TRACE("vkCreateDevice: textureCompressionASTC_LDR/ETC2 are emulated by the HAL; not passed to the ICD");
        if (astc) st->feats.textureCompressionASTC_LDR = VK_FALSE;
        if (etc2) st->feats.textureCompressionETC2 = VK_FALSE;
        st->feats.shaderStorageImageWriteWithoutFormat = VK_TRUE; /* the decoders' output images */
        out->pEnabledFeatures = &st->feats;
        return VK_SUCCESS;
    }
    st->f2 = *f2;
    if (astc) st->f2.features.textureCompressionASTC_LDR = VK_FALSE;
    if (etc2) st->f2.features.textureCompressionETC2 = VK_FALSE;
    st->f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    if (!nvhal_chain_replace(ci->pNext, f2, &st->f2, (const void **)&st->f2.pNext, st->chain, sizeof st->chain,
                             "vkCreateDevice(compressed-format emulation features)", &out->pNext))
        return VK_ERROR_INITIALIZATION_FAILED;
    return VK_SUCCESS;
}

VkResult nvhal_astc_device_init(struct nvhal_device *d) {
    const char *missing = NULL;
#define REQ(type, field, name) if (!d->cs.field && !missing) missing = name;
    NVHAL_ASTC_FNS(REQ)
#undef REQ
    if (!missing) {
        missing = !d->cs.BeginCommandBuffer ? "vkBeginCommandBuffer"
                : !d->cs.CreateCommandPool ? "vkCreateCommandPool"
                : !d->cs.CmdBindPipeline ? "vkCmdBindPipeline"
                : !d->cs.CmdBindDescriptorSets ? "vkCmdBindDescriptorSets"
                : !d->cs.CmdPushConstants ? "vkCmdPushConstants"
                : !d->xl.CmdPipelineBarrier ? "vkCmdPipelineBarrier"
                : !d->xl.CmdCopyBufferToImage ? "vkCmdCopyBufferToImage"
                : !d->xl.CmdCopyImage ? "vkCmdCopyImage"
                : !d->xl.CmdCopyImageToBuffer ? "vkCmdCopyImageToBuffer"
                : !d->xl.GetImageMemoryRequirements ? "vkGetImageMemoryRequirements" : NULL;
    }
    if (missing) {
        NVHAL_ERR("vkCreateDevice: the ICD returned NULL for %s, which the compressed-format emulation needs -> "
                  "VK_ERROR_INITIALIZATION_FAILED", missing);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct nvhal_astc_dev *a = calloc(1, sizeof *a);
    if (!a) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pthread_mutex_init(&a->lock, NULL);
    VkPhysicalDeviceProperties props;
    d->inst->GetPhysicalDeviceProperties(d->physical_device, &props);
    a->texel_align = props.limits.minTexelBufferOffsetAlignment ? props.limits.minTexelBufferOffsetAlignment : 1;
    d->astc = a;
    return VK_SUCCESS;
}

/* defined next to combine(), inside the block test/astc-blocks-host.c extracts */
static VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a);

static int32_t mem_type(const struct nvhal_device *d, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < d->mem_props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (d->mem_props.memoryTypes[i].propertyFlags & want) == want) return (int32_t)i;
    return -1;
}

/* vk_texcompress_astc_init() + the shader module: the LUT buffer and its views, the set and pipeline layouts.
 * Caller holds a->lock. */
static VkResult decoder_init(struct nvhal_device *d) {
    struct nvhal_astc_dev *a = d->astc;
    if (a->init) return a->init > 0 ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    a->init = -1;
    VkDevice dev = d->handle;
    VkDeviceSize off[NVHAL_ASTC_NUM_LUTS + NVHAL_ASTC_NUM_PARTITION_TABLES], size = 0;
    const struct nvhal_astc_lut *lut[NVHAL_ASTC_NUM_LUTS + NVHAL_ASTC_NUM_PARTITION_TABLES];
    for (unsigned i = 0; i < NVHAL_ASTC_NUM_LUTS; i++) lut[i] = &nvhal_astc_luts[i];
    for (unsigned i = 0; i < NVHAL_ASTC_NUM_PARTITION_TABLES; i++) lut[NVHAL_ASTC_NUM_LUTS + i] = &nvhal_astc_partition_tables[i];
    const unsigned nl = NVHAL_ASTC_NUM_LUTS + NVHAL_ASTC_NUM_PARTITION_TABLES;
    for (unsigned i = 0; i < nl; i++) {
        size = align_up(size, a->texel_align);
        off[i] = size;
        size += lut[i]->size;
    }
    const char *step = "vkCreateBuffer(LUTs)";
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
                              VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkResult r = d->xl.CreateBuffer(dev, &bci, NULL, &a->lut_buf);
    VkMemoryRequirements mr = {0};
    int32_t type = -1;
    if (r == VK_SUCCESS) {
        d->cs.GetBufferMemoryRequirements(dev, a->lut_buf, &mr);
        type = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        step = "a HOST_VISIBLE|HOST_COHERENT memory type for the LUT buffer";
        if (type < 0) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (r == VK_SUCCESS) {
        step = "vkAllocateMemory(LUTs)";
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)type};
        r = d->AllocateMemory(dev, &mai, NULL, &a->lut_mem);
    }
    if (r == VK_SUCCESS) {
        step = "vkBindBufferMemory(LUTs)";
        r = d->cs.BindBufferMemory(dev, a->lut_buf, a->lut_mem, 0);
    }
    void *map = NULL;
    if (r == VK_SUCCESS) {
        step = "vkMapMemory(LUTs)";
        r = d->cs.MapMemory(dev, a->lut_mem, 0, VK_WHOLE_SIZE, 0, &map);
    }
    if (r == VK_SUCCESS) {
        for (unsigned i = 0; i < nl; i++) memcpy((unsigned char *)map + off[i], lut[i]->data, lut[i]->size);
        d->cs.UnmapMemory(dev, a->lut_mem);
        step = "vkCreateBufferView(LUTs)";
        for (unsigned i = 0; i < nl && r == VK_SUCCESS; i++) {
            VkBufferViewCreateInfo v = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, a->lut_buf, lut[i]->format,
                                        off[i], lut[i]->size};
            r = d->cs.CreateBufferView(dev, &v, NULL,
                                       i < NVHAL_ASTC_NUM_LUTS ? &a->lut_view[i] : &a->part_view[i - NVHAL_ASTC_NUM_LUTS]);
        }
    }
    if (r == VK_SUCCESS) {
        /* vk_texcompress_astc.c create_layout(), without the push-descriptor flag: the HAL allocates sets */
        VkDescriptorSetLayoutBinding b[8];
        for (uint32_t i = 0; i < 8; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                     : i == 1 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                              : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                                  1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 8, b};
        step = "vkCreateDescriptorSetLayout";
        r = d->cs.CreateDescriptorSetLayout(dev, &dci, NULL, &a->dsl);
    }
    if (r == VK_SUCCESS) {
        VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, PUSH_SIZE};
        VkPipelineLayoutCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &a->dsl, 1, &pcr};
        step = "vkCreatePipelineLayout";
        r = d->cs.CreatePipelineLayout(dev, &pci, NULL, &a->pl);
    }
    if (r == VK_SUCCESS) {
        VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, nvhal_astc_spv_size,
                                        nvhal_astc_spv};
        step = "vkCreateShaderModule(astc_decoder.glsl)";
        r = d->cs.CreateShaderModule(dev, &smi, NULL, &a->sm);
    }
    if (r != VK_SUCCESS) {
        NVHAL_ERR("ASTC emulation: decoder setup failed at %s -> %s; ASTC uploads on device %p stay undecoded " R8_TAG,
                  step, NVHAL_VKR(r), (void *)dev);
        return r;
    }
    NVHAL_TRACE("ASTC emulation: decoder ready on device %p (LUT buffer %llu B, texel alignment %llu)", (void *)dev,
                (unsigned long long)size, (unsigned long long)a->texel_align);
    a->init = 1;
    return VK_SUCCESS;
}

/* the ASTC->BC1 encoder (src/bc1_encode.comp). Caller holds a->lock.
 * set 0: binding 0 = the decoder's RGBA8 tmp image (storage), binding 1 = the BC1 block buffer (SSBO);
 * push constants: block_start, block_end, blocks_per_row. */
static VkPipeline bc1_encoder(struct nvhal_device *d) {
    struct nvhal_astc_dev *a = d->astc;
    if (a->bc1_init) return a->bc1_init > 0 ? a->bc1_pipe : VK_NULL_HANDLE;
    a->bc1_init = -1;
    VkDevice dev = d->handle;
    const char *step = "vkCreateDescriptorSetLayout(BC1 encoder)";
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b};
    VkResult r = d->cs.CreateDescriptorSetLayout(dev, &dci, NULL, &a->bc1_dsl);
    if (r == VK_SUCCESS) {
        step = "vkCreatePipelineLayout(BC1 encoder)";
        VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 20};
        VkPipelineLayoutCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &a->bc1_dsl, 1, &pcr};
        r = d->cs.CreatePipelineLayout(dev, &pci, NULL, &a->bc1_pl);
    }
    if (r == VK_SUCCESS) {
        step = "vkCreateShaderModule(bc1_encode.comp)";
        VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, nvhal_bc1_encode_spv_size,
                                        nvhal_bc1_encode_spv};
        r = d->cs.CreateShaderModule(dev, &smi, NULL, &a->bc1_sm);
    }
    if (r == VK_SUCCESS) {
        step = "vkCreateComputePipelines(BC1 encoder)";
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, a->bc1_sm,
             "main", NULL}, a->bc1_pl, VK_NULL_HANDLE, -1};
        r = d->cs.CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &a->bc1_pipe);
    }
    if (r == VK_SUCCESS) {
        step = "vkCreateDescriptorPool(BC1 encoder)";
        VkDescriptorPoolSize sz[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8}};
        VkDescriptorPoolCreateInfo pci2 = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
                                           VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, 8, 2, sz};
        r = d->cs.CreateDescriptorPool(dev, &pci2, NULL, &a->bc1_pool);
    }
    if (r != VK_SUCCESS) {
        NVHAL_ERR("ASTC->BC1: encoder setup failed at %s -> %s; ASTC 4x4 uploads stay RGBA8 %s", step, NVHAL_VKR(r),
                  R8_TAG);
        return VK_NULL_HANDLE;
    }
    a->bc1_init = 1;
    return a->bc1_pipe;
}

/* vk_texcompress_astc.c create_astc_decode_pipeline(): specialization = block size, DECODE_8BIT = true.
 * Caller holds a->lock. */
static VkPipeline decoder_pipeline(struct nvhal_device *d, unsigned t) {
    struct nvhal_astc_dev *a = d->astc;
    if (a->pipe[t] || a->pipe_failed[t]) return a->pipe[t];
    uint32_t spec[3] = {block_dims[t][0], block_dims[t][1], 1};
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo si = {3, me, sizeof spec, spec};
    VkComputePipelineCreateInfo ci = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, a->sm, "main", &si},
        a->pl, VK_NULL_HANDLE, -1};
    VkResult r = d->cs.CreateComputePipelines(d->handle, VK_NULL_HANDLE, 1, &ci, NULL, &a->pipe[t]);
    if (r != VK_SUCCESS) {
        a->pipe[t] = VK_NULL_HANDLE;
        a->pipe_failed[t] = true;
        NVHAL_ERR("ASTC emulation: vkCreateComputePipelines(decoder %ux%u) -> %s; uploads of that block size stay "
                  "undecoded " R8_TAG, block_dims[t][0], block_dims[t][1], NVHAL_VKR(r));
    }
    return a->pipe[t];
}

/* the ETC2/EAC decoder's set and pipeline layouts and shader module. Caller holds a->lock. */
static VkResult etc2_decoder_init(struct nvhal_device *d) {
    struct nvhal_astc_dev *a = d->astc;
    if (a->etc2_init) return a->etc2_init > 0 ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    a->etc2_init = -1;
    VkDevice dev = d->handle;
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b};
    const char *step = "vkCreateDescriptorSetLayout";
    VkResult r = d->cs.CreateDescriptorSetLayout(dev, &dci, NULL, &a->etc2_dsl);
    if (r == VK_SUCCESS) {
        VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, ETC2_PUSH_SIZE};
        VkPipelineLayoutCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &a->etc2_dsl, 1, &pcr};
        step = "vkCreatePipelineLayout";
        r = d->cs.CreatePipelineLayout(dev, &pci, NULL, &a->etc2_pl);
    }
    if (r == VK_SUCCESS) {
        VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, nvhal_etc2_spv_size,
                                        nvhal_etc2_spv};
        step = "vkCreateShaderModule(etc2_decoder.comp)";
        r = d->cs.CreateShaderModule(dev, &smi, NULL, &a->etc2_sm);
    }
    if (r != VK_SUCCESS) {
        NVHAL_ERR("ETC2/EAC emulation: decoder setup failed at %s -> %s; ETC2/EAC uploads on device %p stay undecoded "
                  ETC2_TAG, step, NVHAL_VKR(r), (void *)dev);
        return r;
    }
    NVHAL_TRACE("ETC2/EAC emulation: decoder ready on device %p", (void *)dev);
    a->etc2_init = 1;
    return VK_SUCCESS;
}

/* specialization = the variant's KIND and SIGNED constants. Caller holds a->lock. */
static VkPipeline etc2_pipeline(struct nvhal_device *d, unsigned v) {
    struct nvhal_astc_dev *a = d->astc;
    if (a->etc2_pipe[v] || a->etc2_pipe_failed[v]) return a->etc2_pipe[v];
    const uint32_t spec[2] = {etc2_spec[v][0], etc2_spec[v][1]}; /* SIGNED is a bool: 32-bit 0/1 */
    VkSpecializationMapEntry me[2] = {{0, 0, 4}, {1, 4, 4}};
    VkSpecializationInfo si = {2, me, sizeof spec, spec};
    VkComputePipelineCreateInfo ci = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, a->etc2_sm, "main", &si},
        a->etc2_pl, VK_NULL_HANDLE, -1};
    VkResult r = d->cs.CreateComputePipelines(d->handle, VK_NULL_HANDLE, 1, &ci, NULL, &a->etc2_pipe[v]);
    if (r != VK_SUCCESS) {
        a->etc2_pipe[v] = VK_NULL_HANDLE;
        a->etc2_pipe_failed[v] = true;
        NVHAL_ERR("ETC2/EAC emulation: vkCreateComputePipelines(decoder %s) -> %s; uploads of that format stay "
                  "undecoded " ETC2_TAG, etc2_variant_name[v], NVHAL_VKR(r));
    }
    return a->etc2_pipe[v];
}

/* one descriptor set of the family's decoder from the HAL's pools. Caller holds a->lock. */
static VkResult set_alloc(struct nvhal_device *d, unsigned fam, VkDescriptorSet *set, VkDescriptorPool *pool) {
    struct nvhal_astc_dev *a = d->astc;
    VkDescriptorPool **pools = fam == FAM_ASTC ? &a->pools : &a->etc2_pools;
    uint32_t *npools = fam == FAM_ASTC ? &a->npools : &a->etc2_npools;
    VkDescriptorSetLayout *dsl = fam == FAM_ASTC ? &a->dsl : &a->etc2_dsl;
    for (uint32_t i = *npools; i-- > 0;) {
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, (*pools)[i], 1, dsl};
        VkResult r = d->cs.AllocateDescriptorSets(d->handle, &ai, set);
        if (r == VK_SUCCESS) {
            *pool = (*pools)[i];
            return r;
        }
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) return r;
    }
    enum { SETS = 64 };
    VkDescriptorPoolSize sz[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SETS},
                                  {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, SETS},
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 6 * SETS}};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
                                      VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, SETS, fam == FAM_ASTC ? 3 : 2,
                                      sz};
    VkDescriptorPool *np = realloc(*pools, (*npools + 1) * sizeof *np);
    if (!np) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *pools = np;
    VkResult r = d->cs.CreateDescriptorPool(d->handle, &pci, NULL, &np[*npools]);
    if (r != VK_SUCCESS) return r;
    *pool = np[(*npools)++];
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, *pool, 1, dsl};
    return d->cs.AllocateDescriptorSets(d->handle, &ai, set);
}

/* ------------------------------------------------------------------ images */

/* the decoder's inputs for one (mip level, layer range) of an image, created once, freed with the image */
struct astc_slot {
    uint32_t mip, base, count;
    VkImageView src, dst;
    VkDescriptorSet set;
    VkDescriptorPool pool;
};

struct nvhal_astc_image {
    struct nvhal_device *dev;
    VkImage image;          /* the app's handle = the ICD's image of the decoded format */
    VkFormat format;        /* the app's compressed format */
    struct efmt e;
    uint32_t bw, bh, table; /* e.bw, e.bh, e.cls */
    VkExtent3D extent;
    uint32_t mips, layers;
    VkImageUsageFlags usage; /* the app's */
    VkImage block[MAX_MIPS];
    VkDeviceSize off[1 + MAX_MIPS]; /* binding offsets: [0] RGBA image, [1 + m] block image of mip m; the block
                                       entries are 0 when the HAL owns their memory (they are already bound) */
    VkDeviceSize size[1 + MAX_MIPS]; /* each part's own memory size */
    bool sparse;                    /* SPARSE_BINDING: bound with vkQueueBindSparse */
    bool blocks_host;               /* task-17: the block images live in blocks_mem, not in the app's memory */
    bool counted;                   /* bh_live_add() ran: the summary's live figures include this image */
    VkDeviceMemory blocks_mem;      /* the HAL's own HOST_VISIBLE allocation behind the block images */
    VkDeviceSize blocks_size;       /* its size (blocks_host only) */
    uint32_t blocks_type;           /* its memoryTypeIndex (blocks_host only) */
    bool decoded_host;              /* EXPERIMENT: the decoded image lives in decoded_mem (host memory) */
    VkDeviceMemory decoded_mem;     /* the HAL's own allocation behind the decoded image (decoded_host only) */
    bool bc1;                       /* the app's image is a BC1 image; decode goes through tmp + bc1_buf */
    VkImage tmp;                    /* RGBA8 storage image the ASTC decoder writes into (bc1 only) */
    VkDeviceMemory tmp_mem;         /* its memory (HAL-owned, host) */
    VkImageView tmp_view;           /* storage view of tmp */
    VkBuffer bc1_buf;               /* the BC1 blocks the encoder writes (bc1 only) */
    VkDeviceMemory bc1_mem;         /* its memory (HAL-owned, host) */
    VkDeviceSize bc1_blocks;        /* block count across all mips and layers */
    VkMemoryRequirements req;       /* what the app is told */
    struct astc_slot *slots;        /* d->astc->lock */
    uint32_t nslots, cap;
};

static struct nvhal_hash images = NVHAL_HASH_INIT; /* RGBA VkImage -> struct nvhal_astc_image */
static atomic_size_t n_images;

bool nvhal_no_astc(void) { return atomic_load_explicit(&n_images, memory_order_acquire) == 0; }

static struct nvhal_astc_image *lookup(VkImage image) {
    if (image == VK_NULL_HANDLE || nvhal_no_astc()) return NULL;
    return nvhal_hash_get(&images, NVHAL_ND_U64(image));
}

bool nvhal_astc_is_image(VkImage image) { return lookup(image) != NULL; }

static bool bc1_on(void);   /* the switch is defined further down */

struct astc_cis {
    bool bc1;                     /* the app's image is BC1; rgba2 is the decoder's RGBA8 target */
    VkImageCreateInfo rgba;
    VkImageCreateInfo rgba2;
    VkImageFormatListCreateInfo list;
    VkFormat list_fmts[3];
    struct efmt e;
    VkImageCreateInfo block[MAX_MIPS];
    uint32_t mips;
};

/* the ICD create infos behind an app create info of an emulated compressed format; refusals are logged and
 * VK_ERROR_FORMAT_NOT_SUPPORTED */
static VkResult build_cis(const struct nvhal_astc_pd *p, const VkImageCreateInfo *ci, struct astc_cis *c,
                          const char *who) {
    const char *why;
    char buf[160];
    efmt_get(ci->format, &c->e);
    const struct efmt *e = &c->e;
    VkResult r = ifp_check(p, ci->imageType, ci->tiling, ci->usage, ci->flags, &why);
    if (r == VK_SUCCESS) {
        why = ci->samples != VK_SAMPLE_COUNT_1_BIT ? "multisampled"
            : ci->mipLevels == 0 || ci->mipLevels > MAX_MIPS ? "mip level count out of range"
            : ci->extent.depth != 1 ? "depth != 1"
            : NULL;
    }
    bool list = false, plain = false, twin = false;
    for (const VkBaseInStructure *s = ci->pNext; s && !why; s = s->pNext) {
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: {
            const VkImageFormatListCreateInfo *fl = (const void *)s;
            for (uint32_t i = 0; i < fl->viewFormatCount && !why; i++) {
                VkFormat v = fl->pViewFormats[i];
                struct efmt ve;
                if (!efmt_get(v, &ve) || ve.cls != e->cls) {
                    snprintf(buf, sizeof buf, "view format %d in VkImageFormatListCreateInfo (block-texel views and EAC "
                             "UNORM/SNORM reinterpretation are not emulated)", v);
                    why = buf;
                    break;
                }
                plain |= !ve.twin;
                twin |= ve.twin;
            }
            list |= fl->viewFormatCount > 0;
            break;
        }
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO:
            if (((const VkExternalMemoryImageCreateInfo *)s)->handleTypes) why = "external memory handle types";
            break;
        case VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID:
            if (((const VkExternalFormatANDROID *)s)->externalFormat) why = "an Android external format";
            break;
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO:
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_IMAGE_CREATE_INFO_NV:
            break; /* nothing for a color image / a dedication hint the emulation cannot keep */
        default:
            snprintf(buf, sizeof buf, "pNext structure sType %d", (int)s->sType);
            why = buf;
            break;
        }
    }
    if (why) {
        NVHAL_ERR("%s: VkFormat %d (%s %ux%u blocks) %ux%u, %u mip(s), %u layer(s), type %d, tiling %d, samples %d, usage "
                  "0x%x, flags 0x%x: not emulated: %s %s -> VK_ERROR_FORMAT_NOT_SUPPORTED", who, ci->format, fam_name(e),
                  e->bw, e->bh, ci->extent.width, ci->extent.height, ci->mipLevels, ci->arrayLayers, ci->imageType,
                  ci->tiling, ci->samples, ci->usage, ci->flags, why, gap(e));
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    /* ASTC 4x4 -> BC1: the app's image is a BC1 image (no storage usage: BC1 cannot be a storage image),
     * and the decoder writes into a separate HAL-owned RGBA8 storage image instead. */
    c->bc1 = bc1_on() && e->bc1 != VK_FORMAT_UNDEFINED;
    c->rgba2 = c->rgba;   /* the decoder's own RGBA8 target (bc1 only) */
    c->rgba = *ci;
    c->rgba.pNext = NULL;
    c->rgba.format = c->bc1 ? e->bc1 : e->decoded;
    if (c->bc1) {
        c->rgba.usage = (ci->usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT) & ~VK_IMAGE_USAGE_STORAGE_BIT;
        c->rgba.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        c->rgba2 = *ci;
        c->rgba2.pNext = NULL;
        c->rgba2.format = e->decoded;
        c->rgba2.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        c->rgba2.flags |= RGBA_EXTRA_FLAGS;
    } else {
        c->rgba.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        c->rgba.flags |= RGBA_EXTRA_FLAGS;
    }
    if (list) { /* the app's view formats, as decoded formats, plus the decoder's storage view */
        struct efmt pe, te;
        uint32_t n = 0;
        efmt_get(e->twin ? ci->format - 1 : ci->format, &pe); /* the pair: UNORM/SRGB or UNORM/SNORM, adjacent */
        efmt_get(e->twin ? ci->format : ci->format + 1, &te);
        if (plain) c->list_fmts[n++] = pe.decoded;
        if (twin) c->list_fmts[n++] = te.decoded;
        c->list_fmts[n++] = e->store;
        c->list = (VkImageFormatListCreateInfo){VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, n, c->list_fmts};
        c->rgba.pNext = &c->list;
    }
    c->mips = ci->mipLevels;
    for (uint32_t m = 0; m < c->mips; m++)
        c->block[m] = (VkImageCreateInfo){
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, ci->flags & SPARSE_FLAGS, VK_IMAGE_TYPE_2D, e->block,
            {div_up(mip_dim(ci->extent.width, m), e->bw), div_up(mip_dim(ci->extent.height, m), e->bh), 1},
            1, ci->arrayLayers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, BLOCK_USAGE, ci->sharingMode,
            ci->queueFamilyIndexCount, ci->pQueueFamilyIndices, ci->initialLayout};
    return VK_SUCCESS;
}

/* NVHAL_ASTC_BLOCK_HOST_BEGIN: 'make astc-blocks-host' copies everything from this line to the matching END line into
 * out/gen/astc_blocks_host.inc, which test/astc-blocks-host.c compiles and runs on a host with no GPU (the same trick
 * as the task-12 memory-type mirror test). The block must therefore stay free of HAL globals and of logging. */
static VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

/* lay the parts out one after another in one allocation; false when no memory type fits all of them. sparse:
 * every part starts on the largest alignment (the app's sparse block size), so a bind range the app aligns to it
 * maps onto whole sparse blocks of each part */
static bool combine(const VkMemoryRequirements *parts, uint32_t n, bool sparse, VkDeviceSize *off,
                    VkMemoryRequirements *out) {
    VkDeviceSize end = 0, align = 1;
    uint32_t bits = ~0u;
    for (uint32_t i = 0; i < n; i++)
        if (parts[i].alignment > align) align = parts[i].alignment;
    for (uint32_t i = 0; i < n; i++) {
        VkDeviceSize a = sparse ? align : parts[i].alignment ? parts[i].alignment : 1;
        end = align_up(end, a);
        off[i] = end;
        end += parts[i].size;
        if (a > align) align = a;
        bits &= parts[i].memoryTypeBits;
    }
    *out = (VkMemoryRequirements){sparse ? align_up(end, align) : end, align, bits};
    return bits != 0;
}

/* ------------------------------------- compressed blocks in HAL-owned host memory (task-17) ----------------------
 *
 * Normally the block images are bound behind the decoded image inside the app's allocation, so the memory
 * requirements the app is given cover both (4 B + block bytes per texel). That is what makes this Pascal card run
 * out: 588 ASTC textures of 3DMark Steel Nomad Light want 5.90 GiB decoded + 1.22 GiB of blocks, DEVICE_LOCAL
 * allocations fail 119 times and 1.35 GiB spill into host memory. NVHAL_ASTC_BLOCKS_HOST / debug.nvhal.astc_blocks_host
 * = 1 keeps the block images in one HAL-owned allocation per image instead, in HOST_VISIBLE system memory, and tells
 * the app only what its decoded image needs. Everything else stays: the block images still follow the app image's
 * barriers, the upload still copies the blocks into them and decodes from them, vkCmdCopyImageToBuffer still reads the
 * compressed bits back out of them, and every refusal is unchanged. Sparse images and any allocation/bind failure keep
 * the combined path. */
#define BH_MAX_TYPES 32u /* VK_MAX_MEMORY_TYPES */

struct bh_plan {
    bool host;                      /* the HAL owns the block images' memory */
    uint32_t type;                  /* that memory's type index */
    VkDeviceSize size;              /* its size in bytes */
    VkDeviceSize off[1 + MAX_MIPS]; /* [1 + m]: block image m's offset in it; [0] stays 0 */
};

/* the memory type for the block images. Order of preference:
 *  1. HOST_VISIBLE|HOST_COHERENT and *not* DEVICE_LOCAL: system RAM (on this driver the VRAM type is
 *     HOST_VISIBLE|HOST_COHERENT too, and a block image in it would save nothing);
 *  2. any HOST_VISIBLE|HOST_COHERENT, 3. any HOST_VISIBLE;
 *  4. any *system* type (not DEVICE_LOCAL). This tier matters: a block image is an R32G32B32A32_UINT /
 *     R32G32_UINT OPTIMAL image, and NVIDIA's ICD reports typeBits 0x82 for those, i.e. only type 1 and type 7.
 *     Type 1 is heap 1 (host memory) with flags 0x0 -- it has no HOST_VISIBLE bit because the driver never intends
 *     it to be mapped, yet it is host memory and the block image is only ever touched by copies and the decoder
 *     dispatch, never by the CPU. Without tier 4 the whole switch silently does nothing on this driver.
 * -1 when memoryTypeBits offers none (the caller keeps the combined path). */
static int32_t bh_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits) {
    static const VkMemoryPropertyFlags want[3] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT};
    for (unsigned k = 0; k < 3; k++)
        for (uint32_t i = 0; i < mp->memoryTypeCount && i < BH_MAX_TYPES; i++) {
            const VkMemoryPropertyFlags f = mp->memoryTypes[i].propertyFlags;
            if (!(bits & (1u << i)) || (f & want[k]) != want[k]) continue;
            if (k == 0 && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
            return (int32_t)i;
        }
    /* tier 4: host memory the driver does not mark HOST_VISIBLE (see the comment above) */
    for (uint32_t i = 0; i < mp->memoryTypeCount && i < BH_MAX_TYPES; i++) {
        const VkMemoryPropertyFlags f = mp->memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i)) || (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
        if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) continue;
        return (int32_t)i;
    }
    return -1;
}

/* what the app is told: the decoded image alone in host mode, every part combined otherwise. off[0] is where the
 * decoded image starts in the app's memory; the block images' off[] entries stay 0 in host mode (they are already
 * bound there, so nothing binds them again). */
static bool bh_requirements(const VkMemoryRequirements *parts, uint32_t n, bool host, bool sparse, VkDeviceSize *off,
                            VkMemoryRequirements *out) {
    return combine(parts, host ? 1u : n, host ? false : sparse, off, out);
}

/* decide once per image, before anything is allocated or bound: host mode when the switch is on, the image is not
 * sparse, and one HOST_VISIBLE memory type fits every block image (packed exactly as combine() packs the parts) */
static bool bh_plan_make(bool on, bool sparse, const VkMemoryRequirements *parts, uint32_t n,
                         const VkPhysicalDeviceMemoryProperties *mp, struct bh_plan *pl) {
    *pl = (struct bh_plan){0};
    if (!on || sparse || n < 2) return false;
    VkMemoryRequirements req;
    if (!combine(parts + 1, n - 1, false, pl->off + 1, &req)) return false; /* no type is common to all block images */
    const int32_t t = bh_type(mp, req.memoryTypeBits);
    if (t < 0) return false;
    pl->host = true;
    pl->type = (uint32_t)t;
    pl->size = req.size;
    return true;
}
/* NVHAL_ASTC_BLOCK_HOST_END */

/* f is an emulated compressed format the ICD does not support itself (*e: its description) */
static bool icd_lacks(const struct nvhal_device *d, VkFormat f, struct efmt *e) {
    return efmt_get(f, e) && (!d->astc_pd || fam_state(d->astc_pd, e) != NVHAL_ASTC_NATIVE);
}
/* the device emulates e's family */
static bool emulating(const struct nvhal_device *d, const struct efmt *e) {
    return d->astc && fam_state(d->astc_pd, e) == NVHAL_ASTC_EMULATE;
}
static const char *unavailable_why(const struct nvhal_device *d, const struct efmt *e) {
    const char *w = !d->astc_pd ? "" : e->fam == FAM_ASTC ? d->astc_pd->why : d->astc_pd->etc2_why;
    return w[0] ? w : "not set up";
}

/* --------------------------------------------------- the block-host switch (task-17) ----------------------------
 * debug.nvhal.astc_blocks_host / NVHAL_ASTC_BLOCKS_HOST: **ON by default since 2026-10-31**; an explicit 0 turns it
 * off. When on, the emulated compressed images' block images live in HAL-owned host memory instead of behind the
 * decoded image in the app's allocation (the bh_* block above is the decision), so the memory requirements the app
 * is given cover the decoded image only. The value is read once per process: set it before the app starts.
 * Why on by default: on a Pascal card with 8 GiB this is the difference between the app running out of
 * DEVICE_LOCAL memory 139 times (1.35 GiB spilling into host memory, every frame crossing PCIe) and 16 times.
 * Measured 205 -> 754 -> 1267 points on 3DMark Steel Nomad Light (docs/BREAKTHROUGH-blocks-host-3.7x.md).
 * Off costs one acquire load per emulated vkCreateImage, per AllocateMemory/FreeMemory, per DestroyDevice. */
static _Atomic int bh_state; /* 0 unprobed, 1 off, 2 on */
static pthread_once_t bh_once = PTHREAD_ONCE_INIT;

static void bh_init(void) {
    char v[32] = "";
#ifdef __ANDROID__
    __system_property_get("debug.nvhal.astc_blocks_host", v);
#else
    const char *e = getenv("NVHAL_ASTC_BLOCKS_HOST");
    if (e) snprintf(v, sizeof v, "%s", e);
#endif
    /* on unless explicitly set to 0 (see the block comment above) */
    const bool on = !(v[0] && atoi(v) <= 0);
    atomic_store_explicit(&bh_state, on ? 2 : 1, memory_order_release);
    if (on)
        NVHAL_WARN("astc blocks host: ON by default (set NVHAL_ASTC_BLOCKS_HOST / debug.nvhal.astc_blocks_host "
                       "to 0 to combine the block images into the app's allocation): each emulated "
                   "compressed image's block images get a HAL-owned HOST_VISIBLE allocation, so the app's memory "
                   "requirements cover the decoded image only; one summary line at vkDestroyDevice");
}

/* read once per process, like the other debug switches */
static inline bool bh_on(void) {
    int s = atomic_load_explicit(&bh_state, memory_order_acquire);
    if (s == 0) {
        pthread_once(&bh_once, bh_init);
        s = atomic_load_explicit(&bh_state, memory_order_acquire);
    }
    return s == 2;
}

/* --------------------------------------------------- VkImage emulation byte accounting (opt-in, task-8) --------------
 * An emulated compressed image costs VRAM twice over: the app's handle IS an ICD image of the decoded format
 * (RGBA8 UNORM/SRGB or R16[G16], 4/2 bytes per texel) and beside it the HAL keeps one block image per mip level
 * (one texel per block, 16 B for a 128-bit ASTC block, 8 B for a 64-bit ETC2/EAC block). This counts, for every
 * image vkCreateImage emulates: how many bytes the app has to allocate (the memory requirements it is given - the
 * decoded image plus every block image), what the same image in its native compressed format would have taken,
 * and buckets the images by family and block size. Host: NVHAL_ASTC_BYTES. Guest: debug.nvhal.astc_bytes
 * (window: NVHAL_ASTC_BYTES_WINDOW / debug.nvhal.astc_bytes.window, seconds, default 1.0, floor 0.1).
 * Off by default: an emulated vkCreateImage then pays one acquire load and a not-taken branch, and
 * vkDestroyDevice one acquire load. Independent of NVHAL_XLATE_STATS. */
#define ABS_ASTC_BLOCK_BYTES 16u /* every ASTC LDR block is 128 bit */
#define ABS_ETC2_BLOCK_BYTES 8u  /* every ETC2/EAC block is 64 bit */
#define ABS_BUCKETS (NVHAL_ASTC_NUM_PARTITION_TABLES + ETC2_VARIANTS) /* ASTC block sizes, then ETC2/EAC variants */

struct abs_bucket {
    uint64_t n, decoded, block, native;
};

static struct {
    pthread_mutex_t lock;
    struct abs_bucket b[ABS_BUCKETS];
    uint64_t n, refused;                 /* cumulative emulated + refused vkCreateImage */
    uint64_t decoded, block, native, total; /* cumulative bytes: parts, native compressed, app-visible requ. */
    uint64_t live, live_bytes;           /* emulated images alive right now and their app-visible bytes */
    uint64_t printed;                    /* images + refusals at the last line, to skip idle windows */
    uint64_t last_ns, window_ns;
} abs_ = {.lock = PTHREAD_MUTEX_INITIALIZER, .window_ns = 1000000000ull};

static _Atomic int abs_state; /* 0 unprobed, 1 off, 2 on */
static pthread_once_t abs_once = PTHREAD_ONCE_INIT;

static uint64_t abs_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void abs_init(void) {
    char v[32] = "", w[32] = "";
#ifdef __ANDROID__
    __system_property_get("debug.nvhal.astc_bytes", v);
    __system_property_get("debug.nvhal.astc_bytes.window", w);
#else
    const char *e = getenv("NVHAL_ASTC_BYTES");
    if (e) snprintf(v, sizeof v, "%s", e);
    e = getenv("NVHAL_ASTC_BYTES_WINDOW");
    if (e) snprintf(w, sizeof w, "%s", e);
#endif
    if (w[0]) {
        const double s = atof(w);
        if (s >= 0.1) abs_.window_ns = (uint64_t)(s * 1e9);
    }
    const bool on = atoi(v) > 0;
    abs_.last_ns = abs_now();
    atomic_store_explicit(&abs_state, on ? 2 : 1, memory_order_release);
    if (on)
        NVHAL_WARN("astc bytes: statistics ON (window %.2f s; NVHAL_ASTC_BYTES / debug.nvhal.astc_bytes): one line per "
                   "window over the emulated compressed images, and the cumulative line at vkDestroyDevice",
                   abs_.window_ns / 1e9);
}

/* read once per process, like the other debug switches: set it before the app starts */
static inline bool abs_on(void) {
    int s = atomic_load_explicit(&abs_state, memory_order_acquire);
    if (s == 0) {
        pthread_once(&abs_once, abs_init);
        s = atomic_load_explicit(&abs_state, memory_order_acquire);
    }
    return s == 2;
}

static void abs_human(uint64_t bytes, char *out, size_t n) {
    if (bytes >= (uint64_t)1 << 30) snprintf(out, n, "%.2f GiB", (double)bytes / (double)((uint64_t)1 << 30));
    else snprintf(out, n, "%.1f MiB", (double)bytes / (double)(1u << 20));
}

static double abs_ratio(uint64_t a, uint64_t b) { return b ? (double)a / (double)b : 0.0; }

/* "4x4 x812, 8x8 x30" / "RGBA8 x12"; empty when nothing was created (the caller prints "-") */
static size_t abs_hist(char *out, size_t n, bool astc) {
    const unsigned first = astc ? 0 : NVHAL_ASTC_NUM_PARTITION_TABLES;
    const unsigned count = astc ? NVHAL_ASTC_NUM_PARTITION_TABLES : ETC2_VARIANTS;
    size_t o = 0;
    out[0] = 0;
    for (unsigned i = 0; i < count; i++) {
        const struct abs_bucket *x = &abs_.b[first + i];
        if (!x->n) continue;
        char one[48];
        if (astc)
            snprintf(one, sizeof one, "%ux%u x%" PRIu64, (unsigned)block_dims[i][0], (unsigned)block_dims[i][1], x->n);
        else snprintf(one, sizeof one, "%s x%" PRIu64, etc2_variant_name[i], x->n);
        const int written = snprintf(out + o, n - o, "%s%s", o ? ", " : "", one);
        if (written < 0 || (size_t)written >= n - o) break;
        o += (size_t)written;
    }
    return o;
}

/* one line per window, plus the cumulative one at vkDestroyDevice - reads abs_ under its lock */
static void abs_report(bool final_) {
    char ha[352], he[224], lb[32], tb[32], db[32], kb[32], nb[32];
    pthread_mutex_lock(&abs_.lock);
    const uint64_t now = abs_now();
    if (!final_ && now - abs_.last_ns < abs_.window_ns) {
        pthread_mutex_unlock(&abs_.lock);
        return;
    }
    if (!final_ && abs_.n + abs_.refused == abs_.printed) { /* idle window: nothing to say */
        pthread_mutex_unlock(&abs_.lock);
        return;
    }
    const uint64_t n = abs_.n, refused = abs_.refused, live = abs_.live, live_bytes = abs_.live_bytes;
    const uint64_t decoded = abs_.decoded, block = abs_.block, native = abs_.native, total = abs_.total;
    const uint64_t fresh = n + refused - abs_.printed;
    abs_.printed = n + refused;
    abs_.last_ns = now;
    if (!abs_hist(ha, sizeof ha, true)) snprintf(ha, sizeof ha, "-");
    if (!abs_hist(he, sizeof he, false)) snprintf(he, sizeof he, "-");
    pthread_mutex_unlock(&abs_.lock);
    abs_human(live_bytes, lb, sizeof lb);
    abs_human(total, tb, sizeof tb);
    abs_human(decoded, db, sizeof db);
    abs_human(block, kb, sizeof kb);
    abs_human(native, nb, sizeof nb);
    NVHAL_WARN("astc bytes[%s %.2f s]: +%" PRIu64 " emulated/refused image(s) (cumulative %" PRIu64 " emulated, %" PRIu64
               " refused, %" PRIu64 " alive) | ASTC: %s | ETC2/EAC: %s | app-visible live %" PRIu64 " B (%s), "
               "cumulative created %" PRIu64 " B (%s) = decoded %" PRIu64 " B (%s) + block images %" PRIu64 " B (%s); "
               "native compressed %" PRIu64 " B (%s) -> %.1fx inflation (decoded alone %.1fx)",
               final_ ? "cumulative at vkDestroyDevice" : "window", final_ ? 0.0 : (double)abs_.window_ns / 1e9, fresh, n,
               refused, live, ha, he, live_bytes, lb, total, tb, decoded, db, block, kb, native, nb,
               abs_ratio(total, native), abs_ratio(decoded, native));
}

/* an emulated image exists: ai->size[0] is the decoded image, size[1..mips] the block images (ICD requirements) */
static void abs_add(const struct nvhal_astc_image *ai) {
    const struct efmt *e = &ai->e;
    const uint32_t blk = e->fam == FAM_ASTC ? ABS_ASTC_BLOCK_BYTES : ABS_ETC2_BLOCK_BYTES;
    uint64_t native = 0, block = 0;
    for (uint32_t m = 0; m < ai->mips; m++) {
        const uint64_t bx = div_up(mip_dim(ai->extent.width, m), e->bw), by = div_up(mip_dim(ai->extent.height, m), e->bh);
        native += bx * by * ai->layers * blk;
        block += ai->size[1 + m];
    }
    pthread_mutex_lock(&abs_.lock);
    struct abs_bucket *b = &abs_.b[e->cls];
    b->n++;
    b->decoded += ai->size[0];
    b->block += block;
    b->native += native;
    abs_.n++;
    abs_.decoded += ai->size[0];
    abs_.block += block;
    abs_.native += native;
    abs_.total += ai->req.size;
    abs_.live++;
    abs_.live_bytes += ai->req.size;
    pthread_mutex_unlock(&abs_.lock);
    abs_report(false);
}

static void abs_del(const struct nvhal_astc_image *ai) {
    pthread_mutex_lock(&abs_.lock);
    if (abs_.live) abs_.live--;
    abs_.live_bytes = abs_.live_bytes >= ai->req.size ? abs_.live_bytes - ai->req.size : 0;
    pthread_mutex_unlock(&abs_.lock);
}

static void abs_refuse(void) {
    pthread_mutex_lock(&abs_.lock);
    abs_.refused++;
    pthread_mutex_unlock(&abs_.lock);
    abs_report(false);
}

/* the device (and with it every image of it) is gone: print the cumulative line and drop the live figures */
static void abs_finish(void) {
    if (!abs_on()) return;
    abs_report(true);
    pthread_mutex_lock(&abs_.lock);
    abs_.live = 0;
    abs_.live_bytes = 0;
    pthread_mutex_unlock(&abs_.lock);
}

/* ---------------------------------------------------- block-host summary (task-17) -------------------------------
 * With the switch on, one NVHAL_WARN line at vkDestroyDevice says what it changed: which memory type each emulated
 * image's app memory ended up in (a VkDeviceMemory does not tell, so vkAllocateMemory's type is noted while the switch
 * is on), how many bytes of block images the HAL holds itself, and the app-visible requirements on the other side.
 * Off: the two allocation hooks cost one acquire load each. */
struct bh_mem {
    VkDeviceMemory mem;
    uint32_t type;
};

static struct {
    pthread_mutex_t lock;
    struct bh_mem *e;            /* the app's live memories and their types (switch on only) */
    uint32_t n, cap;
    uint64_t hist[BH_MAX_TYPES]; /* emulated images per memoryTypeIndex of the memory they were bound to */
    uint64_t unreported;         /* binds of memory this HAL never allocated */
    uint64_t created;            /* emulated images created (cumulative) */
    uint64_t host;               /* of those: with HAL-owned block images (cumulative) */
    uint64_t fallback;           /* of those: the HAL-owned path failed and the combined one was used */
    uint64_t app_live, app_total;
    uint64_t host_live, host_total; /* bytes of HAL-owned block memory */
} bhs_ = {.lock = PTHREAD_MUTEX_INITIALIZER};

/* ahb.c: vkAllocateMemory handed this VkDeviceMemory to the app */
void nvhal_astc_mem_note(VkDeviceMemory mem, uint32_t type) {
    if (mem == VK_NULL_HANDLE || type >= BH_MAX_TYPES || !bh_on()) return;
    pthread_mutex_lock(&bhs_.lock);
    for (uint32_t i = 0; i < bhs_.n; i++)
        if (bhs_.e[i].mem == mem) {
            bhs_.e[i].type = type;
            pthread_mutex_unlock(&bhs_.lock);
            return;
        }
    if (bhs_.n == bhs_.cap) {
        const uint32_t cap = bhs_.cap ? bhs_.cap * 2 : 512;
        struct bh_mem *e = realloc(bhs_.e, cap * sizeof *e);
        if (!e) { /* diagnostics only: the histogram then counts this memory as unreported */
            pthread_mutex_unlock(&bhs_.lock);
            return;
        }
        bhs_.e = e;
        bhs_.cap = cap;
    }
    bhs_.e[bhs_.n++] = (struct bh_mem){mem, type};
    pthread_mutex_unlock(&bhs_.lock);
}

/* ahb.c: vkFreeMemory */
void nvhal_astc_mem_forget(VkDeviceMemory mem) {
    if (mem == VK_NULL_HANDLE || !bh_on()) return;
    pthread_mutex_lock(&bhs_.lock);
    for (uint32_t i = 0; i < bhs_.n; i++)
        if (bhs_.e[i].mem == mem) {
            bhs_.e[i] = bhs_.e[--bhs_.n];
            break;
        }
    pthread_mutex_unlock(&bhs_.lock);
}

/* the app bound an emulated image to this memory: one histogram entry */
static void bh_bound(VkDeviceMemory mem) {
    pthread_mutex_lock(&bhs_.lock);
    bool known = false;
    uint32_t type = 0;
    for (uint32_t i = 0; i < bhs_.n && !known; i++)
        if (bhs_.e[i].mem == mem) {
            type = bhs_.e[i].type;
            known = true;
        }
    if (known) bhs_.hist[type]++;
    else bhs_.unreported++;
    pthread_mutex_unlock(&bhs_.lock);
}

static void bh_live_add(const struct nvhal_astc_image *ai) {
    pthread_mutex_lock(&bhs_.lock);
    bhs_.created++;
    bhs_.app_live += ai->req.size;
    bhs_.app_total += ai->req.size;
    if (ai->blocks_host) {
        bhs_.host++;
        bhs_.host_live += ai->blocks_size;
        bhs_.host_total += ai->blocks_size;
    }
    pthread_mutex_unlock(&bhs_.lock);
}

static void bh_live_del(const struct nvhal_astc_image *ai) {
    pthread_mutex_lock(&bhs_.lock);
    bhs_.app_live = bhs_.app_live >= ai->req.size ? bhs_.app_live - ai->req.size : 0;
    if (ai->blocks_host) bhs_.host_live = bhs_.host_live >= ai->blocks_size ? bhs_.host_live - ai->blocks_size : 0;
    pthread_mutex_unlock(&bhs_.lock);
}

static void bh_fallback(void) {
    pthread_mutex_lock(&bhs_.lock);
    bhs_.fallback++;
    pthread_mutex_unlock(&bhs_.lock);
}

/* "type 0 x512, type 3 x3"; empty when nothing was bound. Caller holds bhs_.lock. */
static size_t bh_hist_str(char *out, size_t n) {
    size_t o = 0;
    out[0] = 0;
    for (uint32_t t = 0; t < BH_MAX_TYPES; t++) {
        if (!bhs_.hist[t]) continue;
        const int w = snprintf(out + o, n - o, "%stype %u x%" PRIu64, o ? ", " : "", t, bhs_.hist[t]);
        if (w < 0 || (size_t)w >= n - o) break;
        o += (size_t)w;
    }
    return o;
}

/* the one summary line, at vkDestroyDevice; silent when no image was emulated */
static void bh_report(void) {
    if (!bh_on()) return;
    char hist[320], al[32], at[32], hb[32], ht[32];
    pthread_mutex_lock(&bhs_.lock);
    if (!bhs_.created) {
        pthread_mutex_unlock(&bhs_.lock);
        return;
    }
    const uint64_t created = bhs_.created, host = bhs_.host, fallback = bhs_.fallback, unreported = bhs_.unreported;
    const uint64_t app_live = bhs_.app_live, app_total = bhs_.app_total;
    const uint64_t host_live = bhs_.host_live, host_total = bhs_.host_total;
    if (!bh_hist_str(hist, sizeof hist)) snprintf(hist, sizeof hist, "-");
    pthread_mutex_unlock(&bhs_.lock);
    abs_human(app_live, al, sizeof al);
    abs_human(app_total, at, sizeof at);
    abs_human(host_live, hb, sizeof hb);
    abs_human(host_total, ht, sizeof ht);
    NVHAL_WARN("astc blocks host summary at vkDestroyDevice: %" PRIu64 " emulated image(s) created, %" PRIu64 " with "
               "HAL-owned block images (%" PRIu64 " B (%s) alive, %" PRIu64 " B (%s) cumulative), %" PRIu64 " fell back "
               "to the combined allocation; app-visible requirements %" PRIu64 " B (%s) alive, %" PRIu64 " B (%s) "
               "cumulative (decoded images only); bind memoryTypeIndex histogram: %s; %" PRIu64 " bind(s) of memory this "
               "HAL did not allocate",
               created, host, host_live, hb, host_total, ht, fallback, app_live, al, app_total, at, hist, unreported);
}

/* ---- ASTC -> BC1 (docs/astc-bc1-plan.md): the app's image is BC1 instead of RGBA8 ---------------
 * RGBA8 is 4 B/texel; BC1 is 0.5, and on an 8 GiB card the decoded images alone were 5.90 GiB of the
 * 7.12 GiB the app was told to allocate (588 ASTC images, measured).  BC1 has no STORAGE_IMAGE feature
 * (optimalTilingFeatures 0x1d401 is SAMPLED|BLIT_SRC|FILTER_LINEAR|TRANSFER_SRC|TRANSFER_DST), so the
 * decoder cannot write it directly: the decode goes block image -> HAL-owned RGBA8 tmp -> bc1_encode ->
 * HAL-owned BC1 block buffer -> vkCmdCopyBufferToImage -> the app's BC1 image.
 * Only 4x4 ASTC: that is the block size 1:1 with BC1's own 4x4 blocks (378 of the 588 measured images);
 * every other block size keeps the RGBA8 path.  Default off; any failure falls back to RGBA8. */
static _Atomic int bc1_state; /* 0 unprobed, 1 off, 2 on */
static pthread_once_t bc1_once = PTHREAD_ONCE_INIT;

static void bc1_init(void) {
    char v[32] = "";
#ifdef __ANDROID__
    __system_property_get("debug.nvhal.astc_bc1", v);
#else
    const char *e = getenv("NVHAL_ASTC_BC1");
    if (e) snprintf(v, sizeof v, "%s", e);
#endif
    atomic_store_explicit(&bc1_state, atoi(v) > 0 ? 2 : 1, memory_order_release);
}

static bool bc1_on(void) {
    int st = atomic_load_explicit(&bc1_state, memory_order_acquire);
    if (st == 0) {
        pthread_once(&bc1_once, bc1_init);
        st = atomic_load_explicit(&bc1_state, memory_order_acquire);
    }
    return st == 2;
}

/* EXPERIMENT (docs/astc-bc1-plan.md): put the DECODED image in host memory too, so that an app whose textures
 * would not fit in VRAM at all can still render. This is not the fix -- BC1 is (0.5 B/texel instead of 4) -- it
 * measures the upper bound of "textures are the problem" before the BC1 work starts. Off unless
 * NVHAL_ASTC_DECODED_HOST / debug.nvhal.astc_decoded_host is 1. The decoded image is *sampled* every frame, so
 * unlike a block image it is read through PCIe by every texture fetch: expect this to be much worse per frame. */
static int32_t dh_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits) {
    for (uint32_t i = 0; i < mp->memoryTypeCount && i < BH_MAX_TYPES; i++) {
        const VkMemoryPropertyFlags f = mp->memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i))) continue;
        if (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) continue;
        if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) continue;
        return (int32_t)i;
    }
    return -1;
}

static _Atomic int dh_state; /* 0 unprobed, 1 off, 2 on */
static pthread_once_t dh_once = PTHREAD_ONCE_INIT;

static void dh_init(void) {
    char v[32] = "";
#ifdef __ANDROID__
    __system_property_get("debug.nvhal.astc_decoded_host", v);
#else
    const char *e = getenv("NVHAL_ASTC_DECODED_HOST");
    if (e) snprintf(v, sizeof v, "%s", e);
#endif
    atomic_store_explicit(&dh_state, atoi(v) > 0 ? 2 : 1, memory_order_release);
}

static bool dh_on(void) {
    int s = atomic_load_explicit(&dh_state, memory_order_acquire);
    if (s == 0) {
        pthread_once(&dh_once, dh_init);
        s = atomic_load_explicit(&dh_state, memory_order_acquire);
    }
    return s == 2;
}

/* switch=1: bind the block images into one HAL-owned HOST_VISIBLE allocation (bp: layout and type, decided by
 * bh_plan_make() before anything was allocated). 1 = done; 0 = not usable, the block images have been recreated
 * unbound so the combined path can bind them (logged); -1 = hard failure, *r set. */
static int blocks_bind_host(struct nvhal_device *d, struct nvhal_astc_image *ai, const struct bh_plan *bp,
                            const struct astc_cis *c, const char **step, VkResult *r) {
    VkDevice dev = d->handle;
    *step = "vkAllocateMemory(block images in HAL-owned host memory)";
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, bp->size, bp->type};
    VkResult ar = d->AllocateMemory(dev, &mai, NULL, &ai->blocks_mem);
    if (ar == VK_SUCCESS) {
        *step = "vkBindImageMemory(block images in HAL-owned host memory)";
        for (uint32_t m = 0; m < c->mips; m++) {
            ar = d->xl.BindImageMemory(dev, ai->block[m], ai->blocks_mem, bp->off[1 + m]);
            if (ar != VK_SUCCESS) break;
        }
    }
    if (ar == VK_SUCCESS) return 1;
    NVHAL_ERR("vkCreateImage(compressed-format emulation): the block images could not be placed in HAL-owned host "
              "memory (%llu B, memoryTypeIndex %u, %u image(s)): %s at %s -> falling back to the combined allocation "
              "(the app's memory covers the block images again) %s",
              (unsigned long long)bp->size, bp->type, c->mips, NVHAL_VKR(ar), *step, gap(&c->e));
    bh_fallback();
    /* nothing bound now, but a bound image cannot be rebound and a partly bound set cannot be completed: recreate
     * them all unbound, so the combined path can bind them wherever the app's memory is */
    if (ai->blocks_mem) {
        d->FreeMemory(dev, ai->blocks_mem, NULL);
        ai->blocks_mem = VK_NULL_HANDLE;
    }
    for (uint32_t m = 0; m < MAX_MIPS; m++)
        if (ai->block[m]) {
            d->DestroyImage(dev, ai->block[m], NULL);
            ai->block[m] = VK_NULL_HANDLE;
        }
    for (uint32_t m = 0; m < c->mips; m++) {
        *step = "vkCreateImage(block image, after the HAL-owned binding failed)";
        const VkResult cr = d->CreateImage(dev, &c->block[m], NULL, &ai->block[m]);
        if (cr != VK_SUCCESS) {
            *r = cr;
            return -1;
        }
    }
    return 0;
}

static void destroy_parts(struct nvhal_device *d, struct nvhal_astc_image *ai, const VkAllocationCallbacks *alloc,
                          bool app_image) {
    if (d->astc) pthread_mutex_lock(&d->astc->lock);
    for (uint32_t i = 0; i < ai->nslots; i++) {
        struct astc_slot *s = &ai->slots[i];
        if (s->set) d->cs.FreeDescriptorSets(d->handle, s->pool, 1, &s->set);
        if (s->src) d->cs.DestroyImageView(d->handle, s->src, NULL);
        if (s->dst) d->cs.DestroyImageView(d->handle, s->dst, NULL);
    }
    if (d->astc) pthread_mutex_unlock(&d->astc->lock);
    for (uint32_t m = 0; m < MAX_MIPS; m++)
        if (ai->block[m]) d->DestroyImage(d->handle, ai->block[m], NULL);
    if (ai->blocks_mem) d->FreeMemory(d->handle, ai->blocks_mem, NULL); /* task-17: the block images' own memory */
    if (ai->decoded_mem) d->FreeMemory(d->handle, ai->decoded_mem, NULL); /* the decoded image's own memory */
    if (app_image && ai->image) d->DestroyImage(d->handle, ai->image, alloc);
    if (ai->counted) bh_live_del(ai);
    free(ai->slots);
    free(ai);
}

bool nvhal_astc_CreateImage(struct nvhal_device *d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *alloc,
                            VkImage *pImage, VkResult *r) {
    static const char who[] = "vkCreateImage(compressed-format emulation)";
    struct efmt e;
    if (!icd_lacks(d, ci->format, &e)) return false;
    if (!emulating(d, &e)) {
        NVHAL_ERR("%s: VkFormat %d is %s, which the ICD does not support, and this device has no %s emulation (%s) %s -> "
                  "VK_ERROR_FORMAT_NOT_SUPPORTED", who, ci->format, fam_name(&e), fam_name(&e), unavailable_why(d, &e),
                  gap(&e));
        if (__builtin_expect(abs_on(), 0)) abs_refuse();
        *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
        return true;
    }
    struct astc_cis c;
    *r = build_cis(d->astc_pd, ci, &c, who);
    if (*r != VK_SUCCESS) {
        if (__builtin_expect(abs_on(), 0)) abs_refuse();
        return true;
    }
    struct nvhal_astc_image *ai = calloc(1, sizeof *ai);
    if (!ai) {
        *r = VK_ERROR_OUT_OF_HOST_MEMORY;
        return true;
    }
    ai->dev = d;
    ai->format = ci->format;
    ai->e = c.e;
    ai->bw = c.e.bw;
    ai->bh = c.e.bh;
    ai->table = c.e.cls;
    ai->extent = ci->extent;
    ai->mips = c.mips;
    ai->layers = ci->arrayLayers;
    ai->usage = ci->usage;
    VkDevice dev = d->handle;
    const char *step = "vkCreateImage(decoded image)";
    ai->bc1 = c.bc1;
    *r = d->CreateImage(dev, &c.rgba, alloc, &ai->image);
    if (*r == VK_SUCCESS && c.bc1) {
        step = "vkCreateImage(RGBA8 tmp for the BC1 encoder)";
        *r = d->CreateImage(dev, &c.rgba2, NULL, &ai->tmp);
    }
    for (uint32_t m = 0; m < c.mips && *r == VK_SUCCESS; m++) {
        step = "vkCreateImage(block image)";
        *r = d->CreateImage(dev, &c.block[m], NULL, &ai->block[m]);
    }
    VkMemoryRequirements parts[1 + MAX_MIPS];
    VkMemoryRequirements tmp_part = {0, 0, 0};
    struct bh_plan bp;
    bool host = false; /* task-17: the block images are in HAL-owned host memory */
    if (*r == VK_SUCCESS) {
        if (c.bc1) d->xl.GetImageMemoryRequirements(dev, ai->tmp, &tmp_part);
        d->xl.GetImageMemoryRequirements(dev, ai->image, &parts[0]);
        for (uint32_t m = 0; m < c.mips; m++) d->xl.GetImageMemoryRequirements(dev, ai->block[m], &parts[1 + m]);
        /* a part the ICD wants dedicated cannot share the app's allocation (asked where the core query is valid) */
        PFN_vkGetImageMemoryRequirements2 req2 = d->xl.GetImageMemoryRequirements2;
        if (req2 && d->api_version >= VK_API_VERSION_1_1) {
            for (uint32_t i = 0; i <= c.mips && *r == VK_SUCCESS; i++) {
                VkMemoryDedicatedRequirements dr = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS, NULL, 0, 0};
                VkMemoryRequirements2 m2 = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dr, {0}};
                VkImageMemoryRequirementsInfo2 q = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, NULL,
                                                    i ? ai->block[i - 1] : ai->image};
                req2(dev, &q, &m2);
                if (dr.requiresDedicatedAllocation) {
                    step = "memory requirements (the ICD requires a dedicated allocation for a part; the parts share one)";
                    *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
                }
            }
        }
        ai->sparse = (ci->flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) != 0;
        for (uint32_t i = 0; i <= c.mips; i++) ai->size[i] = parts[i].size;
        /* EXPERIMENT: put the decoded image in host memory as well (docs/astc-bc1-plan.md); sparse images keep
         * the app path. On failure fall through to the normal path -- this switch must never fail a create. */
        if (*r == VK_SUCCESS && !ai->sparse && dh_on()) {
            const int32_t t = dh_type(&d->mem_props, parts[0].memoryTypeBits);
            if (t >= 0) {
                VkMemoryAllocateInfo mi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, parts[0].size, (uint32_t)t};
                VkDeviceMemory m = VK_NULL_HANDLE;
                if (d->AllocateMemory(dev, &mi, NULL, &m) == VK_SUCCESS) {
                    if (d->xl.BindImageMemory(dev, ai->image, m, 0) == VK_SUCCESS) {
                        ai->decoded_mem = m;
                        ai->decoded_host = true;
                    } else {
                        d->FreeMemory(dev, m, NULL);
                    }
                }
            }
        }
        if (*r == VK_SUCCESS && ai->bc1) {
            /* the tmp RGBA8 image and the BC1 block buffer are HAL-private: only the decoder writes tmp and
             * only the encoder reads it, nothing samples them, so host memory is fine and keeps them out of
             * the 8 GiB the app is competing for. */
            step = "HAL-owned memory for the BC1 path (tmp image + block buffer)";
            uint32_t bytes_per_block = 8u;                     /* BC1 */
            VkDeviceSize blocks = 0;
            for (uint32_t m = 0; m < c.mips; m++) {
                const uint32_t w = mip_dim(ci->extent.width, m), h = mip_dim(ci->extent.height, m);
                blocks += (VkDeviceSize)div_up(w, 4) * (VkDeviceSize)div_up(h, 4) * ci->arrayLayers;
            }
            ai->bc1_blocks = blocks;
            const int32_t tt = dh_type(&d->mem_props, tmp_part.memoryTypeBits);
            if (tt < 0) {
                *r = VK_ERROR_OUT_OF_HOST_MEMORY;
            } else {
                VkMemoryAllocateInfo mi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, tmp_part.size, (uint32_t)tt};
                if (d->AllocateMemory(dev, &mi, NULL, &ai->tmp_mem) != VK_SUCCESS) {
                    *r = VK_ERROR_OUT_OF_HOST_MEMORY;
                }
                if (*r == VK_SUCCESS && d->xl.BindImageMemory(dev, ai->tmp, ai->tmp_mem, 0) != VK_SUCCESS) {
                    d->FreeMemory(dev, ai->tmp_mem, NULL);
                    ai->tmp_mem = VK_NULL_HANDLE;
                    *r = VK_ERROR_OUT_OF_HOST_MEMORY;
                }
            }
            if (*r == VK_SUCCESS) {
                VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0,
                                         blocks * bytes_per_block,
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                         VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
                if (d->xl.CreateBuffer(dev, &bi, NULL, &ai->bc1_buf) != VK_SUCCESS) {
                    *r = VK_ERROR_OUT_OF_HOST_MEMORY;
                } else {
                    VkMemoryRequirements br;
                    d->cs.GetBufferMemoryRequirements(dev, ai->bc1_buf, &br);
                    const int32_t bt = dh_type(&d->mem_props, br.memoryTypeBits);
                    if (bt < 0) {
                        *r = VK_ERROR_OUT_OF_HOST_MEMORY;
                    } else {
                        VkMemoryAllocateInfo bi2 = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, br.size, (uint32_t)bt};
                        if (d->AllocateMemory(dev, &bi2, NULL, &ai->bc1_mem) != VK_SUCCESS ||
                            d->cs.BindBufferMemory(dev, ai->bc1_buf, ai->bc1_mem, 0) != VK_SUCCESS) {
                            *r = VK_ERROR_OUT_OF_HOST_MEMORY;
                        }
                    }
                }
            }
            if (*r != VK_SUCCESS) {
                /* this switch must never fail a create: drop the BC1 path and let the image stay RGBA8 */
                NVHAL_ERR("vkCreateImage: the BC1 path could not allocate its private memory -> falling back to the "
                          "RGBA8 decoded image %s", gap(&c.e));
                if (ai->bc1_mem) d->FreeMemory(dev, ai->bc1_mem, NULL);
                if (ai->bc1_buf) d->cs.DestroyBuffer(dev, ai->bc1_buf, NULL);
                if (ai->tmp_mem) d->FreeMemory(dev, ai->tmp_mem, NULL);
                if (ai->tmp) d->DestroyImage(dev, ai->tmp, NULL);
                ai->bc1_mem = VK_NULL_HANDLE; ai->bc1_buf = VK_NULL_HANDLE;
                ai->tmp_mem = VK_NULL_HANDLE; ai->tmp = VK_NULL_HANDLE;
                ai->bc1 = false;
                *r = VK_SUCCESS;
            }
        }
        if (*r == VK_SUCCESS) {
            /* decided before anything is allocated: on the switch, the image not being sparse, and one HOST_VISIBLE
             * memory type fitting every block image (bh_plan_make) */
            host = bh_plan_make(bh_on(), ai->sparse, parts, 1 + c.mips, &d->mem_props, &bp);
            if (host) {
                const int hb = blocks_bind_host(d, ai, &bp, &c, &step, r); /* -1: *r is set */
                host = hb > 0;
            }
            if (*r == VK_SUCCESS && ai->decoded_host) {
                /* the decoded image is already bound in HAL-owned host memory: tell the app nothing needs
                 * binding, and give it the alignment of the parts that still do (the block images) */
                VkMemoryRequirements rest;
                if (host) {
                    memset(&rest, 0, sizeof rest);
                    rest.alignment = 1;
                } else if (!bh_requirements(parts, 1 + c.mips, false, ai->sparse, ai->off, &rest)) {
                    step = "memory requirements (no memory type fits the block images)";
                    *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
                }
                if (*r == VK_SUCCESS) {
                    ai->req = rest;
                    ai->req.size = 0;
                    /* nothing of the decoded image is bound from the app's memory */
                    if (host) ai->off[0] = 0;
                }
            } else if (*r == VK_SUCCESS && !bh_requirements(parts, 1 + c.mips, host, ai->sparse, ai->off, &ai->req)) {
                step = "memory requirements (no memory type fits the decoded image and the block images)";
                *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
            if (host) {
                ai->blocks_host = true;
                ai->blocks_size = bp.size;
                ai->blocks_type = bp.type;
            }
        }
    }
    if (*r == VK_SUCCESS && !nvhal_hash_put(&images, NVHAL_ND_U64(ai->image), ai)) {
        step = "recording the image";
        *r = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (*r != VK_SUCCESS) {
        if (__builtin_expect(abs_on(), 0)) abs_refuse();
        NVHAL_ERR("%s: VkFormat %d %ux%u: %s -> %s %s", who, ci->format, ci->extent.width, ci->extent.height, step,
                  NVHAL_VKR(*r), gap(&c.e));
        destroy_parts(d, ai, alloc, true);
        return true;
    }
    atomic_fetch_add_explicit(&n_images, 1, memory_order_release);
    if (__builtin_expect(abs_on(), 0)) abs_add(ai); /* opt-in byte accounting (task-8) */
    if (bh_on()) { /* task-17: the summary line's live figures; off = one acquire load */
        ai->counted = true;
        bh_live_add(ai);
    }
    NVHAL_TRACE("%s: VkImage %p (VkFormat %d) = VkFormat %d %ux%u, %u mip(s), %u layer(s) + %u block image(s); memory "
                "%llu B (decoded image %llu B), alignment %llu, types 0x%x; block images in %s", who,
                NVHAL_LOGH(ai->image), ci->format, c.e.decoded, ci->extent.width, ci->extent.height, ai->mips, ai->layers,
                ai->mips, (unsigned long long)ai->req.size, (unsigned long long)parts[0].size,
                (unsigned long long)ai->req.alignment, ai->req.memoryTypeBits,
                ai->blocks_host ? "HAL-owned host memory" : "the app's allocation");
    *pImage = ai->image;
    return true;
}

bool nvhal_astc_DestroyImage(struct nvhal_device *d, VkImage image, const VkAllocationCallbacks *alloc) {
    if (image == VK_NULL_HANDLE || nvhal_no_astc()) return false;
    struct nvhal_astc_image *ai = nvhal_hash_del(&images, NVHAL_ND_U64(image));
    if (!ai) return false;
    atomic_fetch_sub_explicit(&n_images, 1, memory_order_release);
    if (__builtin_expect(abs_on(), 0)) abs_del(ai);
    destroy_parts(d, ai, alloc, true);
    return true;
}

static void fill_reqs(const VkMemoryRequirements *req, VkMemoryRequirements2 *out) {
    out->memoryRequirements = *req;
    for (VkBaseOutStructure *s = out->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            ((VkMemoryDedicatedRequirements *)s)->prefersDedicatedAllocation = VK_FALSE;
            ((VkMemoryDedicatedRequirements *)s)->requiresDedicatedAllocation = VK_FALSE;
        }
}

bool nvhal_astc_image_requirements(VkImage image, VkMemoryRequirements2 *out) {
    struct nvhal_astc_image *ai = lookup(image);
    if (!ai) return false;
    fill_reqs(&ai->req, out);
    return true;
}

bool nvhal_astc_ci_refused(struct nvhal_device *d, const VkImageCreateInfo *ci, const char *who) {
    struct efmt e;
    if (!ci || !icd_lacks(d, ci->format, &e)) return false;
    NVHAL_TRACE("%s: %s create info (VkFormat %d) answered by the HAL, not the ICD", who, fam_name(&e), ci->format);
    return true;
}

bool nvhal_astc_ci_requirements(struct nvhal_device *d, const VkImageCreateInfo *ci, VkMemoryRequirements2 *out, bool khr) {
    static const char who[] = "vkGetDeviceImageMemoryRequirements(compressed-format emulation)";
    struct efmt e;
    if (!ci || !icd_lacks(d, ci->format, &e)) return false;
    memset(&out->memoryRequirements, 0, sizeof out->memoryRequirements);
    if (!emulating(d, &e)) {
        NVHAL_ERR("%s: VkFormat %d is %s, which the ICD does not support, and this device has no %s emulation (%s) %s; "
                  "zero requirements returned", who, ci->format, fam_name(&e), fam_name(&e), unavailable_why(d, &e),
                  gap(&e));
        return true;
    }
    struct astc_cis c;
    if (build_cis(d->astc_pd, ci, &c, who) != VK_SUCCESS) return true;
    PFN_vkGetDeviceImageMemoryRequirements f =
        khr ? d->xl.GetDeviceImageMemoryRequirementsKHR : d->xl.GetDeviceImageMemoryRequirements;
    VkMemoryRequirements parts[1 + MAX_MIPS];
    VkDeviceSize off[1 + MAX_MIPS];
    for (uint32_t i = 0; i <= c.mips; i++) {
        VkDeviceImageMemoryRequirements q = {VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, NULL,
                                             i ? &c.block[i - 1] : &c.rgba, 0};
        VkMemoryRequirements2 m2 = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, NULL, {0}};
        f(d->handle, &q, &m2);
        parts[i] = m2.memoryRequirements;
    }
    /* the same decision vkCreateImage will take (bh_plan_make + bh_requirements), so the requirements an app queries
     * from a create info match the ones it gets from the image */
    const bool sparse = (ci->flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) != 0;
    struct bh_plan bp;
    const bool host = bh_plan_make(bh_on(), sparse, parts, 1 + c.mips, &d->mem_props, &bp);
    VkMemoryRequirements req;
    if (!bh_requirements(parts, 1 + c.mips, host, sparse, off, &req)) {
        NVHAL_ERR("%s: VkFormat %d: no memory type fits the decoded image and the block images %s", who, ci->format,
                  gap(&e));
        return true;
    }
    fill_reqs(&req, out);
    return true;
}

VkResult nvhal_astc_bind1(struct nvhal_device *d, VkImage image, VkDeviceMemory mem, VkDeviceSize offset, bool *handled) {
    struct nvhal_astc_image *ai = lookup(image);
    *handled = ai != NULL;
    if (!ai) return VK_SUCCESS;
    VkResult r = d->xl.BindImageMemory(d->handle, image, mem, offset + ai->off[0]);
    if (r == VK_SUCCESS && bh_on()) bh_bound(mem); /* task-17: the summary's memoryTypeIndex histogram */
    if (ai->blocks_host) { /* already bound in the HAL's own memory (task-17): only the app's image is left */
        if (r != VK_SUCCESS)
            NVHAL_ERR("vkBindImageMemory: emulated compressed image %p at memory %p offset %llu -> %s (its block images "
                      "are in the HAL's own memory) %s", NVHAL_LOGH(image), NVHAL_LOGH(mem), (unsigned long long)offset,
                      NVHAL_VKR(r), gap(&ai->e));
        return r;
    }
    for (uint32_t m = 0; m < ai->mips && r == VK_SUCCESS; m++)
        r = d->xl.BindImageMemory(d->handle, ai->block[m], mem, offset + ai->off[1 + m]);
    if (r != VK_SUCCESS)
        NVHAL_ERR("vkBindImageMemory: emulated compressed image %p (+%u block images) at memory %p offset %llu -> %s", NVHAL_LOGH(image),
                  ai->mips, NVHAL_LOGH(mem), (unsigned long long)offset, NVHAL_VKR(r));
    return r;
}

VkResult nvhal_astc_expand_binds(uint32_t n, const VkBindImageMemoryInfo *in, VkBindImageMemoryInfo **out, uint32_t *nout) {
    *out = (VkBindImageMemoryInfo *)in;
    *nout = n;
    if (nvhal_no_astc()) return VK_SUCCESS;
    /* task-17: a host-mode image's block images are already bound in the HAL's own memory, so they add nothing here */
    const bool hist = bh_on();
    uint32_t extra = 0;
    for (uint32_t i = 0; i < n; i++) {
        struct nvhal_astc_image *ai = lookup(in[i].image);
        if (ai) extra += ai->blocks_host ? 0 : ai->mips;
        if (ai && hist) bh_bound(in[i].memory);
    }
    if (!extra) return VK_SUCCESS;
    VkBindImageMemoryInfo *o = malloc((n + extra) * sizeof *o);
    if (!o) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        o[k++] = in[i];
        struct nvhal_astc_image *ai = lookup(in[i].image);
        for (uint32_t m = 0; ai && !ai->blocks_host && m < ai->mips; m++)
            o[k++] = (VkBindImageMemoryInfo){VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, NULL, ai->block[m], in[i].memory,
                                             in[i].memoryOffset + ai->off[1 + m]};
    }
    *out = o;
    *nout = k;
    return VK_SUCCESS;
}

bool nvhal_astc_CreateImageView(struct nvhal_device *d, VkDevice device, const VkImageViewCreateInfo *ci,
                                const VkAllocationCallbacks *alloc, VkImageView *view, VkResult *r) {
    struct nvhal_astc_image *ai = lookup(ci->image);
    if (!ai) return false;
    struct efmt ve;
    if (!efmt_get(ci->format, &ve) || ve.cls != ai->table) {
        NVHAL_ERR("vkCreateImageView: view format %d of %s image %p (VkFormat %d): only formats of the image's own decode "
                  "class are emulated (block-texel views and EAC UNORM/SNORM reinterpretation are not) %s -> "
                  "VK_ERROR_FORMAT_NOT_SUPPORTED", ci->format, fam_name(&ai->e), NVHAL_LOGH(ci->image), ai->format,
                  gap(&ai->e));
        *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
        return true;
    }
    VkImageViewCreateInfo t = *ci;
    t.format = ve.decoded;
    bool has_usage = false;
    const void *decode_mode = NULL;
    for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO) has_usage = true;
        if (s->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_ASTC_DECODE_MODE_EXT) decode_mode = s; /* no decode mode there */
    }
    /* the image has STORAGE usage for the decoder; the app's view must not inherit it (SRGB/SNORM: no storage) */
    VkImageViewUsageCreateInfo u = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, ai->usage};
    unsigned char storage[1024];
    if ((decode_mode || !has_usage) &&
        !nvhal_chain_replace(ci->pNext, decode_mode, has_usage ? NULL : &u, (const void **)&u.pNext, storage,
                             sizeof storage, "vkCreateImageView(compressed-format emulation)", &t.pNext)) {
        *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
        return true;
    }
    *r = d->xl.CreateImageView(device, &t, alloc, view);
    return true;
}

int nvhal_astc_alloc_info(const VkMemoryAllocateInfo *ai, VkMemoryAllocateInfo *out, void *storage, size_t storage_size) {
    if (nvhal_no_astc()) return 0;
    const VkBaseInStructure *ded = NULL;
    VkImage image = VK_NULL_HANDLE;
    for (const VkBaseInStructure *s = ai->pNext; s && !ded; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO)
            image = ((const VkMemoryDedicatedAllocateInfo *)s)->image;
        else if (s->sType == VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_MEMORY_ALLOCATE_INFO_NV)
            image = ((const VkDedicatedAllocationMemoryAllocateInfoNV *)s)->image;
        else
            continue;
        if (lookup(image)) ded = s;
    }
    if (!ded) return 0;
    *out = *ai;
    if (!nvhal_chain_replace(ai->pNext, ded, NULL, NULL, storage, storage_size, "vkAllocateMemory(compressed-format "
                             "emulation)", &out->pNext))
        return -1;
    NVHAL_TRACE("vkAllocateMemory: dedicated allocation for emulated compressed image %p made non-dedicated (its memory also "
                "backs the block images)", NVHAL_LOGH(image));
    return 1;
}

/* ------------------------------------------------------------------ sparse binding */

VkResult nvhal_astc_bind_sparse(uint32_t n, const VkBindSparseInfo *in, VkBindSparseInfo **out, void ***owned,
                                uint32_t *nowned) {
    *out = (VkBindSparseInfo *)in;
    *owned = NULL;
    *nowned = 0;
    if (nvhal_no_astc()) return VK_SUCCESS;
    bool any = false;
    struct nvhal_astc_image *ai;
    for (uint32_t i = 0; i < n && !any; i++) {
        for (uint32_t k = 0; k < in[i].imageOpaqueBindCount && !any; k++) any = lookup(in[i].pImageOpaqueBinds[k].image);
        for (uint32_t k = 0; k < in[i].imageBindCount; k++)
            if ((ai = lookup(in[i].pImageBinds[k].image))) {
                NVHAL_ERR("vkQueueBindSparse: VkSparseImageMemoryBindInfo for %s image %p: emulated compressed images have "
                          "no sparse residency %s -> VK_ERROR_FORMAT_NOT_SUPPORTED", fam_name(&ai->e),
                          NVHAL_LOGH(in[i].pImageBinds[k].image), gap(&ai->e));
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
    }
    if (!any) return VK_SUCCESS;
    VkBindSparseInfo *o = malloc(n * sizeof *o);
    void **own = calloc(2 * (size_t)n + 1, sizeof *own);
    if (!o || !own) {
        free(o);
        free(own);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memcpy(o, in, n * sizeof *o);
    own[(*nowned)++] = o;
    for (uint32_t i = 0; i < n; i++) {
        /* count: each emulated image becomes one bind info per part, each with up to the app's bind count */
        uint32_t ninfo = 0, nbind = 0;
        for (uint32_t k = 0; k < in[i].imageOpaqueBindCount; k++) {
            const VkSparseImageOpaqueMemoryBindInfo *b = &in[i].pImageOpaqueBinds[k];
            struct nvhal_astc_image *ai = lookup(b->image);
            const uint32_t nparts = ai && !ai->blocks_host ? 1 + ai->mips : 1; /* task-17: host blocks are bound */
            ninfo += nparts;
            nbind += ai ? nparts * b->bindCount : 0;
        }
        if (ninfo == in[i].imageOpaqueBindCount && !nbind) continue;
        VkSparseImageOpaqueMemoryBindInfo *infos = malloc(ninfo * sizeof *infos + nbind * sizeof(VkSparseMemoryBind));
        if (!infos) {
            for (uint32_t j = 0; j < *nowned; j++) free(own[j]);
            free(own);
            *nowned = 0;
            *out = (VkBindSparseInfo *)in;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        own[(*nowned)++] = infos;
        VkSparseMemoryBind *binds = (VkSparseMemoryBind *)(infos + ninfo);
        uint32_t ni = 0;
        for (uint32_t k = 0; k < in[i].imageOpaqueBindCount; k++) {
            const VkSparseImageOpaqueMemoryBindInfo *b = &in[i].pImageOpaqueBinds[k];
            struct nvhal_astc_image *ai = lookup(b->image);
            if (!ai) {
                infos[ni++] = *b;
                continue;
            }
            const uint32_t nparts = ai->blocks_host ? 1u : 1 + ai->mips; /* host blocks are already bound (task-17) */
            for (uint32_t part = 0; part < nparts; part++) {
                const VkDeviceSize ps = ai->off[part], pe = ps + ai->size[part];
                VkSparseImageOpaqueMemoryBindInfo *t = &infos[ni++];
                *t = (VkSparseImageOpaqueMemoryBindInfo){part ? ai->block[part - 1] : ai->image, 0, binds};
                for (uint32_t j = 0; j < b->bindCount; j++) {
                    const VkSparseMemoryBind *m = &b->pBinds[j];
                    VkDeviceSize s0 = m->resourceOffset > ps ? m->resourceOffset : ps;
                    VkDeviceSize e0 = m->resourceOffset + m->size < pe ? m->resourceOffset + m->size : pe;
                    if (s0 >= e0) continue; /* outside this part (or in the padding between parts) */
                    binds[t->bindCount++] = (VkSparseMemoryBind){
                        s0 - ps, e0 - s0, m->memory,
                        m->memory ? m->memoryOffset + (s0 - m->resourceOffset) : 0, m->flags};
                }
                binds += t->bindCount;
            }
        }
        o[i].imageOpaqueBindCount = ni;
        o[i].pImageOpaqueBinds = infos;
    }
    *out = o;
    *owned = own;
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ commands */

bool nvhal_astc_refuse(VkImage image, const char *what) {
    struct nvhal_astc_image *ai = lookup(image);
    if (!ai) return false;
    NVHAL_ERR("%s names %s image %p (VkFormat %d), which the compressed-format emulation does not implement for this "
              "command; the command is dropped %s", what, fam_name(&ai->e), NVHAL_LOGH(image), ai->format, gap(&ai->e));
    return true;
}

static uint32_t range_mips(const struct nvhal_astc_image *ai, const VkImageSubresourceRange *s, uint32_t *first) {
    uint32_t base = s->baseMipLevel;
    if (base >= ai->mips) return 0;
    uint32_t n = s->levelCount == VK_REMAINING_MIP_LEVELS ? ai->mips - base : s->levelCount;
    if (n > ai->mips - base) n = ai->mips - base;
    *first = base;
    return n;
}

#define ASTC_BARRIERS(fn, T)                                                                                       \
    const T *fn(const T *in, uint32_t n, uint32_t *nout, T **owned) {                                             \
        *owned = NULL;                                                                                            \
        *nout = n;                                                                                                \
        if (!n || !in || nvhal_no_astc()) return in;                                                              \
        uint32_t extra = 0, first = 0;                                                                            \
        for (uint32_t i = 0; i < n; i++) {                                                                        \
            struct nvhal_astc_image *ai = lookup(in[i].image);                                                    \
            if (ai) extra += range_mips(ai, &in[i].subresourceRange, &first);                                     \
        }                                                                                                         \
        if (!extra) return in;                                                                                    \
        T *o = malloc((n + extra) * sizeof *o);                                                                   \
        if (!o) {                                                                                                 \
            NVHAL_ERR("image barrier: out of host memory adding the emulated images' block-image barriers; their " \
                      "layouts now differ from the app's image " R8_TAG " " ETC2_TAG);                            \
            return in;                                                                                            \
        }                                                                                                         \
        memcpy(o, in, n * sizeof *o);                                                                             \
        uint32_t k = n;                                                                                           \
        for (uint32_t i = 0; i < n; i++) {                                                                        \
            struct nvhal_astc_image *ai = lookup(in[i].image);                                                    \
            uint32_t c = ai ? range_mips(ai, &in[i].subresourceRange, &first) : 0;                                \
            for (uint32_t m = first; m < first + c; m++) {                                                        \
                o[k] = in[i];                                                                                     \
                o[k].image = ai->block[m];                                                                        \
                o[k].subresourceRange.baseMipLevel = 0;                                                           \
                o[k].subresourceRange.levelCount = 1;                                                             \
                k++;                                                                                              \
            }                                                                                                     \
        }                                                                                                         \
        *owned = o;                                                                                               \
        *nout = k;                                                                                                \
        return o;                                                                                                 \
    }
ASTC_BARRIERS(nvhal_astc_barriers, VkImageMemoryBarrier)
ASTC_BARRIERS(nvhal_astc_barriers2, VkImageMemoryBarrier2)

static uint32_t layers_of(const struct nvhal_astc_image *ai, const VkImageSubresourceLayers *s) {
    if (s->layerCount != VK_REMAINING_ARRAY_LAYERS) return s->layerCount;
    return s->baseArrayLayer < ai->layers ? ai->layers - s->baseArrayLayer : 0;
}

/* a buffer<->image region of the emulated image as the same region of its block image (texels -> blocks) */
static VkBufferImageCopy region_blocks(const struct nvhal_astc_image *ai, const VkBufferImageCopy *r) {
    VkBufferImageCopy b = *r;
    b.bufferRowLength = div_up(r->bufferRowLength, ai->bw);
    b.bufferImageHeight = div_up(r->bufferImageHeight, ai->bh);
    b.imageSubresource.mipLevel = 0;
    b.imageOffset = (VkOffset3D){r->imageOffset.x / (int32_t)ai->bw, r->imageOffset.y / (int32_t)ai->bh, 0};
    b.imageExtent = (VkExtent3D){div_up(r->imageExtent.width, ai->bw), div_up(r->imageExtent.height, ai->bh), 1};
    return b;
}

struct decode_job {
    struct nvhal_astc_image *ai;
    uint32_t mip, base, count;
    int32_t x, y; /* texels, block-aligned */
    uint32_t w, h;
    VkImageLayout layout; /* the app's layout of the image (and so of the block image) */
};

static void record_bc1(struct nvhal_device *d, VkCommandBuffer cb, const struct decode_job *j);

/* the cached views + descriptor set for (mip, layers). Caller holds a->lock. */
static VkResult slot_get(struct nvhal_device *d, struct nvhal_astc_image *ai, uint32_t mip, uint32_t base, uint32_t count,
                         struct astc_slot *out) {
    for (uint32_t i = 0; i < ai->nslots; i++)
        if (ai->slots[i].mip == mip && ai->slots[i].base == base && ai->slots[i].count == count) {
            *out = ai->slots[i];
            return VK_SUCCESS;
        }
    if (ai->nslots == ai->cap) {
        uint32_t cap = ai->cap ? ai->cap * 2 : 4;
        struct astc_slot *s = realloc(ai->slots, cap * sizeof *s);
        if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
        ai->slots = s;
        ai->cap = cap;
    }
    struct nvhal_astc_dev *a = d->astc;
    struct astc_slot s = {mip, base, count, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    const VkComponentMapping id = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                   VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    VkImageViewCreateInfo sv = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, ai->block[mip],
                                VK_IMAGE_VIEW_TYPE_2D_ARRAY, ai->e.block, id,
                                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, base, count}};
    VkImageViewUsageCreateInfo su = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, VK_IMAGE_USAGE_STORAGE_BIT};
    /* BC1: the decoder writes into the HAL's own RGBA8 tmp image; the app's BC1 image cannot be a storage
     * image (BC1 has no STORAGE_IMAGE feature) and is filled by the encoder + a copyBufferToImage afterwards */
    VkImage dst_img = ai->bc1 ? ai->tmp : ai->image;
    VkImageViewCreateInfo dv = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &su, 0, dst_img, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                ai->e.store, id, {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, base, count}};
    VkResult r = d->xl.CreateImageView(d->handle, &sv, NULL, &s.src);
    if (r == VK_SUCCESS) r = d->xl.CreateImageView(d->handle, &dv, NULL, &s.dst);
    if (r == VK_SUCCESS) r = set_alloc(d, ai->e.fam, &s.set, &s.pool);
    if (r != VK_SUCCESS) {
        if (s.src) d->cs.DestroyImageView(d->handle, s.src, NULL);
        if (s.dst) d->cs.DestroyImageView(d->handle, s.dst, NULL);
        return r;
    }
    /* vk_texcompress_astc_fill_write_descriptor_sets(); the ETC2 decoder has only the first two bindings */
    VkDescriptorImageInfo di = {VK_NULL_HANDLE, s.dst, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo si = {VK_NULL_HANDLE, s.src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[8];
    w[0] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s.set, 0, 0, 1,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &di, NULL, NULL};
    w[1] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s.set, 1, 0, 1,
                                  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &si, NULL, NULL};
    uint32_t nw = 2;
    if (ai->e.fam == FAM_ASTC) {
        for (uint32_t i = 0; i < NVHAL_ASTC_NUM_LUTS; i++)
            w[2 + i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s.set, 2 + i, 0, 1,
                                              VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, &a->lut_view[i]};
        w[7] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s.set, 7, 0, 1,
                                      VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, &a->part_view[ai->table]};
        nw = 8;
    }
    d->cs.UpdateDescriptorSets(d->handle, nw, w, 0, NULL);
    ai->slots[ai->nslots++] = s;
    *out = s;
    return VK_SUCCESS;
}

/* ASTC->BC1: after the decoder has written j's texels into ai->tmp, encode them into BC1 blocks in ai->bc1_buf
 * and copy those blocks into the app's BC1 image.  Runs inside the app's command buffer, like the decode itself. */
static void record_bc1(struct nvhal_device *d, VkCommandBuffer cb, const struct decode_job *j) {
    struct nvhal_astc_image *ai = j->ai;
    struct nvhal_astc_dev *a = d->astc;
    VkPipeline pipe = bc1_encoder(d);
    if (!pipe) return;   /* logged once by bc1_encoder; the RGBA8 texels stay in tmp */

    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, ai->tmp,
                                VK_IMAGE_VIEW_TYPE_2D_ARRAY, ai->e.decoded, {VK_COMPONENT_SWIZZLE_IDENTITY,
                                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                VK_COMPONENT_SWIZZLE_IDENTITY},
                                {VK_IMAGE_ASPECT_COLOR_BIT, j->mip, 1, j->base, j->count}};
    VkImageViewUsageCreateInfo uv = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, VK_IMAGE_USAGE_STORAGE_BIT};
    vi.pNext = &uv;
    VkImageView view = VK_NULL_HANDLE;
    if (d->xl.CreateImageView(d->handle, &vi, NULL, &view) != VK_SUCCESS) {
        NVHAL_ERR("ASTC->BC1: cannot view the tmp image for the encoder %s", R8_TAG);
        return;
    }
    VkDescriptorSetAllocateInfo ai2 = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, a->bc1_pool, 1, &a->bc1_dsl};
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (d->cs.AllocateDescriptorSets(d->handle, &ai2, &set) != VK_SUCCESS) {
        d->cs.DestroyImageView(d->handle, view, NULL);
        NVHAL_ERR("ASTC->BC1: out of descriptor sets for the encoder %s", R8_TAG);
        return;
    }
    VkDescriptorImageInfo di = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi = {ai->bc1_buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &di, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL},
    };
    d->cs.UpdateDescriptorSets(d->handle, 2, w, 0, NULL);

    /* the decoder wrote tmp (COMPUTE) -> the encoder reads it (COMPUTE) */
    VkImageMemoryBarrier pre = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, ai->tmp,
                                {VK_IMAGE_ASPECT_COLOR_BIT, j->mip, 1, j->base, j->count}};
    d->xl.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, NULL, 0, NULL, 1, &pre);
    d->cs.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    d->cs.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, a->bc1_pl, 0, 1, &set, 0, NULL);
    const uint32_t rows = div_up(ai->extent.width, 4);
    const int32_t pc[5] = {(int32_t)div_up(j->x, 4), (int32_t)div_up(j->y, 4),
                           (int32_t)div_up(j->x + (int32_t)j->w, 4), (int32_t)div_up(j->y + (int32_t)j->h, 4), (int32_t)rows};
    d->cs.CmdPushConstants(cb, a->bc1_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 20, pc);
    d->cs.CmdDispatch(cb, div_up(div_up(j->w, 4), 8), div_up(div_up(j->h, 4), 8), j->count);

    /* encoder writes bc1_buf -> the copy reads it (TRANSFER), and the app's BC1 image takes the blocks */
    VkBufferMemoryBarrier bb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT,
                                VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                                ai->bc1_buf, 0, VK_WHOLE_SIZE};
    VkImageMemoryBarrier post = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, ai->image,
                                 {VK_IMAGE_ASPECT_COLOR_BIT, j->mip, 1, j->base, j->count}};
    d->xl.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, NULL, 1, &bb, 1, &post);

    /* one BC1 block per 4x4 texels; bufferRowLength/ImageHeight are in texels, bufferOffset must be 8 B aligned */
    VkBufferImageCopy c;
    memset(&c, 0, sizeof c);
    c.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, j->mip, j->base, j->count};
    c.imageOffset = (VkOffset3D){(int32_t)j->x, (int32_t)j->y, 0};
    c.imageExtent = (VkExtent3D){(uint32_t)j->w, (uint32_t)j->h, 1};
    d->xl.CmdCopyBufferToImage(cb, ai->bc1_buf, ai->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);

    d->cs.FreeDescriptorSets(d->handle, a->bc1_pool, 1, &set);
    d->cs.DestroyImageView(d->handle, view, NULL);
}

static void record_decode(struct nvhal_device *d, VkCommandBuffer cb, const struct decode_job *j, VkPipeline pipe,
                          const struct astc_slot *s) {
    struct nvhal_astc_image *ai = j->ai;
    /* the copy into the block image (TRANSFER) -> the decoder (COMPUTE); the app's own barrier before its copy
     * covers TRANSFER, which chains into this one for the decoded image's earlier accesses */
    VkImageMemoryBarrier pre[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, j->layout,
         VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, ai->block[j->mip],
         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, j->base, j->count}},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT,
         j->layout, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
         ai->bc1 ? ai->tmp : ai->image,
         {VK_IMAGE_ASPECT_COLOR_BIT, j->mip, 1, j->base, j->count}},
    };
    d->xl.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL,
                             2, pre);
    d->cs.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    if (ai->e.fam == FAM_ASTC) {
        d->cs.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d->astc->pl, 0, 1, &s->set, 0, NULL);
        /* radv_meta_astc_decode.c / vn_astc_emu: block coordinates of the start, texel coordinates of the end, 2D */
        const uint32_t pc[5] = {(uint32_t)j->x / ai->bw, (uint32_t)j->y / ai->bh, (uint32_t)j->x + j->w,
                                (uint32_t)j->y + j->h, 0};
        d->cs.CmdPushConstants(cb, d->astc->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, PUSH_SIZE, pc);
        /* each workgroup decodes 2x2 blocks (local size = block size x 4) */
        d->cs.CmdDispatch(cb, div_up(div_up(j->w, ai->bw), 2), div_up(div_up(j->h, ai->bh), 2), j->count);
        if (ai->bc1) record_bc1(d, cb, j);
    } else {
        d->cs.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d->astc->etc2_pl, 0, 1, &s->set, 0, NULL);
        /* etc2_decoder.comp: the texel rectangle [start, end); one invocation per texel, 8x8 per workgroup */
        const int32_t pc[4] = {j->x, j->y, j->x + (int32_t)j->w, j->y + (int32_t)j->h};
        d->cs.CmdPushConstants(cb, d->astc->etc2_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, ETC2_PUSH_SIZE, pc);
        d->cs.CmdDispatch(cb, div_up(j->w, 8), div_up(j->h, 8), j->count);
    }
    /* back to the app's layout; dst stage TRANSFER so that the app's next barrier, whose first scope is the
     * transfer it recorded, chains with the decoder's writes */
    VkImageMemoryBarrier post[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
         VK_IMAGE_LAYOUT_GENERAL, j->layout, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, ai->block[j->mip],
         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, j->base, j->count}},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT,
         VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, j->layout,
         VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, ai->image, {VK_IMAGE_ASPECT_COLOR_BIT, j->mip, 1, j->base, j->count}},
    };
    d->xl.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                             2, post);
}

/* decode the blocks just copied into the block images; then give the app its compute state back */
static void decode_jobs(struct nvhal_device *d, VkCommandBuffer cb, const struct decode_job *jobs, uint32_t n,
                        const char *who) {
    if (!n) return;
    const struct efmt *e0 = &jobs[0].ai->e;
    struct nvhal_cmd *e = nvhal_cmd_get(cb);
    uint32_t family = e ? e->family : UINT32_MAX;
    if (family == UINT32_MAX || family >= d->astc_pd->nq || !(d->astc_pd->qflags[family] & VK_QUEUE_COMPUTE_BIT)) {
        NVHAL_ERR("%s: %s image %p: the compressed blocks were copied, but command buffer %p belongs to queue family %d, "
                  "which has no VK_QUEUE_COMPUTE_BIT (flags 0x%x); the decode needs compute, the texels stay undecoded %s",
                  who, fam_name(e0), NVHAL_LOGH(jobs[0].ai->image), (void *)cb, (int)family,
                  family < d->astc_pd->nq ? d->astc_pd->qflags[family] : 0, gap(e0));
        return;
    }
    if (nvhal_cs_conditional(cb))
        NVHAL_ERR("%s: %s image %p: the upload is recorded inside conditional rendering (command buffer %p); the "
                  "decode dispatch obeys the app's predicate while the copy does not, so a false predicate leaves the "
                  "texels undecoded %s", who, fam_name(e0), NVHAL_LOGH(jobs[0].ai->image), (void *)cb, gap(e0));
    struct { VkPipeline pipe; struct astc_slot slot; } st[8], *res = n <= 8 ? st : malloc(n * sizeof *res);
    if (!res) {
        NVHAL_ERR("%s: out of host memory; %s decode dropped %s", who, fam_name(e0), gap(e0));
        return;
    }
    struct nvhal_astc_dev *a = d->astc;
    pthread_mutex_lock(&a->lock);
    for (uint32_t i = 0; i < n; i++) {
        const struct efmt *je = &jobs[i].ai->e;
        if (je->fam == FAM_ASTC)
            res[i].pipe = decoder_init(d) == VK_SUCCESS ? decoder_pipeline(d, je->cls) : VK_NULL_HANDLE;
        else
            res[i].pipe = etc2_decoder_init(d) == VK_SUCCESS ? etc2_pipeline(d, je->variant) : VK_NULL_HANDLE;
        VkResult rs = VK_ERROR_INITIALIZATION_FAILED;
        if (res[i].pipe) rs = slot_get(d, jobs[i].ai, jobs[i].mip, jobs[i].base, jobs[i].count, &res[i].slot);
        if (res[i].pipe && rs != VK_SUCCESS) {
            NVHAL_ERR("%s: %s image %p mip %u layers %u+%u: decoder views/descriptor set -> %s; this region stays "
                      "undecoded %s", who, fam_name(je), NVHAL_LOGH(jobs[i].ai->image), jobs[i].mip, jobs[i].base,
                      jobs[i].count, NVHAL_VKR(rs), gap(je));
            res[i].pipe = VK_NULL_HANDLE;
        }
    }
    pthread_mutex_unlock(&a->lock);
    bool any = false;
    for (uint32_t i = 0; i < n; i++)
        if (res[i].pipe && jobs[i].w && jobs[i].h && jobs[i].count) {
            record_decode(d, cb, &jobs[i], res[i].pipe, &res[i].slot);
            any = true;
            NVHAL_TRACE("%s: %s image %p mip %u layers %u+%u texels (%d,%d) %ux%u decoded in command buffer %p", who,
                        fam_name(&jobs[i].ai->e), NVHAL_LOGH(jobs[i].ai->image), jobs[i].mip, jobs[i].base,
                        jobs[i].count, jobs[i].x, jobs[i].y, jobs[i].w, jobs[i].h, (void *)cb);
        }
    if (res != st) free(res);
    if (any) nvhal_cs_restore(d, cb, who);
}

static bool mip_ok(const struct nvhal_astc_image *ai, uint32_t mip, const char *who) {
    if (mip < ai->mips) return true;
    NVHAL_ERR("%s: mip level %u of %s image %p, which has %u; region skipped", who, mip, fam_name(&ai->e),
              NVHAL_LOGH(ai->image), ai->mips);
    return false;
}

bool nvhal_astc_cmd_copy_buffer_to_image(struct nvhal_device *d, VkCommandBuffer cb, VkBuffer src, VkImage dst,
                                         VkImageLayout layout, uint32_t n, const VkBufferImageCopy *r) {
    static const char who[] = "vkCmdCopyBufferToImage(compressed-format emulation)";
    struct nvhal_astc_image *ai = lookup(dst);
    if (!ai) return false;
    struct decode_job st[8] = {0}, *jobs = n <= 8 ? st : malloc(n * sizeof *jobs);
    if (!jobs) {
        NVHAL_ERR("%s: out of host memory; copy dropped %s", who, gap(&ai->e));
        return true;
    }
    uint32_t nj = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t mip = r[i].imageSubresource.mipLevel;
        if (!mip_ok(ai, mip, who)) continue;
        VkBufferImageCopy b = region_blocks(ai, &r[i]);
        d->xl.CmdCopyBufferToImage(cb, src, ai->block[mip], layout, 1, &b);
        jobs[nj++] = (struct decode_job){ai, mip, r[i].imageSubresource.baseArrayLayer, layers_of(ai, &r[i].imageSubresource),
                                         r[i].imageOffset.x, r[i].imageOffset.y, r[i].imageExtent.width,
                                         r[i].imageExtent.height, layout};
    }
    decode_jobs(d, cb, jobs, nj, who);
    if (jobs != st) free(jobs);
    return true;
}

bool nvhal_astc_cmd_copy_image_to_buffer(struct nvhal_device *d, VkCommandBuffer cb, VkImage src, VkImageLayout layout,
                                         VkBuffer dst, uint32_t n, const VkBufferImageCopy *r) {
    struct nvhal_astc_image *ai = lookup(src);
    if (!ai) return false;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t mip = r[i].imageSubresource.mipLevel;
        if (!mip_ok(ai, mip, "vkCmdCopyImageToBuffer(compressed-format emulation)")) continue;
        VkBufferImageCopy b = region_blocks(ai, &r[i]);
        d->xl.CmdCopyImageToBuffer(cb, ai->block[mip], layout, dst, 1, &b);
    }
    return true;
}

bool nvhal_astc_cmd_copy_image(struct nvhal_device *d, VkCommandBuffer cb, VkImage src, VkImageLayout srcL, VkImage dst,
                               VkImageLayout dstL, uint32_t n, const VkImageCopy *r) {
    static const char who[] = "vkCmdCopyImage(compressed-format emulation)";
    struct nvhal_astc_image *sa = lookup(src), *da = lookup(dst);
    if (!sa && !da) return false;
    /* the same decode class on both sides (the same ASTC block size, or the same ETC2/EAC decoder variant): the
     * decoded texels are copied as the app's regions say. Otherwise the destination's blocks are decoded: blocks of
     * another family or variant decode to other texels (size-compatible compressed formats) */
    const bool texels = sa && da && sa->table == da->table;
    if (texels) d->xl.CmdCopyImage(cb, src, srcL, dst, dstL, n, r);
    const bool decode = da && !texels;
    struct decode_job st[8] = {0}, *jobs = st;
    if (decode && n > 8 && !(jobs = malloc(n * sizeof *jobs))) {
        NVHAL_ERR("%s: out of host memory; copy dropped %s", who, gap(&da->e));
        return true;
    }
    uint32_t nj = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkImageCopy b = r[i];
        VkImage bs = src, bd = dst;
        if (sa) { /* source texels -> blocks; the extent is the source's (spec: compressed <-> uncompressed copies) */
            if (!mip_ok(sa, r[i].srcSubresource.mipLevel, who)) continue;
            bs = sa->block[r[i].srcSubresource.mipLevel];
            b.srcSubresource.mipLevel = 0;
            b.srcOffset = (VkOffset3D){r[i].srcOffset.x / (int32_t)sa->bw, r[i].srcOffset.y / (int32_t)sa->bh, 0};
            b.extent = (VkExtent3D){div_up(r[i].extent.width, sa->bw), div_up(r[i].extent.height, sa->bh), 1};
        }
        if (da) {
            if (!mip_ok(da, r[i].dstSubresource.mipLevel, who)) continue;
            bd = da->block[r[i].dstSubresource.mipLevel];
            b.dstSubresource.mipLevel = 0;
            b.dstOffset = (VkOffset3D){r[i].dstOffset.x / (int32_t)da->bw, r[i].dstOffset.y / (int32_t)da->bh, 0};
        }
        d->xl.CmdCopyImage(cb, bs, srcL, bd, dstL, 1, &b);
        if (decode) { /* the source's texels or blocks are blocks of the destination: decode what they cover */
            const uint32_t mip = r[i].dstSubresource.mipLevel;
            uint32_t mw = mip_dim(da->extent.width, mip), mh = mip_dim(da->extent.height, mip);
            uint32_t x = (uint32_t)r[i].dstOffset.x, y = (uint32_t)r[i].dstOffset.y;
            /* the extent is in source texels: blocks of an uncompressed source, texels of a compressed one */
            uint32_t w = sa ? div_up(r[i].extent.width, sa->bw) * da->bw : r[i].extent.width * da->bw;
            uint32_t h = sa ? div_up(r[i].extent.height, sa->bh) * da->bh : r[i].extent.height * da->bh;
            if (x + w > mw) w = mw > x ? mw - x : 0;
            if (y + h > mh) h = mh > y ? mh - y : 0;
            jobs[nj++] = (struct decode_job){da, mip, r[i].dstSubresource.baseArrayLayer,
                                             layers_of(da, &r[i].dstSubresource), (int32_t)x, (int32_t)y, w, h, dstL};
        }
    }
    decode_jobs(d, cb, jobs, nj, who);
    if (jobs != st) free(jobs);
    return true;
}

/* the *2 commands: same work; a pNext the emulation does not know is refused */
static bool regions2_plain(const void *info_next, const void *regions, uint32_t n, size_t stride, const char *who) {
    bool ok = info_next == NULL;
    for (uint32_t i = 0; i < n && ok; i++) ok = ((const VkBaseInStructure *)((const char *)regions + i * stride))->pNext == NULL;
    if (!ok) NVHAL_ERR("%s: a pNext structure on the copy or a region is not implemented by the compressed-format "
                       "emulation; the command is dropped " R8_TAG " " ETC2_TAG, who);
    return ok;
}

bool nvhal_astc_cmd_copy_buffer_to_image2(struct nvhal_device *d, VkCommandBuffer cb, const VkCopyBufferToImageInfo2 *info) {
    if (!lookup(info->dstImage)) return false;
    if (!regions2_plain(info->pNext, info->pRegions, info->regionCount, sizeof *info->pRegions, "vkCmdCopyBufferToImage2"))
        return true;
    VkBufferImageCopy st[8] = {0}, *r = info->regionCount <= 8 ? st : malloc(info->regionCount * sizeof *r);
    if (!r) return true;
    for (uint32_t i = 0; i < info->regionCount; i++) {
        const VkBufferImageCopy2 *s = &info->pRegions[i];
        r[i] = (VkBufferImageCopy){s->bufferOffset, s->bufferRowLength, s->bufferImageHeight, s->imageSubresource,
                                   s->imageOffset, s->imageExtent};
    }
    nvhal_astc_cmd_copy_buffer_to_image(d, cb, info->srcBuffer, info->dstImage, info->dstImageLayout, info->regionCount, r);
    if (r != st) free(r);
    return true;
}

bool nvhal_astc_cmd_copy_image_to_buffer2(struct nvhal_device *d, VkCommandBuffer cb, const VkCopyImageToBufferInfo2 *info) {
    if (!lookup(info->srcImage)) return false;
    if (!regions2_plain(info->pNext, info->pRegions, info->regionCount, sizeof *info->pRegions, "vkCmdCopyImageToBuffer2"))
        return true;
    VkBufferImageCopy st[8] = {0}, *r = info->regionCount <= 8 ? st : malloc(info->regionCount * sizeof *r);
    if (!r) return true;
    for (uint32_t i = 0; i < info->regionCount; i++) {
        const VkBufferImageCopy2 *s = &info->pRegions[i];
        r[i] = (VkBufferImageCopy){s->bufferOffset, s->bufferRowLength, s->bufferImageHeight, s->imageSubresource,
                                   s->imageOffset, s->imageExtent};
    }
    nvhal_astc_cmd_copy_image_to_buffer(d, cb, info->srcImage, info->srcImageLayout, info->dstBuffer, info->regionCount, r);
    if (r != st) free(r);
    return true;
}

bool nvhal_astc_cmd_copy_image2(struct nvhal_device *d, VkCommandBuffer cb, const VkCopyImageInfo2 *info) {
    if (!lookup(info->srcImage) && !lookup(info->dstImage)) return false;
    if (!regions2_plain(info->pNext, info->pRegions, info->regionCount, sizeof *info->pRegions, "vkCmdCopyImage2"))
        return true;
    VkImageCopy st[8] = {0}, *r = info->regionCount <= 8 ? st : malloc(info->regionCount * sizeof *r);
    if (!r) return true;
    for (uint32_t i = 0; i < info->regionCount; i++) {
        const VkImageCopy2 *s = &info->pRegions[i];
        r[i] = (VkImageCopy){s->srcSubresource, s->srcOffset, s->dstSubresource, s->dstOffset, s->extent};
    }
    nvhal_astc_cmd_copy_image(d, cb, info->srcImage, info->srcImageLayout, info->dstImage, info->dstImageLayout,
                              info->regionCount, r);
    if (r != st) free(r);
    return true;
}

/* ------------------------------------------------------------------ teardown */

static bool of_device(void *v, void *arg) { return ((struct nvhal_astc_image *)v)->dev == arg; }
static void leaked_image(void *v) {
    struct nvhal_astc_image *ai = v;
    atomic_fetch_sub_explicit(&n_images, 1, memory_order_release);
    NVHAL_WARN("vkDestroyDevice: %s image %p was not destroyed by the app; its HAL objects are freed now",
               fam_name(&ai->e), NVHAL_LOGH(ai->image));
    destroy_parts(ai->dev, ai, NULL, false); /* the decoded image is the app's (leaked) object */
}

void nvhal_astc_device_teardown(struct nvhal_device *d) {
    abs_finish(); /* the cumulative compressed-format asset line; no-op unless NVHAL_ASTC_BYTES is on */
    bh_report();  /* task-17: one line when astc_blocks_host is on and something was emulated */
    struct nvhal_astc_dev *a = d->astc;
    if (!a) return;
    nvhal_hash_sweep(&images, of_device, d, leaked_image);
    VkDevice dev = d->handle;
    for (unsigned i = 0; i < NVHAL_ASTC_NUM_PARTITION_TABLES; i++)
        if (a->pipe[i]) d->cs.DestroyPipeline(dev, a->pipe[i], NULL);
    for (uint32_t i = 0; i < a->npools; i++) d->cs.DestroyDescriptorPool(dev, a->pools[i], NULL);
    for (unsigned i = 0; i < ETC2_VARIANTS; i++)
        if (a->etc2_pipe[i]) d->cs.DestroyPipeline(dev, a->etc2_pipe[i], NULL);
    for (uint32_t i = 0; i < a->etc2_npools; i++) d->cs.DestroyDescriptorPool(dev, a->etc2_pools[i], NULL);
    if (a->etc2_pl) d->cs.DestroyPipelineLayout(dev, a->etc2_pl, NULL);
    if (a->etc2_dsl) d->cs.DestroyDescriptorSetLayout(dev, a->etc2_dsl, NULL);
    if (a->etc2_sm) d->cs.DestroyShaderModule(dev, a->etc2_sm, NULL);
    free(a->etc2_pools);
    if (a->pl) d->cs.DestroyPipelineLayout(dev, a->pl, NULL);
    if (a->dsl) d->cs.DestroyDescriptorSetLayout(dev, a->dsl, NULL);
    if (a->sm) d->cs.DestroyShaderModule(dev, a->sm, NULL);
    for (unsigned i = 0; i < NVHAL_ASTC_NUM_LUTS; i++)
        if (a->lut_view[i]) d->cs.DestroyBufferView(dev, a->lut_view[i], NULL);
    for (unsigned i = 0; i < NVHAL_ASTC_NUM_PARTITION_TABLES; i++)
        if (a->part_view[i]) d->cs.DestroyBufferView(dev, a->part_view[i], NULL);
    if (a->lut_buf) d->cs.DestroyBuffer(dev, a->lut_buf, NULL);
    if (a->lut_mem) d->FreeMemory(dev, a->lut_mem, NULL);
    free(a->pools);
    pthread_mutex_destroy(&a->lock);
    free(a);
    d->astc = NULL;
}
