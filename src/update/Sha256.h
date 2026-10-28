#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ex::update {

// Lower-case SHA-256 hex digest (64 characters).
std::string Sha256Hex(const void* data, size_t size);
std::string Sha256Hex(const std::vector<uint8_t>& data);
std::string Sha256FileHex(const std::wstring& path);

// Accepts "sha256:<hex>", a bare 64-character hex digest, or a checksum file
// line ("<hex>  filename"). Returns the lower-case digest or an empty string.
std::string NormalizeSha256(const std::string& text);
bool IsSha256Hex(const std::string& text);

} // namespace ex::update
