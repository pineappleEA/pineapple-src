// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bitset>
#include <memory>
#include <string_view>
#include <utility>

#include <glad/glad.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/math_util.h"
#include "common/microprofile.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_opengl/gl_device.h"
#include "video_core/renderer_opengl/gl_query_cache.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_opengl/gl_shader_cache.h"
#include "video_core/renderer_opengl/gl_texture_cache.h"
#include "video_core/renderer_opengl/maxwell_to_gl.h"
#include "video_core/renderer_opengl/renderer_opengl.h"
#include "video_core/shader_cache.h"
#include "video_core/texture_cache/texture_cache_base.h"

namespace OpenGL {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;
using GLvec4 = std::array<GLfloat, 4>;

using VideoCore::Surface::PixelFormat;
using VideoCore::Surface::SurfaceTarget;
using VideoCore::Surface::SurfaceType;

MICROPROFILE_DEFINE(OpenGL_Drawing, "OpenGL", "Drawing", MP_RGB(128, 128, 192));
MICROPROFILE_DEFINE(OpenGL_Clears, "OpenGL", "Clears", MP_RGB(128, 128, 192));
MICROPROFILE_DEFINE(OpenGL_Blits, "OpenGL", "Blits", MP_RGB(128, 128, 192));
MICROPROFILE_DEFINE(OpenGL_CacheManagement, "OpenGL", "Cache Management", MP_RGB(100, 255, 100));

namespace {
constexpr size_t NUM_SUPPORTED_VERTEX_ATTRIBUTES = 16;

void oglEnable(GLenum cap, bool state) {
    (state ? glEnable : glDisable)(cap);
}
} // Anonymous namespace

RasterizerOpenGL::RasterizerOpenGL(Core::Frontend::EmuWindow& emu_window_, Tegra::GPU& gpu_,
                                   Core::Memory::Memory& cpu_memory_, const Device& device_,
                                   ScreenInfo& screen_info_, ProgramManager& program_manager_,
                                   StateTracker& state_tracker_)
    : RasterizerAccelerated(cpu_memory_), gpu(gpu_), device(device_), screen_info(screen_info_),
      program_manager(program_manager_), state_tracker(state_tracker_),
      texture_cache_runtime(device, program_manager, state_tracker),
      texture_cache(texture_cache_runtime, *this), buffer_cache_runtime(device),
      buffer_cache(*this, cpu_memory_, buffer_cache_runtime),
      shader_cache(*this, emu_window_, device, texture_cache, buffer_cache, program_manager,
                   state_tracker, gpu.ShaderNotify()),
      query_cache(*this, cpu_memory_), accelerate_dma(buffer_cache, texture_cache),
      fence_manager(*this, gpu, texture_cache, buffer_cache, query_cache),
      blit_image(program_manager_) {}

RasterizerOpenGL::~RasterizerOpenGL() = default;

void RasterizerOpenGL::SyncVertexFormats() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::VertexFormats]) {
        return;
    }
    flags[Dirty::VertexFormats] = false;

    // Use the vertex array as-is, assumes that the data is formatted correctly for OpenGL. Enables
    // the first 16 vertex attributes always, as we don't know which ones are actually used until
    // shader time. Note, Tegra technically supports 32, but we're capping this to 16 for now to
    // avoid OpenGL errors.
    // TODO(Subv): Analyze the shader to identify which attributes are actually used and don't
    // assume every shader uses them all.
    for (std::size_t index = 0; index < NUM_SUPPORTED_VERTEX_ATTRIBUTES; ++index) {
        if (!flags[Dirty::VertexFormat0 + index]) {
            continue;
        }
        flags[Dirty::VertexFormat0 + index] = false;

        const auto& attrib = maxwell3d->regs.vertex_attrib_format[index];
        const auto gl_index = static_cast<GLuint>(index);

        // Disable constant attributes.
        if (attrib.constant) {
            glDisableVertexAttribArray(gl_index);
            continue;
        }
        glEnableVertexAttribArray(gl_index);

        if (attrib.type == Maxwell::VertexAttribute::Type::SInt ||
            attrib.type == Maxwell::VertexAttribute::Type::UInt) {
            glVertexAttribIFormat(gl_index, attrib.ComponentCount(),
                                  MaxwellToGL::VertexFormat(attrib), attrib.offset);
        } else {
            glVertexAttribFormat(gl_index, attrib.ComponentCount(),
                                 MaxwellToGL::VertexFormat(attrib),
                                 attrib.IsNormalized() ? GL_TRUE : GL_FALSE, attrib.offset);
        }
        glVertexAttribBinding(gl_index, attrib.buffer);
    }
}

void RasterizerOpenGL::SyncVertexInstances() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::VertexInstances]) {
        return;
    }
    flags[Dirty::VertexInstances] = false;

    const auto& regs = maxwell3d->regs;
    for (std::size_t index = 0; index < NUM_SUPPORTED_VERTEX_ATTRIBUTES; ++index) {
        if (!flags[Dirty::VertexInstance0 + index]) {
            continue;
        }
        flags[Dirty::VertexInstance0 + index] = false;

        const auto gl_index = static_cast<GLuint>(index);
        const bool instancing_enabled = regs.vertex_stream_instances.IsInstancingEnabled(gl_index);
        const GLuint divisor = instancing_enabled ? regs.vertex_streams[index].frequency : 0;
        glVertexBindingDivisor(gl_index, divisor);
    }
}

void RasterizerOpenGL::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                         const VideoCore::DiskResourceLoadCallback& callback) {
    shader_cache.LoadDiskResources(title_id, stop_loading, callback);
}

void RasterizerOpenGL::Clear(u32 layer_count) {
    MICROPROFILE_SCOPE(OpenGL_Clears);

    gpu_memory->FlushCaching();
    const auto& regs = maxwell3d->regs;
    bool use_color{};
    bool use_depth{};
    bool use_stencil{};

    if (regs.clear_surface.R || regs.clear_surface.G || regs.clear_surface.B ||
        regs.clear_surface.A) {
        use_color = true;

        const GLuint index = regs.clear_surface.RT;
        state_tracker.NotifyColorMask(index);
        glColorMaski(index, regs.clear_surface.R != 0, regs.clear_surface.G != 0,
                     regs.clear_surface.B != 0, regs.clear_surface.A != 0);

        // TODO(Rodrigo): Determine if clamping is used on clears
        SyncFragmentColorClampState();
        SyncFramebufferSRGB();
    }
    if (regs.clear_surface.Z) {
        ASSERT_MSG(regs.zeta_enable != 0, "Tried to clear Z but buffer is not enabled!");
        use_depth = true;

        state_tracker.NotifyDepthMask();
        glDepthMask(GL_TRUE);
    }
    if (regs.clear_surface.S) {
        ASSERT_MSG(regs.zeta_enable, "Tried to clear stencil but buffer is not enabled!");
        use_stencil = true;
    }

    if (!use_color && !use_depth && !use_stencil) {
        // No color surface nor depth/stencil surface are enabled
        return;
    }

    SyncRasterizeEnable();
    SyncStencilTestState();

    std::scoped_lock lock{texture_cache.mutex};
    texture_cache.UpdateRenderTargets(true);
    state_tracker.BindFramebuffer(texture_cache.GetFramebuffer()->Handle());
    SyncViewport();
    if (regs.clear_control.use_scissor) {
        SyncScissorTest();
    } else {
        state_tracker.NotifyScissor0();
        glDisablei(GL_SCISSOR_TEST, 0);
    }
    UNIMPLEMENTED_IF(regs.clear_control.use_viewport_clip0);

    if (use_color) {
        glClearBufferfv(GL_COLOR, regs.clear_surface.RT, regs.clear_color.data());
    }
    if (use_depth && use_stencil) {
        glClearBufferfi(GL_DEPTH_STENCIL, 0, regs.clear_depth, regs.clear_stencil);
    } else if (use_depth) {
        glClearBufferfv(GL_DEPTH, 0, &regs.clear_depth);
    } else if (use_stencil) {
        glClearBufferiv(GL_STENCIL, 0, &regs.clear_stencil);
    }
    ++num_queued_commands;
}

template <typename Func>
void RasterizerOpenGL::PrepareDraw(bool is_indexed, Func&& draw_func) {
    MICROPROFILE_SCOPE(OpenGL_Drawing);

    SCOPE_EXIT({ gpu.TickWork(); });
    gpu_memory->FlushCaching();
    query_cache.UpdateCounters();

    GraphicsPipeline* const pipeline{shader_cache.CurrentGraphicsPipeline()};
    if (!pipeline) {
        return;
    }

    gpu.TickWork();

    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    pipeline->SetEngine(maxwell3d, gpu_memory);
    pipeline->Configure(is_indexed);

    SyncState();

    const auto& draw_state = maxwell3d->draw_manager->GetDrawState();

    const GLenum primitive_mode = MaxwellToGL::PrimitiveTopology(draw_state.topology);
    BeginTransformFeedback(pipeline, primitive_mode);

    draw_func(primitive_mode);

    EndTransformFeedback();

    ++num_queued_commands;
    has_written_global_memory |= pipeline->WritesGlobalMemory();
}

void RasterizerOpenGL::Draw(bool is_indexed, u32 instance_count) {
    PrepareDraw(is_indexed, [this, is_indexed, instance_count](GLenum primitive_mode) {
        const auto& draw_state = maxwell3d->draw_manager->GetDrawState();
        const GLuint base_instance = static_cast<GLuint>(draw_state.base_instance);
        const GLsizei num_instances = static_cast<GLsizei>(instance_count);
        if (is_indexed) {
            const GLint base_vertex = static_cast<GLint>(draw_state.base_index);
            const GLsizei num_vertices = static_cast<GLsizei>(draw_state.index_buffer.count);
            const GLvoid* const offset = buffer_cache_runtime.IndexOffset();
            const GLenum format = MaxwellToGL::IndexFormat(draw_state.index_buffer.format);
            if (num_instances == 1 && base_instance == 0 && base_vertex == 0) {
                glDrawElements(primitive_mode, num_vertices, format, offset);
            } else if (num_instances == 1 && base_instance == 0) {
                glDrawElementsBaseVertex(primitive_mode, num_vertices, format, offset, base_vertex);
            } else if (base_vertex == 0 && base_instance == 0) {
                glDrawElementsInstanced(primitive_mode, num_vertices, format, offset,
                                        num_instances);
            } else if (base_vertex == 0) {
                glDrawElementsInstancedBaseInstance(primitive_mode, num_vertices, format, offset,
                                                    num_instances, base_instance);
            } else if (base_instance == 0) {
                glDrawElementsInstancedBaseVertex(primitive_mode, num_vertices, format, offset,
                                                  num_instances, base_vertex);
            } else {
                glDrawElementsInstancedBaseVertexBaseInstance(primitive_mode, num_vertices, format,
                                                              offset, num_instances, base_vertex,
                                                              base_instance);
            }
        } else {
            const GLint base_vertex = static_cast<GLint>(draw_state.vertex_buffer.first);
            const GLsizei num_vertices = static_cast<GLsizei>(draw_state.vertex_buffer.count);
            if (num_instances == 1 && base_instance == 0) {
                glDrawArrays(primitive_mode, base_vertex, num_vertices);
            } else if (base_instance == 0) {
                glDrawArraysInstanced(primitive_mode, base_vertex, num_vertices, num_instances);
            } else {
                glDrawArraysInstancedBaseInstance(primitive_mode, base_vertex, num_vertices,
                                                  num_instances, base_instance);
            }
        }
    });
}

void RasterizerOpenGL::DrawIndirect() {
    const auto& params = maxwell3d->draw_manager->GetIndirectParams();
    buffer_cache.SetDrawIndirect(&params);
    PrepareDraw(params.is_indexed, [this, &params](GLenum primitive_mode) {
        const auto [buffer, offset] = buffer_cache.GetDrawIndirectBuffer();
        const GLvoid* const gl_offset =
            reinterpret_cast<const GLvoid*>(static_cast<uintptr_t>(offset));
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buffer->Handle());
        if (params.include_count) {
            const auto [draw_buffer, offset_base] = buffer_cache.GetDrawIndirectCount();
            glBindBuffer(GL_PARAMETER_BUFFER, draw_buffer->Handle());

            if (params.is_indexed) {
                const GLenum format = MaxwellToGL::IndexFormat(maxwell3d->regs.index_buffer.format);
                glMultiDrawElementsIndirectCount(primitive_mode, format, gl_offset,
                                                 static_cast<GLintptr>(offset_base),
                                                 static_cast<GLsizei>(params.max_draw_counts),
                                                 static_cast<GLsizei>(params.stride));
            } else {
                glMultiDrawArraysIndirectCount(primitive_mode, gl_offset,
                                               static_cast<GLintptr>(offset_base),
                                               static_cast<GLsizei>(params.max_draw_counts),
                                               static_cast<GLsizei>(params.stride));
            }
            return;
        }
        if (params.is_indexed) {
            const GLenum format = MaxwellToGL::IndexFormat(maxwell3d->regs.index_buffer.format);
            glMultiDrawElementsIndirect(primitive_mode, format, gl_offset,
                                        static_cast<GLsizei>(params.max_draw_counts),
                                        static_cast<GLsizei>(params.stride));
        } else {
            glMultiDrawArraysIndirect(primitive_mode, gl_offset,
                                      static_cast<GLsizei>(params.max_draw_counts),
                                      static_cast<GLsizei>(params.stride));
        }
    });
    buffer_cache.SetDrawIndirect(nullptr);
}

void RasterizerOpenGL::DrawTexture() {
    MICROPROFILE_SCOPE(OpenGL_Drawing);

    SCOPE_EXIT({ gpu.TickWork(); });
    query_cache.UpdateCounters();

    texture_cache.SynchronizeGraphicsDescriptors();
    texture_cache.UpdateRenderTargets(false);

    SyncState();

    const auto& draw_texture_state = maxwell3d->draw_manager->GetDrawTextureState();
    const auto& sampler = texture_cache.GetGraphicsSampler(draw_texture_state.src_sampler);
    const auto& texture = texture_cache.GetImageView(draw_texture_state.src_texture);

    if (device.HasDrawTexture()) {
        state_tracker.BindFramebuffer(texture_cache.GetFramebuffer()->Handle());

        glDrawTextureNV(texture.DefaultHandle(), sampler->Handle(), draw_texture_state.dst_x0,
                        draw_texture_state.dst_y0, draw_texture_state.dst_x1,
                        draw_texture_state.dst_y1, 0,
                        draw_texture_state.src_x0 / static_cast<float>(texture.size.width),
                        draw_texture_state.src_y0 / static_cast<float>(texture.size.height),
                        draw_texture_state.src_x1 / static_cast<float>(texture.size.width),
                        draw_texture_state.src_y1 / static_cast<float>(texture.size.height));
    } else {
        Region2D dst_region = {Offset2D{.x = static_cast<s32>(draw_texture_state.dst_x0),
                                        .y = static_cast<s32>(draw_texture_state.dst_y0)},
                               Offset2D{.x = static_cast<s32>(draw_texture_state.dst_x1),
                                        .y = static_cast<s32>(draw_texture_state.dst_y1)}};
        Region2D src_region = {Offset2D{.x = static_cast<s32>(draw_texture_state.src_x0),
                                        .y = static_cast<s32>(draw_texture_state.src_y0)},
                               Offset2D{.x = static_cast<s32>(draw_texture_state.src_x1),
                                        .y = static_cast<s32>(draw_texture_state.src_y1)}};
        blit_image.BlitColor(texture_cache.GetFramebuffer()->Handle(), texture.DefaultHandle(),
                             sampler->Handle(), dst_region, src_region, texture.size);
        state_tracker.InvalidateState();
    }

    ++num_queued_commands;
}

void RasterizerOpenGL::DispatchCompute() {
    gpu_memory->FlushCaching();
    ComputePipeline* const pipeline{shader_cache.CurrentComputePipeline()};
    if (!pipeline) {
        return;
    }
    pipeline->SetEngine(kepler_compute, gpu_memory);
    pipeline->Configure();
    const auto& qmd{kepler_compute->launch_description};
    glDispatchCompute(qmd.grid_dim_x, qmd.grid_dim_y, qmd.grid_dim_z);
    ++num_queued_commands;
    has_written_global_memory |= pipeline->WritesGlobalMemory();
}

void RasterizerOpenGL::ResetCounter(VideoCore::QueryType type) {
    query_cache.ResetCounter(type);
}

void RasterizerOpenGL::Query(GPUVAddr gpu_addr, VideoCore::QueryType type,
                             std::optional<u64> timestamp) {
    query_cache.Query(gpu_addr, type, timestamp);
}

void RasterizerOpenGL::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                                 u32 size) {
    std::scoped_lock lock{buffer_cache.mutex};
    buffer_cache.BindGraphicsUniformBuffer(stage, index, gpu_addr, size);
}

void RasterizerOpenGL::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    buffer_cache.DisableGraphicsUniformBuffer(stage, index);
}

void RasterizerOpenGL::FlushAll() {}

void RasterizerOpenGL::FlushRegion(VAddr addr, u64 size, VideoCommon::CacheType which) {
    MICROPROFILE_SCOPE(OpenGL_CacheManagement);
    if (addr == 0 || size == 0) {
        return;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.DownloadMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.DownloadMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::QueryCache))) {
        query_cache.FlushRegion(addr, size);
    }
}

bool RasterizerOpenGL::MustFlushRegion(VAddr addr, u64 size, VideoCommon::CacheType which) {
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.IsRegionGpuModified(addr, size)) {
            return true;
        }
    }
    if (!Settings::IsGPULevelHigh()) {
        return false;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        return texture_cache.IsRegionGpuModified(addr, size);
    }
    return false;
}

VideoCore::RasterizerDownloadArea RasterizerOpenGL::GetFlushArea(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        auto area = texture_cache.GetFlushArea(addr, size);
        if (area) {
            return *area;
        }
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        auto area = buffer_cache.GetFlushArea(addr, size);
        if (area) {
            return *area;
        }
    }
    VideoCore::RasterizerDownloadArea new_area{
        .start_address = Common::AlignDown(addr, Core::Memory::YUZU_PAGESIZE),
        .end_address = Common::AlignUp(addr + size, Core::Memory::YUZU_PAGESIZE),
        .preemtive = true,
    };
    return new_area;
}

void RasterizerOpenGL::InvalidateRegion(VAddr addr, u64 size, VideoCommon::CacheType which) {
    MICROPROFILE_SCOPE(OpenGL_CacheManagement);
    if (addr == 0 || size == 0) {
        return;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    if (True(which & VideoCommon::CacheType::BufferCache)) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    if (True(which & VideoCommon::CacheType::ShaderCache)) {
        shader_cache.InvalidateRegion(addr, size);
    }
    if (True(which & VideoCommon::CacheType::QueryCache)) {
        query_cache.InvalidateRegion(addr, size);
    }
}

void RasterizerOpenGL::OnCPUWrite(VAddr addr, u64 size) {
    MICROPROFILE_SCOPE(OpenGL_CacheManagement);
    if (addr == 0 || size == 0) {
        return;
    }
    shader_cache.OnCPUWrite(addr, size);
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.CachedWriteMemory(addr, size);
    }
}

void RasterizerOpenGL::InvalidateGPUCache() {
    MICROPROFILE_SCOPE(OpenGL_CacheManagement);
    shader_cache.SyncGuestHost();
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.FlushCachedWrites();
    }
}

void RasterizerOpenGL::UnmapMemory(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.UnmapMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    shader_cache.OnCPUWrite(addr, size);
}

void RasterizerOpenGL::ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.UnmapGPUMemory(as_id, addr, size);
    }
}

void RasterizerOpenGL::SignalFence(std::function<void()>&& func) {
    fence_manager.SignalFence(std::move(func));
}

void RasterizerOpenGL::SyncOperation(std::function<void()>&& func) {
    fence_manager.SyncOperation(std::move(func));
}

void RasterizerOpenGL::SignalSyncPoint(u32 value) {
    fence_manager.SignalSyncPoint(value);
}

void RasterizerOpenGL::SignalReference() {
    fence_manager.SignalOrdering();
}

void RasterizerOpenGL::ReleaseFences() {
    fence_manager.WaitPendingFences();
}

void RasterizerOpenGL::FlushAndInvalidateRegion(VAddr addr, u64 size,
                                                VideoCommon::CacheType which) {
    if (Settings::IsGPULevelExtreme()) {
        FlushRegion(addr, size, which);
    }
    InvalidateRegion(addr, size, which);
}

void RasterizerOpenGL::WaitForIdle() {
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    SignalReference();
}

void RasterizerOpenGL::FragmentBarrier() {
    glTextureBarrier();
    glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void RasterizerOpenGL::TiledCacheBarrier() {
    glTextureBarrier();
}

void RasterizerOpenGL::FlushCommands() {
    // Only flush when we have commands queued to OpenGL.
    if (num_queued_commands == 0) {
        return;
    }
    num_queued_commands = 0;

    // Make sure memory stored from the previous GL command stream is visible
    // This is only needed on assembly shaders where we write to GPU memory with raw pointers
    if (has_written_global_memory) {
        has_written_global_memory = false;
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    }
    glFlush();
}

void RasterizerOpenGL::TickFrame() {
    // Ticking a frame means that buffers will be swapped, calling glFlush implicitly.
    num_queued_commands = 0;

    fence_manager.TickFrame();
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.TickFrame();
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.TickFrame();
    }
}

bool RasterizerOpenGL::AccelerateConditionalRendering() {
    gpu_memory->FlushCaching();
    if (Settings::IsGPULevelHigh()) {
        // Reimplement Host conditional rendering.
        return false;
    }
    // Medium / Low Hack: stub any checks on queries written into the buffer cache.
    const GPUVAddr condition_address{maxwell3d->regs.render_enable.Address()};
    Maxwell::ReportSemaphore::Compare cmp;
    if (gpu_memory->IsMemoryDirty(condition_address, sizeof(cmp),
                                  VideoCommon::CacheType::BufferCache)) {
        return true;
    }
    return false;
}

bool RasterizerOpenGL::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                             const Tegra::Engines::Fermi2D::Surface& dst,
                                             const Tegra::Engines::Fermi2D::Config& copy_config) {
    MICROPROFILE_SCOPE(OpenGL_Blits);
    std::scoped_lock lock{texture_cache.mutex};
    return texture_cache.BlitImage(dst, src, copy_config);
}

Tegra::Engines::AccelerateDMAInterface& RasterizerOpenGL::AccessAccelerateDMA() {
    return accelerate_dma;
}

void RasterizerOpenGL::AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                                std::span<const u8> memory) {
    auto cpu_addr = gpu_memory->GpuToCpuAddress(address);
    if (!cpu_addr) [[unlikely]] {
        gpu_memory->WriteBlock(address, memory.data(), copy_size);
        return;
    }
    gpu_memory->WriteBlockUnsafe(address, memory.data(), copy_size);
    {
        std::unique_lock<std::recursive_mutex> lock{buffer_cache.mutex};
        if (!buffer_cache.InlineMemory(*cpu_addr, copy_size, memory)) {
            buffer_cache.WriteMemory(*cpu_addr, copy_size);
        }
    }
    {
        std::scoped_lock lock_texture{texture_cache.mutex};
        texture_cache.WriteMemory(*cpu_addr, copy_size);
    }
    shader_cache.InvalidateRegion(*cpu_addr, copy_size);
    query_cache.InvalidateRegion(*cpu_addr, copy_size);
}

bool RasterizerOpenGL::AccelerateDisplay(const Tegra::FramebufferConfig& config,
                                         VAddr framebuffer_addr, u32 pixel_stride) {
    if (framebuffer_addr == 0) {
        return false;
    }
    MICROPROFILE_SCOPE(OpenGL_CacheManagement);

    std::scoped_lock lock{texture_cache.mutex};
    ImageView* const image_view{texture_cache.TryFindFramebufferImageView(framebuffer_addr)};
    if (!image_view) {
        return false;
    }
    // Verify that the cached surface is the same size and format as the requested framebuffer
    // ASSERT_MSG(image_view->size.width == config.width, "Framebuffer width is different");
    // ASSERT_MSG(image_view->size.height == config.height, "Framebuffer height is different");

    screen_info.texture.width = image_view->size.width;
    screen_info.texture.height = image_view->size.height;
    screen_info.display_texture = image_view->Handle(Shader::TextureType::Color2D);
    screen_info.display_srgb = VideoCore::Surface::IsPixelFormatSRGB(image_view->format);
    return true;
}

void RasterizerOpenGL::SyncState() {
    SyncViewport();
    SyncRasterizeEnable();
    SyncPolygonModes();
    SyncColorMask();
    SyncFragmentColorClampState();
    SyncMultiSampleState();
    SyncDepthTestState();
    SyncDepthClamp();
    SyncStencilTestState();
    SyncBlendState();
    SyncLogicOpState();
    SyncCullMode();
    SyncPrimitiveRestart();
    SyncScissorTest();
    SyncPointState();
    SyncLineState();
    SyncPolygonOffset();
    SyncAlphaTest();
    SyncFramebufferSRGB();
    SyncVertexFormats();
    SyncVertexInstances();
}

void RasterizerOpenGL::SyncViewport() {
    auto& flags = maxwell3d->dirty.flags;
    const auto& regs = maxwell3d->regs;

    const bool rescale_viewports = flags[VideoCommon::Dirty::RescaleViewports];
    const bool dirty_viewport = flags[Dirty::Viewports] || rescale_viewports;
    const bool dirty_clip_control = flags[Dirty::ClipControl];

    if (dirty_viewport || dirty_clip_control || flags[Dirty::FrontFace]) {
        flags[Dirty::FrontFace] = false;

        GLenum mode = MaxwellToGL::FrontFace(regs.gl_front_face);
        bool flip_faces = true;
        if (regs.window_origin.flip_y != 0) {
            flip_faces = !flip_faces;
        }
        if (regs.viewport_transform[0].scale_y < 0.0f) {
            flip_faces = !flip_faces;
        }
        if (flip_faces) {
            switch (mode) {
            case GL_CW:
                mode = GL_CCW;
                break;
            case GL_CCW:
                mode = GL_CW;
                break;
            }
        }
        glFrontFace(mode);
    }
    if (dirty_viewport || dirty_clip_control) {
        flags[Dirty::ClipControl] = false;

        bool flip_y = false;
        if (regs.viewport_transform[0].scale_y < 0.0f) {
            flip_y = !flip_y;
        }
        const bool lower_left{regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft};
        if (lower_left) {
            flip_y = !flip_y;
        }
        const bool is_zero_to_one = regs.depth_mode == Maxwell::DepthMode::ZeroToOne;
        const GLenum origin = flip_y ? GL_UPPER_LEFT : GL_LOWER_LEFT;
        const GLenum depth = is_zero_to_one ? GL_ZERO_TO_ONE : GL_NEGATIVE_ONE_TO_ONE;
        state_tracker.ClipControl(origin, depth);
        state_tracker.SetYNegate(lower_left);
    }
    const bool is_rescaling{texture_cache.IsRescaling()};
    const float scale = is_rescaling ? Settings::values.resolution_info.up_factor : 1.0f;
    const auto conv = [scale](float value) -> GLfloat {
        float new_value = value * scale;
        if (scale < 1.0f) {
            const bool sign = std::signbit(value);
            new_value = std::round(std::abs(new_value));
            new_value = sign ? -new_value : new_value;
        }
        return static_cast<GLfloat>(new_value);
    };

    if (dirty_viewport) {
        flags[Dirty::Viewports] = false;

        const bool force = flags[Dirty::ViewportTransform] || rescale_viewports;
        flags[Dirty::ViewportTransform] = false;
        flags[VideoCommon::Dirty::RescaleViewports] = false;

        for (size_t index = 0; index < Maxwell::NumViewports; ++index) {
            if (!force && !flags[Dirty::Viewport0 + index]) {
                continue;
            }
            flags[Dirty::Viewport0 + index] = false;

            if (!regs.viewport_scale_offset_enabled) {
                const auto x = static_cast<GLfloat>(regs.surface_clip.x);
                const auto y = static_cast<GLfloat>(regs.surface_clip.y);
                const auto width = static_cast<GLfloat>(regs.surface_clip.width);
                const auto height = static_cast<GLfloat>(regs.surface_clip.height);
                glViewportIndexedf(static_cast<GLuint>(index), x, y, width != 0.0f ? width : 1.0f,
                                   height != 0.0f ? height : 1.0f);
                continue;
            }

            const auto& src = regs.viewport_transform[index];
            GLfloat x = conv(src.translate_x - src.scale_x);
            GLfloat y = conv(src.translate_y - src.scale_y);
            GLfloat width = conv(src.scale_x * 2.0f);
            GLfloat height = conv(src.scale_y * 2.0f);

            if (height < 0) {
                y += height;
                height = -height;
            }
            glViewportIndexedf(static_cast<GLuint>(index), x, y, width != 0.0f ? width : 1.0f,
                               height != 0.0f ? height : 1.0f);

            const GLdouble reduce_z = regs.depth_mode == Maxwell::DepthMode::MinusOneToOne;
            const GLdouble near_depth = src.translate_z - src.scale_z * reduce_z;
            const GLdouble far_depth = src.translate_z + src.scale_z;
            if (device.HasDepthBufferFloat()) {
                glDepthRangeIndexeddNV(static_cast<GLuint>(index), near_depth, far_depth);
            } else {
                glDepthRangeIndexed(static_cast<GLuint>(index), near_depth, far_depth);
            }

            if (!GLAD_GL_NV_viewport_swizzle) {
                continue;
            }
            glViewportSwizzleNV(static_cast<GLuint>(index),
                                MaxwellToGL::ViewportSwizzle(src.swizzle.x),
                                MaxwellToGL::ViewportSwizzle(src.swizzle.y),
                                MaxwellToGL::ViewportSwizzle(src.swizzle.z),
                                MaxwellToGL::ViewportSwizzle(src.swizzle.w));
        }
    }
}

void RasterizerOpenGL::SyncDepthClamp() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::DepthClampEnabled]) {
        return;
    }
    flags[Dirty::DepthClampEnabled] = false;

    bool depth_clamp_disabled{maxwell3d->regs.viewport_clip_control.geometry_clip ==
                                  Maxwell::ViewportClipControl::GeometryClip::Passthrough ||
                              maxwell3d->regs.viewport_clip_control.geometry_clip ==
                                  Maxwell::ViewportClipControl::GeometryClip::FrustumXYZ ||
                              maxwell3d->regs.viewport_clip_control.geometry_clip ==
                                  Maxwell::ViewportClipControl::GeometryClip::FrustumZ};
    oglEnable(GL_DEPTH_CLAMP, !depth_clamp_disabled);
}

void RasterizerOpenGL::SyncClipEnabled(u32 clip_mask) {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::ClipDistances] && !flags[VideoCommon::Dirty::Shaders]) {
        return;
    }
    flags[Dirty::ClipDistances] = false;

    clip_mask &= maxwell3d->regs.user_clip_enable.raw;
    if (clip_mask == last_clip_distance_mask) {
        return;
    }
    last_clip_distance_mask = clip_mask;

    for (std::size_t i = 0; i < Maxwell::Regs::NumClipDistances; ++i) {
        oglEnable(static_cast<GLenum>(GL_CLIP_DISTANCE0 + i), (clip_mask >> i) & 1);
    }
}

void RasterizerOpenGL::SyncClipCoef() {
    UNIMPLEMENTED();
}

void RasterizerOpenGL::SyncCullMode() {
    auto& flags = maxwell3d->dirty.flags;
    const auto& regs = maxwell3d->regs;

    if (flags[Dirty::CullTest]) {
        flags[Dirty::CullTest] = false;

        if (regs.gl_cull_test_enabled) {
            glEnable(GL_CULL_FACE);
            glCullFace(MaxwellToGL::CullFace(regs.gl_cull_face));
        } else {
            glDisable(GL_CULL_FACE);
        }
    }
}

void RasterizerOpenGL::SyncPrimitiveRestart() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::PrimitiveRestart]) {
        return;
    }
    flags[Dirty::PrimitiveRestart] = false;

    if (maxwell3d->regs.primitive_restart.enabled) {
        glEnable(GL_PRIMITIVE_RESTART);
        glPrimitiveRestartIndex(maxwell3d->regs.primitive_restart.index);
    } else {
        glDisable(GL_PRIMITIVE_RESTART);
    }
}

void RasterizerOpenGL::SyncDepthTestState() {
    auto& flags = maxwell3d->dirty.flags;
    const auto& regs = maxwell3d->regs;

    if (flags[Dirty::DepthMask]) {
        flags[Dirty::DepthMask] = false;
        glDepthMask(regs.depth_write_enabled ? GL_TRUE : GL_FALSE);
    }

    if (flags[Dirty::DepthTest]) {
        flags[Dirty::DepthTest] = false;
        if (regs.depth_test_enable) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(MaxwellToGL::ComparisonOp(regs.depth_test_func));
        } else {
            glDisable(GL_DEPTH_TEST);
        }
    }
}

void RasterizerOpenGL::SyncStencilTestState() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::StencilTest]) {
        return;
    }
    flags[Dirty::StencilTest] = false;

    const auto& regs = maxwell3d->regs;
    oglEnable(GL_STENCIL_TEST, regs.stencil_enable);

    glStencilFuncSeparate(GL_FRONT, MaxwellToGL::ComparisonOp(regs.stencil_front_op.func),
                          regs.stencil_front_ref, regs.stencil_front_func_mask);
    glStencilOpSeparate(GL_FRONT, MaxwellToGL::StencilOp(regs.stencil_front_op.fail),
                        MaxwellToGL::StencilOp(regs.stencil_front_op.zfail),
                        MaxwellToGL::StencilOp(regs.stencil_front_op.zpass));
    glStencilMaskSeparate(GL_FRONT, regs.stencil_front_mask);

    if (regs.stencil_two_side_enable) {
        glStencilFuncSeparate(GL_BACK, MaxwellToGL::ComparisonOp(regs.stencil_back_op.func),
                              regs.stencil_back_ref, regs.stencil_back_func_mask);
        glStencilOpSeparate(GL_BACK, MaxwellToGL::StencilOp(regs.stencil_back_op.fail),
                            MaxwellToGL::StencilOp(regs.stencil_back_op.zfail),
                            MaxwellToGL::StencilOp(regs.stencil_back_op.zpass));
        glStencilMaskSeparate(GL_BACK, regs.stencil_back_mask);
    } else {
        glStencilFuncSeparate(GL_BACK, GL_ALWAYS, 0, 0xFFFFFFFF);
        glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilMaskSeparate(GL_BACK, 0xFFFFFFFF);
    }
}

void RasterizerOpenGL::SyncRasterizeEnable() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::RasterizeEnable]) {
        return;
    }
    flags[Dirty::RasterizeEnable] = false;

    oglEnable(GL_RASTERIZER_DISCARD, maxwell3d->regs.rasterize_enable == 0);
}

void RasterizerOpenGL::SyncPolygonModes() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::PolygonModes]) {
        return;
    }
    flags[Dirty::PolygonModes] = false;

    const auto& regs = maxwell3d->regs;
    if (regs.fill_via_triangle_mode != Maxwell::FillViaTriangleMode::Disabled) {
        if (!GLAD_GL_NV_fill_rectangle) {
            LOG_ERROR(Render_OpenGL, "GL_NV_fill_rectangle used and not supported");
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            return;
        }

        flags[Dirty::PolygonModeFront] = true;
        flags[Dirty::PolygonModeBack] = true;
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL_RECTANGLE_NV);
        return;
    }

    if (regs.polygon_mode_front == regs.polygon_mode_back) {
        flags[Dirty::PolygonModeFront] = false;
        flags[Dirty::PolygonModeBack] = false;
        glPolygonMode(GL_FRONT_AND_BACK, MaxwellToGL::PolygonMode(regs.polygon_mode_front));
        return;
    }

    if (flags[Dirty::PolygonModeFront]) {
        flags[Dirty::PolygonModeFront] = false;
        glPolygonMode(GL_FRONT, MaxwellToGL::PolygonMode(regs.polygon_mode_front));
    }

    if (flags[Dirty::PolygonModeBack]) {
        flags[Dirty::PolygonModeBack] = false;
        glPolygonMode(GL_BACK, MaxwellToGL::PolygonMode(regs.polygon_mode_back));
    }
}

void RasterizerOpenGL::SyncColorMask() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::ColorMasks]) {
        return;
    }
    flags[Dirty::ColorMasks] = false;

    const bool force = flags[Dirty::ColorMaskCommon];
    flags[Dirty::ColorMaskCommon] = false;

    const auto& regs = maxwell3d->regs;
    if (regs.color_mask_common) {
        if (!force && !flags[Dirty::ColorMask0]) {
            return;
        }
        flags[Dirty::ColorMask0] = false;

        auto& mask = regs.color_mask[0];
        glColorMask(mask.R != 0, mask.B != 0, mask.G != 0, mask.A != 0);
        return;
    }

    // Path without color_mask_common set
    for (std::size_t i = 0; i < Maxwell::NumRenderTargets; ++i) {
        if (!force && !flags[Dirty::ColorMask0 + i]) {
            continue;
        }
        flags[Dirty::ColorMask0 + i] = false;

        const auto& mask = regs.color_mask[i];
        glColorMaski(static_cast<GLuint>(i), mask.R != 0, mask.G != 0, mask.B != 0, mask.A != 0);
    }
}

void RasterizerOpenGL::SyncMultiSampleState() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::MultisampleControl]) {
        return;
    }
    flags[Dirty::MultisampleControl] = false;

    const auto& regs = maxwell3d->regs;
    oglEnable(GL_SAMPLE_ALPHA_TO_COVERAGE, regs.anti_alias_alpha_control.alpha_to_coverage);
    oglEnable(GL_SAMPLE_ALPHA_TO_ONE, regs.anti_alias_alpha_control.alpha_to_one);
}

void RasterizerOpenGL::SyncFragmentColorClampState() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::FragmentClampColor]) {
        return;
    }
    flags[Dirty::FragmentClampColor] = false;

    glClampColor(GL_CLAMP_FRAGMENT_COLOR,
                 maxwell3d->regs.frag_color_clamp.AnyEnabled() ? GL_TRUE : GL_FALSE);
}

void RasterizerOpenGL::SyncBlendState() {
    auto& flags = maxwell3d->dirty.flags;
    const auto& regs = maxwell3d->regs;

    if (flags[Dirty::BlendColor]) {
        flags[Dirty::BlendColor] = false;
        glBlendColor(regs.blend_color.r, regs.blend_color.g, regs.blend_color.b,
                     regs.blend_color.a);
    }

    // TODO(Rodrigo): Revisit blending, there are several registers we are not reading

    if (!flags[Dirty::BlendStates]) {
        return;
    }
    flags[Dirty::BlendStates] = false;

    if (!regs.blend_per_target_enabled) {
        if (!regs.blend.enable[0]) {
            glDisable(GL_BLEND);
            return;
        }
        glEnable(GL_BLEND);
        glBlendFuncSeparate(MaxwellToGL::BlendFunc(regs.blend.color_source),
                            MaxwellToGL::BlendFunc(regs.blend.color_dest),
                            MaxwellToGL::BlendFunc(regs.blend.alpha_source),
                            MaxwellToGL::BlendFunc(regs.blend.alpha_dest));
        glBlendEquationSeparate(MaxwellToGL::BlendEquation(regs.blend.color_op),
                                MaxwellToGL::BlendEquation(regs.blend.alpha_op));
        return;
    }

    const bool force = flags[Dirty::BlendIndependentEnabled];
    flags[Dirty::BlendIndependentEnabled] = false;

    for (std::size_t i = 0; i < Maxwell::NumRenderTargets; ++i) {
        if (!force && !flags[Dirty::BlendState0 + i]) {
            continue;
        }
        flags[Dirty::BlendState0 + i] = false;

        if (!regs.blend.enable[i]) {
            glDisablei(GL_BLEND, static_cast<GLuint>(i));
            continue;
        }
        glEnablei(GL_BLEND, static_cast<GLuint>(i));

        const auto& src = regs.blend_per_target[i];
        glBlendFuncSeparatei(static_cast<GLuint>(i), MaxwellToGL::BlendFunc(src.color_source),
                             MaxwellToGL::BlendFunc(src.color_dest),
                             MaxwellToGL::BlendFunc(src.alpha_source),
                             MaxwellToGL::BlendFunc(src.alpha_dest));
        glBlendEquationSeparatei(static_cast<GLuint>(i), MaxwellToGL::BlendEquation(src.color_op),
                                 MaxwellToGL::BlendEquation(src.alpha_op));
    }
}

void RasterizerOpenGL::SyncLogicOpState() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::LogicOp]) {
        return;
    }
    flags[Dirty::LogicOp] = false;

    const auto& regs = maxwell3d->regs;
    if (regs.logic_op.enable) {
        glEnable(GL_COLOR_LOGIC_OP);
        glLogicOp(MaxwellToGL::LogicOp(regs.logic_op.op));
    } else {
        glDisable(GL_COLOR_LOGIC_OP);
    }
}

void RasterizerOpenGL::SyncScissorTest() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::Scissors] && !flags[VideoCommon::Dirty::RescaleScissors]) {
        return;
    }
    flags[Dirty::Scissors] = false;

    const bool force = flags[VideoCommon::Dirty::RescaleScissors];
    flags[VideoCommon::Dirty::RescaleScissors] = false;

    const auto& regs = maxwell3d->regs;

    const auto& resolution = Settings::values.resolution_info;
    const bool is_rescaling{texture_cache.IsRescaling()};
    const u32 up_scale = is_rescaling ? resolution.up_scale : 1U;
    const u32 down_shift = is_rescaling ? resolution.down_shift : 0U;
    const auto scale_up = [up_scale, down_shift](u32 value) -> u32 {
        if (value == 0) {
            return 0U;
        }
        const u32 upset = value * up_scale;
        u32 acumm{};
        if ((up_scale >> down_shift) == 0) {
            acumm = upset % 2;
        }
        const u32 converted_value = upset >> down_shift;
        return std::max<u32>(converted_value + acumm, 1U);
    };
    for (std::size_t index = 0; index < Maxwell::NumViewports; ++index) {
        if (!force && !flags[Dirty::Scissor0 + index]) {
            continue;
        }
        flags[Dirty::Scissor0 + index] = false;

        const auto& src = regs.scissor_test[index];
        if (src.enable) {
            glEnablei(GL_SCISSOR_TEST, static_cast<GLuint>(index));
            glScissorIndexed(static_cast<GLuint>(index), scale_up(src.min_x), scale_up(src.min_y),
                             scale_up(src.max_x - src.min_x), scale_up(src.max_y - src.min_y));
        } else {
            glDisablei(GL_SCISSOR_TEST, static_cast<GLuint>(index));
        }
    }
}

void RasterizerOpenGL::SyncPointState() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::PointSize]) {
        return;
    }
    flags[Dirty::PointSize] = false;

    oglEnable(GL_POINT_SPRITE, maxwell3d->regs.point_sprite_enable);
    oglEnable(GL_PROGRAM_POINT_SIZE, maxwell3d->regs.point_size_attribute.enabled);
    const bool is_rescaling{texture_cache.IsRescaling()};
    const float scale = is_rescaling ? Settings::values.resolution_info.up_factor : 1.0f;
    glPointSize(std::max(1.0f, maxwell3d->regs.point_size * scale));
}

void RasterizerOpenGL::SyncLineState() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::LineWidth]) {
        return;
    }
    flags[Dirty::LineWidth] = false;

    const auto& regs = maxwell3d->regs;
    oglEnable(GL_LINE_SMOOTH, regs.line_anti_alias_enable);
    glLineWidth(regs.line_anti_alias_enable ? regs.line_width_smooth : regs.line_width_aliased);
}

void RasterizerOpenGL::SyncPolygonOffset() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::PolygonOffset]) {
        return;
    }
    flags[Dirty::PolygonOffset] = false;

    const auto& regs = maxwell3d->regs;
    oglEnable(GL_POLYGON_OFFSET_FILL, regs.polygon_offset_fill_enable);
    oglEnable(GL_POLYGON_OFFSET_LINE, regs.polygon_offset_line_enable);
    oglEnable(GL_POLYGON_OFFSET_POINT, regs.polygon_offset_point_enable);

    if (regs.polygon_offset_fill_enable || regs.polygon_offset_line_enable ||
        regs.polygon_offset_point_enable) {
        // Hardware divides polygon offset units by two
        glPolygonOffsetClamp(regs.slope_scale_depth_bias, regs.depth_bias / 2.0f,
                             regs.depth_bias_clamp);
    }
}

void RasterizerOpenGL::SyncAlphaTest() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::AlphaTest]) {
        return;
    }
    flags[Dirty::AlphaTest] = false;

    const auto& regs = maxwell3d->regs;
    if (regs.alpha_test_enabled) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(MaxwellToGL::ComparisonOp(regs.alpha_test_func), regs.alpha_test_ref);
    } else {
        glDisable(GL_ALPHA_TEST);
    }
}

void RasterizerOpenGL::SyncFramebufferSRGB() {
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::FramebufferSRGB]) {
        return;
    }
    flags[Dirty::FramebufferSRGB] = false;

    oglEnable(GL_FRAMEBUFFER_SRGB, maxwell3d->regs.framebuffer_srgb);
}

void RasterizerOpenGL::BeginTransformFeedback(GraphicsPipeline* program, GLenum primitive_mode) {
    const auto& regs = maxwell3d->regs;
    if (regs.transform_feedback_enabled == 0) {
        return;
    }
    program->ConfigureTransformFeedback();

    UNIMPLEMENTED_IF(regs.IsShaderConfigEnabled(Maxwell::ShaderType::TessellationInit) ||
                     regs.IsShaderConfigEnabled(Maxwell::ShaderType::Tessellation) ||
                     regs.IsShaderConfigEnabled(Maxwell::ShaderType::Geometry));
    UNIMPLEMENTED_IF(primitive_mode != GL_POINTS);

    // We may have to call BeginTransformFeedbackNV here since they seem to call different
    // implementations on Nvidia's driver (the pointer is different) but we are using
    // ARB_transform_feedback3 features with NV_transform_feedback interactions and the ARB
    // extension doesn't define BeginTransformFeedback (without NV) interactions. It just works.
    glBeginTransformFeedback(GL_POINTS);
}

void RasterizerOpenGL::EndTransformFeedback() {
    if (maxwell3d->regs.transform_feedback_enabled != 0) {
        glEndTransformFeedback();
    }
}

void RasterizerOpenGL::InitializeChannel(Tegra::Control::ChannelState& channel) {
    CreateChannel(channel);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.CreateChannel(channel);
        buffer_cache.CreateChannel(channel);
    }
    shader_cache.CreateChannel(channel);
    query_cache.CreateChannel(channel);
    state_tracker.SetupTables(channel);
}

void RasterizerOpenGL::BindChannel(Tegra::Control::ChannelState& channel) {
    const s32 channel_id = channel.bind_id;
    BindToChannel(channel_id);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.BindToChannel(channel_id);
        buffer_cache.BindToChannel(channel_id);
    }
    shader_cache.BindToChannel(channel_id);
    query_cache.BindToChannel(channel_id);
    state_tracker.ChangeChannel(channel);
    state_tracker.InvalidateState();
}

void RasterizerOpenGL::ReleaseChannel(s32 channel_id) {
    EraseChannel(channel_id);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.EraseChannel(channel_id);
        buffer_cache.EraseChannel(channel_id);
    }
    shader_cache.EraseChannel(channel_id);
    query_cache.EraseChannel(channel_id);
}

AccelerateDMA::AccelerateDMA(BufferCache& buffer_cache_, TextureCache& texture_cache_)
    : buffer_cache{buffer_cache_}, texture_cache{texture_cache_} {}

bool AccelerateDMA::BufferCopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount) {
    std::scoped_lock lock{buffer_cache.mutex};
    return buffer_cache.DMACopy(src_address, dest_address, amount);
}

bool AccelerateDMA::BufferClear(GPUVAddr src_address, u64 amount, u32 value) {
    std::scoped_lock lock{buffer_cache.mutex};
    return buffer_cache.DMAClear(src_address, amount, value);
}

template <bool IS_IMAGE_UPLOAD>
bool AccelerateDMA::DmaBufferImageCopy(const Tegra::DMA::ImageCopy& copy_info,
                                       const Tegra::DMA::BufferOperand& buffer_operand,
                                       const Tegra::DMA::ImageOperand& image_operand) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const auto image_id = texture_cache.DmaImageId(image_operand, IS_IMAGE_UPLOAD);
    if (image_id == VideoCommon::NULL_IMAGE_ID) {
        return false;
    }
    const u32 buffer_size = static_cast<u32>(buffer_operand.pitch * buffer_operand.height);
    static constexpr auto sync_info = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
    const auto post_op = VideoCommon::ObtainBufferOperation::DoNothing;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(buffer_operand.address, buffer_size, sync_info, post_op);

    const auto [image, copy] = texture_cache.DmaBufferImageCopy(
        copy_info, buffer_operand, image_operand, image_id, IS_IMAGE_UPLOAD);
    const std::span copy_span{&copy, 1};

    if constexpr (IS_IMAGE_UPLOAD) {
        image->UploadMemory(buffer->Handle(), offset, copy_span);
    } else {
        texture_cache.DownloadImageIntoBuffer(image, buffer->Handle(), offset, copy_span,
                                              buffer_operand.address, buffer_size);
    }
    return true;
}

bool AccelerateDMA::ImageToBuffer(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::ImageOperand& image_operand,
                                  const Tegra::DMA::BufferOperand& buffer_operand) {
    return DmaBufferImageCopy<false>(copy_info, buffer_operand, image_operand);
}

bool AccelerateDMA::BufferToImage(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::BufferOperand& buffer_operand,
                                  const Tegra::DMA::ImageOperand& image_operand) {
    return DmaBufferImageCopy<true>(copy_info, buffer_operand, image_operand);
}

} // namespace OpenGL
