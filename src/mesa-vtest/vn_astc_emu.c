/*
 * Copyright 2026 waydroid-nvidia project
 * SPDX-License-Identifier: MIT
 */

#include "vn_astc_emu.h"

#include "vk_format.h"
#include "vk_texcompress_astc.h"

#include "vn_bc1_encode_spv.h"
#include "vn_buffer.h"
#include "vn_command_buffer.h"
#include "vn_device.h"
#include "vn_device_memory.h"
#include "vn_image.h"
#include "vn_physical_device.h"

#include "venus-protocol/vn_protocol_driver_buffer.h"
#include "venus-protocol/vn_protocol_driver_command_buffer.h"
#include "venus-protocol/vn_protocol_driver_descriptor_set.h"
#include "venus-protocol/vn_protocol_driver_descriptor_set_layout.h"
#include "venus-protocol/vn_protocol_driver_device_memory.h"
#include "venus-protocol/vn_protocol_driver_image.h"
#include "venus-protocol/vn_protocol_driver_image_view.h"
#include "venus-protocol/vn_protocol_driver_pipeline.h"
#include "venus-protocol/vn_protocol_driver_pipeline_layout.h"
#include "venus-protocol/vn_protocol_driver_shader_module.h"

#include "util/os_misc.h"

/* All entrypoints used below are venus' own: they encode into the command
 * stream / ring exactly like app calls, so the decode pass is transparent to
 * the host.
 */

static VkResult
vn_astc_emu_bc1_init(struct vn_device *dev);

static void
vn_astc_emu_bc1_fini(struct vn_device *dev);

bool
vn_astc_emu_format(VkFormat format)
{
   return vk_texcompress_astc_emulation_format(format) != VK_FORMAT_UNDEFINED;
}

/* ASTC 4x4 is the only block geometry BC1 can stand in for: both are 4x4, so
 * one ASTC block maps onto exactly one BC1 block with no resampling.  Every
 * other ASTC block size keeps the RGBA8 decode.
 */
VkFormat
vn_astc_emu_bc1_format(VkFormat astc_format)
{
   const bool srgb = astc_format == VK_FORMAT_ASTC_4x4_SRGB_BLOCK;

   if (astc_format != VK_FORMAT_ASTC_4x4_UNORM_BLOCK && !srgb)
      return VK_FORMAT_UNDEFINED;

   return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
}

bool
vn_astc_emu_bc1_enabled(struct vn_device *dev)
{
   struct vn_astc_emu *emu = &dev->astc_emu;

   if (likely(emu->bc1_state))
      return emu->bc1_state == 2;

   simple_mtx_lock(&emu->mutex);
   if (!emu->bc1_state) {
      const bool on = debug_get_bool_option("VN_ASTC_BC1", false);
      emu->bc1_state = on ? 2 : 1;
      if (on)
         vn_log(dev->instance,
                "ASTC emu: 4x4 images decoded to BC1 (debug.mesa.vn.astc.bc1)");
   }
   simple_mtx_unlock(&emu->mutex);

   return emu->bc1_state == 2;
}

/* The format the host image is actually allocated with.  This is what
 * vn_physical_device / vn_image consult, so it must already know about the
 * BC1 retarget.
 */
VkFormat
vn_astc_emu_get_rgba_format(VkFormat format)
{
   return vk_texcompress_astc_emulation_format(format);
}

void
vn_astc_emu_device_init(struct vn_device *dev)
{
   simple_mtx_init(&dev->astc_emu.mutex, mtx_plain);
   dev->astc_emu.texcompress = NULL;

   dev->astc_emu.bc1_state = 0;
   dev->astc_emu.bc1_dsl = VK_NULL_HANDLE;
   dev->astc_emu.bc1_pl = VK_NULL_HANDLE;
   dev->astc_emu.bc1_sm = VK_NULL_HANDLE;
   dev->astc_emu.bc1_pipe = VK_NULL_HANDLE;

   /* the BC1 encoder is built eagerly when the switch is on: it must be
    * ready before the first compressed upload, which may arrive mid-frame
    */
   if (dev->physical_device->emu_astc_ldr && vn_astc_emu_bc1_enabled(dev) &&
       vn_astc_emu_bc1_init(dev) != VK_SUCCESS) {
      vn_log(dev->instance,
             "ASTC emu: BC1 retarget unavailable, keeping the RGBA8 decode");
      dev->astc_emu.bc1_state = 1;
   }
}

void
vn_astc_emu_device_fini(struct vn_device *dev)
{
   if (dev->astc_emu.texcompress) {
      vk_texcompress_astc_finish(&dev->base.vk, &dev->base.vk.alloc,
                                 dev->astc_emu.texcompress);
      dev->astc_emu.texcompress = NULL;
   }
   vn_astc_emu_bc1_fini(dev);
   simple_mtx_destroy(&dev->astc_emu.mutex);
}

static struct vk_texcompress_astc_state *
vn_astc_emu_get_state(struct vn_device *dev)
{
   struct vn_astc_emu *emu = &dev->astc_emu;

   if (likely(emu->texcompress))
      return emu->texcompress;

   simple_mtx_lock(&emu->mutex);
   if (!emu->texcompress) {
      struct vk_texcompress_astc_state *state = NULL;
      VkResult result = vk_texcompress_astc_init(
         &dev->base.vk, &dev->base.vk.alloc, VK_NULL_HANDLE, &state,
         (struct vk_texcompress_astc_params){
            .luts_alignment = 64,
            .luts_memory_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
         });
      if (result == VK_SUCCESS)
         emu->texcompress = state;
      else
         vn_log(dev->instance, "ASTC emu: vk_texcompress_astc_init failed");
   }
   simple_mtx_unlock(&emu->mutex);

   return emu->texcompress;
}

static struct vn_astc_emu_temp_obj *
vn_astc_emu_alloc_temp(struct vn_command_buffer *cmd)
{
   const VkAllocationCallbacks *alloc = &cmd->base.vk.pool->alloc;
   struct vn_astc_emu_temp_obj *temp =
      vk_zalloc(alloc, sizeof(*temp), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!temp)
      return NULL;

   list_addtail(&temp->head, &cmd->builder.astc_temp_objs);
   return temp;
}

void
vn_astc_emu_cmd_reset(struct vn_command_buffer *cmd)
{
   struct vn_device *dev = vn_device_from_vk(cmd->base.vk.pool->base.device);
   VkDevice dev_handle = vn_device_to_handle(dev);
   const VkAllocationCallbacks *alloc = &cmd->base.vk.pool->alloc;

   list_for_each_entry_safe(struct vn_astc_emu_temp_obj, temp,
                            &cmd->builder.astc_temp_objs, head) {
      if (temp->src_view != VK_NULL_HANDLE)
         vn_DestroyImageView(dev_handle, temp->src_view, NULL);
      if (temp->dst_view != VK_NULL_HANDLE)
         vn_DestroyImageView(dev_handle, temp->dst_view, NULL);
      if (temp->image != VK_NULL_HANDLE)
         vn_DestroyImage(dev_handle, temp->image, NULL);
      if (temp->memory != VK_NULL_HANDLE)
         vn_FreeMemory(dev_handle, temp->memory, NULL);
      /* BC1 path transients */
      if (temp->rgba_view != VK_NULL_HANDLE)
         vn_DestroyImageView(dev_handle, temp->rgba_view, NULL);
      if (temp->rgba != VK_NULL_HANDLE)
         vn_DestroyImage(dev_handle, temp->rgba, NULL);
      if (temp->rgba_mem != VK_NULL_HANDLE)
         vn_FreeMemory(dev_handle, temp->rgba_mem, NULL);
      if (temp->bc1_buf != VK_NULL_HANDLE)
         vn_DestroyBuffer(dev_handle, temp->bc1_buf, NULL);
      if (temp->bc1_mem != VK_NULL_HANDLE)
         vn_FreeMemory(dev_handle, temp->bc1_mem, NULL);
      list_del(&temp->head);
      vk_free(alloc, temp);
   }
}

static uint32_t
vn_astc_emu_find_memory_type(struct vn_device *dev,
                             uint32_t type_bits)
{
   const VkPhysicalDeviceMemoryProperties *props =
      &dev->physical_device->memory_properties;

   /* prefer DEVICE_LOCAL */
   for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
      if ((type_bits & (1u << i)) &&
          (props->memoryTypes[i].propertyFlags &
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
         return i;
   }
   for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
      if (type_bits & (1u << i))
         return i;
   }
   return 0;
}

/* Create the transient block image holding raw ASTC blocks for one copy
 * region: R32G32B32A32_UINT, one texel per block, single mip.
 */
static VkResult
vn_astc_emu_create_block_image(struct vn_command_buffer *cmd,
                               struct vn_astc_emu_temp_obj *temp,
                               VkExtent2D block_extent,
                               uint32_t layer_count)
{
   struct vn_device *dev = vn_device_from_vk(cmd->base.vk.pool->base.device);
   VkDevice dev_handle = vn_device_to_handle(dev);
   VkResult result;

   const VkImageCreateInfo img_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R32G32B32A32_UINT,
      .extent = { block_extent.width, block_extent.height, 1 },
      .mipLevels = 1,
      .arrayLayers = layer_count,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   result = vn_CreateImage(dev_handle, &img_info, NULL, &temp->image);
   if (result != VK_SUCCESS)
      return result;

   const VkImageMemoryRequirementsInfo2 reqs_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
      .image = temp->image,
   };
   VkMemoryRequirements2 reqs = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
   };
   vn_GetImageMemoryRequirements2(dev_handle, &reqs_info, &reqs);

   const VkMemoryAllocateInfo mem_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = reqs.memoryRequirements.size,
      .memoryTypeIndex = vn_astc_emu_find_memory_type(
         dev, reqs.memoryRequirements.memoryTypeBits),
   };
   result = vn_AllocateMemory(dev_handle, &mem_info, NULL, &temp->memory);
   if (result != VK_SUCCESS)
      return result;

   const VkBindImageMemoryInfo bind_info = {
      .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
      .image = temp->image,
      .memory = temp->memory,
      .memoryOffset = 0,
   };
   /* vn only implements the 2 variant; host-side this is 1.1 core which the
    * renderer resolves independently of the app's device version */
   return vn_BindImageMemory2(dev_handle, 1, &bind_info);
}

static void
vn_astc_emu_save_compute_state(struct vn_command_buffer *cmd)
{
   /* the shadow is maintained by vn_Cmd* hooks; nothing to snapshot */
}

static void
vn_astc_emu_restore_compute_state(struct vn_command_buffer *cmd)
{
   VkCommandBuffer cmd_handle = vn_command_buffer_to_handle(cmd);
   const struct vn_astc_emu_saved_compute_state *s =
      &cmd->builder.astc_saved_compute;

   if (s->pipeline != VK_NULL_HANDLE) {
      vn_CmdBindPipeline(cmd_handle, VK_PIPELINE_BIND_POINT_COMPUTE,
                         s->pipeline);
   }
   if (s->has_desc_set) {
      vn_CmdBindDescriptorSets(cmd_handle, VK_PIPELINE_BIND_POINT_COMPUTE,
                               s->desc_layout, s->desc_first_set, 1,
                               &s->desc_set, 0, NULL);
   }
   if (s->has_push_const) {
      vn_CmdPushConstants(cmd_handle, s->push_const_layout,
                          s->push_const_stages, s->push_const_offset,
                          s->push_const_size, s->push_const_data);
   }
}


/* ---- ASTC 4x4 -> BC1 retarget ------------------------------------------
 *
 * The stock emulation decodes every compressed image to RGBA8 (4 B/texel).
 * 3DMark's texture set is ~1.2 GiB compressed, so the decode inflates it to
 * ~5.9 GiB and exhausts the 8 GiB card (measured on glibc-shim: fb.used
 * 8105 MiB -> OODM/OOHM; on venus the same pressure surfaces as
 * VK_ERROR_OUT_OF_HOST_MEMORY from vkEndCommandBuffer).
 *
 * BC1 is 0.5 B/texel and its 4x4 block geometry matches ASTC 4x4 exactly, so
 * one ASTC block maps onto one 8-byte BC1 block with no resampling. BC1 has no
 * STORAGE_IMAGE usage, so the decode cannot write it directly: the decode pass
 * targets a transient RGBA8 image, a second pass encodes that into a transient
 * BC1 block buffer, and a plain vkCmdCopyBufferToImage lands the blocks in the
 * app's BC1 image. Both transients are command-buffer-scoped and never
 * sampled, so they do not compete with the app for VRAM.
 *
 * Lossy, hence the debug.mesa.vn.astc.bc1 opt-in.
 */

static VkResult
vn_astc_emu_bc1_init(struct vn_device *dev)
{
   struct vn_astc_emu *emu = &dev->astc_emu;
   VkDevice dev_handle = vn_device_to_handle(dev);
   const VkAllocationCallbacks *alloc = &dev->base.vk.alloc;
   VkResult result;

   if (emu->bc1_pipe != VK_NULL_HANDLE)
      return VK_SUCCESS;

   const VkDescriptorSetLayoutBinding bindings[2] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
   };
   const VkDescriptorSetLayoutCreateInfo dsl_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
      .bindingCount = ARRAY_SIZE(bindings),
      .pBindings = bindings,
   };
   result = vn_CreateDescriptorSetLayout(dev_handle, &dsl_info, NULL,
                                         &emu->bc1_dsl);
   if (result != VK_SUCCESS)
      goto fail;

   const VkPushConstantRange pc_range = {
      VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) * 5,
   };
   const VkPipelineLayoutCreateInfo pl_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &emu->bc1_dsl,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pc_range,
   };
   result = vn_CreatePipelineLayout(dev_handle, &pl_info, NULL, &emu->bc1_pl);
   if (result != VK_SUCCESS)
      goto fail;

   const VkShaderModuleCreateInfo sm_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = vn_bc1_encode_spv_size,
      .pCode = vn_bc1_encode_spv,
   };
   result = vn_CreateShaderModule(dev_handle, &sm_info, NULL, &emu->bc1_sm);
   if (result != VK_SUCCESS)
      goto fail;

   const VkComputePipelineCreateInfo pipe_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = emu->bc1_sm,
         .pName = "main",
      },
      .layout = emu->bc1_pl,
   };
   result = vn_CreateComputePipelines(dev_handle, VK_NULL_HANDLE, 1,
                                      &pipe_info, NULL, &emu->bc1_pipe);
   if (result != VK_SUCCESS) {
      vn_log(dev->instance, "ASTC emu: BC1 encode pipeline creation failed");
      goto fail;
   }

   (void)alloc;
   return VK_SUCCESS;

fail:
   if (emu->bc1_dsl != VK_NULL_HANDLE) {
      vn_DestroyDescriptorSetLayout(dev_handle, emu->bc1_dsl, NULL);
      emu->bc1_dsl = VK_NULL_HANDLE;
   }
   if (emu->bc1_pl != VK_NULL_HANDLE) {
      vn_DestroyPipelineLayout(dev_handle, emu->bc1_pl, NULL);
      emu->bc1_pl = VK_NULL_HANDLE;
   }
   if (emu->bc1_sm != VK_NULL_HANDLE) {
      vn_DestroyShaderModule(dev_handle, emu->bc1_sm, NULL);
      emu->bc1_sm = VK_NULL_HANDLE;
   }
   return result;
}

static void
vn_astc_emu_bc1_fini(struct vn_device *dev)
{
   struct vn_astc_emu *emu = &dev->astc_emu;
   VkDevice dev_handle = vn_device_to_handle(dev);

   if (emu->bc1_pipe != VK_NULL_HANDLE) {
      vn_DestroyPipeline(dev_handle, emu->bc1_pipe, NULL);
      emu->bc1_pipe = VK_NULL_HANDLE;
   }
   if (emu->bc1_pl != VK_NULL_HANDLE) {
      vn_DestroyPipelineLayout(dev_handle, emu->bc1_pl, NULL);
      emu->bc1_pl = VK_NULL_HANDLE;
   }
   if (emu->bc1_sm != VK_NULL_HANDLE) {
      vn_DestroyShaderModule(dev_handle, emu->bc1_sm, NULL);
      emu->bc1_sm = VK_NULL_HANDLE;
   }
   if (emu->bc1_dsl != VK_NULL_HANDLE) {
      vn_DestroyDescriptorSetLayout(dev_handle, emu->bc1_dsl, NULL);
      emu->bc1_dsl = VK_NULL_HANDLE;
   }
}

/* The decoded-texel image the decode pass writes and the BC1 block buffer the
 * encode pass fills, both for one copy region at the decoded image's extent so
 * the two passes share one coordinate space.
 */
static VkResult
vn_astc_emu_create_rgba_target(struct vn_command_buffer *cmd,
                               struct vn_astc_emu_temp_obj *temp,
                               VkExtent2D texel_extent,
                               uint32_t layer_count)
{
   struct vn_device *dev = vn_device_from_vk(cmd->base.vk.pool->base.device);
   VkDevice dev_handle = vn_device_to_handle(dev);
   VkResult result;

   const VkImageCreateInfo img_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = { texel_extent.width, texel_extent.height, 1 },
      .mipLevels = 1,
      .arrayLayers = layer_count,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   result = vn_CreateImage(dev_handle, &img_info, NULL, &temp->rgba);
   if (result != VK_SUCCESS)
      return result;

   const VkImageMemoryRequirementsInfo2 reqs_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
      .image = temp->rgba,
   };
   VkMemoryRequirements2 reqs = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
   };
   vn_GetImageMemoryRequirements2(dev_handle, &reqs_info, &reqs);

   const VkMemoryAllocateInfo mem_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = reqs.memoryRequirements.size,
      .memoryTypeIndex =
         vn_astc_emu_find_memory_type(dev, reqs.memoryRequirements.memoryTypeBits),
   };
   result = vn_AllocateMemory(dev_handle, &mem_info, NULL, &temp->rgba_mem);
   if (result != VK_SUCCESS)
      return result;

   const VkBindImageMemoryInfo bind_info = {
      .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
      .image = temp->rgba,
      .memory = temp->rgba_mem,
      .memoryOffset = 0,
   };
   result = vn_BindImageMemory2(dev_handle, 1, &bind_info);
   if (result != VK_SUCCESS)
      return result;

   const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = temp->rgba,
      .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .format = VK_FORMAT_R8G8B8A8_UINT,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layer_count },
   };
   return vn_CreateImageView(dev_handle, &view_info, NULL, &temp->rgba_view);
}

static VkResult
vn_astc_emu_create_bc1_buffer(struct vn_command_buffer *cmd,
                              struct vn_astc_emu_temp_obj *temp,
                              uint32_t block_count)
{
   struct vn_device *dev = vn_device_from_vk(cmd->base.vk.pool->base.device);
   VkDevice dev_handle = vn_device_to_handle(dev);
   VkResult result;

   const VkDeviceSize size = (VkDeviceSize)block_count * 8u;
   const VkBufferCreateInfo buf_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   result = vn_CreateBuffer(dev_handle, &buf_info, NULL, &temp->bc1_buf);
   if (result != VK_SUCCESS)
      return result;

   const VkBufferMemoryRequirementsInfo2 reqs_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
      .buffer = temp->bc1_buf,
   };
   VkMemoryRequirements2 reqs = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
   };
   vn_GetBufferMemoryRequirements2(dev_handle, &reqs_info, &reqs);

   const VkMemoryAllocateInfo mem_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = reqs.memoryRequirements.size,
      .memoryTypeIndex =
         vn_astc_emu_find_memory_type(dev, reqs.memoryRequirements.memoryTypeBits),
   };
   result = vn_AllocateMemory(dev_handle, &mem_info, NULL, &temp->bc1_mem);
   if (result != VK_SUCCESS)
      return result;

   const VkBindBufferMemoryInfo bind_info = {
      .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
      .buffer = temp->bc1_buf,
      .memory = temp->bc1_mem,
      .memoryOffset = 0,
   };
   return vn_BindBufferMemory2(dev_handle, 1, &bind_info);
}

/* Decode one region into temp->rgba, encode those texels into temp->bc1_buf,
 * then copy the blocks into the app's BC1 image at the region's position.
 */
static void
vn_astc_emu_decode_region_bc1(struct vn_command_buffer *cmd,
                              struct vn_image *img,
                              const VkBufferImageCopy2 *region,
                              VkBuffer src_buffer,
                              VkImageLayout dst_layout,
                              struct vn_astc_emu_temp_obj *temp)
{
   struct vn_device *dev = vn_device_from_vk(cmd->base.vk.pool->base.device);
   VkDevice dev_handle = vn_device_to_handle(dev);
   VkCommandBuffer cmd_handle = vn_command_buffer_to_handle(cmd);
   VkImage img_handle = vn_image_to_handle(img);
   const VkFormat astc_format = img->astc_emu_format;
   const uint32_t bw = vk_format_get_blockwidth(astc_format);
   const uint32_t bh = vk_format_get_blockheight(astc_format);

   struct vk_texcompress_astc_state *astc = vn_astc_emu_get_state(dev);
   if (!astc)
      return;
   VkPipeline dec_pipeline = vk_texcompress_astc_get_decode_pipeline(
      &dev->base.vk, (VkAllocationCallbacks *)&dev->base.vk.alloc, astc,
      VK_NULL_HANDLE, astc_format);
   if (dec_pipeline == VK_NULL_HANDLE) {
      vn_log(dev->instance, "ASTC emu: no decode pipeline");
      return;
   }

   const uint32_t block_off_x = region->imageOffset.x / bw;
   const uint32_t block_off_y = region->imageOffset.y / bh;
   const VkExtent2D region_blocks = {
      DIV_ROUND_UP(region->imageExtent.width, bw),
      DIV_ROUND_UP(region->imageExtent.height, bh),
   };
   /* texel extent of the decoded scratch image: the decode shader writes at
    * (block_off + block) * 4, so the scratch must cover the region's final
    * position, exactly like the RGBA8 path's block image does
    */
   const VkExtent2D texel_extent = {
      (block_off_x + region_blocks.width) * bw,
      (block_off_y + region_blocks.height) * bh,
   };
   const uint32_t layer_count = region->imageSubresource.layerCount;

   if (vn_astc_emu_create_block_image(cmd, temp,
                                      (VkExtent2D){ block_off_x + region_blocks.width,
                                                    block_off_y + region_blocks.height },
                                      layer_count) != VK_SUCCESS)
      return;
   if (vn_astc_emu_create_rgba_target(cmd, temp, texel_extent,
                                      layer_count) != VK_SUCCESS)
      return;
   if (vn_astc_emu_create_bc1_buffer(cmd, temp,
                                     region_blocks.width * region_blocks.height *
                                        layer_count) != VK_SUCCESS)
      return;

   /* 1. block image UNDEFINED -> TRANSFER_DST, upload the raw ASTC blocks */
   const VkImageMemoryBarrier to_xfer = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = temp->image,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layer_count },
   };
   vn_CmdPipelineBarrier(cmd_handle, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &to_xfer);

   const VkBufferImageCopy block_region = {
      .bufferOffset = region->bufferOffset,
      .bufferRowLength = DIV_ROUND_UP(region->bufferRowLength, bw),
      .bufferImageHeight = DIV_ROUND_UP(region->bufferImageHeight, bh),
      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layer_count },
      .imageOffset = { (int32_t)block_off_x, (int32_t)block_off_y, 0 },
      .imageExtent = { region_blocks.width, region_blocks.height, 1 },
   };
   vn_CmdCopyBufferToImage(cmd_handle, src_buffer, temp->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &block_region);

   /* 2. barriers: block image -> shader read; scratch RGBA -> GENERAL
    *    (the decode pass writes it, we then read it as the encode source)
    */
   const VkImageMemoryBarrier pre_decode[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = temp->image,
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layer_count },
      },
      {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = 0,
         .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = temp->rgba,
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layer_count },
      },
   };
   vn_CmdPipelineBarrier(cmd_handle, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, ARRAY_SIZE(pre_decode), pre_decode);

   /* 3. decoded-texel view over the scratch image (UINT, as the decoder
    *    writes through a UINT view)
    */
   const VkImageViewCreateInfo rgba_uint_view = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = temp->rgba,
      .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .format = VK_FORMAT_R8G8B8A8_UINT,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layer_count },
   };
   VkImageView dec_dst_view = temp->rgba_view;
   if (dec_dst_view == VK_NULL_HANDLE &&
       vn_CreateImageView(dev_handle, &rgba_uint_view, NULL,
                          &dec_dst_view) != VK_SUCCESS)
      return;

   const VkImageViewCreateInfo src_view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = temp->image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .format = VK_FORMAT_R32G32B32A32_UINT,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layer_count },
   };
   if (temp->src_view == VK_NULL_HANDLE &&
       vn_CreateImageView(dev_handle, &src_view_info, NULL,
                          &temp->src_view) != VK_SUCCESS)
      return;

   /* 4. run the ASTC decoder into the scratch image */
   vn_CmdBindPipeline(cmd_handle, VK_PIPELINE_BIND_POINT_COMPUTE, dec_pipeline);

   struct vk_texcompress_astc_write_descriptor_set writes;
   vk_texcompress_astc_fill_write_descriptor_sets(
      astc, &writes, temp->src_view, VK_IMAGE_LAYOUT_GENERAL, dec_dst_view,
      astc_format);
   vn_CmdPushDescriptorSet(cmd_handle, VK_PIPELINE_BIND_POINT_COMPUTE,
                           astc->p_layout, 0,
                           ARRAY_SIZE(writes.descriptor_set),
                           writes.descriptor_set);

   const uint32_t dec_push_const[] = {
      block_off_x,
      block_off_y,
      region->imageOffset.x + region->imageExtent.width,
      region->imageOffset.y + region->imageExtent.height,
      false,
   };
   vn_CmdPushConstants(cmd_handle, astc->p_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(dec_push_const), dec_push_const);
   vn_CmdDispatch(cmd_handle, DIV_ROUND_UP(region_blocks.width, 2),
                  DIV_ROUND_UP(region_blocks.height, 2), layer_count);

   /* 5. encode the decoded texels into BC1 blocks */
   const VkImageMemoryBarrier post_decode = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = temp->rgba,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layer_count },
   };
   vn_CmdPipelineBarrier(cmd_handle, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &post_decode);

   vn_CmdBindPipeline(cmd_handle, VK_PIPELINE_BIND_POINT_COMPUTE,
                      dev->astc_emu.bc1_pipe);

   const VkDescriptorImageInfo bc1_src_info = {
      .sampler = VK_NULL_HANDLE,
      .imageView = dec_dst_view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   const VkDescriptorBufferInfo bc1_dst_info = {
      .buffer = temp->bc1_buf,
      .offset = 0,
      .range = VK_WHOLE_SIZE,
   };
   const VkWriteDescriptorSet bc1_writes[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &bc1_src_info,
      },
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &bc1_dst_info,
      },
   };
   vn_CmdPushDescriptorSet(cmd_handle, VK_PIPELINE_BIND_POINT_COMPUTE,
                           dev->astc_emu.bc1_pl, 0, ARRAY_SIZE(bc1_writes),
                           bc1_writes);

   const uint32_t enc_push_const[5] = {
      region_blocks.width, region_blocks.height,
      region_blocks.width * region_blocks.height, block_off_x, block_off_y,
   };
   vn_CmdPushConstants(cmd_handle, dev->astc_emu.bc1_pl,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(enc_push_const), enc_push_const);
   vn_CmdDispatch(cmd_handle, region_blocks.width, region_blocks.height,
                  layer_count);

   /* 6. blocks -> the app's BC1 image, then back to the app's layout */
   const VkMemoryBarrier enc_done = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
   };
   const VkImageMemoryBarrier to_copy = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = dst_layout,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = img_handle,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT,
                            region->imageSubresource.mipLevel, 1,
                            region->imageSubresource.baseArrayLayer,
                            layer_count },
   };
   vn_CmdPipelineBarrier(cmd_handle, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &enc_done, 0,
                         NULL, 1, &to_copy);

   /* BC1 blocks map one-to-one onto the app image's blocks: the encode wrote
    * region-local blocks, so the destination offset is the region start
    */
   const VkBufferImageCopy bc1_copy = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT,
                            region->imageSubresource.mipLevel,
                            region->imageSubresource.baseArrayLayer,
                            layer_count },
      .imageOffset = { region->imageOffset.x, region->imageOffset.y, 0 },
      .imageExtent = { region->imageExtent.width, region->imageExtent.height,
                       1 },
   };
   /* raw: the destination IS the app's emulated image, so the public
    * entrypoint would recurse straight back into this function
    */
   vn_cmd_copy_buffer_to_image_raw(cmd_handle, temp->bc1_buf, img_handle,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &bc1_copy);

   /* 7. app image back to the layout the app expects */
   const VkImageMemoryBarrier post_copy = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = dst_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = img_handle,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT,
                            region->imageSubresource.mipLevel, 1,
                            region->imageSubresource.baseArrayLayer,
                            layer_count },
   };
   vn_CmdPipelineBarrier(cmd_handle, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &post_copy);
}

static void
vn_astc_emu_decode_region(struct vn_command_buffer *cmd,
                          struct vn_image *img,
                          const VkBufferImageCopy2 *region,
                          VkBuffer src_buffer,
                          VkImageLayout dst_layout)
{
   struct vn_device *dev = vn_device_from_vk(cmd->base.vk.pool->base.device);
   VkDevice dev_handle = vn_device_to_handle(dev);
   VkCommandBuffer cmd_handle = vn_command_buffer_to_handle(cmd);
   VkImage img_handle = vn_image_to_handle(img);
   const VkFormat astc_format = img->astc_emu_format;
   const uint32_t bw = vk_format_get_blockwidth(astc_format);
   const uint32_t bh = vk_format_get_blockheight(astc_format);

   struct vk_texcompress_astc_state *astc = vn_astc_emu_get_state(dev);
   if (!astc)
      return;

   VkPipeline pipeline = vk_texcompress_astc_get_decode_pipeline(
      &dev->base.vk, (VkAllocationCallbacks *)&dev->base.vk.alloc, astc,
      VK_NULL_HANDLE, astc_format);
   if (pipeline == VK_NULL_HANDLE) {
      vn_log(dev->instance, "ASTC emu: no decode pipeline");
      return;
   }

   /* compressed-copy imageOffset is block aligned per spec */
   const uint32_t block_off_x = region->imageOffset.x / bw;
   const uint32_t block_off_y = region->imageOffset.y / bh;
   const VkExtent2D region_blocks = {
      DIV_ROUND_UP(region->imageExtent.width, bw),
      DIV_ROUND_UP(region->imageExtent.height, bh),
   };
   /* temp image spans [0, offset+extent) blocks so that src and dst share
    * one coordinate space (the decode shader adds the pushed block offset
    * to both reads and writes)
    */
   const VkExtent2D block_extent = {
      block_off_x + region_blocks.width,
      block_off_y + region_blocks.height,
   };
   const uint32_t layer_count = region->imageSubresource.layerCount;

   struct vn_astc_emu_temp_obj *temp = vn_astc_emu_alloc_temp(cmd);
   if (!temp)
      return;

   if (vn_astc_emu_create_block_image(cmd, temp, block_extent,
                                      layer_count) != VK_SUCCESS)
      return;

   /* 1. block image UNDEFINED -> TRANSFER_DST */
   const VkImageMemoryBarrier to_xfer = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = temp->image,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                            layer_count },
   };
   vn_CmdPipelineBarrier(cmd_handle, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &to_xfer);

   /* 2. upload raw blocks: buffer layout of ASTC (16B blocks) matches
    * R32G32B32A32 texels one to one
    */
   /* use the 1.0 command: the app's device may predate 1.3 and the host
    * proc table would have no vkCmdCopyBufferToImage2 for it
    */
   const VkBufferImageCopy block_region = {
      .bufferOffset = region->bufferOffset,
      .bufferRowLength = DIV_ROUND_UP(region->bufferRowLength, bw),
      .bufferImageHeight = DIV_ROUND_UP(region->bufferImageHeight, bh),
      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layer_count },
      .imageOffset = { (int32_t)block_off_x, (int32_t)block_off_y, 0 },
      .imageExtent = { region_blocks.width, region_blocks.height, 1 },
   };
   /* temp image is not emulated: falls through to the plain encode */
   vn_CmdCopyBufferToImage(cmd_handle, src_buffer, temp->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &block_region);

   /* 3. block image -> shader read; dst RGBA region -> GENERAL */
   const VkImageMemoryBarrier pre_decode[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = temp->image,
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                               layer_count },
      },
      {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
         .oldLayout = dst_layout,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = img_handle,
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT,
                               region->imageSubresource.mipLevel, 1,
                               region->imageSubresource.baseArrayLayer,
                               layer_count },
      },
   };
   vn_CmdPipelineBarrier(cmd_handle, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 2, pre_decode);

   /* 4. views */
   const VkImageViewCreateInfo src_view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = temp->image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .format = VK_FORMAT_R32G32B32A32_UINT,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                            layer_count },
   };
   if (vn_CreateImageView(dev_handle, &src_view_info, NULL,
                          &temp->src_view) != VK_SUCCESS)
      return;

   const VkImageViewCreateInfo dst_view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .pNext = &(VkImageViewUsageCreateInfo){
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
         .usage = VK_IMAGE_USAGE_STORAGE_BIT,
      },
      .image = img_handle,
      .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .format = VK_FORMAT_R8G8B8A8_UINT,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT,
                            region->imageSubresource.mipLevel, 1,
                            region->imageSubresource.baseArrayLayer,
                            layer_count },
   };
   if (vn_CreateImageView(dev_handle, &dst_view_info, NULL,
                          &temp->dst_view) != VK_SUCCESS)
      return;

   /* 5. bind + push descriptors + push constants + dispatch */
   vn_CmdBindPipeline(cmd_handle, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   struct vk_texcompress_astc_write_descriptor_set writes;
   vk_texcompress_astc_fill_write_descriptor_sets(
      astc, &writes, temp->src_view, VK_IMAGE_LAYOUT_GENERAL, temp->dst_view,
      astc_format);
   vn_CmdPushDescriptorSet(cmd_handle, VK_PIPELINE_BIND_POINT_COMPUTE,
                           astc->p_layout, 0,
                           ARRAY_SIZE(writes.descriptor_set),
                           writes.descriptor_set);

   /* shader convention (see radv_meta_astc_decode.c): block coords of the
    * copy start, texel coords of the end, is_3d
    */
   const uint32_t push_const[] = {
      block_off_x,
      block_off_y,
      region->imageOffset.x + region->imageExtent.width,
      region->imageOffset.y + region->imageExtent.height,
      false, /* no 3D (emulation rejects 3D ASTC images) */
   };
   vn_CmdPushConstants(cmd_handle, astc->p_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_const),
                       push_const);

   /* each workgroup handles 2x2 blocks */
   vn_CmdDispatch(cmd_handle, DIV_ROUND_UP(region_blocks.width, 2),
                  DIV_ROUND_UP(region_blocks.height, 2), layer_count);

   /* 6. dst RGBA back to the app's layout */
   const VkImageMemoryBarrier post_decode = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT |
                       VK_ACCESS_TRANSFER_READ_BIT |
                       VK_ACCESS_SHADER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = dst_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = img_handle,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT,
                            region->imageSubresource.mipLevel, 1,
                            region->imageSubresource.baseArrayLayer,
                            layer_count },
   };
   vn_CmdPipelineBarrier(cmd_handle, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &post_decode);
}

bool
vn_astc_emu_cmd_copy_buffer_to_image(struct vn_command_buffer *cmd,
                                     const VkCopyBufferToImageInfo2 *info)
{
   struct vn_image *img = vn_image_from_handle(info->dstImage);

   if (likely(!img->astc_emulated))
      return false;

   vn_astc_emu_save_compute_state(cmd);

   /* BC1 retarget: only 4x4 ASTC can map onto BC1, and only when the app
    * image was created with the BC1 backing format.
    */
   const bool bc1 = img->astc_bc1;

   for (uint32_t i = 0; i < info->regionCount; i++) {
      if (bc1) {
         struct vn_astc_emu_temp_obj *temp = vn_astc_emu_alloc_temp(cmd);
         if (!temp) {
            vn_astc_emu_decode_region(cmd, img, &info->pRegions[i],
                                      info->srcBuffer, info->dstImageLayout);
            continue;
         }
         vn_astc_emu_decode_region_bc1(cmd, img, &info->pRegions[i],
                                       info->srcBuffer, info->dstImageLayout,
                                       temp);
      } else {
         vn_astc_emu_decode_region(cmd, img, &info->pRegions[i],
                                   info->srcBuffer, info->dstImageLayout);
      }
   }

   vn_astc_emu_restore_compute_state(cmd);

   return true;
}
