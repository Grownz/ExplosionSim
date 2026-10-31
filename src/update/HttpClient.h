#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace ex::update {

struct Url {
    std::string scheme;
    std::string host;
    std::string path = "/";
    int port = 443;
};

// Parses "https://host[:port]/path". Only https is accepted.
bool ParseUrl(const std::string& text, Url& out);
// GitHub hosts the release API and its asset downloads on these domains.
bool IsAllowedHost(const std::string& host);

struct HttpRequest {
    std::string userAgent = "ExplosionSim-Updater";
    std::vector<std::string> headers;
    int maxRedirects = 5;
    size_t maxBody = 512 * 1024;
};

// Fetches a URL into memory. Returns false and fills `error` on failure.
bool HttpGet(const std::string& url, const HttpRequest& request,
             std::string& body, std::string& error);

// Streams a URL into `path`. `progress` is called with (downloaded, total) and
// may return false to cancel; `total` is 0 when the server sends no length.
bool HttpDownload(
    const std::string& url, const std::string& path,
    const HttpRequest& request,
    const std::function<bool(unsigned long long, unsigned long long)>& progress,
    std::string& error);

} // namespace ex::update
