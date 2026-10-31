#pragma once

#include <cstdint>
#include <string>

namespace ex::sim {
class Simulation;
}

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

struct LinuxUiState {
    bool visible = true;
    bool paused = false;
    bool applySimulation = false;
    bool resetRequested = false;
    bool detonateRequested = false;
    bool quitRequested = false;
    bool reframeRequested = false;
    bool solverAvailable = false;

    float exposure = 0.8f;
    float bloomStrength = 0.10f;
    int raymarchSteps = 128;
    bool enableVolume = true;
    bool enableParticles = true;
    bool enableGround = true;
    bool enableBloom = true;
    bool showGrid = true;

    double chargeMassKg = 0.2;
    double heightOfBurstM = 0.4;
    double timeScale = 0.06;
    bool highFidelity = false;
    int solverResolution = 128;

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

// Dear ImGui overlay for the native Linux shell. It is optional: the headless
// path and the benchmark never initialize it.
class LinuxUi {
public:
    ~LinuxUi();

    bool Initialize(void* sdlWindow, const LinuxUiHandles& handles);
    void Shutdown();
    void ProcessEvent(const void* sdlEvent);
    void NewFrame();
    void Draw(LinuxUiState& state, const ex::sim::Simulation& simulation,
              float frameMs);
    // Must be called inside the swapchain render pass.
    void Render(void* commandBuffer);
    // Notifies the backend after a swapchain recreation.
    void SetMinImageCount(uint32_t count);

private:
    bool initialized_ = false;
    bool toggleVisible_ = false;
    void* device_ = nullptr;
    void* descriptorPool_ = nullptr;
};

} // namespace ex::app
