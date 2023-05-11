// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include <boost/container/static_vector.hpp>

#include "common/common_types.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/rasterizer_accelerated.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_vulkan/blit_image.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_fence_manager.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_query_cache.h"
#include "video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Core {
class System;
}

namespace Core::Frontend {
class EmuWindow;
}

namespace Tegra::Engines {
class Maxwell3D;
}

namespace Vulkan {

struct ScreenInfo;

class StateTracker;

class AccelerateDMA : public Tegra::Engines::AccelerateDMAInterface {
public:
    explicit AccelerateDMA(BufferCache& buffer_cache, TextureCache& texture_cache,
                           Scheduler& scheduler);

    bool BufferCopy(GPUVAddr start_address, GPUVAddr end_address, u64 amount) override;

    bool BufferClear(GPUVAddr src_address, u64 amount, u32 value) override;

    bool ImageToBuffer(const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::ImageOperand& src,
                       const Tegra::DMA::BufferOperand& dst) override;

    bool BufferToImage(const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::BufferOperand& src,
                       const Tegra::DMA::ImageOperand& dst) override;

private:
    template <bool IS_IMAGE_UPLOAD>
    bool DmaBufferImageCopy(const Tegra::DMA::ImageCopy& copy_info,
                            const Tegra::DMA::BufferOperand& src,
                            const Tegra::DMA::ImageOperand& dst);

    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    Scheduler& scheduler;
};

class RasterizerVulkan final : public VideoCore::RasterizerAccelerated,
                               protected VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo> {
public:
    explicit RasterizerVulkan(Core::Frontend::EmuWindow& emu_window_, Tegra::GPU& gpu_,
                              Core::Memory::Memory& cpu_memory_, ScreenInfo& screen_info_,
                              const Device& device_, MemoryAllocator& memory_allocator_,
                              StateTracker& state_tracker_, Scheduler& scheduler_);
    ~RasterizerVulkan() override;

    void Draw(bool is_indexed, u32 instance_count) override;
    void DrawIndirect() override;
    void DrawTexture() override;
    void Clear(u32 layer_count) override;
    void DispatchCompute() override;
    void ResetCounter(VideoCore::QueryType type) override;
    void Query(GPUVAddr gpu_addr, VideoCore::QueryType type, std::optional<u64> timestamp) override;
    void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr, u32 size) override;
    void DisableGraphicsUniformBuffer(size_t stage, u32 index) override;
    void FlushAll() override;
    void FlushRegion(VAddr addr, u64 size,
                     VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    bool MustFlushRegion(VAddr addr, u64 size,
                         VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    VideoCore::RasterizerDownloadArea GetFlushArea(VAddr addr, u64 size) override;
    void InvalidateRegion(VAddr addr, u64 size,
                          VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void InnerInvalidation(std::span<const std::pair<VAddr, std::size_t>> sequences) override;
    void OnCPUWrite(VAddr addr, u64 size) override;
    void InvalidateGPUCache() override;
    void UnmapMemory(VAddr addr, u64 size) override;
    void ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) override;
    void SignalFence(std::function<void()>&& func) override;
    void SyncOperation(std::function<void()>&& func) override;
    void SignalSyncPoint(u32 value) override;
    void SignalReference() override;
    void ReleaseFences() override;
    void FlushAndInvalidateRegion(
        VAddr addr, u64 size, VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void WaitForIdle() override;
    void FragmentBarrier() override;
    void TiledCacheBarrier() override;
    void FlushCommands() override;
    void TickFrame() override;
    bool AccelerateConditionalRendering() override;
    bool AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                               const Tegra::Engines::Fermi2D::Surface& dst,
                               const Tegra::Engines::Fermi2D::Config& copy_config) override;
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override;
    void AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                  std::span<const u8> memory) override;
    bool AccelerateDisplay(const Tegra::FramebufferConfig& config, VAddr framebuffer_addr,
                           u32 pixel_stride) override;
    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback) override;

    void InitializeChannel(Tegra::Control::ChannelState& channel) override;

    void BindChannel(Tegra::Control::ChannelState& channel) override;

    void ReleaseChannel(s32 channel_id) override;

private:
    static constexpr size_t MAX_TEXTURES = 192;
    static constexpr size_t MAX_IMAGES = 48;
    static constexpr size_t MAX_IMAGE_VIEWS = MAX_TEXTURES + MAX_IMAGES;

    static constexpr VkDeviceSize DEFAULT_BUFFER_SIZE = 4 * sizeof(float);

    template <typename Func>
    void PrepareDraw(bool is_indexed, Func&&);

    void FlushWork();

    void UpdateDynamicStates();

    void BeginTransformFeedback();

    void EndTransformFeedback();

    void UpdateViewportsState(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateScissorsState(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthBias(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateBlendConstants(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthBounds(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateStencilFaces(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateLineWidth(Tegra::Engines::Maxwell3D::Regs& regs);

    void UpdateCullMode(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthBoundsTestEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthTestEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthWriteEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthCompareOp(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdatePrimitiveRestartEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateRasterizerDiscardEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthBiasEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateLogicOpEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthClampEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateFrontFace(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateStencilOp(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateStencilTestEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateLogicOp(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateBlending(Tegra::Engines::Maxwell3D::Regs& regs);

    void UpdateVertexInput(Tegra::Engines::Maxwell3D::Regs& regs);

    Tegra::GPU& gpu;

    ScreenInfo& screen_info;
    const Device& device;
    MemoryAllocator& memory_allocator;
    StateTracker& state_tracker;
    Scheduler& scheduler;

    StagingBufferPool staging_pool;
    DescriptorPool descriptor_pool;
    GuestDescriptorQueue guest_descriptor_queue;
    ComputePassDescriptorQueue compute_pass_descriptor_queue;
    BlitImageHelper blit_image;
    RenderPassCache render_pass_cache;

    TextureCacheRuntime texture_cache_runtime;
    TextureCache texture_cache;
    BufferCacheRuntime buffer_cache_runtime;
    BufferCache buffer_cache;
    PipelineCache pipeline_cache;
    QueryCache query_cache;
    AccelerateDMA accelerate_dma;
    FenceManager fence_manager;

    vk::Event wfi_event;

    boost::container::static_vector<u32, MAX_IMAGE_VIEWS> image_view_indices;
    std::array<VideoCommon::ImageViewId, MAX_IMAGE_VIEWS> image_view_ids;
    boost::container::static_vector<VkSampler, MAX_TEXTURES> sampler_handles;

    u32 draw_counter = 0;
};

} // namespace Vulkan
