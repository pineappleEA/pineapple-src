// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <QDialog>
#include "core/frontend/applets/cabinet.h"

class GMainWindow;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QGroupBox;
class QLabel;

namespace InputCommon {
class InputSubsystem;
}

namespace Ui {
class QtAmiiboSettingsDialog;
}

namespace Service::NFC {
class NfcDevice;
} // namespace Service::NFC

class QtAmiiboSettingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit QtAmiiboSettingsDialog(QWidget* parent, Core::Frontend::CabinetParameters parameters_,
                                    InputCommon::InputSubsystem* input_subsystem_,
                                    std::shared_ptr<Service::NFC::NfcDevice> nfp_device_);
    ~QtAmiiboSettingsDialog() override;

    int exec() override;

    std::string GetName() const;

private:
    void LoadInfo();
    void LoadAmiiboInfo();
    void LoadAmiiboApiInfo(std::string_view amiibo_id);
    void LoadAmiiboData();
    void LoadAmiiboGameInfo();
    void SetGameDataName(u32 application_area_id);
    void SetSettingsDescription();

    std::unique_ptr<Ui::QtAmiiboSettingsDialog> ui;

    InputCommon::InputSubsystem* input_subsystem;
    std::shared_ptr<Service::NFC::NfcDevice> nfp_device;

    // Parameters sent in from the backend HLE applet.
    Core::Frontend::CabinetParameters parameters;

    // If false amiibo settings failed to load
    bool is_initalized{};
};

class QtAmiiboSettings final : public QObject, public Core::Frontend::CabinetApplet {
    Q_OBJECT

public:
    explicit QtAmiiboSettings(GMainWindow& parent);
    ~QtAmiiboSettings() override;

    void Close() const override;
    void ShowCabinetApplet(const Core::Frontend::CabinetCallback& callback_,
                           const Core::Frontend::CabinetParameters& parameters,
                           std::shared_ptr<Service::NFC::NfcDevice> nfp_device) const override;

signals:
    void MainWindowShowAmiiboSettings(const Core::Frontend::CabinetParameters& parameters,
                                      std::shared_ptr<Service::NFC::NfcDevice> nfp_device) const;
    void MainWindowRequestExit() const;

private:
    void MainWindowFinished(bool is_success, const std::string& name);

    mutable Core::Frontend::CabinetCallback callback;
};
