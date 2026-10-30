#include "app/SplashSdl.h"

#include "core/Log.h"
#include "gfx/ImageIO.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ex::app {

namespace {

void DrawText(SDL_Renderer* renderer, float x, float y, float scale,
              const char* text) {
    if (!renderer || !text || !*text) {
        return;
    }
    const float s = std::max(1.0f, scale);
    SDL_SetRenderScale(renderer, s, s);
    SDL_RenderDebugText(renderer, x / s, y / s, text);
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
}

float TextWidth(const char* text, float scale) {
    return float(text ? std::strlen(text) : 0) * 8.0f * std::max(1.0f, scale);
}

} // namespace

SplashSdl::~SplashSdl() {
    Close();
}

bool SplashSdl::Show(const std::vector<std::string>& framePaths,
                     const std::string& title, const std::string& subtitle) {
    Close();
    framePaths_ = framePaths;
    title_ = title;
    subtitle_ = subtitle;

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        LogWarn("Splash: SDL video unavailable (%s); continuing without it.",
                SDL_GetError());
        return false;
    }
    videoInitialized_ = true;

    SDL_Rect bounds{0, 0, 1280, 800};
    const SDL_DisplayID display = SDL_GetPrimaryDisplay();
    if (display) {
        SDL_GetDisplayUsableBounds(display, &bounds);
    }
    width_ = std::max(320, std::min(980, int(float(bounds.w) * 0.92f)));
    height_ = std::max(240, std::min(640, int(float(bounds.h) * 0.92f)));

    window_ = SDL_CreateWindow(title.c_str(), width_, height_,
                               SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP);
    if (!window_) {
        LogWarn("Splash: window creation failed (%s).", SDL_GetError());
        return false;
    }
    SDL_SetWindowPosition(window_, bounds.x + (bounds.w - width_) / 2,
                          bounds.y + (bounds.h - height_) / 2);

    renderer_ = SDL_CreateRenderer(window_, SDL_SOFTWARE_RENDERER);
    if (!renderer_) {
        renderer_ = SDL_CreateRenderer(window_, nullptr);
    }
    if (!renderer_) {
        LogWarn("Splash: renderer creation failed (%s).", SDL_GetError());
        Close();
        return false;
    }
    SDL_SetRenderVSync(renderer_, 1);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    for (int i = 0; i < int(framePaths_.size()); ++i) {
        if (LoadFrame(i)) {
            break;
        }
    }

    progress_ = 0.02f;
    displayProgress_ = 0.0f;
    stageStartProgress_ = 0.0f;
    stageTarget_ = 0.02f;
    stageDuration_ = 0.25f;
    stageStartMs_ = SDL_GetTicks();
    stageActive_ = true;
    Render();
    return true;
}

void SplashSdl::SetProgress(float progress, const std::string& status,
                            float expectedSeconds) {
    if (!window_) {
        return;
    }
    progress_ = std::clamp(progress, 0.0f, 1.0f);
    if (!status.empty()) {
        status_ = status;
    }
    stageStartProgress_ = displayProgress_;
    stageTarget_ = std::max(progress_, displayProgress_);
    stageDuration_ = std::max(expectedSeconds, 0.01f);
    stageStartMs_ = SDL_GetTicks();
    stageActive_ = true;
}

void SplashSdl::Pump() {
    if (!window_) {
        return;
    }
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        // The splash is not user-closable; the application closes it.
    }

    const uint64_t now = SDL_GetTicks();
    if (stageActive_) {
        const float elapsed = float(now - stageStartMs_) / 1000.0f;
        const float t = std::clamp(elapsed / stageDuration_, 0.0f, 1.0f);
        displayProgress_ =
            stageStartProgress_ + (stageTarget_ - stageStartProgress_) * t;
        if (t >= 1.0f) {
            displayProgress_ = stageTarget_;
            stageActive_ = false;
        }
        if (elapsed > stageDuration_ + 0.25f) {
            pulse_ = std::fmod(pulse_ + 0.018f, 1.0f);
        } else {
            pulse_ = 0.0f;
        }
    }

    if (!framePaths_.empty()) {
        const int frame = std::clamp(
            int(displayProgress_ * float(framePaths_.size())), 0,
            int(framePaths_.size()) - 1);
        if (frame != currentFrame_) {
            LoadFrame(frame);
        }
    }
    Render();
}

bool SplashSdl::LoadFrame(int index) {
    if (index < 0 || index >= int(framePaths_.size()) || !renderer_) {
        return false;
    }
    if (index == currentFrame_ && frame_) {
        return true;
    }
    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    if (!gfx::LoadImageBgra(framePaths_[index], bgra, width, height) ||
        width <= 0 || height <= 0) {
        return false;
    }
    if (frame_) {
        SDL_DestroyTexture(frame_);
        frame_ = nullptr;
    }
    frame_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_BGRA32,
                               SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!frame_) {
        return false;
    }
    SDL_UpdateTexture(frame_, nullptr, bgra.data(), width * 4);
    frameWidth_ = width;
    frameHeight_ = height;
    currentFrame_ = index;
    return true;
}

void SplashSdl::Render() {
    if (!renderer_) {
        return;
    }
    int width = width_;
    int height = height_;
    SDL_GetWindowSize(window_, &width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }

    SDL_SetRenderDrawColor(renderer_, 18, 20, 24, 255);
    SDL_RenderClear(renderer_);

    if (frame_ && frameWidth_ > 0 && frameHeight_ > 0) {
        const float cover = std::max(float(width) / float(frameWidth_),
                                     float(height) / float(frameHeight_));
        SDL_FRect source{
            (float(frameWidth_) - float(width) / cover) * 0.5f,
            (float(frameHeight_) - float(height) / cover) * 0.5f,
            float(width) / cover,
            float(height) / cover};
        SDL_RenderTexture(renderer_, frame_, &source, nullptr);
    }

    // Bottom-up dark gradient, matching the Windows overlay.
    constexpr int kStrips = 32;
    const int gradientTop = int(float(height) * 0.40f);
    for (int i = 0; i < kStrips; ++i) {
        const float t = float(i + 1) / float(kStrips);
        const int y0 = gradientTop + (height - gradientTop) * i / kStrips;
        const int y1 = gradientTop + (height - gradientTop) * (i + 1) / kStrips;
        const int alpha = int(225.0f * t * t);
        SDL_SetRenderDrawColor(renderer_, 8, 9, 11, uint8_t(alpha));
        SDL_FRect rect{0.0f, float(y0), float(width), float(y1 - y0 + 1)};
        SDL_RenderFillRect(renderer_, &rect);
    }

    const float scale = float(height) / 640.0f;
    const int margin = int(46.0f * scale);
    const int barHeight = std::max(3, int(9.0f * scale));
    const int barY = height - int(86.0f * scale);
    const int barWidth = std::max(1, width - margin * 2);
    const int statusY = height - int(126.0f * scale);

    SDL_SetRenderDrawColor(renderer_, 246, 243, 238, 255);
    DrawText(renderer_, float(margin), float(height - int(244.0f * scale)),
             std::max(1.0f, 3.2f * scale), title_.c_str());
    SDL_SetRenderDrawColor(renderer_, 214, 208, 199, 255);
    DrawText(renderer_, float(margin), float(height - int(184.0f * scale)),
             std::max(1.0f, 2.0f * scale), subtitle_.c_str());

    SDL_SetRenderDrawColor(renderer_, 224, 219, 211, 255);
    DrawText(renderer_, float(margin), float(statusY),
             std::max(1.0f, 1.6f * scale), status_.c_str());

    char percent[16] = {};
    std::snprintf(percent, sizeof(percent), "%d%%",
                  int(std::lround(displayProgress_ * 100.0f)));
    const float percentScale = std::max(1.0f, 1.6f * scale);
    SDL_SetRenderDrawColor(renderer_, 240, 236, 230, 255);
    DrawText(renderer_,
             float(width - margin) - TextWidth(percent, percentScale),
             float(statusY), percentScale, percent);

    SDL_SetRenderDrawColor(renderer_, 40, 38, 35, 255);
    SDL_FRect track{float(margin), float(barY), float(barWidth),
                    float(barHeight)};
    SDL_RenderFillRect(renderer_, &track);
    SDL_SetRenderDrawColor(renderer_, 78, 72, 64, 255);
    SDL_FRect border{float(margin), float(barY), float(barWidth), 1.0f};
    SDL_RenderFillRect(renderer_, &border);

    const int fillWidth = int(float(barWidth) * displayProgress_);
    if (fillWidth > 1) {
        SDL_SetRenderDrawColor(renderer_, 228, 116, 44, 255);
        SDL_FRect fill{float(margin), float(barY), float(fillWidth),
                       float(barHeight)};
        SDL_RenderFillRect(renderer_, &fill);

        if (pulse_ > 0.0f && fillWidth > barHeight * 3) {
            const int highlightWidth =
                std::min(fillWidth, std::max(1, int(74.0f * scale)));
            const int travel = std::max(1, fillWidth - highlightWidth);
            const int highlightX = margin + int(float(travel) * pulse_);
            SDL_SetRenderDrawColor(renderer_, 247, 163, 88, 255);
            SDL_FRect highlight{float(highlightX), float(barY),
                                float(highlightWidth), float(barHeight)};
            SDL_RenderFillRect(renderer_, &highlight);
        }
    }

    SDL_RenderPresent(renderer_);
}

bool SplashSdl::Capture(std::vector<uint8_t>& bgra, int& width, int& height) {
    if (!renderer_ || !window_) {
        return false;
    }
    SDL_Surface* surface = SDL_RenderReadPixels(renderer_, nullptr);
    if (!surface) {
        return false;
    }
    SDL_Surface* converted =
        SDL_ConvertSurface(surface, SDL_PIXELFORMAT_BGRA32);
    SDL_DestroySurface(surface);
    if (!converted) {
        return false;
    }
    width = converted->w;
    height = converted->h;
    if (width <= 0 || height <= 0) {
        SDL_DestroySurface(converted);
        return false;
    }
    bgra.resize(size_t(width) * size_t(height) * 4);
    for (int y = 0; y < height; ++y) {
        std::memcpy(bgra.data() + size_t(y) * size_t(width) * 4,
                    static_cast<const uint8_t*>(converted->pixels) +
                        size_t(y) * size_t(converted->pitch),
                    size_t(width) * 4);
    }
    SDL_DestroySurface(converted);
    return true;
}

void SplashSdl::Close() {    if (frame_) {
        SDL_DestroyTexture(frame_);
        frame_ = nullptr;
    }
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    framePaths_.clear();
    currentFrame_ = -1;
    frameWidth_ = 0;
    frameHeight_ = 0;
    if (videoInitialized_) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        videoInitialized_ = false;
    }
}

} // namespace ex::app
