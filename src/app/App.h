#pragma once

#include <windows.h>

#include <memory>
#include <string>

#include "app/Settings.h"
#include "core/ThreadPool.h"
#include "gfx/Camera.h"
#include "gfx/GfxDevice.h"
#include "gfx/Renderer.h"
#include "sim/Simulation.h"

struct LaunchOptions {
    std::wstring screenshotPath;
    int width = 1600;
    int height = 900;
    double captureTime = 0.12;
    double chargeMassKg = -1.0;
    double heightOfBurstM = -1.0;
    float azimuth = 128.0f;
    float elevation = 14.0f;
    float distanceScale = 1.0f;
    bool vsync = true;
    bool debugLayer = false;
    bool benchmark = false;
    int benchmarkFrames = 120;
    bool headless = false;
    bool highFidelity = false;
    int solverResolution = 0;
    int solverSteps = 0;
    int volumeResolution = 0;
    bool autoScaleVolume = true;
    bool autoScaleVolumeSet = false;
    bool solverAutoScale = true;
    bool solverAutoScaleSet = false;
    double solverTime = -1.0;
};

namespace ex::app {

class App {
public:
    int Run(const LaunchOptions& options);
    static LRESULT CALLBACK WndProcStatic(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

private:
    bool CreateMainWindow();
    bool InitializeGraphics();
    bool InitializeImGui();
    void Shutdown();

    int RunInteractive();
    int RunHeadless();

    void HandleCameraInput(float dt);
    void FrameCamera();
    void ApplySimulationSettings();
    void RestartSimulation();
    bool CaptureScreenshot(const std::wstring& path);
    std::wstring NextScreenshotPath() const;

    LRESULT WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    LaunchOptions options_{};
    HWND window_ = nullptr;
    bool graphicsReady_ = false;
    bool imguiReady_ = false;
    bool running_ = true;
    float dpiScale_ = 1.0f;

    gfx::GfxDevice graphics_{};
    gfx::Renderer renderer_{};
    gfx::Camera camera_{};
    std::unique_ptr<core::ThreadPool> pool_;
    sim::Simulation simulation_{};
    AppSettings settings_{};

    float fps_ = 0.0f;
    float frameMs_ = 0.0f;
    double statAccumulator_ = 0.0;
    int statFrames_ = 0;
    int screenshotCounter_ = 0;
};

} // namespace ex::app
