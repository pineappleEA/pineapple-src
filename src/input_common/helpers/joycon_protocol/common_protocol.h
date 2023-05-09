// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Based on dkms-hid-nintendo implementation, CTCaer joycon toolkit and dekuNukem reverse
// engineering https://github.com/nicman23/dkms-hid-nintendo/blob/master/src/hid-nintendo.c
// https://github.com/CTCaer/jc_toolkit
// https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering

#pragma once

#include <memory>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "input_common/helpers/joycon_protocol/joycon_types.h"

namespace InputCommon::Joycon {

/// Joycon driver functions that handle low level communication
class JoyconCommonProtocol {
public:
    explicit JoyconCommonProtocol(std::shared_ptr<JoyconHandle> hidapi_handle_);

    /**
     * Sets handle to blocking. In blocking mode, SDL_hid_read() will wait (block) until there is
     * data to read before returning.
     */
    void SetBlocking();

    /**
     * Sets handle to non blocking. In non-blocking mode calls to SDL_hid_read() will return
     * immediately with a value of 0 if there is no data to be read
     */
    void SetNonBlocking();

    /**
     * Sends a request to obtain the joycon type from device
     * @returns controller type of the joycon
     */
    DriverResult GetDeviceType(ControllerType& controller_type);

    /**
     * Verifies and sets the joycon_handle if device is valid
     * @param device info from the driver
     * @returns success if the device is valid
     */
    DriverResult CheckDeviceAccess(SDL_hid_device_info* device);

    /**
     * Sends a request to set the polling mode of the joycon
     * @param report_mode polling mode to be set
     */
    DriverResult SetReportMode(Joycon::ReportMode report_mode);

    /**
     * Sends data to the joycon device
     * @param buffer data to be send
     */
    DriverResult SendRawData(std::span<const u8> buffer);

    template <typename Output>
        requires std::is_trivially_copyable_v<Output>
    DriverResult SendData(const Output& output) {
        std::array<u8, sizeof(Output)> buffer;
        std::memcpy(buffer.data(), &output, sizeof(Output));
        return SendRawData(buffer);
    }

    /**
     * Waits for incoming data of the joycon device that matches the subcommand
     * @param sub_command type of data to be returned
     * @returns a buffer containing the response
     */
    DriverResult GetSubCommandResponse(SubCommand sub_command, SubCommandResponse& output);

    /**
     * Sends a sub command to the device and waits for it's reply
     * @param sc sub command to be send
     * @param buffer data to be send
     * @returns output buffer containing the response
     */
    DriverResult SendSubCommand(SubCommand sc, std::span<const u8> buffer,
                                SubCommandResponse& output);

    /**
     * Sends a sub command to the device and waits for it's reply and ignores the output
     * @param sc sub command to be send
     * @param buffer data to be send
     */
    DriverResult SendSubCommand(SubCommand sc, std::span<const u8> buffer);

    /**
     * Sends a mcu command to the device
     * @param sc sub command to be send
     * @param buffer data to be send
     */
    DriverResult SendMCUCommand(SubCommand sc, std::span<const u8> buffer);

    /**
     * Sends vibration data to the joycon
     * @param buffer data to be send
     */
    DriverResult SendVibrationReport(std::span<const u8> buffer);

    /**
     * Reads the SPI memory stored on the joycon
     * @param Initial address location
     * @returns output buffer containing the response
     */
    DriverResult ReadRawSPI(SpiAddress addr, std::span<u8> output);

    /**
     * Reads the SPI memory stored on the joycon
     * @param Initial address location
     * @returns output object containing the response
     */
    template <typename Output>
        requires std::is_trivially_copyable_v<Output>
    DriverResult ReadSPI(SpiAddress addr, Output& output) {
        std::array<u8, sizeof(Output)> buffer;
        output = {};

        const auto result = ReadRawSPI(addr, buffer);
        if (result != DriverResult::Success) {
            return result;
        }

        std::memcpy(&output, buffer.data(), sizeof(Output));
        return DriverResult::Success;
    }

    /**
     * Enables MCU chip on the joycon
     * @param enable if true the chip will be enabled
     */
    DriverResult EnableMCU(bool enable);

    /**
     * Configures the MCU to the corresponding mode
     * @param MCUConfig configuration
     */
    DriverResult ConfigureMCU(const MCUConfig& config);

    /**
     * Waits until there's MCU data available. On timeout returns error
     * @param report mode of the expected reply
     * @returns a buffer containing the response
     */
    DriverResult GetMCUDataResponse(ReportMode report_mode_, MCUCommandResponse& output);

    /**
     * Sends data to the MCU chip and waits for it's reply
     * @param report mode of the expected reply
     * @param sub command to be send
     * @param buffer data to be send
     * @returns output buffer containing the response
     */
    DriverResult SendMCUData(ReportMode report_mode, MCUSubCommand sc, std::span<const u8> buffer,
                             MCUCommandResponse& output);

    /**
     * Wait's until the MCU chip is on the specified mode
     * @param report mode of the expected reply
     * @param MCUMode configuration
     */
    DriverResult WaitSetMCUMode(ReportMode report_mode, MCUMode mode);

    /**
     * Calculates the checksum from the MCU data
     * @param buffer containing the data to be send
     * @param size of the buffer in bytes
     * @returns byte with the correct checksum
     */
    u8 CalculateMCU_CRC8(u8* buffer, u8 size) const;

private:
    /**
     * Increments and returns the packet counter of the handle
     * @param joycon_handle device to send the data
     * @returns packet counter value
     */
    u8 GetCounter();

    std::shared_ptr<JoyconHandle> hidapi_handle;
};

class ScopedSetBlocking {
public:
    explicit ScopedSetBlocking(JoyconCommonProtocol* self) : m_self{self} {
        m_self->SetBlocking();
    }

    ~ScopedSetBlocking() {
        m_self->SetNonBlocking();
    }

private:
    JoyconCommonProtocol* m_self{};
};
} // namespace InputCommon::Joycon
