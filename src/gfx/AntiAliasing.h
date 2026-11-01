#pragma once

#include <cstdint>
#include <memory>

namespace ex::rhi {
class Device;
class Texture;
} // namespace ex::rhi

namespace ex::gfx {

// Post-process anti-aliasing mode. The values match the `antiAliasing` setting
// in settings.ini and the combo boxes on both platforms.
enum class AntiAliasingMode : int {
    Off = 0,
    Fxaa = 1,
    Smaa1x = 2,
};

// SMAA 1x lookup tables. The reference textures are RG8 (area) and R8
// (search); both are expanded to RGBA8 so the shared RHI can upload them on
// D3D11 and Vulkan with the same code. Returns null if the texture could not
// be created.
std::unique_ptr<rhi::Texture> CreateSmaaAreaTexture(rhi::Device* device);
std::unique_ptr<rhi::Texture> CreateSmaaSearchTexture(rhi::Device* device);

} // namespace ex::gfx
