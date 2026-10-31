/* Data of the ASTC LDR decoder (astc.c), all generated at build time from the pinned Mesa commit
 * (Makefile MESA_SHA):
 *   nvhal_astc_luts / nvhal_astc_partition_tables  tools/astc-luts-gen.cpp over Mesa's
 *       src/util/texcompress_astc_luts.cpp (out/gen/nvhal_astc_luts.c)
 *   nvhal_astc_spv  Mesa's src/compiler/glsl/astc_decoder.glsl compiled by glslangValidator exactly as
 *       Mesa's src/vulkan/runtime/meson.build does (-V -S comp -x; out/gen/astc_spv.h, src/astc_spv.c) */
#ifndef NVHAL_ASTC_H
#define NVHAL_ASTC_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/* vk_texcompress_astc.h: LUT order matches the shader's bindings 2..6, then the partition table (7) */
#define NVHAL_ASTC_NUM_LUTS 5
#define NVHAL_ASTC_NUM_PARTITION_TABLES 14

struct nvhal_astc_lut {
    const uint8_t *data;
    size_t size;     /* bytes */
    VkFormat format; /* texel format of its uniform texel buffer view */
};
extern const struct nvhal_astc_lut nvhal_astc_luts[NVHAL_ASTC_NUM_LUTS];
/* index = (VkFormat - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) / 2, i.e. 4x4, 5x4, ... 12x12 */
extern const struct nvhal_astc_lut nvhal_astc_partition_tables[NVHAL_ASTC_NUM_PARTITION_TABLES];

extern const uint32_t nvhal_astc_spv[];
extern const size_t nvhal_astc_spv_size; /* bytes */

/* the ETC2/EAC decoder: src/etc2_decoder.comp compiled by glslangValidator -V -S comp -x (out/gen/etc2_spv.h,
 * src/etc2_spv.c) */
extern const uint32_t nvhal_etc2_spv[];
extern const size_t nvhal_etc2_spv_size; /* bytes */

/* the ASTC->BC1 encoder (src/bc1_encode.comp, out/gen/bc1_encode_spv.h, src/bc1_spv.c) */
extern const uint32_t nvhal_bc1_encode_spv[];
extern const size_t nvhal_bc1_encode_spv_size; /* bytes */

#endif
