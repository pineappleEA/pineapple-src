// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/thread_worker.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/shader_cache.h"

namespace Core {
class System;
}

namespace Shader::IR {
struct Program;
}

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

struct ComputePipelineCacheKey {
    u64 unique_hash;
    u32 shared_memory_size;
    std::array<u32, 3> workgroup_size;

    size_t Hash() const noexcept;

    bool operator==(const ComputePipelineCacheKey& rhs) const noexcept;

    bool operator!=(const ComputePipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }
};
static_assert(std::has_unique_object_representations_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<ComputePipelineCacheKey>);

} // namespace Vulkan

namespace std {

template <>
struct hash<Vulkan::ComputePipelineCacheKey> {
    size_t operator()(const Vulkan::ComputePipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};

} // namespace std

namespace Vulkan {

class ComputePipeline;
class DescriptorPool;
class Device;
class PipelineStatistics;
class RasterizerVulkan;
class RenderPassCache;
class Scheduler;

using VideoCommon::ShaderInfo;

struct ShaderPools {
    void ReleaseContents() {
        flow_block.ReleaseContents();
        block.ReleaseContents();
        inst.ReleaseContents();
    }

    Shader::ObjectPool<Shader::IR::Inst> inst;
    Shader::ObjectPool<Shader::IR::Block> block;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block;
};

class PipelineCache : public VideoCommon::ShaderCache {
public:
    explicit PipelineCache(RasterizerVulkan& rasterizer, const Device& device, Scheduler& scheduler,
                           DescriptorPool& descriptor_pool,
                           GuestDescriptorQueue& guest_descriptor_queue,
                           RenderPassCache& render_pass_cache, BufferCache& buffer_cache,
                           TextureCache& texture_cache, VideoCore::ShaderNotify& shader_notify_);
    ~PipelineCache();

    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipeline();

    [[nodiscard]] ComputePipeline* CurrentComputePipeline();

    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback);

private:
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipelineSlowPath();

    [[nodiscard]] GraphicsPipeline* BuiltPipeline(GraphicsPipeline* pipeline) const noexcept;

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline();

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        ShaderPools& pools, const GraphicsPipelineCacheKey& key,
        std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
        bool build_in_parallel);

    std::unique_ptr<ComputePipeline> CreateComputePipeline(const ComputePipelineCacheKey& key,
                                                           const ShaderInfo* shader);

    std::unique_ptr<ComputePipeline> CreateComputePipeline(ShaderPools& pools,
                                                           const ComputePipelineCacheKey& key,
                                                           Shader::Environment& env,
                                                           PipelineStatistics* statistics,
                                                           bool build_in_parallel);

    void SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                      const vk::PipelineCache& pipeline_cache, u32 cache_version);

    vk::PipelineCache LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                              u32 expected_cache_version);

    const Device& device;
    Scheduler& scheduler;
    DescriptorPool& descriptor_pool;
    GuestDescriptorQueue& guest_descriptor_queue;
    RenderPassCache& render_pass_cache;
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    VideoCore::ShaderNotify& shader_notify;
    bool use_asynchronous_shaders{};
    bool use_vulkan_pipeline_cache{};

    GraphicsPipelineCacheKey graphics_key{};
    GraphicsPipeline* current_pipeline{};

    std::unordered_map<ComputePipelineCacheKey, std::unique_ptr<ComputePipeline>> compute_cache;
    std::unordered_map<GraphicsPipelineCacheKey, std::unique_ptr<GraphicsPipeline>> graphics_cache;

    ShaderPools main_pools;

    Shader::Profile profile;
    Shader::HostTranslateInfo host_info;

    std::filesystem::path pipeline_cache_filename;

    std::filesystem::path vulkan_pipeline_cache_filename;
    vk::PipelineCache vulkan_pipeline_cache;

    Common::ThreadWorker workers;
    Common::ThreadWorker serialization_thread;
    DynamicFeatures dynamic_features;
};

} // namespace Vulkan
