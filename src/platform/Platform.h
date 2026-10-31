#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ex::platform {

struct WindowDesc {
    const char* title = "ExplosionSim";
    int width = 1280;
    int height = 720;
    bool hidden = false;
    bool resizable = true;
};

// Platform-neutral window and event loop. The Vulkan surface is created through
// a callback so this header stays free of Vulkan and Win32 types.
class Window {
public:
    virtual ~Window() = default;

    virtual void* NativeHandle() const = 0;

    // Processes pending events. Returns false when the window should close.
    virtual bool Poll() = 0;

    virtual void GetSize(int& width, int& height) const = 0;
    virtual void SetTitle(const char* title) = 0;
    // True while the window is minimized; the application can skip rendering.
    virtual bool Minimized() const = 0;
    // Brings the window to the front and gives it focus.
    virtual void Raise() = 0;
    // Keeps the window above other applications while enabled.
    virtual void SetAlwaysOnTop(bool enabled) = 0;
    // Optional raw-event sink. The event is a backend event object (SDL_Event
    // on the SDL3 backend) and is only valid for the duration of the call.
    using EventCallback = void (*)(void* user, const void* event);
    virtual void SetEventCallback(EventCallback callback, void* user) = 0;

    virtual const char* const* VulkanInstanceExtensions(
        uint32_t& count) const = 0;
    // `instance` and `surface` are VkInstance/VkSurfaceKHR as opaque handles.
    virtual bool CreateVulkanSurface(void* instance, uint64_t* surface) const = 0;
};

std::unique_ptr<Window> CreatePlatformWindow(const WindowDesc& desc);

// Directory that contains the running executable (with a trailing separator),
// or an empty path when it cannot be determined. Used to resolve assets that
// ship next to the binary.
std::filesystem::path ExecutableDirectory();

} // namespace ex::platform
