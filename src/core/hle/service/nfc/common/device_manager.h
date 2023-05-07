// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>

#include "core/hid/hid_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nfc/mifare_types.h"
#include "core/hle/service/nfc/nfc_types.h"
#include "core/hle/service/nfp/nfp_types.h"
#include "core/hle/service/service.h"
#include "core/hle/service/time/clock_types.h"

namespace Service::NFC {
class NfcDevice;

class DeviceManager {
public:
    explicit DeviceManager(Core::System& system_, KernelHelpers::ServiceContext& service_context_);
    ~DeviceManager();

    // Nfc device manager
    Result Initialize();
    Result Finalize();
    Result ListDevices(std::vector<u64>& nfp_devices, std::size_t max_allowed_devices) const;
    DeviceState GetDeviceState(u64 device_handle) const;
    Result GetNpadId(u64 device_handle, Core::HID::NpadIdType& npad_id) const;
    Kernel::KReadableEvent& AttachAvailabilityChangeEvent() const;
    Result StartDetection(u64 device_handle, NfcProtocol tag_protocol);
    Result StopDetection(u64 device_handle);
    Result GetTagInfo(u64 device_handle, NFP::TagInfo& tag_info, bool is_mifare) const;
    Kernel::KReadableEvent& AttachActivateEvent(u64 device_handle) const;
    Kernel::KReadableEvent& AttachDeactivateEvent(u64 device_handle) const;
    Result ReadMifare(u64 device_handle,
                      const std::span<const MifareReadBlockParameter> read_parameters,
                      std::span<MifareReadBlockData> read_data);
    Result WriteMifare(u64 device_handle,
                       std::span<const MifareWriteBlockParameter> write_parameters);
    Result SendCommandByPassThrough(u64 device_handle, const Time::Clock::TimeSpanType& timeout,
                                    std::span<const u8> command_data, std::span<u8> out_data);

    // Nfp device manager
    Result Mount(u64 device_handle, NFP::ModelType model_type, NFP::MountTarget mount_target);
    Result Unmount(u64 device_handle);
    Result OpenApplicationArea(u64 device_handle, u32 access_id);
    Result GetApplicationArea(u64 device_handle, std::span<u8> data) const;
    Result SetApplicationArea(u64 device_handle, std::span<const u8> data);
    Result Flush(u64 device_handle);
    Result Restore(u64 device_handle);
    Result CreateApplicationArea(u64 device_handle, u32 access_id, std::span<const u8> data);
    Result GetRegisterInfo(u64 device_handle, NFP::RegisterInfo& register_info) const;
    Result GetCommonInfo(u64 device_handle, NFP::CommonInfo& common_info) const;
    Result GetModelInfo(u64 device_handle, NFP::ModelInfo& model_info) const;
    u32 GetApplicationAreaSize() const;
    Result RecreateApplicationArea(u64 device_handle, u32 access_id, std::span<const u8> data);
    Result Format(u64 device_handle);
    Result GetAdminInfo(u64 device_handle, NFP::AdminInfo& admin_info) const;
    Result GetRegisterInfoPrivate(u64 device_handle, NFP::RegisterInfoPrivate& register_info) const;
    Result SetRegisterInfoPrivate(u64 device_handle, const NFP::RegisterInfoPrivate& register_info);
    Result DeleteRegisterInfo(u64 device_handle);
    Result DeleteApplicationArea(u64 device_handle);
    Result ExistsApplicationArea(u64 device_handle, bool& has_application_area) const;
    Result GetAll(u64 device_handle, NFP::NfpData& nfp_data) const;
    Result SetAll(u64 device_handle, const NFP::NfpData& nfp_data);
    Result FlushDebug(u64 device_handle);
    Result BreakTag(u64 device_handle, NFP::BreakType break_type);
    Result ReadBackupData(u64 device_handle, std::span<u8> data) const;
    Result WriteBackupData(u64 device_handle, std::span<const u8> data);
    Result WriteNtf(u64 device_handle, NFP::WriteType, std::span<const u8> data);

private:
    Result IsNfcEnabled() const;
    Result IsNfcParameterSet() const;
    Result IsNfcInitialized() const;

    Result GetDeviceFromHandle(u64 handle, std::shared_ptr<NfcDevice>& device,
                               bool check_state) const;

    Result GetDeviceHandle(u64 handle, std::shared_ptr<NfcDevice>& device) const;
    Result VerifyDeviceResult(std::shared_ptr<NfcDevice> device, Result operation_result) const;
    Result CheckDeviceState(std::shared_ptr<NfcDevice> device) const;

    std::optional<std::shared_ptr<NfcDevice>> GetNfcDevice(u64 handle);
    const std::optional<std::shared_ptr<NfcDevice>> GetNfcDevice(u64 handle) const;

    bool is_initialized = false;
    mutable std::mutex mutex;
    std::array<std::shared_ptr<NfcDevice>, 10> devices{};

    Core::System& system;
    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* availability_change_event;
};

} // namespace Service::NFC
