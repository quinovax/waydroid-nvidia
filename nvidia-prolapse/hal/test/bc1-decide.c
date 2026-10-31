/* Does the ASTC->BC1 path pick the right image and format, without a GPU?
 *
 * The expensive parts of the switch are the decisions, not the Vulkan calls:
 *   - only ASTC 4x4 can map onto BC1 (BC1 is a 4x4 format), everything else must
 *     keep the RGBA8 decoded image;
 *   - when the switch is on and the format qualifies, the app's image is a BC1
 *     image and the decoder targets a separate RGBA8 tmp image;
 *   - when the switch is off, nothing changes at all;
 *   - when the memory type the BC1 path needs does not exist, the create must
 *     fall back instead of failing.
 *
 * This file mirrors those functions by including the same source text through
 * the 'bc1-plan' make target, the same trick test/astc-blocks-host.c uses.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#define MAX_MIPS 16
#define NVHAL_ASTC_NUM_PARTITION_TABLES 14
#define R8_TAG "(ASTC gap, R8)"
#define ETC2_TAG "(ETC2 gap)"

/* the format table, verbatim from src/astc.c's block_dims + efmt shape */
struct efmt {
    uint8_t fam;
    uint8_t bw, bh;
    uint8_t cls;
    uint8_t variant;
    bool twin;
    VkFormat decoded;
    VkFormat store;
    VkFormat block;
    VkFormat bc1;
};
enum { FAM_ASTC, FAM_ETC2 };
#define ASTC_FIRST VK_FORMAT_ASTC_4x4_UNORM_BLOCK
#define ASTC_LAST  VK_FORMAT_ASTC_12x12_SRGB_BLOCK
#define ETC2_FIRST VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK
#define ETC2_LAST  VK_FORMAT_EAC_R11G11_SNORM_BLOCK

#include "bc1-decide.inc"

static int fails;
static void ck(const char *what, long long got, long long want) {
    if (got != want) { printf("FAIL %-52s got %lld want %lld\n", what, got, want); fails++; }
    else printf("ok   %-52s %lld\n", what, got);
}

int main(void) {
    struct efmt e;

    /* 1. only ASTC 4x4 gets a BC1 target */
    ck("4x4 UNORM has BC1", efmt_get(VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &e) && e.bc1 != VK_FORMAT_UNDEFINED, 1);
    ck("  and it is BC1_RGBA_UNORM", e.bc1 == VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 1);
    ck("4x4 SRGB has BC1", efmt_get(VK_FORMAT_ASTC_4x4_SRGB_BLOCK, &e) && e.bc1 != VK_FORMAT_UNDEFINED, 1);
    ck("  and it is BC1_RGBA_SRGB", e.bc1 == VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 1);

    /* 2. every other ASTC block size keeps RGBA8 */
    static const VkFormat other[] = {
        VK_FORMAT_ASTC_5x4_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_UNORM_BLOCK,
        VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_UNORM_BLOCK,
    };
    for (unsigned i = 0; i < sizeof other / sizeof *other; i++) {
        char b[64];
        snprintf(b, sizeof b, "format %d keeps no BC1 target", (int)other[i]);
        ck(b, efmt_get(other[i], &e) && e.bc1 != VK_FORMAT_UNDEFINED, 0);
    }

    /* 3. ETC2/EAC never gets one */
    ck("ETC2 RGB8 has no BC1", efmt_get(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, &e) && e.bc1 != VK_FORMAT_UNDEFINED, 0);
    ck("EAC R11 has no BC1", efmt_get(VK_FORMAT_EAC_R11_UNORM_BLOCK, &e) && e.bc1 != VK_FORMAT_UNDEFINED, 0);

    /* 4. the decode classes still line up for 4x4 (the twin shares cls 0) */
    ck("4x4 UNORM cls is 0", efmt_get(VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &e) && e.cls == 0, 1);
    ck("4x4 SRGB cls is 0", efmt_get(VK_FORMAT_ASTC_4x4_SRGB_BLOCK, &e) && e.cls == 0, 1);

    /* 5. the switch decision: on + qualifying format -> BC1, otherwise RGBA8 */
    ck("decide(on,4x4) -> bc1", bc1_decide(true, VK_FORMAT_ASTC_4x4_UNORM_BLOCK), 1);
    ck("decide(off,4x4) -> rg8", bc1_decide(false, VK_FORMAT_ASTC_4x4_UNORM_BLOCK), 0);
    ck("decide(on,6x6) -> rg8", bc1_decide(true, VK_FORMAT_ASTC_6x6_UNORM_BLOCK), 0);

    /* 6. block count math: 3 mips of a 64x64 4x4 ASTC, 1 layer
     *    256x256 texels -> 64x64 blocks = 4096  (mip0)  ... 16x16 blocks = 256
     *    128x128 texels -> 32x32 blocks = 1024  (mip1)  ...  8x8  blocks = 64
     *     64x64  texels -> 16x16 blocks =  256  (mip2)  ...  4x4  blocks = 16
     *    nope: 64x64 is the *image*, so 16/8/4 blocks per side -> 256+64+16 = 336 */
    ck("blocks(64x64,3 mips,1 layer)", bc1_blocks(64, 64, 3, 1), 336);
    ck("blocks(2560x1440,1 mip,1 layer)", bc1_blocks(2560, 1440, 1, 1), 640 * 360);
    ck("blocks bytes are 8 per block", bc1_bytes(21), 168);

    printf("%s (%d failure(s))\n", fails ? "BC1 DECIDE TEST FAILED" : "BC1 DECIDE TEST PASSED", fails);
    return fails != 0;
}
