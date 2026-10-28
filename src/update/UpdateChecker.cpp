#include "update/UpdateChecker.h"

#include "core/Log.h"
#include "update/Version.h"

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace ex::update {

namespace {

constexpr size_t kMaxApiResponse = 2 * 1024 * 1024;

struct WinHttpHandle {
    HINTERNET value = nullptr;

    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : value(handle) {}
    ~WinHttpHandle() {
        if (value) {
            WinHttpCloseHandle(value);
        }
    }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& other) noexcept : value(other.value) {
        other.value = nullptr;
    }
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            if (value) {
                WinHttpCloseHandle(value);
            }
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
    explicit operator bool() const { return value != nullptr; }
};

std::string WinHttpError(const char* what) {
    return std::string(what) + " failed (WinHTTP error " + std::to_string(GetLastError()) + ")";
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(size_t(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), length);
    return wide;
}

bool IsAllowedDownloadHost(const std::wstring& host) {
    return _wcsicmp(host.c_str(), L"github.com") == 0 ||
           _wcsicmp(host.c_str(), L"objects.githubusercontent.com") == 0;
}

struct UrlParts {
    std::wstring host;
    std::wstring path;
};

bool CrackUrl(const std::wstring& url, UrlParts& parts) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = DWORD(-1);
    components.dwUrlPathLength = DWORD(-1);
    components.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
        return false;
    }
    parts.host.assign(components.lpszHostName, components.dwHostNameLength);
    parts.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        parts.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    return !parts.host.empty() && !parts.path.empty();
}

bool OpenGetRequest(const std::wstring& host, const std::wstring& path,
                    const std::wstring& userAgent, bool githubHeaders,
                    WinHttpHandle& session, WinHttpHandle& connect,
                    WinHttpHandle& request, std::string& error) {
    session = WinHttpHandle(WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = WinHttpError("WinHttpOpen");
        return false;
    }
    WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000);

    connect = WinHttpHandle(
        WinHttpConnect(session.value, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connect) {
        error = WinHttpError("WinHttpConnect");
        return false;
    }

    request = WinHttpHandle(WinHttpOpenRequest(connect.value, L"GET", path.c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE));
    if (!request) {
        error = WinHttpError("WinHttpOpenRequest");
        return false;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                     sizeof(redirectPolicy));

    if (githubHeaders) {
        const std::wstring headers =
            L"Accept: application/vnd.github+json\r\n"
            L"X-GitHub-Api-Version: 2022-11-28\r\n";
        WinHttpAddRequestHeaders(request.value, headers.c_str(), DWORD(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        error = WinHttpError("WinHttpSendRequest");
        return false;
    }
    if (!WinHttpReceiveResponse(request.value, nullptr)) {
        error = WinHttpError("WinHttpReceiveResponse");
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        error = WinHttpError("WinHttpQueryHeaders(status)");
        return false;
    }
    if (status != 200) {
        error = "HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

bool ReadBody(HINTERNET request, std::string& body, std::string& error,
              std::stop_token token) {
    for (;;) {
        if (token.stop_requested()) {
            error = "cancelled";
            return false;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            error = WinHttpError("WinHttpQueryDataAvailable");
            return false;
        }
        if (available == 0) {
            return true;
        }
        std::string chunk(size_t(available), '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) {
            error = WinHttpError("WinHttpReadData");
            return false;
        }
        if (read == 0) {
            return true;
        }
        body.append(chunk.data(), read);
        if (body.size() > kMaxApiResponse) {
            error = "response too large";
            return false;
        }
    }
}

unsigned long long ContentLength(HINTERNET request) {
    unsigned long long total = 0;
    DWORD size = sizeof(total);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                             WINHTTP_HEADER_NAME_BY_INDEX, &total, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return 0;
    }
    return total;
}

bool ParseRelease(const std::string& body, const std::string& localVersion,
                  const std::string& skippedVersion, ReleaseInfo& release,
                  bool& isNewer, std::string& error) {
    try {
        nlohmann::json json = nlohmann::json::parse(body, nullptr, false);
        if (json.is_discarded() || !json.is_object()) {
            error = "invalid GitHub response";
            return false;
        }
        if (json.value("draft", false) || json.value("prerelease", false)) {
            isNewer = false;
            return true;
        }

        release.tag = json.value("tag_name", "");
        release.name = json.value("name", "");
        release.notes = json.value("body", "");
        release.publishedAt = json.value("published_at", "");

        SemanticVersion remote;
        SemanticVersion local;
        if (!ParseVersion(release.tag, remote) || !ParseVersion(localVersion, local)) {
            error = "invalid release tag";
            return false;
        }
        release.version = VersionToString(remote);
        isNewer = CompareVersions(remote, local) > 0;

        if (isNewer && !skippedVersion.empty()) {
            SemanticVersion skipped;
            if (ParseVersion(skippedVersion, skipped) &&
                CompareVersions(remote, skipped) == 0) {
                isNewer = false;
            }
        }

        if (!json.contains("assets") || !json["assets"].is_array()) {
            error = "release has no assets";
            return false;
        }

        const std::string expected = "ExplosionSim-" + release.version + "-win64.zip";
        const nlohmann::json* chosen = nullptr;
        for (const auto& asset : json["assets"]) {
            if (!asset.is_object()) {
                continue;
            }
            const std::string name = asset.value("name", "");
            if (name == expected) {
                chosen = &asset;
                break;
            }
            if (!chosen && name.rfind("ExplosionSim-", 0) == 0 && name.size() > 4 &&
                name.substr(name.size() - 4) == ".zip") {
                chosen = &asset;
            }
        }
        if (!chosen) {
            // A release without a Windows zip is not actionable.
            isNewer = false;
            return true;
        }

        release.assetName = chosen->value("name", "");
        release.assetUrl = chosen->value("browser_download_url", "");
        release.assetSize = chosen->value("size", 0ull);
        if (release.assetUrl.rfind(
                "https://github.com/Grownz/ExplosionSim/releases/download/", 0) != 0) {
            error = "unexpected download URL";
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = std::string("release parse failed: ") + exception.what();
        return false;
    }
}

} // namespace

UpdateChecker::UpdateChecker() {
    worker_ = std::jthread([this](std::stop_token token) { WorkerLoop(token); });
}

UpdateChecker::~UpdateChecker() {
    Shutdown();
}

void UpdateChecker::CheckAsync(std::string localVersion, std::string skippedVersion,
                               bool manual) {
    {
        std::lock_guard lock(taskMutex_);
        if (taskPending_) {
            return;
        }
        task_ = [this, local = std::move(localVersion), skipped = std::move(skippedVersion),
                 manual](std::stop_token token) {
            CheckWorker(token, local, skipped, manual);
        };
        taskPending_ = true;
    }
    taskCv_.notify_one();
}

void UpdateChecker::DownloadAsync(ReleaseInfo release, std::wstring targetPath) {
    cancelDownload_ = false;
    {
        std::lock_guard lock(taskMutex_);
        task_ = [this, release = std::move(release),
                 targetPath = std::move(targetPath)](std::stop_token token) {
            DownloadWorker(token, release, targetPath);
        };
        taskPending_ = true;
    }
    taskCv_.notify_one();
}

void UpdateChecker::CancelDownload() {
    cancelDownload_ = true;
}

UpdateChecker::Snapshot UpdateChecker::GetSnapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void UpdateChecker::Shutdown() {
    cancelDownload_ = true;
    worker_.request_stop();
    taskCv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void UpdateChecker::WorkerLoop(std::stop_token token) {
    for (;;) {
        std::function<void(std::stop_token)> task;
        {
            std::unique_lock lock(taskMutex_);
            taskCv_.wait(lock, token, [this] { return taskPending_; });
            if (token.stop_requested()) {
                return;
            }
            task = std::move(task_);
            taskPending_ = false;
        }
        if (task) {
            task(token);
        }
    }
}

void UpdateChecker::CheckWorker(std::stop_token token, std::string localVersion,
                                std::string skippedVersion, bool manual) {
    {
        std::lock_guard lock(mutex_);
        snapshot_ = Snapshot{};
        snapshot_.status = UpdateStatus::Checking;
        snapshot_.manual = manual;
    }

    WinHttpHandle session;
    WinHttpHandle connect;
    WinHttpHandle request;
    std::string error;
    const std::wstring userAgent = L"ExplosionSim/" + Widen(localVersion);
    if (!OpenGetRequest(L"api.github.com", L"/repos/Grownz/ExplosionSim/releases/latest",
                        userAgent, true, session, connect, request, error)) {
        SetStatus(UpdateStatus::Failed, error);
        return;
    }

    std::string body;
    if (!ReadBody(request.value, body, error, token)) {
        SetStatus(token.stop_requested() ? UpdateStatus::Idle : UpdateStatus::Failed, error);
        return;
    }

    ReleaseInfo release;
    bool isNewer = false;
    if (!ParseRelease(body, localVersion, skippedVersion, release, isNewer, error)) {
        SetStatus(UpdateStatus::Failed, error);
        return;
    }

    std::lock_guard lock(mutex_);
    snapshot_.release = release;
    snapshot_.status = isNewer ? UpdateStatus::Available : UpdateStatus::UpToDate;
    snapshot_.error.clear();
    snapshot_.manual = manual;
}

void UpdateChecker::DownloadWorker(std::stop_token token, const ReleaseInfo& release,
                                   const std::wstring& targetPath) {
    {
        std::lock_guard lock(mutex_);
        snapshot_.status = UpdateStatus::Downloading;
        snapshot_.release = release;
        snapshot_.downloaded = 0;
        snapshot_.total = release.assetSize;
        snapshot_.progress = 0.0f;
        snapshot_.error.clear();
    }

    UrlParts parts;
    if (!CrackUrl(Widen(release.assetUrl), parts) || !IsAllowedDownloadHost(parts.host)) {
        SetStatus(UpdateStatus::Failed, "unsupported download host");
        return;
    }

    WinHttpHandle session;
    WinHttpHandle connect;
    WinHttpHandle request;
    std::string error;
    if (!OpenGetRequest(parts.host, parts.path, L"ExplosionSim", false, session, connect,
                        request, error)) {
        SetStatus(UpdateStatus::Failed, error);
        return;
    }

    const unsigned long long total = ContentLength(request.value);
    const unsigned long long expected = total > 0 ? total : release.assetSize;
    SetProgress(0, expected);

    std::filesystem::path target(targetPath);
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    std::filesystem::path part = target;
    part += L".part";
    std::ofstream file(part, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        SetStatus(UpdateStatus::Failed, "cannot create the download file");
        return;
    }

    unsigned long long downloaded = 0;
    bool ok = true;
    while (ok) {
        if (cancelDownload_.load() || token.stop_requested()) {
            error = "cancelled";
            ok = false;
            break;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available)) {
            error = WinHttpError("WinHttpQueryDataAvailable");
            ok = false;
            break;
        }
        if (available == 0) {
            break;
        }
        std::vector<char> buffer(static_cast<size_t>(available));
        DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer.data(), available, &read)) {
            error = WinHttpError("WinHttpReadData");
            ok = false;
            break;
        }
        if (read == 0) {
            break;
        }
        file.write(buffer.data(), std::streamsize(read));
        if (!file) {
            error = "write failed";
            ok = false;
            break;
        }
        downloaded += read;
        SetProgress(downloaded, expected);
    }
    file.close();

    if (!ok) {
        std::filesystem::remove(part, ec);
        SetStatus(cancelDownload_.load() ? UpdateStatus::Idle : UpdateStatus::Failed, error);
        return;
    }
    if (expected > 0 && downloaded != expected) {
        std::filesystem::remove(part, ec);
        SetStatus(UpdateStatus::Failed, "incomplete download");
        return;
    }

    std::filesystem::remove(target, ec);
    std::filesystem::rename(part, target, ec);
    if (ec) {
        std::filesystem::remove(part, ec);
        SetStatus(UpdateStatus::Failed, "cannot finalise the download");
        return;
    }

    {
        std::lock_guard lock(mutex_);
        snapshot_.status = UpdateStatus::Downloaded;
        snapshot_.downloaded = downloaded;
        snapshot_.total = expected > 0 ? expected : downloaded;
        snapshot_.progress = 1.0f;
        snapshot_.error.clear();
    }
}

void UpdateChecker::SetStatus(UpdateStatus status, std::string error) {
    std::lock_guard lock(mutex_);
    snapshot_.status = status;
    snapshot_.error = std::move(error);
}

void UpdateChecker::SetProgress(unsigned long long downloaded, unsigned long long total) {
    std::lock_guard lock(mutex_);
    snapshot_.downloaded = downloaded;
    snapshot_.total = total;
    snapshot_.progress =
        total > 0 ? float(double(downloaded) / double(total)) : 0.0f;
}

} // namespace ex::update
