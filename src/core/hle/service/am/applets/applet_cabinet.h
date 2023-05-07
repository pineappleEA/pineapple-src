// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "core/hle/result.h"
#include "core/hle/service/am/applets/applets.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nfp/nfp_types.h"

namespace Kernel {
class KEvent;
class KReadableEvent;
} // namespace Kernel

namespace Core {
class System;
} // namespace Core

namespace Service::NFC {
class NfcDevice;
}

namespace Service::AM::Applets {

enum class CabinetAppletVersion : u32 {
    Version1 = 0x1,
};

enum class CabinetResult : u8 {
    Cancel = 0,
    TagInfo = 1 << 1,
    RegisterInfo = 1 << 2,
    All = TagInfo | RegisterInfo,
};
DECLARE_ENUM_FLAG_OPERATORS(CabinetResult)

// This is nn::nfp::AmiiboSettingsStartParam
struct AmiiboSettingsStartParam {
    u64 device_handle;
    std::array<u8, 0x20> param_1;
    u8 param_2;
};
static_assert(sizeof(AmiiboSettingsStartParam) == 0x30,
              "AmiiboSettingsStartParam is an invalid size");

#pragma pack(push, 1)
// This is nn::nfp::StartParamForAmiiboSettings
struct StartParamForAmiiboSettings {
    u8 param_1;
    Service::NFP::CabinetMode applet_mode;
    u8 flags;
    u8 amiibo_settings_1;
    u64 device_handle;
    Service::NFP::TagInfo tag_info;
    Service::NFP::RegisterInfo register_info;
    std::array<u8, 0x20> amiibo_settings_3;
    INSERT_PADDING_BYTES(0x24);
};
static_assert(sizeof(StartParamForAmiiboSettings) == 0x1A8,
              "StartParamForAmiiboSettings is an invalid size");

// This is nn::nfp::ReturnValueForAmiiboSettings
struct ReturnValueForAmiiboSettings {
    CabinetResult result;
    INSERT_PADDING_BYTES(0x3);
    u64 device_handle;
    Service::NFP::TagInfo tag_info;
    Service::NFP::RegisterInfo register_info;
    INSERT_PADDING_BYTES(0x24);
};
static_assert(sizeof(ReturnValueForAmiiboSettings) == 0x188,
              "ReturnValueForAmiiboSettings is an invalid size");
#pragma pack(pop)

class Cabinet final : public Applet {
public:
    explicit Cabinet(Core::System& system_, LibraryAppletMode applet_mode_,
                     const Core::Frontend::CabinetApplet& frontend_);
    ~Cabinet() override;

    void Initialize() override;

    bool TransactionComplete() const override;
    Result GetStatus() const override;
    void ExecuteInteractive() override;
    void Execute() override;
    void DisplayCompleted(bool apply_changes, std::string_view amiibo_name);
    void Cancel();
    Result RequestExit() override;

private:
    const Core::Frontend::CabinetApplet& frontend;
    Core::System& system;

    bool is_complete{false};
    std::shared_ptr<Service::NFC::NfcDevice> nfp_device;
    Kernel::KEvent* availability_change_event;
    KernelHelpers::ServiceContext service_context;
    StartParamForAmiiboSettings applet_input_common{};
};

} // namespace Service::AM::Applets
