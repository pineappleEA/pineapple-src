// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <vector>

#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
namespace {
VkBufferCopy MakeBufferCopy(const VideoCommon::BufferCopy& copy) {
    return VkBufferCopy{
        .srcOffset = copy.src_offset,
        .dstOffset = copy.dst_offset,
        .size = copy.size,
    };
}

VkIndexType IndexTypeFromNumElements(const Device& device, u32 num_elements) {
    if (num_elements <= 0xff && device.IsExtIndexTypeUint8Supported()) {
        return VK_INDEX_TYPE_UINT8_EXT;
    }
    if (num_elements <= 0xffff) {
        return VK_INDEX_TYPE_UINT16;
    }
    return VK_INDEX_TYPE_UINT32;
}

size_t BytesPerIndex(VkIndexType index_type) {
    switch (index_type) {
    case VK_INDEX_TYPE_UINT8_EXT:
        return 1;
    case VK_INDEX_TYPE_UINT16:
        return 2;
    case VK_INDEX_TYPE_UINT32:
        return 4;
    default:
        ASSERT_MSG(false, "Invalid index type={}", index_type);
        return 1;
    }
}

vk::Buffer CreateBuffer(const Device& device, u64 size) {
    VkBufferUsageFlags flags =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (device.IsExtTransformFeedbackSupported()) {
        flags |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    return device.GetLogical().CreateBuffer({
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = size,
        .usage = flags,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    });
}
} // Anonymous namespace

Buffer::Buffer(BufferCacheRuntime&, VideoCommon::NullBufferParams null_params)
    : VideoCommon::BufferBase<VideoCore::RasterizerInterface>(null_params) {}

Buffer::Buffer(BufferCacheRuntime& runtime, VideoCore::RasterizerInterface& rasterizer_,
               VAddr cpu_addr_, u64 size_bytes_)
    : VideoCommon::BufferBase<VideoCore::RasterizerInterface>(rasterizer_, cpu_addr_, size_bytes_),
      device{&runtime.device}, buffer{CreateBuffer(*device, SizeBytes())},
      commit{runtime.memory_allocator.Commit(buffer, MemoryUsage::DeviceLocal)} {
    if (runtime.device.HasDebuggingToolAttached()) {
        buffer.SetObjectNameEXT(fmt::format("Buffer 0x{:x}", CpuAddr()).c_str());
    }
}

VkBufferView Buffer::View(u32 offset, u32 size, VideoCore::Surface::PixelFormat format) {
    if (!device) {
        // Null buffer, return a null descriptor
        return VK_NULL_HANDLE;
    }
    const auto it{std::ranges::find_if(views, [offset, size, format](const BufferView& view) {
        return offset == view.offset && size == view.size && format == view.format;
    })};
    if (it != views.end()) {
        return *it->handle;
    }
    views.push_back({
        .offset = offset,
        .size = size,
        .format = format,
        .handle = device->GetLogical().CreateBufferView({
            .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .buffer = *buffer,
            .format = MaxwellToVK::SurfaceFormat(*device, FormatType::Buffer, false, format).format,
            .offset = offset,
            .range = size,
        }),
    });
    return *views.back().handle;
}

class QuadIndexBuffer {
public:
    QuadIndexBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                    Scheduler& scheduler_, StagingBufferPool& staging_pool_)
        : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
          staging_pool{staging_pool_} {}

    virtual ~QuadIndexBuffer() = default;

    void UpdateBuffer(u32 num_indices_) {
        if (num_indices_ <= num_indices) {
            return;
        }

        scheduler.Finish();

        num_indices = num_indices_;
        index_type = IndexTypeFromNumElements(device, num_indices);

        const u32 num_quads = GetQuadsNum(num_indices);
        const u32 num_triangle_indices = num_quads * 6;
        const u32 num_first_offset_copies = 4;
        const size_t bytes_per_index = BytesPerIndex(index_type);
        const size_t size_bytes = num_triangle_indices * bytes_per_index * num_first_offset_copies;
        buffer = device.GetLogical().CreateBuffer(VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size_bytes,
            .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        });
        if (device.HasDebuggingToolAttached()) {
            buffer.SetObjectNameEXT("Quad LUT");
        }
        memory_commit = memory_allocator.Commit(buffer, MemoryUsage::DeviceLocal);

        const StagingBufferRef staging = staging_pool.Request(size_bytes, MemoryUsage::Upload);
        u8* staging_data = staging.mapped_span.data();
        const size_t quad_size = bytes_per_index * 6;

        for (u32 first = 0; first < num_first_offset_copies; ++first) {
            for (u32 quad = 0; quad < num_quads; ++quad) {
                MakeAndUpdateIndices(staging_data, quad_size, quad, first);
                staging_data += quad_size;
            }
        }

        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([src_buffer = staging.buffer, src_offset = staging.offset,
                          dst_buffer = *buffer, size_bytes](vk::CommandBuffer cmdbuf) {
            const VkBufferCopy copy{
                .srcOffset = src_offset,
                .dstOffset = 0,
                .size = size_bytes,
            };
            const VkBufferMemoryBarrier write_barrier{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_INDEX_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = dst_buffer,
                .offset = 0,
                .size = size_bytes,
            };
            cmdbuf.CopyBuffer(src_buffer, dst_buffer, copy);
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, write_barrier);
        });
    }

    void BindBuffer(u32 first) {
        const VkIndexType index_type_ = index_type;
        const size_t sub_first_offset = static_cast<size_t>(first % 4) * GetQuadsNum(num_indices);
        const size_t offset =
            (sub_first_offset + GetQuadsNum(first)) * 6ULL * BytesPerIndex(index_type);
        scheduler.Record([buffer = *buffer, index_type_, offset](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindIndexBuffer(buffer, offset, index_type_);
        });
    }

protected:
    virtual u32 GetQuadsNum(u32 num_indices) const = 0;

    virtual void MakeAndUpdateIndices(u8* staging_data, size_t quad_size, u32 quad, u32 first) = 0;

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    StagingBufferPool& staging_pool;

    vk::Buffer buffer{};
    MemoryCommit memory_commit{};
    VkIndexType index_type{};
    u32 num_indices = 0;
};

class QuadArrayIndexBuffer : public QuadIndexBuffer {
public:
    QuadArrayIndexBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                         Scheduler& scheduler_, StagingBufferPool& staging_pool_)
        : QuadIndexBuffer(device_, memory_allocator_, scheduler_, staging_pool_) {}

    ~QuadArrayIndexBuffer() = default;

private:
    u32 GetQuadsNum(u32 num_indices_) const override {
        return num_indices_ / 4;
    }

    template <typename T>
    static std::array<T, 6> MakeIndices(u32 quad, u32 first) {
        std::array<T, 6> indices{0, 1, 2, 0, 2, 3};
        for (T& index : indices) {
            index = static_cast<T>(first + index + quad * 4);
        }
        return indices;
    }

    void MakeAndUpdateIndices(u8* staging_data, size_t quad_size, u32 quad, u32 first) override {
        switch (index_type) {
        case VK_INDEX_TYPE_UINT8_EXT:
            std::memcpy(staging_data, MakeIndices<u8>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT16:
            std::memcpy(staging_data, MakeIndices<u16>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT32:
            std::memcpy(staging_data, MakeIndices<u32>(quad, first).data(), quad_size);
            break;
        default:
            ASSERT(false);
            break;
        }
    }
};

class QuadStripIndexBuffer : public QuadIndexBuffer {
public:
    QuadStripIndexBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                         Scheduler& scheduler_, StagingBufferPool& staging_pool_)
        : QuadIndexBuffer(device_, memory_allocator_, scheduler_, staging_pool_) {}

    ~QuadStripIndexBuffer() = default;

private:
    u32 GetQuadsNum(u32 num_indices_) const override {
        return num_indices_ >= 4 ? (num_indices_ - 2) / 2 : 0;
    }

    template <typename T>
    static std::array<T, 6> MakeIndices(u32 quad, u32 first) {
        std::array<T, 6> indices{0, 3, 1, 0, 2, 3};
        for (T& index : indices) {
            index = static_cast<T>(first + index + quad * 2);
        }
        return indices;
    }

    void MakeAndUpdateIndices(u8* staging_data, size_t quad_size, u32 quad, u32 first) override {
        switch (index_type) {
        case VK_INDEX_TYPE_UINT8_EXT:
            std::memcpy(staging_data, MakeIndices<u8>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT16:
            std::memcpy(staging_data, MakeIndices<u16>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT32:
            std::memcpy(staging_data, MakeIndices<u32>(quad, first).data(), quad_size);
            break;
        default:
            ASSERT(false);
            break;
        }
    }
};

BufferCacheRuntime::BufferCacheRuntime(const Device& device_, MemoryAllocator& memory_allocator_,
                                       Scheduler& scheduler_, StagingBufferPool& staging_pool_,
                                       GuestDescriptorQueue& guest_descriptor_queue_,
                                       ComputePassDescriptorQueue& compute_pass_descriptor_queue,
                                       DescriptorPool& descriptor_pool)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
      staging_pool{staging_pool_}, guest_descriptor_queue{guest_descriptor_queue_},
      uint8_pass(device, scheduler, descriptor_pool, staging_pool, compute_pass_descriptor_queue),
      quad_index_pass(device, scheduler, descriptor_pool, staging_pool,
                      compute_pass_descriptor_queue) {
    quad_array_index_buffer = std::make_shared<QuadArrayIndexBuffer>(device_, memory_allocator_,
                                                                     scheduler_, staging_pool_);
    quad_strip_index_buffer = std::make_shared<QuadStripIndexBuffer>(device_, memory_allocator_,
                                                                     scheduler_, staging_pool_);
}

StagingBufferRef BufferCacheRuntime::UploadStagingBuffer(size_t size) {
    return staging_pool.Request(size, MemoryUsage::Upload);
}

StagingBufferRef BufferCacheRuntime::DownloadStagingBuffer(size_t size, bool deferred) {
    return staging_pool.Request(size, MemoryUsage::Download, deferred);
}

void BufferCacheRuntime::FreeDeferredStagingBuffer(StagingBufferRef& ref) {
    staging_pool.FreeDeferred(ref);
}

u64 BufferCacheRuntime::GetDeviceLocalMemory() const {
    return device.GetDeviceLocalMemory();
}

u64 BufferCacheRuntime::GetDeviceMemoryUsage() const {
    return device.GetDeviceMemoryUsage();
}

bool BufferCacheRuntime::CanReportMemoryUsage() const {
    return device.CanReportMemoryUsage();
}

void BufferCacheRuntime::Finish() {
    scheduler.Finish();
}

void BufferCacheRuntime::CopyBuffer(VkBuffer dst_buffer, VkBuffer src_buffer,
                                    std::span<const VideoCommon::BufferCopy> copies, bool barrier) {
    if (dst_buffer == VK_NULL_HANDLE || src_buffer == VK_NULL_HANDLE) {
        return;
    }
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    static constexpr VkMemoryBarrier WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };
    // Measuring a popular game, this number never exceeds the specified size once data is warmed up
    boost::container::small_vector<VkBufferCopy, 3> vk_copies(copies.size());
    std::ranges::transform(copies, vk_copies.begin(), MakeBufferCopy);
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([src_buffer, dst_buffer, vk_copies, barrier](vk::CommandBuffer cmdbuf) {
        if (barrier) {
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, READ_BARRIER);
        }
        cmdbuf.CopyBuffer(src_buffer, dst_buffer, vk_copies);
        if (barrier) {
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, WRITE_BARRIER);
        }
    });
}

void BufferCacheRuntime::PreCopyBarrier() {
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, READ_BARRIER);
    });
}

void BufferCacheRuntime::PostCopyBarrier() {
    static constexpr VkMemoryBarrier WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0, WRITE_BARRIER);
    });
}

void BufferCacheRuntime::ClearBuffer(VkBuffer dest_buffer, u32 offset, size_t size, u32 value) {
    if (dest_buffer == VK_NULL_HANDLE) {
        return;
    }
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    static constexpr VkMemoryBarrier WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dest_buffer, offset, size, value](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, READ_BARRIER);
        cmdbuf.FillBuffer(dest_buffer, offset, size, value);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0, WRITE_BARRIER);
    });
}

void BufferCacheRuntime::BindIndexBuffer(PrimitiveTopology topology, IndexFormat index_format,
                                         u32 base_vertex, u32 num_indices, VkBuffer buffer,
                                         u32 offset, [[maybe_unused]] u32 size) {
    VkIndexType vk_index_type = MaxwellToVK::IndexFormat(index_format);
    VkDeviceSize vk_offset = offset;
    VkBuffer vk_buffer = buffer;
    if (topology == PrimitiveTopology::Quads || topology == PrimitiveTopology::QuadStrip) {
        vk_index_type = VK_INDEX_TYPE_UINT32;
        std::tie(vk_buffer, vk_offset) =
            quad_index_pass.Assemble(index_format, num_indices, base_vertex, buffer, offset,
                                     topology == PrimitiveTopology::QuadStrip);
    } else if (vk_index_type == VK_INDEX_TYPE_UINT8_EXT && !device.IsExtIndexTypeUint8Supported()) {
        vk_index_type = VK_INDEX_TYPE_UINT16;
        std::tie(vk_buffer, vk_offset) = uint8_pass.Assemble(num_indices, buffer, offset);
    }
    if (vk_buffer == VK_NULL_HANDLE) {
        // Vulkan doesn't support null index buffers. Replace it with our own null buffer.
        ReserveNullBuffer();
        vk_buffer = *null_buffer;
    }
    scheduler.Record([vk_buffer, vk_offset, vk_index_type](vk::CommandBuffer cmdbuf) {
        cmdbuf.BindIndexBuffer(vk_buffer, vk_offset, vk_index_type);
    });
}

void BufferCacheRuntime::BindQuadIndexBuffer(PrimitiveTopology topology, u32 first, u32 count) {
    if (count == 0) {
        ReserveNullBuffer();
        scheduler.Record([this](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindIndexBuffer(*null_buffer, 0, VK_INDEX_TYPE_UINT32);
        });
        return;
    }

    if (topology == PrimitiveTopology::Quads) {
        quad_array_index_buffer->UpdateBuffer(first + count);
        quad_array_index_buffer->BindBuffer(first);
    } else if (topology == PrimitiveTopology::QuadStrip) {
        quad_strip_index_buffer->UpdateBuffer(first + count);
        quad_strip_index_buffer->BindBuffer(first);
    }
}

void BufferCacheRuntime::BindVertexBuffer(u32 index, VkBuffer buffer, u32 offset, u32 size,
                                          u32 stride) {
    if (index >= device.GetMaxVertexInputBindings()) {
        return;
    }
    if (device.IsExtExtendedDynamicStateSupported()) {
        scheduler.Record([index, buffer, offset, size, stride](vk::CommandBuffer cmdbuf) {
            const VkDeviceSize vk_offset = buffer != VK_NULL_HANDLE ? offset : 0;
            const VkDeviceSize vk_size = buffer != VK_NULL_HANDLE ? size : VK_WHOLE_SIZE;
            const VkDeviceSize vk_stride = stride;
            cmdbuf.BindVertexBuffers2EXT(index, 1, &buffer, &vk_offset, &vk_size, &vk_stride);
        });
    } else {
        if (!device.HasNullDescriptor() && buffer == VK_NULL_HANDLE) {
            ReserveNullBuffer();
            buffer = *null_buffer;
            offset = 0;
        }
        scheduler.Record([index, buffer, offset](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindVertexBuffer(index, buffer, offset);
        });
    }
}

void BufferCacheRuntime::BindTransformFeedbackBuffer(u32 index, VkBuffer buffer, u32 offset,
                                                     u32 size) {
    if (!device.IsExtTransformFeedbackSupported()) {
        // Already logged in the rasterizer
        return;
    }
    if (buffer == VK_NULL_HANDLE) {
        // Vulkan doesn't support null transform feedback buffers.
        // Replace it with our own null buffer.
        ReserveNullBuffer();
        buffer = *null_buffer;
        offset = 0;
        size = 0;
    }
    scheduler.Record([index, buffer, offset, size](vk::CommandBuffer cmdbuf) {
        const VkDeviceSize vk_offset = offset;
        const VkDeviceSize vk_size = size;
        cmdbuf.BindTransformFeedbackBuffersEXT(index, 1, &buffer, &vk_offset, &vk_size);
    });
}

void BufferCacheRuntime::ReserveNullBuffer() {
    if (null_buffer) {
        return;
    }
    VkBufferCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = 4,
        .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        create_info.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    create_info.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    null_buffer = device.GetLogical().CreateBuffer(create_info);
    if (device.HasDebuggingToolAttached()) {
        null_buffer.SetObjectNameEXT("Null buffer");
    }
    null_buffer_commit = memory_allocator.Commit(null_buffer, MemoryUsage::DeviceLocal);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([buffer = *null_buffer](vk::CommandBuffer cmdbuf) {
        cmdbuf.FillBuffer(buffer, 0, VK_WHOLE_SIZE, 0);
    });
}

} // namespace Vulkan
