// Native Linux application shell. It owns the SDL3 window and the Vulkan RHI
// device; the reduced-order renderer is ported on top of this loop.

#include "Version.h"
#include "app/LinuxPreview.h"
#include "app/LinuxUi.h"
#include "app/LinuxUpdate.h"
#include "app/SettingsPath.h"
#include "app/SplashSdl.h"
#include "core/Log.h"
#include "core/ThreadPool.h"
#include "gfx/Camera.h"
#include "gfx/ImageIO.h"
#include "gfx/GpuVolumeBaker.h"
#include "gfx/GpuParticleSystem.h"
#include "gfx/RhiEulerSolver.h"
#include "gfx/rhi/Rhi.h"
#include "gfx/rhi/vulkan/VulkanRhi.h"
#include "platform/Platform.h"
#include "sim/Simulation.h"
#include "update/Version.h"
#include "EulerBodyFlowShader.h"
#include "EulerInitShader.h"
#include "EulerLightShader.h"
#include "EulerReduceShader.h"
#include "EulerRenderShader.h"
#include "EulerRescaleShader.h"
#include "EulerSweepShader.h"
#include "ParticlesGpuShaders.h"
#include "VolumeBakeShader.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    int width = 1600;
    int height = 900;
    int frames = 0; // 0 = unlimited for a window, one frame for a capture
    bool headless = false;
    bool debug = false;
    bool vsync = true;
    std::string screenshot;
    double captureTime = 0.12;
    double chargeMassKg = -1.0;
    double heightOfBurstM = -1.0;
    double timeScale = 0.06;
    double fuseSeconds = 2.0;
    double windSpeed = 1.0;
    double ambientTemperature = 288.15;
    int particleBudget = 40000;
    float azimuth = 128.0f;
    float elevation = 14.0f;
    float distanceScale = 1.0f;
    uint32_t sceneSeed = 69707;
    bool sceneSeedSet = false;
    float sceneDensity = 1.0f;
    bool sceneDensitySet = false;
    bool sceneEnabled = true;
    bool sceneEnabledSet = false;
    int sceneQuality = 1;
    float sceneChunkScale = 1.0f;
    bool sceneVoronoiChunks = true;
    bool sceneBuildings = true;
    bool sceneVehicles = true;
    bool sceneVegetation = true;
    int volumeResolution = 0;
    bool gpuVolumeBake = true;
    bool gpuParticles = true;
    bool highFidelity = false;
    bool flowAround = true;
    bool bodyDrag = true;
    int solverResolution = 0;
    int solverSteps = 0;
    double solverTime = -1.0;
    bool solverAutoScale = true;
    bool solverAutoScaleSet = false;
    bool solverHemisphere = false;
    bool solverBenchmark = false;
    long long solverStepCount = 0;
    bool splashTest = false;
    bool benchmark = false;
    std::string splashScreenshot;
    std::string writeSettings;
    int raymarchSteps = 128;
    float exposure = 0.8f;
    float bloomStrength = 0.10f;
    float particleScale = 1.0f;
    float sunAzimuth = 302.0f;
    float sunElevation = 13.0f;
    bool bloomEnabled = true;
    bool groundEnabled = true;
    bool volumeEnabled = true;
    bool particlesEnabled = true;
    bool showGrid = true;
    bool noUpdateCheck = false;
    bool checkForUpdates = true;
    std::string skippedVersion;
    long long lastCheckUnix = 0;
};

void PrintUsage() {
    std::printf("Explosion Simulator %s\n", EXPLOSION_VERSION_STRING);
    std::printf("Usage: explosion_linux [options]\n\n");
    std::printf("  --screenshot <path>  Render an offscreen image (.png or .ppm) and exit.\n");
    std::printf("  --time <seconds>     Simulation time of the screenshot (default 0.12).\n");
    std::printf("  --width <pixels>     Output width (default 1600).\n");
    std::printf("  --height <pixels>    Output height (default 900).\n");
    std::printf("  --frames <count>     Number of frames to render.\n");
    std::printf("  --charge <kg>        TNT equivalent mass in kilograms.\n");
    std::printf("  --hob <metres>       Height of burst above ground.\n");
    std::printf("  --azimuth <degrees>  Camera azimuth for captures.\n");
    std::printf("  --elevation <deg>    Camera elevation for captures.\n");
    std::printf("  --distance-scale <x> Multiplier for the framing distance.\n");
    std::printf("  --scene-seed <n>     Seed for the procedural environment.\n");
    std::printf("  --scene-density <x>  Environment density multiplier.\n");
    std::printf("  --no-scene           Disable the procedural environment.\n");
    std::printf("  --volume-resolution <n>  Volume grid resolution (default 128).\n");
    std::printf("  --no-gpu-volume-bake Disable the GPU volume bake.\n");
    std::printf("  --no-gpu-particles   Disable the GPU particle path.\n");
    std::printf("  --high-fidelity      Use the GPU Euler solver.\n");
    std::printf("  --solver-resolution <n>  Solver grid resolution (default 128).\n");
    std::printf("  --solver-steps <n>   Max solver steps per frame (default 96).\n");
    std::printf("  --solver-time <s>    Solver time for the high-fidelity capture.\n");
    std::printf("  --solver-benchmark   Run the high-fidelity solver benchmark and exit.\n");
    std::printf("  --solver-step-count <n>  Measured solver steps for the benchmark.\n");
    std::printf("  --flow-around / --no-flow-around  Toggle scene obstacles (default on).\n");
    std::printf("  --body-drag / --no-body-drag      Toggle 4C debris drag (default on).\n");
    std::printf("  --splash-test        Show the SDL3 splash briefly and exit.\n");
    std::printf("  --benchmark          Render a fixed frame count and report the average.\n");
    std::printf("  --write-settings <path>  Write the active settings and exit.\n");
    std::printf("  --no-solver-auto-scale  Disable high-fidelity domain auto-scale.\n");
    std::printf("  --raymarch-steps <n>     Volume raymarch steps (default 128).\n");
    std::printf("  --exposure <x>       Tonemap exposure (default 0.8).\n");
    std::printf("  --bloom-strength <x> Bloom strength (default 0.10).\n");
    std::printf("  --particle-scale <x> Particle size multiplier.\n");
    std::printf("  --sun-azimuth <deg>  Sun azimuth (default 302).\n");
    std::printf("  --sun-elevation <deg> Sun elevation (default 13).\n");
    std::printf("  --no-bloom           Disable bloom.\n");
    std::printf("  --no-ground          Disable the ground plane.\n");
    std::printf("  --no-volume          Disable volumetric smoke.\n");
    std::printf("  --no-particles       Disable particles.\n");
    std::printf("  --no-update-check    Skip the startup update check for this run.\n");
    std::printf("  --headless           Use an offscreen target without a window.\n");
    std::printf("  --no-vsync           Disable vertical synchronisation.\n");
    std::printf("  --debug              Enable Vulkan validation layers.\n");
    std::printf("  --version            Print the version and exit.\n");
    std::printf("  --help               Show this help.\n");
}

bool ParseInt(const char* text, int& value) {
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (!end || *end != '\0' || parsed <= 0 || parsed > 16384) {
        return false;
    }
    value = int(parsed);
    return true;
}

bool ParseDouble(const char* text, double& value) {
    char* end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (!end || *end != '\0') {
        return false;
    }
    value = parsed;
    return true;
}

bool ParseFloat(const char* text, float& value) {
    double parsed = double(value);
    if (!ParseDouble(text, parsed)) {
        return false;
    }
    value = float(parsed);
    return true;
}

bool ParseSeed(const char* text, uint32_t& value) {
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (!end || *end != '\0' || parsed < 0 || parsed > 1000000) {
        return false;
    }
    value = uint32_t(parsed);
    return true;
}

std::string Trim(std::string value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool IniBool(const std::string& value, bool fallback) {
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (lowered == "true" || lowered == "1" || lowered == "yes") {
        return true;
    }
    if (lowered == "false" || lowered == "0" || lowered == "no") {
        return false;
    }
    return fallback;
}

void LoadSettingsFile(const std::string& path, Options& options) {
    std::ifstream file(path);
    if (!file) {
        return;
    }
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == '[') {
            continue;
        }
        const size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        values[Trim(line.substr(0, separator))] =
            Trim(line.substr(separator + 1));
    }
    auto number = [&](const char* key, double fallback) {
        const auto it = values.find(key);
        return it == values.end() ? fallback : std::atof(it->second.c_str());
    };
    auto integer = [&](const char* key, int fallback) {
        const auto it = values.find(key);
        return it == values.end() ? fallback : std::atoi(it->second.c_str());
    };
    auto boolean = [&](const char* key, bool fallback) {
        const auto it = values.find(key);
        return it == values.end() ? fallback : IniBool(it->second, fallback);
    };
    auto longLong = [&](const char* key, long long fallback) {
        const auto it = values.find(key);
        return it == values.end() ? fallback : std::atoll(it->second.c_str());
    };

    options.chargeMassKg = number("chargeMassKg", options.chargeMassKg);
    options.heightOfBurstM =
        number("heightOfBurstM", options.heightOfBurstM);
    options.timeScale = number("timeScale", options.timeScale);
    options.fuseSeconds = number("fuseSeconds", options.fuseSeconds);
    options.windSpeed = number("windSpeedMs", options.windSpeed);
    options.ambientTemperature =
        number("ambientTemperatureK", options.ambientTemperature);
    options.particleBudget =
        integer("particleBudget", options.particleBudget);
    options.volumeResolution =
        integer("volumeResolution", options.volumeResolution);
    options.highFidelity =
        integer("simulationMode", options.highFidelity ? 1 : 0) == 1;
    options.solverResolution =
        std::clamp(integer("solverResolution", options.solverResolution), 32,
                   512);
    options.solverSteps =
        std::clamp(integer("solverStepsPerFrame", options.solverSteps), 8, 256);
    options.solverAutoScale =
        boolean("solverAutoScale", options.solverAutoScale);
    options.solverAutoScaleSet = true;
    options.solverHemisphere =
        boolean("solverHemisphere", options.solverHemisphere);
    options.flowAround = boolean("solverFlowAround", options.flowAround);
    options.bodyDrag = boolean("solverBodyDrag", options.bodyDrag);

    options.sceneEnabled = boolean("sceneEnabled", options.sceneEnabled);
    options.sceneEnabledSet = true;
    options.sceneSeed =
        uint32_t(std::clamp(integer("sceneSeed", int(options.sceneSeed)), 0,
                            1000000));
    options.sceneSeedSet = true;
    options.sceneDensity =
        float(number("sceneDensity", options.sceneDensity));
    options.sceneDensitySet = true;
    options.sceneBuildings =
        boolean("sceneBuildings", options.sceneBuildings);
    options.sceneVehicles = boolean("sceneVehicles", options.sceneVehicles);
    options.sceneVegetation =
        boolean("sceneVegetation", options.sceneVegetation);
    options.sceneVoronoiChunks =
        boolean("sceneVoronoiChunks", options.sceneVoronoiChunks);
    options.sceneQuality =
        std::clamp(integer("sceneQuality", options.sceneQuality), 0, 2);
    options.sceneChunkScale =
        float(std::clamp(number("sceneChunkScale", options.sceneChunkScale),
                         0.25, 2.0));

    options.raymarchSteps =
        std::clamp(integer("raymarchSteps", options.raymarchSteps), 16, 512);
    options.exposure =
        float(std::clamp(number("exposure", options.exposure), 0.01, 8.0));
    options.bloomStrength =
        float(std::clamp(number("bloomStrength", options.bloomStrength), 0.0,
                         4.0));
    options.volumeEnabled =
        boolean("enableVolume", options.volumeEnabled);
    options.particlesEnabled =
        boolean("enableParticles", options.particlesEnabled);
    options.groundEnabled = boolean("enableGround", options.groundEnabled);
    options.bloomEnabled = boolean("enableBloom", options.bloomEnabled);
    options.showGrid = boolean("showGrid", options.showGrid);
    options.sunAzimuth =
        float(number("sunAzimuthDeg", options.sunAzimuth));
    options.sunElevation =
        float(number("sunElevationDeg", options.sunElevation));
    options.particleScale =
        float(std::clamp(number("particleScale", options.particleScale), 0.0,
                         8.0));
    options.gpuParticles = boolean("gpuParticles", options.gpuParticles);
    options.gpuVolumeBake = boolean("gpuVolumeBake", options.gpuVolumeBake);
    options.checkForUpdates =
        boolean("checkForUpdates", options.checkForUpdates);
    const auto skipped = values.find("skippedVersion");
    if (skipped != values.end()) {
        options.skippedVersion = skipped->second;
    }
    options.lastCheckUnix =
        longLong("lastCheckUnix", options.lastCheckUnix);
}

const char* IniBoolText(bool value) {
    return value ? "true" : "false";
}

bool SaveSettingsFile(const std::string& path, const Options& options) {
    std::ofstream file(path);
    if (!file) {
        return false;
    }
    const double chargeMassKg =
        options.chargeMassKg > 0.0 ? options.chargeMassKg : 0.2;
    const double heightOfBurstM =
        options.heightOfBurstM >= 0.0 ? options.heightOfBurstM : 0.4;
    file << "# Explosion Simulator settings written by explosion_linux\n\n";
    file << "[simulation]\n";
    file << "chargeMassKg=" << chargeMassKg << "\n";
    file << "heightOfBurstM=" << heightOfBurstM << "\n";
    file << "ambientTemperatureK=" << options.ambientTemperature << "\n";
    file << "windSpeedMs=" << options.windSpeed << "\n";
    file << "timeScale=" << options.timeScale << "\n";
    file << "fuseSeconds=" << options.fuseSeconds << "\n";
    file << "simulationMode=" << (options.highFidelity ? 1 : 0) << "\n";
    file << "solverResolution="
         << (options.solverResolution > 0 ? options.solverResolution : 128)
         << "\n";
    file << "solverStepsPerFrame="
         << (options.solverSteps > 0 ? options.solverSteps : 96) << "\n";
    file << "solverAutoScale=" << IniBoolText(options.solverAutoScale) << "\n";
    file << "solverHemisphere=" << IniBoolText(options.solverHemisphere)
         << "\n";
    file << "solverFlowAround=" << IniBoolText(options.flowAround) << "\n";
    file << "solverBodyDrag=" << IniBoolText(options.bodyDrag) << "\n";
    file << "particleBudget=" << options.particleBudget << "\n";
    file << "gpuParticles=" << IniBoolText(options.gpuParticles) << "\n";
    file << "volumeResolution="
         << (options.volumeResolution > 0 ? options.volumeResolution : 128)
         << "\n";
    file << "gpuVolumeBake=" << IniBoolText(options.gpuVolumeBake) << "\n";
    file << "sceneEnabled=" << IniBoolText(options.sceneEnabled) << "\n";
    file << "sceneSeed=" << options.sceneSeed << "\n";
    file << "sceneDensity=" << options.sceneDensity << "\n";
    file << "sceneBuildings=" << IniBoolText(options.sceneBuildings) << "\n";
    file << "sceneVehicles=" << IniBoolText(options.sceneVehicles) << "\n";
    file << "sceneVegetation=" << IniBoolText(options.sceneVegetation) << "\n";
    file << "sceneVoronoiChunks=" << IniBoolText(options.sceneVoronoiChunks)
         << "\n";
    file << "sceneQuality=" << options.sceneQuality << "\n";
    file << "sceneChunkScale=" << options.sceneChunkScale << "\n\n";
    file << "[render]\n";
    file << "raymarchSteps=" << options.raymarchSteps << "\n";
    file << "exposure=" << options.exposure << "\n";
    file << "bloomStrength=" << options.bloomStrength << "\n";
    file << "enableVolume=" << IniBoolText(options.volumeEnabled) << "\n";
    file << "enableParticles=" << IniBoolText(options.particlesEnabled) << "\n";
    file << "enableGround=" << IniBoolText(options.groundEnabled) << "\n";
    file << "enableBloom=" << IniBoolText(options.bloomEnabled) << "\n";
    file << "showGrid=" << IniBoolText(options.showGrid) << "\n";
    file << "sunAzimuthDeg=" << options.sunAzimuth << "\n";
    file << "sunElevationDeg=" << options.sunElevation << "\n";
    file << "particleScale=" << options.particleScale << "\n\n";
    file << "[update]\n";
    file << "checkForUpdates=" << IniBoolText(options.checkForUpdates) << "\n";
    file << "skippedVersion=" << options.skippedVersion << "\n";
    file << "lastCheckUnix=" << options.lastCheckUnix << "\n";
    return file.good();
}

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            PrintUsage();
            std::exit(0);
        }
        if (std::strcmp(arg, "--version") == 0) {
            std::printf("%s\n", EXPLOSION_VERSION_STRING);
            std::exit(0);
        }
        if (std::strcmp(arg, "--settings") == 0 && i + 1 < argc) {
            ++i;
            continue;
        }
        if (std::strcmp(arg, "--no-flow-around") == 0) {
            options.flowAround = false;
            continue;
        }
        if (std::strcmp(arg, "--flow-around") == 0) {
            options.flowAround = true;
            continue;
        }
        if (std::strcmp(arg, "--no-body-drag") == 0) {
            options.bodyDrag = false;
            continue;
        }
        if (std::strcmp(arg, "--body-drag") == 0) {
            options.bodyDrag = true;
            continue;
        }
        if (std::strcmp(arg, "--high-fidelity") == 0) {
            options.highFidelity = true;
            continue;
        }
        if (std::strcmp(arg, "--reduced-order") == 0) {
            options.highFidelity = false;
            continue;
        }
        if (std::strcmp(arg, "--no-update-check") == 0) {
            options.noUpdateCheck = true;
            continue;
        }
        if (std::strcmp(arg, "--solver-resolution") == 0 && i + 1 < argc) {
            if (!ParseInt(argv[++i], options.solverResolution)) {
                std::fprintf(stderr, "Invalid solver resolution.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--solver-steps") == 0 && i + 1 < argc) {
            if (!ParseInt(argv[++i], options.solverSteps)) {
                std::fprintf(stderr, "Invalid solver step count.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--solver-time") == 0 && i + 1 < argc) {
            if (!ParseDouble(argv[++i], options.solverTime)) {
                std::fprintf(stderr, "Invalid solver time.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--no-solver-auto-scale") == 0) {
            options.solverAutoScale = false;
            options.solverAutoScaleSet = true;
            continue;
        }
        if (std::strcmp(arg, "--solver-benchmark") == 0) {
            options.solverBenchmark = true;
            options.highFidelity = true;
            options.headless = true;
            continue;
        }
        if (std::strcmp(arg, "--splash-test") == 0) {
            options.splashTest = true;
            continue;
        }
        if (std::strcmp(arg, "--benchmark") == 0) {
            options.benchmark = true;
            options.headless = true;
            continue;
        }
        if (std::strcmp(arg, "--splash-screenshot") == 0 && i + 1 < argc) {
            options.splashScreenshot = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "--write-settings") == 0 && i + 1 < argc) {
            options.writeSettings = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "--solver-step-count") == 0 && i + 1 < argc) {
            char* end = nullptr;
            const long long parsed = std::strtoll(argv[++i], &end, 10);
            if (!end || *end != '\0' || parsed <= 0) {
                std::fprintf(stderr, "Invalid solver step count.\n");
                return false;
            }
            options.solverStepCount = parsed;
            continue;
        }
        if (std::strcmp(arg, "--headless") == 0) {
            options.headless = true;
            continue;
        }
        if (std::strcmp(arg, "--debug") == 0) {
            options.debug = true;
            continue;
        }
        if (std::strcmp(arg, "--no-vsync") == 0) {
            options.vsync = false;
            continue;
        }
        if (std::strcmp(arg, "--screenshot") == 0 && i + 1 < argc) {
            options.screenshot = argv[++i];
            options.headless = true;
            continue;
        }
        if (std::strcmp(arg, "--width") == 0 && i + 1 < argc) {
            if (!ParseInt(argv[++i], options.width)) {
                std::fprintf(stderr, "Invalid width.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--height") == 0 && i + 1 < argc) {
            if (!ParseInt(argv[++i], options.height)) {
                std::fprintf(stderr, "Invalid height.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--frames") == 0 && i + 1 < argc) {
            if (!ParseInt(argv[++i], options.frames)) {
                std::fprintf(stderr, "Invalid frame count.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--time") == 0 && i + 1 < argc) {
            if (!ParseDouble(argv[++i], options.captureTime)) {
                std::fprintf(stderr, "Invalid capture time.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--charge") == 0 && i + 1 < argc) {
            if (!ParseDouble(argv[++i], options.chargeMassKg)) {
                std::fprintf(stderr, "Invalid charge mass.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--hob") == 0 && i + 1 < argc) {
            if (!ParseDouble(argv[++i], options.heightOfBurstM)) {
                std::fprintf(stderr, "Invalid burst height.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--azimuth") == 0 && i + 1 < argc) {
            if (!ParseFloat(argv[++i], options.azimuth)) {
                std::fprintf(stderr, "Invalid azimuth.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--elevation") == 0 && i + 1 < argc) {
            if (!ParseFloat(argv[++i], options.elevation)) {
                std::fprintf(stderr, "Invalid elevation.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--distance-scale") == 0 && i + 1 < argc) {
            if (!ParseFloat(argv[++i], options.distanceScale)) {
                std::fprintf(stderr, "Invalid distance scale.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--scene-seed") == 0 && i + 1 < argc) {
            if (!ParseSeed(argv[++i], options.sceneSeed)) {
                std::fprintf(stderr, "Invalid scene seed.\n");
                return false;
            }
            options.sceneSeedSet = true;
            continue;
        }
        if (std::strcmp(arg, "--scene-density") == 0 && i + 1 < argc) {
            if (!ParseFloat(argv[++i], options.sceneDensity)) {
                std::fprintf(stderr, "Invalid scene density.\n");
                return false;
            }
            options.sceneDensitySet = true;
            continue;
        }
        if (std::strcmp(arg, "--no-scene") == 0) {
            options.sceneEnabled = false;
            options.sceneEnabledSet = true;
            continue;
        }
        if (std::strcmp(arg, "--volume-resolution") == 0 && i + 1 < argc) {
            if (!ParseInt(argv[++i], options.volumeResolution)) {
                std::fprintf(stderr, "Invalid volume resolution.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--raymarch-steps") == 0 && i + 1 < argc) {
            if (!ParseInt(argv[++i], options.raymarchSteps)) {
                std::fprintf(stderr, "Invalid raymarch step count.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--exposure") == 0 && i + 1 < argc) {
            if (!ParseFloat(argv[++i], options.exposure)) {
                std::fprintf(stderr, "Invalid exposure.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--bloom-strength") == 0 && i + 1 < argc) {
            if (!ParseFloat(argv[++i], options.bloomStrength)) {
                std::fprintf(stderr, "Invalid bloom strength.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--particle-scale") == 0 && i + 1 < argc) {
            if (!ParseFloat(argv[++i], options.particleScale)) {
                std::fprintf(stderr, "Invalid particle scale.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--sun-azimuth") == 0 && i + 1 < argc) {
            if (!ParseFloat(argv[++i], options.sunAzimuth)) {
                std::fprintf(stderr, "Invalid sun azimuth.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--sun-elevation") == 0 && i + 1 < argc) {
            if (!ParseFloat(argv[++i], options.sunElevation)) {
                std::fprintf(stderr, "Invalid sun elevation.\n");
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--no-bloom") == 0) {
            options.bloomEnabled = false;
            continue;
        }
        if (std::strcmp(arg, "--no-ground") == 0) {
            options.groundEnabled = false;
            continue;
        }
        if (std::strcmp(arg, "--no-volume") == 0) {
            options.volumeEnabled = false;
            continue;
        }
        if (std::strcmp(arg, "--no-particles") == 0) {
            options.particlesEnabled = false;
            continue;
        }
        if (std::strcmp(arg, "--no-gpu-volume-bake") == 0) {
            options.gpuVolumeBake = false;
            continue;
        }
        if (std::strcmp(arg, "--no-gpu-particles") == 0) {
            options.gpuParticles = false;
            continue;
        }
        std::fprintf(stderr, "Unknown option: %s\n", arg);
        return false;
    }
    return true;
}

double NowSeconds() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

bool CreateVulkanSurface(void* user, void* instance, uint64_t* surface) {
    auto* window = static_cast<ex::platform::Window*>(user);
    return window && window->CreateVulkanSurface(instance, surface);
}

void FrameCamera(const ex::sim::Simulation& simulation, ex::gfx::Camera& camera,
                 const Options& options,
                 const ex::gfx::SolverStats* solverStats) {
    const ex::sim::BlastModel& blast = simulation.Blast();
    const ex::sim::BlastState& state = blast.State();
    camera.SetViewport(options.width, options.height);
    camera.SetClipPlanes(0.5f, 20000.0f);

    if (solverStats) {
        // Match the Windows high-fidelity framing: follow the measured plume
        // instead of the analytic cloud, which runs ahead of the resolved head.
        const float sceneExtent = simulation.Scene().Extent();
        float centerY = float(state.cloudCenterY);
        float top = 0.0f;
        float half = 0.0f;
        if (solverStats->active && solverStats->fieldBoundsValid &&
            solverStats->plumeTop > 1e-3f) {
            top = solverStats->plumeTop;
            half = std::max(solverStats->plumeRadius, 0.5f * top);
            centerY = 0.5f * top;
        } else {
            const float fireball = float(blast.FireballMaxRadiusM());
            const float cloudRadius =
                std::max(float(state.cloudRadius), 3.0f * fireball);
            top = float(state.cloudCenterY) + 2.4f * cloudRadius;
            half = std::max(cloudRadius * 1.6f, 3.0f * fireball);
            top = std::max(top, 2.0f * half);
        }
        const float distance =
            std::max({top * 1.15f, half * 2.6f, sceneExtent * 0.9f, 8.0f}) *
            std::max(0.1f, options.distanceScale);
        const ex::sim::Environment& environment = blast.GetEnvironment();
        const double drift = solverStats->active ? solverStats->time : 0.0;
        camera.SetTarget(
            DirectX::XMFLOAT3(float(environment.WindX() * drift), centerY,
                              float(environment.WindZ() * drift)),
            distance);
        camera.SetOrientation(options.azimuth, options.elevation);
        camera.Snap();
        return;
    }

    const float fireball = float(blast.FireballMaxRadiusM());
    const float cloud = float(state.cloudRadius);
    const float burstHeight = float(blast.GetCharge().heightOfBurstM);
    const float sceneExtent = simulation.Scene().Extent();
    const float radius =
        std::max({fireball * 3.2f, cloud * 2.4f, burstHeight * 1.6f,
                  sceneExtent * 0.8f, 1.2f});
    const float centerY = burstHeight + fireball * 0.6f;
    camera.SetTarget(DirectX::XMFLOAT3(0.0f, centerY, 0.0f),
                     radius * 1.9f * std::max(0.1f, options.distanceScale));
    camera.SetOrientation(options.azimuth, options.elevation);
    camera.Snap();
}

DirectX::XMFLOAT3 SunDirectionFor(const Options& options) {
    const float azimuth = DirectX::XMConvertToRadians(options.sunAzimuth);
    const float elevation = DirectX::XMConvertToRadians(options.sunElevation);
    return DirectX::XMFLOAT3(std::cos(elevation) * std::sin(azimuth),
                             std::sin(elevation),
                             std::cos(elevation) * std::cos(azimuth));
}

ex::gfx::EulerConfig MakeEulerConfig(const ex::sim::Simulation& simulation) {
    const ex::sim::BlastModel& blast = simulation.Blast();
    const ex::sim::Charge& charge = blast.GetCharge();
    const ex::sim::Environment& environment = blast.GetEnvironment();

    ex::gfx::EulerConfig config;
    config.resolution =
        std::clamp(simulation.Settings().solverResolution, 32, 512);
    const double fireball = std::max(blast.FireballMaxRadiusM(), 0.1);
    config.domainHalfExtent = std::clamp(
        ex::gfx::SolverHalfExtentForBurst(charge.heightOfBurstM, fireball), 1.0,
        3000.0);
    config.hemispherical = simulation.Settings().solverHemisphere;
    config.chargeMassKg = std::max(charge.tntMassKg, 1e-6);
    config.chargeEnergyJ = charge.EnergyJ();
    config.heightOfBurstM =
        std::clamp(charge.heightOfBurstM, 0.05, 2.0 * config.domainHalfExtent - 0.05);
    config.ambientDensity = environment.AirDensity();
    config.ambientPressure = environment.pressurePa;
    config.ambientTemperature = environment.VirtualTemperatureK();
    config.gamma = environment.gamma;
    config.gasConstant = environment.gasConstant;
    config.gravity = environment.gravity;
    config.fireballRadiusM = fireball;
    return config;
}

void UpdatePlumeModel(ex::gfx::RhiEulerSolver& solver,
                      const ex::sim::Simulation& simulation) {
    const ex::sim::BlastState& state = simulation.Blast().State();
    const float cloudRadius = float(state.cloudRadius);
    const float radius = 0.62f * cloudRadius;
    const float centerY = float(state.cloudCenterY);
    const float capTop = centerY + 0.52f * cloudRadius;
    solver.SetPlumeModel(radius, centerY, capTop, cloudRadius,
                         float(state.cloudRiseSpeed));
}

void MaybeExpandSolver(ex::gfx::RhiEulerSolver& solver,
                       const ex::sim::Simulation& simulation,
                       const Options& options, bool& capWarned) {
    if (!options.solverAutoScale || !solver.HasVolume()) {
        return;
    }

    constexpr double kTriggerFraction = 0.78;
    constexpr double kFieldMargin = 1.12;
    constexpr float kExpansion = 1.3f;
    constexpr float kMaxAccuracy = 16.0f;

    const ex::sim::BlastState& state = simulation.Blast().State();
    const double analyticHalf = std::max(
        {state.fireballRadius * 2.4, state.cloudRadius * 1.35, 0.5});
    const double analyticTop = std::max(
        {state.fireballCenterY + state.fireballRadius * 2.0,
         state.cloudCenterY + state.cloudRadius * 1.6, 1.0});

    double requiredHalf = analyticHalf;
    double requiredTop = analyticTop;
    double measuredDome = 0.0;
    if (solver.HasFieldBounds()) {
        const double measuredRadius = double(solver.FieldRadius());
        const double measuredTop = double(solver.FieldTop());
        if (measuredRadius > 0.0) {
            requiredHalf = std::max(measuredRadius * kFieldMargin, 0.5);
        }
        if (measuredTop > 0.0) {
            requiredTop = std::max(measuredTop * kFieldMargin, 1.0);
        }
        measuredDome = double(solver.FieldDomeFraction());
    }
    const bool hemisphere = simulation.Settings().solverHemisphere;

    for (int attempt = 0; attempt < 4; ++attempt) {
        const ex::sim::VolumeBounds& bounds = solver.Bounds();
        const double half = 0.5 * (bounds.max.x - bounds.min.x);
        const double top = bounds.max.y;
        const bool tooWide = requiredHalf > half * kTriggerFraction;
        const bool tooTall = requiredTop > top * kTriggerFraction;
        const bool tooDome = hemisphere && measuredDome > kTriggerFraction;
        if (!tooWide && !tooTall && !tooDome) {
            break;
        }
        if (solver.Stats().accuracyFactor >= kMaxAccuracy) {
            if (!capWarned) {
                ex::LogWarn("High-fidelity auto-scale reached the accuracy budget "
                            "(x%.1f); the plume may leave the domain.",
                            solver.Stats().accuracyFactor);
                capWarned = true;
            }
            break;
        }
        if (!solver.RescaleDomain(kExpansion)) {
            break;
        }
    }
}

// Owns the RHI solver and the per-blast bookkeeping used by the Linux shell.
struct HighFidelityRuntime {
    ex::gfx::RhiEulerSolver solver;
    uint64_t blastSerial = 0;
    uint64_t version = 0;
    bool capWarned = false;
    bool published = false;
    int obstacleFrame = 0;
    std::vector<ex::sim::FlowBody> flowBodies;

    void BeginFrame(const ex::sim::Simulation& simulation) {
        if (simulation.Settings().solverBodyDrag) {
            flowBodies = simulation.CollectFlowBodies(solver.Bounds());
        } else {
            flowBodies.clear();
        }
        solver.SetFlowBodies(flowBodies);
    }

    void EndFrame(ex::sim::Simulation& simulation, double advance) {
        if (flowBodies.empty() || advance <= 0.0) {
            return;
        }
        std::vector<ex::sim::BodyFlowSample> samples;
        solver.ConsumeBodyFlow(samples);
        simulation.ApplyBodyDrag(flowBodies, samples, float(advance));
    }

    void UpdateObstacles(const ex::sim::Simulation& simulation) {
        solver.SetObstacles(
            simulation.Settings().solverFlowAround
                ? simulation.CollectSolverObstacles(solver.Bounds())
                : std::vector<ex::sim::ObstacleBox>{});
    }

    bool Ensure(const ex::sim::Simulation& simulation) {
        if (!solver.HasVolume() || blastSerial != simulation.BlastSerial()) {
            if (!solver.Reset(MakeEulerConfig(simulation))) {
                ex::LogError("High-fidelity solver could not be initialised.");
                return false;
            }
            UpdateObstacles(simulation);
            blastSerial = simulation.BlastSerial();
            capWarned = false;
        }
        // The wall mask follows the debris; refresh it every other frame. The
        // solver skips the upload when nothing moved.
        if (++obstacleFrame >= 2) {
            obstacleFrame = 0;
            UpdateObstacles(simulation);
        }
        return solver.HasVolume();
    }

    void Publish(ex::app::LinuxPreview& preview, const Options& options) {
        solver.UpdateRenderVolume();
        solver.UpdateLightVolume(SunDirectionFor(options));
        solver.TransitionToShaderRead();
        preview.SetGpuVolume(solver.RenderTexture(), solver.Bounds(),
                             solver.ExtinctionScale(), ++version);
        preview.SetGpuLightVolume(solver.LightTexture(), solver.Bounds());
        published = true;
    }
};

bool WritePpm(const std::string& path, const std::vector<uint8_t>& bgra,
              int width, int height) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file << "P6\n" << width << " " << height << "\n255\n";
    for (int i = 0; i < width * height; ++i) {
        const size_t index = size_t(i) * 4;
        file.put(char(bgra[index + 2]));
        file.put(char(bgra[index + 1]));
        file.put(char(bgra[index + 0]));
    }
    return file.good();
}

void LinuxUiEventCallback(void* user, const void* event) {
    static_cast<ex::app::LinuxUi*>(user)->ProcessEvent(event);
}

bool WriteImage(const std::string& path, const std::vector<uint8_t>& bgra,
                int width, int height) {
    const bool ppm = path.size() >= 4 &&
                     (path.compare(path.size() - 4, 4, ".ppm") == 0 ||
                      path.compare(path.size() - 4, 4, ".PPM") == 0);
    return ppm ? WritePpm(path, bgra, width, height)
               : ex::gfx::SavePng(path, bgra.data(), width, height);
}

std::filesystem::path FindAssetDirectory(const char* argv0) {
    std::error_code error;
    const std::filesystem::path executable =
        std::filesystem::weakly_canonical(std::filesystem::path(argv0), error);
    std::vector<std::filesystem::path> candidates;
    if (!error) {
        candidates.push_back(executable.parent_path() / "assets");
    }
    candidates.push_back(std::filesystem::current_path() / "assets");
    candidates.push_back(std::filesystem::path("assets"));
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate / "splash")) {
            return candidate;
        }
    }
    return {};
}

std::string CacheDirectory() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        return std::string(xdg) + "/explosionsim";
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::string(home) + "/.cache/explosionsim";
    }
    return "/tmp/explosionsim";
}

std::vector<std::string> SplashFrames(const std::filesystem::path& assets) {
    std::vector<std::string> frames;
    frames.reserve(20);
    for (int i = 0; i < 20; ++i) {
        char name[32] = {};
        std::snprintf(name, sizeof(name), "frame_%02d.jpg", i);
        frames.push_back((assets / "splash" / name).string());
    }
    return frames;
}

} // namespace

int main(int argc, char** argv) {
    Options options{};
    std::string settingsPath;
    bool settingsExplicit = false;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--settings") == 0) {
            settingsPath = argv[i + 1];
            settingsExplicit = true;
            break;
        }
    }
    if (settingsPath.empty()) {
        const char* home = std::getenv("HOME");
        const std::filesystem::path candidates[] = {
            std::filesystem::path("settings.ini"),
            std::filesystem::path(argv[0]).parent_path() / "settings.ini",
            ex::app::UserSettingsPath(false),
            home ? std::filesystem::path(home) / ".config" /
                       "ExplosionSim" / "settings.ini"
                 : std::filesystem::path(),
        };
        for (const auto& candidate : candidates) {
            if (!candidate.empty() &&
                std::filesystem::exists(candidate)) {
                settingsPath = candidate.string();
                break;
            }
        }
    }
    if (!settingsPath.empty()) {
        if (std::filesystem::exists(settingsPath)) {
            LoadSettingsFile(settingsPath, options);
            ex::LogInfo("Loaded settings: %s", settingsPath.c_str());
        } else {
            ex::LogWarn("Settings file not found: %s", settingsPath.c_str());
        }
    }
    if (!ParseOptions(argc, argv, options)) {
        PrintUsage();
        return 2;
    }

    if (options.benchmark && options.frames <= 0) {
        options.frames = 60;
    }

    if (!options.writeSettings.empty()) {
        if (!SaveSettingsFile(options.writeSettings, options)) {
            ex::LogError("Could not write settings: %s",
                         options.writeSettings.c_str());
            return 1;
        }
        ex::LogInfo("Wrote settings: %s", options.writeSettings.c_str());
        return 0;
    }

    if (options.splashTest) {
        ex::app::SplashSdl splash;
        if (!splash.Show(SplashFrames(FindAssetDirectory(argv[0])),
                         "Explosion Simulator", "Splash test")) {
            return 1;
        }
        splash.SetProgress(1.0f, "Loading assets...", 0.4f);
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - start)
                   .count() < 0.8) {
            splash.Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        if (!options.splashScreenshot.empty()) {
            std::vector<uint8_t> pixels;
            int width = 0;
            int height = 0;
            if (splash.Capture(pixels, width, height)) {
                ex::gfx::SavePng(options.splashScreenshot, pixels.data(), width,
                                 height);
            }
        }
        splash.Close();
        return 0;
    }
    std::unique_ptr<ex::app::SplashSdl> splash;
    bool splashVisible = false;
    const auto splashStart = std::chrono::steady_clock::now();
    if (!options.headless) {
        splash = std::make_unique<ex::app::SplashSdl>();
        splashVisible =
            splash->Show(SplashFrames(FindAssetDirectory(argv[0])),
                         "Explosion Simulator", "Native Linux / Vulkan");
        if (splashVisible) {
            splash->SetProgress(0.02f, "Initializing SDL3...", 0.10f);
            splash->Pump();
        }
    }
    auto pumpSplash = [&](float progress, const char* status, float expected) {
        if (splashVisible) {
            splash->SetProgress(progress, status, expected);
            splash->Pump();
        }
    };

    std::unique_ptr<ex::platform::Window> window;
    ex::rhi::DeviceDesc desc{};
    desc.backend = ex::rhi::Backend::Vulkan;
    desc.debug = options.debug;
    desc.width = options.width;
    desc.height = options.height;

    if (options.headless) {
        desc.offscreen = true;
    } else {
        ex::platform::WindowDesc windowDesc{};
        windowDesc.title = "Explosion Simulator";
        windowDesc.width = options.width;
        windowDesc.height = options.height;
        window = ex::platform::CreatePlatformWindow(windowDesc);
        if (!window) {
            ex::LogError("Could not create the SDL3 window.");
            return 1;
        }
        uint32_t extensionCount = 0;
        const char* const* extensions =
            window->VulkanInstanceExtensions(extensionCount);
        desc.window = window->NativeHandle();
        desc.offscreen = false;
        desc.surface.instanceExtensions = extensions;
        desc.surface.instanceExtensionCount = extensionCount;
        desc.surface.user = window.get();
        desc.surface.create = &CreateVulkanSurface;
    }
    pumpSplash(0.12f, "Creating the window...", 0.10f);

    std::unique_ptr<ex::rhi::Device> device = ex::rhi::CreateVulkanDevice(desc);
    if (!device) {
        ex::LogError("Could not create the Vulkan device.");
        return 1;
    }
    pumpSplash(0.30f, "Initializing Vulkan...", 0.20f);

    HighFidelityRuntime highFidelityRuntime;
    bool solverReady = false;
    bool highFidelity = false;
    if (options.highFidelity || !options.headless) {
        ex::gfx::RhiEulerSolver::Shaders eulerShaders{
            {kEulerCSInit, kEulerCSInitSize},
            {kEulerCSSweepX, kEulerCSSweepXSize},
            {kEulerCSSweepY, kEulerCSSweepYSize},
            {kEulerCSSweepZ, kEulerCSSweepZSize},
            {kEulerCSSweepXSource, kEulerCSSweepXSourceSize},
            {kEulerCSSweepYSource, kEulerCSSweepYSourceSize},
            {kEulerCSSweepZSource, kEulerCSSweepZSourceSize},
            {kEulerCSReduce, kEulerCSReduceSize},
            {kEulerCSRender, kEulerCSRenderSize},
            {kEulerCSLight, kEulerCSLightSize},
            {kEulerCSRescale, kEulerCSRescaleSize},
            {kEulerCSBodyFlow, kEulerCSBodyFlowSize},
        };
        solverReady =
            highFidelityRuntime.solver.Initialize(device.get(), eulerShaders);
        highFidelity = options.highFidelity && solverReady;
        if (options.highFidelity && !solverReady) {
            ex::LogWarn("High-fidelity solver unavailable; using the "
                        "reduced-order path.");
        }
    }

    ex::gfx::GpuVolumeBaker gpuVolume;
    bool gpuVolumeReady =
        options.gpuVolumeBake &&
        device->GetBackend() == ex::rhi::Backend::Vulkan &&
        gpuVolume.Initialize(device.get(), kVolumeBakeCS, kVolumeBakeCSSize);
    if (!gpuVolumeReady) {
        ex::LogWarn("GPU volume bake unavailable; using the CPU baker.");
    }

    ex::gfx::GpuParticleSystem gpuParticles;
    const int particleCapacity =
        std::clamp(options.particleBudget, 1024, 200000);
    ex::gfx::GpuParticleSystem::ShaderSource particleApply{
        kParticlesCSApply, kParticlesCSApplySize};
    ex::gfx::GpuParticleSystem::ShaderSource particleReset{
        kParticlesCSReset, kParticlesCSResetSize};
    ex::gfx::GpuParticleSystem::ShaderSource particleIntegrate{
        kParticlesCSIntegrate, kParticlesCSIntegrateSize};
    ex::gfx::GpuParticleSystem::ShaderSource particleFinalize{
        kParticlesCSFinalize, kParticlesCSFinalizeSize};
    bool gpuParticlesReady =
        options.gpuParticles &&
        device->GetBackend() == ex::rhi::Backend::Vulkan &&
        gpuParticles.Initialize(device.get(), particleCapacity, particleApply,
                                particleReset, particleIntegrate,
                                particleFinalize);
    if (!gpuParticlesReady) {
        ex::LogWarn("GPU particles unavailable; using the CPU particle path.");
    }
    pumpSplash(0.50f, "Compiling compute pipelines...", 0.25f);

    ex::core::ThreadPool pool;
    ex::sim::SimulationSettings simulationSettings{};
    simulationSettings.chargeMassKg =
        options.chargeMassKg > 0.0 ? options.chargeMassKg : 0.2;
    simulationSettings.heightOfBurstM =
        options.heightOfBurstM >= 0.0 ? options.heightOfBurstM : 0.4;
    simulationSettings.mode = highFidelity
                                  ? ex::sim::SimulationMode::HighFidelity
                                  : ex::sim::SimulationMode::ReducedOrder;
    simulationSettings.solverResolution =
        std::clamp(options.solverResolution > 0 ? options.solverResolution : 128,
                   32, 512);
    simulationSettings.solverStepsPerFrame =
        std::clamp(options.solverSteps > 0 ? options.solverSteps : 96, 8, 256);
    simulationSettings.solverAutoScale = options.solverAutoScale;
    simulationSettings.solverHemisphere = options.solverHemisphere;
    simulationSettings.solverFlowAround = options.flowAround;
    simulationSettings.solverBodyDrag = options.bodyDrag;
    simulationSettings.gpuParticles = gpuParticlesReady;
    simulationSettings.gpuVolumeBake = gpuVolumeReady;
    simulationSettings.timeScale = options.timeScale;
    simulationSettings.fuseSeconds =
        options.headless ? 0.0 : options.fuseSeconds;
    simulationSettings.windSpeedMs = options.windSpeed;
    simulationSettings.ambientTemperatureK = options.ambientTemperature;
    simulationSettings.particleBudget =
        std::clamp(options.particleBudget, 1024, 200000);
    simulationSettings.volumeResolution =
        options.volumeResolution > 0
            ? std::clamp(options.volumeResolution, 32, 512)
            : 128;
    simulationSettings.scene.seed = options.sceneSeed;
    simulationSettings.scene.density =
        std::clamp(options.sceneDensity, 0.0f, 4.0f);
    simulationSettings.scene.enabled = options.sceneEnabled;
    simulationSettings.scene.quality =
        std::clamp(options.sceneQuality, 0, 2);
    simulationSettings.scene.chunkScale =
        std::clamp(options.sceneChunkScale, 0.25f, 2.0f);
    simulationSettings.scene.voronoiChunks = options.sceneVoronoiChunks;
    simulationSettings.scene.buildings = options.sceneBuildings;
    simulationSettings.scene.vehicles = options.sceneVehicles;
    simulationSettings.scene.vegetation = options.sceneVegetation;

    ex::sim::Simulation simulation;
    if (!simulation.Initialize(simulationSettings, pool, false)) {
        ex::LogError("Could not initialize the simulation.");
        return 1;
    }
    pumpSplash(0.70f, "Generating the scene...", 0.30f);

    ex::gfx::Camera camera;
    camera.SetViewport(options.width, options.height);
    camera.SetClipPlanes(0.5f, 20000.0f);

    ex::app::LinuxPreview preview;
    ex::app::LinuxPreview::Settings previewSettings{};
    previewSettings.raymarchSteps = options.raymarchSteps;
    previewSettings.exposure = options.exposure;
    previewSettings.bloomStrength = options.bloomStrength;
    previewSettings.enableVolume = options.volumeEnabled;
    previewSettings.enableParticles = options.particlesEnabled;
    previewSettings.enableGround = options.groundEnabled;
    previewSettings.enableBloom = options.bloomEnabled;
    previewSettings.showGrid = options.showGrid;
    previewSettings.sunAzimuthDeg = options.sunAzimuth;
    previewSettings.sunElevationDeg = options.sunElevation;
    previewSettings.particleScale = options.particleScale;
    preview.SetSettings(previewSettings);
    preview.SetHighFidelity(highFidelity);
    if (!preview.Initialize(device.get(), device->Width(), device->Height())) {
        ex::LogError("Could not initialize the reduced-order preview.");
        return 1;
    }
    if (gpuParticlesReady) {
        preview.SetGpuParticles(gpuParticles.AdditiveBuffer(),
                                gpuParticles.AlphaBuffer(),
                                gpuParticles.FireBuffer(),
                                gpuParticles.IndirectBuffer());
    }
    pumpSplash(0.92f, "Preparing the renderer...", 0.25f);
    ex::LogInfo("Linux app ready: %dx%d, %s", device->Width(),
                device->Height(), options.headless ? "offscreen" : "windowed");

    if (options.solverBenchmark) {
        if (!highFidelity) {
            ex::LogError("Solver benchmark requires the high-fidelity solver.");
            return 1;
        }
        const long long steps =
            options.solverStepCount > 0 ? options.solverStepCount : 256;
        const int stepsPerFrame = std::clamp(
            options.solverSteps > 0 ? options.solverSteps
                                    : simulation.Settings().solverStepsPerFrame,
            1, 256);
        highFidelityRuntime.solver.SetDeterministic(false);
        if (!highFidelityRuntime.Ensure(simulation)) {
            ex::LogError("Solver benchmark: solver could not be initialised.");
            return 1;
        }
        auto runSteps = [&](long long count) {
            long long done = 0;
            while (done < count) {
                const int batch =
                    int(std::min<long long>(stepsPerFrame, count - done));
                UpdatePlumeModel(highFidelityRuntime.solver, simulation);
                highFidelityRuntime.solver.Step(batch);
                highFidelityRuntime.solver.UpdateRenderVolume();
                highFidelityRuntime.solver.WaitForReduction();
                const double advance =
                    highFidelityRuntime.solver.ConsumeAdvance();
                if (advance > 0.0) {
                    simulation.AdvanceExternal(advance);
                }
                done += batch;
            }
        };
        // Warm-up lets the driver/GPU settle before the measured window.
        runSteps(std::min<long long>(steps, std::max(8, stepsPerFrame * 2)));
        const int frames = int((steps + stepsPerFrame - 1) / stepsPerFrame);
        const bool timed = highFidelityRuntime.solver.BeginTiming(
            int(steps * 5 + frames * 3 + 16));
        const auto start = std::chrono::steady_clock::now();
        runSteps(steps);
        const double elapsed = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - start)
                                   .count();
        const ex::gfx::SolverTiming timing =
            highFidelityRuntime.solver.ResolveTiming();
        ex::LogInfo("Solver benchmark: %d^3, %lld steps in %d-step frames, "
                    "domain half %.2f m, voxel %.1f mm, maxSpeed %.0f m/s",
                    highFidelityRuntime.solver.Stats().resolution, steps,
                    stepsPerFrame,
                    highFidelityRuntime.solver.Stats().domainHalfExtent,
                    highFidelityRuntime.solver.Stats().voxelSize * 1000.0f,
                    highFidelityRuntime.solver.Stats().maxSpeed);
        if (timing.valid) {
            const auto passMs = [&timing](ex::gfx::SolverPass pass) {
                return timing.passMs[int(pass)];
            };
            const auto passCount = [&timing](ex::gfx::SolverPass pass) {
                return timing.passCount[int(pass)];
            };
            const double sweepMs = passMs(ex::gfx::SolverPass::SweepX) +
                                   passMs(ex::gfx::SolverPass::SweepY) +
                                   passMs(ex::gfx::SolverPass::SweepZ);
            ex::LogInfo("  GPU total %.2f ms (%.3f ms/step, %.3f ms/frame), "
                        "wall %.2f ms",
                        timing.totalMs, timing.totalMs / double(steps),
                        timing.totalMs / double(frames), elapsed * 1000.0);
            ex::LogInfo("  sweeps %.2f ms (X %.2f, Y %.2f, Z %.2f), reduce "
                        "%.2f ms (%d passes), render %.2f ms (%d passes)",
                        sweepMs, passMs(ex::gfx::SolverPass::SweepX),
                        passMs(ex::gfx::SolverPass::SweepY),
                        passMs(ex::gfx::SolverPass::SweepZ),
                        passMs(ex::gfx::SolverPass::Reduce),
                        passCount(ex::gfx::SolverPass::Reduce),
                        passMs(ex::gfx::SolverPass::Render),
                        passCount(ex::gfx::SolverPass::Render));
        } else {
            ex::LogWarn("Solver benchmark: GPU timestamps unavailable "
                        "(begin=%d); reporting wall time only (%.3f ms/step).",
                        timed ? 1 : 0, elapsed * 1000.0 / double(steps));
        }
        return 0;
    }

    if (options.headless) {
        const double targetTime = std::max(0.0, options.captureTime);
        if (highFidelity) {
            const double solverTarget =
                options.solverTime >= 0.0 ? options.solverTime : targetTime;
            highFidelityRuntime.solver.SetDeterministic(true);
            if (!highFidelityRuntime.Ensure(simulation)) {
                ex::LogError("High-fidelity solver reset failed.");
                return 1;
            }
            const auto start = std::chrono::steady_clock::now();
            int guard = 0;
            while (highFidelityRuntime.solver.Stats().time < solverTarget &&
                   guard++ < 200000) {
                MaybeExpandSolver(highFidelityRuntime.solver, simulation,
                                  options, highFidelityRuntime.capWarned);
                highFidelityRuntime.UpdateObstacles(simulation);
                highFidelityRuntime.BeginFrame(simulation);
                UpdatePlumeModel(highFidelityRuntime.solver, simulation);
                highFidelityRuntime.solver.Step(16);
                const double advance =
                    highFidelityRuntime.solver.ConsumeAdvance();
                if (advance > 0.0) {
                    simulation.AdvanceExternal(advance);
                }
                highFidelityRuntime.EndFrame(simulation, advance);
                if (gpuParticlesReady) {
                    gpuParticles.Update(simulation.Particles());
                }
                const double elapsed = std::chrono::duration<double>(
                                           std::chrono::steady_clock::now() -
                                           start)
                                           .count();
                if (elapsed > 600.0) {
                    ex::LogWarn("High-fidelity solver stopped at %.4f s "
                                "(time budget reached).",
                                highFidelityRuntime.solver.Stats().time);
                    break;
                }
            }
            highFidelityRuntime.Publish(preview, options);
            ex::LogInfo("High-fidelity: t=%.4f s (analytic %.4f s), %lld steps, "
                        "%d^3, dt=%.2f us, maxSpeed=%.0f m/s",
                        highFidelityRuntime.solver.Stats().time,
                        simulation.Blast().State().time,
                        highFidelityRuntime.solver.Stats().steps,
                        highFidelityRuntime.solver.Stats().resolution,
                        highFidelityRuntime.solver.Stats().dt * 1e6f,
                        highFidelityRuntime.solver.Stats().maxSpeed);
        } else {
            const double step = 1.0 / 120.0;
            int guard = 0;
            while (simulation.Time() + 1e-9 < targetTime && guard++ < 200000) {
                simulation.StepFixed(
                    std::min(step, targetTime - simulation.Time()));
                if (gpuParticlesReady) {
                    gpuParticles.Update(simulation.Particles());
                }
            }
            simulation.Volume().WaitForIdle();
            simulation.Volume().Poll();
            simulation.Volume().BakeNow(simulation.Blast());
            simulation.Volume().Poll();
            if (gpuVolumeReady) {
                ex::sim::VolumeBakeRequest request;
                if (simulation.Volume().TakeGpuRequest(request) &&
                    gpuVolume.Bake(request)) {
                    preview.SetGpuVolume(gpuVolume.Texture(), gpuVolume.Bounds(),
                                         gpuVolume.Extinction(),
                                         gpuVolume.Version());
                } else {
                    ex::LogWarn("GPU volume bake failed; using the CPU baker.");
                    gpuVolumeReady = false;
                    simulation.Volume().SetGpuBake(false);
                    simulation.Volume().BakeNow(simulation.Blast());
                    simulation.Volume().Poll();
                }
            }
        }
    }
    if (splashVisible) {
        splash->SetProgress(1.0f, "Ready", 0.15f);
        while (std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - splashStart)
                   .count() < 1.0) {
            splash->Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        // The splash is always-on-top; raise the application window as the
        // splash closes so it keeps the same z-level instead of dropping
        // behind other applications.
        if (window) {
            window->Raise();
        }
        splash->Close();
        if (window) {
            window->Raise();
        }
        splashVisible = false;
    }

    ex::rhi::vulkan::Device* vkDevice =
        dynamic_cast<ex::rhi::vulkan::Device*>(device.get());
    ex::app::LinuxUi linuxUi;
    ex::app::LinuxUiState uiState{};
    bool uiReady = false;
    if (!options.headless && window && vkDevice) {
        ex::app::LinuxUiHandles handles{};
        handles.instance = reinterpret_cast<void*>(vkDevice->Instance());
        handles.physicalDevice =
            reinterpret_cast<void*>(vkDevice->PhysicalDevice());
        handles.device = reinterpret_cast<void*>(vkDevice->DeviceHandle());
        handles.queue = reinterpret_cast<void*>(vkDevice->Queue());
        handles.queueFamily = vkDevice->QueueFamily();
        handles.renderPass =
            reinterpret_cast<void*>(vkDevice->ActiveRenderPass());
        handles.pipelineCache =
            reinterpret_cast<void*>(vkDevice->PipelineCache());
        handles.imageCount = vkDevice->SwapchainImageCount();
        uiReady = linuxUi.Initialize(window->NativeHandle(), handles);
        if (uiReady) {
            window->SetEventCallback(&LinuxUiEventCallback, &linuxUi);
            preview.SetOverlayCallback([&linuxUi, vkDevice]() {
                linuxUi.Render(reinterpret_cast<void*>(
                    vkDevice->ActiveCommandBuffer()));
            });
            uiState.exposure = options.exposure;
            uiState.bloomStrength = options.bloomStrength;
            uiState.raymarchSteps = options.raymarchSteps;
            uiState.enableVolume = options.volumeEnabled;
            uiState.enableParticles = options.particlesEnabled;
            uiState.enableGround = options.groundEnabled;
            uiState.enableBloom = options.bloomEnabled;
            uiState.showGrid = options.showGrid;
            uiState.chargeMassKg = simulation.Settings().chargeMassKg;
            uiState.heightOfBurstM = simulation.Settings().heightOfBurstM;
            uiState.timeScale = simulation.Settings().timeScale;
            uiState.highFidelity = highFidelity;
            uiState.flowAround = options.flowAround;
            uiState.bodyDrag = options.bodyDrag;
            uiState.solverResolution = simulation.Settings().solverResolution;
            uiState.solverAvailable = solverReady;
            uiState.updateCheckEnabled = options.checkForUpdates;
        }
    }

    // The updater is a separate helper binary next to the application; it owns
    // the network, archive and install work.
    ex::app::LinuxUpdater updater;
    if (uiReady) {
        std::error_code ec;
        const std::filesystem::path executable =
            std::filesystem::canonical(argv[0], ec);
        const std::filesystem::path installDirectory =
            (ec ? std::filesystem::absolute(argv[0]) : executable)
                .parent_path();
        const std::filesystem::path helper =
            installDirectory / "ExplosionSimUpdater";
        if (std::filesystem::is_regular_file(helper, ec)) {
            updater.Initialize(helper.string(), installDirectory.string(),
                               "ExplosionSim", CacheDirectory());
            if (!options.noUpdateCheck && options.checkForUpdates) {
                const long long now =
                    static_cast<long long>(std::time(nullptr));
                if (now - options.lastCheckUnix >= 24 * 3600) {
                    updater.CheckAsync();
                    options.lastCheckUnix = now;
                }
            }
        } else {
            ex::LogWarn("Update helper not found; automatic updates are "
                        "disabled.");
        }
    }
    FrameCamera(simulation, camera, options,
                highFidelity ? &highFidelityRuntime.solver.Stats() : nullptr);

    const int frameLimit =
        options.headless ? std::max(1, options.frames) : options.frames;
    int rendered = 0;
    double lastTime = NowSeconds();
    const double benchmarkStart = NowSeconds();
    float frameMs = 0.0f;
    bool running = true;
    while (running) {
        if (window) {
            if (!window->Poll()) {
                break;
            }
            int width = 0;
            int height = 0;
            window->GetSize(width, height);
            if (width > 0 && height > 0 &&
                (width != device->Width() || height != device->Height())) {
                if (!device->Resize(width, height)) {
                    ex::LogWarn("Swapchain resize failed.");
                } else {
                    camera.SetViewport(width, height);
                    if (uiReady) {
                        linuxUi.SetMinImageCount(
                            vkDevice->SwapchainImageCount());
                    }
                }
            }
        }

        if (window && window->Minimized()) {
            // Skip rendering while minimized; keep the frame clock in sync so
            // restoring the window does not produce a large dt.
            lastTime = NowSeconds();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        if (!options.headless) {
            const double now = NowSeconds();
            const float dt = float(std::clamp(now - lastTime, 0.0, 0.05));
            lastTime = now;
            frameMs = dt * 1000.0f;
            if (uiReady) {
                linuxUi.NewFrame();
                linuxUi.Draw(uiState, simulation, frameMs);

                updater.Poll();
                const ex::app::LinuxUpdateSnapshot update = updater.Get();
                uiState.updateStatus = update.status;
                uiState.updateProgress = update.progress;
                uiState.updateBusy =
                    update.phase == ex::app::LinuxUpdatePhase::Checking ||
                    update.phase == ex::app::LinuxUpdatePhase::Downloading;
                uiState.updateReady =
                    update.phase == ex::app::LinuxUpdatePhase::Downloaded;
                uiState.updateFailed =
                    update.phase == ex::app::LinuxUpdatePhase::Failed;
                uiState.updateVersion = update.info.version;
                uiState.updateNotes = update.info.notes;
                bool newer = false;
                if (update.phase == ex::app::LinuxUpdatePhase::Available ||
                    update.phase == ex::app::LinuxUpdatePhase::Downloaded) {
                    ex::update::SemanticVersion remote{};
                    ex::update::SemanticVersion local{};
                    newer =
                        ex::update::ParseVersion(update.info.version, remote) &&
                        ex::update::ParseVersion(EXPLOSION_VERSION_STRING,
                                                 local) &&
                        ex::update::CompareVersions(remote, local) > 0 &&
                        update.info.version != options.skippedVersion;
                }
                uiState.updateAvailable = newer;
                if (uiState.updateCheckRequested) {
                    uiState.updateCheckRequested = false;
                    options.lastCheckUnix =
                        static_cast<long long>(std::time(nullptr));
                    updater.CheckAsync();
                }
                if (uiState.updateCheckEnabled != options.checkForUpdates) {
                    options.checkForUpdates = uiState.updateCheckEnabled;
                }
                if (uiState.updateDownloadRequested) {
                    uiState.updateDownloadRequested = false;
                    updater.DownloadAsync();
                }
                if (uiState.updateSkipRequested) {
                    uiState.updateSkipRequested = false;
                    options.skippedVersion = update.info.version;
                    uiState.updateAvailable = false;
                }
                if (uiState.updateInstallRequested) {
                    uiState.updateInstallRequested = false;
                    if (updater.InstallAndRestart()) {
                        running = false;
                    }
                }

                if (uiState.quitRequested) {
                    running = false;
                }
                if (uiState.resetRequested) {
                    simulation.Reset();
                    uiState.resetRequested = false;
                }
                if (uiState.detonateRequested) {
                    simulation.DetonateNow();
                    uiState.detonateRequested = false;
                }
                if (uiState.flowAround != options.flowAround) {
                    options.flowAround = uiState.flowAround;
                    ex::sim::SimulationSettings settings = simulation.Settings();
                    settings.solverFlowAround = options.flowAround;
                    simulation.ApplySettings(settings);
                    if (highFidelityRuntime.solver.HasVolume()) {
                        highFidelityRuntime.UpdateObstacles(simulation);
                    }
                }
                if (uiState.bodyDrag != options.bodyDrag) {
                    options.bodyDrag = uiState.bodyDrag;
                    ex::sim::SimulationSettings settings = simulation.Settings();
                    settings.solverBodyDrag = options.bodyDrag;
                    simulation.ApplySettings(settings);
                }
                if (uiState.applySimulation) {
                    ex::sim::SimulationSettings settings = simulation.Settings();
                    settings.chargeMassKg =
                        std::max(uiState.chargeMassKg, 0.01);
                    settings.heightOfBurstM =
                        std::max(uiState.heightOfBurstM, 0.0);
                    settings.timeScale = std::max(uiState.timeScale, 0.01);
                    settings.mode =
                        (uiState.highFidelity && solverReady)
                            ? ex::sim::SimulationMode::HighFidelity
                            : ex::sim::SimulationMode::ReducedOrder;
                    settings.solverResolution =
                        std::clamp(uiState.solverResolution, 32, 256);
                    simulation.ApplySettings(settings);
                    highFidelity = settings.mode ==
                                   ex::sim::SimulationMode::HighFidelity;
                    preview.SetHighFidelity(highFidelity);
                    uiState.applySimulation = false;
                }
                if (uiState.reframeRequested) {
                    FrameCamera(
                        simulation, camera, options,
                        highFidelity ? &highFidelityRuntime.solver.Stats()
                                     : nullptr);
                    uiState.reframeRequested = false;
                }
                previewSettings.exposure = uiState.exposure;
                previewSettings.bloomStrength = uiState.bloomStrength;
                previewSettings.raymarchSteps = uiState.raymarchSteps;
                previewSettings.enableVolume = uiState.enableVolume;
                previewSettings.enableParticles = uiState.enableParticles;
                previewSettings.enableGround = uiState.enableGround;
                previewSettings.enableBloom = uiState.enableBloom;
                previewSettings.showGrid = uiState.showGrid;
                preview.SetSettings(previewSettings);
            }
            if (!uiState.paused) {
            if (highFidelity) {
                if (simulation.IsArmed()) {
                    // Tick the fuse through the analytic model; the solver
                    // resets itself once the blast serial changes.
                    simulation.Update(dt);
                } else if (highFidelityRuntime.Ensure(simulation)) {
                    MaybeExpandSolver(highFidelityRuntime.solver, simulation,
                                      options, highFidelityRuntime.capWarned);
                    highFidelityRuntime.BeginFrame(simulation);
                    const double target =
                        std::max(0.0, double(dt) *
                                          simulation.Settings().timeScale);
                    const int maxSteps = std::clamp(
                        simulation.Settings().solverStepsPerFrame, 8, 256);
                    const int batchMax = std::min(maxSteps, 64);
                    double advanced = 0.0;
                    int submitted = 0;
                    while (advanced + 1e-9 < target && submitted < maxSteps) {
                        const float solverDt =
                            highFidelityRuntime.solver.Stats().dt;
                        int steps = 1;
                        if (solverDt > 0.0f) {
                            steps = int(std::ceil((target - advanced) /
                                                  double(solverDt)));
                            steps = std::clamp(
                                steps, 1,
                                std::min(batchMax, maxSteps - submitted));
                        }
                        UpdatePlumeModel(highFidelityRuntime.solver,
                                         simulation);
                        highFidelityRuntime.solver.Step(steps);
                        const double advance =
                            highFidelityRuntime.solver.ConsumeAdvance();
                        advanced += advance;
                        submitted += steps;
                        if (advance <= 0.0) {
                            break;
                        }
                    }
                    highFidelityRuntime.EndFrame(simulation, advanced);
                    if (submitted > 0 || !highFidelityRuntime.published) {
                        highFidelityRuntime.Publish(preview, options);
                    }
                }
                if (gpuParticlesReady) {
                    gpuParticles.Update(simulation.Particles());
                }
            } else {
                simulation.Update(dt);
                if (gpuParticlesReady) {
                    gpuParticles.Update(simulation.Particles());
                }
                if (gpuVolumeReady) {
                    ex::sim::VolumeBakeRequest request;
                    if (simulation.Volume().TakeGpuRequest(request)) {
                        if (gpuVolume.Bake(request)) {
                            preview.SetGpuVolume(
                                gpuVolume.Texture(), gpuVolume.Bounds(),
                                gpuVolume.Extinction(), gpuVolume.Version());
                        } else {
                            ex::LogWarn(
                                "GPU volume bake failed; using the CPU baker.");
                            gpuVolumeReady = false;
                            simulation.Volume().SetGpuBake(false);
                        }
                    }
                } else {
                    simulation.Volume().Poll();
                }
            }
            }
        }

        preview.Render(device.get(), simulation, camera, device->Width(),
                       device->Height(), options.vsync);
        ++rendered;
        if (frameLimit > 0 && rendered >= frameLimit) {
            running = false;
        }
    }

    if (options.benchmark && rendered > 0) {
        const double elapsed = NowSeconds() - benchmarkStart;
        ex::LogInfo("Benchmark: %d frames at %dx%d in %.3f s", rendered,
                    device->Width(), device->Height(), elapsed);
        ex::LogInfo("Average: %.2f ms/frame (%.1f fps)",
                    elapsed * 1000.0 / double(rendered),
                    double(rendered) / std::max(elapsed, 1e-9));
    }

    // Persist the update bookkeeping (skipped version, last check) the same
    // way the Windows application does. A profile passed with --settings is a
    // read-only input and is never rewritten.
    if (!settingsPath.empty() && !settingsExplicit &&
        !SaveSettingsFile(settingsPath, options)) {
        ex::LogWarn("Could not save settings: %s", settingsPath.c_str());
    }

    if (options.headless && !options.screenshot.empty()) {
        device->WaitIdle();
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
        if (!device->ReadBack(pixels, width, height)) {
            ex::LogError("Could not read back the offscreen frame.");
            return 1;
        }
        if (!WriteImage(options.screenshot, pixels, width, height)) {
            ex::LogError("Could not write %s.", options.screenshot.c_str());
            return 1;
        }
        ex::LogInfo("Saved screenshot: %s (%dx%d)", options.screenshot.c_str(),
                    width, height);
    }

    if (uiReady) {
        device->WaitIdle();
        window->SetEventCallback(nullptr, nullptr);
        linuxUi.Shutdown();
    }
    device->WaitIdle();
    ex::LogInfo("Rendered %d frame(s).", rendered);
    return 0;
}
