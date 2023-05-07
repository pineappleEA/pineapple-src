// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/core.h"
#include "core/hid/hid_types.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/nfc/common/device.h"
#include "core/hle/service/nfc/common/device_manager.h"
#include "core/hle/service/nfc/mifare_result.h"
#include "core/hle/service/nfc/mifare_types.h"
#include "core/hle/service/nfc/nfc_interface.h"
#include "core/hle/service/nfc/nfc_result.h"
#include "core/hle/service/nfc/nfc_types.h"
#include "core/hle/service/nfp/nfp_result.h"
#include "core/hle/service/time/clock_types.h"

namespace Service::NFC {

NfcInterface::NfcInterface(Core::System& system_, const char* name, BackendType service_backend)
    : ServiceFramework{system_, name}, service_context{system_, service_name},
      backend_type{service_backend} {}

NfcInterface ::~NfcInterface() = default;

void NfcInterface::Initialize(HLERequestContext& ctx) {
    LOG_INFO(Service_NFC, "called");

    auto manager = GetManager();
    auto result = manager->Initialize();

    if (result.IsSuccess()) {
        state = State::Initialized;
    } else {
        manager->Finalize();
    }

    IPC::ResponseBuilder rb{ctx, 2, 0};
    rb.Push(result);
}

void NfcInterface::Finalize(HLERequestContext& ctx) {
    LOG_INFO(Service_NFC, "called");

    if (state != State::NonInitialized) {
        if (GetBackendType() != BackendType::None) {
            GetManager()->Finalize();
        }
        device_manager = nullptr;
        state = State::NonInitialized;
    }

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void NfcInterface::GetState(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NFC, "called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushEnum(state);
}

void NfcInterface::IsNfcEnabled(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NFC, "called");

    // TODO: This calls nn::settings::detail::GetNfcEnableFlag
    const bool is_enabled = true;

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(is_enabled);
}

void NfcInterface::ListDevices(HLERequestContext& ctx) {
    std::vector<u64> nfp_devices;
    const std::size_t max_allowed_devices = ctx.GetWriteBufferNumElements<u64>();
    LOG_DEBUG(Service_NFC, "called");

    auto result = GetManager()->ListDevices(nfp_devices, max_allowed_devices);
    result = TranslateResultToServiceError(result);

    if (result.IsError()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result);
        return;
    }

    ctx.WriteBuffer(nfp_devices);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(static_cast<s32>(nfp_devices.size()));
}

void NfcInterface::GetDeviceState(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_DEBUG(Service_NFC, "called, device_handle={}", device_handle);

    const auto device_state = GetManager()->GetDeviceState(device_handle);

    if (device_state > DeviceState::Finalized) {
        ASSERT_MSG(false, "Invalid device state");
    }

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushEnum(device_state);
}

void NfcInterface::GetNpadId(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_DEBUG(Service_NFC, "called, device_handle={}", device_handle);

    Core::HID::NpadIdType npad_id{};
    auto result = GetManager()->GetNpadId(device_handle, npad_id);
    result = TranslateResultToServiceError(result);

    if (result.IsError()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result);
        return;
    }

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushEnum(npad_id);
}

void NfcInterface::AttachAvailabilityChangeEvent(HLERequestContext& ctx) {
    LOG_INFO(Service_NFC, "called");

    IPC::ResponseBuilder rb{ctx, 2, 1};
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(GetManager()->AttachAvailabilityChangeEvent());
}

void NfcInterface::StartDetection(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    const auto tag_protocol{rp.PopEnum<NfcProtocol>()};
    LOG_INFO(Service_NFC, "called, device_handle={}, nfp_protocol={}", device_handle, tag_protocol);

    auto result = GetManager()->StartDetection(device_handle, tag_protocol);
    result = TranslateResultToServiceError(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void NfcInterface::StopDetection(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_INFO(Service_NFC, "called, device_handle={}", device_handle);

    auto result = GetManager()->StopDetection(device_handle);
    result = TranslateResultToServiceError(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void NfcInterface::GetTagInfo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_INFO(Service_NFC, "called, device_handle={}", device_handle);

    TagInfo tag_info{};
    auto result =
        GetManager()->GetTagInfo(device_handle, tag_info, backend_type == BackendType::Mifare);
    result = TranslateResultToServiceError(result);

    if (result.IsSuccess()) {
        ctx.WriteBuffer(tag_info);
    }

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void NfcInterface::AttachActivateEvent(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_DEBUG(Service_NFC, "called, device_handle={}", device_handle);

    IPC::ResponseBuilder rb{ctx, 2, 1};
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(GetManager()->AttachActivateEvent(device_handle));
}

void NfcInterface::AttachDeactivateEvent(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    LOG_DEBUG(Service_NFC, "called, device_handle={}", device_handle);

    IPC::ResponseBuilder rb{ctx, 2, 1};
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(GetManager()->AttachDeactivateEvent(device_handle));
}

void NfcInterface::ReadMifare(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    const auto buffer{ctx.ReadBuffer()};
    const auto number_of_commands{ctx.GetReadBufferNumElements<MifareReadBlockParameter>()};
    std::vector<MifareReadBlockParameter> read_commands(number_of_commands);

    memcpy(read_commands.data(), buffer.data(),
           number_of_commands * sizeof(MifareReadBlockParameter));

    LOG_INFO(Service_NFC, "(STUBBED) called, device_handle={}, read_commands_size={}",
             device_handle, number_of_commands);

    std::vector<MifareReadBlockData> out_data(number_of_commands);
    auto result = GetManager()->ReadMifare(device_handle, read_commands, out_data);
    result = TranslateResultToServiceError(result);

    if (result.IsSuccess()) {
        ctx.WriteBuffer(out_data);
    }

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void NfcInterface::WriteMifare(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    const auto buffer{ctx.ReadBuffer()};
    const auto number_of_commands{ctx.GetReadBufferNumElements<MifareWriteBlockParameter>()};
    std::vector<MifareWriteBlockParameter> write_commands(number_of_commands);

    memcpy(write_commands.data(), buffer.data(),
           number_of_commands * sizeof(MifareWriteBlockParameter));

    LOG_INFO(Service_NFC, "(STUBBED) called, device_handle={}, write_commands_size={}",
             device_handle, number_of_commands);

    auto result = GetManager()->WriteMifare(device_handle, write_commands);
    result = TranslateResultToServiceError(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void NfcInterface::SendCommandByPassThrough(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto device_handle{rp.Pop<u64>()};
    const auto timeout{rp.PopRaw<Time::Clock::TimeSpanType>()};
    const auto command_data{ctx.ReadBuffer()};
    LOG_INFO(Service_NFC, "(STUBBED) called, device_handle={}, timeout={}, data_size={}",
             device_handle, timeout.ToSeconds(), command_data.size());

    std::vector<u8> out_data(1);
    auto result =
        GetManager()->SendCommandByPassThrough(device_handle, timeout, command_data, out_data);
    result = TranslateResultToServiceError(result);

    if (result.IsError()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result);
        return;
    }

    ctx.WriteBuffer(out_data);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(static_cast<u32>(out_data.size()));
}

std::shared_ptr<DeviceManager> NfcInterface::GetManager() {
    if (device_manager == nullptr) {
        device_manager = std::make_shared<DeviceManager>(system, service_context);
    }
    return device_manager;
}

BackendType NfcInterface::GetBackendType() const {
    return backend_type;
}

Result NfcInterface::TranslateResultToServiceError(Result result) const {
    const auto backend = GetBackendType();

    if (result.IsSuccess()) {
        return result;
    }

    if (result.module != ErrorModule::NFC) {
        return result;
    }

    switch (backend) {
    case BackendType::Mifare:
        return TranslateResultToNfp(result);
    case BackendType::Nfp: {
        return TranslateResultToNfp(result);
    }
    default:
        if (result != ResultUnknown216) {
            return result;
        }
        return ResultUnknown74;
    }
}

Result NfcInterface::TranslateResultToNfp(Result result) const {
    if (result == ResultDeviceNotFound) {
        return NFP::ResultDeviceNotFound;
    }
    if (result == ResultInvalidArgument) {
        return NFP::ResultInvalidArgument;
    }
    if (result == ResultWrongApplicationAreaSize) {
        return NFP::ResultWrongApplicationAreaSize;
    }
    if (result == ResultWrongDeviceState) {
        return NFP::ResultWrongDeviceState;
    }
    if (result == ResultUnknown74) {
        return NFP::ResultUnknown74;
    }
    if (result == ResultNfcDisabled) {
        return NFP::ResultNfcDisabled;
    }
    if (result == ResultNfcNotInitialized) {
        return NFP::ResultNfcDisabled;
    }
    if (result == ResultWriteAmiiboFailed) {
        return NFP::ResultWriteAmiiboFailed;
    }
    if (result == ResultTagRemoved) {
        return NFP::ResultTagRemoved;
    }
    if (result == ResultRegistrationIsNotInitialized) {
        return NFP::ResultRegistrationIsNotInitialized;
    }
    if (result == ResultApplicationAreaIsNotInitialized) {
        return NFP::ResultApplicationAreaIsNotInitialized;
    }
    if (result == ResultCorruptedData) {
        return NFP::ResultCorruptedData;
    }
    if (result == ResultWrongApplicationAreaId) {
        return NFP::ResultWrongApplicationAreaId;
    }
    if (result == ResultApplicationAreaExist) {
        return NFP::ResultApplicationAreaExist;
    }
    if (result == ResultNotAnAmiibo) {
        return NFP::ResultNotAnAmiibo;
    }
    LOG_WARNING(Service_NFC, "Result conversion not handled");
    return result;
}

Result NfcInterface::TranslateResultToMifare(Result result) const {
    if (result == ResultDeviceNotFound) {
        return Mifare::ResultDeviceNotFound;
    }
    if (result == ResultInvalidArgument) {
        return Mifare::ResultInvalidArgument;
    }
    if (result == ResultWrongDeviceState) {
        return Mifare::ResultWrongDeviceState;
    }
    if (result == ResultNfcDisabled) {
        return Mifare::ResultNfcDisabled;
    }
    if (result == ResultTagRemoved) {
        return Mifare::ResultTagRemoved;
    }
    LOG_WARNING(Service_NFC, "Result conversion not handled");
    return result;
}

} // namespace Service::NFC
