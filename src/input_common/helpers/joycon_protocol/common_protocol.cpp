// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "input_common/helpers/joycon_protocol/common_protocol.h"

namespace InputCommon::Joycon {
JoyconCommonProtocol::JoyconCommonProtocol(std::shared_ptr<JoyconHandle> hidapi_handle_)
    : hidapi_handle{std::move(hidapi_handle_)} {}

u8 JoyconCommonProtocol::GetCounter() {
    hidapi_handle->packet_counter = (hidapi_handle->packet_counter + 1) & 0x0F;
    return hidapi_handle->packet_counter;
}

void JoyconCommonProtocol::SetBlocking() {
    SDL_hid_set_nonblocking(hidapi_handle->handle, 0);
}

void JoyconCommonProtocol::SetNonBlocking() {
    SDL_hid_set_nonblocking(hidapi_handle->handle, 1);
}

DriverResult JoyconCommonProtocol::GetDeviceType(ControllerType& controller_type) {
    const auto result = ReadSPI(SpiAddress::DEVICE_TYPE, controller_type);

    if (result == DriverResult::Success) {
        // Fallback to 3rd party pro controllers
        if (controller_type == ControllerType::None) {
            controller_type = ControllerType::Pro;
        }
    }

    return result;
}

DriverResult JoyconCommonProtocol::CheckDeviceAccess(SDL_hid_device_info* device_info) {
    ControllerType controller_type{ControllerType::None};
    const auto result = GetDeviceType(controller_type);

    if (result != DriverResult::Success || controller_type == ControllerType::None) {
        return DriverResult::UnsupportedControllerType;
    }

    hidapi_handle->handle =
        SDL_hid_open(device_info->vendor_id, device_info->product_id, device_info->serial_number);

    if (!hidapi_handle->handle) {
        LOG_ERROR(Input, "Yuzu can't gain access to this device: ID {:04X}:{:04X}.",
                  device_info->vendor_id, device_info->product_id);
        return DriverResult::HandleInUse;
    }

    SetNonBlocking();
    return DriverResult::Success;
}

DriverResult JoyconCommonProtocol::SetReportMode(ReportMode report_mode) {
    const std::array<u8, 1> buffer{static_cast<u8>(report_mode)};
    return SendSubCommand(SubCommand::SET_REPORT_MODE, buffer);
}

DriverResult JoyconCommonProtocol::SendRawData(std::span<const u8> buffer) {
    const auto result = SDL_hid_write(hidapi_handle->handle, buffer.data(), buffer.size());

    if (result == -1) {
        return DriverResult::ErrorWritingData;
    }

    return DriverResult::Success;
}

DriverResult JoyconCommonProtocol::GetSubCommandResponse(SubCommand sc,
                                                         SubCommandResponse& output) {
    constexpr int timeout_mili = 66;
    constexpr int MaxTries = 15;
    int tries = 0;

    do {
        int result = SDL_hid_read_timeout(hidapi_handle->handle, reinterpret_cast<u8*>(&output),
                                          sizeof(SubCommandResponse), timeout_mili);

        if (result < 1) {
            LOG_ERROR(Input, "No response from joycon");
        }
        if (tries++ > MaxTries) {
            return DriverResult::Timeout;
        }
    } while (output.input_report.report_mode != ReportMode::SUBCMD_REPLY &&
             output.sub_command != sc);

    return DriverResult::Success;
}

DriverResult JoyconCommonProtocol::SendSubCommand(SubCommand sc, std::span<const u8> buffer,
                                                  SubCommandResponse& output) {
    SubCommandPacket packet{
        .output_report = OutputReport::RUMBLE_AND_SUBCMD,
        .packet_counter = GetCounter(),
        .sub_command = sc,
        .command_data = {},
    };

    if (buffer.size() > packet.command_data.size()) {
        return DriverResult::InvalidParameters;
    }

    memcpy(packet.command_data.data(), buffer.data(), buffer.size());

    auto result = SendData(packet);

    if (result != DriverResult::Success) {
        return result;
    }

    result = GetSubCommandResponse(sc, output);

    return DriverResult::Success;
}

DriverResult JoyconCommonProtocol::SendSubCommand(SubCommand sc, std::span<const u8> buffer) {
    SubCommandResponse output{};
    return SendSubCommand(sc, buffer, output);
}

DriverResult JoyconCommonProtocol::SendMCUCommand(SubCommand sc, std::span<const u8> buffer) {
    SubCommandPacket packet{
        .output_report = OutputReport::MCU_DATA,
        .packet_counter = GetCounter(),
        .sub_command = sc,
        .command_data = {},
    };

    if (buffer.size() > packet.command_data.size()) {
        return DriverResult::InvalidParameters;
    }

    memcpy(packet.command_data.data(), buffer.data(), buffer.size());

    return SendData(packet);
}

DriverResult JoyconCommonProtocol::SendVibrationReport(std::span<const u8> buffer) {
    VibrationPacket packet{
        .output_report = OutputReport::RUMBLE_ONLY,
        .packet_counter = GetCounter(),
        .vibration_data = {},
    };

    if (buffer.size() > packet.vibration_data.size()) {
        return DriverResult::InvalidParameters;
    }

    memcpy(packet.vibration_data.data(), buffer.data(), buffer.size());

    return SendData(packet);
}

DriverResult JoyconCommonProtocol::ReadRawSPI(SpiAddress addr, std::span<u8> output) {
    constexpr std::size_t HeaderSize = 5;
    constexpr std::size_t MaxTries = 10;
    std::size_t tries = 0;
    SubCommandResponse response{};
    std::array<u8, sizeof(ReadSpiPacket)> buffer{};
    const ReadSpiPacket packet_data{
        .spi_address = addr,
        .size = static_cast<u8>(output.size()),
    };

    memcpy(buffer.data(), &packet_data, sizeof(ReadSpiPacket));
    do {
        const auto result = SendSubCommand(SubCommand::SPI_FLASH_READ, buffer, response);
        if (result != DriverResult::Success) {
            return result;
        }

        if (tries++ > MaxTries) {
            return DriverResult::Timeout;
        }
    } while (response.spi_address != addr);

    if (response.command_data.size() < packet_data.size + HeaderSize) {
        return DriverResult::WrongReply;
    }

    // Remove header from output
    memcpy(output.data(), response.command_data.data() + HeaderSize, packet_data.size);
    return DriverResult::Success;
}

DriverResult JoyconCommonProtocol::EnableMCU(bool enable) {
    const std::array<u8, 1> mcu_state{static_cast<u8>(enable ? 1 : 0)};
    const auto result = SendSubCommand(SubCommand::SET_MCU_STATE, mcu_state);

    if (result != DriverResult::Success) {
        LOG_ERROR(Input, "Failed with error {}", result);
    }

    return result;
}

DriverResult JoyconCommonProtocol::ConfigureMCU(const MCUConfig& config) {
    LOG_DEBUG(Input, "ConfigureMCU");
    std::array<u8, sizeof(MCUConfig)> config_buffer;
    memcpy(config_buffer.data(), &config, sizeof(MCUConfig));
    config_buffer[37] = CalculateMCU_CRC8(config_buffer.data() + 1, 36);

    const auto result = SendSubCommand(SubCommand::SET_MCU_CONFIG, config_buffer);

    if (result != DriverResult::Success) {
        LOG_ERROR(Input, "Failed with error {}", result);
    }

    return result;
}

DriverResult JoyconCommonProtocol::GetMCUDataResponse(ReportMode report_mode,
                                                      MCUCommandResponse& output) {
    constexpr int TimeoutMili = 200;
    constexpr int MaxTries = 9;
    int tries = 0;

    do {
        int result = SDL_hid_read_timeout(hidapi_handle->handle, reinterpret_cast<u8*>(&output),
                                          sizeof(MCUCommandResponse), TimeoutMili);

        if (result < 1) {
            LOG_ERROR(Input, "No response from joycon attempt {}", tries);
        }
        if (tries++ > MaxTries) {
            return DriverResult::Timeout;
        }
    } while (output.input_report.report_mode != report_mode ||
             output.mcu_report == MCUReport::EmptyAwaitingCmd);

    return DriverResult::Success;
}

DriverResult JoyconCommonProtocol::SendMCUData(ReportMode report_mode, MCUSubCommand sc,
                                               std::span<const u8> buffer,
                                               MCUCommandResponse& output) {
    SubCommandPacket packet{
        .output_report = OutputReport::MCU_DATA,
        .packet_counter = GetCounter(),
        .mcu_sub_command = sc,
        .command_data = {},
    };

    if (buffer.size() > packet.command_data.size()) {
        return DriverResult::InvalidParameters;
    }

    memcpy(packet.command_data.data(), buffer.data(), buffer.size());

    auto result = SendData(packet);

    if (result != DriverResult::Success) {
        return result;
    }

    result = GetMCUDataResponse(report_mode, output);

    return DriverResult::Success;
}

DriverResult JoyconCommonProtocol::WaitSetMCUMode(ReportMode report_mode, MCUMode mode) {
    MCUCommandResponse output{};
    constexpr std::size_t MaxTries{8};
    std::size_t tries{};

    do {
        const auto result = SendMCUData(report_mode, MCUSubCommand::SetDeviceMode, {}, output);

        if (result != DriverResult::Success) {
            return result;
        }

        if (tries++ > MaxTries) {
            return DriverResult::WrongReply;
        }
    } while (output.mcu_report != MCUReport::StateReport ||
             output.mcu_data[6] != static_cast<u8>(mode));

    return DriverResult::Success;
}

// crc-8-ccitt / polynomial 0x07 look up table
constexpr std::array<u8, 256> mcu_crc8_table = {
    0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15, 0x38, 0x3F, 0x36, 0x31, 0x24, 0x23, 0x2A, 0x2D,
    0x70, 0x77, 0x7E, 0x79, 0x6C, 0x6B, 0x62, 0x65, 0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D,
    0xE0, 0xE7, 0xEE, 0xE9, 0xFC, 0xFB, 0xF2, 0xF5, 0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD,
    0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85, 0xA8, 0xAF, 0xA6, 0xA1, 0xB4, 0xB3, 0xBA, 0xBD,
    0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2, 0xFF, 0xF8, 0xF1, 0xF6, 0xE3, 0xE4, 0xED, 0xEA,
    0xB7, 0xB0, 0xB9, 0xBE, 0xAB, 0xAC, 0xA5, 0xA2, 0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D, 0x9A,
    0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32, 0x1F, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0D, 0x0A,
    0x57, 0x50, 0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42, 0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A,
    0x89, 0x8E, 0x87, 0x80, 0x95, 0x92, 0x9B, 0x9C, 0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4,
    0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC, 0xC1, 0xC6, 0xCF, 0xC8, 0xDD, 0xDA, 0xD3, 0xD4,
    0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C, 0x51, 0x56, 0x5F, 0x58, 0x4D, 0x4A, 0x43, 0x44,
    0x19, 0x1E, 0x17, 0x10, 0x05, 0x02, 0x0B, 0x0C, 0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A, 0x33, 0x34,
    0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B, 0x76, 0x71, 0x78, 0x7F, 0x6A, 0x6D, 0x64, 0x63,
    0x3E, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B, 0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13,
    0xAE, 0xA9, 0xA0, 0xA7, 0xB2, 0xB5, 0xBC, 0xBB, 0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83,
    0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB, 0xE6, 0xE1, 0xE8, 0xEF, 0xFA, 0xFD, 0xF4, 0xF3};

u8 JoyconCommonProtocol::CalculateMCU_CRC8(u8* buffer, u8 size) const {
    u8 crc8 = 0x0;

    for (int i = 0; i < size; ++i) {
        crc8 = mcu_crc8_table[static_cast<u8>(crc8 ^ buffer[i])];
    }
    return crc8;
}

} // namespace InputCommon::Joycon
