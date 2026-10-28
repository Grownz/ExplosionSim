// Standalone update helper. It waits for the running application to exit,
// extracts the downloaded release zip into a staging directory, replaces the
// executable and assets in the target directory (keeping settings.ini) and
// relaunches the application. On failure it restores the previous files.
//
// The helper is launched from a temporary copy so it can replace its own
// installed copy without hitting a file lock.

#include <windows.h>
#include <shellapi.h>

#include "miniz.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::ofstream gLog;

std::string Narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string narrow(size_t(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), narrow.data(), length,
                        nullptr, nullptr);
    return narrow;
}

void Log(const std::string& text) {
    if (gLog) {
        gLog << text << "\n";
        gLog.flush();
    }
}

fs::path TempDirectory() {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD length = GetTempPathW(DWORD(MAX_PATH * 2), buffer);
    if (length > 0) {
        return fs::path(std::wstring(buffer, length));
    }
    std::error_code error;
    return fs::temp_directory_path(error);
}

struct Options {
    DWORD pid = 0;
    fs::path zip;
    fs::path target;
    std::wstring exeName = L"ExplosionSim.exe";
    bool restart = true;
};

bool ParseOptions(Options& options, std::wstring& error) {
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments) {
        error = L"cannot parse the command line";
        return false;
    }

    for (int i = 1; i < argumentCount; ++i) {
        const std::wstring argument = arguments[i];
        const auto next = [&](std::wstring& value) {
            if (i + 1 >= argumentCount) {
                return false;
            }
            value = arguments[++i];
            return true;
        };

        if (argument == L"--pid") {
            std::wstring value;
            if (!next(value)) {
                error = L"missing value for --pid";
                break;
            }
            options.pid = DWORD(std::wcstoul(value.c_str(), nullptr, 10));
        } else if (argument == L"--zip") {
            std::wstring value;
            if (!next(value)) {
                error = L"missing value for --zip";
                break;
            }
            options.zip = value;
        } else if (argument == L"--target") {
            std::wstring value;
            if (!next(value)) {
                error = L"missing value for --target";
                break;
            }
            options.target = value;
        } else if (argument == L"--exe") {
            std::wstring value;
            if (!next(value)) {
                error = L"missing value for --exe";
                break;
            }
            options.exeName = value;
        } else if (argument == L"--no-restart") {
            options.restart = false;
        }
    }

    LocalFree(arguments);
    if (!error.empty()) {
        return false;
    }
    if (options.zip.empty() || options.target.empty()) {
        error = L"--zip and --target are required";
        return false;
    }
    return true;
}

void WaitForProcess(DWORD pid) {
    if (pid == 0) {
        return;
    }
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) {
        return;
    }
    WaitForSingleObject(process, 120000);
    CloseHandle(process);
}

size_t ReadCallback(void* opaque, mz_uint64 offset, void* buffer, size_t size) {
    auto* stream = static_cast<std::ifstream*>(opaque);
    stream->clear();
    stream->seekg(std::streamoff(offset));
    if (!*stream) {
        return 0;
    }
    stream->read(static_cast<char*>(buffer), std::streamsize(size));
    return size_t(stream->gcount());
}

size_t WriteCallback(void* opaque, mz_uint64, const void* buffer, size_t size) {
    auto* stream = static_cast<std::ofstream*>(opaque);
    stream->write(static_cast<const char*>(buffer), std::streamsize(size));
    return stream->good() ? size : 0;
}

bool IsSafeRelativePath(const fs::path& path) {
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }
    for (const fs::path& part : path) {
        if (part == L"..") {
            return false;
        }
    }
    return true;
}

bool ExtractZip(const fs::path& zipPath, const fs::path& staging, std::wstring& error) {
    std::ifstream input(zipPath, std::ios::binary);
    if (!input) {
        error = L"cannot open the update zip";
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    input.seekg(0);
    if (size <= 0) {
        error = L"the update zip is empty";
        return false;
    }

    mz_zip_archive archive{};
    archive.m_pRead = ReadCallback;
    archive.m_pIO_opaque = &input;
    if (!mz_zip_reader_init(&archive, mz_uint64(size), 0)) {
        error = L"the update zip is invalid";
        return false;
    }

    bool ok = true;
    const mz_uint fileCount = mz_zip_reader_get_num_files(&archive);
    for (mz_uint i = 0; i < fileCount && ok; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, i, &stat)) {
            error = L"invalid zip entry";
            ok = false;
            break;
        }

        const fs::path relative = fs::path(stat.m_filename);
        if (!IsSafeRelativePath(relative)) {
            error = L"zip entry escapes the staging directory";
            ok = false;
            break;
        }

        const fs::path output = staging / relative;
        std::error_code ec;
        if (mz_zip_reader_is_file_a_directory(&archive, i)) {
            fs::create_directories(output, ec);
            continue;
        }
        fs::create_directories(output.parent_path(), ec);

        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = L"cannot create " + output.wstring();
            ok = false;
            break;
        }
        if (!mz_zip_reader_extract_to_callback(&archive, i, WriteCallback, &stream, 0)) {
            error = L"cannot extract " + output.wstring();
            ok = false;
        }
        stream.close();
    }

    mz_zip_reader_end(&archive);
    return ok;
}

bool CopyTree(const fs::path& source, const fs::path& target, std::wstring& error) {
    std::error_code ec;
    fs::create_directories(target, ec);
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(source, ec)) {
        if (ec) {
            error = L"cannot read the extracted update";
            return false;
        }
        const fs::path relative = fs::relative(entry.path(), source, ec);
        if (ec) {
            error = L"cannot resolve an update path";
            return false;
        }
        if (relative.filename() == L"settings.ini") {
            continue; // never overwrite user settings
        }
        const fs::path destination = target / relative;
        if (entry.is_directory()) {
            fs::create_directories(destination, ec);
            continue;
        }
        fs::create_directories(destination.parent_path(), ec);
        fs::copy_file(entry.path(), destination, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = L"cannot copy " + destination.wstring();
            return false;
        }
    }
    return true;
}

bool Install(const fs::path& staging, const Options& options, std::wstring& error) {
    const fs::path targetExe = options.target / options.exeName;
    const fs::path targetAssets = options.target / L"assets";
    const fs::path backup = options.target / L".update-backup";
    std::error_code ec;

    fs::remove_all(backup, ec);
    fs::create_directories(backup, ec);
    if (ec) {
        error = L"cannot create the backup directory";
        return false;
    }

    if (fs::exists(targetExe)) {
        fs::copy_file(targetExe, backup / options.exeName, fs::copy_options::overwrite_existing,
                      ec);
        if (ec) {
            fs::remove_all(backup, ec);
            error = L"cannot back up the executable";
            return false;
        }
    }
    if (fs::exists(targetAssets)) {
        fs::copy(targetAssets, backup / L"assets", fs::copy_options::recursive |
                                                     fs::copy_options::overwrite_existing,
                 ec);
        if (ec) {
            fs::remove_all(backup, ec);
            error = L"cannot back up the assets";
            return false;
        }
    }

    fs::remove(targetExe, ec);
    fs::remove_all(targetAssets, ec);
    if (!CopyTree(staging, options.target, error)) {
        // Roll back to the previous installation.
        std::error_code rollbackError;
        fs::remove(targetExe, rollbackError);
        fs::remove_all(targetAssets, rollbackError);
        if (fs::exists(backup / options.exeName)) {
            fs::copy_file(backup / options.exeName, targetExe,
                          fs::copy_options::overwrite_existing, rollbackError);
        }
        if (fs::exists(backup / L"assets")) {
            fs::copy(backup / L"assets", targetAssets,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                     rollbackError);
        }
        return false;
    }

    fs::remove_all(backup, ec);
    return true;
}

bool Relaunch(const Options& options) {
    const fs::path executable = options.target / options.exeName;
    if (!fs::exists(executable)) {
        return false;
    }
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        options.target.c_str(), &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    const fs::path logPath = TempDirectory() / L"ExplosionSimUpdater.log";
    gLog.open(logPath, std::ios::app);
    Log("updater: start");

    Options options;
    std::wstring error;
    if (!ParseOptions(options, error)) {
        Log("updater: invalid arguments: " + Narrow(error));
        return 2;
    }

    const auto relaunch = [&options]() {
        if (options.restart) {
            Relaunch(options);
        }
    };

    Log("updater: waiting for the application to exit");
    WaitForProcess(options.pid);

    const fs::path staging =
        TempDirectory() / (L"ExplosionSimUpdate-" + std::to_wstring(options.pid));
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) {
        Log("updater: cannot create the staging directory");
        return 3;
    }

    Log("updater: extracting " + Narrow(options.zip.wstring()));
    if (!ExtractZip(options.zip, staging, error)) {
        Log("updater: extraction failed: " + Narrow(error));
        fs::remove_all(staging, ec);
        relaunch();
        return 4;
    }
    if (!fs::exists(staging / options.exeName)) {
        Log("updater: the update has no executable");
        fs::remove_all(staging, ec);
        relaunch();
        return 5;
    }

    Log("updater: installing");
    if (!Install(staging, options, error)) {
        Log("updater: install failed: " + Narrow(error));
        fs::remove_all(staging, ec);
        relaunch();
        return 6;
    }

    fs::remove_all(staging, ec);
    fs::remove(options.zip, ec);
    if (options.restart) {
        Log("updater: relaunching");
        relaunch();
    }
    Log("updater: done");
    return 0;
}
