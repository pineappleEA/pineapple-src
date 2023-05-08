// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QWidget>
#include <qglobal.h>
#include <qnamespace.h>
#include <qobjectdefs.h>

#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/frontend/emu_window.h"

class GMainWindow;
class QCamera;
class QCameraImageCapture;
class QCloseEvent;
class QFocusEvent;
class QKeyEvent;
class QMouseEvent;
class QObject;
class QResizeEvent;
class QShowEvent;
class QTimer;
class QTouchEvent;
class QWheelEvent;

namespace Core {
class System;
} // namespace Core

namespace InputCommon {
class InputSubsystem;
enum class MouseButton;
} // namespace InputCommon

namespace InputCommon::TasInput {
enum class TasState;
} // namespace InputCommon::TasInput

namespace VideoCore {
enum class LoadCallbackStage;
} // namespace VideoCore

class EmuThread final : public QThread {
    Q_OBJECT

public:
    explicit EmuThread(Core::System& system);
    ~EmuThread() override;

    /**
     * Start emulation (on new thread)
     * @warning Only call when not running!
     */
    void run() override;

    /**
     * Sets whether the emulation thread should run or not
     * @param should_run Boolean value, set the emulation thread to running if true
     */
    void SetRunning(bool should_run) {
        // TODO: Prevent other threads from modifying the state until we finish.
        {
            // Notify the running thread to change state.
            std::unique_lock run_lk{m_should_run_mutex};
            m_should_run = should_run;
            m_should_run_cv.notify_one();
        }

        // Wait until paused, if pausing.
        if (!should_run) {
            m_is_running.wait(true);
        }
    }

    /**
     * Check if the emulation thread is running or not
     * @return True if the emulation thread is running, otherwise false
     */
    bool IsRunning() const {
        return m_is_running.load() || m_should_run;
    }

    /**
     * Requests for the emulation thread to immediately stop running
     */
    void ForceStop() {
        LOG_WARNING(Frontend, "Force stopping EmuThread");
        m_stop_source.request_stop();
    }

private:
    void EmulationPaused(std::unique_lock<std::mutex>& lk);
    void EmulationResumed(std::unique_lock<std::mutex>& lk);

private:
    Core::System& m_system;

    std::stop_source m_stop_source;
    std::mutex m_should_run_mutex;
    std::condition_variable_any m_should_run_cv;
    std::atomic<bool> m_is_running{false};
    bool m_should_run{true};

signals:
    /**
     * Emitted when the CPU has halted execution
     *
     * @warning When connecting to this signal from other threads, make sure to specify either
     * Qt::QueuedConnection (invoke slot within the destination object's message thread) or even
     * Qt::BlockingQueuedConnection (additionally block source thread until slot returns)
     */
    void DebugModeEntered();

    /**
     * Emitted right before the CPU continues execution
     *
     * @warning When connecting to this signal from other threads, make sure to specify either
     * Qt::QueuedConnection (invoke slot within the destination object's message thread) or even
     * Qt::BlockingQueuedConnection (additionally block source thread until slot returns)
     */
    void DebugModeLeft();

    void LoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total);
};

class GRenderWindow : public QWidget, public Core::Frontend::EmuWindow {
    Q_OBJECT

public:
    explicit GRenderWindow(GMainWindow* parent, EmuThread* emu_thread_,
                           std::shared_ptr<InputCommon::InputSubsystem> input_subsystem_,
                           Core::System& system_);
    ~GRenderWindow() override;

    // EmuWindow implementation.
    void OnFrameDisplayed() override;
    bool IsShown() const override;
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override;

    void BackupGeometry();
    void RestoreGeometry();
    void restoreGeometry(const QByteArray& geometry_); // overridden
    QByteArray saveGeometry();                         // overridden

    qreal windowPixelRatio() const;

    std::pair<u32, u32> ScaleTouch(const QPointF& pos) const;

    void closeEvent(QCloseEvent* event) override;

    void resizeEvent(QResizeEvent* event) override;

    /// Converts a Qt keybard key into NativeKeyboard key
    static int QtKeyToSwitchKey(Qt::Key qt_keys);

    /// Converts a Qt modifier keys into NativeKeyboard modifier keys
    static int QtModifierToSwitchModifier(Qt::KeyboardModifiers qt_modifiers);

    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

    /// Converts a Qt mouse button into MouseInput mouse button
    static InputCommon::MouseButton QtButtonToMouseButton(Qt::MouseButton button);

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

    void InitializeCamera();
    void FinalizeCamera();

    bool event(QEvent* event) override;

    void focusOutEvent(QFocusEvent* event) override;

    bool InitRenderTarget();

    /// Destroy the previous run's child_widget which should also destroy the child_window
    void ReleaseRenderTarget();

    bool IsLoadingComplete() const;

    void CaptureScreenshot(const QString& screenshot_path);

    /**
     * Instructs the window to re-launch the application using the specified program_index.
     * @param program_index Specifies the index within the application of the program to launch.
     */
    void ExecuteProgram(std::size_t program_index);

    /// Instructs the window to exit the application.
    void Exit();

public slots:
    void OnEmulationStarting(EmuThread* emu_thread_);
    void OnEmulationStopping();
    void OnFramebufferSizeChanged();

signals:
    /// Emitted when the window is closed
    void Closed();
    void FirstFrameDisplayed();
    void ExecuteProgramSignal(std::size_t program_index);
    void ExitSignal();
    void MouseActivity();
    void TasPlaybackStateChanged();

private:
    void TouchBeginEvent(const QTouchEvent* event);
    void TouchUpdateEvent(const QTouchEvent* event);
    void TouchEndEvent();

    void RequestCameraCapture();
    void OnCameraCapture(int requestId, const QImage& img);

    void OnMinimalClientAreaChangeRequest(std::pair<u32, u32> minimal_size) override;

    bool InitializeOpenGL();
    bool InitializeVulkan();
    void InitializeNull();
    bool LoadOpenGL();
    QStringList GetUnsupportedGLExtensions() const;

    EmuThread* emu_thread;
    std::shared_ptr<InputCommon::InputSubsystem> input_subsystem;

    // Main context that will be shared with all other contexts that are requested.
    // If this is used in a shared context setting, then this should not be used directly, but
    // should instead be shared from
    std::shared_ptr<Core::Frontend::GraphicsContext> main_context;

    /// Temporary storage of the screenshot taken
    QImage screenshot_image;

    QByteArray geometry;

    QWidget* child_widget = nullptr;

    bool first_frame = false;
    InputCommon::TasInput::TasState last_tas_state;

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0)) && YUZU_USE_QT_MULTIMEDIA
    bool is_virtual_camera;
    int pending_camera_snapshots;
    std::vector<u32> camera_data;
    std::unique_ptr<QCamera> camera;
    std::unique_ptr<QCameraImageCapture> camera_capture;
    std::unique_ptr<QTimer> camera_timer;
#endif

    Core::System& system;

protected:
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;
};
