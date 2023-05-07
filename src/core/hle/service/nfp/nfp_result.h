// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/result.h"

namespace Service::NFP {

constexpr Result ResultDeviceNotFound(ErrorModule::NFP, 64);
constexpr Result ResultInvalidArgument(ErrorModule::NFP, 65);
constexpr Result ResultWrongApplicationAreaSize(ErrorModule::NFP, 68);
constexpr Result ResultWrongDeviceState(ErrorModule::NFP, 73);
constexpr Result ResultUnknown74(ErrorModule::NFC, 74);
constexpr Result ResultNfcDisabled(ErrorModule::NFP, 80);
constexpr Result ResultWriteAmiiboFailed(ErrorModule::NFP, 88);
constexpr Result ResultTagRemoved(ErrorModule::NFP, 97);
constexpr Result ResultRegistrationIsNotInitialized(ErrorModule::NFP, 120);
constexpr Result ResultApplicationAreaIsNotInitialized(ErrorModule::NFP, 128);
constexpr Result ResultCorruptedData(ErrorModule::NFP, 144);
constexpr Result ResultWrongApplicationAreaId(ErrorModule::NFP, 152);
constexpr Result ResultApplicationAreaExist(ErrorModule::NFP, 168);
constexpr Result ResultNotAnAmiibo(ErrorModule::NFP, 178);

} // namespace Service::NFP
