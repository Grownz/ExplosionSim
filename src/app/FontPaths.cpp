#include "app/FontPaths.h"

#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ex::app {

namespace {

bool Exists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

std::string FirstExisting(const std::vector<std::string>& candidates) {
    for (const std::string& candidate : candidates) {
        if (Exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::string WindowsFontsDirectory() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetWindowsDirectoryW(buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        const int size =
            WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr,
                                nullptr);
        if (size > 0) {
            std::string result(size_t(size - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, buffer, -1, result.data(), size,
                                nullptr, nullptr);
            return result + "\\Fonts\\";
        }
    }
    return "C:\\Windows\\Fonts\\";
#else
    return {};
#endif
}

} // namespace

std::string UiFontPath() {
#ifdef _WIN32
    const std::string fonts = WindowsFontsDirectory();
    return FirstExisting({fonts + "segoeui.ttf", fonts + "arial.ttf",
                          fonts + "tahoma.ttf"});
#else
    return FirstExisting({
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    });
#endif
}

std::string UiFontSemiboldPath() {
#ifdef _WIN32
    const std::string fonts = WindowsFontsDirectory();
    const std::string semibold = FirstExisting(
        {fonts + "seguisb.ttf", fonts + "segoeuib.ttf", fonts + "arialbd.ttf"});
#else
    const std::string semibold = FirstExisting({
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/noto/NotoSans-Bold.ttf",
    });
#endif
    if (!semibold.empty()) {
        return semibold;
    }
    return UiFontPath();
}

} // namespace ex::app
