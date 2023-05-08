// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string_view>

#include "common/assert.h"
#include "common/fs/path_util.h"
#include "common/logging/log.h"
#include "common/settings.h"

namespace Settings {

Values values;
static bool configuring_global = true;

std::string GetTimeZoneString() {
    static constexpr std::array timezones{
        "auto",      "default",   "CET", "CST6CDT", "Cuba",    "EET",    "Egypt",     "Eire",
        "EST",       "EST5EDT",   "GB",  "GB-Eire", "GMT",     "GMT+0",  "GMT-0",     "GMT0",
        "Greenwich", "Hongkong",  "HST", "Iceland", "Iran",    "Israel", "Jamaica",   "Japan",
        "Kwajalein", "Libya",     "MET", "MST",     "MST7MDT", "Navajo", "NZ",        "NZ-CHAT",
        "Poland",    "Portugal",  "PRC", "PST8PDT", "ROC",     "ROK",    "Singapore", "Turkey",
        "UCT",       "Universal", "UTC", "W-SU",    "WET",     "Zulu",
    };

    const auto time_zone_index = static_cast<std::size_t>(values.time_zone_index.GetValue());
    ASSERT(time_zone_index < timezones.size());
    return timezones[time_zone_index];
}

void LogSettings() {
    const auto log_setting = [](std::string_view name, const auto& value) {
        LOG_INFO(Config, "{}: {}", name, value);
    };

    const auto log_path = [](std::string_view name, const std::filesystem::path& path) {
        LOG_INFO(Config, "{}: {}", name, Common::FS::PathToUTF8String(path));
    };

    LOG_INFO(Config, "yuzu Configuration:");
    log_setting("Controls_UseDockedMode", values.use_docked_mode.GetValue());
    log_setting("System_RngSeed", values.rng_seed.GetValue().value_or(0));
    log_setting("System_DeviceName", values.device_name.GetValue());
    log_setting("System_CurrentUser", values.current_user.GetValue());
    log_setting("System_LanguageIndex", values.language_index.GetValue());
    log_setting("System_RegionIndex", values.region_index.GetValue());
    log_setting("System_TimeZoneIndex", values.time_zone_index.GetValue());
    log_setting("System_UnsafeMemoryLayout", values.use_unsafe_extended_memory_layout.GetValue());
    log_setting("Core_UseMultiCore", values.use_multi_core.GetValue());
    log_setting("CPU_Accuracy", values.cpu_accuracy.GetValue());
    log_setting("Renderer_UseResolutionScaling", values.resolution_setup.GetValue());
    log_setting("Renderer_ScalingFilter", values.scaling_filter.GetValue());
    log_setting("Renderer_FSRSlider", values.fsr_sharpening_slider.GetValue());
    log_setting("Renderer_AntiAliasing", values.anti_aliasing.GetValue());
    log_setting("Renderer_UseSpeedLimit", values.use_speed_limit.GetValue());
    log_setting("Renderer_SpeedLimit", values.speed_limit.GetValue());
    log_setting("Renderer_UseDiskShaderCache", values.use_disk_shader_cache.GetValue());
    log_setting("Renderer_GPUAccuracyLevel", values.gpu_accuracy.GetValue());
    log_setting("Renderer_UseAsynchronousGpuEmulation",
                values.use_asynchronous_gpu_emulation.GetValue());
    log_setting("Renderer_NvdecEmulation", values.nvdec_emulation.GetValue());
    log_setting("Renderer_AccelerateASTC", values.accelerate_astc.GetValue());
    log_setting("Renderer_AsyncASTC", values.async_astc.GetValue());
    log_setting("Renderer_UseVsync", values.vsync_mode.GetValue());
    log_setting("Renderer_UseReactiveFlushing", values.use_reactive_flushing.GetValue());
    log_setting("Renderer_ShaderBackend", values.shader_backend.GetValue());
    log_setting("Renderer_UseAsynchronousShaders", values.use_asynchronous_shaders.GetValue());
    log_setting("Renderer_AnisotropicFilteringLevel", values.max_anisotropy.GetValue());
    log_setting("Audio_OutputEngine", values.sink_id.GetValue());
    log_setting("Audio_OutputDevice", values.audio_output_device_id.GetValue());
    log_setting("Audio_InputDevice", values.audio_input_device_id.GetValue());
    log_setting("DataStorage_UseVirtualSd", values.use_virtual_sd.GetValue());
    log_path("DataStorage_CacheDir", Common::FS::GetYuzuPath(Common::FS::YuzuPath::CacheDir));
    log_path("DataStorage_ConfigDir", Common::FS::GetYuzuPath(Common::FS::YuzuPath::ConfigDir));
    log_path("DataStorage_LoadDir", Common::FS::GetYuzuPath(Common::FS::YuzuPath::LoadDir));
    log_path("DataStorage_NANDDir", Common::FS::GetYuzuPath(Common::FS::YuzuPath::NANDDir));
    log_path("DataStorage_SDMCDir", Common::FS::GetYuzuPath(Common::FS::YuzuPath::SDMCDir));
    log_setting("Debugging_ProgramArgs", values.program_args.GetValue());
    log_setting("Debugging_GDBStub", values.use_gdbstub.GetValue());
    log_setting("Input_EnableMotion", values.motion_enabled.GetValue());
    log_setting("Input_EnableVibration", values.vibration_enabled.GetValue());
    log_setting("Input_EnableTouch", values.touchscreen.enabled);
    log_setting("Input_EnableMouse", values.mouse_enabled.GetValue());
    log_setting("Input_EnableKeyboard", values.keyboard_enabled.GetValue());
    log_setting("Input_EnableRingController", values.enable_ring_controller.GetValue());
    log_setting("Input_EnableIrSensor", values.enable_ir_sensor.GetValue());
    log_setting("Input_EnableCustomJoycon", values.enable_joycon_driver.GetValue());
    log_setting("Input_EnableCustomProController", values.enable_procon_driver.GetValue());
    log_setting("Input_EnableRawInput", values.enable_raw_input.GetValue());
}

bool IsConfiguringGlobal() {
    return configuring_global;
}

void SetConfiguringGlobal(bool is_global) {
    configuring_global = is_global;
}

bool IsGPULevelExtreme() {
    return values.gpu_accuracy.GetValue() == GPUAccuracy::Extreme;
}

bool IsGPULevelHigh() {
    return values.gpu_accuracy.GetValue() == GPUAccuracy::Extreme ||
           values.gpu_accuracy.GetValue() == GPUAccuracy::High;
}

bool IsFastmemEnabled() {
    if (values.cpu_debug_mode) {
        return static_cast<bool>(values.cpuopt_fastmem);
    }
    return true;
}

float Volume() {
    if (values.audio_muted) {
        return 0.0f;
    }
    return values.volume.GetValue() / static_cast<f32>(values.volume.GetDefault());
}

void UpdateRescalingInfo() {
    const auto setup = values.resolution_setup.GetValue();
    auto& info = values.resolution_info;
    info.downscale = false;
    switch (setup) {
    case ResolutionSetup::Res1_2X:
        info.up_scale = 1;
        info.down_shift = 1;
        info.downscale = true;
        break;
    case ResolutionSetup::Res3_4X:
        info.up_scale = 3;
        info.down_shift = 2;
        info.downscale = true;
        break;
    case ResolutionSetup::Res1X:
        info.up_scale = 1;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res3_2X:
        info.up_scale = 3;
        info.down_shift = 1;
        break;
    case ResolutionSetup::Res2X:
        info.up_scale = 2;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res3X:
        info.up_scale = 3;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res4X:
        info.up_scale = 4;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res5X:
        info.up_scale = 5;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res6X:
        info.up_scale = 6;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res7X:
        info.up_scale = 7;
        info.down_shift = 0;
        break;
    case ResolutionSetup::Res8X:
        info.up_scale = 8;
        info.down_shift = 0;
        break;
    default:
        ASSERT(false);
        info.up_scale = 1;
        info.down_shift = 0;
        break;
    }
    info.up_factor = static_cast<f32>(info.up_scale) / (1U << info.down_shift);
    info.down_factor = static_cast<f32>(1U << info.down_shift) / info.up_scale;
    info.active = info.up_scale != 1 || info.down_shift != 0;
}

void RestoreGlobalState(bool is_powered_on) {
    // If a game is running, DO NOT restore the global settings state
    if (is_powered_on) {
        return;
    }

    // Audio
    values.volume.SetGlobal(true);

    // Core
    values.use_multi_core.SetGlobal(true);
    values.use_unsafe_extended_memory_layout.SetGlobal(true);

    // CPU
    values.cpu_accuracy.SetGlobal(true);
    values.cpuopt_unsafe_unfuse_fma.SetGlobal(true);
    values.cpuopt_unsafe_reduce_fp_error.SetGlobal(true);
    values.cpuopt_unsafe_ignore_standard_fpcr.SetGlobal(true);
    values.cpuopt_unsafe_inaccurate_nan.SetGlobal(true);
    values.cpuopt_unsafe_fastmem_check.SetGlobal(true);
    values.cpuopt_unsafe_ignore_global_monitor.SetGlobal(true);

    // Renderer
    values.fsr_sharpening_slider.SetGlobal(true);
    values.renderer_backend.SetGlobal(true);
    values.async_presentation.SetGlobal(true);
    values.renderer_force_max_clock.SetGlobal(true);
    values.vulkan_device.SetGlobal(true);
    values.fullscreen_mode.SetGlobal(true);
    values.aspect_ratio.SetGlobal(true);
    values.resolution_setup.SetGlobal(true);
    values.scaling_filter.SetGlobal(true);
    values.anti_aliasing.SetGlobal(true);
    values.max_anisotropy.SetGlobal(true);
    values.use_speed_limit.SetGlobal(true);
    values.speed_limit.SetGlobal(true);
    values.use_disk_shader_cache.SetGlobal(true);
    values.gpu_accuracy.SetGlobal(true);
    values.use_asynchronous_gpu_emulation.SetGlobal(true);
    values.nvdec_emulation.SetGlobal(true);
    values.accelerate_astc.SetGlobal(true);
    values.async_astc.SetGlobal(true);
    values.use_reactive_flushing.SetGlobal(true);
    values.shader_backend.SetGlobal(true);
    values.use_asynchronous_shaders.SetGlobal(true);
    values.use_fast_gpu_time.SetGlobal(true);
    values.use_vulkan_driver_pipeline_cache.SetGlobal(true);
    values.bg_red.SetGlobal(true);
    values.bg_green.SetGlobal(true);
    values.bg_blue.SetGlobal(true);
    values.enable_compute_pipelines.SetGlobal(true);

    // System
    values.language_index.SetGlobal(true);
    values.region_index.SetGlobal(true);
    values.time_zone_index.SetGlobal(true);
    values.rng_seed.SetGlobal(true);
    values.sound_index.SetGlobal(true);

    // Controls
    values.players.SetGlobal(true);
    values.use_docked_mode.SetGlobal(true);
    values.vibration_enabled.SetGlobal(true);
    values.motion_enabled.SetGlobal(true);
}

} // namespace Settings
