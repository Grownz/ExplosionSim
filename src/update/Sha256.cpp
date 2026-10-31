#include "update/Sha256.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <cctype>
#include <vector>

namespace ex::update {

namespace {

std::string ToHex(const unsigned char* digest, size_t size) {
    static const char* kDigits = "0123456789abcdef";
    std::string text;
    text.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        text.push_back(kDigits[digest[i] >> 4]);
        text.push_back(kDigits[digest[i] & 0x0F]);
    }
    return text;
}

class Sha256Hash {
public:
    Sha256Hash() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            return;
        }
        DWORD objectSize = 0;
        DWORD result = 0;
        if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                              &result, 0) < 0) {
            return;
        }
        object_.resize(objectSize);
        if (BCryptCreateHash(algorithm_, &hash_, object_.data(), objectSize, nullptr, 0, 0) < 0) {
            return;
        }
        valid_ = true;
    }

    ~Sha256Hash() {
        if (hash_) {
            BCryptDestroyHash(hash_);
        }
        if (algorithm_) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    Sha256Hash(const Sha256Hash&) = delete;
    Sha256Hash& operator=(const Sha256Hash&) = delete;

    bool Valid() const { return valid_; }

    bool Update(const void* data, size_t size) {
        if (!valid_ || size == 0) {
            return valid_;
        }
        return BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)),
                              static_cast<ULONG>(size), 0) >= 0;
    }

    std::string Finish() {
        if (!valid_) {
            return {};
        }
        unsigned char digest[32]{};
        if (BCryptFinishHash(hash_, digest, sizeof(digest), 0) < 0) {
            return {};
        }
        valid_ = false;
        return ToHex(digest, sizeof(digest));
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<UCHAR> object_;
    bool valid_ = false;
};

} // namespace

std::string Sha256Hex(const void* data, size_t size) {
    Sha256Hash hash;
    if (!hash.Valid() || !hash.Update(data, size)) {
        return {};
    }
    return hash.Finish();
}

std::string Sha256Hex(const std::vector<uint8_t>& data) {
    return Sha256Hex(data.data(), data.size());
}

std::string Sha256FileHex(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }

    Sha256Hash hash;
    if (!hash.Valid()) {
        CloseHandle(file);
        return {};
    }

    std::vector<unsigned char> buffer(64 * 1024);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), DWORD(buffer.size()), &read, nullptr)) {
            CloseHandle(file);
            return {};
        }
        if (read == 0) {
            break;
        }
        if (!hash.Update(buffer.data(), size_t(read))) {
            CloseHandle(file);
            return {};
        }
    }
    CloseHandle(file);
    return hash.Finish();
}

std::string Sha256FileHex(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), size);
    wide.resize(size_t(size - 1));
    return Sha256FileHex(wide);
}

std::string NormalizeSha256(const std::string& text) {
    std::string value = text;

    // Strip a "sha256:" prefix (case-insensitive).
    const std::string prefix = "sha256:";
    if (value.size() >= prefix.size()) {
        bool matches = true;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(value[i])) != prefix[i]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            value.erase(0, prefix.size());
        }
    }

    std::string digest;
    digest.reserve(64);
    for (char character : value) {
        if (std::isxdigit(static_cast<unsigned char>(character))) {
            digest.push_back(char(std::tolower(static_cast<unsigned char>(character))));
            if (digest.size() == 64) {
                break;
            }
        } else if (!digest.empty()) {
            // Stop at the filename or whitespace after the digest.
            break;
        }
    }
    return digest.size() == 64 ? digest : std::string{};
}

bool IsSha256Hex(const std::string& text) {
    if (text.size() != 64) {
        return false;
    }
    for (char character : text) {
        if (!std::isxdigit(static_cast<unsigned char>(character))) {
            return false;
        }
    }
    return true;
}

} // namespace ex::update
