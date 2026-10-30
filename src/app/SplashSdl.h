#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace ex::app {

// SDL3 splash used by the native Linux shell. It keeps the Windows layout
// (980x640, one still per 5 % step, progress bar) but renders through SDL's 2D
// renderer and the portable stb image loader, so it needs no GDI, WIC or a
// desktop toolkit. The shell pumps it cooperatively between initialisation
// stages; no extra thread is required.
class SplashSdl {
public:
    ~SplashSdl();

    // framePaths are shown in order as the progress crosses each 5 % step.
    bool Show(const std::vector<std::string>& framePaths,
              const std::string& title, const std::string& subtitle);
    void SetProgress(float progress, const std::string& status,
                     float expectedSeconds = 0.35f);
    // Processes events, advances the animated progress and redraws.
    void Pump();
    // Reads the current splash framebuffer as BGRA (used by --splash-test).
    bool Capture(std::vector<uint8_t>& bgra, int& width, int& height);
    void Close();
    bool Visible() const { return window_ != nullptr; }

private:
    bool LoadFrame(int index);
    void Render();

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* frame_ = nullptr;
    std::vector<std::string> framePaths_;
    std::string title_;
    std::string subtitle_;
    std::string status_ = "Loading...";

    int frameWidth_ = 0;
    int frameHeight_ = 0;
    int currentFrame_ = -1;
    int width_ = 980;
    int height_ = 640;
    float progress_ = 0.0f;
    float displayProgress_ = 0.0f;
    float stageStartProgress_ = 0.0f;
    float stageTarget_ = 0.0f;
    float stageDuration_ = 0.35f;
    uint64_t stageStartMs_ = 0;
    bool stageActive_ = false;
    float pulse_ = 0.0f;
    bool videoInitialized_ = false;
};

} // namespace ex::app
