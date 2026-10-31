#include "app/SplashFont.h"

#include "core/Log.h"

#include <SDL3/SDL.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

namespace ex::app {

namespace {

// Decodes one UTF-8 code point and advances the cursor.
uint32_t NextCodepoint(const std::string& text, size_t& index) {
    if (index >= text.size()) {
        return 0;
    }
    const unsigned char first = static_cast<unsigned char>(text[index]);
    if (first < 0x80) {
        ++index;
        return first;
    }
    int length = 1;
    uint32_t value = 0;
    if ((first & 0xE0) == 0xC0) {
        length = 2;
        value = first & 0x1Fu;
    } else if ((first & 0xF0) == 0xE0) {
        length = 3;
        value = first & 0x0Fu;
    } else if ((first & 0xF8) == 0xF0) {
        length = 4;
        value = first & 0x07u;
    } else {
        ++index;
        return 0xFFFD;
    }
    if (index + size_t(length) > text.size()) {
        index = text.size();
        return 0xFFFD;
    }
    for (int i = 1; i < length; ++i) {
        const unsigned char next =
            static_cast<unsigned char>(text[index + size_t(i)]);
        if ((next & 0xC0) != 0x80) {
            index += size_t(i);
            return 0xFFFD;
        }
        value = (value << 6) | (next & 0x3Fu);
    }
    index += size_t(length);
    return value;
}

} // namespace

struct SplashFont::Impl {
    struct Entry {
        SDL_Texture* texture = nullptr;
        float width = 0.0f;
        float height = 0.0f;
    };

    struct Glyph {
        uint32_t codepoint = 0;
        float x = 0.0f;
    };

    struct Layout {
        std::vector<Glyph> glyphs;
        int width = 1;
        int height = 1;
    };

    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    float scale = 0.0f;
    float ascent = 0.0f;
    float descent = 0.0f;
    float height = 0.0f;
    float pixelHeight = 0.0f;
    bool ready = false;
    std::map<std::string, Entry> cache;

    ~Impl() {
        for (auto& [text, entry] : cache) {
            (void)text;
            if (entry.texture) {
                SDL_DestroyTexture(entry.texture);
            }
        }
    }

    Layout BuildLayout(const std::string& text) const {
        Layout layout;
        float penX = 0.0f;
        uint32_t previous = 0;
        size_t index = 0;
        while (index < text.size()) {
            const uint32_t codepoint = NextCodepoint(text, index);
            if (codepoint == 0) {
                continue;
            }
            if (previous != 0) {
                penX += float(stbtt_GetCodepointKernAdvance(
                            &info, int(previous), int(codepoint))) *
                        scale;
            }
            int advance = 0;
            int leftSideBearing = 0;
            stbtt_GetCodepointHMetrics(&info, int(codepoint), &advance,
                                       &leftSideBearing);
            layout.glyphs.push_back({codepoint, penX});
            penX += float(advance) * scale;
            previous = codepoint;
        }
        layout.width = std::max(1, int(std::ceil(penX)));
        layout.height = std::max(1, int(std::ceil(height)));
        return layout;
    }

    bool BuildTexture(SDL_Renderer* renderer, const std::string& text,
                      Entry& entry) {
        if (!renderer) {
            return false;
        }
        const Layout layout = BuildLayout(text);
        std::vector<uint8_t> coverage(
            size_t(layout.width) * size_t(layout.height), 0);
        for (const Glyph& glyph : layout.glyphs) {
            int glyphWidth = 0;
            int glyphHeight = 0;
            int xOffset = 0;
            int yOffset = 0;
            unsigned char* bitmap = stbtt_GetCodepointBitmap(
                &info, scale, scale, int(glyph.codepoint), &glyphWidth,
                &glyphHeight, &xOffset, &yOffset);
            if (!bitmap) {
                continue;
            }
            const int destX = int(std::lround(glyph.x)) + xOffset;
            const int destY = int(std::lround(ascent)) + yOffset;
            for (int y = 0; y < glyphHeight; ++y) {
                const int targetY = destY + y;
                if (targetY < 0 || targetY >= layout.height) {
                    continue;
                }
                for (int x = 0; x < glyphWidth; ++x) {
                    const int targetX = destX + x;
                    if (targetX < 0 || targetX >= layout.width) {
                        continue;
                    }
                    const uint8_t value =
                        bitmap[size_t(y) * size_t(glyphWidth) + size_t(x)];
                    uint8_t& target =
                        coverage[size_t(targetY) * size_t(layout.width) +
                                 size_t(targetX)];
                    target = std::max(target, value);
                }
            }
            stbtt_FreeBitmap(bitmap, nullptr);
        }

        std::vector<uint32_t> pixels(
            size_t(layout.width) * size_t(layout.height));
        for (size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = (uint32_t(coverage[i]) << 24) | 0x00FFFFFFu;
        }

        entry.texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
            layout.width, layout.height);
        if (!entry.texture) {
            LogWarn("Splash: text texture creation failed (%s).",
                    SDL_GetError());
            return false;
        }
        SDL_SetTextureBlendMode(entry.texture, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(entry.texture, nullptr, pixels.data(),
                          layout.width * 4);
        entry.width = float(layout.width);
        entry.height = float(layout.height);
        return true;
    }

    Entry& GetEntry(const std::string& text) {
        auto cached = cache.find(text);
        if (cached != cache.end()) {
            return cached->second;
        }
        if (cache.size() >= 64) {
            // The splash only ever shows a handful of distinct strings; a
            // full reset keeps the cache bounded without an LRU.
            for (auto& [key, entry] : cache) {
                (void)key;
                if (entry.texture) {
                    SDL_DestroyTexture(entry.texture);
                }
            }
            cache.clear();
        }
        Entry entry;
        const Layout layout = BuildLayout(text);
        entry.width = float(layout.width);
        entry.height = float(layout.height);
        return cache.emplace(text, entry).first->second;
    }
};

SplashFont::SplashFont() : impl_(std::make_unique<Impl>()) {}

SplashFont::~SplashFont() = default;

bool SplashFont::Load(const std::string& path, float pixelHeight) {
    if (!impl_) {
        return false;
    }
    impl_->ready = false;
    impl_->data.clear();
    impl_->cache.clear();
    if (path.empty() || pixelHeight <= 0.0f) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        LogWarn("Splash: font '%s' is unavailable.", path.c_str());
        return false;
    }
    impl_->data.assign(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
    if (impl_->data.empty()) {
        return false;
    }
    const int offset = stbtt_GetFontOffsetForIndex(impl_->data.data(), 0);
    if (offset < 0 ||
        !stbtt_InitFont(&impl_->info, impl_->data.data(), offset)) {
        LogWarn("Splash: font '%s' could not be parsed.", path.c_str());
        impl_->data.clear();
        return false;
    }
    impl_->pixelHeight = pixelHeight;
    impl_->scale = stbtt_ScaleForPixelHeight(&impl_->info, pixelHeight);
    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
    stbtt_GetFontVMetrics(&impl_->info, &ascent, &descent, &lineGap);
    impl_->ascent = float(ascent) * impl_->scale;
    impl_->descent = float(descent) * impl_->scale;
    impl_->height = impl_->ascent - impl_->descent;
    impl_->ready = true;
    return true;
}

bool SplashFont::Ready() const {
    return impl_ && impl_->ready;
}

float SplashFont::Height() const {
    return impl_ && impl_->ready ? impl_->height : 0.0f;
}

float SplashFont::Measure(const std::string& text) {
    if (!impl_ || !impl_->ready || text.empty()) {
        return 0.0f;
    }
    return impl_->GetEntry(text).width;
}

void SplashFont::Draw(SDL_Renderer* renderer, float x, float y,
                      const std::string& text, uint8_t r, uint8_t g, uint8_t b,
                      uint8_t a) {
    if (!renderer || !impl_ || !impl_->ready || text.empty()) {
        return;
    }
    Impl::Entry& entry = impl_->GetEntry(text);
    if (!entry.texture && !impl_->BuildTexture(renderer, text, entry)) {
        return;
    }
    SDL_SetTextureColorMod(entry.texture, r, g, b);
    SDL_SetTextureAlphaMod(entry.texture, a);
    SDL_FRect destination{x, y, entry.width, entry.height};
    SDL_RenderTexture(renderer, entry.texture, nullptr, &destination);
}

} // namespace ex::app
