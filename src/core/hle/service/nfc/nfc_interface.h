// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nfc/nfc_types.h"
#include "core/hle/service/service.h"

namespace Service::NFC {
class DeviceManager;

class NfcInterface : public ServiceFramework<NfcInterface> {
public:
    explicit NfcInterface(Core::System& system_, const char* name, BackendType service_backend);
    ~NfcInterface();

    void Initialize(HLERequestContext& ctx);
    void Finalize(HLERequestContext& ctx);
    void GetState(HLERequestContext& ctx);
    void IsNfcEnabled(HLERequestContext& ctx);
    void ListDevices(HLERequestContext& ctx);
    void GetDeviceState(HLERequestContext& ctx);
    void GetNpadId(HLERequestContext& ctx);
    void AttachAvailabilityChangeEvent(HLERequestContext& ctx);
    void StartDetection(HLERequestContext& ctx);
    void StopDetection(HLERequestContext& ctx);
    void GetTagInfo(HLERequestContext& ctx);
    void AttachActivateEvent(HLERequestContext& ctx);
    void AttachDeactivateEvent(HLERequestContext& ctx);
    void ReadMifare(HLERequestContext& ctx);
    void WriteMifare(HLERequestContext& ctx);
    void SendCommandByPassThrough(HLERequestContext& ctx);

protected:
    std::shared_ptr<DeviceManager> GetManager();
    BackendType GetBackendType() const;
    Result TranslateResultToServiceError(Result result) const;
    Result TranslateResultToNfp(Result result) const;
    Result TranslateResultToMifare(Result result) const;

    KernelHelpers::ServiceContext service_context;

    BackendType backend_type;
    State state{State::NonInitialized};
    std::shared_ptr<DeviceManager> device_manager = nullptr;
};

} // namespace Service::NFC
