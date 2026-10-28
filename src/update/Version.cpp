#include "update/Version.h"

#include <charconv>
#include <cctype>

namespace ex::update {

namespace {

bool ParseComponent(std::string_view text, size_t& index, int& value) {
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) {
        ++index;
    }
    const size_t begin = index;
    while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index]))) {
        ++index;
    }
    if (begin == index) {
        return false;
    }
    const std::string_view digits = text.substr(begin, index - begin);
    int parsed = 0;
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
        return false;
    }
    value = parsed;
    return true;
}

} // namespace

bool ParseVersion(std::string_view text, SemanticVersion& out) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return false;
    }

    // Stop at a prerelease or build suffix.
    const size_t suffix = text.find_first_of("-+");
    if (suffix != std::string_view::npos) {
        text = text.substr(0, suffix);
    }

    size_t index = 0;
    SemanticVersion parsed;
    if (!ParseComponent(text, index, parsed.major)) {
        return false;
    }
    if (index >= text.size() || text[index] != '.') {
        return false;
    }
    ++index;
    if (!ParseComponent(text, index, parsed.minor)) {
        return false;
    }
    if (index >= text.size() || text[index] != '.') {
        return false;
    }
    ++index;
    if (!ParseComponent(text, index, parsed.patch)) {
        return false;
    }
    if (index != text.size()) {
        return false;
    }
    out = parsed;
    return true;
}

int CompareVersions(const SemanticVersion& a, const SemanticVersion& b) {
    if (a.major != b.major) {
        return a.major < b.major ? -1 : 1;
    }
    if (a.minor != b.minor) {
        return a.minor < b.minor ? -1 : 1;
    }
    if (a.patch != b.patch) {
        return a.patch < b.patch ? -1 : 1;
    }
    return 0;
}

std::string VersionToString(const SemanticVersion& version) {
    return std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
           std::to_string(version.patch);
}

} // namespace ex::update
