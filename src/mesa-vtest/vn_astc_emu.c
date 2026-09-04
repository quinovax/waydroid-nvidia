/*
 * Copyright 2026 waydroid-nvidia project
 * SPDX-License-Identifier: MIT
 */

#include "vn_astc_emu.h"

#include "vk_format.h"
#include "vk_texcompress_astc.h"

#include "vn_command_buffer.h"
#include "vn_device.h"
#include "vn_image.h"
#include "vn_physical_device.h"

/* All entrypoints used below are venus' own: they encode into the command
 * stream / ring exactly like app calls, so the decode pass is transparent to
 * the host.
 */

bool
vn_astc_emu_format(VkFormat format)
{
   return vk_texcompress_astc_emulation_format(format) != VK_FORMAT_UNDEFINED;
}

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
}

void
vn_astc_emu_device_fini(struct vn_device *dev)
{
   if (dev->astc_emu.texcompress) {
      vk_texcompress_astc_finish(&dev->base.vk, &dev->base.vk.alloc,
                                 dev->astc_emu.texcompress);
      dev->astc_emu.texcompress = NULL;
   }
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

   for (uint32_t i = 0; i < info->regionCount; i++) {
      vn_astc_emu_decode_region(cmd, img, &info->pRegions[i],
                                info->srcBuffer, info->dstImageLayout);
   }

   vn_astc_emu_restore_compute_state(cmd);

   return true;
}
