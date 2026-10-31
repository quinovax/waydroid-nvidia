/* The ASTC->BC1 encoder compute shader as SPIR-V words. out/gen/bc1_encode_spv.h is produced at build time by
 *   glslangValidator -V --target-env vulkan1.0 -S comp -x -o out/gen/bc1_encode_spv.h src/bc1_encode.comp
 * (Makefile), validated with spirv-val; the glslang version that compiled it is recorded in
 * out/gen/bc1_encode_spv.version. Nothing here is checked in as a binary. */
#include "nvhal_astc.h"

const uint32_t nvhal_bc1_encode_spv[] = {
#include "bc1_encode_spv.h"
};
const size_t nvhal_bc1_encode_spv_size = sizeof nvhal_bc1_encode_spv;
