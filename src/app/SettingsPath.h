#pragma once

#include <filesystem>

namespace ex::app {

// Per-user settings location. Windows uses %LOCALAPPDATA%\ExplosionSim,
// Linux follows the XDG base directory spec ($XDG_CONFIG_HOME or
// ~/.config/explosionsim). Returns an empty path when no home directory is
// available.
std::filesystem::path UserSettingsPath(bool createDirectory = true);

} // namespace ex::app
