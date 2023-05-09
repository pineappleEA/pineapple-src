// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <utility>
#include <QGridLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QTimer>
#include "common/assert.h"
#include "common/param_package.h"
#include "core/hid/emulated_controller.h"
#include "core/hid/hid_core.h"
#include "core/hid/hid_types.h"
#include "input_common/drivers/keyboard.h"
#include "input_common/drivers/mouse.h"
#include "input_common/main.h"
#include "ui_configure_input_player.h"
#include "yuzu/bootmanager.h"
#include "yuzu/configuration/config.h"
#include "yuzu/configuration/configure_input_player.h"
#include "yuzu/configuration/configure_input_player_widget.h"
#include "yuzu/configuration/input_profiles.h"
#include "yuzu/util/limitable_input_dialog.h"

const std::array<std::string, ConfigureInputPlayer::ANALOG_SUB_BUTTONS_NUM>
    ConfigureInputPlayer::analog_sub_buttons{{
        "up",
        "down",
        "left",
        "right",
    }};

namespace {

QString GetKeyName(int key_code) {
    switch (key_code) {
    case Qt::Key_Shift:
        return QObject::tr("Shift");
    case Qt::Key_Control:
        return QObject::tr("Ctrl");
    case Qt::Key_Alt:
        return QObject::tr("Alt");
    case Qt::Key_Meta:
        return {};
    default:
        return QKeySequence(key_code).toString();
    }
}

QString GetButtonName(Common::Input::ButtonNames button_name) {
    switch (button_name) {
    case Common::Input::ButtonNames::ButtonLeft:
        return QObject::tr("Left");
    case Common::Input::ButtonNames::ButtonRight:
        return QObject::tr("Right");
    case Common::Input::ButtonNames::ButtonDown:
        return QObject::tr("Down");
    case Common::Input::ButtonNames::ButtonUp:
        return QObject::tr("Up");
    case Common::Input::ButtonNames::TriggerZ:
        return QObject::tr("Z");
    case Common::Input::ButtonNames::TriggerR:
        return QObject::tr("R");
    case Common::Input::ButtonNames::TriggerL:
        return QObject::tr("L");
    case Common::Input::ButtonNames::TriggerZR:
        return QObject::tr("ZR");
    case Common::Input::ButtonNames::TriggerZL:
        return QObject::tr("ZL");
    case Common::Input::ButtonNames::TriggerSR:
        return QObject::tr("SR");
    case Common::Input::ButtonNames::TriggerSL:
        return QObject::tr("SL");
    case Common::Input::ButtonNames::ButtonStickL:
        return QObject::tr("Stick L");
    case Common::Input::ButtonNames::ButtonStickR:
        return QObject::tr("Stick R");
    case Common::Input::ButtonNames::ButtonA:
        return QObject::tr("A");
    case Common::Input::ButtonNames::ButtonB:
        return QObject::tr("B");
    case Common::Input::ButtonNames::ButtonX:
        return QObject::tr("X");
    case Common::Input::ButtonNames::ButtonY:
        return QObject::tr("Y");
    case Common::Input::ButtonNames::ButtonStart:
        return QObject::tr("Start");
    case Common::Input::ButtonNames::ButtonPlus:
        return QObject::tr("Plus");
    case Common::Input::ButtonNames::ButtonMinus:
        return QObject::tr("Minus");
    case Common::Input::ButtonNames::ButtonHome:
        return QObject::tr("Home");
    case Common::Input::ButtonNames::ButtonCapture:
        return QObject::tr("Capture");
    case Common::Input::ButtonNames::L1:
        return QObject::tr("L1");
    case Common::Input::ButtonNames::L2:
        return QObject::tr("L2");
    case Common::Input::ButtonNames::L3:
        return QObject::tr("L3");
    case Common::Input::ButtonNames::R1:
        return QObject::tr("R1");
    case Common::Input::ButtonNames::R2:
        return QObject::tr("R2");
    case Common::Input::ButtonNames::R3:
        return QObject::tr("R3");
    case Common::Input::ButtonNames::Circle:
        return QObject::tr("Circle");
    case Common::Input::ButtonNames::Cross:
        return QObject::tr("Cross");
    case Common::Input::ButtonNames::Square:
        return QObject::tr("Square");
    case Common::Input::ButtonNames::Triangle:
        return QObject::tr("Triangle");
    case Common::Input::ButtonNames::Share:
        return QObject::tr("Share");
    case Common::Input::ButtonNames::Options:
        return QObject::tr("Options");
    case Common::Input::ButtonNames::Home:
        return QObject::tr("Home");
    case Common::Input::ButtonNames::Touch:
        return QObject::tr("Touch");
    case Common::Input::ButtonNames::ButtonMouseWheel:
        return QObject::tr("Wheel", "Indicates the mouse wheel");
    case Common::Input::ButtonNames::ButtonBackward:
        return QObject::tr("Backward");
    case Common::Input::ButtonNames::ButtonForward:
        return QObject::tr("Forward");
    case Common::Input::ButtonNames::ButtonTask:
        return QObject::tr("Task");
    case Common::Input::ButtonNames::ButtonExtra:
        return QObject::tr("Extra");
    default:
        return QObject::tr("[undefined]");
    }
}

QString GetDirectionName(const std::string& direction) {
    if (direction == "left") {
        return QObject::tr("Left");
    }
    if (direction == "right") {
        return QObject::tr("Right");
    }
    if (direction == "up") {
        return QObject::tr("Up");
    }
    if (direction == "down") {
        return QObject::tr("Down");
    }
    UNIMPLEMENTED_MSG("Unimplemented direction name={}", direction);
    return QString::fromStdString(direction);
}

void SetAnalogParam(const Common::ParamPackage& input_param, Common::ParamPackage& analog_param,
                    const std::string& button_name) {
    // The poller returned a complete axis, so set all the buttons
    if (input_param.Has("axis_x") && input_param.Has("axis_y")) {
        analog_param = input_param;
        return;
    }
    // Check if the current configuration has either no engine or an axis binding.
    // Clears out the old binding and adds one with analog_from_button.
    if (!analog_param.Has("engine") || analog_param.Has("axis_x") || analog_param.Has("axis_y")) {
        analog_param = {
            {"engine", "analog_from_button"},
        };
    }
    analog_param.Set(button_name, input_param.Serialize());
}
} // namespace

QString ConfigureInputPlayer::ButtonToText(const Common::ParamPackage& param) {
    if (!param.Has("engine")) {
        return QObject::tr("[not set]");
    }

    const QString toggle = QString::fromStdString(param.Get("toggle", false) ? "~" : "");
    const QString inverted = QString::fromStdString(param.Get("inverted", false) ? "!" : "");
    const QString invert = QString::fromStdString(param.Get("invert", "+") == "-" ? "-" : "");
    const QString turbo = QString::fromStdString(param.Get("turbo", false) ? "$" : "");
    const auto common_button_name = input_subsystem->GetButtonName(param);

    // Retrieve the names from Qt
    if (param.Get("engine", "") == "keyboard") {
        const QString button_str = GetKeyName(param.Get("code", 0));
        return QObject::tr("%1%2%3%4").arg(turbo, toggle, inverted, button_str);
    }

    if (common_button_name == Common::Input::ButtonNames::Invalid) {
        return QObject::tr("[invalid]");
    }

    if (common_button_name == Common::Input::ButtonNames::Engine) {
        return QString::fromStdString(param.Get("engine", ""));
    }

    if (common_button_name == Common::Input::ButtonNames::Value) {
        if (param.Has("hat")) {
            const QString hat = GetDirectionName(param.Get("direction", ""));
            return QObject::tr("%1%2%3Hat %4").arg(turbo, toggle, inverted, hat);
        }
        if (param.Has("axis")) {
            const QString axis = QString::fromStdString(param.Get("axis", ""));
            return QObject::tr("%1%2%3Axis %4").arg(toggle, inverted, invert, axis);
        }
        if (param.Has("axis_x") && param.Has("axis_y") && param.Has("axis_z")) {
            const QString axis_x = QString::fromStdString(param.Get("axis_x", ""));
            const QString axis_y = QString::fromStdString(param.Get("axis_y", ""));
            const QString axis_z = QString::fromStdString(param.Get("axis_z", ""));
            return QObject::tr("%1%2Axis %3,%4,%5").arg(toggle, inverted, axis_x, axis_y, axis_z);
        }
        if (param.Has("motion")) {
            const QString motion = QString::fromStdString(param.Get("motion", ""));
            return QObject::tr("%1%2Motion %3").arg(toggle, inverted, motion);
        }
        if (param.Has("button")) {
            const QString button = QString::fromStdString(param.Get("button", ""));
            return QObject::tr("%1%2%3Button %4").arg(turbo, toggle, inverted, button);
        }
    }

    QString button_name = GetButtonName(common_button_name);
    if (param.Has("hat")) {
        return QObject::tr("%1%2%3Hat %4").arg(turbo, toggle, inverted, button_name);
    }
    if (param.Has("axis")) {
        return QObject::tr("%1%2%3Axis %4").arg(toggle, inverted, invert, button_name);
    }
    if (param.Has("motion")) {
        return QObject::tr("%1%2Axis %3").arg(toggle, inverted, button_name);
    }
    if (param.Has("button")) {
        return QObject::tr("%1%2%3Button %4").arg(turbo, toggle, inverted, button_name);
    }

    return QObject::tr("[unknown]");
}

QString ConfigureInputPlayer::AnalogToText(const Common::ParamPackage& param,
                                           const std::string& dir) {
    if (!param.Has("engine")) {
        return QObject::tr("[not set]");
    }

    if (param.Get("engine", "") == "analog_from_button") {
        return ButtonToText(Common::ParamPackage{param.Get(dir, "")});
    }

    if (!param.Has("axis_x") || !param.Has("axis_y")) {
        return QObject::tr("[unknown]");
    }

    const auto engine_str = param.Get("engine", "");
    const QString axis_x_str = QString::fromStdString(param.Get("axis_x", ""));
    const QString axis_y_str = QString::fromStdString(param.Get("axis_y", ""));
    const bool invert_x = param.Get("invert_x", "+") == "-";
    const bool invert_y = param.Get("invert_y", "+") == "-";

    if (dir == "modifier") {
        return QObject::tr("[unused]");
    }

    if (dir == "left") {
        const QString invert_x_str = QString::fromStdString(invert_x ? "+" : "-");
        return QObject::tr("Axis %1%2").arg(axis_x_str, invert_x_str);
    }
    if (dir == "right") {
        const QString invert_x_str = QString::fromStdString(invert_x ? "-" : "+");
        return QObject::tr("Axis %1%2").arg(axis_x_str, invert_x_str);
    }
    if (dir == "up") {
        const QString invert_y_str = QString::fromStdString(invert_y ? "-" : "+");
        return QObject::tr("Axis %1%2").arg(axis_y_str, invert_y_str);
    }
    if (dir == "down") {
        const QString invert_y_str = QString::fromStdString(invert_y ? "+" : "-");
        return QObject::tr("Axis %1%2").arg(axis_y_str, invert_y_str);
    }

    return QObject::tr("[unknown]");
}

ConfigureInputPlayer::ConfigureInputPlayer(QWidget* parent, std::size_t player_index_,
                                           QWidget* bottom_row_,
                                           InputCommon::InputSubsystem* input_subsystem_,
                                           InputProfiles* profiles_, Core::HID::HIDCore& hid_core_,
                                           bool is_powered_on_, bool debug_)
    : QWidget(parent),
      ui(std::make_unique<Ui::ConfigureInputPlayer>()), player_index{player_index_}, debug{debug_},
      is_powered_on{is_powered_on_}, input_subsystem{input_subsystem_}, profiles(profiles_),
      timeout_timer(std::make_unique<QTimer>()),
      poll_timer(std::make_unique<QTimer>()), bottom_row{bottom_row_}, hid_core{hid_core_} {
    if (player_index == 0) {
        auto* emulated_controller_p1 =
            hid_core.GetEmulatedController(Core::HID::NpadIdType::Player1);
        auto* emulated_controller_handheld =
            hid_core.GetEmulatedController(Core::HID::NpadIdType::Handheld);
        emulated_controller_p1->SaveCurrentConfig();
        emulated_controller_p1->EnableConfiguration();
        emulated_controller_handheld->SaveCurrentConfig();
        emulated_controller_handheld->EnableConfiguration();
        if (emulated_controller_handheld->IsConnected(true)) {
            emulated_controller_p1->Disconnect();
            emulated_controller = emulated_controller_handheld;
        } else {
            emulated_controller = emulated_controller_p1;
        }
    } else {
        emulated_controller = hid_core.GetEmulatedControllerByIndex(player_index);
        emulated_controller->SaveCurrentConfig();
        emulated_controller->EnableConfiguration();
    }
    ui->setupUi(this);

    setFocusPolicy(Qt::ClickFocus);

    button_map = {
        ui->buttonA,        ui->buttonB,      ui->buttonX,         ui->buttonY,
        ui->buttonLStick,   ui->buttonRStick, ui->buttonL,         ui->buttonR,
        ui->buttonZL,       ui->buttonZR,     ui->buttonPlus,      ui->buttonMinus,
        ui->buttonDpadLeft, ui->buttonDpadUp, ui->buttonDpadRight, ui->buttonDpadDown,
        ui->buttonSL,       ui->buttonSR,     ui->buttonHome,      ui->buttonScreenshot,
    };

    analog_map_buttons = {{
        {
            ui->buttonLStickUp,
            ui->buttonLStickDown,
            ui->buttonLStickLeft,
            ui->buttonLStickRight,
        },
        {
            ui->buttonRStickUp,
            ui->buttonRStickDown,
            ui->buttonRStickLeft,
            ui->buttonRStickRight,
        },
    }};

    motion_map = {
        ui->buttonMotionLeft,
        ui->buttonMotionRight,
    };

    analog_map_deadzone_label = {ui->labelLStickDeadzone, ui->labelRStickDeadzone};
    analog_map_deadzone_slider = {ui->sliderLStickDeadzone, ui->sliderRStickDeadzone};
    analog_map_modifier_groupbox = {ui->buttonLStickModGroup, ui->buttonRStickModGroup};
    analog_map_modifier_button = {ui->buttonLStickMod, ui->buttonRStickMod};
    analog_map_modifier_label = {ui->labelLStickModifierRange, ui->labelRStickModifierRange};
    analog_map_modifier_slider = {ui->sliderLStickModifierRange, ui->sliderRStickModifierRange};
    analog_map_range_groupbox = {ui->buttonLStickRangeGroup, ui->buttonRStickRangeGroup};
    analog_map_range_spinbox = {ui->spinboxLStickRange, ui->spinboxRStickRange};

    ui->controllerFrame->SetController(emulated_controller);

    for (int button_id = 0; button_id < Settings::NativeButton::NumButtons; ++button_id) {
        auto* const button = button_map[button_id];

        if (button == nullptr) {
            continue;
        }

        connect(button, &QPushButton::clicked, [=, this] {
            HandleClick(
                button, button_id,
                [=, this](const Common::ParamPackage& params) {
                    emulated_controller->SetButtonParam(button_id, params);
                },
                InputCommon::Polling::InputType::Button);
        });

        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QPushButton::customContextMenuRequested,
                [=, this](const QPoint& menu_location) {
                    QMenu context_menu;
                    Common::ParamPackage param = emulated_controller->GetButtonParam(button_id);
                    context_menu.addAction(tr("Clear"), [&] {
                        emulated_controller->SetButtonParam(button_id, {});
                        button_map[button_id]->setText(tr("[not set]"));
                    });
                    if (param.Has("code") || param.Has("button") || param.Has("hat")) {
                        context_menu.addAction(tr("Invert button"), [&] {
                            const bool invert_value = !param.Get("inverted", false);
                            param.Set("inverted", invert_value);
                            button_map[button_id]->setText(ButtonToText(param));
                            emulated_controller->SetButtonParam(button_id, param);
                        });
                        context_menu.addAction(tr("Toggle button"), [&] {
                            const bool toggle_value = !param.Get("toggle", false);
                            param.Set("toggle", toggle_value);
                            button_map[button_id]->setText(ButtonToText(param));
                            emulated_controller->SetButtonParam(button_id, param);
                        });
                        context_menu.addAction(tr("Turbo button"), [&] {
                            const bool turbo_value = !param.Get("turbo", false);
                            param.Set("turbo", turbo_value);
                            button_map[button_id]->setText(ButtonToText(param));
                            emulated_controller->SetButtonParam(button_id, param);
                        });
                    }
                    if (param.Has("axis")) {
                        context_menu.addAction(tr("Invert axis"), [&] {
                            const bool toggle_value = !(param.Get("invert", "+") == "-");
                            param.Set("invert", toggle_value ? "-" : "+");
                            button_map[button_id]->setText(ButtonToText(param));
                            emulated_controller->SetButtonParam(button_id, param);
                        });
                        context_menu.addAction(tr("Invert button"), [&] {
                            const bool invert_value = !param.Get("inverted", false);
                            param.Set("inverted", invert_value);
                            button_map[button_id]->setText(ButtonToText(param));
                            emulated_controller->SetButtonParam(button_id, param);
                        });
                        context_menu.addAction(tr("Set threshold"), [&] {
                            const int button_threshold =
                                static_cast<int>(param.Get("threshold", 0.5f) * 100.0f);
                            const int new_threshold = QInputDialog::getInt(
                                this, tr("Set threshold"), tr("Choose a value between 0% and 100%"),
                                button_threshold, 0, 100);
                            param.Set("threshold", new_threshold / 100.0f);

                            if (button_id == Settings::NativeButton::ZL) {
                                ui->sliderZLThreshold->setValue(new_threshold);
                            }
                            if (button_id == Settings::NativeButton::ZR) {
                                ui->sliderZRThreshold->setValue(new_threshold);
                            }
                            emulated_controller->SetButtonParam(button_id, param);
                        });
                        context_menu.addAction(tr("Toggle axis"), [&] {
                            const bool toggle_value = !param.Get("toggle", false);
                            param.Set("toggle", toggle_value);
                            button_map[button_id]->setText(ButtonToText(param));
                            emulated_controller->SetButtonParam(button_id, param);
                        });
                    }
                    context_menu.exec(button_map[button_id]->mapToGlobal(menu_location));
                });
    }

    for (int motion_id = 0; motion_id < Settings::NativeMotion::NumMotions; ++motion_id) {
        auto* const button = motion_map[motion_id];
        if (button == nullptr) {
            continue;
        }

        connect(button, &QPushButton::clicked, [=, this] {
            HandleClick(
                button, motion_id,
                [=, this](const Common::ParamPackage& params) {
                    emulated_controller->SetMotionParam(motion_id, params);
                },
                InputCommon::Polling::InputType::Motion);
        });

        button->setContextMenuPolicy(Qt::CustomContextMenu);

        connect(button, &QPushButton::customContextMenuRequested,
                [=, this](const QPoint& menu_location) {
                    QMenu context_menu;
                    Common::ParamPackage param = emulated_controller->GetMotionParam(motion_id);
                    context_menu.addAction(tr("Clear"), [&] {
                        emulated_controller->SetMotionParam(motion_id, {});
                        motion_map[motion_id]->setText(tr("[not set]"));
                    });
                    if (param.Has("motion")) {
                        context_menu.addAction(tr("Set gyro threshold"), [&] {
                            const int gyro_threshold =
                                static_cast<int>(param.Get("threshold", 0.007f) * 1000.0f);
                            const int new_threshold = QInputDialog::getInt(
                                this, tr("Set threshold"), tr("Choose a value between 0% and 100%"),
                                gyro_threshold, 0, 100);
                            param.Set("threshold", new_threshold / 1000.0f);
                            emulated_controller->SetMotionParam(motion_id, param);
                        });
                        context_menu.addAction(tr("Calibrate sensor"), [&] {
                            emulated_controller->StartMotionCalibration();
                        });
                    }
                    context_menu.exec(motion_map[motion_id]->mapToGlobal(menu_location));
                });
    }

    connect(ui->sliderZLThreshold, &QSlider::valueChanged, [=, this] {
        Common::ParamPackage param =
            emulated_controller->GetButtonParam(Settings::NativeButton::ZL);
        if (param.Has("threshold")) {
            const auto slider_value = ui->sliderZLThreshold->value();
            param.Set("threshold", slider_value / 100.0f);
            emulated_controller->SetButtonParam(Settings::NativeButton::ZL, param);
        }
    });

    connect(ui->sliderZRThreshold, &QSlider::valueChanged, [=, this] {
        Common::ParamPackage param =
            emulated_controller->GetButtonParam(Settings::NativeButton::ZR);
        if (param.Has("threshold")) {
            const auto slider_value = ui->sliderZRThreshold->value();
            param.Set("threshold", slider_value / 100.0f);
            emulated_controller->SetButtonParam(Settings::NativeButton::ZR, param);
        }
    });

    for (int analog_id = 0; analog_id < Settings::NativeAnalog::NumAnalogs; ++analog_id) {
        for (int sub_button_id = 0; sub_button_id < ANALOG_SUB_BUTTONS_NUM; ++sub_button_id) {
            auto* const analog_button = analog_map_buttons[analog_id][sub_button_id];

            if (analog_button == nullptr) {
                continue;
            }

            connect(analog_button, &QPushButton::clicked, [=, this] {
                if (!map_analog_stick_accepted) {
                    map_analog_stick_accepted =
                        QMessageBox::information(
                            this, tr("Map Analog Stick"),
                            tr("After pressing OK, first move your joystick horizontally, and then "
                               "vertically.\nTo invert the axes, first move your joystick "
                               "vertically, and then horizontally."),
                            QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Ok;
                    if (!map_analog_stick_accepted) {
                        return;
                    }
                }
                HandleClick(
                    analog_map_buttons[analog_id][sub_button_id], analog_id,
                    [=, this](const Common::ParamPackage& params) {
                        Common::ParamPackage param = emulated_controller->GetStickParam(analog_id);
                        SetAnalogParam(params, param, analog_sub_buttons[sub_button_id]);
                        // Correct axis direction for inverted sticks
                        if (input_subsystem->IsStickInverted(param)) {
                            switch (analog_id) {
                            case Settings::NativeAnalog::LStick: {
                                const bool invert_value = param.Get("invert_x", "+") == "-";
                                const std::string invert_str = invert_value ? "+" : "-";
                                param.Set("invert_x", invert_str);
                                break;
                            }
                            case Settings::NativeAnalog::RStick: {
                                const bool invert_value = param.Get("invert_y", "+") == "-";
                                const std::string invert_str = invert_value ? "+" : "-";
                                param.Set("invert_y", invert_str);
                                break;
                            }
                            default:
                                break;
                            }
                        }
                        emulated_controller->SetStickParam(analog_id, param);
                    },
                    InputCommon::Polling::InputType::Stick);
            });

            analog_button->setContextMenuPolicy(Qt::CustomContextMenu);

            connect(analog_button, &QPushButton::customContextMenuRequested,
                    [=, this](const QPoint& menu_location) {
                        QMenu context_menu;
                        Common::ParamPackage param = emulated_controller->GetStickParam(analog_id);
                        context_menu.addAction(tr("Clear"), [&] {
                            if (param.Get("engine", "") != "analog_from_button") {
                                emulated_controller->SetStickParam(analog_id, {});
                                for (auto button : analog_map_buttons[analog_id]) {
                                    button->setText(tr("[not set]"));
                                }
                                return;
                            }
                            switch (sub_button_id) {
                            case 0:
                                param.Erase("up");
                                break;
                            case 1:
                                param.Erase("down");
                                break;
                            case 2:
                                param.Erase("left");
                                break;
                            case 3:
                                param.Erase("right");
                                break;
                            }
                            emulated_controller->SetStickParam(analog_id, param);
                            analog_map_buttons[analog_id][sub_button_id]->setText(tr("[not set]"));
                        });
                        context_menu.addAction(tr("Center axis"), [&] {
                            const auto stick_value =
                                emulated_controller->GetSticksValues()[analog_id];
                            const float offset_x = stick_value.x.properties.offset;
                            const float offset_y = stick_value.y.properties.offset;
                            float raw_value_x = stick_value.x.raw_value;
                            float raw_value_y = stick_value.y.raw_value;
                            // See Core::HID::SanitizeStick() to obtain the original raw axis value
                            if (std::abs(offset_x) < 0.5f) {
                                if (raw_value_x > 0) {
                                    raw_value_x *= 1 + offset_x;
                                } else {
                                    raw_value_x *= 1 - offset_x;
                                }
                            }
                            if (std::abs(offset_x) < 0.5f) {
                                if (raw_value_y > 0) {
                                    raw_value_y *= 1 + offset_y;
                                } else {
                                    raw_value_y *= 1 - offset_y;
                                }
                            }
                            param.Set("offset_x", -raw_value_x + offset_x);
                            param.Set("offset_y", -raw_value_y + offset_y);
                            emulated_controller->SetStickParam(analog_id, param);
                        });
                        context_menu.addAction(tr("Invert axis"), [&] {
                            if (sub_button_id == 2 || sub_button_id == 3) {
                                const bool invert_value = param.Get("invert_x", "+") == "-";
                                const std::string invert_str = invert_value ? "+" : "-";
                                param.Set("invert_x", invert_str);
                                emulated_controller->SetStickParam(analog_id, param);
                            }
                            if (sub_button_id == 0 || sub_button_id == 1) {
                                const bool invert_value = param.Get("invert_y", "+") == "-";
                                const std::string invert_str = invert_value ? "+" : "-";
                                param.Set("invert_y", invert_str);
                                emulated_controller->SetStickParam(analog_id, param);
                            }
                            for (int analog_sub_button_id = 0;
                                 analog_sub_button_id < ANALOG_SUB_BUTTONS_NUM;
                                 ++analog_sub_button_id) {
                                analog_map_buttons[analog_id][analog_sub_button_id]->setText(
                                    AnalogToText(param, analog_sub_buttons[analog_sub_button_id]));
                            }
                        });
                        context_menu.exec(analog_map_buttons[analog_id][sub_button_id]->mapToGlobal(
                            menu_location));
                    });
        }

        // Handle clicks for the modifier buttons as well.
        connect(analog_map_modifier_button[analog_id], &QPushButton::clicked, [=, this] {
            HandleClick(
                analog_map_modifier_button[analog_id], analog_id,
                [=, this](const Common::ParamPackage& params) {
                    Common::ParamPackage param = emulated_controller->GetStickParam(analog_id);
                    param.Set("modifier", params.Serialize());
                    emulated_controller->SetStickParam(analog_id, param);
                },
                InputCommon::Polling::InputType::Button);
        });

        analog_map_modifier_button[analog_id]->setContextMenuPolicy(Qt::CustomContextMenu);

        connect(
            analog_map_modifier_button[analog_id], &QPushButton::customContextMenuRequested,
            [=, this](const QPoint& menu_location) {
                QMenu context_menu;
                Common::ParamPackage param = emulated_controller->GetStickParam(analog_id);
                context_menu.addAction(tr("Clear"), [&] {
                    param.Set("modifier", "");
                    analog_map_modifier_button[analog_id]->setText(tr("[not set]"));
                    emulated_controller->SetStickParam(analog_id, param);
                });
                context_menu.addAction(tr("Toggle button"), [&] {
                    Common::ParamPackage modifier_param =
                        Common::ParamPackage{param.Get("modifier", "")};
                    const bool toggle_value = !modifier_param.Get("toggle", false);
                    modifier_param.Set("toggle", toggle_value);
                    param.Set("modifier", modifier_param.Serialize());
                    analog_map_modifier_button[analog_id]->setText(ButtonToText(modifier_param));
                    emulated_controller->SetStickParam(analog_id, param);
                });
                context_menu.addAction(tr("Invert button"), [&] {
                    Common::ParamPackage modifier_param =
                        Common::ParamPackage{param.Get("modifier", "")};
                    const bool invert_value = !modifier_param.Get("inverted", false);
                    modifier_param.Set("inverted", invert_value);
                    param.Set("modifier", modifier_param.Serialize());
                    analog_map_modifier_button[analog_id]->setText(ButtonToText(modifier_param));
                    emulated_controller->SetStickParam(analog_id, param);
                });
                context_menu.exec(
                    analog_map_modifier_button[analog_id]->mapToGlobal(menu_location));
            });

        connect(analog_map_range_spinbox[analog_id], qOverload<int>(&QSpinBox::valueChanged),
                [=, this] {
                    Common::ParamPackage param = emulated_controller->GetStickParam(analog_id);
                    const auto spinbox_value = analog_map_range_spinbox[analog_id]->value();
                    param.Set("range", spinbox_value / 100.0f);
                    emulated_controller->SetStickParam(analog_id, param);
                });

        connect(analog_map_deadzone_slider[analog_id], &QSlider::valueChanged, [=, this] {
            Common::ParamPackage param = emulated_controller->GetStickParam(analog_id);
            const auto slider_value = analog_map_deadzone_slider[analog_id]->value();
            analog_map_deadzone_label[analog_id]->setText(tr("Deadzone: %1%").arg(slider_value));
            param.Set("deadzone", slider_value / 100.0f);
            emulated_controller->SetStickParam(analog_id, param);
        });

        connect(analog_map_modifier_slider[analog_id], &QSlider::valueChanged, [=, this] {
            Common::ParamPackage param = emulated_controller->GetStickParam(analog_id);
            const auto slider_value = analog_map_modifier_slider[analog_id]->value();
            analog_map_modifier_label[analog_id]->setText(
                tr("Modifier Range: %1%").arg(slider_value));
            param.Set("modifier_scale", slider_value / 100.0f);
            emulated_controller->SetStickParam(analog_id, param);
        });
    }

    // Player Connected checkbox
    connect(ui->groupConnectedController, &QGroupBox::toggled,
            [this](bool checked) { emit Connected(checked); });

    if (player_index == 0) {
        connect(ui->comboControllerType, qOverload<int>(&QComboBox::currentIndexChanged),
                [this](int index) {
                    emit HandheldStateChanged(GetControllerTypeFromIndex(index) ==
                                              Core::HID::NpadStyleIndex::Handheld);
                });
    }

    if (debug || player_index == 9) {
        ui->groupConnectedController->setCheckable(false);
    }

    // The Debug Controller can only choose the Pro Controller.
    if (debug) {
        ui->buttonScreenshot->setEnabled(false);
        ui->buttonHome->setEnabled(false);
        ui->comboControllerType->addItem(tr("Pro Controller"));
    } else {
        SetConnectableControllers();
    }

    UpdateControllerAvailableButtons();
    UpdateControllerEnabledButtons();
    UpdateControllerButtonNames();
    UpdateMotionButtons();
    connect(ui->comboControllerType, qOverload<int>(&QComboBox::currentIndexChanged), [this](int) {
        UpdateControllerAvailableButtons();
        UpdateControllerEnabledButtons();
        UpdateControllerButtonNames();
        UpdateMotionButtons();
        const Core::HID::NpadStyleIndex type =
            GetControllerTypeFromIndex(ui->comboControllerType->currentIndex());

        if (player_index == 0) {
            auto* emulated_controller_p1 =
                hid_core.GetEmulatedController(Core::HID::NpadIdType::Player1);
            auto* emulated_controller_handheld =
                hid_core.GetEmulatedController(Core::HID::NpadIdType::Handheld);
            bool is_connected = emulated_controller->IsConnected(true);

            emulated_controller_p1->SetNpadStyleIndex(type);
            emulated_controller_handheld->SetNpadStyleIndex(type);
            if (is_connected) {
                if (type == Core::HID::NpadStyleIndex::Handheld) {
                    emulated_controller_p1->Disconnect();
                    emulated_controller_handheld->Connect(true);
                    emulated_controller = emulated_controller_handheld;
                } else {
                    emulated_controller_handheld->Disconnect();
                    emulated_controller_p1->Connect(true);
                    emulated_controller = emulated_controller_p1;
                }
            }
            ui->controllerFrame->SetController(emulated_controller);
        }
        emulated_controller->SetNpadStyleIndex(type);
    });

    connect(ui->comboDevices, qOverload<int>(&QComboBox::activated), this,
            &ConfigureInputPlayer::UpdateMappingWithDefaults);
    ui->comboDevices->installEventFilter(this);

    ui->comboDevices->setCurrentIndex(-1);

    timeout_timer->setSingleShot(true);
    connect(timeout_timer.get(), &QTimer::timeout, [this] { SetPollingResult({}, true); });

    connect(poll_timer.get(), &QTimer::timeout, [this] {
        const auto& params = input_subsystem->GetNextInput();
        if (params.Has("engine") && IsInputAcceptable(params)) {
            SetPollingResult(params, false);
            return;
        }
    });

    UpdateInputProfiles();

    connect(ui->buttonProfilesNew, &QPushButton::clicked, this,
            &ConfigureInputPlayer::CreateProfile);
    connect(ui->buttonProfilesDelete, &QPushButton::clicked, this,
            &ConfigureInputPlayer::DeleteProfile);
    connect(ui->comboProfiles, qOverload<int>(&QComboBox::activated), this,
            &ConfigureInputPlayer::LoadProfile);
    connect(ui->buttonProfilesSave, &QPushButton::clicked, this,
            &ConfigureInputPlayer::SaveProfile);

    LoadConfiguration();
}

ConfigureInputPlayer::~ConfigureInputPlayer() {
    if (player_index == 0) {
        auto* emulated_controller_p1 =
            hid_core.GetEmulatedController(Core::HID::NpadIdType::Player1);
        auto* emulated_controller_handheld =
            hid_core.GetEmulatedController(Core::HID::NpadIdType::Handheld);
        emulated_controller_p1->DisableConfiguration();
        emulated_controller_handheld->DisableConfiguration();
    } else {
        emulated_controller->DisableConfiguration();
    }
}

void ConfigureInputPlayer::ApplyConfiguration() {
    if (player_index == 0) {
        auto* emulated_controller_p1 =
            hid_core.GetEmulatedController(Core::HID::NpadIdType::Player1);
        auto* emulated_controller_handheld =
            hid_core.GetEmulatedController(Core::HID::NpadIdType::Handheld);
        emulated_controller_p1->DisableConfiguration();
        emulated_controller_p1->SaveCurrentConfig();
        emulated_controller_p1->EnableConfiguration();
        emulated_controller_handheld->DisableConfiguration();
        emulated_controller_handheld->SaveCurrentConfig();
        emulated_controller_handheld->EnableConfiguration();
        return;
    }
    emulated_controller->DisableConfiguration();
    emulated_controller->SaveCurrentConfig();
    emulated_controller->EnableConfiguration();
}

void ConfigureInputPlayer::showEvent(QShowEvent* event) {
    if (bottom_row == nullptr) {
        return;
    }
    QWidget::showEvent(event);
    ui->main->addWidget(bottom_row);
}

void ConfigureInputPlayer::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureInputPlayer::RetranslateUI() {
    ui->retranslateUi(this);
    UpdateUI();
}

void ConfigureInputPlayer::LoadConfiguration() {
    emulated_controller->ReloadFromSettings();

    UpdateUI();
    UpdateInputDeviceCombobox();

    if (debug) {
        return;
    }

    const int comboBoxIndex =
        GetIndexFromControllerType(emulated_controller->GetNpadStyleIndex(true));
    ui->comboControllerType->setCurrentIndex(comboBoxIndex);
    ui->groupConnectedController->setChecked(emulated_controller->IsConnected(true));
}

void ConfigureInputPlayer::ConnectPlayer(bool connected) {
    ui->groupConnectedController->setChecked(connected);
    if (connected) {
        emulated_controller->Connect(true);
    } else {
        emulated_controller->Disconnect();
    }
}

void ConfigureInputPlayer::UpdateInputDeviceCombobox() {
    // Skip input device persistence if "Input Devices" is set to "Any".
    if (ui->comboDevices->currentIndex() == 0) {
        UpdateInputDevices();
        return;
    }

    const auto devices = emulated_controller->GetMappedDevices();
    UpdateInputDevices();

    if (devices.empty()) {
        return;
    }

    if (devices.size() > 2) {
        ui->comboDevices->setCurrentIndex(0);
        return;
    }

    const auto first_engine = devices[0].Get("engine", "");
    const auto first_guid = devices[0].Get("guid", "");
    const auto first_port = devices[0].Get("port", 0);
    const auto first_pad = devices[0].Get("pad", 0);

    if (devices.size() == 1) {
        const auto devices_it = std::find_if(
            input_devices.begin(), input_devices.end(),
            [first_engine, first_guid, first_port, first_pad](const Common::ParamPackage& param) {
                return param.Get("engine", "") == first_engine &&
                       param.Get("guid", "") == first_guid && param.Get("port", 0) == first_port &&
                       param.Get("pad", 0) == first_pad;
            });
        const int device_index =
            devices_it != input_devices.end()
                ? static_cast<int>(std::distance(input_devices.begin(), devices_it))
                : 0;
        ui->comboDevices->setCurrentIndex(device_index);
        return;
    }

    const auto second_engine = devices[1].Get("engine", "");
    const auto second_guid = devices[1].Get("guid", "");
    const auto second_port = devices[1].Get("port", 0);

    const bool is_keyboard_mouse = (first_engine == "keyboard" || first_engine == "mouse") &&
                                   (second_engine == "keyboard" || second_engine == "mouse");

    if (is_keyboard_mouse) {
        ui->comboDevices->setCurrentIndex(2);
        return;
    }

    const bool is_engine_equal = first_engine == second_engine;
    const bool is_port_equal = first_port == second_port;

    if (is_engine_equal && is_port_equal) {
        const auto devices_it = std::find_if(
            input_devices.begin(), input_devices.end(),
            [first_engine, first_guid, second_guid, first_port](const Common::ParamPackage& param) {
                const bool is_guid_valid =
                    (param.Get("guid", "") == first_guid &&
                     param.Get("guid2", "") == second_guid) ||
                    (param.Get("guid", "") == second_guid && param.Get("guid2", "") == first_guid);
                return param.Get("engine", "") == first_engine && is_guid_valid &&
                       param.Get("port", 0) == first_port;
            });
        const int device_index =
            devices_it != input_devices.end()
                ? static_cast<int>(std::distance(input_devices.begin(), devices_it))
                : 0;
        ui->comboDevices->setCurrentIndex(device_index);
    } else {
        ui->comboDevices->setCurrentIndex(0);
    }
}

void ConfigureInputPlayer::RestoreDefaults() {
    UpdateMappingWithDefaults();
}

void ConfigureInputPlayer::ClearAll() {
    for (int button_id = 0; button_id < Settings::NativeButton::NumButtons; ++button_id) {
        const auto* const button = button_map[button_id];
        if (button == nullptr) {
            continue;
        }
        emulated_controller->SetButtonParam(button_id, {});
    }

    for (int analog_id = 0; analog_id < Settings::NativeAnalog::NumAnalogs; ++analog_id) {
        for (int sub_button_id = 0; sub_button_id < ANALOG_SUB_BUTTONS_NUM; ++sub_button_id) {
            const auto* const analog_button = analog_map_buttons[analog_id][sub_button_id];
            if (analog_button == nullptr) {
                continue;
            }
            emulated_controller->SetStickParam(analog_id, {});
        }
    }

    for (int motion_id = 0; motion_id < Settings::NativeMotion::NumMotions; ++motion_id) {
        const auto* const motion_button = motion_map[motion_id];
        if (motion_button == nullptr) {
            continue;
        }
        emulated_controller->SetMotionParam(motion_id, {});
    }

    UpdateUI();
    UpdateInputDevices();
}

void ConfigureInputPlayer::UpdateUI() {
    for (int button = 0; button < Settings::NativeButton::NumButtons; ++button) {
        const Common::ParamPackage param = emulated_controller->GetButtonParam(button);
        button_map[button]->setText(ButtonToText(param));
    }

    const Common::ParamPackage ZL_param =
        emulated_controller->GetButtonParam(Settings::NativeButton::ZL);
    if (ZL_param.Has("threshold")) {
        const int button_threshold = static_cast<int>(ZL_param.Get("threshold", 0.5f) * 100.0f);
        ui->sliderZLThreshold->setValue(button_threshold);
    }

    const Common::ParamPackage ZR_param =
        emulated_controller->GetButtonParam(Settings::NativeButton::ZR);
    if (ZR_param.Has("threshold")) {
        const int button_threshold = static_cast<int>(ZR_param.Get("threshold", 0.5f) * 100.0f);
        ui->sliderZRThreshold->setValue(button_threshold);
    }

    for (int motion_id = 0; motion_id < Settings::NativeMotion::NumMotions; ++motion_id) {
        const Common::ParamPackage param = emulated_controller->GetMotionParam(motion_id);
        motion_map[motion_id]->setText(ButtonToText(param));
    }

    for (int analog_id = 0; analog_id < Settings::NativeAnalog::NumAnalogs; ++analog_id) {
        const Common::ParamPackage param = emulated_controller->GetStickParam(analog_id);
        for (int sub_button_id = 0; sub_button_id < ANALOG_SUB_BUTTONS_NUM; ++sub_button_id) {
            auto* const analog_button = analog_map_buttons[analog_id][sub_button_id];

            if (analog_button == nullptr) {
                continue;
            }

            analog_button->setText(AnalogToText(param, analog_sub_buttons[sub_button_id]));
        }

        analog_map_modifier_button[analog_id]->setText(
            ButtonToText(Common::ParamPackage{param.Get("modifier", "")}));

        const auto deadzone_label = analog_map_deadzone_label[analog_id];
        const auto deadzone_slider = analog_map_deadzone_slider[analog_id];
        const auto modifier_groupbox = analog_map_modifier_groupbox[analog_id];
        const auto modifier_label = analog_map_modifier_label[analog_id];
        const auto modifier_slider = analog_map_modifier_slider[analog_id];
        const auto range_groupbox = analog_map_range_groupbox[analog_id];
        const auto range_spinbox = analog_map_range_spinbox[analog_id];

        int slider_value;
        const bool is_controller = input_subsystem->IsController(param);

        if (is_controller) {
            slider_value = static_cast<int>(param.Get("deadzone", 0.15f) * 100);
            deadzone_label->setText(tr("Deadzone: %1%").arg(slider_value));
            deadzone_slider->setValue(slider_value);
            range_spinbox->setValue(static_cast<int>(param.Get("range", 0.95f) * 100));
        } else {
            slider_value = static_cast<int>(param.Get("modifier_scale", 0.5f) * 100);
            modifier_label->setText(tr("Modifier Range: %1%").arg(slider_value));
            modifier_slider->setValue(slider_value);
        }

        deadzone_label->setVisible(is_controller);
        deadzone_slider->setVisible(is_controller);
        modifier_groupbox->setVisible(!is_controller);
        modifier_label->setVisible(!is_controller);
        modifier_slider->setVisible(!is_controller);
        range_groupbox->setVisible(is_controller);
    }
}

void ConfigureInputPlayer::SetConnectableControllers() {
    const auto npad_style_set = hid_core.GetSupportedStyleTag();
    index_controller_type_pairs.clear();
    ui->comboControllerType->clear();

    const auto add_item = [&](Core::HID::NpadStyleIndex controller_type,
                              const QString& controller_name) {
        index_controller_type_pairs.emplace_back(ui->comboControllerType->count(), controller_type);
        ui->comboControllerType->addItem(controller_name);
    };

    if (npad_style_set.fullkey == 1) {
        add_item(Core::HID::NpadStyleIndex::ProController, tr("Pro Controller"));
    }

    if (npad_style_set.joycon_dual == 1) {
        add_item(Core::HID::NpadStyleIndex::JoyconDual, tr("Dual Joycons"));
    }

    if (npad_style_set.joycon_left == 1) {
        add_item(Core::HID::NpadStyleIndex::JoyconLeft, tr("Left Joycon"));
    }

    if (npad_style_set.joycon_right == 1) {
        add_item(Core::HID::NpadStyleIndex::JoyconRight, tr("Right Joycon"));
    }

    if (player_index == 0 && npad_style_set.handheld == 1) {
        add_item(Core::HID::NpadStyleIndex::Handheld, tr("Handheld"));
    }

    if (npad_style_set.gamecube == 1) {
        add_item(Core::HID::NpadStyleIndex::GameCube, tr("GameCube Controller"));
    }

    // Disable all unsupported controllers
    if (!Settings::values.enable_all_controllers) {
        return;
    }

    if (npad_style_set.palma == 1) {
        add_item(Core::HID::NpadStyleIndex::Pokeball, tr("Poke Ball Plus"));
    }

    if (npad_style_set.lark == 1) {
        add_item(Core::HID::NpadStyleIndex::NES, tr("NES Controller"));
    }

    if (npad_style_set.lucia == 1) {
        add_item(Core::HID::NpadStyleIndex::SNES, tr("SNES Controller"));
    }

    if (npad_style_set.lagoon == 1) {
        add_item(Core::HID::NpadStyleIndex::N64, tr("N64 Controller"));
    }

    if (npad_style_set.lager == 1) {
        add_item(Core::HID::NpadStyleIndex::SegaGenesis, tr("Sega Genesis"));
    }
}

Core::HID::NpadStyleIndex ConfigureInputPlayer::GetControllerTypeFromIndex(int index) const {
    const auto it =
        std::find_if(index_controller_type_pairs.begin(), index_controller_type_pairs.end(),
                     [index](const auto& pair) { return pair.first == index; });

    if (it == index_controller_type_pairs.end()) {
        return Core::HID::NpadStyleIndex::ProController;
    }

    return it->second;
}

int ConfigureInputPlayer::GetIndexFromControllerType(Core::HID::NpadStyleIndex type) const {
    const auto it =
        std::find_if(index_controller_type_pairs.begin(), index_controller_type_pairs.end(),
                     [type](const auto& pair) { return pair.second == type; });

    if (it == index_controller_type_pairs.end()) {
        return -1;
    }

    return it->first;
}

void ConfigureInputPlayer::UpdateInputDevices() {
    input_devices = input_subsystem->GetInputDevices();
    ui->comboDevices->clear();
    for (const auto& device : input_devices) {
        ui->comboDevices->addItem(QString::fromStdString(device.Get("display", "Unknown")), {});
    }
}

void ConfigureInputPlayer::UpdateControllerAvailableButtons() {
    auto layout = GetControllerTypeFromIndex(ui->comboControllerType->currentIndex());
    if (debug) {
        layout = Core::HID::NpadStyleIndex::ProController;
    }

    // List of all the widgets that will be hidden by any of the following layouts that need
    // "unhidden" after the controller type changes
    const std::array<QWidget*, 11> layout_show = {
        ui->buttonShoulderButtonsSLSR,
        ui->horizontalSpacerShoulderButtonsWidget,
        ui->horizontalSpacerShoulderButtonsWidget2,
        ui->buttonShoulderButtonsLeft,
        ui->buttonMiscButtonsMinusScreenshot,
        ui->bottomLeft,
        ui->buttonShoulderButtonsRight,
        ui->buttonMiscButtonsPlusHome,
        ui->bottomRight,
        ui->buttonMiscButtonsMinusGroup,
        ui->buttonMiscButtonsScreenshotGroup,
    };

    for (auto* widget : layout_show) {
        widget->show();
    }

    std::vector<QWidget*> layout_hidden;
    switch (layout) {
    case Core::HID::NpadStyleIndex::ProController:
    case Core::HID::NpadStyleIndex::JoyconDual:
    case Core::HID::NpadStyleIndex::Handheld:
        layout_hidden = {
            ui->buttonShoulderButtonsSLSR,
            ui->horizontalSpacerShoulderButtonsWidget2,
        };
        break;
    case Core::HID::NpadStyleIndex::JoyconLeft:
        layout_hidden = {
            ui->horizontalSpacerShoulderButtonsWidget2,
            ui->buttonShoulderButtonsRight,
            ui->buttonMiscButtonsPlusHome,
            ui->bottomRight,
        };
        break;
    case Core::HID::NpadStyleIndex::JoyconRight:
        layout_hidden = {
            ui->horizontalSpacerShoulderButtonsWidget,
            ui->buttonShoulderButtonsLeft,
            ui->buttonMiscButtonsMinusScreenshot,
            ui->bottomLeft,
        };
        break;
    case Core::HID::NpadStyleIndex::GameCube:
        layout_hidden = {
            ui->buttonShoulderButtonsSLSR,
            ui->horizontalSpacerShoulderButtonsWidget2,
            ui->buttonMiscButtonsMinusGroup,
            ui->buttonMiscButtonsScreenshotGroup,
        };
        break;
    default:
        break;
    }

    for (auto* widget : layout_hidden) {
        widget->hide();
    }
}

void ConfigureInputPlayer::UpdateControllerEnabledButtons() {
    auto layout = GetControllerTypeFromIndex(ui->comboControllerType->currentIndex());
    if (debug) {
        layout = Core::HID::NpadStyleIndex::ProController;
    }

    // List of all the widgets that will be disabled by any of the following layouts that need
    // "enabled" after the controller type changes
    const std::array<QWidget*, 3> layout_enable = {
        ui->buttonLStickPressedGroup,
        ui->groupRStickPressed,
        ui->buttonShoulderButtonsButtonLGroup,
    };

    for (auto* widget : layout_enable) {
        widget->setEnabled(true);
    }

    std::vector<QWidget*> layout_disable;
    switch (layout) {
    case Core::HID::NpadStyleIndex::ProController:
    case Core::HID::NpadStyleIndex::JoyconDual:
    case Core::HID::NpadStyleIndex::Handheld:
    case Core::HID::NpadStyleIndex::JoyconLeft:
    case Core::HID::NpadStyleIndex::JoyconRight:
        break;
    case Core::HID::NpadStyleIndex::GameCube:
        layout_disable = {
            ui->buttonHome,
            ui->buttonLStickPressedGroup,
            ui->groupRStickPressed,
            ui->buttonShoulderButtonsButtonLGroup,
        };
        break;
    default:
        break;
    }

    for (auto* widget : layout_disable) {
        widget->setEnabled(false);
    }
}

void ConfigureInputPlayer::UpdateMotionButtons() {
    if (debug) {
        // Motion isn't used with the debug controller, hide both groupboxes.
        ui->buttonMotionLeftGroup->hide();
        ui->buttonMotionRightGroup->hide();
        return;
    }

    // Show/hide the "Motion 1/2" groupboxes depending on the currently selected controller.
    switch (GetControllerTypeFromIndex(ui->comboControllerType->currentIndex())) {
    case Core::HID::NpadStyleIndex::ProController:
    case Core::HID::NpadStyleIndex::JoyconLeft:
    case Core::HID::NpadStyleIndex::Handheld:
        // Show "Motion 1" and hide "Motion 2".
        ui->buttonMotionLeftGroup->show();
        ui->buttonMotionRightGroup->hide();
        break;
    case Core::HID::NpadStyleIndex::JoyconRight:
        // Show "Motion 2" and hide "Motion 1".
        ui->buttonMotionLeftGroup->hide();
        ui->buttonMotionRightGroup->show();
        break;
    case Core::HID::NpadStyleIndex::GameCube:
        // Hide both "Motion 1/2".
        ui->buttonMotionLeftGroup->hide();
        ui->buttonMotionRightGroup->hide();
        break;
    case Core::HID::NpadStyleIndex::JoyconDual:
    default:
        // Show both "Motion 1/2".
        ui->buttonMotionLeftGroup->show();
        ui->buttonMotionRightGroup->show();
        break;
    }
}

void ConfigureInputPlayer::UpdateControllerButtonNames() {
    auto layout = GetControllerTypeFromIndex(ui->comboControllerType->currentIndex());
    if (debug) {
        layout = Core::HID::NpadStyleIndex::ProController;
    }

    switch (layout) {
    case Core::HID::NpadStyleIndex::ProController:
    case Core::HID::NpadStyleIndex::JoyconDual:
    case Core::HID::NpadStyleIndex::Handheld:
    case Core::HID::NpadStyleIndex::JoyconLeft:
    case Core::HID::NpadStyleIndex::JoyconRight:
        ui->buttonMiscButtonsPlusGroup->setTitle(tr("Plus"));
        ui->buttonShoulderButtonsButtonZLGroup->setTitle(tr("ZL"));
        ui->buttonShoulderButtonsZRGroup->setTitle(tr("ZR"));
        ui->buttonShoulderButtonsRGroup->setTitle(tr("R"));
        ui->LStick->setTitle(tr("Left Stick"));
        ui->RStick->setTitle(tr("Right Stick"));
        break;
    case Core::HID::NpadStyleIndex::GameCube:
        ui->buttonMiscButtonsPlusGroup->setTitle(tr("Start / Pause"));
        ui->buttonShoulderButtonsButtonZLGroup->setTitle(tr("L"));
        ui->buttonShoulderButtonsZRGroup->setTitle(tr("R"));
        ui->buttonShoulderButtonsRGroup->setTitle(tr("Z"));
        ui->LStick->setTitle(tr("Control Stick"));
        ui->RStick->setTitle(tr("C-Stick"));
        break;
    default:
        break;
    }
}

void ConfigureInputPlayer::UpdateMappingWithDefaults() {
    if (ui->comboDevices->currentIndex() == 0) {
        return;
    }

    for (int button_id = 0; button_id < Settings::NativeButton::NumButtons; ++button_id) {
        const auto* const button = button_map[button_id];
        if (button == nullptr) {
            continue;
        }
        emulated_controller->SetButtonParam(button_id, {});
    }

    for (int analog_id = 0; analog_id < Settings::NativeAnalog::NumAnalogs; ++analog_id) {
        for (int sub_button_id = 0; sub_button_id < ANALOG_SUB_BUTTONS_NUM; ++sub_button_id) {
            const auto* const analog_button = analog_map_buttons[analog_id][sub_button_id];
            if (analog_button == nullptr) {
                continue;
            }
            emulated_controller->SetStickParam(analog_id, {});
        }
    }

    for (int motion_id = 0; motion_id < Settings::NativeMotion::NumMotions; ++motion_id) {
        const auto* const motion_button = motion_map[motion_id];
        if (motion_button == nullptr) {
            continue;
        }
        emulated_controller->SetMotionParam(motion_id, {});
    }

    // Reset keyboard or mouse bindings
    if (ui->comboDevices->currentIndex() == 1 || ui->comboDevices->currentIndex() == 2) {
        for (int button_id = 0; button_id < Settings::NativeButton::NumButtons; ++button_id) {
            emulated_controller->SetButtonParam(
                button_id, Common::ParamPackage{InputCommon::GenerateKeyboardParam(
                               Config::default_buttons[button_id])});
        }
        for (int analog_id = 0; analog_id < Settings::NativeAnalog::NumAnalogs; ++analog_id) {
            Common::ParamPackage analog_param{};
            for (int sub_button_id = 0; sub_button_id < ANALOG_SUB_BUTTONS_NUM; ++sub_button_id) {
                Common::ParamPackage params{InputCommon::GenerateKeyboardParam(
                    Config::default_analogs[analog_id][sub_button_id])};
                SetAnalogParam(params, analog_param, analog_sub_buttons[sub_button_id]);
            }

            analog_param.Set("modifier", InputCommon::GenerateKeyboardParam(
                                             Config::default_stick_mod[analog_id]));
            emulated_controller->SetStickParam(analog_id, analog_param);
        }

        for (int motion_id = 0; motion_id < Settings::NativeMotion::NumMotions; ++motion_id) {
            emulated_controller->SetMotionParam(
                motion_id, Common::ParamPackage{InputCommon::GenerateKeyboardParam(
                               Config::default_motions[motion_id])});
        }

        // If mouse is selected we want to override with mappings from the driver
        if (ui->comboDevices->currentIndex() == 1) {
            UpdateUI();
            return;
        }
    }

    // Reset controller bindings
    const auto& device = input_devices[ui->comboDevices->currentIndex()];
    auto button_mappings = input_subsystem->GetButtonMappingForDevice(device);
    auto analog_mappings = input_subsystem->GetAnalogMappingForDevice(device);
    auto motion_mappings = input_subsystem->GetMotionMappingForDevice(device);

    for (const auto& button_mapping : button_mappings) {
        const std::size_t index = button_mapping.first;
        emulated_controller->SetButtonParam(index, button_mapping.second);
    }
    for (const auto& analog_mapping : analog_mappings) {
        const std::size_t index = analog_mapping.first;
        emulated_controller->SetStickParam(index, analog_mapping.second);
    }
    for (const auto& motion_mapping : motion_mappings) {
        const std::size_t index = motion_mapping.first;
        emulated_controller->SetMotionParam(index, motion_mapping.second);
    }

    UpdateUI();
}

void ConfigureInputPlayer::HandleClick(
    QPushButton* button, std::size_t button_id,
    std::function<void(const Common::ParamPackage&)> new_input_setter,
    InputCommon::Polling::InputType type) {
    if (timeout_timer->isActive()) {
        return;
    }
    if (button == ui->buttonMotionLeft || button == ui->buttonMotionRight) {
        button->setText(tr("Shake!"));
    } else {
        button->setText(tr("[waiting]"));
    }
    button->setFocus();

    input_setter = std::move(new_input_setter);

    input_subsystem->BeginMapping(type);

    QWidget::grabMouse();
    QWidget::grabKeyboard();

    if (type == InputCommon::Polling::InputType::Button) {
        ui->controllerFrame->BeginMappingButton(button_id);
    } else if (type == InputCommon::Polling::InputType::Stick) {
        ui->controllerFrame->BeginMappingAnalog(button_id);
    }

    timeout_timer->start(4000); // Cancel after 4 seconds
    poll_timer->start(25);      // Check for new inputs every 25ms
}

void ConfigureInputPlayer::SetPollingResult(const Common::ParamPackage& params, bool abort) {
    timeout_timer->stop();
    poll_timer->stop();
    input_subsystem->StopMapping();

    QWidget::releaseMouse();
    QWidget::releaseKeyboard();

    if (!abort) {
        (*input_setter)(params);
    }

    UpdateUI();
    UpdateInputDeviceCombobox();
    ui->controllerFrame->EndMapping();

    input_setter = std::nullopt;
}

bool ConfigureInputPlayer::IsInputAcceptable(const Common::ParamPackage& params) const {
    if (ui->comboDevices->currentIndex() == 0) {
        return true;
    }

    if (params.Has("motion")) {
        return true;
    }

    // Keyboard/Mouse
    if (ui->comboDevices->currentIndex() == 1 || ui->comboDevices->currentIndex() == 2) {
        return params.Get("engine", "") == "keyboard" || params.Get("engine", "") == "mouse";
    }

    const auto& current_input_device = input_devices[ui->comboDevices->currentIndex()];
    return params.Get("engine", "") == current_input_device.Get("engine", "") &&
           (params.Get("guid", "") == current_input_device.Get("guid", "") ||
            params.Get("guid", "") == current_input_device.Get("guid2", "")) &&
           params.Get("port", 0) == current_input_device.Get("port", 0);
}

void ConfigureInputPlayer::mousePressEvent(QMouseEvent* event) {
    if (!input_setter || !event) {
        return;
    }

    const auto button = GRenderWindow::QtButtonToMouseButton(event->button());
    input_subsystem->GetMouse()->PressButton(0, 0, button);
}

void ConfigureInputPlayer::wheelEvent(QWheelEvent* event) {
    const int x = event->angleDelta().x();
    const int y = event->angleDelta().y();
    input_subsystem->GetMouse()->MouseWheelChange(x, y);
}

void ConfigureInputPlayer::keyPressEvent(QKeyEvent* event) {
    if (!input_setter || !event) {
        return;
    }
    event->ignore();
    if (event->key() != Qt::Key_Escape) {
        input_subsystem->GetKeyboard()->PressKey(event->key());
    }
}

bool ConfigureInputPlayer::eventFilter(QObject* object, QEvent* event) {
    if (object == ui->comboDevices && event->type() == QEvent::MouseButtonPress) {
        RefreshInputDevices();
    }
    return object->eventFilter(object, event);
}

void ConfigureInputPlayer::CreateProfile() {
    const auto profile_name =
        LimitableInputDialog::GetText(this, tr("New Profile"), tr("Enter a profile name:"), 1, 30,
                                      LimitableInputDialog::InputLimiter::Filesystem);

    if (profile_name.isEmpty()) {
        return;
    }

    if (!InputProfiles::IsProfileNameValid(profile_name.toStdString())) {
        QMessageBox::critical(this, tr("Create Input Profile"),
                              tr("The given profile name is not valid!"));
        return;
    }

    ApplyConfiguration();

    if (!profiles->CreateProfile(profile_name.toStdString(), player_index)) {
        QMessageBox::critical(this, tr("Create Input Profile"),
                              tr("Failed to create the input profile \"%1\"").arg(profile_name));
        UpdateInputProfiles();
        emit RefreshInputProfiles(player_index);
        return;
    }

    emit RefreshInputProfiles(player_index);

    ui->comboProfiles->addItem(profile_name);
    ui->comboProfiles->setCurrentIndex(ui->comboProfiles->count() - 1);
}

void ConfigureInputPlayer::DeleteProfile() {
    const QString profile_name = ui->comboProfiles->itemText(ui->comboProfiles->currentIndex());

    if (profile_name.isEmpty()) {
        return;
    }

    if (!profiles->DeleteProfile(profile_name.toStdString())) {
        QMessageBox::critical(this, tr("Delete Input Profile"),
                              tr("Failed to delete the input profile \"%1\"").arg(profile_name));
        UpdateInputProfiles();
        emit RefreshInputProfiles(player_index);
        return;
    }

    emit RefreshInputProfiles(player_index);

    ui->comboProfiles->removeItem(ui->comboProfiles->currentIndex());
    ui->comboProfiles->setCurrentIndex(-1);
}

void ConfigureInputPlayer::LoadProfile() {
    const QString profile_name = ui->comboProfiles->itemText(ui->comboProfiles->currentIndex());

    if (profile_name.isEmpty()) {
        return;
    }

    ApplyConfiguration();

    if (!profiles->LoadProfile(profile_name.toStdString(), player_index)) {
        QMessageBox::critical(this, tr("Load Input Profile"),
                              tr("Failed to load the input profile \"%1\"").arg(profile_name));
        UpdateInputProfiles();
        emit RefreshInputProfiles(player_index);
        return;
    }

    LoadConfiguration();
}

void ConfigureInputPlayer::SaveProfile() {
    static constexpr size_t HANDHELD_INDEX = 8;
    const QString profile_name = ui->comboProfiles->itemText(ui->comboProfiles->currentIndex());

    if (profile_name.isEmpty()) {
        return;
    }

    ApplyConfiguration();

    // When we're in handheld mode, only the handheld emulated controller bindings are updated
    const bool is_handheld = player_index == 0 && emulated_controller->GetNpadIdType() ==
                                                      Core::HID::NpadIdType::Handheld;
    const auto profile_player_index = is_handheld ? HANDHELD_INDEX : player_index;

    if (!profiles->SaveProfile(profile_name.toStdString(), profile_player_index)) {
        QMessageBox::critical(this, tr("Save Input Profile"),
                              tr("Failed to save the input profile \"%1\"").arg(profile_name));
        UpdateInputProfiles();
        emit RefreshInputProfiles(player_index);
        return;
    }
}

void ConfigureInputPlayer::UpdateInputProfiles() {
    ui->comboProfiles->clear();

    for (const auto& profile_name : profiles->GetInputProfileNames()) {
        ui->comboProfiles->addItem(QString::fromStdString(profile_name));
    }

    ui->comboProfiles->setCurrentIndex(-1);
}
