// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <utility>

#include "common/common_types.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCommon {
struct SwizzleParameters;
}

namespace Vulkan {

class Device;
class StagingBufferPool;
class Scheduler;
class Image;
struct StagingBufferRef;

class ComputePass {
public:
    explicit ComputePass(const Device& device, DescriptorPool& descriptor_pool,
                         vk::Span<VkDescriptorSetLayoutBinding> bindings,
                         vk::Span<VkDescriptorUpdateTemplateEntry> templates,
                         const DescriptorBankInfo& bank_info,
                         vk::Span<VkPushConstantRange> push_constants, std::span<const u32> code);
    ~ComputePass();

protected:
    const Device& device;
    vk::DescriptorUpdateTemplate descriptor_template;
    vk::PipelineLayout layout;
    vk::Pipeline pipeline;
    vk::DescriptorSetLayout descriptor_set_layout;
    DescriptorAllocator descriptor_allocator;

private:
    vk::ShaderModule module;
};

class Uint8Pass final : public ComputePass {
public:
    explicit Uint8Pass(const Device& device_, Scheduler& scheduler_,
                       DescriptorPool& descriptor_pool_, StagingBufferPool& staging_buffer_pool_,
                       ComputePassDescriptorQueue& compute_pass_descriptor_queue_);
    ~Uint8Pass();

    /// Assemble uint8 indices into an uint16 index buffer
    /// Returns a pair with the staging buffer, and the offset where the assembled data is
    std::pair<VkBuffer, VkDeviceSize> Assemble(u32 num_vertices, VkBuffer src_buffer,
                                               u32 src_offset);

private:
    Scheduler& scheduler;
    StagingBufferPool& staging_buffer_pool;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
};

class QuadIndexedPass final : public ComputePass {
public:
    explicit QuadIndexedPass(const Device& device_, Scheduler& scheduler_,
                             DescriptorPool& descriptor_pool_,
                             StagingBufferPool& staging_buffer_pool_,
                             ComputePassDescriptorQueue& compute_pass_descriptor_queue_);
    ~QuadIndexedPass();

    std::pair<VkBuffer, VkDeviceSize> Assemble(
        Tegra::Engines::Maxwell3D::Regs::IndexFormat index_format, u32 num_vertices,
        u32 base_vertex, VkBuffer src_buffer, u32 src_offset, bool is_strip);

private:
    Scheduler& scheduler;
    StagingBufferPool& staging_buffer_pool;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
};

class ASTCDecoderPass final : public ComputePass {
public:
    explicit ASTCDecoderPass(const Device& device_, Scheduler& scheduler_,
                             DescriptorPool& descriptor_pool_,
                             StagingBufferPool& staging_buffer_pool_,
                             ComputePassDescriptorQueue& compute_pass_descriptor_queue_,
                             MemoryAllocator& memory_allocator_);
    ~ASTCDecoderPass();

    void Assemble(Image& image, const StagingBufferRef& map,
                  std::span<const VideoCommon::SwizzleParameters> swizzles);

private:
    Scheduler& scheduler;
    StagingBufferPool& staging_buffer_pool;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
    MemoryAllocator& memory_allocator;
};

} // namespace Vulkan
