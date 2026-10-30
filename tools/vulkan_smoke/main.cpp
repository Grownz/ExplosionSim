// Minimal Vulkan bring-up test for milestone M2. It creates a device, clears a
// window or an offscreen BGRA8 target and (headless) writes a PPM image.

#include "gfx/rhi/vulkan/VulkanRhi.h"
#include "platform/Platform.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Options {
    int width = 640;
    int height = 360;
    int frames = -1; // -1 = default (3 headless, unlimited windowed)
    bool headless = false;
    bool debug = false;
    std::string output = "vulkan_smoke.ppm";
};

bool ParseInt(const char* text, int& value) {
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (!end || *end != '\0' || parsed < 0) {
        return false;
    }
    value = int(parsed);
    return true;
}

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--headless") == 0) {
            options.headless = true;
            continue;
        }
        if (std::strcmp(arg, "--debug") == 0) {
            options.debug = true;
            continue;
        }
        if (std::strcmp(arg, "--width") == 0 && i + 1 < argc) {
            if (!ParseInt(argv[++i], options.width) || options.width == 0) {
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--height") == 0 && i + 1 < argc) {
            if (!ParseInt(argv[++i], options.height) || options.height == 0) {
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--frames") == 0 && i + 1 < argc) {
            if (!ParseInt(argv[++i], options.frames)) {
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--output") == 0 && i + 1 < argc) {
            options.output = argv[++i];
            continue;
        }
        std::fprintf(stderr, "Unknown argument: %s\n", arg);
        return false;
    }
    return true;
}

bool WritePpm(const std::string& path, const std::vector<uint8_t>& bgra, int width,
              int height) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file << "P6\n" << width << " " << height << "\n255\n";
    for (int i = 0; i < width * height; ++i) {
        const uint8_t b = bgra[size_t(i) * 4 + 0];
        const uint8_t g = bgra[size_t(i) * 4 + 1];
        const uint8_t r = bgra[size_t(i) * 4 + 2];
        file.put(char(r));
        file.put(char(g));
        file.put(char(b));
    }
    return file.good();
}

bool CreateSurface(void* user, void* instance, uint64_t* surface) {
    auto* window = static_cast<ex::platform::Window*>(user);
    return window && window->CreateVulkanSurface(instance, surface);
}

} // namespace

int main(int argc, char** argv) {
    Options options{};
    if (!ParseOptions(argc, argv, options)) {
        return 2;
    }

    std::unique_ptr<ex::platform::Window> window;
    ex::rhi::DeviceDesc desc{};
    desc.backend = ex::rhi::Backend::Vulkan;
    desc.debug = options.debug;
    desc.width = options.width;
    desc.height = options.height;
    desc.offscreen = options.headless;

    if (!options.headless) {
        ex::platform::WindowDesc windowDesc{};
        windowDesc.title = "ExplosionSim Vulkan smoke";
        windowDesc.width = options.width;
        windowDesc.height = options.height;
        window = ex::platform::CreatePlatformWindow(windowDesc);
        if (!window) {
            std::fprintf(stderr, "SDL window creation failed.\n");
            return 1;
        }

        uint32_t extensionCount = 0;
        const char* const* extensions =
            window->VulkanInstanceExtensions(extensionCount);
        desc.surface.instanceExtensions = extensions;
        desc.surface.instanceExtensionCount = extensionCount;
        desc.surface.user = window.get();
        desc.surface.create = CreateSurface;
        desc.window = window->NativeHandle();
    }

    std::unique_ptr<ex::rhi::Device> base = ex::rhi::CreateVulkanDevice(desc);
    if (!base) {
        std::fprintf(stderr, "Vulkan device creation failed.\n");
        return 1;
    }
    auto* device = static_cast<ex::rhi::vulkan::Device*>(base.get());

    const int frames =
        options.frames >= 0 ? options.frames : (options.headless ? 3 : 0);
    int rendered = 0;
    for (int frame = 0; frames == 0 || frame < frames; ++frame) {
        if (window && !window->Poll()) {
            break;
        }
        const float t = float(frame % 120) / 120.0f;
        device->BeginFrame(0.1f + 0.2f * t, 0.2f, 0.4f - 0.2f * t, 1.0f);
        device->EndFrame(true);
        ++rendered;
    }

    if (options.headless) {
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
        if (!device->ReadBack(pixels, width, height)) {
            std::fprintf(stderr, "Vulkan readback failed.\n");
            return 1;
        }
        if (!WritePpm(options.output, pixels, width, height)) {
            std::fprintf(stderr, "Could not write %s\n", options.output.c_str());
            return 1;
        }
        std::printf("Vulkan smoke: %s, %dx%d, %d frame(s) -> %s\n",
                    device->AdapterName().c_str(), width, height, rendered,
                    options.output.c_str());
    } else {
        std::printf("Vulkan smoke: %s, %dx%d, %d frame(s)\n",
                    device->AdapterName().c_str(), device->Width(),
                    device->Height(), rendered);
    }

    base.reset();
    window.reset();
    return 0;
}
