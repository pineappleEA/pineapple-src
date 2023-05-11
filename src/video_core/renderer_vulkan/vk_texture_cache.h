// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/image_view_base.h"
#include "video_core/texture_cache/texture_cache_base.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Settings {
struct ResolutionScalingInfo;
}

namespace Vulkan {

using VideoCommon::ImageId;
using VideoCommon::NUM_RT;
using VideoCommon::Region2D;
using VideoCommon::RenderTargets;
using VideoCommon::SlotVector;
using VideoCore::Surface::PixelFormat;

class BlitImageHelper;
class DescriptorPool;
class Device;
class Image;
class ImageView;
class Framebuffer;
class RenderPassCache;
class StagingBufferPool;
class Scheduler;

class TextureCacheRuntime {
public:
    explicit TextureCacheRuntime(const Device& device_, Scheduler& scheduler_,
                                 MemoryAllocator& memory_allocator_,
                                 StagingBufferPool& staging_buffer_pool_,
                                 BlitImageHelper& blit_image_helper_,
                                 RenderPassCache& render_pass_cache_,
                                 DescriptorPool& descriptor_pool,
                                 ComputePassDescriptorQueue& compute_pass_descriptor_queue);

    void Finish();

    StagingBufferRef UploadStagingBuffer(size_t size);

    StagingBufferRef DownloadStagingBuffer(size_t size, bool deferred = false);

    void FreeDeferredStagingBuffer(StagingBufferRef& ref);

    void TickFrame();

    u64 GetDeviceLocalMemory() const;

    u64 GetDeviceMemoryUsage() const;

    bool CanReportMemoryUsage() const;

    void BlitImage(Framebuffer* dst_framebuffer, ImageView& dst, ImageView& src,
                   const Region2D& dst_region, const Region2D& src_region,
                   Tegra::Engines::Fermi2D::Filter filter,
                   Tegra::Engines::Fermi2D::Operation operation);

    void CopyImage(Image& dst, Image& src, std::span<const VideoCommon::ImageCopy> copies);

    void CopyImageMSAA(Image& dst, Image& src, std::span<const VideoCommon::ImageCopy> copies);

    bool ShouldReinterpret(Image& dst, Image& src);

    void ReinterpretImage(Image& dst, Image& src, std::span<const VideoCommon::ImageCopy> copies);

    void ConvertImage(Framebuffer* dst, ImageView& dst_view, ImageView& src_view);

    bool CanAccelerateImageUpload(Image&) const noexcept {
        return false;
    }

    bool CanUploadMSAA() const noexcept {
        // TODO: Implement buffer to MSAA uploads
        return false;
    }

    void AccelerateImageUpload(Image&, const StagingBufferRef&,
                               std::span<const VideoCommon::SwizzleParameters>);

    void InsertUploadMemoryBarrier() {}

    bool HasBrokenTextureViewFormats() const noexcept {
        // No known Vulkan driver has broken image views
        return false;
    }

    bool HasNativeBgr() const noexcept {
        // All known Vulkan drivers can natively handle BGR textures
        return true;
    }

    [[nodiscard]] VkBuffer GetTemporaryBuffer(size_t needed_size);

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;
    StagingBufferPool& staging_buffer_pool;
    BlitImageHelper& blit_image_helper;
    RenderPassCache& render_pass_cache;
    std::optional<ASTCDecoderPass> astc_decoder_pass;
    const Settings::ResolutionScalingInfo& resolution;

    static constexpr size_t indexing_slots = 8 * sizeof(size_t);
    std::array<vk::Buffer, indexing_slots> buffers{};
    std::array<std::unique_ptr<MemoryCommit>, indexing_slots> buffer_commits{};
};

class Image : public VideoCommon::ImageBase {
public:
    explicit Image(TextureCacheRuntime&, const VideoCommon::ImageInfo& info, GPUVAddr gpu_addr,
                   VAddr cpu_addr);
    explicit Image(const VideoCommon::NullImageParams&);

    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    Image(Image&&) = default;
    Image& operator=(Image&&) = default;

    void UploadMemory(VkBuffer buffer, VkDeviceSize offset,
                      std::span<const VideoCommon::BufferImageCopy> copies);

    void UploadMemory(const StagingBufferRef& map,
                      std::span<const VideoCommon::BufferImageCopy> copies);

    void DownloadMemory(VkBuffer buffer, VkDeviceSize offset,
                        std::span<const VideoCommon::BufferImageCopy> copies);

    void DownloadMemory(std::span<VkBuffer> buffers, std::span<VkDeviceSize> offsets,
                        std::span<const VideoCommon::BufferImageCopy> copies);

    void DownloadMemory(const StagingBufferRef& map,
                        std::span<const VideoCommon::BufferImageCopy> copies);

    [[nodiscard]] VkImage Handle() const noexcept {
        return current_image;
    }

    [[nodiscard]] VkImageAspectFlags AspectMask() const noexcept {
        return aspect_mask;
    }

    [[nodiscard]] VkImageView StorageImageView(s32 level) const noexcept {
        return *storage_image_views[level];
    }

    /// Returns true when the image is already initialized and mark it as initialized
    [[nodiscard]] bool ExchangeInitialization() noexcept {
        return std::exchange(initialized, true);
    }

    bool IsRescaled() const noexcept;

    bool ScaleUp(bool ignore = false);

    bool ScaleDown(bool ignore = false);

private:
    bool BlitScaleHelper(bool scale_up);

    bool NeedsScaleHelper() const;

    Scheduler* scheduler{};
    TextureCacheRuntime* runtime{};

    vk::Image original_image;
    MemoryCommit commit;
    std::vector<vk::ImageView> storage_image_views;
    VkImageAspectFlags aspect_mask = 0;
    bool initialized = false;
    vk::Image scaled_image{};
    MemoryCommit scaled_commit{};
    VkImage current_image{};

    std::unique_ptr<Framebuffer> scale_framebuffer;
    std::unique_ptr<ImageView> scale_view;

    std::unique_ptr<Framebuffer> normal_framebuffer;
    std::unique_ptr<ImageView> normal_view;
};

class ImageView : public VideoCommon::ImageViewBase {
public:
    explicit ImageView(TextureCacheRuntime&, const VideoCommon::ImageViewInfo&, ImageId, Image&);
    explicit ImageView(TextureCacheRuntime&, const VideoCommon::ImageViewInfo&, ImageId, Image&,
                       const SlotVector<Image>&);
    explicit ImageView(TextureCacheRuntime&, const VideoCommon::ImageInfo&,
                       const VideoCommon::ImageViewInfo&, GPUVAddr);
    explicit ImageView(TextureCacheRuntime&, const VideoCommon::NullImageViewParams&);

    ~ImageView();

    ImageView(const ImageView&) = delete;
    ImageView& operator=(const ImageView&) = delete;

    ImageView(ImageView&&) = default;
    ImageView& operator=(ImageView&&) = default;

    [[nodiscard]] VkImageView DepthView();

    [[nodiscard]] VkImageView StencilView();

    [[nodiscard]] VkImageView ColorView();

    [[nodiscard]] VkImageView StorageView(Shader::TextureType texture_type,
                                          Shader::ImageFormat image_format);

    [[nodiscard]] bool IsRescaled() const noexcept;

    [[nodiscard]] VkImageView Handle(Shader::TextureType texture_type) const noexcept {
        return *image_views[static_cast<size_t>(texture_type)];
    }

    [[nodiscard]] VkImage ImageHandle() const noexcept {
        return image_handle;
    }

    [[nodiscard]] VkImageView RenderTarget() const noexcept {
        return render_target;
    }

    [[nodiscard]] VkSampleCountFlagBits Samples() const noexcept {
        return samples;
    }

    [[nodiscard]] GPUVAddr GpuAddr() const noexcept {
        return gpu_addr;
    }

    [[nodiscard]] u32 BufferSize() const noexcept {
        return buffer_size;
    }

private:
    struct StorageViews {
        std::array<vk::ImageView, Shader::NUM_TEXTURE_TYPES> signeds;
        std::array<vk::ImageView, Shader::NUM_TEXTURE_TYPES> unsigneds;
    };

    [[nodiscard]] vk::ImageView MakeView(VkFormat vk_format, VkImageAspectFlags aspect_mask);

    const Device* device = nullptr;
    const SlotVector<Image>* slot_images = nullptr;

    std::array<vk::ImageView, Shader::NUM_TEXTURE_TYPES> image_views;
    std::unique_ptr<StorageViews> storage_views;
    vk::ImageView depth_view;
    vk::ImageView stencil_view;
    vk::ImageView color_view;
    VkImage image_handle = VK_NULL_HANDLE;
    VkImageView render_target = VK_NULL_HANDLE;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    u32 buffer_size = 0;
};

class ImageAlloc : public VideoCommon::ImageAllocBase {};

class Sampler {
public:
    explicit Sampler(TextureCacheRuntime&, const Tegra::Texture::TSCEntry&);

    [[nodiscard]] VkSampler Handle() const noexcept {
        return *sampler;
    }

private:
    vk::Sampler sampler;
};

class Framebuffer {
public:
    explicit Framebuffer(TextureCacheRuntime& runtime, std::span<ImageView*, NUM_RT> color_buffers,
                         ImageView* depth_buffer, const VideoCommon::RenderTargets& key);

    explicit Framebuffer(TextureCacheRuntime& runtime, ImageView* color_buffer,
                         ImageView* depth_buffer, VkExtent2D extent, bool is_rescaled);

    ~Framebuffer();

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    Framebuffer(Framebuffer&&) = default;
    Framebuffer& operator=(Framebuffer&&) = default;

    void CreateFramebuffer(TextureCacheRuntime& runtime,
                           std::span<ImageView*, NUM_RT> color_buffers, ImageView* depth_buffer,
                           bool is_rescaled = false);

    [[nodiscard]] VkFramebuffer Handle() const noexcept {
        return *framebuffer;
    }

    [[nodiscard]] VkRenderPass RenderPass() const noexcept {
        return renderpass;
    }

    [[nodiscard]] VkExtent2D RenderArea() const noexcept {
        return render_area;
    }

    [[nodiscard]] VkSampleCountFlagBits Samples() const noexcept {
        return samples;
    }

    [[nodiscard]] u32 NumColorBuffers() const noexcept {
        return num_color_buffers;
    }

    [[nodiscard]] u32 NumImages() const noexcept {
        return num_images;
    }

    [[nodiscard]] const std::array<VkImage, 9>& Images() const noexcept {
        return images;
    }

    [[nodiscard]] const std::array<VkImageSubresourceRange, 9>& ImageRanges() const noexcept {
        return image_ranges;
    }

    [[nodiscard]] bool HasAspectColorBit(size_t index) const noexcept {
        return (image_ranges.at(index).aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
    }

    [[nodiscard]] bool HasAspectDepthBit() const noexcept {
        return has_depth;
    }

    [[nodiscard]] bool HasAspectStencilBit() const noexcept {
        return has_stencil;
    }

private:
    vk::Framebuffer framebuffer;
    VkRenderPass renderpass{};
    VkExtent2D render_area{};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    u32 num_color_buffers = 0;
    u32 num_images = 0;
    std::array<VkImage, 9> images{};
    std::array<VkImageSubresourceRange, 9> image_ranges{};
    bool has_depth{};
    bool has_stencil{};
};

struct TextureCacheParams {
    static constexpr bool ENABLE_VALIDATION = true;
    static constexpr bool FRAMEBUFFER_BLITS = false;
    static constexpr bool HAS_EMULATED_COPIES = false;
    static constexpr bool HAS_DEVICE_MEMORY_INFO = true;
    static constexpr bool IMPLEMENTS_ASYNC_DOWNLOADS = true;

    using Runtime = Vulkan::TextureCacheRuntime;
    using Image = Vulkan::Image;
    using ImageAlloc = Vulkan::ImageAlloc;
    using ImageView = Vulkan::ImageView;
    using Sampler = Vulkan::Sampler;
    using Framebuffer = Vulkan::Framebuffer;
    using AsyncBuffer = Vulkan::StagingBufferRef;
    using BufferType = VkBuffer;
};

using TextureCache = VideoCommon::TextureCache<TextureCacheParams>;

} // namespace Vulkan
