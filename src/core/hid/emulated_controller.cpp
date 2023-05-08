// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <common/scope_exit.h>

#include "common/polyfill_ranges.h"
#include "common/thread.h"
#include "core/hid/emulated_controller.h"
#include "core/hid/input_converter.h"

namespace Core::HID {
constexpr s32 HID_JOYSTICK_MAX = 0x7fff;
constexpr s32 HID_TRIGGER_MAX = 0x7fff;
constexpr u32 TURBO_BUTTON_DELAY = 4;
// Use a common UUID for TAS and Virtual Gamepad
constexpr Common::UUID TAS_UUID =
    Common::UUID{{0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x7, 0xA5, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0}};
constexpr Common::UUID VIRTUAL_UUID =
    Common::UUID{{0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x7, 0xFF, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0}};

EmulatedController::EmulatedController(NpadIdType npad_id_type_) : npad_id_type(npad_id_type_) {}

EmulatedController::~EmulatedController() = default;

NpadStyleIndex EmulatedController::MapSettingsTypeToNPad(Settings::ControllerType type) {
    switch (type) {
    case Settings::ControllerType::ProController:
        return NpadStyleIndex::ProController;
    case Settings::ControllerType::DualJoyconDetached:
        return NpadStyleIndex::JoyconDual;
    case Settings::ControllerType::LeftJoycon:
        return NpadStyleIndex::JoyconLeft;
    case Settings::ControllerType::RightJoycon:
        return NpadStyleIndex::JoyconRight;
    case Settings::ControllerType::Handheld:
        return NpadStyleIndex::Handheld;
    case Settings::ControllerType::GameCube:
        return NpadStyleIndex::GameCube;
    case Settings::ControllerType::Pokeball:
        return NpadStyleIndex::Pokeball;
    case Settings::ControllerType::NES:
        return NpadStyleIndex::NES;
    case Settings::ControllerType::SNES:
        return NpadStyleIndex::SNES;
    case Settings::ControllerType::N64:
        return NpadStyleIndex::N64;
    case Settings::ControllerType::SegaGenesis:
        return NpadStyleIndex::SegaGenesis;
    default:
        return NpadStyleIndex::ProController;
    }
}

Settings::ControllerType EmulatedController::MapNPadToSettingsType(NpadStyleIndex type) {
    switch (type) {
    case NpadStyleIndex::ProController:
        return Settings::ControllerType::ProController;
    case NpadStyleIndex::JoyconDual:
        return Settings::ControllerType::DualJoyconDetached;
    case NpadStyleIndex::JoyconLeft:
        return Settings::ControllerType::LeftJoycon;
    case NpadStyleIndex::JoyconRight:
        return Settings::ControllerType::RightJoycon;
    case NpadStyleIndex::Handheld:
        return Settings::ControllerType::Handheld;
    case NpadStyleIndex::GameCube:
        return Settings::ControllerType::GameCube;
    case NpadStyleIndex::Pokeball:
        return Settings::ControllerType::Pokeball;
    case NpadStyleIndex::NES:
        return Settings::ControllerType::NES;
    case NpadStyleIndex::SNES:
        return Settings::ControllerType::SNES;
    case NpadStyleIndex::N64:
        return Settings::ControllerType::N64;
    case NpadStyleIndex::SegaGenesis:
        return Settings::ControllerType::SegaGenesis;
    default:
        return Settings::ControllerType::ProController;
    }
}

void EmulatedController::ReloadFromSettings() {
    const auto player_index = NpadIdTypeToIndex(npad_id_type);
    const auto& player = Settings::values.players.GetValue()[player_index];

    for (std::size_t index = 0; index < player.buttons.size(); ++index) {
        button_params[index] = Common::ParamPackage(player.buttons[index]);
    }
    for (std::size_t index = 0; index < player.analogs.size(); ++index) {
        stick_params[index] = Common::ParamPackage(player.analogs[index]);
    }
    for (std::size_t index = 0; index < player.motions.size(); ++index) {
        motion_params[index] = Common::ParamPackage(player.motions[index]);
    }

    controller.color_values = {};
    controller.colors_state.fullkey = {
        .body = GetNpadColor(player.body_color_left),
        .button = GetNpadColor(player.button_color_left),
    };
    controller.colors_state.left = {
        .body = GetNpadColor(player.body_color_left),
        .button = GetNpadColor(player.button_color_left),
    };
    controller.colors_state.right = {
        .body = GetNpadColor(player.body_color_right),
        .button = GetNpadColor(player.button_color_right),
    };

    ring_params[0] = Common::ParamPackage(Settings::values.ringcon_analogs);

    // Other or debug controller should always be a pro controller
    if (npad_id_type != NpadIdType::Other) {
        SetNpadStyleIndex(MapSettingsTypeToNPad(player.controller_type));
        original_npad_type = npad_type;
    } else {
        SetNpadStyleIndex(NpadStyleIndex::ProController);
        original_npad_type = npad_type;
    }

    Disconnect();
    if (player.connected) {
        Connect();
    }

    ReloadInput();
}

void EmulatedController::LoadDevices() {
    // TODO(german77): Use more buttons to detect the correct device
    const auto left_joycon = button_params[Settings::NativeButton::DRight];
    const auto right_joycon = button_params[Settings::NativeButton::A];

    // Triggers for GC controllers
    trigger_params[LeftIndex] = button_params[Settings::NativeButton::ZL];
    trigger_params[RightIndex] = button_params[Settings::NativeButton::ZR];

    color_params[LeftIndex] = left_joycon;
    color_params[RightIndex] = right_joycon;
    color_params[LeftIndex].Set("color", true);
    color_params[RightIndex].Set("color", true);

    battery_params[LeftIndex] = left_joycon;
    battery_params[RightIndex] = right_joycon;
    battery_params[LeftIndex].Set("battery", true);
    battery_params[RightIndex].Set("battery", true);

    camera_params[0] = right_joycon;
    camera_params[0].Set("camera", true);
    camera_params[1] = Common::ParamPackage{"engine:camera,camera:1"};
    ring_params[1] = Common::ParamPackage{"engine:joycon,axis_x:100,axis_y:101"};
    nfc_params[0] = Common::ParamPackage{"engine:virtual_amiibo,nfc:1"};
    nfc_params[1] = right_joycon;
    nfc_params[1].Set("nfc", true);

    output_params[LeftIndex] = left_joycon;
    output_params[RightIndex] = right_joycon;
    output_params[2] = camera_params[1];
    output_params[3] = nfc_params[0];
    output_params[LeftIndex].Set("output", true);
    output_params[RightIndex].Set("output", true);
    output_params[2].Set("output", true);
    output_params[3].Set("output", true);

    LoadTASParams();
    LoadVirtualGamepadParams();

    std::ranges::transform(button_params, button_devices.begin(), Common::Input::CreateInputDevice);
    std::ranges::transform(stick_params, stick_devices.begin(), Common::Input::CreateInputDevice);
    std::ranges::transform(motion_params, motion_devices.begin(), Common::Input::CreateInputDevice);
    std::ranges::transform(trigger_params, trigger_devices.begin(),
                           Common::Input::CreateInputDevice);
    std::ranges::transform(battery_params, battery_devices.begin(),
                           Common::Input::CreateInputDevice);
    std::ranges::transform(color_params, color_devices.begin(), Common::Input::CreateInputDevice);
    std::ranges::transform(camera_params, camera_devices.begin(), Common::Input::CreateInputDevice);
    std::ranges::transform(ring_params, ring_analog_devices.begin(),
                           Common::Input::CreateInputDevice);
    std::ranges::transform(nfc_params, nfc_devices.begin(), Common::Input::CreateInputDevice);
    std::ranges::transform(output_params, output_devices.begin(),
                           Common::Input::CreateOutputDevice);

    // Initialize TAS devices
    std::ranges::transform(tas_button_params, tas_button_devices.begin(),
                           Common::Input::CreateInputDevice);
    std::ranges::transform(tas_stick_params, tas_stick_devices.begin(),
                           Common::Input::CreateInputDevice);

    // Initialize virtual gamepad devices
    std::ranges::transform(virtual_button_params, virtual_button_devices.begin(),
                           Common::Input::CreateInputDevice);
    std::ranges::transform(virtual_stick_params, virtual_stick_devices.begin(),
                           Common::Input::CreateInputDevice);
}

void EmulatedController::LoadTASParams() {
    const auto player_index = NpadIdTypeToIndex(npad_id_type);
    Common::ParamPackage common_params{};
    common_params.Set("engine", "tas");
    common_params.Set("port", static_cast<int>(player_index));
    for (auto& param : tas_button_params) {
        param = common_params;
    }
    for (auto& param : tas_stick_params) {
        param = common_params;
    }

    // TODO(german77): Replace this with an input profile or something better
    tas_button_params[Settings::NativeButton::A].Set("button", 0);
    tas_button_params[Settings::NativeButton::B].Set("button", 1);
    tas_button_params[Settings::NativeButton::X].Set("button", 2);
    tas_button_params[Settings::NativeButton::Y].Set("button", 3);
    tas_button_params[Settings::NativeButton::LStick].Set("button", 4);
    tas_button_params[Settings::NativeButton::RStick].Set("button", 5);
    tas_button_params[Settings::NativeButton::L].Set("button", 6);
    tas_button_params[Settings::NativeButton::R].Set("button", 7);
    tas_button_params[Settings::NativeButton::ZL].Set("button", 8);
    tas_button_params[Settings::NativeButton::ZR].Set("button", 9);
    tas_button_params[Settings::NativeButton::Plus].Set("button", 10);
    tas_button_params[Settings::NativeButton::Minus].Set("button", 11);
    tas_button_params[Settings::NativeButton::DLeft].Set("button", 12);
    tas_button_params[Settings::NativeButton::DUp].Set("button", 13);
    tas_button_params[Settings::NativeButton::DRight].Set("button", 14);
    tas_button_params[Settings::NativeButton::DDown].Set("button", 15);
    tas_button_params[Settings::NativeButton::SL].Set("button", 16);
    tas_button_params[Settings::NativeButton::SR].Set("button", 17);
    tas_button_params[Settings::NativeButton::Home].Set("button", 18);
    tas_button_params[Settings::NativeButton::Screenshot].Set("button", 19);

    tas_stick_params[Settings::NativeAnalog::LStick].Set("axis_x", 0);
    tas_stick_params[Settings::NativeAnalog::LStick].Set("axis_y", 1);
    tas_stick_params[Settings::NativeAnalog::RStick].Set("axis_x", 2);
    tas_stick_params[Settings::NativeAnalog::RStick].Set("axis_y", 3);

    // set to optimal stick to avoid sanitizing the stick and tweaking the coordinates
    // making sure they play back in the game as originally written down in the script file
    tas_stick_params[Settings::NativeAnalog::LStick].Set("deadzone", 0.0f);
    tas_stick_params[Settings::NativeAnalog::LStick].Set("range", 1.0f);
    tas_stick_params[Settings::NativeAnalog::RStick].Set("deadzone", 0.0f);
    tas_stick_params[Settings::NativeAnalog::RStick].Set("range", 1.0f);
}

void EmulatedController::LoadVirtualGamepadParams() {
    const auto player_index = NpadIdTypeToIndex(npad_id_type);
    Common::ParamPackage common_params{};
    common_params.Set("engine", "virtual_gamepad");
    common_params.Set("port", static_cast<int>(player_index));
    for (auto& param : virtual_button_params) {
        param = common_params;
    }
    for (auto& param : virtual_stick_params) {
        param = common_params;
    }

    // TODO(german77): Replace this with an input profile or something better
    virtual_button_params[Settings::NativeButton::A].Set("button", 0);
    virtual_button_params[Settings::NativeButton::B].Set("button", 1);
    virtual_button_params[Settings::NativeButton::X].Set("button", 2);
    virtual_button_params[Settings::NativeButton::Y].Set("button", 3);
    virtual_button_params[Settings::NativeButton::LStick].Set("button", 4);
    virtual_button_params[Settings::NativeButton::RStick].Set("button", 5);
    virtual_button_params[Settings::NativeButton::L].Set("button", 6);
    virtual_button_params[Settings::NativeButton::R].Set("button", 7);
    virtual_button_params[Settings::NativeButton::ZL].Set("button", 8);
    virtual_button_params[Settings::NativeButton::ZR].Set("button", 9);
    virtual_button_params[Settings::NativeButton::Plus].Set("button", 10);
    virtual_button_params[Settings::NativeButton::Minus].Set("button", 11);
    virtual_button_params[Settings::NativeButton::DLeft].Set("button", 12);
    virtual_button_params[Settings::NativeButton::DUp].Set("button", 13);
    virtual_button_params[Settings::NativeButton::DRight].Set("button", 14);
    virtual_button_params[Settings::NativeButton::DDown].Set("button", 15);
    virtual_button_params[Settings::NativeButton::SL].Set("button", 16);
    virtual_button_params[Settings::NativeButton::SR].Set("button", 17);
    virtual_button_params[Settings::NativeButton::Home].Set("button", 18);
    virtual_button_params[Settings::NativeButton::Screenshot].Set("button", 19);

    virtual_stick_params[Settings::NativeAnalog::LStick].Set("axis_x", 0);
    virtual_stick_params[Settings::NativeAnalog::LStick].Set("axis_y", 1);
    virtual_stick_params[Settings::NativeAnalog::RStick].Set("axis_x", 2);
    virtual_stick_params[Settings::NativeAnalog::RStick].Set("axis_y", 3);
    virtual_stick_params[Settings::NativeAnalog::LStick].Set("deadzone", 0.0f);
    virtual_stick_params[Settings::NativeAnalog::LStick].Set("range", 1.0f);
    virtual_stick_params[Settings::NativeAnalog::RStick].Set("deadzone", 0.0f);
    virtual_stick_params[Settings::NativeAnalog::RStick].Set("range", 1.0f);
}

void EmulatedController::ReloadInput() {
    // If you load any device here add the equivalent to the UnloadInput() function
    LoadDevices();
    for (std::size_t index = 0; index < button_devices.size(); ++index) {
        if (!button_devices[index]) {
            continue;
        }
        const auto uuid = Common::UUID{button_params[index].Get("guid", "")};
        button_devices[index]->SetCallback({
            .on_change =
                [this, index, uuid](const Common::Input::CallbackStatus& callback) {
                    SetButton(callback, index, uuid);
                },
        });
        button_devices[index]->ForceUpdate();
    }

    for (std::size_t index = 0; index < stick_devices.size(); ++index) {
        if (!stick_devices[index]) {
            continue;
        }
        const auto uuid = Common::UUID{stick_params[index].Get("guid", "")};
        stick_devices[index]->SetCallback({
            .on_change =
                [this, index, uuid](const Common::Input::CallbackStatus& callback) {
                    SetStick(callback, index, uuid);
                },
        });
        stick_devices[index]->ForceUpdate();
    }

    for (std::size_t index = 0; index < trigger_devices.size(); ++index) {
        if (!trigger_devices[index]) {
            continue;
        }
        const auto uuid = Common::UUID{trigger_params[index].Get("guid", "")};
        trigger_devices[index]->SetCallback({
            .on_change =
                [this, index, uuid](const Common::Input::CallbackStatus& callback) {
                    SetTrigger(callback, index, uuid);
                },
        });
        trigger_devices[index]->ForceUpdate();
    }

    for (std::size_t index = 0; index < battery_devices.size(); ++index) {
        if (!battery_devices[index]) {
            continue;
        }
        battery_devices[index]->SetCallback({
            .on_change =
                [this, index](const Common::Input::CallbackStatus& callback) {
                    SetBattery(callback, index);
                },
        });
        battery_devices[index]->ForceUpdate();
    }

    for (std::size_t index = 0; index < color_devices.size(); ++index) {
        if (!color_devices[index]) {
            continue;
        }
        color_devices[index]->SetCallback({
            .on_change =
                [this, index](const Common::Input::CallbackStatus& callback) {
                    SetColors(callback, index);
                },
        });
        color_devices[index]->ForceUpdate();
    }

    for (std::size_t index = 0; index < motion_devices.size(); ++index) {
        if (!motion_devices[index]) {
            continue;
        }
        motion_devices[index]->SetCallback({
            .on_change =
                [this, index](const Common::Input::CallbackStatus& callback) {
                    SetMotion(callback, index);
                },
        });

        // Restore motion state
        auto& emulated_motion = controller.motion_values[index].emulated;
        auto& motion = controller.motion_state[index];
        emulated_motion.ResetRotations();
        emulated_motion.ResetQuaternion();
        motion.accel = emulated_motion.GetAcceleration();
        motion.gyro = emulated_motion.GetGyroscope();
        motion.rotation = emulated_motion.GetRotations();
        motion.euler = emulated_motion.GetEulerAngles();
        motion.orientation = emulated_motion.GetOrientation();
        motion.is_at_rest = !emulated_motion.IsMoving(motion_sensitivity);
    }

    for (std::size_t index = 0; index < camera_devices.size(); ++index) {
        if (!camera_devices[index]) {
            continue;
        }
        camera_devices[index]->SetCallback({
            .on_change =
                [this](const Common::Input::CallbackStatus& callback) { SetCamera(callback); },
        });
        camera_devices[index]->ForceUpdate();
    }

    for (std::size_t index = 0; index < ring_analog_devices.size(); ++index) {
        if (!ring_analog_devices[index]) {
            continue;
        }
        ring_analog_devices[index]->SetCallback({
            .on_change =
                [this](const Common::Input::CallbackStatus& callback) { SetRingAnalog(callback); },
        });
        ring_analog_devices[index]->ForceUpdate();
    }

    for (std::size_t index = 0; index < nfc_devices.size(); ++index) {
        if (!nfc_devices[index]) {
            continue;
        }
        nfc_devices[index]->SetCallback({
            .on_change =
                [this](const Common::Input::CallbackStatus& callback) { SetNfc(callback); },
        });
        nfc_devices[index]->ForceUpdate();
    }

    // Register TAS devices. No need to force update
    for (std::size_t index = 0; index < tas_button_devices.size(); ++index) {
        if (!tas_button_devices[index]) {
            continue;
        }
        tas_button_devices[index]->SetCallback({
            .on_change =
                [this, index](const Common::Input::CallbackStatus& callback) {
                    SetButton(callback, index, TAS_UUID);
                },
        });
    }

    for (std::size_t index = 0; index < tas_stick_devices.size(); ++index) {
        if (!tas_stick_devices[index]) {
            continue;
        }
        tas_stick_devices[index]->SetCallback({
            .on_change =
                [this, index](const Common::Input::CallbackStatus& callback) {
                    SetStick(callback, index, TAS_UUID);
                },
        });
    }

    // Register virtual devices. No need to force update
    for (std::size_t index = 0; index < virtual_button_devices.size(); ++index) {
        if (!virtual_button_devices[index]) {
            continue;
        }
        virtual_button_devices[index]->SetCallback({
            .on_change =
                [this, index](const Common::Input::CallbackStatus& callback) {
                    SetButton(callback, index, VIRTUAL_UUID);
                },
        });
    }

    for (std::size_t index = 0; index < virtual_stick_devices.size(); ++index) {
        if (!virtual_stick_devices[index]) {
            continue;
        }
        virtual_stick_devices[index]->SetCallback({
            .on_change =
                [this, index](const Common::Input::CallbackStatus& callback) {
                    SetStick(callback, index, VIRTUAL_UUID);
                },
        });
    }
    turbo_button_state = 0;
}

void EmulatedController::UnloadInput() {
    for (auto& button : button_devices) {
        button.reset();
    }
    for (auto& stick : stick_devices) {
        stick.reset();
    }
    for (auto& motion : motion_devices) {
        motion.reset();
    }
    for (auto& trigger : trigger_devices) {
        trigger.reset();
    }
    for (auto& battery : battery_devices) {
        battery.reset();
    }
    for (auto& color : color_devices) {
        color.reset();
    }
    for (auto& output : output_devices) {
        output.reset();
    }
    for (auto& button : tas_button_devices) {
        button.reset();
    }
    for (auto& stick : tas_stick_devices) {
        stick.reset();
    }
    for (auto& button : virtual_button_devices) {
        button.reset();
    }
    for (auto& stick : virtual_stick_devices) {
        stick.reset();
    }
    for (auto& camera : camera_devices) {
        camera.reset();
    }
    for (auto& ring : ring_analog_devices) {
        ring.reset();
    }
    for (auto& nfc : nfc_devices) {
        nfc.reset();
    }
}

void EmulatedController::EnableConfiguration() {
    is_configuring = true;
    tmp_is_connected = is_connected;
    tmp_npad_type = npad_type;
}

void EmulatedController::DisableConfiguration() {
    is_configuring = false;

    // Get Joycon colors before turning on the controller
    for (const auto& color_device : color_devices) {
        color_device->ForceUpdate();
    }

    // Apply temporary npad type to the real controller
    if (tmp_npad_type != npad_type) {
        if (is_connected) {
            Disconnect();
        }
        SetNpadStyleIndex(tmp_npad_type);
        original_npad_type = tmp_npad_type;
    }

    // Apply temporary connected status to the real controller
    if (tmp_is_connected != is_connected) {
        if (tmp_is_connected) {
            Connect();
            return;
        }
        Disconnect();
    }
}

void EmulatedController::EnableSystemButtons() {
    std::scoped_lock lock{mutex};
    system_buttons_enabled = true;
}

void EmulatedController::DisableSystemButtons() {
    std::scoped_lock lock{mutex};
    system_buttons_enabled = false;
    controller.home_button_state.raw = 0;
    controller.capture_button_state.raw = 0;
}

void EmulatedController::ResetSystemButtons() {
    std::scoped_lock lock{mutex};
    controller.home_button_state.home.Assign(false);
    controller.capture_button_state.capture.Assign(false);
}

bool EmulatedController::IsConfiguring() const {
    return is_configuring;
}

void EmulatedController::SaveCurrentConfig() {
    const auto player_index = NpadIdTypeToIndex(npad_id_type);
    auto& player = Settings::values.players.GetValue()[player_index];
    player.connected = is_connected;
    player.controller_type = MapNPadToSettingsType(npad_type);
    for (std::size_t index = 0; index < player.buttons.size(); ++index) {
        player.buttons[index] = button_params[index].Serialize();
    }
    for (std::size_t index = 0; index < player.analogs.size(); ++index) {
        player.analogs[index] = stick_params[index].Serialize();
    }
    for (std::size_t index = 0; index < player.motions.size(); ++index) {
        player.motions[index] = motion_params[index].Serialize();
    }
    if (npad_id_type == NpadIdType::Player1) {
        Settings::values.ringcon_analogs = ring_params[0].Serialize();
    }
}

void EmulatedController::RestoreConfig() {
    if (!is_configuring) {
        return;
    }
    ReloadFromSettings();
}

std::vector<Common::ParamPackage> EmulatedController::GetMappedDevices() const {
    std::vector<Common::ParamPackage> devices;
    for (const auto& param : button_params) {
        if (!param.Has("engine")) {
            continue;
        }
        const auto devices_it = std::find_if(
            devices.begin(), devices.end(), [&param](const Common::ParamPackage& param_) {
                return param.Get("engine", "") == param_.Get("engine", "") &&
                       param.Get("guid", "") == param_.Get("guid", "") &&
                       param.Get("port", 0) == param_.Get("port", 0) &&
                       param.Get("pad", 0) == param_.Get("pad", 0);
            });
        if (devices_it != devices.end()) {
            continue;
        }

        auto& device = devices.emplace_back();
        device.Set("engine", param.Get("engine", ""));
        device.Set("guid", param.Get("guid", ""));
        device.Set("port", param.Get("port", 0));
        device.Set("pad", param.Get("pad", 0));
    }

    for (const auto& param : stick_params) {
        if (!param.Has("engine")) {
            continue;
        }
        if (param.Get("engine", "") == "analog_from_button") {
            continue;
        }
        const auto devices_it = std::find_if(
            devices.begin(), devices.end(), [&param](const Common::ParamPackage& param_) {
                return param.Get("engine", "") == param_.Get("engine", "") &&
                       param.Get("guid", "") == param_.Get("guid", "") &&
                       param.Get("port", 0) == param_.Get("port", 0) &&
                       param.Get("pad", 0) == param_.Get("pad", 0);
            });
        if (devices_it != devices.end()) {
            continue;
        }

        auto& device = devices.emplace_back();
        device.Set("engine", param.Get("engine", ""));
        device.Set("guid", param.Get("guid", ""));
        device.Set("port", param.Get("port", 0));
        device.Set("pad", param.Get("pad", 0));
    }
    return devices;
}

Common::ParamPackage EmulatedController::GetButtonParam(std::size_t index) const {
    if (index >= button_params.size()) {
        return {};
    }
    return button_params[index];
}

Common::ParamPackage EmulatedController::GetStickParam(std::size_t index) const {
    if (index >= stick_params.size()) {
        return {};
    }
    return stick_params[index];
}

Common::ParamPackage EmulatedController::GetMotionParam(std::size_t index) const {
    if (index >= motion_params.size()) {
        return {};
    }
    return motion_params[index];
}

void EmulatedController::SetButtonParam(std::size_t index, Common::ParamPackage param) {
    if (index >= button_params.size()) {
        return;
    }
    button_params[index] = std::move(param);
    ReloadInput();
}

void EmulatedController::SetStickParam(std::size_t index, Common::ParamPackage param) {
    if (index >= stick_params.size()) {
        return;
    }
    stick_params[index] = std::move(param);
    ReloadInput();
}

void EmulatedController::SetMotionParam(std::size_t index, Common::ParamPackage param) {
    if (index >= motion_params.size()) {
        return;
    }
    motion_params[index] = std::move(param);
    ReloadInput();
}

void EmulatedController::SetButton(const Common::Input::CallbackStatus& callback, std::size_t index,
                                   Common::UUID uuid) {
    if (index >= controller.button_values.size()) {
        return;
    }
    std::unique_lock lock{mutex};
    bool value_changed = false;
    const auto new_status = TransformToButton(callback);
    auto& current_status = controller.button_values[index];

    // Only read button values that have the same uuid or are pressed once
    if (current_status.uuid != uuid) {
        if (!new_status.value) {
            return;
        }
    }

    current_status.toggle = new_status.toggle;
    current_status.turbo = new_status.turbo;
    current_status.uuid = uuid;

    // Update button status with current
    if (!current_status.toggle) {
        current_status.locked = false;
        if (current_status.value != new_status.value) {
            current_status.value = new_status.value;
            value_changed = true;
        }
    } else {
        // Toggle button and lock status
        if (new_status.value && !current_status.locked) {
            current_status.locked = true;
            current_status.value = !current_status.value;
            value_changed = true;
        }

        // Unlock button ready for next press
        if (!new_status.value && current_status.locked) {
            current_status.locked = false;
        }
    }

    if (!value_changed) {
        return;
    }

    if (is_configuring) {
        controller.npad_button_state.raw = NpadButton::None;
        controller.debug_pad_button_state.raw = 0;
        controller.home_button_state.raw = 0;
        controller.capture_button_state.raw = 0;
        lock.unlock();
        TriggerOnChange(ControllerTriggerType::Button, false);
        return;
    }

    // GC controllers have triggers not buttons
    if (npad_type == NpadStyleIndex::GameCube) {
        if (index == Settings::NativeButton::ZR) {
            return;
        }
        if (index == Settings::NativeButton::ZL) {
            return;
        }
    }

    switch (index) {
    case Settings::NativeButton::A:
        controller.npad_button_state.a.Assign(current_status.value);
        controller.debug_pad_button_state.a.Assign(current_status.value);
        break;
    case Settings::NativeButton::B:
        controller.npad_button_state.b.Assign(current_status.value);
        controller.debug_pad_button_state.b.Assign(current_status.value);
        break;
    case Settings::NativeButton::X:
        controller.npad_button_state.x.Assign(current_status.value);
        controller.debug_pad_button_state.x.Assign(current_status.value);
        break;
    case Settings::NativeButton::Y:
        controller.npad_button_state.y.Assign(current_status.value);
        controller.debug_pad_button_state.y.Assign(current_status.value);
        break;
    case Settings::NativeButton::LStick:
        controller.npad_button_state.stick_l.Assign(current_status.value);
        break;
    case Settings::NativeButton::RStick:
        controller.npad_button_state.stick_r.Assign(current_status.value);
        break;
    case Settings::NativeButton::L:
        controller.npad_button_state.l.Assign(current_status.value);
        controller.debug_pad_button_state.l.Assign(current_status.value);
        break;
    case Settings::NativeButton::R:
        controller.npad_button_state.r.Assign(current_status.value);
        controller.debug_pad_button_state.r.Assign(current_status.value);
        break;
    case Settings::NativeButton::ZL:
        controller.npad_button_state.zl.Assign(current_status.value);
        controller.debug_pad_button_state.zl.Assign(current_status.value);
        break;
    case Settings::NativeButton::ZR:
        controller.npad_button_state.zr.Assign(current_status.value);
        controller.debug_pad_button_state.zr.Assign(current_status.value);
        break;
    case Settings::NativeButton::Plus:
        controller.npad_button_state.plus.Assign(current_status.value);
        controller.debug_pad_button_state.plus.Assign(current_status.value);
        break;
    case Settings::NativeButton::Minus:
        controller.npad_button_state.minus.Assign(current_status.value);
        controller.debug_pad_button_state.minus.Assign(current_status.value);
        break;
    case Settings::NativeButton::DLeft:
        controller.npad_button_state.left.Assign(current_status.value);
        controller.debug_pad_button_state.d_left.Assign(current_status.value);
        break;
    case Settings::NativeButton::DUp:
        controller.npad_button_state.up.Assign(current_status.value);
        controller.debug_pad_button_state.d_up.Assign(current_status.value);
        break;
    case Settings::NativeButton::DRight:
        controller.npad_button_state.right.Assign(current_status.value);
        controller.debug_pad_button_state.d_right.Assign(current_status.value);
        break;
    case Settings::NativeButton::DDown:
        controller.npad_button_state.down.Assign(current_status.value);
        controller.debug_pad_button_state.d_down.Assign(current_status.value);
        break;
    case Settings::NativeButton::SL:
        controller.npad_button_state.left_sl.Assign(current_status.value);
        controller.npad_button_state.right_sl.Assign(current_status.value);
        break;
    case Settings::NativeButton::SR:
        controller.npad_button_state.left_sr.Assign(current_status.value);
        controller.npad_button_state.right_sr.Assign(current_status.value);
        break;
    case Settings::NativeButton::Home:
        if (!system_buttons_enabled) {
            break;
        }
        controller.home_button_state.home.Assign(current_status.value);
        break;
    case Settings::NativeButton::Screenshot:
        if (!system_buttons_enabled) {
            break;
        }
        controller.capture_button_state.capture.Assign(current_status.value);
        break;
    }

    lock.unlock();

    if (!is_connected) {
        if (npad_id_type == NpadIdType::Player1 && npad_type != NpadStyleIndex::Handheld) {
            Connect();
        }
        if (npad_id_type == NpadIdType::Handheld && npad_type == NpadStyleIndex::Handheld) {
            Connect();
        }
    }
    TriggerOnChange(ControllerTriggerType::Button, true);
}

void EmulatedController::SetStick(const Common::Input::CallbackStatus& callback, std::size_t index,
                                  Common::UUID uuid) {
    if (index >= controller.stick_values.size()) {
        return;
    }
    auto trigger_guard =
        SCOPE_GUARD({ TriggerOnChange(ControllerTriggerType::Stick, !is_configuring); });
    std::scoped_lock lock{mutex};
    const auto stick_value = TransformToStick(callback);

    // Only read stick values that have the same uuid or are over the threshold to avoid flapping
    if (controller.stick_values[index].uuid != uuid) {
        const bool is_tas = uuid == TAS_UUID;
        if (is_tas && stick_value.x.value == 0 && stick_value.y.value == 0) {
            trigger_guard.Cancel();
            return;
        }
        if (!is_tas && !stick_value.down && !stick_value.up && !stick_value.left &&
            !stick_value.right) {
            trigger_guard.Cancel();
            return;
        }
    }

    controller.stick_values[index] = stick_value;
    controller.stick_values[index].uuid = uuid;

    if (is_configuring) {
        controller.analog_stick_state.left = {};
        controller.analog_stick_state.right = {};
        return;
    }

    const AnalogStickState stick{
        .x = static_cast<s32>(controller.stick_values[index].x.value * HID_JOYSTICK_MAX),
        .y = static_cast<s32>(controller.stick_values[index].y.value * HID_JOYSTICK_MAX),
    };

    switch (index) {
    case Settings::NativeAnalog::LStick:
        controller.analog_stick_state.left = stick;
        controller.npad_button_state.stick_l_left.Assign(controller.stick_values[index].left);
        controller.npad_button_state.stick_l_up.Assign(controller.stick_values[index].up);
        controller.npad_button_state.stick_l_right.Assign(controller.stick_values[index].right);
        controller.npad_button_state.stick_l_down.Assign(controller.stick_values[index].down);
        break;
    case Settings::NativeAnalog::RStick:
        controller.analog_stick_state.right = stick;
        controller.npad_button_state.stick_r_left.Assign(controller.stick_values[index].left);
        controller.npad_button_state.stick_r_up.Assign(controller.stick_values[index].up);
        controller.npad_button_state.stick_r_right.Assign(controller.stick_values[index].right);
        controller.npad_button_state.stick_r_down.Assign(controller.stick_values[index].down);
        break;
    }
}

void EmulatedController::SetTrigger(const Common::Input::CallbackStatus& callback,
                                    std::size_t index, Common::UUID uuid) {
    if (index >= controller.trigger_values.size()) {
        return;
    }
    auto trigger_guard =
        SCOPE_GUARD({ TriggerOnChange(ControllerTriggerType::Trigger, !is_configuring); });
    std::scoped_lock lock{mutex};
    const auto trigger_value = TransformToTrigger(callback);

    // Only read trigger values that have the same uuid or are pressed once
    if (controller.trigger_values[index].uuid != uuid) {
        if (!trigger_value.pressed.value) {
            return;
        }
    }

    controller.trigger_values[index] = trigger_value;
    controller.trigger_values[index].uuid = uuid;

    if (is_configuring) {
        controller.gc_trigger_state.left = 0;
        controller.gc_trigger_state.right = 0;
        return;
    }

    // Only GC controllers have analog triggers
    if (npad_type != NpadStyleIndex::GameCube) {
        trigger_guard.Cancel();
        return;
    }

    const auto& trigger = controller.trigger_values[index];

    switch (index) {
    case Settings::NativeTrigger::LTrigger:
        controller.gc_trigger_state.left = static_cast<s32>(trigger.analog.value * HID_TRIGGER_MAX);
        controller.npad_button_state.zl.Assign(trigger.pressed.value);
        break;
    case Settings::NativeTrigger::RTrigger:
        controller.gc_trigger_state.right =
            static_cast<s32>(trigger.analog.value * HID_TRIGGER_MAX);
        controller.npad_button_state.zr.Assign(trigger.pressed.value);
        break;
    }
}

void EmulatedController::SetMotion(const Common::Input::CallbackStatus& callback,
                                   std::size_t index) {
    if (index >= controller.motion_values.size()) {
        return;
    }
    SCOPE_EXIT({ TriggerOnChange(ControllerTriggerType::Motion, !is_configuring); });
    std::scoped_lock lock{mutex};
    auto& raw_status = controller.motion_values[index].raw_status;
    auto& emulated = controller.motion_values[index].emulated;

    raw_status = TransformToMotion(callback);
    emulated.SetAcceleration(Common::Vec3f{
        raw_status.accel.x.value,
        raw_status.accel.y.value,
        raw_status.accel.z.value,
    });
    emulated.SetGyroscope(Common::Vec3f{
        raw_status.gyro.x.value,
        raw_status.gyro.y.value,
        raw_status.gyro.z.value,
    });
    emulated.SetUserGyroThreshold(raw_status.gyro.x.properties.threshold);
    emulated.UpdateRotation(raw_status.delta_timestamp);
    emulated.UpdateOrientation(raw_status.delta_timestamp);

    auto& motion = controller.motion_state[index];
    motion.accel = emulated.GetAcceleration();
    motion.gyro = emulated.GetGyroscope();
    motion.rotation = emulated.GetRotations();
    motion.euler = emulated.GetEulerAngles();
    motion.orientation = emulated.GetOrientation();
    motion.is_at_rest = !emulated.IsMoving(motion_sensitivity);
}

void EmulatedController::SetColors(const Common::Input::CallbackStatus& callback,
                                   std::size_t index) {
    if (index >= controller.color_values.size()) {
        return;
    }
    auto trigger_guard =
        SCOPE_GUARD({ TriggerOnChange(ControllerTriggerType::Color, !is_configuring); });
    std::scoped_lock lock{mutex};
    controller.color_values[index] = TransformToColor(callback);

    if (is_configuring) {
        return;
    }

    if (controller.color_values[index].body == 0) {
        trigger_guard.Cancel();
        return;
    }

    controller.colors_state.fullkey = {
        .body = GetNpadColor(controller.color_values[index].body),
        .button = GetNpadColor(controller.color_values[index].buttons),
    };
    if (npad_type == NpadStyleIndex::ProController) {
        controller.colors_state.left = {
            .body = GetNpadColor(controller.color_values[index].left_grip),
            .button = GetNpadColor(controller.color_values[index].buttons),
        };
        controller.colors_state.right = {
            .body = GetNpadColor(controller.color_values[index].right_grip),
            .button = GetNpadColor(controller.color_values[index].buttons),
        };
    } else {
        switch (index) {
        case LeftIndex:
            controller.colors_state.left = {
                .body = GetNpadColor(controller.color_values[index].body),
                .button = GetNpadColor(controller.color_values[index].buttons),
            };
            break;
        case RightIndex:
            controller.colors_state.right = {
                .body = GetNpadColor(controller.color_values[index].body),
                .button = GetNpadColor(controller.color_values[index].buttons),
            };
            break;
        }
    }
}

void EmulatedController::SetBattery(const Common::Input::CallbackStatus& callback,
                                    std::size_t index) {
    if (index >= controller.battery_values.size()) {
        return;
    }
    SCOPE_EXIT({ TriggerOnChange(ControllerTriggerType::Battery, !is_configuring); });
    std::scoped_lock lock{mutex};
    controller.battery_values[index] = TransformToBattery(callback);

    if (is_configuring) {
        return;
    }

    bool is_charging = false;
    bool is_powered = false;
    NpadBatteryLevel battery_level = 0;
    switch (controller.battery_values[index]) {
    case Common::Input::BatteryLevel::Charging:
        is_charging = true;
        is_powered = true;
        battery_level = 6;
        break;
    case Common::Input::BatteryLevel::Medium:
        battery_level = 6;
        break;
    case Common::Input::BatteryLevel::Low:
        battery_level = 4;
        break;
    case Common::Input::BatteryLevel::Critical:
        battery_level = 2;
        break;
    case Common::Input::BatteryLevel::Empty:
        battery_level = 0;
        break;
    case Common::Input::BatteryLevel::None:
    case Common::Input::BatteryLevel::Full:
    default:
        is_powered = true;
        battery_level = 8;
        break;
    }

    switch (index) {
    case LeftIndex:
        controller.battery_state.left = {
            .is_powered = is_powered,
            .is_charging = is_charging,
            .battery_level = battery_level,
        };
        break;
    case RightIndex:
        controller.battery_state.right = {
            .is_powered = is_powered,
            .is_charging = is_charging,
            .battery_level = battery_level,
        };
        break;
    case DualIndex:
        controller.battery_state.dual = {
            .is_powered = is_powered,
            .is_charging = is_charging,
            .battery_level = battery_level,
        };
        break;
    }
}

void EmulatedController::SetCamera(const Common::Input::CallbackStatus& callback) {
    SCOPE_EXIT({ TriggerOnChange(ControllerTriggerType::IrSensor, !is_configuring); });
    std::scoped_lock lock{mutex};
    controller.camera_values = TransformToCamera(callback);

    if (is_configuring) {
        return;
    }

    controller.camera_state.sample++;
    controller.camera_state.format =
        static_cast<Core::IrSensor::ImageTransferProcessorFormat>(controller.camera_values.format);
    controller.camera_state.data = controller.camera_values.data;
}

void EmulatedController::SetRingAnalog(const Common::Input::CallbackStatus& callback) {
    SCOPE_EXIT({ TriggerOnChange(ControllerTriggerType::RingController, !is_configuring); });
    std::scoped_lock lock{mutex};
    const auto force_value = TransformToStick(callback);

    controller.ring_analog_value = force_value.x;

    if (is_configuring) {
        return;
    }

    controller.ring_analog_state.force = force_value.x.value;
}

void EmulatedController::SetNfc(const Common::Input::CallbackStatus& callback) {
    SCOPE_EXIT({ TriggerOnChange(ControllerTriggerType::Nfc, !is_configuring); });
    std::scoped_lock lock{mutex};
    controller.nfc_values = TransformToNfc(callback);

    if (is_configuring) {
        return;
    }

    controller.nfc_state = {
        controller.nfc_values.state,
        controller.nfc_values.data,
    };
}

bool EmulatedController::SetVibration(std::size_t device_index, VibrationValue vibration) {
    if (device_index >= output_devices.size()) {
        return false;
    }
    if (!output_devices[device_index]) {
        return false;
    }
    const auto player_index = NpadIdTypeToIndex(npad_id_type);
    const auto& player = Settings::values.players.GetValue()[player_index];
    const f32 strength = static_cast<f32>(player.vibration_strength) / 100.0f;

    if (!player.vibration_enabled) {
        return false;
    }

    // Exponential amplification is too strong at low amplitudes. Switch to a linear
    // amplification if strength is set below 0.7f
    const Common::Input::VibrationAmplificationType type =
        strength > 0.7f ? Common::Input::VibrationAmplificationType::Exponential
                        : Common::Input::VibrationAmplificationType::Linear;

    const Common::Input::VibrationStatus status = {
        .low_amplitude = std::min(vibration.low_amplitude * strength, 1.0f),
        .low_frequency = vibration.low_frequency,
        .high_amplitude = std::min(vibration.high_amplitude * strength, 1.0f),
        .high_frequency = vibration.high_frequency,
        .type = type,
    };
    return output_devices[device_index]->SetVibration(status) ==
           Common::Input::DriverResult::Success;
}

bool EmulatedController::IsVibrationEnabled(std::size_t device_index) {
    const auto player_index = NpadIdTypeToIndex(npad_id_type);
    const auto& player = Settings::values.players.GetValue()[player_index];

    if (!player.vibration_enabled) {
        return false;
    }

    if (device_index >= output_devices.size()) {
        return false;
    }

    if (!output_devices[device_index]) {
        return false;
    }

    return output_devices[device_index]->IsVibrationEnabled();
}

Common::Input::DriverResult EmulatedController::SetPollingMode(
    EmulatedDeviceIndex device_index, Common::Input::PollingMode polling_mode) {
    LOG_INFO(Service_HID, "Set polling mode {}, device_index={}", polling_mode, device_index);

    auto& left_output_device = output_devices[static_cast<std::size_t>(DeviceIndex::Left)];
    auto& right_output_device = output_devices[static_cast<std::size_t>(DeviceIndex::Right)];
    auto& nfc_output_device = output_devices[3];

    if (device_index == EmulatedDeviceIndex::LeftIndex) {
        return left_output_device->SetPollingMode(polling_mode);
    }

    if (device_index == EmulatedDeviceIndex::RightIndex) {
        const auto virtual_nfc_result = nfc_output_device->SetPollingMode(polling_mode);
        const auto mapped_nfc_result = right_output_device->SetPollingMode(polling_mode);

        if (virtual_nfc_result == Common::Input::DriverResult::Success) {
            return virtual_nfc_result;
        }
        return mapped_nfc_result;
    }

    left_output_device->SetPollingMode(polling_mode);
    right_output_device->SetPollingMode(polling_mode);
    nfc_output_device->SetPollingMode(polling_mode);
    return Common::Input::DriverResult::Success;
}

bool EmulatedController::SetCameraFormat(
    Core::IrSensor::ImageTransferProcessorFormat camera_format) {
    LOG_INFO(Service_HID, "Set camera format {}", camera_format);

    auto& right_output_device = output_devices[static_cast<std::size_t>(DeviceIndex::Right)];
    auto& camera_output_device = output_devices[2];

    if (right_output_device->SetCameraFormat(static_cast<Common::Input::CameraFormat>(
            camera_format)) == Common::Input::DriverResult::Success) {
        return true;
    }

    // Fallback to Qt camera if native device doesn't have support
    return camera_output_device->SetCameraFormat(static_cast<Common::Input::CameraFormat>(
               camera_format)) == Common::Input::DriverResult::Success;
}

Common::ParamPackage EmulatedController::GetRingParam() const {
    return ring_params[0];
}

void EmulatedController::SetRingParam(Common::ParamPackage param) {
    ring_params[0] = std::move(param);
    ReloadInput();
}

bool EmulatedController::HasNfc() const {
    const auto& nfc_output_device = output_devices[3];

    switch (npad_type) {
    case NpadStyleIndex::JoyconRight:
    case NpadStyleIndex::JoyconDual:
    case NpadStyleIndex::ProController:
    case NpadStyleIndex::Handheld:
        break;
    default:
        return false;
    }

    const bool has_virtual_nfc =
        npad_id_type == NpadIdType::Player1 || npad_id_type == NpadIdType::Handheld;
    const bool is_virtual_nfc_supported =
        nfc_output_device->SupportsNfc() != Common::Input::NfcState::NotSupported;

    return is_connected && (has_virtual_nfc && is_virtual_nfc_supported);
}

bool EmulatedController::WriteNfc(const std::vector<u8>& data) {
    auto& nfc_output_device = output_devices[3];

    return nfc_output_device->WriteNfcData(data) == Common::Input::NfcState::Success;
}

void EmulatedController::SetLedPattern() {
    for (auto& device : output_devices) {
        if (!device) {
            continue;
        }

        const LedPattern pattern = GetLedPattern();
        const Common::Input::LedStatus status = {
            .led_1 = pattern.position1 != 0,
            .led_2 = pattern.position2 != 0,
            .led_3 = pattern.position3 != 0,
            .led_4 = pattern.position4 != 0,
        };
        device->SetLED(status);
    }
}

void EmulatedController::SetGyroscopeZeroDriftMode(GyroscopeZeroDriftMode mode) {
    for (auto& motion : controller.motion_values) {
        switch (mode) {
        case GyroscopeZeroDriftMode::Loose:
            motion_sensitivity = motion.emulated.IsAtRestLoose;
            motion.emulated.SetGyroThreshold(motion.emulated.ThresholdLoose);
            break;
        case GyroscopeZeroDriftMode::Tight:
            motion_sensitivity = motion.emulated.IsAtRestThight;
            motion.emulated.SetGyroThreshold(motion.emulated.ThresholdThight);
            break;
        case GyroscopeZeroDriftMode::Standard:
        default:
            motion_sensitivity = motion.emulated.IsAtRestStandard;
            motion.emulated.SetGyroThreshold(motion.emulated.ThresholdStandard);
            break;
        }
    }
}

void EmulatedController::SetSupportedNpadStyleTag(NpadStyleTag supported_styles) {
    supported_style_tag = supported_styles;
    if (!is_connected) {
        return;
    }

    // Attempt to reconnect with the original type
    if (npad_type != original_npad_type) {
        Disconnect();
        const auto current_npad_type = npad_type;
        SetNpadStyleIndex(original_npad_type);
        if (IsControllerSupported()) {
            Connect();
            return;
        }
        SetNpadStyleIndex(current_npad_type);
        Connect();
    }

    if (IsControllerSupported()) {
        return;
    }

    Disconnect();

    // Fallback Fullkey controllers to Pro controllers
    if (IsControllerFullkey() && supported_style_tag.fullkey) {
        LOG_WARNING(Service_HID, "Reconnecting controller type {} as Pro controller", npad_type);
        SetNpadStyleIndex(NpadStyleIndex::ProController);
        Connect();
        return;
    }

    // Fallback Dual joycon controllers to Pro controllers
    if (npad_type == NpadStyleIndex::JoyconDual && supported_style_tag.fullkey) {
        LOG_WARNING(Service_HID, "Reconnecting controller type {} as Pro controller", npad_type);
        SetNpadStyleIndex(NpadStyleIndex::ProController);
        Connect();
        return;
    }

    // Fallback Pro controllers to Dual joycon
    if (npad_type == NpadStyleIndex::ProController && supported_style_tag.joycon_dual) {
        LOG_WARNING(Service_HID, "Reconnecting controller type {} as Dual Joycons", npad_type);
        SetNpadStyleIndex(NpadStyleIndex::JoyconDual);
        Connect();
        return;
    }

    LOG_ERROR(Service_HID, "Controller type {} is not supported. Disconnecting controller",
              npad_type);
}

bool EmulatedController::IsControllerFullkey(bool use_temporary_value) const {
    std::scoped_lock lock{mutex};
    const auto type = is_configuring && use_temporary_value ? tmp_npad_type : npad_type;
    switch (type) {
    case NpadStyleIndex::ProController:
    case NpadStyleIndex::GameCube:
    case NpadStyleIndex::NES:
    case NpadStyleIndex::SNES:
    case NpadStyleIndex::N64:
    case NpadStyleIndex::SegaGenesis:
        return true;
    default:
        return false;
    }
}

bool EmulatedController::IsControllerSupported(bool use_temporary_value) const {
    std::scoped_lock lock{mutex};
    const auto type = is_configuring && use_temporary_value ? tmp_npad_type : npad_type;
    switch (type) {
    case NpadStyleIndex::ProController:
        return supported_style_tag.fullkey.As<bool>();
    case NpadStyleIndex::Handheld:
        return supported_style_tag.handheld.As<bool>();
    case NpadStyleIndex::JoyconDual:
        return supported_style_tag.joycon_dual.As<bool>();
    case NpadStyleIndex::JoyconLeft:
        return supported_style_tag.joycon_left.As<bool>();
    case NpadStyleIndex::JoyconRight:
        return supported_style_tag.joycon_right.As<bool>();
    case NpadStyleIndex::GameCube:
        return supported_style_tag.gamecube.As<bool>();
    case NpadStyleIndex::Pokeball:
        return supported_style_tag.palma.As<bool>();
    case NpadStyleIndex::NES:
        return supported_style_tag.lark.As<bool>();
    case NpadStyleIndex::SNES:
        return supported_style_tag.lucia.As<bool>();
    case NpadStyleIndex::N64:
        return supported_style_tag.lagoon.As<bool>();
    case NpadStyleIndex::SegaGenesis:
        return supported_style_tag.lager.As<bool>();
    default:
        return false;
    }
}

void EmulatedController::Connect(bool use_temporary_value) {
    if (!IsControllerSupported(use_temporary_value)) {
        const auto type = is_configuring && use_temporary_value ? tmp_npad_type : npad_type;
        LOG_ERROR(Service_HID, "Controller type {} is not supported", type);
        return;
    }

    auto trigger_guard =
        SCOPE_GUARD({ TriggerOnChange(ControllerTriggerType::Connected, !is_configuring); });
    std::scoped_lock lock{mutex};
    if (is_configuring) {
        tmp_is_connected = true;
        return;
    }

    if (is_connected) {
        trigger_guard.Cancel();
        return;
    }
    is_connected = true;
}

void EmulatedController::Disconnect() {
    auto trigger_guard =
        SCOPE_GUARD({ TriggerOnChange(ControllerTriggerType::Disconnected, !is_configuring); });
    std::scoped_lock lock{mutex};
    if (is_configuring) {
        tmp_is_connected = false;
        return;
    }

    if (!is_connected) {
        trigger_guard.Cancel();
        return;
    }
    is_connected = false;
}

bool EmulatedController::IsConnected(bool get_temporary_value) const {
    std::scoped_lock lock{mutex};
    if (get_temporary_value && is_configuring) {
        return tmp_is_connected;
    }
    return is_connected;
}

NpadIdType EmulatedController::GetNpadIdType() const {
    std::scoped_lock lock{mutex};
    return npad_id_type;
}

NpadStyleIndex EmulatedController::GetNpadStyleIndex(bool get_temporary_value) const {
    std::scoped_lock lock{mutex};
    if (get_temporary_value && is_configuring) {
        return tmp_npad_type;
    }
    return npad_type;
}

void EmulatedController::SetNpadStyleIndex(NpadStyleIndex npad_type_) {
    auto trigger_guard =
        SCOPE_GUARD({ TriggerOnChange(ControllerTriggerType::Type, !is_configuring); });
    std::scoped_lock lock{mutex};

    if (is_configuring) {
        if (tmp_npad_type == npad_type_) {
            trigger_guard.Cancel();
            return;
        }
        tmp_npad_type = npad_type_;
        return;
    }

    if (npad_type == npad_type_) {
        trigger_guard.Cancel();
        return;
    }
    if (is_connected) {
        LOG_WARNING(Service_HID, "Controller {} type changed while it's connected",
                    NpadIdTypeToIndex(npad_id_type));
    }
    npad_type = npad_type_;
}

LedPattern EmulatedController::GetLedPattern() const {
    switch (npad_id_type) {
    case NpadIdType::Player1:
        return LedPattern{1, 0, 0, 0};
    case NpadIdType::Player2:
        return LedPattern{1, 1, 0, 0};
    case NpadIdType::Player3:
        return LedPattern{1, 1, 1, 0};
    case NpadIdType::Player4:
        return LedPattern{1, 1, 1, 1};
    case NpadIdType::Player5:
        return LedPattern{1, 0, 0, 1};
    case NpadIdType::Player6:
        return LedPattern{1, 0, 1, 0};
    case NpadIdType::Player7:
        return LedPattern{1, 0, 1, 1};
    case NpadIdType::Player8:
        return LedPattern{0, 1, 1, 0};
    default:
        return LedPattern{0, 0, 0, 0};
    }
}

ButtonValues EmulatedController::GetButtonsValues() const {
    std::scoped_lock lock{mutex};
    return controller.button_values;
}

SticksValues EmulatedController::GetSticksValues() const {
    std::scoped_lock lock{mutex};
    return controller.stick_values;
}

TriggerValues EmulatedController::GetTriggersValues() const {
    std::scoped_lock lock{mutex};
    return controller.trigger_values;
}

ControllerMotionValues EmulatedController::GetMotionValues() const {
    std::scoped_lock lock{mutex};
    return controller.motion_values;
}

ColorValues EmulatedController::GetColorsValues() const {
    std::scoped_lock lock{mutex};
    return controller.color_values;
}

BatteryValues EmulatedController::GetBatteryValues() const {
    std::scoped_lock lock{mutex};
    return controller.battery_values;
}

CameraValues EmulatedController::GetCameraValues() const {
    std::scoped_lock lock{mutex};
    return controller.camera_values;
}

RingAnalogValue EmulatedController::GetRingSensorValues() const {
    return controller.ring_analog_value;
}

HomeButtonState EmulatedController::GetHomeButtons() const {
    std::scoped_lock lock{mutex};
    if (is_configuring) {
        return {};
    }
    return controller.home_button_state;
}

CaptureButtonState EmulatedController::GetCaptureButtons() const {
    std::scoped_lock lock{mutex};
    if (is_configuring) {
        return {};
    }
    return controller.capture_button_state;
}

NpadButtonState EmulatedController::GetNpadButtons() const {
    std::scoped_lock lock{mutex};
    if (is_configuring) {
        return {};
    }
    return {controller.npad_button_state.raw & GetTurboButtonMask()};
}

DebugPadButton EmulatedController::GetDebugPadButtons() const {
    std::scoped_lock lock{mutex};
    if (is_configuring) {
        return {};
    }
    return controller.debug_pad_button_state;
}

AnalogSticks EmulatedController::GetSticks() const {
    std::scoped_lock lock{mutex};

    if (is_configuring) {
        return {};
    }

    return controller.analog_stick_state;
}

NpadGcTriggerState EmulatedController::GetTriggers() const {
    std::scoped_lock lock{mutex};
    if (is_configuring) {
        return {};
    }
    return controller.gc_trigger_state;
}

MotionState EmulatedController::GetMotions() const {
    std::unique_lock lock{mutex};
    return controller.motion_state;
}

ControllerColors EmulatedController::GetColors() const {
    std::scoped_lock lock{mutex};
    return controller.colors_state;
}

BatteryLevelState EmulatedController::GetBattery() const {
    std::scoped_lock lock{mutex};
    return controller.battery_state;
}

const CameraState& EmulatedController::GetCamera() const {
    std::scoped_lock lock{mutex};
    return controller.camera_state;
}

RingSensorForce EmulatedController::GetRingSensorForce() const {
    return controller.ring_analog_state;
}

const NfcState& EmulatedController::GetNfc() const {
    std::scoped_lock lock{mutex};
    return controller.nfc_state;
}

NpadColor EmulatedController::GetNpadColor(u32 color) {
    return {
        .r = static_cast<u8>((color >> 16) & 0xFF),
        .g = static_cast<u8>((color >> 8) & 0xFF),
        .b = static_cast<u8>(color & 0xFF),
        .a = 0xff,
    };
}

void EmulatedController::TriggerOnChange(ControllerTriggerType type, bool is_npad_service_update) {
    std::scoped_lock lock{callback_mutex};
    for (const auto& poller_pair : callback_list) {
        const ControllerUpdateCallback& poller = poller_pair.second;
        if (!is_npad_service_update && poller.is_npad_service) {
            continue;
        }
        if (poller.on_change) {
            poller.on_change(type);
        }
    }
}

int EmulatedController::SetCallback(ControllerUpdateCallback update_callback) {
    std::scoped_lock lock{callback_mutex};
    callback_list.insert_or_assign(last_callback_key, std::move(update_callback));
    return last_callback_key++;
}

void EmulatedController::DeleteCallback(int key) {
    std::scoped_lock lock{callback_mutex};
    const auto& iterator = callback_list.find(key);
    if (iterator == callback_list.end()) {
        LOG_ERROR(Input, "Tried to delete non-existent callback {}", key);
        return;
    }
    callback_list.erase(iterator);
}

void EmulatedController::StatusUpdate() {
    turbo_button_state = (turbo_button_state + 1) % (TURBO_BUTTON_DELAY * 2);

    // Some drivers like key motion need constant refreshing
    for (std::size_t index = 0; index < motion_devices.size(); ++index) {
        const auto& raw_status = controller.motion_values[index].raw_status;
        auto& device = motion_devices[index];
        if (!raw_status.force_update) {
            continue;
        }
        if (!device) {
            continue;
        }
        device->ForceUpdate();
    }
}

NpadButton EmulatedController::GetTurboButtonMask() const {
    // Apply no mask when disabled
    if (turbo_button_state < TURBO_BUTTON_DELAY) {
        return {NpadButton::All};
    }

    NpadButtonState button_mask{};
    for (std::size_t index = 0; index < controller.button_values.size(); ++index) {
        if (!controller.button_values[index].turbo) {
            continue;
        }

        switch (index) {
        case Settings::NativeButton::A:
            button_mask.a.Assign(1);
            break;
        case Settings::NativeButton::B:
            button_mask.b.Assign(1);
            break;
        case Settings::NativeButton::X:
            button_mask.x.Assign(1);
            break;
        case Settings::NativeButton::Y:
            button_mask.y.Assign(1);
            break;
        case Settings::NativeButton::L:
            button_mask.l.Assign(1);
            break;
        case Settings::NativeButton::R:
            button_mask.r.Assign(1);
            break;
        case Settings::NativeButton::ZL:
            button_mask.zl.Assign(1);
            break;
        case Settings::NativeButton::ZR:
            button_mask.zr.Assign(1);
            break;
        case Settings::NativeButton::DLeft:
            button_mask.left.Assign(1);
            break;
        case Settings::NativeButton::DUp:
            button_mask.up.Assign(1);
            break;
        case Settings::NativeButton::DRight:
            button_mask.right.Assign(1);
            break;
        case Settings::NativeButton::DDown:
            button_mask.down.Assign(1);
            break;
        case Settings::NativeButton::SL:
            button_mask.left_sl.Assign(1);
            button_mask.right_sl.Assign(1);
            break;
        case Settings::NativeButton::SR:
            button_mask.left_sr.Assign(1);
            button_mask.right_sr.Assign(1);
            break;
        default:
            break;
        }
    }

    return static_cast<NpadButton>(~button_mask.raw);
}

} // namespace Core::HID
