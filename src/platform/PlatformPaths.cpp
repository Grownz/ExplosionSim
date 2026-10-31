#include "platform/Platform.h"

#include <SDL3/SDL.h>

namespace ex::platform {

std::filesystem::path ExecutableDirectory() {
    const char* base = SDL_GetBasePath();
    if (!base) {
        return {};
    }
    return std::filesystem::path(base);
}

} // namespace ex::platform
