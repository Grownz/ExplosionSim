#include "app/Settings.h"

#include "gfx/Shader.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace ex::app {

namespace {

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return value;
}

std::string Trim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ParseBool(const std::string& value, bool fallback) {
    const std::string lowered = ToLower(Trim(value));
    if (lowered == "true" || lowered == "1" || lowered == "yes") {
        return true;
    }
    if (lowered == "false" || lowered == "0" || lowered == "no") {
        return false;
    }
    return fallback;
}

float ParseFloat(const std::map<std::string, std::string>& values, const char* key, float fallback) {
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    try {
        return std::stof(it->second);
    } catch (...) {
        return fallback;
    }
}

double ParseDouble(const std::map<std::string, std::string>& values, const char* key, double fallback) {
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    try {
        return std::stod(it->second);
    } catch (...) {
        return fallback;
    }
}

int ParseInt(const std::map<std::string, std::string>& values, const char* key, int fallback) {
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    try {
        return std::stoi(it->second);
    } catch (...) {
        return fallback;
    }
}

bool ParseBool(const std::map<std::string, std::string>& values, const char* key, bool fallback) {
    const auto it = values.find(key);
    return it == values.end() ? fallback : ParseBool(it->second, fallback);
}

} // namespace

std::wstring DefaultSettingsPath() {
    const std::wstring resolved = ex::gfx::ResolveAssetPath(L"settings.ini");
    if (!resolved.empty()) {
        return resolved;
    }
    return L"settings.ini";
}

void LoadSettings(AppSettings& settings, const std::wstring& path) {
    std::ifstream stream(path);
    if (!stream.is_open()) {
        return;
    }

    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(stream, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == '[') {
            continue;
        }
        const size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        values[Trim(line.substr(0, separator))] = Trim(line.substr(separator + 1));
    }

    auto& simulation = settings.simulation;
    simulation.chargeMassKg = std::clamp(ParseDouble(values, "chargeMassKg", simulation.chargeMassKg), 0.01, 1.0e9);
    simulation.heightOfBurstM =
        std::clamp(ParseDouble(values, "heightOfBurstM", simulation.heightOfBurstM), 0.0, 5000.0);
    simulation.ambientTemperatureK =
        std::clamp(ParseDouble(values, "ambientTemperatureK", simulation.ambientTemperatureK), 200.0, 340.0);
    simulation.ambientPressurePa =
        std::clamp(ParseDouble(values, "ambientPressurePa", simulation.ambientPressurePa), 50000.0, 110000.0);
    simulation.relativeHumidity =
        std::clamp(ParseDouble(values, "relativeHumidity", simulation.relativeHumidity), 0.0, 1.0);
    simulation.windSpeedMs = std::clamp(ParseDouble(values, "windSpeedMs", simulation.windSpeedMs), 0.0, 60.0);
    simulation.windDirectionDeg =
        std::clamp(ParseDouble(values, "windDirectionDeg", simulation.windDirectionDeg), 0.0, 360.0);
    simulation.timeScale = std::clamp(ParseDouble(values, "timeScale", simulation.timeScale), 0.005, 2.0);
    simulation.fuseSeconds = std::clamp(ParseDouble(values, "fuseSeconds", simulation.fuseSeconds), 0.0, 30.0);
    simulation.mode = ParseInt(values, "simulationMode", int(simulation.mode)) == 1
                          ? sim::SimulationMode::HighFidelity
                          : sim::SimulationMode::ReducedOrder;
    simulation.solverResolution =
        std::clamp(ParseInt(values, "solverResolution", simulation.solverResolution), 32, 512);
    simulation.solverStepsPerFrame =
        std::clamp(ParseInt(values, "solverStepsPerFrame", simulation.solverStepsPerFrame), 1, 96);
    simulation.solverAutoScale =
        ParseBool(values, "solverAutoScale", simulation.solverAutoScale);
    simulation.loop = ParseBool(values, "loop", simulation.loop);
    simulation.particleBudget =
        std::clamp(ParseInt(values, "particleBudget", simulation.particleBudget), 4000, 400000);
    simulation.volumeResolution =
        std::clamp(ParseInt(values, "volumeResolution", simulation.volumeResolution), 32, 1024);
    simulation.bakeVolume = ParseBool(values, "bakeVolume", simulation.bakeVolume);
    simulation.autoScaleVolume = ParseBool(values, "autoScaleVolume", simulation.autoScaleVolume);

    simulation.scene.enabled = ParseBool(values, "sceneEnabled", simulation.scene.enabled);
    simulation.scene.seed = uint32_t(std::clamp(
        ParseInt(values, "sceneSeed", int(simulation.scene.seed)), 0, 1000000));
    simulation.scene.density = std::clamp(
        ParseFloat(values, "sceneDensity", simulation.scene.density), 0.0f, 4.0f);
    simulation.scene.buildings =
        ParseBool(values, "sceneBuildings", simulation.scene.buildings);
    simulation.scene.vehicles =
        ParseBool(values, "sceneVehicles", simulation.scene.vehicles);
    simulation.scene.vegetation =
        ParseBool(values, "sceneVegetation", simulation.scene.vegetation);
    simulation.scene.voronoiChunks =
        ParseBool(values, "sceneVoronoiChunks", simulation.scene.voronoiChunks);
    simulation.scene.quality =
        std::clamp(ParseInt(values, "sceneQuality", simulation.scene.quality), 0, 2);
    simulation.scene.groundReflection =
        std::clamp(ParseFloat(values, "sceneGroundReflection",
                              simulation.scene.groundReflection),
                   0.0f, 1.0f);

    auto& render = settings.render;
    render.raymarchSteps = std::clamp(ParseInt(values, "raymarchSteps", render.raymarchSteps), 16, 256);
    render.exposure = std::clamp(ParseFloat(values, "exposure", render.exposure), 0.05f, 8.0f);
    render.bloomStrength = std::clamp(ParseFloat(values, "bloomStrength", render.bloomStrength), 0.0f, 3.0f);
    render.enableVolume = ParseBool(values, "enableVolume", render.enableVolume);
    render.enableParticles = ParseBool(values, "enableParticles", render.enableParticles);
    render.enableGround = ParseBool(values, "enableGround", render.enableGround);
    render.enableBloom = ParseBool(values, "enableBloom", render.enableBloom);
    render.showGrid = ParseBool(values, "showGrid", render.showGrid);
    render.sunAzimuthDeg = ParseFloat(values, "sunAzimuthDeg", render.sunAzimuthDeg);
    render.sunElevationDeg =
        std::clamp(ParseFloat(values, "sunElevationDeg", render.sunElevationDeg), -5.0f, 85.0f);
    render.particleScale = std::clamp(ParseFloat(values, "particleScale", render.particleScale), 0.1f, 4.0f);

    settings.paused = ParseBool(values, "paused", settings.paused);
    settings.showUi = ParseBool(values, "showUi", settings.showUi);
    settings.observerDistanceM =
        std::clamp(ParseDouble(values, "observerDistanceM", settings.observerDistanceM), 5.0, 5000.0);
}

void SaveSettings(const AppSettings& settings, const std::wstring& path) {
    std::ofstream stream(path);
    if (!stream.is_open()) {
        return;
    }

    const auto& simulation = settings.simulation;
    const auto& render = settings.render;

    stream << "# Explosion Simulator settings\n";
    stream << "[simulation]\n";
    stream << "chargeMassKg=" << simulation.chargeMassKg << "\n";
    stream << "heightOfBurstM=" << simulation.heightOfBurstM << "\n";
    stream << "ambientTemperatureK=" << simulation.ambientTemperatureK << "\n";
    stream << "ambientPressurePa=" << simulation.ambientPressurePa << "\n";
    stream << "relativeHumidity=" << simulation.relativeHumidity << "\n";
    stream << "windSpeedMs=" << simulation.windSpeedMs << "\n";
    stream << "windDirectionDeg=" << simulation.windDirectionDeg << "\n";
    stream << "timeScale=" << simulation.timeScale << "\n";
    stream << "fuseSeconds=" << simulation.fuseSeconds << "\n";
    stream << "simulationMode=" << int(simulation.mode) << "\n";
    stream << "solverResolution=" << simulation.solverResolution << "\n";
    stream << "solverStepsPerFrame=" << simulation.solverStepsPerFrame << "\n";
    stream << "solverAutoScale=" << (simulation.solverAutoScale ? "true" : "false") << "\n";
    stream << "loop=" << (simulation.loop ? "true" : "false") << "\n";
    stream << "particleBudget=" << simulation.particleBudget << "\n";
    stream << "volumeResolution=" << simulation.volumeResolution << "\n";
    stream << "bakeVolume=" << (simulation.bakeVolume ? "true" : "false") << "\n";
    stream << "autoScaleVolume=" << (simulation.autoScaleVolume ? "true" : "false") << "\n";
    stream << "sceneEnabled=" << (simulation.scene.enabled ? "true" : "false") << "\n";
    stream << "sceneSeed=" << simulation.scene.seed << "\n";
    stream << "sceneDensity=" << simulation.scene.density << "\n";
    stream << "sceneBuildings=" << (simulation.scene.buildings ? "true" : "false") << "\n";
    stream << "sceneVehicles=" << (simulation.scene.vehicles ? "true" : "false") << "\n";
    stream << "sceneVegetation=" << (simulation.scene.vegetation ? "true" : "false") << "\n";
    stream << "sceneVoronoiChunks=" << (simulation.scene.voronoiChunks ? "true" : "false") << "\n";
    stream << "sceneQuality=" << simulation.scene.quality << "\n";
    stream << "sceneGroundReflection=" << simulation.scene.groundReflection << "\n";

    stream << "[render]\n";
    stream << "raymarchSteps=" << render.raymarchSteps << "\n";
    stream << "exposure=" << render.exposure << "\n";
    stream << "bloomStrength=" << render.bloomStrength << "\n";
    stream << "enableVolume=" << (render.enableVolume ? "true" : "false") << "\n";
    stream << "enableParticles=" << (render.enableParticles ? "true" : "false") << "\n";
    stream << "enableGround=" << (render.enableGround ? "true" : "false") << "\n";
    stream << "enableBloom=" << (render.enableBloom ? "true" : "false") << "\n";
    stream << "showGrid=" << (render.showGrid ? "true" : "false") << "\n";
    stream << "sunAzimuthDeg=" << render.sunAzimuthDeg << "\n";
    stream << "sunElevationDeg=" << render.sunElevationDeg << "\n";
    stream << "particleScale=" << render.particleScale << "\n";

    stream << "[ui]\n";
    stream << "paused=" << (settings.paused ? "true" : "false") << "\n";
    stream << "showUi=" << (settings.showUi ? "true" : "false") << "\n";
    stream << "observerDistanceM=" << settings.observerDistanceM << "\n";
}

} // namespace ex::app
