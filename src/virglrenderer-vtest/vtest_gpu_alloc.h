/*
 * Host-side buffer allocator for VCMD_RESOURCE_ALLOC_GPU.
 *
 * GPU path: exportable VkImage + dedicated VkDeviceMemory on the render GPU
 * (NVIDIA), exported as a dma_buf.
 *
 *   Discrete NVIDIA (compositor on NVIDIA): block-linear DRM modifiers.
 *   NVIDIA EGL binds LINEAR only as GL_TEXTURE_EXTERNAL_OES; KWin needs
 *   GL_TEXTURE_2D, which block-linear provides.
 *
 *   Hybrid (iGPU compositor + NVIDIA render): NVIDIA LINEAR. Intel/AMD
 *   import LINEAR dma_bufs as GL_TEXTURE_2D (tests/crossimport.c);
 *   NVIDIA block-linear is not advertised by the iGPU compositor.
 *   Auto-detected from DRM connectors / boot_vga; override with
 *   WAYDROID_NVIDIA_PRESENT=linear|block-linear.
 *
 * CPU path: NVIDIA LINEAR (host-visible) first, udmabuf fallback, for
 * BO_USE_SW_* gralloc buffers the guest must mmap.
 */

#ifndef VTEST_GPU_ALLOC_H
#define VTEST_GPU_ALLOC_H

#include <stdbool.h>
#include <stdint.h>

int vtest_gpu_alloc_gpu(uint32_t width, uint32_t height, uint32_t drm_format,
                        uint32_t *out_stride, uint64_t *out_modifier,
                        uint64_t *out_size, int *out_fd);

int vtest_gpu_alloc_cpu(uint32_t width, uint32_t height, uint32_t drm_format,
                        uint32_t *out_stride, uint64_t *out_size, int *out_fd);

#endif /* VTEST_GPU_ALLOC_H */
