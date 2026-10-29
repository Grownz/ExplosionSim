#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ex::app {

// Borderless GDI splash shown while the Direct3D device, shaders, simulation
// and scene are prepared. It runs on its own thread so the bar keeps animating
// while the main thread compiles shaders or generates the town.
class Splash {
public:
    ~Splash();

    // framePaths are shown in order as the progress crosses each 5 % step.
    bool Show(const std::vector<std::wstring>& framePaths, const std::wstring& title,
              const std::wstring& subtitle);
    // expectedSeconds is the measured duration of the stage; the bar
    // interpolates from its current value to `progress` over that time.
    void SetProgress(float progress, const std::wstring& status,
                     float expectedSeconds = 0.35f);
    void Close();

private:
    void ThreadMain();
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void Paint(HDC target);
    void CreateFonts();
    void CreateOverlay();
    bool EnsureFrame(int index);
    void DestroyBackground();
    void ReleaseResources();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<HWND> window_{nullptr};

    std::mutex mutex_;
    float progress_ = 0.0f;
    float targetDuration_ = 0.35f;
    bool stageChanged_ = false;
    std::wstring status_ = L"Loading...";

    // Splash-thread state.
    std::vector<std::wstring> framePaths_;
    std::wstring title_;
    std::wstring subtitle_;
    std::vector<uint8_t> imagePixels_; // first frame, loaded on the main thread
    HBITMAP background_ = nullptr;
    HDC backgroundDc_ = nullptr;
    int imageWidth_ = 0;
    int imageHeight_ = 0;
    int frameCount_ = 0;
    int currentFrame_ = -1;
    HBITMAP overlay_ = nullptr;
    HDC overlayDc_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT subtitleFont_ = nullptr;
    HFONT statusFont_ = nullptr;
    HFONT percentFont_ = nullptr;
    int width_ = 980;
    int height_ = 640;
    int dpi_ = 96;
    float pulse_ = 0.0f;
    float displayProgress_ = 0.0f;
    float stageStart_ = 0.0f;
    float stageTarget_ = 0.0f;
    float stageDuration_ = 0.35f;
    std::chrono::steady_clock::time_point stageStartTime_{};
    bool stageActive_ = false;
    bool classRegistered_ = false;
};

} // namespace ex::app
