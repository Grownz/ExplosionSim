#include "app/App.h"

#include "core/Log.h"
#include "gfx/ImageIO.h"
#include "gfx/Shader.h"
#include "ui/Ui.h"

#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wparam,
                                                             LPARAM lparam);

namespace ex::app {

namespace {

constexpr wchar_t kWindowClass[] = L"ExplosionSimulatorWindow";
constexpr wchar_t kWindowTitle[] = L"Explosion Simulator";

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

    if (!options_.headless) {
        LoadSettings(settings_, DefaultSettingsPath());
    }

    if (options_.chargeMassKg > 0.0) {
        settings_.simulation.chargeMassKg = options_.chargeMassKg;
    }
    if (options_.heightOfBurstM >= 0.0) {
        settings_.simulation.heightOfBurstM = options_.heightOfBurstM;
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
    simulation_.Initialize(settings_.simulation, *pool_);
    FrameCamera();

    LogInfo("Simulation ready: %.0f kg TNT, %.1f m burst height, %d workers",
            settings_.simulation.chargeMassKg, settings_.simulation.heightOfBurstM,
            int(pool_->WorkerCount()));

    const int result = options_.headless ? RunHeadless() : RunInteractive();

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

    window_ = CreateWindowExW(extendedStyle, kWindowClass, kWindowTitle, style, x, y, windowWidth,
                              windowHeight, nullptr, nullptr, instance, this);
    if (!window_) {
        LogError("Failed to create the application window.");
        return false;
    }

    const BOOL darkMode = TRUE;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    SetWindowTextW(window_, kWindowTitle);
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

    // Frame the maximum fireball / early cloud rather than the current (still
    // tiny) state so the camera does not jump right after detonation.
    const float fireball = float(blast.FireballMaxRadiusM());
    const float cloud = float(simulation_.Blast().State().cloudRadius);
    const float burstHeight = float(charge.heightOfBurstM);

    const float radius =
        std::max({fireball * 3.2f, cloud * 2.4f, burstHeight * 1.6f, 1.2f});
    const float centerY = burstHeight + fireball * 0.6f;

    camera_.SetTarget(DirectX::XMFLOAT3(0.0f, centerY, 0.0f), radius * 1.9f);
    if (options_.headless) {
        camera_.SetOrientation(options_.azimuth, options_.elevation);
    }
    camera_.Snap();
}

void App::ApplySimulationSettings() {
    simulation_.ApplySettings(settings_.simulation);
    simulation_.FinishPendingVolumeWork();
}

void App::RestartSimulation() {
    simulation_.Reset();
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
        float dt = float(now - lastTime);
        lastTime = now;
        dt = std::clamp(dt, 0.0f, 0.1f);

        smoothedFrameMs = smoothedFrameMs * 0.9 + double(dt) * 1000.0 * 0.1;
        statAccumulator_ += dt;
        ++statFrames_;
        if (statAccumulator_ >= 0.4) {
            fps_ = float(statFrames_ / statAccumulator_);
            statAccumulator_ = 0.0;
            statFrames_ = 0;
        }
        frameMs_ = float(smoothedFrameMs);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        HandleCameraInput(dt);

        bool forceRestart = false;
        bool stepOnce = false;
        bool captureNow = false;
        if (!io.WantCaptureKeyboard) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
                settings_.paused = !settings_.paused;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_R)) {
                forceRestart = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F)) {
                FrameCamera();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_H)) {
                settings_.showUi = !settings_.showUi;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F12)) {
                captureNow = true;
            }
        }

        if (!settings_.paused) {
            simulation_.Update(dt);
        }

        ui::UiContext context{settings_, simulation_, camera_, renderer_};
        context.fps = fps_;
        context.frameMs = frameMs_;
        context.workerThreads = int(pool_->WorkerCount());
        context.adapterName = graphics_.AdapterName().c_str();

        const ui::UiActions actions = ui::Draw(context);
        if (actions.stepOnce) {
            stepOnce = true;
        }
        if (actions.restart) {
            forceRestart = true;
        }
        if (actions.frameCamera) {
            FrameCamera();
        }
        if (actions.detonateNow) {
            if (simulation_.IsArmed()) {
                simulation_.DetonateNow();
            } else {
                RestartSimulation();
            }
            simulation_.FinishPendingVolumeWork();
        }
        if (actions.applySimulationSettings) {
            ApplySimulationSettings();
        }

        if (stepOnce) {
            simulation_.StepFixed(1.0 / 60.0);
        }
        if (forceRestart) {
            RestartSimulation();
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
    }

    return 0;
}

int App::RunHeadless() {
    Clock clock;

    // Screenshots are specified in post-detonation simulation time.
    settings_.simulation.fuseSeconds = 0.0;
    simulation_.ApplySettings(settings_.simulation);

    simulation_.Volume().WaitForIdle();
    simulation_.Volume().Poll();

    const double step = 1.0 / 120.0;
    const double targetTime = std::max(0.0, options_.captureTime);
    int guard = 0;
    while (simulation_.Time() + 1e-9 < targetTime && guard++ < 200000) {
        simulation_.StepFixed(std::min(step, targetTime - simulation_.Time()));
    }

    simulation_.Volume().WaitForIdle();
    simulation_.Volume().Poll();
    simulation_.Volume().BakeNow(simulation_.Blast());
    simulation_.Volume().Poll();

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
