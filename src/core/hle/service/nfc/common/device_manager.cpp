// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/logging/log.h"
#include "core/core.h"
#include "core/hid/hid_types.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/nfc/common/device.h"
#include "core/hle/service/nfc/common/device_manager.h"
#include "core/hle/service/nfc/nfc_result.h"
#include "core/hle/service/time/clock_types.h"

namespace Service::NFC {

DeviceManager::DeviceManager(Core::System& system_, KernelHelpers::ServiceContext& service_context_)
    : system{system_}, service_context{service_context_} {

    availability_change_event =
        service_context.CreateEvent("Nfc:DeviceManager:AvailabilityChangeEvent");

    for (u32 device_index = 0; device_index < devices.size(); device_index++) {
        devices[device_index] =
            std::make_shared<NfcDevice>(Core::HID::IndexToNpadIdType(device_index), system,
                                        service_context, availability_change_event);
    }

    is_initialized = false;
}

DeviceManager ::~DeviceManager() {
    service_context.CloseEvent(availability_change_event);
}

Result DeviceManager::Initialize() {
    for (auto& device : devices) {
        device->Initialize();
    }
    is_initialized = true;
    return ResultSuccess;
}

Result DeviceManager::Finalize() {
    for (auto& device : devices) {
        device->Finalize();
    }
    is_initialized = false;
    return ResultSuccess;
}

Result DeviceManager::ListDevices(std::vector<u64>& nfp_devices,
                                  std::size_t max_allowed_devices) const {
    for (auto& device : devices) {
        if (nfp_devices.size() >= max_allowed_devices) {
            continue;
        }
        if (device->GetCurrentState() != DeviceState::Unavailable) {
            nfp_devices.push_back(device->GetHandle());
        }
    }

    if (nfp_devices.empty()) {
        return ResultDeviceNotFound;
    }

    return ResultSuccess;
}

DeviceState DeviceManager::GetDeviceState(u64 device_handle) const {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    const auto result = GetDeviceFromHandle(device_handle, device, false);

    if (result.IsSuccess()) {
        return device->GetCurrentState();
    }

    return DeviceState::Unavailable;
}

Result DeviceManager::GetNpadId(u64 device_handle, Core::HID::NpadIdType& npad_id) const {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->GetNpadId(npad_id);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Kernel::KReadableEvent& DeviceManager::AttachAvailabilityChangeEvent() const {
    return availability_change_event->GetReadableEvent();
}

Result DeviceManager::StartDetection(u64 device_handle, NfcProtocol tag_protocol) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->StartDetection(tag_protocol);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::StopDetection(u64 device_handle) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->StopDetection();
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::GetTagInfo(u64 device_handle, TagInfo& tag_info, bool is_mifare) const {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->GetTagInfo(tag_info, is_mifare);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Kernel::KReadableEvent& DeviceManager::AttachActivateEvent(u64 device_handle) const {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    GetDeviceFromHandle(device_handle, device, false);

    // TODO: Return proper error code on failure
    return device->GetActivateEvent();
}

Kernel::KReadableEvent& DeviceManager::AttachDeactivateEvent(u64 device_handle) const {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    GetDeviceFromHandle(device_handle, device, false);

    // TODO: Return proper error code on failure
    return device->GetDeactivateEvent();
}

Result DeviceManager::ReadMifare(u64 device_handle,
                                 std::span<const MifareReadBlockParameter> read_parameters,
                                 std::span<MifareReadBlockData> read_data) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->ReadMifare(read_parameters, read_data);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::WriteMifare(u64 device_handle,
                                  std::span<const MifareWriteBlockParameter> write_parameters) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->WriteMifare(write_parameters);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::SendCommandByPassThrough(u64 device_handle,
                                               const Time::Clock::TimeSpanType& timeout,
                                               std::span<const u8> command_data,
                                               std::span<u8> out_data) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->SendCommandByPassThrough(timeout, command_data, out_data);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::Mount(u64 device_handle, NFP::ModelType model_type,
                            NFP::MountTarget mount_target) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->Mount(model_type, mount_target);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::Unmount(u64 device_handle) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->Unmount();
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::OpenApplicationArea(u64 device_handle, u32 access_id) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->OpenApplicationArea(access_id);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::GetApplicationArea(u64 device_handle, std::span<u8> data) const {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->GetApplicationArea(data);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::SetApplicationArea(u64 device_handle, std::span<const u8> data) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->SetApplicationArea(data);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::Flush(u64 device_handle) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->Flush();
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::Restore(u64 device_handle) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->Restore();
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::CreateApplicationArea(u64 device_handle, u32 access_id,
                                            std::span<const u8> data) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->CreateApplicationArea(access_id, data);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::GetRegisterInfo(u64 device_handle, NFP::RegisterInfo& register_info) const {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->GetRegisterInfo(register_info);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::GetCommonInfo(u64 device_handle, NFP::CommonInfo& common_info) const {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->GetCommonInfo(common_info);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::GetModelInfo(u64 device_handle, NFP::ModelInfo& model_info) const {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->GetModelInfo(model_info);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

u32 DeviceManager::GetApplicationAreaSize() const {
    return sizeof(NFP::ApplicationArea);
}

Result DeviceManager::RecreateApplicationArea(u64 device_handle, u32 access_id,
                                              std::span<const u8> data) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->RecreateApplicationArea(access_id, data);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::Format(u64 device_handle) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->Format();
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::GetAdminInfo(u64 device_handle, NFP::AdminInfo& admin_info) const {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->GetAdminInfo(admin_info);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::GetRegisterInfoPrivate(u64 device_handle,
                                             NFP::RegisterInfoPrivate& register_info) const {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->GetRegisterInfoPrivate(register_info);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::SetRegisterInfoPrivate(u64 device_handle,
                                             const NFP::RegisterInfoPrivate& register_info) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->SetRegisterInfoPrivate(register_info);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::DeleteRegisterInfo(u64 device_handle) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->DeleteRegisterInfo();
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::DeleteApplicationArea(u64 device_handle) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->DeleteApplicationArea();
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::ExistsApplicationArea(u64 device_handle, bool& has_application_area) const {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->ExistsApplicationArea(has_application_area);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::GetAll(u64 device_handle, NFP::NfpData& nfp_data) const {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->GetAll(nfp_data);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::SetAll(u64 device_handle, const NFP::NfpData& nfp_data) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->SetAll(nfp_data);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::FlushDebug(u64 device_handle) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->FlushDebug();
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::BreakTag(u64 device_handle, NFP::BreakType break_type) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->BreakTag(break_type);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::ReadBackupData(u64 device_handle, std::span<u8> data) const {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->ReadBackupData(data);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::WriteBackupData(u64 device_handle, std::span<const u8> data) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->WriteBackupData(data);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::WriteNtf(u64 device_handle, NFP::WriteType, std::span<const u8> data) {
    std::scoped_lock lock{mutex};

    std::shared_ptr<NfcDevice> device = nullptr;
    auto result = GetDeviceHandle(device_handle, device);

    if (result.IsSuccess()) {
        result = device->WriteNtf(data);
        result = VerifyDeviceResult(device, result);
    }

    return result;
}

Result DeviceManager::GetDeviceFromHandle(u64 handle, std::shared_ptr<NfcDevice>& nfc_device,
                                          bool check_state) const {
    if (check_state) {
        const Result is_parameter_set = IsNfcParameterSet();
        if (is_parameter_set.IsError()) {
            return is_parameter_set;
        }
        const Result is_enabled = IsNfcEnabled();
        if (is_enabled.IsError()) {
            return is_enabled;
        }
        const Result is_nfc_initialized = IsNfcInitialized();
        if (is_nfc_initialized.IsError()) {
            return is_nfc_initialized;
        }
    }

    for (auto& device : devices) {
        if (device->GetHandle() == handle) {
            nfc_device = device;
            return ResultSuccess;
        }
    }

    return ResultDeviceNotFound;
}

std::optional<std::shared_ptr<NfcDevice>> DeviceManager::GetNfcDevice(u64 handle) {
    for (auto& device : devices) {
        if (device->GetHandle() == handle) {
            return device;
        }
    }
    return std::nullopt;
}

const std::optional<std::shared_ptr<NfcDevice>> DeviceManager::GetNfcDevice(u64 handle) const {
    for (auto& device : devices) {
        if (device->GetHandle() == handle) {
            return device;
        }
    }
    return std::nullopt;
}

Result DeviceManager::GetDeviceHandle(u64 handle, std::shared_ptr<NfcDevice>& device) const {
    const auto result = GetDeviceFromHandle(handle, device, true);
    if (result.IsError()) {
        return result;
    }
    return CheckDeviceState(device);
}

Result DeviceManager::VerifyDeviceResult(std::shared_ptr<NfcDevice> device,
                                         Result operation_result) const {
    if (operation_result.IsSuccess()) {
        return operation_result;
    }

    const Result is_parameter_set = IsNfcParameterSet();
    if (is_parameter_set.IsError()) {
        return is_parameter_set;
    }
    const Result is_enabled = IsNfcEnabled();
    if (is_enabled.IsError()) {
        return is_enabled;
    }
    const Result is_nfc_initialized = IsNfcInitialized();
    if (is_nfc_initialized.IsError()) {
        return is_nfc_initialized;
    }
    const Result device_state = CheckDeviceState(device);
    if (device_state.IsError()) {
        return device_state;
    }

    return operation_result;
}

Result DeviceManager::CheckDeviceState(std::shared_ptr<NfcDevice> device) const {
    if (device == nullptr) {
        return ResultInvalidArgument;
    }

    return ResultSuccess;
}

Result DeviceManager::IsNfcEnabled() const {
    // TODO: This calls nn::settings::detail::GetNfcEnableFlag
    const bool is_enabled = true;
    if (!is_enabled) {
        return ResultNfcDisabled;
    }
    return ResultSuccess;
}

Result DeviceManager::IsNfcParameterSet() const {
    // TODO: This calls checks against a bool on offset 0x450
    const bool is_set = true;
    if (!is_set) {
        return ResultUnknown76;
    }
    return ResultSuccess;
}

Result DeviceManager::IsNfcInitialized() const {
    if (!is_initialized) {
        return ResultNfcNotInitialized;
    }
    return ResultSuccess;
}

} // namespace Service::NFC
