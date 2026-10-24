#pragma once

#include <string>

#include "gfx/Renderer.h"
#include "sim/Simulation.h"

namespace ex::app {

struct AppSettings {
    sim::SimulationSettings simulation{};
    gfx::RenderSettings render{};
    bool paused = false;
    bool showUi = true;
    double observerDistanceM = 6.0;
};

void LoadSettings(AppSettings& settings, const std::wstring& path);
void SaveSettings(const AppSettings& settings, const std::wstring& path);
std::wstring DefaultSettingsPath();

} // namespace ex::app
