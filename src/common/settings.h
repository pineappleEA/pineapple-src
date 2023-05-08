// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/settings_input.h"

namespace Settings {

enum class VSyncMode : u32 {
    Immediate = 0,
    Mailbox = 1,
    FIFO = 2,
    FIFORelaxed = 3,
};

enum class RendererBackend : u32 {
    OpenGL = 0,
    Vulkan = 1,
    Null = 2,
};

enum class ShaderBackend : u32 {
    GLSL = 0,
    GLASM = 1,
    SPIRV = 2,
};

enum class GPUAccuracy : u32 {
    Normal = 0,
    High = 1,
    Extreme = 2,
};

enum class CPUAccuracy : u32 {
    Auto = 0,
    Accurate = 1,
    Unsafe = 2,
    Paranoid = 3,
};

enum class FullscreenMode : u32 {
    Borderless = 0,
    Exclusive = 1,
};

enum class NvdecEmulation : u32 {
    Off = 0,
    CPU = 1,
    GPU = 2,
};

enum class ResolutionSetup : u32 {
    Res1_2X = 0,
    Res3_4X = 1,
    Res1X = 2,
    Res3_2X = 3,
    Res2X = 4,
    Res3X = 5,
    Res4X = 6,
    Res5X = 7,
    Res6X = 8,
    Res7X = 9,
    Res8X = 10,
};

enum class ScalingFilter : u32 {
    NearestNeighbor = 0,
    Bilinear = 1,
    Bicubic = 2,
    Gaussian = 3,
    ScaleForce = 4,
    Fsr = 5,
    LastFilter = Fsr,
};

enum class AntiAliasing : u32 {
    None = 0,
    Fxaa = 1,
    Smaa = 2,
    LastAA = Smaa,
};

struct ResolutionScalingInfo {
    u32 up_scale{1};
    u32 down_shift{0};
    f32 up_factor{1.0f};
    f32 down_factor{1.0f};
    bool active{};
    bool downscale{};

    s32 ScaleUp(s32 value) const {
        if (value == 0) {
            return 0;
        }
        return std::max((value * static_cast<s32>(up_scale)) >> static_cast<s32>(down_shift), 1);
    }

    u32 ScaleUp(u32 value) const {
        if (value == 0U) {
            return 0U;
        }
        return std::max((value * up_scale) >> down_shift, 1U);
    }
};

/** The Setting class is a simple resource manager. It defines a label and default value alongside
 * the actual value of the setting for simpler and less-error prone use with frontend
 * configurations. Specifying a default value and label is required. A minimum and maximum range can
 * be specified for sanitization.
 */
template <typename Type, bool ranged = false>
class Setting {
protected:
    Setting() = default;

    /**
     * Only sets the setting to the given initializer, leaving the other members to their default
     * initializers.
     *
     * @param global_val Initial value of the setting
     */
    explicit Setting(const Type& val) : value{val} {}

public:
    /**
     * Sets a default value, label, and setting value.
     *
     * @param default_val Initial value of the setting, and default value of the setting
     * @param name Label for the setting
     */
    explicit Setting(const Type& default_val, const std::string& name)
        requires(!ranged)
        : value{default_val}, default_value{default_val}, label{name} {}
    virtual ~Setting() = default;

    /**
     * Sets a default value, minimum value, maximum value, and label.
     *
     * @param default_val Initial value of the setting, and default value of the setting
     * @param min_val Sets the minimum allowed value of the setting
     * @param max_val Sets the maximum allowed value of the setting
     * @param name Label for the setting
     */
    explicit Setting(const Type& default_val, const Type& min_val, const Type& max_val,
                     const std::string& name)
        requires(ranged)
        : value{default_val},
          default_value{default_val}, maximum{max_val}, minimum{min_val}, label{name} {}

    /**
     *  Returns a reference to the setting's value.
     *
     * @returns A reference to the setting
     */
    [[nodiscard]] virtual const Type& GetValue() const {
        return value;
    }

    /**
     * Sets the setting to the given value.
     *
     * @param val The desired value
     */
    virtual void SetValue(const Type& val) {
        Type temp{ranged ? std::clamp(val, minimum, maximum) : val};
        std::swap(value, temp);
    }

    /**
     * Returns the value that this setting was created with.
     *
     * @returns A reference to the default value
     */
    [[nodiscard]] const Type& GetDefault() const {
        return default_value;
    }

    /**
     * Returns the label this setting was created with.
     *
     * @returns A reference to the label
     */
    [[nodiscard]] const std::string& GetLabel() const {
        return label;
    }

    /**
     * Assigns a value to the setting.
     *
     * @param val The desired setting value
     *
     * @returns A reference to the setting
     */
    virtual const Type& operator=(const Type& val) {
        Type temp{ranged ? std::clamp(val, minimum, maximum) : val};
        std::swap(value, temp);
        return value;
    }

    /**
     * Returns a reference to the setting.
     *
     * @returns A reference to the setting
     */
    explicit virtual operator const Type&() const {
        return value;
    }

protected:
    Type value{};               ///< The setting
    const Type default_value{}; ///< The default value
    const Type maximum{};       ///< Maximum allowed value of the setting
    const Type minimum{};       ///< Minimum allowed value of the setting
    const std::string label{};  ///< The setting's label
};

/**
 * The SwitchableSetting class is a slightly more complex version of the Setting class. This adds a
 * custom setting to switch to when a guest application specifically requires it. The effect is that
 * other components of the emulator can access the setting's intended value without any need for the
 * component to ask whether the custom or global setting is needed at the moment.
 *
 * By default, the global setting is used.
 */
template <typename Type, bool ranged = false>
class SwitchableSetting : virtual public Setting<Type, ranged> {
public:
    /**
     * Sets a default value, label, and setting value.
     *
     * @param default_val Initial value of the setting, and default value of the setting
     * @param name Label for the setting
     */
    explicit SwitchableSetting(const Type& default_val, const std::string& name)
        requires(!ranged)
        : Setting<Type>{default_val, name} {}
    virtual ~SwitchableSetting() = default;

    /**
     * Sets a default value, minimum value, maximum value, and label.
     *
     * @param default_val Initial value of the setting, and default value of the setting
     * @param min_val Sets the minimum allowed value of the setting
     * @param max_val Sets the maximum allowed value of the setting
     * @param name Label for the setting
     */
    explicit SwitchableSetting(const Type& default_val, const Type& min_val, const Type& max_val,
                               const std::string& name)
        requires(ranged)
        : Setting<Type, true>{default_val, min_val, max_val, name} {}

    /**
     * Tells this setting to represent either the global or custom setting when other member
     * functions are used.
     *
     * @param to_global Whether to use the global or custom setting.
     */
    void SetGlobal(bool to_global) {
        use_global = to_global;
    }

    /**
     * Returns whether this setting is using the global setting or not.
     *
     * @returns The global state
     */
    [[nodiscard]] bool UsingGlobal() const {
        return use_global;
    }

    /**
     * Returns either the global or custom setting depending on the values of this setting's global
     * state or if the global value was specifically requested.
     *
     * @param need_global Request global value regardless of setting's state; defaults to false
     *
     * @returns The required value of the setting
     */
    [[nodiscard]] virtual const Type& GetValue() const override {
        if (use_global) {
            return this->value;
        }
        return custom;
    }
    [[nodiscard]] virtual const Type& GetValue(bool need_global) const {
        if (use_global || need_global) {
            return this->value;
        }
        return custom;
    }

    /**
     * Sets the current setting value depending on the global state.
     *
     * @param val The new value
     */
    void SetValue(const Type& val) override {
        Type temp{ranged ? std::clamp(val, this->minimum, this->maximum) : val};
        if (use_global) {
            std::swap(this->value, temp);
        } else {
            std::swap(custom, temp);
        }
    }

    /**
     * Assigns the current setting value depending on the global state.
     *
     * @param val The new value
     *
     * @returns A reference to the current setting value
     */
    const Type& operator=(const Type& val) override {
        Type temp{ranged ? std::clamp(val, this->minimum, this->maximum) : val};
        if (use_global) {
            std::swap(this->value, temp);
            return this->value;
        }
        std::swap(custom, temp);
        return custom;
    }

    /**
     * Returns the current setting value depending on the global state.
     *
     * @returns A reference to the current setting value
     */
    virtual explicit operator const Type&() const override {
        if (use_global) {
            return this->value;
        }
        return custom;
    }

protected:
    bool use_global{true}; ///< The setting's global state
    Type custom{};         ///< The custom value of the setting
};

/**
 * The InputSetting class allows for getting a reference to either the global or custom members.
 * This is required as we cannot easily modify the values of user-defined types within containers
 * using the SetValue() member function found in the Setting class. The primary purpose of this
 * class is to store an array of 10 PlayerInput structs for both the global and custom setting and
 * allows for easily accessing and modifying both settings.
 */
template <typename Type>
class InputSetting final {
public:
    InputSetting() = default;
    explicit InputSetting(Type val) : Setting<Type>(val) {}
    ~InputSetting() = default;
    void SetGlobal(bool to_global) {
        use_global = to_global;
    }
    [[nodiscard]] bool UsingGlobal() const {
        return use_global;
    }
    [[nodiscard]] Type& GetValue(bool need_global = false) {
        if (use_global || need_global) {
            return global;
        }
        return custom;
    }

private:
    bool use_global{true}; ///< The setting's global state
    Type global{};         ///< The setting
    Type custom{};         ///< The custom setting value
};

struct TouchFromButtonMap {
    std::string name;
    std::vector<std::string> buttons;
};

struct Values {
    // Audio
    Setting<std::string> sink_id{"auto", "output_engine"};
    Setting<std::string> audio_output_device_id{"auto", "output_device"};
    Setting<std::string> audio_input_device_id{"auto", "input_device"};
    Setting<bool> audio_muted{false, "audio_muted"};
    SwitchableSetting<u8, true> volume{100, 0, 200, "volume"};
    Setting<bool> dump_audio_commands{false, "dump_audio_commands"};

    // Core
    SwitchableSetting<bool> use_multi_core{true, "use_multi_core"};
    SwitchableSetting<bool> use_unsafe_extended_memory_layout{false,
                                                              "use_unsafe_extended_memory_layout"};

    // Cpu
    SwitchableSetting<CPUAccuracy, true> cpu_accuracy{CPUAccuracy::Auto, CPUAccuracy::Auto,
                                                      CPUAccuracy::Paranoid, "cpu_accuracy"};
    // TODO: remove cpu_accuracy_first_time, migration setting added 8 July 2021
    Setting<bool> cpu_accuracy_first_time{true, "cpu_accuracy_first_time"};
    Setting<bool> cpu_debug_mode{false, "cpu_debug_mode"};

    Setting<bool> cpuopt_page_tables{true, "cpuopt_page_tables"};
    Setting<bool> cpuopt_block_linking{true, "cpuopt_block_linking"};
    Setting<bool> cpuopt_return_stack_buffer{true, "cpuopt_return_stack_buffer"};
    Setting<bool> cpuopt_fast_dispatcher{true, "cpuopt_fast_dispatcher"};
    Setting<bool> cpuopt_context_elimination{true, "cpuopt_context_elimination"};
    Setting<bool> cpuopt_const_prop{true, "cpuopt_const_prop"};
    Setting<bool> cpuopt_misc_ir{true, "cpuopt_misc_ir"};
    Setting<bool> cpuopt_reduce_misalign_checks{true, "cpuopt_reduce_misalign_checks"};
    Setting<bool> cpuopt_fastmem{true, "cpuopt_fastmem"};
    Setting<bool> cpuopt_fastmem_exclusives{true, "cpuopt_fastmem_exclusives"};
    Setting<bool> cpuopt_recompile_exclusives{true, "cpuopt_recompile_exclusives"};
    Setting<bool> cpuopt_ignore_memory_aborts{true, "cpuopt_ignore_memory_aborts"};

    SwitchableSetting<bool> cpuopt_unsafe_unfuse_fma{true, "cpuopt_unsafe_unfuse_fma"};
    SwitchableSetting<bool> cpuopt_unsafe_reduce_fp_error{true, "cpuopt_unsafe_reduce_fp_error"};
    SwitchableSetting<bool> cpuopt_unsafe_ignore_standard_fpcr{
        true, "cpuopt_unsafe_ignore_standard_fpcr"};
    SwitchableSetting<bool> cpuopt_unsafe_inaccurate_nan{true, "cpuopt_unsafe_inaccurate_nan"};
    SwitchableSetting<bool> cpuopt_unsafe_fastmem_check{true, "cpuopt_unsafe_fastmem_check"};
    SwitchableSetting<bool> cpuopt_unsafe_ignore_global_monitor{
        true, "cpuopt_unsafe_ignore_global_monitor"};

    // Renderer
    SwitchableSetting<RendererBackend, true> renderer_backend{
        RendererBackend::Vulkan, RendererBackend::OpenGL, RendererBackend::Null, "backend"};
    SwitchableSetting<bool> async_presentation{false, "async_presentation"};
    SwitchableSetting<bool> renderer_force_max_clock{false, "force_max_clock"};
    Setting<bool> renderer_debug{false, "debug"};
    Setting<bool> renderer_shader_feedback{false, "shader_feedback"};
    Setting<bool> enable_nsight_aftermath{false, "nsight_aftermath"};
    Setting<bool> disable_shader_loop_safety_checks{false, "disable_shader_loop_safety_checks"};
    SwitchableSetting<int> vulkan_device{0, "vulkan_device"};

    ResolutionScalingInfo resolution_info{};
    SwitchableSetting<ResolutionSetup> resolution_setup{ResolutionSetup::Res1X, "resolution_setup"};
    SwitchableSetting<ScalingFilter> scaling_filter{ScalingFilter::Bilinear, "scaling_filter"};
    SwitchableSetting<int, true> fsr_sharpening_slider{25, 0, 200, "fsr_sharpening_slider"};
    SwitchableSetting<AntiAliasing> anti_aliasing{AntiAliasing::None, "anti_aliasing"};
    // *nix platforms may have issues with the borderless windowed fullscreen mode.
    // Default to exclusive fullscreen on these platforms for now.
    SwitchableSetting<FullscreenMode, true> fullscreen_mode{
#ifdef _WIN32
        FullscreenMode::Borderless,
#else
        FullscreenMode::Exclusive,
#endif
        FullscreenMode::Borderless, FullscreenMode::Exclusive, "fullscreen_mode"};
    SwitchableSetting<int, true> aspect_ratio{0, 0, 4, "aspect_ratio"};
    SwitchableSetting<int, true> max_anisotropy{0, 0, 5, "max_anisotropy"};
    SwitchableSetting<bool> use_speed_limit{true, "use_speed_limit"};
    SwitchableSetting<u16, true> speed_limit{100, 0, 9999, "speed_limit"};
    SwitchableSetting<bool> use_disk_shader_cache{true, "use_disk_shader_cache"};
    SwitchableSetting<GPUAccuracy, true> gpu_accuracy{GPUAccuracy::High, GPUAccuracy::Normal,
                                                      GPUAccuracy::Extreme, "gpu_accuracy"};
    SwitchableSetting<bool> use_asynchronous_gpu_emulation{true, "use_asynchronous_gpu_emulation"};
    SwitchableSetting<NvdecEmulation> nvdec_emulation{NvdecEmulation::GPU, "nvdec_emulation"};
    SwitchableSetting<bool> accelerate_astc{true, "accelerate_astc"};
    SwitchableSetting<bool> async_astc{false, "async_astc"};
    Setting<VSyncMode, true> vsync_mode{VSyncMode::FIFO, VSyncMode::Immediate,
                                        VSyncMode::FIFORelaxed, "use_vsync"};
    SwitchableSetting<bool> use_reactive_flushing{true, "use_reactive_flushing"};
    SwitchableSetting<ShaderBackend, true> shader_backend{ShaderBackend::GLSL, ShaderBackend::GLSL,
                                                          ShaderBackend::SPIRV, "shader_backend"};
    SwitchableSetting<bool> use_asynchronous_shaders{false, "use_asynchronous_shaders"};
    SwitchableSetting<bool> use_fast_gpu_time{true, "use_fast_gpu_time"};
    SwitchableSetting<bool> use_vulkan_driver_pipeline_cache{true,
                                                             "use_vulkan_driver_pipeline_cache"};
    SwitchableSetting<bool> enable_compute_pipelines{false, "enable_compute_pipelines"};

    SwitchableSetting<u8> bg_red{0, "bg_red"};
    SwitchableSetting<u8> bg_green{0, "bg_green"};
    SwitchableSetting<u8> bg_blue{0, "bg_blue"};

    // System
    SwitchableSetting<std::optional<u32>> rng_seed{std::optional<u32>(), "rng_seed"};
    Setting<std::string> device_name{"Yuzu", "device_name"};
    // Measured in seconds since epoch
    std::optional<s64> custom_rtc;
    // Set on game boot, reset on stop. Seconds difference between current time and `custom_rtc`
    s64 custom_rtc_differential;

    Setting<s32> current_user{0, "current_user"};
    SwitchableSetting<s32, true> language_index{1, 0, 17, "language_index"};
    SwitchableSetting<s32, true> region_index{1, 0, 6, "region_index"};
    SwitchableSetting<s32, true> time_zone_index{0, 0, 45, "time_zone_index"};
    SwitchableSetting<s32, true> sound_index{1, 0, 2, "sound_index"};

    // Controls
    InputSetting<std::array<PlayerInput, 10>> players;

    SwitchableSetting<bool> use_docked_mode{true, "use_docked_mode"};

    Setting<bool> enable_raw_input{false, "enable_raw_input"};
    Setting<bool> controller_navigation{true, "controller_navigation"};
    Setting<bool> enable_joycon_driver{true, "enable_joycon_driver"};
    Setting<bool> enable_procon_driver{false, "enable_procon_driver"};

    SwitchableSetting<bool> vibration_enabled{true, "vibration_enabled"};
    SwitchableSetting<bool> enable_accurate_vibrations{false, "enable_accurate_vibrations"};

    SwitchableSetting<bool> motion_enabled{true, "motion_enabled"};
    Setting<std::string> udp_input_servers{"127.0.0.1:26760", "udp_input_servers"};
    Setting<bool> enable_udp_controller{false, "enable_udp_controller"};

    Setting<bool> pause_tas_on_load{true, "pause_tas_on_load"};
    Setting<bool> tas_enable{false, "tas_enable"};
    Setting<bool> tas_loop{false, "tas_loop"};

    Setting<bool> mouse_panning{false, "mouse_panning"};
    Setting<u8, true> mouse_panning_sensitivity{50, 1, 100, "mouse_panning_sensitivity"};
    Setting<bool> mouse_enabled{false, "mouse_enabled"};

    Setting<bool> emulate_analog_keyboard{false, "emulate_analog_keyboard"};
    Setting<bool> keyboard_enabled{false, "keyboard_enabled"};

    Setting<bool> debug_pad_enabled{false, "debug_pad_enabled"};
    ButtonsRaw debug_pad_buttons;
    AnalogsRaw debug_pad_analogs;

    TouchscreenInput touchscreen;

    Setting<std::string> touch_device{"min_x:100,min_y:50,max_x:1800,max_y:850", "touch_device"};
    Setting<int> touch_from_button_map_index{0, "touch_from_button_map"};
    std::vector<TouchFromButtonMap> touch_from_button_maps;

    Setting<bool> enable_ring_controller{true, "enable_ring_controller"};
    RingconRaw ringcon_analogs;

    Setting<bool> enable_ir_sensor{false, "enable_ir_sensor"};
    Setting<std::string> ir_sensor_device{"auto", "ir_sensor_device"};

    // Data Storage
    Setting<bool> use_virtual_sd{true, "use_virtual_sd"};
    Setting<bool> gamecard_inserted{false, "gamecard_inserted"};
    Setting<bool> gamecard_current_game{false, "gamecard_current_game"};
    Setting<std::string> gamecard_path{std::string(), "gamecard_path"};

    // Debugging
    bool record_frame_times;
    Setting<bool> use_gdbstub{false, "use_gdbstub"};
    Setting<u16> gdbstub_port{6543, "gdbstub_port"};
    Setting<std::string> program_args{std::string(), "program_args"};
    Setting<bool> dump_exefs{false, "dump_exefs"};
    Setting<bool> dump_nso{false, "dump_nso"};
    Setting<bool> dump_shaders{false, "dump_shaders"};
    Setting<bool> dump_macros{false, "dump_macros"};
    Setting<bool> enable_fs_access_log{false, "enable_fs_access_log"};
    Setting<bool> reporting_services{false, "reporting_services"};
    Setting<bool> quest_flag{false, "quest_flag"};
    Setting<bool> disable_macro_jit{false, "disable_macro_jit"};
    Setting<bool> disable_macro_hle{false, "disable_macro_hle"};
    Setting<bool> extended_logging{false, "extended_logging"};
    Setting<bool> use_debug_asserts{false, "use_debug_asserts"};
    Setting<bool> use_auto_stub{false, "use_auto_stub"};
    Setting<bool> enable_all_controllers{false, "enable_all_controllers"};
    Setting<bool> create_crash_dumps{false, "create_crash_dumps"};
    Setting<bool> perform_vulkan_check{true, "perform_vulkan_check"};

    // Miscellaneous
    Setting<std::string> log_filter{"*:Info", "log_filter"};
    Setting<bool> use_dev_keys{false, "use_dev_keys"};

    // Network
    Setting<std::string> network_interface{std::string(), "network_interface"};

    // WebService
    Setting<bool> enable_telemetry{true, "enable_telemetry"};
    Setting<std::string> web_api_url{"https://api.yuzu-emu.org", "web_api_url"};
    Setting<std::string> yuzu_username{std::string(), "yuzu_username"};
    Setting<std::string> yuzu_token{std::string(), "yuzu_token"};

    // Add-Ons
    std::map<u64, std::vector<std::string>> disabled_addons;
};

extern Values values;

bool IsConfiguringGlobal();
void SetConfiguringGlobal(bool is_global);

bool IsGPULevelExtreme();
bool IsGPULevelHigh();

bool IsFastmemEnabled();

float Volume();

std::string GetTimeZoneString();

void LogSettings();

void UpdateRescalingInfo();

// Restore the global state of all applicable settings in the Values struct
void RestoreGlobalState(bool is_powered_on);

} // namespace Settings
