#include "gfx/Shader.h"

#include "core/Log.h"

#include <d3dcompiler.h>

#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ex::gfx {

namespace {

// Resolves #include directives relative to the shader directory.
class IncludeHandler final : public ID3DInclude {
public:
    explicit IncludeHandler(std::wstring directory) : directory_(std::move(directory)) {}

    HRESULT __stdcall Open(D3D_INCLUDE_TYPE includeType, LPCSTR fileName, LPCVOID,
                           LPCVOID* outData, UINT* outSize) override {
        std::string name(fileName);
        std::wstring wide(name.begin(), name.end());
        const std::wstring full = directory_ + wide;

        HANDLE file = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            LogError("Shader include not found: %ls", full.c_str());
            return E_FAIL;
        }

        LARGE_INTEGER size{};
        GetFileSizeEx(file, &size);
        auto* data = new char[size_t(size.QuadPart)];
        DWORD read = 0;
        const BOOL ok = ReadFile(file, data, DWORD(size.QuadPart), &read, nullptr);
        CloseHandle(file);
        if (!ok) {
            delete[] data;
            return E_FAIL;
        }

        *outData = data;
        *outSize = read;
        return S_OK;
    }

    HRESULT __stdcall Close(LPCVOID data) override {
        delete[] static_cast<const char*>(data);
        return S_OK;
    }

private:
    std::wstring directory_;
};

std::wstring ExecutableDirectory() {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
    if (length == 0) {
        return {};
    }
    std::wstring path(buffer, length);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash + 1);
}

bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

std::wstring ResolveAssetPath(const std::wstring& relativePath) {
    std::vector<std::wstring> candidates;
    const std::wstring exeDir = ExecutableDirectory();
    if (!exeDir.empty()) {
        candidates.push_back(exeDir + relativePath);
    }
    candidates.push_back(relativePath);

    wchar_t cwd[MAX_PATH * 2]{};
    if (GetCurrentDirectoryW(DWORD(std::size(cwd)), cwd) > 0) {
        candidates.push_back(std::wstring(cwd) + L"\\" + relativePath);
        candidates.push_back(std::wstring(cwd) + L"\\..\\" + relativePath);
    }

    for (const auto& candidate : candidates) {
        if (FileExists(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::wstring ShaderDirectory() {
    const std::wstring path = ResolveAssetPath(L"assets\\shaders\\volume.hlsl");
    if (path.empty()) {
        return {};
    }
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash + 1);
}

ComPtr<ID3DBlob> CompileShaderFile(const std::wstring& path, const char* entryPoint,
                                   const char* profile, std::string& errors) {
    const std::wstring shaderDir = ShaderDirectory();
    IncludeHandler includeHandler(shaderDir.empty() ? L".\\" : shaderDir);

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> messages;
    const HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, &includeHandler, entryPoint, profile,
                                          flags, 0, &code, &messages);
    if (FAILED(hr)) {
        if (messages) {
            errors.assign(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize());
        } else {
            errors = "Unknown shader compilation error.";
        }
        LogError("Shader compilation failed (%ls, entry %s):\n%s", path.c_str(), entryPoint, errors.c_str());
        return nullptr;
    }

    if (messages && messages->GetBufferSize() > 1) {
        LogWarn("Shader warnings (%ls):\n%.*s", path.c_str(), int(messages->GetBufferSize()),
                static_cast<const char*>(messages->GetBufferPointer()));
    }
    return code;
}

} // namespace ex::gfx
