#include "gfx/ImageIO.h"

#include "core/Log.h"

#include <filesystem>

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#ifdef _WIN32
#include <windows.h>
#endif

namespace ex::gfx {

namespace {

std::string ToUtf8(const std::wstring& path) {
#ifdef _WIN32
    if (path.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(size_t(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, result.data(), size, nullptr,
                        nullptr);
    return result;
#else
    return std::filesystem::path(path).string();
#endif
}

} // namespace

bool SavePng(const std::string& path, const uint8_t* bgra, int width, int height) {
    if (!bgra || width <= 0 || height <= 0 || path.empty()) {
        return false;
    }
    const size_t pixels = size_t(width) * size_t(height);
    std::vector<uint8_t> rgba(pixels * 4);
    for (size_t i = 0; i < pixels; ++i) {
        rgba[i * 4 + 0] = bgra[i * 4 + 2];
        rgba[i * 4 + 1] = bgra[i * 4 + 1];
        rgba[i * 4 + 2] = bgra[i * 4 + 0];
        rgba[i * 4 + 3] = bgra[i * 4 + 3];
    }
    if (!stbi_write_png(path.c_str(), width, height, 4, rgba.data(),
                        width * 4)) {
        LogError("Could not write PNG '%s'.", path.c_str());
        return false;
    }
    LogInfo("Saved screenshot: %s (%dx%d)", path.c_str(), width, height);
    return true;
}

bool SavePng(const std::wstring& path, const uint8_t* bgra, int width, int height) {
    return SavePng(ToUtf8(path), bgra, width, height);
}

bool LoadImageBgra(const std::string& path, std::vector<uint8_t>& bgra, int& width,
                   int& height) {
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels || w <= 0 || h <= 0) {
        LogError("Could not load image '%s': %s", path.c_str(),
                 stbi_failure_reason() ? stbi_failure_reason() : "unknown error");
        if (pixels) {
            stbi_image_free(pixels);
        }
        return false;
    }
    const size_t count = size_t(w) * size_t(h);
    bgra.resize(count * 4);
    for (size_t i = 0; i < count; ++i) {
        bgra[i * 4 + 0] = pixels[i * 4 + 2];
        bgra[i * 4 + 1] = pixels[i * 4 + 1];
        bgra[i * 4 + 2] = pixels[i * 4 + 0];
        bgra[i * 4 + 3] = pixels[i * 4 + 3];
    }
    stbi_image_free(pixels);
    width = w;
    height = h;
    return true;
}

bool LoadImageBgra(const std::wstring& path, std::vector<uint8_t>& bgra, int& width,
                   int& height) {
    return LoadImageBgra(ToUtf8(path), bgra, width, height);
}

} // namespace ex::gfx
