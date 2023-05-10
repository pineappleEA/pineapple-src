// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4701) // Potentially uninitialized local variable 'result' used
#endif

#include <boost/crc.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "common/input.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "common/tiny_mt.h"
#include "core/core.h"
#include "core/hid/emulated_controller.h"
#include "core/hid/hid_core.h"
#include "core/hid/hid_types.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/mii/mii_manager.h"
#include "core/hle/service/mii/types.h"
#include "core/hle/service/nfc/common/amiibo_crypto.h"
#include "core/hle/service/nfc/common/device.h"
#include "core/hle/service/nfc/mifare_result.h"
#include "core/hle/service/nfc/nfc_result.h"
#include "core/hle/service/time/time_manager.h"
#include "core/hle/service/time/time_zone_content_manager.h"
#include "core/hle/service/time/time_zone_types.h"

namespace Service::NFC {
NfcDevice::NfcDevice(Core::HID::NpadIdType npad_id_, Core::System& system_,
                     KernelHelpers::ServiceContext& service_context_,
                     Kernel::KEvent* availability_change_event_)
    : npad_id{npad_id_}, system{system_}, service_context{service_context_},
      availability_change_event{availability_change_event_} {
    activate_event = service_context.CreateEvent("NFC:ActivateEvent");
    deactivate_event = service_context.CreateEvent("NFC:DeactivateEvent");
    npad_device = system.HIDCore().GetEmulatedController(npad_id);

    Core::HID::ControllerUpdateCallback engine_callback{
        .on_change = [this](Core::HID::ControllerTriggerType type) { NpadUpdate(type); },
        .is_npad_service = false,
    };
    is_controller_set = true;
    callback_key = npad_device->SetCallback(engine_callback);
}

NfcDevice::~NfcDevice() {
    service_context.CloseEvent(activate_event);
    service_context.CloseEvent(deactivate_event);
    if (!is_controller_set) {
        return;
    }
    npad_device->DeleteCallback(callback_key);
    is_controller_set = false;
};

void NfcDevice::NpadUpdate(Core::HID::ControllerTriggerType type) {
    if (!is_initalized) {
        return;
    }

    if (type == Core::HID::ControllerTriggerType::Connected) {
        Initialize();
        availability_change_event->Signal();
        return;
    }

    if (type == Core::HID::ControllerTriggerType::Disconnected) {
        device_state = DeviceState::Unavailable;
        availability_change_event->Signal();
        return;
    }

    if (type != Core::HID::ControllerTriggerType::Nfc) {
        return;
    }

    if (!npad_device->IsConnected()) {
        return;
    }

    const auto nfc_status = npad_device->GetNfc();
    switch (nfc_status.state) {
    case Common::Input::NfcState::NewAmiibo:
        LoadNfcTag(nfc_status.data);
        break;
    case Common::Input::NfcState::AmiiboRemoved:
        if (device_state == DeviceState::Initialized || device_state == DeviceState::TagRemoved) {
            break;
        }
        if (device_state != DeviceState::SearchingForTag) {
            CloseNfcTag();
        }
        break;
    default:
        break;
    }
}

bool NfcDevice::LoadNfcTag(std::span<const u8> data) {
    if (device_state != DeviceState::SearchingForTag) {
        LOG_ERROR(Service_NFC, "Game is not looking for nfc tag, current state {}", device_state);
        return false;
    }

    if (data.size() < sizeof(NFP::EncryptedNTAG215File)) {
        LOG_ERROR(Service_NFC, "Not an amiibo, size={}", data.size());
        return false;
    }

    mifare_data.resize(data.size());
    memcpy(mifare_data.data(), data.data(), data.size());

    memcpy(&tag_data, data.data(), sizeof(NFP::EncryptedNTAG215File));
    is_plain_amiibo = NFP::AmiiboCrypto::IsAmiiboValid(tag_data);

    if (is_plain_amiibo) {
        encrypted_tag_data = NFP::AmiiboCrypto::EncodedDataToNfcData(tag_data);
        LOG_INFO(Service_NFP, "Using plain amiibo");
    } else {
        tag_data = {};
        memcpy(&encrypted_tag_data, data.data(), sizeof(NFP::EncryptedNTAG215File));
    }

    device_state = DeviceState::TagFound;
    deactivate_event->GetReadableEvent().Clear();
    activate_event->Signal();
    return true;
}

void NfcDevice::CloseNfcTag() {
    LOG_INFO(Service_NFC, "Remove nfc tag");

    if (device_state == DeviceState::TagMounted) {
        Unmount();
    }

    device_state = DeviceState::TagRemoved;
    encrypted_tag_data = {};
    tag_data = {};
    mifare_data = {};
    activate_event->GetReadableEvent().Clear();
    deactivate_event->Signal();
}

Kernel::KReadableEvent& NfcDevice::GetActivateEvent() const {
    return activate_event->GetReadableEvent();
}

Kernel::KReadableEvent& NfcDevice::GetDeactivateEvent() const {
    return deactivate_event->GetReadableEvent();
}

void NfcDevice::Initialize() {
    device_state = npad_device->HasNfc() ? DeviceState::Initialized : DeviceState::Unavailable;
    encrypted_tag_data = {};
    tag_data = {};
    mifare_data = {};
    is_initalized = true;
}

void NfcDevice::Finalize() {
    if (device_state == DeviceState::TagMounted) {
        Unmount();
    }
    if (device_state == DeviceState::SearchingForTag || device_state == DeviceState::TagRemoved) {
        StopDetection();
    }
    device_state = DeviceState::Unavailable;
    is_initalized = false;
}

Result NfcDevice::StartDetection(NfcProtocol allowed_protocol) {
    if (device_state != DeviceState::Initialized && device_state != DeviceState::TagRemoved) {
        LOG_ERROR(Service_NFC, "Wrong device state {}", device_state);
        return ResultWrongDeviceState;
    }

    if (npad_device->SetPollingMode(Core::HID::EmulatedDeviceIndex::RightIndex,
                                    Common::Input::PollingMode::NFC) !=
        Common::Input::DriverResult::Success) {
        LOG_ERROR(Service_NFC, "Nfc not supported");
        return ResultNfcDisabled;
    }

    device_state = DeviceState::SearchingForTag;
    allowed_protocols = allowed_protocol;
    return ResultSuccess;
}

Result NfcDevice::StopDetection() {
    npad_device->SetPollingMode(Core::HID::EmulatedDeviceIndex::RightIndex,
                                Common::Input::PollingMode::Active);

    if (device_state == DeviceState::Initialized) {
        return ResultSuccess;
    }

    if (device_state == DeviceState::TagFound || device_state == DeviceState::TagMounted) {
        CloseNfcTag();
    }

    if (device_state == DeviceState::SearchingForTag || device_state == DeviceState::TagRemoved) {
        device_state = DeviceState::Initialized;
        return ResultSuccess;
    }

    LOG_ERROR(Service_NFC, "Wrong device state {}", device_state);
    return ResultWrongDeviceState;
}

Result NfcDevice::GetTagInfo(NFP::TagInfo& tag_info, bool is_mifare) const {
    if (device_state != DeviceState::TagFound && device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFC, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    UniqueSerialNumber uuid = encrypted_tag_data.uuid.uid;

    // Generate random UUID to bypass amiibo load limits
    if (Settings::values.random_amiibo_id) {
        Common::TinyMT rng{};
        rng.Initialize(static_cast<u32>(GetCurrentPosixTime()));
        rng.GenerateRandomBytes(uuid.data(), sizeof(UniqueSerialNumber));
        uuid[3] = 0x88 ^ uuid[0] ^ uuid[1] ^ uuid[2];
    }

    if (is_mifare) {
        tag_info = {
            .uuid = uuid,
            .uuid_extension = {},
            .uuid_length = static_cast<u8>(uuid.size()),
            .protocol = NfcProtocol::TypeA,
            .tag_type = TagType::Type4,
        };
        return ResultSuccess;
    }

    // Protocol and tag type may change here
    tag_info = {
        .uuid = uuid,
        .uuid_extension = {},
        .uuid_length = static_cast<u8>(uuid.size()),
        .protocol = NfcProtocol::TypeA,
        .tag_type = TagType::Type2,
    };

    return ResultSuccess;
}

Result NfcDevice::ReadMifare(std::span<const MifareReadBlockParameter> parameters,
                             std::span<MifareReadBlockData> read_block_data) const {
    Result result = ResultSuccess;

    for (std::size_t i = 0; i < parameters.size(); i++) {
        result = ReadMifare(parameters[i], read_block_data[i]);
        if (result.IsError()) {
            break;
        }
    }

    return result;
}

Result NfcDevice::ReadMifare(const MifareReadBlockParameter& parameter,
                             MifareReadBlockData& read_block_data) const {
    const std::size_t sector_index = parameter.sector_number * sizeof(DataBlock);
    read_block_data.sector_number = parameter.sector_number;

    if (device_state != DeviceState::TagFound && device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFC, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mifare_data.size() < sector_index + sizeof(DataBlock)) {
        return Mifare::ResultReadError;
    }

    // TODO: Use parameter.sector_key to read encrypted data
    memcpy(read_block_data.data.data(), mifare_data.data() + sector_index, sizeof(DataBlock));

    return ResultSuccess;
}

Result NfcDevice::WriteMifare(std::span<const MifareWriteBlockParameter> parameters) {
    Result result = ResultSuccess;

    for (std::size_t i = 0; i < parameters.size(); i++) {
        result = WriteMifare(parameters[i]);
        if (result.IsError()) {
            break;
        }
    }

    if (!npad_device->WriteNfc(mifare_data)) {
        LOG_ERROR(Service_NFP, "Error writing to file");
        return Mifare::ResultReadError;
    }

    return result;
}

Result NfcDevice::WriteMifare(const MifareWriteBlockParameter& parameter) {
    const std::size_t sector_index = parameter.sector_number * sizeof(DataBlock);

    if (device_state != DeviceState::TagFound && device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFC, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mifare_data.size() < sector_index + sizeof(DataBlock)) {
        return Mifare::ResultReadError;
    }

    // TODO: Use parameter.sector_key to encrypt the data
    memcpy(mifare_data.data() + sector_index, parameter.data.data(), sizeof(DataBlock));

    return ResultSuccess;
}

Result NfcDevice::SendCommandByPassThrough(const Time::Clock::TimeSpanType& timeout,
                                           std::span<const u8> command_data,
                                           std::span<u8> out_data) {
    // Not implemented
    return ResultSuccess;
}

Result NfcDevice::Mount(NFP::ModelType model_type, NFP::MountTarget mount_target_) {
    if (device_state != DeviceState::TagFound) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        return ResultWrongDeviceState;
    }

    // The loaded amiibo is not encrypted
    if (is_plain_amiibo) {
        device_state = DeviceState::TagMounted;
        mount_target = mount_target_;
        return ResultSuccess;
    }

    if (!NFP::AmiiboCrypto::IsAmiiboValid(encrypted_tag_data)) {
        LOG_ERROR(Service_NFP, "Not an amiibo");
        return ResultNotAnAmiibo;
    }

    // Mark amiibos as read only when keys are missing
    if (!NFP::AmiiboCrypto::IsKeyAvailable()) {
        LOG_ERROR(Service_NFP, "No keys detected");
        device_state = DeviceState::TagMounted;
        mount_target = NFP::MountTarget::Rom;
        return ResultSuccess;
    }

    if (!NFP::AmiiboCrypto::DecodeAmiibo(encrypted_tag_data, tag_data)) {
        LOG_ERROR(Service_NFP, "Can't decode amiibo {}", device_state);
        return ResultCorruptedData;
    }

    device_state = DeviceState::TagMounted;
    mount_target = mount_target_;
    return ResultSuccess;
}

Result NfcDevice::Unmount() {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    // Save data before unloading the amiibo
    if (is_data_moddified) {
        Flush();
    }

    device_state = DeviceState::TagFound;
    mount_target = NFP::MountTarget::None;
    is_app_area_open = false;

    return ResultSuccess;
}

Result NfcDevice::Flush() {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFP, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    auto& settings = tag_data.settings;

    const auto& current_date = GetAmiiboDate(GetCurrentPosixTime());
    if (settings.write_date.raw_date != current_date.raw_date) {
        settings.write_date = current_date;
        UpdateSettingsCrc();
    }

    tag_data.write_counter++;

    FlushWithBreak(NFP::BreakType::Normal);

    is_data_moddified = false;

    return ResultSuccess;
}

Result NfcDevice::FlushDebug() {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFC, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFC, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    tag_data.write_counter++;

    FlushWithBreak(NFP::BreakType::Normal);

    is_data_moddified = false;

    return ResultSuccess;
}

Result NfcDevice::FlushWithBreak(NFP::BreakType break_type) {
    if (break_type != NFP::BreakType::Normal) {
        LOG_ERROR(Service_NFC, "Break type not implemented {}", break_type);
        return ResultWrongDeviceState;
    }

    std::vector<u8> data(sizeof(NFP::EncryptedNTAG215File));
    if (is_plain_amiibo) {
        memcpy(data.data(), &tag_data, sizeof(tag_data));
    } else {
        if (!NFP::AmiiboCrypto::EncodeAmiibo(tag_data, encrypted_tag_data)) {
            LOG_ERROR(Service_NFP, "Failed to encode data");
            return ResultWriteAmiiboFailed;
        }

        memcpy(data.data(), &encrypted_tag_data, sizeof(encrypted_tag_data));
    }

    if (!npad_device->WriteNfc(data)) {
        LOG_ERROR(Service_NFP, "Error writing to file");
        return ResultWriteAmiiboFailed;
    }

    return ResultSuccess;
}

Result NfcDevice::Restore() {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFC, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    // TODO: Load amiibo from backup on system
    LOG_ERROR(Service_NFP, "Not Implemented");
    return ResultSuccess;
}

Result NfcDevice::GetCommonInfo(NFP::CommonInfo& common_info) const {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFP, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    const auto& settings = tag_data.settings;

    // TODO: Validate this data
    common_info = {
        .last_write_date = settings.write_date.GetWriteDate(),
        .write_counter = tag_data.write_counter,
        .version = tag_data.amiibo_version,
        .application_area_size = sizeof(NFP::ApplicationArea),
    };
    return ResultSuccess;
}

Result NfcDevice::GetModelInfo(NFP::ModelInfo& model_info) const {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    const auto& model_info_data = encrypted_tag_data.user_memory.model_info;

    model_info = {
        .character_id = model_info_data.character_id,
        .character_variant = model_info_data.character_variant,
        .amiibo_type = model_info_data.amiibo_type,
        .model_number = model_info_data.model_number,
        .series = model_info_data.series,
    };
    return ResultSuccess;
}

Result NfcDevice::GetRegisterInfo(NFP::RegisterInfo& register_info) const {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFP, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    if (tag_data.settings.settings.amiibo_initialized == 0) {
        return ResultRegistrationIsNotInitialized;
    }

    Service::Mii::MiiManager manager;
    const auto& settings = tag_data.settings;

    // TODO: Validate this data
    register_info = {
        .mii_char_info = manager.ConvertV3ToCharInfo(tag_data.owner_mii),
        .creation_date = settings.init_date.GetWriteDate(),
        .amiibo_name = GetAmiiboName(settings),
        .font_region = settings.settings.font_region,
    };

    return ResultSuccess;
}

Result NfcDevice::GetRegisterInfoPrivate(NFP::RegisterInfoPrivate& register_info) const {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFP, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    if (tag_data.settings.settings.amiibo_initialized == 0) {
        return ResultRegistrationIsNotInitialized;
    }

    Service::Mii::MiiManager manager;
    const auto& settings = tag_data.settings;

    // TODO: Validate and complete this data
    register_info = {
        .mii_store_data = {},
        .creation_date = settings.init_date.GetWriteDate(),
        .amiibo_name = GetAmiiboName(settings),
        .font_region = settings.settings.font_region,
    };

    return ResultSuccess;
}

Result NfcDevice::GetAdminInfo(NFP::AdminInfo& admin_info) const {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFC, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFC, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    u8 flags = static_cast<u8>(tag_data.settings.settings.raw >> 0x4);
    if (tag_data.settings.settings.amiibo_initialized == 0) {
        flags = flags & 0xfe;
    }

    u64 application_id = 0;
    u32 application_area_id = 0;
    NFP::AppAreaVersion app_area_version = NFP::AppAreaVersion::NotSet;
    if (tag_data.settings.settings.appdata_initialized != 0) {
        application_id = tag_data.application_id;
        app_area_version = static_cast<NFP::AppAreaVersion>(
            application_id >> NFP::application_id_version_offset & 0xf);

        // Restore application id to original value
        if (application_id >> 0x38 != 0) {
            const u8 application_byte = tag_data.application_id_byte & 0xf;
            application_id =
                RemoveVersionByte(application_id) |
                (static_cast<u64>(application_byte) << NFP::application_id_version_offset);
        }

        application_area_id = tag_data.application_area_id;
    }

    // TODO: Validate this data
    admin_info = {
        .application_id = application_id,
        .application_area_id = application_area_id,
        .crc_change_counter = tag_data.settings.crc_counter,
        .flags = flags,
        .tag_type = PackedTagType::Type2,
        .app_area_version = app_area_version,
    };

    return ResultSuccess;
}

Result NfcDevice::DeleteRegisterInfo() {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFC, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFC, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    if (tag_data.settings.settings.amiibo_initialized == 0) {
        return ResultRegistrationIsNotInitialized;
    }

    Common::TinyMT rng{};
    rng.Initialize(static_cast<u32>(GetCurrentPosixTime()));
    rng.GenerateRandomBytes(&tag_data.owner_mii, sizeof(tag_data.owner_mii));
    rng.GenerateRandomBytes(&tag_data.settings.amiibo_name, sizeof(tag_data.settings.amiibo_name));
    rng.GenerateRandomBytes(&tag_data.unknown, sizeof(u8));
    rng.GenerateRandomBytes(&tag_data.unknown2[0], sizeof(u32));
    rng.GenerateRandomBytes(&tag_data.unknown2[1], sizeof(u32));
    rng.GenerateRandomBytes(&tag_data.register_info_crc, sizeof(u32));
    rng.GenerateRandomBytes(&tag_data.settings.init_date, sizeof(u32));
    tag_data.settings.settings.font_region.Assign(0);
    tag_data.settings.settings.amiibo_initialized.Assign(0);

    return Flush();
}

Result NfcDevice::SetRegisterInfoPrivate(const NFP::RegisterInfoPrivate& register_info) {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFP, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    Service::Mii::MiiManager manager;
    const auto mii = manager.BuildDefault(0);
    auto& settings = tag_data.settings;

    if (tag_data.settings.settings.amiibo_initialized == 0) {
        settings.init_date = GetAmiiboDate(GetCurrentPosixTime());
        settings.write_date.raw_date = 0;
    }

    SetAmiiboName(settings, register_info.amiibo_name);
    tag_data.owner_mii = manager.BuildFromStoreData(mii);
    tag_data.mii_extension = manager.SetFromStoreData(mii);
    tag_data.unknown = 0;
    tag_data.unknown2 = {};
    settings.country_code_id = 0;
    settings.settings.font_region.Assign(0);
    settings.settings.amiibo_initialized.Assign(1);

    UpdateRegisterInfoCrc();

    return Flush();
}

Result NfcDevice::RestoreAmiibo() {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFP, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    // TODO: Load amiibo from backup on system
    LOG_ERROR(Service_NFP, "Not Implemented");
    return ResultSuccess;
}

Result NfcDevice::Format() {
    auto result1 = DeleteApplicationArea();
    auto result2 = DeleteRegisterInfo();

    if (result1.IsError()) {
        return result1;
    }

    if (result2.IsError()) {
        return result2;
    }

    return Flush();
}

Result NfcDevice::OpenApplicationArea(u32 access_id) {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFP, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    if (tag_data.settings.settings.appdata_initialized.Value() == 0) {
        LOG_WARNING(Service_NFP, "Application area is not initialized");
        return ResultApplicationAreaIsNotInitialized;
    }

    if (tag_data.application_area_id != access_id) {
        LOG_WARNING(Service_NFP, "Wrong application area id");
        return ResultWrongApplicationAreaId;
    }

    is_app_area_open = true;

    return ResultSuccess;
}

Result NfcDevice::GetApplicationAreaId(u32& application_area_id) const {
    application_area_id = {};

    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFP, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    if (tag_data.settings.settings.appdata_initialized.Value() == 0) {
        LOG_WARNING(Service_NFP, "Application area is not initialized");
        return ResultApplicationAreaIsNotInitialized;
    }

    application_area_id = tag_data.application_area_id;

    return ResultSuccess;
}

Result NfcDevice::GetApplicationArea(std::span<u8> data) const {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFP, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    if (!is_app_area_open) {
        LOG_ERROR(Service_NFP, "Application area is not open");
        return ResultWrongDeviceState;
    }

    if (tag_data.settings.settings.appdata_initialized.Value() == 0) {
        LOG_ERROR(Service_NFP, "Application area is not initialized");
        return ResultApplicationAreaIsNotInitialized;
    }

    memcpy(data.data(), tag_data.application_area.data(),
           std::min(data.size(), sizeof(NFP::ApplicationArea)));

    return ResultSuccess;
}

Result NfcDevice::SetApplicationArea(std::span<const u8> data) {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFP, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    if (!is_app_area_open) {
        LOG_ERROR(Service_NFP, "Application area is not open");
        return ResultWrongDeviceState;
    }

    if (tag_data.settings.settings.appdata_initialized.Value() == 0) {
        LOG_ERROR(Service_NFP, "Application area is not initialized");
        return ResultApplicationAreaIsNotInitialized;
    }

    if (data.size() > sizeof(NFP::ApplicationArea)) {
        LOG_ERROR(Service_NFP, "Wrong data size {}", data.size());
        return ResultUnknown;
    }

    Common::TinyMT rng{};
    rng.Initialize(static_cast<u32>(GetCurrentPosixTime()));
    std::memcpy(tag_data.application_area.data(), data.data(), data.size());
    // Fill remaining data with random numbers
    rng.GenerateRandomBytes(tag_data.application_area.data() + data.size(),
                            sizeof(NFP::ApplicationArea) - data.size());

    if (tag_data.application_write_counter != NFP::counter_limit) {
        tag_data.application_write_counter++;
    }

    is_data_moddified = true;

    return ResultSuccess;
}

Result NfcDevice::CreateApplicationArea(u32 access_id, std::span<const u8> data) {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (tag_data.settings.settings.appdata_initialized.Value() != 0) {
        LOG_ERROR(Service_NFP, "Application area already exist");
        return ResultApplicationAreaExist;
    }

    return RecreateApplicationArea(access_id, data);
}

Result NfcDevice::RecreateApplicationArea(u32 access_id, std::span<const u8> data) {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (is_app_area_open) {
        LOG_ERROR(Service_NFP, "Application area is open");
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFP, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    if (data.size() > sizeof(NFP::ApplicationArea)) {
        LOG_ERROR(Service_NFP, "Wrong data size {}", data.size());
        return ResultWrongApplicationAreaSize;
    }

    Common::TinyMT rng{};
    rng.Initialize(static_cast<u32>(GetCurrentPosixTime()));
    std::memcpy(tag_data.application_area.data(), data.data(), data.size());
    // Fill remaining data with random numbers
    rng.GenerateRandomBytes(tag_data.application_area.data() + data.size(),
                            sizeof(NFP::ApplicationArea) - data.size());

    if (tag_data.application_write_counter != NFP::counter_limit) {
        tag_data.application_write_counter++;
    }

    const u64 application_id = system.GetApplicationProcessProgramID();

    tag_data.application_id_byte =
        static_cast<u8>(application_id >> NFP::application_id_version_offset & 0xf);
    tag_data.application_id =
        RemoveVersionByte(application_id) | (static_cast<u64>(NFP::AppAreaVersion::NintendoSwitch)
                                             << NFP::application_id_version_offset);
    tag_data.settings.settings.appdata_initialized.Assign(1);
    tag_data.application_area_id = access_id;
    tag_data.unknown = {};
    tag_data.unknown2 = {};

    UpdateRegisterInfoCrc();

    return Flush();
}

Result NfcDevice::DeleteApplicationArea() {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFP, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFP, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    if (tag_data.settings.settings.appdata_initialized == 0) {
        return ResultApplicationAreaIsNotInitialized;
    }

    if (tag_data.application_write_counter != NFP::counter_limit) {
        tag_data.application_write_counter++;
    }

    Common::TinyMT rng{};
    rng.Initialize(static_cast<u32>(GetCurrentPosixTime()));
    rng.GenerateRandomBytes(tag_data.application_area.data(), sizeof(NFP::ApplicationArea));
    rng.GenerateRandomBytes(&tag_data.application_id, sizeof(u64));
    rng.GenerateRandomBytes(&tag_data.application_area_id, sizeof(u32));
    rng.GenerateRandomBytes(&tag_data.application_id_byte, sizeof(u8));
    tag_data.settings.settings.appdata_initialized.Assign(0);
    tag_data.unknown = {};
    tag_data.unknown2 = {};
    is_app_area_open = false;

    UpdateRegisterInfoCrc();

    return Flush();
}

Result NfcDevice::ExistsApplicationArea(bool& has_application_area) const {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFC, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFC, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    has_application_area = tag_data.settings.settings.appdata_initialized.Value() != 0;

    return ResultSuccess;
}

Result NfcDevice::GetAll(NFP::NfpData& data) const {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFC, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFC, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    NFP::CommonInfo common_info{};
    Service::Mii::MiiManager manager;
    const u64 application_id = tag_data.application_id;

    GetCommonInfo(common_info);

    data = {
        .magic = tag_data.constant_value,
        .write_counter = tag_data.write_counter,
        .settings_crc = tag_data.settings.crc,
        .common_info = common_info,
        .mii_char_info = tag_data.owner_mii,
        .mii_store_data_extension = tag_data.mii_extension,
        .creation_date = tag_data.settings.init_date.GetWriteDate(),
        .amiibo_name = tag_data.settings.amiibo_name,
        .amiibo_name_null_terminated = 0,
        .settings = tag_data.settings.settings,
        .unknown1 = tag_data.unknown,
        .register_info_crc = tag_data.register_info_crc,
        .unknown2 = tag_data.unknown2,
        .application_id = application_id,
        .access_id = tag_data.application_area_id,
        .settings_crc_counter = tag_data.settings.crc_counter,
        .font_region = tag_data.settings.settings.font_region,
        .tag_type = PackedTagType::Type2,
        .console_type = static_cast<NFP::AppAreaVersion>(
            application_id >> NFP::application_id_version_offset & 0xf),
        .application_id_byte = tag_data.application_id_byte,
        .application_area = tag_data.application_area,
    };

    return ResultSuccess;
}

Result NfcDevice::SetAll(const NFP::NfpData& data) {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFC, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFC, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    tag_data.constant_value = data.magic;
    tag_data.write_counter = data.write_counter;
    tag_data.settings.crc = data.settings_crc;
    tag_data.settings.write_date.SetWriteDate(data.common_info.last_write_date);
    tag_data.write_counter = data.common_info.write_counter;
    tag_data.amiibo_version = data.common_info.version;
    tag_data.owner_mii = data.mii_char_info;
    tag_data.mii_extension = data.mii_store_data_extension;
    tag_data.settings.init_date.SetWriteDate(data.creation_date);
    tag_data.settings.amiibo_name = data.amiibo_name;
    tag_data.settings.settings = data.settings;
    tag_data.unknown = data.unknown1;
    tag_data.register_info_crc = data.register_info_crc;
    tag_data.unknown2 = data.unknown2;
    tag_data.application_id = data.application_id;
    tag_data.application_area_id = data.access_id;
    tag_data.settings.crc_counter = data.settings_crc_counter;
    tag_data.settings.settings.font_region.Assign(data.font_region);
    tag_data.application_id_byte = data.application_id_byte;
    tag_data.application_area = data.application_area;

    return ResultSuccess;
}

Result NfcDevice::BreakTag(NFP::BreakType break_type) {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFC, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFC, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    // TODO: Complete this implementation

    return FlushWithBreak(break_type);
}

Result NfcDevice::ReadBackupData(std::span<u8> data) const {
    // Not implemented
    return ResultSuccess;
}

Result NfcDevice::WriteBackupData(std::span<const u8> data) {
    // Not implemented
    return ResultSuccess;
}

Result NfcDevice::WriteNtf(std::span<const u8> data) {
    if (device_state != DeviceState::TagMounted) {
        LOG_ERROR(Service_NFC, "Wrong device state {}", device_state);
        if (device_state == DeviceState::TagRemoved) {
            return ResultTagRemoved;
        }
        return ResultWrongDeviceState;
    }

    if (mount_target == NFP::MountTarget::None || mount_target == NFP::MountTarget::Rom) {
        LOG_ERROR(Service_NFC, "Amiibo is read only", device_state);
        return ResultWrongDeviceState;
    }

    // Not implemented

    return ResultSuccess;
}

NFP::AmiiboName NfcDevice::GetAmiiboName(const NFP::AmiiboSettings& settings) const {
    std::array<char16_t, NFP::amiibo_name_length> settings_amiibo_name{};
    NFP::AmiiboName amiibo_name{};

    // Convert from big endian to little endian
    for (std::size_t i = 0; i < NFP::amiibo_name_length; i++) {
        settings_amiibo_name[i] = static_cast<u16>(settings.amiibo_name[i]);
    }

    // Convert from utf16 to utf8
    const auto amiibo_name_utf8 = Common::UTF16ToUTF8(settings_amiibo_name.data());
    memcpy(amiibo_name.data(), amiibo_name_utf8.data(), amiibo_name_utf8.size());

    return amiibo_name;
}

void NfcDevice::SetAmiiboName(NFP::AmiiboSettings& settings, const NFP::AmiiboName& amiibo_name) {
    std::array<char16_t, NFP::amiibo_name_length> settings_amiibo_name{};

    // Convert from utf8 to utf16
    const auto amiibo_name_utf16 = Common::UTF8ToUTF16(amiibo_name.data());
    memcpy(settings_amiibo_name.data(), amiibo_name_utf16.data(),
           amiibo_name_utf16.size() * sizeof(char16_t));

    // Convert from little endian to big endian
    for (std::size_t i = 0; i < NFP::amiibo_name_length; i++) {
        settings.amiibo_name[i] = static_cast<u16_be>(settings_amiibo_name[i]);
    }
}

NFP::AmiiboDate NfcDevice::GetAmiiboDate(s64 posix_time) const {
    const auto& time_zone_manager =
        system.GetTimeManager().GetTimeZoneContentManager().GetTimeZoneManager();
    Time::TimeZone::CalendarInfo calendar_info{};
    NFP::AmiiboDate amiibo_date{};

    amiibo_date.SetYear(2000);
    amiibo_date.SetMonth(1);
    amiibo_date.SetDay(1);

    if (time_zone_manager.ToCalendarTime({}, posix_time, calendar_info) == ResultSuccess) {
        amiibo_date.SetYear(calendar_info.time.year);
        amiibo_date.SetMonth(calendar_info.time.month);
        amiibo_date.SetDay(calendar_info.time.day);
    }

    return amiibo_date;
}

u64 NfcDevice::GetCurrentPosixTime() const {
    auto& standard_steady_clock{system.GetTimeManager().GetStandardSteadyClockCore()};
    return standard_steady_clock.GetCurrentTimePoint(system).time_point;
}

u64 NfcDevice::RemoveVersionByte(u64 application_id) const {
    return application_id & ~(0xfULL << NFP::application_id_version_offset);
}

void NfcDevice::UpdateSettingsCrc() {
    auto& settings = tag_data.settings;

    if (settings.crc_counter != NFP::counter_limit) {
        settings.crc_counter++;
    }

    // TODO: this reads data from a global, find what it is
    std::array<u8, 8> unknown_input{};
    boost::crc_32_type crc;
    crc.process_bytes(&unknown_input, sizeof(unknown_input));
    settings.crc = crc.checksum();
}

void NfcDevice::UpdateRegisterInfoCrc() {
#pragma pack(push, 1)
    struct CrcData {
        Mii::Ver3StoreData mii;
        u8 application_id_byte;
        u8 unknown;
        Mii::NfpStoreDataExtension mii_extension;
        std::array<u32, 0x5> unknown2;
    };
    static_assert(sizeof(CrcData) == 0x7e, "CrcData is an invalid size");
#pragma pack(pop)

    const CrcData crc_data{
        .mii = tag_data.owner_mii,
        .application_id_byte = tag_data.application_id_byte,
        .unknown = tag_data.unknown,
        .mii_extension = tag_data.mii_extension,
        .unknown2 = tag_data.unknown2,
    };

    boost::crc_32_type crc;
    crc.process_bytes(&crc_data, sizeof(CrcData));
    tag_data.register_info_crc = crc.checksum();
}

u64 NfcDevice::GetHandle() const {
    // Generate a handle based of the npad id
    return static_cast<u64>(npad_id);
}

DeviceState NfcDevice::GetCurrentState() const {
    return device_state;
}

Result NfcDevice::GetNpadId(Core::HID::NpadIdType& out_npad_id) const {
    out_npad_id = npad_id;
    return ResultSuccess;
}

} // namespace Service::NFC
