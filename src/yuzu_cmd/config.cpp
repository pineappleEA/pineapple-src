// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <optional>
#include <sstream>
#include <INIReader.h>
#include <SDL.h>
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/hle/service/acc/profile_manager.h"
#include "input_common/main.h"
#include "yuzu_cmd/config.h"
#include "yuzu_cmd/default_ini.h"

namespace FS = Common::FS;

const std::filesystem::path default_config_path =
    FS::GetYuzuPath(FS::YuzuPath::ConfigDir) / "sdl2-config.ini";

Config::Config(std::optional<std::filesystem::path> config_path)
    : sdl2_config_loc{config_path.value_or(default_config_path)},
      sdl2_config{std::make_unique<INIReader>(FS::PathToUTF8String(sdl2_config_loc))} {
    Reload();
}

Config::~Config() = default;

bool Config::LoadINI(const std::string& default_contents, bool retry) {
    const auto config_loc_str = FS::PathToUTF8String(sdl2_config_loc);
    if (sdl2_config->ParseError() < 0) {
        if (retry) {
            LOG_WARNING(Config, "Failed to load {}. Creating file from defaults...",
                        config_loc_str);

            void(FS::CreateParentDir(sdl2_config_loc));
            void(FS::WriteStringToFile(sdl2_config_loc, FS::FileType::TextFile, default_contents));

            sdl2_config = std::make_unique<INIReader>(config_loc_str);

            return LoadINI(default_contents, false);
        }
        LOG_ERROR(Config, "Failed.");
        return false;
    }
    LOG_INFO(Config, "Successfully loaded {}", config_loc_str);
    return true;
}

static const std::array<int, Settings::NativeButton::NumButtons> default_buttons = {
    SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_T,
    SDL_SCANCODE_G, SDL_SCANCODE_F, SDL_SCANCODE_H, SDL_SCANCODE_Q, SDL_SCANCODE_W,
    SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_B,
};

static const std::array<int, Settings::NativeMotion::NumMotions> default_motions = {
    SDL_SCANCODE_7,
    SDL_SCANCODE_8,
};

static const std::array<std::array<int, 5>, Settings::NativeAnalog::NumAnalogs> default_analogs{{
    {
        SDL_SCANCODE_UP,
        SDL_SCANCODE_DOWN,
        SDL_SCANCODE_LEFT,
        SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_D,
    },
    {
        SDL_SCANCODE_I,
        SDL_SCANCODE_K,
        SDL_SCANCODE_J,
        SDL_SCANCODE_L,
        SDL_SCANCODE_D,
    },
}};

template <>
void Config::ReadSetting(const std::string& group, Settings::Setting<std::string>& setting) {
    std::string setting_value = sdl2_config->Get(group, setting.GetLabel(), setting.GetDefault());
    if (setting_value.empty()) {
        setting_value = setting.GetDefault();
    }
    setting = std::move(setting_value);
}

template <>
void Config::ReadSetting(const std::string& group, Settings::Setting<bool>& setting) {
    setting = sdl2_config->GetBoolean(group, setting.GetLabel(), setting.GetDefault());
}

template <typename Type, bool ranged>
void Config::ReadSetting(const std::string& group, Settings::Setting<Type, ranged>& setting) {
    setting = static_cast<Type>(sdl2_config->GetInteger(group, setting.GetLabel(),
                                                        static_cast<long>(setting.GetDefault())));
}

void Config::ReadValues() {
    // Controls
    for (std::size_t p = 0; p < Settings::values.players.GetValue().size(); ++p) {
        auto& player = Settings::values.players.GetValue()[p];

        const auto group = fmt::format("ControlsP{}", p);
        for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
            std::string default_param = InputCommon::GenerateKeyboardParam(default_buttons[i]);
            player.buttons[i] =
                sdl2_config->Get(group, Settings::NativeButton::mapping[i], default_param);
            if (player.buttons[i].empty()) {
                player.buttons[i] = default_param;
            }
        }

        for (int i = 0; i < Settings::NativeAnalog::NumAnalogs; ++i) {
            std::string default_param = InputCommon::GenerateAnalogParamFromKeys(
                default_analogs[i][0], default_analogs[i][1], default_analogs[i][2],
                default_analogs[i][3], default_analogs[i][4], 0.5f);
            player.analogs[i] =
                sdl2_config->Get(group, Settings::NativeAnalog::mapping[i], default_param);
            if (player.analogs[i].empty()) {
                player.analogs[i] = default_param;
            }
        }

        for (int i = 0; i < Settings::NativeMotion::NumMotions; ++i) {
            const std::string default_param =
                InputCommon::GenerateKeyboardParam(default_motions[i]);
            auto& player_motions = player.motions[i];

            player_motions =
                sdl2_config->Get(group, Settings::NativeMotion::mapping[i], default_param);
            if (player_motions.empty()) {
                player_motions = default_param;
            }
        }

        player.connected = sdl2_config->GetBoolean(group, "connected", false);
    }

    ReadSetting("ControlsGeneral", Settings::values.mouse_enabled);

    ReadSetting("ControlsGeneral", Settings::values.touch_device);

    ReadSetting("ControlsGeneral", Settings::values.keyboard_enabled);

    ReadSetting("ControlsGeneral", Settings::values.debug_pad_enabled);
    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        std::string default_param = InputCommon::GenerateKeyboardParam(default_buttons[i]);
        Settings::values.debug_pad_buttons[i] = sdl2_config->Get(
            "ControlsGeneral", std::string("debug_pad_") + Settings::NativeButton::mapping[i],
            default_param);
        if (Settings::values.debug_pad_buttons[i].empty())
            Settings::values.debug_pad_buttons[i] = default_param;
    }

    for (int i = 0; i < Settings::NativeAnalog::NumAnalogs; ++i) {
        std::string default_param = InputCommon::GenerateAnalogParamFromKeys(
            default_analogs[i][0], default_analogs[i][1], default_analogs[i][2],
            default_analogs[i][3], default_analogs[i][4], 0.5f);
        Settings::values.debug_pad_analogs[i] = sdl2_config->Get(
            "ControlsGeneral", std::string("debug_pad_") + Settings::NativeAnalog::mapping[i],
            default_param);
        if (Settings::values.debug_pad_analogs[i].empty())
            Settings::values.debug_pad_analogs[i] = default_param;
    }

    ReadSetting("ControlsGeneral", Settings::values.enable_raw_input);
    ReadSetting("ControlsGeneral", Settings::values.enable_joycon_driver);
    ReadSetting("ControlsGeneral", Settings::values.enable_procon_driver);
    ReadSetting("ControlsGeneral", Settings::values.random_amiibo_id);
    ReadSetting("ControlsGeneral", Settings::values.emulate_analog_keyboard);
    ReadSetting("ControlsGeneral", Settings::values.vibration_enabled);
    ReadSetting("ControlsGeneral", Settings::values.enable_accurate_vibrations);
    ReadSetting("ControlsGeneral", Settings::values.motion_enabled);
    Settings::values.touchscreen.enabled =
        sdl2_config->GetBoolean("ControlsGeneral", "touch_enabled", true);
    Settings::values.touchscreen.rotation_angle =
        sdl2_config->GetInteger("ControlsGeneral", "touch_angle", 0);
    Settings::values.touchscreen.diameter_x =
        sdl2_config->GetInteger("ControlsGeneral", "touch_diameter_x", 15);
    Settings::values.touchscreen.diameter_y =
        sdl2_config->GetInteger("ControlsGeneral", "touch_diameter_y", 15);

    int num_touch_from_button_maps =
        sdl2_config->GetInteger("ControlsGeneral", "touch_from_button_map", 0);
    if (num_touch_from_button_maps > 0) {
        for (int i = 0; i < num_touch_from_button_maps; ++i) {
            Settings::TouchFromButtonMap map;
            map.name = sdl2_config->Get("ControlsGeneral",
                                        std::string("touch_from_button_maps_") + std::to_string(i) +
                                            std::string("_name"),
                                        "default");
            const int num_touch_maps = sdl2_config->GetInteger(
                "ControlsGeneral",
                std::string("touch_from_button_maps_") + std::to_string(i) + std::string("_count"),
                0);
            map.buttons.reserve(num_touch_maps);

            for (int j = 0; j < num_touch_maps; ++j) {
                std::string touch_mapping =
                    sdl2_config->Get("ControlsGeneral",
                                     std::string("touch_from_button_maps_") + std::to_string(i) +
                                         std::string("_bind_") + std::to_string(j),
                                     "");
                map.buttons.emplace_back(std::move(touch_mapping));
            }

            Settings::values.touch_from_button_maps.emplace_back(std::move(map));
        }
    } else {
        Settings::values.touch_from_button_maps.emplace_back(
            Settings::TouchFromButtonMap{"default", {}});
        num_touch_from_button_maps = 1;
    }
    Settings::values.touch_from_button_map_index = std::clamp(
        Settings::values.touch_from_button_map_index.GetValue(), 0, num_touch_from_button_maps - 1);

    ReadSetting("ControlsGeneral", Settings::values.udp_input_servers);

    // Data Storage
    ReadSetting("Data Storage", Settings::values.use_virtual_sd);
    FS::SetYuzuPath(FS::YuzuPath::NANDDir,
                    sdl2_config->Get("Data Storage", "nand_directory",
                                     FS::GetYuzuPathString(FS::YuzuPath::NANDDir)));
    FS::SetYuzuPath(FS::YuzuPath::SDMCDir,
                    sdl2_config->Get("Data Storage", "sdmc_directory",
                                     FS::GetYuzuPathString(FS::YuzuPath::SDMCDir)));
    FS::SetYuzuPath(FS::YuzuPath::LoadDir,
                    sdl2_config->Get("Data Storage", "load_directory",
                                     FS::GetYuzuPathString(FS::YuzuPath::LoadDir)));
    FS::SetYuzuPath(FS::YuzuPath::DumpDir,
                    sdl2_config->Get("Data Storage", "dump_directory",
                                     FS::GetYuzuPathString(FS::YuzuPath::DumpDir)));
    ReadSetting("Data Storage", Settings::values.gamecard_inserted);
    ReadSetting("Data Storage", Settings::values.gamecard_current_game);
    ReadSetting("Data Storage", Settings::values.gamecard_path);

    // System
    ReadSetting("System", Settings::values.use_docked_mode);

    ReadSetting("System", Settings::values.current_user);
    Settings::values.current_user = std::clamp<int>(Settings::values.current_user.GetValue(), 0,
                                                    Service::Account::MAX_USERS - 1);

    const auto rng_seed_enabled = sdl2_config->GetBoolean("System", "rng_seed_enabled", false);
    if (rng_seed_enabled) {
        Settings::values.rng_seed.SetValue(sdl2_config->GetInteger("System", "rng_seed", 0));
    } else {
        Settings::values.rng_seed.SetValue(std::nullopt);
    }

    const auto custom_rtc_enabled = sdl2_config->GetBoolean("System", "custom_rtc_enabled", false);
    if (custom_rtc_enabled) {
        Settings::values.custom_rtc = sdl2_config->GetInteger("System", "custom_rtc", 0);
    } else {
        Settings::values.custom_rtc = std::nullopt;
    }

    ReadSetting("System", Settings::values.language_index);
    ReadSetting("System", Settings::values.region_index);
    ReadSetting("System", Settings::values.time_zone_index);
    ReadSetting("System", Settings::values.sound_index);

    // Core
    ReadSetting("Core", Settings::values.use_multi_core);
    ReadSetting("Core", Settings::values.use_unsafe_extended_memory_layout);

    // Cpu
    ReadSetting("Cpu", Settings::values.cpu_accuracy);
    ReadSetting("Cpu", Settings::values.cpu_debug_mode);
    ReadSetting("Cpu", Settings::values.cpuopt_page_tables);
    ReadSetting("Cpu", Settings::values.cpuopt_block_linking);
    ReadSetting("Cpu", Settings::values.cpuopt_return_stack_buffer);
    ReadSetting("Cpu", Settings::values.cpuopt_fast_dispatcher);
    ReadSetting("Cpu", Settings::values.cpuopt_context_elimination);
    ReadSetting("Cpu", Settings::values.cpuopt_const_prop);
    ReadSetting("Cpu", Settings::values.cpuopt_misc_ir);
    ReadSetting("Cpu", Settings::values.cpuopt_reduce_misalign_checks);
    ReadSetting("Cpu", Settings::values.cpuopt_fastmem);
    ReadSetting("Cpu", Settings::values.cpuopt_fastmem_exclusives);
    ReadSetting("Cpu", Settings::values.cpuopt_recompile_exclusives);
    ReadSetting("Cpu", Settings::values.cpuopt_ignore_memory_aborts);
    ReadSetting("Cpu", Settings::values.cpuopt_unsafe_unfuse_fma);
    ReadSetting("Cpu", Settings::values.cpuopt_unsafe_reduce_fp_error);
    ReadSetting("Cpu", Settings::values.cpuopt_unsafe_ignore_standard_fpcr);
    ReadSetting("Cpu", Settings::values.cpuopt_unsafe_inaccurate_nan);
    ReadSetting("Cpu", Settings::values.cpuopt_unsafe_fastmem_check);
    ReadSetting("Cpu", Settings::values.cpuopt_unsafe_ignore_global_monitor);

    // Renderer
    ReadSetting("Renderer", Settings::values.renderer_backend);
    ReadSetting("Renderer", Settings::values.async_presentation);
    ReadSetting("Renderer", Settings::values.renderer_force_max_clock);
    ReadSetting("Renderer", Settings::values.renderer_debug);
    ReadSetting("Renderer", Settings::values.renderer_shader_feedback);
    ReadSetting("Renderer", Settings::values.enable_nsight_aftermath);
    ReadSetting("Renderer", Settings::values.disable_shader_loop_safety_checks);
    ReadSetting("Renderer", Settings::values.vulkan_device);

    ReadSetting("Renderer", Settings::values.resolution_setup);
    ReadSetting("Renderer", Settings::values.scaling_filter);
    ReadSetting("Renderer", Settings::values.fsr_sharpening_slider);
    ReadSetting("Renderer", Settings::values.anti_aliasing);
    ReadSetting("Renderer", Settings::values.fullscreen_mode);
    ReadSetting("Renderer", Settings::values.aspect_ratio);
    ReadSetting("Renderer", Settings::values.max_anisotropy);
    ReadSetting("Renderer", Settings::values.use_speed_limit);
    ReadSetting("Renderer", Settings::values.speed_limit);
    ReadSetting("Renderer", Settings::values.use_disk_shader_cache);
    ReadSetting("Renderer", Settings::values.gpu_accuracy);
    ReadSetting("Renderer", Settings::values.use_asynchronous_gpu_emulation);
    ReadSetting("Renderer", Settings::values.vsync_mode);
    ReadSetting("Renderer", Settings::values.shader_backend);
    ReadSetting("Renderer", Settings::values.use_reactive_flushing);
    ReadSetting("Renderer", Settings::values.use_asynchronous_shaders);
    ReadSetting("Renderer", Settings::values.nvdec_emulation);
    ReadSetting("Renderer", Settings::values.accelerate_astc);
    ReadSetting("Renderer", Settings::values.async_astc);
    ReadSetting("Renderer", Settings::values.use_fast_gpu_time);
    ReadSetting("Renderer", Settings::values.use_vulkan_driver_pipeline_cache);

    ReadSetting("Renderer", Settings::values.bg_red);
    ReadSetting("Renderer", Settings::values.bg_green);
    ReadSetting("Renderer", Settings::values.bg_blue);

    // Audio
    ReadSetting("Audio", Settings::values.sink_id);
    ReadSetting("Audio", Settings::values.audio_output_device_id);
    ReadSetting("Audio", Settings::values.volume);

    // Miscellaneous
    // log_filter has a different default here than from common
    Settings::values.log_filter =
        sdl2_config->Get("Miscellaneous", Settings::values.log_filter.GetLabel(), "*:Trace");
    ReadSetting("Miscellaneous", Settings::values.use_dev_keys);

    // Debugging
    Settings::values.record_frame_times =
        sdl2_config->GetBoolean("Debugging", "record_frame_times", false);
    ReadSetting("Debugging", Settings::values.dump_exefs);
    ReadSetting("Debugging", Settings::values.dump_nso);
    ReadSetting("Debugging", Settings::values.enable_fs_access_log);
    ReadSetting("Debugging", Settings::values.reporting_services);
    ReadSetting("Debugging", Settings::values.quest_flag);
    ReadSetting("Debugging", Settings::values.use_debug_asserts);
    ReadSetting("Debugging", Settings::values.use_auto_stub);
    ReadSetting("Debugging", Settings::values.disable_macro_jit);
    ReadSetting("Debugging", Settings::values.disable_macro_hle);
    ReadSetting("Debugging", Settings::values.use_gdbstub);
    ReadSetting("Debugging", Settings::values.gdbstub_port);

    const auto title_list = sdl2_config->Get("AddOns", "title_ids", "");
    std::stringstream ss(title_list);
    std::string line;
    while (std::getline(ss, line, '|')) {
        const auto title_id = std::stoul(line, nullptr, 16);
        const auto disabled_list = sdl2_config->Get("AddOns", "disabled_" + line, "");

        std::stringstream inner_ss(disabled_list);
        std::string inner_line;
        std::vector<std::string> out;
        while (std::getline(inner_ss, inner_line, '|')) {
            out.push_back(inner_line);
        }

        Settings::values.disabled_addons.insert_or_assign(title_id, out);
    }

    // Web Service
    ReadSetting("WebService", Settings::values.enable_telemetry);
    ReadSetting("WebService", Settings::values.web_api_url);
    ReadSetting("WebService", Settings::values.yuzu_username);
    ReadSetting("WebService", Settings::values.yuzu_token);

    // Network
    ReadSetting("Network", Settings::values.network_interface);
}

void Config::Reload() {
    LoadINI(DefaultINI::sdl2_config_file);
    ReadValues();
}
