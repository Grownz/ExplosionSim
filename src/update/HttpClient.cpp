#include "update/HttpClient.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

namespace ex::update {

namespace {

const char* const kCaBundles[] = {
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/cert.pem",
    "/etc/ssl/certs/ca-bundle.crt",
};

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return char(std::tolower(c));
    });
    return text;
}

struct TlsConnection {
    mbedtls_net_context net{};
    mbedtls_ssl_context ssl{};
    mbedtls_ssl_config config{};
    mbedtls_x509_crt ca{};
    mbedtls_ctr_drbg_context drbg{};
    mbedtls_entropy_context entropy{};
    bool netReady = false;
    bool sslReady = false;
    bool handshakeDone = false;
    bool caReady = false;
    bool drbgReady = false;
    bool entropyReady = false;
    bool configReady = false;

    ~TlsConnection() {
        if (handshakeDone) {
            mbedtls_ssl_close_notify(&ssl);
        }
        if (sslReady) {
            mbedtls_ssl_free(&ssl);
        }
        if (configReady) {
            mbedtls_ssl_config_free(&config);
        }
        if (caReady) {
            mbedtls_x509_crt_free(&ca);
        }
        if (drbgReady) {
            mbedtls_ctr_drbg_free(&drbg);
        }
        if (entropyReady) {
            mbedtls_entropy_free(&entropy);
        }
        if (netReady) {
            mbedtls_net_free(&net);
        }
    }
};

std::string MbedError(int code) {
    char buffer[256] = {};
    mbedtls_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

bool Connect(TlsConnection& connection, const Url& url, std::string& error) {
    mbedtls_net_init(&connection.net);
    connection.netReady = true;
    mbedtls_ssl_init(&connection.ssl);
    connection.sslReady = true;
    mbedtls_ssl_config_init(&connection.config);
    connection.configReady = true;
    mbedtls_x509_crt_init(&connection.ca);
    connection.caReady = true;
    mbedtls_ctr_drbg_init(&connection.drbg);
    connection.drbgReady = true;
    mbedtls_entropy_init(&connection.entropy);
    connection.entropyReady = true;

    bool loaded = false;
    for (const char* bundle : kCaBundles) {
        if (mbedtls_x509_crt_parse_file(&connection.ca, bundle) == 0) {
            loaded = true;
            break;
        }
    }
    if (!loaded) {
        error = "no system CA bundle could be loaded";
        return false;
    }

    if (mbedtls_ctr_drbg_seed(&connection.drbg, mbedtls_entropy_func,
                              &connection.entropy, nullptr, 0) != 0) {
        error = "cannot seed the TLS random generator";
        return false;
    }
    connection.drbgReady = true;

    if (mbedtls_ssl_config_defaults(&connection.config, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        error = "cannot configure TLS";
        return false;
    }
    mbedtls_ssl_conf_authmode(&connection.config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&connection.config, &connection.ca, nullptr);
    mbedtls_ssl_conf_rng(&connection.config, mbedtls_ctr_drbg_random,
                         &connection.drbg);
    mbedtls_ssl_conf_min_tls_version(&connection.config,
                                     MBEDTLS_SSL_VERSION_TLS1_2);

    const std::string port = std::to_string(url.port);
    const int netStatus = mbedtls_net_connect(
        &connection.net, url.host.c_str(), port.c_str(), MBEDTLS_NET_PROTO_TCP);
    if (netStatus != 0) {
        error = "cannot connect to " + url.host + ": " + MbedError(netStatus);
        return false;
    }

    if (mbedtls_ssl_setup(&connection.ssl, &connection.config) != 0) {
        error = "cannot initialize the TLS session";
        return false;
    }
    connection.sslReady = true;
    mbedtls_ssl_set_hostname(&connection.ssl, url.host.c_str());
    mbedtls_ssl_set_bio(&connection.ssl, &connection.net, mbedtls_net_send,
                        mbedtls_net_recv, nullptr);

    for (;;) {
        const int status = mbedtls_ssl_handshake(&connection.ssl);
        if (status == 0) {
            break;
        }
        if (status != MBEDTLS_ERR_SSL_WANT_READ &&
            status != MBEDTLS_ERR_SSL_WANT_WRITE) {
            error = "TLS handshake failed: " + MbedError(status);
            return false;
        }
    }
    if (mbedtls_ssl_get_verify_result(&connection.ssl) != 0) {
        error = "the server certificate is not trusted";
        return false;
    }
    connection.handshakeDone = true;
    return true;
}

bool WriteAll(mbedtls_ssl_context& ssl, const void* data, size_t size,
              std::string& error) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    while (size > 0) {
        const int written = mbedtls_ssl_write(&ssl, bytes, size);
        if (written <= 0) {
            error = "TLS write failed: " + MbedError(written);
            return false;
        }
        bytes += written;
        size -= size_t(written);
    }
    return true;
}

// Reads one byte at a time until CRLF. Returns false on close or error.
bool ReadLine(mbedtls_ssl_context& ssl, std::string& line, std::string& error) {
    line.clear();
    for (;;) {
        unsigned char character = 0;
        const int status = mbedtls_ssl_read(&ssl, &character, 1);
        if (status == MBEDTLS_ERR_SSL_WANT_READ ||
            status == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (status == 0) {
            return false;
        }
        if (status < 0) {
            error = "TLS read failed: " + MbedError(status);
            return false;
        }
        if (character == '\n') {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return true;
        }
        line.push_back(char(character));
        if (line.size() > 16 * 1024) {
            error = "response header line is too long";
            return false;
        }
    }
}

struct ResponseHead {
    int status = 0;
    std::string location;
    bool chunked = false;
    unsigned long long contentLength = 0;
    bool hasContentLength = false;
};

bool ReadResponseHead(mbedtls_ssl_context& ssl, ResponseHead& head,
                      std::string& error) {
    std::string statusLine;
    if (!ReadLine(ssl, statusLine, error)) {
        error = error.empty() ? "empty response" : error;
        return false;
    }
    const size_t space = statusLine.find(' ');
    if (space == std::string::npos) {
        error = "malformed HTTP status line";
        return false;
    }
    head.status = std::atoi(statusLine.c_str() + space + 1);

    std::string line;
    while (ReadLine(ssl, line, error)) {
        if (line.empty()) {
            break;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string name = Lower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.erase(value.begin());
        }
        if (name == "location") {
            head.location = value;
        } else if (name == "transfer-encoding" &&
                   Lower(value).find("chunked") != std::string::npos) {
            head.chunked = true;
        } else if (name == "content-length") {
            head.contentLength = std::strtoull(value.c_str(), nullptr, 10);
            head.hasContentLength = true;
        }
    }
    return true;
}

bool ReadChunked(mbedtls_ssl_context& ssl,
                 const std::function<bool(const void*, size_t)>& sink,
                 std::string& error) {
    for (;;) {
        std::string sizeLine;
        if (!ReadLine(ssl, sizeLine, error)) {
            error = error.empty() ? "truncated chunked body" : error;
            return false;
        }
        const size_t extension = sizeLine.find(';');
        if (extension != std::string::npos) {
            sizeLine.resize(extension);
        }
        const unsigned long long chunkSize =
            std::strtoull(sizeLine.c_str(), nullptr, 16);
        if (chunkSize == 0) {
            // Consume the trailer.
            std::string trailer;
            while (ReadLine(ssl, trailer, error) && !trailer.empty()) {
            }
            return true;
        }
        std::vector<unsigned char> buffer(static_cast<size_t>(chunkSize));
        size_t received = 0;
        while (received < buffer.size()) {
            const int status = mbedtls_ssl_read(
                &ssl, buffer.data() + received, buffer.size() - received);
            if (status == MBEDTLS_ERR_SSL_WANT_READ ||
                status == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            if (status <= 0) {
                error = "truncated chunked body";
                return false;
            }
            received += size_t(status);
        }
        if (!sink(buffer.data(), buffer.size())) {
            error = "cancelled";
            return false;
        }
        std::string terminator;
        if (!ReadLine(ssl, terminator, error)) {
            error = "truncated chunked body";
            return false;
        }
    }
}

bool ReadFixed(mbedtls_ssl_context& ssl, unsigned long long length,
               const std::function<bool(const void*, size_t)>& sink,
               std::string& error) {
    std::vector<unsigned char> buffer(64 * 1024);
    unsigned long long remaining = length;
    while (remaining > 0) {
        const size_t want = size_t(std::min<unsigned long long>(
            remaining, buffer.size()));
        const int status = mbedtls_ssl_read(&ssl, buffer.data(), want);
        if (status == MBEDTLS_ERR_SSL_WANT_READ ||
            status == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (status <= 0) {
            error = "truncated response body";
            return false;
        }
        if (!sink(buffer.data(), size_t(status))) {
            error = "cancelled";
            return false;
        }
        remaining -= std::min<unsigned long long>(remaining, size_t(status));
    }
    return true;
}

bool ReadUntilClose(mbedtls_ssl_context& ssl,
                    const std::function<bool(const void*, size_t)>& sink,
                    std::string& error) {
    std::vector<unsigned char> buffer(64 * 1024);
    for (;;) {
        const int status = mbedtls_ssl_read(&ssl, buffer.data(), buffer.size());
        if (status == MBEDTLS_ERR_SSL_WANT_READ ||
            status == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (status == 0) {
            return true;
        }
        if (status < 0) {
            error = "TLS read failed: " + MbedError(status);
            return false;
        }
        if (!sink(buffer.data(), size_t(status))) {
            error = "cancelled";
            return false;
        }
    }
}

bool RequestOnce(
    const Url& url, const HttpRequest& request,
    const std::function<bool(const ResponseHead&, const void*, size_t)>& sink,
    std::string& error, ResponseHead& head) {
    TlsConnection connection;
    if (!Connect(connection, url, error)) {
        return false;
    }

    std::string head_text;
    head_text.reserve(512);
    head_text += "GET " + url.path + " HTTP/1.1\r\n";
    head_text += "Host: " + url.host + "\r\n";
    head_text += "User-Agent: " + request.userAgent + "\r\n";
    head_text += "Accept: */*\r\n";
    head_text += "Connection: close\r\n";
    for (const std::string& header : request.headers) {
        head_text += header + "\r\n";
    }
    head_text += "\r\n";
    if (!WriteAll(connection.ssl, head_text.data(), head_text.size(), error)) {
        return false;
    }

    if (!ReadResponseHead(connection.ssl, head, error)) {
        return false;
    }
    if (head.status >= 300 && head.status < 400 && !head.location.empty()) {
        return true; // redirect, caller retries
    }
    if (head.status != 200) {
        error = "HTTP " + std::to_string(head.status);
        return false;
    }
    if (!sink(head, nullptr, 0)) {
        error = "cancelled";
        return false;
    }
    if (head.chunked) {
        return ReadChunked(connection.ssl,
                           [&sink, &head](const void* data, size_t size) {
                               return sink(head, data, size);
                           },
                           error);
    }
    if (head.hasContentLength) {
        return ReadFixed(connection.ssl, head.contentLength,
                         [&sink, &head](const void* data, size_t size) {
                             return sink(head, data, size);
                         },
                         error);
    }
    return ReadUntilClose(connection.ssl,
                          [&sink, &head](const void* data, size_t size) {
                              return sink(head, data, size);
                          },
                          error);
}

std::string ResolveRedirect(const Url& base, const std::string& location) {
    if (location.rfind("http://", 0) == 0 ||
        location.rfind("https://", 0) == 0) {
        return location;
    }
    std::string result = base.scheme + "://" + base.host;
    if (location.empty() || location.front() != '/') {
        result += "/";
    }
    result += location;
    return result;
}

} // namespace

bool ParseUrl(const std::string& text, Url& out) {
    const std::string prefix = "https://";
    if (text.rfind(prefix, 0) != 0) {
        return false;
    }
    const size_t hostStart = prefix.size();
    const size_t pathStart = text.find('/', hostStart);
    const std::string authority = text.substr(
        hostStart, pathStart == std::string::npos ? std::string::npos
                                                  : pathStart - hostStart);
    if (authority.empty()) {
        return false;
    }
    out = {};
    out.scheme = "https";
    out.port = 443;
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        out.host = authority.substr(0, colon);
        out.port = std::atoi(authority.c_str() + colon + 1);
    } else {
        out.host = authority;
    }
    out.path = pathStart == std::string::npos ? "/" : text.substr(pathStart);
    return !out.host.empty() && out.port > 0;
}

bool IsAllowedHost(const std::string& host) {
    const std::string lower = Lower(host);
    return lower == "api.github.com" || lower == "github.com" ||
           lower == "objects.githubusercontent.com" ||
           lower == "release-assets.githubusercontent.com";
}

bool HttpGet(const std::string& url, const HttpRequest& request,
             std::string& body, std::string& error) {
    std::string current = url;
    for (int redirect = 0; redirect <= request.maxRedirects; ++redirect) {
        Url parsed;
        if (!ParseUrl(current, parsed) || !IsAllowedHost(parsed.host)) {
            error = "the update URL is not a trusted GitHub host";
            return false;
        }
        ResponseHead head;
        std::string localError;
        const bool ok = RequestOnce(
            parsed, request,
            [&body, &request](const ResponseHead&, const void* data,
                              size_t size) {
                if (size == 0) {
                    return true;
                }
                if (body.size() + size > request.maxBody) {
                    return false;
                }
                body.append(static_cast<const char*>(data), size);
                return true;
            },
            localError, head);
        if (!ok) {
            error = localError;
            return false;
        }
        if (head.status >= 300 && head.status < 400) {
            current = ResolveRedirect(parsed, head.location);
            body.clear();
            continue;
        }
        return true;
    }
    error = "too many redirects";
    return false;
}

bool HttpDownload(
    const std::string& url, const std::string& path,
    const HttpRequest& request,
    const std::function<bool(unsigned long long, unsigned long long)>& progress,
    std::string& error) {
    std::string current = url;
    for (int redirect = 0; redirect <= request.maxRedirects; ++redirect) {
        Url parsed;
        if (!ParseUrl(current, parsed) || !IsAllowedHost(parsed.host)) {
            error = "the update URL is not a trusted GitHub host";
            return false;
        }
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "cannot open the download target";
            return false;
        }
        unsigned long long written = 0;
        unsigned long long total = 0;
        ResponseHead head;
        std::string localError;
        const bool ok = RequestOnce(
            parsed, request,
            [&file, &written, &total, &progress, &request](
                const ResponseHead& response, const void* data, size_t size) {
                if (data == nullptr) {
                    total = response.hasContentLength ? response.contentLength
                                                      : 0;
                    return true;
                }
                file.write(static_cast<const char*>(data),
                           std::streamsize(size));
                if (!file) {
                    return false;
                }
                written += size;
                return progress ? progress(written, total) : true;
            },
            localError, head);
        file.close();
        if (!ok) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            error = localError;
            return false;
        }
        if (head.status >= 300 && head.status < 400) {
            current = ResolveRedirect(parsed, head.location);
            continue;
        }
        return true;
    }
    error = "too many redirects";
    return false;
}

} // namespace ex::update
