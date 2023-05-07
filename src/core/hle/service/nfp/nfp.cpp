// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/nfp/nfp.h"
#include "core/hle/service/nfp/nfp_interface.h"
#include "core/hle/service/server_manager.h"

namespace Service::NFP {

class IUser final : public Interface {
public:
    explicit IUser(Core::System& system_) : Interface(system_, "NFP:IUser") {
        // clang-format off
        static const FunctionInfoTyped<IUser> functions[] = {
            {0, &IUser::Initialize, "Initialize"},
            {1, &IUser::Finalize, "Finalize"},
            {2, &IUser::ListDevices, "ListDevices"},
            {3, &IUser::StartDetection, "StartDetection"},
            {4, &IUser::StopDetection, "StopDetection"},
            {5, &IUser::Mount, "Mount"},
            {6, &IUser::Unmount, "Unmount"},
            {7, &IUser::OpenApplicationArea, "OpenApplicationArea"},
            {8, &IUser::GetApplicationArea, "GetApplicationArea"},
            {9, &IUser::SetApplicationArea, "SetApplicationArea"},
            {10, &IUser::Flush, "Flush"},
            {11, &IUser::Restore, "Restore"},
            {12, &IUser::CreateApplicationArea, "CreateApplicationArea"},
            {13, &IUser::GetTagInfo, "GetTagInfo"},
            {14, &IUser::GetRegisterInfo, "GetRegisterInfo"},
            {15, &IUser::GetCommonInfo, "GetCommonInfo"},
            {16, &IUser::GetModelInfo, "GetModelInfo"},
            {17, &IUser::AttachActivateEvent, "AttachActivateEvent"},
            {18, &IUser::AttachDeactivateEvent, "AttachDeactivateEvent"},
            {19, &IUser::GetState, "GetState"},
            {20, &IUser::GetDeviceState, "GetDeviceState"},
            {21, &IUser::GetNpadId, "GetNpadId"},
            {22, &IUser::GetApplicationAreaSize, "GetApplicationAreaSize"},
            {23, &IUser::AttachAvailabilityChangeEvent, "AttachAvailabilityChangeEvent"},
            {24, &IUser::RecreateApplicationArea, "RecreateApplicationArea"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class ISystem final : public Interface {
public:
    explicit ISystem(Core::System& system_) : Interface(system_, "NFP:ISystem") {
        // clang-format off
        static const FunctionInfoTyped<ISystem> functions[] = {
            {0, &ISystem::InitializeSystem, "InitializeSystem"},
            {1, &ISystem::FinalizeSystem, "FinalizeSystem"},
            {2, &ISystem::ListDevices, "ListDevices"},
            {3, &ISystem::StartDetection, "StartDetection"},
            {4, &ISystem::StopDetection, "StopDetection"},
            {5, &ISystem::Mount, "Mount"},
            {6, &ISystem::Unmount, "Unmount"},
            {10, &ISystem::Flush, "Flush"},
            {11, &ISystem::Restore, "Restore"},
            {12, &ISystem::CreateApplicationArea, "CreateApplicationArea"},
            {13, &ISystem::GetTagInfo, "GetTagInfo"},
            {14, &ISystem::GetRegisterInfo, "GetRegisterInfo"},
            {15, &ISystem::GetCommonInfo, "GetCommonInfo"},
            {16, &ISystem::GetModelInfo, "GetModelInfo"},
            {17, &ISystem::AttachActivateEvent, "AttachActivateEvent"},
            {18, &ISystem::AttachDeactivateEvent, "AttachDeactivateEvent"},
            {19, &ISystem::GetState, "GetState"},
            {20, &ISystem::GetDeviceState, "GetDeviceState"},
            {21, &ISystem::GetNpadId, "GetNpadId"},
            {23, &ISystem::AttachAvailabilityChangeEvent, "AttachAvailabilityChangeEvent"},
            {100, &ISystem::Format, "Format"},
            {101, &ISystem::GetAdminInfo, "GetAdminInfo"},
            {102, &ISystem::GetRegisterInfoPrivate, "GetRegisterInfoPrivate"},
            {103, &ISystem::SetRegisterInfoPrivate, "SetRegisterInfoPrivate"},
            {104, &ISystem::DeleteRegisterInfo, "DeleteRegisterInfo"},
            {105, &ISystem::DeleteApplicationArea, "DeleteApplicationArea"},
            {106, &ISystem::ExistsApplicationArea, "ExistsApplicationArea"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class IDebug final : public Interface {
public:
    explicit IDebug(Core::System& system_) : Interface(system_, "NFP:IDebug") {
        // clang-format off
        static const FunctionInfoTyped<IDebug> functions[] = {
            {0, &IDebug::InitializeDebug, "InitializeDebug"},
            {1, &IDebug::FinalizeDebug, "FinalizeDebug"},
            {2, &IDebug::ListDevices, "ListDevices"},
            {3, &IDebug::StartDetection, "StartDetection"},
            {4, &IDebug::StopDetection, "StopDetection"},
            {5, &IDebug::Mount, "Mount"},
            {6, &IDebug::Unmount, "Unmount"},
            {7, &IDebug::OpenApplicationArea, "OpenApplicationArea"},
            {8, &IDebug::GetApplicationArea, "GetApplicationArea"},
            {9, &IDebug::SetApplicationArea, "SetApplicationArea"},
            {10, &IDebug::Flush, "Flush"},
            {11, &IDebug::Restore, "Restore"},
            {12, &IDebug::CreateApplicationArea, "CreateApplicationArea"},
            {13, &IDebug::GetTagInfo, "GetTagInfo"},
            {14, &IDebug::GetRegisterInfo, "GetRegisterInfo"},
            {15, &IDebug::GetCommonInfo, "GetCommonInfo"},
            {16, &IDebug::GetModelInfo, "GetModelInfo"},
            {17, &IDebug::AttachActivateEvent, "AttachActivateEvent"},
            {18, &IDebug::AttachDeactivateEvent, "AttachDeactivateEvent"},
            {19, &IDebug::GetState, "GetState"},
            {20, &IDebug::GetDeviceState, "GetDeviceState"},
            {21, &IDebug::GetNpadId, "GetNpadId"},
            {22, &IDebug::GetApplicationAreaSize, "GetApplicationAreaSize"},
            {23, &IDebug::AttachAvailabilityChangeEvent, "AttachAvailabilityChangeEvent"},
            {24, &IDebug::RecreateApplicationArea, "RecreateApplicationArea"},
            {100, &IDebug::Format, "Format"},
            {101, &IDebug::GetAdminInfo, "GetAdminInfo"},
            {102, &IDebug::GetRegisterInfoPrivate, "GetRegisterInfoPrivate"},
            {103, &IDebug::SetRegisterInfoPrivate, "SetRegisterInfoPrivate"},
            {104, &IDebug::DeleteRegisterInfo, "DeleteRegisterInfo"},
            {105, &IDebug::DeleteApplicationArea, "DeleteApplicationArea"},
            {106, &IDebug::ExistsApplicationArea, "ExistsApplicationArea"},
            {200, &IDebug::GetAll, "GetAll"},
            {201, &IDebug::SetAll, "SetAll"},
            {202, &IDebug::FlushDebug, "FlushDebug"},
            {203, &IDebug::BreakTag, "BreakTag"},
            {204, &IDebug::ReadBackupData, "ReadBackupData"},
            {205, &IDebug::WriteBackupData, "WriteBackupData"},
            {206, &IDebug::WriteNtf, "WriteNtf"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class IUserManager final : public ServiceFramework<IUserManager> {
public:
    explicit IUserManager(Core::System& system_) : ServiceFramework{system_, "nfp:user"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IUserManager::CreateUserInterface, "CreateUserInterface"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void CreateUserInterface(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NFP, "called");

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IUser>(system);
    }
};

class ISystemManager final : public ServiceFramework<ISystemManager> {
public:
    explicit ISystemManager(Core::System& system_) : ServiceFramework{system_, "nfp:sys"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &ISystemManager::CreateSystemInterface, "CreateSystemInterface"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void CreateSystemInterface(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NFP, "called");

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ISystem>(system);
    }
};

class IDebugManager final : public ServiceFramework<IDebugManager> {
public:
    explicit IDebugManager(Core::System& system_) : ServiceFramework{system_, "nfp:dbg"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IDebugManager::CreateDebugInterface, "CreateDebugInterface"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void CreateDebugInterface(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NFP, "called");

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IDebug>(system);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("nfp:user", std::make_shared<IUserManager>(system));
    server_manager->RegisterNamedService("nfp:sys", std::make_shared<ISystemManager>(system));
    server_manager->RegisterNamedService("nfp:dbg", std::make_shared<IDebugManager>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::NFP
