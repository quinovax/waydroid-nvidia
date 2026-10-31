# ASTC → BC1 仿真目标格式改造（task-18 设计草案）

## 为什么

Run A（artifacts/bench3d-20261025T205640-gl-halstats-A）一手数据：588 张 ASTC 全部仿真，app 可见峰值 **7.06 GiB** = 解码 RGBA8 **5.90 GiB** + block 图 1.22 GiB，而等价的 native 压缩数据只有 1.16 GiB。8 GiB 显存被填到 99%，渲染期 119 次 OOM、1352 MiB 落宿主内存，每帧 2.9 GB 走 PCIe ⇒ 1.2-1.6 fps。

把「采样目标」从 RGBA8（4 B/texel）换成 **BC1（0.5 B/texel）**：

| 项 | 现在 | BC1 |
|---|---|---|
| 采样图显存 | 5.90 GiB | **≈0.74 GiB** |
| 采样带宽 | 4 B/texel | **0.5 B/texel**（硬件过滤，8x 更少） |
| block 图（ASTC 原始位） | 1.22 GiB（VRAM，task-17 后移宿主） | 不变 |
| 合计 VRAM | 7.12 GiB | **≈0.74 GiB** |

## 硬约束

1. **BC 格式不能做 storage image**（Vulkan 规定），所以编码结果必须先写进一个普通 `VkBuffer`，再 `vkCmdCopyBufferToImage` 进 BC1 图（BC1 图需带 `TRANSFER_DST`；ASTC 的 app usage 里本来就有 transfer 位）。
2. **readback 必须仍逐位往返**：app 若 `vkCmdCopyImageToBuffer` 读压缩数据，HAL 应继续从 **block 图**（原始 ASTC 位）拷贝，而不是从 BC1 图 —— 所以 block 图必须保留（task-17 已把它移到宿主内存）。
3. BC1 的块恒为 4x4；ASTC 可能是 4x4/5x4/…/12x12。**先只做 ASTC 4x4**（实测 4x4 x378、6x6 x210；4x4 是绝大多数），其余 block size 继续走 RGBA8 路径。这样 4x4 用例可以做到「一个 ASTC 块 ↔ 一个 BC1 块」的 1:1 映射，最简单也最快。
4. **失败必须回退**：BC1 特性不可用 / 分配失败 / 编码失败 → 打 NVHAL_ERR 并回退到现有 RGBA8 路径（不能满足的用例就直接不给 BC1）。

## 可行性（宿主已验）

- 宿主 vulkaninfo（同一 580.178.04 + GTX 1080）：`textureCompressionBC = true`、`shaderStorageImageWriteWithoutFormat = true`。BC1 是桌面 NVIDIA 的必备格式（GTX 1080 全系支持）。落地前仍需用 `vulkaninfo`/`vkGetPhysicalDeviceFormatProperties` 确认 `BC1_RGBA_UNORM(_SRGB)_BLOCK` 的 optimalTilingFeatures 含 `SAMPLED_IMAGE|TRANSFER_SRC|TRANSFER_DST|BLIT_SRC|MUTABLE_FORMAT|EXTENDED_USAGE`。

## 实现路径（建议两段式，先不碰 Mesa 的 ASTC 解码器）

现有解码链：`block 图 --(Mesa astc_decoder.glsl, unorm8 模式)--> app 的解码 RGBA8 图`（每 mip 一次 dispatch）。

BC1 链：

1. `block 图 --(同一个解码器)--> **临时 RGBA8 图**`（HAL 私有，按需扩大，峰值几十 MiB，可复用）
2. `临时 RGBA8 图 --(新 shader bc1_encode.comp)--> **BC1 块 VkBuffer**`（每线程一个 4x4 块：读 16 texel，算 min/max 端点，量化到 5:6:5，4 色索引）
3. `vkCmdCopyBufferToImage(BC1 块 buffer → BC1 图)`（bufferRowLength 按 texel 计，bufferOffset 对齐块大小 8 B）

这样完全复用现有 ASTC 解码器（不写 CPU 解码器），只新增一个 ~60 行的 GLSL 编码器和一个临时图。

## 代码落点（nvidia-prolapse/hal）

- `src/bc1_encode.comp`（新）+ Makefile 里照 `out/gen/etc2_spv.h` 的写法生成 `out/gen/bc1_encode_spv.h`，再按 `src/etc2_spv.c` 的样子加 `src/bc1_spv.c`。
- `src/astc.c`：
  - `struct efmt` 增加 BC1 目标（`decoded` 字段换成 BC1 格式；或新增 `bc1`/`sampled` 字段），`efmt_get()` 只对 **4x4 ASTC** 返回 BC1 目标；
  - 建图：app 的图像仍按 ASTC 创建，HAL 内部创建 BC1 图（usage = 原 usage | TRANSFER_DST | SAMPLED），view 用 BC1 格式；
  - 上传拦截（`nvhal_astc_cmd_copy_buffer_to_image` 等）：先按现有逻辑把块写进 block 图，再走上面 1-2-3；
  - `vkCmdCopyImage(ASTC→ASTC)`、重新上传 → 重跑 1-2-3；
  - 销毁：释放 BC1 图/临时图/buffer；
  - 开关 `debug.nvhal.astc_bc1`（默认 0），关闭时行为与现在完全一致。

## 验收

1. 四产物构建 exit 0、`-Wall -Wextra -Werror` 干净、strings 含 `astc_bc1`。
2. host 侧无 GPU 镜像测试：4x4 ASTC 走 BC1 分支、6x6 走 RGBA8 分支、BC1 特性缺失时回退、编码器对纯色块/渐变块的单测（可用 test/ 现有模式）。
3. 实跑判据（Run E）：`fb.used` 峰值应从 8105 MiB 降到 ~2000-3000 MiB；`astc bytes` 行里 `decoded` 的体积换成 BC1 体积；分数应显著上升（这是把分数推向 >3000 的主攻方向）。

---

## 实现状态（2026-10-31 20:40）

### 已完成并编译通过

1. **编码器 shader** `src/bc1_encode.comp`：每个 invocation 产出 4x4 BC1 块（端点 565 + 16 个 2-bit 索引，4 色模式）。
   - `glslangValidator -V --target-env vulkan1.0 -S comp` 通过；`spirv-val` 通过。
   - Makefile 已接线：`out/gen/bc1_encode_spv.h` 可生成（实测 20556 B），已加入 `ASTC_GEN` 依赖。
2. **格式选择**：`struct efmt` 新增 `bc1` 字段；`efmt_get()` **只对 ASTC 4x4（block_dims 表索引 0）**给出 `BC1_RGBA_UNORM/SRGB_BLOCK`，其余（含 6x6 与全部 ETC2/EAC）为 `VK_FORMAT_UNDEFINED` 并继续走 RGBA8。
3. **build_cis**：开关开启且该格式有 BC1 目标时，`c->bc1 = true`；app 的图改为 BC1 格式（去掉 STORAGE，保留 TRANSFER_DST/SRC + SAMPLED + MUTABLE_FORMAT），同时准备 `c->rgba2`（解码器专用的 RGBA8 storage 图）。
4. **vkCreateImage**：BC1 模式下额外创建 `ai->tmp`（RGBA8 storage 图）；把 tmp 与 BC1 块 buffer 都分配到 **HAL 自有宿主内存**（它们不被采样器读，只在 decoder/encoder 之间流动）。块数按每 mip 的 `ceil(w/4)*ceil(h/4)*layers` 累加，每块 8 B。
5. **失败回退**：BC1 路径任何一步失败（无可用宿主类型、分配失败、绑定失败）都 `NVHAL_ERR` 报错、释放已分配的资源、把 `ai->bc1` 置 false 并回到 RGBA8 路径——**这个开关永远不会让一个 vkCreateImage 失败**。
6. **校准的入口点访问路径**（踩过的坑，记录以备后人）：buffer 相关函数分属两个结构体——
   - `d->xl.CreateBuffer`（在 `NVHAL_XL_FNS_CORE`）；
   - `d->cs.GetBufferMemoryRequirements` / `d->cs.BindBufferMemory` / `d->cs.DestroyBuffer`（在 `NVHAL_ASTC_FNS`，而 `nvhal_cs_fns` 同时展开 CS_FNS + ASTC_FNS）。
   弄混会得到 "has no member named ...; did you mean ..." 一类编译错误。

### 已完成（2026-10-31 21:00）

- **解码目标改到 `ai->tmp`**：`slot_get` 的 dst view 与 `record_decode` 的发布屏障都按 `ai->bc1` 选择 tmp 或 app 图。
- **`bc1_encoder()`**：惰性创建 descriptor set layout（binding 0 = tmp 的 storage image，binding 1 = `bc1_buf` 的 SSBO）、pipeline layout（20 B push constants）、shader module（`nvhal_bc1_encode_spv`）、compute pipeline、descriptor pool。失败只记录一次并返回 NULL。
- **`record_bc1()`**：解码 dispatch 之后依次做 tmp 的 COMPUTE 到 COMPUTE 屏障、bind、allocate descriptor set、update、`CmdDispatch`（每 workgroup 8x8 块）、buffer COMPUTE 到 TRANSFER 与 app 图 UNDEFINED 到 TRANSFER_DST 的屏障、`CmdCopyBufferToImage`、释放 descriptor set 与 view。
- **构建**：`src/bc1_spv.c` 包裹 `out/gen/bc1_encode_spv.h`（裸数组字面量，与 `etc2_spv.c` 同一写法），`nvhal_astc.h` 声明 `nvhal_bc1_encode_spv` 与 `_size`，Makefile 的 `SRCS` 加入 `src/bc1_spv.c`。
- **实测**：`out/android` 2500968 B（`astc_bc1` 命中 1）、`out/android32` 2290224 B（命中 1），均为 2026-10-31 21:00 构建，`-Wall -Wextra -Werror` 零告警。

### 仍然缺的（诚实）

1. **view 翻译未做**：`nvhal_astc_CreateImageView` 仍把 app 的 ASTC view 转到 `e.decoded`（RGBA8），而 BC1 模式下 app 的图是 BC1。**这一条不补，开启开关会得到错误的采样结果**，所以开关必须保持默认 0。
2. **销毁路径未补**：`destroy_parts` 还没释放 `tmp`、`tmp_mem`、`bc1_buf`、`bc1_mem` 以及 encoder 对象（会泄漏）。
3. **未做镜像测试、未实机验证、未部署**。
4. `out/host` 与 `out/host32` 的 strings 里没有 `astc_bc1`（宿主编译走 `getenv` 且字符串被合并），这是正常的，不是缺陷。

> 结论：BC1 的创建与编码链路已经能编译、能产出 android 产物，但因第 1、2 条未补，**开关仍必须是 0**，当前部署的 HAL 行为不变。