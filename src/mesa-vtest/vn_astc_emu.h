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

   /* ---- ASTC 4x4 -> BC1 retarget (see vn_astc_emu.c) ---- */
   uint32_t bc1_state;      /* 0 unprobed, 1 off, 2 on */
   VkDescriptorSetLayout bc1_dsl;
   VkPipelineLayout bc1_pl;
   VkShaderModule bc1_sm;
   VkPipeline bc1_pipe;
   VkDescriptorPool bc1_pool;
};

/* transient objects recorded into a command buffer; freed at cmd reset */
struct vn_astc_emu_temp_obj {
   VkImage image;
   VkDeviceMemory memory;
   VkImageView src_view;
   VkImageView dst_view;
   struct list_head head;

   /* BC1 path only: the decoded-texel image, its view, the BC1 block buffer
    * (all transient and command-buffer scoped) */
   VkImage rgba;
   VkDeviceMemory rgba_mem;
   VkImageView rgba_view;
   VkBuffer bc1_buf;
   VkDeviceMemory bc1_mem;
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

/* the app-visible format for a BC1-retargeted ASTC format, or UNDEFINED */
VkFormat
vn_astc_emu_bc1_format(VkFormat astc_format);

bool
vn_astc_emu_bc1_enabled(struct vn_device *dev);

/* returns true if the copy was fully handled by the emulation path */
bool
vn_astc_emu_cmd_copy_buffer_to_image(
   struct vn_command_buffer *cmd,
   const VkCopyBufferToImageInfo2 *info);

void
vn_astc_emu_cmd_reset(struct vn_command_buffer *cmd);

#endif /* VN_ASTC_EMU_H */
