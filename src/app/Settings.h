#pragma once

#include <string>

#include "gfx/Renderer.h"
#include "sim/Simulation.h"

namespace ex::app {

struct UpdateSettings {
    bool checkForUpdates = true;
    std::string lastSeenVersion;
    std::string skippedVersion;
    std::string proxy;
    long long lastCheckUnix = 0;
};

struct AppSettings {
    sim::SimulationSettings simulation{};
    gfx::RenderSettings render{};
    UpdateSettings update{};
    bool paused = false;
    bool showUi = true;
    bool showOverlays = true;
    int maxFps = 144;
    double observerDistanceM = 6.0;
};

void LoadSettings(AppSettings& settings, const std::wstring& path);
void SaveSettings(const AppSettings& settings, const std::wstring& path);
std::wstring DefaultSettingsPath();

} // namespace ex::app
