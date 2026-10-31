#pragma once

#include "app/SplashFont.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace ex::app {

// SDL3 splash. It keeps the Windows layout (980x640, one still per 5 % step,
// progress bar) and renders through SDL's 2D renderer and the portable stb
// image loader, so it needs no GDI, WIC or a desktop toolkit.
//
// Two modes are supported:
//   * cooperative (Linux shell): Show() creates the window and the caller
//     pumps it between initialisation stages;
//   * threaded (Windows app): ShowThreaded() runs the animation on a worker
//     thread so it keeps moving during the long shader compilation.
class SplashSdl {
public:
    ~SplashSdl();

    bool Show(const std::vector<std::string>& framePaths,
              const std::string& title, const std::string& subtitle);
    bool Show(const std::vector<std::wstring>& framePaths,
              const std::wstring& title, const std::wstring& subtitle);
    bool ShowThreaded(const std::vector<std::string>& framePaths,
                      const std::string& title, const std::string& subtitle);
    bool ShowThreaded(const std::vector<std::wstring>& framePaths,
                      const std::wstring& title, const std::wstring& subtitle);

    void SetProgress(float progress, const std::string& status,
                     float expectedSeconds = 0.35f);
    void SetProgress(float progress, const std::wstring& status,
                     float expectedSeconds = 0.35f);
    // Cooperative mode: processes events, advances the progress and redraws.
    void Pump();
    // Reads the current splash framebuffer as BGRA (used by --splash-test).
    bool Capture(std::vector<uint8_t>& bgra, int& width, int& height);
    void Close();
    bool Visible() const { return visible_.load(std::memory_order_acquire); }

private:
    bool CreateWindowAndRenderer();
    void ThreadMain();
    void PumpFrame();
    void DestroyResources();
    bool LoadFrame(int index);
    void Render(float displayProgress, float pulse, const std::string& status);
    float FadeAlpha(uint64_t now) const;

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* frame_ = nullptr;
    // Outgoing frame of the current crossfade.
    SDL_Texture* previousFrame_ = nullptr;
    int previousFrameWidth_ = 0;
    int previousFrameHeight_ = 0;
    uint64_t fadeStartMs_ = 0;
    uint64_t fadeDurationMs_ = 0;
    uint64_t lastFrameChangeMs_ = 0;
    bool fadeActive_ = false;

    std::unique_ptr<SplashFont> titleFont_;
    std::unique_ptr<SplashFont> subtitleFont_;
    std::unique_ptr<SplashFont> statusFont_;
    std::unique_ptr<SplashFont> percentFont_;

    std::vector<std::string> framePaths_;
    std::string title_;
    std::string subtitle_;

    int frameWidth_ = 0;
    int frameHeight_ = 0;
    int currentFrame_ = -1;
    int width_ = 980;
    int height_ = 640;

    // Shared with the UI thread in threaded mode.
    std::mutex mutex_;
    std::string status_ = "Loading...";
    float progress_ = 0.0f;
    float displayProgress_ = 0.0f;
    float stageStartProgress_ = 0.0f;
    float stageTarget_ = 0.0f;
    float stageDuration_ = 0.35f;
    float pulse_ = 0.0f;
    uint64_t stageStartMs_ = 0;
    bool stageActive_ = false;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> visible_{false};
    bool threaded_ = false;
    bool videoInitialized_ = false;
};

} // namespace ex::app
