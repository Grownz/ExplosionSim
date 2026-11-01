#include "gfx/AntiAliasing.h"

#include "gfx/rhi/Rhi.h"
#include "gfx/SmaaAreaTex.h"
#include "gfx/SmaaSearchTex.h"

#include <vector>

namespace ex::gfx {
namespace {

// The reference headers store 8-bit channels in the layout the SMAA shaders
// expect: RG8 for the area table (the shaders read .rg) and R8 for the search
// table (read as .r). Expand them into RGBA8, which both backends support as a
// sampled texture.
std::unique_ptr<rhi::Texture> MakeLookupTexture(rhi::Device* device,
                                               const unsigned char* source,
                                               uint32_t width, uint32_t height,
                                               uint32_t channels) {
    if (!device || !source || width == 0 || height == 0) {
        return nullptr;
    }
    std::vector<uint8_t> rgba(size_t(width) * size_t(height) * 4, 255);
    for (size_t texel = 0; texel < size_t(width) * size_t(height); ++texel) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            rgba[texel * 4 + channel] = source[texel * channels + channel];
        }
        // Unused channels are zero; alpha stays opaque.
        for (uint32_t channel = channels; channel < 3; ++channel) {
            rgba[texel * 4 + channel] = 0;
        }
    }

    rhi::TextureDesc desc{};
    desc.format = rhi::Format::R8G8B8A8_UNORM;
    desc.width = width;
    desc.height = height;
    desc.usage = rhi::TextureUsage::Sampled;
    desc.initialData = rgba.data();
    return device->CreateTexture(desc);
}

} // namespace

std::unique_ptr<rhi::Texture> CreateSmaaAreaTexture(rhi::Device* device) {
    return MakeLookupTexture(device, areaTexBytes, AREATEX_WIDTH,
                             AREATEX_HEIGHT, 2);
}

std::unique_ptr<rhi::Texture> CreateSmaaSearchTexture(rhi::Device* device) {
    return MakeLookupTexture(device, searchTexBytes, SEARCHTEX_WIDTH,
                             SEARCHTEX_HEIGHT, 1);
}

} // namespace ex::gfx
