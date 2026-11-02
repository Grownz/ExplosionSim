#include "app/LinuxUi.h"

#include "app/FontPaths.h"
#include "core/Log.h"
#include "scene/SceneTypes.h"
#include "sim/Simulation.h"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace ex::app {

namespace {

// The panel mirrors the Windows application: the same sections, labels, ranges
// and defaults, so a setting is found in the same place on both platforms.
const ImVec4 kAccent(1.00f, 0.52f, 0.16f, 1.00f);
const ImVec4 kAccentDim(0.80f, 0.36f, 0.10f, 1.00f);
const ImVec4 kWarning(1.00f, 0.78f, 0.35f, 1.00f);
const ImVec4 kMuted(0.62f, 0.65f, 0.70f, 1.00f);

// Style before the display scale is applied; ApplyScale() starts from it.
ImGuiStyle g_baseStyle;

void SectionHeader(const char* label) {
    ImGui::Spacing();
    ImGui::SeparatorText(label);
    ImGui::Spacing();
}

void Readout(const char* label, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    ImGui::TextUnformatted(label);
    const float textWidth = ImGui::CalcTextSize(buffer).x;
    const float rightEdge =
        ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(
        std::max(ImGui::GetCursorPosX() + 8.0f, rightEdge - textWidth));
    ImGui::TextUnformatted(buffer);
}

void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void ApplyOnRelease(LinuxUiState& state) {
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        state.applySimulation = true;
    }
}

// Slider on a double that applies the simulation settings when it is released.
bool DoubleSlider(const char* label, double* value, float minValue,
                  float maxValue, const char* format, ImGuiSliderFlags flags,
                  LinuxUiState& state) {
    float temporary = float(*value);
    const bool changed =
        ImGui::SliderFloat(label, &temporary, minValue, maxValue, format, flags);
    if (changed) {
        *value = double(temporary);
    }
    ApplyOnRelease(state);
    return changed;
}

void DrawDetonation(LinuxUiState& state, const ex::sim::Simulation& simulation) {
    SectionHeader("DETONATION");
    ex::sim::SimulationSettings& sim = state.sim;

    double massKg = sim.chargeMassKg;
    const double minMass = 0.01;
    const double maxMass = 1.0e8; // 100 000 t
    const char* massFormat = massKg < 100.0 ? "%.3f kg TNT" : "%.0f kg TNT";
    if (ImGui::SliderScalar("Charge", ImGuiDataType_Double, &massKg, &minMass,
                            &maxMass, massFormat,
                            ImGuiSliderFlags_Logarithmic)) {
        sim.chargeMassKg = std::clamp(massKg, 0.01, 1.0e8);
    }
    ApplyOnRelease(state);

    const char* presetLabels[] = {"20 g", "200 g", "1 kg", "50 kg", "1 t"};
    const double presetValues[] = {0.02, 0.2, 1.0, 50.0, 1000.0};
    const float presetWidth = ImGui::GetContentRegionAvail().x / 5.0f - 2.0f;
    for (int i = 0; i < 5; ++i) {
        if (i > 0) {
            ImGui::SameLine();
        }
        if (ImGui::Button(presetLabels[i], ImVec2(presetWidth, 0))) {
            sim.chargeMassKg = presetValues[i];
            state.applySimulation = true;
            state.restartRequested = true;
        }
    }
    const char* largeLabels[] = {"10 t", "100 t", "1000 t", "10000 t"};
    const double largeValues[] = {1.0e4, 1.0e5, 1.0e6, 1.0e7};
    const float largeWidth = ImGui::GetContentRegionAvail().x / 4.0f - 2.0f;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) {
            ImGui::SameLine();
        }
        if (ImGui::Button(largeLabels[i], ImVec2(largeWidth, 0))) {
            sim.chargeMassKg = largeValues[i];
            state.applySimulation = true;
            state.restartRequested = true;
        }
    }

    DoubleSlider("Height of burst", &sim.heightOfBurstM, 0.01f, 5000.0f,
                 "%.2f m", ImGuiSliderFlags_Logarithmic, state);

    float fuse = float(sim.fuseSeconds);
    if (ImGui::SliderFloat("Fuse", &fuse, 0.0f, 10.0f, "%.1f s")) {
        sim.fuseSeconds = double(fuse);
    }
    ApplyOnRelease(state);

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
    const bool rendering = !simulation.IsArmed();
    const char* mainLabel =
        rendering ? "REARM##main_detonate" : "DETONATE##main_detonate";
    if (ImGui::Button(mainLabel, ImVec2(-1.0f, ImGui::GetFontSize() * 2.2f))) {
        if (rendering) {
            state.rearmRequested = true;
        } else {
            state.detonateRequested = true;
        }
    }
    ImGui::PopStyleColor(3);

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
    const float actionHeight = ImGui::GetFontSize() * 2.2f;
    const float halfWidth = (ImGui::GetContentRegionAvail().x - 8.0f) * 0.5f;
    if (ImGui::Button(state.paused ? "RESUME##main_pause" : "PAUSE##main_pause",
                      ImVec2(halfWidth, actionHeight))) {
        state.paused = !state.paused;
    }
    ImGui::SameLine();
    if (ImGui::Button("FRAME##main_frame", ImVec2(halfWidth, actionHeight))) {
        state.reframeRequested = true;
    }
    ImGui::PopStyleColor(3);

    if (rendering) {
        Hint("Stops the current solver and resets the scene. The next click "
             "starts the new explosion.");
    } else {
        Hint("Equivalent TNT energy: the charge mass is converted with "
             "4.184 MJ/kg.");
    }
}

void DrawAtmosphere(LinuxUiState& state) {
    SectionHeader("ATMOSPHERE");
    ex::sim::SimulationSettings& sim = state.sim;

    float temperatureC = float(sim.ambientTemperatureK - 273.15);
    if (ImGui::SliderFloat("Temperature", &temperatureC, -20.0f, 45.0f,
                           "%.1f °C")) {
        sim.ambientTemperatureK = double(temperatureC) + 273.15;
    }
    ApplyOnRelease(state);

    float pressureHpa = float(sim.ambientPressurePa / 100.0);
    if (ImGui::SliderFloat("Pressure", &pressureHpa, 850.0f, 1050.0f,
                           "%.0f hPa")) {
        sim.ambientPressurePa = double(pressureHpa) * 100.0;
    }
    ApplyOnRelease(state);

    DoubleSlider("Wind speed", &sim.windSpeedMs, 0.0f, 30.0f, "%.1f m/s",
                 ImGuiSliderFlags_None, state);
    DoubleSlider("Wind direction", &sim.windDirectionDeg, 0.0f, 360.0f,
                 "%.0f°", ImGuiSliderFlags_None, state);

    float humidity = float(sim.relativeHumidity * 100.0);
    if (ImGui::SliderFloat("Humidity", &humidity, 0.0f, 100.0f, "%.0f %%")) {
        sim.relativeHumidity = double(humidity) / 100.0;
    }
    ApplyOnRelease(state);

    // Derived quantities show the physical effect of the atmosphere.
    ex::sim::Environment environment{};
    environment.pressurePa = sim.ambientPressurePa;
    environment.temperatureK = sim.ambientTemperatureK;
    environment.relativeHumidity = sim.relativeHumidity;
    Readout("Air density", "%.4f kg/m³", environment.AirDensity());
    Readout("Speed of sound", "%.1f m/s", environment.SoundSpeed());
    Readout("Vapour pressure", "%.0f Pa", environment.VaporPressurePa());
    Readout("Virtual temperature", "%.1f K", environment.VirtualTemperatureK());

    Hint("Density and sound speed drive the blast integration (Sachs scaling). "
         "Humidity lowers the density and raises the sound speed; wind "
         "advects the rising cloud and particles.");
}

void DrawPlayback(LinuxUiState& state, const ex::sim::Simulation& simulation) {
    SectionHeader("PLAYBACK");
    ex::sim::SimulationSettings& sim = state.sim;

    ImGui::Checkbox("Pause##playback", &state.paused);
    ImGui::SameLine();
    if (ImGui::Button("Step")) {
        state.stepRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Rearm")) {
        state.rearmRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart")) {
        state.restartRequested = true;
    }

    float timeScale = float(sim.timeScale);
    if (ImGui::SliderFloat("Time scale", &timeScale, 0.005f, 2.0f, "×%.3f",
                           ImGuiSliderFlags_Logarithmic)) {
        sim.timeScale = double(timeScale);
        state.applySimulation = true;
    }
    if (ImGui::Button("Set real time (1x)", ImVec2(-1.0f, 0.0f))) {
        sim.timeScale = 1.0;
        state.applySimulation = true;
    }
    Hint("Playback speed: 1.0 plays one simulated second per real second. The "
         "high-fidelity solver follows this scale directly.");

    if (ImGui::Checkbox("Loop simulation", &sim.loop)) {
        state.applySimulation = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Frame camera")) {
        state.reframeRequested = true;
    }

    if (simulation.IsArmed()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
        if (simulation.IsFuseHeld()) {
            Readout("State", "Ready to detonate");
        } else {
            Readout("Fuse", "%.2f s", simulation.FuseRemaining());
        }
        ImGui::PopStyleColor();
        if (ImGui::Button("Detonate now", ImVec2(-1.0f, 0.0f))) {
            state.detonateRequested = true;
        }
    } else {
        Readout("Simulation time", "%.4f s", simulation.Time());
    }
    Readout("Real time", "%.1f s", simulation.RealTime());
}

void DrawEnvironment(LinuxUiState& state,
                     const ex::sim::Simulation& simulation) {
    SectionHeader("ENVIRONMENT");
    ex::scene::SceneSettings& scene = state.sim.scene;

    if (ImGui::Checkbox("Procedural scene", &scene.enabled)) {
        state.applySimulation = true;
    }

    int seed = int(scene.seed);
    if (ImGui::InputInt("Scene seed", &seed)) {
        scene.seed = uint32_t(std::clamp(seed, 0, 1000000));
    }
    ApplyOnRelease(state);

    float density = scene.density;
    if (ImGui::SliderFloat("Scene density", &density, 0.0f, 2.0f, "%.2f")) {
        scene.density = density;
    }
    ApplyOnRelease(state);

    if (ImGui::Checkbox("Buildings", &scene.buildings)) {
        state.applySimulation = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Vehicles", &scene.vehicles)) {
        state.applySimulation = true;
    }
    if (ImGui::Checkbox("Vegetation", &scene.vegetation)) {
        state.applySimulation = true;
    }

    const char* qualityLabels[] = {"Low", "Medium", "High"};
    int quality = std::clamp(scene.quality, 0, 2);
    if (ImGui::Combo("Scene quality", &quality, qualityLabels, 3)) {
        scene.quality = quality;
        state.applySimulation = true;
    }

    if (ImGui::Checkbox("Voronoi chunks", &scene.voronoiChunks)) {
        state.applySimulation = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Convex Voronoi cells clipped to the building box "
                          "(default). Disable to use the cheaper jittered grid "
                          "instead.");
    }

    ImGui::BeginDisabled(!scene.voronoiChunks);
    float chunkScale = scene.chunkScale;
    if (ImGui::SliderFloat("Chunk size", &chunkScale, 0.25f, 2.0f, "%.2fx",
                           ImGuiSliderFlags_Logarithmic)) {
        scene.chunkScale = chunkScale;
    }
    // Rebuild once when the drag ends instead of on every intermediate frame.
    ApplyOnRelease(state);
    ImGui::EndDisabled();
    Hint("Average Voronoi chunk size: 0.25x = four times smaller, 2x = twice "
         "as large. Very small chunks are limited by the per-building cell "
         "cap.");
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("Global scene budget: %d chunks.",
                       ex::scene::kSceneChunkBudget);
    ImGui::PopStyleColor();

    if (ImGui::Button("Regenerate scene")) {
        scene.seed += 1;
        state.applySimulation = true;
    }

    Readout("Objects", "%d", int(simulation.Scene().Objects().size()));
    Readout("Chunks", "%d", simulation.Scene().Stats().chunks);
    Readout("Scene radius", "%.0f m", simulation.Scene().Extent());
    Hint("Buildings, vehicles and trees are generated deterministically "
         "around ground zero. The scene reacts to the analytic blast field.");
}

void DrawPhysics(LinuxUiState& state, const ex::sim::Simulation& simulation) {
    SectionHeader("SIMULATION MODEL");
    ex::sim::SimulationSettings& sim = state.sim;

    int modelIndex =
        sim.mode == ex::sim::SimulationMode::ReducedOrder
            ? 0
            : (sim.solverHemisphere ? 2 : 1);
    const char* modeLabels[] = {"Reduced-order (analytic)",
                                "High-fidelity (cube)",
                                "High-fidelity (hemisphere)"};
    if (ImGui::Combo("Model", &modelIndex, modeLabels, 3) &&
        (modelIndex == 0 || state.solverAvailable)) {
        sim.mode = modelIndex == 0 ? ex::sim::SimulationMode::ReducedOrder
                                   : ex::sim::SimulationMode::HighFidelity;
        sim.solverHemisphere = modelIndex == 2;
        state.applySimulation = true;
    }
    if (!state.solverAvailable) {
        Hint("The high-fidelity solver is not available on this device.");
    }

    // The fire model is chosen before the detonation: it changes how the scene
    // ignites, so it can only be edited while the simulation is armed.
    ImGui::BeginDisabled(!simulation.IsArmed());
    if (ImGui::Checkbox("Fire propagation", &sim.firePropagation)) {
        state.applySimulation = true;
    }
    ImGui::EndDisabled();
    if (sim.firePropagation && !simulation.IsArmed()) {
        Readout("Burning objects", "%d", simulation.Scene().BurningCount());
        Readout("Burned out", "%d", simulation.Scene().BurnedOutCount());
    }
    Hint(simulation.IsArmed()
             ? "Fuel, burn-out and radiative ignition of neighbouring objects. "
               "The fire advances in simulated time: use time scale 1x to "
               "watch it spread."
             : "Rearm to change the fire model.");

    if (sim.mode == ex::sim::SimulationMode::HighFidelity) {
        if (ImGui::Checkbox("Flow around objects", &sim.solverFlowAround)) {
            state.applySimulation = true;
        }
        if (ImGui::Checkbox("Debris drag", &sim.solverBodyDrag)) {
            state.applySimulation = true;
        }
        const int solverResolutions[] = {64, 96, 128, 192, 256, 512};
        const char* solverResolutionLabels[] = {"64³",  "96³",  "128³",
                                                "192³", "256³", "512³"};
        int current = 2;
        for (int i = 0; i < 6; ++i) {
            if (solverResolutions[i] == sim.solverResolution) {
                current = i;
            }
        }
        if (ImGui::Combo("Solver grid", &current, solverResolutionLabels, 6)) {
            sim.solverResolution = solverResolutions[current];
            state.applySimulation = true;
        }

        ImGui::SliderInt("Max steps/frame", &sim.solverStepsPerFrame, 8, 256);
        ApplyOnRelease(state);
        Hint("Caps the high-fidelity solver work per frame. Lower values keep "
             "the frame rate up but play the explosion slower; the playback "
             "time scale is the target.");

        ImGui::SliderInt("Solver time/frame", &sim.solverFrameBudgetMs, 0, 500,
                         "%d ms");
        ApplyOnRelease(state);
        Hint("Wall-clock limit for the solver steps of one frame (0 = off). "
             "Keeps the first frames after the detonation short while the GPU "
             "clocks up; when it is reached the explosion plays slower.");

        if (ImGui::Checkbox("Solver auto-scale", &sim.solverAutoScale)) {
            state.applySimulation = true;
        }
        if (sim.solverHemisphere) {
            Hint("Hemisphere mode masks the Cartesian solver volume to a "
                 "semi-ellipsoidal dome. The 3D texture stays cubic, so memory "
                 "use is unchanged; the dome uses the radial sponge and "
                 "auto-scale.");
        }
        if (sim.solverResolution >= 256) {
            Hint("Solver grids of 256³ and 512³ need several GB of VRAM and "
                 "are not real-time; lower the time scale or grid.");
        }

        const ex::gfx::SolverStats& stats = state.solver;
        Readout("Solver time", "%.4f s", stats.time);
        Readout("Time step", "%.2f us", stats.dt * 1e6f);
        Readout("Max signal speed", "%.0f m/s", stats.maxSpeed);
        Readout("Solver steps", "%lld", stats.steps);
        Readout("Domain", "%.2f m half", stats.domainHalfExtent);
        Readout("Domain top", "%.2f m", stats.domainTop);
        Readout("Voxel size", "%.1f mm", stats.voxelSize * 1000.0f);
        Readout("Accuracy", "x%.2f", stats.accuracyFactor);
        Readout("Expansions", "%d", stats.expansions);
        if (stats.fieldBoundsValid) {
            Readout("Plume top", "%.2f m", stats.plumeTop);
            Readout("Plume radius", "%.2f m", stats.plumeRadius);
            if (sim.solverHemisphere) {
                Readout("Dome fill", "%.2f", stats.plumeDomeFraction);
            }
            if (stats.plumeTop > stats.domainTop * 0.9f) {
                Hint("The plume is close to the top of the solver domain. "
                     "Auto-scale expands it until the accuracy factor reaches "
                     "x16.");
            }
        }
        Hint("Compressible Euler solver (HLLC + MUSCL) running on the GPU. It "
             "uses the moist-air density and virtual temperature; wind drifts "
             "the rendered volume. The solver supplies the fireball and "
             "smoke; scene fires on burning objects are still rendered.");
    }

    SectionHeader("BLAST PHYSICS");

    const ex::sim::BlastModel& blast = simulation.Blast();
    const ex::sim::BlastState& blastState = blast.State();

    Readout("Shock radius", "%.1f m", blastState.shockRadius);
    Readout("Shock speed", "%.0f m/s (M %.2f)", blastState.shockSpeed,
            blastState.shockMach);
    Readout("Peak overpressure", "%.2f kPa",
            blastState.shockOverpressurePa / 1000.0);
    Readout("Fireball radius", "%.1f m", blastState.fireballRadius);
    Readout("Fireball surface", "%.0f K", blastState.fireballTemperatureK);
    Readout("Cloud top", "%.0f m", blast.CloudTopHeightM());
    Readout("Cloud radius", "%.1f m", blastState.cloudRadius);
    Readout("Dust ring", "%.0f m", blastState.dustRingRadius);

    SectionHeader("PREDICTED EFFECTS");

    Readout("5 psi (heavy damage)", "%.0f m",
            blast.RadiusForOverpressure(34474.0));
    Readout("1 psi (window damage)", "%.0f m",
            blast.RadiusForOverpressure(6895.0));
    Readout("3rd degree burns", "%.0f m",
            blast.RadiusForThermalFluence(80000.0));

    SectionHeader("OBSERVER");

    float distanceFloat = float(state.observerDistanceM);
    if (ImGui::SliderFloat("Distance", &distanceFloat, 1.0f, 3000.0f, "%.1f m",
                           ImGuiSliderFlags_Logarithmic)) {
        state.observerDistanceM = double(distanceFloat);
    }
    const double distance = state.observerDistanceM;
    const double slant = blast.ObserverSlantRange(distance);
    const double arrival = blast.ArrivalTimeS(slant);
    const double peak = blast.PeakOverpressureAtDistance(slant);
    const double duration = blast.PositivePhaseDurationS(slant);

    Readout("Slant range", "%.0f m", slant);
    if (arrival < 0.0) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
        Readout("Shock arrival", "not yet");
        ImGui::PopStyleColor();
    } else {
        Readout("Shock arrival", "%.3f s", arrival);
    }
    Readout("Peak overpressure", "%.2f kPa (%.2f psi)", peak / 1000.0,
            peak / 6894.76);
    Readout("Positive phase", "%.1f ms", duration * 1000.0);

    // Friedlander pressure history at the observer.
    std::vector<float> waveform(192);
    float maxPressure = 0.0f;
    if (arrival >= 0.0) {
        const double start = std::max(0.0, arrival - duration * 0.25);
        const double end = arrival + duration * 1.8;
        for (size_t i = 0; i < waveform.size(); ++i) {
            const double t = start + (end - start) * double(i) /
                                         double(waveform.size() - 1);
            const float pressure =
                float(blast.OverpressureAtTime(slant, t) / 1000.0);
            waveform[i] = pressure;
            maxPressure = std::max(maxPressure, pressure);
        }
        maxPressure = std::max(maxPressure, 0.5f);
        ImGui::PlotLines("##waveform", waveform.data(), int(waveform.size()), 0,
                         "kPa", 0.0f, maxPressure * 1.15f,
                         ImVec2(-1.0f, ImGui::GetFontSize() * 4.5f));
    }
    Hint("Brode overpressure fit, Rankine-Hugoniot shock speed and a "
         "Friedlander waveform. Free-air burst, idealised.");
}

void DrawRendering(LinuxUiState& state, const ex::sim::Simulation& simulation) {
    SectionHeader("RENDERING");
    ex::sim::SimulationSettings& sim = state.sim;

    ImGui::SliderFloat("Exposure", &state.exposure, 0.1f, 4.0f, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Bloom strength", &state.bloomStrength, 0.0f, 1.5f,
                       "%.2f");
    ImGui::SliderInt("Ray steps", &state.raymarchSteps, 24, 192);

    {
        static const int kAaValues[] = {0, 1, 2};
        static const char* kAaLabels[] = {"Off", "FXAA", "SMAA 1x"};
        int aaIndex = 0;
        for (int i = 0; i < 3; ++i) {
            if (kAaValues[i] == state.antiAliasing) {
                aaIndex = i;
            }
        }
        if (ImGui::Combo("Anti-aliasing", &aaIndex, kAaLabels, 3)) {
            state.antiAliasing = kAaValues[aaIndex];
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Post-process anti-aliasing for the finished "
                              "frame. FXAA is the cheapest; SMAA 1x keeps more "
                              "edge detail.");
        }
    }

    const int resolutions[] = {64, 96, 128, 160, 192, 256, 512, 1024};
    const char* resolutionLabels[] = {"64³",  "96³",  "128³", "160³",
                                      "192³", "256³", "512³", "1024³"};
    int resolutionIndex = 2;
    for (int i = 0; i < 8; ++i) {
        if (resolutions[i] == sim.volumeResolution) {
            resolutionIndex = i;
        }
    }
    if (ImGui::Combo("Volume resolution", &resolutionIndex, resolutionLabels,
                     8)) {
        sim.volumeResolution = resolutions[resolutionIndex];
        state.applySimulation = true;
    }
    if (sim.volumeResolution >= 512) {
        Hint("Resolutions of 512³ and 1024³ require several GB of RAM/VRAM and "
             "are baked at a reduced rate.");
    }

    ImGui::Checkbox("Volume", &state.enableVolume);
    ImGui::SameLine();
    ImGui::Checkbox("Particles", &state.enableParticles);
    ImGui::Checkbox("Ground", &state.enableGround);
    ImGui::SameLine();
    ImGui::Checkbox("Bloom", &state.enableBloom);
    ImGui::Checkbox("Grid overlay", &state.showGrid);

    const bool highFidelity = sim.mode == ex::sim::SimulationMode::HighFidelity;
    ImGui::BeginDisabled(highFidelity);
    if (ImGui::Checkbox("Auto-scale volume", &sim.autoScaleVolume)) {
        state.applySimulation = true;
    }
    ImGui::EndDisabled();
    // The compute paths bind GPU resources of the renderer, so they are chosen
    // at start-up (--no-gpu-volume-bake, --no-gpu-particles) and only shown.
    ImGui::BeginDisabled();
    bool gpuVolumeBake = state.gpuVolumeActive;
    ImGui::Checkbox("GPU volume bake", &gpuVolumeBake);
    bool gpuParticles = state.gpuParticlesActive;
    ImGui::Checkbox("GPU particles", &gpuParticles);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Chosen at start-up: --no-gpu-volume-bake and "
                          "--no-gpu-particles.");
    }

    if (highFidelity) {
        Readout("Solver grid", "%d³", state.solver.resolution);
        Hint("The Euler solver uses a fixed domain sized at detonation; "
             "auto-scale applies to the reduced-order volume.");
    } else {
        const ex::sim::VolumeFrame& frame = simulation.Volume().Current();
        if (frame.resolution > 0) {
            const float sizeX = frame.bounds.max.x - frame.bounds.min.x;
            const float sizeY = frame.bounds.max.y - frame.bounds.min.y;
            const float sizeZ = frame.bounds.max.z - frame.bounds.min.z;
            Readout("Volume box", "%.1f × %.1f × %.1f m", sizeX, sizeY, sizeZ);
            Readout("Voxel size", "%.1f mm",
                    (sizeX / float(frame.resolution)) * 1000.0f);
        }
        Hint("Auto-scale grows the baked volume to keep the fireball and smoke "
             "cloud inside it. The voxel count stays fixed, so the effective "
             "resolution drops as the box grows.");
    }

    ImGui::SliderFloat("Sun azimuth", &state.sunAzimuthDeg, 0.0f, 360.0f,
                       "%.0f°");
    ImGui::SliderFloat("Sun elevation", &state.sunElevationDeg, 1.0f, 80.0f,
                       "%.0f°");
    ImGui::SliderFloat("Particle scale", &state.particleScale, 0.25f, 2.0f,
                       "%.2f");
}

void DrawPerformance(LinuxUiState& state,
                     const ex::sim::Simulation& simulation) {
    SectionHeader("PERFORMANCE");

    static const int kFpsChoices[] = {30, 60, 90, 120, 144};
    static const char* kFpsLabels[] = {"30", "60", "90", "120", "144"};
    int fpsIndex = 4;
    for (int i = 0; i < 5; ++i) {
        if (kFpsChoices[i] == state.maxFps) {
            fpsIndex = i;
        }
    }
    if (ImGui::Combo("Max FPS", &fpsIndex, kFpsLabels, 5)) {
        state.maxFps = kFpsChoices[fpsIndex];
    }
    Hint("Caps the interactive frame rate; lower caps leave more GPU time for "
         "the high-fidelity solver.");

    Readout("Frame rate", "%.1f fps", state.fps);
    Readout("Frame time", "%.2f ms", state.frameMs);
    Readout("Volume bake", "%s", state.gpuVolumeActive ? "GPU" : "CPU");
    if (!state.gpuVolumeActive) {
        Readout("Bake time", "%.2f ms",
                simulation.Volume().LastBakeMilliseconds());
    }
    Readout("Model", "%s",
            simulation.Settings().mode == ex::sim::SimulationMode::HighFidelity
                ? "High-fidelity (GPU)"
                : "Reduced-order");
    Readout("Worker threads", "%d", state.workerThreads);
    const ex::sim::ParticleSystem& particles = simulation.Particles();
    Readout("Particles", "%d / %d", particles.AliveCount(),
            particles.Capacity());
    Readout("Particle backend", "%s", state.gpuParticlesActive ? "GPU" : "CPU");
    Readout("Debris", "%d", particles.AliveCount(ex::sim::ParticleKind::Debris));
    Readout("Sparks", "%d", particles.AliveCount(ex::sim::ParticleKind::Spark));
    Readout("Fire", "%d", particles.AliveCount(ex::sim::ParticleKind::Fire));
    Readout("Smoke", "%d", particles.AliveCount(ex::sim::ParticleKind::Smoke));
    Readout("Dust", "%d", particles.AliveCount(ex::sim::ParticleKind::Dust));
    Readout("GPU", "%s", state.adapterName.c_str());

    Hint("Particle integration and volume baking are distributed over a "
         "work-stealing thread pool; rendering runs on the GPU via Vulkan.");
}

void DrawUpdates(LinuxUiState& state) {
    ImGui::Checkbox("Check on startup", &state.updateCheckEnabled);
    ImGui::SameLine();
    ImGui::BeginDisabled(state.updateBusy);
    if (ImGui::Button("Check now")) {
        state.updateCheckRequested = true;
    }
    ImGui::EndDisabled();
    if (!state.updateStatus.empty()) {
        ImGui::TextWrapped("%s", state.updateStatus.c_str());
    }
    if (state.updateAvailable) {
        if (state.updateReady) {
            if (ImGui::Button("Install and restart")) {
                state.updateInstallRequested = true;
            }
        } else {
            ImGui::BeginDisabled(state.updateBusy);
            if (ImGui::Button("Download update")) {
                state.updateDownloadRequested = true;
            }
            ImGui::EndDisabled();
            if (state.updateBusy) {
                ImGui::ProgressBar(state.updateProgress, ImVec2(-1.0f, 0.0f));
            }
        }
        if (ImGui::Button("Skip this version")) {
            state.updateSkipRequested = true;
        }
        if (!state.updateNotes.empty() &&
            ImGui::CollapsingHeader("Release notes##update_notes")) {
            ImGui::TextWrapped("%s", state.updateNotes.c_str());
        }
    }
    if (!state.currentReleaseNotes.empty() &&
        ImGui::CollapsingHeader("What's new in this version")) {
        ImGui::TextWrapped("%s", state.currentReleaseNotes.c_str());
    }
}

} // namespace

LinuxUi::~LinuxUi() {
    Shutdown();
}

bool LinuxUi::BuildFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    // Use the same TTF family as the splash and the Windows panel. The size
    // follows the content scale of the display; the glyphs are rasterised at
    // the pixel density of the window so they stay sharp on a scaled desktop.
    const std::string fontPath = ex::app::UiFontPath();
    ImFont* font = nullptr;
    if (!fontPath.empty()) {
        ImFontConfig fontConfig{};
        fontConfig.OversampleH = 2;
        fontConfig.OversampleV = 1;
        fontConfig.PixelSnapH = true;
        fontConfig.RasterizerDensity = pixelDensity_;
        font = io.Fonts->AddFontFromFileTTF(fontPath.c_str(),
                                            16.0f * contentScale_, &fontConfig,
                                            io.Fonts->GetGlyphRangesDefault());
    }
    if (!font) {
        ImFontConfig fontConfig{};
        fontConfig.SizePixels = 13.0f * contentScale_;
        fontConfig.RasterizerDensity = pixelDensity_;
        io.Fonts->AddFontDefault(&fontConfig);
    }
    return true;
}

bool LinuxUi::Initialize(void* sdlWindow, const LinuxUiHandles& handles) {
    if (!sdlWindow || !handles.device || !handles.renderPass ||
        !handles.instance || !handles.physicalDevice || !handles.queue) {
        return false;
    }
    Shutdown();

    window_ = sdlWindow;
    SDL_Window* window = static_cast<SDL_Window*>(sdlWindow);
    pixelDensity_ = std::clamp(SDL_GetWindowPixelDensity(window), 1.0f, 4.0f);
    const float displayScale = SDL_GetWindowDisplayScale(window);
    contentScale_ = std::clamp(
        displayScale > 0.0f ? displayScale / pixelDensity_ : 1.0f, 0.75f, 4.0f);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // The panel is driven by the mouse and the shortcuts (Space, D, R, F, H);
    // keyboard navigation would also activate focused widgets on Space.
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(9.0f, 5.0f);
    style.ItemSpacing = ImVec2(9.0f, 7.0f);
    style.WindowRounding = 8.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 5.0f;
    g_baseStyle = style;
    style.ScaleAllSizes(contentScale_);

    BuildFonts();

    if (!ImGui_ImplSDL3_InitForVulkan(window)) {
        LogWarn("Linux UI: ImGui SDL3 backend initialization failed.");
        ImGui::DestroyContext();
        return false;
    }

    const VkDevice device = reinterpret_cast<VkDevice>(handles.device);
    const VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 8},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 8;
    poolInfo.poolSizeCount = uint32_t(std::size(poolSizes));
    poolInfo.pPoolSizes = poolSizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) !=
        VK_SUCCESS) {
        LogWarn("Linux UI: descriptor pool creation failed.");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = reinterpret_cast<VkInstance>(handles.instance);
    info.PhysicalDevice =
        reinterpret_cast<VkPhysicalDevice>(handles.physicalDevice);
    info.Device = device;
    info.QueueFamily = handles.queueFamily;
    info.Queue = reinterpret_cast<VkQueue>(handles.queue);
    info.DescriptorPool = pool;
    info.RenderPass = reinterpret_cast<VkRenderPass>(handles.renderPass);
    info.MinImageCount = 2;
    info.ImageCount = std::max(2u, handles.imageCount);
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.PipelineCache =
        reinterpret_cast<VkPipelineCache>(handles.pipelineCache);
    info.Subpass = 0;
    if (!ImGui_ImplVulkan_Init(&info)) {
        LogWarn("Linux UI: ImGui Vulkan backend initialization failed.");
        vkDestroyDescriptorPool(device, pool, nullptr);
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    initialized_ = true;
    device_ = handles.device;
    descriptorPool_ = pool;
    LogInfo("Linux UI ready: ImGui %s, scale %.2f, pixel density %.2f",
            IMGUI_VERSION, double(contentScale_), double(pixelDensity_));
    return true;
}

void LinuxUi::Shutdown() {
    if (!initialized_) {
        return;
    }
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (descriptorPool_) {
        vkDestroyDescriptorPool(reinterpret_cast<VkDevice>(device_),
                                reinterpret_cast<VkDescriptorPool>(descriptorPool_),
                                nullptr);
    }
    initialized_ = false;
    window_ = nullptr;
    device_ = nullptr;
    descriptorPool_ = nullptr;
}

void LinuxUi::ProcessEvent(const void* sdlEvent) {
    if (!initialized_ || !sdlEvent) {
        return;
    }
    const SDL_Event* event = static_cast<const SDL_Event*>(sdlEvent);
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_F1) {
        toggleVisible_ = true;
    }
    if (event->type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED ||
        event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        // Moving the window to another display (or changing the desktop
        // scale) changes the font size and density; the application rebuilds
        // the fonts between frames, when the GPU no longer uses them.
        SDL_Window* window = static_cast<SDL_Window*>(window_);
        const float density =
            std::clamp(SDL_GetWindowPixelDensity(window), 1.0f, 4.0f);
        const float displayScale = SDL_GetWindowDisplayScale(window);
        const float content = std::clamp(
            displayScale > 0.0f ? displayScale / density : 1.0f, 0.75f, 4.0f);
        if (std::abs(density - pixelDensity_) > 0.01f ||
            std::abs(content - contentScale_) > 0.01f) {
            rescalePending_ = true;
        }
    }
    ImGui_ImplSDL3_ProcessEvent(event);
}

void LinuxUi::ApplyScale() {
    if (!initialized_ || !rescalePending_) {
        return;
    }
    rescalePending_ = false;
    SDL_Window* window = static_cast<SDL_Window*>(window_);
    pixelDensity_ = std::clamp(SDL_GetWindowPixelDensity(window), 1.0f, 4.0f);
    const float displayScale = SDL_GetWindowDisplayScale(window);
    contentScale_ = std::clamp(
        displayScale > 0.0f ? displayScale / pixelDensity_ : 1.0f, 0.75f, 4.0f);

    ImGui_ImplVulkan_DestroyFontsTexture();
    BuildFonts();
    ImGuiStyle& style = ImGui::GetStyle();
    style = g_baseStyle;  // colours and unscaled sizes
    style.ScaleAllSizes(contentScale_);
    // The backend recreates the font texture in the next NewFrame().
    LogInfo("Linux UI rescaled: scale %.2f, pixel density %.2f",
            double(contentScale_), double(pixelDensity_));
}

LinuxCameraInput LinuxUi::CameraInput() {
    LinuxCameraInput input;
    if (!initialized_) {
        return input;
    }
    const ImGuiIO& io = ImGui::GetIO();
    // A drag belongs to the scene when it started there (see App.cpp).
    const ImGuiMouseButton buttons[3] = {ImGuiMouseButton_Left,
                                         ImGuiMouseButton_Right,
                                         ImGuiMouseButton_Middle};
    for (int i = 0; i < 3; ++i) {
        if (ImGui::IsMouseClicked(buttons[i]) && !io.WantCaptureMouse) {
            cameraDrag_[i] = true;
        }
        if (!ImGui::IsMouseDown(buttons[i])) {
            cameraDrag_[i] = false;
        }
    }
    if (cameraDrag_[0] && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        input.orbitX = io.MouseDelta.x;
        input.orbitY = io.MouseDelta.y;
    }
    if ((cameraDrag_[1] &&
         ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) ||
        (cameraDrag_[2] &&
         ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))) {
        input.panX = io.MouseDelta.x;
        input.panY = io.MouseDelta.y;
    }
    if (!io.WantCaptureMouse) {
        input.zoom = io.MouseWheel;
    }
    return input;
}

void LinuxUi::NewFrame() {
    if (!initialized_) {
        return;
    }
    ImGui_ImplSDL3_NewFrame();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
}

void LinuxUi::Draw(LinuxUiState& state, const ex::sim::Simulation& simulation,
                   float frameMs) {
    if (!initialized_) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (toggleVisible_) {
        state.visible = !state.visible;
        toggleVisible_ = false;
    }

    // Keyboard shortcuts of the Windows application (F1 also toggles the panel).
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            state.paused = !state.paused;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_D, false) && simulation.IsArmed()) {
            state.detonateRequested = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
            state.rearmRequested = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            state.reframeRequested = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_H, false)) {
            state.visible = !state.visible;
        }
    }

    state.fps = frameMs > 1e-4f ? 1000.0f / frameMs : 0.0f;
    state.frameMs = frameMs;

    // The town is generated on a worker thread: show the same progress strip
    // as the Windows application along the bottom edge, panel visible or not.
    if (simulation.SceneGenerating()) {
        ImDrawList* foreground = ImGui::GetForegroundDrawList();
        const ImVec2 display = io.DisplaySize;
        const float height = display.y * 0.05f;
        const float top = display.y - height;
        foreground->AddRectFilled(ImVec2(0.0f, top), display,
                                  IM_COL32(14, 16, 19, 235));
        const float progress =
            std::clamp(simulation.SceneProgress(), 0.0f, 1.0f);
        if (progress > 0.0f) {
            foreground->AddRectFilled(ImVec2(0.0f, top),
                                      ImVec2(display.x * progress, display.y),
                                      IM_COL32(52, 199, 89, 255));
        }
        char label[64];
        std::snprintf(label, sizeof(label), "Generating scene  %d%%",
                      int(progress * 100.0f + 0.5f));
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        foreground->AddText(ImVec2((display.x - textSize.x) * 0.5f,
                                   top + (height - textSize.y) * 0.5f),
                            IM_COL32(245, 248, 250, 255), label);
    }

    // Armed overlay: the charge is ready or counting down.
    if (simulation.IsArmed()) {
        ImDrawList* foreground = ImGui::GetForegroundDrawList();
        const ImVec2 display = io.DisplaySize;
        const float baseSize = ImGui::GetFontSize();
        const char* title =
            simulation.IsFuseHeld() ? "CHARGE READY" : "CHARGE ARMED";
        char big[64];
        if (simulation.IsFuseHeld()) {
            std::snprintf(big, sizeof(big), "READY  (D to detonate)");
        } else {
            std::snprintf(big, sizeof(big), "DETONATION IN %.2f s",
                          simulation.FuseRemaining());
        }
        const ImVec2 titleSize = ImGui::CalcTextSize(title);
        foreground->AddText(
            ImVec2((display.x - titleSize.x) * 0.5f, display.y * 0.10f),
            ImGui::GetColorU32(kWarning), title);
        const float bigSize = baseSize * 2.0f;
        const ImVec2 bigText = ImGui::GetFont()->CalcTextSizeA(
            bigSize, FLT_MAX, 0.0f, big);
        foreground->AddText(
            ImGui::GetFont(), bigSize,
            ImVec2((display.x - bigText.x) * 0.5f,
                   display.y * 0.10f + baseSize * 1.6f),
            ImGui::GetColorU32(kAccent), big);
    }

    if (!state.visible) {
        return;
    }

    ImGui::SetNextWindowPos(
        ImVec2(16.0f * contentScale_, 16.0f * contentScale_),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(
        ImVec2(400.0f * contentScale_, 640.0f * contentScale_),
        ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Explosion Simulator", &state.visible)) {
        ImGui::End();
        return;
    }

    ImGui::Text("Time %.3f s   %.2f ms   %.0f fps", simulation.Time(),
                double(frameMs), double(state.fps));
    ImGui::Text("Mode: %s",
                simulation.Settings().mode == ex::sim::SimulationMode::HighFidelity
                    ? "High fidelity"
                    : "Reduced order");
    ImGui::TextColored(kMuted, "F1 or H hides this panel");

    if (ImGui::CollapsingHeader("Detonation", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawDetonation(state, simulation);
    }
    if (ImGui::CollapsingHeader("Atmosphere")) {
        DrawAtmosphere(state);
    }
    if (ImGui::CollapsingHeader("Environment")) {
        DrawEnvironment(state, simulation);
    }
    if (ImGui::CollapsingHeader("Playback")) {
        DrawPlayback(state, simulation);
    }
    if (ImGui::CollapsingHeader("Physics & Effects",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawPhysics(state, simulation);
    }
    if (ImGui::CollapsingHeader("Rendering")) {
        DrawRendering(state, simulation);
    }
    if (ImGui::CollapsingHeader("Performance")) {
        DrawPerformance(state, simulation);
    }
    if (ImGui::CollapsingHeader("Updates")) {
        DrawUpdates(state);
    }

    ImGui::Separator();
    if (ImGui::Button("Quit")) {
        state.quitRequested = true;
    }
    ImGui::End();
}

void LinuxUi::Render(void* commandBuffer) {
    if (!initialized_ || !commandBuffer) {
        return;
    }
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(
        ImGui::GetDrawData(),
        reinterpret_cast<VkCommandBuffer>(commandBuffer));
}

void LinuxUi::SetMinImageCount(uint32_t count) {
    if (initialized_ && count > 0) {
        ImGui_ImplVulkan_SetMinImageCount(count);
    }
}

} // namespace ex::app
