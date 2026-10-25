#pragma once

#include "app/Settings.h"
#include "gfx/Camera.h"
#include "gfx/Renderer.h"
#include "sim/Simulation.h"

namespace ex::ui {

struct UiActions {
    bool restart = false;
    bool rearm = false;
    bool detonateNow = false;
    bool stepOnce = false;
    bool frameCamera = false;
    bool screenshot = false;
    bool quit = false;
    bool applySimulationSettings = false;
};

struct UiContext {
    app::AppSettings& settings;
    sim::Simulation& simulation;
    gfx::Camera& camera;
    gfx::Renderer& renderer;
    float fps = 0.0f;
    float frameMs = 0.0f;
    int workerThreads = 0;
    const char* adapterName = "";
};

void ApplyTheme(float dpiScale);
void LoadFonts(float dpiScale);
UiActions Draw(UiContext& context);

} // namespace ex::ui
