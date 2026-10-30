// Native Linux application shell. It owns the SDL3 window and the Vulkan RHI
// device; the reduced-order renderer is ported on top of this loop.

#include "Version.h"
#include "core/Log.h"
#include "gfx/rhi/Rhi.h"
#include "platform/Platform.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
};

void PrintUsage() {
    std::printf("Explosion Simulator %s\n", EXPLOSION_VERSION_STRING);
    std::printf("Usage: explosion_linux [options]\n\n");
    std::printf("  --screenshot <path>  Render an offscreen image (.png or .ppm) and exit.\n");
    std::printf("  --width <pixels>     Output width (default 1600).\n");
    std::printf("  --height <pixels>    Output height (default 900).\n");
    std::printf("  --frames <count>     Number of frames to render.\n");
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
    ex::LogInfo("Linux app ready: %dx%d, %s", device->Width(),
                device->Height(), options.headless ? "offscreen" : "windowed");

    const int frameLimit =
        options.headless ? std::max(1, options.frames) : options.frames;
    int rendered = 0;
    const double start = NowSeconds();
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
                }
            }
        }

        const double time = NowSeconds() - start;
        const float r = 0.02f + 0.01f * float(std::sin(time * 0.7));
        const float g = 0.03f + 0.01f * float(std::sin(time * 0.5 + 1.3));
        const float b = 0.06f + 0.02f * float(std::sin(time * 0.3 + 2.1));
        device->BeginFrame(r, g, b, 1.0f);
        device->EndFrame(options.vsync);
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
