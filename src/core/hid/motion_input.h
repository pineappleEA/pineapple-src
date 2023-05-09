// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/quaternion.h"
#include "common/vector_math.h"

namespace Core::HID {

class MotionInput {
public:
    static constexpr float ThresholdLoose = 0.01f;
    static constexpr float ThresholdStandard = 0.007f;
    static constexpr float ThresholdThight = 0.002f;

    static constexpr float IsAtRestRelaxed = 0.05f;
    static constexpr float IsAtRestLoose = 0.02f;
    static constexpr float IsAtRestStandard = 0.01f;
    static constexpr float IsAtRestThight = 0.005f;

    static constexpr float GyroMaxValue = 5.0f;
    static constexpr float AccelMaxValue = 7.0f;

    static constexpr std::size_t CalibrationSamples = 300;

    explicit MotionInput();

    MotionInput(const MotionInput&) = default;
    MotionInput& operator=(const MotionInput&) = default;

    MotionInput(MotionInput&&) = default;
    MotionInput& operator=(MotionInput&&) = default;

    void SetPID(f32 new_kp, f32 new_ki, f32 new_kd);
    void SetAcceleration(const Common::Vec3f& acceleration);
    void SetGyroscope(const Common::Vec3f& gyroscope);
    void SetQuaternion(const Common::Quaternion<f32>& quaternion);
    void SetEulerAngles(const Common::Vec3f& euler_angles);
    void SetGyroBias(const Common::Vec3f& bias);
    void SetGyroThreshold(f32 threshold);

    /// Applies a modifier on top of the normal gyro threshold
    void SetUserGyroThreshold(f32 threshold);

    void EnableReset(bool reset);
    void ResetRotations();
    void ResetQuaternion();

    void UpdateRotation(u64 elapsed_time);
    void UpdateOrientation(u64 elapsed_time);

    void Calibrate();

    [[nodiscard]] std::array<Common::Vec3f, 3> GetOrientation() const;
    [[nodiscard]] Common::Vec3f GetAcceleration() const;
    [[nodiscard]] Common::Vec3f GetGyroscope() const;
    [[nodiscard]] Common::Vec3f GetGyroBias() const;
    [[nodiscard]] Common::Vec3f GetRotations() const;
    [[nodiscard]] Common::Quaternion<f32> GetQuaternion() const;
    [[nodiscard]] Common::Vec3f GetEulerAngles() const;

    [[nodiscard]] bool IsMoving(f32 sensitivity) const;
    [[nodiscard]] bool IsCalibrated(f32 sensitivity) const;

private:
    void StopCalibration();
    void ResetOrientation();
    void SetOrientationFromAccelerometer();

    // PID constants
    f32 kp;
    f32 ki;
    f32 kd;

    // PID errors
    Common::Vec3f real_error;
    Common::Vec3f integral_error;
    Common::Vec3f derivative_error;

    // Quaternion containing the device orientation
    Common::Quaternion<f32> quat;

    // Number of full rotations in each axis
    Common::Vec3f rotations;

    // Acceleration vector measurement in G force
    Common::Vec3f accel;

    // Gyroscope vector measurement in radians/s.
    Common::Vec3f gyro;

    // Vector to be subtracted from gyro measurements
    Common::Vec3f gyro_bias;

    // Minimum gyro amplitude to detect if the device is moving
    f32 gyro_threshold = 0.0f;

    // Multiplies gyro_threshold by this value
    f32 user_gyro_threshold = 0.0f;

    // Number of invalid sequential data
    u32 reset_counter = 0;

    // If the provided data is invalid the device will be autocalibrated
    bool reset_enabled = true;

    // Use accelerometer values to calculate position
    bool only_accelerometer = true;

    // When enabled it will aggressively adjust for gyro drift
    bool calibration_mode = false;

    // Used to auto disable calibration mode
    std::size_t calibration_counter = 0;
};

} // namespace Core::HID
