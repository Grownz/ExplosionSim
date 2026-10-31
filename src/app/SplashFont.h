#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct SDL_Renderer;

namespace ex::app {

// Rasterizes TTF text for the SDL3 splash with the same font family as the
// ImGui panels. Glyph coverage is uploaded as a white alpha texture, so a
// cached string can be tinted per draw.
class SplashFont {
public:
    SplashFont();
    ~SplashFont();
    SplashFont(const SplashFont&) = delete;
    SplashFont& operator=(const SplashFont&) = delete;

    // Loads the font at the given pixel height. Returns false when the file
    // cannot be read or parsed; the splash then keeps its bitmap fallback.
    bool Load(const std::string& path, float pixelHeight);
    bool Ready() const;
    // Distance from the top of a line to its baseline plus the descender.
    float Height() const;
    // Pixel width of the string, rasterizing it on first use.
    float Measure(const std::string& text);

    // Draws with the top-left corner at (x, y).
    void Draw(SDL_Renderer* renderer, float x, float y, const std::string& text,
              uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ex::app
