#include "app/LinuxUi.h"

#include "app/FontPaths.h"
#include "core/Log.h"
#include "sim/Simulation.h"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <iterator>
#include <string>

namespace ex::app {

LinuxUi::~LinuxUi() {
    Shutdown();
}

bool LinuxUi::Initialize(void* sdlWindow, const LinuxUiHandles& handles) {
    if (!sdlWindow || !handles.device || !handles.renderPass ||
        !handles.instance || !handles.physicalDevice || !handles.queue) {
        return false;
    }
    Shutdown();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Keep the working directory clean; the UI is not user-configurable yet.
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    // Use the same TTF family as the splash and the Windows panel.
    const std::string fontPath = ex::app::UiFontPath();
    ImFont* font = nullptr;
    if (!fontPath.empty()) {
        ImFontConfig fontConfig{};
        fontConfig.OversampleH = 2;
        fontConfig.OversampleV = 1;
        fontConfig.PixelSnapH = true;
        font = io.Fonts->AddFontFromFileTTF(
            fontPath.c_str(), 16.0f, &fontConfig,
            io.Fonts->GetGlyphRangesDefault());
    }
    if (!font) {
        io.Fonts->AddFontDefault();
    }

    if (!ImGui_ImplSDL3_InitForVulkan(static_cast<SDL_Window*>(sdlWindow))) {
        LogWarn("Linux UI: ImGui SDL3 backend initialization failed.");
        ImGui::DestroyContext();
        return false;
    }

    const VkDevice device = reinterpret_cast<VkDevice>(handles.device);
    const VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 8},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 8;
    poolInfo.poolSizeCount = uint32_t(std::size(poolSizes));
    poolInfo.pPoolSizes = poolSizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) !=
        VK_SUCCESS) {
        LogWarn("Linux UI: descriptor pool creation failed.");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = reinterpret_cast<VkInstance>(handles.instance);
    info.PhysicalDevice =
        reinterpret_cast<VkPhysicalDevice>(handles.physicalDevice);
    info.Device = device;
    info.QueueFamily = handles.queueFamily;
    info.Queue = reinterpret_cast<VkQueue>(handles.queue);
    info.DescriptorPool = pool;
    info.RenderPass = reinterpret_cast<VkRenderPass>(handles.renderPass);
    info.MinImageCount = 2;
    info.ImageCount = std::max(2u, handles.imageCount);
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.PipelineCache =
        reinterpret_cast<VkPipelineCache>(handles.pipelineCache);
    info.Subpass = 0;
    if (!ImGui_ImplVulkan_Init(&info)) {
        LogWarn("Linux UI: ImGui Vulkan backend initialization failed.");
        vkDestroyDescriptorPool(device, pool, nullptr);
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    initialized_ = true;
    device_ = handles.device;
    descriptorPool_ = pool;
    LogInfo("Linux UI ready: ImGui %s", IMGUI_VERSION);
    return true;
}

void LinuxUi::Shutdown() {
    if (!initialized_) {
        return;
    }
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (descriptorPool_) {
        vkDestroyDescriptorPool(reinterpret_cast<VkDevice>(device_),
                                reinterpret_cast<VkDescriptorPool>(descriptorPool_),
                                nullptr);
    }
    initialized_ = false;
    device_ = nullptr;
    descriptorPool_ = nullptr;
}

void LinuxUi::ProcessEvent(const void* sdlEvent) {
    if (!initialized_ || !sdlEvent) {
        return;
    }
    const SDL_Event* event = static_cast<const SDL_Event*>(sdlEvent);
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_F1) {
        toggleVisible_ = true;
    }
    ImGui_ImplSDL3_ProcessEvent(event);
}

void LinuxUi::NewFrame() {
    if (!initialized_) {
        return;
    }
    ImGui_ImplSDL3_NewFrame();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
}

void LinuxUi::Draw(LinuxUiState& state, const ex::sim::Simulation& simulation,
                   float frameMs) {
    if (!initialized_) {
        return;
    }
    if (toggleVisible_) {
        state.visible = !state.visible;
        toggleVisible_ = false;
    }
    if (!state.visible) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(360.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Explosion Simulator", &state.visible)) {
        ImGui::End();
        return;
    }

    const double time = simulation.Time();
    const double fps = frameMs > 1e-4f ? 1000.0 / double(frameMs) : 0.0;
    ImGui::Text("Time %.3f s   %.2f ms   %.0f fps", time, double(frameMs),
                fps);
    ImGui::Text("Mode: %s",
                simulation.Settings().mode == ex::sim::SimulationMode::HighFidelity
                    ? "High fidelity"
                    : "Reduced order");

    ImGui::Separator();
    ImGui::Checkbox("Pause", &state.paused);
    ImGui::SameLine();
    if (ImGui::Button("Detonate")) {
        state.detonateRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        state.resetRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Frame camera")) {
        state.reframeRequested = true;
    }

    ImGui::SeparatorText("Blast");
    ImGui::InputDouble("Charge (kg)", &state.chargeMassKg, 0.1, 1.0, "%.3f");
    ImGui::InputDouble("Height of burst (m)", &state.heightOfBurstM, 0.1, 1.0,
                       "%.2f");
    ImGui::InputDouble("Time scale", &state.timeScale, 0.01, 0.1, "%.3f");
    ImGui::BeginDisabled(!state.solverAvailable);
    ImGui::Checkbox("High fidelity", &state.highFidelity);
    ImGui::EndDisabled();
    if (state.highFidelity) {
        ImGui::Checkbox("Flow around objects", &state.flowAround);
        ImGui::SliderInt("Solver grid", &state.solverResolution, 32, 256);
    }
    if (ImGui::Button("Apply blast settings")) {
        state.applySimulation = true;
    }

    ImGui::SeparatorText("Render");
    ImGui::SliderFloat("Exposure", &state.exposure, 0.1f, 3.0f, "%.2f");
    ImGui::SliderFloat("Bloom", &state.bloomStrength, 0.0f, 1.5f, "%.2f");
    ImGui::SliderInt("Raymarch steps", &state.raymarchSteps, 32, 256);
    ImGui::Checkbox("Volume", &state.enableVolume);
    ImGui::Checkbox("Particles", &state.enableParticles);
    ImGui::Checkbox("Ground", &state.enableGround);
    ImGui::Checkbox("Bloom enabled", &state.enableBloom);
    ImGui::Checkbox("Grid", &state.showGrid);

    ImGui::SeparatorText("Updates");
    ImGui::Checkbox("Check on startup", &state.updateCheckEnabled);
    ImGui::SameLine();
    ImGui::BeginDisabled(state.updateBusy);
    if (ImGui::Button("Check now")) {
        state.updateCheckRequested = true;
    }
    ImGui::EndDisabled();
    if (!state.updateStatus.empty()) {
        ImGui::TextWrapped("%s", state.updateStatus.c_str());
    }
    if (state.updateAvailable) {
        if (state.updateReady) {
            if (ImGui::Button("Install and restart")) {
                state.updateInstallRequested = true;
            }
        } else {
            ImGui::BeginDisabled(state.updateBusy);
            if (ImGui::Button("Download update")) {
                state.updateDownloadRequested = true;
            }
            ImGui::EndDisabled();
            if (state.updateBusy) {
                ImGui::ProgressBar(state.updateProgress, ImVec2(-1.0f, 0.0f));
            }
        }
        if (ImGui::Button("Skip this version")) {
            state.updateSkipRequested = true;
        }
        if (!state.updateNotes.empty() &&
            ImGui::CollapsingHeader("Release notes")) {
            ImGui::TextWrapped("%s", state.updateNotes.c_str());
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("F1 toggles this panel");
    if (ImGui::Button("Quit")) {
        state.quitRequested = true;
    }
    ImGui::End();
}

void LinuxUi::Render(void* commandBuffer) {
    if (!initialized_ || !commandBuffer) {
        return;
    }
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(
        ImGui::GetDrawData(),
        reinterpret_cast<VkCommandBuffer>(commandBuffer));
}

void LinuxUi::SetMinImageCount(uint32_t count) {
    if (initialized_ && count > 0) {
        ImGui_ImplVulkan_SetMinImageCount(count);
    }
}

} // namespace ex::app
