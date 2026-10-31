#pragma once

#include <string>

namespace ex::app {

// The UI font used by the ImGui panels and the splash. Both must use the same
// family so the startup screen matches the rest of the program.
//
// The lookup prefers the platform's native UI face (Segoe UI on Windows) and
// falls back to the common Linux sans families. An empty string means that no
// usable TTF was found; callers should then fall back to their built-in font.
std::string UiFontPath();
// Semibold/bold variant for titles; falls back to UiFontPath().
std::string UiFontSemiboldPath();

} // namespace ex::app
