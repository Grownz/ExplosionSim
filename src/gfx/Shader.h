#pragma once

#include "gfx/GfxDevice.h"

#include <string>

namespace ex::gfx {

// Compiles an HLSL file from disk. Includes are resolved relative to the
// shader directory. Errors are returned in `errors` and logged. `defines` may
// be null; it is passed through to D3DCompileFromFile.
ComPtr<ID3DBlob> CompileShaderFile(const std::wstring& path,
                                   const char* entryPoint,
                                   const char* profile,
                                   std::string& errors,
                                   const D3D_SHADER_MACRO* defines = nullptr);

// Resolves a runtime asset path by probing next to the executable and the
// current working directory. Returns an empty string if nothing was found.
std::wstring ResolveAssetPath(const std::wstring& relativePath);

// Directory that contains the shaders (with trailing backslash), or empty.
std::wstring ShaderDirectory();

} // namespace ex::gfx
