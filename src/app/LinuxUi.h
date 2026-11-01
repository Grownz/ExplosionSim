#pragma once

#include "gfx/EulerSolverTypes.h"
#include "sim/Simulation.h"

#include <cstdint>
#include <string>

namespace ex::app {

// Opaque Vulkan handles for the optional Linux ImGui overlay. The app fills
// them from the Vulkan RHI device; the UI implementation casts them back.
struct LinuxUiHandles {
    void* instance = nullptr;
    void* physicalDevice = nullptr;
    void* device = nullptr;
    void* queue = nullptr;
    uint32_t queueFamily = 0;
    void* renderPass = nullptr;
    void* pipelineCache = nullptr;
    uint32_t imageCount = 0;
};

// State shared between the panel and the application loop. The panel edits
// `sim` and the render values; the application applies them and fills the
// read-only information below.
struct LinuxUiState {
    // Panel and playback requests.
    bool visible = true;
    bool paused = false;
    bool applySimulation = false;
    bool restartRequested = false;
    bool rearmRequested = false;
    bool detonateRequested = false;
    bool stepRequested = false;
    bool quitRequested = false;
    bool reframeRequested = false;
    bool solverAvailable = false;

    // Simulation settings under edit (a copy of Simulation::Settings()); the
    // application applies it when `applySimulation` is set.
    ex::sim::SimulationSettings sim;
    double observerDistanceM = 100.0;
    int maxFps = 144;

    // Render settings (applied every frame).
    float exposure = 0.8f;
    float bloomStrength = 0.10f;
    int raymarchSteps = 128;
    int antiAliasing = 0;
    bool enableVolume = true;
    bool enableParticles = true;
    bool enableGround = true;
    bool enableBloom = true;
    bool showGrid = true;
    float sunAzimuthDeg = 302.0f;
    float sunElevationDeg = 13.0f;
    float particleScale = 1.0f;

    // Read-only information from the application.
    float fps = 0.0f;
    float frameMs = 0.0f;
    int workerThreads = 0;
    bool gpuVolumeActive = false;
    bool gpuParticlesActive = false;
    std::string adapterName;
    ex::gfx::SolverStats solver;
    std::string currentReleaseNotes;

    // Update panel.
    bool updateCheckEnabled = true;
    bool updateCheckRequested = false;
    bool updateDownloadRequested = false;
    bool updateInstallRequested = false;
    bool updateSkipRequested = false;
    bool updateAvailable = false;
    bool updateReady = false;
    bool updateBusy = false;
    bool updateFailed = false;
    std::string updateVersion;
    std::string updateNotes;
    std::string updateStatus;
    float updateProgress = 0.0f;
};

// Mouse input for the camera that is not captured by the panel (same mapping
// as the Windows application: left drag orbits, right/middle drag pans, the
// wheel zooms).
struct LinuxCameraInput {
    float orbitX = 0.0f;
    float orbitY = 0.0f;
    float panX = 0.0f;
    float panY = 0.0f;
    float zoom = 0.0f;
};

// Dear ImGui overlay for the native Linux shell. It is optional: the headless
// path and the benchmark never initialize it.
class LinuxUi {
public:
    ~LinuxUi();

    bool Initialize(void* sdlWindow, const LinuxUiHandles& handles);
    void Shutdown();
    void ProcessEvent(const void* sdlEvent);
    // True after the window moved to a display with another scale; the
    // application waits for the GPU and calls ApplyScale().
    bool NeedsRescale() const { return rescalePending_; }
    void ApplyScale();
    void NewFrame();
    // Valid after Draw() of the current frame (it needs the panel hit test).
    LinuxCameraInput CameraInput();
    void Draw(LinuxUiState& state, const ex::sim::Simulation& simulation,
              float frameMs);
    // Must be called inside the swapchain render pass.
    void Render(void* commandBuffer);
    // Notifies the backend after a swapchain recreation.
    void SetMinImageCount(uint32_t count);

private:
    bool BuildFonts();

    bool initialized_ = false;
    bool toggleVisible_ = false;
    bool rescalePending_ = false;
    bool cameraDrag_[3] = {false, false, false};
    void* window_ = nullptr;
    void* device_ = nullptr;
    void* descriptorPool_ = nullptr;
    // Content scale of the display (font size and spacing) and the pixel
    // density of the window (glyph rasterisation density).
    float contentScale_ = 1.0f;
    float pixelDensity_ = 1.0f;
};

} // namespace ex::app
