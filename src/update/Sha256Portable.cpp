// Portable SHA-256 used by the Linux update helper. The Windows build keeps
// the BCrypt implementation in Sha256.cpp; this file provides the same API on
// non-Windows platforms.

#include "update/Sha256.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ex::update {

namespace {

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline uint32_t RotateRight(uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

class Sha256 {
public:
    Sha256() {
        state_[0] = 0x6a09e667u;
        state_[1] = 0xbb67ae85u;
        state_[2] = 0x3c6ef372u;
        state_[3] = 0xa54ff53au;
        state_[4] = 0x510e527fu;
        state_[5] = 0x9b05688cu;
        state_[6] = 0x1f83d9abu;
        state_[7] = 0x5be0cd19u;
    }

    void Update(const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        totalBytes_ += size;
        while (size > 0) {
            const size_t space = 64 - bufferSize_;
            const size_t take = size < space ? size : space;
            std::memcpy(buffer_ + bufferSize_, bytes, take);
            bufferSize_ += take;
            bytes += take;
            size -= take;
            if (bufferSize_ == 64) {
                Transform(buffer_);
                bufferSize_ = 0;
            }
        }
    }

    std::string FinalHex() {
        const uint64_t bitLength = totalBytes_ * 8;
        const uint8_t pad = 0x80;
        Update(&pad, 1);
        const uint8_t zero = 0;
        while (bufferSize_ != 56) {
            Update(&zero, 1);
        }
        uint8_t lengthBytes[8];
        for (int i = 0; i < 8; ++i) {
            lengthBytes[i] = uint8_t(bitLength >> (56 - i * 8));
        }
        Update(lengthBytes, sizeof(lengthBytes));

        static const char* kDigits = "0123456789abcdef";
        std::string text;
        text.reserve(64);
        for (uint32_t word : state_) {
            for (int shift = 28; shift >= 0; shift -= 4) {
                text.push_back(kDigits[(word >> shift) & 0x0F]);
            }
        }
        return text;
    }

private:
    void Transform(const uint8_t* block) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(block[i * 4]) << 24) |
                   (uint32_t(block[i * 4 + 1]) << 16) |
                   (uint32_t(block[i * 4 + 2]) << 8) |
                   uint32_t(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = RotateRight(w[i - 15], 7) ^
                                RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = RotateRight(w[i - 2], 17) ^
                                RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state_[0];
        uint32_t b = state_[1];
        uint32_t c = state_[2];
        uint32_t d = state_[3];
        uint32_t e = state_[4];
        uint32_t f = state_[5];
        uint32_t g = state_[6];
        uint32_t h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 =
                RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
            const uint32_t s0 =
                RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    uint32_t state_[8]{};
    uint8_t buffer_[64]{};
    size_t bufferSize_ = 0;
    uint64_t totalBytes_ = 0;
};

} // namespace

std::string Sha256Hex(const void* data, size_t size) {
    Sha256 hash;
    if (data && size > 0) {
        hash.Update(data, size);
    }
    return hash.FinalHex();
}

std::string Sha256Hex(const std::vector<uint8_t>& data) {
    return Sha256Hex(data.data(), data.size());
}

std::string Sha256FileHex(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    Sha256 hash;
    std::vector<char> buffer(64 * 1024);
    while (file) {
        file.read(buffer.data(), std::streamsize(buffer.size()));
        const std::streamsize read = file.gcount();
        if (read > 0) {
            hash.Update(buffer.data(), size_t(read));
        }
    }
    return hash.FinalHex();
}

std::string Sha256FileHex(const std::wstring& path) {
    return Sha256FileHex(std::filesystem::path(path).string());
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
            digest.push_back(
                char(std::tolower(static_cast<unsigned char>(character))));
            if (digest.size() == 64) {
                break;
            }
        } else if (!digest.empty()) {
            break;
        }
    }
    return digest.size() == 64 ? digest : std::string();
}

bool IsSha256Hex(const std::string& text) {
    return text.size() == 64 && NormalizeSha256(text) == text;
}

} // namespace ex::update
