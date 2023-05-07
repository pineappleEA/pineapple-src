// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>

#include "common/logging/log.h"
#include "common/settings.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/nfc/nfc.h"
#include "core/hle/service/nfc/nfc_interface.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"

namespace Service::NFC {

class IUser final : public NfcInterface {
public:
    explicit IUser(Core::System& system_) : NfcInterface(system_, "NFC::IUser", BackendType::Nfc) {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &NfcInterface::Initialize, "InitializeOld"},
            {1, &NfcInterface::Finalize, "FinalizeOld"},
            {2, &NfcInterface::GetState, "GetStateOld"},
            {3, &NfcInterface::IsNfcEnabled, "IsNfcEnabledOld"},
            {400, &NfcInterface::Initialize, "Initialize"},
            {401, &NfcInterface::Finalize, "Finalize"},
            {402, &NfcInterface::GetState, "GetState"},
            {403, &NfcInterface::IsNfcEnabled, "IsNfcEnabled"},
            {404, &NfcInterface::ListDevices, "ListDevices"},
            {405, &NfcInterface::GetDeviceState, "GetDeviceState"},
            {406, &NfcInterface::GetNpadId, "GetNpadId"},
            {407, &NfcInterface::AttachAvailabilityChangeEvent, "AttachAvailabilityChangeEvent"},
            {408, &NfcInterface::StartDetection, "StartDetection"},
            {409, &NfcInterface::StopDetection, "StopDetection"},
            {410, &NfcInterface::GetTagInfo, "GetTagInfo"},
            {411, &NfcInterface::AttachActivateEvent, "AttachActivateEvent"},
            {412, &NfcInterface::AttachDeactivateEvent, "AttachDeactivateEvent"},
            {1000, &NfcInterface::ReadMifare, "ReadMifare"},
            {1001, &NfcInterface::WriteMifare ,"WriteMifare"},
            {1300, &NfcInterface::SendCommandByPassThrough, "SendCommandByPassThrough"},
            {1301, nullptr, "KeepPassThroughSession"},
            {1302, nullptr, "ReleasePassThroughSession"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class ISystem final : public NfcInterface {
public:
    explicit ISystem(Core::System& system_)
        : NfcInterface{system_, "NFC::ISystem", BackendType::Nfc} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &NfcInterface::Initialize, "InitializeOld"},
            {1, &NfcInterface::Finalize, "FinalizeOld"},
            {2, &NfcInterface::GetState, "GetStateOld"},
            {3, &NfcInterface::IsNfcEnabled, "IsNfcEnabledOld"},
            {100, nullptr, "SetNfcEnabledOld"},
            {400, &NfcInterface::Initialize, "Initialize"},
            {401, &NfcInterface::Finalize, "Finalize"},
            {402, &NfcInterface::GetState, "GetState"},
            {403, &NfcInterface::IsNfcEnabled, "IsNfcEnabled"},
            {404, &NfcInterface::ListDevices, "ListDevices"},
            {405, &NfcInterface::GetDeviceState, "GetDeviceState"},
            {406, &NfcInterface::GetNpadId, "GetNpadId"},
            {407, &NfcInterface::AttachAvailabilityChangeEvent, "AttachAvailabilityChangeEvent"},
            {408, &NfcInterface::StartDetection, "StartDetection"},
            {409, &NfcInterface::StopDetection, "StopDetection"},
            {410, &NfcInterface::GetTagInfo, "GetTagInfo"},
            {411, &NfcInterface::AttachActivateEvent, "AttachActivateEvent"},
            {412, &NfcInterface::AttachDeactivateEvent, "AttachDeactivateEvent"},
            {500, nullptr, "SetNfcEnabled"},
            {510, nullptr, "OutputTestWave"},
            {1000, &NfcInterface::ReadMifare, "ReadMifare"},
            {1001, &NfcInterface::WriteMifare, "WriteMifare"},
            {1300, &NfcInterface::SendCommandByPassThrough, "SendCommandByPassThrough"},
            {1301, nullptr, "KeepPassThroughSession"},
            {1302, nullptr, "ReleasePassThroughSession"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

// MFInterface has an unique interface but it's identical to NfcInterface so we can keep the code
// simpler
using MFInterface = NfcInterface;
class MFIUser final : public MFInterface {
public:
    explicit MFIUser(Core::System& system_)
        : MFInterface{system_, "NFC::MFInterface", BackendType::Mifare} {
        // clang-format off
        static const FunctionInfoTyped<MFIUser> functions[] = {
            {0, &MFIUser::Initialize, "Initialize"},
            {1, &MFIUser::Finalize, "Finalize"},
            {2, &MFIUser::ListDevices, "ListDevices"},
            {3, &MFIUser::StartDetection, "StartDetection"},
            {4, &MFIUser::StopDetection, "StopDetection"},
            {5, &MFIUser::ReadMifare, "Read"},
            {6, &MFIUser::WriteMifare, "Write"},
            {7, &MFIUser::GetTagInfo, "GetTagInfo"},
            {8, &MFIUser::AttachActivateEvent, "GetActivateEventHandle"},
            {9, &MFIUser::AttachDeactivateEvent, "GetDeactivateEventHandle"},
            {10, &MFIUser::GetState, "GetState"},
            {11, &MFIUser::GetDeviceState, "GetDeviceState"},
            {12, &MFIUser::GetNpadId, "GetNpadId"},
            {13, &MFIUser::AttachAvailabilityChangeEvent, "GetAvailabilityChangeEventHandle"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class IAm final : public ServiceFramework<IAm> {
public:
    explicit IAm(Core::System& system_) : ServiceFramework{system_, "NFC::IAm"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "Initialize"},
            {1, nullptr, "Finalize"},
            {2, nullptr, "NotifyForegroundApplet"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class NFC_AM final : public ServiceFramework<NFC_AM> {
public:
    explicit NFC_AM(Core::System& system_) : ServiceFramework{system_, "nfc:am"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &NFC_AM::CreateAmNfcInterface, "CreateAmNfcInterface"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void CreateAmNfcInterface(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NFC, "called");

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IAm>(system);
    }
};

class NFC_MF_U final : public ServiceFramework<NFC_MF_U> {
public:
    explicit NFC_MF_U(Core::System& system_) : ServiceFramework{system_, "nfc:mf:u"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &NFC_MF_U::CreateUserNfcInterface, "CreateUserNfcInterface"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void CreateUserNfcInterface(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NFC, "called");

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<MFIUser>(system);
    }
};

class NFC_U final : public ServiceFramework<NFC_U> {
public:
    explicit NFC_U(Core::System& system_) : ServiceFramework{system_, "nfc:user"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &NFC_U::CreateUserNfcInterface, "CreateUserNfcInterface"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void CreateUserNfcInterface(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NFC, "called");

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IUser>(system);
    }
};

class NFC_SYS final : public ServiceFramework<NFC_SYS> {
public:
    explicit NFC_SYS(Core::System& system_) : ServiceFramework{system_, "nfc:sys"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &NFC_SYS::CreateSystemNfcInterface, "CreateSystemNfcInterface"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void CreateSystemNfcInterface(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NFC, "called");

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ISystem>(system);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("nfc:am", std::make_shared<NFC_AM>(system));
    server_manager->RegisterNamedService("nfc:mf:u", std::make_shared<NFC_MF_U>(system));
    server_manager->RegisterNamedService("nfc:user", std::make_shared<NFC_U>(system));
    server_manager->RegisterNamedService("nfc:sys", std::make_shared<NFC_SYS>(system));

    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::NFC
