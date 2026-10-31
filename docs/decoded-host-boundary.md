# decoded_host 实验：纹理移出显存的边界（2026-10-31）

## 实验目的

在实施 BC1（0.5 B/texel）之前，先量化「纹理完全不占 VRAM」的理论上界。
为此加了姊妹开关 `debug.nvhal.astc_decoded_host=1`：把**解码图本身**也绑到 HAL 自有宿主内存，
app 可见 requirements 变为 0（block 图 + 解码图全部由 HAL 持有）。

## 结果：失败，而且失败方式很有信息量

```
fb peak = 1312 MiB        <-- 显存确实腾出来了（默认配置是 8100 MiB）
status.detailedMessage = "Loading a timeline: Could not get memory for an image!"
status.statusCode = ERROR       <-- 6136 B 无效 zip，workload 未跑起来
```

`Could not get memory for an image!` 正是历史**类 A**（10-18 那批 glibc-shim 连挂五次）的错误码。

HAL 没有报任何 NVHAL_ERR，`astc bytes` 只记录了 1 张图 ⇒ 失败发生在 ICD 侧，
而不是我们的 emulation 逻辑。

## 判读

1. **解码图必须留在 DEVICE_LOCAL**：它每帧被采样（`SAMPLED_IMAGE`），
   放到宿主内存后 GPU 侧纹理采样走 PCIe，驱动对这类布局拒绝（或布局不成立）。
2. 这与「block 图可以移出」形成清晰对照：block 图只被 copy/dispatch 触碰，宿主内存可行；
   解码图被采样器读，不行。
3. **因此 BC1 的价值被进一步确认**：BC1 让纹理**仍然留在 VRAM**，
   但体积只有 RGBA8 的 **1/8**（0.5 vs 4 B/texel）⇒ 5.90 GiB → 约 0.74 GiB，
   彻底消除溢出，且采样仍走正常的 VRAM 路径。

## 对这个实验的处理

保留 `debug.nvhal.astc_decoded_host` 开关（默认关，只在显式设为 1 时生效），
因为它是**有价值的负向证据**：证明「把纹理搬出显存」这条路不可行，
从而把唯一可行的方向收敛到「缩小纹理」= BC1。

## 对当前默认配置的影响

无。`astc_blocks_host` 默认开，`astc_decoded_host` 默认关，两者互不干扰。
默认配置实测 **917 / 915 分**（两次），结果 zip 15-21 KB。

## 产物

- `artifacts/bench3d-20261031T183306-decoded-host/`（1312 MiB 显存 + WORKLOAD_ERROR 6136 B zip）
- 默认配置对照：`artifacts/bench3d-20261031T182048-stable-917/`（915 分，21038 B zip）