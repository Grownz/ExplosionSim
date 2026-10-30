#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ex::gfx {

// Portable PNG/JPEG image I/O backed by stb_image/stb_image_write. The
// wide-string overloads are kept for the Win32 call sites; on Linux use the
// UTF-8 std::string overloads.
bool SavePng(const std::string& path, const uint8_t* bgra, int width, int height);
bool SavePng(const std::wstring& path, const uint8_t* bgra, int width, int height);

// Loads an image as top-down 32-bit BGRA.
bool LoadImageBgra(const std::string& path, std::vector<uint8_t>& bgra, int& width,
                   int& height);
bool LoadImageBgra(const std::wstring& path, std::vector<uint8_t>& bgra, int& width,
                   int& height);

} // namespace ex::gfx
