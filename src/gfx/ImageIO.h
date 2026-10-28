#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ex::gfx {

// Writes an 8-bit BGRA image as PNG using the Windows Imaging Component.
bool SavePng(const std::wstring& path, const uint8_t* bgra, int width, int height);

// Loads an image as top-down 32-bit BGRA using the Windows Imaging Component.
bool LoadImageBgra(const std::wstring& path, std::vector<uint8_t>& bgra, int& width,
                   int& height);

} // namespace ex::gfx
