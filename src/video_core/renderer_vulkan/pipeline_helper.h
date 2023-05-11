// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/types.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

using Shader::Backend::SPIRV::NUM_TEXTURE_AND_IMAGE_SCALING_WORDS;

class DescriptorLayoutBuilder {
public:
    DescriptorLayoutBuilder(const Device& device_) : device{&device_} {}

    bool CanUsePushDescriptor() const noexcept {
        return device->IsKhrPushDescriptorSupported() &&
               num_descriptors <= device->MaxPushDescriptors();
    }

    vk::DescriptorSetLayout CreateDescriptorSetLayout(bool use_push_descriptor) const {
        if (bindings.empty()) {
            return nullptr;
        }
        const VkDescriptorSetLayoutCreateFlags flags =
            use_push_descriptor ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
        return device->GetLogical().CreateDescriptorSetLayout({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = flags,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        });
    }

    vk::DescriptorUpdateTemplate CreateTemplate(VkDescriptorSetLayout descriptor_set_layout,
                                                VkPipelineLayout pipeline_layout,
                                                bool use_push_descriptor) const {
        if (entries.empty()) {
            return nullptr;
        }
        const VkDescriptorUpdateTemplateType type =
            use_push_descriptor ? VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR
                                : VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
        return device->GetLogical().CreateDescriptorUpdateTemplate({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .descriptorUpdateEntryCount = static_cast<u32>(entries.size()),
            .pDescriptorUpdateEntries = entries.data(),
            .templateType = type,
            .descriptorSetLayout = descriptor_set_layout,
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .pipelineLayout = pipeline_layout,
            .set = 0,
        });
    }

    vk::PipelineLayout CreatePipelineLayout(VkDescriptorSetLayout descriptor_set_layout) const {
        using Shader::Backend::SPIRV::RenderAreaLayout;
        using Shader::Backend::SPIRV::RescalingLayout;
        const u32 size_offset = is_compute ? sizeof(RescalingLayout::down_factor) : 0u;
        const VkPushConstantRange range{
            .stageFlags = static_cast<VkShaderStageFlags>(
                is_compute ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ALL_GRAPHICS),
            .offset = 0,
            .size = static_cast<u32>(sizeof(RescalingLayout)) - size_offset +
                    static_cast<u32>(sizeof(RenderAreaLayout)),
        };
        return device->GetLogical().CreatePipelineLayout({
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = descriptor_set_layout ? 1U : 0U,
            .pSetLayouts = bindings.empty() ? nullptr : &descriptor_set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        });
    }

    void Add(const Shader::Info& info, VkShaderStageFlags stage) {
        is_compute |= (stage & VK_SHADER_STAGE_COMPUTE_BIT) != 0;

        Add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, stage, info.constant_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, stage, info.storage_buffers_descriptors);
        Add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, stage, info.texture_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, stage, info.image_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage, info.texture_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, stage, info.image_descriptors);
    }

private:
    template <typename Descriptors>
    void Add(VkDescriptorType type, VkShaderStageFlags stage, const Descriptors& descriptors) {
        const size_t num{descriptors.size()};
        for (size_t i = 0; i < num; ++i) {
            bindings.push_back({
                .binding = binding,
                .descriptorType = type,
                .descriptorCount = descriptors[i].count,
                .stageFlags = stage,
                .pImmutableSamplers = nullptr,
            });
            entries.push_back({
                .dstBinding = binding,
                .dstArrayElement = 0,
                .descriptorCount = descriptors[i].count,
                .descriptorType = type,
                .offset = offset,
                .stride = sizeof(DescriptorUpdateEntry),
            });
            ++binding;
            num_descriptors += descriptors[i].count;
            offset += sizeof(DescriptorUpdateEntry);
        }
    }

    const Device* device{};
    bool is_compute{};
    boost::container::small_vector<VkDescriptorSetLayoutBinding, 32> bindings;
    boost::container::small_vector<VkDescriptorUpdateTemplateEntry, 32> entries;
    u32 binding{};
    u32 num_descriptors{};
    size_t offset{};
};

class RescalingPushConstant {
public:
    explicit RescalingPushConstant() noexcept {}

    void PushTexture(bool is_rescaled) noexcept {
        *texture_ptr |= is_rescaled ? texture_bit : 0u;
        texture_bit <<= 1u;
        if (texture_bit == 0u) {
            texture_bit = 1u;
            ++texture_ptr;
        }
    }

    void PushImage(bool is_rescaled) noexcept {
        *image_ptr |= is_rescaled ? image_bit : 0u;
        image_bit <<= 1u;
        if (image_bit == 0u) {
            image_bit = 1u;
            ++image_ptr;
        }
    }

    const std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS>& Data() const noexcept {
        return words;
    }

private:
    std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS> words{};
    u32* texture_ptr{words.data()};
    u32* image_ptr{words.data() + Shader::Backend::SPIRV::NUM_TEXTURE_SCALING_WORDS};
    u32 texture_bit{1u};
    u32 image_bit{1u};
};

class RenderAreaPushConstant {
public:
    bool uses_render_area{};
    std::array<f32, 4> words{};
};

inline void PushImageDescriptors(TextureCache& texture_cache,
                                 GuestDescriptorQueue& guest_descriptor_queue,
                                 const Shader::Info& info, RescalingPushConstant& rescaling,
                                 const VkSampler*& samplers,
                                 const VideoCommon::ImageViewInOut*& views) {
    const u32 num_texture_buffers = Shader::NumDescriptors(info.texture_buffer_descriptors);
    const u32 num_image_buffers = Shader::NumDescriptors(info.image_buffer_descriptors);
    views += num_texture_buffers;
    views += num_image_buffers;
    for (const auto& desc : info.texture_descriptors) {
        for (u32 index = 0; index < desc.count; ++index) {
            const VideoCommon::ImageViewId image_view_id{(views++)->id};
            const VkSampler sampler{*(samplers++)};
            ImageView& image_view{texture_cache.GetImageView(image_view_id)};
            const VkImageView vk_image_view{image_view.Handle(desc.type)};
            guest_descriptor_queue.AddSampledImage(vk_image_view, sampler);
            rescaling.PushTexture(texture_cache.IsRescaling(image_view));
        }
    }
    for (const auto& desc : info.image_descriptors) {
        for (u32 index = 0; index < desc.count; ++index) {
            ImageView& image_view{texture_cache.GetImageView((views++)->id)};
            if (desc.is_written) {
                texture_cache.MarkModification(image_view.image_id);
            }
            const VkImageView vk_image_view{image_view.StorageView(desc.type, desc.format)};
            guest_descriptor_queue.AddImage(vk_image_view);
            rescaling.PushImage(texture_cache.IsRescaling(image_view));
        }
    }
}

} // namespace Vulkan
