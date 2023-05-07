// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cinttypes>
#include <clocale>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#ifdef __APPLE__
#include <unistd.h> // for chdir
#endif
#ifdef __unix__
#include <csignal>
#include <sys/socket.h>
#endif

// VFS includes must be before glad as they will conflict with Windows file api, which uses defines.
#include "applets/qt_amiibo_settings.h"
#include "applets/qt_controller.h"
#include "applets/qt_error.h"
#include "applets/qt_profile_select.h"
#include "applets/qt_software_keyboard.h"
#include "applets/qt_web_browser.h"
#include "common/nvidia_flags.h"
#include "configuration/configure_input.h"
#include "configuration/configure_per_game.h"
#include "configuration/configure_tas.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs.h"
#include "core/file_sys/vfs_real.h"
#include "core/frontend/applets/cabinet.h"
#include "core/frontend/applets/controller.h"
#include "core/frontend/applets/general_frontend.h"
#include "core/frontend/applets/mii_edit.h"
#include "core/frontend/applets/software_keyboard.h"
#include "core/hid/emulated_controller.h"
#include "core/hid/hid_core.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/applet_ae.h"
#include "core/hle/service/am/applet_oe.h"
#include "core/hle/service/am/applets/applets.h"
#include "yuzu/multiplayer/state.h"
#include "yuzu/util/controller_navigation.h"

// These are wrappers to avoid the calls to CreateDirectory and CreateFile because of the Windows
// defines.
static FileSys::VirtualDir VfsFilesystemCreateDirectoryWrapper(
    const FileSys::VirtualFilesystem& vfs, const std::string& path, FileSys::Mode mode) {
    return vfs->CreateDirectory(path, mode);
}

static FileSys::VirtualFile VfsDirectoryCreateFileWrapper(const FileSys::VirtualDir& dir,
                                                          const std::string& path) {
    return dir->CreateFile(path);
}

#include <fmt/ostream.h>
#include <glad/glad.h>

#define QT_NO_OPENGL
#include <QClipboard>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QStatusBar>
#include <QString>
#include <QSysInfo>
#include <QUrl>
#include <QtConcurrent/QtConcurrent>

#ifdef HAVE_SDL2
#include <SDL.h> // For SDL ScreenSaver functions
#endif

#include <fmt/format.h>
#include "common/detached_tasks.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/literals.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/memory_detect.h"
#include "common/microprofile.h"
#include "common/scm_rev.h"
#include "common/scope_exit.h"
#ifdef _WIN32
#include "common/windows/timer_resolution.h"
#endif
#ifdef ARCHITECTURE_x86_64
#include "common/x64/cpu_detect.h"
#endif
#include "common/settings.h"
#include "common/telemetry.h"
#include "core/core.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/submission_package.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/sm/sm.h"
#include "core/loader/loader.h"
#include "core/perf_stats.h"
#include "core/telemetry_session.h"
#include "input_common/drivers/tas_input.h"
#include "input_common/drivers/virtual_amiibo.h"
#include "input_common/main.h"
#include "ui_main.h"
#include "util/overlay_dialog.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/shader_notify.h"
#include "yuzu/about_dialog.h"
#include "yuzu/bootmanager.h"
#include "yuzu/compatdb.h"
#include "yuzu/compatibility_list.h"
#include "yuzu/configuration/config.h"
#include "yuzu/configuration/configure_dialog.h"
#include "yuzu/configuration/configure_input_per_game.h"
#include "yuzu/debugger/console.h"
#include "yuzu/debugger/controller.h"
#include "yuzu/debugger/profiler.h"
#include "yuzu/debugger/wait_tree.h"
#include "yuzu/discord.h"
#include "yuzu/game_list.h"
#include "yuzu/game_list_p.h"
#include "yuzu/hotkeys.h"
#include "yuzu/install_dialog.h"
#include "yuzu/loading_screen.h"
#include "yuzu/main.h"
#include "yuzu/startup_checks.h"
#include "yuzu/uisettings.h"
#include "yuzu/util/clickable_label.h"

#ifdef YUZU_DBGHELP
#include "yuzu/mini_dump.h"
#endif

using namespace Common::Literals;

#ifdef USE_DISCORD_PRESENCE
#include "yuzu/discord_impl.h"
#endif

#ifdef QT_STATICPLUGIN
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#endif

#ifdef _WIN32
#include <windows.h>
extern "C" {
// tells Nvidia and AMD drivers to use the dedicated GPU by default on laptops with switchable
// graphics
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

constexpr int default_mouse_hide_timeout = 2500;
constexpr int default_mouse_center_timeout = 10;
constexpr int default_input_update_timeout = 1;

/**
 * "Callouts" are one-time instructional messages shown to the user. In the config settings, there
 * is a bitfield "callout_flags" options, used to track if a message has already been shown to the
 * user. This is 32-bits - if we have more than 32 callouts, we should retire and recycle old ones.
 */
enum class CalloutFlag : uint32_t {
    Telemetry = 0x1,
    DRDDeprecation = 0x2,
};

void GMainWindow::ShowTelemetryCallout() {
    if (UISettings::values.callout_flags.GetValue() &
        static_cast<uint32_t>(CalloutFlag::Telemetry)) {
        return;
    }

    UISettings::values.callout_flags =
        UISettings::values.callout_flags.GetValue() | static_cast<uint32_t>(CalloutFlag::Telemetry);
    const QString telemetry_message =
        tr("<a href='https://yuzu-emu.org/help/feature/telemetry/'>Anonymous "
           "data is collected</a> to help improve yuzu. "
           "<br/><br/>Would you like to share your usage data with us?");
    if (QMessageBox::question(this, tr("Telemetry"), telemetry_message) != QMessageBox::Yes) {
        Settings::values.enable_telemetry = false;
        system->ApplySettings();
    }
}

const int GMainWindow::max_recent_files_item;

static void RemoveCachedContents() {
    const auto cache_dir = Common::FS::GetYuzuPath(Common::FS::YuzuPath::CacheDir);
    const auto offline_fonts = cache_dir / "fonts";
    const auto offline_manual = cache_dir / "offline_web_applet_manual";
    const auto offline_legal_information = cache_dir / "offline_web_applet_legal_information";
    const auto offline_system_data = cache_dir / "offline_web_applet_system_data";

    Common::FS::RemoveDirRecursively(offline_fonts);
    Common::FS::RemoveDirRecursively(offline_manual);
    Common::FS::RemoveDirRecursively(offline_legal_information);
    Common::FS::RemoveDirRecursively(offline_system_data);
}

static void LogRuntimes() {
#ifdef _MSC_VER
    // It is possible that the name of the dll will change.
    // vcruntime140.dll is for 2015 and onwards
    static constexpr char runtime_dll_name[] = "vcruntime140.dll";
    UINT sz = GetFileVersionInfoSizeA(runtime_dll_name, nullptr);
    bool runtime_version_inspection_worked = false;
    if (sz > 0) {
        std::vector<u8> buf(sz);
        if (GetFileVersionInfoA(runtime_dll_name, 0, sz, buf.data())) {
            VS_FIXEDFILEINFO* pvi;
            sz = sizeof(VS_FIXEDFILEINFO);
            if (VerQueryValueA(buf.data(), "\\", reinterpret_cast<LPVOID*>(&pvi), &sz)) {
                if (pvi->dwSignature == VS_FFI_SIGNATURE) {
                    runtime_version_inspection_worked = true;
                    LOG_INFO(Frontend, "MSVC Compiler: {} Runtime: {}.{}.{}.{}", _MSC_VER,
                             pvi->dwProductVersionMS >> 16, pvi->dwProductVersionMS & 0xFFFF,
                             pvi->dwProductVersionLS >> 16, pvi->dwProductVersionLS & 0xFFFF);
                }
            }
        }
    }
    if (!runtime_version_inspection_worked) {
        LOG_INFO(Frontend, "Unable to inspect {}", runtime_dll_name);
    }
#endif
    LOG_INFO(Frontend, "Qt Compile: {} Runtime: {}", QT_VERSION_STR, qVersion());
}

static QString PrettyProductName() {
#ifdef _WIN32
    // After Windows 10 Version 2004, Microsoft decided to switch to a different notation: 20H2
    // With that notation change they changed the registry key used to denote the current version
    QSettings windows_registry(
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"),
        QSettings::NativeFormat);
    const QString release_id = windows_registry.value(QStringLiteral("ReleaseId")).toString();
    if (release_id == QStringLiteral("2009")) {
        const u32 current_build = windows_registry.value(QStringLiteral("CurrentBuild")).toUInt();
        const QString display_version =
            windows_registry.value(QStringLiteral("DisplayVersion")).toString();
        const u32 ubr = windows_registry.value(QStringLiteral("UBR")).toUInt();
        u32 version = 10;
        if (current_build >= 22000) {
            version = 11;
        }
        return QStringLiteral("Windows %1 Version %2 (Build %3.%4)")
            .arg(QString::number(version), display_version, QString::number(current_build),
                 QString::number(ubr));
    }
#endif
    return QSysInfo::prettyProductName();
}

#ifdef _WIN32
static void OverrideWindowsFont() {
    // Qt5 chooses these fonts on Windows and they have fairly ugly alphanumeric/cyrillic characters
    // Asking to use "MS Shell Dlg 2" gives better other chars while leaving the Chinese Characters.
    const QString startup_font = QApplication::font().family();
    const QStringList ugly_fonts = {QStringLiteral("SimSun"), QStringLiteral("PMingLiU")};
    if (ugly_fonts.contains(startup_font)) {
        QApplication::setFont(QFont(QStringLiteral("MS Shell Dlg 2"), 9, QFont::Normal));
    }
}
#endif

bool GMainWindow::CheckDarkMode() {
#ifdef __unix__
    const QPalette test_palette(qApp->palette());
    const QColor text_color = test_palette.color(QPalette::Active, QPalette::Text);
    const QColor window_color = test_palette.color(QPalette::Active, QPalette::Window);
    return (text_color.value() > window_color.value());
#else
    // TODO: Windows
    return false;
#endif // __unix__
}

GMainWindow::GMainWindow(std::unique_ptr<Config> config_, bool has_broken_vulkan)
    : ui{std::make_unique<Ui::MainWindow>()}, system{std::make_unique<Core::System>()},
      input_subsystem{std::make_shared<InputCommon::InputSubsystem>()}, config{std::move(config_)},
      vfs{std::make_shared<FileSys::RealVfsFilesystem>()},
      provider{std::make_unique<FileSys::ManualContentProvider>()} {
#ifdef __unix__
    SetupSigInterrupts();
#endif
    system->Initialize();

    Common::Log::Initialize();
    Common::Log::Start();

    LoadTranslation();

    setAcceptDrops(true);
    ui->setupUi(this);
    statusBar()->hide();

    // Check dark mode before a theme is loaded
    os_dark_mode = CheckDarkMode();
    startup_icon_theme = QIcon::themeName();
    // fallback can only be set once, colorful theme icons are okay on both light/dark
    QIcon::setFallbackThemeName(QStringLiteral("colorful"));
    QIcon::setFallbackSearchPaths(QStringList(QStringLiteral(":/icons")));

    default_theme_paths = QIcon::themeSearchPaths();
    UpdateUITheme();

    SetDiscordEnabled(UISettings::values.enable_discord_presence.GetValue());
    discord_rpc->Update();

    system->GetRoomNetwork().Init();

    RegisterMetaTypes();

    InitializeWidgets();
    InitializeDebugWidgets();
    InitializeRecentFileMenuActions();
    InitializeHotkeys();

    SetDefaultUIGeometry();
    RestoreUIState();

    ConnectMenuEvents();
    ConnectWidgetEvents();

    system->HIDCore().ReloadInputDevices();
    controller_dialog->refreshConfiguration();

    const auto branch_name = std::string(Common::g_scm_branch);
    const auto description = std::string(Common::g_scm_desc);
    const auto build_id = std::string(Common::g_build_id);

    const auto yuzu_build = fmt::format("yuzu Development Build | {}-{}", branch_name, description);
    const auto override_build =
        fmt::format(fmt::runtime(std::string(Common::g_title_bar_format_idle)), build_id);
    const auto yuzu_build_version = override_build.empty() ? yuzu_build : override_build;
    const auto processor_count = std::thread::hardware_concurrency();

    LOG_INFO(Frontend, "yuzu Version: {}", yuzu_build_version);
    LogRuntimes();
#ifdef ARCHITECTURE_x86_64
    const auto& caps = Common::GetCPUCaps();
    std::string cpu_string = caps.cpu_string;
    if (caps.avx || caps.avx2 || caps.avx512f) {
        cpu_string += " | AVX";
        if (caps.avx512f) {
            cpu_string += "512";
        } else if (caps.avx2) {
            cpu_string += '2';
        }
        if (caps.fma || caps.fma4) {
            cpu_string += " | FMA";
        }
    }
    LOG_INFO(Frontend, "Host CPU: {}", cpu_string);
    if (std::optional<int> processor_core = Common::GetProcessorCount()) {
        LOG_INFO(Frontend, "Host CPU Cores: {}", *processor_core);
    }
#endif
    LOG_INFO(Frontend, "Host CPU Threads: {}", processor_count);
    LOG_INFO(Frontend, "Host OS: {}", PrettyProductName().toStdString());
    LOG_INFO(Frontend, "Host RAM: {:.2f} GiB",
             Common::GetMemInfo().TotalPhysicalMemory / f64{1_GiB});
    LOG_INFO(Frontend, "Host Swap: {:.2f} GiB", Common::GetMemInfo().TotalSwapMemory / f64{1_GiB});
#ifdef _WIN32
    LOG_INFO(Frontend, "Host Timer Resolution: {:.4f} ms",
             std::chrono::duration_cast<std::chrono::duration<f64, std::milli>>(
                 Common::Windows::SetCurrentTimerResolutionToMaximum())
                 .count());
#endif
    UpdateWindowTitle();

    show();

    system->SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system->RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                    provider.get());
    system->GetFileSystemController().CreateFactories(*vfs);

    // Remove cached contents generated during the previous session
    RemoveCachedContents();

    // Gen keys if necessary
    OnReinitializeKeys(ReinitializeKeyBehavior::NoWarning);

    game_list->LoadCompatibilityList();
    game_list->PopulateAsync(UISettings::values.game_dirs);

    // Show one-time "callout" messages to the user
    ShowTelemetryCallout();

    // make sure menubar has the arrow cursor instead of inheriting from this
    ui->menubar->setCursor(QCursor());
    statusBar()->setCursor(QCursor());

    mouse_hide_timer.setInterval(default_mouse_hide_timeout);
    connect(&mouse_hide_timer, &QTimer::timeout, this, &GMainWindow::HideMouseCursor);
    connect(ui->menubar, &QMenuBar::hovered, this, &GMainWindow::ShowMouseCursor);

    mouse_center_timer.setInterval(default_mouse_center_timeout);
    connect(&mouse_center_timer, &QTimer::timeout, this, &GMainWindow::CenterMouseCursor);

    update_input_timer.setInterval(default_input_update_timeout);
    connect(&update_input_timer, &QTimer::timeout, this, &GMainWindow::UpdateInputDrivers);
    update_input_timer.start();

    MigrateConfigFiles();

    if (has_broken_vulkan) {
        UISettings::values.has_broken_vulkan = true;

        QMessageBox::warning(this, tr("Broken Vulkan Installation Detected"),
                             tr("Vulkan initialization failed during boot.<br><br>Click <a "
                                "href='https://yuzu-emu.org/wiki/faq/"
                                "#yuzu-starts-with-the-error-broken-vulkan-installation-detected'>"
                                "here for instructions to fix the issue</a>."));

        Settings::values.renderer_backend = Settings::RendererBackend::OpenGL;

        renderer_status_button->setDisabled(true);
        renderer_status_button->setChecked(false);
    }

#if defined(HAVE_SDL2) && !defined(_WIN32)
    SDL_InitSubSystem(SDL_INIT_VIDEO);
    // SDL disables the screen saver by default, and setting the hint
    // SDL_HINT_VIDEO_ALLOW_SCREENSAVER doesn't seem to work, so we just enable the screen saver
    // for now.
    SDL_EnableScreenSaver();
#endif

    SetupPrepareForSleep();

    QStringList args = QApplication::arguments();

    if (args.size() < 2) {
        return;
    }

    QString game_path;
    bool has_gamepath = false;
    bool is_fullscreen = false;

    for (int i = 1; i < args.size(); ++i) {
        // Preserves drag/drop functionality
        if (args.size() == 2 && !args[1].startsWith(QChar::fromLatin1('-'))) {
            game_path = args[1];
            has_gamepath = true;
            break;
        }

        // Launch game in fullscreen mode
        if (args[i] == QStringLiteral("-f")) {
            is_fullscreen = true;
            continue;
        }

        // Launch game with a specific user
        if (args[i] == QStringLiteral("-u")) {
            if (i >= args.size() - 1) {
                continue;
            }

            if (args[i + 1].startsWith(QChar::fromLatin1('-'))) {
                continue;
            }

            bool argument_ok;
            const std::size_t selected_user = args[++i].toUInt(&argument_ok);

            if (!argument_ok) {
                LOG_ERROR(Frontend, "Invalid user argument");
                continue;
            }

            const Service::Account::ProfileManager manager;
            if (!manager.UserExistsIndex(selected_user)) {
                LOG_ERROR(Frontend, "Selected user doesn't exist");
                continue;
            }

            Settings::values.current_user = static_cast<s32>(selected_user);
            continue;
        }

        // Launch game at path
        if (args[i] == QStringLiteral("-g")) {
            if (i >= args.size() - 1) {
                continue;
            }

            if (args[i + 1].startsWith(QChar::fromLatin1('-'))) {
                continue;
            }

            game_path = args[++i];
            has_gamepath = true;
        }
    }

    // Override fullscreen setting if gamepath or argument is provided
    if (has_gamepath || is_fullscreen) {
        ui->action_Fullscreen->setChecked(is_fullscreen);
    }

    if (!game_path.isEmpty()) {
        BootGame(game_path);
    }
}

GMainWindow::~GMainWindow() {
    // will get automatically deleted otherwise
    if (render_window->parent() == nullptr) {
        delete render_window;
    }

#ifdef __unix__
    ::close(sig_interrupt_fds[0]);
    ::close(sig_interrupt_fds[1]);
#endif
}

void GMainWindow::RegisterMetaTypes() {
    // Register integral and floating point types
    qRegisterMetaType<u8>("u8");
    qRegisterMetaType<u16>("u16");
    qRegisterMetaType<u32>("u32");
    qRegisterMetaType<u64>("u64");
    qRegisterMetaType<u128>("u128");
    qRegisterMetaType<s8>("s8");
    qRegisterMetaType<s16>("s16");
    qRegisterMetaType<s32>("s32");
    qRegisterMetaType<s64>("s64");
    qRegisterMetaType<f32>("f32");
    qRegisterMetaType<f64>("f64");

    // Register string types
    qRegisterMetaType<std::string>("std::string");
    qRegisterMetaType<std::wstring>("std::wstring");
    qRegisterMetaType<std::u8string>("std::u8string");
    qRegisterMetaType<std::u16string>("std::u16string");
    qRegisterMetaType<std::u32string>("std::u32string");
    qRegisterMetaType<std::string_view>("std::string_view");
    qRegisterMetaType<std::wstring_view>("std::wstring_view");
    qRegisterMetaType<std::u8string_view>("std::u8string_view");
    qRegisterMetaType<std::u16string_view>("std::u16string_view");
    qRegisterMetaType<std::u32string_view>("std::u32string_view");

    // Register applet types

    // Cabinet Applet
    qRegisterMetaType<Core::Frontend::CabinetParameters>("Core::Frontend::CabinetParameters");
    qRegisterMetaType<std::shared_ptr<Service::NFC::NfcDevice>>(
        "std::shared_ptr<Service::NFC::NfcDevice>");

    // Controller Applet
    qRegisterMetaType<Core::Frontend::ControllerParameters>("Core::Frontend::ControllerParameters");

    // Profile Select Applet
    qRegisterMetaType<Core::Frontend::ProfileSelectParameters>(
        "Core::Frontend::ProfileSelectParameters");

    // Software Keyboard Applet
    qRegisterMetaType<Core::Frontend::KeyboardInitializeParameters>(
        "Core::Frontend::KeyboardInitializeParameters");
    qRegisterMetaType<Core::Frontend::InlineAppearParameters>(
        "Core::Frontend::InlineAppearParameters");
    qRegisterMetaType<Core::Frontend::InlineTextParameters>("Core::Frontend::InlineTextParameters");
    qRegisterMetaType<Service::AM::Applets::SwkbdResult>("Service::AM::Applets::SwkbdResult");
    qRegisterMetaType<Service::AM::Applets::SwkbdTextCheckResult>(
        "Service::AM::Applets::SwkbdTextCheckResult");
    qRegisterMetaType<Service::AM::Applets::SwkbdReplyType>("Service::AM::Applets::SwkbdReplyType");

    // Web Browser Applet
    qRegisterMetaType<Service::AM::Applets::WebExitReason>("Service::AM::Applets::WebExitReason");

    // Register loader types
    qRegisterMetaType<Core::SystemResultStatus>("Core::SystemResultStatus");
}

void GMainWindow::AmiiboSettingsShowDialog(const Core::Frontend::CabinetParameters& parameters,
                                           std::shared_ptr<Service::NFC::NfcDevice> nfp_device) {
    cabinet_applet =
        new QtAmiiboSettingsDialog(this, parameters, input_subsystem.get(), nfp_device);
    SCOPE_EXIT({
        cabinet_applet->deleteLater();
        cabinet_applet = nullptr;
    });

    cabinet_applet->setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowStaysOnTopHint |
                                   Qt::WindowTitleHint | Qt::WindowSystemMenuHint);
    cabinet_applet->setWindowModality(Qt::WindowModal);

    if (cabinet_applet->exec() == QDialog::Rejected) {
        emit AmiiboSettingsFinished(false, {});
        return;
    }

    emit AmiiboSettingsFinished(true, cabinet_applet->GetName());
}

void GMainWindow::AmiiboSettingsRequestExit() {
    if (cabinet_applet) {
        cabinet_applet->reject();
    }
}

void GMainWindow::ControllerSelectorReconfigureControllers(
    const Core::Frontend::ControllerParameters& parameters) {
    controller_applet =
        new QtControllerSelectorDialog(this, parameters, input_subsystem.get(), *system);
    SCOPE_EXIT({
        controller_applet->deleteLater();
        controller_applet = nullptr;
    });

    controller_applet->setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint |
                                      Qt::WindowStaysOnTopHint | Qt::WindowTitleHint |
                                      Qt::WindowSystemMenuHint);
    controller_applet->setWindowModality(Qt::WindowModal);
    bool is_success = controller_applet->exec() != QDialog::Rejected;

    // Don't forget to apply settings.
    system->HIDCore().DisableAllControllerConfiguration();
    system->ApplySettings();
    config->Save();

    UpdateStatusButtons();

    emit ControllerSelectorReconfigureFinished(is_success);
}

void GMainWindow::ControllerSelectorRequestExit() {
    if (controller_applet) {
        controller_applet->reject();
    }
}

void GMainWindow::ProfileSelectorSelectProfile(
    const Core::Frontend::ProfileSelectParameters& parameters) {
    profile_select_applet = new QtProfileSelectionDialog(system->HIDCore(), this, parameters);
    SCOPE_EXIT({
        profile_select_applet->deleteLater();
        profile_select_applet = nullptr;
    });

    profile_select_applet->setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint |
                                          Qt::WindowStaysOnTopHint | Qt::WindowTitleHint |
                                          Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
    profile_select_applet->setWindowModality(Qt::WindowModal);
    if (profile_select_applet->exec() == QDialog::Rejected) {
        emit ProfileSelectorFinishedSelection(std::nullopt);
        return;
    }

    const Service::Account::ProfileManager manager;
    const auto uuid = manager.GetUser(static_cast<std::size_t>(profile_select_applet->GetIndex()));
    if (!uuid.has_value()) {
        emit ProfileSelectorFinishedSelection(std::nullopt);
        return;
    }

    emit ProfileSelectorFinishedSelection(uuid);
}

void GMainWindow::ProfileSelectorRequestExit() {
    if (profile_select_applet) {
        profile_select_applet->reject();
    }
}

void GMainWindow::SoftwareKeyboardInitialize(
    bool is_inline, Core::Frontend::KeyboardInitializeParameters initialize_parameters) {
    if (software_keyboard) {
        LOG_ERROR(Frontend, "The software keyboard is already initialized!");
        return;
    }

    software_keyboard = new QtSoftwareKeyboardDialog(render_window, *system, is_inline,
                                                     std::move(initialize_parameters));

    if (is_inline) {
        connect(
            software_keyboard, &QtSoftwareKeyboardDialog::SubmitInlineText, this,
            [this](Service::AM::Applets::SwkbdReplyType reply_type, std::u16string submitted_text,
                   s32 cursor_position) {
                emit SoftwareKeyboardSubmitInlineText(reply_type, submitted_text, cursor_position);
            },
            Qt::QueuedConnection);
    } else {
        connect(
            software_keyboard, &QtSoftwareKeyboardDialog::SubmitNormalText, this,
            [this](Service::AM::Applets::SwkbdResult result, std::u16string submitted_text,
                   bool confirmed) {
                emit SoftwareKeyboardSubmitNormalText(result, submitted_text, confirmed);
            },
            Qt::QueuedConnection);
    }
}

void GMainWindow::SoftwareKeyboardShowNormal() {
    if (!software_keyboard) {
        LOG_ERROR(Frontend, "The software keyboard is not initialized!");
        return;
    }

    const auto& layout = render_window->GetFramebufferLayout();

    const auto x = layout.screen.left;
    const auto y = layout.screen.top;
    const auto w = layout.screen.GetWidth();
    const auto h = layout.screen.GetHeight();
    const auto scale_ratio = devicePixelRatioF();

    software_keyboard->ShowNormalKeyboard(render_window->mapToGlobal(QPoint(x, y) / scale_ratio),
                                          QSize(w, h) / scale_ratio);
}

void GMainWindow::SoftwareKeyboardShowTextCheck(
    Service::AM::Applets::SwkbdTextCheckResult text_check_result,
    std::u16string text_check_message) {
    if (!software_keyboard) {
        LOG_ERROR(Frontend, "The software keyboard is not initialized!");
        return;
    }

    software_keyboard->ShowTextCheckDialog(text_check_result, text_check_message);
}

void GMainWindow::SoftwareKeyboardShowInline(
    Core::Frontend::InlineAppearParameters appear_parameters) {
    if (!software_keyboard) {
        LOG_ERROR(Frontend, "The software keyboard is not initialized!");
        return;
    }

    const auto& layout = render_window->GetFramebufferLayout();

    const auto x =
        static_cast<int>(layout.screen.left + (0.5f * layout.screen.GetWidth() *
                                               ((2.0f * appear_parameters.key_top_translate_x) +
                                                (1.0f - appear_parameters.key_top_scale_x))));
    const auto y =
        static_cast<int>(layout.screen.top + (layout.screen.GetHeight() *
                                              ((2.0f * appear_parameters.key_top_translate_y) +
                                               (1.0f - appear_parameters.key_top_scale_y))));
    const auto w = static_cast<int>(layout.screen.GetWidth() * appear_parameters.key_top_scale_x);
    const auto h = static_cast<int>(layout.screen.GetHeight() * appear_parameters.key_top_scale_y);
    const auto scale_ratio = devicePixelRatioF();

    software_keyboard->ShowInlineKeyboard(std::move(appear_parameters),
                                          render_window->mapToGlobal(QPoint(x, y) / scale_ratio),
                                          QSize(w, h) / scale_ratio);
}

void GMainWindow::SoftwareKeyboardHideInline() {
    if (!software_keyboard) {
        LOG_ERROR(Frontend, "The software keyboard is not initialized!");
        return;
    }

    software_keyboard->HideInlineKeyboard();
}

void GMainWindow::SoftwareKeyboardInlineTextChanged(
    Core::Frontend::InlineTextParameters text_parameters) {
    if (!software_keyboard) {
        LOG_ERROR(Frontend, "The software keyboard is not initialized!");
        return;
    }

    software_keyboard->InlineTextChanged(std::move(text_parameters));
}

void GMainWindow::SoftwareKeyboardExit() {
    if (!software_keyboard) {
        return;
    }

    software_keyboard->ExitKeyboard();

    software_keyboard = nullptr;
}

void GMainWindow::WebBrowserOpenWebPage(const std::string& main_url,
                                        const std::string& additional_args, bool is_local) {
#ifdef YUZU_USE_QT_WEB_ENGINE

    // Raw input breaks with the web applet, Disable web applets if enabled
    if (UISettings::values.disable_web_applet || Settings::values.enable_raw_input) {
        emit WebBrowserClosed(Service::AM::Applets::WebExitReason::WindowClosed,
                              "http://localhost/");
        return;
    }

    web_applet = new QtNXWebEngineView(this, *system, input_subsystem.get());

    ui->action_Pause->setEnabled(false);
    ui->action_Restart->setEnabled(false);
    ui->action_Stop->setEnabled(false);

    {
        QProgressDialog loading_progress(this);
        loading_progress.setLabelText(tr("Loading Web Applet..."));
        loading_progress.setRange(0, 3);
        loading_progress.setValue(0);

        if (is_local && !Common::FS::Exists(main_url)) {
            loading_progress.show();

            auto future = QtConcurrent::run([this] { emit WebBrowserExtractOfflineRomFS(); });

            while (!future.isFinished()) {
                QCoreApplication::processEvents();

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        loading_progress.setValue(1);

        if (is_local) {
            web_applet->LoadLocalWebPage(main_url, additional_args);
        } else {
            web_applet->LoadExternalWebPage(main_url, additional_args);
        }

        if (render_window->IsLoadingComplete()) {
            render_window->hide();
        }

        const auto& layout = render_window->GetFramebufferLayout();
        const auto scale_ratio = devicePixelRatioF();
        web_applet->resize(layout.screen.GetWidth() / scale_ratio,
                           layout.screen.GetHeight() / scale_ratio);
        web_applet->move(layout.screen.left / scale_ratio,
                         (layout.screen.top / scale_ratio) + menuBar()->height());
        web_applet->setZoomFactor(static_cast<qreal>(layout.screen.GetWidth() / scale_ratio) /
                                  static_cast<qreal>(Layout::ScreenUndocked::Width));

        web_applet->setFocus();
        web_applet->show();

        loading_progress.setValue(2);

        QCoreApplication::processEvents();

        loading_progress.setValue(3);
    }

    bool exit_check = false;

    // TODO (Morph): Remove this
    QAction* exit_action = new QAction(tr("Disable Web Applet"), this);
    connect(exit_action, &QAction::triggered, this, [this] {
        const auto result = QMessageBox::warning(
            this, tr("Disable Web Applet"),
            tr("Disabling the web applet can lead to undefined behavior and should only be used "
               "with Super Mario 3D All-Stars. Are you sure you want to disable the web "
               "applet?\n(This can be re-enabled in the Debug settings.)"),
            QMessageBox::Yes | QMessageBox::No);
        if (result == QMessageBox::Yes) {
            UISettings::values.disable_web_applet = true;
            web_applet->SetFinished(true);
        }
    });
    ui->menubar->addAction(exit_action);

    while (!web_applet->IsFinished()) {
        QCoreApplication::processEvents();

        if (!exit_check) {
            web_applet->page()->runJavaScript(
                QStringLiteral("end_applet;"), [&](const QVariant& variant) {
                    exit_check = false;
                    if (variant.toBool()) {
                        web_applet->SetFinished(true);
                        web_applet->SetExitReason(
                            Service::AM::Applets::WebExitReason::EndButtonPressed);
                    }
                });

            exit_check = true;
        }

        if (web_applet->GetCurrentURL().contains(QStringLiteral("localhost"))) {
            if (!web_applet->IsFinished()) {
                web_applet->SetFinished(true);
                web_applet->SetExitReason(Service::AM::Applets::WebExitReason::CallbackURL);
            }

            web_applet->SetLastURL(web_applet->GetCurrentURL().toStdString());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto exit_reason = web_applet->GetExitReason();
    const auto last_url = web_applet->GetLastURL();

    web_applet->hide();

    render_window->setFocus();

    if (render_window->IsLoadingComplete()) {
        render_window->show();
    }

    ui->action_Pause->setEnabled(true);
    ui->action_Restart->setEnabled(true);
    ui->action_Stop->setEnabled(true);

    ui->menubar->removeAction(exit_action);

    QCoreApplication::processEvents();

    emit WebBrowserClosed(exit_reason, last_url);

#else

    // Utilize the same fallback as the default web browser applet.
    emit WebBrowserClosed(Service::AM::Applets::WebExitReason::WindowClosed, "http://localhost/");

#endif
}

void GMainWindow::WebBrowserRequestExit() {
#ifdef YUZU_USE_QT_WEB_ENGINE
    if (web_applet) {
        web_applet->SetExitReason(Service::AM::Applets::WebExitReason::ExitRequested);
        web_applet->SetFinished(true);
    }
#endif
}

void GMainWindow::InitializeWidgets() {
#ifdef YUZU_ENABLE_COMPATIBILITY_REPORTING
    ui->action_Report_Compatibility->setVisible(true);
#endif
    render_window = new GRenderWindow(this, emu_thread.get(), input_subsystem, *system);
    render_window->hide();

    game_list = new GameList(vfs, provider.get(), *system, this);
    ui->horizontalLayout->addWidget(game_list);

    game_list_placeholder = new GameListPlaceholder(this);
    ui->horizontalLayout->addWidget(game_list_placeholder);
    game_list_placeholder->setVisible(false);

    loading_screen = new LoadingScreen(this);
    loading_screen->hide();
    ui->horizontalLayout->addWidget(loading_screen);
    connect(loading_screen, &LoadingScreen::Hidden, [&] {
        loading_screen->Clear();
        if (emulation_running) {
            render_window->show();
            render_window->setFocus();
        }
    });

    multiplayer_state = new MultiplayerState(this, game_list->GetModel(), ui->action_Leave_Room,
                                             ui->action_Show_Room, *system);
    multiplayer_state->setVisible(false);

    // Create status bar
    message_label = new QLabel();
    // Configured separately for left alignment
    message_label->setFrameStyle(QFrame::NoFrame);
    message_label->setContentsMargins(4, 0, 4, 0);
    message_label->setAlignment(Qt::AlignLeft);
    statusBar()->addPermanentWidget(message_label, 1);

    shader_building_label = new QLabel();
    shader_building_label->setToolTip(tr("The amount of shaders currently being built"));
    res_scale_label = new QLabel();
    res_scale_label->setToolTip(tr("The current selected resolution scaling multiplier."));
    emu_speed_label = new QLabel();
    emu_speed_label->setToolTip(
        tr("Current emulation speed. Values higher or lower than 100% "
           "indicate emulation is running faster or slower than a Switch."));
    game_fps_label = new QLabel();
    game_fps_label->setToolTip(tr("How many frames per second the game is currently displaying. "
                                  "This will vary from game to game and scene to scene."));
    emu_frametime_label = new QLabel();
    emu_frametime_label->setToolTip(
        tr("Time taken to emulate a Switch frame, not counting framelimiting or v-sync. For "
           "full-speed emulation this should be at most 16.67 ms."));

    for (auto& label : {shader_building_label, res_scale_label, emu_speed_label, game_fps_label,
                        emu_frametime_label}) {
        label->setVisible(false);
        label->setFrameStyle(QFrame::NoFrame);
        label->setContentsMargins(4, 0, 4, 0);
        statusBar()->addPermanentWidget(label);
    }

    // TODO (flTobi): Add the widget when multiplayer is fully implemented
    statusBar()->addPermanentWidget(multiplayer_state->GetStatusText(), 0);
    statusBar()->addPermanentWidget(multiplayer_state->GetStatusIcon(), 0);

    tas_label = new QLabel();
    tas_label->setObjectName(QStringLiteral("TASlabel"));
    tas_label->setFocusPolicy(Qt::NoFocus);
    statusBar()->insertPermanentWidget(0, tas_label);

    volume_popup = new QWidget(this);
    volume_popup->setWindowFlags(Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::Popup);
    volume_popup->setLayout(new QVBoxLayout());
    volume_popup->setMinimumWidth(200);

    volume_slider = new QSlider(Qt::Horizontal);
    volume_slider->setObjectName(QStringLiteral("volume_slider"));
    volume_slider->setMaximum(200);
    volume_slider->setPageStep(5);
    connect(volume_slider, &QSlider::valueChanged, this, [this](int percentage) {
        Settings::values.audio_muted = false;
        const auto volume = static_cast<u8>(percentage);
        Settings::values.volume.SetValue(volume);
        UpdateVolumeUI();
    });
    volume_popup->layout()->addWidget(volume_slider);

    volume_button = new QPushButton();
    volume_button->setObjectName(QStringLiteral("TogglableStatusBarButton"));
    volume_button->setFocusPolicy(Qt::NoFocus);
    volume_button->setCheckable(true);
    UpdateVolumeUI();
    connect(volume_button, &QPushButton::clicked, this, [&] {
        UpdateVolumeUI();
        volume_popup->setVisible(!volume_popup->isVisible());
        QRect rect = volume_button->geometry();
        QPoint bottomLeft = statusBar()->mapToGlobal(rect.topLeft());
        bottomLeft.setY(bottomLeft.y() - volume_popup->geometry().height());
        volume_popup->setGeometry(QRect(bottomLeft, QSize(rect.width(), rect.height())));
    });
    statusBar()->insertPermanentWidget(0, volume_button);

    // setup AA button
    aa_status_button = new QPushButton();
    aa_status_button->setObjectName(QStringLiteral("TogglableStatusBarButton"));
    aa_status_button->setFocusPolicy(Qt::NoFocus);
    connect(aa_status_button, &QPushButton::clicked, [&] {
        auto aa_mode = Settings::values.anti_aliasing.GetValue();
        if (aa_mode == Settings::AntiAliasing::LastAA) {
            aa_mode = Settings::AntiAliasing::None;
        } else {
            aa_mode = static_cast<Settings::AntiAliasing>(static_cast<u32>(aa_mode) + 1);
        }
        Settings::values.anti_aliasing.SetValue(aa_mode);
        aa_status_button->setChecked(true);
        UpdateAAText();
    });
    UpdateAAText();
    aa_status_button->setCheckable(true);
    aa_status_button->setChecked(true);
    statusBar()->insertPermanentWidget(0, aa_status_button);

    // Setup Filter button
    filter_status_button = new QPushButton();
    filter_status_button->setObjectName(QStringLiteral("TogglableStatusBarButton"));
    filter_status_button->setFocusPolicy(Qt::NoFocus);
    connect(filter_status_button, &QPushButton::clicked, this,
            &GMainWindow::OnToggleAdaptingFilter);
    UpdateFilterText();
    filter_status_button->setCheckable(true);
    filter_status_button->setChecked(true);
    statusBar()->insertPermanentWidget(0, filter_status_button);

    // Setup Dock button
    dock_status_button = new QPushButton();
    dock_status_button->setObjectName(QStringLiteral("DockingStatusBarButton"));
    dock_status_button->setFocusPolicy(Qt::NoFocus);
    connect(dock_status_button, &QPushButton::clicked, this, &GMainWindow::OnToggleDockedMode);
    dock_status_button->setCheckable(true);
    UpdateDockedButton();
    statusBar()->insertPermanentWidget(0, dock_status_button);

    gpu_accuracy_button = new QPushButton();
    gpu_accuracy_button->setObjectName(QStringLiteral("GPUStatusBarButton"));
    gpu_accuracy_button->setCheckable(true);
    gpu_accuracy_button->setFocusPolicy(Qt::NoFocus);
    connect(gpu_accuracy_button, &QPushButton::clicked, this, &GMainWindow::OnToggleGpuAccuracy);
    UpdateGPUAccuracyButton();
    statusBar()->insertPermanentWidget(0, gpu_accuracy_button);

    // Setup Renderer API button
    renderer_status_button = new QPushButton();
    renderer_status_button->setObjectName(QStringLiteral("RendererStatusBarButton"));
    renderer_status_button->setCheckable(true);
    renderer_status_button->setFocusPolicy(Qt::NoFocus);
    connect(renderer_status_button, &QPushButton::clicked, this, &GMainWindow::OnToggleGraphicsAPI);
    UpdateAPIText();
    renderer_status_button->setCheckable(true);
    renderer_status_button->setChecked(Settings::values.renderer_backend.GetValue() ==
                                       Settings::RendererBackend::Vulkan);
    statusBar()->insertPermanentWidget(0, renderer_status_button);

    statusBar()->setVisible(true);
    setStyleSheet(QStringLiteral("QStatusBar::item{border: none;}"));
}

void GMainWindow::InitializeDebugWidgets() {
    QMenu* debug_menu = ui->menu_View_Debugging;

#if MICROPROFILE_ENABLED
    microProfileDialog = new MicroProfileDialog(this);
    microProfileDialog->hide();
    debug_menu->addAction(microProfileDialog->toggleViewAction());
#endif

    waitTreeWidget = new WaitTreeWidget(*system, this);
    addDockWidget(Qt::LeftDockWidgetArea, waitTreeWidget);
    waitTreeWidget->hide();
    debug_menu->addAction(waitTreeWidget->toggleViewAction());

    controller_dialog = new ControllerDialog(system->HIDCore(), input_subsystem, this);
    controller_dialog->hide();
    debug_menu->addAction(controller_dialog->toggleViewAction());

    connect(this, &GMainWindow::EmulationStarting, waitTreeWidget,
            &WaitTreeWidget::OnEmulationStarting);
    connect(this, &GMainWindow::EmulationStopping, waitTreeWidget,
            &WaitTreeWidget::OnEmulationStopping);
}

void GMainWindow::InitializeRecentFileMenuActions() {
    for (int i = 0; i < max_recent_files_item; ++i) {
        actions_recent_files[i] = new QAction(this);
        actions_recent_files[i]->setVisible(false);
        connect(actions_recent_files[i], &QAction::triggered, this, &GMainWindow::OnMenuRecentFile);

        ui->menu_recent_files->addAction(actions_recent_files[i]);
    }
    ui->menu_recent_files->addSeparator();
    QAction* action_clear_recent_files = new QAction(this);
    action_clear_recent_files->setText(tr("&Clear Recent Files"));
    connect(action_clear_recent_files, &QAction::triggered, this, [this] {
        UISettings::values.recent_files.clear();
        UpdateRecentFiles();
    });
    ui->menu_recent_files->addAction(action_clear_recent_files);

    UpdateRecentFiles();
}

void GMainWindow::LinkActionShortcut(QAction* action, const QString& action_name) {
    static const QString main_window = QStringLiteral("Main Window");
    action->setShortcut(hotkey_registry.GetKeySequence(main_window, action_name));
    action->setShortcutContext(hotkey_registry.GetShortcutContext(main_window, action_name));
    action->setAutoRepeat(false);

    this->addAction(action);

    auto* controller = system->HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    const auto* controller_hotkey =
        hotkey_registry.GetControllerHotkey(main_window, action_name, controller);
    connect(
        controller_hotkey, &ControllerShortcut::Activated, this, [action] { action->trigger(); },
        Qt::QueuedConnection);
}

void GMainWindow::InitializeHotkeys() {
    hotkey_registry.LoadHotkeys();

    LinkActionShortcut(ui->action_Load_File, QStringLiteral("Load File"));
    LinkActionShortcut(ui->action_Load_Amiibo, QStringLiteral("Load/Remove Amiibo"));
    LinkActionShortcut(ui->action_Exit, QStringLiteral("Exit yuzu"));
    LinkActionShortcut(ui->action_Restart, QStringLiteral("Restart Emulation"));
    LinkActionShortcut(ui->action_Pause, QStringLiteral("Continue/Pause Emulation"));
    LinkActionShortcut(ui->action_Stop, QStringLiteral("Stop Emulation"));
    LinkActionShortcut(ui->action_Show_Filter_Bar, QStringLiteral("Toggle Filter Bar"));
    LinkActionShortcut(ui->action_Show_Status_Bar, QStringLiteral("Toggle Status Bar"));
    LinkActionShortcut(ui->action_Fullscreen, QStringLiteral("Fullscreen"));
    LinkActionShortcut(ui->action_Capture_Screenshot, QStringLiteral("Capture Screenshot"));
    LinkActionShortcut(ui->action_TAS_Start, QStringLiteral("TAS Start/Stop"));
    LinkActionShortcut(ui->action_TAS_Record, QStringLiteral("TAS Record"));
    LinkActionShortcut(ui->action_TAS_Reset, QStringLiteral("TAS Reset"));

    static const QString main_window = QStringLiteral("Main Window");
    const auto connect_shortcut = [&]<typename Fn>(const QString& action_name, const Fn& function) {
        const auto* hotkey = hotkey_registry.GetHotkey(main_window, action_name, this);
        auto* controller = system->HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
        const auto* controller_hotkey =
            hotkey_registry.GetControllerHotkey(main_window, action_name, controller);
        connect(hotkey, &QShortcut::activated, this, function);
        connect(controller_hotkey, &ControllerShortcut::Activated, this, function,
                Qt::QueuedConnection);
    };

    connect_shortcut(QStringLiteral("Exit Fullscreen"), [&] {
        if (emulation_running && ui->action_Fullscreen->isChecked()) {
            ui->action_Fullscreen->setChecked(false);
            ToggleFullscreen();
        }
    });
    connect_shortcut(QStringLiteral("Change Adapting Filter"),
                     &GMainWindow::OnToggleAdaptingFilter);
    connect_shortcut(QStringLiteral("Change Docked Mode"), &GMainWindow::OnToggleDockedMode);
    connect_shortcut(QStringLiteral("Change GPU Accuracy"), &GMainWindow::OnToggleGpuAccuracy);
    connect_shortcut(QStringLiteral("Audio Mute/Unmute"), &GMainWindow::OnMute);
    connect_shortcut(QStringLiteral("Audio Volume Down"), &GMainWindow::OnDecreaseVolume);
    connect_shortcut(QStringLiteral("Audio Volume Up"), &GMainWindow::OnIncreaseVolume);
    connect_shortcut(QStringLiteral("Toggle Framerate Limit"), [] {
        Settings::values.use_speed_limit.SetValue(!Settings::values.use_speed_limit.GetValue());
    });
    connect_shortcut(QStringLiteral("Toggle Mouse Panning"), [&] {
        if (Settings::values.mouse_enabled) {
            Settings::values.mouse_panning = false;
            QMessageBox::warning(
                this, tr("Emulated mouse is enabled"),
                tr("Real mouse input and mouse panning are incompatible. Please disable the "
                   "emulated mouse in input advanced settings to allow mouse panning."));
            return;
        }
        Settings::values.mouse_panning = !Settings::values.mouse_panning;
        if (Settings::values.mouse_panning) {
            render_window->installEventFilter(render_window);
            render_window->setAttribute(Qt::WA_Hover, true);
        }
    });
}

void GMainWindow::SetDefaultUIGeometry() {
    // geometry: 53% of the window contents are in the upper screen half, 47% in the lower half
    const QRect screenRect = QGuiApplication::primaryScreen()->geometry();

    const int w = screenRect.width() * 2 / 3;
    const int h = screenRect.height() * 2 / 3;
    const int x = (screenRect.x() + screenRect.width()) / 2 - w / 2;
    const int y = (screenRect.y() + screenRect.height()) / 2 - h * 53 / 100;

    setGeometry(x, y, w, h);
}

void GMainWindow::RestoreUIState() {
    setWindowFlags(windowFlags() & ~Qt::FramelessWindowHint);
    restoreGeometry(UISettings::values.geometry);
    // Work-around because the games list isn't supposed to be full screen
    if (isFullScreen()) {
        showNormal();
    }
    restoreState(UISettings::values.state);
    render_window->setWindowFlags(render_window->windowFlags() & ~Qt::FramelessWindowHint);
    render_window->restoreGeometry(UISettings::values.renderwindow_geometry);
#if MICROPROFILE_ENABLED
    microProfileDialog->restoreGeometry(UISettings::values.microprofile_geometry);
    microProfileDialog->setVisible(UISettings::values.microprofile_visible.GetValue());
#endif

    game_list->LoadInterfaceLayout();

    ui->action_Single_Window_Mode->setChecked(UISettings::values.single_window_mode.GetValue());
    ToggleWindowMode();

    ui->action_Fullscreen->setChecked(UISettings::values.fullscreen.GetValue());

    ui->action_Display_Dock_Widget_Headers->setChecked(
        UISettings::values.display_titlebar.GetValue());
    OnDisplayTitleBars(ui->action_Display_Dock_Widget_Headers->isChecked());

    ui->action_Show_Filter_Bar->setChecked(UISettings::values.show_filter_bar.GetValue());
    game_list->SetFilterVisible(ui->action_Show_Filter_Bar->isChecked());

    ui->action_Show_Status_Bar->setChecked(UISettings::values.show_status_bar.GetValue());
    statusBar()->setVisible(ui->action_Show_Status_Bar->isChecked());
    Debugger::ToggleConsole();
}

void GMainWindow::OnAppFocusStateChanged(Qt::ApplicationState state) {
    if (state != Qt::ApplicationHidden && state != Qt::ApplicationInactive &&
        state != Qt::ApplicationActive) {
        LOG_DEBUG(Frontend, "ApplicationState unusual flag: {} ", state);
    }
    if (!emulation_running) {
        return;
    }
    if (UISettings::values.pause_when_in_background) {
        if (emu_thread->IsRunning() &&
            (state & (Qt::ApplicationHidden | Qt::ApplicationInactive))) {
            auto_paused = true;
            OnPauseGame();
        } else if (!emu_thread->IsRunning() && auto_paused && state == Qt::ApplicationActive) {
            auto_paused = false;
            RequestGameResume();
            OnStartGame();
        }
    }
    if (UISettings::values.mute_when_in_background) {
        if (!Settings::values.audio_muted &&
            (state & (Qt::ApplicationHidden | Qt::ApplicationInactive))) {
            Settings::values.audio_muted = true;
            auto_muted = true;
        } else if (auto_muted && state == Qt::ApplicationActive) {
            Settings::values.audio_muted = false;
            auto_muted = false;
        }
    }
}

void GMainWindow::ConnectWidgetEvents() {
    connect(game_list, &GameList::BootGame, this, &GMainWindow::BootGame);
    connect(game_list, &GameList::GameChosen, this, &GMainWindow::OnGameListLoadFile);
    connect(game_list, &GameList::OpenDirectory, this, &GMainWindow::OnGameListOpenDirectory);
    connect(game_list, &GameList::OpenFolderRequested, this, &GMainWindow::OnGameListOpenFolder);
    connect(game_list, &GameList::OpenTransferableShaderCacheRequested, this,
            &GMainWindow::OnTransferableShaderCacheOpenFile);
    connect(game_list, &GameList::RemoveInstalledEntryRequested, this,
            &GMainWindow::OnGameListRemoveInstalledEntry);
    connect(game_list, &GameList::RemoveFileRequested, this, &GMainWindow::OnGameListRemoveFile);
    connect(game_list, &GameList::DumpRomFSRequested, this, &GMainWindow::OnGameListDumpRomFS);
    connect(game_list, &GameList::CopyTIDRequested, this, &GMainWindow::OnGameListCopyTID);
    connect(game_list, &GameList::NavigateToGamedbEntryRequested, this,
            &GMainWindow::OnGameListNavigateToGamedbEntry);
    connect(game_list, &GameList::CreateShortcut, this, &GMainWindow::OnGameListCreateShortcut);
    connect(game_list, &GameList::AddDirectory, this, &GMainWindow::OnGameListAddDirectory);
    connect(game_list_placeholder, &GameListPlaceholder::AddDirectory, this,
            &GMainWindow::OnGameListAddDirectory);
    connect(game_list, &GameList::ShowList, this, &GMainWindow::OnGameListShowList);
    connect(game_list, &GameList::PopulatingCompleted,
            [this] { multiplayer_state->UpdateGameList(game_list->GetModel()); });
    connect(game_list, &GameList::SaveConfig, this, &GMainWindow::OnSaveConfig);

    connect(game_list, &GameList::OpenPerGameGeneralRequested, this,
            &GMainWindow::OnGameListOpenPerGameProperties);

    connect(this, &GMainWindow::UpdateInstallProgress, this,
            &GMainWindow::IncrementInstallProgress);

    connect(this, &GMainWindow::EmulationStarting, render_window,
            &GRenderWindow::OnEmulationStarting);
    connect(this, &GMainWindow::EmulationStopping, render_window,
            &GRenderWindow::OnEmulationStopping);

    // Software Keyboard Applet
    connect(this, &GMainWindow::EmulationStarting, this, &GMainWindow::SoftwareKeyboardExit);
    connect(this, &GMainWindow::EmulationStopping, this, &GMainWindow::SoftwareKeyboardExit);

    connect(&status_bar_update_timer, &QTimer::timeout, this, &GMainWindow::UpdateStatusBar);

    connect(this, &GMainWindow::UpdateThemedIcons, multiplayer_state,
            &MultiplayerState::UpdateThemedIcons);
}

void GMainWindow::ConnectMenuEvents() {
    const auto connect_menu = [&]<typename Fn>(QAction* action, const Fn& event_fn) {
        connect(action, &QAction::triggered, this, event_fn);
        // Add actions to this window so that hiding menus in fullscreen won't disable them
        addAction(action);
        // Add actions to the render window so that they work outside of single window mode
        render_window->addAction(action);
    };

    // File
    connect_menu(ui->action_Load_File, &GMainWindow::OnMenuLoadFile);
    connect_menu(ui->action_Load_Folder, &GMainWindow::OnMenuLoadFolder);
    connect_menu(ui->action_Install_File_NAND, &GMainWindow::OnMenuInstallToNAND);
    connect_menu(ui->action_Exit, &QMainWindow::close);
    connect_menu(ui->action_Load_Amiibo, &GMainWindow::OnLoadAmiibo);

    // Emulation
    connect_menu(ui->action_Pause, &GMainWindow::OnPauseContinueGame);
    connect_menu(ui->action_Stop, &GMainWindow::OnStopGame);
    connect_menu(ui->action_Report_Compatibility, &GMainWindow::OnMenuReportCompatibility);
    connect_menu(ui->action_Open_Mods_Page, &GMainWindow::OnOpenModsPage);
    connect_menu(ui->action_Open_Quickstart_Guide, &GMainWindow::OnOpenQuickstartGuide);
    connect_menu(ui->action_Open_FAQ, &GMainWindow::OnOpenFAQ);
    connect_menu(ui->action_Restart, &GMainWindow::OnRestartGame);
    connect_menu(ui->action_Configure, &GMainWindow::OnConfigure);
    connect_menu(ui->action_Configure_Current_Game, &GMainWindow::OnConfigurePerGame);

    // View
    connect_menu(ui->action_Fullscreen, &GMainWindow::ToggleFullscreen);
    connect_menu(ui->action_Single_Window_Mode, &GMainWindow::ToggleWindowMode);
    connect_menu(ui->action_Display_Dock_Widget_Headers, &GMainWindow::OnDisplayTitleBars);
    connect_menu(ui->action_Show_Filter_Bar, &GMainWindow::OnToggleFilterBar);
    connect_menu(ui->action_Show_Status_Bar, &GMainWindow::OnToggleStatusBar);

    connect_menu(ui->action_Reset_Window_Size_720, &GMainWindow::ResetWindowSize720);
    connect_menu(ui->action_Reset_Window_Size_900, &GMainWindow::ResetWindowSize900);
    connect_menu(ui->action_Reset_Window_Size_1080, &GMainWindow::ResetWindowSize1080);
    ui->menu_Reset_Window_Size->addActions({ui->action_Reset_Window_Size_720,
                                            ui->action_Reset_Window_Size_900,
                                            ui->action_Reset_Window_Size_1080});

    // Multiplayer
    connect(ui->action_View_Lobby, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnViewLobby);
    connect(ui->action_Start_Room, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnCreateRoom);
    connect(ui->action_Leave_Room, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnCloseRoom);
    connect(ui->action_Connect_To_Room, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnDirectConnectToRoom);
    connect(ui->action_Show_Room, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnOpenNetworkRoom);
    connect(multiplayer_state, &MultiplayerState::SaveConfig, this, &GMainWindow::OnSaveConfig);

    // Tools
    connect_menu(ui->action_Rederive, std::bind(&GMainWindow::OnReinitializeKeys, this,
                                                ReinitializeKeyBehavior::Warning));
    connect_menu(ui->action_Capture_Screenshot, &GMainWindow::OnCaptureScreenshot);

    // TAS
    connect_menu(ui->action_TAS_Start, &GMainWindow::OnTasStartStop);
    connect_menu(ui->action_TAS_Record, &GMainWindow::OnTasRecord);
    connect_menu(ui->action_TAS_Reset, &GMainWindow::OnTasReset);
    connect_menu(ui->action_Configure_Tas, &GMainWindow::OnConfigureTas);

    // Help
    connect_menu(ui->action_Open_yuzu_Folder, &GMainWindow::OnOpenYuzuFolder);
    connect_menu(ui->action_About, &GMainWindow::OnAbout);
}

void GMainWindow::UpdateMenuState() {
    const bool is_paused = emu_thread == nullptr || !emu_thread->IsRunning();

    const std::array running_actions{
        ui->action_Stop,
        ui->action_Restart,
        ui->action_Configure_Current_Game,
        ui->action_Report_Compatibility,
        ui->action_Load_Amiibo,
        ui->action_Pause,
    };

    for (QAction* action : running_actions) {
        action->setEnabled(emulation_running);
    }

    ui->action_Capture_Screenshot->setEnabled(emulation_running && !is_paused);

    if (emulation_running && is_paused) {
        ui->action_Pause->setText(tr("&Continue"));
    } else {
        ui->action_Pause->setText(tr("&Pause"));
    }

    multiplayer_state->UpdateNotificationStatus();
}

void GMainWindow::OnDisplayTitleBars(bool show) {
    QList<QDockWidget*> widgets = findChildren<QDockWidget*>();

    if (show) {
        for (QDockWidget* widget : widgets) {
            QWidget* old = widget->titleBarWidget();
            widget->setTitleBarWidget(nullptr);
            if (old != nullptr)
                delete old;
        }
    } else {
        for (QDockWidget* widget : widgets) {
            QWidget* old = widget->titleBarWidget();
            widget->setTitleBarWidget(new QWidget());
            if (old != nullptr)
                delete old;
        }
    }
}

void GMainWindow::SetupPrepareForSleep() {
#ifdef __unix__
    auto bus = QDBusConnection::systemBus();
    if (bus.isConnected()) {
        const bool success = bus.connect(
            QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
            QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("PrepareForSleep"),
            QStringLiteral("b"), this, SLOT(OnPrepareForSleep(bool)));

        if (!success) {
            LOG_WARNING(Frontend, "Couldn't register PrepareForSleep signal");
        }
    } else {
        LOG_WARNING(Frontend, "QDBusConnection system bus is not connected");
    }
#endif // __unix__
}

void GMainWindow::OnPrepareForSleep(bool prepare_sleep) {
    if (emu_thread == nullptr) {
        return;
    }

    if (prepare_sleep) {
        if (emu_thread->IsRunning()) {
            auto_paused = true;
            OnPauseGame();
        }
    } else {
        if (!emu_thread->IsRunning() && auto_paused) {
            auto_paused = false;
            RequestGameResume();
            OnStartGame();
        }
    }
}

#ifdef __unix__
static std::optional<QDBusObjectPath> HoldWakeLockLinux(u32 window_id = 0) {
    if (!QDBusConnection::sessionBus().isConnected()) {
        return {};
    }
    // reference: https://flatpak.github.io/xdg-desktop-portal/#gdbus-org.freedesktop.portal.Inhibit
    QDBusInterface xdp(QString::fromLatin1("org.freedesktop.portal.Desktop"),
                       QString::fromLatin1("/org/freedesktop/portal/desktop"),
                       QString::fromLatin1("org.freedesktop.portal.Inhibit"));
    if (!xdp.isValid()) {
        LOG_WARNING(Frontend, "Couldn't connect to XDP D-Bus endpoint");
        return {};
    }
    QVariantMap options = {};
    //: TRANSLATORS: This string is shown to the user to explain why yuzu needs to prevent the
    //: computer from sleeping
    options.insert(QString::fromLatin1("reason"),
                   QCoreApplication::translate("GMainWindow", "yuzu is running a game"));
    // 0x4: Suspend lock; 0x8: Idle lock
    QDBusReply<QDBusObjectPath> reply =
        xdp.call(QString::fromLatin1("Inhibit"),
                 QString::fromLatin1("x11:") + QString::number(window_id, 16), 12U, options);

    if (reply.isValid()) {
        return reply.value();
    }
    LOG_WARNING(Frontend, "Couldn't read Inhibit reply from XDP: {}",
                reply.error().message().toStdString());
    return {};
}

static void ReleaseWakeLockLinux(QDBusObjectPath lock) {
    if (!QDBusConnection::sessionBus().isConnected()) {
        return;
    }
    QDBusInterface unlocker(QString::fromLatin1("org.freedesktop.portal.Desktop"), lock.path(),
                            QString::fromLatin1("org.freedesktop.portal.Request"));
    unlocker.call(QString::fromLatin1("Close"));
}

std::array<int, 3> GMainWindow::sig_interrupt_fds{0, 0, 0};

void GMainWindow::SetupSigInterrupts() {
    if (sig_interrupt_fds[2] == 1) {
        return;
    }
    socketpair(AF_UNIX, SOCK_STREAM, 0, sig_interrupt_fds.data());
    sig_interrupt_fds[2] = 1;

    struct sigaction sa;
    sa.sa_handler = &GMainWindow::HandleSigInterrupt;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    sig_interrupt_notifier = new QSocketNotifier(sig_interrupt_fds[1], QSocketNotifier::Read, this);
    connect(sig_interrupt_notifier, &QSocketNotifier::activated, this,
            &GMainWindow::OnSigInterruptNotifierActivated);
    connect(this, &GMainWindow::SigInterrupt, this, &GMainWindow::close);
}

void GMainWindow::HandleSigInterrupt(int sig) {
    if (sig == SIGINT) {
        _exit(1);
    }

    // Calling into Qt directly from a signal handler is not safe,
    // so wake up a QSocketNotifier with this hacky write call instead.
    char a = 1;
    int ret = write(sig_interrupt_fds[0], &a, sizeof(a));
    (void)ret;
}

void GMainWindow::OnSigInterruptNotifierActivated() {
    sig_interrupt_notifier->setEnabled(false);

    char a;
    int ret = read(sig_interrupt_fds[1], &a, sizeof(a));
    (void)ret;

    sig_interrupt_notifier->setEnabled(true);

    emit SigInterrupt();
}
#endif // __unix__

void GMainWindow::PreventOSSleep() {
#ifdef _WIN32
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
#elif defined(HAVE_SDL2)
    SDL_DisableScreenSaver();
#ifdef __unix__
    auto reply = HoldWakeLockLinux(winId());
    if (reply) {
        wake_lock = std::move(reply.value());
    }
#endif
#endif
}

void GMainWindow::AllowOSSleep() {
#ifdef _WIN32
    SetThreadExecutionState(ES_CONTINUOUS);
#elif defined(HAVE_SDL2)
    SDL_EnableScreenSaver();
#ifdef __unix__
    if (!wake_lock.path().isEmpty()) {
        ReleaseWakeLockLinux(wake_lock);
    }
#endif
#endif
}

bool GMainWindow::LoadROM(const QString& filename, u64 program_id, std::size_t program_index) {
    // Shutdown previous session if the emu thread is still active...
    if (emu_thread != nullptr) {
        ShutdownGame();
    }

    if (!render_window->InitRenderTarget()) {
        return false;
    }

    system->SetFilesystem(vfs);

    system->SetAppletFrontendSet({
        std::make_unique<QtAmiiboSettings>(*this),     // Amiibo Settings
        std::make_unique<QtControllerSelector>(*this), // Controller Selector
        std::make_unique<QtErrorDisplay>(*this),       // Error Display
        nullptr,                                       // Mii Editor
        nullptr,                                       // Parental Controls
        nullptr,                                       // Photo Viewer
        std::make_unique<QtProfileSelector>(*this),    // Profile Selector
        std::make_unique<QtSoftwareKeyboard>(*this),   // Software Keyboard
        std::make_unique<QtWebBrowser>(*this),         // Web Browser
    });

    const Core::SystemResultStatus result{
        system->Load(*render_window, filename.toStdString(), program_id, program_index)};

    const auto drd_callout = (UISettings::values.callout_flags.GetValue() &
                              static_cast<u32>(CalloutFlag::DRDDeprecation)) == 0;

    if (result == Core::SystemResultStatus::Success &&
        system->GetAppLoader().GetFileType() == Loader::FileType::DeconstructedRomDirectory &&
        drd_callout) {
        UISettings::values.callout_flags = UISettings::values.callout_flags.GetValue() |
                                           static_cast<u32>(CalloutFlag::DRDDeprecation);
        QMessageBox::warning(
            this, tr("Warning Outdated Game Format"),
            tr("You are using the deconstructed ROM directory format for this game, which is an "
               "outdated format that has been superseded by others such as NCA, NAX, XCI, or "
               "NSP. Deconstructed ROM directories lack icons, metadata, and update "
               "support.<br><br>For an explanation of the various Switch formats yuzu supports, <a "
               "href='https://yuzu-emu.org/wiki/overview-of-switch-game-formats'>check out our "
               "wiki</a>. This message will not be shown again."));
    }

    if (result != Core::SystemResultStatus::Success) {
        switch (result) {
        case Core::SystemResultStatus::ErrorGetLoader:
            LOG_CRITICAL(Frontend, "Failed to obtain loader for {}!", filename.toStdString());
            QMessageBox::critical(this, tr("Error while loading ROM!"),
                                  tr("The ROM format is not supported."));
            break;
        case Core::SystemResultStatus::ErrorVideoCore:
            QMessageBox::critical(
                this, tr("An error occurred initializing the video core."),
                tr("yuzu has encountered an error while running the video core. "
                   "This is usually caused by outdated GPU drivers, including integrated ones. "
                   "Please see the log for more details. "
                   "For more information on accessing the log, please see the following page: "
                   "<a href='https://yuzu-emu.org/help/reference/log-files/'>"
                   "How to Upload the Log File</a>. "));
            break;
        default:
            if (result > Core::SystemResultStatus::ErrorLoader) {
                const u16 loader_id = static_cast<u16>(Core::SystemResultStatus::ErrorLoader);
                const u16 error_id = static_cast<u16>(result) - loader_id;
                const std::string error_code = fmt::format("({:04X}-{:04X})", loader_id, error_id);
                LOG_CRITICAL(Frontend, "Failed to load ROM! {}", error_code);

                const auto title =
                    tr("Error while loading ROM! %1", "%1 signifies a numeric error code.")
                        .arg(QString::fromStdString(error_code));
                const auto description =
                    tr("%1<br>Please follow <a href='https://yuzu-emu.org/help/quickstart/'>the "
                       "yuzu quickstart guide</a> to redump your files.<br>You can refer "
                       "to the yuzu wiki</a> or the yuzu Discord</a> for help.",
                       "%1 signifies an error string.")
                        .arg(QString::fromStdString(
                            GetResultStatusString(static_cast<Loader::ResultStatus>(error_id))));

                QMessageBox::critical(this, title, description);
            } else {
                QMessageBox::critical(
                    this, tr("Error while loading ROM!"),
                    tr("An unknown error occurred. Please see the log for more details."));
            }
            break;
        }
        return false;
    }
    current_game_path = filename;

    system->TelemetrySession().AddField(Common::Telemetry::FieldType::App, "Frontend", "Qt");
    return true;
}

bool GMainWindow::SelectAndSetCurrentUser(
    const Core::Frontend::ProfileSelectParameters& parameters) {
    QtProfileSelectionDialog dialog(system->HIDCore(), this, parameters);
    dialog.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                          Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
    dialog.setWindowModality(Qt::WindowModal);

    if (dialog.exec() == QDialog::Rejected) {
        return false;
    }

    Settings::values.current_user = dialog.GetIndex();
    return true;
}

void GMainWindow::BootGame(const QString& filename, u64 program_id, std::size_t program_index,
                           StartGameType type) {
    LOG_INFO(Frontend, "yuzu starting...");
    StoreRecentFile(filename); // Put the filename on top of the list

    // Save configurations
    UpdateUISettings();
    game_list->SaveInterfaceLayout();
    config->Save();

    u64 title_id{0};

    last_filename_booted = filename;

    const auto v_file = Core::GetGameFileFromPath(vfs, filename.toUtf8().constData());
    const auto loader = Loader::GetLoader(*system, v_file, program_id, program_index);

    if (loader != nullptr && loader->ReadProgramId(title_id) == Loader::ResultStatus::Success &&
        type == StartGameType::Normal) {
        // Load per game settings
        const auto file_path =
            std::filesystem::path{Common::U16StringFromBuffer(filename.utf16(), filename.size())};
        const auto config_file_name = title_id == 0
                                          ? Common::FS::PathToUTF8String(file_path.filename())
                                          : fmt::format("{:016X}", title_id);
        Config per_game_config(config_file_name, Config::ConfigType::PerGameConfig);
        system->HIDCore().ReloadInputDevices();
        system->ApplySettings();
    }

    Settings::LogSettings();

    if (UISettings::values.select_user_on_boot) {
        const Core::Frontend::ProfileSelectParameters parameters{
            .mode = Service::AM::Applets::UiMode::UserSelector,
            .invalid_uid_list = {},
            .display_options = {},
            .purpose = Service::AM::Applets::UserSelectionPurpose::General,
        };
        if (SelectAndSetCurrentUser(parameters) == false) {
            return;
        }
    }

    if (!LoadROM(filename, program_id, program_index)) {
        return;
    }

    system->SetShuttingDown(false);

    // Create and start the emulation thread
    emu_thread = std::make_unique<EmuThread>(*system);
    emit EmulationStarting(emu_thread.get());
    emu_thread->start();

    // Register an ExecuteProgram callback such that Core can execute a sub-program
    system->RegisterExecuteProgramCallback(
        [this](std::size_t program_index_) { render_window->ExecuteProgram(program_index_); });

    system->RegisterExitCallback([this] {
        emu_thread->ForceStop();
        render_window->Exit();
    });

    connect(render_window, &GRenderWindow::Closed, this, &GMainWindow::OnStopGame);
    connect(render_window, &GRenderWindow::MouseActivity, this, &GMainWindow::OnMouseActivity);
    // BlockingQueuedConnection is important here, it makes sure we've finished refreshing our views
    // before the CPU continues
    connect(emu_thread.get(), &EmuThread::DebugModeEntered, waitTreeWidget,
            &WaitTreeWidget::OnDebugModeEntered, Qt::BlockingQueuedConnection);
    connect(emu_thread.get(), &EmuThread::DebugModeLeft, waitTreeWidget,
            &WaitTreeWidget::OnDebugModeLeft, Qt::BlockingQueuedConnection);

    connect(emu_thread.get(), &EmuThread::LoadProgress, loading_screen,
            &LoadingScreen::OnLoadProgress, Qt::QueuedConnection);

    // Update the GUI
    UpdateStatusButtons();
    if (ui->action_Single_Window_Mode->isChecked()) {
        game_list->hide();
        game_list_placeholder->hide();
    }
    status_bar_update_timer.start(500);
    renderer_status_button->setDisabled(true);

    if (UISettings::values.hide_mouse || Settings::values.mouse_panning) {
        render_window->installEventFilter(render_window);
        render_window->setAttribute(Qt::WA_Hover, true);
    }

    if (UISettings::values.hide_mouse) {
        mouse_hide_timer.start();
    }

    render_window->InitializeCamera();

    std::string title_name;
    std::string title_version;
    const auto res = system->GetGameName(title_name);

    const auto metadata = [this, title_id] {
        const FileSys::PatchManager pm(title_id, system->GetFileSystemController(),
                                       system->GetContentProvider());
        return pm.GetControlMetadata();
    }();
    if (metadata.first != nullptr) {
        title_version = metadata.first->GetVersionString();
        title_name = metadata.first->GetApplicationName();
    }
    if (res != Loader::ResultStatus::Success || title_name.empty()) {
        title_name = Common::FS::PathToUTF8String(
            std::filesystem::path{Common::U16StringFromBuffer(filename.utf16(), filename.size())}
                .filename());
    }
    const bool is_64bit = system->Kernel().ApplicationProcess()->Is64BitProcess();
    const auto instruction_set_suffix = is_64bit ? tr("(64-bit)") : tr("(32-bit)");
    title_name = tr("%1 %2", "%1 is the title name. %2 indicates if the title is 64-bit or 32-bit")
                     .arg(QString::fromStdString(title_name), instruction_set_suffix)
                     .toStdString();
    LOG_INFO(Frontend, "Booting game: {:016X} | {} | {}", title_id, title_name, title_version);
    const auto gpu_vendor = system->GPU().Renderer().GetDeviceVendor();
    UpdateWindowTitle(title_name, title_version, gpu_vendor);

    loading_screen->Prepare(system->GetAppLoader());
    loading_screen->show();

    emulation_running = true;
    if (ui->action_Fullscreen->isChecked()) {
        ShowFullscreen();
    }
    OnStartGame();
}

bool GMainWindow::OnShutdownBegin() {
    if (!emulation_running) {
        return false;
    }

    if (ui->action_Fullscreen->isChecked()) {
        HideFullscreen();
    }

    AllowOSSleep();

    // Disable unlimited frame rate
    Settings::values.use_speed_limit.SetValue(true);

    if (system->IsShuttingDown()) {
        return false;
    }

    system->SetShuttingDown(true);
    discord_rpc->Pause();

    RequestGameExit();
    emu_thread->disconnect();
    emu_thread->SetRunning(true);

    emit EmulationStopping();

    shutdown_timer.setSingleShot(true);
    shutdown_timer.start(system->DebuggerEnabled() ? 0 : 5000);
    connect(&shutdown_timer, &QTimer::timeout, this, &GMainWindow::OnEmulationStopTimeExpired);
    connect(emu_thread.get(), &QThread::finished, this, &GMainWindow::OnEmulationStopped);

    // Disable everything to prevent anything from being triggered here
    ui->action_Pause->setEnabled(false);
    ui->action_Restart->setEnabled(false);
    ui->action_Stop->setEnabled(false);

    return true;
}

void GMainWindow::OnShutdownBeginDialog() {
    shutdown_dialog = new OverlayDialog(this, *system, QString{}, tr("Closing software..."),
                                        QString{}, QString{}, Qt::AlignHCenter | Qt::AlignVCenter);
    shutdown_dialog->open();
}

void GMainWindow::OnEmulationStopTimeExpired() {
    if (emu_thread) {
        emu_thread->ForceStop();
    }
}

void GMainWindow::OnEmulationStopped() {
    shutdown_timer.stop();
    if (emu_thread) {
        emu_thread->disconnect();
        emu_thread->wait();
        emu_thread.reset();
    }

    if (shutdown_dialog) {
        shutdown_dialog->deleteLater();
        shutdown_dialog = nullptr;
    }

    emulation_running = false;

    discord_rpc->Update();

    // The emulation is stopped, so closing the window or not does not matter anymore
    disconnect(render_window, &GRenderWindow::Closed, this, &GMainWindow::OnStopGame);

    // Update the GUI
    UpdateMenuState();

    render_window->hide();
    loading_screen->hide();
    loading_screen->Clear();
    if (game_list->IsEmpty()) {
        game_list_placeholder->show();
    } else {
        game_list->show();
    }
    game_list->SetFilterFocus();
    tas_label->clear();
    input_subsystem->GetTas()->Stop();
    OnTasStateChanged();
    render_window->FinalizeCamera();

    // Enable all controllers
    system->HIDCore().SetSupportedStyleTag({Core::HID::NpadStyleSet::All});

    render_window->removeEventFilter(render_window);
    render_window->setAttribute(Qt::WA_Hover, false);

    UpdateWindowTitle();

    // Disable status bar updates
    status_bar_update_timer.stop();
    shader_building_label->setVisible(false);
    res_scale_label->setVisible(false);
    emu_speed_label->setVisible(false);
    game_fps_label->setVisible(false);
    emu_frametime_label->setVisible(false);
    renderer_status_button->setEnabled(!UISettings::values.has_broken_vulkan);

    current_game_path.clear();

    // When closing the game, destroy the GLWindow to clear the context after the game is closed
    render_window->ReleaseRenderTarget();

    Settings::RestoreGlobalState(system->IsPoweredOn());
    system->HIDCore().ReloadInputDevices();
    UpdateStatusButtons();
}

void GMainWindow::ShutdownGame() {
    if (!emulation_running) {
        return;
    }

    OnShutdownBegin();
    OnEmulationStopTimeExpired();
    OnEmulationStopped();
}

void GMainWindow::StoreRecentFile(const QString& filename) {
    UISettings::values.recent_files.prepend(filename);
    UISettings::values.recent_files.removeDuplicates();
    while (UISettings::values.recent_files.size() > max_recent_files_item) {
        UISettings::values.recent_files.removeLast();
    }

    UpdateRecentFiles();
}

void GMainWindow::UpdateRecentFiles() {
    const int num_recent_files =
        std::min(static_cast<int>(UISettings::values.recent_files.size()), max_recent_files_item);

    for (int i = 0; i < num_recent_files; i++) {
        const QString text = QStringLiteral("&%1. %2").arg(i + 1).arg(
            QFileInfo(UISettings::values.recent_files[i]).fileName());
        actions_recent_files[i]->setText(text);
        actions_recent_files[i]->setData(UISettings::values.recent_files[i]);
        actions_recent_files[i]->setToolTip(UISettings::values.recent_files[i]);
        actions_recent_files[i]->setVisible(true);
    }

    for (int j = num_recent_files; j < max_recent_files_item; ++j) {
        actions_recent_files[j]->setVisible(false);
    }

    // Enable the recent files menu if the list isn't empty
    ui->menu_recent_files->setEnabled(num_recent_files != 0);
}

void GMainWindow::OnGameListLoadFile(QString game_path, u64 program_id) {
    BootGame(game_path, program_id);
}

void GMainWindow::OnGameListOpenFolder(u64 program_id, GameListOpenTarget target,
                                       const std::string& game_path) {
    std::filesystem::path path;
    QString open_target;

    const auto [user_save_size, device_save_size] = [this, &game_path, &program_id] {
        const FileSys::PatchManager pm{program_id, system->GetFileSystemController(),
                                       system->GetContentProvider()};
        const auto control = pm.GetControlMetadata().first;
        if (control != nullptr) {
            return std::make_pair(control->GetDefaultNormalSaveSize(),
                                  control->GetDeviceSaveDataSize());
        } else {
            const auto file = Core::GetGameFileFromPath(vfs, game_path);
            const auto loader = Loader::GetLoader(*system, file);

            FileSys::NACP nacp{};
            loader->ReadControlData(nacp);
            return std::make_pair(nacp.GetDefaultNormalSaveSize(), nacp.GetDeviceSaveDataSize());
        }
    }();

    const bool has_user_save{user_save_size > 0};
    const bool has_device_save{device_save_size > 0};

    ASSERT_MSG(has_user_save != has_device_save, "Game uses both user and device savedata?");

    switch (target) {
    case GameListOpenTarget::SaveData: {
        open_target = tr("Save Data");
        const auto nand_dir = Common::FS::GetYuzuPath(Common::FS::YuzuPath::NANDDir);
        auto vfs_nand_dir =
            vfs->OpenDirectory(Common::FS::PathToUTF8String(nand_dir), FileSys::Mode::Read);

        if (has_user_save) {
            // User save data
            const auto select_profile = [this] {
                const Core::Frontend::ProfileSelectParameters parameters{
                    .mode = Service::AM::Applets::UiMode::UserSelector,
                    .invalid_uid_list = {},
                    .display_options = {},
                    .purpose = Service::AM::Applets::UserSelectionPurpose::General,
                };
                QtProfileSelectionDialog dialog(system->HIDCore(), this, parameters);
                dialog.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                                      Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
                dialog.setWindowModality(Qt::WindowModal);

                if (dialog.exec() == QDialog::Rejected) {
                    return -1;
                }

                return dialog.GetIndex();
            };

            const auto index = select_profile();
            if (index == -1) {
                return;
            }

            Service::Account::ProfileManager manager;
            const auto user_id = manager.GetUser(static_cast<std::size_t>(index));
            ASSERT(user_id);

            const auto user_save_data_path = FileSys::SaveDataFactory::GetFullPath(
                *system, vfs_nand_dir, FileSys::SaveDataSpaceId::NandUser,
                FileSys::SaveDataType::SaveData, program_id, user_id->AsU128(), 0);

            path = Common::FS::ConcatPathSafe(nand_dir, user_save_data_path);
        } else {
            // Device save data
            const auto device_save_data_path = FileSys::SaveDataFactory::GetFullPath(
                *system, vfs_nand_dir, FileSys::SaveDataSpaceId::NandUser,
                FileSys::SaveDataType::SaveData, program_id, {}, 0);

            path = Common::FS::ConcatPathSafe(nand_dir, device_save_data_path);
        }

        if (!Common::FS::CreateDirs(path)) {
            LOG_ERROR(Frontend, "Unable to create the directories for save data");
        }

        break;
    }
    case GameListOpenTarget::ModData: {
        open_target = tr("Mod Data");
        path = Common::FS::GetYuzuPath(Common::FS::YuzuPath::LoadDir) /
               fmt::format("{:016X}", program_id);
        break;
    }
    default:
        UNIMPLEMENTED();
        break;
    }

    const QString qpath = QString::fromStdString(Common::FS::PathToUTF8String(path));
    const QDir dir(qpath);
    if (!dir.exists()) {
        QMessageBox::warning(this, tr("Error Opening %1 Folder").arg(open_target),
                             tr("Folder does not exist!"));
        return;
    }
    LOG_INFO(Frontend, "Opening {} path for program_id={:016x}", open_target.toStdString(),
             program_id);
    QDesktopServices::openUrl(QUrl::fromLocalFile(qpath));
}

void GMainWindow::OnTransferableShaderCacheOpenFile(u64 program_id) {
    const auto shader_cache_dir = Common::FS::GetYuzuPath(Common::FS::YuzuPath::ShaderDir);
    const auto shader_cache_folder_path{shader_cache_dir / fmt::format("{:016x}", program_id)};
    if (!Common::FS::CreateDirs(shader_cache_folder_path)) {
        QMessageBox::warning(this, tr("Error Opening Transferable Shader Cache"),
                             tr("Failed to create the shader cache directory for this title."));
        return;
    }
    const auto shader_path_string{Common::FS::PathToUTF8String(shader_cache_folder_path)};
    const auto qt_shader_cache_path = QString::fromStdString(shader_path_string);
    QDesktopServices::openUrl(QUrl::fromLocalFile(qt_shader_cache_path));
}

static std::size_t CalculateRomFSEntrySize(const FileSys::VirtualDir& dir, bool full) {
    std::size_t out = 0;

    for (const auto& subdir : dir->GetSubdirectories()) {
        out += 1 + CalculateRomFSEntrySize(subdir, full);
    }

    return out + (full ? dir->GetFiles().size() : 0);
}

static bool RomFSRawCopy(QProgressDialog& dialog, const FileSys::VirtualDir& src,
                         const FileSys::VirtualDir& dest, std::size_t block_size, bool full) {
    if (src == nullptr || dest == nullptr || !src->IsReadable() || !dest->IsWritable())
        return false;
    if (dialog.wasCanceled())
        return false;

    if (full) {
        for (const auto& file : src->GetFiles()) {
            const auto out = VfsDirectoryCreateFileWrapper(dest, file->GetName());
            if (!FileSys::VfsRawCopy(file, out, block_size))
                return false;
            dialog.setValue(dialog.value() + 1);
            if (dialog.wasCanceled())
                return false;
        }
    }

    for (const auto& dir : src->GetSubdirectories()) {
        const auto out = dest->CreateSubdirectory(dir->GetName());
        if (!RomFSRawCopy(dialog, dir, out, block_size, full))
            return false;
        dialog.setValue(dialog.value() + 1);
        if (dialog.wasCanceled())
            return false;
    }

    return true;
}

QString GMainWindow::GetGameListErrorRemoving(InstalledEntryType type) const {
    switch (type) {
    case InstalledEntryType::Game:
        return tr("Error Removing Contents");
    case InstalledEntryType::Update:
        return tr("Error Removing Update");
    case InstalledEntryType::AddOnContent:
        return tr("Error Removing DLC");
    default:
        return QStringLiteral("Error Removing <Invalid Type>");
    }
}
void GMainWindow::OnGameListRemoveInstalledEntry(u64 program_id, InstalledEntryType type) {
    const QString entry_question = [type] {
        switch (type) {
        case InstalledEntryType::Game:
            return tr("Remove Installed Game Contents?");
        case InstalledEntryType::Update:
            return tr("Remove Installed Game Update?");
        case InstalledEntryType::AddOnContent:
            return tr("Remove Installed Game DLC?");
        default:
            return QStringLiteral("Remove Installed Game <Invalid Type>?");
        }
    }();

    if (QMessageBox::question(this, tr("Remove Entry"), entry_question,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    switch (type) {
    case InstalledEntryType::Game:
        RemoveBaseContent(program_id, type);
        [[fallthrough]];
    case InstalledEntryType::Update:
        RemoveUpdateContent(program_id, type);
        if (type != InstalledEntryType::Game) {
            break;
        }
        [[fallthrough]];
    case InstalledEntryType::AddOnContent:
        RemoveAddOnContent(program_id, type);
        break;
    }
    Common::FS::RemoveDirRecursively(Common::FS::GetYuzuPath(Common::FS::YuzuPath::CacheDir) /
                                     "game_list");
    game_list->PopulateAsync(UISettings::values.game_dirs);
}

void GMainWindow::RemoveBaseContent(u64 program_id, InstalledEntryType type) {
    const auto& fs_controller = system->GetFileSystemController();
    const auto res = fs_controller.GetUserNANDContents()->RemoveExistingEntry(program_id) ||
                     fs_controller.GetSDMCContents()->RemoveExistingEntry(program_id);

    if (res) {
        QMessageBox::information(this, tr("Successfully Removed"),
                                 tr("Successfully removed the installed base game."));
    } else {
        QMessageBox::warning(
            this, GetGameListErrorRemoving(type),
            tr("The base game is not installed in the NAND and cannot be removed."));
    }
}

void GMainWindow::RemoveUpdateContent(u64 program_id, InstalledEntryType type) {
    const auto update_id = program_id | 0x800;
    const auto& fs_controller = system->GetFileSystemController();
    const auto res = fs_controller.GetUserNANDContents()->RemoveExistingEntry(update_id) ||
                     fs_controller.GetSDMCContents()->RemoveExistingEntry(update_id);

    if (res) {
        QMessageBox::information(this, tr("Successfully Removed"),
                                 tr("Successfully removed the installed update."));
    } else {
        QMessageBox::warning(this, GetGameListErrorRemoving(type),
                             tr("There is no update installed for this title."));
    }
}

void GMainWindow::RemoveAddOnContent(u64 program_id, InstalledEntryType type) {
    u32 count{};
    const auto& fs_controller = system->GetFileSystemController();
    const auto dlc_entries = system->GetContentProvider().ListEntriesFilter(
        FileSys::TitleType::AOC, FileSys::ContentRecordType::Data);

    for (const auto& entry : dlc_entries) {
        if (FileSys::GetBaseTitleID(entry.title_id) == program_id) {
            const auto res =
                fs_controller.GetUserNANDContents()->RemoveExistingEntry(entry.title_id) ||
                fs_controller.GetSDMCContents()->RemoveExistingEntry(entry.title_id);
            if (res) {
                ++count;
            }
        }
    }

    if (count == 0) {
        QMessageBox::warning(this, GetGameListErrorRemoving(type),
                             tr("There are no DLC installed for this title."));
        return;
    }

    QMessageBox::information(this, tr("Successfully Removed"),
                             tr("Successfully removed %1 installed DLC.").arg(count));
}

void GMainWindow::OnGameListRemoveFile(u64 program_id, GameListRemoveTarget target,
                                       const std::string& game_path) {
    const QString question = [target] {
        switch (target) {
        case GameListRemoveTarget::GlShaderCache:
            return tr("Delete OpenGL Transferable Shader Cache?");
        case GameListRemoveTarget::VkShaderCache:
            return tr("Delete Vulkan Transferable Shader Cache?");
        case GameListRemoveTarget::AllShaderCache:
            return tr("Delete All Transferable Shader Caches?");
        case GameListRemoveTarget::CustomConfiguration:
            return tr("Remove Custom Game Configuration?");
        default:
            return QString{};
        }
    }();

    if (QMessageBox::question(this, tr("Remove File"), question, QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    switch (target) {
    case GameListRemoveTarget::VkShaderCache:
        RemoveVulkanDriverPipelineCache(program_id);
        [[fallthrough]];
    case GameListRemoveTarget::GlShaderCache:
        RemoveTransferableShaderCache(program_id, target);
        break;
    case GameListRemoveTarget::AllShaderCache:
        RemoveAllTransferableShaderCaches(program_id);
        break;
    case GameListRemoveTarget::CustomConfiguration:
        RemoveCustomConfiguration(program_id, game_path);
        break;
    }
}

void GMainWindow::RemoveTransferableShaderCache(u64 program_id, GameListRemoveTarget target) {
    const auto target_file_name = [target] {
        switch (target) {
        case GameListRemoveTarget::GlShaderCache:
            return "opengl.bin";
        case GameListRemoveTarget::VkShaderCache:
            return "vulkan.bin";
        default:
            return "";
        }
    }();
    const auto shader_cache_dir = Common::FS::GetYuzuPath(Common::FS::YuzuPath::ShaderDir);
    const auto shader_cache_folder_path = shader_cache_dir / fmt::format("{:016x}", program_id);
    const auto target_file = shader_cache_folder_path / target_file_name;

    if (!Common::FS::Exists(target_file)) {
        QMessageBox::warning(this, tr("Error Removing Transferable Shader Cache"),
                             tr("A shader cache for this title does not exist."));
        return;
    }
    if (Common::FS::RemoveFile(target_file)) {
        QMessageBox::information(this, tr("Successfully Removed"),
                                 tr("Successfully removed the transferable shader cache."));
    } else {
        QMessageBox::warning(this, tr("Error Removing Transferable Shader Cache"),
                             tr("Failed to remove the transferable shader cache."));
    }
}

void GMainWindow::RemoveVulkanDriverPipelineCache(u64 program_id) {
    static constexpr std::string_view target_file_name = "vulkan_pipelines.bin";

    const auto shader_cache_dir = Common::FS::GetYuzuPath(Common::FS::YuzuPath::ShaderDir);
    const auto shader_cache_folder_path = shader_cache_dir / fmt::format("{:016x}", program_id);
    const auto target_file = shader_cache_folder_path / target_file_name;

    if (!Common::FS::Exists(target_file)) {
        return;
    }
    if (!Common::FS::RemoveFile(target_file)) {
        QMessageBox::warning(this, tr("Error Removing Vulkan Driver Pipeline Cache"),
                             tr("Failed to remove the driver pipeline cache."));
    }
}

void GMainWindow::RemoveAllTransferableShaderCaches(u64 program_id) {
    const auto shader_cache_dir = Common::FS::GetYuzuPath(Common::FS::YuzuPath::ShaderDir);
    const auto program_shader_cache_dir = shader_cache_dir / fmt::format("{:016x}", program_id);

    if (!Common::FS::Exists(program_shader_cache_dir)) {
        QMessageBox::warning(this, tr("Error Removing Transferable Shader Caches"),
                             tr("A shader cache for this title does not exist."));
        return;
    }
    if (Common::FS::RemoveDirRecursively(program_shader_cache_dir)) {
        QMessageBox::information(this, tr("Successfully Removed"),
                                 tr("Successfully removed the transferable shader caches."));
    } else {
        QMessageBox::warning(this, tr("Error Removing Transferable Shader Caches"),
                             tr("Failed to remove the transferable shader cache directory."));
    }
}

void GMainWindow::RemoveCustomConfiguration(u64 program_id, const std::string& game_path) {
    const auto file_path = std::filesystem::path(Common::FS::ToU8String(game_path));
    const auto config_file_name =
        program_id == 0 ? Common::FS::PathToUTF8String(file_path.filename()).append(".ini")
                        : fmt::format("{:016X}.ini", program_id);
    const auto custom_config_file_path =
        Common::FS::GetYuzuPath(Common::FS::YuzuPath::ConfigDir) / "custom" / config_file_name;

    if (!Common::FS::Exists(custom_config_file_path)) {
        QMessageBox::warning(this, tr("Error Removing Custom Configuration"),
                             tr("A custom configuration for this title does not exist."));
        return;
    }

    if (Common::FS::RemoveFile(custom_config_file_path)) {
        QMessageBox::information(this, tr("Successfully Removed"),
                                 tr("Successfully removed the custom game configuration."));
    } else {
        QMessageBox::warning(this, tr("Error Removing Custom Configuration"),
                             tr("Failed to remove the custom game configuration."));
    }
}

void GMainWindow::OnGameListDumpRomFS(u64 program_id, const std::string& game_path,
                                      DumpRomFSTarget target) {
    const auto failed = [this] {
        QMessageBox::warning(this, tr("RomFS Extraction Failed!"),
                             tr("There was an error copying the RomFS files or the user "
                                "cancelled the operation."));
    };

    const auto loader = Loader::GetLoader(*system, vfs->OpenFile(game_path, FileSys::Mode::Read));
    if (loader == nullptr) {
        failed();
        return;
    }

    FileSys::VirtualFile file;
    if (loader->ReadRomFS(file) != Loader::ResultStatus::Success) {
        failed();
        return;
    }

    const auto& installed = system->GetContentProvider();
    const auto romfs_title_id = SelectRomFSDumpTarget(installed, program_id);

    if (!romfs_title_id) {
        failed();
        return;
    }

    const auto dump_dir =
        target == DumpRomFSTarget::Normal
            ? Common::FS::GetYuzuPath(Common::FS::YuzuPath::DumpDir)
            : Common::FS::GetYuzuPath(Common::FS::YuzuPath::SDMCDir) / "atmosphere" / "contents";
    const auto romfs_dir = fmt::format("{:016X}/romfs", *romfs_title_id);

    const auto path = Common::FS::PathToUTF8String(dump_dir / romfs_dir);

    FileSys::VirtualFile romfs;

    if (*romfs_title_id == program_id) {
        const u64 ivfc_offset = loader->ReadRomFSIVFCOffset();
        const FileSys::PatchManager pm{program_id, system->GetFileSystemController(), installed};
        romfs =
            pm.PatchRomFS(file, ivfc_offset, FileSys::ContentRecordType::Program, nullptr, false);
    } else {
        romfs = installed.GetEntry(*romfs_title_id, FileSys::ContentRecordType::Data)->GetRomFS();
    }

    const auto extracted = FileSys::ExtractRomFS(romfs, FileSys::RomFSExtractionType::Full);
    if (extracted == nullptr) {
        failed();
        return;
    }

    const auto out = VfsFilesystemCreateDirectoryWrapper(vfs, path, FileSys::Mode::ReadWrite);

    if (out == nullptr) {
        failed();
        vfs->DeleteDirectory(path);
        return;
    }

    bool ok = false;
    const QStringList selections{tr("Full"), tr("Skeleton")};
    const auto res = QInputDialog::getItem(
        this, tr("Select RomFS Dump Mode"),
        tr("Please select the how you would like the RomFS dumped.<br>Full will copy all of the "
           "files into the new directory while <br>skeleton will only create the directory "
           "structure."),
        selections, 0, false, &ok);
    if (!ok) {
        failed();
        vfs->DeleteDirectory(path);
        return;
    }

    const auto full = res == selections.constFirst();
    const auto entry_size = CalculateRomFSEntrySize(extracted, full);

    // The minimum required space is the size of the extracted RomFS + 1 GiB
    const auto minimum_free_space = extracted->GetSize() + 0x40000000;

    if (full && Common::FS::GetFreeSpaceSize(path) < minimum_free_space) {
        QMessageBox::warning(this, tr("RomFS Extraction Failed!"),
                             tr("There is not enough free space at %1 to extract the RomFS. Please "
                                "free up space or select a different dump directory at "
                                "Emulation > Configure > System > Filesystem > Dump Root")
                                 .arg(QString::fromStdString(path)));
        return;
    }

    QProgressDialog progress(tr("Extracting RomFS..."), tr("Cancel"), 0,
                             static_cast<s32>(entry_size), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(100);

    if (RomFSRawCopy(progress, extracted, out, 0x400000, full)) {
        progress.close();
        QMessageBox::information(this, tr("RomFS Extraction Succeeded!"),
                                 tr("The operation completed successfully."));
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(path)));
    } else {
        progress.close();
        failed();
        vfs->DeleteDirectory(path);
    }
}

void GMainWindow::OnGameListCopyTID(u64 program_id) {
    QClipboard* clipboard = QGuiApplication::clipboard();
    clipboard->setText(QString::fromStdString(fmt::format("{:016X}", program_id)));
}

void GMainWindow::OnGameListNavigateToGamedbEntry(u64 program_id,
                                                  const CompatibilityList& compatibility_list) {
    const auto it = FindMatchingCompatibilityEntry(compatibility_list, program_id);

    QString directory;
    if (it != compatibility_list.end()) {
        directory = it->second.second;
    }

    QDesktopServices::openUrl(QUrl(QStringLiteral("https://yuzu-emu.org/game/") + directory));
}

void GMainWindow::OnGameListCreateShortcut(u64 program_id, const std::string& game_path,
                                           GameListShortcutTarget target) {
    // Get path to yuzu executable
    const QStringList args = QApplication::arguments();
    std::filesystem::path yuzu_command = args[0].toStdString();

#if defined(__linux__) || defined(__FreeBSD__)
    // If relative path, make it an absolute path
    if (yuzu_command.c_str()[0] == '.') {
        yuzu_command = Common::FS::GetCurrentDir() / yuzu_command;
    }

#if defined(__linux__)
    // Warn once if we are making a shortcut to a volatile AppImage
    const std::string appimage_ending =
        std::string(Common::g_scm_rev).substr(0, 9).append(".AppImage");
    if (yuzu_command.string().ends_with(appimage_ending) &&
        !UISettings::values.shortcut_already_warned) {
        if (QMessageBox::warning(this, tr("Create Shortcut"),
                                 tr("This will create a shortcut to the current AppImage. This may "
                                    "not work well if you update. Continue?"),
                                 QMessageBox::StandardButton::Ok |
                                     QMessageBox::StandardButton::Cancel) ==
            QMessageBox::StandardButton::Cancel) {
            return;
        }
        UISettings::values.shortcut_already_warned = true;
    }
#endif // __linux__
#endif // __linux__ || __FreeBSD__

    std::filesystem::path target_directory{};
    // Determine target directory for shortcut
#if defined(__linux__) || defined(__FreeBSD__)
    const char* home = std::getenv("HOME");
    const std::filesystem::path home_path = (home == nullptr ? "~" : home);
    const char* xdg_data_home = std::getenv("XDG_DATA_HOME");

    if (target == GameListShortcutTarget::Desktop) {
        target_directory = home_path / "Desktop";
        if (!Common::FS::IsDir(target_directory)) {
            QMessageBox::critical(
                this, tr("Create Shortcut"),
                tr("Cannot create shortcut on desktop. Path \"%1\" does not exist.")
                    .arg(QString::fromStdString(target_directory)),
                QMessageBox::StandardButton::Ok);
            return;
        }
    } else if (target == GameListShortcutTarget::Applications) {
        target_directory = (xdg_data_home == nullptr ? home_path / ".local/share" : xdg_data_home) /
                           "applications";
        if (!Common::FS::CreateDirs(target_directory)) {
            QMessageBox::critical(this, tr("Create Shortcut"),
                                  tr("Cannot create shortcut in applications menu. Path \"%1\" "
                                     "does not exist and cannot be created.")
                                      .arg(QString::fromStdString(target_directory)),
                                  QMessageBox::StandardButton::Ok);
            return;
        }
    }
#endif

    const std::string game_file_name = std::filesystem::path(game_path).filename().string();
    // Determine full paths for icon and shortcut
#if defined(__linux__) || defined(__FreeBSD__)
    std::filesystem::path system_icons_path =
        (xdg_data_home == nullptr ? home_path / ".local/share/" : xdg_data_home) /
        "icons/hicolor/256x256";
    if (!Common::FS::CreateDirs(system_icons_path)) {
        QMessageBox::critical(
            this, tr("Create Icon"),
            tr("Cannot create icon file. Path \"%1\" does not exist and cannot be created.")
                .arg(QString::fromStdString(system_icons_path)),
            QMessageBox::StandardButton::Ok);
        return;
    }
    std::filesystem::path icon_path =
        system_icons_path / (program_id == 0 ? fmt::format("yuzu-{}.png", game_file_name)
                                             : fmt::format("yuzu-{:016X}.png", program_id));
    const std::filesystem::path shortcut_path =
        target_directory / (program_id == 0 ? fmt::format("yuzu-{}.desktop", game_file_name)
                                            : fmt::format("yuzu-{:016X}.desktop", program_id));
#else
    const std::filesystem::path icon_path{};
    const std::filesystem::path shortcut_path{};
#endif

    // Get title from game file
    const FileSys::PatchManager pm{program_id, system->GetFileSystemController(),
                                   system->GetContentProvider()};
    const auto control = pm.GetControlMetadata();
    const auto loader = Loader::GetLoader(*system, vfs->OpenFile(game_path, FileSys::Mode::Read));

    std::string title{fmt::format("{:016X}", program_id)};

    if (control.first != nullptr) {
        title = control.first->GetApplicationName();
    } else {
        loader->ReadTitle(title);
    }

    // Get icon from game file
    std::vector<u8> icon_image_file{};
    if (control.second != nullptr) {
        icon_image_file = control.second->ReadAllBytes();
    } else if (loader->ReadIcon(icon_image_file) != Loader::ResultStatus::Success) {
        LOG_WARNING(Frontend, "Could not read icon from {:s}", game_path);
    }

    QImage icon_jpeg =
        QImage::fromData(icon_image_file.data(), static_cast<int>(icon_image_file.size()));
#if defined(__linux__) || defined(__FreeBSD__)
    // Convert and write the icon as a PNG
    if (!icon_jpeg.save(QString::fromStdString(icon_path.string()))) {
        LOG_ERROR(Frontend, "Could not write icon as PNG to file");
    } else {
        LOG_INFO(Frontend, "Wrote an icon to {}", icon_path.string());
    }
#endif // __linux__

#if defined(__linux__) || defined(__FreeBSD__)
    const std::string comment =
        tr("Start %1 with the yuzu Emulator").arg(QString::fromStdString(title)).toStdString();
    const std::string arguments = fmt::format("-g \"{:s}\"", game_path);
    const std::string categories = "Game;Emulator;Qt;";
    const std::string keywords = "Switch;Nintendo;";
#else
    const std::string comment{};
    const std::string arguments{};
    const std::string categories{};
    const std::string keywords{};
#endif
    if (!CreateShortcut(shortcut_path.string(), title, comment, icon_path.string(),
                        yuzu_command.string(), arguments, categories, keywords)) {
        QMessageBox::critical(this, tr("Create Shortcut"),
                              tr("Failed to create a shortcut at %1")
                                  .arg(QString::fromStdString(shortcut_path.string())));
        return;
    }

    LOG_INFO(Frontend, "Wrote a shortcut to {}", shortcut_path.string());
    QMessageBox::information(
        this, tr("Create Shortcut"),
        tr("Successfully created a shortcut to %1").arg(QString::fromStdString(title)));
}

void GMainWindow::OnGameListOpenDirectory(const QString& directory) {
    std::filesystem::path fs_path;
    if (directory == QStringLiteral("SDMC")) {
        fs_path =
            Common::FS::GetYuzuPath(Common::FS::YuzuPath::SDMCDir) / "Nintendo/Contents/registered";
    } else if (directory == QStringLiteral("UserNAND")) {
        fs_path =
            Common::FS::GetYuzuPath(Common::FS::YuzuPath::NANDDir) / "user/Contents/registered";
    } else if (directory == QStringLiteral("SysNAND")) {
        fs_path =
            Common::FS::GetYuzuPath(Common::FS::YuzuPath::NANDDir) / "system/Contents/registered";
    } else {
        fs_path = directory.toStdString();
    }

    const auto qt_path = QString::fromStdString(Common::FS::PathToUTF8String(fs_path));

    if (!Common::FS::IsDir(fs_path)) {
        QMessageBox::critical(this, tr("Error Opening %1").arg(qt_path),
                              tr("Folder does not exist!"));
        return;
    }

    QDesktopServices::openUrl(QUrl::fromLocalFile(qt_path));
}

void GMainWindow::OnGameListAddDirectory() {
    const QString dir_path = QFileDialog::getExistingDirectory(this, tr("Select Directory"));
    if (dir_path.isEmpty()) {
        return;
    }

    UISettings::GameDir game_dir{dir_path, false, true};
    if (!UISettings::values.game_dirs.contains(game_dir)) {
        UISettings::values.game_dirs.append(game_dir);
        game_list->PopulateAsync(UISettings::values.game_dirs);
    } else {
        LOG_WARNING(Frontend, "Selected directory is already in the game list");
    }

    OnSaveConfig();
}

void GMainWindow::OnGameListShowList(bool show) {
    if (emulation_running && ui->action_Single_Window_Mode->isChecked())
        return;
    game_list->setVisible(show);
    game_list_placeholder->setVisible(!show);
};

void GMainWindow::OnGameListOpenPerGameProperties(const std::string& file) {
    u64 title_id{};
    const auto v_file = Core::GetGameFileFromPath(vfs, file);
    const auto loader = Loader::GetLoader(*system, v_file);

    if (loader == nullptr || loader->ReadProgramId(title_id) != Loader::ResultStatus::Success) {
        QMessageBox::information(this, tr("Properties"),
                                 tr("The game properties could not be loaded."));
        return;
    }

    OpenPerGameConfiguration(title_id, file);
}

void GMainWindow::OnMenuLoadFile() {
    if (is_load_file_select_active) {
        return;
    }

    is_load_file_select_active = true;
    const QString extensions =
        QStringLiteral("*.")
            .append(GameList::supported_file_extensions.join(QStringLiteral(" *.")))
            .append(QStringLiteral(" main"));
    const QString file_filter = tr("Switch Executable (%1);;All Files (*.*)",
                                   "%1 is an identifier for the Switch executable file extensions.")
                                    .arg(extensions);
    const QString filename = QFileDialog::getOpenFileName(
        this, tr("Load File"), UISettings::values.roms_path, file_filter);
    is_load_file_select_active = false;

    if (filename.isEmpty()) {
        return;
    }

    UISettings::values.roms_path = QFileInfo(filename).path();
    BootGame(filename);
}

void GMainWindow::OnMenuLoadFolder() {
    const QString dir_path =
        QFileDialog::getExistingDirectory(this, tr("Open Extracted ROM Directory"));

    if (dir_path.isNull()) {
        return;
    }

    const QDir dir{dir_path};
    const QStringList matching_main = dir.entryList({QStringLiteral("main")}, QDir::Files);
    if (matching_main.size() == 1) {
        BootGame(dir.path() + QDir::separator() + matching_main[0]);
    } else {
        QMessageBox::warning(this, tr("Invalid Directory Selected"),
                             tr("The directory you have selected does not contain a 'main' file."));
    }
}

void GMainWindow::IncrementInstallProgress() {
    install_progress->setValue(install_progress->value() + 1);
}

void GMainWindow::OnMenuInstallToNAND() {
    const QString file_filter =
        tr("Installable Switch File (*.nca *.nsp *.xci);;Nintendo Content Archive "
           "(*.nca);;Nintendo Submission Package (*.nsp);;NX Cartridge "
           "Image (*.xci)");

    QStringList filenames = QFileDialog::getOpenFileNames(
        this, tr("Install Files"), UISettings::values.roms_path, file_filter);

    if (filenames.isEmpty()) {
        return;
    }

    InstallDialog installDialog(this, filenames);
    if (installDialog.exec() == QDialog::Rejected) {
        return;
    }

    const QStringList files = installDialog.GetFiles();

    if (files.isEmpty()) {
        return;
    }

    // Save folder location of the first selected file
    UISettings::values.roms_path = QFileInfo(filenames[0]).path();

    int remaining = filenames.size();

    // This would only overflow above 2^43 bytes (8.796 TB)
    int total_size = 0;
    for (const QString& file : files) {
        total_size += static_cast<int>(QFile(file).size() / 0x1000);
    }
    if (total_size < 0) {
        LOG_CRITICAL(Frontend, "Attempting to install too many files, aborting.");
        return;
    }

    QStringList new_files{};         // Newly installed files that do not yet exist in the NAND
    QStringList overwritten_files{}; // Files that overwrote those existing in the NAND
    QStringList failed_files{};      // Files that failed to install due to errors
    bool detected_base_install{};    // Whether a base game was attempted to be installed

    ui->action_Install_File_NAND->setEnabled(false);

    install_progress = new QProgressDialog(QString{}, tr("Cancel"), 0, total_size, this);
    install_progress->setWindowFlags(windowFlags() & ~Qt::WindowMaximizeButtonHint);
    install_progress->setAttribute(Qt::WA_DeleteOnClose, true);
    install_progress->setFixedWidth(installDialog.GetMinimumWidth() + 40);
    install_progress->show();

    for (const QString& file : files) {
        install_progress->setWindowTitle(tr("%n file(s) remaining", "", remaining));
        install_progress->setLabelText(
            tr("Installing file \"%1\"...").arg(QFileInfo(file).fileName()));

        QFuture<InstallResult> future;
        InstallResult result;

        if (file.endsWith(QStringLiteral("xci"), Qt::CaseInsensitive) ||
            file.endsWith(QStringLiteral("nsp"), Qt::CaseInsensitive)) {

            future = QtConcurrent::run([this, &file] { return InstallNSPXCI(file); });

            while (!future.isFinished()) {
                QCoreApplication::processEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            result = future.result();

        } else {
            result = InstallNCA(file);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        switch (result) {
        case InstallResult::Success:
            new_files.append(QFileInfo(file).fileName());
            break;
        case InstallResult::Overwrite:
            overwritten_files.append(QFileInfo(file).fileName());
            break;
        case InstallResult::Failure:
            failed_files.append(QFileInfo(file).fileName());
            break;
        case InstallResult::BaseInstallAttempted:
            failed_files.append(QFileInfo(file).fileName());
            detected_base_install = true;
            break;
        }

        --remaining;
    }

    install_progress->close();

    if (detected_base_install) {
        QMessageBox::warning(
            this, tr("Install Results"),
            tr("To avoid possible conflicts, we discourage users from installing base games to the "
               "NAND.\nPlease, only use this feature to install updates and DLC."));
    }

    const QString install_results =
        (new_files.isEmpty() ? QString{}
                             : tr("%n file(s) were newly installed\n", "", new_files.size())) +
        (overwritten_files.isEmpty()
             ? QString{}
             : tr("%n file(s) were overwritten\n", "", overwritten_files.size())) +
        (failed_files.isEmpty() ? QString{}
                                : tr("%n file(s) failed to install\n", "", failed_files.size()));

    QMessageBox::information(this, tr("Install Results"), install_results);
    Common::FS::RemoveDirRecursively(Common::FS::GetYuzuPath(Common::FS::YuzuPath::CacheDir) /
                                     "game_list");
    game_list->PopulateAsync(UISettings::values.game_dirs);
    ui->action_Install_File_NAND->setEnabled(true);
}

InstallResult GMainWindow::InstallNSPXCI(const QString& filename) {
    const auto qt_raw_copy = [this](const FileSys::VirtualFile& src,
                                    const FileSys::VirtualFile& dest, std::size_t block_size) {
        if (src == nullptr || dest == nullptr) {
            return false;
        }
        if (!dest->Resize(src->GetSize())) {
            return false;
        }

        std::array<u8, 0x1000> buffer{};

        for (std::size_t i = 0; i < src->GetSize(); i += buffer.size()) {
            if (install_progress->wasCanceled()) {
                dest->Resize(0);
                return false;
            }

            emit UpdateInstallProgress();

            const auto read = src->Read(buffer.data(), buffer.size(), i);
            dest->Write(buffer.data(), read, i);
        }
        return true;
    };

    std::shared_ptr<FileSys::NSP> nsp;
    if (filename.endsWith(QStringLiteral("nsp"), Qt::CaseInsensitive)) {
        nsp = std::make_shared<FileSys::NSP>(
            vfs->OpenFile(filename.toStdString(), FileSys::Mode::Read));
        if (nsp->IsExtractedType()) {
            return InstallResult::Failure;
        }
    } else {
        const auto xci = std::make_shared<FileSys::XCI>(
            vfs->OpenFile(filename.toStdString(), FileSys::Mode::Read));
        nsp = xci->GetSecurePartitionNSP();
    }

    if (nsp->GetStatus() != Loader::ResultStatus::Success) {
        return InstallResult::Failure;
    }
    const auto res = system->GetFileSystemController().GetUserNANDContents()->InstallEntry(
        *nsp, true, qt_raw_copy);
    switch (res) {
    case FileSys::InstallResult::Success:
        return InstallResult::Success;
    case FileSys::InstallResult::OverwriteExisting:
        return InstallResult::Overwrite;
    case FileSys::InstallResult::ErrorBaseInstall:
        return InstallResult::BaseInstallAttempted;
    default:
        return InstallResult::Failure;
    }
}

InstallResult GMainWindow::InstallNCA(const QString& filename) {
    const auto qt_raw_copy = [this](const FileSys::VirtualFile& src,
                                    const FileSys::VirtualFile& dest, std::size_t block_size) {
        if (src == nullptr || dest == nullptr) {
            return false;
        }
        if (!dest->Resize(src->GetSize())) {
            return false;
        }

        std::array<u8, 0x1000> buffer{};

        for (std::size_t i = 0; i < src->GetSize(); i += buffer.size()) {
            if (install_progress->wasCanceled()) {
                dest->Resize(0);
                return false;
            }

            emit UpdateInstallProgress();

            const auto read = src->Read(buffer.data(), buffer.size(), i);
            dest->Write(buffer.data(), read, i);
        }
        return true;
    };

    const auto nca =
        std::make_shared<FileSys::NCA>(vfs->OpenFile(filename.toStdString(), FileSys::Mode::Read));
    const auto id = nca->GetStatus();

    // Game updates necessary are missing base RomFS
    if (id != Loader::ResultStatus::Success &&
        id != Loader::ResultStatus::ErrorMissingBKTRBaseRomFS) {
        return InstallResult::Failure;
    }

    const QStringList tt_options{tr("System Application"),
                                 tr("System Archive"),
                                 tr("System Application Update"),
                                 tr("Firmware Package (Type A)"),
                                 tr("Firmware Package (Type B)"),
                                 tr("Game"),
                                 tr("Game Update"),
                                 tr("Game DLC"),
                                 tr("Delta Title")};
    bool ok;
    const auto item = QInputDialog::getItem(
        this, tr("Select NCA Install Type..."),
        tr("Please select the type of title you would like to install this NCA as:\n(In "
           "most instances, the default 'Game' is fine.)"),
        tt_options, 5, false, &ok);

    auto index = tt_options.indexOf(item);
    if (!ok || index == -1) {
        QMessageBox::warning(this, tr("Failed to Install"),
                             tr("The title type you selected for the NCA is invalid."));
        return InstallResult::Failure;
    }

    // If index is equal to or past Game, add the jump in TitleType.
    if (index >= 5) {
        index += static_cast<size_t>(FileSys::TitleType::Application) -
                 static_cast<size_t>(FileSys::TitleType::FirmwarePackageB);
    }

    const bool is_application = index >= static_cast<s32>(FileSys::TitleType::Application);
    const auto& fs_controller = system->GetFileSystemController();
    auto* registered_cache = is_application ? fs_controller.GetUserNANDContents()
                                            : fs_controller.GetSystemNANDContents();

    const auto res = registered_cache->InstallEntry(*nca, static_cast<FileSys::TitleType>(index),
                                                    true, qt_raw_copy);
    if (res == FileSys::InstallResult::Success) {
        return InstallResult::Success;
    } else if (res == FileSys::InstallResult::OverwriteExisting) {
        return InstallResult::Overwrite;
    } else {
        return InstallResult::Failure;
    }
}

void GMainWindow::OnMenuRecentFile() {
    QAction* action = qobject_cast<QAction*>(sender());
    assert(action);

    const QString filename = action->data().toString();
    if (QFileInfo::exists(filename)) {
        BootGame(filename);
    } else {
        // Display an error message and remove the file from the list.
        QMessageBox::information(this, tr("File not found"),
                                 tr("File \"%1\" not found").arg(filename));

        UISettings::values.recent_files.removeOne(filename);
        UpdateRecentFiles();
    }
}

void GMainWindow::OnStartGame() {
    PreventOSSleep();

    emu_thread->SetRunning(true);

    UpdateMenuState();
    OnTasStateChanged();

    discord_rpc->Update();
}

void GMainWindow::OnRestartGame() {
    if (!system->IsPoweredOn()) {
        return;
    }
    // Make a copy since ShutdownGame edits game_path
    const auto current_game = QString(current_game_path);
    ShutdownGame();
    BootGame(current_game);
}

void GMainWindow::OnPauseGame() {
    emu_thread->SetRunning(false);
    UpdateMenuState();
    AllowOSSleep();
}

void GMainWindow::OnPauseContinueGame() {
    if (emulation_running) {
        if (emu_thread->IsRunning()) {
            OnPauseGame();
        } else {
            RequestGameResume();
            OnStartGame();
        }
    }
}

void GMainWindow::OnStopGame() {
    if (system->GetExitLock() && !ConfirmForceLockedExit()) {
        return;
    }

    if (OnShutdownBegin()) {
        OnShutdownBeginDialog();
    } else {
        OnEmulationStopped();
    }
}

void GMainWindow::OnLoadComplete() {
    loading_screen->OnLoadComplete();
}

void GMainWindow::OnExecuteProgram(std::size_t program_index) {
    ShutdownGame();
    BootGame(last_filename_booted, 0, program_index);
}

void GMainWindow::OnExit() {
    OnStopGame();
}

void GMainWindow::OnSaveConfig() {
    system->ApplySettings();
    config->Save();
}

void GMainWindow::ErrorDisplayDisplayError(QString error_code, QString error_text) {
    error_applet = new OverlayDialog(render_window, *system, error_code, error_text, QString{},
                                     tr("OK"), Qt::AlignLeft | Qt::AlignVCenter);
    SCOPE_EXIT({
        error_applet->deleteLater();
        error_applet = nullptr;
    });
    error_applet->exec();

    emit ErrorDisplayFinished();
}

void GMainWindow::ErrorDisplayRequestExit() {
    if (error_applet) {
        error_applet->reject();
    }
}

void GMainWindow::OnMenuReportCompatibility() {
#if defined(ARCHITECTURE_x86_64) && !defined(__APPLE__)
    const auto& caps = Common::GetCPUCaps();
    const bool has_fma = caps.fma || caps.fma4;
    const auto processor_count = std::thread::hardware_concurrency();
    const bool has_4threads = processor_count == 0 || processor_count >= 4;
    const bool has_8gb_ram = Common::GetMemInfo().TotalPhysicalMemory >= 8_GiB;
    const bool has_broken_vulkan = UISettings::values.has_broken_vulkan;

    if (!has_fma || !has_4threads || !has_8gb_ram || has_broken_vulkan) {
        QMessageBox::critical(this, tr("Hardware requirements not met"),
                              tr("Your system does not meet the recommended hardware requirements. "
                                 "Compatibility reporting has been disabled."));
        return;
    }

    if (!Settings::values.yuzu_token.GetValue().empty() &&
        !Settings::values.yuzu_username.GetValue().empty()) {
        CompatDB compatdb{system->TelemetrySession(), this};
        compatdb.exec();
    } else {
        QMessageBox::critical(
            this, tr("Missing yuzu Account"),
            tr("In order to submit a game compatibility test case, you must link your yuzu "
               "account.<br><br/>To link your yuzu account, go to Emulation &gt; Configuration "
               "&gt; "
               "Web."));
    }
#else
    QMessageBox::critical(this, tr("Hardware requirements not met"),
                          tr("Your system does not meet the recommended hardware requirements. "
                             "Compatibility reporting has been disabled."));
#endif
}

void GMainWindow::OpenURL(const QUrl& url) {
    const bool open = QDesktopServices::openUrl(url);
    if (!open) {
        QMessageBox::warning(this, tr("Error opening URL"),
                             tr("Unable to open the URL \"%1\".").arg(url.toString()));
    }
}

void GMainWindow::OnOpenModsPage() {
    OpenURL(QUrl(QStringLiteral("https://github.com/yuzu-emu/yuzu/wiki/Switch-Mods")));
}

void GMainWindow::OnOpenQuickstartGuide() {
    OpenURL(QUrl(QStringLiteral("https://yuzu-emu.org/help/quickstart/")));
}

void GMainWindow::OnOpenFAQ() {
    OpenURL(QUrl(QStringLiteral("https://yuzu-emu.org/wiki/faq/")));
}

void GMainWindow::ToggleFullscreen() {
    if (!emulation_running) {
        return;
    }
    if (ui->action_Fullscreen->isChecked()) {
        ShowFullscreen();
    } else {
        HideFullscreen();
    }
}

// We're going to return the screen that the given window has the most pixels on
static QScreen* GuessCurrentScreen(QWidget* window) {
    const QList<QScreen*> screens = QGuiApplication::screens();
    return *std::max_element(
        screens.cbegin(), screens.cend(), [window](const QScreen* left, const QScreen* right) {
            const QSize left_size = left->geometry().intersected(window->geometry()).size();
            const QSize right_size = right->geometry().intersected(window->geometry()).size();
            return (left_size.height() * left_size.width()) <
                   (right_size.height() * right_size.width());
        });
}

bool GMainWindow::UsingExclusiveFullscreen() {
    return Settings::values.fullscreen_mode.GetValue() == Settings::FullscreenMode::Exclusive ||
           QGuiApplication::platformName() == QStringLiteral("wayland") ||
           QGuiApplication::platformName() == QStringLiteral("wayland-egl");
}

void GMainWindow::ShowFullscreen() {
    const auto show_fullscreen = [this](QWidget* window) {
        if (UsingExclusiveFullscreen()) {
            window->showFullScreen();
            return;
        }
        window->hide();
        window->setWindowFlags(window->windowFlags() | Qt::FramelessWindowHint);
        const auto screen_geometry = GuessCurrentScreen(window)->geometry();
        window->setGeometry(screen_geometry.x(), screen_geometry.y(), screen_geometry.width(),
                            screen_geometry.height() + 1);
        window->raise();
        window->showNormal();
    };

    if (ui->action_Single_Window_Mode->isChecked()) {
        UISettings::values.geometry = saveGeometry();

        ui->menubar->hide();
        statusBar()->hide();

        show_fullscreen(this);
    } else {
        UISettings::values.renderwindow_geometry = render_window->saveGeometry();
        show_fullscreen(render_window);
    }
}

void GMainWindow::HideFullscreen() {
    if (ui->action_Single_Window_Mode->isChecked()) {
        if (UsingExclusiveFullscreen()) {
            showNormal();
            restoreGeometry(UISettings::values.geometry);
        } else {
            hide();
            setWindowFlags(windowFlags() & ~Qt::FramelessWindowHint);
            restoreGeometry(UISettings::values.geometry);
            raise();
            show();
        }

        statusBar()->setVisible(ui->action_Show_Status_Bar->isChecked());
        ui->menubar->show();
    } else {
        if (UsingExclusiveFullscreen()) {
            render_window->showNormal();
            render_window->restoreGeometry(UISettings::values.renderwindow_geometry);
        } else {
            render_window->hide();
            render_window->setWindowFlags(windowFlags() & ~Qt::FramelessWindowHint);
            render_window->restoreGeometry(UISettings::values.renderwindow_geometry);
            render_window->raise();
            render_window->show();
        }
    }
}

void GMainWindow::ToggleWindowMode() {
    if (ui->action_Single_Window_Mode->isChecked()) {
        // Render in the main window...
        render_window->BackupGeometry();
        ui->horizontalLayout->addWidget(render_window);
        render_window->setFocusPolicy(Qt::StrongFocus);
        if (emulation_running) {
            render_window->setVisible(true);
            render_window->setFocus();
            game_list->hide();
        }

    } else {
        // Render in a separate window...
        ui->horizontalLayout->removeWidget(render_window);
        render_window->setParent(nullptr);
        render_window->setFocusPolicy(Qt::NoFocus);
        if (emulation_running) {
            render_window->setVisible(true);
            render_window->RestoreGeometry();
            game_list->show();
        }
    }
}

void GMainWindow::ResetWindowSize(u32 width, u32 height) {
    const auto aspect_ratio = Layout::EmulationAspectRatio(
        static_cast<Layout::AspectRatio>(Settings::values.aspect_ratio.GetValue()),
        static_cast<float>(height) / width);
    if (!ui->action_Single_Window_Mode->isChecked()) {
        render_window->resize(height / aspect_ratio, height);
    } else {
        const bool show_status_bar = ui->action_Show_Status_Bar->isChecked();
        const auto status_bar_height = show_status_bar ? statusBar()->height() : 0;
        resize(height / aspect_ratio, height + menuBar()->height() + status_bar_height);
    }
}

void GMainWindow::ResetWindowSize720() {
    ResetWindowSize(Layout::ScreenUndocked::Width, Layout::ScreenUndocked::Height);
}

void GMainWindow::ResetWindowSize900() {
    ResetWindowSize(1600U, 900U);
}

void GMainWindow::ResetWindowSize1080() {
    ResetWindowSize(Layout::ScreenDocked::Width, Layout::ScreenDocked::Height);
}

void GMainWindow::OnConfigure() {
    const auto old_theme = UISettings::values.theme;
    const bool old_discord_presence = UISettings::values.enable_discord_presence.GetValue();

    Settings::SetConfiguringGlobal(true);
    ConfigureDialog configure_dialog(this, hotkey_registry, input_subsystem.get(), *system,
                                     !multiplayer_state->IsHostingPublicRoom());
    connect(&configure_dialog, &ConfigureDialog::LanguageChanged, this,
            &GMainWindow::OnLanguageChanged);

    const auto result = configure_dialog.exec();
    if (result != QDialog::Accepted && !UISettings::values.configuration_applied &&
        !UISettings::values.reset_to_defaults) {
        // Runs if the user hit Cancel or closed the window, and did not ever press the Apply button
        // or `Reset to Defaults` button
        return;
    } else if (result == QDialog::Accepted) {
        // Only apply new changes if user hit Okay
        // This is here to avoid applying changes if the user hit Apply, made some changes, then hit
        // Cancel
        configure_dialog.ApplyConfiguration();
    } else if (UISettings::values.reset_to_defaults) {
        LOG_INFO(Frontend, "Resetting all settings to defaults");
        if (!Common::FS::RemoveFile(config->GetConfigFilePath())) {
            LOG_WARNING(Frontend, "Failed to remove configuration file");
        }
        if (!Common::FS::RemoveDirContentsRecursively(
                Common::FS::GetYuzuPath(Common::FS::YuzuPath::ConfigDir) / "custom")) {
            LOG_WARNING(Frontend, "Failed to remove custom configuration files");
        }
        if (!Common::FS::RemoveDirRecursively(
                Common::FS::GetYuzuPath(Common::FS::YuzuPath::CacheDir) / "game_list")) {
            LOG_WARNING(Frontend, "Failed to remove game metadata cache files");
        }

        // Explicitly save the game directories, since reinitializing config does not explicitly do
        // so.
        QVector<UISettings::GameDir> old_game_dirs = std::move(UISettings::values.game_dirs);
        QVector<u64> old_favorited_ids = std::move(UISettings::values.favorited_ids);

        Settings::values.disabled_addons.clear();

        config = std::make_unique<Config>();
        UISettings::values.reset_to_defaults = false;

        UISettings::values.game_dirs = std::move(old_game_dirs);
        UISettings::values.favorited_ids = std::move(old_favorited_ids);

        InitializeRecentFileMenuActions();

        SetDefaultUIGeometry();
        RestoreUIState();

        ShowTelemetryCallout();
    }
    InitializeHotkeys();

    if (UISettings::values.theme != old_theme) {
        UpdateUITheme();
    }
    if (UISettings::values.enable_discord_presence.GetValue() != old_discord_presence) {
        SetDiscordEnabled(UISettings::values.enable_discord_presence.GetValue());
    }

    if (!multiplayer_state->IsHostingPublicRoom()) {
        multiplayer_state->UpdateCredentials();
    }

    emit UpdateThemedIcons();

    const auto reload = UISettings::values.is_game_list_reload_pending.exchange(false);
    if (reload) {
        game_list->PopulateAsync(UISettings::values.game_dirs);
    }

    UISettings::values.configuration_applied = false;

    config->Save();

    if ((UISettings::values.hide_mouse || Settings::values.mouse_panning) && emulation_running) {
        render_window->installEventFilter(render_window);
        render_window->setAttribute(Qt::WA_Hover, true);
    } else {
        render_window->removeEventFilter(render_window);
        render_window->setAttribute(Qt::WA_Hover, false);
    }

    if (UISettings::values.hide_mouse) {
        mouse_hide_timer.start();
    }

    // Restart camera config
    if (emulation_running) {
        render_window->FinalizeCamera();
        render_window->InitializeCamera();
    }

    if (!UISettings::values.has_broken_vulkan) {
        renderer_status_button->setEnabled(!emulation_running);
    }

    UpdateStatusButtons();
    controller_dialog->refreshConfiguration();
    system->ApplySettings();
}

void GMainWindow::OnConfigureTas() {
    ConfigureTasDialog dialog(this);
    const auto result = dialog.exec();

    if (result != QDialog::Accepted && !UISettings::values.configuration_applied) {
        Settings::RestoreGlobalState(system->IsPoweredOn());
        return;
    } else if (result == QDialog::Accepted) {
        dialog.ApplyConfiguration();
        OnSaveConfig();
    }
}

void GMainWindow::OnTasStartStop() {
    if (!emulation_running) {
        return;
    }

    // Disable system buttons to prevent TAS from executing a hotkey
    auto* controller = system->HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    controller->ResetSystemButtons();

    input_subsystem->GetTas()->StartStop();
    OnTasStateChanged();
}

void GMainWindow::OnTasRecord() {
    if (!emulation_running) {
        return;
    }
    if (is_tas_recording_dialog_active) {
        return;
    }

    // Disable system buttons to prevent TAS from recording a hotkey
    auto* controller = system->HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    controller->ResetSystemButtons();

    const bool is_recording = input_subsystem->GetTas()->Record();
    if (!is_recording) {
        is_tas_recording_dialog_active = true;
        ControllerNavigation* controller_navigation =
            new ControllerNavigation(system->HIDCore(), this);
        // Use QMessageBox instead of question so we can link controller navigation
        QMessageBox* box_dialog = new QMessageBox();
        box_dialog->setWindowTitle(tr("TAS Recording"));
        box_dialog->setText(tr("Overwrite file of player 1?"));
        box_dialog->setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        box_dialog->setDefaultButton(QMessageBox::Yes);
        connect(controller_navigation, &ControllerNavigation::TriggerKeyboardEvent,
                [box_dialog](Qt::Key key) {
                    QKeyEvent* event = new QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier);
                    QCoreApplication::postEvent(box_dialog, event);
                });
        int res = box_dialog->exec();
        controller_navigation->UnloadController();
        input_subsystem->GetTas()->SaveRecording(res == QMessageBox::Yes);
        is_tas_recording_dialog_active = false;
    }
    OnTasStateChanged();
}

void GMainWindow::OnTasReset() {
    input_subsystem->GetTas()->Reset();
}

void GMainWindow::OnToggleDockedMode() {
    const bool is_docked = Settings::values.use_docked_mode.GetValue();
    auto* player_1 = system->HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    auto* handheld = system->HIDCore().GetEmulatedController(Core::HID::NpadIdType::Handheld);

    if (!is_docked && handheld->IsConnected()) {
        QMessageBox::warning(this, tr("Invalid config detected"),
                             tr("Handheld controller can't be used on docked mode. Pro "
                                "controller will be selected."));
        handheld->Disconnect();
        player_1->SetNpadStyleIndex(Core::HID::NpadStyleIndex::ProController);
        player_1->Connect();
        controller_dialog->refreshConfiguration();
    }

    Settings::values.use_docked_mode.SetValue(!is_docked);
    UpdateDockedButton();
    OnDockedModeChanged(is_docked, !is_docked, *system);
}

void GMainWindow::OnToggleGpuAccuracy() {
    switch (Settings::values.gpu_accuracy.GetValue()) {
    case Settings::GPUAccuracy::High: {
        Settings::values.gpu_accuracy.SetValue(Settings::GPUAccuracy::Normal);
        break;
    }
    case Settings::GPUAccuracy::Normal:
    case Settings::GPUAccuracy::Extreme:
    default: {
        Settings::values.gpu_accuracy.SetValue(Settings::GPUAccuracy::High);
        break;
    }
    }

    system->ApplySettings();
    UpdateGPUAccuracyButton();
}

void GMainWindow::OnMute() {
    Settings::values.audio_muted = !Settings::values.audio_muted;
    UpdateVolumeUI();
}

void GMainWindow::OnDecreaseVolume() {
    Settings::values.audio_muted = false;
    const auto current_volume = static_cast<s32>(Settings::values.volume.GetValue());
    int step = 5;
    if (current_volume <= 30) {
        step = 2;
    }
    if (current_volume <= 6) {
        step = 1;
    }
    Settings::values.volume.SetValue(std::max(current_volume - step, 0));
    UpdateVolumeUI();
}

void GMainWindow::OnIncreaseVolume() {
    Settings::values.audio_muted = false;
    const auto current_volume = static_cast<s32>(Settings::values.volume.GetValue());
    int step = 5;
    if (current_volume < 30) {
        step = 2;
    }
    if (current_volume < 6) {
        step = 1;
    }
    Settings::values.volume.SetValue(current_volume + step);
    UpdateVolumeUI();
}

void GMainWindow::OnToggleAdaptingFilter() {
    auto filter = Settings::values.scaling_filter.GetValue();
    if (filter == Settings::ScalingFilter::LastFilter) {
        filter = Settings::ScalingFilter::NearestNeighbor;
    } else {
        filter = static_cast<Settings::ScalingFilter>(static_cast<u32>(filter) + 1);
    }
    Settings::values.scaling_filter.SetValue(filter);
    filter_status_button->setChecked(true);
    UpdateFilterText();
}

void GMainWindow::OnToggleGraphicsAPI() {
    auto api = Settings::values.renderer_backend.GetValue();
    if (api == Settings::RendererBackend::OpenGL) {
        api = Settings::RendererBackend::Vulkan;
    } else {
        api = Settings::RendererBackend::OpenGL;
    }
    Settings::values.renderer_backend.SetValue(api);
    renderer_status_button->setChecked(api == Settings::RendererBackend::Vulkan);
    UpdateAPIText();
}

void GMainWindow::OnConfigurePerGame() {
    const u64 title_id = system->GetApplicationProcessProgramID();
    OpenPerGameConfiguration(title_id, current_game_path.toStdString());
}

void GMainWindow::OpenPerGameConfiguration(u64 title_id, const std::string& file_name) {
    const auto v_file = Core::GetGameFileFromPath(vfs, file_name);

    Settings::SetConfiguringGlobal(false);
    ConfigurePerGame dialog(this, title_id, file_name, *system);
    dialog.LoadFromFile(v_file);
    const auto result = dialog.exec();

    if (result != QDialog::Accepted && !UISettings::values.configuration_applied) {
        Settings::RestoreGlobalState(system->IsPoweredOn());
        return;
    } else if (result == QDialog::Accepted) {
        dialog.ApplyConfiguration();
    }

    const auto reload = UISettings::values.is_game_list_reload_pending.exchange(false);
    if (reload) {
        game_list->PopulateAsync(UISettings::values.game_dirs);
    }

    // Do not cause the global config to write local settings into the config file
    const bool is_powered_on = system->IsPoweredOn();
    Settings::RestoreGlobalState(is_powered_on);
    system->HIDCore().ReloadInputDevices();

    UISettings::values.configuration_applied = false;

    if (!is_powered_on) {
        config->Save();
    }
}

bool GMainWindow::CreateShortcut(const std::string& shortcut_path, const std::string& title,
                                 const std::string& comment, const std::string& icon_path,
                                 const std::string& command, const std::string& arguments,
                                 const std::string& categories, const std::string& keywords) {
#if defined(__linux__) || defined(__FreeBSD__)
    // This desktop file template was writing referencing
    // https://specifications.freedesktop.org/desktop-entry-spec/desktop-entry-spec-1.0.html
    std::string shortcut_contents{};
    shortcut_contents.append("[Desktop Entry]\n");
    shortcut_contents.append("Type=Application\n");
    shortcut_contents.append("Version=1.0\n");
    shortcut_contents.append(fmt::format("Name={:s}\n", title));
    shortcut_contents.append(fmt::format("Comment={:s}\n", comment));
    shortcut_contents.append(fmt::format("Icon={:s}\n", icon_path));
    shortcut_contents.append(fmt::format("TryExec={:s}\n", command));
    shortcut_contents.append(fmt::format("Exec={:s} {:s}\n", command, arguments));
    shortcut_contents.append(fmt::format("Categories={:s}\n", categories));
    shortcut_contents.append(fmt::format("Keywords={:s}\n", keywords));

    std::ofstream shortcut_stream(shortcut_path);
    if (!shortcut_stream.is_open()) {
        LOG_WARNING(Common, "Failed to create file {:s}", shortcut_path);
        return false;
    }
    shortcut_stream << shortcut_contents;
    shortcut_stream.close();

    return true;
#endif
    return false;
}

void GMainWindow::OnLoadAmiibo() {
    if (emu_thread == nullptr || !emu_thread->IsRunning()) {
        return;
    }
    if (is_amiibo_file_select_active) {
        return;
    }

    auto* virtual_amiibo = input_subsystem->GetVirtualAmiibo();

    // Remove amiibo if one is connected
    if (virtual_amiibo->GetCurrentState() == InputCommon::VirtualAmiibo::State::AmiiboIsOpen) {
        virtual_amiibo->CloseAmiibo();
        QMessageBox::warning(this, tr("Amiibo"), tr("The current amiibo has been removed"));
        return;
    }

    if (virtual_amiibo->GetCurrentState() != InputCommon::VirtualAmiibo::State::WaitingForAmiibo) {
        QMessageBox::warning(this, tr("Error"), tr("The current game is not looking for amiibos"));
        return;
    }

    is_amiibo_file_select_active = true;
    const QString extensions{QStringLiteral("*.bin")};
    const QString file_filter = tr("Amiibo File (%1);; All Files (*.*)").arg(extensions);
    const QString filename = QFileDialog::getOpenFileName(this, tr("Load Amiibo"), {}, file_filter);
    is_amiibo_file_select_active = false;

    if (filename.isEmpty()) {
        return;
    }

    LoadAmiibo(filename);
}

void GMainWindow::LoadAmiibo(const QString& filename) {
    auto* virtual_amiibo = input_subsystem->GetVirtualAmiibo();
    const QString title = tr("Error loading Amiibo data");
    // Remove amiibo if one is connected
    if (virtual_amiibo->GetCurrentState() == InputCommon::VirtualAmiibo::State::AmiiboIsOpen) {
        virtual_amiibo->CloseAmiibo();
        QMessageBox::warning(this, tr("Amiibo"), tr("The current amiibo has been removed"));
        return;
    }

    switch (virtual_amiibo->LoadAmiibo(filename.toStdString())) {
    case InputCommon::VirtualAmiibo::Info::NotAnAmiibo:
        QMessageBox::warning(this, title, tr("The selected file is not a valid amiibo"));
        break;
    case InputCommon::VirtualAmiibo::Info::UnableToLoad:
        QMessageBox::warning(this, title, tr("The selected file is already on use"));
        break;
    case InputCommon::VirtualAmiibo::Info::WrongDeviceState:
        QMessageBox::warning(this, title, tr("The current game is not looking for amiibos"));
        break;
    case InputCommon::VirtualAmiibo::Info::Unknown:
        QMessageBox::warning(this, title, tr("An unknown error occurred"));
        break;
    default:
        break;
    }
}

void GMainWindow::OnOpenYuzuFolder() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QString::fromStdString(Common::FS::GetYuzuPathString(Common::FS::YuzuPath::YuzuDir))));
}

void GMainWindow::OnAbout() {
    AboutDialog aboutDialog(this);
    aboutDialog.exec();
}

void GMainWindow::OnToggleFilterBar() {
    game_list->SetFilterVisible(ui->action_Show_Filter_Bar->isChecked());
    if (ui->action_Show_Filter_Bar->isChecked()) {
        game_list->SetFilterFocus();
    } else {
        game_list->ClearFilter();
    }
}

void GMainWindow::OnToggleStatusBar() {
    statusBar()->setVisible(ui->action_Show_Status_Bar->isChecked());
}

void GMainWindow::OnCaptureScreenshot() {
    if (emu_thread == nullptr || !emu_thread->IsRunning()) {
        return;
    }

    const u64 title_id = system->GetApplicationProcessProgramID();
    const auto screenshot_path =
        QString::fromStdString(Common::FS::GetYuzuPathString(Common::FS::YuzuPath::ScreenshotsDir));
    const auto date =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_hh-mm-ss-zzz"));
    QString filename = QStringLiteral("%1/%2_%3.png")
                           .arg(screenshot_path)
                           .arg(title_id, 16, 16, QLatin1Char{'0'})
                           .arg(date);

    if (!Common::FS::CreateDir(screenshot_path.toStdString())) {
        return;
    }

#ifdef _WIN32
    if (UISettings::values.enable_screenshot_save_as) {
        OnPauseGame();
        filename = QFileDialog::getSaveFileName(this, tr("Capture Screenshot"), filename,
                                                tr("PNG Image (*.png)"));
        OnStartGame();
        if (filename.isEmpty()) {
            return;
        }
    }
#endif
    render_window->CaptureScreenshot(filename);
}

// TODO: Written 2020-10-01: Remove per-game config migration code when it is irrelevant
void GMainWindow::MigrateConfigFiles() {
    const auto config_dir_fs_path = Common::FS::GetYuzuPath(Common::FS::YuzuPath::ConfigDir);
    const QDir config_dir =
        QDir(QString::fromStdString(Common::FS::PathToUTF8String(config_dir_fs_path)));
    const QStringList config_dir_list = config_dir.entryList(QStringList(QStringLiteral("*.ini")));

    if (!Common::FS::CreateDirs(config_dir_fs_path / "custom")) {
        LOG_ERROR(Frontend, "Failed to create new config file directory");
    }

    for (auto it = config_dir_list.constBegin(); it != config_dir_list.constEnd(); ++it) {
        const auto filename = it->toStdString();
        if (filename.find_first_not_of("0123456789abcdefACBDEF", 0) < 16) {
            continue;
        }
        const auto origin = config_dir_fs_path / filename;
        const auto destination = config_dir_fs_path / "custom" / filename;
        LOG_INFO(Frontend, "Migrating config file from {} to {}", origin.string(),
                 destination.string());
        if (!Common::FS::RenameFile(origin, destination)) {
            // Delete the old config file if one already exists in the new location.
            Common::FS::RemoveFile(origin);
        }
    }
}

void GMainWindow::UpdateWindowTitle(std::string_view title_name, std::string_view title_version,
                                    std::string_view gpu_vendor) {
    const auto branch_name = std::string(Common::g_scm_branch);
    const auto description = std::string(Common::g_scm_desc);
    const auto build_id = std::string(Common::g_build_id);

    const auto yuzu_title = fmt::format("yuzu | {}-{}", branch_name, description);
    const auto override_title =
        fmt::format(fmt::runtime(std::string(Common::g_title_bar_format_idle)), build_id);
    const auto window_title = override_title.empty() ? yuzu_title : override_title;

    if (title_name.empty()) {
        setWindowTitle(QString::fromStdString(window_title));
    } else {
        const auto run_title = [window_title, title_name, title_version, gpu_vendor]() {
            if (title_version.empty()) {
                return fmt::format("{} | {} | {}", window_title, title_name, gpu_vendor);
            }
            return fmt::format("{} | {} | {} | {}", window_title, title_name, title_version,
                               gpu_vendor);
        }();
        setWindowTitle(QString::fromStdString(run_title));
    }
}

std::string GMainWindow::CreateTASFramesString(
    std::array<size_t, InputCommon::TasInput::PLAYER_NUMBER> frames) const {
    std::string string = "";
    size_t maxPlayerIndex = 0;
    for (size_t i = 0; i < frames.size(); i++) {
        if (frames[i] != 0) {
            if (maxPlayerIndex != 0)
                string += ", ";
            while (maxPlayerIndex++ != i)
                string += "0, ";
            string += std::to_string(frames[i]);
        }
    }
    return string;
}

QString GMainWindow::GetTasStateDescription() const {
    auto [tas_status, current_tas_frame, total_tas_frames] = input_subsystem->GetTas()->GetStatus();
    std::string tas_frames_string = CreateTASFramesString(total_tas_frames);
    switch (tas_status) {
    case InputCommon::TasInput::TasState::Running:
        return tr("TAS state: Running %1/%2")
            .arg(current_tas_frame)
            .arg(QString::fromStdString(tas_frames_string));
    case InputCommon::TasInput::TasState::Recording:
        return tr("TAS state: Recording %1").arg(total_tas_frames[0]);
    case InputCommon::TasInput::TasState::Stopped:
        return tr("TAS state: Idle %1/%2")
            .arg(current_tas_frame)
            .arg(QString::fromStdString(tas_frames_string));
    default:
        return tr("TAS State: Invalid");
    }
}

void GMainWindow::OnTasStateChanged() {
    bool is_running = false;
    bool is_recording = false;
    if (emulation_running) {
        const InputCommon::TasInput::TasState tas_status =
            std::get<0>(input_subsystem->GetTas()->GetStatus());
        is_running = tas_status == InputCommon::TasInput::TasState::Running;
        is_recording = tas_status == InputCommon::TasInput::TasState::Recording;
    }

    ui->action_TAS_Start->setText(is_running ? tr("&Stop Running") : tr("&Start"));
    ui->action_TAS_Record->setText(is_recording ? tr("Stop R&ecording") : tr("R&ecord"));

    ui->action_TAS_Start->setEnabled(emulation_running);
    ui->action_TAS_Record->setEnabled(emulation_running);
    ui->action_TAS_Reset->setEnabled(emulation_running);
}

void GMainWindow::UpdateStatusBar() {
    if (emu_thread == nullptr || !system->IsPoweredOn()) {
        status_bar_update_timer.stop();
        return;
    }

    if (Settings::values.tas_enable) {
        tas_label->setText(GetTasStateDescription());
    } else {
        tas_label->clear();
    }

    auto results = system->GetAndResetPerfStats();
    auto& shader_notify = system->GPU().ShaderNotify();
    const int shaders_building = shader_notify.ShadersBuilding();

    if (shaders_building > 0) {
        shader_building_label->setText(tr("Building: %n shader(s)", "", shaders_building));
        shader_building_label->setVisible(true);
    } else {
        shader_building_label->setVisible(false);
    }

    const auto res_info = Settings::values.resolution_info;
    const auto res_scale = res_info.up_factor;
    res_scale_label->setText(
        tr("Scale: %1x", "%1 is the resolution scaling factor").arg(res_scale));

    if (Settings::values.use_speed_limit.GetValue()) {
        emu_speed_label->setText(tr("Speed: %1% / %2%")
                                     .arg(results.emulation_speed * 100.0, 0, 'f', 0)
                                     .arg(Settings::values.speed_limit.GetValue()));
    } else {
        emu_speed_label->setText(tr("Speed: %1%").arg(results.emulation_speed * 100.0, 0, 'f', 0));
    }
    if (!Settings::values.use_speed_limit) {
        game_fps_label->setText(
            tr("Game: %1 FPS (Unlocked)").arg(std::round(results.average_game_fps), 0, 'f', 0));
    } else {
        game_fps_label->setText(
            tr("Game: %1 FPS").arg(std::round(results.average_game_fps), 0, 'f', 0));
    }
    emu_frametime_label->setText(tr("Frame: %1 ms").arg(results.frametime * 1000.0, 0, 'f', 2));

    res_scale_label->setVisible(true);
    emu_speed_label->setVisible(!Settings::values.use_multi_core.GetValue());
    game_fps_label->setVisible(true);
    emu_frametime_label->setVisible(true);
}

void GMainWindow::UpdateGPUAccuracyButton() {
    switch (Settings::values.gpu_accuracy.GetValue()) {
    case Settings::GPUAccuracy::Normal: {
        gpu_accuracy_button->setText(tr("GPU NORMAL"));
        gpu_accuracy_button->setChecked(false);
        break;
    }
    case Settings::GPUAccuracy::High: {
        gpu_accuracy_button->setText(tr("GPU HIGH"));
        gpu_accuracy_button->setChecked(true);
        break;
    }
    case Settings::GPUAccuracy::Extreme: {
        gpu_accuracy_button->setText(tr("GPU EXTREME"));
        gpu_accuracy_button->setChecked(true);
        break;
    }
    default: {
        gpu_accuracy_button->setText(tr("GPU ERROR"));
        gpu_accuracy_button->setChecked(true);
        break;
    }
    }
}

void GMainWindow::UpdateDockedButton() {
    const bool is_docked = Settings::values.use_docked_mode.GetValue();
    dock_status_button->setChecked(is_docked);
    dock_status_button->setText(is_docked ? tr("DOCKED") : tr("HANDHELD"));
}

void GMainWindow::UpdateAPIText() {
    const auto api = Settings::values.renderer_backend.GetValue();
    switch (api) {
    case Settings::RendererBackend::OpenGL:
        renderer_status_button->setText(tr("OPENGL"));
        break;
    case Settings::RendererBackend::Vulkan:
        renderer_status_button->setText(tr("VULKAN"));
        break;
    case Settings::RendererBackend::Null:
        renderer_status_button->setText(tr("NULL"));
        break;
    }
}

void GMainWindow::UpdateFilterText() {
    const auto filter = Settings::values.scaling_filter.GetValue();
    switch (filter) {
    case Settings::ScalingFilter::NearestNeighbor:
        filter_status_button->setText(tr("NEAREST"));
        break;
    case Settings::ScalingFilter::Bilinear:
        filter_status_button->setText(tr("BILINEAR"));
        break;
    case Settings::ScalingFilter::Bicubic:
        filter_status_button->setText(tr("BICUBIC"));
        break;
    case Settings::ScalingFilter::Gaussian:
        filter_status_button->setText(tr("GAUSSIAN"));
        break;
    case Settings::ScalingFilter::ScaleForce:
        filter_status_button->setText(tr("SCALEFORCE"));
        break;
    case Settings::ScalingFilter::Fsr:
        filter_status_button->setText(tr("FSR"));
        break;
    default:
        filter_status_button->setText(tr("BILINEAR"));
        break;
    }
}

void GMainWindow::UpdateAAText() {
    const auto aa_mode = Settings::values.anti_aliasing.GetValue();
    switch (aa_mode) {
    case Settings::AntiAliasing::None:
        aa_status_button->setText(tr("NO AA"));
        break;
    case Settings::AntiAliasing::Fxaa:
        aa_status_button->setText(tr("FXAA"));
        break;
    case Settings::AntiAliasing::Smaa:
        aa_status_button->setText(tr("SMAA"));
        break;
    default:
        aa_status_button->setText(tr("NO AA"));
        break;
    }
}

void GMainWindow::UpdateVolumeUI() {
    const auto volume_value = static_cast<int>(Settings::values.volume.GetValue());
    volume_slider->setValue(volume_value);
    if (Settings::values.audio_muted) {
        volume_button->setChecked(false);
        volume_button->setText(tr("VOLUME: MUTE"));
    } else {
        volume_button->setChecked(true);
        volume_button->setText(tr("VOLUME: %1%", "Volume percentage (e.g. 50%)").arg(volume_value));
    }
}

void GMainWindow::UpdateStatusButtons() {
    renderer_status_button->setChecked(Settings::values.renderer_backend.GetValue() ==
                                       Settings::RendererBackend::Vulkan);
    UpdateAPIText();
    UpdateGPUAccuracyButton();
    UpdateDockedButton();
    UpdateFilterText();
    UpdateAAText();
    UpdateVolumeUI();
}

void GMainWindow::UpdateUISettings() {
    if (!ui->action_Fullscreen->isChecked()) {
        UISettings::values.geometry = saveGeometry();
        UISettings::values.renderwindow_geometry = render_window->saveGeometry();
    }
    UISettings::values.state = saveState();
#if MICROPROFILE_ENABLED
    UISettings::values.microprofile_geometry = microProfileDialog->saveGeometry();
    UISettings::values.microprofile_visible = microProfileDialog->isVisible();
#endif
    UISettings::values.single_window_mode = ui->action_Single_Window_Mode->isChecked();
    UISettings::values.fullscreen = ui->action_Fullscreen->isChecked();
    UISettings::values.display_titlebar = ui->action_Display_Dock_Widget_Headers->isChecked();
    UISettings::values.show_filter_bar = ui->action_Show_Filter_Bar->isChecked();
    UISettings::values.show_status_bar = ui->action_Show_Status_Bar->isChecked();
    UISettings::values.first_start = false;
}

void GMainWindow::UpdateInputDrivers() {
    if (!input_subsystem) {
        return;
    }
    input_subsystem->PumpEvents();
}

void GMainWindow::HideMouseCursor() {
    if (emu_thread == nullptr && UISettings::values.hide_mouse) {
        mouse_hide_timer.stop();
        ShowMouseCursor();
        return;
    }
    render_window->setCursor(QCursor(Qt::BlankCursor));
}

void GMainWindow::ShowMouseCursor() {
    render_window->unsetCursor();
    if (emu_thread != nullptr && UISettings::values.hide_mouse) {
        mouse_hide_timer.start();
    }
}

void GMainWindow::CenterMouseCursor() {
    if (emu_thread == nullptr || !Settings::values.mouse_panning) {
        mouse_center_timer.stop();
        return;
    }
    if (!this->isActiveWindow()) {
        mouse_center_timer.stop();
        return;
    }
    const int center_x = render_window->width() / 2;
    const int center_y = render_window->height() / 2;

    QCursor::setPos(mapToGlobal(QPoint{center_x, center_y}));
}

void GMainWindow::OnMouseActivity() {
    if (!Settings::values.mouse_panning) {
        ShowMouseCursor();
    }
    mouse_center_timer.stop();
}

void GMainWindow::OnReinitializeKeys(ReinitializeKeyBehavior behavior) {
    if (behavior == ReinitializeKeyBehavior::Warning) {
        const auto res = QMessageBox::information(
            this, tr("Confirm Key Rederivation"),
            tr("You are about to force rederive all of your keys. \nIf you do not know what "
               "this "
               "means or what you are doing, \nthis is a potentially destructive action. "
               "\nPlease "
               "make sure this is what you want \nand optionally make backups.\n\nThis will "
               "delete "
               "your autogenerated key files and re-run the key derivation module."),
            QMessageBox::StandardButtons{QMessageBox::Ok, QMessageBox::Cancel});

        if (res == QMessageBox::Cancel)
            return;

        const auto keys_dir = Common::FS::GetYuzuPath(Common::FS::YuzuPath::KeysDir);

        Common::FS::RemoveFile(keys_dir / "prod.keys_autogenerated");
        Common::FS::RemoveFile(keys_dir / "console.keys_autogenerated");
        Common::FS::RemoveFile(keys_dir / "title.keys_autogenerated");
    }

    Core::Crypto::KeyManager& keys = Core::Crypto::KeyManager::Instance();
    bool all_keys_present{true};

    if (keys.BaseDeriveNecessary()) {
        Core::Crypto::PartitionDataManager pdm{vfs->OpenDirectory("", FileSys::Mode::Read)};

        const auto function = [this, &keys, &pdm] {
            keys.PopulateFromPartitionData(pdm);

            system->GetFileSystemController().CreateFactories(*vfs);
            keys.DeriveETicket(pdm, system->GetContentProvider());
        };

        QString errors;
        if (!pdm.HasFuses()) {
            errors += tr("Missing fuses");
        }
        if (!pdm.HasBoot0()) {
            errors += tr(" - Missing BOOT0");
        }
        if (!pdm.HasPackage2()) {
            errors += tr(" - Missing BCPKG2-1-Normal-Main");
        }
        if (!pdm.HasProdInfo()) {
            errors += tr(" - Missing PRODINFO");
        }
        if (!errors.isEmpty()) {
            all_keys_present = false;
            QMessageBox::warning(
                this, tr("Derivation Components Missing"),
                tr("Encryption keys are missing. "
                   "<br>Please follow <a href='https://yuzu-emu.org/help/quickstart/'>the yuzu "
                   "quickstart guide</a> to get all your keys, firmware and "
                   "games.<br><br><small>(%1)</small>")
                    .arg(errors));
        }

        QProgressDialog prog(this);
        prog.setRange(0, 0);
        prog.setLabelText(tr("Deriving keys...\nThis may take up to a minute depending \non your "
                             "system's performance."));
        prog.setWindowTitle(tr("Deriving Keys"));

        prog.show();

        auto future = QtConcurrent::run(function);
        while (!future.isFinished()) {
            QCoreApplication::processEvents();
        }

        prog.close();
    }

    system->GetFileSystemController().CreateFactories(*vfs);

    if (all_keys_present && !this->CheckSystemArchiveDecryption()) {
        LOG_WARNING(Frontend, "Mii model decryption failed");
        QMessageBox::warning(
            this, tr("System Archive Decryption Failed"),
            tr("Encryption keys failed to decrypt firmware. "
               "<br>Please follow <a href='https://yuzu-emu.org/help/quickstart/'>the yuzu "
               "quickstart guide</a> to get all your keys, firmware and "
               "games."));
    }

    if (behavior == ReinitializeKeyBehavior::Warning) {
        game_list->PopulateAsync(UISettings::values.game_dirs);
    }
}

bool GMainWindow::CheckSystemArchiveDecryption() {
    constexpr u64 MiiModelId = 0x0100000000000802;

    auto bis_system = system->GetFileSystemController().GetSystemNANDContents();
    if (!bis_system) {
        // Not having system BIS files is not an error.
        return true;
    }

    auto mii_nca = bis_system->GetEntry(MiiModelId, FileSys::ContentRecordType::Data);
    if (!mii_nca) {
        // Not having the Mii model is not an error.
        return true;
    }

    // Return whether we are able to decrypt the RomFS of the Mii model.
    return mii_nca->GetRomFS().get() != nullptr;
}

std::optional<u64> GMainWindow::SelectRomFSDumpTarget(const FileSys::ContentProvider& installed,
                                                      u64 program_id) {
    const auto dlc_entries =
        installed.ListEntriesFilter(FileSys::TitleType::AOC, FileSys::ContentRecordType::Data);
    std::vector<FileSys::ContentProviderEntry> dlc_match;
    dlc_match.reserve(dlc_entries.size());
    std::copy_if(dlc_entries.begin(), dlc_entries.end(), std::back_inserter(dlc_match),
                 [&program_id, &installed](const FileSys::ContentProviderEntry& entry) {
                     return FileSys::GetBaseTitleID(entry.title_id) == program_id &&
                            installed.GetEntry(entry)->GetStatus() == Loader::ResultStatus::Success;
                 });

    std::vector<u64> romfs_tids;
    romfs_tids.push_back(program_id);
    for (const auto& entry : dlc_match) {
        romfs_tids.push_back(entry.title_id);
    }

    if (romfs_tids.size() > 1) {
        QStringList list{QStringLiteral("Base")};
        for (std::size_t i = 1; i < romfs_tids.size(); ++i) {
            list.push_back(QStringLiteral("DLC %1").arg(romfs_tids[i] & 0x7FF));
        }

        bool ok;
        const auto res = QInputDialog::getItem(
            this, tr("Select RomFS Dump Target"),
            tr("Please select which RomFS you would like to dump."), list, 0, false, &ok);
        if (!ok) {
            return {};
        }

        return romfs_tids[list.indexOf(res)];
    }

    return program_id;
}

bool GMainWindow::ConfirmClose() {
    if (emu_thread == nullptr || !UISettings::values.confirm_before_closing) {
        return true;
    }
    const auto text = tr("Are you sure you want to close yuzu?");
    const auto answer = QMessageBox::question(this, tr("yuzu"), text);
    return answer != QMessageBox::No;
}

void GMainWindow::closeEvent(QCloseEvent* event) {
    if (!ConfirmClose()) {
        event->ignore();
        return;
    }

    UpdateUISettings();
    game_list->SaveInterfaceLayout();
    hotkey_registry.SaveHotkeys();

    // Unload controllers early
    controller_dialog->UnloadController();
    game_list->UnloadController();

    // Shutdown session if the emu thread is active...
    if (emu_thread != nullptr) {
        ShutdownGame();
    }

    render_window->close();
    multiplayer_state->Close();
    system->HIDCore().UnloadInputDevices();
    system->GetRoomNetwork().Shutdown();

    QWidget::closeEvent(event);
}

static bool IsSingleFileDropEvent(const QMimeData* mime) {
    return mime->hasUrls() && mime->urls().length() == 1;
}

void GMainWindow::AcceptDropEvent(QDropEvent* event) {
    if (IsSingleFileDropEvent(event->mimeData())) {
        event->setDropAction(Qt::DropAction::LinkAction);
        event->accept();
    }
}

bool GMainWindow::DropAction(QDropEvent* event) {
    if (!IsSingleFileDropEvent(event->mimeData())) {
        return false;
    }

    const QMimeData* mime_data = event->mimeData();
    const QString& filename = mime_data->urls().at(0).toLocalFile();

    if (emulation_running && QFileInfo(filename).suffix() == QStringLiteral("bin")) {
        // Amiibo
        LoadAmiibo(filename);
    } else {
        // Game
        if (ConfirmChangeGame()) {
            BootGame(filename);
        }
    }
    return true;
}

void GMainWindow::dropEvent(QDropEvent* event) {
    DropAction(event);
}

void GMainWindow::dragEnterEvent(QDragEnterEvent* event) {
    AcceptDropEvent(event);
}

void GMainWindow::dragMoveEvent(QDragMoveEvent* event) {
    AcceptDropEvent(event);
}

void GMainWindow::leaveEvent(QEvent* event) {
    if (Settings::values.mouse_panning) {
        const QRect& rect = geometry();
        QPoint position = QCursor::pos();

        qint32 x = qBound(rect.left(), position.x(), rect.right());
        qint32 y = qBound(rect.top(), position.y(), rect.bottom());
        // Only start the timer if the mouse has left the window bound.
        // The leave event is also triggered when the window looses focus.
        if (x != position.x() || y != position.y()) {
            mouse_center_timer.start();
        }
        event->accept();
    }
}

bool GMainWindow::ConfirmChangeGame() {
    if (emu_thread == nullptr)
        return true;

    const auto answer = QMessageBox::question(
        this, tr("yuzu"),
        tr("Are you sure you want to stop the emulation? Any unsaved progress will be lost."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return answer != QMessageBox::No;
}

bool GMainWindow::ConfirmForceLockedExit() {
    if (emu_thread == nullptr || !UISettings::values.confirm_before_closing) {
        return true;
    }
    const auto text = tr("The currently running application has requested yuzu to not exit.\n\n"
                         "Would you like to bypass this and exit anyway?");

    const auto answer = QMessageBox::question(this, tr("yuzu"), text);
    return answer != QMessageBox::No;
}

void GMainWindow::RequestGameExit() {
    if (!system->IsPoweredOn()) {
        return;
    }

    auto& sm{system->ServiceManager()};
    auto applet_oe = sm.GetService<Service::AM::AppletOE>("appletOE");
    auto applet_ae = sm.GetService<Service::AM::AppletAE>("appletAE");
    bool has_signalled = false;

    if (applet_oe != nullptr) {
        applet_oe->GetMessageQueue()->RequestExit();
        has_signalled = true;
    }

    if (applet_ae != nullptr && !has_signalled) {
        applet_ae->GetMessageQueue()->RequestExit();
    }
}

void GMainWindow::RequestGameResume() {
    auto& sm{system->ServiceManager()};
    auto applet_oe = sm.GetService<Service::AM::AppletOE>("appletOE");
    auto applet_ae = sm.GetService<Service::AM::AppletAE>("appletAE");

    if (applet_oe != nullptr) {
        applet_oe->GetMessageQueue()->RequestResume();
        return;
    }

    if (applet_ae != nullptr) {
        applet_ae->GetMessageQueue()->RequestResume();
    }
}

void GMainWindow::filterBarSetChecked(bool state) {
    ui->action_Show_Filter_Bar->setChecked(state);
    emit(OnToggleFilterBar());
}

static void AdjustLinkColor() {
    QPalette new_pal(qApp->palette());
    if (UISettings::IsDarkTheme()) {
        new_pal.setColor(QPalette::Link, QColor(0, 190, 255, 255));
    } else {
        new_pal.setColor(QPalette::Link, QColor(0, 140, 200, 255));
    }
    if (qApp->palette().color(QPalette::Link) != new_pal.color(QPalette::Link)) {
        qApp->setPalette(new_pal);
    }
}

void GMainWindow::UpdateUITheme() {
    const QString default_theme =
        QString::fromUtf8(UISettings::themes[static_cast<size_t>(Config::default_theme)].second);
    QString current_theme = UISettings::values.theme;

    if (current_theme.isEmpty()) {
        current_theme = default_theme;
    }

#ifdef _WIN32
    QIcon::setThemeName(current_theme);
    AdjustLinkColor();
#else
    if (current_theme == QStringLiteral("default") || current_theme == QStringLiteral("colorful")) {
        QIcon::setThemeName(current_theme == QStringLiteral("colorful") ? current_theme
                                                                        : startup_icon_theme);
        QIcon::setThemeSearchPaths(QStringList(default_theme_paths));
        if (CheckDarkMode()) {
            current_theme = QStringLiteral("default_dark");
        }
    } else {
        QIcon::setThemeName(current_theme);
        QIcon::setThemeSearchPaths(QStringList(QStringLiteral(":/icons")));
        AdjustLinkColor();
    }
#endif
    if (current_theme != default_theme) {
        QString theme_uri{QStringLiteral(":%1/style.qss").arg(current_theme)};
        QFile f(theme_uri);
        if (!f.open(QFile::ReadOnly | QFile::Text)) {
            LOG_ERROR(Frontend, "Unable to open style \"{}\", fallback to the default theme",
                      UISettings::values.theme.toStdString());
            current_theme = default_theme;
        }
    }

    QString theme_uri{QStringLiteral(":%1/style.qss").arg(current_theme)};
    QFile f(theme_uri);
    if (f.open(QFile::ReadOnly | QFile::Text)) {
        QTextStream ts(&f);
        qApp->setStyleSheet(ts.readAll());
        setStyleSheet(ts.readAll());
    } else {
        LOG_ERROR(Frontend, "Unable to set style \"{}\", stylesheet file not found",
                  UISettings::values.theme.toStdString());
        qApp->setStyleSheet({});
        setStyleSheet({});
    }
}

void GMainWindow::LoadTranslation() {
    bool loaded;

    if (UISettings::values.language.isEmpty()) {
        // If the selected language is empty, use system locale
        loaded = translator.load(QLocale(), {}, {}, QStringLiteral(":/languages/"));
    } else {
        // Otherwise load from the specified file
        loaded = translator.load(UISettings::values.language, QStringLiteral(":/languages/"));
    }

    if (loaded) {
        qApp->installTranslator(&translator);
    } else {
        UISettings::values.language = QStringLiteral("en");
    }
}

void GMainWindow::OnLanguageChanged(const QString& locale) {
    if (UISettings::values.language != QStringLiteral("en")) {
        qApp->removeTranslator(&translator);
    }

    UISettings::values.language = locale;
    LoadTranslation();
    ui->retranslateUi(this);
    multiplayer_state->retranslateUi();
    UpdateWindowTitle();
}

void GMainWindow::SetDiscordEnabled([[maybe_unused]] bool state) {
#ifdef USE_DISCORD_PRESENCE
    if (state) {
        discord_rpc = std::make_unique<DiscordRPC::DiscordImpl>(*system);
    } else {
        discord_rpc = std::make_unique<DiscordRPC::NullImpl>();
    }
#else
    discord_rpc = std::make_unique<DiscordRPC::NullImpl>();
#endif
    discord_rpc->Update();
}

void GMainWindow::changeEvent(QEvent* event) {
#ifdef __unix__
    // PaletteChange event appears to only reach so far into the GUI, explicitly asking to
    // UpdateUITheme is a decent work around
    if (event->type() == QEvent::PaletteChange) {
        const QPalette test_palette(qApp->palette());
        const QString current_theme = UISettings::values.theme;
        // Keeping eye on QPalette::Window to avoid looping. QPalette::Text might be useful too
        static QColor last_window_color;
        const QColor window_color = test_palette.color(QPalette::Active, QPalette::Window);
        if (last_window_color != window_color && (current_theme == QStringLiteral("default") ||
                                                  current_theme == QStringLiteral("colorful"))) {
            UpdateUITheme();
        }
        last_window_color = window_color;
    }
#endif // __unix__
    QWidget::changeEvent(event);
}

#ifdef main
#undef main
#endif

static void SetHighDPIAttributes() {
#ifdef _WIN32
    // For Windows, we want to avoid scaling artifacts on fractional scaling ratios.
    // This is done by setting the optimal scaling policy for the primary screen.

    // Create a temporary QApplication.
    int temp_argc = 0;
    char** temp_argv = nullptr;
    QApplication temp{temp_argc, temp_argv};

    // Get the current screen geometry.
    const QScreen* primary_screen = QGuiApplication::primaryScreen();
    if (primary_screen == nullptr) {
        return;
    }

    const QRect screen_rect = primary_screen->geometry();
    const int real_width = screen_rect.width();
    const int real_height = screen_rect.height();
    const float real_ratio = primary_screen->logicalDotsPerInch() / 96.0f;

    // Recommended minimum width and height for proper window fit.
    // Any screen with a lower resolution than this will still have a scale of 1.
    constexpr float minimum_width = 1350.0f;
    constexpr float minimum_height = 900.0f;

    const float width_ratio = std::max(1.0f, real_width / minimum_width);
    const float height_ratio = std::max(1.0f, real_height / minimum_height);

    // Get the lower of the 2 ratios and truncate, this is the maximum integer scale.
    const float max_ratio = std::trunc(std::min(width_ratio, height_ratio));

    if (max_ratio > real_ratio) {
        QApplication::setHighDpiScaleFactorRoundingPolicy(
            Qt::HighDpiScaleFactorRoundingPolicy::Round);
    } else {
        QApplication::setHighDpiScaleFactorRoundingPolicy(
            Qt::HighDpiScaleFactorRoundingPolicy::Floor);
    }
#else
    // Other OSes should be better than Windows at fractional scaling.
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif

    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
}

int main(int argc, char* argv[]) {
    std::unique_ptr<Config> config = std::make_unique<Config>();
    bool has_broken_vulkan = false;
    bool is_child = false;
    if (CheckEnvVars(&is_child)) {
        return 0;
    }

#ifdef YUZU_DBGHELP
    PROCESS_INFORMATION pi;
    if (!is_child && Settings::values.create_crash_dumps.GetValue() &&
        MiniDump::SpawnDebuggee(argv[0], pi)) {
        // Delete the config object so that it doesn't save when the program exits
        config.reset(nullptr);
        MiniDump::DebugDebuggee(pi);
        return 0;
    }
#endif

    if (StartupChecks(argv[0], &has_broken_vulkan,
                      Settings::values.perform_vulkan_check.GetValue())) {
        return 0;
    }

    Common::DetachedTasks detached_tasks;
    MicroProfileOnThreadCreate("Frontend");
    SCOPE_EXIT({ MicroProfileShutdown(); });

    Common::ConfigureNvidiaEnvironmentFlags();

    // Init settings params
    QCoreApplication::setOrganizationName(QStringLiteral("yuzu team"));
    QCoreApplication::setApplicationName(QStringLiteral("yuzu"));

#ifdef _WIN32
    // Increases the maximum open file limit to 8192
    _setmaxstdio(8192);
#endif

#ifdef __APPLE__
    // If you start a bundle (binary) on OSX without the Terminal, the working directory is "/".
    // But since we require the working directory to be the executable path for the location of
    // the user folder in the Qt Frontend, we need to cd into that working directory
    const auto bin_path = Common::FS::GetBundleDirectory() / "..";
    chdir(Common::FS::PathToUTF8String(bin_path).c_str());
#endif

#ifdef __linux__
    // Set the DISPLAY variable in order to open web browsers
    // TODO (lat9nq): Find a better solution for AppImages to start external applications
    if (QString::fromLocal8Bit(qgetenv("DISPLAY")).isEmpty()) {
        qputenv("DISPLAY", ":0");
    }
#endif

    SetHighDPIAttributes();

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Disables the "?" button on all dialogs. Disabled by default on Qt6.
    QCoreApplication::setAttribute(Qt::AA_DisableWindowContextHelpButton);
#endif

    // Enables the core to make the qt created contexts current on std::threads
    QCoreApplication::setAttribute(Qt::AA_DontCheckOpenGLContextThreadAffinity);

    QApplication app(argc, argv);

#ifdef _WIN32
    OverrideWindowsFont();
#endif

    // Workaround for QTBUG-85409, for Suzhou numerals the number 1 is actually \u3021
    // so we can see if we get \u3008 instead
    // TL;DR all other number formats are consecutive in unicode code points
    // This bug is fixed in Qt6, specifically 6.0.0-alpha1
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    const QLocale locale = QLocale::system();
    if (QStringLiteral("\u3008") == locale.toString(1)) {
        QLocale::setDefault(QLocale::system().name());
    }
#endif

    // Qt changes the locale and causes issues in float conversion using std::to_string() when
    // generating shaders
    setlocale(LC_ALL, "C");

    GMainWindow main_window{std::move(config), has_broken_vulkan};
    // After settings have been loaded by GMainWindow, apply the filter
    main_window.show();

    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &main_window,
                     &GMainWindow::OnAppFocusStateChanged);

    int result = app.exec();
    detached_tasks.WaitForAllTasks();
    return result;
}
