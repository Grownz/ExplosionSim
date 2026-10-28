#pragma once

#include "app/Settings.h"
#include "gfx/Camera.h"
#include "gfx/Renderer.h"
#include "sim/Simulation.h"

#include <string>

namespace ex::ui {

struct UpdateUiState {
    bool enabled = true;
    bool checking = false;
    bool available = false;
    bool downloading = false;
    bool downloaded = false;
    bool failed = false;
    bool upToDate = false;
    std::string version;
    std::string notes;
    std::string error;
    std::string assetName;
    float progress = 0.0f;
    unsigned long long downloadedBytes = 0;
    unsigned long long totalBytes = 0;
};

struct UiActions {
    bool restart = false;
    bool rearm = false;
    bool detonateNow = false;
    bool stepOnce = false;
    bool frameCamera = false;
    bool screenshot = false;
    bool quit = false;
    bool applySimulationSettings = false;
    bool checkForUpdates = false;
    bool downloadUpdate = false;
    bool cancelDownload = false;
    bool openDownloadFolder = false;
    bool installUpdate = false;
    bool dismissWhatsNew = false;
    bool skipUpdateVersion = false;
    bool showChangelog = false;
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
    UpdateUiState update;
    const std::string* changelogText = nullptr;
    const std::string* currentReleaseNotes = nullptr;
    bool showWhatsNew = false;
};

void ApplyTheme(float dpiScale);
void LoadFonts(float dpiScale);
UiActions Draw(UiContext& context);

} // namespace ex::ui
