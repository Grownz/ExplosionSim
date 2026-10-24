#pragma once

#include <cstdint>
#include <string>

namespace ex::gfx {

// Writes an 8-bit BGRA image as PNG using the Windows Imaging Component.
bool SavePng(const std::wstring& path, const uint8_t* bgra, int width, int height);

} // namespace ex::gfx
