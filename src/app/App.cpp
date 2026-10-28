#include "app/App.h"

#include "Version.h"
#include "core/Log.h"
#include "gfx/ImageIO.h"
#include "gfx/Shader.h"
#include "ui/Ui.h"

#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <shellapi.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wparam,
                                                             LPARAM lparam);

namespace ex::app {

namespace {

constexpr wchar_t kWindowClass[] = L"ExplosionSimulatorWindow";

class Clock {
public:
    Clock() {
        QueryPerformanceFrequency(&frequency_);
        QueryPerformanceCounter(&start_);
    }

    double Now() const {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return double(counter.QuadPart - start_.QuadPart) / double(frequency_.QuadPart);
    }

private:
    LARGE_INTEGER frequency_{};
    LARGE_INTEGER start_{};
};

long long UnixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::wstring UpdateDownloadDirectory() {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD length =
        GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, DWORD(MAX_PATH * 2));
    std::wstring directory = length > 0 ? std::wstring(buffer, length) : std::wstring(L".");
    directory += L"\\ExplosionSim\\updates";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return directory;
}

std::wstring ExecutableDirectory() {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, DWORD(MAX_PATH * 2));
    if (length == 0) {
        return {};
    }
    std::wstring path(buffer, length);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

std::wstring TempDirectory() {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD length = GetTempPathW(DWORD(MAX_PATH * 2), buffer);
    return length > 0 ? std::wstring(buffer, length) : std::wstring(L".");
}

bool RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    windowClass.lpfnWndProc = App::WndProcStatic;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = windowClass.hIcon;
    return RegisterClassExW(&windowClass) != 0;
}

} // namespace

int App::Run(const LaunchOptions& options) {
    options_ = options;

    LoadSettings(settings_, DefaultSettingsPath());

    if (options_.chargeMassKg > 0.0) {
        settings_.simulation.chargeMassKg = options_.chargeMassKg;
    }
    if (options_.heightOfBurstM >= 0.0) {
        settings_.simulation.heightOfBurstM = options_.heightOfBurstM;
    }
    if (options_.highFidelity) {
        settings_.simulation.mode = sim::SimulationMode::HighFidelity;
    } else if (options_.reducedOrder) {
        settings_.simulation.mode = sim::SimulationMode::ReducedOrder;
    }
    if (options_.solverResolution > 0) {
        settings_.simulation.solverResolution =
            std::clamp(options_.solverResolution, 32, 512);
    }
    if (options_.solverSteps > 0) {
        settings_.simulation.solverStepsPerFrame = std::clamp(options_.solverSteps, 8, 256);
    }
    if (options_.volumeResolution > 0) {
        settings_.simulation.volumeResolution =
            std::clamp(options_.volumeResolution, 32, 1024);
    }
    if (options_.autoScaleVolumeSet) {
        settings_.simulation.autoScaleVolume = options_.autoScaleVolume;
    }
    if (options_.gpuVolumeBakeSet) {
        settings_.simulation.gpuVolumeBake = options_.gpuVolumeBake;
    }
    if (options_.solverAutoScaleSet) {
        settings_.simulation.solverAutoScale = options_.solverAutoScale;
    }
    if (options_.sceneSeedSet) {
        settings_.simulation.scene.seed = options_.sceneSeed;
    }
    if (options_.sceneEnabledSet) {
        settings_.simulation.scene.enabled = options_.sceneEnabled;
    }
    if (options_.sceneDensitySet) {
        settings_.simulation.scene.density =
            std::clamp(options_.sceneDensity, 0.0f, 4.0f);
    }
    if (options_.solverBenchmark) {
        // Solver-only: no town generation, no CPU volume bakes and a fixed grid.
        settings_.simulation.mode = sim::SimulationMode::HighFidelity;
        settings_.simulation.scene.enabled = false;
        settings_.simulation.bakeVolume = false;
        settings_.simulation.solverAutoScale = false;
    }
    if (options_.checkUpdatesNow) {
        return RunUpdateCheck();
    }

    if (!options_.headless && !CreateMainWindow()) {
        return 1;
    }

    if (!InitializeGraphics()) {
        return 1;
    }

    if (!options_.headless && !InitializeImGui()) {
        return 1;
    }

    pool_ = std::make_unique<core::ThreadPool>();
    // The interactive app generates the town on a worker thread so the loading
    // bar can animate; headless captures and tests stay synchronous.
    simulation_.SetAsyncSceneGeneration(!options_.headless);
    simulation_.Initialize(settings_.simulation, *pool_, !options_.headless);
    FrameCamera();

    const std::wstring changelogPath = gfx::ResolveAssetPath(L"CHANGELOG.md");
    if (!changelogPath.empty()) {
        changelogText_ = ReadTextFile(changelogPath);
        changelogSections_ = ParseChangelog(changelogText_);
        currentReleaseNotes_ =
            CurrentReleaseNotes(changelogSections_, EXPLOSION_VERSION_STRING);
    }
    if (currentReleaseNotes_.empty()) {
        currentReleaseNotes_ = "No changelog is bundled with this build.";
    }
    showWhatsNew_ = !options_.headless &&
                    settings_.update.lastSeenVersion != EXPLOSION_VERSION_STRING;

    if (!options_.headless && !options_.noUpdateCheck && settings_.update.checkForUpdates) {
        const long long now = UnixNow();
        if (now - settings_.update.lastCheckUnix >= 24 * 3600) {
            updateChecker_.CheckAsync(EXPLOSION_VERSION_STRING,
                                      settings_.update.skippedVersion,
                                      settings_.update.proxy, false);
            settings_.update.lastCheckUnix = now;
        }
    }

    LogInfo("Simulation ready: %.4g kg TNT, %.2f m burst height, %d workers",
            settings_.simulation.chargeMassKg, settings_.simulation.heightOfBurstM,
            int(pool_->WorkerCount()));

    if (!options_.headless) {
        timeBeginPeriod(1);
    }
    const int result = options_.headless ? RunHeadless() : RunInteractive();
    if (!options_.headless) {
        timeEndPeriod(1);
    }

    Shutdown();
    return result;
}

bool App::CreateMainWindow() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!RegisterWindowClass(instance)) {
        LogError("Failed to register window class.");
        return false;
    }

    const DWORD style = WS_OVERLAPPEDWINDOW;
    const DWORD extendedStyle = 0;
    RECT rect{0, 0, options_.width, options_.height};
    AdjustWindowRectExForDpi(&rect, style, FALSE, extendedStyle, GetDpiForSystem());

    const int windowWidth = rect.right - rect.left;
    const int windowHeight = rect.bottom - rect.top;

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int x = workArea.left + ((workArea.right - workArea.left) - windowWidth) / 2;
    const int y = workArea.top + ((workArea.bottom - workArea.top) - windowHeight) / 2;

    const std::wstring title =
        std::wstring(L"Explosion Simulator ") + EXPLOSION_VERSION_WSTRING;

    window_ = CreateWindowExW(extendedStyle, kWindowClass, title.c_str(), style, x, y, windowWidth,
                              windowHeight, nullptr, nullptr, instance, this);
    if (!window_) {
        LogError("Failed to create the application window.");
        return false;
    }

    const BOOL darkMode = TRUE;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    SetWindowTextW(window_, title.c_str());
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
    return true;
}

bool App::InitializeGraphics() {
    if (!graphics_.Init(window_, options_.width, options_.height, options_.headless,
                        options_.debugLayer)) {
        return false;
    }
    graphicsReady_ = true;

    camera_.SetViewport(graphics_.Width(), graphics_.Height());
    camera_.SetFovY(DirectX::XMConvertToRadians(48.0f));

    if (!renderer_.Initialize(graphics_)) {
        return false;
    }
    return true;
}

bool App::InitializeImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    dpiScale_ = std::max(1.0f, float(GetDpiForWindow(window_)) / 96.0f);

    if (!ImGui_ImplWin32_Init(window_)) {
        LogError("ImGui Win32 backend initialisation failed.");
        return false;
    }
    if (!ImGui_ImplDX11_Init(graphics_.Device(), graphics_.Context())) {
        LogError("ImGui DirectX 11 backend initialisation failed.");
        return false;
    }

    imguiReady_ = true;
    ui::LoadFonts(dpiScale_);
    ui::ApplyTheme(dpiScale_);
    return true;
}

void App::Shutdown() {
    updateChecker_.Shutdown();
    if (pool_) {
        simulation_.Shutdown();
    }
    renderer_.Shutdown();

    if (imguiReady_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiReady_ = false;
    }

    if (graphicsReady_) {
        graphics_.Shutdown();
        graphicsReady_ = false;
    }

    if (window_) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));

    if (!options_.headless) {
        SaveSettings(settings_, DefaultSettingsPath());
    }
}

void App::HandleCameraInput(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureMouse) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            camera_.Orbit(io.MouseDelta.x, io.MouseDelta.y);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) ||
            ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
            camera_.Pan(io.MouseDelta.x, io.MouseDelta.y);
        }
        if (io.MouseWheel != 0.0f) {
            camera_.Zoom(io.MouseWheel);
        }
    }

    camera_.Update(dt);
    camera_.SetViewport(graphics_.Width(), graphics_.Height());
}

void App::FrameCamera() {
    const sim::BlastModel& blast = simulation_.Blast();
    const sim::Charge& charge = blast.GetCharge();

    if (simulation_.Settings().mode == sim::SimulationMode::HighFidelity) {
        // Prefer the cloud the solver actually produced. The analytic model
        // runs ahead of the resolved plume, so framing it leaves the real head
        // sitting low in the view. Fall back to the analytic cloud only before
        // the first reduction has measured anything.
        const gfx::SolverStats& solverStats = renderer_.Solver();
        const sim::BlastState& state = blast.State();
        const float sceneExtent = simulation_.Scene().Extent();
        float centerY = float(state.cloudCenterY);
        float top = 0.0f;
        float half = 0.0f;
        if (solverStats.active && solverStats.fieldBoundsValid &&
            solverStats.plumeTop > 1e-3f) {
            top = solverStats.plumeTop;
            half = std::max(solverStats.plumeRadius, 0.5f * top);
            centerY = 0.5f * top;
        } else {
            const float fireball = float(blast.FireballMaxRadiusM());
            const float cloudRadius = std::max(float(state.cloudRadius), 3.0f * fireball);
            top = float(state.cloudCenterY) + 2.4f * cloudRadius;
            half = std::max(cloudRadius * 1.6f, 3.0f * fireball);
            top = std::max(top, 2.0f * half);
        }
        const float distance =
            std::max({top * 1.15f, half * 2.6f, sceneExtent * 0.9f, 8.0f}) *
            std::max(0.1f, options_.distanceScale);

        // Match the wind drift applied to the rendered volume in Renderer::Render
        // so "Frame camera" follows the downwind cloud instead of the origin.
        const sim::Environment& environment = blast.GetEnvironment();
        const double drift = solverStats.active ? solverStats.time : 0.0;
        const float windX = float(environment.WindX() * drift);
        const float windZ = float(environment.WindZ() * drift);
        camera_.SetTarget(DirectX::XMFLOAT3(windX, centerY, windZ), distance);
        if (options_.headless) {
            camera_.SetOrientation(options_.azimuth, options_.elevation);
        }
        camera_.Snap();
        return;
    }

    // Frame the maximum fireball / early cloud rather than the current (still
    // tiny) state so the camera does not jump right after detonation.
    const float fireball = float(blast.FireballMaxRadiusM());
    const float cloud = float(simulation_.Blast().State().cloudRadius);
    const float burstHeight = float(charge.heightOfBurstM);
    const float sceneExtent = simulation_.Scene().Extent();

    const float radius =
        std::max({fireball * 3.2f, cloud * 2.4f, burstHeight * 1.6f,
                  sceneExtent * 0.8f, 1.2f});
    const float centerY = burstHeight + fireball * 0.6f;

    camera_.SetTarget(DirectX::XMFLOAT3(0.0f, centerY, 0.0f),
                      radius * 1.9f * std::max(0.1f, options_.distanceScale));
    if (options_.headless) {
        camera_.SetOrientation(options_.azimuth, options_.elevation);
    }
    camera_.Snap();
}

void App::ApplySimulationSettings() {
    const sim::SimulationMode previousMode = simulation_.Settings().mode;
    simulation_.ApplySettings(settings_.simulation);
    if (previousMode != simulation_.Settings().mode) {
        FrameCamera();
    }
    simulation_.FinishPendingVolumeWork();
}

void App::RestartSimulation() {
    simulation_.Reset();
    simulation_.FinishPendingVolumeWork();
}

void App::RearmSimulation() {
    simulation_.Rearm();
    simulation_.FinishPendingVolumeWork();
}

bool App::CaptureScreenshot(const std::wstring& path) {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    if (!graphics_.ReadBack(pixels, width, height)) {
        LogError("Failed to read back the frame buffer.");
        return false;
    }
    return gfx::SavePng(path, pixels.data(), width, height);
}

std::wstring App::NextScreenshotPath() const {
    wchar_t buffer[64];
    swprintf_s(buffer, L"screenshot_%04d.png", screenshotCounter_);
    return buffer;
}

int App::RunInteractive() {
    Clock clock;
    double lastTime = clock.Now();
    double smoothedFrameMs = 16.0;

    while (running_) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running_ = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running_) {
            break;
        }

        if (IsIconic(window_)) {
            Sleep(16);
            continue;
        }

        const double now = clock.Now();
        const float frameDt = std::clamp(float(now - lastTime), 0.0f, 1.0f);
        lastTime = now;
        const float dt = std::clamp(frameDt, 0.0f, 0.1f);

        smoothedFrameMs = smoothedFrameMs * 0.9 + double(frameDt) * 1000.0 * 0.1;
        statAccumulator_ += frameDt;
        ++statFrames_;
        if (statAccumulator_ >= 0.4) {
            fps_ = float(statFrames_ / statAccumulator_);
            statAccumulator_ = 0.0;
            statFrames_ = 0;
        }
        frameMs_ = float(smoothedFrameMs);

        simulation_.PollSceneGeneration();
        if (simulation_.ConsumeScenePreview() && !sceneFramed_) {
            // Show and frame the intact layout while the chunks are fractured.
            FrameCamera();
            sceneFramed_ = true;
        }
        if (simulation_.ConsumeSceneCompletion() && !sceneFramed_) {
            // The first town is ready; frame it once. Later resets keep the
            // camera where the user left it.
            FrameCamera();
            sceneFramed_ = true;
        }
        const bool loadingScene = simulation_.SceneGenerating();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        HandleCameraInput(dt);

        bool forceRestart = false;
        bool forceRearm = false;
        bool forceDetonate = false;
        bool stepOnce = false;
        bool captureNow = false;
        if (!io.WantCaptureKeyboard) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
                settings_.paused = !settings_.paused;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_D)) {
                if (simulation_.IsArmed()) {
                    forceDetonate = true;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_R)) {
                forceRearm = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F)) {
                FrameCamera();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_H)) {
                if (settings_.showUi && settings_.showOverlays) {
                    settings_.showUi = false;
                } else if (!settings_.showUi && settings_.showOverlays) {
                    settings_.showOverlays = false;
                } else {
                    settings_.showUi = true;
                    settings_.showOverlays = true;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F12)) {
                captureNow = true;
            }
        }

        if (!settings_.paused && !loadingScene) {
            if (simulation_.Settings().mode == sim::SimulationMode::HighFidelity) {
                if (simulation_.IsArmed()) {
                    simulation_.Update(frameDt);
                } else {
                    const double advance = renderer_.AdvanceSolver(simulation_, frameDt);
                    if (advance > 0.0) {
                        simulation_.AdvanceExternal(advance, frameDt);
                    }
                }
            } else {
                simulation_.Update(frameDt);
            }
        }
        renderer_.SetSolverPaused(settings_.paused || loadingScene);

        ui::UiContext context{settings_, simulation_, camera_, renderer_};
        context.fps = fps_;
        context.frameMs = frameMs_;
        context.workerThreads = int(pool_->WorkerCount());
        context.adapterName = graphics_.AdapterName().c_str();

        const update::UpdateChecker::Snapshot updateSnapshot = updateChecker_.GetSnapshot();
        ui::UpdateUiState updateState;
        updateState.enabled = settings_.update.checkForUpdates;
        updateState.checking = updateSnapshot.status == update::UpdateStatus::Checking;
        updateState.available = updateSnapshot.status == update::UpdateStatus::Available;
        updateState.downloading = updateSnapshot.status == update::UpdateStatus::Downloading;
        updateState.downloaded = updateSnapshot.status == update::UpdateStatus::Downloaded;
        updateState.failed = updateSnapshot.status == update::UpdateStatus::Failed;
        updateState.upToDate = updateSnapshot.status == update::UpdateStatus::UpToDate;
        updateState.version = updateSnapshot.release.version;
        updateState.notes = updateSnapshot.release.notes;
        updateState.error = updateSnapshot.error;
        updateState.assetName = updateSnapshot.release.assetName;
        updateState.progress = updateSnapshot.progress;
        updateState.downloadedBytes = updateSnapshot.downloaded;
        updateState.totalBytes = updateSnapshot.total;
        updateState.verified = updateSnapshot.verified;
        updateState.downloadFailed = updateSnapshot.downloadFailed;
        context.update = updateState;
        context.changelogText = &changelogText_;
        context.currentReleaseNotes = &currentReleaseNotes_;
        context.showWhatsNew = showWhatsNew_;

        const ui::UiActions actions = ui::Draw(context);
        if (actions.stepOnce) {
            stepOnce = true;
        }
        if (actions.restart) {
            forceRestart = true;
        }
        if (actions.rearm) {
            forceRearm = true;
        }
        if (actions.frameCamera) {
            FrameCamera();
        }
        if (!loadingScene) {
            if (actions.detonateNow || forceDetonate) {
                if (simulation_.IsArmed()) {
                    simulation_.DetonateNow();
                    simulation_.FinishPendingVolumeWork();
                } else if (actions.detonateNow) {
                    forceRearm = true;
                }
            }
            if (stepOnce) {
                if (simulation_.Settings().mode == sim::SimulationMode::HighFidelity) {
                    if (!simulation_.IsArmed()) {
                        renderer_.StepSolverOnce(8);
                        const double advance = renderer_.ConsumeSolverAdvance();
                        if (advance > 0.0) {
                            simulation_.AdvanceExternal(advance, frameDt);
                        }
                    }
                } else {
                    simulation_.StepFixed(1.0 / 60.0);
                }
            }
            if (forceRestart) {
                RestartSimulation();
            }
            if (forceRearm) {
                RearmSimulation();
            }
        }
        if (actions.applySimulationSettings) {
            ApplySimulationSettings();
        }

        if (actions.checkForUpdates) {
            updateChecker_.CheckAsync(EXPLOSION_VERSION_STRING,
                                      settings_.update.skippedVersion,
                                      settings_.update.proxy, true);
            settings_.update.lastCheckUnix = UnixNow();
        }
        if (actions.downloadUpdate && !updateSnapshot.release.assetUrl.empty()) {
            const std::wstring directory = UpdateDownloadDirectory();
            const std::wstring fileName(updateSnapshot.release.assetName.begin(),
                                        updateSnapshot.release.assetName.end());
            updateChecker_.DownloadAsync(updateSnapshot.release,
                                         directory + L"\\" + fileName,
                                         settings_.update.proxy);
        }
        if (actions.cancelDownload) {
            updateChecker_.CancelDownload();
        }
        if (actions.openDownloadFolder) {
            const std::wstring directory = UpdateDownloadDirectory();
            ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
        }
        if (actions.installUpdate && !updateSnapshot.release.assetName.empty()) {
            const std::wstring directory = UpdateDownloadDirectory();
            const std::wstring fileName(updateSnapshot.release.assetName.begin(),
                                        updateSnapshot.release.assetName.end());
            if (StartUpdater(directory + L"\\" + fileName, updateSnapshot.release.sha256)) {
                running_ = false;
            }
        }
        if (actions.skipUpdateVersion && !updateSnapshot.release.version.empty()) {
            settings_.update.skippedVersion = updateSnapshot.release.version;
            SaveSettings(settings_, DefaultSettingsPath());
        }
        if (actions.dismissWhatsNew) {
            settings_.update.lastSeenVersion = EXPLOSION_VERSION_STRING;
            showWhatsNew_ = false;
            SaveSettings(settings_, DefaultSettingsPath());
        }

        ImGui::Render();

        renderer_.Render(simulation_, camera_, settings_.render);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        if (captureNow) {
            ++screenshotCounter_;
            const std::wstring path = NextScreenshotPath();
            CaptureScreenshot(path);
        }

        graphics_.Present(options_.vsync);

        // Optional frame-rate cap. The wait is included in the next frame's
        // wall-clock delta, so the high-fidelity time-scale target stays honest.
        const double targetInterval =
            1.0 / double(std::clamp(settings_.maxFps, 30, 144));
        while (clock.Now() - now < targetInterval) {
            const double remaining = targetInterval - (clock.Now() - now);
            if (remaining > 0.0015) {
                std::this_thread::sleep_for(
                    std::chrono::duration<double>(remaining - 0.001));
            } else {
                std::this_thread::yield();
            }
        }
    }

    return 0;
}

int App::RunUpdateCheck() {
    update::UpdateChecker checker;
    checker.CheckAsync(EXPLOSION_VERSION_STRING, settings_.update.skippedVersion,
                       settings_.update.proxy, true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        const update::UpdateChecker::Snapshot snapshot = checker.GetSnapshot();
        if (snapshot.status == update::UpdateStatus::Available) {
            LogInfo("Update available: %s (%s, %.1f MB)", snapshot.release.version.c_str(),
                    snapshot.release.assetName.c_str(),
                    double(snapshot.release.assetSize) / (1024.0 * 1024.0));
            checker.Shutdown();
            return 0;
        }
        if (snapshot.status == update::UpdateStatus::UpToDate) {
            LogInfo("Up to date: version %s is the latest release.",
                    EXPLOSION_VERSION_STRING);
            checker.Shutdown();
            return 0;
        }
        if (snapshot.status == update::UpdateStatus::Failed) {
            LogError("Update check failed: %s", snapshot.error.c_str());
            checker.Shutdown();
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    LogError("Update check timed out.");
    checker.Shutdown();
    return 1;
}

bool App::StartUpdater(const std::wstring& zipPath, const std::string& sha256) {
    const std::wstring updater = gfx::ResolveAssetPath(L"ExplosionSimUpdater.exe");
    if (updater.empty()) {
        LogError("Update helper not found next to the executable.");
        return false;
    }
    const std::wstring target = ExecutableDirectory();
    if (target.empty()) {
        LogError("Cannot determine the installation directory.");
        return false;
    }

    // Launch from a temporary copy so the helper can replace its installed copy.
    const std::wstring tempHelper =
        TempDirectory() + L"ExplosionSimUpdater-" + std::to_wstring(GetCurrentProcessId()) +
        L".exe";
    std::error_code error;
    std::filesystem::copy_file(updater, tempHelper,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        LogError("Could not stage the update helper: %s", error.message().c_str());
        return false;
    }

    std::wstring command = L"\"" + tempHelper + L"\" --pid " +
                           std::to_wstring(GetCurrentProcessId()) + L" --zip \"" +
                           zipPath + L"\" --target \"" + target +
                           L"\" --exe ExplosionSim.exe";
    if (!sha256.empty()) {
        command += L" --sha256 " + std::wstring(sha256.begin(), sha256.end());
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, target.c_str(), &startup, &process)) {
        LogError("Could not start the update helper.");
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

int App::RunSolverBenchmark() {
    const long long steps = options_.solverStepCount > 0 ? options_.solverStepCount : 256;
    const int stepsPerFrame = options_.solverSteps > 0
                                  ? options_.solverSteps
                                  : simulation_.Settings().solverStepsPerFrame;

    const gfx::SolverBenchmarkResult result =
        renderer_.BenchmarkSolver(simulation_, steps, stepsPerFrame);
    if (!result.valid) {
        LogError("Solver benchmark failed.");
        return 1;
    }

    const auto passMs = [&result](gfx::SolverPass pass) {
        return result.timing.passMs[int(pass)];
    };
    const auto passCount = [&result](gfx::SolverPass pass) {
        return result.timing.passCount[int(pass)];
    };
    const double sweepMs = passMs(gfx::SolverPass::SweepX) +
                           passMs(gfx::SolverPass::SweepY) +
                           passMs(gfx::SolverPass::SweepZ);

    LogInfo("Solver benchmark: %d^3, %lld steps in %d frames of %d, domain half %.2f m, "
            "voxel %.1f mm",
            result.resolution, result.steps, result.frames, result.stepsPerFrame,
            result.domainHalfExtent, result.voxelSize * 1000.0f);
    LogInfo("  GPU total %.2f ms (%.3f ms/step, %.3f ms/frame), CPU submit %.2f ms, "
            "wall %.2f ms",
            result.timing.totalMs, result.msPerStep, result.msPerFrame, result.submissionMs,
            result.wallMs);
    LogInfo("  sweeps %.2f ms (X %.2f, Y %.2f, Z %.2f), reduce %.2f ms (%d passes), "
            "render %.2f ms (%d passes)",
            sweepMs, passMs(gfx::SolverPass::SweepX), passMs(gfx::SolverPass::SweepY),
            passMs(gfx::SolverPass::SweepZ), passMs(gfx::SolverPass::Reduce),
            passCount(gfx::SolverPass::Reduce), passMs(gfx::SolverPass::Render),
            passCount(gfx::SolverPass::Render));
    return 0;
}

int App::RunHeadless() {
    if (options_.solverBenchmark) {
        return RunSolverBenchmark();
    }

    Clock clock;

    // Screenshots are specified in post-detonation simulation time.
    settings_.simulation.fuseSeconds = 0.0;
    simulation_.ApplySettings(settings_.simulation);

    const bool highFidelity =
        simulation_.Settings().mode == sim::SimulationMode::HighFidelity;
    const bool stepCapture = highFidelity && options_.solverStepCount > 0;

    const double step = 1.0 / 120.0;
    const double targetTime = std::max(0.0, options_.captureTime);
    // In high-fidelity mode the solver drives the analytic model (see
    // PrepareHighFidelity*), so it must not be advanced twice here.
    if (!stepCapture && !highFidelity) {
        int guard = 0;
        while (simulation_.Time() + 1e-9 < targetTime && guard++ < 200000) {
            simulation_.StepFixed(std::min(step, targetTime - simulation_.Time()));
        }
    }

    simulation_.Volume().WaitForIdle();
    simulation_.Volume().Poll();

    const double solverTarget =
        options_.solverTime >= 0.0 ? options_.solverTime : targetTime;
    if (stepCapture) {
        renderer_.PrepareHighFidelitySteps(simulation_, options_.solverStepCount);
    } else if (highFidelity) {
        renderer_.PrepareHighFidelity(simulation_, solverTarget);
    } else {
        simulation_.Volume().BakeNow(simulation_.Blast());
        simulation_.Volume().Poll();
    }

    FrameCamera();

    renderer_.Render(simulation_, camera_, settings_.render);
    graphics_.Present(false);

    if (options_.benchmark) {
        const int frames = std::max(1, options_.benchmarkFrames);
        const double start = clock.Now();
        for (int frame = 0; frame < frames; ++frame) {
            renderer_.Render(simulation_, camera_, settings_.render);
            graphics_.Present(false);
            graphics_.Context()->Flush();
        }
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
        graphics_.ReadBack(pixels, width, height);

        const double elapsed = clock.Now() - start;
        LogInfo("Benchmark: %d frames at %dx%d in %.3f s", frames, width, height, elapsed);
        LogInfo("Average: %.2f ms/frame (%.1f fps)", elapsed * 1000.0 / frames,
                double(frames) / std::max(elapsed, 1e-6));
        LogInfo("Particles: %d, volume bake %.2f ms at %d^3, workers %d",
                simulation_.Particles().AliveCount(), simulation_.Volume().LastBakeMilliseconds(),
                simulation_.Volume().Resolution(), int(pool_->WorkerCount()));
        LogInfo("Simulation time after benchmark: %.3f s (solver %.4f s)",
                simulation_.Time(), renderer_.Solver().time);
        const scene::SceneStats sceneStats = simulation_.Scene().Stats();
        LogInfo("Scene: %d buildings, %d vehicles, %d trees, %d chunks, %d rigid bodies "
                "(%d static structure)",
                sceneStats.buildings, sceneStats.vehicles, sceneStats.trees,
                sceneStats.chunks, sceneStats.bodies, sceneStats.staticBodies);
        LogInfo("Physics: %d awake of %d bodies",
                simulation_.Scene().Physics().AwakeCount(),
                simulation_.Scene().Physics().BodyCount());
    } else {
        if (options_.screenshotPath.empty()) {
            LogError("Headless mode requires --screenshot <path> or --benchmark.");
            return 1;
        }
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
        if (!graphics_.ReadBack(pixels, width, height)) {
            LogError("Failed to read back the rendered frame.");
            return 1;
        }
        if (!gfx::SavePng(options_.screenshotPath, pixels.data(), width, height)) {
            return 1;
        }
        LogInfo("Simulation time %.3f s, %d particles", simulation_.Time(),
                simulation_.Particles().AliveCount());
    }

    return 0;
}

LRESULT CALLBACK App::WndProcStatic(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        if (app) {
            app->window_ = window;
        }
    }

    if (app) {
        return app->WndProc(window, message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT App::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (imguiReady_ && ImGui::GetCurrentContext() &&
        ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) {
        return 1;
    }

    switch (message) {
    case WM_SIZE: {
        if (graphicsReady_ && wparam != SIZE_MINIMIZED) {
            const int width = LOWORD(lparam);
            const int height = HIWORD(lparam);
            if (width > 0 && height > 0) {
                graphics_.Resize(width, height);
                camera_.SetViewport(width, height);
                renderer_.Resize(width, height);
            }
        }
        return 0;
    }
    case WM_DPICHANGED: {
        dpiScale_ = std::max(1.0f, float(HIWORD(wparam)) / 96.0f);
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested) {
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        running_ = false;
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        running_ = false;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace ex::app
