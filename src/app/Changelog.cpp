#include "app/Changelog.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ex::app {

namespace {

std::string Trim(std::string value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool IsLinkDefinition(const std::string& line) {
    if (line.empty() || line.front() != '[') {
        return false;
    }
    const size_t close = line.find("]:");
    return close != std::string::npos;
}

std::string NormaliseVersion(std::string version) {
    version = Trim(std::move(version));
    if (!version.empty() && (version.front() == 'v' || version.front() == 'V')) {
        version.erase(version.begin());
    }
    return version;
}

} // namespace

std::string ReadTextFile(const std::wstring& path) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    std::string text = buffer.str();

    // Drop a UTF-8 BOM if present.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

std::vector<ChangelogSection> ParseChangelog(std::string_view text) {
    std::vector<ChangelogSection> sections;
    std::istringstream stream{std::string(text)};
    std::string line;

    ChangelogSection current;
    bool inSection = false;
    std::vector<std::string> body;

    const auto finish = [&]() {
        if (!inSection) {
            return;
        }
        while (!body.empty() && Trim(body.front()).empty()) {
            body.erase(body.begin());
        }
        while (!body.empty() && Trim(body.back()).empty()) {
            body.pop_back();
        }
        std::ostringstream joined;
        for (size_t i = 0; i < body.size(); ++i) {
            if (i > 0) {
                joined << '\n';
            }
            joined << body[i];
        }
        current.body = joined.str();
        sections.push_back(current);
        body.clear();
        inSection = false;
    };

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.rfind("## [", 0) == 0) {
            finish();
            const size_t close = line.find(']', 4);
            if (close == std::string::npos) {
                continue;
            }
            current = ChangelogSection{};
            current.version = NormaliseVersion(line.substr(4, close - 4));

            // Optional " - YYYY-MM-DD" after the closing bracket.
            const size_t dash = line.find(" - ", close + 1);
            if (dash != std::string::npos) {
                current.date = Trim(line.substr(dash + 3));
            }
            inSection = true;
            continue;
        }

        if (!inSection) {
            continue;
        }
        if (IsLinkDefinition(line)) {
            // Keep-alive link definitions are not part of the release body.
            continue;
        }
        body.push_back(line);
    }
    finish();
    return sections;
}

const ChangelogSection* FindSection(const std::vector<ChangelogSection>& sections,
                                    std::string_view version) {
    const std::string wanted = NormaliseVersion(std::string(version));
    for (const ChangelogSection& section : sections) {
        if (NormaliseVersion(section.version) == wanted) {
            return &section;
        }
    }
    return nullptr;
}

std::string CurrentReleaseNotes(const std::vector<ChangelogSection>& sections,
                                std::string_view version) {
    const ChangelogSection* section = FindSection(sections, version);
    return section ? section->body : std::string{};
}

} // namespace ex::app
