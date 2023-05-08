// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include <fmt/format.h>

#include <QAbstractButton>
#include <QCheckBox>
#include <QPushButton>
#include <QString>
#include <QTimer>

#include "common/fs/fs_util.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/xts_archive.h"
#include "core/loader/loader.h"
#include "ui_configure_per_game.h"
#include "yuzu/configuration/config.h"
#include "yuzu/configuration/configure_audio.h"
#include "yuzu/configuration/configure_cpu.h"
#include "yuzu/configuration/configure_general.h"
#include "yuzu/configuration/configure_graphics.h"
#include "yuzu/configuration/configure_graphics_advanced.h"
#include "yuzu/configuration/configure_input_per_game.h"
#include "yuzu/configuration/configure_per_game.h"
#include "yuzu/configuration/configure_per_game_addons.h"
#include "yuzu/configuration/configure_system.h"
#include "yuzu/uisettings.h"
#include "yuzu/util/util.h"

ConfigurePerGame::ConfigurePerGame(QWidget* parent, u64 title_id_, const std::string& file_name,
                                   Core::System& system_)
    : QDialog(parent),
      ui(std::make_unique<Ui::ConfigurePerGame>()), title_id{title_id_}, system{system_} {
    const auto file_path = std::filesystem::path(Common::FS::ToU8String(file_name));
    const auto config_file_name = title_id == 0 ? Common::FS::PathToUTF8String(file_path.filename())
                                                : fmt::format("{:016X}", title_id);
    game_config = std::make_unique<Config>(config_file_name, Config::ConfigType::PerGameConfig);

    addons_tab = std::make_unique<ConfigurePerGameAddons>(system_, this);
    audio_tab = std::make_unique<ConfigureAudio>(system_, this);
    cpu_tab = std::make_unique<ConfigureCpu>(system_, this);
    general_tab = std::make_unique<ConfigureGeneral>(system_, this);
    graphics_advanced_tab = std::make_unique<ConfigureGraphicsAdvanced>(system_, this);
    graphics_tab = std::make_unique<ConfigureGraphics>(
        system_, [&]() { graphics_advanced_tab->ExposeComputeOption(); }, this);
    input_tab = std::make_unique<ConfigureInputPerGame>(system_, game_config.get(), this);
    system_tab = std::make_unique<ConfigureSystem>(system_, this);

    ui->setupUi(this);

    ui->tabWidget->addTab(addons_tab.get(), tr("Add-Ons"));
    ui->tabWidget->addTab(general_tab.get(), tr("General"));
    ui->tabWidget->addTab(system_tab.get(), tr("System"));
    ui->tabWidget->addTab(cpu_tab.get(), tr("CPU"));
    ui->tabWidget->addTab(graphics_tab.get(), tr("Graphics"));
    ui->tabWidget->addTab(graphics_advanced_tab.get(), tr("Adv. Graphics"));
    ui->tabWidget->addTab(audio_tab.get(), tr("Audio"));
    ui->tabWidget->addTab(input_tab.get(), tr("Input Profiles"));

    setFocusPolicy(Qt::ClickFocus);
    setWindowTitle(tr("Properties"));

    addons_tab->SetTitleId(title_id);

    scene = new QGraphicsScene;
    ui->icon_view->setScene(scene);

    if (system.IsPoweredOn()) {
        QPushButton* apply_button = ui->buttonBox->addButton(QDialogButtonBox::Apply);
        connect(apply_button, &QAbstractButton::clicked, this,
                &ConfigurePerGame::HandleApplyButtonClicked);
    }

    LoadConfiguration();
}

ConfigurePerGame::~ConfigurePerGame() = default;

void ConfigurePerGame::ApplyConfiguration() {
    addons_tab->ApplyConfiguration();
    general_tab->ApplyConfiguration();
    cpu_tab->ApplyConfiguration();
    system_tab->ApplyConfiguration();
    graphics_tab->ApplyConfiguration();
    graphics_advanced_tab->ApplyConfiguration();
    audio_tab->ApplyConfiguration();
    input_tab->ApplyConfiguration();

    system.ApplySettings();
    Settings::LogSettings();

    game_config->Save();
}

void ConfigurePerGame::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QDialog::changeEvent(event);
}

void ConfigurePerGame::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigurePerGame::HandleApplyButtonClicked() {
    UISettings::values.configuration_applied = true;
    ApplyConfiguration();
}

void ConfigurePerGame::LoadFromFile(FileSys::VirtualFile file_) {
    file = std::move(file_);
    LoadConfiguration();
}

void ConfigurePerGame::LoadConfiguration() {
    if (file == nullptr) {
        return;
    }

    addons_tab->LoadFromFile(file);

    ui->display_title_id->setText(
        QStringLiteral("%1").arg(title_id, 16, 16, QLatin1Char{'0'}).toUpper());

    const FileSys::PatchManager pm{title_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto control = pm.GetControlMetadata();
    const auto loader = Loader::GetLoader(system, file);

    if (control.first != nullptr) {
        ui->display_version->setText(QString::fromStdString(control.first->GetVersionString()));
        ui->display_name->setText(QString::fromStdString(control.first->GetApplicationName()));
        ui->display_developer->setText(QString::fromStdString(control.first->GetDeveloperName()));
    } else {
        std::string title;
        if (loader->ReadTitle(title) == Loader::ResultStatus::Success)
            ui->display_name->setText(QString::fromStdString(title));

        FileSys::NACP nacp;
        if (loader->ReadControlData(nacp) == Loader::ResultStatus::Success)
            ui->display_developer->setText(QString::fromStdString(nacp.GetDeveloperName()));

        ui->display_version->setText(QStringLiteral("1.0.0"));
    }

    if (control.second != nullptr) {
        scene->clear();

        QPixmap map;
        const auto bytes = control.second->ReadAllBytes();
        map.loadFromData(bytes.data(), static_cast<u32>(bytes.size()));

        scene->addPixmap(map.scaled(ui->icon_view->width(), ui->icon_view->height(),
                                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    } else {
        std::vector<u8> bytes;
        if (loader->ReadIcon(bytes) == Loader::ResultStatus::Success) {
            scene->clear();

            QPixmap map;
            map.loadFromData(bytes.data(), static_cast<u32>(bytes.size()));

            scene->addPixmap(map.scaled(ui->icon_view->width(), ui->icon_view->height(),
                                        Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        }
    }

    ui->display_filename->setText(QString::fromStdString(file->GetName()));

    ui->display_format->setText(
        QString::fromStdString(Loader::GetFileTypeString(loader->GetFileType())));

    const auto valueText = ReadableByteSize(file->GetSize());
    ui->display_size->setText(valueText);
}
