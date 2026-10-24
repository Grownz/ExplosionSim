#include "ui/Ui.h"

#include "Version.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <vector>

namespace ex::ui {

namespace {

constexpr ImVec4 kAccent(1.00f, 0.52f, 0.16f, 1.00f);
constexpr ImVec4 kAccentDim(0.80f, 0.36f, 0.10f, 1.00f);
constexpr ImVec4 kWarning(1.00f, 0.78f, 0.35f, 1.00f);
constexpr ImVec4 kMuted(0.62f, 0.65f, 0.70f, 1.00f);

void SectionHeader(const char* label) {
    ImGui::Spacing();
    ImGui::SeparatorText(label);
    ImGui::Spacing();
}

void Readout(const char* label, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    ImGui::TextUnformatted(label);
    const float textWidth = ImGui::CalcTextSize(buffer).x;
    const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8.0f, rightEdge - textWidth));
    ImGui::TextUnformatted(buffer);
}

void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// Bottom-left map scale for the active grid overlay. The bar length is snapped
// to a 1-2-5 sequence and its pixel size is kept within a readable range as the
// camera zooms.
void DrawScaleBar(const gfx::Camera& camera, const ImVec2& displaySize) {
    if (displaySize.x < 200.0f || displaySize.y < 200.0f) {
        return;
    }

    const float distance = std::max(camera.Distance(), 0.1f);
    const float worldHeight = 2.0f * distance * std::tan(camera.FovY() * 0.5f);
    const float pixelsPerMeter = displaySize.y / std::max(worldHeight, 1e-4f);
    if (!std::isfinite(pixelsPerMeter) || pixelsPerMeter <= 0.0f) {
        return;
    }

    const float desiredPixels = std::clamp(displaySize.y * 0.13f, 80.0f, 190.0f);
    const float rawMeters = desiredPixels / pixelsPerMeter;
    const float exponent = std::floor(std::log10(std::max(rawMeters, 1e-5f)));
    float base = std::pow(10.0f, exponent);

    const float mantissa = rawMeters / base;
    float nice = 1.0f;
    if (mantissa >= 7.0f) {
        nice = 10.0f;
    } else if (mantissa >= 3.0f) {
        nice = 5.0f;
    } else if (mantissa >= 1.5f) {
        nice = 2.0f;
    }

    float meters = nice * base;
    float barPixels = meters * pixelsPerMeter;

    for (int i = 0; i < 8 && barPixels > 280.0f; ++i) {
        if (nice > 5.0f) {
            nice = 5.0f;
        } else if (nice > 2.0f) {
            nice = 2.0f;
        } else if (nice > 1.0f) {
            nice = 1.0f;
        } else {
            nice = 5.0f;
            base *= 0.1f;
        }
        meters = nice * base;
        barPixels = meters * pixelsPerMeter;
    }

    for (int i = 0; i < 8 && barPixels < 60.0f; ++i) {
        if (nice < 2.0f) {
            nice = 2.0f;
        } else if (nice < 5.0f) {
            nice = 5.0f;
        } else if (nice < 10.0f) {
            nice = 10.0f;
        } else {
            nice = 1.0f;
            base *= 10.0f;
        }
        meters = nice * base;
        barPixels = meters * pixelsPerMeter;
    }

    char label[64];
    if (meters >= 1000.0f) {
        std::snprintf(label, sizeof(label), "%.3g km", meters / 1000.0f);
    } else {
        std::snprintf(label, sizeof(label), "%.3g m", meters);
    }

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const float fontSize = ImGui::GetFontSize();
    const float margin = std::max(20.0f, fontSize * 1.2f);
    const float baseline = displaySize.y - margin - fontSize * 1.2f;
    const float x0 = margin;
    const float x1 = x0 + barPixels;

    const ImU32 color = IM_COL32(238, 241, 245, 235);
    const ImU32 shadow = IM_COL32(0, 0, 0, 160);

    drawList->AddRectFilled(ImVec2(x0 - 12.0f, baseline - fontSize * 2.4f),
                            ImVec2(x1 + 12.0f, baseline + 12.0f),
                            IM_COL32(8, 10, 14, 140), 6.0f);
    drawList->AddLine(ImVec2(x0, baseline), ImVec2(x1, baseline), shadow, 4.0f);
    drawList->AddLine(ImVec2(x0, baseline), ImVec2(x1, baseline), color, 2.0f);
    drawList->AddLine(ImVec2(x0, baseline - 7.0f), ImVec2(x0, baseline + 7.0f), color, 2.0f);
    drawList->AddLine(ImVec2(x1, baseline - 7.0f), ImVec2(x1, baseline + 7.0f), color, 2.0f);

    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 textPosition((x0 + x1) * 0.5f - textSize.x * 0.5f,
                              baseline - textSize.y - 9.0f);
    drawList->AddText(ImVec2(textPosition.x + 1.0f, textPosition.y + 1.0f), shadow, label);
    drawList->AddText(textPosition, color, label);
}

bool PhysSlider(const char* label, double* value, float minValue, float maxValue,
                const char* format, ImGuiSliderFlags flags, UiActions& actions) {
    float temporary = float(*value);
    const bool changed = ImGui::SliderFloat(label, &temporary, minValue, maxValue, format, flags);
    if (changed) {
        *value = double(temporary);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }
    return changed;
}

void DrawDetonation(app::AppSettings& settings, UiActions& actions) {
    SectionHeader("DETONATION");

    double massKg = settings.simulation.chargeMassKg;
    float kilograms = float(massKg);
    if (ImGui::SliderFloat("Charge", &kilograms, 0.01f, 20000.0f, "%.3f kg TNT",
                           ImGuiSliderFlags_Logarithmic)) {
        settings.simulation.chargeMassKg = std::clamp(double(kilograms), 0.01, 1.0e9);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }

    const char* presetLabels[] = {"20 g", "200 g", "1 kg", "50 kg", "1 t"};
    const double presetValues[] = {0.02, 0.2, 1.0, 50.0, 1000.0};
    for (int i = 0; i < 5; ++i) {
        if (i > 0) {
            ImGui::SameLine();
        }
        if (ImGui::Button(presetLabels[i], ImVec2(ImGui::GetContentRegionAvail().x / 5.0f - 2.0f, 0))) {
            settings.simulation.chargeMassKg = presetValues[i];
            actions.applySimulationSettings = true;
            actions.restart = true;
        }
    }

    PhysSlider("Height of burst", &settings.simulation.heightOfBurstM, 0.01, 400.0, "%.2f m",
               ImGuiSliderFlags_Logarithmic, actions);

    float fuse = float(settings.simulation.fuseSeconds);
    if (ImGui::SliderFloat("Fuse", &fuse, 0.0f, 10.0f, "%.1f s")) {
        settings.simulation.fuseSeconds = double(fuse);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
    if (ImGui::Button("DETONATE##main_detonate", ImVec2(-1.0f, ImGui::GetFontSize() * 2.2f))) {
        actions.detonateNow = true;
    }
    ImGui::PopStyleColor(3);
    Hint("Equivalent TNT energy: the charge mass is converted with 4.184 MJ/kg.");
}

void DrawAtmosphere(app::AppSettings& settings, UiActions& actions) {
    SectionHeader("ATMOSPHERE");

    double temperatureK = settings.simulation.ambientTemperatureK;
    float temperatureC = float(temperatureK - 273.15);
    if (ImGui::SliderFloat("Temperature", &temperatureC, -20.0f, 45.0f, "%.1f °C")) {
        settings.simulation.ambientTemperatureK = double(temperatureC) + 273.15;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }

    double pressurePa = settings.simulation.ambientPressurePa;
    float pressureHpa = float(pressurePa / 100.0);
    if (ImGui::SliderFloat("Pressure", &pressureHpa, 850.0f, 1050.0f, "%.0f hPa")) {
        settings.simulation.ambientPressurePa = double(pressureHpa) * 100.0;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }

    PhysSlider("Wind speed", &settings.simulation.windSpeedMs, 0.0, 30.0, "%.1f m/s",
               ImGuiSliderFlags_None, actions);
    PhysSlider("Wind direction", &settings.simulation.windDirectionDeg, 0.0, 360.0, "%.0f°",
               ImGuiSliderFlags_None, actions);

    float humidity = float(settings.simulation.relativeHumidity * 100.0);
    if (ImGui::SliderFloat("Humidity", &humidity, 0.0f, 100.0f, "%.0f %%")) {
        settings.simulation.relativeHumidity = double(humidity) / 100.0;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }

    // Derived quantities show the physical effect of the atmosphere.
    sim::Environment environment{};
    environment.pressurePa = settings.simulation.ambientPressurePa;
    environment.temperatureK = settings.simulation.ambientTemperatureK;
    environment.relativeHumidity = settings.simulation.relativeHumidity;
    Readout("Air density", "%.4f kg/m³", environment.AirDensity());
    Readout("Speed of sound", "%.1f m/s", environment.SoundSpeed());
    Readout("Vapour pressure", "%.0f Pa", environment.VaporPressurePa());
    Readout("Virtual temperature", "%.1f K", environment.VirtualTemperatureK());

    Hint("Density and sound speed drive the blast integration (Sachs scaling). "
         "Humidity lowers the density and raises the sound speed; wind advects the "
         "rising cloud and particles.");
}

void DrawPlayback(app::AppSettings& settings, UiActions& actions, const sim::Simulation& simulation) {
    SectionHeader("PLAYBACK");

    if (ImGui::Checkbox("Pause##playback", &settings.paused)) {
    }
    ImGui::SameLine();
    if (ImGui::Button("Step")) {
        actions.stepOnce = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Rearm")) {
        actions.restart = true;
    }

    float timeScale = float(settings.simulation.timeScale);
    if (ImGui::SliderFloat("Time scale", &timeScale, 0.005f, 2.0f, "×%.3f",
                           ImGuiSliderFlags_Logarithmic)) {
        settings.simulation.timeScale = double(timeScale);
        actions.applySimulationSettings = true;
    }

    if (ImGui::Checkbox("Loop simulation", &settings.simulation.loop)) {
        actions.applySimulationSettings = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Frame camera")) {
        actions.frameCamera = true;
    }

    if (simulation.IsArmed()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
        Readout("Fuse", "%.2f s", simulation.FuseRemaining());
        ImGui::PopStyleColor();
        if (ImGui::Button("Detonate now", ImVec2(-1.0f, 0.0f))) {
            actions.detonateNow = true;
        }
    } else {
        Readout("Simulation time", "%.4f s", simulation.Time());
    }
    Readout("Real time", "%.1f s", simulation.RealTime());
}

void DrawEnvironment(app::AppSettings& settings, const sim::Simulation& simulation,
                     UiActions& actions) {
    SectionHeader("ENVIRONMENT");

    auto& scene = settings.simulation.scene;
    if (ImGui::Checkbox("Procedural scene", &scene.enabled)) {
        actions.applySimulationSettings = true;
    }

    int seed = int(scene.seed);
    if (ImGui::InputInt("Scene seed", &seed)) {
        scene.seed = uint32_t(std::max(0, seed));
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }

    float density = scene.density;
    if (ImGui::SliderFloat("Scene density", &density, 0.0f, 2.0f, "%.2f")) {
        scene.density = density;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }

    if (ImGui::Checkbox("Buildings", &scene.buildings)) {
        actions.applySimulationSettings = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Vehicles", &scene.vehicles)) {
        actions.applySimulationSettings = true;
    }
    if (ImGui::Checkbox("Vegetation", &scene.vegetation)) {
        actions.applySimulationSettings = true;
    }

    const char* qualityLabels[] = {"Low", "Medium", "High"};
    int quality = std::clamp(scene.quality, 0, 2);
    if (ImGui::Combo("Scene quality", &quality, qualityLabels, 3)) {
        scene.quality = quality;
        actions.applySimulationSettings = true;
    }

    if (ImGui::Button("Regenerate scene")) {
        scene.seed += 1;
        actions.applySimulationSettings = true;
    }

    Readout("Objects", "%d", int(simulation.Scene().Objects().size()));
    Readout("Scene extent", "%.0f m", simulation.Scene().Extent());
    Hint("Buildings, vehicles and trees are generated deterministically around ground "
         "zero. The scene reacts to the analytic blast field.");
}

void DrawPhysics(app::AppSettings& settings, const sim::Simulation& simulation,
                 gfx::Renderer& renderer, UiActions& actions) {
    SectionHeader("SIMULATION MODEL");

    int mode = int(settings.simulation.mode);
    const char* modeLabels[] = {"Reduced-order (analytic)", "High-fidelity (GPU Euler)"};
    if (ImGui::Combo("Model", &mode, modeLabels, 2)) {
        settings.simulation.mode = sim::SimulationMode(mode);
        actions.applySimulationSettings = true;
    }

    if (settings.simulation.mode == sim::SimulationMode::HighFidelity) {
        const int solverResolutions[] = {64, 96, 128, 192, 256, 512};
        const char* solverResolutionLabels[] = {"64³", "96³", "128³", "192³", "256³", "512³"};
        int current = 2;
        for (int i = 0; i < 6; ++i) {
            if (solverResolutions[i] == settings.simulation.solverResolution) {
                current = i;
            }
        }
        if (ImGui::Combo("Solver grid", &current, solverResolutionLabels, 6)) {
            settings.simulation.solverResolution = solverResolutions[current];
            actions.applySimulationSettings = true;
        }

        if (ImGui::SliderInt("Steps/frame", &settings.simulation.solverStepsPerFrame, 4, 64)) {
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            actions.applySimulationSettings = true;
        }

        if (ImGui::Checkbox("Solver auto-scale", &settings.simulation.solverAutoScale)) {
            actions.applySimulationSettings = true;
        }

        if (settings.simulation.solverResolution >= 256) {
            Hint("Solver grids of 256³ and 512³ need several GB of VRAM and are not "
                 "real-time; reduce steps/frame.");
        }

        const gfx::SolverStats& stats = renderer.Solver();
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
            if (stats.plumeTop > stats.domainTop * 0.9f) {
                Hint("The plume is close to the top of the solver domain. Auto-scale "
                     "expands it until the accuracy factor reaches x16.");
            }
        }

        Hint("Compressible Euler solver (HLLC + MUSCL) running on the GPU. It uses the "
             "moist-air density and virtual temperature; wind drifts the rendered volume. "
             "Particle rendering is disabled in this mode.");
    }

    SectionHeader("BLAST PHYSICS");

    const sim::BlastModel& blast = simulation.Blast();
    const sim::BlastState& state = blast.State();

    Readout("Shock radius", "%.1f m", state.shockRadius);
    Readout("Shock speed", "%.0f m/s (M %.2f)", state.shockSpeed, state.shockMach);
    Readout("Peak overpressure", "%.2f kPa", state.shockOverpressurePa / 1000.0);
    Readout("Fireball radius", "%.1f m", state.fireballRadius);
    Readout("Fireball surface", "%.0f K", state.fireballTemperatureK);
    Readout("Cloud top", "%.0f m", blast.CloudTopHeightM());
    Readout("Cloud radius", "%.1f m", state.cloudRadius);
    Readout("Dust ring", "%.0f m", state.dustRingRadius);

    SectionHeader("PREDICTED EFFECTS");

    const double psi5 = blast.RadiusForOverpressure(34474.0);
    const double psi1 = blast.RadiusForOverpressure(6895.0);
    const double burn = blast.RadiusForThermalFluence(80000.0);

    Readout("5 psi (heavy damage)", "%.0f m", psi5);
    Readout("1 psi (window damage)", "%.0f m", psi1);
    Readout("3rd degree burns", "%.0f m", burn);

    SectionHeader("OBSERVER");

    double distance = settings.observerDistanceM;
    float distanceFloat = float(distance);
    if (ImGui::SliderFloat("Distance", &distanceFloat, 1.0f, 3000.0f, "%.1f m",
                           ImGuiSliderFlags_Logarithmic)) {
        distance = double(distanceFloat);
        settings.observerDistanceM = distance;
    }

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
    Readout("Peak overpressure", "%.2f kPa (%.2f psi)", peak / 1000.0, peak / 6894.76);
    Readout("Positive phase", "%.1f ms", duration * 1000.0);

    // Friedlander pressure history at the observer.
    std::vector<float> waveform(192);
    float maxPressure = 0.0f;
    if (arrival >= 0.0) {
        const double start = std::max(0.0, arrival - duration * 0.25);
        const double end = arrival + duration * 1.8;
        for (size_t i = 0; i < waveform.size(); ++i) {
            const double t = start + (end - start) * double(i) / double(waveform.size() - 1);
            const float pressure = float(blast.OverpressureAtTime(slant, t) / 1000.0);
            waveform[i] = pressure;
            maxPressure = std::max(maxPressure, pressure);
        }
        maxPressure = std::max(maxPressure, 0.5f);
        ImGui::PlotLines("##waveform", waveform.data(), int(waveform.size()), 0, "kPa",
                         0.0f, maxPressure * 1.15f, ImVec2(-1.0f, ImGui::GetFontSize() * 4.5f));
    }

    Hint("Brode overpressure fit, Rankine-Hugoniot shock speed and a Friedlander "
         "waveform. Free-air burst, idealised.");
}

void DrawRendering(app::AppSettings& settings, UiActions& actions, const gfx::Renderer& renderer,
                   const sim::Simulation& simulation) {
    SectionHeader("RENDERING");

    gfx::RenderSettings& render = settings.render;
    ImGui::SliderFloat("Exposure", &render.exposure, 0.1f, 4.0f, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Bloom strength", &render.bloomStrength, 0.0f, 1.5f, "%.2f");
    ImGui::SliderInt("Ray steps", &render.raymarchSteps, 24, 192);

    const int resolutions[] = {64, 96, 128, 160, 192, 256, 512, 1024};
    const char* resolutionLabels[] = {"64³", "96³", "128³", "160³",
                                      "192³", "256³", "512³", "1024³"};
    int resolutionIndex = 2;
    for (int i = 0; i < 8; ++i) {
        if (resolutions[i] == settings.simulation.volumeResolution) {
            resolutionIndex = i;
        }
    }
    if (ImGui::Combo("Volume resolution", &resolutionIndex, resolutionLabels, 8)) {
        settings.simulation.volumeResolution = resolutions[resolutionIndex];
        actions.applySimulationSettings = true;
    }
    if (settings.simulation.volumeResolution >= 512) {
        Hint("Resolutions of 512³ and 1024³ require several GB of RAM/VRAM and are "
             "baked at a reduced rate.");
    }

    ImGui::Checkbox("Volume", &render.enableVolume);
    ImGui::SameLine();
    ImGui::Checkbox("Particles", &render.enableParticles);
    ImGui::Checkbox("Ground", &render.enableGround);
    ImGui::SameLine();
    ImGui::Checkbox("Bloom", &render.enableBloom);
    ImGui::Checkbox("Grid overlay", &render.showGrid);

    const bool highFidelity = settings.simulation.mode == sim::SimulationMode::HighFidelity;
    ImGui::BeginDisabled(highFidelity);
    if (ImGui::Checkbox("Auto-scale volume", &settings.simulation.autoScaleVolume)) {
        actions.applySimulationSettings = true;
    }
    ImGui::EndDisabled();

    if (highFidelity) {
        Readout("Solver grid", "%d³", renderer.Solver().resolution);
        Hint("The Euler solver uses a fixed domain sized at detonation; auto-scale "
             "applies to the reduced-order volume.");
    } else {
        const sim::VolumeFrame& frame = simulation.Volume().Current();
        if (!frame.empty()) {
            const float sizeX = frame.bounds.max.x - frame.bounds.min.x;
            const float sizeY = frame.bounds.max.y - frame.bounds.min.y;
            const float sizeZ = frame.bounds.max.z - frame.bounds.min.z;
            Readout("Volume box", "%.1f × %.1f × %.1f m", sizeX, sizeY, sizeZ);
            Readout("Voxel size", "%.1f mm", (sizeX / float(frame.resolution)) * 1000.0f);
        }
        Hint("Auto-scale grows the baked volume to keep the fireball and smoke cloud "
             "inside it. The voxel count stays fixed, so the effective resolution drops "
             "as the box grows.");
    }

    ImGui::SliderFloat("Sun azimuth", &render.sunAzimuthDeg, 0.0f, 360.0f, "%.0f°");
    ImGui::SliderFloat("Sun elevation", &render.sunElevationDeg, 1.0f, 80.0f, "%.0f°");
    ImGui::SliderFloat("Particle scale", &render.particleScale, 0.25f, 2.0f, "%.2f");

    Readout("Volume texture", "%d³", renderer.VolumeTextureResolution());
}

void DrawPerformance(const UiContext& context) {
    SectionHeader("PERFORMANCE");

    Readout("Frame rate", "%.1f fps", context.fps);
    Readout("Frame time", "%.2f ms", context.frameMs);
    Readout("CPU render", "%.2f ms", context.renderer.Info().cpuRenderMs);
    Readout("Volume bake", "%.2f ms", context.simulation.Volume().LastBakeMilliseconds());
    Readout("Model", "%s", context.renderer.Info().highFidelity ? "High-fidelity (GPU)"
                                                                : "Reduced-order");
    Readout("Worker threads", "%d", context.workerThreads);
    Readout("Particles", "%d / %d", context.simulation.Particles().AliveCount(),
            context.simulation.Particles().Capacity());
    Readout("Debris", "%d", context.simulation.Particles().AliveCount(sim::ParticleKind::Debris));
    Readout("Sparks", "%d", context.simulation.Particles().AliveCount(sim::ParticleKind::Spark));
    Readout("Fire", "%d", context.simulation.Particles().AliveCount(sim::ParticleKind::Fire));
    Readout("Smoke", "%d", context.simulation.Particles().AliveCount(sim::ParticleKind::Smoke));
    Readout("Dust", "%d", context.simulation.Particles().AliveCount(sim::ParticleKind::Dust));
    Readout("GPU", "%s", context.adapterName);

    Hint("Particle integration and volume baking are distributed over a "
         "work-stealing thread pool; rendering runs on the GPU via Direct3D 11.");
}

} // namespace

void ApplyTheme(float dpiScale) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark();

    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(9.0f, 5.0f);
    style.ItemSpacing = ImVec2(9.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 5.0f);
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 5.0f;
    style.ScrollbarRounding = 8.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.043f, 0.047f, 0.056f, 0.94f);
    style.Colors[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.055f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.13f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.05f, 0.055f, 0.065f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.06f, 0.065f, 0.075f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(1.0f, 0.52f, 0.16f, 0.55f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(1.0f, 0.52f, 0.16f, 0.80f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.0f, 0.52f, 0.16f, 0.85f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.62f, 0.30f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.52f, 0.16f, 1.0f);
    style.Colors[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
    style.Colors[ImGuiCol_Text] = ImVec4(0.90f, 0.91f, 0.93f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.55f, 0.60f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(1.0f, 0.58f, 0.22f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 0.72f, 0.35f, 1.0f);

    style.ScaleAllSizes(dpiScale);
}

void LoadFonts(float dpiScale) {
    ImGuiIO& io = ImGui::GetIO();
    const float fontSize = std::round(16.5f * dpiScale);

    ImFontConfig config{};
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.PixelSnapH = true;

    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", fontSize, &config,
                                               io.Fonts->GetGlyphRangesDefault());
    if (!font) {
        io.Fonts->AddFontDefault();
    }
}

UiActions Draw(UiContext& context) {
    UiActions actions;
    if (!context.settings.showUi) {
        return actions;
    }

    app::AppSettings& settings = context.settings;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float panelWidth = ImGui::GetFontSize() * 25.0f;
    const float margin = ImGui::GetFontSize() * 0.85f;

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - panelWidth - margin,
                                   viewport->WorkPos.y + margin));
    ImGui::SetNextWindowSize(ImVec2(panelWidth, viewport->WorkSize.y - margin * 2.0f));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    ImGui::Begin("##control_panel", nullptr, flags);
    ImGui::PopStyleVar();

    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted("EXPLOSION SIMULATOR");
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::Text("v%s  ·  Physically based blast visualisation", EXPLOSION_VERSION_STRING);
    ImGui::PopStyleColor();

    ImGui::Spacing();

    if (ImGui::Button(settings.paused ? "Resume##top_pause" : "Pause##top_pause",
                      ImVec2(panelWidth * 0.30f, 0))) {
        settings.paused = !settings.paused;
    }
    ImGui::SameLine();
    if (ImGui::Button("Detonate##top_detonate", ImVec2(panelWidth * 0.30f, 0))) {
        actions.detonateNow = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Frame##top_frame", ImVec2(panelWidth * 0.24f, 0))) {
        actions.frameCamera = true;
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Detonation", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawDetonation(settings, actions);
    }
    if (ImGui::CollapsingHeader("Atmosphere")) {
        DrawAtmosphere(settings, actions);
    }
    if (ImGui::CollapsingHeader("Environment")) {
        DrawEnvironment(settings, context.simulation, actions);
    }
    if (ImGui::CollapsingHeader("Playback")) {
        DrawPlayback(settings, actions, context.simulation);
    }
    if (ImGui::CollapsingHeader("Physics & Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawPhysics(settings, context.simulation, context.renderer, actions);
    }
    if (ImGui::CollapsingHeader("Rendering")) {
        DrawRendering(settings, actions, context.renderer, context.simulation);
    }
    if (ImGui::CollapsingHeader("Performance")) {
        DrawPerformance(context);
    }

    ImGui::Spacing();
    ImGui::Separator();
    Hint("Space pause · R rearm · F frame · H hide UI");
    ImGui::End();

    if (settings.render.showGrid) {
        DrawScaleBar(context.camera, ImGui::GetIO().DisplaySize);
    }

    // --- On-screen overlays -------------------------------------------------
    const ImGuiWindowFlags overlayFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;

    if (context.simulation.IsArmed()) {
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                   viewport->WorkPos.y + viewport->WorkSize.y * 0.12f),
            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::Begin("##armed_overlay", nullptr,
                     overlayFlags | ImGuiWindowFlags_NoBackground);
        ImGui::SetWindowFontScale(1.1f);
        ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
        ImGui::TextUnformatted("CHARGE ARMED");
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(2.4f);
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::Text("DETONATION IN %.2f s", context.simulation.FuseRemaining());
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::End();
    } else {
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + margin, viewport->WorkPos.y + margin),
                                ImGuiCond_Always);
        ImGui::Begin("##timecode", nullptr, overlayFlags | ImGuiWindowFlags_NoBackground);
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::Text("t = %.4f s   |   %.3g kg TNT   |   shock front %.1f m",
                    context.simulation.Time(), context.settings.simulation.chargeMassKg,
                    context.simulation.Blast().State().shockRadius);
        ImGui::PopStyleColor();
        ImGui::End();
    }

    return actions;
}

} // namespace ex::ui
