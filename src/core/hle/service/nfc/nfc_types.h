// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_funcs.h"
#include "common/common_types.h"

namespace Service::NFC {
enum class BackendType : u32 {
    None,
    Nfc,
    Nfp,
    Mifare,
};

// This is nn::nfc::DeviceState
enum class DeviceState : u32 {
    Initialized,
    SearchingForTag,
    TagFound,
    TagRemoved,
    TagMounted,
    Unavailable,
    Finalized,
};

// This is nn::nfc::State
enum class State : u32 {
    NonInitialized,
    Initialized,
};

// This is nn::nfc::TagType
enum class TagType : u32 {
    None,
    Type1, // ISO14443A RW 96-2k bytes 106kbit/s
    Type2, // ISO14443A RW/RO 540 bytes 106kbit/s
    Type3, // Sony FeliCa RW/RO 2k bytes 212kbit/s
    Type4, // ISO14443A RW/RO 4k-32k bytes 424kbit/s
    Type5, // ISO15693 RW/RO 540 bytes 106kbit/s
};

enum class PackedTagType : u8 {
    None,
    Type1, // ISO14443A RW 96-2k bytes 106kbit/s
    Type2, // ISO14443A RW/RO 540 bytes 106kbit/s
    Type3, // Sony FeliCa RW/RO 2k bytes 212kbit/s
    Type4, // ISO14443A RW/RO 4k-32k bytes 424kbit/s
    Type5, // ISO15693 RW/RO 540 bytes 106kbit/s
};

// This is nn::nfc::NfcProtocol
// Verify this enum. It might be completely wrong default protocol is 0x48
enum class NfcProtocol : u32 {
    None,
    TypeA = 1U << 0, // ISO14443A
    TypeB = 1U << 1, // ISO14443B
    TypeF = 1U << 2, // Sony FeliCa
    Unknown1 = 1U << 3,
    Unknown2 = 1U << 5,
    All = 0xFFFFFFFFU,
};

// this is nn::nfc::TestWaveType
enum class TestWaveType : u32 {
    Unknown,
};

using UniqueSerialNumber = std::array<u8, 7>;
using UniqueSerialNumberExtension = std::array<u8, 3>;

// This is nn::nfc::DeviceHandle
using DeviceHandle = u64;

// This is nn::nfc::TagInfo
struct TagInfo {
    UniqueSerialNumber uuid;
    UniqueSerialNumberExtension uuid_extension;
    u8 uuid_length;
    INSERT_PADDING_BYTES(0x15);
    NfcProtocol protocol;
    TagType tag_type;
    INSERT_PADDING_BYTES(0x30);
};
static_assert(sizeof(TagInfo) == 0x58, "TagInfo is an invalid size");

} // namespace Service::NFC
