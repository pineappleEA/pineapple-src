// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Include this early to include Vulkan headers how we want to
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

#include <algorithm>
#include <functional>
#include <iosfwd>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <QBoxLayout>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QStringLiteral>
#include <QtCore/qobjectdefs.h>
#include <qcoreevent.h>
#include <qglobal.h>
#include <vulkan/vulkan_core.h>

#include "common/common_types.h"
#include "common/dynamic_library.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "ui_configure_graphics.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_surface.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_graphics.h"
#include "yuzu/qt_common.h"
#include "yuzu/uisettings.h"

static const std::vector<VkPresentModeKHR> default_present_modes{VK_PRESENT_MODE_IMMEDIATE_KHR,
                                                                 VK_PRESENT_MODE_FIFO_KHR};

// Converts a setting to a present mode (or vice versa)
static constexpr VkPresentModeKHR VSyncSettingToMode(Settings::VSyncMode mode) {
    switch (mode) {
    case Settings::VSyncMode::Immediate:
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case Settings::VSyncMode::Mailbox:
        return VK_PRESENT_MODE_MAILBOX_KHR;
    case Settings::VSyncMode::FIFO:
        return VK_PRESENT_MODE_FIFO_KHR;
    case Settings::VSyncMode::FIFORelaxed:
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    default:
        return VK_PRESENT_MODE_FIFO_KHR;
    }
}

static constexpr Settings::VSyncMode PresentModeToSetting(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return Settings::VSyncMode::Immediate;
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return Settings::VSyncMode::Mailbox;
    case VK_PRESENT_MODE_FIFO_KHR:
        return Settings::VSyncMode::FIFO;
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return Settings::VSyncMode::FIFORelaxed;
    default:
        return Settings::VSyncMode::FIFO;
    }
}

ConfigureGraphics::ConfigureGraphics(const Core::System& system_,
                                     const std::function<void()>& expose_compute_option_,
                                     QWidget* parent)
    : QWidget(parent), ui{std::make_unique<Ui::ConfigureGraphics>()},
      expose_compute_option{expose_compute_option_}, system{system_} {
    vulkan_device = Settings::values.vulkan_device.GetValue();
    RetrieveVulkanDevices();

    ui->setupUi(this);

    for (const auto& device : vulkan_devices) {
        ui->device->addItem(device);
    }

    ui->backend->addItem(QStringLiteral("GLSL"));
    ui->backend->addItem(tr("GLASM (Assembly Shaders, NVIDIA Only)"));
    ui->backend->addItem(tr("SPIR-V (Experimental, Mesa Only)"));

    SetupPerGameUI();

    SetConfiguration();

    connect(ui->api, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        UpdateAPILayout();
        PopulateVSyncModeSelection();
        if (!Settings::IsConfiguringGlobal()) {
            ConfigurationShared::SetHighlight(
                ui->api_widget, ui->api->currentIndex() != ConfigurationShared::USE_GLOBAL_INDEX);
        }
    });
    connect(ui->device, qOverload<int>(&QComboBox::activated), this, [this](int device) {
        UpdateDeviceSelection(device);
        PopulateVSyncModeSelection();
    });
    connect(ui->backend, qOverload<int>(&QComboBox::activated), this,
            [this](int backend) { UpdateShaderBackendSelection(backend); });

    connect(ui->bg_button, &QPushButton::clicked, this, [this] {
        const QColor new_bg_color = QColorDialog::getColor(bg_color);
        if (!new_bg_color.isValid()) {
            return;
        }
        UpdateBackgroundColorButton(new_bg_color);
    });

    ui->api->setEnabled(!UISettings::values.has_broken_vulkan && ui->api->isEnabled());
    ui->api_widget->setEnabled(
        (!UISettings::values.has_broken_vulkan || Settings::IsConfiguringGlobal()) &&
        ui->api_widget->isEnabled());
    ui->bg_label->setVisible(Settings::IsConfiguringGlobal());
    ui->bg_combobox->setVisible(!Settings::IsConfiguringGlobal());

    connect(ui->fsr_sharpening_slider, &QSlider::valueChanged, this,
            &ConfigureGraphics::SetFSRIndicatorText);
    ui->fsr_sharpening_combobox->setVisible(!Settings::IsConfiguringGlobal());
    ui->fsr_sharpening_label->setVisible(Settings::IsConfiguringGlobal());
}

void ConfigureGraphics::PopulateVSyncModeSelection() {
    const Settings::RendererBackend backend{GetCurrentGraphicsBackend()};
    if (backend == Settings::RendererBackend::Null) {
        ui->vsync_mode_combobox->setEnabled(false);
        return;
    }
    ui->vsync_mode_combobox->setEnabled(true);

    const int current_index = //< current selected vsync mode from combobox
        ui->vsync_mode_combobox->currentIndex();
    const auto current_mode = //< current selected vsync mode as a VkPresentModeKHR
        current_index == -1 ? VSyncSettingToMode(Settings::values.vsync_mode.GetValue())
                            : vsync_mode_combobox_enum_map[current_index];
    int index{};
    const int device{ui->device->currentIndex()}; //< current selected Vulkan device
    const auto& present_modes = //< relevant vector of present modes for the selected device or API
        backend == Settings::RendererBackend::Vulkan ? device_present_modes[device]
                                                     : default_present_modes;

    ui->vsync_mode_combobox->clear();
    vsync_mode_combobox_enum_map.clear();
    vsync_mode_combobox_enum_map.reserve(present_modes.size());
    for (const auto present_mode : present_modes) {
        const auto mode_name = TranslateVSyncMode(present_mode, backend);
        if (mode_name.isEmpty()) {
            continue;
        }

        ui->vsync_mode_combobox->insertItem(index, mode_name);
        vsync_mode_combobox_enum_map.push_back(present_mode);
        if (present_mode == current_mode) {
            ui->vsync_mode_combobox->setCurrentIndex(index);
        }
        index++;
    }
}

void ConfigureGraphics::UpdateDeviceSelection(int device) {
    if (device == -1) {
        return;
    }
    if (GetCurrentGraphicsBackend() == Settings::RendererBackend::Vulkan) {
        vulkan_device = device;
    }
}

void ConfigureGraphics::UpdateShaderBackendSelection(int backend) {
    if (backend == -1) {
        return;
    }
    if (GetCurrentGraphicsBackend() == Settings::RendererBackend::OpenGL) {
        shader_backend = static_cast<Settings::ShaderBackend>(backend);
    }
}

ConfigureGraphics::~ConfigureGraphics() = default;

void ConfigureGraphics::SetConfiguration() {
    const bool runtime_lock = !system.IsPoweredOn();

    ui->api_widget->setEnabled(runtime_lock);
    ui->use_asynchronous_gpu_emulation->setEnabled(runtime_lock);
    ui->use_disk_shader_cache->setEnabled(runtime_lock);
    ui->nvdec_emulation_widget->setEnabled(runtime_lock);
    ui->resolution_combobox->setEnabled(runtime_lock);
    ui->accelerate_astc->setEnabled(runtime_lock);
    ui->vsync_mode_layout->setEnabled(runtime_lock ||
                                      Settings::values.renderer_backend.GetValue() ==
                                          Settings::RendererBackend::Vulkan);
    ui->use_disk_shader_cache->setChecked(Settings::values.use_disk_shader_cache.GetValue());
    ui->use_asynchronous_gpu_emulation->setChecked(
        Settings::values.use_asynchronous_gpu_emulation.GetValue());
    ui->accelerate_astc->setChecked(Settings::values.accelerate_astc.GetValue());

    if (Settings::IsConfiguringGlobal()) {
        ui->api->setCurrentIndex(static_cast<int>(Settings::values.renderer_backend.GetValue()));
        ui->fullscreen_mode_combobox->setCurrentIndex(
            static_cast<int>(Settings::values.fullscreen_mode.GetValue()));
        ui->nvdec_emulation->setCurrentIndex(
            static_cast<int>(Settings::values.nvdec_emulation.GetValue()));
        ui->aspect_ratio_combobox->setCurrentIndex(Settings::values.aspect_ratio.GetValue());
        ui->resolution_combobox->setCurrentIndex(
            static_cast<int>(Settings::values.resolution_setup.GetValue()));
        ui->scaling_filter_combobox->setCurrentIndex(
            static_cast<int>(Settings::values.scaling_filter.GetValue()));
        ui->fsr_sharpening_slider->setValue(Settings::values.fsr_sharpening_slider.GetValue());
        ui->anti_aliasing_combobox->setCurrentIndex(
            static_cast<int>(Settings::values.anti_aliasing.GetValue()));
    } else {
        ConfigurationShared::SetPerGameSetting(ui->api, &Settings::values.renderer_backend);
        ConfigurationShared::SetHighlight(ui->api_widget,
                                          !Settings::values.renderer_backend.UsingGlobal());

        ConfigurationShared::SetPerGameSetting(ui->nvdec_emulation,
                                               &Settings::values.nvdec_emulation);
        ConfigurationShared::SetHighlight(ui->nvdec_emulation_widget,
                                          !Settings::values.nvdec_emulation.UsingGlobal());

        ConfigurationShared::SetPerGameSetting(ui->fullscreen_mode_combobox,
                                               &Settings::values.fullscreen_mode);
        ConfigurationShared::SetHighlight(ui->fullscreen_mode_label,
                                          !Settings::values.fullscreen_mode.UsingGlobal());

        ConfigurationShared::SetPerGameSetting(ui->aspect_ratio_combobox,
                                               &Settings::values.aspect_ratio);
        ConfigurationShared::SetHighlight(ui->ar_label,
                                          !Settings::values.aspect_ratio.UsingGlobal());

        ConfigurationShared::SetPerGameSetting(ui->resolution_combobox,
                                               &Settings::values.resolution_setup);
        ConfigurationShared::SetHighlight(ui->resolution_label,
                                          !Settings::values.resolution_setup.UsingGlobal());

        ConfigurationShared::SetPerGameSetting(ui->scaling_filter_combobox,
                                               &Settings::values.scaling_filter);
        ConfigurationShared::SetHighlight(ui->scaling_filter_label,
                                          !Settings::values.scaling_filter.UsingGlobal());

        ConfigurationShared::SetPerGameSetting(ui->anti_aliasing_combobox,
                                               &Settings::values.anti_aliasing);
        ConfigurationShared::SetHighlight(ui->anti_aliasing_label,
                                          !Settings::values.anti_aliasing.UsingGlobal());

        ui->fsr_sharpening_combobox->setCurrentIndex(
            Settings::values.fsr_sharpening_slider.UsingGlobal() ? 0 : 1);
        ui->fsr_sharpening_slider->setEnabled(
            !Settings::values.fsr_sharpening_slider.UsingGlobal());
        ui->fsr_sharpening_value->setEnabled(!Settings::values.fsr_sharpening_slider.UsingGlobal());
        ConfigurationShared::SetHighlight(ui->fsr_sharpening_layout,
                                          !Settings::values.fsr_sharpening_slider.UsingGlobal());
        ui->fsr_sharpening_slider->setValue(Settings::values.fsr_sharpening_slider.GetValue());

        ui->bg_combobox->setCurrentIndex(Settings::values.bg_red.UsingGlobal() ? 0 : 1);
        ui->bg_button->setEnabled(!Settings::values.bg_red.UsingGlobal());
        ConfigurationShared::SetHighlight(ui->bg_layout, !Settings::values.bg_red.UsingGlobal());
    }
    UpdateBackgroundColorButton(QColor::fromRgb(Settings::values.bg_red.GetValue(),
                                                Settings::values.bg_green.GetValue(),
                                                Settings::values.bg_blue.GetValue()));
    UpdateAPILayout();
    PopulateVSyncModeSelection(); //< must happen after UpdateAPILayout
    SetFSRIndicatorText(ui->fsr_sharpening_slider->sliderPosition());

    // VSync setting needs to be determined after populating the VSync combobox
    if (Settings::IsConfiguringGlobal()) {
        const auto vsync_mode_setting = Settings::values.vsync_mode.GetValue();
        const auto vsync_mode = VSyncSettingToMode(vsync_mode_setting);
        int index{};
        for (const auto mode : vsync_mode_combobox_enum_map) {
            if (mode == vsync_mode) {
                break;
            }
            index++;
        }
        if (static_cast<unsigned long>(index) < vsync_mode_combobox_enum_map.size()) {
            ui->vsync_mode_combobox->setCurrentIndex(index);
        }
    }
}

void ConfigureGraphics::SetFSRIndicatorText(int percentage) {
    ui->fsr_sharpening_value->setText(
        tr("%1%", "FSR sharpening percentage (e.g. 50%)").arg(100 - (percentage / 2)));
}

const QString ConfigureGraphics::TranslateVSyncMode(VkPresentModeKHR mode,
                                                    Settings::RendererBackend backend) const {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return backend == Settings::RendererBackend::OpenGL
                   ? tr("Off")
                   : QStringLiteral("Immediate (%1)").arg(tr("VSync Off"));
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return QStringLiteral("Mailbox (%1)").arg(tr("Recommended"));
    case VK_PRESENT_MODE_FIFO_KHR:
        return backend == Settings::RendererBackend::OpenGL
                   ? tr("On")
                   : QStringLiteral("FIFO (%1)").arg(tr("VSync On"));
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return QStringLiteral("FIFO Relaxed");
    default:
        return {};
        break;
    }
}

void ConfigureGraphics::ApplyConfiguration() {
    const auto resolution_setup = static_cast<Settings::ResolutionSetup>(
        ui->resolution_combobox->currentIndex() -
        ((Settings::IsConfiguringGlobal()) ? 0 : ConfigurationShared::USE_GLOBAL_OFFSET));

    const auto scaling_filter = static_cast<Settings::ScalingFilter>(
        ui->scaling_filter_combobox->currentIndex() -
        ((Settings::IsConfiguringGlobal()) ? 0 : ConfigurationShared::USE_GLOBAL_OFFSET));

    const auto anti_aliasing = static_cast<Settings::AntiAliasing>(
        ui->anti_aliasing_combobox->currentIndex() -
        ((Settings::IsConfiguringGlobal()) ? 0 : ConfigurationShared::USE_GLOBAL_OFFSET));

    ConfigurationShared::ApplyPerGameSetting(&Settings::values.fullscreen_mode,
                                             ui->fullscreen_mode_combobox);
    ConfigurationShared::ApplyPerGameSetting(&Settings::values.aspect_ratio,
                                             ui->aspect_ratio_combobox);
    ConfigurationShared::ApplyPerGameSetting(&Settings::values.use_disk_shader_cache,
                                             ui->use_disk_shader_cache, use_disk_shader_cache);
    ConfigurationShared::ApplyPerGameSetting(&Settings::values.use_asynchronous_gpu_emulation,
                                             ui->use_asynchronous_gpu_emulation,
                                             use_asynchronous_gpu_emulation);
    ConfigurationShared::ApplyPerGameSetting(&Settings::values.accelerate_astc, ui->accelerate_astc,
                                             accelerate_astc);

    if (Settings::IsConfiguringGlobal()) {
        // Guard if during game and set to game-specific value
        if (Settings::values.renderer_backend.UsingGlobal()) {
            Settings::values.renderer_backend.SetValue(GetCurrentGraphicsBackend());
        }
        if (Settings::values.nvdec_emulation.UsingGlobal()) {
            Settings::values.nvdec_emulation.SetValue(GetCurrentNvdecEmulation());
        }
        if (Settings::values.shader_backend.UsingGlobal()) {
            Settings::values.shader_backend.SetValue(shader_backend);
        }
        if (Settings::values.vulkan_device.UsingGlobal()) {
            Settings::values.vulkan_device.SetValue(vulkan_device);
        }
        if (Settings::values.bg_red.UsingGlobal()) {
            Settings::values.bg_red.SetValue(static_cast<u8>(bg_color.red()));
            Settings::values.bg_green.SetValue(static_cast<u8>(bg_color.green()));
            Settings::values.bg_blue.SetValue(static_cast<u8>(bg_color.blue()));
        }
        if (Settings::values.resolution_setup.UsingGlobal()) {
            Settings::values.resolution_setup.SetValue(resolution_setup);
        }
        if (Settings::values.scaling_filter.UsingGlobal()) {
            Settings::values.scaling_filter.SetValue(scaling_filter);
        }
        if (Settings::values.anti_aliasing.UsingGlobal()) {
            Settings::values.anti_aliasing.SetValue(anti_aliasing);
        }
        Settings::values.fsr_sharpening_slider.SetValue(ui->fsr_sharpening_slider->value());

        const auto mode = vsync_mode_combobox_enum_map[ui->vsync_mode_combobox->currentIndex()];
        const auto vsync_mode = PresentModeToSetting(mode);
        Settings::values.vsync_mode.SetValue(vsync_mode);
    } else {
        if (ui->resolution_combobox->currentIndex() == ConfigurationShared::USE_GLOBAL_INDEX) {
            Settings::values.resolution_setup.SetGlobal(true);
        } else {
            Settings::values.resolution_setup.SetGlobal(false);
            Settings::values.resolution_setup.SetValue(resolution_setup);
        }
        if (ui->scaling_filter_combobox->currentIndex() == ConfigurationShared::USE_GLOBAL_INDEX) {
            Settings::values.scaling_filter.SetGlobal(true);
        } else {
            Settings::values.scaling_filter.SetGlobal(false);
            Settings::values.scaling_filter.SetValue(scaling_filter);
        }
        if (ui->anti_aliasing_combobox->currentIndex() == ConfigurationShared::USE_GLOBAL_INDEX) {
            Settings::values.anti_aliasing.SetGlobal(true);
        } else {
            Settings::values.anti_aliasing.SetGlobal(false);
            Settings::values.anti_aliasing.SetValue(anti_aliasing);
        }
        if (ui->api->currentIndex() == ConfigurationShared::USE_GLOBAL_INDEX) {
            Settings::values.renderer_backend.SetGlobal(true);
            Settings::values.shader_backend.SetGlobal(true);
            Settings::values.vulkan_device.SetGlobal(true);
        } else {
            Settings::values.renderer_backend.SetGlobal(false);
            Settings::values.renderer_backend.SetValue(GetCurrentGraphicsBackend());
            switch (GetCurrentGraphicsBackend()) {
            case Settings::RendererBackend::OpenGL:
            case Settings::RendererBackend::Null:
                Settings::values.shader_backend.SetGlobal(false);
                Settings::values.vulkan_device.SetGlobal(true);
                Settings::values.shader_backend.SetValue(shader_backend);
                break;
            case Settings::RendererBackend::Vulkan:
                Settings::values.shader_backend.SetGlobal(true);
                Settings::values.vulkan_device.SetGlobal(false);
                Settings::values.vulkan_device.SetValue(vulkan_device);
                break;
            }
        }

        if (ui->nvdec_emulation->currentIndex() == ConfigurationShared::USE_GLOBAL_INDEX) {
            Settings::values.nvdec_emulation.SetGlobal(true);
        } else {
            Settings::values.nvdec_emulation.SetGlobal(false);
            Settings::values.nvdec_emulation.SetValue(GetCurrentNvdecEmulation());
        }

        if (ui->bg_combobox->currentIndex() == ConfigurationShared::USE_GLOBAL_INDEX) {
            Settings::values.bg_red.SetGlobal(true);
            Settings::values.bg_green.SetGlobal(true);
            Settings::values.bg_blue.SetGlobal(true);
        } else {
            Settings::values.bg_red.SetGlobal(false);
            Settings::values.bg_green.SetGlobal(false);
            Settings::values.bg_blue.SetGlobal(false);
            Settings::values.bg_red.SetValue(static_cast<u8>(bg_color.red()));
            Settings::values.bg_green.SetValue(static_cast<u8>(bg_color.green()));
            Settings::values.bg_blue.SetValue(static_cast<u8>(bg_color.blue()));
        }

        if (ui->fsr_sharpening_combobox->currentIndex() == ConfigurationShared::USE_GLOBAL_INDEX) {
            Settings::values.fsr_sharpening_slider.SetGlobal(true);
        } else {
            Settings::values.fsr_sharpening_slider.SetGlobal(false);
            Settings::values.fsr_sharpening_slider.SetValue(ui->fsr_sharpening_slider->value());
        }
    }
}

void ConfigureGraphics::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureGraphics::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureGraphics::UpdateBackgroundColorButton(QColor color) {
    bg_color = color;

    QPixmap pixmap(ui->bg_button->size());
    pixmap.fill(bg_color);

    const QIcon color_icon(pixmap);
    ui->bg_button->setIcon(color_icon);
}

void ConfigureGraphics::UpdateAPILayout() {
    if (!Settings::IsConfiguringGlobal() &&
        ui->api->currentIndex() == ConfigurationShared::USE_GLOBAL_INDEX) {
        vulkan_device = Settings::values.vulkan_device.GetValue(true);
        shader_backend = Settings::values.shader_backend.GetValue(true);
        ui->device_widget->setEnabled(false);
        ui->backend_widget->setEnabled(false);
    } else {
        vulkan_device = Settings::values.vulkan_device.GetValue();
        shader_backend = Settings::values.shader_backend.GetValue();
        ui->device_widget->setEnabled(true);
        ui->backend_widget->setEnabled(true);
    }

    switch (GetCurrentGraphicsBackend()) {
    case Settings::RendererBackend::OpenGL:
        ui->backend->setCurrentIndex(static_cast<u32>(shader_backend));
        ui->device_widget->setVisible(false);
        ui->backend_widget->setVisible(true);
        break;
    case Settings::RendererBackend::Vulkan:
        if (static_cast<int>(vulkan_device) < ui->device->count()) {
            ui->device->setCurrentIndex(vulkan_device);
        }
        ui->device_widget->setVisible(true);
        ui->backend_widget->setVisible(false);
        break;
    case Settings::RendererBackend::Null:
        ui->device_widget->setVisible(false);
        ui->backend_widget->setVisible(false);
        break;
    }
}

void ConfigureGraphics::RetrieveVulkanDevices() try {
    if (UISettings::values.has_broken_vulkan) {
        return;
    }

    using namespace Vulkan;

    auto* window = this->window()->windowHandle();
    auto wsi = QtCommon::GetWindowSystemInfo(window);

    vk::InstanceDispatch dld;
    const Common::DynamicLibrary library = OpenLibrary();
    const vk::Instance instance = CreateInstance(library, dld, VK_API_VERSION_1_1, wsi.type);
    const std::vector<VkPhysicalDevice> physical_devices = instance.EnumeratePhysicalDevices();
    vk::SurfaceKHR surface = CreateSurface(instance, wsi);

    vulkan_devices.clear();
    vulkan_devices.reserve(physical_devices.size());
    device_present_modes.clear();
    device_present_modes.reserve(physical_devices.size());
    for (const VkPhysicalDevice device : physical_devices) {
        const auto physical_device = vk::PhysicalDevice(device, dld);
        const std::string name = physical_device.GetProperties().deviceName;
        const std::vector<VkPresentModeKHR> present_modes =
            physical_device.GetSurfacePresentModesKHR(*surface);
        vulkan_devices.push_back(QString::fromStdString(name));
        device_present_modes.push_back(present_modes);

        VkPhysicalDeviceDriverProperties driver_properties{};
        driver_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        driver_properties.pNext = nullptr;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
        properties.pNext = &driver_properties;
        dld.vkGetPhysicalDeviceProperties2(physical_device, &properties);
        if (driver_properties.driverID == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS) {
            expose_compute_option();
        }
    }
} catch (const Vulkan::vk::Exception& exception) {
    LOG_ERROR(Frontend, "Failed to enumerate devices with error: {}", exception.what());
}

Settings::RendererBackend ConfigureGraphics::GetCurrentGraphicsBackend() const {
    if (Settings::IsConfiguringGlobal()) {
        return static_cast<Settings::RendererBackend>(ui->api->currentIndex());
    }

    if (ui->api->currentIndex() == ConfigurationShared::USE_GLOBAL_INDEX) {
        Settings::values.renderer_backend.SetGlobal(true);
        return Settings::values.renderer_backend.GetValue();
    }
    Settings::values.renderer_backend.SetGlobal(false);
    return static_cast<Settings::RendererBackend>(ui->api->currentIndex() -
                                                  ConfigurationShared::USE_GLOBAL_OFFSET);
}

Settings::NvdecEmulation ConfigureGraphics::GetCurrentNvdecEmulation() const {
    if (Settings::IsConfiguringGlobal()) {
        return static_cast<Settings::NvdecEmulation>(ui->nvdec_emulation->currentIndex());
    }

    if (ui->nvdec_emulation->currentIndex() == ConfigurationShared::USE_GLOBAL_INDEX) {
        Settings::values.nvdec_emulation.SetGlobal(true);
        return Settings::values.nvdec_emulation.GetValue();
    }
    Settings::values.nvdec_emulation.SetGlobal(false);
    return static_cast<Settings::NvdecEmulation>(ui->nvdec_emulation->currentIndex() -
                                                 ConfigurationShared::USE_GLOBAL_OFFSET);
}

void ConfigureGraphics::SetupPerGameUI() {
    if (Settings::IsConfiguringGlobal()) {
        ui->api->setEnabled(Settings::values.renderer_backend.UsingGlobal());
        ui->device->setEnabled(Settings::values.renderer_backend.UsingGlobal());
        ui->fullscreen_mode_combobox->setEnabled(Settings::values.fullscreen_mode.UsingGlobal());
        ui->aspect_ratio_combobox->setEnabled(Settings::values.aspect_ratio.UsingGlobal());
        ui->resolution_combobox->setEnabled(Settings::values.resolution_setup.UsingGlobal());
        ui->scaling_filter_combobox->setEnabled(Settings::values.scaling_filter.UsingGlobal());
        ui->fsr_sharpening_slider->setEnabled(Settings::values.fsr_sharpening_slider.UsingGlobal());
        ui->anti_aliasing_combobox->setEnabled(Settings::values.anti_aliasing.UsingGlobal());
        ui->use_asynchronous_gpu_emulation->setEnabled(
            Settings::values.use_asynchronous_gpu_emulation.UsingGlobal());
        ui->nvdec_emulation->setEnabled(Settings::values.nvdec_emulation.UsingGlobal());
        ui->accelerate_astc->setEnabled(Settings::values.accelerate_astc.UsingGlobal());
        ui->use_disk_shader_cache->setEnabled(Settings::values.use_disk_shader_cache.UsingGlobal());
        ui->bg_button->setEnabled(Settings::values.bg_red.UsingGlobal());
        ui->fsr_slider_layout->setEnabled(Settings::values.fsr_sharpening_slider.UsingGlobal());

        return;
    }

    connect(ui->bg_combobox, qOverload<int>(&QComboBox::activated), this, [this](int index) {
        ui->bg_button->setEnabled(index == 1);
        ConfigurationShared::SetHighlight(ui->bg_layout, index == 1);
    });

    connect(ui->fsr_sharpening_combobox, qOverload<int>(&QComboBox::activated), this,
            [this](int index) {
                ui->fsr_sharpening_slider->setEnabled(index == 1);
                ui->fsr_sharpening_value->setEnabled(index == 1);
                ConfigurationShared::SetHighlight(ui->fsr_sharpening_layout, index == 1);
            });

    ConfigurationShared::SetColoredTristate(
        ui->use_disk_shader_cache, Settings::values.use_disk_shader_cache, use_disk_shader_cache);
    ConfigurationShared::SetColoredTristate(ui->accelerate_astc, Settings::values.accelerate_astc,
                                            accelerate_astc);
    ConfigurationShared::SetColoredTristate(ui->use_asynchronous_gpu_emulation,
                                            Settings::values.use_asynchronous_gpu_emulation,
                                            use_asynchronous_gpu_emulation);

    ConfigurationShared::SetColoredComboBox(ui->aspect_ratio_combobox, ui->ar_label,
                                            Settings::values.aspect_ratio.GetValue(true));
    ConfigurationShared::SetColoredComboBox(
        ui->fullscreen_mode_combobox, ui->fullscreen_mode_label,
        static_cast<int>(Settings::values.fullscreen_mode.GetValue(true)));
    ConfigurationShared::SetColoredComboBox(
        ui->resolution_combobox, ui->resolution_label,
        static_cast<int>(Settings::values.resolution_setup.GetValue(true)));
    ConfigurationShared::SetColoredComboBox(
        ui->scaling_filter_combobox, ui->scaling_filter_label,
        static_cast<int>(Settings::values.scaling_filter.GetValue(true)));
    ConfigurationShared::SetColoredComboBox(
        ui->anti_aliasing_combobox, ui->anti_aliasing_label,
        static_cast<int>(Settings::values.anti_aliasing.GetValue(true)));
    ConfigurationShared::InsertGlobalItem(
        ui->api, static_cast<int>(Settings::values.renderer_backend.GetValue(true)));
    ConfigurationShared::InsertGlobalItem(
        ui->nvdec_emulation, static_cast<int>(Settings::values.nvdec_emulation.GetValue(true)));

    ui->vsync_mode_layout->setVisible(false);
}
