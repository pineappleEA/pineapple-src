// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_funcs.h"
#include "common/common_types.h"

namespace Service::NFC {

enum class MifareCmd : u8 {
    AuthA = 0x60,
    AuthB = 0x61,
    Read = 0x30,
    Write = 0xA0,
    Transfer = 0xB0,
    Decrement = 0xC0,
    Increment = 0xC1,
    Store = 0xC2
};

using DataBlock = std::array<u8, 0x10>;
using KeyData = std::array<u8, 0x6>;

struct SectorKey {
    MifareCmd command;
    u8 unknown; // Usually 1
    INSERT_PADDING_BYTES(0x6);
    KeyData sector_key;
    INSERT_PADDING_BYTES(0x2);
};
static_assert(sizeof(SectorKey) == 0x10, "SectorKey is an invalid size");

// This is nn::nfc::MifareReadBlockParameter
struct MifareReadBlockParameter {
    u8 sector_number;
    INSERT_PADDING_BYTES(0x7);
    SectorKey sector_key;
};
static_assert(sizeof(MifareReadBlockParameter) == 0x18,
              "MifareReadBlockParameter is an invalid size");

// This is nn::nfc::MifareReadBlockData
struct MifareReadBlockData {
    DataBlock data;
    u8 sector_number;
    INSERT_PADDING_BYTES(0x7);
};
static_assert(sizeof(MifareReadBlockData) == 0x18, "MifareReadBlockData is an invalid size");

// This is nn::nfc::MifareWriteBlockParameter
struct MifareWriteBlockParameter {
    DataBlock data;
    u8 sector_number;
    INSERT_PADDING_BYTES(0x7);
    SectorKey sector_key;
};
static_assert(sizeof(MifareWriteBlockParameter) == 0x28,
              "MifareWriteBlockParameter is an invalid size");

} // namespace Service::NFC
