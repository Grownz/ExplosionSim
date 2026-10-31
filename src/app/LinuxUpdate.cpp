#include "app/LinuxUpdate.h"

#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ex::app {

namespace {

std::string Trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

} // namespace

LinuxUpdater::~LinuxUpdater() {
    Shutdown();
}

void LinuxUpdater::Initialize(const std::string& helperPath,
                              const std::string& installDirectory,
                              const std::string& executableName,
                              const std::string& cacheDirectory) {
    helper_ = helperPath;
    installDirectory_ = installDirectory;
    executableName_ = executableName;
    cacheDirectory_ = cacheDirectory;
    std::error_code error;
    std::filesystem::create_directories(cacheDirectory_, error);
}

LinuxUpdateSnapshot LinuxUpdater::Get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void LinuxUpdater::StopProcess() {
    if (child_ > 0) {
        if (processRunning_) {
            kill(child_, SIGTERM);
        }
        int status = 0;
        waitpid(child_, &status, 0);
        child_ = -1;
    }
    if (pipe_ >= 0) {
        close(pipe_);
        pipe_ = -1;
    }
    processRunning_ = false;
    lineBuffer_.clear();
}

void LinuxUpdater::Shutdown() {
    StopProcess();
}

void LinuxUpdater::CheckAsync() {
    if (helper_.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.phase == LinuxUpdatePhase::Checking ||
            snapshot_.phase == LinuxUpdatePhase::Downloading) {
            return;
        }
        snapshot_.phase = LinuxUpdatePhase::Checking;
        snapshot_.status = "Checking for updates...";
        snapshot_.error.clear();
    }
    StartCheck();
}

void LinuxUpdater::DownloadAsync() {
    if (helper_.empty()) {
        return;
    }
    LinuxUpdateInfo info;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        info = snapshot_.info;
        if (!info.valid) {
            return;
        }
        snapshot_.phase = LinuxUpdatePhase::Downloading;
        snapshot_.status = "Downloading...";
        snapshot_.downloaded = 0;
        snapshot_.total = info.size;
        snapshot_.progress = 0.0f;
    }
    archivePath_ = cacheDirectory_ + "/" + info.asset;
    StartDownload();
}

bool LinuxUpdater::InstallAndRestart() {
    if (helper_.empty() || archivePath_.empty()) {
        return false;
    }
    LinuxUpdateInfo info;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        info = snapshot_.info;
    }
    if (!std::filesystem::is_regular_file(archivePath_)) {
        return false;
    }

    const std::string logPath = cacheDirectory_ + "/updater.log";
    const pid_t child = fork();
    if (child < 0) {
        LogError("Update: cannot start the helper.");
        return false;
    }
    if (child == 0) {
        setsid();
        const int log = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND,
                             0644);
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
            close(log);
        }
        const std::string pid = std::to_string(getppid());
        execl(helper_.c_str(), helper_.c_str(), "--apply", archivePath_.c_str(),
              installDirectory_.c_str(), executableName_.c_str(), "--pid",
              pid.c_str(), "--sha256", info.sha256.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
}

void LinuxUpdater::StartCheck() {
    StopProcess();
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) {
        return;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (child == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl(helper_.c_str(), helper_.c_str(), "--check",
              static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    pipe_ = fds[0];
    child_ = child;
    processRunning_ = true;
}

void LinuxUpdater::StartDownload() {
    StopProcess();
    LinuxUpdateInfo info;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        info = snapshot_.info;
    }
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) {
        return;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (child == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl(helper_.c_str(), helper_.c_str(), "--download", info.url.c_str(),
              archivePath_.c_str(), "--sha256", info.sha256.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    pipe_ = fds[0];
    child_ = child;
    processRunning_ = true;
}

void LinuxUpdater::Poll() {
    if (pipe_ < 0) {
        return;
    }
    char buffer[4096];
    for (;;) {
        const ssize_t read = ::read(pipe_, buffer, sizeof(buffer));
        if (read > 0) {
            lineBuffer_.append(buffer, size_t(read));
            continue;
        }
        if (read == 0) {
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        break;
    }

    size_t newline = 0;
    while ((newline = lineBuffer_.find('\n')) != std::string::npos) {
        const std::string line = Trim(lineBuffer_.substr(0, newline));
        lineBuffer_.erase(0, newline + 1);
        if (line.empty()) {
            continue;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!line.empty() && line.front() == '{') {
            try {
                const nlohmann::json result = nlohmann::json::parse(line);
                if (!result.value("ok", false)) {
                    snapshot_.phase = LinuxUpdatePhase::Failed;
                    snapshot_.error = result.value("error", "update check failed");
                    snapshot_.status = snapshot_.error;
                    continue;
                }
                snapshot_.info.version = result.value("version", "");
                snapshot_.info.tag = result.value("tag", "");
                snapshot_.info.name = result.value("name", "");
                snapshot_.info.notes = result.value("notes", "");
                snapshot_.info.asset = result.value("asset", "");
                snapshot_.info.url = result.value("url", "");
                snapshot_.info.sha256 = result.value("sha256", "");
                snapshot_.info.published = result.value("published", "");
                snapshot_.info.size = result.value("size", 0ull);
                snapshot_.info.valid = !snapshot_.info.version.empty() &&
                                       !snapshot_.info.url.empty();
                if (!snapshot_.info.valid) {
                    snapshot_.phase = LinuxUpdatePhase::Failed;
                    snapshot_.error = "the release is incomplete";
                    snapshot_.status = snapshot_.error;
                    continue;
                }
                snapshot_.phase = LinuxUpdatePhase::Available;
                snapshot_.status =
                    "Version " + snapshot_.info.version + " is available.";
            } catch (const std::exception&) {
                snapshot_.phase = LinuxUpdatePhase::Failed;
                snapshot_.error = "invalid update response";
                snapshot_.status = snapshot_.error;
            }
            continue;
        }
        if (line.rfind("progress ", 0) == 0) {
            unsigned long long done = 0;
            unsigned long long total = 0;
            if (std::sscanf(line.c_str() + 9, "%llu %llu", &done, &total) == 2) {
                snapshot_.downloaded = done;
                snapshot_.total = total;
                snapshot_.progress =
                    total > 0 ? float(double(done) / double(total)) : 0.0f;
                snapshot_.status = "Downloading...";
            }
            continue;
        }
        if (line == "done") {
            if (snapshot_.phase == LinuxUpdatePhase::Downloading) {
                snapshot_.phase = LinuxUpdatePhase::Downloaded;
                snapshot_.status = "The update is ready to install.";
                snapshot_.archivePath = archivePath_;
                snapshot_.progress = 1.0f;
            }
            continue;
        }
        if (line.rfind("error ", 0) == 0) {
            snapshot_.phase = LinuxUpdatePhase::Failed;
            snapshot_.error = line.substr(6);
            snapshot_.status = snapshot_.error;
        }
    }

    int status = 0;
    const pid_t result = waitpid(child_, &status, WNOHANG);
    if (result == child_) {
        close(pipe_);
        pipe_ = -1;
        child_ = -1;
        processRunning_ = false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.phase == LinuxUpdatePhase::Checking) {
            snapshot_.phase = LinuxUpdatePhase::Failed;
            snapshot_.error = "the update check returned no result";
            snapshot_.status = snapshot_.error;
        } else if (snapshot_.phase == LinuxUpdatePhase::Downloading) {
            snapshot_.phase = LinuxUpdatePhase::Failed;
            snapshot_.error = "the download was interrupted";
            snapshot_.status = snapshot_.error;
        }
    }
}

} // namespace ex::app
