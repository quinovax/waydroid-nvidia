# 三模式验收记录（2026-11-08）

验收标准（用户原始目标）：3DMark Steel Nomad Light 在三种驱动模式下都能完成并出分，
**结果 zip > 10 KB 且分数 > 3000**。

## 结果：三模式全部通过

| 模式 | 分数 | result.zip | raw_result status | 产物目录 |
|---|---|---|---|---|
| glibc-shim（闭源 580 + HAL BC1） | **5237** | 48017 B | SUCCESS | `artifacts/bench3d-20261107T201805-bc1-runE2/` |
| glibc-shim 复现 | **4986** | 48209 B | SUCCESS | `artifacts/bench3d-20261107T202633-bc1-runE3/` |
| mesa-venus（venus + BC1） | **5413** | 43273 B | SUCCESS | `artifacts/bench3d-20261108T142953-venus-bc1-t2/` |
| nvidia-prolapse（NVK + HAL BC1） | **3182** | 33047 B | SUCCESS | `artifacts/bench3d-20261108T170322-prolapse-bc1-t2/` |

四条记录的最低分 3182 > 3000，最小 zip 33047 B > 10000 B。

## 复现命令

```bash
# glibc-shim
dev/bench3d --label bc1 --props "debug.nvhal.astc_bc1=1;debug.nvhal.astc_blocks_host=0"

# nvidia-prolapse（NVK）—— blocks_host 必须显式 0，否则 NVK 路径 OOM
dev/bench3d --label prolapse-bc1 --props "debug.nvhal.astc_bc1=1;debug.nvhal.astc_blocks_host=0"

# mesa-venus
dev/bench3d --label venus-bc1 --props "debug.mesa.vn.astc.emu=1;debug.mesa.vn.astc.bc1=1"
```

## 负向对照（证明修复的因果性）

| 模式 | 关掉修复后 | 说明 |
|---|---|---|
| mesa-venus | fb 峰值 **8026 MiB** → `VK_ERROR_OUT_OF_HOST_MEMORY on vkEndCommandBuffer`，无 zip | `artifacts/bench3d-20261108T143420-venus-nobc1-ctl/` |
| nvidia-prolapse | zip 4269 B、分数 0、Arielle `code="10000"`、`Application quit during loading` | `artifacts/bench3d-20261108T165852-prolapse-bc1-t1/` |
| glibc-shim（BC1 之前） | 205 分、OODM 139 次、1.35 GiB 溢出到宿主内存 | 见 `docs/BC1-RESULT-5237.md` |

## 修复摘要

同一根因（ASTC 被解码成 RGBA8，4 B/texel，把 1.16 GiB 纹理集膨胀到 5.90 GiB，5.1x，打满 8 GiB 显存），
在两套独立代码路径上做了同一个修复（ASTC 4x4 → BC1，0.5 B/texel）：

- **HAL**（`nvidia-prolapse/hal/src/astc.c` + `bc1_encode.comp`）：服务 glibc-shim 与 nvidia-prolapse。
- **venus 驱动**（`mesa/src/virtio/vulkan/vn_astc_emu.c` + `vn_bc1_encode.comp`）：服务 mesa-venus。

两个开关默认关闭（BC1 有损，Geekbench 的像素敏感 workload 需要原路径）。

## 未完成的另一半目标：ORB

Geekbench Feature Matching 的 `failed validation` **未修复**。
已定性（`docs/orb-three-stack-evidence.md`）：GPU 侧非确定性产出、方向恒偏高 16.7%-25.3%、
**非闭源路径独有**（glibc-shim / venus / 历史 NVK 都出现过，而 driver=26.0.0 两次与 580.178.16 一次通过）。
runtime 与吞吐在通过与失败时几乎相同 ⇒ 不是算错、不是慢，是特定 kernel 的竞态或残留读。
