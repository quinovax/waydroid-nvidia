/*
 * Copyright 2026 waydroid-nvidia project
 * SPDX-License-Identifier: MIT
 *
 * ASTC LDR emulation for venus on hosts without textureCompressionASTC_LDR
 * (desktop NVIDIA). The app-visible image is backed by a host RGBA image;
 * compressed uploads are staged into a transient R32G32B32A32_UINT "block"
 * image (one texel per ASTC block) and decoded into the RGBA image with the
 * common vk_texcompress_astc compute pipelines, all recorded inline into the
 * app's command buffer.
 */

#ifndef VN_ASTC_EMU_H
#define VN_ASTC_EMU_H

#include "vn_common.h"

struct vk_texcompress_astc_state;
struct vn_command_buffer;
struct vn_device;
struct vn_image;

struct vn_astc_emu {
   simple_mtx_t mutex;
   /* lazily initialized on first compressed upload */
   struct vk_texcompress_astc_state *texcompress;
};

/* transient objects recorded into a command buffer; freed at cmd reset */
struct vn_astc_emu_temp_obj {
   VkImage image;
   VkDeviceMemory memory;
   VkImageView src_view;
   VkImageView dst_view;
   struct list_head head;
};

/* last-known compute state to restore after injected decode dispatches */
struct vn_astc_emu_saved_compute_state {
   VkPipeline pipeline;
   VkPipelineLayout desc_layout;
   uint32_t desc_first_set;
   VkDescriptorSet desc_set;
   bool has_desc_set;
   VkPipelineLayout push_const_layout;
   VkShaderStageFlags push_const_stages;
   uint32_t push_const_offset;
   uint32_t push_const_size;
   uint8_t push_const_data[256];
   bool has_push_const;
};

void
vn_astc_emu_device_init(struct vn_device *dev);

void
vn_astc_emu_device_fini(struct vn_device *dev);

bool
vn_astc_emu_format(VkFormat format);

VkFormat
vn_astc_emu_get_rgba_format(VkFormat format);

/* returns true if the copy was fully handled by the emulation path */
bool
vn_astc_emu_cmd_copy_buffer_to_image(
   struct vn_command_buffer *cmd,
   const VkCopyBufferToImageInfo2 *info);

void
vn_astc_emu_cmd_reset(struct vn_command_buffer *cmd);

#endif /* VN_ASTC_EMU_H */
