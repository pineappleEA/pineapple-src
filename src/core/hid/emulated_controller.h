// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/input.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "common/vector_math.h"
#include "core/hid/hid_types.h"
#include "core/hid/irs_types.h"
#include "core/hid/motion_input.h"

namespace Core::HID {
const std::size_t max_emulated_controllers = 2;
const std::size_t output_devices_size = 4;
struct ControllerMotionInfo {
    Common::Input::MotionStatus raw_status{};
    MotionInput emulated{};
};

using ButtonDevices =
    std::array<std::unique_ptr<Common::Input::InputDevice>, Settings::NativeButton::NumButtons>;
using StickDevices =
    std::array<std::unique_ptr<Common::Input::InputDevice>, Settings::NativeAnalog::NumAnalogs>;
using ControllerMotionDevices =
    std::array<std::unique_ptr<Common::Input::InputDevice>, Settings::NativeMotion::NumMotions>;
using TriggerDevices =
    std::array<std::unique_ptr<Common::Input::InputDevice>, Settings::NativeTrigger::NumTriggers>;
using ColorDevices =
    std::array<std::unique_ptr<Common::Input::InputDevice>, max_emulated_controllers>;
using BatteryDevices =
    std::array<std::unique_ptr<Common::Input::InputDevice>, max_emulated_controllers>;
using CameraDevices =
    std::array<std::unique_ptr<Common::Input::InputDevice>, max_emulated_controllers>;
using RingAnalogDevices =
    std::array<std::unique_ptr<Common::Input::InputDevice>, max_emulated_controllers>;
using NfcDevices =
    std::array<std::unique_ptr<Common::Input::InputDevice>, max_emulated_controllers>;
using OutputDevices = std::array<std::unique_ptr<Common::Input::OutputDevice>, output_devices_size>;

using ButtonParams = std::array<Common::ParamPackage, Settings::NativeButton::NumButtons>;
using StickParams = std::array<Common::ParamPackage, Settings::NativeAnalog::NumAnalogs>;
using ControllerMotionParams = std::array<Common::ParamPackage, Settings::NativeMotion::NumMotions>;
using TriggerParams = std::array<Common::ParamPackage, Settings::NativeTrigger::NumTriggers>;
using ColorParams = std::array<Common::ParamPackage, max_emulated_controllers>;
using BatteryParams = std::array<Common::ParamPackage, max_emulated_controllers>;
using CameraParams = std::array<Common::ParamPackage, max_emulated_controllers>;
using RingAnalogParams = std::array<Common::ParamPackage, max_emulated_controllers>;
using NfcParams = std::array<Common::ParamPackage, max_emulated_controllers>;
using OutputParams = std::array<Common::ParamPackage, output_devices_size>;

using ButtonValues = std::array<Common::Input::ButtonStatus, Settings::NativeButton::NumButtons>;
using SticksValues = std::array<Common::Input::StickStatus, Settings::NativeAnalog::NumAnalogs>;
using TriggerValues =
    std::array<Common::Input::TriggerStatus, Settings::NativeTrigger::NumTriggers>;
using ControllerMotionValues = std::array<ControllerMotionInfo, Settings::NativeMotion::NumMotions>;
using ColorValues = std::array<Common::Input::BodyColorStatus, max_emulated_controllers>;
using BatteryValues = std::array<Common::Input::BatteryStatus, max_emulated_controllers>;
using CameraValues = Common::Input::CameraStatus;
using RingAnalogValue = Common::Input::AnalogStatus;
using NfcValues = Common::Input::NfcStatus;
using VibrationValues = std::array<Common::Input::VibrationStatus, max_emulated_controllers>;

struct AnalogSticks {
    AnalogStickState left{};
    AnalogStickState right{};
};

struct ControllerColors {
    NpadControllerColor fullkey{};
    NpadControllerColor left{};
    NpadControllerColor right{};
};

struct BatteryLevelState {
    NpadPowerInfo dual{};
    NpadPowerInfo left{};
    NpadPowerInfo right{};
};

struct CameraState {
    Core::IrSensor::ImageTransferProcessorFormat format{};
    std::vector<u8> data{};
    std::size_t sample{};
};

struct RingSensorForce {
    f32 force;
};

struct NfcState {
    Common::Input::NfcState state{};
    std::vector<u8> data{};
};

struct ControllerMotion {
    Common::Vec3f accel{};
    Common::Vec3f gyro{};
    Common::Vec3f rotation{};
    Common::Vec3f euler{};
    std::array<Common::Vec3f, 3> orientation{};
    bool is_at_rest{};
};

enum EmulatedDeviceIndex : u8 {
    LeftIndex,
    RightIndex,
    DualIndex,
    AllDevices,
};

using MotionState = std::array<ControllerMotion, 2>;

struct ControllerStatus {
    // Data from input_common
    ButtonValues button_values{};
    SticksValues stick_values{};
    ControllerMotionValues motion_values{};
    TriggerValues trigger_values{};
    ColorValues color_values{};
    BatteryValues battery_values{};
    VibrationValues vibration_values{};
    CameraValues camera_values{};
    RingAnalogValue ring_analog_value{};
    NfcValues nfc_values{};

    // Data for HID services
    HomeButtonState home_button_state{};
    CaptureButtonState capture_button_state{};
    NpadButtonState npad_button_state{};
    DebugPadButton debug_pad_button_state{};
    AnalogSticks analog_stick_state{};
    MotionState motion_state{};
    NpadGcTriggerState gc_trigger_state{};
    ControllerColors colors_state{};
    BatteryLevelState battery_state{};
    CameraState camera_state{};
    RingSensorForce ring_analog_state{};
    NfcState nfc_state{};
};

enum class ControllerTriggerType {
    Button,
    Stick,
    Trigger,
    Motion,
    Color,
    Battery,
    Vibration,
    IrSensor,
    RingController,
    Nfc,
    Connected,
    Disconnected,
    Type,
    All,
};

struct ControllerUpdateCallback {
    std::function<void(ControllerTriggerType)> on_change;
    bool is_npad_service;
};

class EmulatedController {
public:
    /**
     * Contains all input data (buttons, joysticks, vibration, and motion) within this controller.
     * @param npad_id_type npad id type for this specific controller
     */
    explicit EmulatedController(NpadIdType npad_id_type_);
    ~EmulatedController();

    YUZU_NON_COPYABLE(EmulatedController);
    YUZU_NON_MOVEABLE(EmulatedController);

    /// Converts the controller type from settings to npad type
    static NpadStyleIndex MapSettingsTypeToNPad(Settings::ControllerType type);

    /// Converts npad type to the equivalent of controller type from settings
    static Settings::ControllerType MapNPadToSettingsType(NpadStyleIndex type);

    /// Gets the NpadIdType for this controller
    NpadIdType GetNpadIdType() const;

    /// Sets the NpadStyleIndex for this controller
    void SetNpadStyleIndex(NpadStyleIndex npad_type_);

    /**
     * Gets the NpadStyleIndex for this controller
     * @param get_temporary_value If true tmp_npad_type will be returned
     * @return NpadStyleIndex set on the controller
     */
    NpadStyleIndex GetNpadStyleIndex(bool get_temporary_value = false) const;

    /**
     * Sets the supported controller types. Disconnects the controller if current type is not
     * supported
     * @param supported_styles bitflag with supported types
     */
    void SetSupportedNpadStyleTag(NpadStyleTag supported_styles);

    /**
     * Sets the connected status to true
     * @param use_temporary_value If true tmp_npad_type will be used
     */
    void Connect(bool use_temporary_value = false);

    /// Sets the connected status to false
    void Disconnect();

    /**
     * Is the emulated connected
     * @param get_temporary_value If true tmp_is_connected will be returned
     * @return true if the controller has the connected status
     */
    bool IsConnected(bool get_temporary_value = false) const;

    /// Removes all callbacks created from input devices
    void UnloadInput();

    /**
     * Sets the emulated controller into configuring mode
     * This prevents the modification of the HID state of the emulated controller by input commands
     */
    void EnableConfiguration();

    /// Returns the emulated controller into normal mode, allowing the modification of the HID state
    void DisableConfiguration();

    /// Enables Home and Screenshot buttons
    void EnableSystemButtons();

    /// Disables Home and Screenshot buttons
    void DisableSystemButtons();

    /// Sets Home and Screenshot buttons to false
    void ResetSystemButtons();

    /// Returns true if the emulated controller is in configuring mode
    bool IsConfiguring() const;

    /// Reload all input devices
    void ReloadInput();

    /// Overrides current mapped devices with the stored configuration and reloads all input devices
    void ReloadFromSettings();

    /// Saves the current mapped configuration
    void SaveCurrentConfig();

    /// Reverts any mapped changes made that weren't saved
    void RestoreConfig();

    /// Returns a vector of mapped devices from the mapped button and stick parameters
    std::vector<Common::ParamPackage> GetMappedDevices() const;

    // Returns the current mapped button device
    Common::ParamPackage GetButtonParam(std::size_t index) const;

    // Returns the current mapped stick device
    Common::ParamPackage GetStickParam(std::size_t index) const;

    // Returns the current mapped motion device
    Common::ParamPackage GetMotionParam(std::size_t index) const;

    /**
     * Updates the current mapped button device
     * @param param ParamPackage with controller data to be mapped
     */
    void SetButtonParam(std::size_t index, Common::ParamPackage param);

    /**
     * Updates the current mapped stick device
     * @param param ParamPackage with controller data to be mapped
     */
    void SetStickParam(std::size_t index, Common::ParamPackage param);

    /**
     * Updates the current mapped motion device
     * @param param ParamPackage with controller data to be mapped
     */
    void SetMotionParam(std::size_t index, Common::ParamPackage param);

    /// Returns the latest button status from the controller with parameters
    ButtonValues GetButtonsValues() const;

    /// Returns the latest analog stick status from the controller with parameters
    SticksValues GetSticksValues() const;

    /// Returns the latest trigger status from the controller with parameters
    TriggerValues GetTriggersValues() const;

    /// Returns the latest motion status from the controller with parameters
    ControllerMotionValues GetMotionValues() const;

    /// Returns the latest color status from the controller with parameters
    ColorValues GetColorsValues() const;

    /// Returns the latest battery status from the controller with parameters
    BatteryValues GetBatteryValues() const;

    /// Returns the latest camera status from the controller with parameters
    CameraValues GetCameraValues() const;

    /// Returns the latest status of analog input from the ring sensor with parameters
    RingAnalogValue GetRingSensorValues() const;

    /// Returns the latest status of button input for the hid::HomeButton service
    HomeButtonState GetHomeButtons() const;

    /// Returns the latest status of button input for the hid::CaptureButton service
    CaptureButtonState GetCaptureButtons() const;

    /// Returns the latest status of button input for the hid::Npad service
    NpadButtonState GetNpadButtons() const;

    /// Returns the latest status of button input for the debug pad service
    DebugPadButton GetDebugPadButtons() const;

    /// Returns the latest status of stick input from the mouse
    AnalogSticks GetSticks() const;

    /// Returns the latest status of trigger input from the mouse
    NpadGcTriggerState GetTriggers() const;

    /// Returns the latest status of motion input from the mouse
    MotionState GetMotions() const;

    /// Returns the latest color value from the controller
    ControllerColors GetColors() const;

    /// Returns the latest battery status from the controller
    BatteryLevelState GetBattery() const;

    /// Returns the latest camera status from the controller
    const CameraState& GetCamera() const;

    /// Returns the latest ringcon force sensor value
    RingSensorForce GetRingSensorForce() const;

    /// Returns the latest ntag status from the controller
    const NfcState& GetNfc() const;

    /**
     * Sends a specific vibration to the output device
     * @return true if vibration had no errors
     */
    bool SetVibration(std::size_t device_index, VibrationValue vibration);

    /**
     * Sends a small vibration to the output device
     * @return true if SetVibration was successful
     */
    bool IsVibrationEnabled(std::size_t device_index);

    /**
     * Sets the desired data to be polled from a controller
     * @param device_index index of the controller to set the polling mode
     * @param polling_mode type of input desired buttons, gyro, nfc, ir, etc.
     * @return driver result from this command
     */
    Common::Input::DriverResult SetPollingMode(EmulatedDeviceIndex device_index,
                                               Common::Input::PollingMode polling_mode);

    /**
     * Sets the desired camera format to be polled from a controller
     * @param camera_format size of each frame
     * @return true if SetCameraFormat was successful
     */
    bool SetCameraFormat(Core::IrSensor::ImageTransferProcessorFormat camera_format);

    // Returns the current mapped ring device
    Common::ParamPackage GetRingParam() const;

    /**
     * Updates the current mapped ring device
     * @param param ParamPackage with ring sensor data to be mapped
     */
    void SetRingParam(Common::ParamPackage param);

    /// Returns true if the device has nfc support
    bool HasNfc() const;

    /// Returns true if the nfc tag was written
    bool WriteNfc(const std::vector<u8>& data);

    /// Returns the led pattern corresponding to this emulated controller
    LedPattern GetLedPattern() const;

    /// Asks the output device to change the player led pattern
    void SetLedPattern();

    /// Changes sensitivity of the motion sensor
    void SetGyroscopeZeroDriftMode(GyroscopeZeroDriftMode mode);

    /**
     * Adds a callback to the list of events
     * @param update_callback A ConsoleUpdateCallback that will be triggered
     * @return an unique key corresponding to the callback index in the list
     */
    int SetCallback(ControllerUpdateCallback update_callback);

    /**
     * Removes a callback from the list stopping any future events to this object
     * @param key Key corresponding to the callback index in the list
     */
    void DeleteCallback(int key);

    /// Swaps the state of the turbo buttons and updates motion input
    void StatusUpdate();

private:
    /// creates input devices from params
    void LoadDevices();

    /// Set the params for TAS devices
    void LoadTASParams();

    /// Set the params for virtual pad devices
    void LoadVirtualGamepadParams();

    /**
     * @param use_temporary_value If true tmp_npad_type will be used
     * @return true if the controller style is fullkey
     */
    bool IsControllerFullkey(bool use_temporary_value = false) const;

    /**
     * Checks the current controller type against the supported_style_tag
     * @param use_temporary_value If true tmp_npad_type will be used
     * @return true if the controller is supported
     */
    bool IsControllerSupported(bool use_temporary_value = false) const;

    /**
     * Updates the button status of the controller
     * @param callback A CallbackStatus containing the button status
     * @param index Button ID of the to be updated
     */
    void SetButton(const Common::Input::CallbackStatus& callback, std::size_t index,
                   Common::UUID uuid);

    /**
     * Updates the analog stick status of the controller
     * @param callback A CallbackStatus containing the analog stick status
     * @param index stick ID of the to be updated
     */
    void SetStick(const Common::Input::CallbackStatus& callback, std::size_t index,
                  Common::UUID uuid);

    /**
     * Updates the trigger status of the controller
     * @param callback A CallbackStatus containing the trigger status
     * @param index trigger ID of the to be updated
     */
    void SetTrigger(const Common::Input::CallbackStatus& callback, std::size_t index,
                    Common::UUID uuid);

    /**
     * Updates the motion status of the controller
     * @param callback A CallbackStatus containing gyro and accelerometer data
     * @param index motion ID of the to be updated
     */
    void SetMotion(const Common::Input::CallbackStatus& callback, std::size_t index);

    /**
     * Updates the color status of the controller
     * @param callback A CallbackStatus containing the color status
     * @param index color ID of the to be updated
     */
    void SetColors(const Common::Input::CallbackStatus& callback, std::size_t index);

    /**
     * Updates the battery status of the controller
     * @param callback A CallbackStatus containing the battery status
     * @param index battery ID of the to be updated
     */
    void SetBattery(const Common::Input::CallbackStatus& callback, std::size_t index);

    /**
     * Updates the camera status of the controller
     * @param callback A CallbackStatus containing the camera status
     */
    void SetCamera(const Common::Input::CallbackStatus& callback);

    /**
     * Updates the ring analog sensor status of the ring controller
     * @param callback A CallbackStatus containing the force status
     */
    void SetRingAnalog(const Common::Input::CallbackStatus& callback);

    /**
     * Updates the nfc status of the controller
     * @param callback A CallbackStatus containing the nfc status
     */
    void SetNfc(const Common::Input::CallbackStatus& callback);

    /**
     * Converts a color format from bgra to rgba
     * @param color in bgra format
     * @return NpadColor in rgba format
     */
    NpadColor GetNpadColor(u32 color);

    /**
     * Triggers a callback that something has changed on the controller status
     * @param type Input type of the event to trigger
     * @param is_service_update indicates if this event should only be sent to HID services
     */
    void TriggerOnChange(ControllerTriggerType type, bool is_service_update);

    NpadButton GetTurboButtonMask() const;

    const NpadIdType npad_id_type;
    NpadStyleIndex npad_type{NpadStyleIndex::None};
    NpadStyleIndex original_npad_type{NpadStyleIndex::None};
    NpadStyleTag supported_style_tag{NpadStyleSet::All};
    bool is_connected{false};
    bool is_configuring{false};
    bool system_buttons_enabled{true};
    f32 motion_sensitivity{Core::HID::MotionInput::IsAtRestStandard};
    u32 turbo_button_state{0};

    // Temporary values to avoid doing changes while the controller is in configuring mode
    NpadStyleIndex tmp_npad_type{NpadStyleIndex::None};
    bool tmp_is_connected{false};

    ButtonParams button_params;
    StickParams stick_params;
    ControllerMotionParams motion_params;
    TriggerParams trigger_params;
    BatteryParams battery_params;
    ColorParams color_params;
    CameraParams camera_params;
    RingAnalogParams ring_params;
    NfcParams nfc_params;
    OutputParams output_params;

    ButtonDevices button_devices;
    StickDevices stick_devices;
    ControllerMotionDevices motion_devices;
    TriggerDevices trigger_devices;
    BatteryDevices battery_devices;
    ColorDevices color_devices;
    CameraDevices camera_devices;
    RingAnalogDevices ring_analog_devices;
    NfcDevices nfc_devices;
    OutputDevices output_devices;

    // TAS related variables
    ButtonParams tas_button_params;
    StickParams tas_stick_params;
    ButtonDevices tas_button_devices;
    StickDevices tas_stick_devices;

    // Virtual gamepad related variables
    ButtonParams virtual_button_params;
    StickParams virtual_stick_params;
    ButtonDevices virtual_button_devices;
    StickDevices virtual_stick_devices;

    mutable std::mutex mutex;
    mutable std::mutex callback_mutex;
    std::unordered_map<int, ControllerUpdateCallback> callback_list;
    int last_callback_key = 0;

    // Stores the current status of all controller input
    ControllerStatus controller;
};

} // namespace Core::HID
