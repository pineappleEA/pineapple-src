// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nfc/nfc_interface.h"
#include "core/hle/service/service.h"

namespace Service::NFP {

class Interface : public NFC::NfcInterface {
public:
    explicit Interface(Core::System& system_, const char* name);
    ~Interface() override;

    void InitializeSystem(HLERequestContext& ctx);
    void InitializeDebug(HLERequestContext& ctx);
    void FinalizeSystem(HLERequestContext& ctx);
    void FinalizeDebug(HLERequestContext& ctx);
    void Mount(HLERequestContext& ctx);
    void Unmount(HLERequestContext& ctx);
    void OpenApplicationArea(HLERequestContext& ctx);
    void GetApplicationArea(HLERequestContext& ctx);
    void SetApplicationArea(HLERequestContext& ctx);
    void Flush(HLERequestContext& ctx);
    void Restore(HLERequestContext& ctx);
    void CreateApplicationArea(HLERequestContext& ctx);
    void GetRegisterInfo(HLERequestContext& ctx);
    void GetCommonInfo(HLERequestContext& ctx);
    void GetModelInfo(HLERequestContext& ctx);
    void GetApplicationAreaSize(HLERequestContext& ctx);
    void RecreateApplicationArea(HLERequestContext& ctx);
    void Format(HLERequestContext& ctx);
    void GetAdminInfo(HLERequestContext& ctx);
    void GetRegisterInfoPrivate(HLERequestContext& ctx);
    void SetRegisterInfoPrivate(HLERequestContext& ctx);
    void DeleteRegisterInfo(HLERequestContext& ctx);
    void DeleteApplicationArea(HLERequestContext& ctx);
    void ExistsApplicationArea(HLERequestContext& ctx);
    void GetAll(HLERequestContext& ctx);
    void SetAll(HLERequestContext& ctx);
    void FlushDebug(HLERequestContext& ctx);
    void BreakTag(HLERequestContext& ctx);
    void ReadBackupData(HLERequestContext& ctx);
    void WriteBackupData(HLERequestContext& ctx);
    void WriteNtf(HLERequestContext& ctx);
};

} // namespace Service::NFP
