// Linux update helper. The application spawns it for three jobs:
//
//   --check                       query the GitHub releases API (JSON on stdout)
//   --download <url> <dest>       stream an asset and verify its SHA-256
//   --apply <archive> <dir> <exe> wait for the app to exit, extract the
//                                 tar.gz, swap the install directory and
//                                 relaunch
//
// It is a separate binary so the application keeps no HTTP or TLS dependency.

#include "update/HttpClient.h"
#include "update/Sha256.h"
#include "update/TarGz.h"

#include <nlohmann/json.hpp>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char* kApiUrl =
    "https://api.github.com/repos/Grownz/ExplosionSim/releases/latest";
constexpr const char* kAssetSuffix = "linux-x86_64.tar.gz";

std::string CacheDirectory() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        return std::string(xdg) + "/explosionsim";
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::string(home) + "/.cache/explosionsim";
    }
    return "/tmp/explosionsim";
}

void LogLine(const std::string& text) {
    std::error_code ec;
    fs::create_directories(CacheDirectory(), ec);
    std::ofstream log(CacheDirectory() + "/updater.log",
                      std::ios::app);
    if (log) {
        log << text << "\n";
    }
}

void Emit(const std::string& line) {
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int Check() {
    ex::update::HttpRequest request;
    request.userAgent = "ExplosionSim-Updater";
    request.headers = {"Accept: application/vnd.github+json",
                       "X-GitHub-Api-Version: 2022-11-28"};
    std::string body;
    std::string error;
    if (!ex::update::HttpGet(kApiUrl, request, body, error)) {
        Emit(json{{"ok", false}, {"error", error}}.dump());
        return 1;
    }

    json response;
    try {
        response = json::parse(body);
    } catch (const std::exception&) {
        Emit(json{{"ok", false}, {"error", "invalid GitHub response"}}.dump());
        return 1;
    }
    if (response.contains("message")) {
        Emit(json{{"ok", false}, {"error", response["message"]}}.dump());
        return 1;
    }

    const std::string tag = response.value("tag_name", "");
    if (tag.empty()) {
        Emit(json{{"ok", false}, {"error", "the release has no tag"}}.dump());
        return 1;
    }
    std::string version = tag;
    if (!version.empty() && version.front() == 'v') {
        version.erase(version.begin());
    }

    std::string assetName;
    std::string assetUrl;
    std::string sha256;
    unsigned long long assetSize = 0;
    for (const json& asset : response.value("assets", json::array())) {
        const std::string name = asset.value("name", "");
        if (name.size() < std::strlen(kAssetSuffix) ||
            name.compare(name.size() - std::strlen(kAssetSuffix),
                         std::strlen(kAssetSuffix), kAssetSuffix) != 0) {
            continue;
        }
        assetName = name;
        assetUrl = asset.value("browser_download_url", "");
        sha256 = ex::update::NormalizeSha256(asset.value("digest", ""));
        assetSize = asset.value("size", 0ull);
        break;
    }
    if (assetName.empty() || assetUrl.empty()) {
        Emit(json{{"ok", false},
                  {"error", "the release has no Linux asset"}}
                 .dump());
        return 1;
    }
    ex::update::Url parsed;
    if (!ex::update::ParseUrl(assetUrl, parsed) ||
        !ex::update::IsAllowedHost(parsed.host)) {
        Emit(json{{"ok", false},
                  {"error", "the asset URL is not a trusted GitHub host"}}
                 .dump());
        return 1;
    }

    const json result = {
        {"ok", true},
        {"version", version},
        {"tag", tag},
        {"name", response.value("name", "")},
        {"notes", response.value("body", "")},
        {"asset", assetName},
        {"url", assetUrl},
        {"sha256", sha256},
        {"size", assetSize},
        {"published", response.value("published_at", "")},
    };
    Emit(result.dump());
    return 0;
}

int Download(const std::string& url, const std::string& destination,
             const std::string& expectedSha256) {
    ex::update::Url parsed;
    if (!ex::update::ParseUrl(url, parsed) ||
        !ex::update::IsAllowedHost(parsed.host)) {
        Emit("error the update URL is not a trusted GitHub host");
        return 1;
    }
    const std::string part = destination + ".part";
    ex::update::HttpRequest request;
    request.userAgent = "ExplosionSim-Updater";
    std::string error;
    const bool ok = ex::update::HttpDownload(
        url, part, request,
        [](unsigned long long done, unsigned long long total) {
            Emit("progress " + std::to_string(done) + " " +
                 std::to_string(total));
            return true;
        },
        error);
    if (!ok) {
        std::error_code ec;
        fs::remove(part, ec);
        Emit("error " + error);
        return 1;
    }
    if (!expectedSha256.empty()) {
        const std::string actual = ex::update::Sha256FileHex(part);
        if (actual.empty() || actual != expectedSha256) {
            std::error_code ec;
            fs::remove(part, ec);
            Emit("error the download digest does not match");
            return 1;
        }
    }
    std::error_code ec;
    fs::rename(part, destination, ec);
    if (ec) {
        Emit("error cannot finalize the download");
        return 1;
    }
    Emit("done");
    return 0;
}

bool WaitForProcess(pid_t pid, int seconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (kill(pid, 0) != 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return kill(pid, 0) != 0;
}

bool Relaunch(const fs::path& executable) {
    const pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        setsid();
        chdir("/");
        // argv[0] is the full path so a script can resolve its own directory.
        execl(executable.c_str(), executable.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
}

int Apply(const std::string& archive, const std::string& installDirectory,
          const std::string& executableName, pid_t parentPid,
          const std::string& expectedSha256) {
    LogLine("apply: waiting for the application to exit");
    Emit("waiting");
    if (!WaitForProcess(parentPid, 30)) {
        Emit("error the application did not exit");
        return 1;
    }

    if (!expectedSha256.empty()) {
        const std::string actual = ex::update::Sha256FileHex(archive);
        if (actual.empty() || actual != expectedSha256) {
            Emit("error the archive digest does not match");
            return 1;
        }
    }

    const fs::path install(installDirectory);
    const std::string suffix = ".update-" + std::to_string(parentPid);
    const fs::path staging(installDirectory + suffix);
    const fs::path backup(installDirectory + ".old-" + std::to_string(parentPid));
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);

    Emit("extracting");
    std::string error;
    if (!ex::update::ExtractTarGz(archive, staging.string(), error)) {
        fs::remove_all(staging, ec);
        Emit("error " + error);
        return 1;
    }

    // The package wraps everything in a top-level directory.
    fs::path root = staging;
    {
        std::vector<fs::path> entries;
        for (const auto& entry : fs::directory_iterator(staging, ec)) {
            entries.push_back(entry.path());
        }
        if (entries.size() == 1 && fs::is_directory(entries.front())) {
            root = entries.front();
        }
    }
    const fs::path newExecutable = root / executableName;
    if (!fs::is_regular_file(newExecutable, ec)) {
        fs::remove_all(staging, ec);
        Emit("error the archive does not contain " + executableName);
        return 1;
    }
    fs::permissions(newExecutable,
                    fs::perms::owner_all | fs::perms::group_read |
                        fs::perms::group_exec | fs::perms::others_read |
                        fs::perms::others_exec,
                    fs::perm_options::replace, ec);

    // Keep a portable settings.ini that lives next to the executable.
    const fs::path oldSettings = install / "settings.ini";
    const fs::path newSettings = root / "settings.ini";
    if (fs::is_regular_file(oldSettings, ec) && !fs::exists(newSettings, ec)) {
        fs::copy_file(oldSettings, newSettings, ec);
    }

    Emit("swapping");
    fs::remove_all(backup, ec);
    fs::rename(install, backup, ec);
    if (ec) {
        fs::remove_all(staging, ec);
        Emit("error cannot move the current installation aside");
        return 1;
    }
    fs::rename(root, install, ec);
    if (ec) {
        fs::rename(backup, install, ec);
        fs::remove_all(staging, ec);
        Emit("error cannot install the new version");
        return 1;
    }
    fs::remove_all(staging, ec);
    fs::remove_all(backup, ec);
    std::error_code removeError;
    fs::remove(archive, removeError);

    Emit("relaunching");
    if (!Relaunch(install / executableName)) {
        Emit("error cannot restart the application");
        return 1;
    }
    LogLine("apply: updated to the new version");
    Emit("done");
    return 0;
}

void Usage() {
    std::printf(
        "ExplosionSimUpdater - Linux update helper\n"
        "\n"
        "  --check\n"
        "  --download <url> <destination> [--sha256 <hex>]\n"
        "  --apply <archive> <install-dir> <executable> --pid <pid> "
        "[--sha256 <hex>]\n"
        "  --version\n");
}

} // namespace

int main(int argc, char** argv) {
    chdir("/");
    std::string mode;
    std::string first;
    std::string second;
    std::string third;
    std::string sha256;
    pid_t parentPid = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--check" || argument == "--version") {
            mode = argument;
        } else if (argument == "--download") {
            mode = argument;
            if (i + 2 < argc) {
                first = argv[++i];
                second = argv[++i];
            }
        } else if (argument == "--apply") {
            mode = argument;
            if (i + 3 < argc) {
                first = argv[++i];
                second = argv[++i];
                third = argv[++i];
            }
        } else if (argument == "--sha256" && i + 1 < argc) {
            sha256 = ex::update::NormalizeSha256(argv[++i]);
        } else if (argument == "--pid" && i + 1 < argc) {
            parentPid = pid_t(std::atoi(argv[++i]));
        } else if (argument == "--help" || argument == "-h") {
            Usage();
            return 0;
        }
    }

    if (mode == "--version") {
        std::printf("ExplosionSimUpdater\n");
        return 0;
    }
    if (mode == "--check") {
        return Check();
    }
    if (mode == "--download") {
        if (first.empty() || second.empty()) {
            Usage();
            return 2;
        }
        return Download(first, second, sha256);
    }
    if (mode == "--apply") {
        if (first.empty() || second.empty() || third.empty() || parentPid <= 0) {
            Usage();
            return 2;
        }
        return Apply(first, second, third, parentPid, sha256);
    }
    Usage();
    return 2;
}
