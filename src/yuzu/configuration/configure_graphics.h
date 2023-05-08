// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <QColor>
#include <QString>
#include <QWidget>
#include <qobjectdefs.h>
#include <vulkan/vulkan_core.h>
#include "common/common_types.h"

class QEvent;
class QObject;

namespace Settings {
enum class NvdecEmulation : u32;
enum class RendererBackend : u32;
enum class ShaderBackend : u32;
} // namespace Settings

namespace Core {
class System;
}

namespace ConfigurationShared {
enum class CheckState;
}

namespace Ui {
class ConfigureGraphics;
}

class ConfigureGraphics : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureGraphics(const Core::System& system_,
                               const std::function<void()>& expose_compute_option_,
                               QWidget* parent = nullptr);
    ~ConfigureGraphics() override;

    void ApplyConfiguration();
    void SetConfiguration();

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    void PopulateVSyncModeSelection();
    void UpdateBackgroundColorButton(QColor color);
    void UpdateAPILayout();
    void UpdateDeviceSelection(int device);
    void UpdateShaderBackendSelection(int backend);

    void RetrieveVulkanDevices();

    void SetFSRIndicatorText(int percentage);
    /* Turns a Vulkan present mode into a textual string for a UI
     * (and eventually for a human to read) */
    const QString TranslateVSyncMode(VkPresentModeKHR mode,
                                     Settings::RendererBackend backend) const;

    void SetupPerGameUI();

    Settings::RendererBackend GetCurrentGraphicsBackend() const;
    Settings::NvdecEmulation GetCurrentNvdecEmulation() const;

    std::unique_ptr<Ui::ConfigureGraphics> ui;
    QColor bg_color;

    ConfigurationShared::CheckState use_nvdec_emulation;
    ConfigurationShared::CheckState accelerate_astc;
    ConfigurationShared::CheckState use_disk_shader_cache;
    ConfigurationShared::CheckState use_asynchronous_gpu_emulation;

    std::vector<QString> vulkan_devices;
    std::vector<std::vector<VkPresentModeKHR>> device_present_modes;
    std::vector<VkPresentModeKHR>
        vsync_mode_combobox_enum_map; //< Keeps track of which present mode corresponds to which
                                      // selection in the combobox
    u32 vulkan_device{};
    Settings::ShaderBackend shader_backend{};
    const std::function<void()>& expose_compute_option;

    const Core::System& system;
};
