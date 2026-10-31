#include "app/SettingsPath.h"

#include <cstdlib>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ex::app {

std::filesystem::path UserSettingsPath(bool createDirectory) {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH * 2] = {};
    std::filesystem::path base;
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
                                           DWORD(std::size(buffer)));
    if (length > 0 && length < std::size(buffer)) {
        base = buffer;
    } else {
        length = GetEnvironmentVariableW(L"APPDATA", buffer,
                                         DWORD(std::size(buffer)));
        if (length > 0 && length < std::size(buffer)) {
            base = buffer;
        }
    }
    if (base.empty()) {
        return {};
    }
    const std::filesystem::path path =
        base / L"ExplosionSim" / L"settings.ini";
#else
    std::filesystem::path base;
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (home && *home) {
            base = std::filesystem::path(home) / ".config";
        }
    }
    if (base.empty()) {
        return {};
    }
    const std::filesystem::path path = base / "explosionsim" / "settings.ini";
#endif
    if (createDirectory) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
    }
    return path;
}

std::filesystem::path ExecutableDirectory() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH * 2] = {};
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(buffer).parent_path();
#else
    std::error_code error;
    const std::filesystem::path executable =
        std::filesystem::read_symlink("/proc/self/exe", error);
    if (error || executable.empty()) {
        return {};
    }
    return executable.parent_path();
#endif
}

} // namespace ex::app
