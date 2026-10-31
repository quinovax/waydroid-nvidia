import sys, pathlib
src = pathlib.Path(sys.argv[1]).read_text(encoding='utf-8')
out = []
key = 'static const uint8_t block_dims'
i = src.find(key)
if i < 0: sys.exit('no block_dims')
j = src.find(';', i)
out.append(src[i:j+1])
key2 = 'static bool efmt_get(VkFormat f, struct efmt *e) {'
i = src.find(key2)
if i < 0: sys.exit('no efmt_get; looked for: ' + key2)
b = src.find('{', i)
depth = 0
k = b
while k < len(src):
    c = src[k]
    if c == '{': depth += 1
    elif c == '}':
        depth -= 1
        if depth == 0: break
    k += 1
out.append(src[i:k+1])
out.append('')
out.append('/* derived from src/astc.c: the switch decision and the block-count math */')
out.append('static bool bc1_decide(bool on, VkFormat f) {')
out.append('    struct efmt e;')
out.append('    return on && efmt_get(f, &e) && e.bc1 != VK_FORMAT_UNDEFINED;')
out.append('}')
out.append('static uint64_t bc1_blocks(uint32_t w, uint32_t h, uint32_t mips, uint32_t layers) {')
out.append('    uint64_t n = 0;')
out.append('    for (uint32_t m = 0; m < mips; m++) {')
out.append('        const uint32_t mw = (w >> m) ? (w >> m) : 1, mh = (h >> m) ? (h >> m) : 1;')
out.append('        n += (uint64_t)((mw + 3) / 4) * (uint64_t)((mh + 3) / 4) * layers;')
out.append('    }')
out.append('    return n;')
out.append('}')
out.append('static uint64_t bc1_bytes(uint64_t blocks) { return blocks * 8u; }')
pathlib.Path(sys.argv[2]).write_text(chr(10).join(out) + chr(10), encoding='utf-8')
print('wrote', sys.argv[2])