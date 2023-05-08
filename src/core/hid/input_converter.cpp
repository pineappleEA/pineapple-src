// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <random>

#include "common/input.h"
#include "core/hid/input_converter.h"

namespace Core::HID {

Common::Input::BatteryStatus TransformToBattery(const Common::Input::CallbackStatus& callback) {
    Common::Input::BatteryStatus battery{Common::Input::BatteryStatus::None};
    switch (callback.type) {
    case Common::Input::InputType::Analog:
    case Common::Input::InputType::Trigger: {
        const auto value = TransformToTrigger(callback).analog.value;
        battery = Common::Input::BatteryLevel::Empty;
        if (value > 0.2f) {
            battery = Common::Input::BatteryLevel::Critical;
        }
        if (value > 0.4f) {
            battery = Common::Input::BatteryLevel::Low;
        }
        if (value > 0.6f) {
            battery = Common::Input::BatteryLevel::Medium;
        }
        if (value > 0.8f) {
            battery = Common::Input::BatteryLevel::Full;
        }
        if (value >= 0.95f) {
            battery = Common::Input::BatteryLevel::Charging;
        }
        break;
    }
    case Common::Input::InputType::Button:
        battery = callback.button_status.value ? Common::Input::BatteryLevel::Charging
                                               : Common::Input::BatteryLevel::Critical;
        break;
    case Common::Input::InputType::Battery:
        battery = callback.battery_status;
        break;
    default:
        LOG_ERROR(Input, "Conversion from type {} to battery not implemented", callback.type);
        break;
    }

    return battery;
}

Common::Input::ButtonStatus TransformToButton(const Common::Input::CallbackStatus& callback) {
    Common::Input::ButtonStatus status{};
    switch (callback.type) {
    case Common::Input::InputType::Analog:
        status.value = TransformToTrigger(callback).pressed.value;
        status.toggle = callback.analog_status.properties.toggle;
        status.inverted = callback.analog_status.properties.inverted_button;
        break;
    case Common::Input::InputType::Trigger:
        status.value = TransformToTrigger(callback).pressed.value;
        break;
    case Common::Input::InputType::Button:
        status = callback.button_status;
        break;
    case Common::Input::InputType::Motion:
        status.value = std::abs(callback.motion_status.gyro.x.raw_value) > 1.0f;
        break;
    default:
        LOG_ERROR(Input, "Conversion from type {} to button not implemented", callback.type);
        break;
    }

    if (status.inverted) {
        status.value = !status.value;
    }

    return status;
}

Common::Input::MotionStatus TransformToMotion(const Common::Input::CallbackStatus& callback) {
    Common::Input::MotionStatus status{};
    switch (callback.type) {
    case Common::Input::InputType::Button: {
        Common::Input::AnalogProperties properties{
            .deadzone = 0.0f,
            .range = 1.0f,
            .offset = 0.0f,
        };
        status.delta_timestamp = 1000;
        status.force_update = true;
        status.accel.x = {
            .value = 0.0f,
            .raw_value = 0.0f,
            .properties = properties,
        };
        status.accel.y = {
            .value = 0.0f,
            .raw_value = 0.0f,
            .properties = properties,
        };
        status.accel.z = {
            .value = 0.0f,
            .raw_value = -1.0f,
            .properties = properties,
        };
        status.gyro.x = {
            .value = 0.0f,
            .raw_value = 0.0f,
            .properties = properties,
        };
        status.gyro.y = {
            .value = 0.0f,
            .raw_value = 0.0f,
            .properties = properties,
        };
        status.gyro.z = {
            .value = 0.0f,
            .raw_value = 0.0f,
            .properties = properties,
        };
        if (TransformToButton(callback).value) {
            std::random_device device;
            std::mt19937 gen(device());
            std::uniform_int_distribution<s16> distribution(-5000, 5000);
            status.accel.x.raw_value = static_cast<f32>(distribution(gen)) * 0.001f;
            status.accel.y.raw_value = static_cast<f32>(distribution(gen)) * 0.001f;
            status.accel.z.raw_value = static_cast<f32>(distribution(gen)) * 0.001f;
            status.gyro.x.raw_value = static_cast<f32>(distribution(gen)) * 0.001f;
            status.gyro.y.raw_value = static_cast<f32>(distribution(gen)) * 0.001f;
            status.gyro.z.raw_value = static_cast<f32>(distribution(gen)) * 0.001f;
        }
        break;
    }
    case Common::Input::InputType::Motion:
        status = callback.motion_status;
        break;
    default:
        LOG_ERROR(Input, "Conversion from type {} to motion not implemented", callback.type);
        break;
    }
    SanitizeAnalog(status.accel.x, false);
    SanitizeAnalog(status.accel.y, false);
    SanitizeAnalog(status.accel.z, false);
    SanitizeAnalog(status.gyro.x, false);
    SanitizeAnalog(status.gyro.y, false);
    SanitizeAnalog(status.gyro.z, false);

    return status;
}

Common::Input::StickStatus TransformToStick(const Common::Input::CallbackStatus& callback) {
    Common::Input::StickStatus status{};

    switch (callback.type) {
    case Common::Input::InputType::Stick:
        status = callback.stick_status;
        break;
    default:
        LOG_ERROR(Input, "Conversion from type {} to stick not implemented", callback.type);
        break;
    }

    SanitizeStick(status.x, status.y, true);
    const auto& properties_x = status.x.properties;
    const auto& properties_y = status.y.properties;
    const float x = status.x.value;
    const float y = status.y.value;

    // Set directional buttons
    status.right = x > properties_x.threshold;
    status.left = x < -properties_x.threshold;
    status.up = y > properties_y.threshold;
    status.down = y < -properties_y.threshold;

    return status;
}

Common::Input::TouchStatus TransformToTouch(const Common::Input::CallbackStatus& callback) {
    Common::Input::TouchStatus status{};

    switch (callback.type) {
    case Common::Input::InputType::Touch:
        status = callback.touch_status;
        break;
    case Common::Input::InputType::Stick:
        status.x = callback.stick_status.x;
        status.y = callback.stick_status.y;
        break;
    default:
        LOG_ERROR(Input, "Conversion from type {} to touch not implemented", callback.type);
        break;
    }

    SanitizeAnalog(status.x, true);
    SanitizeAnalog(status.y, true);
    float& x = status.x.value;
    float& y = status.y.value;

    // Adjust if value is inverted
    x = status.x.properties.inverted ? 1.0f + x : x;
    y = status.y.properties.inverted ? 1.0f + y : y;

    // clamp value
    x = std::clamp(x, 0.0f, 1.0f);
    y = std::clamp(y, 0.0f, 1.0f);

    if (status.pressed.inverted) {
        status.pressed.value = !status.pressed.value;
    }

    return status;
}

Common::Input::TriggerStatus TransformToTrigger(const Common::Input::CallbackStatus& callback) {
    Common::Input::TriggerStatus status{};
    float& raw_value = status.analog.raw_value;
    bool calculate_button_value = true;

    switch (callback.type) {
    case Common::Input::InputType::Analog:
        status.analog.properties = callback.analog_status.properties;
        raw_value = callback.analog_status.raw_value;
        break;
    case Common::Input::InputType::Button:
        status.analog.properties.range = 1.0f;
        status.analog.properties.inverted = callback.button_status.inverted;
        raw_value = callback.button_status.value ? 1.0f : 0.0f;
        break;
    case Common::Input::InputType::Trigger:
        status = callback.trigger_status;
        calculate_button_value = false;
        break;
    case Common::Input::InputType::Motion:
        status.analog.properties.range = 1.0f;
        raw_value = callback.motion_status.accel.x.raw_value;
        break;
    default:
        LOG_ERROR(Input, "Conversion from type {} to trigger not implemented", callback.type);
        break;
    }

    SanitizeAnalog(status.analog, true);
    const auto& properties = status.analog.properties;
    float& value = status.analog.value;

    // Set button status
    if (calculate_button_value) {
        status.pressed.value = value > properties.threshold;
    }

    // Adjust if value is inverted
    value = properties.inverted ? 1.0f + value : value;

    // clamp value
    value = std::clamp(value, 0.0f, 1.0f);

    return status;
}

Common::Input::AnalogStatus TransformToAnalog(const Common::Input::CallbackStatus& callback) {
    Common::Input::AnalogStatus status{};

    switch (callback.type) {
    case Common::Input::InputType::Analog:
        status.properties = callback.analog_status.properties;
        status.raw_value = callback.analog_status.raw_value;
        break;
    default:
        LOG_ERROR(Input, "Conversion from type {} to analog not implemented", callback.type);
        break;
    }

    SanitizeAnalog(status, false);

    // Adjust if value is inverted
    status.value = status.properties.inverted ? -status.value : status.value;

    return status;
}

Common::Input::CameraStatus TransformToCamera(const Common::Input::CallbackStatus& callback) {
    Common::Input::CameraStatus camera{};
    switch (callback.type) {
    case Common::Input::InputType::IrSensor:
        camera = {
            .format = callback.camera_status,
            .data = callback.raw_data,
        };
        break;
    default:
        LOG_ERROR(Input, "Conversion from type {} to camera not implemented", callback.type);
        break;
    }

    return camera;
}

Common::Input::NfcStatus TransformToNfc(const Common::Input::CallbackStatus& callback) {
    Common::Input::NfcStatus nfc{};
    switch (callback.type) {
    case Common::Input::InputType::Nfc:
        nfc = {
            .state = callback.nfc_status,
            .data = callback.raw_data,
        };
        break;
    default:
        LOG_ERROR(Input, "Conversion from type {} to NFC not implemented", callback.type);
        break;
    }

    return nfc;
}

Common::Input::BodyColorStatus TransformToColor(const Common::Input::CallbackStatus& callback) {
    switch (callback.type) {
    case Common::Input::InputType::Color:
        return callback.color_status;
        break;
    default:
        LOG_ERROR(Input, "Conversion from type {} to color not implemented", callback.type);
        return {};
        break;
    }
}

void SanitizeAnalog(Common::Input::AnalogStatus& analog, bool clamp_value) {
    const auto& properties = analog.properties;
    float& raw_value = analog.raw_value;
    float& value = analog.value;

    if (!std::isnormal(raw_value)) {
        raw_value = 0;
    }

    // Apply center offset
    raw_value -= properties.offset;

    // Set initial values to be formatted
    value = raw_value;

    // Calculate vector size
    const float r = std::abs(value);

    // Return zero if value is smaller than the deadzone
    if (r <= properties.deadzone || properties.deadzone == 1.0f) {
        analog.value = 0;
        return;
    }

    // Adjust range of value
    const float deadzone_factor =
        1.0f / r * (r - properties.deadzone) / (1.0f - properties.deadzone);
    value = value * deadzone_factor / properties.range;

    // Invert direction if needed
    if (properties.inverted) {
        value = -value;
    }

    // Clamp value
    if (clamp_value) {
        value = std::clamp(value, -1.0f, 1.0f);
    }
}

void SanitizeStick(Common::Input::AnalogStatus& analog_x, Common::Input::AnalogStatus& analog_y,
                   bool clamp_value) {
    const auto& properties_x = analog_x.properties;
    const auto& properties_y = analog_y.properties;
    float& raw_x = analog_x.raw_value;
    float& raw_y = analog_y.raw_value;
    float& x = analog_x.value;
    float& y = analog_y.value;

    if (!std::isnormal(raw_x)) {
        raw_x = 0;
    }
    if (!std::isnormal(raw_y)) {
        raw_y = 0;
    }

    // Apply center offset
    raw_x += properties_x.offset;
    raw_y += properties_y.offset;

    // Apply X scale correction from offset
    if (std::abs(properties_x.offset) < 0.75f) {
        if (raw_x > 0) {
            raw_x /= 1 + properties_x.offset;
        } else {
            raw_x /= 1 - properties_x.offset;
        }
    }

    // Apply Y scale correction from offset
    if (std::abs(properties_y.offset) < 0.75f) {
        if (raw_y > 0) {
            raw_y /= 1 + properties_y.offset;
        } else {
            raw_y /= 1 - properties_y.offset;
        }
    }

    // Invert direction if needed
    raw_x = properties_x.inverted ? -raw_x : raw_x;
    raw_y = properties_y.inverted ? -raw_y : raw_y;

    // Set initial values to be formatted
    x = raw_x;
    y = raw_y;

    // Calculate vector size
    float r = x * x + y * y;
    r = std::sqrt(r);

    // TODO(German77): Use deadzone and range of both axis

    // Return zero if values are smaller than the deadzone
    if (r <= properties_x.deadzone || properties_x.deadzone >= 1.0f) {
        x = 0;
        y = 0;
        return;
    }

    // Adjust range of joystick
    const float deadzone_factor =
        1.0f / r * (r - properties_x.deadzone) / (1.0f - properties_x.deadzone);
    x = x * deadzone_factor / properties_x.range;
    y = y * deadzone_factor / properties_x.range;
    r = r * deadzone_factor / properties_x.range;

    // Normalize joystick
    if (clamp_value && r > 1.0f) {
        x /= r;
        y /= r;
    }
}

} // namespace Core::HID
