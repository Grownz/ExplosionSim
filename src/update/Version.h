#pragma once

#include <string>
#include <string_view>

namespace ex::update {

// Semantic version with the three numeric components used by the project.
struct SemanticVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

// Parses "1.2.3" or "v1.2.3". A trailing prerelease/build suffix (for example
// "-beta.1" or "+build") is ignored. Returns false for malformed input.
bool ParseVersion(std::string_view text, SemanticVersion& out);

// Returns <0 if a < b, 0 if equal, >0 if a > b.
int CompareVersions(const SemanticVersion& a, const SemanticVersion& b);

std::string VersionToString(const SemanticVersion& version);

} // namespace ex::update
