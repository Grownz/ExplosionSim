// Tests for the Linux update helper: SHA-256, URL parsing, tar.gz extraction
// and the install swap. The helper binary is passed as argv[1] by CTest.

#include "update/Sha256.h"
#include "update/TarGz.h"
#include "update/HttpClient.h"

#include "miniz.h"

#include <sys/wait.h>
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

namespace {

int gFailures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAILED: %s\n", message);
        ++gFailures;
    }
}

void WriteOctal(uint8_t* field, size_t size, uint64_t value) {
    std::snprintf(reinterpret_cast<char*>(field), size, "%0*llo",
                  int(size - 1), static_cast<unsigned long long>(value));
}

void WriteString(uint8_t* field, size_t size, const std::string& text) {
    std::memcpy(field, text.data(), std::min(size, text.size()));
}

void AppendTarEntry(std::vector<uint8_t>& tar, const std::string& name,
                    char type, uint32_t mode, const std::string& content) {
    uint8_t header[512] = {};
    WriteString(header, 100, name);
    WriteOctal(header + 100, 8, mode);
    WriteOctal(header + 108, 8, 0);
    WriteOctal(header + 116, 8, 0);
    WriteOctal(header + 124, 12, content.size());
    WriteOctal(header + 136, 12, 0);
    std::memset(header + 148, ' ', 8);
    header[156] = uint8_t(type);
    std::memcpy(header + 257, "ustar", 5);
    std::memcpy(header + 263, "00", 2);
    uint32_t checksum = 0;
    for (uint8_t byte : header) {
        checksum += byte;
    }
    std::snprintf(reinterpret_cast<char*>(header + 148), 8, "%06o", checksum);
    header[154] = 0;
    header[155] = ' ';
    tar.insert(tar.end(), header, header + 512);
    tar.insert(tar.end(), content.begin(), content.end());
    const size_t remainder = content.size() % 512;
    if (remainder != 0) {
        tar.insert(tar.end(), 512 - remainder, 0);
    }
}

std::vector<uint8_t> Gzip(const std::vector<uint8_t>& data) {
    size_t compressedSize = 0;
    void* compressed = tdefl_compress_mem_to_heap(
        data.data(), data.size(), &compressedSize, TDEFL_DEFAULT_MAX_PROBES);
    std::vector<uint8_t> output = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 3};
    if (compressed) {
        const auto* bytes = static_cast<const uint8_t*>(compressed);
        output.insert(output.end(), bytes, bytes + compressedSize);
        mz_free(compressed);
    }
    const uint32_t crc = uint32_t(mz_crc32(0, data.data(), data.size()));
    const uint32_t size = uint32_t(data.size());
    for (int i = 0; i < 4; ++i) {
        output.push_back(uint8_t(crc >> (i * 8)));
    }
    for (int i = 0; i < 4; ++i) {
        output.push_back(uint8_t(size >> (i * 8)));
    }
    return output;
}

std::vector<uint8_t> MakePackageArchive() {
    std::vector<uint8_t> tar;
    AppendTarEntry(tar, "pkg/", '5', 0755, {});
    AppendTarEntry(
        tar, "pkg/ExplosionSim", '0', 0755,
        "#!/bin/sh\n"
        "dir=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\n"
        "printf 'relaunched' > \"$dir/marker.txt\"\n");
    AppendTarEntry(tar, "pkg/assets/", '5', 0755, {});
    AppendTarEntry(tar, "pkg/assets/new.txt", '0', 0644, "new asset\n");
    tar.insert(tar.end(), 1024, 0);
    return Gzip(tar);
}

int RunHelper(const std::string& helper, const std::vector<std::string>& args) {
    const pid_t child = fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(helper.c_str()));
        for (const std::string& argument : args) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execv(helper.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void TestSha256() {
    Check(ex::update::Sha256Hex("", 0) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA-256 of an empty input");
    Check(ex::update::Sha256Hex("abc", 3) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 of 'abc'");
    Check(ex::update::NormalizeSha256(
              "sha256:314E74E684EFCE798E58DACCDA15B6C8B19947E4F184448713100E4308"
              "7D2FF6") ==
              "314e74e684efce798e58daccda15b6c8b19947e4f184448713100e43087d2ff6",
          "digest normalization strips the sha256 prefix");
}

void TestUrlParsing() {
    ex::update::Url url;
    Check(ex::update::ParseUrl("https://api.github.com/repos/x/y", url),
          "parse the API URL");
    Check(url.host == "api.github.com" && url.path == "/repos/x/y" &&
              url.port == 443,
          "API URL fields");
    Check(ex::update::ParseUrl("https://github.com:8443/a", url) &&
              url.port == 8443,
          "parse an explicit port");
    Check(!ex::update::ParseUrl("http://github.com/a", url),
          "reject plain HTTP");
    Check(!ex::update::ParseUrl("https:///a", url), "reject an empty host");
    Check(ex::update::IsAllowedHost("release-assets.githubusercontent.com"),
          "allow the GitHub asset CDN");
    Check(!ex::update::IsAllowedHost("example.com"), "reject other hosts");
}

void TestExtraction() {
    const fs::path directory =
        fs::temp_directory_path() / ("explosion-extract-" + std::to_string(getpid()));
    std::error_code ec;
    fs::remove_all(directory, ec);
    const std::vector<uint8_t> archive = MakePackageArchive();
    std::string error;
    Check(ex::update::ExtractTarGzBuffer(archive.data(), archive.size(),
                                         directory.string(), error),
          "extract a tar.gz archive");
    if (!error.empty()) {
        std::printf("[test] extraction error: %s\n", error.c_str());
    }
    Check(fs::is_regular_file(directory / "pkg/ExplosionSim", ec),
          "extract the executable");
    Check(fs::is_regular_file(directory / "pkg/assets/new.txt", ec),
          "extract nested assets");
    const fs::perms permissions =
        fs::status(directory / "pkg/ExplosionSim", ec).permissions();
    Check((permissions & fs::perms::owner_exec) != fs::perms::none,
          "preserve the executable bit");
    fs::remove_all(directory, ec);
}

void TestApply(const std::string& helper) {
    if (helper.empty() || !fs::is_regular_file(helper)) {
        std::printf("SKIP: the updater binary was not provided.\n");
        return;
    }
    const fs::path base =
        fs::temp_directory_path() / ("explosion-apply-" + std::to_string(getpid()));
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path install = base / "install";
    fs::create_directories(install / "assets", ec);
    {
        std::ofstream oldBinary(install / "ExplosionSim");
        oldBinary << "old binary\n";
        std::ofstream oldAsset(install / "assets/old.txt");
        oldAsset << "old\n";
        std::ofstream settings(install / "settings.ini");
        settings << "chargeMassKg=0.2\n";
    }
    const std::vector<uint8_t> archive = MakePackageArchive();
    const fs::path archivePath = base / "update.tar.gz";
    {
        std::ofstream file(archivePath, std::ios::binary);
        file.write(reinterpret_cast<const char*>(archive.data()),
                   std::streamsize(archive.size()));
    }
    const std::string digest = ex::update::Sha256FileHex(archivePath.string());

    const int exitCode = RunHelper(
        helper, {"--apply", archivePath.string(), install.string(),
                 "ExplosionSim", "--pid", "999999", "--sha256", digest});
    Check(exitCode == 0, "apply exits successfully");

    {
        std::ifstream binary(install / "ExplosionSim");
        std::string firstLine;
        std::getline(binary, firstLine);
        Check(firstLine == "#!/bin/sh", "replace the executable");
    }
    Check(fs::is_regular_file(install / "assets/new.txt", ec),
          "install the new assets");
    Check(!fs::exists(install / "assets/old.txt", ec),
          "drop the previous assets");
    Check(fs::is_regular_file(install / "settings.ini", ec),
          "keep a portable settings.ini");

    for (int i = 0; i < 50 && !fs::exists(install / "marker.txt", ec); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    Check(fs::is_regular_file(install / "marker.txt", ec),
          "relaunch the installed application");
    fs::remove_all(base, ec);
}

} // namespace

int main(int argc, char** argv) {
    TestSha256();
    TestUrlParsing();
    TestExtraction();
    TestApply(argc > 1 ? argv[1] : "");
    if (gFailures > 0) {
        std::printf("%d updater check(s) failed.\n", gFailures);
        return 1;
    }
    std::printf("All updater tests passed.\n");
    return 0;
}
