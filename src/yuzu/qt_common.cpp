// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QGuiApplication>
#include <QStringLiteral>
#include <QWindow>
#include "common/logging/log.h"
#include "core/frontend/emu_window.h"
#include "yuzu/qt_common.h"

#if !defined(WIN32) && !defined(__APPLE__)
#include <qpa/qplatformnativeinterface.h>
#endif

namespace QtCommon {
Core::Frontend::WindowSystemType GetWindowSystemType() {
    // Determine WSI type based on Qt platform.
    QString platform_name = QGuiApplication::platformName();
    if (platform_name == QStringLiteral("windows"))
        return Core::Frontend::WindowSystemType::Windows;
    else if (platform_name == QStringLiteral("xcb"))
        return Core::Frontend::WindowSystemType::X11;
    else if (platform_name == QStringLiteral("wayland"))
        return Core::Frontend::WindowSystemType::Wayland;
    else if (platform_name == QStringLiteral("wayland-egl"))
        return Core::Frontend::WindowSystemType::Wayland;
    else if (platform_name == QStringLiteral("cocoa"))
        return Core::Frontend::WindowSystemType::Cocoa;
    else if (platform_name == QStringLiteral("android"))
        return Core::Frontend::WindowSystemType::Android;

    LOG_CRITICAL(Frontend, "Unknown Qt platform {}!", platform_name.toStdString());
    return Core::Frontend::WindowSystemType::Windows;
} // namespace Core::Frontend::WindowSystemType

Core::Frontend::EmuWindow::WindowSystemInfo GetWindowSystemInfo(QWindow* window) {
    Core::Frontend::EmuWindow::WindowSystemInfo wsi;
    wsi.type = GetWindowSystemType();

    // Our Win32 Qt external doesn't have the private API.
#if defined(WIN32) || defined(__APPLE__)
    wsi.render_surface = window ? reinterpret_cast<void*>(window->winId()) : nullptr;
#else
    QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
    wsi.display_connection = pni->nativeResourceForWindow("display", window);
    if (wsi.type == Core::Frontend::WindowSystemType::Wayland)
        wsi.render_surface = window ? pni->nativeResourceForWindow("surface", window) : nullptr;
    else
        wsi.render_surface = window ? reinterpret_cast<void*>(window->winId()) : nullptr;
#endif
    wsi.render_surface_scale = window ? static_cast<float>(window->devicePixelRatio()) : 1.0f;

    return wsi;
}
} // namespace QtCommon
