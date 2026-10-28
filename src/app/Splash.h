#pragma once

#include <windows.h>

#include <atomic>
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

    bool Show(const std::wstring& imagePath, const std::wstring& title,
              const std::wstring& subtitle);
    void SetProgress(float progress, const std::wstring& status);
    void Close();

private:
    void ThreadMain();
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void Paint(HDC target);
    void CreateFonts();
    void CreateOverlay();
    void ReleaseResources();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<HWND> window_{nullptr};

    std::mutex mutex_;
    float progress_ = 0.0f;
    std::wstring status_ = L"Loading...";

    // Splash-thread state.
    std::wstring imagePath_;
    std::wstring title_;
    std::wstring subtitle_;
    std::vector<uint8_t> imagePixels_;
    HBITMAP background_ = nullptr;
    HDC backgroundDc_ = nullptr;
    int imageWidth_ = 0;
    int imageHeight_ = 0;
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
    float creepCeiling_ = 0.0f;
    float lastProgress_ = -1.0f;
    int stillTicks_ = 0;
    bool classRegistered_ = false;
};

} // namespace ex::app
