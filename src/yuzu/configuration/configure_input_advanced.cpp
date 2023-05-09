// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QColorDialog>
#include "common/settings.h"
#include "core/core.h"
#include "ui_configure_input_advanced.h"
#include "yuzu/configuration/configure_input_advanced.h"

ConfigureInputAdvanced::ConfigureInputAdvanced(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureInputAdvanced>()) {
    ui->setupUi(this);

    controllers_color_buttons = {{
        {
            ui->player1_left_body_button,
            ui->player1_left_buttons_button,
            ui->player1_right_body_button,
            ui->player1_right_buttons_button,
        },
        {
            ui->player2_left_body_button,
            ui->player2_left_buttons_button,
            ui->player2_right_body_button,
            ui->player2_right_buttons_button,
        },
        {
            ui->player3_left_body_button,
            ui->player3_left_buttons_button,
            ui->player3_right_body_button,
            ui->player3_right_buttons_button,
        },
        {
            ui->player4_left_body_button,
            ui->player4_left_buttons_button,
            ui->player4_right_body_button,
            ui->player4_right_buttons_button,
        },
        {
            ui->player5_left_body_button,
            ui->player5_left_buttons_button,
            ui->player5_right_body_button,
            ui->player5_right_buttons_button,
        },
        {
            ui->player6_left_body_button,
            ui->player6_left_buttons_button,
            ui->player6_right_body_button,
            ui->player6_right_buttons_button,
        },
        {
            ui->player7_left_body_button,
            ui->player7_left_buttons_button,
            ui->player7_right_body_button,
            ui->player7_right_buttons_button,
        },
        {
            ui->player8_left_body_button,
            ui->player8_left_buttons_button,
            ui->player8_right_body_button,
            ui->player8_right_buttons_button,
        },
    }};

    for (std::size_t player_idx = 0; player_idx < controllers_color_buttons.size(); ++player_idx) {
        auto& color_buttons = controllers_color_buttons[player_idx];
        for (std::size_t button_idx = 0; button_idx < color_buttons.size(); ++button_idx) {
            connect(color_buttons[button_idx], &QPushButton::clicked, this,
                    [this, player_idx, button_idx] {
                        OnControllerButtonClick(player_idx, button_idx);
                    });
        }
    }

    connect(ui->mouse_enabled, &QCheckBox::stateChanged, this,
            &ConfigureInputAdvanced::UpdateUIEnabled);
    connect(ui->debug_enabled, &QCheckBox::stateChanged, this,
            &ConfigureInputAdvanced::UpdateUIEnabled);
    connect(ui->touchscreen_enabled, &QCheckBox::stateChanged, this,
            &ConfigureInputAdvanced::UpdateUIEnabled);
    connect(ui->enable_ring_controller, &QCheckBox::stateChanged, this,
            &ConfigureInputAdvanced::UpdateUIEnabled);

    connect(ui->debug_configure, &QPushButton::clicked, this,
            [this] { CallDebugControllerDialog(); });
    connect(ui->touchscreen_advanced, &QPushButton::clicked, this,
            [this] { CallTouchscreenConfigDialog(); });
    connect(ui->buttonMotionTouch, &QPushButton::clicked, this,
            [this] { CallMotionTouchConfigDialog(); });
    connect(ui->ring_controller_configure, &QPushButton::clicked, this,
            [this] { CallRingControllerDialog(); });
    connect(ui->camera_configure, &QPushButton::clicked, this, [this] { CallCameraDialog(); });

#ifndef _WIN32
    ui->enable_raw_input->setVisible(false);
#endif

    LoadConfiguration();
}

ConfigureInputAdvanced::~ConfigureInputAdvanced() = default;

void ConfigureInputAdvanced::OnControllerButtonClick(std::size_t player_idx,
                                                     std::size_t button_idx) {
    const QColor new_bg_color = QColorDialog::getColor(controllers_colors[player_idx][button_idx]);
    if (!new_bg_color.isValid()) {
        return;
    }
    controllers_colors[player_idx][button_idx] = new_bg_color;
    controllers_color_buttons[player_idx][button_idx]->setStyleSheet(
        QStringLiteral("background-color: %1; min-width: 60px;")
            .arg(controllers_colors[player_idx][button_idx].name()));
}

void ConfigureInputAdvanced::ApplyConfiguration() {
    for (std::size_t player_idx = 0; player_idx < controllers_color_buttons.size(); ++player_idx) {
        auto& player = Settings::values.players.GetValue()[player_idx];
        std::array<u32, 4> colors{};
        std::transform(controllers_colors[player_idx].begin(), controllers_colors[player_idx].end(),
                       colors.begin(), [](QColor color) { return color.rgb(); });

        player.body_color_left = colors[0];
        player.button_color_left = colors[1];
        player.body_color_right = colors[2];
        player.button_color_right = colors[3];
    }

    Settings::values.debug_pad_enabled = ui->debug_enabled->isChecked();
    Settings::values.mouse_enabled = ui->mouse_enabled->isChecked();
    Settings::values.keyboard_enabled = ui->keyboard_enabled->isChecked();
    Settings::values.emulate_analog_keyboard = ui->emulate_analog_keyboard->isChecked();
    Settings::values.mouse_panning = ui->mouse_panning->isChecked();
    Settings::values.mouse_panning_sensitivity =
        static_cast<float>(ui->mouse_panning_sensitivity->value());
    Settings::values.touchscreen.enabled = ui->touchscreen_enabled->isChecked();
    Settings::values.enable_raw_input = ui->enable_raw_input->isChecked();
    Settings::values.enable_udp_controller = ui->enable_udp_controller->isChecked();
    Settings::values.controller_navigation = ui->controller_navigation->isChecked();
    Settings::values.enable_ring_controller = ui->enable_ring_controller->isChecked();
    Settings::values.enable_ir_sensor = ui->enable_ir_sensor->isChecked();
    Settings::values.enable_joycon_driver = ui->enable_joycon_driver->isChecked();
    Settings::values.enable_procon_driver = ui->enable_procon_driver->isChecked();
    Settings::values.random_amiibo_id = ui->random_amiibo_id->isChecked();
}

void ConfigureInputAdvanced::LoadConfiguration() {
    for (std::size_t player_idx = 0; player_idx < controllers_color_buttons.size(); ++player_idx) {
        auto& player = Settings::values.players.GetValue()[player_idx];
        std::array<u32, 4> colors = {
            player.body_color_left,
            player.button_color_left,
            player.body_color_right,
            player.button_color_right,
        };

        std::transform(colors.begin(), colors.end(), controllers_colors[player_idx].begin(),
                       [](u32 rgb) { return QColor::fromRgb(rgb); });

        for (std::size_t button_idx = 0; button_idx < colors.size(); ++button_idx) {
            controllers_color_buttons[player_idx][button_idx]->setStyleSheet(
                QStringLiteral("background-color: %1; min-width: 60px;")
                    .arg(controllers_colors[player_idx][button_idx].name()));
        }
    }

    ui->debug_enabled->setChecked(Settings::values.debug_pad_enabled.GetValue());
    ui->mouse_enabled->setChecked(Settings::values.mouse_enabled.GetValue());
    ui->keyboard_enabled->setChecked(Settings::values.keyboard_enabled.GetValue());
    ui->emulate_analog_keyboard->setChecked(Settings::values.emulate_analog_keyboard.GetValue());
    ui->mouse_panning->setChecked(Settings::values.mouse_panning.GetValue());
    ui->mouse_panning_sensitivity->setValue(Settings::values.mouse_panning_sensitivity.GetValue());
    ui->touchscreen_enabled->setChecked(Settings::values.touchscreen.enabled);
    ui->enable_raw_input->setChecked(Settings::values.enable_raw_input.GetValue());
    ui->enable_udp_controller->setChecked(Settings::values.enable_udp_controller.GetValue());
    ui->controller_navigation->setChecked(Settings::values.controller_navigation.GetValue());
    ui->enable_ring_controller->setChecked(Settings::values.enable_ring_controller.GetValue());
    ui->enable_ir_sensor->setChecked(Settings::values.enable_ir_sensor.GetValue());
    ui->enable_joycon_driver->setChecked(Settings::values.enable_joycon_driver.GetValue());
    ui->enable_procon_driver->setChecked(Settings::values.enable_procon_driver.GetValue());
    ui->random_amiibo_id->setChecked(Settings::values.random_amiibo_id.GetValue());

    UpdateUIEnabled();
}

void ConfigureInputAdvanced::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureInputAdvanced::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureInputAdvanced::UpdateUIEnabled() {
    ui->debug_configure->setEnabled(ui->debug_enabled->isChecked());
    ui->touchscreen_advanced->setEnabled(ui->touchscreen_enabled->isChecked());
    ui->mouse_panning->setEnabled(!ui->mouse_enabled->isChecked());
    ui->mouse_panning_sensitivity->setEnabled(!ui->mouse_enabled->isChecked());
    ui->ring_controller_configure->setEnabled(ui->enable_ring_controller->isChecked());
#if QT_VERSION > QT_VERSION_CHECK(6, 0, 0) || !defined(YUZU_USE_QT_MULTIMEDIA)
    ui->enable_ir_sensor->setEnabled(false);
    ui->camera_configure->setEnabled(false);
#endif
}
