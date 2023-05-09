// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>

#include "common/math_util.h"
#include "core/hid/motion_input.h"

namespace Core::HID {

MotionInput::MotionInput() {
    // Initialize PID constants with default values
    SetPID(0.3f, 0.005f, 0.0f);
    SetGyroThreshold(ThresholdStandard);
    ResetQuaternion();
    ResetRotations();
}

void MotionInput::SetPID(f32 new_kp, f32 new_ki, f32 new_kd) {
    kp = new_kp;
    ki = new_ki;
    kd = new_kd;
}

void MotionInput::SetAcceleration(const Common::Vec3f& acceleration) {
    accel = acceleration;

    accel.x = std::clamp(accel.x, -AccelMaxValue, AccelMaxValue);
    accel.y = std::clamp(accel.y, -AccelMaxValue, AccelMaxValue);
    accel.z = std::clamp(accel.z, -AccelMaxValue, AccelMaxValue);
}

void MotionInput::SetGyroscope(const Common::Vec3f& gyroscope) {
    gyro = gyroscope - gyro_bias;

    gyro.x = std::clamp(gyro.x, -GyroMaxValue, GyroMaxValue);
    gyro.y = std::clamp(gyro.y, -GyroMaxValue, GyroMaxValue);
    gyro.z = std::clamp(gyro.z, -GyroMaxValue, GyroMaxValue);

    // Auto adjust gyro_bias to minimize drift
    if (!IsMoving(IsAtRestRelaxed)) {
        gyro_bias = (gyro_bias * 0.9999f) + (gyroscope * 0.0001f);
    }

    // Adjust drift when calibration mode is enabled
    if (calibration_mode) {
        gyro_bias = (gyro_bias * 0.99f) + (gyroscope * 0.01f);
        StopCalibration();
    }

    if (gyro.Length() < gyro_threshold * user_gyro_threshold) {
        gyro = {};
    } else {
        only_accelerometer = false;
    }
}

void MotionInput::SetQuaternion(const Common::Quaternion<f32>& quaternion) {
    quat = quaternion;
}

void MotionInput::SetEulerAngles(const Common::Vec3f& euler_angles) {
    const float cr = std::cos(euler_angles.x * 0.5f);
    const float sr = std::sin(euler_angles.x * 0.5f);
    const float cp = std::cos(euler_angles.y * 0.5f);
    const float sp = std::sin(euler_angles.y * 0.5f);
    const float cy = std::cos(euler_angles.z * 0.5f);
    const float sy = std::sin(euler_angles.z * 0.5f);

    quat.w = cr * cp * cy + sr * sp * sy;
    quat.xyz.x = sr * cp * cy - cr * sp * sy;
    quat.xyz.y = cr * sp * cy + sr * cp * sy;
    quat.xyz.z = cr * cp * sy - sr * sp * cy;
}

void MotionInput::SetGyroBias(const Common::Vec3f& bias) {
    gyro_bias = bias;
}

void MotionInput::SetGyroThreshold(f32 threshold) {
    gyro_threshold = threshold;
}

void MotionInput::SetUserGyroThreshold(f32 threshold) {
    user_gyro_threshold = threshold / ThresholdStandard;
}

void MotionInput::EnableReset(bool reset) {
    reset_enabled = reset;
}

void MotionInput::ResetRotations() {
    rotations = {};
}

void MotionInput::ResetQuaternion() {
    quat = {{0.0f, 0.0f, -1.0f}, 0.0f};
}

bool MotionInput::IsMoving(f32 sensitivity) const {
    return gyro.Length() >= sensitivity || accel.Length() <= 0.9f || accel.Length() >= 1.1f;
}

bool MotionInput::IsCalibrated(f32 sensitivity) const {
    return real_error.Length() < sensitivity;
}

void MotionInput::UpdateRotation(u64 elapsed_time) {
    const auto sample_period = static_cast<f32>(elapsed_time) / 1000000.0f;
    if (sample_period > 0.1f) {
        return;
    }
    rotations += gyro * sample_period;
}

void MotionInput::Calibrate() {
    calibration_mode = true;
    calibration_counter = 0;
}

void MotionInput::StopCalibration() {
    if (calibration_counter++ > CalibrationSamples) {
        calibration_mode = false;
        ResetQuaternion();
        ResetRotations();
    }
}

// Based on Madgwick's implementation of Mayhony's AHRS algorithm.
// https://github.com/xioTechnologies/Open-Source-AHRS-With-x-IMU/blob/master/x-IMU%20IMU%20and%20AHRS%20Algorithms/x-IMU%20IMU%20and%20AHRS%20Algorithms/AHRS/MahonyAHRS.cs
void MotionInput::UpdateOrientation(u64 elapsed_time) {
    if (!IsCalibrated(0.1f)) {
        ResetOrientation();
    }
    // Short name local variable for readability
    f32 q1 = quat.w;
    f32 q2 = quat.xyz[0];
    f32 q3 = quat.xyz[1];
    f32 q4 = quat.xyz[2];
    const auto sample_period = static_cast<f32>(elapsed_time) / 1000000.0f;

    // Ignore invalid elapsed time
    if (sample_period > 0.1f) {
        return;
    }

    const auto normal_accel = accel.Normalized();
    auto rad_gyro = gyro * Common::PI * 2;
    const f32 swap = rad_gyro.x;
    rad_gyro.x = rad_gyro.y;
    rad_gyro.y = -swap;
    rad_gyro.z = -rad_gyro.z;

    // Clear gyro values if there is no gyro present
    if (only_accelerometer) {
        rad_gyro.x = 0;
        rad_gyro.y = 0;
        rad_gyro.z = 0;
    }

    // Ignore drift correction if acceleration is not reliable
    if (accel.Length() >= 0.75f && accel.Length() <= 1.25f) {
        const f32 ax = -normal_accel.x;
        const f32 ay = normal_accel.y;
        const f32 az = -normal_accel.z;

        // Estimated direction of gravity
        const f32 vx = 2.0f * (q2 * q4 - q1 * q3);
        const f32 vy = 2.0f * (q1 * q2 + q3 * q4);
        const f32 vz = q1 * q1 - q2 * q2 - q3 * q3 + q4 * q4;

        // Error is cross product between estimated direction and measured direction of gravity
        const Common::Vec3f new_real_error = {
            az * vx - ax * vz,
            ay * vz - az * vy,
            ax * vy - ay * vx,
        };

        derivative_error = new_real_error - real_error;
        real_error = new_real_error;

        // Prevent integral windup
        if (ki != 0.0f && !IsCalibrated(0.05f)) {
            integral_error += real_error;
        } else {
            integral_error = {};
        }

        // Apply feedback terms
        if (!only_accelerometer) {
            rad_gyro += kp * real_error;
            rad_gyro += ki * integral_error;
            rad_gyro += kd * derivative_error;
        } else {
            // Give more weight to accelerometer values to compensate for the lack of gyro
            rad_gyro += 35.0f * kp * real_error;
            rad_gyro += 10.0f * ki * integral_error;
            rad_gyro += 10.0f * kd * derivative_error;

            // Emulate gyro values for games that need them
            gyro.x = -rad_gyro.y;
            gyro.y = rad_gyro.x;
            gyro.z = -rad_gyro.z;
            UpdateRotation(elapsed_time);
        }
    }

    const f32 gx = rad_gyro.y;
    const f32 gy = rad_gyro.x;
    const f32 gz = rad_gyro.z;

    // Integrate rate of change of quaternion
    const f32 pa = q2;
    const f32 pb = q3;
    const f32 pc = q4;
    q1 = q1 + (-q2 * gx - q3 * gy - q4 * gz) * (0.5f * sample_period);
    q2 = pa + (q1 * gx + pb * gz - pc * gy) * (0.5f * sample_period);
    q3 = pb + (q1 * gy - pa * gz + pc * gx) * (0.5f * sample_period);
    q4 = pc + (q1 * gz + pa * gy - pb * gx) * (0.5f * sample_period);

    quat.w = q1;
    quat.xyz[0] = q2;
    quat.xyz[1] = q3;
    quat.xyz[2] = q4;
    quat = quat.Normalized();
}

std::array<Common::Vec3f, 3> MotionInput::GetOrientation() const {
    const Common::Quaternion<float> quad{
        .xyz = {-quat.xyz[1], -quat.xyz[0], -quat.w},
        .w = -quat.xyz[2],
    };
    const std::array<float, 16> matrix4x4 = quad.ToMatrix();

    return {Common::Vec3f(matrix4x4[0], matrix4x4[1], -matrix4x4[2]),
            Common::Vec3f(matrix4x4[4], matrix4x4[5], -matrix4x4[6]),
            Common::Vec3f(-matrix4x4[8], -matrix4x4[9], matrix4x4[10])};
}

Common::Vec3f MotionInput::GetAcceleration() const {
    return accel;
}

Common::Vec3f MotionInput::GetGyroscope() const {
    return gyro;
}

Common::Vec3f MotionInput::GetGyroBias() const {
    return gyro_bias;
}

Common::Quaternion<f32> MotionInput::GetQuaternion() const {
    return quat;
}

Common::Vec3f MotionInput::GetRotations() const {
    return rotations;
}

Common::Vec3f MotionInput::GetEulerAngles() const {
    // roll (x-axis rotation)
    const float sinr_cosp = 2 * (quat.w * quat.xyz.x + quat.xyz.y * quat.xyz.z);
    const float cosr_cosp = 1 - 2 * (quat.xyz.x * quat.xyz.x + quat.xyz.y * quat.xyz.y);

    // pitch (y-axis rotation)
    const float sinp = std::sqrt(1 + 2 * (quat.w * quat.xyz.y - quat.xyz.x * quat.xyz.z));
    const float cosp = std::sqrt(1 - 2 * (quat.w * quat.xyz.y - quat.xyz.x * quat.xyz.z));

    // yaw (z-axis rotation)
    const float siny_cosp = 2 * (quat.w * quat.xyz.z + quat.xyz.x * quat.xyz.y);
    const float cosy_cosp = 1 - 2 * (quat.xyz.y * quat.xyz.y + quat.xyz.z * quat.xyz.z);

    return {
        std::atan2(sinr_cosp, cosr_cosp),
        2 * std::atan2(sinp, cosp) - Common::PI / 2,
        std::atan2(siny_cosp, cosy_cosp),
    };
}

void MotionInput::ResetOrientation() {
    if (!reset_enabled || only_accelerometer) {
        return;
    }
    if (!IsMoving(IsAtRestRelaxed) && accel.z <= -0.9f) {
        ++reset_counter;
        if (reset_counter > 900) {
            quat.w = 0;
            quat.xyz[0] = 0;
            quat.xyz[1] = 0;
            quat.xyz[2] = -1;
            SetOrientationFromAccelerometer();
            integral_error = {};
            reset_counter = 0;
        }
    } else {
        reset_counter = 0;
    }
}

void MotionInput::SetOrientationFromAccelerometer() {
    int iterations = 0;
    const f32 sample_period = 0.015f;

    const auto normal_accel = accel.Normalized();

    while (!IsCalibrated(0.01f) && ++iterations < 100) {
        // Short name local variable for readability
        f32 q1 = quat.w;
        f32 q2 = quat.xyz[0];
        f32 q3 = quat.xyz[1];
        f32 q4 = quat.xyz[2];

        Common::Vec3f rad_gyro;
        const f32 ax = -normal_accel.x;
        const f32 ay = normal_accel.y;
        const f32 az = -normal_accel.z;

        // Estimated direction of gravity
        const f32 vx = 2.0f * (q2 * q4 - q1 * q3);
        const f32 vy = 2.0f * (q1 * q2 + q3 * q4);
        const f32 vz = q1 * q1 - q2 * q2 - q3 * q3 + q4 * q4;

        // Error is cross product between estimated direction and measured direction of gravity
        const Common::Vec3f new_real_error = {
            az * vx - ax * vz,
            ay * vz - az * vy,
            ax * vy - ay * vx,
        };

        derivative_error = new_real_error - real_error;
        real_error = new_real_error;

        rad_gyro += 10.0f * kp * real_error;
        rad_gyro += 5.0f * ki * integral_error;
        rad_gyro += 10.0f * kd * derivative_error;

        const f32 gx = rad_gyro.y;
        const f32 gy = rad_gyro.x;
        const f32 gz = rad_gyro.z;

        // Integrate rate of change of quaternion
        const f32 pa = q2;
        const f32 pb = q3;
        const f32 pc = q4;
        q1 = q1 + (-q2 * gx - q3 * gy - q4 * gz) * (0.5f * sample_period);
        q2 = pa + (q1 * gx + pb * gz - pc * gy) * (0.5f * sample_period);
        q3 = pb + (q1 * gy - pa * gz + pc * gx) * (0.5f * sample_period);
        q4 = pc + (q1 * gz + pa * gy - pb * gx) * (0.5f * sample_period);

        quat.w = q1;
        quat.xyz[0] = q2;
        quat.xyz[1] = q3;
        quat.xyz[2] = q4;
        quat = quat.Normalized();
    }
}
} // namespace Core::HID
