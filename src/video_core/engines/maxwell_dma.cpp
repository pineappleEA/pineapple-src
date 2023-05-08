// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/algorithm.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "core/core.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_base.h"
#include "video_core/textures/decoders.h"

MICROPROFILE_DECLARE(GPU_DMAEngine);
MICROPROFILE_DECLARE(GPU_DMAEngineBL);
MICROPROFILE_DECLARE(GPU_DMAEngineLB);
MICROPROFILE_DECLARE(GPU_DMAEngineBB);
MICROPROFILE_DEFINE(GPU_DMAEngine, "GPU", "DMA Engine", MP_RGB(224, 224, 128));
MICROPROFILE_DEFINE(GPU_DMAEngineBL, "GPU", "DMA Engine Block - Linear", MP_RGB(224, 224, 128));
MICROPROFILE_DEFINE(GPU_DMAEngineLB, "GPU", "DMA Engine Linear - Block", MP_RGB(224, 224, 128));
MICROPROFILE_DEFINE(GPU_DMAEngineBB, "GPU", "DMA Engine Block - Block", MP_RGB(224, 224, 128));

namespace Tegra::Engines {

using namespace Texture;

MaxwellDMA::MaxwellDMA(Core::System& system_, MemoryManager& memory_manager_)
    : system{system_}, memory_manager{memory_manager_} {
    execution_mask.reset();
    execution_mask[offsetof(Regs, launch_dma) / sizeof(u32)] = true;
}

MaxwellDMA::~MaxwellDMA() = default;

void MaxwellDMA::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
}

void MaxwellDMA::ConsumeSinkImpl() {
    for (auto [method, value] : method_sink) {
        regs.reg_array[method] = value;
    }
    method_sink.clear();
}

void MaxwellDMA::CallMethod(u32 method, u32 method_argument, bool is_last_call) {
    ASSERT_MSG(method < NUM_REGS, "Invalid MaxwellDMA register");

    regs.reg_array[method] = method_argument;

    if (method == offsetof(Regs, launch_dma) / sizeof(u32)) {
        Launch();
    }
}

void MaxwellDMA::CallMultiMethod(u32 method, const u32* base_start, u32 amount,
                                 u32 methods_pending) {
    for (u32 i = 0; i < amount; ++i) {
        CallMethod(method, base_start[i], methods_pending - i <= 1);
    }
}

void MaxwellDMA::Launch() {
    MICROPROFILE_SCOPE(GPU_DMAEngine);
    LOG_TRACE(Render_OpenGL, "DMA copy 0x{:x} -> 0x{:x}", static_cast<GPUVAddr>(regs.offset_in),
              static_cast<GPUVAddr>(regs.offset_out));

    // TODO(Subv): Perform more research and implement all features of this engine.
    const LaunchDMA& launch = regs.launch_dma;
    ASSERT(launch.interrupt_type == LaunchDMA::InterruptType::NONE);
    ASSERT(launch.data_transfer_type == LaunchDMA::DataTransferType::NON_PIPELINED);

    if (launch.multi_line_enable) {
        const bool is_src_pitch = launch.src_memory_layout == LaunchDMA::MemoryLayout::PITCH;
        const bool is_dst_pitch = launch.dst_memory_layout == LaunchDMA::MemoryLayout::PITCH;
        memory_manager.FlushCaching();
        if (!is_src_pitch && !is_dst_pitch) {
            // If both the source and the destination are in block layout, assert.
            MICROPROFILE_SCOPE(GPU_DMAEngineBB);
            CopyBlockLinearToBlockLinear();
            ReleaseSemaphore();
            return;
        }

        if (is_src_pitch && is_dst_pitch) {
            for (u32 line = 0; line < regs.line_count; ++line) {
                const GPUVAddr source_line =
                    regs.offset_in + static_cast<size_t>(line) * regs.pitch_in;
                const GPUVAddr dest_line =
                    regs.offset_out + static_cast<size_t>(line) * regs.pitch_out;
                memory_manager.CopyBlock(dest_line, source_line, regs.line_length_in);
            }
        } else {
            if (!is_src_pitch && is_dst_pitch) {
                MICROPROFILE_SCOPE(GPU_DMAEngineBL);
                CopyBlockLinearToPitch();
            } else {
                MICROPROFILE_SCOPE(GPU_DMAEngineLB);
                CopyPitchToBlockLinear();
            }
        }
    } else {
        // TODO: allow multisized components.
        auto& accelerate = rasterizer->AccessAccelerateDMA();
        const bool is_const_a_dst = regs.remap_const.dst_x == RemapConst::Swizzle::CONST_A;
        if (regs.launch_dma.remap_enable != 0 && is_const_a_dst) {
            ASSERT(regs.remap_const.component_size_minus_one == 3);
            accelerate.BufferClear(regs.offset_out, regs.line_length_in, regs.remap_consta_value);
            std::vector<u32> tmp_buffer(regs.line_length_in, regs.remap_consta_value);
            memory_manager.WriteBlockUnsafe(regs.offset_out,
                                            reinterpret_cast<u8*>(tmp_buffer.data()),
                                            regs.line_length_in * sizeof(u32));
        } else {
            memory_manager.FlushCaching();
            const auto convert_linear_2_blocklinear_addr = [](u64 address) {
                return (address & ~0x1f0ULL) | ((address & 0x40) >> 2) | ((address & 0x10) << 1) |
                       ((address & 0x180) >> 1) | ((address & 0x20) << 3);
            };
            const auto src_kind = memory_manager.GetPageKind(regs.offset_in);
            const auto dst_kind = memory_manager.GetPageKind(regs.offset_out);
            const bool is_src_pitch = IsPitchKind(src_kind);
            const bool is_dst_pitch = IsPitchKind(dst_kind);
            if (!is_src_pitch && is_dst_pitch) {
                UNIMPLEMENTED_IF(regs.line_length_in % 16 != 0);
                UNIMPLEMENTED_IF(regs.offset_in % 16 != 0);
                UNIMPLEMENTED_IF(regs.offset_out % 16 != 0);
                std::vector<u8> tmp_buffer(16);
                for (u32 offset = 0; offset < regs.line_length_in; offset += 16) {
                    memory_manager.ReadBlockUnsafe(
                        convert_linear_2_blocklinear_addr(regs.offset_in + offset),
                        tmp_buffer.data(), tmp_buffer.size());
                    memory_manager.WriteBlockCached(regs.offset_out + offset, tmp_buffer.data(),
                                                    tmp_buffer.size());
                }
            } else if (is_src_pitch && !is_dst_pitch) {
                UNIMPLEMENTED_IF(regs.line_length_in % 16 != 0);
                UNIMPLEMENTED_IF(regs.offset_in % 16 != 0);
                UNIMPLEMENTED_IF(regs.offset_out % 16 != 0);
                std::vector<u8> tmp_buffer(16);
                for (u32 offset = 0; offset < regs.line_length_in; offset += 16) {
                    memory_manager.ReadBlockUnsafe(regs.offset_in + offset, tmp_buffer.data(),
                                                   tmp_buffer.size());
                    memory_manager.WriteBlockCached(
                        convert_linear_2_blocklinear_addr(regs.offset_out + offset),
                        tmp_buffer.data(), tmp_buffer.size());
                }
            } else {
                if (!accelerate.BufferCopy(regs.offset_in, regs.offset_out, regs.line_length_in)) {
                    std::vector<u8> tmp_buffer(regs.line_length_in);
                    memory_manager.ReadBlockUnsafe(regs.offset_in, tmp_buffer.data(),
                                                   regs.line_length_in);
                    memory_manager.WriteBlockCached(regs.offset_out, tmp_buffer.data(),
                                                    regs.line_length_in);
                }
            }
        }
    }

    ReleaseSemaphore();
}

void MaxwellDMA::CopyBlockLinearToPitch() {
    UNIMPLEMENTED_IF(regs.launch_dma.remap_enable != 0);

    u32 bytes_per_pixel = 1;
    DMA::ImageOperand src_operand;
    src_operand.bytes_per_pixel = bytes_per_pixel;
    src_operand.params = regs.src_params;
    src_operand.address = regs.offset_in;

    DMA::BufferOperand dst_operand;
    dst_operand.pitch = regs.pitch_out;
    dst_operand.width = regs.line_length_in;
    dst_operand.height = regs.line_count;
    dst_operand.address = regs.offset_out;
    DMA::ImageCopy copy_info{};
    copy_info.length_x = regs.line_length_in;
    copy_info.length_y = regs.line_count;
    auto& accelerate = rasterizer->AccessAccelerateDMA();
    if (accelerate.ImageToBuffer(copy_info, src_operand, dst_operand)) {
        return;
    }

    UNIMPLEMENTED_IF(regs.src_params.block_size.width != 0);
    UNIMPLEMENTED_IF(regs.src_params.block_size.depth != 0);
    UNIMPLEMENTED_IF(regs.src_params.block_size.depth == 0 && regs.src_params.depth != 1);

    // Deswizzle the input and copy it over.
    const DMA::Parameters& src_params = regs.src_params;

    const bool is_remapping = regs.launch_dma.remap_enable != 0;

    const u32 num_remap_components = regs.remap_const.num_dst_components_minus_one + 1;
    const u32 remap_components_size = regs.remap_const.component_size_minus_one + 1;

    const u32 base_bpp = !is_remapping ? 1U : num_remap_components * remap_components_size;

    u32 width = src_params.width;
    u32 x_elements = regs.line_length_in;
    u32 x_offset = src_params.origin.x;
    u32 bpp_shift = 0U;
    if (!is_remapping) {
        bpp_shift = Common::FoldRight(
            4U, [](u32 x, u32 y) { return std::min(x, static_cast<u32>(std::countr_zero(y))); },
            width, x_elements, x_offset, static_cast<u32>(regs.offset_in));
        width >>= bpp_shift;
        x_elements >>= bpp_shift;
        x_offset >>= bpp_shift;
    }

    bytes_per_pixel = base_bpp << bpp_shift;
    const u32 height = src_params.height;
    const u32 depth = src_params.depth;
    const u32 block_height = src_params.block_size.height;
    const u32 block_depth = src_params.block_size.depth;
    const size_t src_size =
        CalculateSize(true, bytes_per_pixel, width, height, depth, block_height, block_depth);

    const size_t dst_size = static_cast<size_t>(regs.pitch_out) * regs.line_count;
    read_buffer.resize_destructive(src_size);
    write_buffer.resize_destructive(dst_size);

    memory_manager.ReadBlock(src_operand.address, read_buffer.data(), src_size);
    memory_manager.ReadBlock(dst_operand.address, write_buffer.data(), dst_size);

    UnswizzleSubrect(write_buffer, read_buffer, bytes_per_pixel, width, height, depth, x_offset,
                     src_params.origin.y, x_elements, regs.line_count, block_height, block_depth,
                     regs.pitch_out);

    memory_manager.WriteBlockCached(regs.offset_out, write_buffer.data(), dst_size);
}

void MaxwellDMA::CopyPitchToBlockLinear() {
    UNIMPLEMENTED_IF_MSG(regs.dst_params.block_size.width != 0, "Block width is not one");
    UNIMPLEMENTED_IF(regs.dst_params.layer != 0);

    const bool is_remapping = regs.launch_dma.remap_enable != 0;
    const u32 num_remap_components = regs.remap_const.num_dst_components_minus_one + 1;
    const u32 remap_components_size = regs.remap_const.component_size_minus_one + 1;

    u32 bytes_per_pixel = 1;
    DMA::ImageOperand dst_operand;
    dst_operand.bytes_per_pixel = bytes_per_pixel;
    dst_operand.params = regs.dst_params;
    dst_operand.address = regs.offset_out;
    DMA::BufferOperand src_operand;
    src_operand.pitch = regs.pitch_in;
    src_operand.width = regs.line_length_in;
    src_operand.height = regs.line_count;
    src_operand.address = regs.offset_in;
    DMA::ImageCopy copy_info{};
    copy_info.length_x = regs.line_length_in;
    copy_info.length_y = regs.line_count;
    auto& accelerate = rasterizer->AccessAccelerateDMA();
    if (accelerate.BufferToImage(copy_info, src_operand, dst_operand)) {
        return;
    }

    const auto& dst_params = regs.dst_params;

    const u32 base_bpp = !is_remapping ? 1U : num_remap_components * remap_components_size;

    u32 width = dst_params.width;
    u32 x_elements = regs.line_length_in;
    u32 x_offset = dst_params.origin.x;
    u32 bpp_shift = 0U;
    if (!is_remapping) {
        bpp_shift = Common::FoldRight(
            4U, [](u32 x, u32 y) { return std::min(x, static_cast<u32>(std::countr_zero(y))); },
            width, x_elements, x_offset, static_cast<u32>(regs.offset_out));
        width >>= bpp_shift;
        x_elements >>= bpp_shift;
        x_offset >>= bpp_shift;
    }

    bytes_per_pixel = base_bpp << bpp_shift;
    const u32 height = dst_params.height;
    const u32 depth = dst_params.depth;
    const u32 block_height = dst_params.block_size.height;
    const u32 block_depth = dst_params.block_size.depth;
    const size_t dst_size =
        CalculateSize(true, bytes_per_pixel, width, height, depth, block_height, block_depth);
    const size_t src_size = static_cast<size_t>(regs.pitch_in) * regs.line_count;

    read_buffer.resize_destructive(src_size);
    write_buffer.resize_destructive(dst_size);

    memory_manager.ReadBlock(regs.offset_in, read_buffer.data(), src_size);
    memory_manager.ReadBlockUnsafe(regs.offset_out, write_buffer.data(), dst_size);

    // If the input is linear and the output is tiled, swizzle the input and copy it over.
    SwizzleSubrect(write_buffer, read_buffer, bytes_per_pixel, width, height, depth, x_offset,
                   dst_params.origin.y, x_elements, regs.line_count, block_height, block_depth,
                   regs.pitch_in);

    memory_manager.WriteBlockCached(regs.offset_out, write_buffer.data(), dst_size);
}

void MaxwellDMA::CopyBlockLinearToBlockLinear() {
    UNIMPLEMENTED_IF(regs.src_params.block_size.width != 0);

    const bool is_remapping = regs.launch_dma.remap_enable != 0;

    // Deswizzle the input and copy it over.
    const DMA::Parameters& src = regs.src_params;
    const DMA::Parameters& dst = regs.dst_params;

    const u32 num_remap_components = regs.remap_const.num_dst_components_minus_one + 1;
    const u32 remap_components_size = regs.remap_const.component_size_minus_one + 1;

    const u32 base_bpp = !is_remapping ? 1U : num_remap_components * remap_components_size;

    u32 src_width = src.width;
    u32 dst_width = dst.width;
    u32 x_elements = regs.line_length_in;
    u32 src_x_offset = src.origin.x;
    u32 dst_x_offset = dst.origin.x;
    u32 bpp_shift = 0U;
    if (!is_remapping) {
        bpp_shift = Common::FoldRight(
            4U, [](u32 x, u32 y) { return std::min(x, static_cast<u32>(std::countr_zero(y))); },
            src_width, dst_width, x_elements, src_x_offset, dst_x_offset,
            static_cast<u32>(regs.offset_in), static_cast<u32>(regs.offset_out));
        src_width >>= bpp_shift;
        dst_width >>= bpp_shift;
        x_elements >>= bpp_shift;
        src_x_offset >>= bpp_shift;
        dst_x_offset >>= bpp_shift;
    }

    const u32 bytes_per_pixel = base_bpp << bpp_shift;
    const size_t src_size = CalculateSize(true, bytes_per_pixel, src_width, src.height, src.depth,
                                          src.block_size.height, src.block_size.depth);
    const size_t dst_size = CalculateSize(true, bytes_per_pixel, dst_width, dst.height, dst.depth,
                                          dst.block_size.height, dst.block_size.depth);

    const u32 pitch = x_elements * bytes_per_pixel;
    const size_t mid_buffer_size = pitch * regs.line_count;

    read_buffer.resize_destructive(src_size);
    write_buffer.resize_destructive(dst_size);

    intermediate_buffer.resize_destructive(mid_buffer_size);

    memory_manager.ReadBlock(regs.offset_in, read_buffer.data(), src_size);
    memory_manager.ReadBlock(regs.offset_out, write_buffer.data(), dst_size);

    UnswizzleSubrect(intermediate_buffer, read_buffer, bytes_per_pixel, src_width, src.height,
                     src.depth, src_x_offset, src.origin.y, x_elements, regs.line_count,
                     src.block_size.height, src.block_size.depth, pitch);

    SwizzleSubrect(write_buffer, intermediate_buffer, bytes_per_pixel, dst_width, dst.height,
                   dst.depth, dst_x_offset, dst.origin.y, x_elements, regs.line_count,
                   dst.block_size.height, dst.block_size.depth, pitch);

    memory_manager.WriteBlockCached(regs.offset_out, write_buffer.data(), dst_size);
}

void MaxwellDMA::ReleaseSemaphore() {
    const auto type = regs.launch_dma.semaphore_type;
    const GPUVAddr address = regs.semaphore.address;
    const u32 payload = regs.semaphore.payload;
    switch (type) {
    case LaunchDMA::SemaphoreType::NONE:
        break;
    case LaunchDMA::SemaphoreType::RELEASE_ONE_WORD_SEMAPHORE: {
        std::function<void()> operation(
            [this, address, payload] { memory_manager.Write<u32>(address, payload); });
        rasterizer->SignalFence(std::move(operation));
        break;
    }
    case LaunchDMA::SemaphoreType::RELEASE_FOUR_WORD_SEMAPHORE: {
        std::function<void()> operation([this, address, payload] {
            memory_manager.Write<u64>(address + sizeof(u64), system.GPU().GetTicks());
            memory_manager.Write<u64>(address, payload);
        });
        rasterizer->SignalFence(std::move(operation));
        break;
    }
    default:
        ASSERT_MSG(false, "Unknown semaphore type: {}", static_cast<u32>(type.Value()));
        break;
    }
}

} // namespace Tegra::Engines
