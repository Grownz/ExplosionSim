// Minimal Vulkan bring-up test for milestone M2. It creates a device, clears a
// window or an offscreen BGRA8 target and (headless) writes a PPM image.

#include "gfx/rhi/vulkan/VulkanRhi.h"
#include "platform/Platform.h"
#include "TriangleShaders.h"
#include "QuadShaders.h"
#include "SkyShaders.h"
#include "CompositeShaders.h"

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

    if (options.quad) {
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

    if (options.sky) {
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

    if (options.composite) {
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

    const int frames =
        options.frames >= 0 ? options.frames : (options.headless ? 3 : 0);
    int rendered = 0;
    for (int frame = 0; frames == 0 || frame < frames; ++frame) {
        if (window && !window->Poll()) {
            break;
        }
        const float t = float(frame % 120) / 120.0f;
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
        }
        device->EndFrame(true);
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
