// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <thread>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/assert.h"
#include "common/string_util.h"
#include "core/hle/service/nfc/common/device.h"
#include "core/hle/service/nfp/nfp_result.h"
#include "input_common/drivers/virtual_amiibo.h"
#include "input_common/main.h"
#include "ui_qt_amiibo_settings.h"
#ifdef ENABLE_WEB_SERVICE
#include "web_service/web_backend.h"
#endif
#include "yuzu/applets/qt_amiibo_settings.h"
#include "yuzu/main.h"

QtAmiiboSettingsDialog::QtAmiiboSettingsDialog(QWidget* parent,
                                               Core::Frontend::CabinetParameters parameters_,
                                               InputCommon::InputSubsystem* input_subsystem_,
                                               std::shared_ptr<Service::NFC::NfcDevice> nfp_device_)
    : QDialog(parent), ui(std::make_unique<Ui::QtAmiiboSettingsDialog>()),
      input_subsystem{input_subsystem_}, nfp_device{std::move(nfp_device_)},
      parameters(std::move(parameters_)) {
    ui->setupUi(this);

    LoadInfo();

    resize(0, 0);
}

QtAmiiboSettingsDialog::~QtAmiiboSettingsDialog() = default;

int QtAmiiboSettingsDialog::exec() {
    if (!is_initalized) {
        return QDialog::Rejected;
    }
    return QDialog::exec();
}

std::string QtAmiiboSettingsDialog::GetName() const {
    return ui->amiiboCustomNameValue->text().toStdString();
}

void QtAmiiboSettingsDialog::LoadInfo() {
    if (input_subsystem->GetVirtualAmiibo()->ReloadAmiibo() !=
        InputCommon::VirtualAmiibo::Info::Success) {
        return;
    }

    if (nfp_device->GetCurrentState() != Service::NFC::DeviceState::TagFound &&
        nfp_device->GetCurrentState() != Service::NFC::DeviceState::TagMounted) {
        return;
    }
    nfp_device->Mount(Service::NFP::ModelType::Amiibo, Service::NFP::MountTarget::All);

    LoadAmiiboInfo();
    LoadAmiiboData();
    LoadAmiiboGameInfo();

    ui->amiiboDirectoryValue->setText(
        QString::fromStdString(input_subsystem->GetVirtualAmiibo()->GetLastFilePath()));

    SetSettingsDescription();
    is_initalized = true;
}

void QtAmiiboSettingsDialog::LoadAmiiboInfo() {
    Service::NFP::ModelInfo model_info{};
    const auto model_result = nfp_device->GetModelInfo(model_info);

    if (model_result.IsFailure()) {
        ui->amiiboImageLabel->setVisible(false);
        ui->amiiboInfoGroup->setVisible(false);
        return;
    }

    const auto amiibo_id =
        fmt::format("{:04x}{:02x}{:02x}{:04x}{:02x}02", Common::swap16(model_info.character_id),
                    model_info.character_variant, model_info.amiibo_type, model_info.model_number,
                    model_info.series);

    LOG_DEBUG(Frontend, "Loading amiibo id {}", amiibo_id);
    // Note: This function is not being used until we host the images on our server
    // LoadAmiiboApiInfo(amiibo_id);
    ui->amiiboImageLabel->setVisible(false);
    ui->amiiboInfoGroup->setVisible(false);
}

void QtAmiiboSettingsDialog::LoadAmiiboApiInfo(std::string_view amiibo_id) {
#ifdef ENABLE_WEB_SERVICE
    // TODO: Host this data on our website
    WebService::Client client{"https://amiiboapi.com", {}, {}};
    WebService::Client image_client{"https://raw.githubusercontent.com", {}, {}};
    const auto url_path = fmt::format("/api/amiibo/?id={}", amiibo_id);

    const auto amiibo_json = client.GetJson(url_path, true).returned_data;
    if (amiibo_json.empty()) {
        ui->amiiboImageLabel->setVisible(false);
        ui->amiiboInfoGroup->setVisible(false);
        return;
    }

    std::string amiibo_series{};
    std::string amiibo_name{};
    std::string amiibo_image_url{};
    std::string amiibo_type{};

    const auto parsed_amiibo_json_json = nlohmann::json::parse(amiibo_json).at("amiibo");
    parsed_amiibo_json_json.at("amiiboSeries").get_to(amiibo_series);
    parsed_amiibo_json_json.at("name").get_to(amiibo_name);
    parsed_amiibo_json_json.at("image").get_to(amiibo_image_url);
    parsed_amiibo_json_json.at("type").get_to(amiibo_type);

    ui->amiiboSeriesValue->setText(QString::fromStdString(amiibo_series));
    ui->amiiboNameValue->setText(QString::fromStdString(amiibo_name));
    ui->amiiboTypeValue->setText(QString::fromStdString(amiibo_type));

    if (amiibo_image_url.size() < 34) {
        ui->amiiboImageLabel->setVisible(false);
    }

    const auto image_url_path = amiibo_image_url.substr(34, amiibo_image_url.size() - 34);
    const auto image_data = image_client.GetImage(image_url_path, true).returned_data;

    if (image_data.empty()) {
        ui->amiiboImageLabel->setVisible(false);
    }

    QPixmap pixmap;
    pixmap.loadFromData(reinterpret_cast<const u8*>(image_data.data()),
                        static_cast<uint>(image_data.size()));
    pixmap = pixmap.scaled(250, 350, Qt::AspectRatioMode::KeepAspectRatio,
                           Qt::TransformationMode::SmoothTransformation);
    ui->amiiboImageLabel->setPixmap(pixmap);
#endif
}

void QtAmiiboSettingsDialog::LoadAmiiboData() {
    Service::NFP::RegisterInfo register_info{};
    Service::NFP::CommonInfo common_info{};
    const auto register_result = nfp_device->GetRegisterInfo(register_info);
    const auto common_result = nfp_device->GetCommonInfo(common_info);

    if (register_result.IsFailure()) {
        ui->creationDateValue->setDisabled(true);
        ui->modificationDateValue->setDisabled(true);
        ui->amiiboCustomNameValue->setReadOnly(false);
        ui->amiiboOwnerValue->setReadOnly(false);
        return;
    }

    if (parameters.mode == Service::NFP::CabinetMode::StartNicknameAndOwnerSettings) {
        ui->creationDateValue->setDisabled(true);
        ui->modificationDateValue->setDisabled(true);
    }

    const auto amiibo_name = std::string(register_info.amiibo_name.data());
    const auto owner_name = Common::UTF16ToUTF8(register_info.mii_char_info.name.data());
    const auto creation_date =
        QDate(register_info.creation_date.year, register_info.creation_date.month,
              register_info.creation_date.day);

    ui->amiiboCustomNameValue->setText(QString::fromStdString(amiibo_name));
    ui->amiiboOwnerValue->setText(QString::fromStdString(owner_name));
    ui->amiiboCustomNameValue->setReadOnly(true);
    ui->amiiboOwnerValue->setReadOnly(true);
    ui->creationDateValue->setDate(creation_date);

    if (common_result.IsFailure()) {
        ui->modificationDateValue->setDisabled(true);
        return;
    }

    const auto modification_date =
        QDate(common_info.last_write_date.year, common_info.last_write_date.month,
              common_info.last_write_date.day);
    ui->modificationDateValue->setDate(modification_date);
}

void QtAmiiboSettingsDialog::LoadAmiiboGameInfo() {
    u32 application_area_id{};
    const auto application_result = nfp_device->GetApplicationAreaId(application_area_id);

    if (application_result.IsFailure()) {
        ui->gameIdValue->setVisible(false);
        ui->gameIdLabel->setText(tr("No game data present"));
        return;
    }

    SetGameDataName(application_area_id);
}

void QtAmiiboSettingsDialog::SetGameDataName(u32 application_area_id) {
    static constexpr std::array<std::pair<u32, const char*>, 12> game_name_list = {
        // 3ds, wii u
        std::pair<u32, const char*>{0x10110E00, "Super Smash Bros (3DS/WiiU)"},
        {0x00132600, "Mario & Luigi: Paper Jam"},
        {0x0014F000, "Animal Crossing: Happy Home Designer"},
        {0x00152600, "Chibi-Robo!: Zip Lash"},
        {0x10161f00, "Mario Party 10"},
        {0x1019C800, "The Legend of Zelda: Twilight Princess HD"},
        // switch
        {0x10162B00, "Splatoon 2"},
        {0x1016e100, "Shovel Knight: Treasure Trove"},
        {0x1019C800, "The Legend of Zelda: Breath of the Wild"},
        {0x34F80200, "Super Smash Bros. Ultimate"},
        {0x38600500, "Splatoon 3"},
        {0x3B440400, "The Legend of Zelda: Link's Awakening"},
    };

    for (const auto& [game_id, game_name] : game_name_list) {
        if (application_area_id == game_id) {
            ui->gameIdValue->setText(QString::fromStdString(game_name));
            return;
        }
    }

    const auto application_area_string = fmt::format("{:016x}", application_area_id);
    ui->gameIdValue->setText(QString::fromStdString(application_area_string));
}

void QtAmiiboSettingsDialog::SetSettingsDescription() {
    switch (parameters.mode) {
    case Service::NFP::CabinetMode::StartFormatter:
        ui->cabinetActionDescriptionLabel->setText(
            tr("The following amiibo data will be formatted:"));
        break;
    case Service::NFP::CabinetMode::StartGameDataEraser:
        ui->cabinetActionDescriptionLabel->setText(tr("The following game data will removed:"));
        break;
    case Service::NFP::CabinetMode::StartNicknameAndOwnerSettings:
        ui->cabinetActionDescriptionLabel->setText(tr("Set nickname and owner:"));
        break;
    case Service::NFP::CabinetMode::StartRestorer:
        ui->cabinetActionDescriptionLabel->setText(tr("Do you wish to restore this amiibo?"));
        break;
    }
}

QtAmiiboSettings::QtAmiiboSettings(GMainWindow& parent) {
    connect(this, &QtAmiiboSettings::MainWindowShowAmiiboSettings, &parent,
            &GMainWindow::AmiiboSettingsShowDialog, Qt::QueuedConnection);
    connect(this, &QtAmiiboSettings::MainWindowRequestExit, &parent,
            &GMainWindow::AmiiboSettingsRequestExit, Qt::QueuedConnection);
    connect(&parent, &GMainWindow::AmiiboSettingsFinished, this,
            &QtAmiiboSettings::MainWindowFinished, Qt::QueuedConnection);
}

QtAmiiboSettings::~QtAmiiboSettings() = default;

void QtAmiiboSettings::Close() const {
    callback = {};
    emit MainWindowRequestExit();
}

void QtAmiiboSettings::ShowCabinetApplet(
    const Core::Frontend::CabinetCallback& callback_,
    const Core::Frontend::CabinetParameters& parameters,
    std::shared_ptr<Service::NFC::NfcDevice> nfp_device) const {
    callback = std::move(callback_);
    emit MainWindowShowAmiiboSettings(parameters, nfp_device);
}

void QtAmiiboSettings::MainWindowFinished(bool is_success, const std::string& name) {
    if (callback) {
        callback(is_success, name);
    }
}
