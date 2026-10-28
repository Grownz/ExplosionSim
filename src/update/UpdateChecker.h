#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace ex::update {

enum class UpdateStatus {
    Idle,
    Checking,
    UpToDate,
    Available,
    Downloading,
    Downloaded,
    Failed
};

struct ReleaseInfo {
    std::string version;   // "0.10.0"
    std::string tag;       // "v0.10.0"
    std::string name;      // "Explosion Simulator 0.10.0"
    std::string notes;     // release body (markdown)
    std::string assetName; // "ExplosionSim-0.10.0-win64.zip"
    std::string assetUrl;
    std::string sha256;    // normalized lower-case digest, may be empty
    std::string sha256Url; // optional <zip>.sha256 asset
    std::string publishedAt;
    unsigned long long assetSize = 0;
};

// Background GitHub release checker and downloader. All network work happens on
// one worker thread; the UI polls GetSnapshot() and never blocks.
class UpdateChecker {
public:
    struct Snapshot {
        UpdateStatus status = UpdateStatus::Idle;
        ReleaseInfo release;
        std::string error;
        unsigned long long downloaded = 0;
        unsigned long long total = 0;
        float progress = 0.0f;
        bool manual = false;
        bool verified = false;
        bool downloadFailed = false;
    };

    UpdateChecker();
    ~UpdateChecker();

    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    // Queues a check of the latest GitHub release. `skippedVersion` suppresses
    // an update the user chose to ignore. `proxy` is an optional host:port;
    // empty uses the system proxy. `manual` is reported back to the UI.
    void CheckAsync(std::string localVersion, std::string skippedVersion,
                    std::string proxy, bool manual);

    // Queues a download of the release asset to `targetPath`. When the release
    // publishes a SHA-256 digest it is verified before the file is finalised.
    void DownloadAsync(ReleaseInfo release, std::wstring targetPath, std::string proxy);
    void CancelDownload();

    Snapshot GetSnapshot() const;
    void Shutdown();

private:
    void WorkerLoop(std::stop_token token);
    void CheckWorker(std::stop_token token, std::string localVersion,
                     std::string skippedVersion, std::string proxy, bool manual);
    void DownloadWorker(std::stop_token token, const ReleaseInfo& release,
                        const std::wstring& targetPath, std::string proxy);
    void SetStatus(UpdateStatus status, std::string error = {},
                   bool downloadFailed = false);
    void SetProgress(unsigned long long downloaded, unsigned long long total);

    mutable std::mutex mutex_;
    Snapshot snapshot_{};

    std::jthread worker_;
    std::mutex taskMutex_;
    std::condition_variable_any taskCv_;
    std::function<void(std::stop_token)> task_;
    bool taskPending_ = false;
    std::atomic<bool> cancelDownload_{false};
};

} // namespace ex::update
