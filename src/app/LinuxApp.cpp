// Native Linux application shell. It owns the SDL3 window and the Vulkan RHI
// device; the reduced-order renderer is ported on top of this loop.

#include "Version.h"
#include "app/LinuxPreview.h"
#include "core/Log.h"
#include "core/ThreadPool.h"
#include "gfx/Camera.h"
#include "gfx/GpuVolumeBaker.h"
#include "gfx/GpuParticleSystem.h"
#include "gfx/rhi/Rhi.h"
#include "platform/Platform.h"
#include "sim/Simulation.h"
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
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
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
        if (std::strcmp(arg, "--reduced-order") == 0 ||
            std::strcmp(arg, "--high-fidelity") == 0 ||
            std::strcmp(arg, "--no-update-check") == 0) {
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
                 const Options& options) {
    const ex::sim::BlastModel& blast = simulation.Blast();
    const ex::sim::BlastState& state = blast.State();
    const float fireball = float(blast.FireballMaxRadiusM());
    const float cloud = float(state.cloudRadius);
    const float burstHeight = float(blast.GetCharge().heightOfBurstM);
    const float sceneExtent = simulation.Scene().Extent();
    const float radius =
        std::max({fireball * 3.2f, cloud * 2.4f, burstHeight * 1.6f,
                  sceneExtent * 0.8f, 1.2f});
    const float centerY = burstHeight + fireball * 0.6f;
    camera.SetViewport(options.width, options.height);
    camera.SetClipPlanes(0.5f, 20000.0f);
    camera.SetTarget(DirectX::XMFLOAT3(0.0f, centerY, 0.0f),
                     radius * 1.9f * std::max(0.1f, options.distanceScale));
    camera.SetOrientation(options.azimuth, options.elevation);
    camera.Snap();
}

uint32_t Crc32Update(uint32_t crc, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value >> 24));
    out.push_back(uint8_t(value >> 16));
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value));
}

void WriteChunk(std::ofstream& file, const char type[4],
                const std::vector<uint8_t>& data) {
    std::vector<uint8_t> header;
    AppendU32(header, uint32_t(data.size()));
    file.write(reinterpret_cast<const char*>(header.data()),
               std::streamsize(header.size()));
    file.write(type, 4);
    if (!data.empty()) {
        file.write(reinterpret_cast<const char*>(data.data()),
                   std::streamsize(data.size()));
    }
    uint32_t crc = Crc32Update(0xFFFFFFFFu,
                               reinterpret_cast<const uint8_t*>(type), 4);
    crc = Crc32Update(crc, data.data(), data.size()) ^ 0xFFFFFFFFu;
    std::vector<uint8_t> tail;
    AppendU32(tail, crc);
    file.write(reinterpret_cast<const char*>(tail.data()),
               std::streamsize(tail.size()));
}

bool WritePng(const std::string& path, const std::vector<uint8_t>& bgra,
              int width, int height) {
    if (width <= 0 || height <= 0 ||
        bgra.size() < size_t(width) * size_t(height) * 4) {
        return false;
    }
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (1 + size_t(width) * 3));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0); // filter: none
        for (int x = 0; x < width; ++x) {
            const size_t index = (size_t(y) * size_t(width) + size_t(x)) * 4;
            raw.push_back(bgra[index + 2]);
            raw.push_back(bgra[index + 1]);
            raw.push_back(bgra[index + 0]);
        }
    }

    std::vector<uint8_t> zlib;
    zlib.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    zlib.push_back(0x78);
    zlib.push_back(0x01);
    size_t offset = 0;
    while (offset < raw.size()) {
        const size_t block = std::min<size_t>(65535, raw.size() - offset);
        const bool finalBlock = offset + block == raw.size();
        zlib.push_back(finalBlock ? 1 : 0);
        zlib.push_back(uint8_t(block & 0xFF));
        zlib.push_back(uint8_t((block >> 8) & 0xFF));
        zlib.push_back(uint8_t((~block) & 0xFF));
        zlib.push_back(uint8_t(((~block) >> 8) & 0xFF));
        zlib.insert(zlib.end(), raw.begin() + std::ptrdiff_t(offset),
                    raw.begin() + std::ptrdiff_t(offset + block));
        offset += block;
    }
    uint32_t a = 1;
    uint32_t b = 0;
    for (uint8_t value : raw) {
        a = (a + value) % 65521u;
        b = (b + a) % 65521u;
    }
    AppendU32(zlib, (b << 16) | a);

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    file.write(reinterpret_cast<const char*>(signature), 8);

    std::vector<uint8_t> ihdr;
    AppendU32(ihdr, uint32_t(width));
    AppendU32(ihdr, uint32_t(height));
    ihdr.push_back(8); // bit depth
    ihdr.push_back(2); // truecolour
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    WriteChunk(file, "IHDR", ihdr);
    WriteChunk(file, "IDAT", zlib);
    WriteChunk(file, "IEND", {});
    return file.good();
}

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

bool WriteImage(const std::string& path, const std::vector<uint8_t>& bgra,
                int width, int height) {
    const bool ppm = path.size() >= 4 &&
                     (path.compare(path.size() - 4, 4, ".ppm") == 0 ||
                      path.compare(path.size() - 4, 4, ".PPM") == 0);
    return ppm ? WritePpm(path, bgra, width, height)
               : WritePng(path, bgra, width, height);
}

} // namespace

int main(int argc, char** argv) {
    Options options{};
    std::string settingsPath;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--settings") == 0) {
            settingsPath = argv[i + 1];
            break;
        }
    }
    if (settingsPath.empty()) {
        const char* home = std::getenv("HOME");
        const std::filesystem::path candidates[] = {
            std::filesystem::path("settings.ini"),
            std::filesystem::path(argv[0]).parent_path() / "settings.ini",
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

    std::unique_ptr<ex::rhi::Device> device = ex::rhi::CreateVulkanDevice(desc);
    if (!device) {
        ex::LogError("Could not create the Vulkan device.");
        return 1;
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

    ex::core::ThreadPool pool;
    ex::sim::SimulationSettings simulationSettings{};
    simulationSettings.chargeMassKg =
        options.chargeMassKg > 0.0 ? options.chargeMassKg : 0.2;
    simulationSettings.heightOfBurstM =
        options.heightOfBurstM >= 0.0 ? options.heightOfBurstM : 0.4;
    simulationSettings.mode = ex::sim::SimulationMode::ReducedOrder;
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
    ex::LogInfo("Linux app ready: %dx%d, %s", device->Width(),
                device->Height(), options.headless ? "offscreen" : "windowed");

    if (options.headless) {
        const double targetTime = std::max(0.0, options.captureTime);
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
    FrameCamera(simulation, camera, options);

    const int frameLimit =
        options.headless ? std::max(1, options.frames) : options.frames;
    int rendered = 0;
    double lastTime = NowSeconds();
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
                }
            }
        }

        if (!options.headless) {
            const double now = NowSeconds();
            const float dt = float(std::clamp(now - lastTime, 0.0, 0.05));
            lastTime = now;
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

        preview.Render(device.get(), simulation, camera, device->Width(),
                       device->Height(), options.vsync);
        ++rendered;
        if (frameLimit > 0 && rendered >= frameLimit) {
            running = false;
        }
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

    device->WaitIdle();
    ex::LogInfo("Rendered %d frame(s).", rendered);
    return 0;
}
