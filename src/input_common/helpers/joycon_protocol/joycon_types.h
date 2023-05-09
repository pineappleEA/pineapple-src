// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Based on dkms-hid-nintendo implementation, CTCaer joycon toolkit and dekuNukem reverse
// engineering https://github.com/nicman23/dkms-hid-nintendo/blob/master/src/hid-nintendo.c
// https://github.com/CTCaer/jc_toolkit
// https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering

#pragma once

#include <array>
#include <functional>
#include <SDL_hidapi.h>

#include "common/bit_field.h"
#include "common/common_funcs.h"
#include "common/common_types.h"

namespace InputCommon::Joycon {
constexpr u32 MaxErrorCount = 50;
constexpr u32 MaxBufferSize = 368;
constexpr std::array<u8, 8> DefaultVibrationBuffer{0x0, 0x1, 0x40, 0x40, 0x0, 0x1, 0x40, 0x40};

using MacAddress = std::array<u8, 6>;
using SerialNumber = std::array<u8, 15>;

enum class ControllerType : u8 {
    None = 0x00,
    Left = 0x01,
    Right = 0x02,
    Pro = 0x03,
    Dual = 0x05, // TODO: Verify this id
    LarkHvc1 = 0x07,
    LarkHvc2 = 0x08,
    LarkNesLeft = 0x09,
    LarkNesRight = 0x0A,
    Lucia = 0x0B,
    Lagon = 0x0C,
    Lager = 0x0D,
};

enum class PadAxes {
    LeftStickX,
    LeftStickY,
    RightStickX,
    RightStickY,
    Undefined,
};

enum class PadMotion {
    LeftMotion,
    RightMotion,
    Undefined,
};

enum class PadButton : u32 {
    Down = 0x000001,
    Up = 0x000002,
    Right = 0x000004,
    Left = 0x000008,
    LeftSR = 0x000010,
    LeftSL = 0x000020,
    L = 0x000040,
    ZL = 0x000080,
    Y = 0x000100,
    X = 0x000200,
    B = 0x000400,
    A = 0x000800,
    RightSR = 0x001000,
    RightSL = 0x002000,
    R = 0x004000,
    ZR = 0x008000,
    Minus = 0x010000,
    Plus = 0x020000,
    StickR = 0x040000,
    StickL = 0x080000,
    Home = 0x100000,
    Capture = 0x200000,
};

enum class PassivePadButton : u32 {
    Down_A = 0x0001,
    Right_X = 0x0002,
    Left_B = 0x0004,
    Up_Y = 0x0008,
    SL = 0x0010,
    SR = 0x0020,
    Minus = 0x0100,
    Plus = 0x0200,
    StickL = 0x0400,
    StickR = 0x0800,
    Home = 0x1000,
    Capture = 0x2000,
    L_R = 0x4000,
    ZL_ZR = 0x8000,
};

enum class PassivePadStick : u8 {
    Right = 0x00,
    RightDown = 0x01,
    Down = 0x02,
    DownLeft = 0x03,
    Left = 0x04,
    LeftUp = 0x05,
    Up = 0x06,
    UpRight = 0x07,
    Neutral = 0x08,
};

enum class OutputReport : u8 {
    RUMBLE_AND_SUBCMD = 0x01,
    FW_UPDATE_PKT = 0x03,
    RUMBLE_ONLY = 0x10,
    MCU_DATA = 0x11,
    USB_CMD = 0x80,
};

enum class FeatureReport : u8 {
    Last_SUBCMD = 0x02,
    OTA_GW_UPGRADE = 0x70,
    SETUP_MEM_READ = 0x71,
    MEM_READ = 0x72,
    ERASE_MEM_SECTOR = 0x73,
    MEM_WRITE = 0x74,
    LAUNCH = 0x75,
};

enum class SubCommand : u8 {
    STATE = 0x00,
    MANUAL_BT_PAIRING = 0x01,
    REQ_DEV_INFO = 0x02,
    SET_REPORT_MODE = 0x03,
    TRIGGERS_ELAPSED = 0x04,
    GET_PAGE_LIST_STATE = 0x05,
    SET_HCI_STATE = 0x06,
    RESET_PAIRING_INFO = 0x07,
    LOW_POWER_MODE = 0x08,
    SPI_FLASH_READ = 0x10,
    SPI_FLASH_WRITE = 0x11,
    SPI_SECTOR_ERASE = 0x12,
    RESET_MCU = 0x20,
    SET_MCU_CONFIG = 0x21,
    SET_MCU_STATE = 0x22,
    SET_PLAYER_LIGHTS = 0x30,
    GET_PLAYER_LIGHTS = 0x31,
    SET_HOME_LIGHT = 0x38,
    ENABLE_IMU = 0x40,
    SET_IMU_SENSITIVITY = 0x41,
    WRITE_IMU_REG = 0x42,
    READ_IMU_REG = 0x43,
    ENABLE_VIBRATION = 0x48,
    GET_REGULATED_VOLTAGE = 0x50,
    SET_EXTERNAL_CONFIG = 0x58,
    GET_EXTERNAL_DEVICE_INFO = 0x59,
    ENABLE_EXTERNAL_POLLING = 0x5A,
    DISABLE_EXTERNAL_POLLING = 0x5B,
    SET_EXTERNAL_FORMAT_CONFIG = 0x5C,
};

enum class UsbSubCommand : u8 {
    CONN_STATUS = 0x01,
    HADSHAKE = 0x02,
    BAUDRATE_3M = 0x03,
    NO_TIMEOUT = 0x04,
    EN_TIMEOUT = 0x05,
    RESET = 0x06,
    PRE_HANDSHAKE = 0x91,
    SEND_UART = 0x92,
};

enum class CalibrationMagic : u8 {
    USR_MAGIC_0 = 0xB2,
    USR_MAGIC_1 = 0xA1,
};

enum class SpiAddress : u16 {
    MAGIC = 0x0000,
    MAC_ADDRESS = 0x0015,
    PAIRING_INFO = 0x2000,
    SHIPMENT = 0x5000,
    SERIAL_NUMBER = 0x6000,
    DEVICE_TYPE = 0x6012,
    FORMAT_VERSION = 0x601B,
    FACT_IMU_DATA = 0x6020,
    FACT_LEFT_DATA = 0x603d,
    FACT_RIGHT_DATA = 0x6046,
    COLOR_DATA = 0x6050,
    DESIGN_VARIATION = 0x605C,
    SENSOR_DATA = 0x6080,
    USER_LEFT_MAGIC = 0x8010,
    USER_LEFT_DATA = 0x8012,
    USER_RIGHT_MAGIC = 0x801B,
    USER_RIGHT_DATA = 0x801D,
    USER_IMU_MAGIC = 0x8026,
    USER_IMU_DATA = 0x8028,
};

enum class ReportMode : u8 {
    ACTIVE_POLLING_NFC_IR_CAMERA_DATA = 0x00,
    ACTIVE_POLLING_NFC_IR_CAMERA_CONFIGURATION = 0x01,
    ACTIVE_POLLING_NFC_IR_CAMERA_DATA_CONFIGURATION = 0x02,
    ACTIVE_POLLING_IR_CAMERA_DATA = 0x03,
    SUBCMD_REPLY = 0x21,
    MCU_UPDATE_STATE = 0x23,
    STANDARD_FULL_60HZ = 0x30,
    NFC_IR_MODE_60HZ = 0x31,
    SIMPLE_HID_MODE = 0x3F,
    INPUT_USB_RESPONSE = 0x81,
};

enum class GyroSensitivity : u8 {
    DPS250,
    DPS500,
    DPS1000,
    DPS2000, // Default
};

enum class AccelerometerSensitivity : u8 {
    G8, // Default
    G4,
    G2,
    G16,
};

enum class GyroPerformance : u8 {
    HZ833,
    HZ208, // Default
};

enum class AccelerometerPerformance : u8 {
    HZ200,
    HZ100, // Default
};

enum class MCUCommand : u8 {
    ConfigureMCU = 0x21,
    ConfigureIR = 0x23,
};

enum class MCUSubCommand : u8 {
    SetMCUMode = 0x0,
    SetDeviceMode = 0x1,
    ReadDeviceMode = 0x02,
    WriteDeviceRegisters = 0x4,
};

enum class MCUMode : u8 {
    Suspend = 0,
    Standby = 1,
    Ringcon = 3,
    NFC = 4,
    IR = 5,
    MaybeFWUpdate = 6,
};

enum class MCURequest : u8 {
    GetMCUStatus = 1,
    GetNFCData = 2,
    GetIRData = 3,
};

enum class MCUReport : u8 {
    Empty = 0x00,
    StateReport = 0x01,
    IRData = 0x03,
    BusyInitializing = 0x0b,
    IRStatus = 0x13,
    IRRegisters = 0x1b,
    NFCState = 0x2a,
    NFCReadData = 0x3a,
    EmptyAwaitingCmd = 0xff,
};

enum class MCUPacketFlag : u8 {
    MorePacketsRemaining = 0x00,
    LastCommandPacket = 0x08,
};

enum class NFCReadCommand : u8 {
    CancelAll = 0x00,
    StartPolling = 0x01,
    StopPolling = 0x02,
    StartWaitingRecieve = 0x04,
    Ntag = 0x06,
    Mifare = 0x0F,
};

enum class NFCTagType : u8 {
    AllTags = 0x00,
    Ntag215 = 0x01,
};

enum class NFCPages {
    Block0 = 0,
    Block45 = 45,
    Block135 = 135,
    Block231 = 231,
};

enum class NFCStatus : u8 {
    LastPackage = 0x04,
    TagLost = 0x07,
};

enum class IrsMode : u8 {
    None = 0x02,
    Moment = 0x03,
    Dpd = 0x04,
    Clustering = 0x06,
    ImageTransfer = 0x07,
    Silhouette = 0x08,
    TeraImage = 0x09,
    SilhouetteTeraImage = 0x0A,
};

enum class IrsResolution {
    Size320x240,
    Size160x120,
    Size80x60,
    Size40x30,
    Size20x15,
    None,
};

enum class IrsResolutionCode : u8 {
    Size320x240 = 0x00, // Full pixel array
    Size160x120 = 0x50, // Sensor Binning [2 X 2]
    Size80x60 = 0x64,   // Sensor Binning [4 x 2] and Skipping [1 x 2]
    Size40x30 = 0x69,   // Sensor Binning [4 x 2] and Skipping [2 x 4]
    Size20x15 = 0x6A,   // Sensor Binning [4 x 2] and Skipping [4 x 4]
};

// Size of image divided by 300
enum class IrsFragments : u8 {
    Size20x15 = 0x00,
    Size40x30 = 0x03,
    Size80x60 = 0x0f,
    Size160x120 = 0x3f,
    Size320x240 = 0xFF,
};

enum class IrLeds : u8 {
    BrightAndDim = 0x00,
    Bright = 0x20,
    Dim = 0x10,
    None = 0x30,
};

enum class IrExLedFilter : u8 {
    Disabled = 0x00,
    Enabled = 0x03,
};

enum class IrImageFlip : u8 {
    Normal = 0x00,
    Inverted = 0x02,
};

enum class IrRegistersAddress : u16 {
    UpdateTime = 0x0400,
    FinalizeConfig = 0x0700,
    LedFilter = 0x0e00,
    Leds = 0x1000,
    LedIntensitiyMSB = 0x1100,
    LedIntensitiyLSB = 0x1200,
    ImageFlip = 0x2d00,
    Resolution = 0x2e00,
    DigitalGainLSB = 0x2e01,
    DigitalGainMSB = 0x2f01,
    ExposureLSB = 0x3001,
    ExposureMSB = 0x3101,
    ExposureTime = 0x3201,
    WhitePixelThreshold = 0x4301,
    DenoiseSmoothing = 0x6701,
    DenoiseEdge = 0x6801,
    DenoiseColor = 0x6901,
};

enum class ExternalDeviceId : u16 {
    RingController = 0x2000,
    Starlink = 0x2800,
};

enum class DriverResult {
    Success,
    WrongReply,
    Timeout,
    InvalidParameters,
    UnsupportedControllerType,
    HandleInUse,
    ErrorReadingData,
    ErrorWritingData,
    NoDeviceDetected,
    InvalidHandle,
    NotSupported,
    Disabled,
    Unknown,
};

struct MotionSensorCalibration {
    s16 offset;
    s16 scale;
};

struct MotionCalibration {
    std::array<MotionSensorCalibration, 3> accelerometer;
    std::array<MotionSensorCalibration, 3> gyro;
};

// Basic motion data containing data from the sensors and a timestamp in microseconds
struct MotionData {
    float gyro_x{};
    float gyro_y{};
    float gyro_z{};
    float accel_x{};
    float accel_y{};
    float accel_z{};
    u64 delta_timestamp{};
};

// Output from SPI read command containing user calibration magic
struct MagicSpiCalibration {
    CalibrationMagic first;
    CalibrationMagic second;
};
static_assert(sizeof(MagicSpiCalibration) == 0x2, "MagicSpiCalibration is an invalid size");

// Output from SPI read command containing left joystick calibration
struct JoystickLeftSpiCalibration {
    std::array<u8, 3> max;
    std::array<u8, 3> center;
    std::array<u8, 3> min;
};
static_assert(sizeof(JoystickLeftSpiCalibration) == 0x9,
              "JoystickLeftSpiCalibration is an invalid size");

// Output from SPI read command containing right joystick calibration
struct JoystickRightSpiCalibration {
    std::array<u8, 3> center;
    std::array<u8, 3> min;
    std::array<u8, 3> max;
};
static_assert(sizeof(JoystickRightSpiCalibration) == 0x9,
              "JoystickRightSpiCalibration is an invalid size");

struct JoyStickAxisCalibration {
    u16 max;
    u16 min;
    u16 center;
};

struct JoyStickCalibration {
    JoyStickAxisCalibration x;
    JoyStickAxisCalibration y;
};

struct ImuSpiCalibration {
    std::array<s16, 3> accelerometer_offset;
    std::array<s16, 3> accelerometer_scale;
    std::array<s16, 3> gyroscope_offset;
    std::array<s16, 3> gyroscope_scale;
};
static_assert(sizeof(ImuSpiCalibration) == 0x18, "ImuSpiCalibration is an invalid size");

struct RingCalibration {
    s16 default_value;
    s16 max_value;
    s16 min_value;
};

struct Color {
    u32 body;
    u32 buttons;
    u32 left_grip;
    u32 right_grip;
};

struct Battery {
    union {
        u8 raw{};

        BitField<0, 4, u8> unknown;
        BitField<4, 1, u8> charging;
        BitField<5, 3, u8> status;
    };
};

struct VibrationValue {
    f32 low_amplitude;
    f32 low_frequency;
    f32 high_amplitude;
    f32 high_frequency;
};

struct JoyconHandle {
    SDL_hid_device* handle = nullptr;
    u8 packet_counter{};
};

struct MCUConfig {
    MCUCommand command;
    MCUSubCommand sub_command;
    MCUMode mode;
    INSERT_PADDING_BYTES(0x22);
    u8 crc;
};
static_assert(sizeof(MCUConfig) == 0x26, "MCUConfig is an invalid size");

#pragma pack(push, 1)
struct InputReportPassive {
    ReportMode report_mode;
    u16 button_input;
    u8 stick_state;
    std::array<u8, 10> unknown_data;
};
static_assert(sizeof(InputReportPassive) == 0xE, "InputReportPassive is an invalid size");

struct InputReportActive {
    ReportMode report_mode;
    u8 packet_id;
    Battery battery_status;
    std::array<u8, 3> button_input;
    std::array<u8, 3> left_stick_state;
    std::array<u8, 3> right_stick_state;
    u8 vibration_code;
    std::array<s16, 6 * 2> motion_input;
    INSERT_PADDING_BYTES(0x2);
    s16 ring_input;
};
static_assert(sizeof(InputReportActive) == 0x29, "InputReportActive is an invalid size");

struct InputReportNfcIr {
    ReportMode report_mode;
    u8 packet_id;
    Battery battery_status;
    std::array<u8, 3> button_input;
    std::array<u8, 3> left_stick_state;
    std::array<u8, 3> right_stick_state;
    u8 vibration_code;
    std::array<s16, 6 * 2> motion_input;
    INSERT_PADDING_BYTES(0x4);
};
static_assert(sizeof(InputReportNfcIr) == 0x29, "InputReportNfcIr is an invalid size");
#pragma pack(pop)

struct NFCReadBlock {
    u8 start;
    u8 end;
};
static_assert(sizeof(NFCReadBlock) == 0x2, "NFCReadBlock is an invalid size");

struct NFCReadBlockCommand {
    u8 block_count{};
    std::array<NFCReadBlock, 4> blocks{};
};
static_assert(sizeof(NFCReadBlockCommand) == 0x9, "NFCReadBlockCommand is an invalid size");

struct NFCReadCommandData {
    u8 unknown;
    u8 uuid_length;
    u8 unknown_2;
    std::array<u8, 6> uid;
    NFCTagType tag_type;
    NFCReadBlockCommand read_block;
};
static_assert(sizeof(NFCReadCommandData) == 0x13, "NFCReadCommandData is an invalid size");

struct NFCPollingCommandData {
    u8 enable_mifare;
    u8 unknown_1;
    u8 unknown_2;
    u8 unknown_3;
    u8 unknown_4;
};
static_assert(sizeof(NFCPollingCommandData) == 0x05, "NFCPollingCommandData is an invalid size");

struct NFCRequestState {
    NFCReadCommand command_argument;
    u8 packet_id;
    INSERT_PADDING_BYTES(0x1);
    MCUPacketFlag packet_flag;
    u8 data_length;
    union {
        std::array<u8, 0x1F> raw_data;
        NFCReadCommandData nfc_read;
        NFCPollingCommandData nfc_polling;
    };
    u8 crc;
    INSERT_PADDING_BYTES(0x1);
};
static_assert(sizeof(NFCRequestState) == 0x26, "NFCRequestState is an invalid size");

struct IrsConfigure {
    MCUCommand command;
    MCUSubCommand sub_command;
    IrsMode irs_mode;
    IrsFragments number_of_fragments;
    u16 mcu_major_version;
    u16 mcu_minor_version;
    INSERT_PADDING_BYTES(0x1D);
    u8 crc;
};
static_assert(sizeof(IrsConfigure) == 0x26, "IrsConfigure is an invalid size");

#pragma pack(push, 1)
struct IrsRegister {
    IrRegistersAddress address;
    u8 value;
};
static_assert(sizeof(IrsRegister) == 0x3, "IrsRegister is an invalid size");

struct IrsWriteRegisters {
    MCUCommand command;
    MCUSubCommand sub_command;
    u8 number_of_registers;
    std::array<IrsRegister, 9> registers;
    INSERT_PADDING_BYTES(0x7);
    u8 crc;
};
static_assert(sizeof(IrsWriteRegisters) == 0x26, "IrsWriteRegisters is an invalid size");
#pragma pack(pop)

struct FirmwareVersion {
    u8 major;
    u8 minor;
};
static_assert(sizeof(FirmwareVersion) == 0x2, "FirmwareVersion is an invalid size");

struct DeviceInfo {
    FirmwareVersion firmware;
    std::array<u8, 2> unknown_1;
    MacAddress mac_address;
    std::array<u8, 2> unknown_2;
};
static_assert(sizeof(DeviceInfo) == 0xC, "DeviceInfo is an invalid size");

struct MotionStatus {
    bool is_enabled;
    u64 delta_time;
    GyroSensitivity gyro_sensitivity;
    AccelerometerSensitivity accelerometer_sensitivity;
};

struct RingStatus {
    bool is_enabled;
    s16 default_value;
    s16 max_value;
    s16 min_value;
};

struct VibrationPacket {
    OutputReport output_report;
    u8 packet_counter;
    std::array<u8, 0x8> vibration_data;
};
static_assert(sizeof(VibrationPacket) == 0xA, "VibrationPacket is an invalid size");

struct SubCommandPacket {
    OutputReport output_report;
    u8 packet_counter;
    INSERT_PADDING_BYTES(0x8); // This contains vibration data
    union {
        SubCommand sub_command;
        MCUSubCommand mcu_sub_command;
    };
    std::array<u8, 0x26> command_data;
};
static_assert(sizeof(SubCommandPacket) == 0x31, "SubCommandPacket is an invalid size");

#pragma pack(push, 1)
struct ReadSpiPacket {
    SpiAddress spi_address;
    INSERT_PADDING_BYTES(0x2);
    u8 size;
};
static_assert(sizeof(ReadSpiPacket) == 0x5, "ReadSpiPacket is an invalid size");

struct SubCommandResponse {
    InputReportPassive input_report;
    SubCommand sub_command;
    union {
        std::array<u8, 0x30> command_data;
        SpiAddress spi_address;              // Reply from SPI_FLASH_READ subcommand
        ExternalDeviceId external_device_id; // Reply from GET_EXTERNAL_DEVICE_INFO subcommand
        DeviceInfo device_info;              // Reply from REQ_DEV_INFO subcommand
    };
    u8 crc; // This is never used
};
static_assert(sizeof(SubCommandResponse) == 0x40, "SubCommandResponse is an invalid size");
#pragma pack(pop)

struct MCUCommandResponse {
    InputReportNfcIr input_report;
    INSERT_PADDING_BYTES(0x8);
    MCUReport mcu_report;
    std::array<u8, 0x13D> mcu_data;
    u8 crc;
};
static_assert(sizeof(MCUCommandResponse) == 0x170, "MCUCommandResponse is an invalid size");

struct JoyconCallbacks {
    std::function<void(Battery)> on_battery_data;
    std::function<void(Color)> on_color_data;
    std::function<void(int, bool)> on_button_data;
    std::function<void(int, f32)> on_stick_data;
    std::function<void(int, const MotionData&)> on_motion_data;
    std::function<void(f32)> on_ring_data;
    std::function<void(const std::vector<u8>&)> on_amiibo_data;
    std::function<void(const std::vector<u8>&, IrsResolution)> on_camera_data;
};

} // namespace InputCommon::Joycon
