#include "platform/Platform.h"

#include "core/Log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

namespace ex::platform {

namespace {

class Sdl3Window final : public Window {
public:
    Sdl3Window() = default;
    ~Sdl3Window() override {
        if (window_) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    bool Initialize(const WindowDesc& desc) {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            LogError("SDL video initialization failed: %s", SDL_GetError());
            return false;
        }

        SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
        if (desc.hidden) {
            flags |= SDL_WINDOW_HIDDEN;
        }
        if (desc.resizable) {
            flags |= SDL_WINDOW_RESIZABLE;
        }
        window_ = SDL_CreateWindow(desc.title, desc.width, desc.height, flags);
        if (!window_) {
            LogError("SDL window creation failed: %s", SDL_GetError());
            return false;
        }
        return true;
    }

    void* NativeHandle() const override { return window_; }

    bool Poll() override {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
                return false;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE) {
                    return false;
                }
                break;
            default:
                break;
            }
        }
        return true;
    }

    void GetSize(int& width, int& height) const override {
        width = 0;
        height = 0;
        if (window_) {
            SDL_GetWindowSizeInPixels(window_, &width, &height);
        }
    }

    void SetTitle(const char* title) override {
        if (window_) {
            SDL_SetWindowTitle(window_, title);
        }
    }

    const char* const* VulkanInstanceExtensions(uint32_t& count) const override {
        count = 0;
        return SDL_Vulkan_GetInstanceExtensions(&count);
    }

    bool CreateVulkanSurface(void* instance, uint64_t* surface) const override {
        if (!window_ || !instance || !surface) {
            return false;
        }
        VkSurfaceKHR created = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(
                window_, static_cast<VkInstance>(instance), nullptr, &created)) {
            LogError("SDL Vulkan surface creation failed: %s", SDL_GetError());
            return false;
        }
        *surface = reinterpret_cast<uint64_t>(created);
        return true;
    }

private:
    SDL_Window* window_ = nullptr;
};

} // namespace

std::unique_ptr<Window> CreatePlatformWindow(const WindowDesc& desc) {
    auto window = std::make_unique<Sdl3Window>();
    if (!window->Initialize(desc)) {
        return nullptr;
    }
    return window;
}

} // namespace ex::platform
