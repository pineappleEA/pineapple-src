// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <QKeySequence>
#include <QSettings>
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/hid/controllers/npad.h"
#include "input_common/main.h"
#include "network/network.h"
#include "yuzu/configuration/config.h"

namespace FS = Common::FS;

Config::Config(const std::string& config_name, ConfigType config_type) : type(config_type) {
    global = config_type == ConfigType::GlobalConfig;

    Initialize(config_name);
}

Config::~Config() {
    if (global) {
        Save();
    }
}

const std::array<int, Settings::NativeButton::NumButtons> Config::default_buttons = {
    Qt::Key_C,    Qt::Key_X, Qt::Key_V,    Qt::Key_Z,  Qt::Key_F,
    Qt::Key_G,    Qt::Key_Q, Qt::Key_E,    Qt::Key_R,  Qt::Key_T,
    Qt::Key_M,    Qt::Key_N, Qt::Key_Left, Qt::Key_Up, Qt::Key_Right,
    Qt::Key_Down, Qt::Key_Q, Qt::Key_E,    0,          0,
};

const std::array<int, Settings::NativeMotion::NumMotions> Config::default_motions = {
    Qt::Key_7,
    Qt::Key_8,
};

const std::array<std::array<int, 4>, Settings::NativeAnalog::NumAnalogs> Config::default_analogs{{
    {
        Qt::Key_W,
        Qt::Key_S,
        Qt::Key_A,
        Qt::Key_D,
    },
    {
        Qt::Key_I,
        Qt::Key_K,
        Qt::Key_J,
        Qt::Key_L,
    },
}};

const std::array<int, 2> Config::default_stick_mod = {
    Qt::Key_Shift,
    0,
};

const std::array<int, 2> Config::default_ringcon_analogs{{
    Qt::Key_A,
    Qt::Key_D,
}};

// This shouldn't have anything except static initializers (no functions). So
// QKeySequence(...).toString() is NOT ALLOWED HERE.
// This must be in alphabetical order according to action name as it must have the same order as
// UISetting::values.shortcuts, which is alphabetically ordered.
// clang-format off
const std::array<UISettings::Shortcut, 22> Config::default_hotkeys{{
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Audio Mute/Unmute")),        QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("Ctrl+M"),  QStringLiteral("Home+Dpad_Right"), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Audio Volume Down")),        QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("-"),       QStringLiteral("Home+Dpad_Down"), Qt::ApplicationShortcut, true}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Audio Volume Up")),          QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("="),       QStringLiteral("Home+Dpad_Up"), Qt::ApplicationShortcut, true}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Capture Screenshot")),       QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("Ctrl+P"),  QStringLiteral("Screenshot"), Qt::WidgetWithChildrenShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Change Adapting Filter")),   QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("F8"),      QStringLiteral("Home+L"), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Change Docked Mode")),       QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("F10"),     QStringLiteral("Home+X"), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Change GPU Accuracy")),      QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("F9"),      QStringLiteral("Home+R"), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Continue/Pause Emulation")), QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("F4"),      QStringLiteral("Home+Plus"), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Exit Fullscreen")),          QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("Esc"),     QStringLiteral(""), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Exit yuzu")),                QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("Ctrl+Q"),  QStringLiteral("Home+Minus"), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Fullscreen")),               QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("F11"),     QStringLiteral("Home+B"), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Load File")),                QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("Ctrl+O"),  QStringLiteral(""), Qt::WidgetWithChildrenShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Load/Remove Amiibo")),       QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("F2"),      QStringLiteral("Home+A"), Qt::WidgetWithChildrenShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Restart Emulation")),        QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("F6"),      QStringLiteral(""), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Stop Emulation")),           QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("F5"),      QStringLiteral(""), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "TAS Record")),               QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("Ctrl+F7"), QStringLiteral(""), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "TAS Reset")),                QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("Ctrl+F6"), QStringLiteral(""), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "TAS Start/Stop")),           QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("Ctrl+F5"), QStringLiteral(""), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Toggle Filter Bar")),        QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("Ctrl+F"),  QStringLiteral(""), Qt::WindowShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Toggle Framerate Limit")),   QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("Ctrl+U"),  QStringLiteral("Home+Y"), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Toggle Mouse Panning")),     QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("Ctrl+F9"), QStringLiteral(""), Qt::ApplicationShortcut, false}},
    {QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Toggle Status Bar")),        QStringLiteral(QT_TRANSLATE_NOOP("Hotkeys", "Main Window")), {QStringLiteral("Ctrl+S"),  QStringLiteral(""), Qt::WindowShortcut, false}},
}};
// clang-format on

void Config::Initialize(const std::string& config_name) {
    const auto fs_config_loc = FS::GetYuzuPath(FS::YuzuPath::ConfigDir);
    const auto config_file = fmt::format("{}.ini", config_name);

    switch (type) {
    case ConfigType::GlobalConfig:
        qt_config_loc = FS::PathToUTF8String(fs_config_loc / config_file);
        void(FS::CreateParentDir(qt_config_loc));
        qt_config = std::make_unique<QSettings>(QString::fromStdString(qt_config_loc),
                                                QSettings::IniFormat);
        Reload();
        break;
    case ConfigType::PerGameConfig:
        qt_config_loc =
            FS::PathToUTF8String(fs_config_loc / "custom" / FS::ToU8String(config_file));
        void(FS::CreateParentDir(qt_config_loc));
        qt_config = std::make_unique<QSettings>(QString::fromStdString(qt_config_loc),
                                                QSettings::IniFormat);
        Reload();
        break;
    case ConfigType::InputProfile:
        qt_config_loc = FS::PathToUTF8String(fs_config_loc / "input" / config_file);
        void(FS::CreateParentDir(qt_config_loc));
        qt_config = std::make_unique<QSettings>(QString::fromStdString(qt_config_loc),
                                                QSettings::IniFormat);
        break;
    }
}

bool Config::IsCustomConfig() {
    return type == ConfigType::PerGameConfig;
}

/* {Read,Write}BasicSetting and WriteGlobalSetting templates must be defined here before their
 * usages later in this file. This allows explicit definition of some types that don't work
 * nicely with the general version.
 */

// Explicit std::string definition: Qt can't implicitly convert a std::string to a QVariant, nor
// can it implicitly convert a QVariant back to a {std::,Q}string
template <>
void Config::ReadBasicSetting(Settings::Setting<std::string>& setting) {
    const QString name = QString::fromStdString(setting.GetLabel());
    const auto default_value = QString::fromStdString(setting.GetDefault());
    if (qt_config->value(name + QStringLiteral("/default"), false).toBool()) {
        setting.SetValue(default_value.toStdString());
    } else {
        setting.SetValue(qt_config->value(name, default_value).toString().toStdString());
    }
}

template <typename Type, bool ranged>
void Config::ReadBasicSetting(Settings::Setting<Type, ranged>& setting) {
    const QString name = QString::fromStdString(setting.GetLabel());
    const Type default_value = setting.GetDefault();
    if (qt_config->value(name + QStringLiteral("/default"), false).toBool()) {
        setting.SetValue(default_value);
    } else {
        setting.SetValue(
            static_cast<QVariant>(qt_config->value(name, default_value)).value<Type>());
    }
}

// Explicit std::string definition: Qt can't implicitly convert a std::string to a QVariant
template <>
void Config::WriteBasicSetting(const Settings::Setting<std::string>& setting) {
    const QString name = QString::fromStdString(setting.GetLabel());
    const std::string& value = setting.GetValue();
    qt_config->setValue(name + QStringLiteral("/default"), value == setting.GetDefault());
    qt_config->setValue(name, QString::fromStdString(value));
}

template <typename Type, bool ranged>
void Config::WriteBasicSetting(const Settings::Setting<Type, ranged>& setting) {
    const QString name = QString::fromStdString(setting.GetLabel());
    const Type value = setting.GetValue();
    qt_config->setValue(name + QStringLiteral("/default"), value == setting.GetDefault());
    qt_config->setValue(name, value);
}

template <typename Type, bool ranged>
void Config::WriteGlobalSetting(const Settings::SwitchableSetting<Type, ranged>& setting) {
    const QString name = QString::fromStdString(setting.GetLabel());
    const Type& value = setting.GetValue(global);
    if (!global) {
        qt_config->setValue(name + QStringLiteral("/use_global"), setting.UsingGlobal());
    }
    if (global || !setting.UsingGlobal()) {
        qt_config->setValue(name + QStringLiteral("/default"), value == setting.GetDefault());
        qt_config->setValue(name, value);
    }
}

void Config::ReadPlayerValue(std::size_t player_index) {
    const QString player_prefix = [this, player_index] {
        if (type == ConfigType::InputProfile) {
            return QString{};
        } else {
            return QStringLiteral("player_%1_").arg(player_index);
        }
    }();

    auto& player = Settings::values.players.GetValue()[player_index];
    if (IsCustomConfig()) {
        const auto profile_name =
            qt_config->value(QStringLiteral("%1profile_name").arg(player_prefix), QString{})
                .toString()
                .toStdString();
        if (profile_name.empty()) {
            // Use the global input config
            player = Settings::values.players.GetValue(true)[player_index];
            return;
        }
        player.profile_name = profile_name;
    }

    if (player_prefix.isEmpty() && Settings::IsConfiguringGlobal()) {
        const auto controller = static_cast<Settings::ControllerType>(
            qt_config
                ->value(QStringLiteral("%1type").arg(player_prefix),
                        static_cast<u8>(Settings::ControllerType::ProController))
                .toUInt());

        if (controller == Settings::ControllerType::LeftJoycon ||
            controller == Settings::ControllerType::RightJoycon) {
            player.controller_type = controller;
        }
    } else {
        player.connected =
            ReadSetting(QStringLiteral("%1connected").arg(player_prefix), player_index == 0)
                .toBool();

        player.controller_type = static_cast<Settings::ControllerType>(
            qt_config
                ->value(QStringLiteral("%1type").arg(player_prefix),
                        static_cast<u8>(Settings::ControllerType::ProController))
                .toUInt());

        player.vibration_enabled =
            qt_config->value(QStringLiteral("%1vibration_enabled").arg(player_prefix), true)
                .toBool();

        player.vibration_strength =
            qt_config->value(QStringLiteral("%1vibration_strength").arg(player_prefix), 100)
                .toInt();

        player.body_color_left = qt_config
                                     ->value(QStringLiteral("%1body_color_left").arg(player_prefix),
                                             Settings::JOYCON_BODY_NEON_BLUE)
                                     .toUInt();
        player.body_color_right =
            qt_config
                ->value(QStringLiteral("%1body_color_right").arg(player_prefix),
                        Settings::JOYCON_BODY_NEON_RED)
                .toUInt();
        player.button_color_left =
            qt_config
                ->value(QStringLiteral("%1button_color_left").arg(player_prefix),
                        Settings::JOYCON_BUTTONS_NEON_BLUE)
                .toUInt();
        player.button_color_right =
            qt_config
                ->value(QStringLiteral("%1button_color_right").arg(player_prefix),
                        Settings::JOYCON_BUTTONS_NEON_RED)
                .toUInt();
    }

    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        const std::string default_param = InputCommon::GenerateKeyboardParam(default_buttons[i]);
        auto& player_buttons = player.buttons[i];

        player_buttons = qt_config
                             ->value(QStringLiteral("%1").arg(player_prefix) +
                                         QString::fromUtf8(Settings::NativeButton::mapping[i]),
                                     QString::fromStdString(default_param))
                             .toString()
                             .toStdString();
        if (player_buttons.empty()) {
            player_buttons = default_param;
        }
    }

    for (int i = 0; i < Settings::NativeAnalog::NumAnalogs; ++i) {
        const std::string default_param = InputCommon::GenerateAnalogParamFromKeys(
            default_analogs[i][0], default_analogs[i][1], default_analogs[i][2],
            default_analogs[i][3], default_stick_mod[i], 0.5f);
        auto& player_analogs = player.analogs[i];

        player_analogs = qt_config
                             ->value(QStringLiteral("%1").arg(player_prefix) +
                                         QString::fromUtf8(Settings::NativeAnalog::mapping[i]),
                                     QString::fromStdString(default_param))
                             .toString()
                             .toStdString();
        if (player_analogs.empty()) {
            player_analogs = default_param;
        }
    }

    for (int i = 0; i < Settings::NativeMotion::NumMotions; ++i) {
        const std::string default_param = InputCommon::GenerateKeyboardParam(default_motions[i]);
        auto& player_motions = player.motions[i];

        player_motions = qt_config
                             ->value(QStringLiteral("%1").arg(player_prefix) +
                                         QString::fromUtf8(Settings::NativeMotion::mapping[i]),
                                     QString::fromStdString(default_param))
                             .toString()
                             .toStdString();
        if (player_motions.empty()) {
            player_motions = default_param;
        }
    }
}

void Config::ReadDebugValues() {
    ReadBasicSetting(Settings::values.debug_pad_enabled);

    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        const std::string default_param = InputCommon::GenerateKeyboardParam(default_buttons[i]);
        auto& debug_pad_buttons = Settings::values.debug_pad_buttons[i];

        debug_pad_buttons = qt_config
                                ->value(QStringLiteral("debug_pad_") +
                                            QString::fromUtf8(Settings::NativeButton::mapping[i]),
                                        QString::fromStdString(default_param))
                                .toString()
                                .toStdString();
        if (debug_pad_buttons.empty()) {
            debug_pad_buttons = default_param;
        }
    }

    for (int i = 0; i < Settings::NativeAnalog::NumAnalogs; ++i) {
        const std::string default_param = InputCommon::GenerateAnalogParamFromKeys(
            default_analogs[i][0], default_analogs[i][1], default_analogs[i][2],
            default_analogs[i][3], default_stick_mod[i], 0.5f);
        auto& debug_pad_analogs = Settings::values.debug_pad_analogs[i];

        debug_pad_analogs = qt_config
                                ->value(QStringLiteral("debug_pad_") +
                                            QString::fromUtf8(Settings::NativeAnalog::mapping[i]),
                                        QString::fromStdString(default_param))
                                .toString()
                                .toStdString();
        if (debug_pad_analogs.empty()) {
            debug_pad_analogs = default_param;
        }
    }
}

void Config::ReadKeyboardValues() {
    ReadBasicSetting(Settings::values.keyboard_enabled);
}

void Config::ReadMouseValues() {
    ReadBasicSetting(Settings::values.mouse_enabled);
}

void Config::ReadTouchscreenValues() {
    Settings::values.touchscreen.enabled =
        ReadSetting(QStringLiteral("touchscreen_enabled"), true).toBool();

    Settings::values.touchscreen.rotation_angle =
        ReadSetting(QStringLiteral("touchscreen_angle"), 0).toUInt();
    Settings::values.touchscreen.diameter_x =
        ReadSetting(QStringLiteral("touchscreen_diameter_x"), 15).toUInt();
    Settings::values.touchscreen.diameter_y =
        ReadSetting(QStringLiteral("touchscreen_diameter_y"), 15).toUInt();
}

void Config::ReadHidbusValues() {
    Settings::values.enable_ring_controller =
        ReadSetting(QStringLiteral("enable_ring_controller"), true).toBool();

    const std::string default_param = InputCommon::GenerateAnalogParamFromKeys(
        0, 0, default_ringcon_analogs[0], default_ringcon_analogs[1], 0, 0.05f);
    auto& ringcon_analogs = Settings::values.ringcon_analogs;

    ringcon_analogs =
        qt_config->value(QStringLiteral("ring_controller"), QString::fromStdString(default_param))
            .toString()
            .toStdString();
    if (ringcon_analogs.empty()) {
        ringcon_analogs = default_param;
    }
}

void Config::ReadIrCameraValues() {
    ReadBasicSetting(Settings::values.enable_ir_sensor);
    ReadBasicSetting(Settings::values.ir_sensor_device);
}

void Config::ReadAudioValues() {
    qt_config->beginGroup(QStringLiteral("Audio"));

    if (global) {
        ReadBasicSetting(Settings::values.sink_id);
        ReadBasicSetting(Settings::values.audio_output_device_id);
        ReadBasicSetting(Settings::values.audio_input_device_id);
    }
    ReadGlobalSetting(Settings::values.volume);

    qt_config->endGroup();
}

void Config::ReadControlValues() {
    qt_config->beginGroup(QStringLiteral("Controls"));

    Settings::values.players.SetGlobal(!IsCustomConfig());
    for (std::size_t p = 0; p < Settings::values.players.GetValue().size(); ++p) {
        ReadPlayerValue(p);
    }
    ReadGlobalSetting(Settings::values.use_docked_mode);

    // Disable docked mode if handheld is selected
    const auto controller_type = Settings::values.players.GetValue()[0].controller_type;
    if (controller_type == Settings::ControllerType::Handheld) {
        Settings::values.use_docked_mode.SetGlobal(!IsCustomConfig());
        Settings::values.use_docked_mode.SetValue(false);
    }

    ReadGlobalSetting(Settings::values.vibration_enabled);
    ReadGlobalSetting(Settings::values.enable_accurate_vibrations);
    ReadGlobalSetting(Settings::values.motion_enabled);
    if (IsCustomConfig()) {
        qt_config->endGroup();
        return;
    }
    ReadDebugValues();
    ReadKeyboardValues();
    ReadMouseValues();
    ReadTouchscreenValues();
    ReadMotionTouchValues();
    ReadHidbusValues();
    ReadIrCameraValues();

#ifdef _WIN32
    ReadBasicSetting(Settings::values.enable_raw_input);
#else
    Settings::values.enable_raw_input = false;
#endif
    ReadBasicSetting(Settings::values.emulate_analog_keyboard);
    Settings::values.mouse_panning = false;
    ReadBasicSetting(Settings::values.mouse_panning_sensitivity);
    ReadBasicSetting(Settings::values.enable_joycon_driver);
    ReadBasicSetting(Settings::values.enable_procon_driver);
    ReadBasicSetting(Settings::values.random_amiibo_id);

    ReadBasicSetting(Settings::values.tas_enable);
    ReadBasicSetting(Settings::values.tas_loop);
    ReadBasicSetting(Settings::values.pause_tas_on_load);

    ReadBasicSetting(Settings::values.controller_navigation);

    qt_config->endGroup();
}

void Config::ReadMotionTouchValues() {
    int num_touch_from_button_maps =
        qt_config->beginReadArray(QStringLiteral("touch_from_button_maps"));

    if (num_touch_from_button_maps > 0) {
        const auto append_touch_from_button_map = [this] {
            Settings::TouchFromButtonMap map;
            map.name = ReadSetting(QStringLiteral("name"), QStringLiteral("default"))
                           .toString()
                           .toStdString();
            const int num_touch_maps = qt_config->beginReadArray(QStringLiteral("entries"));
            map.buttons.reserve(num_touch_maps);
            for (int i = 0; i < num_touch_maps; i++) {
                qt_config->setArrayIndex(i);
                std::string touch_mapping =
                    ReadSetting(QStringLiteral("bind")).toString().toStdString();
                map.buttons.emplace_back(std::move(touch_mapping));
            }
            qt_config->endArray(); // entries
            Settings::values.touch_from_button_maps.emplace_back(std::move(map));
        };

        for (int i = 0; i < num_touch_from_button_maps; ++i) {
            qt_config->setArrayIndex(i);
            append_touch_from_button_map();
        }
    } else {
        Settings::values.touch_from_button_maps.emplace_back(
            Settings::TouchFromButtonMap{"default", {}});
        num_touch_from_button_maps = 1;
    }
    qt_config->endArray();

    ReadBasicSetting(Settings::values.touch_device);
    ReadBasicSetting(Settings::values.touch_from_button_map_index);
    Settings::values.touch_from_button_map_index = std::clamp(
        Settings::values.touch_from_button_map_index.GetValue(), 0, num_touch_from_button_maps - 1);
    ReadBasicSetting(Settings::values.udp_input_servers);
    ReadBasicSetting(Settings::values.enable_udp_controller);
}

void Config::ReadCoreValues() {
    qt_config->beginGroup(QStringLiteral("Core"));

    ReadGlobalSetting(Settings::values.use_multi_core);
    ReadGlobalSetting(Settings::values.use_unsafe_extended_memory_layout);

    qt_config->endGroup();
}

void Config::ReadDataStorageValues() {
    qt_config->beginGroup(QStringLiteral("Data Storage"));

    ReadBasicSetting(Settings::values.use_virtual_sd);
    FS::SetYuzuPath(
        FS::YuzuPath::NANDDir,
        qt_config
            ->value(QStringLiteral("nand_directory"),
                    QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::NANDDir)))
            .toString()
            .toStdString());
    FS::SetYuzuPath(
        FS::YuzuPath::SDMCDir,
        qt_config
            ->value(QStringLiteral("sdmc_directory"),
                    QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::SDMCDir)))
            .toString()
            .toStdString());
    FS::SetYuzuPath(
        FS::YuzuPath::LoadDir,
        qt_config
            ->value(QStringLiteral("load_directory"),
                    QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::LoadDir)))
            .toString()
            .toStdString());
    FS::SetYuzuPath(
        FS::YuzuPath::DumpDir,
        qt_config
            ->value(QStringLiteral("dump_directory"),
                    QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::DumpDir)))
            .toString()
            .toStdString());
    FS::SetYuzuPath(FS::YuzuPath::TASDir,
                    qt_config
                        ->value(QStringLiteral("tas_directory"),
                                QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::TASDir)))
                        .toString()
                        .toStdString());

    ReadBasicSetting(Settings::values.gamecard_inserted);
    ReadBasicSetting(Settings::values.gamecard_current_game);
    ReadBasicSetting(Settings::values.gamecard_path);

    qt_config->endGroup();
}

void Config::ReadDebuggingValues() {
    qt_config->beginGroup(QStringLiteral("Debugging"));

    // Intentionally not using the QT default setting as this is intended to be changed in the ini
    Settings::values.record_frame_times =
        qt_config->value(QStringLiteral("record_frame_times"), false).toBool();

    ReadBasicSetting(Settings::values.use_gdbstub);
    ReadBasicSetting(Settings::values.gdbstub_port);
    ReadBasicSetting(Settings::values.program_args);
    ReadBasicSetting(Settings::values.dump_exefs);
    ReadBasicSetting(Settings::values.dump_nso);
    ReadBasicSetting(Settings::values.enable_fs_access_log);
    ReadBasicSetting(Settings::values.reporting_services);
    ReadBasicSetting(Settings::values.quest_flag);
    ReadBasicSetting(Settings::values.disable_macro_jit);
    ReadBasicSetting(Settings::values.disable_macro_hle);
    ReadBasicSetting(Settings::values.extended_logging);
    ReadBasicSetting(Settings::values.use_debug_asserts);
    ReadBasicSetting(Settings::values.use_auto_stub);
    ReadBasicSetting(Settings::values.enable_all_controllers);
    ReadBasicSetting(Settings::values.create_crash_dumps);
    ReadBasicSetting(Settings::values.perform_vulkan_check);

    qt_config->endGroup();
}

void Config::ReadServiceValues() {
    qt_config->beginGroup(QStringLiteral("Services"));
    ReadBasicSetting(Settings::values.network_interface);
    qt_config->endGroup();
}

void Config::ReadDisabledAddOnValues() {
    const auto size = qt_config->beginReadArray(QStringLiteral("DisabledAddOns"));

    for (int i = 0; i < size; ++i) {
        qt_config->setArrayIndex(i);
        const auto title_id = ReadSetting(QStringLiteral("title_id"), 0).toULongLong();
        std::vector<std::string> out;
        const auto d_size = qt_config->beginReadArray(QStringLiteral("disabled"));
        for (int j = 0; j < d_size; ++j) {
            qt_config->setArrayIndex(j);
            out.push_back(ReadSetting(QStringLiteral("d"), QString{}).toString().toStdString());
        }
        qt_config->endArray();
        Settings::values.disabled_addons.insert_or_assign(title_id, out);
    }

    qt_config->endArray();
}

void Config::ReadMiscellaneousValues() {
    qt_config->beginGroup(QStringLiteral("Miscellaneous"));

    ReadBasicSetting(Settings::values.log_filter);
    ReadBasicSetting(Settings::values.use_dev_keys);

    qt_config->endGroup();
}

void Config::ReadPathValues() {
    qt_config->beginGroup(QStringLiteral("Paths"));

    UISettings::values.roms_path = ReadSetting(QStringLiteral("romsPath")).toString();
    UISettings::values.symbols_path = ReadSetting(QStringLiteral("symbolsPath")).toString();
    UISettings::values.game_dir_deprecated =
        ReadSetting(QStringLiteral("gameListRootDir"), QStringLiteral(".")).toString();
    UISettings::values.game_dir_deprecated_deepscan =
        ReadSetting(QStringLiteral("gameListDeepScan"), false).toBool();
    const int gamedirs_size = qt_config->beginReadArray(QStringLiteral("gamedirs"));
    for (int i = 0; i < gamedirs_size; ++i) {
        qt_config->setArrayIndex(i);
        UISettings::GameDir game_dir;
        game_dir.path = ReadSetting(QStringLiteral("path")).toString();
        game_dir.deep_scan = ReadSetting(QStringLiteral("deep_scan"), false).toBool();
        game_dir.expanded = ReadSetting(QStringLiteral("expanded"), true).toBool();
        UISettings::values.game_dirs.append(game_dir);
    }
    qt_config->endArray();
    // create NAND and SD card directories if empty, these are not removable through the UI,
    // also carries over old game list settings if present
    if (UISettings::values.game_dirs.isEmpty()) {
        UISettings::GameDir game_dir;
        game_dir.path = QStringLiteral("SDMC");
        game_dir.expanded = true;
        UISettings::values.game_dirs.append(game_dir);
        game_dir.path = QStringLiteral("UserNAND");
        UISettings::values.game_dirs.append(game_dir);
        game_dir.path = QStringLiteral("SysNAND");
        UISettings::values.game_dirs.append(game_dir);
        if (UISettings::values.game_dir_deprecated != QStringLiteral(".")) {
            game_dir.path = UISettings::values.game_dir_deprecated;
            game_dir.deep_scan = UISettings::values.game_dir_deprecated_deepscan;
            UISettings::values.game_dirs.append(game_dir);
        }
    }
    UISettings::values.recent_files = ReadSetting(QStringLiteral("recentFiles")).toStringList();
    UISettings::values.language = ReadSetting(QStringLiteral("language"), QString{}).toString();

    qt_config->endGroup();
}

void Config::ReadCpuValues() {
    qt_config->beginGroup(QStringLiteral("Cpu"));

    ReadBasicSetting(Settings::values.cpu_accuracy_first_time);
    if (Settings::values.cpu_accuracy_first_time) {
        Settings::values.cpu_accuracy.SetValue(Settings::values.cpu_accuracy.GetDefault());
        Settings::values.cpu_accuracy_first_time.SetValue(false);
    } else {
        ReadGlobalSetting(Settings::values.cpu_accuracy);
    }

    ReadGlobalSetting(Settings::values.cpuopt_unsafe_unfuse_fma);
    ReadGlobalSetting(Settings::values.cpuopt_unsafe_reduce_fp_error);
    ReadGlobalSetting(Settings::values.cpuopt_unsafe_ignore_standard_fpcr);
    ReadGlobalSetting(Settings::values.cpuopt_unsafe_inaccurate_nan);
    ReadGlobalSetting(Settings::values.cpuopt_unsafe_fastmem_check);
    ReadGlobalSetting(Settings::values.cpuopt_unsafe_ignore_global_monitor);

    if (global) {
        ReadBasicSetting(Settings::values.cpu_debug_mode);
        ReadBasicSetting(Settings::values.cpuopt_page_tables);
        ReadBasicSetting(Settings::values.cpuopt_block_linking);
        ReadBasicSetting(Settings::values.cpuopt_return_stack_buffer);
        ReadBasicSetting(Settings::values.cpuopt_fast_dispatcher);
        ReadBasicSetting(Settings::values.cpuopt_context_elimination);
        ReadBasicSetting(Settings::values.cpuopt_const_prop);
        ReadBasicSetting(Settings::values.cpuopt_misc_ir);
        ReadBasicSetting(Settings::values.cpuopt_reduce_misalign_checks);
        ReadBasicSetting(Settings::values.cpuopt_fastmem);
        ReadBasicSetting(Settings::values.cpuopt_fastmem_exclusives);
        ReadBasicSetting(Settings::values.cpuopt_recompile_exclusives);
        ReadBasicSetting(Settings::values.cpuopt_ignore_memory_aborts);
    }

    qt_config->endGroup();
}

void Config::ReadRendererValues() {
    qt_config->beginGroup(QStringLiteral("Renderer"));

    ReadGlobalSetting(Settings::values.renderer_backend);
    ReadGlobalSetting(Settings::values.async_presentation);
    ReadGlobalSetting(Settings::values.renderer_force_max_clock);
    ReadGlobalSetting(Settings::values.vulkan_device);
    ReadGlobalSetting(Settings::values.fullscreen_mode);
    ReadGlobalSetting(Settings::values.aspect_ratio);
    ReadGlobalSetting(Settings::values.resolution_setup);
    ReadGlobalSetting(Settings::values.scaling_filter);
    ReadGlobalSetting(Settings::values.fsr_sharpening_slider);
    ReadGlobalSetting(Settings::values.anti_aliasing);
    ReadGlobalSetting(Settings::values.max_anisotropy);
    ReadGlobalSetting(Settings::values.speed_limit);
    ReadGlobalSetting(Settings::values.use_disk_shader_cache);
    ReadGlobalSetting(Settings::values.gpu_accuracy);
    ReadGlobalSetting(Settings::values.use_asynchronous_gpu_emulation);
    ReadGlobalSetting(Settings::values.nvdec_emulation);
    ReadGlobalSetting(Settings::values.accelerate_astc);
    ReadGlobalSetting(Settings::values.async_astc);
    ReadGlobalSetting(Settings::values.use_reactive_flushing);
    ReadGlobalSetting(Settings::values.shader_backend);
    ReadGlobalSetting(Settings::values.use_asynchronous_shaders);
    ReadGlobalSetting(Settings::values.use_fast_gpu_time);
    ReadGlobalSetting(Settings::values.use_vulkan_driver_pipeline_cache);
    ReadGlobalSetting(Settings::values.enable_compute_pipelines);
    ReadGlobalSetting(Settings::values.bg_red);
    ReadGlobalSetting(Settings::values.bg_green);
    ReadGlobalSetting(Settings::values.bg_blue);

    if (global) {
        Settings::values.vsync_mode.SetValue(static_cast<Settings::VSyncMode>(
            ReadSetting(QString::fromStdString(Settings::values.vsync_mode.GetLabel()),
                        static_cast<u32>(Settings::values.vsync_mode.GetDefault()))
                .value<u32>()));
        ReadBasicSetting(Settings::values.renderer_debug);
        ReadBasicSetting(Settings::values.renderer_shader_feedback);
        ReadBasicSetting(Settings::values.enable_nsight_aftermath);
        ReadBasicSetting(Settings::values.disable_shader_loop_safety_checks);
    }

    qt_config->endGroup();
}

void Config::ReadScreenshotValues() {
    qt_config->beginGroup(QStringLiteral("Screenshots"));

    UISettings::values.enable_screenshot_save_as =
        ReadSetting(QStringLiteral("enable_screenshot_save_as"), true).toBool();
    FS::SetYuzuPath(
        FS::YuzuPath::ScreenshotsDir,
        qt_config
            ->value(QStringLiteral("screenshot_path"),
                    QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::ScreenshotsDir)))
            .toString()
            .toStdString());

    qt_config->endGroup();
}

void Config::ReadShortcutValues() {
    qt_config->beginGroup(QStringLiteral("Shortcuts"));

    for (const auto& [name, group, shortcut] : default_hotkeys) {
        qt_config->beginGroup(group);
        qt_config->beginGroup(name);
        // No longer using ReadSetting for shortcut.second as it inaccurately returns a value of 1
        // for WidgetWithChildrenShortcut which is a value of 3. Needed to fix shortcuts the open
        // a file dialog in windowed mode
        UISettings::values.shortcuts.push_back(
            {name,
             group,
             {ReadSetting(QStringLiteral("KeySeq"), shortcut.keyseq).toString(),
              ReadSetting(QStringLiteral("Controller_KeySeq"), shortcut.controller_keyseq)
                  .toString(),
              shortcut.context, ReadSetting(QStringLiteral("Repeat"), shortcut.repeat).toBool()}});
        qt_config->endGroup();
        qt_config->endGroup();
    }

    qt_config->endGroup();
}

void Config::ReadSystemValues() {
    qt_config->beginGroup(QStringLiteral("System"));

    ReadGlobalSetting(Settings::values.language_index);

    ReadGlobalSetting(Settings::values.region_index);

    ReadGlobalSetting(Settings::values.time_zone_index);

    bool rng_seed_enabled;
    ReadSettingGlobal(rng_seed_enabled, QStringLiteral("rng_seed_enabled"), false);
    bool rng_seed_global =
        global || qt_config->value(QStringLiteral("rng_seed/use_global"), true).toBool();
    Settings::values.rng_seed.SetGlobal(rng_seed_global);
    if (global || !rng_seed_global) {
        if (rng_seed_enabled) {
            Settings::values.rng_seed.SetValue(ReadSetting(QStringLiteral("rng_seed"), 0).toUInt());
        } else {
            Settings::values.rng_seed.SetValue(std::nullopt);
        }
    }

    if (global) {
        ReadBasicSetting(Settings::values.current_user);
        Settings::values.current_user = std::clamp<int>(Settings::values.current_user.GetValue(), 0,
                                                        Service::Account::MAX_USERS - 1);

        const auto custom_rtc_enabled =
            ReadSetting(QStringLiteral("custom_rtc_enabled"), false).toBool();
        if (custom_rtc_enabled) {
            Settings::values.custom_rtc = ReadSetting(QStringLiteral("custom_rtc"), 0).toLongLong();
        } else {
            Settings::values.custom_rtc = std::nullopt;
        }
        ReadBasicSetting(Settings::values.device_name);
    }

    ReadGlobalSetting(Settings::values.sound_index);

    qt_config->endGroup();
}

void Config::ReadUIValues() {
    qt_config->beginGroup(QStringLiteral("UI"));

    UISettings::values.theme =
        ReadSetting(
            QStringLiteral("theme"),
            QString::fromUtf8(UISettings::themes[static_cast<size_t>(default_theme)].second))
            .toString();
    ReadBasicSetting(UISettings::values.enable_discord_presence);
    ReadBasicSetting(UISettings::values.select_user_on_boot);

    ReadUIGamelistValues();
    ReadUILayoutValues();
    ReadPathValues();
    ReadScreenshotValues();
    ReadShortcutValues();
    ReadMultiplayerValues();

    ReadBasicSetting(UISettings::values.single_window_mode);
    ReadBasicSetting(UISettings::values.fullscreen);
    ReadBasicSetting(UISettings::values.display_titlebar);
    ReadBasicSetting(UISettings::values.show_filter_bar);
    ReadBasicSetting(UISettings::values.show_status_bar);
    ReadBasicSetting(UISettings::values.confirm_before_closing);
    ReadBasicSetting(UISettings::values.first_start);
    ReadBasicSetting(UISettings::values.callout_flags);
    ReadBasicSetting(UISettings::values.show_console);
    ReadBasicSetting(UISettings::values.pause_when_in_background);
    ReadBasicSetting(UISettings::values.mute_when_in_background);
    ReadBasicSetting(UISettings::values.hide_mouse);
    ReadBasicSetting(UISettings::values.disable_web_applet);

    qt_config->endGroup();
}

void Config::ReadUIGamelistValues() {
    qt_config->beginGroup(QStringLiteral("UIGameList"));

    ReadBasicSetting(UISettings::values.show_add_ons);
    ReadBasicSetting(UISettings::values.show_compat);
    ReadBasicSetting(UISettings::values.show_size);
    ReadBasicSetting(UISettings::values.show_types);
    ReadBasicSetting(UISettings::values.game_icon_size);
    ReadBasicSetting(UISettings::values.folder_icon_size);
    ReadBasicSetting(UISettings::values.row_1_text_id);
    ReadBasicSetting(UISettings::values.row_2_text_id);
    ReadBasicSetting(UISettings::values.cache_game_list);
    ReadBasicSetting(UISettings::values.favorites_expanded);
    const int favorites_size = qt_config->beginReadArray(QStringLiteral("favorites"));
    for (int i = 0; i < favorites_size; i++) {
        qt_config->setArrayIndex(i);
        UISettings::values.favorited_ids.append(
            ReadSetting(QStringLiteral("program_id")).toULongLong());
    }
    qt_config->endArray();

    qt_config->endGroup();
}

void Config::ReadUILayoutValues() {
    qt_config->beginGroup(QStringLiteral("UILayout"));

    UISettings::values.geometry = ReadSetting(QStringLiteral("geometry")).toByteArray();
    UISettings::values.state = ReadSetting(QStringLiteral("state")).toByteArray();
    UISettings::values.renderwindow_geometry =
        ReadSetting(QStringLiteral("geometryRenderWindow")).toByteArray();
    UISettings::values.gamelist_header_state =
        ReadSetting(QStringLiteral("gameListHeaderState")).toByteArray();
    UISettings::values.microprofile_geometry =
        ReadSetting(QStringLiteral("microProfileDialogGeometry")).toByteArray();
    ReadBasicSetting(UISettings::values.microprofile_visible);

    qt_config->endGroup();
}

void Config::ReadWebServiceValues() {
    qt_config->beginGroup(QStringLiteral("WebService"));

    ReadBasicSetting(Settings::values.enable_telemetry);
    ReadBasicSetting(Settings::values.web_api_url);
    ReadBasicSetting(Settings::values.yuzu_username);
    ReadBasicSetting(Settings::values.yuzu_token);

    qt_config->endGroup();
}

void Config::ReadMultiplayerValues() {
    qt_config->beginGroup(QStringLiteral("Multiplayer"));

    ReadBasicSetting(UISettings::values.multiplayer_nickname);
    ReadBasicSetting(UISettings::values.multiplayer_ip);
    ReadBasicSetting(UISettings::values.multiplayer_port);
    ReadBasicSetting(UISettings::values.multiplayer_room_nickname);
    ReadBasicSetting(UISettings::values.multiplayer_room_name);
    ReadBasicSetting(UISettings::values.multiplayer_room_port);
    ReadBasicSetting(UISettings::values.multiplayer_host_type);
    ReadBasicSetting(UISettings::values.multiplayer_port);
    ReadBasicSetting(UISettings::values.multiplayer_max_player);
    ReadBasicSetting(UISettings::values.multiplayer_game_id);
    ReadBasicSetting(UISettings::values.multiplayer_room_description);

    // Read ban list back
    int size = qt_config->beginReadArray(QStringLiteral("username_ban_list"));
    UISettings::values.multiplayer_ban_list.first.resize(size);
    for (int i = 0; i < size; ++i) {
        qt_config->setArrayIndex(i);
        UISettings::values.multiplayer_ban_list.first[i] =
            ReadSetting(QStringLiteral("username")).toString().toStdString();
    }
    qt_config->endArray();
    size = qt_config->beginReadArray(QStringLiteral("ip_ban_list"));
    UISettings::values.multiplayer_ban_list.second.resize(size);
    for (int i = 0; i < size; ++i) {
        qt_config->setArrayIndex(i);
        UISettings::values.multiplayer_ban_list.second[i] =
            ReadSetting(QStringLiteral("ip")).toString().toStdString();
    }
    qt_config->endArray();

    qt_config->endGroup();
}

void Config::ReadValues() {
    if (global) {
        ReadDataStorageValues();
        ReadDebuggingValues();
        ReadDisabledAddOnValues();
        ReadServiceValues();
        ReadUIValues();
        ReadWebServiceValues();
        ReadMiscellaneousValues();
    }
    ReadControlValues();
    ReadCoreValues();
    ReadCpuValues();
    ReadRendererValues();
    ReadAudioValues();
    ReadSystemValues();
}

void Config::SavePlayerValue(std::size_t player_index) {
    const QString player_prefix = [this, player_index] {
        if (type == ConfigType::InputProfile) {
            return QString{};
        } else {
            return QStringLiteral("player_%1_").arg(player_index);
        }
    }();

    const auto& player = Settings::values.players.GetValue()[player_index];
    if (IsCustomConfig()) {
        if (player.profile_name.empty()) {
            // No custom profile selected
            return;
        }
        WriteSetting(QStringLiteral("%1profile_name").arg(player_prefix),
                     QString::fromStdString(player.profile_name), QString{});
    }

    WriteSetting(QStringLiteral("%1type").arg(player_prefix),
                 static_cast<u8>(player.controller_type),
                 static_cast<u8>(Settings::ControllerType::ProController));

    if (!player_prefix.isEmpty() || !Settings::IsConfiguringGlobal()) {
        WriteSetting(QStringLiteral("%1connected").arg(player_prefix), player.connected,
                     player_index == 0);
        WriteSetting(QStringLiteral("%1vibration_enabled").arg(player_prefix),
                     player.vibration_enabled, true);
        WriteSetting(QStringLiteral("%1vibration_strength").arg(player_prefix),
                     player.vibration_strength, 100);
        WriteSetting(QStringLiteral("%1body_color_left").arg(player_prefix), player.body_color_left,
                     Settings::JOYCON_BODY_NEON_BLUE);
        WriteSetting(QStringLiteral("%1body_color_right").arg(player_prefix),
                     player.body_color_right, Settings::JOYCON_BODY_NEON_RED);
        WriteSetting(QStringLiteral("%1button_color_left").arg(player_prefix),
                     player.button_color_left, Settings::JOYCON_BUTTONS_NEON_BLUE);
        WriteSetting(QStringLiteral("%1button_color_right").arg(player_prefix),
                     player.button_color_right, Settings::JOYCON_BUTTONS_NEON_RED);
    }

    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        const std::string default_param = InputCommon::GenerateKeyboardParam(default_buttons[i]);
        WriteSetting(QStringLiteral("%1").arg(player_prefix) +
                         QString::fromStdString(Settings::NativeButton::mapping[i]),
                     QString::fromStdString(player.buttons[i]),
                     QString::fromStdString(default_param));
    }
    for (int i = 0; i < Settings::NativeAnalog::NumAnalogs; ++i) {
        const std::string default_param = InputCommon::GenerateAnalogParamFromKeys(
            default_analogs[i][0], default_analogs[i][1], default_analogs[i][2],
            default_analogs[i][3], default_stick_mod[i], 0.5f);
        WriteSetting(QStringLiteral("%1").arg(player_prefix) +
                         QString::fromStdString(Settings::NativeAnalog::mapping[i]),
                     QString::fromStdString(player.analogs[i]),
                     QString::fromStdString(default_param));
    }
    for (int i = 0; i < Settings::NativeMotion::NumMotions; ++i) {
        const std::string default_param = InputCommon::GenerateKeyboardParam(default_motions[i]);
        WriteSetting(QStringLiteral("%1").arg(player_prefix) +
                         QString::fromStdString(Settings::NativeMotion::mapping[i]),
                     QString::fromStdString(player.motions[i]),
                     QString::fromStdString(default_param));
    }
}

void Config::SaveDebugValues() {
    WriteBasicSetting(Settings::values.debug_pad_enabled);
    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        const std::string default_param = InputCommon::GenerateKeyboardParam(default_buttons[i]);
        WriteSetting(QStringLiteral("debug_pad_") +
                         QString::fromStdString(Settings::NativeButton::mapping[i]),
                     QString::fromStdString(Settings::values.debug_pad_buttons[i]),
                     QString::fromStdString(default_param));
    }
    for (int i = 0; i < Settings::NativeAnalog::NumAnalogs; ++i) {
        const std::string default_param = InputCommon::GenerateAnalogParamFromKeys(
            default_analogs[i][0], default_analogs[i][1], default_analogs[i][2],
            default_analogs[i][3], default_stick_mod[i], 0.5f);
        WriteSetting(QStringLiteral("debug_pad_") +
                         QString::fromStdString(Settings::NativeAnalog::mapping[i]),
                     QString::fromStdString(Settings::values.debug_pad_analogs[i]),
                     QString::fromStdString(default_param));
    }
}

void Config::SaveMouseValues() {
    WriteBasicSetting(Settings::values.mouse_enabled);
}

void Config::SaveTouchscreenValues() {
    const auto& touchscreen = Settings::values.touchscreen;

    WriteSetting(QStringLiteral("touchscreen_enabled"), touchscreen.enabled, true);

    WriteSetting(QStringLiteral("touchscreen_angle"), touchscreen.rotation_angle, 0);
    WriteSetting(QStringLiteral("touchscreen_diameter_x"), touchscreen.diameter_x, 15);
    WriteSetting(QStringLiteral("touchscreen_diameter_y"), touchscreen.diameter_y, 15);
}

void Config::SaveMotionTouchValues() {
    WriteBasicSetting(Settings::values.touch_device);
    WriteBasicSetting(Settings::values.touch_from_button_map_index);
    WriteBasicSetting(Settings::values.udp_input_servers);
    WriteBasicSetting(Settings::values.enable_udp_controller);

    qt_config->beginWriteArray(QStringLiteral("touch_from_button_maps"));
    for (std::size_t p = 0; p < Settings::values.touch_from_button_maps.size(); ++p) {
        qt_config->setArrayIndex(static_cast<int>(p));
        WriteSetting(QStringLiteral("name"),
                     QString::fromStdString(Settings::values.touch_from_button_maps[p].name),
                     QStringLiteral("default"));
        qt_config->beginWriteArray(QStringLiteral("entries"));
        for (std::size_t q = 0; q < Settings::values.touch_from_button_maps[p].buttons.size();
             ++q) {
            qt_config->setArrayIndex(static_cast<int>(q));
            WriteSetting(
                QStringLiteral("bind"),
                QString::fromStdString(Settings::values.touch_from_button_maps[p].buttons[q]));
        }
        qt_config->endArray();
    }
    qt_config->endArray();
}

void Config::SaveHidbusValues() {
    WriteBasicSetting(Settings::values.enable_ring_controller);

    const std::string default_param = InputCommon::GenerateAnalogParamFromKeys(
        0, 0, default_ringcon_analogs[0], default_ringcon_analogs[1], 0, 0.05f);
    WriteSetting(QStringLiteral("ring_controller"),
                 QString::fromStdString(Settings::values.ringcon_analogs),
                 QString::fromStdString(default_param));
}

void Config::SaveIrCameraValues() {
    WriteBasicSetting(Settings::values.enable_ir_sensor);
    WriteBasicSetting(Settings::values.ir_sensor_device);
}

void Config::SaveValues() {
    if (global) {
        SaveDataStorageValues();
        SaveDebuggingValues();
        SaveDisabledAddOnValues();
        SaveNetworkValues();
        SaveUIValues();
        SaveWebServiceValues();
        SaveMiscellaneousValues();
    }
    SaveControlValues();
    SaveCoreValues();
    SaveCpuValues();
    SaveRendererValues();
    SaveAudioValues();
    SaveSystemValues();
    qt_config->sync();
}

void Config::SaveAudioValues() {
    qt_config->beginGroup(QStringLiteral("Audio"));

    if (global) {
        WriteBasicSetting(Settings::values.sink_id);
        WriteBasicSetting(Settings::values.audio_output_device_id);
        WriteBasicSetting(Settings::values.audio_input_device_id);
    }
    WriteGlobalSetting(Settings::values.volume);

    qt_config->endGroup();
}

void Config::SaveControlValues() {
    qt_config->beginGroup(QStringLiteral("Controls"));

    Settings::values.players.SetGlobal(!IsCustomConfig());
    for (std::size_t p = 0; p < Settings::values.players.GetValue().size(); ++p) {
        SavePlayerValue(p);
    }
    if (IsCustomConfig()) {
        qt_config->endGroup();
        return;
    }
    SaveDebugValues();
    SaveMouseValues();
    SaveTouchscreenValues();
    SaveMotionTouchValues();
    SaveHidbusValues();
    SaveIrCameraValues();

    WriteGlobalSetting(Settings::values.use_docked_mode);
    WriteGlobalSetting(Settings::values.vibration_enabled);
    WriteGlobalSetting(Settings::values.enable_accurate_vibrations);
    WriteGlobalSetting(Settings::values.motion_enabled);
    WriteBasicSetting(Settings::values.enable_raw_input);
    WriteBasicSetting(Settings::values.enable_joycon_driver);
    WriteBasicSetting(Settings::values.enable_procon_driver);
    WriteBasicSetting(Settings::values.random_amiibo_id);
    WriteBasicSetting(Settings::values.keyboard_enabled);
    WriteBasicSetting(Settings::values.emulate_analog_keyboard);
    WriteBasicSetting(Settings::values.mouse_panning_sensitivity);
    WriteBasicSetting(Settings::values.controller_navigation);

    WriteBasicSetting(Settings::values.tas_enable);
    WriteBasicSetting(Settings::values.tas_loop);
    WriteBasicSetting(Settings::values.pause_tas_on_load);

    qt_config->endGroup();
}

void Config::SaveCoreValues() {
    qt_config->beginGroup(QStringLiteral("Core"));

    WriteGlobalSetting(Settings::values.use_multi_core);
    WriteGlobalSetting(Settings::values.use_unsafe_extended_memory_layout);

    qt_config->endGroup();
}

void Config::SaveDataStorageValues() {
    qt_config->beginGroup(QStringLiteral("Data Storage"));

    WriteBasicSetting(Settings::values.use_virtual_sd);
    WriteSetting(QStringLiteral("nand_directory"),
                 QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::NANDDir)),
                 QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::NANDDir)));
    WriteSetting(QStringLiteral("sdmc_directory"),
                 QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::SDMCDir)),
                 QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::SDMCDir)));
    WriteSetting(QStringLiteral("load_directory"),
                 QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::LoadDir)),
                 QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::LoadDir)));
    WriteSetting(QStringLiteral("dump_directory"),
                 QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::DumpDir)),
                 QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::DumpDir)));
    WriteSetting(QStringLiteral("tas_directory"),
                 QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::TASDir)),
                 QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::TASDir)));

    WriteBasicSetting(Settings::values.gamecard_inserted);
    WriteBasicSetting(Settings::values.gamecard_current_game);
    WriteBasicSetting(Settings::values.gamecard_path);

    qt_config->endGroup();
}

void Config::SaveDebuggingValues() {
    qt_config->beginGroup(QStringLiteral("Debugging"));

    // Intentionally not using the QT default setting as this is intended to be changed in the ini
    qt_config->setValue(QStringLiteral("record_frame_times"), Settings::values.record_frame_times);
    WriteBasicSetting(Settings::values.use_gdbstub);
    WriteBasicSetting(Settings::values.gdbstub_port);
    WriteBasicSetting(Settings::values.program_args);
    WriteBasicSetting(Settings::values.dump_exefs);
    WriteBasicSetting(Settings::values.dump_nso);
    WriteBasicSetting(Settings::values.enable_fs_access_log);
    WriteBasicSetting(Settings::values.quest_flag);
    WriteBasicSetting(Settings::values.use_debug_asserts);
    WriteBasicSetting(Settings::values.disable_macro_jit);
    WriteBasicSetting(Settings::values.disable_macro_hle);
    WriteBasicSetting(Settings::values.enable_all_controllers);
    WriteBasicSetting(Settings::values.create_crash_dumps);
    WriteBasicSetting(Settings::values.perform_vulkan_check);

    qt_config->endGroup();
}

void Config::SaveNetworkValues() {
    qt_config->beginGroup(QStringLiteral("Services"));

    WriteBasicSetting(Settings::values.network_interface);

    qt_config->endGroup();
}

void Config::SaveDisabledAddOnValues() {
    qt_config->beginWriteArray(QStringLiteral("DisabledAddOns"));

    int i = 0;
    for (const auto& elem : Settings::values.disabled_addons) {
        qt_config->setArrayIndex(i);
        WriteSetting(QStringLiteral("title_id"), QVariant::fromValue<u64>(elem.first), 0);
        qt_config->beginWriteArray(QStringLiteral("disabled"));
        for (std::size_t j = 0; j < elem.second.size(); ++j) {
            qt_config->setArrayIndex(static_cast<int>(j));
            WriteSetting(QStringLiteral("d"), QString::fromStdString(elem.second[j]), QString{});
        }
        qt_config->endArray();
        ++i;
    }

    qt_config->endArray();
}

void Config::SaveMiscellaneousValues() {
    qt_config->beginGroup(QStringLiteral("Miscellaneous"));

    WriteBasicSetting(Settings::values.log_filter);
    WriteBasicSetting(Settings::values.use_dev_keys);

    qt_config->endGroup();
}

void Config::SavePathValues() {
    qt_config->beginGroup(QStringLiteral("Paths"));

    WriteSetting(QStringLiteral("romsPath"), UISettings::values.roms_path);
    WriteSetting(QStringLiteral("symbolsPath"), UISettings::values.symbols_path);
    qt_config->beginWriteArray(QStringLiteral("gamedirs"));
    for (int i = 0; i < UISettings::values.game_dirs.size(); ++i) {
        qt_config->setArrayIndex(i);
        const auto& game_dir = UISettings::values.game_dirs[i];
        WriteSetting(QStringLiteral("path"), game_dir.path);
        WriteSetting(QStringLiteral("deep_scan"), game_dir.deep_scan, false);
        WriteSetting(QStringLiteral("expanded"), game_dir.expanded, true);
    }
    qt_config->endArray();
    WriteSetting(QStringLiteral("recentFiles"), UISettings::values.recent_files);
    WriteSetting(QStringLiteral("language"), UISettings::values.language, QString{});

    qt_config->endGroup();
}

void Config::SaveCpuValues() {
    qt_config->beginGroup(QStringLiteral("Cpu"));

    WriteBasicSetting(Settings::values.cpu_accuracy_first_time);
    WriteSetting(QStringLiteral("cpu_accuracy"),
                 static_cast<u32>(Settings::values.cpu_accuracy.GetValue(global)),
                 static_cast<u32>(Settings::values.cpu_accuracy.GetDefault()),
                 Settings::values.cpu_accuracy.UsingGlobal());

    WriteGlobalSetting(Settings::values.cpuopt_unsafe_unfuse_fma);
    WriteGlobalSetting(Settings::values.cpuopt_unsafe_reduce_fp_error);
    WriteGlobalSetting(Settings::values.cpuopt_unsafe_ignore_standard_fpcr);
    WriteGlobalSetting(Settings::values.cpuopt_unsafe_inaccurate_nan);
    WriteGlobalSetting(Settings::values.cpuopt_unsafe_fastmem_check);
    WriteGlobalSetting(Settings::values.cpuopt_unsafe_ignore_global_monitor);

    if (global) {
        WriteBasicSetting(Settings::values.cpu_debug_mode);
        WriteBasicSetting(Settings::values.cpuopt_page_tables);
        WriteBasicSetting(Settings::values.cpuopt_block_linking);
        WriteBasicSetting(Settings::values.cpuopt_return_stack_buffer);
        WriteBasicSetting(Settings::values.cpuopt_fast_dispatcher);
        WriteBasicSetting(Settings::values.cpuopt_context_elimination);
        WriteBasicSetting(Settings::values.cpuopt_const_prop);
        WriteBasicSetting(Settings::values.cpuopt_misc_ir);
        WriteBasicSetting(Settings::values.cpuopt_reduce_misalign_checks);
        WriteBasicSetting(Settings::values.cpuopt_fastmem);
        WriteBasicSetting(Settings::values.cpuopt_fastmem_exclusives);
        WriteBasicSetting(Settings::values.cpuopt_recompile_exclusives);
        WriteBasicSetting(Settings::values.cpuopt_ignore_memory_aborts);
    }

    qt_config->endGroup();
}

void Config::SaveRendererValues() {
    qt_config->beginGroup(QStringLiteral("Renderer"));

    WriteSetting(QString::fromStdString(Settings::values.renderer_backend.GetLabel()),
                 static_cast<u32>(Settings::values.renderer_backend.GetValue(global)),
                 static_cast<u32>(Settings::values.renderer_backend.GetDefault()),
                 Settings::values.renderer_backend.UsingGlobal());
    WriteGlobalSetting(Settings::values.async_presentation);
    WriteGlobalSetting(Settings::values.renderer_force_max_clock);
    WriteGlobalSetting(Settings::values.vulkan_device);
    WriteSetting(QString::fromStdString(Settings::values.fullscreen_mode.GetLabel()),
                 static_cast<u32>(Settings::values.fullscreen_mode.GetValue(global)),
                 static_cast<u32>(Settings::values.fullscreen_mode.GetDefault()),
                 Settings::values.fullscreen_mode.UsingGlobal());
    WriteGlobalSetting(Settings::values.aspect_ratio);
    WriteSetting(QString::fromStdString(Settings::values.resolution_setup.GetLabel()),
                 static_cast<u32>(Settings::values.resolution_setup.GetValue(global)),
                 static_cast<u32>(Settings::values.resolution_setup.GetDefault()),
                 Settings::values.resolution_setup.UsingGlobal());
    WriteSetting(QString::fromStdString(Settings::values.scaling_filter.GetLabel()),
                 static_cast<u32>(Settings::values.scaling_filter.GetValue(global)),
                 static_cast<u32>(Settings::values.scaling_filter.GetDefault()),
                 Settings::values.scaling_filter.UsingGlobal());
    WriteSetting(QString::fromStdString(Settings::values.fsr_sharpening_slider.GetLabel()),
                 static_cast<u32>(Settings::values.fsr_sharpening_slider.GetValue(global)),
                 static_cast<u32>(Settings::values.fsr_sharpening_slider.GetDefault()),
                 Settings::values.fsr_sharpening_slider.UsingGlobal());
    WriteSetting(QString::fromStdString(Settings::values.anti_aliasing.GetLabel()),
                 static_cast<u32>(Settings::values.anti_aliasing.GetValue(global)),
                 static_cast<u32>(Settings::values.anti_aliasing.GetDefault()),
                 Settings::values.anti_aliasing.UsingGlobal());
    WriteGlobalSetting(Settings::values.max_anisotropy);
    WriteGlobalSetting(Settings::values.speed_limit);
    WriteGlobalSetting(Settings::values.use_disk_shader_cache);
    WriteSetting(QString::fromStdString(Settings::values.gpu_accuracy.GetLabel()),
                 static_cast<u32>(Settings::values.gpu_accuracy.GetValue(global)),
                 static_cast<u32>(Settings::values.gpu_accuracy.GetDefault()),
                 Settings::values.gpu_accuracy.UsingGlobal());
    WriteGlobalSetting(Settings::values.use_asynchronous_gpu_emulation);
    WriteSetting(QString::fromStdString(Settings::values.nvdec_emulation.GetLabel()),
                 static_cast<u32>(Settings::values.nvdec_emulation.GetValue(global)),
                 static_cast<u32>(Settings::values.nvdec_emulation.GetDefault()),
                 Settings::values.nvdec_emulation.UsingGlobal());
    WriteGlobalSetting(Settings::values.accelerate_astc);
    WriteGlobalSetting(Settings::values.async_astc);
    WriteGlobalSetting(Settings::values.use_reactive_flushing);
    WriteSetting(QString::fromStdString(Settings::values.shader_backend.GetLabel()),
                 static_cast<u32>(Settings::values.shader_backend.GetValue(global)),
                 static_cast<u32>(Settings::values.shader_backend.GetDefault()),
                 Settings::values.shader_backend.UsingGlobal());
    WriteGlobalSetting(Settings::values.use_asynchronous_shaders);
    WriteGlobalSetting(Settings::values.use_fast_gpu_time);
    WriteGlobalSetting(Settings::values.use_vulkan_driver_pipeline_cache);
    WriteGlobalSetting(Settings::values.enable_compute_pipelines);
    WriteGlobalSetting(Settings::values.bg_red);
    WriteGlobalSetting(Settings::values.bg_green);
    WriteGlobalSetting(Settings::values.bg_blue);

    if (global) {
        WriteSetting(QString::fromStdString(Settings::values.vsync_mode.GetLabel()),
                     static_cast<u32>(Settings::values.vsync_mode.GetValue()),
                     static_cast<u32>(Settings::values.vsync_mode.GetDefault()));
        WriteBasicSetting(Settings::values.renderer_debug);
        WriteBasicSetting(Settings::values.renderer_shader_feedback);
        WriteBasicSetting(Settings::values.enable_nsight_aftermath);
        WriteBasicSetting(Settings::values.disable_shader_loop_safety_checks);
    }

    qt_config->endGroup();
}

void Config::SaveScreenshotValues() {
    qt_config->beginGroup(QStringLiteral("Screenshots"));

    WriteBasicSetting(UISettings::values.enable_screenshot_save_as);
    WriteSetting(QStringLiteral("screenshot_path"),
                 QString::fromStdString(FS::GetYuzuPathString(FS::YuzuPath::ScreenshotsDir)));

    qt_config->endGroup();
}

void Config::SaveShortcutValues() {
    qt_config->beginGroup(QStringLiteral("Shortcuts"));

    // Lengths of UISettings::values.shortcuts & default_hotkeys are same.
    // However, their ordering must also be the same.
    for (std::size_t i = 0; i < default_hotkeys.size(); i++) {
        const auto& [name, group, shortcut] = UISettings::values.shortcuts[i];
        const auto& default_hotkey = default_hotkeys[i].shortcut;

        qt_config->beginGroup(group);
        qt_config->beginGroup(name);
        WriteSetting(QStringLiteral("KeySeq"), shortcut.keyseq, default_hotkey.keyseq);
        WriteSetting(QStringLiteral("Controller_KeySeq"), shortcut.controller_keyseq,
                     default_hotkey.controller_keyseq);
        WriteSetting(QStringLiteral("Context"), shortcut.context, default_hotkey.context);
        WriteSetting(QStringLiteral("Repeat"), shortcut.repeat, default_hotkey.repeat);
        qt_config->endGroup();
        qt_config->endGroup();
    }

    qt_config->endGroup();
}

void Config::SaveSystemValues() {
    qt_config->beginGroup(QStringLiteral("System"));

    WriteGlobalSetting(Settings::values.language_index);
    WriteGlobalSetting(Settings::values.region_index);
    WriteGlobalSetting(Settings::values.time_zone_index);

    WriteSetting(QStringLiteral("rng_seed_enabled"),
                 Settings::values.rng_seed.GetValue(global).has_value(), false,
                 Settings::values.rng_seed.UsingGlobal());
    WriteSetting(QStringLiteral("rng_seed"), Settings::values.rng_seed.GetValue(global).value_or(0),
                 0, Settings::values.rng_seed.UsingGlobal());

    if (global) {
        WriteBasicSetting(Settings::values.current_user);

        WriteSetting(QStringLiteral("custom_rtc_enabled"), Settings::values.custom_rtc.has_value(),
                     false);
        WriteSetting(QStringLiteral("custom_rtc"),
                     QVariant::fromValue<long long>(Settings::values.custom_rtc.value_or(0)), 0);
        WriteBasicSetting(Settings::values.device_name);
    }

    WriteGlobalSetting(Settings::values.sound_index);

    qt_config->endGroup();
}

void Config::SaveUIValues() {
    qt_config->beginGroup(QStringLiteral("UI"));

    WriteSetting(QStringLiteral("theme"), UISettings::values.theme,
                 QString::fromUtf8(UISettings::themes[static_cast<size_t>(default_theme)].second));
    WriteBasicSetting(UISettings::values.enable_discord_presence);
    WriteBasicSetting(UISettings::values.select_user_on_boot);

    SaveUIGamelistValues();
    SaveUILayoutValues();
    SavePathValues();
    SaveScreenshotValues();
    SaveShortcutValues();
    SaveMultiplayerValues();

    WriteBasicSetting(UISettings::values.single_window_mode);
    WriteBasicSetting(UISettings::values.fullscreen);
    WriteBasicSetting(UISettings::values.display_titlebar);
    WriteBasicSetting(UISettings::values.show_filter_bar);
    WriteBasicSetting(UISettings::values.show_status_bar);
    WriteBasicSetting(UISettings::values.confirm_before_closing);
    WriteBasicSetting(UISettings::values.first_start);
    WriteBasicSetting(UISettings::values.callout_flags);
    WriteBasicSetting(UISettings::values.show_console);
    WriteBasicSetting(UISettings::values.pause_when_in_background);
    WriteBasicSetting(UISettings::values.mute_when_in_background);
    WriteBasicSetting(UISettings::values.hide_mouse);
    WriteBasicSetting(UISettings::values.disable_web_applet);

    qt_config->endGroup();
}

void Config::SaveUIGamelistValues() {
    qt_config->beginGroup(QStringLiteral("UIGameList"));

    WriteBasicSetting(UISettings::values.show_add_ons);
    WriteBasicSetting(UISettings::values.show_compat);
    WriteBasicSetting(UISettings::values.show_size);
    WriteBasicSetting(UISettings::values.show_types);
    WriteBasicSetting(UISettings::values.game_icon_size);
    WriteBasicSetting(UISettings::values.folder_icon_size);
    WriteBasicSetting(UISettings::values.row_1_text_id);
    WriteBasicSetting(UISettings::values.row_2_text_id);
    WriteBasicSetting(UISettings::values.cache_game_list);
    WriteBasicSetting(UISettings::values.favorites_expanded);
    qt_config->beginWriteArray(QStringLiteral("favorites"));
    for (int i = 0; i < UISettings::values.favorited_ids.size(); i++) {
        qt_config->setArrayIndex(i);
        WriteSetting(QStringLiteral("program_id"),
                     QVariant::fromValue(UISettings::values.favorited_ids[i]));
    }
    qt_config->endArray();

    qt_config->endGroup();
}

void Config::SaveUILayoutValues() {
    qt_config->beginGroup(QStringLiteral("UILayout"));

    WriteSetting(QStringLiteral("geometry"), UISettings::values.geometry);
    WriteSetting(QStringLiteral("state"), UISettings::values.state);
    WriteSetting(QStringLiteral("geometryRenderWindow"), UISettings::values.renderwindow_geometry);
    WriteSetting(QStringLiteral("gameListHeaderState"), UISettings::values.gamelist_header_state);
    WriteSetting(QStringLiteral("microProfileDialogGeometry"),
                 UISettings::values.microprofile_geometry);
    WriteBasicSetting(UISettings::values.microprofile_visible);

    qt_config->endGroup();
}

void Config::SaveWebServiceValues() {
    qt_config->beginGroup(QStringLiteral("WebService"));

    WriteBasicSetting(Settings::values.enable_telemetry);
    WriteBasicSetting(Settings::values.web_api_url);
    WriteBasicSetting(Settings::values.yuzu_username);
    WriteBasicSetting(Settings::values.yuzu_token);

    qt_config->endGroup();
}

void Config::SaveMultiplayerValues() {
    qt_config->beginGroup(QStringLiteral("Multiplayer"));

    WriteBasicSetting(UISettings::values.multiplayer_nickname);
    WriteBasicSetting(UISettings::values.multiplayer_ip);
    WriteBasicSetting(UISettings::values.multiplayer_port);
    WriteBasicSetting(UISettings::values.multiplayer_room_nickname);
    WriteBasicSetting(UISettings::values.multiplayer_room_name);
    WriteBasicSetting(UISettings::values.multiplayer_room_port);
    WriteBasicSetting(UISettings::values.multiplayer_host_type);
    WriteBasicSetting(UISettings::values.multiplayer_port);
    WriteBasicSetting(UISettings::values.multiplayer_max_player);
    WriteBasicSetting(UISettings::values.multiplayer_game_id);
    WriteBasicSetting(UISettings::values.multiplayer_room_description);

    // Write ban list
    qt_config->beginWriteArray(QStringLiteral("username_ban_list"));
    for (std::size_t i = 0; i < UISettings::values.multiplayer_ban_list.first.size(); ++i) {
        qt_config->setArrayIndex(static_cast<int>(i));
        WriteSetting(QStringLiteral("username"),
                     QString::fromStdString(UISettings::values.multiplayer_ban_list.first[i]));
    }
    qt_config->endArray();
    qt_config->beginWriteArray(QStringLiteral("ip_ban_list"));
    for (std::size_t i = 0; i < UISettings::values.multiplayer_ban_list.second.size(); ++i) {
        qt_config->setArrayIndex(static_cast<int>(i));
        WriteSetting(QStringLiteral("ip"),
                     QString::fromStdString(UISettings::values.multiplayer_ban_list.second[i]));
    }
    qt_config->endArray();

    qt_config->endGroup();
}

QVariant Config::ReadSetting(const QString& name) const {
    return qt_config->value(name);
}

QVariant Config::ReadSetting(const QString& name, const QVariant& default_value) const {
    QVariant result;
    if (qt_config->value(name + QStringLiteral("/default"), false).toBool()) {
        result = default_value;
    } else {
        result = qt_config->value(name, default_value);
    }
    return result;
}

template <typename Type, bool ranged>
void Config::ReadGlobalSetting(Settings::SwitchableSetting<Type, ranged>& setting) {
    QString name = QString::fromStdString(setting.GetLabel());
    const bool use_global = qt_config->value(name + QStringLiteral("/use_global"), true).toBool();
    setting.SetGlobal(use_global);
    if (global || !use_global) {
        setting.SetValue(static_cast<QVariant>(
                             ReadSetting(name, QVariant::fromValue<Type>(setting.GetDefault())))
                             .value<Type>());
    }
}

template <typename Type>
void Config::ReadSettingGlobal(Type& setting, const QString& name,
                               const QVariant& default_value) const {
    const bool use_global = qt_config->value(name + QStringLiteral("/use_global"), true).toBool();
    if (global || !use_global) {
        setting = ReadSetting(name, default_value).value<Type>();
    }
}

void Config::WriteSetting(const QString& name, const QVariant& value) {
    qt_config->setValue(name, value);
}

void Config::WriteSetting(const QString& name, const QVariant& value,
                          const QVariant& default_value) {
    qt_config->setValue(name + QStringLiteral("/default"), value == default_value);
    qt_config->setValue(name, value);
}

void Config::WriteSetting(const QString& name, const QVariant& value, const QVariant& default_value,
                          bool use_global) {
    if (!global) {
        qt_config->setValue(name + QStringLiteral("/use_global"), use_global);
    }
    if (global || !use_global) {
        qt_config->setValue(name + QStringLiteral("/default"), value == default_value);
        qt_config->setValue(name, value);
    }
}

void Config::Reload() {
    ReadValues();
    // To apply default value changes
    SaveValues();
}

void Config::Save() {
    SaveValues();
}

void Config::ReadControlPlayerValue(std::size_t player_index) {
    qt_config->beginGroup(QStringLiteral("Controls"));
    ReadPlayerValue(player_index);
    qt_config->endGroup();
}

void Config::SaveControlPlayerValue(std::size_t player_index) {
    qt_config->beginGroup(QStringLiteral("Controls"));
    SavePlayerValue(player_index);
    qt_config->endGroup();
}

void Config::ClearControlPlayerValues() {
    qt_config->beginGroup(QStringLiteral("Controls"));
    // If key is an empty string, all keys in the current group() are removed.
    qt_config->remove(QString{});
    qt_config->endGroup();
}

const std::string& Config::GetConfigFilePath() const {
    return qt_config_loc;
}
