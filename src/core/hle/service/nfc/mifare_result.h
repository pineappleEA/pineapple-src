// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/result.h"

namespace Service::NFC::Mifare {

constexpr Result ResultDeviceNotFound(ErrorModule::NFCMifare, 64);
constexpr Result ResultInvalidArgument(ErrorModule::NFCMifare, 65);
constexpr Result ResultWrongDeviceState(ErrorModule::NFCMifare, 73);
constexpr Result ResultNfcDisabled(ErrorModule::NFCMifare, 80);
constexpr Result ResultTagRemoved(ErrorModule::NFCMifare, 97);
constexpr Result ResultReadError(ErrorModule::NFCMifare, 288);

} // namespace Service::NFC::Mifare
