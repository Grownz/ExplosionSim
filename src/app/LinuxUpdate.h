#pragma once

#include <mutex>
#include <string>

namespace ex::app {

struct LinuxUpdateInfo {
    std::string version;
    std::string tag;
    std::string name;
    std::string notes;
    std::string asset;
    std::string url;
    std::string sha256;
    std::string published;
    unsigned long long size = 0;
    bool valid = false;
};

enum class LinuxUpdatePhase {
    Idle,
    Checking,
    UpToDate,
    Available,
    Downloading,
    Downloaded,
    Failed
};

struct LinuxUpdateSnapshot {
    LinuxUpdatePhase phase = LinuxUpdatePhase::Idle;
    LinuxUpdateInfo info;
    std::string status;
    std::string error;
    std::string archivePath;
    unsigned long long downloaded = 0;
    unsigned long long total = 0;
    float progress = 0.0f;
};

// Drives the standalone ExplosionSimUpdater helper. The helper owns all
// network, archive and install work; the application only parses its stdout.
class LinuxUpdater {
public:
    ~LinuxUpdater();

    void Initialize(const std::string& helperPath,
                    const std::string& installDirectory,
                    const std::string& executableName,
                    const std::string& cacheDirectory);
    bool Available() const { return !helper_.empty(); }

    void CheckAsync();
    void DownloadAsync();
    bool InstallAndRestart();
    void Poll();
    LinuxUpdateSnapshot Get() const;
    void Shutdown();

private:
    void StartCheck();
    void StartDownload();
    void StopProcess();

    std::string helper_;
    std::string installDirectory_;
    std::string executableName_;
    std::string cacheDirectory_;
    std::string archivePath_;

    mutable std::mutex mutex_;
    LinuxUpdateSnapshot snapshot_;

    int pipe_ = -1;
    int child_ = -1;
    bool processRunning_ = false;
    std::string lineBuffer_;
};

} // namespace ex::app
