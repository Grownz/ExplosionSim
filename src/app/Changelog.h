#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ex::app {

// One release section of CHANGELOG.md (Keep a Changelog format).
struct ChangelogSection {
    std::string version; // "0.7.0", without the leading 'v'
    std::string date;    // "2026-10-27", may be empty for Unreleased
    std::string body;    // markdown after the heading, without the heading
};

// Reads a UTF-8 text file. Returns an empty string if the file cannot be read.
std::string ReadTextFile(const std::wstring& path);

// Splits a changelog into release sections. Link definitions at the bottom of
// the file and the preamble before the first "## [" heading are ignored.
std::vector<ChangelogSection> ParseChangelog(std::string_view text);

// Finds a release by version, tolerating a leading 'v'. Returns nullptr if the
// version is not present.
const ChangelogSection* FindSection(const std::vector<ChangelogSection>& sections,
                                    std::string_view version);

// Body of the requested release, or an empty string.
std::string CurrentReleaseNotes(const std::vector<ChangelogSection>& sections,
                                std::string_view version);

} // namespace ex::app
