// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <memory>
#include <numeric>

#include "video_core/buffer_cache/buffer_cache_base.h"

namespace VideoCommon {

using Core::Memory::YUZU_PAGESIZE;

template <class P>
BufferCache<P>::BufferCache(VideoCore::RasterizerInterface& rasterizer_,
                            Core::Memory::Memory& cpu_memory_, Runtime& runtime_)
    : runtime{runtime_}, rasterizer{rasterizer_}, cpu_memory{cpu_memory_}, memory_tracker{
                                                                               rasterizer} {
    // Ensure the first slot is used for the null buffer
    void(slot_buffers.insert(runtime, NullBufferParams{}));
    common_ranges.clear();
    inline_buffer_id = NULL_BUFFER_ID;

    if (!runtime.CanReportMemoryUsage()) {
        minimum_memory = DEFAULT_EXPECTED_MEMORY;
        critical_memory = DEFAULT_CRITICAL_MEMORY;
        return;
    }

    const s64 device_memory = static_cast<s64>(runtime.GetDeviceLocalMemory());
    const s64 min_spacing_expected = device_memory - 1_GiB - 512_MiB;
    const s64 min_spacing_critical = device_memory - 1_GiB;
    const s64 mem_threshold = std::min(device_memory, TARGET_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (3 * mem_threshold) / 10;
    minimum_memory = static_cast<u64>(
        std::max(std::min(device_memory - min_vacancy_expected, min_spacing_expected),
                 DEFAULT_EXPECTED_MEMORY));
    critical_memory = static_cast<u64>(
        std::max(std::min(device_memory - min_vacancy_critical, min_spacing_critical),
                 DEFAULT_CRITICAL_MEMORY));
}

template <class P>
void BufferCache<P>::RunGarbageCollector() {
    const bool aggressive_gc = total_used_memory >= critical_memory;
    const u64 ticks_to_destroy = aggressive_gc ? 60 : 120;
    int num_iterations = aggressive_gc ? 64 : 32;
    const auto clean_up = [this, &num_iterations](BufferId buffer_id) {
        if (num_iterations == 0) {
            return true;
        }
        --num_iterations;
        auto& buffer = slot_buffers[buffer_id];
        DownloadBufferMemory(buffer);
        DeleteBuffer(buffer_id);
        return false;
    };
    lru_cache.ForEachItemBelow(frame_tick - ticks_to_destroy, clean_up);
}

template <class P>
void BufferCache<P>::TickFrame() {
    // Calculate hits and shots and move hit bits to the right
    const u32 hits = std::reduce(uniform_cache_hits.begin(), uniform_cache_hits.end());
    const u32 shots = std::reduce(uniform_cache_shots.begin(), uniform_cache_shots.end());
    std::copy_n(uniform_cache_hits.begin(), uniform_cache_hits.size() - 1,
                uniform_cache_hits.begin() + 1);
    std::copy_n(uniform_cache_shots.begin(), uniform_cache_shots.size() - 1,
                uniform_cache_shots.begin() + 1);
    uniform_cache_hits[0] = 0;
    uniform_cache_shots[0] = 0;

    const bool skip_preferred = hits * 256 < shots * 251;
    uniform_buffer_skip_cache_size = skip_preferred ? DEFAULT_SKIP_CACHE_SIZE : 0;

    // If we can obtain the memory info, use it instead of the estimate.
    if (runtime.CanReportMemoryUsage()) {
        total_used_memory = runtime.GetDeviceMemoryUsage();
    }
    if (total_used_memory >= minimum_memory) {
        RunGarbageCollector();
    }
    ++frame_tick;
    delayed_destruction_ring.Tick();

    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        for (auto& buffer : async_buffers_death_ring) {
            runtime.FreeDeferredStagingBuffer(buffer);
        }
        async_buffers_death_ring.clear();
    }
}

template <class P>
void BufferCache<P>::WriteMemory(VAddr cpu_addr, u64 size) {
    memory_tracker.MarkRegionAsCpuModified(cpu_addr, size);
    if (memory_tracker.IsRegionGpuModified(cpu_addr, size)) {
        const IntervalType subtract_interval{cpu_addr, cpu_addr + size};
        ClearDownload(subtract_interval);
        common_ranges.subtract(subtract_interval);
    }
}

template <class P>
void BufferCache<P>::CachedWriteMemory(VAddr cpu_addr, u64 size) {
    memory_tracker.CachedCpuWrite(cpu_addr, size);
}

template <class P>
std::optional<VideoCore::RasterizerDownloadArea> BufferCache<P>::GetFlushArea(VAddr cpu_addr,
                                                                              u64 size) {
    std::optional<VideoCore::RasterizerDownloadArea> area{};
    area.emplace();
    VAddr cpu_addr_start_aligned = Common::AlignDown(cpu_addr, Core::Memory::YUZU_PAGESIZE);
    VAddr cpu_addr_end_aligned = Common::AlignUp(cpu_addr + size, Core::Memory::YUZU_PAGESIZE);
    area->start_address = cpu_addr_start_aligned;
    area->end_address = cpu_addr_end_aligned;
    if (memory_tracker.IsRegionPreflushable(cpu_addr, size)) {
        area->preemtive = true;
        return area;
    };
    memory_tracker.MarkRegionAsPreflushable(cpu_addr_start_aligned,
                                            cpu_addr_end_aligned - cpu_addr_start_aligned);
    area->preemtive = !IsRegionGpuModified(cpu_addr, size);
    return area;
}

template <class P>
void BufferCache<P>::DownloadMemory(VAddr cpu_addr, u64 size) {
    WaitOnAsyncFlushes(cpu_addr, size);
    ForEachBufferInRange(cpu_addr, size, [&](BufferId, Buffer& buffer) {
        DownloadBufferMemory(buffer, cpu_addr, size);
    });
}

template <class P>
void BufferCache<P>::WaitOnAsyncFlushes(VAddr cpu_addr, u64 size) {
    bool must_wait = false;
    ForEachInOverlapCounter(async_downloads, cpu_addr, size,
                            [&](VAddr, VAddr, int) { must_wait = true; });
    bool must_release = false;
    ForEachInRangeSet(pending_ranges, cpu_addr, size, [&](VAddr, VAddr) { must_release = true; });
    if (must_release) {
        std::function<void()> tmp([]() {});
        rasterizer.SignalFence(std::move(tmp));
    }
    if (must_wait || must_release) {
        rasterizer.ReleaseFences();
    }
}

template <class P>
void BufferCache<P>::ClearDownload(IntervalType subtract_interval) {
    RemoveEachInOverlapCounter(async_downloads, subtract_interval, -1024);
    uncommitted_ranges.subtract(subtract_interval);
    pending_ranges.subtract(subtract_interval);
    for (auto& interval_set : committed_ranges) {
        interval_set.subtract(subtract_interval);
    }
}

template <class P>
bool BufferCache<P>::DMACopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount) {
    const std::optional<VAddr> cpu_src_address = gpu_memory->GpuToCpuAddress(src_address);
    const std::optional<VAddr> cpu_dest_address = gpu_memory->GpuToCpuAddress(dest_address);
    if (!cpu_src_address || !cpu_dest_address) {
        return false;
    }
    const bool source_dirty = IsRegionRegistered(*cpu_src_address, amount);
    const bool dest_dirty = IsRegionRegistered(*cpu_dest_address, amount);
    if (!source_dirty && !dest_dirty) {
        return false;
    }

    const IntervalType subtract_interval{*cpu_dest_address, *cpu_dest_address + amount};
    WaitOnAsyncFlushes(*cpu_src_address, static_cast<u32>(amount));
    ClearDownload(subtract_interval);

    BufferId buffer_a;
    BufferId buffer_b;
    do {
        has_deleted_buffers = false;
        buffer_a = FindBuffer(*cpu_src_address, static_cast<u32>(amount));
        buffer_b = FindBuffer(*cpu_dest_address, static_cast<u32>(amount));
    } while (has_deleted_buffers);
    auto& src_buffer = slot_buffers[buffer_a];
    auto& dest_buffer = slot_buffers[buffer_b];
    SynchronizeBuffer(src_buffer, *cpu_src_address, static_cast<u32>(amount));
    SynchronizeBuffer(dest_buffer, *cpu_dest_address, static_cast<u32>(amount));
    std::array copies{BufferCopy{
        .src_offset = src_buffer.Offset(*cpu_src_address),
        .dst_offset = dest_buffer.Offset(*cpu_dest_address),
        .size = amount,
    }};

    boost::container::small_vector<IntervalType, 4> tmp_intervals;
    auto mirror = [&](VAddr base_address, VAddr base_address_end) {
        const u64 size = base_address_end - base_address;
        const VAddr diff = base_address - *cpu_src_address;
        const VAddr new_base_address = *cpu_dest_address + diff;
        const IntervalType add_interval{new_base_address, new_base_address + size};
        tmp_intervals.push_back(add_interval);
        uncommitted_ranges.add(add_interval);
        pending_ranges.add(add_interval);
    };
    ForEachInRangeSet(common_ranges, *cpu_src_address, amount, mirror);
    // This subtraction in this order is important for overlapping copies.
    common_ranges.subtract(subtract_interval);
    const bool has_new_downloads = tmp_intervals.size() != 0;
    for (const IntervalType& add_interval : tmp_intervals) {
        common_ranges.add(add_interval);
    }
    runtime.CopyBuffer(dest_buffer, src_buffer, copies);
    if (has_new_downloads) {
        memory_tracker.MarkRegionAsGpuModified(*cpu_dest_address, amount);
    }
    tmp_buffer.resize(amount);
    cpu_memory.ReadBlockUnsafe(*cpu_src_address, tmp_buffer.data(), amount);
    cpu_memory.WriteBlockUnsafe(*cpu_dest_address, tmp_buffer.data(), amount);
    return true;
}

template <class P>
bool BufferCache<P>::DMAClear(GPUVAddr dst_address, u64 amount, u32 value) {
    const std::optional<VAddr> cpu_dst_address = gpu_memory->GpuToCpuAddress(dst_address);
    if (!cpu_dst_address) {
        return false;
    }
    const bool dest_dirty = IsRegionRegistered(*cpu_dst_address, amount);
    if (!dest_dirty) {
        return false;
    }

    const size_t size = amount * sizeof(u32);
    const IntervalType subtract_interval{*cpu_dst_address, *cpu_dst_address + size};
    ClearDownload(subtract_interval);
    common_ranges.subtract(subtract_interval);

    const BufferId buffer = FindBuffer(*cpu_dst_address, static_cast<u32>(size));
    auto& dest_buffer = slot_buffers[buffer];
    const u32 offset = dest_buffer.Offset(*cpu_dst_address);
    runtime.ClearBuffer(dest_buffer, offset, size, value);
    return true;
}

template <class P>
std::pair<typename P::Buffer*, u32> BufferCache<P>::ObtainBuffer(GPUVAddr gpu_addr, u32 size,
                                                                 ObtainBufferSynchronize sync_info,
                                                                 ObtainBufferOperation post_op) {
    const std::optional<VAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
    if (!cpu_addr) {
        return {&slot_buffers[NULL_BUFFER_ID], 0};
    }
    const BufferId buffer_id = FindBuffer(*cpu_addr, size);
    Buffer& buffer = slot_buffers[buffer_id];

    // synchronize op
    switch (sync_info) {
    case ObtainBufferSynchronize::FullSynchronize:
        SynchronizeBuffer(buffer, *cpu_addr, size);
        break;
    default:
        break;
    }

    switch (post_op) {
    case ObtainBufferOperation::MarkAsWritten:
        MarkWrittenBuffer(buffer_id, *cpu_addr, size);
        break;
    case ObtainBufferOperation::DiscardWrite: {
        IntervalType interval{*cpu_addr, size};
        ClearDownload(interval);
        break;
    }
    default:
        break;
    }

    return {&buffer, buffer.Offset(*cpu_addr)};
}

template <class P>
void BufferCache<P>::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                               u32 size) {
    const std::optional<VAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
    const Binding binding{
        .cpu_addr = *cpu_addr,
        .size = size,
        .buffer_id = BufferId{},
    };
    uniform_buffers[stage][index] = binding;
}

template <class P>
void BufferCache<P>::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    uniform_buffers[stage][index] = NULL_BINDING;
}

template <class P>
void BufferCache<P>::UpdateGraphicsBuffers(bool is_indexed) {
    MICROPROFILE_SCOPE(GPU_PrepareBuffers);
    do {
        has_deleted_buffers = false;
        DoUpdateGraphicsBuffers(is_indexed);
    } while (has_deleted_buffers);
}

template <class P>
void BufferCache<P>::UpdateComputeBuffers() {
    MICROPROFILE_SCOPE(GPU_PrepareBuffers);
    do {
        has_deleted_buffers = false;
        DoUpdateComputeBuffers();
    } while (has_deleted_buffers);
}

template <class P>
void BufferCache<P>::BindHostGeometryBuffers(bool is_indexed) {
    MICROPROFILE_SCOPE(GPU_BindUploadBuffers);
    if (is_indexed) {
        BindHostIndexBuffer();
    } else if constexpr (!HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT) {
        const auto& draw_state = maxwell3d->draw_manager->GetDrawState();
        if (draw_state.topology == Maxwell::PrimitiveTopology::Quads ||
            draw_state.topology == Maxwell::PrimitiveTopology::QuadStrip) {
            runtime.BindQuadIndexBuffer(draw_state.topology, draw_state.vertex_buffer.first,
                                        draw_state.vertex_buffer.count);
        }
    }
    BindHostVertexBuffers();
    BindHostTransformFeedbackBuffers();
    if (current_draw_indirect) {
        BindHostDrawIndirectBuffers();
    }
}

template <class P>
void BufferCache<P>::BindHostStageBuffers(size_t stage) {
    MICROPROFILE_SCOPE(GPU_BindUploadBuffers);
    BindHostGraphicsUniformBuffers(stage);
    BindHostGraphicsStorageBuffers(stage);
    BindHostGraphicsTextureBuffers(stage);
}

template <class P>
void BufferCache<P>::BindHostComputeBuffers() {
    MICROPROFILE_SCOPE(GPU_BindUploadBuffers);
    BindHostComputeUniformBuffers();
    BindHostComputeStorageBuffers();
    BindHostComputeTextureBuffers();
}

template <class P>
void BufferCache<P>::SetUniformBuffersState(const std::array<u32, NUM_STAGES>& mask,
                                            const UniformBufferSizes* sizes) {
    if constexpr (HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS) {
        if (enabled_uniform_buffer_masks != mask) {
            if constexpr (IS_OPENGL) {
                fast_bound_uniform_buffers.fill(0);
            }
            dirty_uniform_buffers.fill(~u32{0});
            uniform_buffer_binding_sizes.fill({});
        }
    }
    enabled_uniform_buffer_masks = mask;
    uniform_buffer_sizes = sizes;
}

template <class P>
void BufferCache<P>::SetComputeUniformBufferState(u32 mask,
                                                  const ComputeUniformBufferSizes* sizes) {
    enabled_compute_uniform_buffer_mask = mask;
    compute_uniform_buffer_sizes = sizes;
}

template <class P>
void BufferCache<P>::UnbindGraphicsStorageBuffers(size_t stage) {
    enabled_storage_buffers[stage] = 0;
    written_storage_buffers[stage] = 0;
}

template <class P>
void BufferCache<P>::BindGraphicsStorageBuffer(size_t stage, size_t ssbo_index, u32 cbuf_index,
                                               u32 cbuf_offset, bool is_written) {
    enabled_storage_buffers[stage] |= 1U << ssbo_index;
    written_storage_buffers[stage] |= (is_written ? 1U : 0U) << ssbo_index;

    const auto& cbufs = maxwell3d->state.shader_stages[stage];
    const GPUVAddr ssbo_addr = cbufs.const_buffers[cbuf_index].address + cbuf_offset;
    storage_buffers[stage][ssbo_index] = StorageBufferBinding(ssbo_addr, cbuf_index, is_written);
}

template <class P>
void BufferCache<P>::UnbindGraphicsTextureBuffers(size_t stage) {
    enabled_texture_buffers[stage] = 0;
    written_texture_buffers[stage] = 0;
    image_texture_buffers[stage] = 0;
}

template <class P>
void BufferCache<P>::BindGraphicsTextureBuffer(size_t stage, size_t tbo_index, GPUVAddr gpu_addr,
                                               u32 size, PixelFormat format, bool is_written,
                                               bool is_image) {
    enabled_texture_buffers[stage] |= 1U << tbo_index;
    written_texture_buffers[stage] |= (is_written ? 1U : 0U) << tbo_index;
    if constexpr (SEPARATE_IMAGE_BUFFERS_BINDINGS) {
        image_texture_buffers[stage] |= (is_image ? 1U : 0U) << tbo_index;
    }
    texture_buffers[stage][tbo_index] = GetTextureBufferBinding(gpu_addr, size, format);
}

template <class P>
void BufferCache<P>::UnbindComputeStorageBuffers() {
    enabled_compute_storage_buffers = 0;
    written_compute_storage_buffers = 0;
    image_compute_texture_buffers = 0;
}

template <class P>
void BufferCache<P>::BindComputeStorageBuffer(size_t ssbo_index, u32 cbuf_index, u32 cbuf_offset,
                                              bool is_written) {
    enabled_compute_storage_buffers |= 1U << ssbo_index;
    written_compute_storage_buffers |= (is_written ? 1U : 0U) << ssbo_index;

    const auto& launch_desc = kepler_compute->launch_description;
    ASSERT(((launch_desc.const_buffer_enable_mask >> cbuf_index) & 1) != 0);

    const auto& cbufs = launch_desc.const_buffer_config;
    const GPUVAddr ssbo_addr = cbufs[cbuf_index].Address() + cbuf_offset;
    compute_storage_buffers[ssbo_index] = StorageBufferBinding(ssbo_addr, cbuf_index, is_written);
}

template <class P>
void BufferCache<P>::UnbindComputeTextureBuffers() {
    enabled_compute_texture_buffers = 0;
    written_compute_texture_buffers = 0;
    image_compute_texture_buffers = 0;
}

template <class P>
void BufferCache<P>::BindComputeTextureBuffer(size_t tbo_index, GPUVAddr gpu_addr, u32 size,
                                              PixelFormat format, bool is_written, bool is_image) {
    enabled_compute_texture_buffers |= 1U << tbo_index;
    written_compute_texture_buffers |= (is_written ? 1U : 0U) << tbo_index;
    if constexpr (SEPARATE_IMAGE_BUFFERS_BINDINGS) {
        image_compute_texture_buffers |= (is_image ? 1U : 0U) << tbo_index;
    }
    compute_texture_buffers[tbo_index] = GetTextureBufferBinding(gpu_addr, size, format);
}

template <class P>
void BufferCache<P>::FlushCachedWrites() {
    memory_tracker.FlushCachedWrites();
}

template <class P>
bool BufferCache<P>::HasUncommittedFlushes() const noexcept {
    return !uncommitted_ranges.empty() || !committed_ranges.empty();
}

template <class P>
void BufferCache<P>::AccumulateFlushes() {
    if (uncommitted_ranges.empty()) {
        return;
    }
    committed_ranges.emplace_back(std::move(uncommitted_ranges));
}

template <class P>
bool BufferCache<P>::ShouldWaitAsyncFlushes() const noexcept {
    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        return (!async_buffers.empty() && async_buffers.front().has_value());
    } else {
        return false;
    }
}

template <class P>
void BufferCache<P>::CommitAsyncFlushesHigh() {
    AccumulateFlushes();

    if (committed_ranges.empty()) {
        if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {

            async_buffers.emplace_back(std::optional<Async_Buffer>{});
        }
        return;
    }
    MICROPROFILE_SCOPE(GPU_DownloadMemory);

    pending_ranges.clear();
    auto it = committed_ranges.begin();
    while (it != committed_ranges.end()) {
        auto& current_intervals = *it;
        auto next_it = std::next(it);
        while (next_it != committed_ranges.end()) {
            for (auto& interval : *next_it) {
                current_intervals.subtract(interval);
            }
            next_it++;
        }
        it++;
    }

    boost::container::small_vector<std::pair<BufferCopy, BufferId>, 1> downloads;
    u64 total_size_bytes = 0;
    u64 largest_copy = 0;
    for (const IntervalSet& intervals : committed_ranges) {
        for (auto& interval : intervals) {
            const std::size_t size = interval.upper() - interval.lower();
            const VAddr cpu_addr = interval.lower();
            ForEachBufferInRange(cpu_addr, size, [&](BufferId buffer_id, Buffer& buffer) {
                const VAddr buffer_start = buffer.CpuAddr();
                const VAddr buffer_end = buffer_start + buffer.SizeBytes();
                const VAddr new_start = std::max(buffer_start, cpu_addr);
                const VAddr new_end = std::min(buffer_end, cpu_addr + size);
                memory_tracker.ForEachDownloadRange(
                    new_start, new_end - new_start, false, [&](u64 cpu_addr_out, u64 range_size) {
                        const VAddr buffer_addr = buffer.CpuAddr();
                        const auto add_download = [&](VAddr start, VAddr end) {
                            const u64 new_offset = start - buffer_addr;
                            const u64 new_size = end - start;
                            downloads.push_back({
                                BufferCopy{
                                    .src_offset = new_offset,
                                    .dst_offset = total_size_bytes,
                                    .size = new_size,
                                },
                                buffer_id,
                            });
                            // Align up to avoid cache conflicts
                            constexpr u64 align = 64ULL;
                            constexpr u64 mask = ~(align - 1ULL);
                            total_size_bytes += (new_size + align - 1) & mask;
                            largest_copy = std::max(largest_copy, new_size);
                        };

                        ForEachInRangeSet(common_ranges, cpu_addr_out, range_size, add_download);
                    });
            });
        }
    }
    committed_ranges.clear();
    if (downloads.empty()) {
        if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {

            async_buffers.emplace_back(std::optional<Async_Buffer>{});
        }
        return;
    }
    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        auto download_staging = runtime.DownloadStagingBuffer(total_size_bytes, true);
        boost::container::small_vector<BufferCopy, 4> normalized_copies;
        IntervalSet new_async_range{};
        runtime.PreCopyBarrier();
        for (auto& [copy, buffer_id] : downloads) {
            copy.dst_offset += download_staging.offset;
            const std::array copies{copy};
            BufferCopy second_copy{copy};
            Buffer& buffer = slot_buffers[buffer_id];
            second_copy.src_offset = static_cast<size_t>(buffer.CpuAddr()) + copy.src_offset;
            VAddr orig_cpu_addr = static_cast<VAddr>(second_copy.src_offset);
            const IntervalType base_interval{orig_cpu_addr, orig_cpu_addr + copy.size};
            async_downloads += std::make_pair(base_interval, 1);
            runtime.CopyBuffer(download_staging.buffer, buffer, copies, false);
            normalized_copies.push_back(second_copy);
        }
        runtime.PostCopyBarrier();
        pending_downloads.emplace_back(std::move(normalized_copies));
        async_buffers.emplace_back(download_staging);
    } else {
        if (!Settings::IsGPULevelHigh()) {
            committed_ranges.clear();
            uncommitted_ranges.clear();
        } else {
            if constexpr (USE_MEMORY_MAPS) {
                auto download_staging = runtime.DownloadStagingBuffer(total_size_bytes);
                runtime.PreCopyBarrier();
                for (auto& [copy, buffer_id] : downloads) {
                    // Have in mind the staging buffer offset for the copy
                    copy.dst_offset += download_staging.offset;
                    const std::array copies{copy};
                    runtime.CopyBuffer(download_staging.buffer, slot_buffers[buffer_id], copies,
                                       false);
                }
                runtime.PostCopyBarrier();
                runtime.Finish();
                for (const auto& [copy, buffer_id] : downloads) {
                    const Buffer& buffer = slot_buffers[buffer_id];
                    const VAddr cpu_addr = buffer.CpuAddr() + copy.src_offset;
                    // Undo the modified offset
                    const u64 dst_offset = copy.dst_offset - download_staging.offset;
                    const u8* read_mapped_memory = download_staging.mapped_span.data() + dst_offset;
                    cpu_memory.WriteBlockUnsafe(cpu_addr, read_mapped_memory, copy.size);
                }
            } else {
                const std::span<u8> immediate_buffer = ImmediateBuffer(largest_copy);
                for (const auto& [copy, buffer_id] : downloads) {
                    Buffer& buffer = slot_buffers[buffer_id];
                    buffer.ImmediateDownload(copy.src_offset,
                                             immediate_buffer.subspan(0, copy.size));
                    const VAddr cpu_addr = buffer.CpuAddr() + copy.src_offset;
                    cpu_memory.WriteBlockUnsafe(cpu_addr, immediate_buffer.data(), copy.size);
                }
            }
        }
    }
}

template <class P>
void BufferCache<P>::CommitAsyncFlushes() {
    CommitAsyncFlushesHigh();
}

template <class P>
void BufferCache<P>::PopAsyncFlushes() {
    MICROPROFILE_SCOPE(GPU_DownloadMemory);
    PopAsyncBuffers();
}

template <class P>
void BufferCache<P>::PopAsyncBuffers() {
    if (async_buffers.empty()) {
        return;
    }
    if (!async_buffers.front().has_value()) {
        async_buffers.pop_front();
        return;
    }
    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        auto& downloads = pending_downloads.front();
        auto& async_buffer = async_buffers.front();
        u8* base = async_buffer->mapped_span.data();
        const size_t base_offset = async_buffer->offset;
        for (const auto& copy : downloads) {
            const VAddr cpu_addr = static_cast<VAddr>(copy.src_offset);
            const u64 dst_offset = copy.dst_offset - base_offset;
            const u8* read_mapped_memory = base + dst_offset;
            ForEachInOverlapCounter(
                async_downloads, cpu_addr, copy.size, [&](VAddr start, VAddr end, int count) {
                    cpu_memory.WriteBlockUnsafe(start, &read_mapped_memory[start - cpu_addr],
                                                end - start);
                    if (count == 1) {
                        const IntervalType base_interval{start, end};
                        common_ranges.subtract(base_interval);
                    }
                });
            const IntervalType subtract_interval{cpu_addr, cpu_addr + copy.size};
            RemoveEachInOverlapCounter(async_downloads, subtract_interval, -1);
        }
        async_buffers_death_ring.emplace_back(*async_buffer);
        async_buffers.pop_front();
        pending_downloads.pop_front();
    }
}

template <class P>
bool BufferCache<P>::IsRegionGpuModified(VAddr addr, size_t size) {
    bool is_dirty = false;
    ForEachInRangeSet(common_ranges, addr, size, [&](VAddr, VAddr) { is_dirty = true; });
    return is_dirty;
}

template <class P>
bool BufferCache<P>::IsRegionRegistered(VAddr addr, size_t size) {
    const VAddr end_addr = addr + size;
    const u64 page_end = Common::DivCeil(end_addr, CACHING_PAGESIZE);
    for (u64 page = addr >> CACHING_PAGEBITS; page < page_end;) {
        const BufferId buffer_id = page_table[page];
        if (!buffer_id) {
            ++page;
            continue;
        }
        Buffer& buffer = slot_buffers[buffer_id];
        const VAddr buf_start_addr = buffer.CpuAddr();
        const VAddr buf_end_addr = buf_start_addr + buffer.SizeBytes();
        if (buf_start_addr < end_addr && addr < buf_end_addr) {
            return true;
        }
        page = Common::DivCeil(end_addr, CACHING_PAGESIZE);
    }
    return false;
}

template <class P>
bool BufferCache<P>::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker.IsRegionCpuModified(addr, size);
}

template <class P>
void BufferCache<P>::BindHostIndexBuffer() {
    Buffer& buffer = slot_buffers[index_buffer.buffer_id];
    TouchBuffer(buffer, index_buffer.buffer_id);
    const u32 offset = buffer.Offset(index_buffer.cpu_addr);
    const u32 size = index_buffer.size;
    const auto& draw_state = maxwell3d->draw_manager->GetDrawState();
    if (!draw_state.inline_index_draw_indexes.empty()) [[unlikely]] {
        if constexpr (USE_MEMORY_MAPS) {
            auto upload_staging = runtime.UploadStagingBuffer(size);
            std::array<BufferCopy, 1> copies{
                {BufferCopy{.src_offset = upload_staging.offset, .dst_offset = 0, .size = size}}};
            std::memcpy(upload_staging.mapped_span.data(),
                        draw_state.inline_index_draw_indexes.data(), size);
            runtime.CopyBuffer(buffer, upload_staging.buffer, copies);
        } else {
            buffer.ImmediateUpload(0, draw_state.inline_index_draw_indexes);
        }
    } else {
        SynchronizeBuffer(buffer, index_buffer.cpu_addr, size);
    }
    if constexpr (HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT) {
        const u32 new_offset =
            offset + draw_state.index_buffer.first * draw_state.index_buffer.FormatSizeInBytes();
        runtime.BindIndexBuffer(buffer, new_offset, size);
    } else {
        runtime.BindIndexBuffer(draw_state.topology, draw_state.index_buffer.format,
                                draw_state.index_buffer.first, draw_state.index_buffer.count,
                                buffer, offset, size);
    }
}

template <class P>
void BufferCache<P>::BindHostVertexBuffers() {
    auto& flags = maxwell3d->dirty.flags;
    for (u32 index = 0; index < NUM_VERTEX_BUFFERS; ++index) {
        const Binding& binding = vertex_buffers[index];
        Buffer& buffer = slot_buffers[binding.buffer_id];
        TouchBuffer(buffer, binding.buffer_id);
        SynchronizeBuffer(buffer, binding.cpu_addr, binding.size);
        if (!flags[Dirty::VertexBuffer0 + index]) {
            continue;
        }
        flags[Dirty::VertexBuffer0 + index] = false;

        const u32 stride = maxwell3d->regs.vertex_streams[index].stride;
        const u32 offset = buffer.Offset(binding.cpu_addr);
        runtime.BindVertexBuffer(index, buffer, offset, binding.size, stride);
    }
}

template <class P>
void BufferCache<P>::BindHostDrawIndirectBuffers() {
    const auto bind_buffer = [this](const Binding& binding) {
        Buffer& buffer = slot_buffers[binding.buffer_id];
        TouchBuffer(buffer, binding.buffer_id);
        SynchronizeBuffer(buffer, binding.cpu_addr, binding.size);
    };
    if (current_draw_indirect->include_count) {
        bind_buffer(count_buffer_binding);
    }
    bind_buffer(indirect_buffer_binding);
}

template <class P>
void BufferCache<P>::BindHostGraphicsUniformBuffers(size_t stage) {
    u32 dirty = ~0U;
    if constexpr (HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS) {
        dirty = std::exchange(dirty_uniform_buffers[stage], 0);
    }
    u32 binding_index = 0;
    ForEachEnabledBit(enabled_uniform_buffer_masks[stage], [&](u32 index) {
        const bool needs_bind = ((dirty >> index) & 1) != 0;
        BindHostGraphicsUniformBuffer(stage, index, binding_index, needs_bind);
        if constexpr (NEEDS_BIND_UNIFORM_INDEX) {
            ++binding_index;
        }
    });
}

template <class P>
void BufferCache<P>::BindHostGraphicsUniformBuffer(size_t stage, u32 index, u32 binding_index,
                                                   bool needs_bind) {
    const Binding& binding = uniform_buffers[stage][index];
    const VAddr cpu_addr = binding.cpu_addr;
    const u32 size = std::min(binding.size, (*uniform_buffer_sizes)[stage][index]);
    Buffer& buffer = slot_buffers[binding.buffer_id];
    TouchBuffer(buffer, binding.buffer_id);
    const bool use_fast_buffer = binding.buffer_id != NULL_BUFFER_ID &&
                                 size <= uniform_buffer_skip_cache_size &&
                                 !memory_tracker.IsRegionGpuModified(cpu_addr, size);
    if (use_fast_buffer) {
        if constexpr (IS_OPENGL) {
            if (runtime.HasFastBufferSubData()) {
                // Fast path for Nvidia
                const bool should_fast_bind =
                    !HasFastUniformBufferBound(stage, binding_index) ||
                    uniform_buffer_binding_sizes[stage][binding_index] != size;
                if (should_fast_bind) {
                    // We only have to bind when the currently bound buffer is not the fast version
                    fast_bound_uniform_buffers[stage] |= 1U << binding_index;
                    uniform_buffer_binding_sizes[stage][binding_index] = size;
                    runtime.BindFastUniformBuffer(stage, binding_index, size);
                }
                const auto span = ImmediateBufferWithData(cpu_addr, size);
                runtime.PushFastUniformBuffer(stage, binding_index, span);
                return;
            }
        }
        if constexpr (IS_OPENGL) {
            fast_bound_uniform_buffers[stage] |= 1U << binding_index;
            uniform_buffer_binding_sizes[stage][binding_index] = size;
        }
        // Stream buffer path to avoid stalling on non-Nvidia drivers or Vulkan
        const std::span<u8> span = runtime.BindMappedUniformBuffer(stage, binding_index, size);
        cpu_memory.ReadBlockUnsafe(cpu_addr, span.data(), size);
        return;
    }
    // Classic cached path
    const bool sync_cached = SynchronizeBuffer(buffer, cpu_addr, size);
    if (sync_cached) {
        ++uniform_cache_hits[0];
    }
    ++uniform_cache_shots[0];

    // Skip binding if it's not needed and if the bound buffer is not the fast version
    // This exists to avoid instances where the fast buffer is bound and a GPU write happens
    needs_bind |= HasFastUniformBufferBound(stage, binding_index);
    if constexpr (HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS) {
        needs_bind |= uniform_buffer_binding_sizes[stage][binding_index] != size;
    }
    if (!needs_bind) {
        return;
    }
    const u32 offset = buffer.Offset(cpu_addr);
    if constexpr (IS_OPENGL) {
        // Fast buffer will be unbound
        fast_bound_uniform_buffers[stage] &= ~(1U << binding_index);

        // Mark the index as dirty if offset doesn't match
        const bool is_copy_bind = offset != 0 && !runtime.SupportsNonZeroUniformOffset();
        dirty_uniform_buffers[stage] |= (is_copy_bind ? 1U : 0U) << index;
    }
    if constexpr (HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS) {
        uniform_buffer_binding_sizes[stage][binding_index] = size;
    }
    if constexpr (NEEDS_BIND_UNIFORM_INDEX) {
        runtime.BindUniformBuffer(stage, binding_index, buffer, offset, size);
    } else {
        runtime.BindUniformBuffer(buffer, offset, size);
    }
}

template <class P>
void BufferCache<P>::BindHostGraphicsStorageBuffers(size_t stage) {
    u32 binding_index = 0;
    ForEachEnabledBit(enabled_storage_buffers[stage], [&](u32 index) {
        const Binding& binding = storage_buffers[stage][index];
        Buffer& buffer = slot_buffers[binding.buffer_id];
        TouchBuffer(buffer, binding.buffer_id);
        const u32 size = binding.size;
        SynchronizeBuffer(buffer, binding.cpu_addr, size);

        const u32 offset = buffer.Offset(binding.cpu_addr);
        const bool is_written = ((written_storage_buffers[stage] >> index) & 1) != 0;
        if constexpr (NEEDS_BIND_STORAGE_INDEX) {
            runtime.BindStorageBuffer(stage, binding_index, buffer, offset, size, is_written);
            ++binding_index;
        } else {
            runtime.BindStorageBuffer(buffer, offset, size, is_written);
        }
    });
}

template <class P>
void BufferCache<P>::BindHostGraphicsTextureBuffers(size_t stage) {
    ForEachEnabledBit(enabled_texture_buffers[stage], [&](u32 index) {
        const TextureBufferBinding& binding = texture_buffers[stage][index];
        Buffer& buffer = slot_buffers[binding.buffer_id];
        const u32 size = binding.size;
        SynchronizeBuffer(buffer, binding.cpu_addr, size);

        const u32 offset = buffer.Offset(binding.cpu_addr);
        const PixelFormat format = binding.format;
        if constexpr (SEPARATE_IMAGE_BUFFERS_BINDINGS) {
            if (((image_texture_buffers[stage] >> index) & 1) != 0) {
                runtime.BindImageBuffer(buffer, offset, size, format);
            } else {
                runtime.BindTextureBuffer(buffer, offset, size, format);
            }
        } else {
            runtime.BindTextureBuffer(buffer, offset, size, format);
        }
    });
}

template <class P>
void BufferCache<P>::BindHostTransformFeedbackBuffers() {
    if (maxwell3d->regs.transform_feedback_enabled == 0) {
        return;
    }
    for (u32 index = 0; index < NUM_TRANSFORM_FEEDBACK_BUFFERS; ++index) {
        const Binding& binding = transform_feedback_buffers[index];
        Buffer& buffer = slot_buffers[binding.buffer_id];
        TouchBuffer(buffer, binding.buffer_id);
        const u32 size = binding.size;
        SynchronizeBuffer(buffer, binding.cpu_addr, size);

        const u32 offset = buffer.Offset(binding.cpu_addr);
        runtime.BindTransformFeedbackBuffer(index, buffer, offset, size);
    }
}

template <class P>
void BufferCache<P>::BindHostComputeUniformBuffers() {
    if constexpr (HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS) {
        // Mark all uniform buffers as dirty
        dirty_uniform_buffers.fill(~u32{0});
        fast_bound_uniform_buffers.fill(0);
    }
    u32 binding_index = 0;
    ForEachEnabledBit(enabled_compute_uniform_buffer_mask, [&](u32 index) {
        const Binding& binding = compute_uniform_buffers[index];
        Buffer& buffer = slot_buffers[binding.buffer_id];
        TouchBuffer(buffer, binding.buffer_id);
        const u32 size = std::min(binding.size, (*compute_uniform_buffer_sizes)[index]);
        SynchronizeBuffer(buffer, binding.cpu_addr, size);

        const u32 offset = buffer.Offset(binding.cpu_addr);
        if constexpr (NEEDS_BIND_UNIFORM_INDEX) {
            runtime.BindComputeUniformBuffer(binding_index, buffer, offset, size);
            ++binding_index;
        } else {
            runtime.BindUniformBuffer(buffer, offset, size);
        }
    });
}

template <class P>
void BufferCache<P>::BindHostComputeStorageBuffers() {
    u32 binding_index = 0;
    ForEachEnabledBit(enabled_compute_storage_buffers, [&](u32 index) {
        const Binding& binding = compute_storage_buffers[index];
        Buffer& buffer = slot_buffers[binding.buffer_id];
        TouchBuffer(buffer, binding.buffer_id);
        const u32 size = binding.size;
        SynchronizeBuffer(buffer, binding.cpu_addr, size);

        const u32 offset = buffer.Offset(binding.cpu_addr);
        const bool is_written = ((written_compute_storage_buffers >> index) & 1) != 0;
        if constexpr (NEEDS_BIND_STORAGE_INDEX) {
            runtime.BindComputeStorageBuffer(binding_index, buffer, offset, size, is_written);
            ++binding_index;
        } else {
            runtime.BindStorageBuffer(buffer, offset, size, is_written);
        }
    });
}

template <class P>
void BufferCache<P>::BindHostComputeTextureBuffers() {
    ForEachEnabledBit(enabled_compute_texture_buffers, [&](u32 index) {
        const TextureBufferBinding& binding = compute_texture_buffers[index];
        Buffer& buffer = slot_buffers[binding.buffer_id];
        const u32 size = binding.size;
        SynchronizeBuffer(buffer, binding.cpu_addr, size);

        const u32 offset = buffer.Offset(binding.cpu_addr);
        const PixelFormat format = binding.format;
        if constexpr (SEPARATE_IMAGE_BUFFERS_BINDINGS) {
            if (((image_compute_texture_buffers >> index) & 1) != 0) {
                runtime.BindImageBuffer(buffer, offset, size, format);
            } else {
                runtime.BindTextureBuffer(buffer, offset, size, format);
            }
        } else {
            runtime.BindTextureBuffer(buffer, offset, size, format);
        }
    });
}

template <class P>
void BufferCache<P>::DoUpdateGraphicsBuffers(bool is_indexed) {
    do {
        has_deleted_buffers = false;
        if (is_indexed) {
            UpdateIndexBuffer();
        }
        UpdateVertexBuffers();
        UpdateTransformFeedbackBuffers();
        for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
            UpdateUniformBuffers(stage);
            UpdateStorageBuffers(stage);
            UpdateTextureBuffers(stage);
        }
        if (current_draw_indirect) {
            UpdateDrawIndirect();
        }
    } while (has_deleted_buffers);
}

template <class P>
void BufferCache<P>::DoUpdateComputeBuffers() {
    UpdateComputeUniformBuffers();
    UpdateComputeStorageBuffers();
    UpdateComputeTextureBuffers();
}

template <class P>
void BufferCache<P>::UpdateIndexBuffer() {
    // We have to check for the dirty flags and index count
    // The index count is currently changed without updating the dirty flags
    const auto& draw_state = maxwell3d->draw_manager->GetDrawState();
    const auto& index_buffer_ref = draw_state.index_buffer;
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::IndexBuffer]) {
        return;
    }
    flags[Dirty::IndexBuffer] = false;
    if (!draw_state.inline_index_draw_indexes.empty()) [[unlikely]] {
        auto inline_index_size = static_cast<u32>(draw_state.inline_index_draw_indexes.size());
        u32 buffer_size = Common::AlignUp(inline_index_size, CACHING_PAGESIZE);
        if (inline_buffer_id == NULL_BUFFER_ID) [[unlikely]] {
            inline_buffer_id = CreateBuffer(0, buffer_size);
        }
        if (slot_buffers[inline_buffer_id].SizeBytes() < buffer_size) [[unlikely]] {
            slot_buffers.erase(inline_buffer_id);
            inline_buffer_id = CreateBuffer(0, buffer_size);
        }
        index_buffer = Binding{
            .cpu_addr = 0,
            .size = inline_index_size,
            .buffer_id = inline_buffer_id,
        };
        return;
    }

    const GPUVAddr gpu_addr_begin = index_buffer_ref.StartAddress();
    const GPUVAddr gpu_addr_end = index_buffer_ref.EndAddress();
    const std::optional<VAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr_begin);
    const u32 address_size = static_cast<u32>(gpu_addr_end - gpu_addr_begin);
    const u32 draw_size =
        (index_buffer_ref.count + index_buffer_ref.first) * index_buffer_ref.FormatSizeInBytes();
    const u32 size = std::min(address_size, draw_size);
    if (size == 0 || !cpu_addr) {
        index_buffer = NULL_BINDING;
        return;
    }
    index_buffer = Binding{
        .cpu_addr = *cpu_addr,
        .size = size,
        .buffer_id = FindBuffer(*cpu_addr, size),
    };
}

template <class P>
void BufferCache<P>::UpdateVertexBuffers() {
    auto& flags = maxwell3d->dirty.flags;
    if (!maxwell3d->dirty.flags[Dirty::VertexBuffers]) {
        return;
    }
    flags[Dirty::VertexBuffers] = false;

    for (u32 index = 0; index < NUM_VERTEX_BUFFERS; ++index) {
        UpdateVertexBuffer(index);
    }
}

template <class P>
void BufferCache<P>::UpdateVertexBuffer(u32 index) {
    if (!maxwell3d->dirty.flags[Dirty::VertexBuffer0 + index]) {
        return;
    }
    const auto& array = maxwell3d->regs.vertex_streams[index];
    const auto& limit = maxwell3d->regs.vertex_stream_limits[index];
    const GPUVAddr gpu_addr_begin = array.Address();
    const GPUVAddr gpu_addr_end = limit.Address() + 1;
    const std::optional<VAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr_begin);
    const u32 address_size = static_cast<u32>(gpu_addr_end - gpu_addr_begin);
    u32 size = address_size; // TODO: Analyze stride and number of vertices
    if (array.enable == 0 || size == 0 || !cpu_addr) {
        vertex_buffers[index] = NULL_BINDING;
        return;
    }
    if (!gpu_memory->IsWithinGPUAddressRange(gpu_addr_end)) {
        size = static_cast<u32>(gpu_memory->MaxContinuousRange(gpu_addr_begin, size));
    }
    vertex_buffers[index] = Binding{
        .cpu_addr = *cpu_addr,
        .size = size,
        .buffer_id = FindBuffer(*cpu_addr, size),
    };
}

template <class P>
void BufferCache<P>::UpdateDrawIndirect() {
    const auto update = [this](GPUVAddr gpu_addr, size_t size, Binding& binding) {
        const std::optional<VAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
        if (!cpu_addr) {
            binding = NULL_BINDING;
            return;
        }
        binding = Binding{
            .cpu_addr = *cpu_addr,
            .size = static_cast<u32>(size),
            .buffer_id = FindBuffer(*cpu_addr, static_cast<u32>(size)),
        };
    };
    if (current_draw_indirect->include_count) {
        update(current_draw_indirect->count_start_address, sizeof(u32), count_buffer_binding);
    }
    update(current_draw_indirect->indirect_start_address, current_draw_indirect->buffer_size,
           indirect_buffer_binding);
}

template <class P>
void BufferCache<P>::UpdateUniformBuffers(size_t stage) {
    ForEachEnabledBit(enabled_uniform_buffer_masks[stage], [&](u32 index) {
        Binding& binding = uniform_buffers[stage][index];
        if (binding.buffer_id) {
            // Already updated
            return;
        }
        // Mark as dirty
        if constexpr (HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS) {
            dirty_uniform_buffers[stage] |= 1U << index;
        }
        // Resolve buffer
        binding.buffer_id = FindBuffer(binding.cpu_addr, binding.size);
    });
}

template <class P>
void BufferCache<P>::UpdateStorageBuffers(size_t stage) {
    const u32 written_mask = written_storage_buffers[stage];
    ForEachEnabledBit(enabled_storage_buffers[stage], [&](u32 index) {
        // Resolve buffer
        Binding& binding = storage_buffers[stage][index];
        const BufferId buffer_id = FindBuffer(binding.cpu_addr, binding.size);
        binding.buffer_id = buffer_id;
        // Mark buffer as written if needed
        if (((written_mask >> index) & 1) != 0) {
            MarkWrittenBuffer(buffer_id, binding.cpu_addr, binding.size);
        }
    });
}

template <class P>
void BufferCache<P>::UpdateTextureBuffers(size_t stage) {
    ForEachEnabledBit(enabled_texture_buffers[stage], [&](u32 index) {
        Binding& binding = texture_buffers[stage][index];
        binding.buffer_id = FindBuffer(binding.cpu_addr, binding.size);
        // Mark buffer as written if needed
        if (((written_texture_buffers[stage] >> index) & 1) != 0) {
            MarkWrittenBuffer(binding.buffer_id, binding.cpu_addr, binding.size);
        }
    });
}

template <class P>
void BufferCache<P>::UpdateTransformFeedbackBuffers() {
    if (maxwell3d->regs.transform_feedback_enabled == 0) {
        return;
    }
    for (u32 index = 0; index < NUM_TRANSFORM_FEEDBACK_BUFFERS; ++index) {
        UpdateTransformFeedbackBuffer(index);
    }
}

template <class P>
void BufferCache<P>::UpdateTransformFeedbackBuffer(u32 index) {
    const auto& binding = maxwell3d->regs.transform_feedback.buffers[index];
    const GPUVAddr gpu_addr = binding.Address() + binding.start_offset;
    const u32 size = binding.size;
    const std::optional<VAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
    if (binding.enable == 0 || size == 0 || !cpu_addr) {
        transform_feedback_buffers[index] = NULL_BINDING;
        return;
    }
    const BufferId buffer_id = FindBuffer(*cpu_addr, size);
    transform_feedback_buffers[index] = Binding{
        .cpu_addr = *cpu_addr,
        .size = size,
        .buffer_id = buffer_id,
    };
    MarkWrittenBuffer(buffer_id, *cpu_addr, size);
}

template <class P>
void BufferCache<P>::UpdateComputeUniformBuffers() {
    ForEachEnabledBit(enabled_compute_uniform_buffer_mask, [&](u32 index) {
        Binding& binding = compute_uniform_buffers[index];
        binding = NULL_BINDING;
        const auto& launch_desc = kepler_compute->launch_description;
        if (((launch_desc.const_buffer_enable_mask >> index) & 1) != 0) {
            const auto& cbuf = launch_desc.const_buffer_config[index];
            const std::optional<VAddr> cpu_addr = gpu_memory->GpuToCpuAddress(cbuf.Address());
            if (cpu_addr) {
                binding.cpu_addr = *cpu_addr;
                binding.size = cbuf.size;
            }
        }
        binding.buffer_id = FindBuffer(binding.cpu_addr, binding.size);
    });
}

template <class P>
void BufferCache<P>::UpdateComputeStorageBuffers() {
    ForEachEnabledBit(enabled_compute_storage_buffers, [&](u32 index) {
        // Resolve buffer
        Binding& binding = compute_storage_buffers[index];
        binding.buffer_id = FindBuffer(binding.cpu_addr, binding.size);
        // Mark as written if needed
        if (((written_compute_storage_buffers >> index) & 1) != 0) {
            MarkWrittenBuffer(binding.buffer_id, binding.cpu_addr, binding.size);
        }
    });
}

template <class P>
void BufferCache<P>::UpdateComputeTextureBuffers() {
    ForEachEnabledBit(enabled_compute_texture_buffers, [&](u32 index) {
        Binding& binding = compute_texture_buffers[index];
        binding.buffer_id = FindBuffer(binding.cpu_addr, binding.size);
        // Mark as written if needed
        if (((written_compute_texture_buffers >> index) & 1) != 0) {
            MarkWrittenBuffer(binding.buffer_id, binding.cpu_addr, binding.size);
        }
    });
}

template <class P>
void BufferCache<P>::MarkWrittenBuffer(BufferId buffer_id, VAddr cpu_addr, u32 size) {
    memory_tracker.MarkRegionAsGpuModified(cpu_addr, size);

    if (memory_tracker.IsRegionCpuModified(cpu_addr, size)) {
        SynchronizeBuffer(slot_buffers[buffer_id], cpu_addr, size);
    }

    const IntervalType base_interval{cpu_addr, cpu_addr + size};
    common_ranges.add(base_interval);
    uncommitted_ranges.add(base_interval);
    pending_ranges.add(base_interval);
}

template <class P>
BufferId BufferCache<P>::FindBuffer(VAddr cpu_addr, u32 size) {
    if (cpu_addr == 0) {
        return NULL_BUFFER_ID;
    }
    const u64 page = cpu_addr >> CACHING_PAGEBITS;
    const BufferId buffer_id = page_table[page];
    if (!buffer_id) {
        return CreateBuffer(cpu_addr, size);
    }
    const Buffer& buffer = slot_buffers[buffer_id];
    if (buffer.IsInBounds(cpu_addr, size)) {
        return buffer_id;
    }
    return CreateBuffer(cpu_addr, size);
}

template <class P>
typename BufferCache<P>::OverlapResult BufferCache<P>::ResolveOverlaps(VAddr cpu_addr,
                                                                       u32 wanted_size) {
    static constexpr int STREAM_LEAP_THRESHOLD = 16;
    std::vector<BufferId> overlap_ids;
    VAddr begin = cpu_addr;
    VAddr end = cpu_addr + wanted_size;
    int stream_score = 0;
    bool has_stream_leap = false;
    if (begin == 0) {
        return OverlapResult{
            .ids = std::move(overlap_ids),
            .begin = begin,
            .end = end,
            .has_stream_leap = has_stream_leap,
        };
    }
    for (; cpu_addr >> CACHING_PAGEBITS < Common::DivCeil(end, CACHING_PAGESIZE);
         cpu_addr += CACHING_PAGESIZE) {
        const BufferId overlap_id = page_table[cpu_addr >> CACHING_PAGEBITS];
        if (!overlap_id) {
            continue;
        }
        Buffer& overlap = slot_buffers[overlap_id];
        if (overlap.IsPicked()) {
            continue;
        }
        overlap_ids.push_back(overlap_id);
        overlap.Pick();
        const VAddr overlap_cpu_addr = overlap.CpuAddr();
        const bool expands_left = overlap_cpu_addr < begin;
        if (expands_left) {
            begin = overlap_cpu_addr;
        }
        const VAddr overlap_end = overlap_cpu_addr + overlap.SizeBytes();
        const bool expands_right = overlap_end > end;
        if (overlap_end > end) {
            end = overlap_end;
        }
        stream_score += overlap.StreamScore();
        if (stream_score > STREAM_LEAP_THRESHOLD && !has_stream_leap) {
            // When this memory region has been joined a bunch of times, we assume it's being used
            // as a stream buffer. Increase the size to skip constantly recreating buffers.
            has_stream_leap = true;
            if (expands_right) {
                begin -= YUZU_PAGESIZE * 256;
                // We're about to increment cpu_addr by YUZU_PAGESIZE, but have not yet checked for
                // a buffer at the new begin.
                cpu_addr = begin - YUZU_PAGESIZE;
            }
            if (expands_left) {
                end += CACHING_PAGESIZE * 256;
            }
        }
    }
    return OverlapResult{
        .ids = std::move(overlap_ids),
        .begin = begin,
        .end = end,
        .has_stream_leap = has_stream_leap,
    };
}

template <class P>
void BufferCache<P>::JoinOverlap(BufferId new_buffer_id, BufferId overlap_id,
                                 bool accumulate_stream_score) {
    Buffer& new_buffer = slot_buffers[new_buffer_id];
    Buffer& overlap = slot_buffers[overlap_id];
    if (accumulate_stream_score) {
        new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
    }
    boost::container::small_vector<BufferCopy, 10> copies;
    const size_t dst_base_offset = overlap.CpuAddr() - new_buffer.CpuAddr();
    copies.push_back(BufferCopy{
        .src_offset = 0,
        .dst_offset = dst_base_offset,
        .size = overlap.SizeBytes(),
    });
    runtime.CopyBuffer(new_buffer, overlap, copies);
    DeleteBuffer(overlap_id, true);
}

template <class P>
BufferId BufferCache<P>::CreateBuffer(VAddr cpu_addr, u32 wanted_size) {
    VAddr cpu_addr_end = Common::AlignUp(cpu_addr + wanted_size, CACHING_PAGESIZE);
    cpu_addr = Common::AlignDown(cpu_addr, CACHING_PAGESIZE);
    wanted_size = static_cast<u32>(cpu_addr_end - cpu_addr);
    const OverlapResult overlap = ResolveOverlaps(cpu_addr, wanted_size);
    const u32 size = static_cast<u32>(overlap.end - overlap.begin);
    const BufferId new_buffer_id = slot_buffers.insert(runtime, rasterizer, overlap.begin, size);
    auto& new_buffer = slot_buffers[new_buffer_id];
    runtime.ClearBuffer(new_buffer, 0, new_buffer.SizeBytes(), 0);
    for (const BufferId overlap_id : overlap.ids) {
        JoinOverlap(new_buffer_id, overlap_id, !overlap.has_stream_leap);
    }
    Register(new_buffer_id);
    TouchBuffer(new_buffer, new_buffer_id);
    return new_buffer_id;
}

template <class P>
void BufferCache<P>::Register(BufferId buffer_id) {
    ChangeRegister<true>(buffer_id);
}

template <class P>
void BufferCache<P>::Unregister(BufferId buffer_id) {
    ChangeRegister<false>(buffer_id);
}

template <class P>
template <bool insert>
void BufferCache<P>::ChangeRegister(BufferId buffer_id) {
    Buffer& buffer = slot_buffers[buffer_id];
    const auto size = buffer.SizeBytes();
    if (insert) {
        total_used_memory += Common::AlignUp(size, 1024);
        buffer.setLRUID(lru_cache.Insert(buffer_id, frame_tick));
    } else {
        total_used_memory -= Common::AlignUp(size, 1024);
        lru_cache.Free(buffer.getLRUID());
    }
    const VAddr cpu_addr_begin = buffer.CpuAddr();
    const VAddr cpu_addr_end = cpu_addr_begin + size;
    const u64 page_begin = cpu_addr_begin / CACHING_PAGESIZE;
    const u64 page_end = Common::DivCeil(cpu_addr_end, CACHING_PAGESIZE);
    for (u64 page = page_begin; page != page_end; ++page) {
        if constexpr (insert) {
            page_table[page] = buffer_id;
        } else {
            page_table[page] = BufferId{};
        }
    }
}

template <class P>
void BufferCache<P>::TouchBuffer(Buffer& buffer, BufferId buffer_id) noexcept {
    if (buffer_id != NULL_BUFFER_ID) {
        lru_cache.Touch(buffer.getLRUID(), frame_tick);
    }
}

template <class P>
bool BufferCache<P>::SynchronizeBuffer(Buffer& buffer, VAddr cpu_addr, u32 size) {
    return SynchronizeBufferImpl(buffer, cpu_addr, size);
}

template <class P>
bool BufferCache<P>::SynchronizeBufferImpl(Buffer& buffer, VAddr cpu_addr, u32 size) {
    boost::container::small_vector<BufferCopy, 4> copies;
    u64 total_size_bytes = 0;
    u64 largest_copy = 0;
    VAddr buffer_start = buffer.CpuAddr();
    memory_tracker.ForEachUploadRange(cpu_addr, size, [&](u64 cpu_addr_out, u64 range_size) {
        copies.push_back(BufferCopy{
            .src_offset = total_size_bytes,
            .dst_offset = cpu_addr_out - buffer_start,
            .size = range_size,
        });
        total_size_bytes += range_size;
        largest_copy = std::max(largest_copy, range_size);
    });
    if (total_size_bytes == 0) {
        return true;
    }
    const std::span<BufferCopy> copies_span(copies.data(), copies.size());
    UploadMemory(buffer, total_size_bytes, largest_copy, copies_span);
    return false;
}

template <class P>
bool BufferCache<P>::SynchronizeBufferNoModified(Buffer& buffer, VAddr cpu_addr, u32 size) {
    boost::container::small_vector<BufferCopy, 4> copies;
    u64 total_size_bytes = 0;
    u64 largest_copy = 0;
    IntervalSet found_sets{};
    auto make_copies = [&] {
        for (auto& interval : found_sets) {
            const std::size_t sub_size = interval.upper() - interval.lower();
            const VAddr cpu_addr_ = interval.lower();
            copies.push_back(BufferCopy{
                .src_offset = total_size_bytes,
                .dst_offset = cpu_addr_ - buffer.CpuAddr(),
                .size = sub_size,
            });
            total_size_bytes += sub_size;
            largest_copy = std::max<u64>(largest_copy, sub_size);
        }
        const std::span<BufferCopy> copies_span(copies.data(), copies.size());
        UploadMemory(buffer, total_size_bytes, largest_copy, copies_span);
    };
    memory_tracker.ForEachUploadRange(cpu_addr, size, [&](u64 cpu_addr_out, u64 range_size) {
        const VAddr base_adr = cpu_addr_out;
        const VAddr end_adr = base_adr + range_size;
        const IntervalType add_interval{base_adr, end_adr};
        found_sets.add(add_interval);
    });
    if (found_sets.empty()) {
        return true;
    }
    const IntervalType search_interval{cpu_addr, cpu_addr + size};
    auto it = common_ranges.lower_bound(search_interval);
    auto it_end = common_ranges.upper_bound(search_interval);
    if (it == common_ranges.end()) {
        make_copies();
        return false;
    }
    while (it != it_end) {
        found_sets.subtract(*it);
        it++;
    }
    make_copies();
    return false;
}

template <class P>
void BufferCache<P>::UploadMemory(Buffer& buffer, u64 total_size_bytes, u64 largest_copy,
                                  std::span<BufferCopy> copies) {
    if constexpr (USE_MEMORY_MAPS) {
        MappedUploadMemory(buffer, total_size_bytes, copies);
    } else {
        ImmediateUploadMemory(buffer, largest_copy, copies);
    }
}

template <class P>
void BufferCache<P>::ImmediateUploadMemory([[maybe_unused]] Buffer& buffer,
                                           [[maybe_unused]] u64 largest_copy,
                                           [[maybe_unused]] std::span<const BufferCopy> copies) {
    if constexpr (!USE_MEMORY_MAPS) {
        std::span<u8> immediate_buffer;
        for (const BufferCopy& copy : copies) {
            std::span<const u8> upload_span;
            const VAddr cpu_addr = buffer.CpuAddr() + copy.dst_offset;
            if (IsRangeGranular(cpu_addr, copy.size)) {
                upload_span = std::span(cpu_memory.GetPointer(cpu_addr), copy.size);
            } else {
                if (immediate_buffer.empty()) {
                    immediate_buffer = ImmediateBuffer(largest_copy);
                }
                cpu_memory.ReadBlockUnsafe(cpu_addr, immediate_buffer.data(), copy.size);
                upload_span = immediate_buffer.subspan(0, copy.size);
            }
            buffer.ImmediateUpload(copy.dst_offset, upload_span);
        }
    }
}

template <class P>
void BufferCache<P>::MappedUploadMemory([[maybe_unused]] Buffer& buffer,
                                        [[maybe_unused]] u64 total_size_bytes,
                                        [[maybe_unused]] std::span<BufferCopy> copies) {
    if constexpr (USE_MEMORY_MAPS) {
        auto upload_staging = runtime.UploadStagingBuffer(total_size_bytes);
        const std::span<u8> staging_pointer = upload_staging.mapped_span;
        for (BufferCopy& copy : copies) {
            u8* const src_pointer = staging_pointer.data() + copy.src_offset;
            const VAddr cpu_addr = buffer.CpuAddr() + copy.dst_offset;
            cpu_memory.ReadBlockUnsafe(cpu_addr, src_pointer, copy.size);

            // Apply the staging offset
            copy.src_offset += upload_staging.offset;
        }
        runtime.CopyBuffer(buffer, upload_staging.buffer, copies);
    }
}

template <class P>
bool BufferCache<P>::InlineMemory(VAddr dest_address, size_t copy_size,
                                  std::span<const u8> inlined_buffer) {
    const bool is_dirty = IsRegionRegistered(dest_address, copy_size);
    if (!is_dirty) {
        return false;
    }
    VAddr aligned_start = Common::AlignDown(dest_address, YUZU_PAGESIZE);
    VAddr aligned_end = Common::AlignUp(dest_address + copy_size, YUZU_PAGESIZE);
    if (!IsRegionGpuModified(aligned_start, aligned_end - aligned_start)) {
        return false;
    }

    const IntervalType subtract_interval{dest_address, dest_address + copy_size};
    ClearDownload(subtract_interval);
    common_ranges.subtract(subtract_interval);

    BufferId buffer_id = FindBuffer(dest_address, static_cast<u32>(copy_size));
    auto& buffer = slot_buffers[buffer_id];
    SynchronizeBuffer(buffer, dest_address, static_cast<u32>(copy_size));

    if constexpr (USE_MEMORY_MAPS) {
        auto upload_staging = runtime.UploadStagingBuffer(copy_size);
        std::array copies{BufferCopy{
            .src_offset = upload_staging.offset,
            .dst_offset = buffer.Offset(dest_address),
            .size = copy_size,
        }};
        u8* const src_pointer = upload_staging.mapped_span.data();
        std::memcpy(src_pointer, inlined_buffer.data(), copy_size);
        runtime.CopyBuffer(buffer, upload_staging.buffer, copies);
    } else {
        buffer.ImmediateUpload(buffer.Offset(dest_address), inlined_buffer.first(copy_size));
    }

    return true;
}

template <class P>
void BufferCache<P>::DownloadBufferMemory(Buffer& buffer) {
    DownloadBufferMemory(buffer, buffer.CpuAddr(), buffer.SizeBytes());
}

template <class P>
void BufferCache<P>::DownloadBufferMemory(Buffer& buffer, VAddr cpu_addr, u64 size) {
    boost::container::small_vector<BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    u64 largest_copy = 0;
    memory_tracker.ForEachDownloadRangeAndClear(
        cpu_addr, size, [&](u64 cpu_addr_out, u64 range_size) {
            const VAddr buffer_addr = buffer.CpuAddr();
            const auto add_download = [&](VAddr start, VAddr end) {
                const u64 new_offset = start - buffer_addr;
                const u64 new_size = end - start;
                copies.push_back(BufferCopy{
                    .src_offset = new_offset,
                    .dst_offset = total_size_bytes,
                    .size = new_size,
                });
                // Align up to avoid cache conflicts
                constexpr u64 align = 64ULL;
                constexpr u64 mask = ~(align - 1ULL);
                total_size_bytes += (new_size + align - 1) & mask;
                largest_copy = std::max(largest_copy, new_size);
            };

            const VAddr start_address = cpu_addr_out;
            const VAddr end_address = start_address + range_size;
            ForEachInRangeSet(common_ranges, start_address, range_size, add_download);
            const IntervalType subtract_interval{start_address, end_address};
            ClearDownload(subtract_interval);
            common_ranges.subtract(subtract_interval);
        });
    if (total_size_bytes == 0) {
        return;
    }
    MICROPROFILE_SCOPE(GPU_DownloadMemory);

    if constexpr (USE_MEMORY_MAPS) {
        auto download_staging = runtime.DownloadStagingBuffer(total_size_bytes);
        const u8* const mapped_memory = download_staging.mapped_span.data();
        const std::span<BufferCopy> copies_span(copies.data(), copies.data() + copies.size());
        for (BufferCopy& copy : copies) {
            // Modify copies to have the staging offset in mind
            copy.dst_offset += download_staging.offset;
        }
        runtime.CopyBuffer(download_staging.buffer, buffer, copies_span);
        runtime.Finish();
        for (const BufferCopy& copy : copies) {
            const VAddr copy_cpu_addr = buffer.CpuAddr() + copy.src_offset;
            // Undo the modified offset
            const u64 dst_offset = copy.dst_offset - download_staging.offset;
            const u8* copy_mapped_memory = mapped_memory + dst_offset;
            cpu_memory.WriteBlockUnsafe(copy_cpu_addr, copy_mapped_memory, copy.size);
        }
    } else {
        const std::span<u8> immediate_buffer = ImmediateBuffer(largest_copy);
        for (const BufferCopy& copy : copies) {
            buffer.ImmediateDownload(copy.src_offset, immediate_buffer.subspan(0, copy.size));
            const VAddr copy_cpu_addr = buffer.CpuAddr() + copy.src_offset;
            cpu_memory.WriteBlockUnsafe(copy_cpu_addr, immediate_buffer.data(), copy.size);
        }
    }
}

template <class P>
void BufferCache<P>::DeleteBuffer(BufferId buffer_id, bool do_not_mark) {
    const auto scalar_replace = [buffer_id](Binding& binding) {
        if (binding.buffer_id == buffer_id) {
            binding.buffer_id = BufferId{};
        }
    };
    const auto replace = [scalar_replace](std::span<Binding> bindings) {
        std::ranges::for_each(bindings, scalar_replace);
    };
    scalar_replace(index_buffer);
    replace(vertex_buffers);
    std::ranges::for_each(uniform_buffers, replace);
    std::ranges::for_each(storage_buffers, replace);
    replace(transform_feedback_buffers);
    replace(compute_uniform_buffers);
    replace(compute_storage_buffers);

    // Mark the whole buffer as CPU written to stop tracking CPU writes
    if (!do_not_mark) {
        Buffer& buffer = slot_buffers[buffer_id];
        memory_tracker.MarkRegionAsCpuModified(buffer.CpuAddr(), buffer.SizeBytes());
    }

    Unregister(buffer_id);
    delayed_destruction_ring.Push(std::move(slot_buffers[buffer_id]));
    slot_buffers.erase(buffer_id);

    NotifyBufferDeletion();
}

template <class P>
void BufferCache<P>::NotifyBufferDeletion() {
    if constexpr (HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS) {
        dirty_uniform_buffers.fill(~u32{0});
        uniform_buffer_binding_sizes.fill({});
    }
    auto& flags = maxwell3d->dirty.flags;
    flags[Dirty::IndexBuffer] = true;
    flags[Dirty::VertexBuffers] = true;
    for (u32 index = 0; index < NUM_VERTEX_BUFFERS; ++index) {
        flags[Dirty::VertexBuffer0 + index] = true;
    }
    has_deleted_buffers = true;
}

template <class P>
typename BufferCache<P>::Binding BufferCache<P>::StorageBufferBinding(GPUVAddr ssbo_addr,
                                                                      u32 cbuf_index,
                                                                      bool is_written) const {
    const GPUVAddr gpu_addr = gpu_memory->Read<u64>(ssbo_addr);
    const auto size = [&]() {
        const bool is_nvn_cbuf = cbuf_index == 0;
        // The NVN driver buffer (index 0) is known to pack the SSBO address followed by its size.
        if (is_nvn_cbuf) {
            return gpu_memory->Read<u32>(ssbo_addr + 8);
        }
        // Other titles (notably Doom Eternal) may use STG/LDG on buffer addresses in custom defined
        // cbufs, which do not store the sizes adjacent to the addresses, so use the fully
        // mapped buffer size for now.
        const u32 memory_layout_size = static_cast<u32>(gpu_memory->GetMemoryLayoutSize(gpu_addr));
        LOG_INFO(HW_GPU, "Binding storage buffer for cbuf index {}, MemoryLayoutSize 0x{:X}",
                 cbuf_index, memory_layout_size);
        return memory_layout_size;
    }();
    const std::optional<VAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
    if (!cpu_addr || size == 0) {
        LOG_WARNING(HW_GPU, "Failed to find storage buffer for cbuf index {}", cbuf_index);
        return NULL_BINDING;
    }
    const VAddr cpu_end = Common::AlignUp(*cpu_addr + size, YUZU_PAGESIZE);
    const Binding binding{
        .cpu_addr = *cpu_addr,
        .size = is_written ? size : static_cast<u32>(cpu_end - *cpu_addr),
        .buffer_id = BufferId{},
    };
    return binding;
}

template <class P>
typename BufferCache<P>::TextureBufferBinding BufferCache<P>::GetTextureBufferBinding(
    GPUVAddr gpu_addr, u32 size, PixelFormat format) {
    const std::optional<VAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
    TextureBufferBinding binding;
    if (!cpu_addr || size == 0) {
        binding.cpu_addr = 0;
        binding.size = 0;
        binding.buffer_id = NULL_BUFFER_ID;
        binding.format = PixelFormat::Invalid;
    } else {
        binding.cpu_addr = *cpu_addr;
        binding.size = size;
        binding.buffer_id = BufferId{};
        binding.format = format;
    }
    return binding;
}

template <class P>
std::span<const u8> BufferCache<P>::ImmediateBufferWithData(VAddr cpu_addr, size_t size) {
    u8* const base_pointer = cpu_memory.GetPointer(cpu_addr);
    if (IsRangeGranular(cpu_addr, size) ||
        base_pointer + size == cpu_memory.GetPointer(cpu_addr + size)) {
        return std::span(base_pointer, size);
    } else {
        const std::span<u8> span = ImmediateBuffer(size);
        cpu_memory.ReadBlockUnsafe(cpu_addr, span.data(), size);
        return span;
    }
}

template <class P>
std::span<u8> BufferCache<P>::ImmediateBuffer(size_t wanted_capacity) {
    immediate_buffer_alloc.resize_destructive(wanted_capacity);
    return std::span<u8>(immediate_buffer_alloc.data(), wanted_capacity);
}

template <class P>
bool BufferCache<P>::HasFastUniformBufferBound(size_t stage, u32 binding_index) const noexcept {
    if constexpr (IS_OPENGL) {
        return ((fast_bound_uniform_buffers[stage] >> binding_index) & 1) != 0;
    } else {
        // Only OpenGL has fast uniform buffers
        return false;
    }
}

template <class P>
std::pair<typename BufferCache<P>::Buffer*, u32> BufferCache<P>::GetDrawIndirectCount() {
    auto& buffer = slot_buffers[count_buffer_binding.buffer_id];
    return std::make_pair(&buffer, buffer.Offset(count_buffer_binding.cpu_addr));
}

template <class P>
std::pair<typename BufferCache<P>::Buffer*, u32> BufferCache<P>::GetDrawIndirectBuffer() {
    auto& buffer = slot_buffers[indirect_buffer_binding.buffer_id];
    return std::make_pair(&buffer, buffer.Offset(indirect_buffer_binding.cpu_addr));
}

} // namespace VideoCommon
