// Minimal Vulkan bring-up test for milestone M2. It creates a device, clears a
// window or an offscreen BGRA8 target and (headless) writes a PPM image.

#include "gfx/rhi/vulkan/VulkanRhi.h"
#include "platform/Platform.h"
#include "TriangleShaders.h"
#include "QuadShaders.h"
#include "SkyShaders.h"
#include "CompositeShaders.h"
#include "GroundShaders.h"
#include "BloomShaders.h"
#include "SceneShaders.h"
#include "ShockShaders.h"
#include "VolumeShaders.h"
#include "ParticleShaders.h"
#include "scene/SceneTypes.h"
#include "sim/BlastModel.h"
#include "gfx/SceneMeshLibrary.h"

#include <DirectXMath.h>

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
    bool triangle = false;
    bool quad = false;
    bool sky = false;
    bool composite = false;
    bool ground = false;
    bool target = false;
    bool bloom = false;
    bool scene = false;
    bool shock = false;
    bool volume = false;
    bool particles = false;
    bool frame = false;
    bool sceneFrame = false;
    bool city = false;
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
        if (std::strcmp(arg, "--triangle") == 0) {
            options.triangle = true;
            continue;
        }
        if (std::strcmp(arg, "--quad") == 0) {
            options.quad = true;
            continue;
        }
        if (std::strcmp(arg, "--sky") == 0) {
            options.sky = true;
            continue;
        }
        if (std::strcmp(arg, "--composite") == 0) {
            options.composite = true;
            continue;
        }
        if (std::strcmp(arg, "--ground") == 0) {
            options.ground = true;
            continue;
        }
        if (std::strcmp(arg, "--target") == 0) {
            options.target = true;
            continue;
        }
        if (std::strcmp(arg, "--bloom") == 0) {
            options.bloom = true;
            continue;
        }
        if (std::strcmp(arg, "--scene") == 0) {
            options.scene = true;
            continue;
        }
        if (std::strcmp(arg, "--shock") == 0) {
            options.shock = true;
            continue;
        }
        if (std::strcmp(arg, "--volume") == 0) {
            options.volume = true;
            continue;
        }
        if (std::strcmp(arg, "--particles") == 0) {
            options.particles = true;
            continue;
        }
        if (std::strcmp(arg, "--frame") == 0) {
            options.frame = true;
            continue;
        }
        if (std::strcmp(arg, "--scene-frame") == 0) {
            options.sceneFrame = true;
            continue;
        }
        if (std::strcmp(arg, "--city") == 0) {
            options.city = true;
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

    std::unique_ptr<ex::rhi::Texture> frameHdrTarget;
    std::unique_ptr<ex::rhi::Texture> frameDepthTarget;
    std::unique_ptr<ex::rhi::DescriptorSet> frameBloomDownSet;
    std::unique_ptr<ex::rhi::DescriptorSet> frameCompositeSet;
    if (options.frame || options.sceneFrame || options.city) {
        ex::rhi::TextureDesc hdrDesc{};
        hdrDesc.format = ex::rhi::Format::B8G8R8A8_UNORM;
        hdrDesc.width = uint32_t(options.width);
        hdrDesc.height = uint32_t(options.height);
        hdrDesc.usage = ex::rhi::TextureUsage::RenderTarget |
                        ex::rhi::TextureUsage::Sampled;
        frameHdrTarget = device->CreateTexture(hdrDesc);
        if (!frameHdrTarget) {
            std::fprintf(stderr, "Vulkan frame target failed.\n");
            return 1;
        }
        if (options.sceneFrame || options.city) {
            ex::rhi::TextureDesc depthDesc{};
            depthDesc.format = ex::rhi::Format::D32_FLOAT;
            depthDesc.width = uint32_t(options.width);
            depthDesc.height = uint32_t(options.height);
            depthDesc.usage = ex::rhi::TextureUsage::DepthStencil |
                              ex::rhi::TextureUsage::Sampled;
            frameDepthTarget = device->CreateTexture(depthDesc);
            if (!frameDepthTarget) {
                std::fprintf(stderr, "Vulkan frame depth target failed.\n");
                return 1;
            }
        }
    }

    struct TriangleVertex {
        float position[3];
        float color[3];
    };
    std::unique_ptr<ex::rhi::Buffer> vertexBuffer;
    std::unique_ptr<ex::rhi::Shader> vertexShader;
    std::unique_ptr<ex::rhi::Shader> pixelShader;
    std::unique_ptr<ex::rhi::Pipeline> pipeline;
    if (options.triangle) {
        const TriangleVertex vertices[] = {
            {{0.0f, -0.7f, 0.0f}, {1.0f, 0.2f, 0.2f}},
            {{0.7f, 0.6f, 0.0f}, {0.2f, 1.0f, 0.3f}},
            {{-0.7f, 0.6f, 0.0f}, {0.2f, 0.4f, 1.0f}},
        };
        ex::rhi::BufferDesc bufferDesc{};
        bufferDesc.size = sizeof(vertices);
        bufferDesc.usage = ex::rhi::BufferUsage::Vertex;
        bufferDesc.initialData = vertices;
        vertexBuffer = device->CreateBuffer(bufferDesc);

        ex::rhi::ShaderDesc shaderDesc{};
        shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
        shaderDesc.data = kTriangleVS;
        shaderDesc.size = kTriangleVSSize;
        shaderDesc.name = "VSMain";
        vertexShader = device->CreateShader(shaderDesc);
        shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
        shaderDesc.data = kTrianglePS;
        shaderDesc.size = kTrianglePSSize;
        shaderDesc.name = "PSMain";
        pixelShader = device->CreateShader(shaderDesc);

        const ex::rhi::VertexAttribute attributes[] = {
            {"POSITION", 0, ex::rhi::VertexFormat::Float3, 0, 0},
            {"COLOR", 0, ex::rhi::VertexFormat::Float3, 12, 0},
        };
        ex::rhi::PipelineDesc pipelineDesc{};
        pipelineDesc.vertexShader = vertexShader.get();
        pipelineDesc.pixelShader = pixelShader.get();
        pipelineDesc.vertexAttributes = attributes;
        pipelineDesc.vertexAttributeCount = 2;
        pipelineDesc.vertexStride = sizeof(TriangleVertex);
        pipelineDesc.depthTest = false;
        pipelineDesc.depthWrite = false;
        pipeline = device->CreatePipeline(pipelineDesc);

        if (!vertexBuffer || !vertexShader || !pixelShader || !pipeline) {
            std::fprintf(stderr, "Vulkan triangle resources failed.\n");
            return 1;
        }
    }

    std::unique_ptr<ex::rhi::Buffer> quadVertexBuffer;
    std::unique_ptr<ex::rhi::Texture> quadTexture;
    std::unique_ptr<ex::rhi::Sampler> quadSampler;
    std::unique_ptr<ex::rhi::Buffer> quadUniformBuffer;
    std::unique_ptr<ex::rhi::Shader> quadVertexShader;
    std::unique_ptr<ex::rhi::Shader> quadPixelShader;
    std::unique_ptr<ex::rhi::Pipeline> quadPipeline;
    std::unique_ptr<ex::rhi::DescriptorSet> quadSet;

    if (options.quad || options.target) {
        struct QuadVertex {
            float position[3];
            float uv[2];
        };
        const QuadVertex vertices[] = {
            {{-0.8f, -0.8f, 0.0f}, {0.0f, 1.0f}},
            {{0.8f, -0.8f, 0.0f}, {1.0f, 1.0f}},
            {{0.8f, 0.8f, 0.0f}, {1.0f, 0.0f}},
            {{-0.8f, -0.8f, 0.0f}, {0.0f, 1.0f}},
            {{0.8f, 0.8f, 0.0f}, {1.0f, 0.0f}},
            {{-0.8f, 0.8f, 0.0f}, {0.0f, 0.0f}},
        };
        ex::rhi::BufferDesc bufferDesc{};
        bufferDesc.size = sizeof(vertices);
        bufferDesc.usage = ex::rhi::BufferUsage::Vertex;
        bufferDesc.initialData = vertices;
        quadVertexBuffer = device->CreateBuffer(bufferDesc);

        const uint8_t pixels[16] = {
            255, 0, 0, 255, 0, 255, 0, 255,
            0, 0, 255, 255, 255, 255, 255, 255,
        };
        ex::rhi::TextureSubresource subresource{};
        subresource.data = pixels;
        subresource.width = 2;
        subresource.height = 2;
        subresource.rowPitch = 8;
        ex::rhi::TextureDesc textureDesc{};
        textureDesc.format = ex::rhi::Format::R8G8B8A8_UNORM;
        textureDesc.width = 2;
        textureDesc.height = 2;
        textureDesc.usage = ex::rhi::TextureUsage::Sampled;
        textureDesc.subresources = &subresource;
        textureDesc.subresourceCount = 1;
        quadTexture = device->CreateTexture(textureDesc);

        ex::rhi::SamplerDesc samplerDesc{};
        samplerDesc.linear = false;
        samplerDesc.clamp = true;
        quadSampler = device->CreateSampler(samplerDesc);

        const float tint[4] = {0.5f, 1.0f, 1.0f, 1.0f};
        ex::rhi::BufferDesc uniformDesc{};
        uniformDesc.size = sizeof(tint);
        uniformDesc.usage =
            ex::rhi::BufferUsage::Uniform | ex::rhi::BufferUsage::Dynamic;
        uniformDesc.initialData = tint;
        quadUniformBuffer = device->CreateBuffer(uniformDesc);

        ex::rhi::ShaderDesc shaderDesc{};
        shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
        shaderDesc.data = kQuadVS;
        shaderDesc.size = kQuadVSSize;
        shaderDesc.name = "VSMain";
        quadVertexShader = device->CreateShader(shaderDesc);
        shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
        shaderDesc.data = kQuadPS;
        shaderDesc.size = kQuadPSSize;
        shaderDesc.name = "PSMain";
        quadPixelShader = device->CreateShader(shaderDesc);

        const ex::rhi::VertexAttribute attributes[] = {
            {"POSITION", 0, ex::rhi::VertexFormat::Float3, 0, 0},
            {"TEXCOORD", 0, ex::rhi::VertexFormat::Float2, 12, 0},
        };
        const ex::rhi::DescriptorBinding bindings[] = {
            {0, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {1, ex::rhi::DescriptorType::Sampler, 1,
             ex::rhi::ShaderStage::Pixel},
            {2, ex::rhi::DescriptorType::UniformBuffer, 1,
             ex::rhi::ShaderStage::Pixel},
        };
        ex::rhi::PipelineDesc pipelineDesc{};
        pipelineDesc.vertexShader = quadVertexShader.get();
        pipelineDesc.pixelShader = quadPixelShader.get();
        pipelineDesc.vertexAttributes = attributes;
        pipelineDesc.vertexAttributeCount = 2;
        pipelineDesc.vertexStride = sizeof(QuadVertex);
        pipelineDesc.descriptorBindings = bindings;
        pipelineDesc.descriptorBindingCount = 3;
        pipelineDesc.depthTest = false;
        pipelineDesc.depthWrite = false;
        quadPipeline = device->CreatePipeline(pipelineDesc);

        if (quadPipeline) {
            quadSet = device->CreateDescriptorSet(quadPipeline.get());
        }
        if (quadSet) {
            quadSet->SetImage(0, quadTexture.get());
            quadSet->SetSampler(1, quadSampler.get());
            quadSet->SetBuffer(2, quadUniformBuffer.get(), 0, sizeof(tint));
        }
        if (!quadVertexBuffer || !quadTexture || !quadSampler ||
            !quadUniformBuffer || !quadVertexShader || !quadPixelShader ||
            !quadPipeline || !quadSet) {
            std::fprintf(stderr, "Vulkan quad resources failed.\n");
            return 1;
        }
    }

    std::unique_ptr<ex::rhi::Buffer> skyUniformBuffer;
    std::unique_ptr<ex::rhi::Shader> skyVertexShader;
    std::unique_ptr<ex::rhi::Shader> skyPixelShader;
    std::unique_ptr<ex::rhi::Pipeline> skyPipeline;
    std::unique_ptr<ex::rhi::DescriptorSet> skySet;

    if (options.sky || options.frame || options.sceneFrame || options.city) {
        // Layout must match FrameConstants in src/gfx/Renderer.h.
        float constants[112] = {};
        constants[0] = 1.0f;
        constants[5] = 1.0f;
        constants[10] = 1.0f;
        constants[15] = 1.0f;
        constants[16] = 1.0f;
        constants[21] = 1.0f;
        constants[26] = 1.0f;
        constants[31] = 1.0f;
        constants[32] = 0.0f;
        constants[33] = 0.0f;
        constants[34] = -10.0f;
        constants[35] = 0.0f;
        constants[36] = 0.3f;
        constants[37] = 0.8f;
        constants[38] = 0.5f;
        constants[39] = 1.0f;
        constants[40] = 1.0f;
        constants[41] = 0.7f;
        constants[42] = 0.44f;
        constants[43] = 1.0f;
        constants[52] = float(options.width);
        constants[53] = float(options.height);
        constants[54] = 1.0f / float(options.width);
        constants[55] = 1.0f / float(options.height);
        constants[64] = 0.0f;
        constants[65] = 1.0f / 60.0f;
        constants[66] = 0.0f;
        constants[67] = 0.0f;
        constants[68] = 1.0f;
        constants[69] = 0.0f;
        constants[70] = 0.0f;
        constants[71] = 1.0f;

        ex::rhi::BufferDesc uniformDesc{};
        uniformDesc.size = sizeof(constants);
        uniformDesc.usage =
            ex::rhi::BufferUsage::Uniform | ex::rhi::BufferUsage::Dynamic;
        uniformDesc.initialData = constants;
        skyUniformBuffer = device->CreateBuffer(uniformDesc);

        ex::rhi::ShaderDesc shaderDesc{};
        shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
        shaderDesc.data = kSkyVS;
        shaderDesc.size = kSkyVSSize;
        shaderDesc.name = "FullscreenVS";
        skyVertexShader = device->CreateShader(shaderDesc);
        shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
        shaderDesc.data = kSkyPS;
        shaderDesc.size = kSkyPSSize;
        shaderDesc.name = "PSSky";
        skyPixelShader = device->CreateShader(shaderDesc);

        const ex::rhi::DescriptorBinding bindings[] = {
            {0, ex::rhi::DescriptorType::UniformBuffer, 1,
             ex::rhi::ShaderStage::Pixel},
        };
        ex::rhi::PipelineDesc pipelineDesc{};
        pipelineDesc.vertexShader = skyVertexShader.get();
        pipelineDesc.pixelShader = skyPixelShader.get();
        pipelineDesc.descriptorBindings = bindings;
        pipelineDesc.descriptorBindingCount = 1;
        pipelineDesc.depthTest = false;
        pipelineDesc.depthWrite = false;
        skyPipeline = device->CreatePipeline(pipelineDesc);
        if (skyPipeline) {
            skySet = device->CreateDescriptorSet(skyPipeline.get());
        }
        if (skySet) {
            skySet->SetBuffer(0, skyUniformBuffer.get(), 0,
                              sizeof(constants));
        }
        if (!skyUniformBuffer || !skyVertexShader || !skyPixelShader ||
            !skyPipeline || !skySet) {
            std::fprintf(stderr, "Vulkan sky resources failed.\n");
            return 1;
        }
    }

    std::unique_ptr<ex::rhi::Texture> compositeSceneTexture;
    std::unique_ptr<ex::rhi::Texture> compositeBloomTexture;
    std::unique_ptr<ex::rhi::Sampler> compositeSampler;
    std::unique_ptr<ex::rhi::Buffer> compositeUniformBuffer;
    std::unique_ptr<ex::rhi::Shader> compositeVertexShader;
    std::unique_ptr<ex::rhi::Shader> compositePixelShader;
    std::unique_ptr<ex::rhi::Pipeline> compositePipeline;
    std::unique_ptr<ex::rhi::DescriptorSet> compositeSet;

    if (options.composite || options.target || options.bloom || options.frame || options.sceneFrame || options.city) {
        const uint8_t scenePixels[16] = {
            128, 64, 32, 255, 128, 64, 32, 255,
            128, 64, 32, 255, 128, 64, 32, 255,
        };
        const uint8_t bloomPixels[16] = {
            16, 16, 16, 255, 16, 16, 16, 255,
            16, 16, 16, 255, 16, 16, 16, 255,
        };
        ex::rhi::TextureSubresource sceneSubresource{};
        sceneSubresource.data = scenePixels;
        sceneSubresource.width = 2;
        sceneSubresource.height = 2;
        sceneSubresource.rowPitch = 8;
        ex::rhi::TextureDesc sceneDesc{};
        sceneDesc.format = ex::rhi::Format::R8G8B8A8_UNORM;
        sceneDesc.width = 2;
        sceneDesc.height = 2;
        sceneDesc.usage = ex::rhi::TextureUsage::Sampled;
        sceneDesc.subresources = &sceneSubresource;
        sceneDesc.subresourceCount = 1;
        compositeSceneTexture = device->CreateTexture(sceneDesc);

        ex::rhi::TextureSubresource bloomSubresource = sceneSubresource;
        bloomSubresource.data = bloomPixels;
        ex::rhi::TextureDesc bloomDesc = sceneDesc;
        bloomDesc.subresources = &bloomSubresource;
        compositeBloomTexture = device->CreateTexture(bloomDesc);

        ex::rhi::SamplerDesc samplerDesc{};
        samplerDesc.linear = true;
        samplerDesc.clamp = true;
        compositeSampler = device->CreateSampler(samplerDesc);

        float bloomConstants[8] = {
            1.0f / float(options.width), 1.0f / float(options.height),
            1.0f / float(options.width), 1.0f / float(options.height),
            1.0f, 1.0f, 1.0f, 1.0f,
        };
        ex::rhi::BufferDesc uniformDesc{};
        uniformDesc.size = sizeof(bloomConstants);
        uniformDesc.usage =
            ex::rhi::BufferUsage::Uniform | ex::rhi::BufferUsage::Dynamic;
        uniformDesc.initialData = bloomConstants;
        compositeUniformBuffer = device->CreateBuffer(uniformDesc);

        ex::rhi::ShaderDesc shaderDesc{};
        shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
        shaderDesc.data = kCompositeVS;
        shaderDesc.size = kCompositeVSSize;
        shaderDesc.name = "FullscreenVS";
        compositeVertexShader = device->CreateShader(shaderDesc);
        shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
        shaderDesc.data = kCompositePS;
        shaderDesc.size = kCompositePSSize;
        shaderDesc.name = "PSComposite";
        compositePixelShader = device->CreateShader(shaderDesc);

        const ex::rhi::DescriptorBinding bindings[] = {
            {8, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {9, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {16, ex::rhi::DescriptorType::Sampler, 1,
             ex::rhi::ShaderStage::Pixel},
            {1, ex::rhi::DescriptorType::UniformBuffer, 1,
             ex::rhi::ShaderStage::Pixel},
        };
        ex::rhi::PipelineDesc pipelineDesc{};
        pipelineDesc.vertexShader = compositeVertexShader.get();
        pipelineDesc.pixelShader = compositePixelShader.get();
        pipelineDesc.descriptorBindings = bindings;
        pipelineDesc.descriptorBindingCount = 4;
        pipelineDesc.depthTest = false;
        pipelineDesc.depthWrite = false;
        compositePipeline = device->CreatePipeline(pipelineDesc);
        if (compositePipeline) {
            compositeSet = device->CreateDescriptorSet(compositePipeline.get());
        }
        if (compositeSet) {
            compositeSet->SetImage(8, compositeSceneTexture.get());
            compositeSet->SetImage(9, compositeBloomTexture.get());
            compositeSet->SetSampler(16, compositeSampler.get());
            compositeSet->SetBuffer(1, compositeUniformBuffer.get(), 0,
                                    sizeof(bloomConstants));
        }
        if (!compositeSceneTexture || !compositeBloomTexture ||
            !compositeSampler || !compositeUniformBuffer ||
            !compositeVertexShader || !compositePixelShader ||
            !compositePipeline || !compositeSet) {
            std::fprintf(stderr, "Vulkan composite resources failed.\n");
            return 1;
        }
    }

    std::unique_ptr<ex::rhi::Buffer> groundUniformBuffer;
    std::unique_ptr<ex::rhi::Buffer> groundShadowBuffer;
    std::unique_ptr<ex::rhi::Texture> groundSceneDepth;
    std::unique_ptr<ex::rhi::Texture> groundShadowTextures[4];
    std::unique_ptr<ex::rhi::Texture> groundVolumeTexture;
    std::unique_ptr<ex::rhi::Sampler> groundShadowSampler;
    std::unique_ptr<ex::rhi::Sampler> groundVolumeSampler;
    std::unique_ptr<ex::rhi::Shader> groundVertexShader;
    std::unique_ptr<ex::rhi::Shader> groundPixelShader;
    std::unique_ptr<ex::rhi::Pipeline> groundPipeline;
    std::unique_ptr<ex::rhi::DescriptorSet> groundSet;

    if (options.ground || options.scene || options.particles || options.sceneFrame || options.city) {
        using namespace DirectX;
        float constants[112] = {};
        const float cameraY = options.city ? 55.0f : 2.0f;
        const float cameraZ = options.city ? -150.0f : -10.0f;
        const XMMATRIX view = XMMatrixLookAtLH(
            XMVectorSet(0.0f, cameraY, cameraZ, 0.0f),
            XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f),
            XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        const XMMATRIX projection = XMMatrixPerspectiveFovLH(
            XMConvertToRadians(60.0f),
            float(options.width) / float(options.height), 0.1f, 1000.0f);
        const XMMATRIX viewProjection = view * projection;
        const XMMATRIX inverseViewProjection =
            XMMatrixInverse(nullptr, viewProjection);
        XMFLOAT4X4 viewProjectionMatrix{};
        XMFLOAT4X4 inverseViewProjectionMatrix{};
        XMStoreFloat4x4(&viewProjectionMatrix, viewProjection);
        XMStoreFloat4x4(&inverseViewProjectionMatrix, inverseViewProjection);
        std::memcpy(&constants[0], &viewProjectionMatrix,
                    sizeof(viewProjectionMatrix));
        std::memcpy(&constants[16], &inverseViewProjectionMatrix,
                    sizeof(inverseViewProjectionMatrix));

        constants[32] = 0.0f;
        constants[33] = cameraY;
        constants[34] = cameraZ;
        constants[35] = 0.0f;
        constants[36] = 0.3f;
        constants[37] = 0.8f;
        constants[38] = 0.5f;
        constants[39] = 1.0f;
        constants[40] = 1.0f;
        constants[41] = 0.7f;
        constants[42] = 0.44f;
        constants[43] = 1.0f;
        constants[44] = 0.0f;
        constants[45] = 0.0f;
        constants[46] = 0.0f;
        constants[47] = 0.1f;
        constants[48] = 1.0f;
        constants[49] = 1.0f;
        constants[50] = 1.0f;
        constants[51] = 32.0f;
        constants[52] = float(options.width);
        constants[53] = float(options.height);
        constants[54] = 1.0f / float(options.width);
        constants[55] = 1.0f / float(options.height);
        constants[56] = 0.0f;
        constants[57] = 0.0f;
        constants[58] = 0.0f;
        constants[59] = 0.2f;
        constants[60] = 0.0f;
        constants[61] = 0.0f;
        constants[62] = 0.5f;
        constants[63] = 0.0f;
        constants[64] = 0.0f;
        constants[65] = 1.0f / 60.0f;
        constants[66] = 0.0f;
        constants[67] = 0.0f;
        constants[68] = 1.0f;
        constants[69] = 0.0f;
        constants[70] = 0.0f;
        constants[71] = 1.0f;
        constants[72] = 1.0f;
        constants[77] = 1.0f;
        constants[80] = 10.0f;
        constants[81] = 1.0f;
        constants[82] = 0.3f;
        constants[83] = 100.0f;
        constants[84] = 0.1f;
        constants[85] = 1000.0f;
        constants[86] = 0.0f;
        constants[87] = 1.0f;
        constants[88] = 0.0f;
        constants[89] = 0.0f;
        constants[90] = 0.0f;
        constants[91] = 0.0f;

        ex::rhi::BufferDesc uniformDesc{};
        uniformDesc.size = sizeof(constants);
        uniformDesc.usage =
            ex::rhi::BufferUsage::Uniform | ex::rhi::BufferUsage::Dynamic;
        uniformDesc.initialData = constants;
        groundUniformBuffer = device->CreateBuffer(uniformDesc);

        float shadow[80] = {};
        for (int matrix = 0; matrix < 4; ++matrix) {
            const int matrixBase = matrix * 16;
            shadow[matrixBase + 0] = 1.0f;
            shadow[matrixBase + 5] = 1.0f;
            shadow[matrixBase + 10] = 1.0f;
            shadow[matrixBase + 15] = 1.0f;
        }
        shadow[64] = 10.0f;
        shadow[65] = 30.0f;
        shadow[66] = 100.0f;
        shadow[67] = 0.0f;
        shadow[68] = 1.0f / 2048.0f;
        shadow[69] = 1.0f / 1024.0f;
        shadow[70] = 1.0f / 1024.0f;
        shadow[71] = 0.0f;
        shadow[72] = 0.1f;
        shadow[73] = 0.1f;
        shadow[74] = 0.1f;
        shadow[75] = 0.0f;
        shadow[76] = 1.0f / 4096.0f;
        shadow[77] = 1.0f / 4096.0f;
        shadow[78] = 0.0004f;
        shadow[79] = 0.0f;

        ex::rhi::BufferDesc shadowDesc{};
        shadowDesc.size = sizeof(shadow);
        shadowDesc.usage =
            ex::rhi::BufferUsage::Uniform | ex::rhi::BufferUsage::Dynamic;
        shadowDesc.initialData = shadow;
        groundShadowBuffer = device->CreateBuffer(shadowDesc);

        const float lit = 1.0f;
        ex::rhi::TextureSubresource depthSubresource{};
        depthSubresource.data = &lit;
        depthSubresource.width = 1;
        depthSubresource.height = 1;
        depthSubresource.rowPitch = 4;
        ex::rhi::TextureDesc depthDesc{};
        depthDesc.format = ex::rhi::Format::D32_FLOAT;
        depthDesc.width = 1;
        depthDesc.height = 1;
        depthDesc.usage = ex::rhi::TextureUsage::Sampled |
                          ex::rhi::TextureUsage::DepthStencil;
        depthDesc.subresources = &depthSubresource;
        depthDesc.subresourceCount = 1;
        groundSceneDepth = device->CreateTexture(depthDesc);

        const float shadowPixels[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        ex::rhi::TextureSubresource shadowSubresource{};
        shadowSubresource.data = shadowPixels;
        shadowSubresource.width = 2;
        shadowSubresource.height = 2;
        shadowSubresource.rowPitch = 8;
        ex::rhi::TextureDesc shadowTextureDesc = depthDesc;
        shadowTextureDesc.width = 2;
        shadowTextureDesc.height = 2;
        shadowTextureDesc.subresources = &shadowSubresource;
        for (int i = 0; i < 4; ++i) {
            groundShadowTextures[i] = device->CreateTexture(shadowTextureDesc);
        }

        const uint8_t volumePixels[32] = {};
        ex::rhi::TextureSubresource volumeSubresource{};
        volumeSubresource.data = volumePixels;
        volumeSubresource.width = 2;
        volumeSubresource.height = 2;
        volumeSubresource.rowPitch = 8;
        volumeSubresource.slicePitch = 16;
        ex::rhi::TextureDesc volumeDesc{};
        volumeDesc.format = ex::rhi::Format::R8G8B8A8_UNORM;
        volumeDesc.width = 2;
        volumeDesc.height = 2;
        volumeDesc.depth = 2;
        volumeDesc.usage = ex::rhi::TextureUsage::Sampled;
        volumeDesc.subresources = &volumeSubresource;
        volumeDesc.subresourceCount = 1;
        groundVolumeTexture = device->CreateTexture(volumeDesc);

        ex::rhi::SamplerDesc shadowSamplerDesc{};
        shadowSamplerDesc.comparison = true;
        shadowSamplerDesc.linear = true;
        shadowSamplerDesc.border[0] = 1.0f;
        shadowSamplerDesc.border[1] = 1.0f;
        shadowSamplerDesc.border[2] = 1.0f;
        shadowSamplerDesc.border[3] = 1.0f;
        groundShadowSampler = device->CreateSampler(shadowSamplerDesc);

        ex::rhi::SamplerDesc volumeSamplerDesc{};
        volumeSamplerDesc.linear = true;
        volumeSamplerDesc.clamp = true;
        groundVolumeSampler = device->CreateSampler(volumeSamplerDesc);

        ex::rhi::ShaderDesc shaderDesc{};
        shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
        shaderDesc.data = kGroundVS;
        shaderDesc.size = kGroundVSSize;
        shaderDesc.name = "FullscreenVS";
        groundVertexShader = device->CreateShader(shaderDesc);
        shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
        shaderDesc.data = kGroundPS;
        shaderDesc.size = kGroundPSSize;
        shaderDesc.name = "PSGround";
        groundPixelShader = device->CreateShader(shaderDesc);

        const ex::rhi::DescriptorBinding bindings[] = {
            {0, ex::rhi::DescriptorType::UniformBuffer, 1,
             ex::rhi::ShaderStage::Pixel},
            {2, ex::rhi::DescriptorType::UniformBuffer, 1,
             ex::rhi::ShaderStage::Pixel},
            {9, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {10, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {11, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {12, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {13, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {14, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {18, ex::rhi::DescriptorType::Sampler, 1,
             ex::rhi::ShaderStage::Pixel},
            {19, ex::rhi::DescriptorType::Sampler, 1,
             ex::rhi::ShaderStage::Pixel},
        };
        ex::rhi::PipelineDesc pipelineDesc{};
        pipelineDesc.vertexShader = groundVertexShader.get();
        pipelineDesc.pixelShader = groundPixelShader.get();
        pipelineDesc.descriptorBindings = bindings;
        pipelineDesc.descriptorBindingCount = 10;
        pipelineDesc.depthTest = true;
        pipelineDesc.depthWrite = true;
        pipelineDesc.depthCompare = ex::rhi::CompareOp::LessEqual;
        groundPipeline = device->CreatePipeline(pipelineDesc);
        if (groundPipeline) {
            groundSet = device->CreateDescriptorSet(groundPipeline.get());
        }
        if (groundSet) {
            groundSet->SetBuffer(0, groundUniformBuffer.get(), 0,
                                 sizeof(constants));
            groundSet->SetBuffer(2, groundShadowBuffer.get(), 0,
                                 sizeof(shadow));
            groundSet->SetImage(9, groundSceneDepth.get());
            for (int i = 0; i < 4; ++i) {
                groundSet->SetImage(uint32_t(10 + i),
                                    groundShadowTextures[i].get());
            }
            groundSet->SetImage(14, groundVolumeTexture.get());
            groundSet->SetSampler(18, groundShadowSampler.get());
            groundSet->SetSampler(19, groundVolumeSampler.get());
        }
        bool ok = groundUniformBuffer && groundShadowBuffer &&
                  groundSceneDepth && groundVolumeTexture &&
                  groundShadowSampler && groundVolumeSampler &&
                  groundVertexShader && groundPixelShader && groundPipeline &&
                  groundSet;
        for (int i = 0; i < 4; ++i) {
            ok = ok && groundShadowTextures[i];
        }
        if (!ok) {
            std::fprintf(stderr, "Vulkan ground resources failed.\n");
            return 1;
        }
    }

    std::unique_ptr<ex::rhi::Texture> bloomSourceTexture;
    std::unique_ptr<ex::rhi::Texture> bloomDownTarget;
    std::unique_ptr<ex::rhi::Texture> bloomUpTarget;
    std::unique_ptr<ex::rhi::Sampler> bloomSampler;
    std::unique_ptr<ex::rhi::Buffer> bloomUniformBuffer;
    std::unique_ptr<ex::rhi::Shader> bloomVertexShader;
    std::unique_ptr<ex::rhi::Shader> bloomDownShader;
    std::unique_ptr<ex::rhi::Shader> bloomUpShader;
    std::unique_ptr<ex::rhi::Pipeline> bloomDownPipeline;
    std::unique_ptr<ex::rhi::Pipeline> bloomUpPipeline;
    std::unique_ptr<ex::rhi::DescriptorSet> bloomDownSet;
    std::unique_ptr<ex::rhi::DescriptorSet> bloomUpSet;
    std::unique_ptr<ex::rhi::DescriptorSet> bloomCompositeSet;

    if (options.bloom || options.frame || options.sceneFrame || options.city) {
        uint8_t sourcePixels[4 * 4 * 4] = {};
        for (int y = 1; y <= 2; ++y) {
            for (int x = 1; x <= 2; ++x) {
                const size_t index = (size_t(y) * 4 + size_t(x)) * 4;
                sourcePixels[index + 0] = 255;
                sourcePixels[index + 1] = 180;
                sourcePixels[index + 2] = 80;
                sourcePixels[index + 3] = 255;
            }
        }
        ex::rhi::TextureSubresource sourceSubresource{};
        sourceSubresource.data = sourcePixels;
        sourceSubresource.width = 4;
        sourceSubresource.height = 4;
        sourceSubresource.rowPitch = 16;
        ex::rhi::TextureDesc sourceDesc{};
        sourceDesc.format = ex::rhi::Format::R8G8B8A8_UNORM;
        sourceDesc.width = 4;
        sourceDesc.height = 4;
        sourceDesc.usage = ex::rhi::TextureUsage::Sampled;
        sourceDesc.subresources = &sourceSubresource;
        sourceDesc.subresourceCount = 1;
        bloomSourceTexture = device->CreateTexture(sourceDesc);

        ex::rhi::TextureDesc targetDesc{};
        targetDesc.format = ex::rhi::Format::B8G8R8A8_UNORM;
        targetDesc.width = uint32_t(options.width);
        targetDesc.height = uint32_t(options.height);
        targetDesc.usage = ex::rhi::TextureUsage::RenderTarget |
                           ex::rhi::TextureUsage::Sampled;
        bloomDownTarget = device->CreateTexture(targetDesc);
        bloomUpTarget = device->CreateTexture(targetDesc);

        ex::rhi::SamplerDesc samplerDesc{};
        samplerDesc.linear = true;
        samplerDesc.clamp = true;
        bloomSampler = device->CreateSampler(samplerDesc);

        const float bloomSourceWidth = (options.frame || options.sceneFrame || options.city) ? float(options.width) : 4.0f;
        const float bloomSourceHeight =
            (options.frame || options.sceneFrame || options.city) ? float(options.height) : 4.0f;
        float bloomConstants[8] = {
            1.0f / bloomSourceWidth, 1.0f / bloomSourceHeight,
            1.0f / bloomSourceWidth, 1.0f / bloomSourceHeight,
            0.5f, 1.0f, 1.0f, 1.0f,
        };
        ex::rhi::BufferDesc uniformDesc{};
        uniformDesc.size = sizeof(bloomConstants);
        uniformDesc.usage =
            ex::rhi::BufferUsage::Uniform | ex::rhi::BufferUsage::Dynamic;
        uniformDesc.initialData = bloomConstants;
        bloomUniformBuffer = device->CreateBuffer(uniformDesc);

        ex::rhi::ShaderDesc shaderDesc{};
        shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
        shaderDesc.data = kBloomVS;
        shaderDesc.size = kBloomVSSize;
        shaderDesc.name = "FullscreenVS";
        bloomVertexShader = device->CreateShader(shaderDesc);
        shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
        shaderDesc.data = kBloomDownPS;
        shaderDesc.size = kBloomDownPSSize;
        shaderDesc.name = "PSDownsample";
        bloomDownShader = device->CreateShader(shaderDesc);
        shaderDesc.data = kBloomUpPS;
        shaderDesc.size = kBloomUpPSSize;
        shaderDesc.name = "PSUpsample";
        bloomUpShader = device->CreateShader(shaderDesc);

        const ex::rhi::DescriptorBinding bindings[] = {
            {8, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {16, ex::rhi::DescriptorType::Sampler, 1,
             ex::rhi::ShaderStage::Pixel},
            {1, ex::rhi::DescriptorType::UniformBuffer, 1,
             ex::rhi::ShaderStage::Pixel},
        };
        ex::rhi::PipelineDesc pipelineDesc{};
        pipelineDesc.vertexShader = bloomVertexShader.get();
        pipelineDesc.pixelShader = bloomDownShader.get();
        pipelineDesc.descriptorBindings = bindings;
        pipelineDesc.descriptorBindingCount = 3;
        pipelineDesc.depthTest = false;
        pipelineDesc.depthWrite = false;
        bloomDownPipeline = device->CreatePipeline(pipelineDesc);
        pipelineDesc.pixelShader = bloomUpShader.get();
        bloomUpPipeline = device->CreatePipeline(pipelineDesc);

        if (bloomDownPipeline) {
            bloomDownSet = device->CreateDescriptorSet(bloomDownPipeline.get());
        }
        if (bloomUpPipeline) {
            bloomUpSet = device->CreateDescriptorSet(bloomUpPipeline.get());
        }
        if (compositePipeline) {
            bloomCompositeSet =
                device->CreateDescriptorSet(compositePipeline.get());
        }
        if (bloomDownSet) {
            bloomDownSet->SetImage(8, bloomSourceTexture.get());
            bloomDownSet->SetSampler(16, bloomSampler.get());
            bloomDownSet->SetBuffer(1, bloomUniformBuffer.get(), 0,
                                    sizeof(bloomConstants));
        }
        if (bloomUpSet) {
            bloomUpSet->SetImage(8, bloomDownTarget.get());
            bloomUpSet->SetSampler(16, bloomSampler.get());
            bloomUpSet->SetBuffer(1, bloomUniformBuffer.get(), 0,
                                  sizeof(bloomConstants));
        }
        if (bloomCompositeSet) {
            bloomCompositeSet->SetImage(8, bloomUpTarget.get());
            bloomCompositeSet->SetImage(9, compositeBloomTexture.get());
            bloomCompositeSet->SetSampler(16, compositeSampler.get());
            bloomCompositeSet->SetBuffer(1, compositeUniformBuffer.get(), 0,
                                         compositeUniformBuffer->Size());
        }
        if (!bloomSourceTexture || !bloomDownTarget || !bloomUpTarget ||
            !bloomSampler || !bloomUniformBuffer || !bloomVertexShader ||
            !bloomDownShader || !bloomUpShader || !bloomDownPipeline ||
            !bloomUpPipeline || !bloomDownSet || !bloomUpSet ||
            !bloomCompositeSet) {
            std::fprintf(stderr, "Vulkan bloom resources failed.\n");
            return 1;
        }
    }

    std::unique_ptr<ex::rhi::Buffer> sceneVertexBuffer;
    std::unique_ptr<ex::rhi::Buffer> sceneIndexBuffer;
    std::unique_ptr<ex::rhi::Buffer> sceneInstanceBuffer;
    std::unique_ptr<ex::rhi::Buffer> sceneShadowCascadeBuffer;
    std::unique_ptr<ex::rhi::Texture> sceneSurfaceTexture;
    std::unique_ptr<ex::rhi::Shader> sceneVertexShader;
    std::unique_ptr<ex::rhi::Shader> scenePixelShader;
    std::unique_ptr<ex::rhi::Pipeline> scenePipeline;
    std::unique_ptr<ex::rhi::DescriptorSet> sceneSet;
    uint32_t sceneInstanceCount = 1;

    if (options.scene || options.sceneFrame || options.city) {
        const ex::gfx::SceneMeshData buildingMesh =
            ex::gfx::MakeBuildingMesh();

        ex::rhi::BufferDesc vertexDesc{};
        vertexDesc.size = buildingMesh.vertices.size() *
                          sizeof(ex::gfx::SceneMeshVertex);
        vertexDesc.usage = ex::rhi::BufferUsage::Vertex;
        vertexDesc.initialData = buildingMesh.vertices.data();
        sceneVertexBuffer = device->CreateBuffer(vertexDesc);

        ex::rhi::BufferDesc indexDesc{};
        indexDesc.size = buildingMesh.indices.size() * sizeof(uint32_t);
        indexDesc.usage = ex::rhi::BufferUsage::Index;
        indexDesc.initialData = buildingMesh.indices.data();
        sceneIndexBuffer = device->CreateBuffer(indexDesc);

        struct SceneInstance {
            float transform[16];
            float color[4];
            float params[4];
            float state[4];
            float textureOrigin[4];
            float textureScale[4];
        };
        std::vector<SceneInstance> instances;
        if (options.city) {
            ex::sim::BlastModel blast;
            blast.Configure({}, {});
            ex::scene::SceneSettings settings{};
            settings.seed = 42;
            settings.density = 0.6f;
            settings.quality = 1;
            settings.voronoiChunks = false;
            ex::scene::Scene scene;
            scene.Generate(settings, blast);
            for (const ex::scene::SceneObject& object : scene.Objects()) {
                SceneInstance item{};
                const DirectX::XMMATRIX world =
                    DirectX::XMMatrixScaling(object.size.x, object.size.y,
                                             object.size.z) *
                    DirectX::XMMatrixTranslation(object.position.x,
                                                 object.position.y,
                                                 object.position.z);
                DirectX::XMFLOAT4X4 matrix{};
                DirectX::XMStoreFloat4x4(&matrix, world);
                std::memcpy(item.transform, &matrix, sizeof(matrix));
                item.color[0] = object.color.x;
                item.color[1] = object.color.y;
                item.color[2] = object.color.z;
                item.color[3] = object.color.w;
                item.params[3] = float(object.type);
                item.textureOrigin[0] = object.position.x;
                item.textureOrigin[2] = object.position.z;
                item.textureScale[0] = object.size.x;
                item.textureScale[1] = object.size.y;
                item.textureScale[2] = object.size.z;
                item.textureScale[3] = float(object.type);
                instances.push_back(item);
            }
        } else {
            SceneInstance instance{};
            instance.transform[0] = 1.0f;
            instance.transform[5] = 1.0f;
            instance.transform[10] = 1.0f;
            instance.transform[15] = 1.0f;
            instance.color[0] = 1.0f;
            instance.color[1] = 1.0f;
            instance.color[2] = 1.0f;
            instance.color[3] = 1.0f;
            instance.textureScale[0] = 1.0f;
            instance.textureScale[1] = 1.0f;
            instance.textureScale[2] = 1.0f;
            instance.textureScale[3] = 0.0f;
            instances.push_back(instance);
        }
        sceneInstanceCount = uint32_t(instances.size());

        ex::rhi::BufferDesc instanceDesc{};
        instanceDesc.size = instances.size() * sizeof(SceneInstance);
        instanceDesc.usage = ex::rhi::BufferUsage::Storage |
                             ex::rhi::BufferUsage::ShaderRead |
                             ex::rhi::BufferUsage::Dynamic;
        instanceDesc.structureStride = sizeof(SceneInstance);
        instanceDesc.initialData = instances.data();
        sceneInstanceBuffer = device->CreateBuffer(instanceDesc);
        if (sceneInstanceBuffer) {
            void* mapped = sceneInstanceBuffer->Map();
            if (mapped) {
                std::memcpy(mapped, instances.data(),
                            instances.size() * sizeof(SceneInstance));
                sceneInstanceBuffer->Unmap();
            }
        }

        float cascade[16] = {};
        cascade[0] = 1.0f;
        cascade[5] = 1.0f;
        cascade[10] = 1.0f;
        cascade[15] = 1.0f;
        ex::rhi::BufferDesc cascadeDesc{};
        cascadeDesc.size = sizeof(cascade);
        cascadeDesc.usage =
            ex::rhi::BufferUsage::Uniform | ex::rhi::BufferUsage::Dynamic;
        cascadeDesc.initialData = cascade;
        sceneShadowCascadeBuffer = device->CreateBuffer(cascadeDesc);

        const uint8_t surfacePixels[16] = {
            128, 128, 128, 255, 128, 128, 128, 255,
            128, 128, 128, 255, 128, 128, 128, 255,
        };
        ex::rhi::TextureSubresource surfaceSubresource{};
        surfaceSubresource.data = surfacePixels;
        surfaceSubresource.width = 2;
        surfaceSubresource.height = 2;
        surfaceSubresource.rowPitch = 8;
        ex::rhi::TextureDesc surfaceDesc{};
        surfaceDesc.format = ex::rhi::Format::R8G8B8A8_UNORM;
        surfaceDesc.width = 2;
        surfaceDesc.height = 2;
        surfaceDesc.usage = ex::rhi::TextureUsage::Sampled;
        surfaceDesc.subresources = &surfaceSubresource;
        surfaceDesc.subresourceCount = 1;
        sceneSurfaceTexture = device->CreateTexture(surfaceDesc);

        ex::rhi::ShaderDesc shaderDesc{};
        shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
        shaderDesc.data = kSceneVS;
        shaderDesc.size = kSceneVSSize;
        shaderDesc.name = "VSScene";
        sceneVertexShader = device->CreateShader(shaderDesc);
        shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
        shaderDesc.data = kScenePS;
        shaderDesc.size = kScenePSSize;
        shaderDesc.name = "PSScene";
        scenePixelShader = device->CreateShader(shaderDesc);

        const ex::rhi::DescriptorBinding bindings[] = {
            {0, ex::rhi::DescriptorType::UniformBuffer, 1,
             ex::rhi::ShaderStage::Pixel, 3},
            {2, ex::rhi::DescriptorType::UniformBuffer, 1,
             ex::rhi::ShaderStage::Pixel},
            {3, ex::rhi::DescriptorType::UniformBuffer, 1,
             ex::rhi::ShaderStage::Pixel},
            {8, ex::rhi::DescriptorType::StorageBuffer, 1,
             ex::rhi::ShaderStage::Vertex},
            {9, ex::rhi::DescriptorType::StorageBuffer, 1,
             ex::rhi::ShaderStage::Vertex},
            {10, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {11, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {12, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {13, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {14, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {15, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {18, ex::rhi::DescriptorType::Sampler, 1,
             ex::rhi::ShaderStage::Pixel},
            {19, ex::rhi::DescriptorType::Sampler, 1,
             ex::rhi::ShaderStage::Pixel},
            {20, ex::rhi::DescriptorType::Sampler, 1,
             ex::rhi::ShaderStage::Pixel},
        };
        ex::rhi::VertexAttribute attributes[] = {
            {"POSITION", 0, ex::rhi::VertexFormat::Float3, 0, 0},
            {"NORMAL", 0, ex::rhi::VertexFormat::Float3, 12, 0},
            {"COLOR", 0, ex::rhi::VertexFormat::Float3, 24, 0},
        };
        ex::rhi::PipelineDesc pipelineDesc{};
        pipelineDesc.vertexShader = sceneVertexShader.get();
        pipelineDesc.pixelShader = scenePixelShader.get();
        pipelineDesc.vertexAttributes = attributes;
        pipelineDesc.vertexAttributeCount = 3;
        pipelineDesc.vertexStride = sizeof(ex::gfx::SceneMeshVertex);
        pipelineDesc.descriptorBindings = bindings;
        pipelineDesc.descriptorBindingCount = 14;
        pipelineDesc.depthTest = true;
        pipelineDesc.depthWrite = true;
        pipelineDesc.depthCompare = ex::rhi::CompareOp::LessEqual;
        pipelineDesc.cullMode = ex::rhi::CullMode::None;
        scenePipeline = device->CreatePipeline(pipelineDesc);
        if (scenePipeline) {
            sceneSet = device->CreateDescriptorSet(scenePipeline.get());
        }
        if (sceneSet) {
            sceneSet->SetBuffer(0, groundUniformBuffer.get(), 0,
                                groundUniformBuffer->Size());
            sceneSet->SetBuffer(2, groundShadowBuffer.get(), 0,
                                groundShadowBuffer->Size());
            sceneSet->SetBuffer(3, sceneShadowCascadeBuffer.get(), 0,
                                sizeof(cascade));
            sceneSet->SetBuffer(8, sceneInstanceBuffer.get(), 0,
                                sceneInstanceBuffer->Size());
            sceneSet->SetBuffer(9, sceneInstanceBuffer.get(), 0,
                                sceneInstanceBuffer->Size());
            for (int i = 0; i < 4; ++i) {
                sceneSet->SetImage(uint32_t(10 + i),
                                   groundShadowTextures[i].get());
            }
            sceneSet->SetImage(14, sceneSurfaceTexture.get());
            sceneSet->SetImage(15, groundVolumeTexture.get());
            sceneSet->SetSampler(18, groundShadowSampler.get());
            sceneSet->SetSampler(19, groundVolumeSampler.get());
            sceneSet->SetSampler(20, groundVolumeSampler.get());
        }
        if (!sceneVertexBuffer || !sceneIndexBuffer || !sceneInstanceBuffer ||
            !sceneShadowCascadeBuffer || !sceneSurfaceTexture ||
            !sceneVertexShader || !scenePixelShader || !scenePipeline ||
            !sceneSet) {
            std::fprintf(stderr, "Vulkan scene resources failed.\n");
            return 1;
        }
    }

    if (options.frame || options.sceneFrame || options.city) {
        if (bloomDownPipeline) {
            frameBloomDownSet =
                device->CreateDescriptorSet(bloomDownPipeline.get());
        }
        if (frameBloomDownSet) {
            frameBloomDownSet->SetImage(8, frameHdrTarget.get());
            frameBloomDownSet->SetSampler(16, bloomSampler.get());
            frameBloomDownSet->SetBuffer(1, bloomUniformBuffer.get(), 0,
                                         bloomUniformBuffer->Size());
        }
        if (compositePipeline) {
            frameCompositeSet =
                device->CreateDescriptorSet(compositePipeline.get());
        }
        if (frameCompositeSet) {
            frameCompositeSet->SetImage(8, frameHdrTarget.get());
            frameCompositeSet->SetImage(9, bloomUpTarget.get());
            frameCompositeSet->SetSampler(16, compositeSampler.get());
            frameCompositeSet->SetBuffer(1, compositeUniformBuffer.get(), 0,
                                         compositeUniformBuffer->Size());
        }
        if (!frameBloomDownSet || !frameCompositeSet) {
            std::fprintf(stderr, "Vulkan frame composite resources failed.\n");
            return 1;
        }
    }

    std::unique_ptr<ex::rhi::Texture> shockSceneColor;
    std::unique_ptr<ex::rhi::Texture> shockSceneDepth;
    std::unique_ptr<ex::rhi::Sampler> shockSampler;
    std::unique_ptr<ex::rhi::Buffer> shockUniformBuffer;
    std::unique_ptr<ex::rhi::Shader> shockVertexShader;
    std::unique_ptr<ex::rhi::Shader> shockPixelShader;
    std::unique_ptr<ex::rhi::Pipeline> shockPipeline;
    std::unique_ptr<ex::rhi::DescriptorSet> shockSet;

    if (options.shock) {
        uint8_t colorPixels[4 * 4 * 4] = {};
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                const size_t index = (size_t(y) * 4 + size_t(x)) * 4;
                colorPixels[index + 0] = uint8_t(40 + x * 50);
                colorPixels[index + 1] = uint8_t(30 + y * 50);
                colorPixels[index + 2] = uint8_t(120 + (x + y) * 20);
                colorPixels[index + 3] = 255;
            }
        }
        ex::rhi::TextureSubresource colorSubresource{};
        colorSubresource.data = colorPixels;
        colorSubresource.width = 4;
        colorSubresource.height = 4;
        colorSubresource.rowPitch = 16;
        ex::rhi::TextureDesc colorDesc{};
        colorDesc.format = ex::rhi::Format::R8G8B8A8_UNORM;
        colorDesc.width = 4;
        colorDesc.height = 4;
        colorDesc.usage = ex::rhi::TextureUsage::Sampled;
        colorDesc.subresources = &colorSubresource;
        colorDesc.subresourceCount = 1;
        shockSceneColor = device->CreateTexture(colorDesc);

        float depthPixels[16];
        for (float& value : depthPixels) {
            value = 0.5f;
        }
        ex::rhi::TextureSubresource depthSubresource{};
        depthSubresource.data = depthPixels;
        depthSubresource.width = 4;
        depthSubresource.height = 4;
        depthSubresource.rowPitch = 16;
        ex::rhi::TextureDesc depthDesc{};
        depthDesc.format = ex::rhi::Format::D32_FLOAT;
        depthDesc.width = 4;
        depthDesc.height = 4;
        depthDesc.usage = ex::rhi::TextureUsage::Sampled |
                          ex::rhi::TextureUsage::DepthStencil;
        depthDesc.subresources = &depthSubresource;
        depthDesc.subresourceCount = 1;
        shockSceneDepth = device->CreateTexture(depthDesc);

        ex::rhi::SamplerDesc samplerDesc{};
        samplerDesc.linear = true;
        samplerDesc.clamp = true;
        shockSampler = device->CreateSampler(samplerDesc);

        float constants[112] = {};
        constants[0] = 1.0f;
        constants[5] = 1.0f;
        constants[10] = 1.0f;
        constants[15] = 1.0f;
        constants[16] = 1.0f;
        constants[21] = 1.0f;
        constants[26] = 1.0f;
        constants[31] = 1.0f;
        constants[32] = 0.0f;
        constants[33] = 0.0f;
        constants[34] = -5.0f;
        constants[35] = 0.0f;
        constants[36] = 0.3f;
        constants[37] = 0.8f;
        constants[38] = 0.5f;
        constants[39] = 1.0f;
        constants[40] = 1.0f;
        constants[41] = 0.7f;
        constants[42] = 0.44f;
        constants[43] = 1.0f;
        constants[52] = float(options.width);
        constants[53] = float(options.height);
        constants[54] = 1.0f / float(options.width);
        constants[55] = 1.0f / float(options.height);
        constants[64] = 0.0f;
        constants[65] = 1.0f / 60.0f;
        constants[66] = 0.0f;
        constants[67] = 2.0f;
        constants[108] = 0.0f;
        constants[109] = 0.0f;
        constants[110] = 0.0f;
        constants[111] = 2.0f;

        ex::rhi::BufferDesc uniformDesc{};
        uniformDesc.size = sizeof(constants);
        uniformDesc.usage =
            ex::rhi::BufferUsage::Uniform | ex::rhi::BufferUsage::Dynamic;
        uniformDesc.initialData = constants;
        shockUniformBuffer = device->CreateBuffer(uniformDesc);

        ex::rhi::ShaderDesc shaderDesc{};
        shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
        shaderDesc.data = kShockVS;
        shaderDesc.size = kShockVSSize;
        shaderDesc.name = "FullscreenVS";
        shockVertexShader = device->CreateShader(shaderDesc);
        shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
        shaderDesc.data = kShockPS;
        shaderDesc.size = kShockPSSize;
        shaderDesc.name = "PSShock";
        shockPixelShader = device->CreateShader(shaderDesc);

        const ex::rhi::DescriptorBinding bindings[] = {
            {0, ex::rhi::DescriptorType::UniformBuffer, 1,
             ex::rhi::ShaderStage::Pixel},
            {8, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {9, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {16, ex::rhi::DescriptorType::Sampler, 1,
             ex::rhi::ShaderStage::Pixel},
        };
        ex::rhi::PipelineDesc pipelineDesc{};
        pipelineDesc.vertexShader = shockVertexShader.get();
        pipelineDesc.pixelShader = shockPixelShader.get();
        pipelineDesc.descriptorBindings = bindings;
        pipelineDesc.descriptorBindingCount = 4;
        pipelineDesc.depthTest = false;
        pipelineDesc.depthWrite = false;
        shockPipeline = device->CreatePipeline(pipelineDesc);
        if (shockPipeline) {
            shockSet = device->CreateDescriptorSet(shockPipeline.get());
        }
        if (shockSet) {
            shockSet->SetBuffer(0, shockUniformBuffer.get(), 0,
                                sizeof(constants));
            shockSet->SetImage(8, shockSceneColor.get());
            shockSet->SetImage(9, shockSceneDepth.get());
            shockSet->SetSampler(16, shockSampler.get());
        }
        if (!shockSceneColor || !shockSceneDepth || !shockSampler ||
            !shockUniformBuffer || !shockVertexShader || !shockPixelShader ||
            !shockPipeline || !shockSet) {
            std::fprintf(stderr, "Vulkan shock resources failed.\n");
            return 1;
        }
    }

    std::unique_ptr<ex::rhi::Texture> volumeTexture;
    std::unique_ptr<ex::rhi::Texture> volumeAuxTexture;
    std::unique_ptr<ex::rhi::Texture> volumeNoiseTexture;
    std::unique_ptr<ex::rhi::Texture> volumeLightTexture;
    std::unique_ptr<ex::rhi::Texture> volumeDepthTexture;
    std::unique_ptr<ex::rhi::Sampler> volumeSampler;
    std::unique_ptr<ex::rhi::Sampler> volumeNoiseSampler;
    std::unique_ptr<ex::rhi::Buffer> volumeUniformBuffer;
    std::unique_ptr<ex::rhi::Shader> volumeVertexShader;
    std::unique_ptr<ex::rhi::Shader> volumePixelShader;
    std::unique_ptr<ex::rhi::Pipeline> volumePipeline;
    std::unique_ptr<ex::rhi::DescriptorSet> volumeSet;

    if (options.volume) {
        uint8_t volumePixels[4 * 4 * 4 * 4] = {};
        for (int z = 0; z < 4; ++z) {
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const size_t index =
                        ((size_t(z) * 4 + size_t(y)) * 4 + size_t(x)) * 4;
                    volumePixels[index + 0] = 255;
                    volumePixels[index + 1] = 200;
                    volumePixels[index + 2] = 80;
                    volumePixels[index + 3] = 40;
                }
            }
        }
        ex::rhi::TextureSubresource volumeSubresource{};
        volumeSubresource.data = volumePixels;
        volumeSubresource.width = 4;
        volumeSubresource.height = 4;
        volumeSubresource.rowPitch = 16;
        volumeSubresource.slicePitch = 64;
        ex::rhi::TextureDesc volumeDesc{};
        volumeDesc.format = ex::rhi::Format::R8G8B8A8_UNORM;
        volumeDesc.width = 4;
        volumeDesc.height = 4;
        volumeDesc.depth = 4;
        volumeDesc.usage = ex::rhi::TextureUsage::Sampled;
        volumeDesc.subresources = &volumeSubresource;
        volumeDesc.subresourceCount = 1;
        volumeTexture = device->CreateTexture(volumeDesc);
        volumeAuxTexture = device->CreateTexture(volumeDesc);

        uint8_t noisePixels[4 * 4 * 4 * 4] = {};
        for (size_t i = 0; i < sizeof(noisePixels); ++i) {
            noisePixels[i] = uint8_t((i * 37) & 0xff);
        }
        ex::rhi::TextureSubresource noiseSubresource = volumeSubresource;
        noiseSubresource.data = noisePixels;
        ex::rhi::TextureDesc noiseDesc = volumeDesc;
        noiseDesc.subresources = &noiseSubresource;
        volumeNoiseTexture = device->CreateTexture(noiseDesc);

        uint8_t lightPixels[4 * 4 * 4 * 4];
        for (size_t i = 0; i < sizeof(lightPixels); ++i) {
            lightPixels[i] = 255;
        }
        ex::rhi::TextureSubresource lightSubresource = volumeSubresource;
        lightSubresource.data = lightPixels;
        ex::rhi::TextureDesc lightDesc = volumeDesc;
        lightDesc.subresources = &lightSubresource;
        volumeLightTexture = device->CreateTexture(lightDesc);

        uint8_t depthPixels[4 * 4 * 4];
        for (uint8_t& value : depthPixels) {
            value = 230;
        }
        ex::rhi::TextureSubresource depthSubresource{};
        depthSubresource.data = depthPixels;
        depthSubresource.width = 4;
        depthSubresource.height = 4;
        depthSubresource.rowPitch = 16;
        ex::rhi::TextureDesc depthDesc{};
        depthDesc.format = ex::rhi::Format::R8G8B8A8_UNORM;
        depthDesc.width = 4;
        depthDesc.height = 4;
        depthDesc.usage = ex::rhi::TextureUsage::Sampled;
        depthDesc.subresources = &depthSubresource;
        depthDesc.subresourceCount = 1;
        volumeDepthTexture = device->CreateTexture(depthDesc);

        ex::rhi::SamplerDesc samplerDesc{};
        samplerDesc.linear = true;
        samplerDesc.clamp = true;
        volumeSampler = device->CreateSampler(samplerDesc);
        volumeNoiseSampler = device->CreateSampler(samplerDesc);

        float constants[112] = {};
        constants[0] = 1.0f;
        constants[5] = 1.0f;
        constants[10] = 1.0f;
        constants[15] = 1.0f;
        constants[16] = 1.0f;
        constants[21] = 1.0f;
        constants[26] = 1.0f;
        constants[31] = 1.0f;
        constants[32] = 0.05f;
        constants[33] = 0.05f;
        constants[34] = -5.0f;
        constants[35] = 0.0f;
        constants[36] = 0.3f;
        constants[37] = 0.8f;
        constants[38] = 0.5f;
        constants[39] = 1.0f;
        constants[40] = 1.0f;
        constants[41] = 0.7f;
        constants[42] = 0.44f;
        constants[43] = 1.0f;
        constants[44] = 0.0f;
        constants[45] = 0.0f;
        constants[46] = 0.0f;
        constants[47] = 5.0f;
        constants[48] = 2.0f;
        constants[49] = 2.0f;
        constants[50] = 2.0f;
        constants[51] = 32.0f;
        constants[52] = float(options.width);
        constants[53] = float(options.height);
        constants[54] = 1.0f / float(options.width);
        constants[55] = 1.0f / float(options.height);
        constants[60] = 0.0f;
        constants[61] = 0.0f;
        constants[62] = 1.0f;
        constants[63] = 0.0f;
        constants[64] = 0.0f;
        constants[65] = 1.0f / 60.0f;
        constants[66] = 0.0f;
        constants[67] = 0.0f;
        constants[88] = 0.0f;
        constants[89] = 0.0f;
        constants[90] = 0.0f;
        constants[91] = 1.0f;
        constants[92] = 0.1f;
        constants[93] = 0.0f;
        constants[94] = 0.0f;
        constants[95] = 0.0f;
        constants[96] = 1.0f;
        constants[97] = 3.0f;
        constants[98] = 3.0f;
        constants[99] = 0.0f;

        ex::rhi::BufferDesc uniformDesc{};
        uniformDesc.size = sizeof(constants);
        uniformDesc.usage =
            ex::rhi::BufferUsage::Uniform | ex::rhi::BufferUsage::Dynamic;
        uniformDesc.initialData = constants;
        volumeUniformBuffer = device->CreateBuffer(uniformDesc);

        ex::rhi::ShaderDesc shaderDesc{};
        shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
        shaderDesc.data = kVolumeVS;
        shaderDesc.size = kVolumeVSSize;
        shaderDesc.name = "FullscreenVS";
        volumeVertexShader = device->CreateShader(shaderDesc);
        shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
        shaderDesc.data = kVolumePS;
        shaderDesc.size = kVolumePSSize;
        shaderDesc.name = "PSVolume";
        volumePixelShader = device->CreateShader(shaderDesc);

        const ex::rhi::DescriptorBinding bindings[] = {
            {0, ex::rhi::DescriptorType::UniformBuffer, 1,
             ex::rhi::ShaderStage::Pixel},
            {8, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {9, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {10, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {11, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {12, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {16, ex::rhi::DescriptorType::Sampler, 1,
             ex::rhi::ShaderStage::Pixel},
            {17, ex::rhi::DescriptorType::Sampler, 1,
             ex::rhi::ShaderStage::Pixel},
        };
        ex::rhi::PipelineDesc pipelineDesc{};
        pipelineDesc.vertexShader = volumeVertexShader.get();
        pipelineDesc.pixelShader = volumePixelShader.get();
        pipelineDesc.descriptorBindings = bindings;
        pipelineDesc.descriptorBindingCount = 8;
        pipelineDesc.depthTest = false;
        pipelineDesc.depthWrite = false;
        pipelineDesc.blendEnable = true;
        pipelineDesc.premultipliedBlend = true;
        volumePipeline = device->CreatePipeline(pipelineDesc);
        if (volumePipeline) {
            volumeSet = device->CreateDescriptorSet(volumePipeline.get());
        }
        if (volumeSet) {
            volumeSet->SetBuffer(0, volumeUniformBuffer.get(), 0,
                                 sizeof(constants));
            volumeSet->SetImage(8, volumeTexture.get());
            volumeSet->SetImage(9, volumeDepthTexture.get());
            volumeSet->SetImage(10, volumeNoiseTexture.get());
            volumeSet->SetImage(11, volumeAuxTexture.get());
            volumeSet->SetImage(12, volumeLightTexture.get());
            volumeSet->SetSampler(16, volumeSampler.get());
            volumeSet->SetSampler(17, volumeNoiseSampler.get());
        }
        if (!volumeTexture || !volumeAuxTexture || !volumeNoiseTexture ||
            !volumeLightTexture || !volumeDepthTexture || !volumeSampler ||
            !volumeNoiseSampler || !volumeUniformBuffer || !volumeVertexShader ||
            !volumePixelShader || !volumePipeline || !volumeSet) {
            std::fprintf(stderr, "Vulkan volume resources failed.\n");
            return 1;
        }
    }

    std::unique_ptr<ex::rhi::Buffer> particlesInstanceBuffer;
    std::unique_ptr<ex::rhi::Texture> particlesDepthTexture;
    std::unique_ptr<ex::rhi::Shader> particlesVertexShader;
    std::unique_ptr<ex::rhi::Shader> particlesPixelShader;
    std::unique_ptr<ex::rhi::Pipeline> particlesPipeline;
    std::unique_ptr<ex::rhi::DescriptorSet> particlesSet;

    if (options.particles) {
        struct ParticleInstance {
            float position[3];
            float size;
            float color[3];
            float opacity;
            float rotation;
            float variation;
            float padding[2];
        };
        ParticleInstance instance{};
        instance.position[2] = 0.0f;
        instance.size = 1.0f;
        instance.color[0] = 1.0f;
        instance.color[1] = 0.6f;
        instance.color[2] = 0.2f;
        instance.opacity = 1.0f;
        instance.rotation = 0.0f;
        instance.variation = 0.0f;
        instance.padding[0] = 1.0f;
        instance.padding[1] = 0.0f;

        ex::rhi::BufferDesc instanceDesc{};
        instanceDesc.size = sizeof(instance);
        instanceDesc.usage = ex::rhi::BufferUsage::Storage |
                             ex::rhi::BufferUsage::ShaderRead |
                             ex::rhi::BufferUsage::Dynamic;
        instanceDesc.structureStride = sizeof(ParticleInstance);
        instanceDesc.initialData = &instance;
        particlesInstanceBuffer = device->CreateBuffer(instanceDesc);
        if (particlesInstanceBuffer) {
            void* mapped = particlesInstanceBuffer->Map();
            if (mapped) {
                std::memcpy(mapped, &instance, sizeof(instance));
                particlesInstanceBuffer->Unmap();
            }
        }

        std::vector<float> depthData(
            size_t(options.width) * size_t(options.height), 1.0f);
        ex::rhi::TextureSubresource depthSubresource{};
        depthSubresource.data = depthData.data();
        depthSubresource.width = uint32_t(options.width);
        depthSubresource.height = uint32_t(options.height);
        depthSubresource.rowPitch = uint32_t(options.width) * 4;
        ex::rhi::TextureDesc depthDesc{};
        depthDesc.format = ex::rhi::Format::R32_FLOAT;
        depthDesc.width = uint32_t(options.width);
        depthDesc.height = uint32_t(options.height);
        depthDesc.usage = ex::rhi::TextureUsage::Sampled;
        depthDesc.subresources = &depthSubresource;
        depthDesc.subresourceCount = 1;
        particlesDepthTexture = device->CreateTexture(depthDesc);

        ex::rhi::ShaderDesc shaderDesc{};
        shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
        shaderDesc.data = kParticlesVS;
        shaderDesc.size = kParticlesVSSize;
        shaderDesc.name = "VSParticle";
        particlesVertexShader = device->CreateShader(shaderDesc);
        shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
        shaderDesc.data = kParticlesPS;
        shaderDesc.size = kParticlesPSSize;
        shaderDesc.name = "PSAdditive";
        particlesPixelShader = device->CreateShader(shaderDesc);

        const ex::rhi::DescriptorBinding bindings[] = {
            {0, ex::rhi::DescriptorType::UniformBuffer, 1,
             ex::rhi::ShaderStage::Pixel, 3},
            {8, ex::rhi::DescriptorType::StorageBuffer, 1,
             ex::rhi::ShaderStage::Vertex},
            {9, ex::rhi::DescriptorType::SampledImage, 1,
             ex::rhi::ShaderStage::Pixel},
            {16, ex::rhi::DescriptorType::Sampler, 1,
             ex::rhi::ShaderStage::Pixel},
        };
        ex::rhi::PipelineDesc pipelineDesc{};
        pipelineDesc.vertexShader = particlesVertexShader.get();
        pipelineDesc.pixelShader = particlesPixelShader.get();
        pipelineDesc.descriptorBindings = bindings;
        pipelineDesc.descriptorBindingCount = 4;
        pipelineDesc.depthTest = false;
        pipelineDesc.depthWrite = false;
        pipelineDesc.blendEnable = true;
        pipelineDesc.additiveBlend = true;
        particlesPipeline = device->CreatePipeline(pipelineDesc);
        if (particlesPipeline) {
            particlesSet = device->CreateDescriptorSet(particlesPipeline.get());
        }
        if (particlesSet) {
            particlesSet->SetBuffer(0, groundUniformBuffer.get(), 0,
                                    groundUniformBuffer->Size());
            particlesSet->SetBuffer(8, particlesInstanceBuffer.get(), 0,
                                    particlesInstanceBuffer->Size());
            particlesSet->SetImage(9, particlesDepthTexture.get());
            particlesSet->SetSampler(16, groundVolumeSampler.get());
        }
        if (!particlesInstanceBuffer || !particlesDepthTexture ||
            !particlesVertexShader || !particlesPixelShader ||
            !particlesPipeline || !particlesSet) {
            std::fprintf(stderr, "Vulkan particle resources failed.\n");
            return 1;
        }
    }

    std::unique_ptr<ex::rhi::Texture> targetTexture;
    std::unique_ptr<ex::rhi::DescriptorSet> targetCompositeSet;
    if (options.target) {
        ex::rhi::TextureDesc targetDesc{};
        targetDesc.format = ex::rhi::Format::B8G8R8A8_UNORM;
        targetDesc.width = uint32_t(options.width);
        targetDesc.height = uint32_t(options.height);
        targetDesc.usage = ex::rhi::TextureUsage::RenderTarget |
                           ex::rhi::TextureUsage::Sampled;
        targetTexture = device->CreateTexture(targetDesc);

        if (compositePipeline) {
            targetCompositeSet =
                device->CreateDescriptorSet(compositePipeline.get());
        }
        if (targetCompositeSet) {
            targetCompositeSet->SetImage(8, targetTexture.get());
            targetCompositeSet->SetImage(9, compositeBloomTexture.get());
            targetCompositeSet->SetSampler(16, compositeSampler.get());
            targetCompositeSet->SetBuffer(1, compositeUniformBuffer.get(), 0,
                                          compositeUniformBuffer->Size());
        }
        if (!targetTexture || !targetCompositeSet) {
            std::fprintf(stderr, "Vulkan render-target resources failed.\n");
            return 1;
        }
    }

    const int frames =
        options.frames >= 0 ? options.frames : (options.headless ? 3 : 0);
    int rendered = 0;
    for (int frame = 0; frames == 0 || frame < frames; ++frame) {
        if (window && !window->Poll()) {
            break;
        }
        const float t = float(frame % 120) / 120.0f;
        if (options.sceneFrame || options.city) {
            device->BeginFrameTexture(frameHdrTarget.get(),
                                      frameDepthTarget.get(), true, 0.02f,
                                      0.02f, 0.03f, 1.0f);
            device->BindPipeline(skyPipeline.get());
            device->BindDescriptorSet(skySet.get());
            device->Draw(3);

            device->BindPipeline(groundPipeline.get());
            device->BindDescriptorSet(groundSet.get());
            device->Draw(3);

            device->BindPipeline(scenePipeline.get());
            device->BindDescriptorSet(sceneSet.get());
            device->BindVertexBuffer(sceneVertexBuffer.get());
            device->BindIndexBuffer(sceneIndexBuffer.get());
            device->DrawIndexedInstanced(36, sceneInstanceCount);
            device->EndFrame(false);

            device->BeginFrameTexture(bloomDownTarget.get(), 0.0f, 0.0f, 0.0f,
                                      1.0f);
            device->BindPipeline(bloomDownPipeline.get());
            device->BindDescriptorSet(frameBloomDownSet.get());
            device->Draw(3);
            device->EndFrame(false);

            device->BeginFrameTexture(bloomUpTarget.get(), 0.0f, 0.0f, 0.0f,
                                      1.0f);
            device->BindPipeline(bloomUpPipeline.get());
            device->BindDescriptorSet(bloomUpSet.get());
            device->Draw(3);
            device->EndFrame(false);

            device->BeginFrame(0.02f, 0.02f, 0.03f, 1.0f);
            device->BindPipeline(compositePipeline.get());
            device->BindDescriptorSet(frameCompositeSet.get());
            device->Draw(3);
            device->EndFrame(true);
        } else if (options.frame) {
            device->BeginFrameTexture(frameHdrTarget.get(), 0.02f, 0.02f,
                                      0.03f, 1.0f);
            device->BindPipeline(skyPipeline.get());
            device->BindDescriptorSet(skySet.get());
            device->Draw(3);
            device->EndFrame(false);

            device->BeginFrameTexture(bloomDownTarget.get(), 0.0f, 0.0f, 0.0f,
                                      1.0f);
            device->BindPipeline(bloomDownPipeline.get());
            device->BindDescriptorSet(frameBloomDownSet.get());
            device->Draw(3);
            device->EndFrame(false);

            device->BeginFrameTexture(bloomUpTarget.get(), 0.0f, 0.0f, 0.0f,
                                      1.0f);
            device->BindPipeline(bloomUpPipeline.get());
            device->BindDescriptorSet(bloomUpSet.get());
            device->Draw(3);
            device->EndFrame(false);

            device->BeginFrame(0.02f, 0.02f, 0.03f, 1.0f);
            device->BindPipeline(compositePipeline.get());
            device->BindDescriptorSet(frameCompositeSet.get());
            device->Draw(3);
            device->EndFrame(true);
        } else if (options.bloom) {
            device->BeginFrameTexture(bloomDownTarget.get(), 0.0f, 0.0f, 0.0f,
                                      1.0f);
            device->BindPipeline(bloomDownPipeline.get());
            device->BindDescriptorSet(bloomDownSet.get());
            device->Draw(3);
            device->EndFrame(false);

            device->BeginFrameTexture(bloomUpTarget.get(), 0.0f, 0.0f, 0.0f,
                                      1.0f);
            device->BindPipeline(bloomUpPipeline.get());
            device->BindDescriptorSet(bloomUpSet.get());
            device->Draw(3);
            device->EndFrame(false);

            device->BeginFrame(0.02f, 0.02f, 0.03f, 1.0f);
            device->BindPipeline(compositePipeline.get());
            device->BindDescriptorSet(bloomCompositeSet.get());
            device->Draw(3);
            device->EndFrame(true);
        } else if (options.target) {
            device->BeginFrameTexture(targetTexture.get(), 0.05f, 0.05f, 0.1f,
                                      1.0f);
            device->BindPipeline(quadPipeline.get());
            device->BindDescriptorSet(quadSet.get());
            device->BindVertexBuffer(quadVertexBuffer.get());
            device->Draw(6);
            device->EndFrame(false);

            device->BeginFrame(0.02f, 0.02f, 0.03f, 1.0f);
            device->BindPipeline(compositePipeline.get());
            device->BindDescriptorSet(targetCompositeSet.get());
            device->Draw(3);
            device->EndFrame(true);
        } else {
            device->BeginFrame(0.1f + 0.2f * t, 0.2f, 0.4f - 0.2f * t, 1.0f);
            if (pipeline) {
                device->BindPipeline(pipeline.get());
                device->BindVertexBuffer(vertexBuffer.get());
                device->Draw(3);
            } else if (quadPipeline) {
                device->BindPipeline(quadPipeline.get());
                device->BindDescriptorSet(quadSet.get());
                device->BindVertexBuffer(quadVertexBuffer.get());
                device->Draw(6);
            } else if (skyPipeline) {
                device->BindPipeline(skyPipeline.get());
                device->BindDescriptorSet(skySet.get());
                device->Draw(3);
            } else if (compositePipeline) {
                device->BindPipeline(compositePipeline.get());
                device->BindDescriptorSet(compositeSet.get());
                device->Draw(3);
            } else if (shockPipeline) {
                device->BindPipeline(shockPipeline.get());
                device->BindDescriptorSet(shockSet.get());
                device->Draw(3);
            } else if (volumePipeline) {
                device->BindPipeline(volumePipeline.get());
                device->BindDescriptorSet(volumeSet.get());
                device->Draw(3);
            } else if (particlesPipeline) {
                device->BindPipeline(particlesPipeline.get());
                device->BindDescriptorSet(particlesSet.get());
                device->DrawInstanced(6, 1);
            } else if (scenePipeline) {
                device->BindPipeline(scenePipeline.get());
                device->BindDescriptorSet(sceneSet.get());
                device->BindVertexBuffer(sceneVertexBuffer.get());
                device->BindIndexBuffer(sceneIndexBuffer.get());
                device->DrawIndexedInstanced(36, sceneInstanceCount);
            } else if (groundPipeline) {
                device->BindPipeline(groundPipeline.get());
                device->BindDescriptorSet(groundSet.get());
                device->Draw(3);
            }
            device->EndFrame(true);
        }
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

    device->WaitIdle();
    frameCompositeSet.reset();
    frameBloomDownSet.reset();
    frameDepthTarget.reset();
    frameHdrTarget.reset();
    particlesSet.reset();
    particlesPipeline.reset();
    particlesPixelShader.reset();
    particlesVertexShader.reset();
    particlesInstanceBuffer.reset();
    particlesDepthTexture.reset();
    volumeSet.reset();
    volumePipeline.reset();
    volumePixelShader.reset();
    volumeVertexShader.reset();
    volumeUniformBuffer.reset();
    volumeNoiseSampler.reset();
    volumeSampler.reset();
    volumeDepthTexture.reset();
    volumeLightTexture.reset();
    volumeNoiseTexture.reset();
    volumeAuxTexture.reset();
    volumeTexture.reset();
    shockSet.reset();
    shockPipeline.reset();
    shockPixelShader.reset();
    shockVertexShader.reset();
    shockUniformBuffer.reset();
    shockSampler.reset();
    shockSceneDepth.reset();
    shockSceneColor.reset();
    sceneSet.reset();
    scenePipeline.reset();
    scenePixelShader.reset();
    sceneVertexShader.reset();
    sceneSurfaceTexture.reset();
    sceneShadowCascadeBuffer.reset();
    sceneInstanceBuffer.reset();
    sceneIndexBuffer.reset();
    sceneVertexBuffer.reset();
    bloomCompositeSet.reset();
    bloomUpSet.reset();
    bloomDownSet.reset();
    bloomUpPipeline.reset();
    bloomDownPipeline.reset();
    bloomUpShader.reset();
    bloomDownShader.reset();
    bloomVertexShader.reset();
    bloomUniformBuffer.reset();
    bloomSampler.reset();
    bloomUpTarget.reset();
    bloomDownTarget.reset();
    bloomSourceTexture.reset();
    targetCompositeSet.reset();
    targetTexture.reset();
    groundSet.reset();
    groundPipeline.reset();
    groundPixelShader.reset();
    groundVertexShader.reset();
    groundVolumeSampler.reset();
    groundShadowSampler.reset();
    groundVolumeTexture.reset();
    for (auto& texture : groundShadowTextures) {
        texture.reset();
    }
    groundSceneDepth.reset();
    groundShadowBuffer.reset();
    groundUniformBuffer.reset();
    compositeSet.reset();
    compositePipeline.reset();
    compositePixelShader.reset();
    compositeVertexShader.reset();
    compositeUniformBuffer.reset();
    compositeSampler.reset();
    compositeBloomTexture.reset();
    compositeSceneTexture.reset();
    skySet.reset();
    skyPipeline.reset();
    skyPixelShader.reset();
    skyVertexShader.reset();
    skyUniformBuffer.reset();
    quadSet.reset();
    quadPipeline.reset();
    quadPixelShader.reset();
    quadVertexShader.reset();
    quadUniformBuffer.reset();
    quadSampler.reset();
    quadTexture.reset();
    quadVertexBuffer.reset();
    pipeline.reset();
    pixelShader.reset();
    vertexShader.reset();
    vertexBuffer.reset();
    base.reset();
    window.reset();
    return 0;
}
