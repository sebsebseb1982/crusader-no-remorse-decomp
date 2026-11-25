#include "platform/http_get.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

#include <cstddef>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

namespace crusader {

namespace {

constexpr std::size_t kMaxBody = 1 << 20;
constexpr const char* kUserAgent = "crusader-no-remorse";

}  // namespace

#ifdef _WIN32

namespace {

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

// A GET sent and its answer received, 2xx: the handles are closed with it.
struct Request {
    Handle session, connection, request;

    bool open(const std::string& url, int timeout_ms) {
        const std::wstring wide = widen(url);
        wchar_t host[256] = {}, path[2048] = {};
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof parts;
        parts.lpszHostName = host;
        parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
        if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) return false;

        const std::wstring agent = widen(kUserAgent);
        session.h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
        // Before Windows 8.1: the proxy of the registry.
        if (!session.h) {
            session.h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
        }
        if (!session.h) return false;
        WinHttpSetTimeouts(session.h, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
        connection.h = WinHttpConnect(session.h, host, parts.nPort, 0);
        if (!connection.h) return false;
        request.h = WinHttpOpenRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!request.h) return false;
        if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.h, nullptr)) {
            return false;
        }
        DWORD status = 0, size = sizeof status;
        return WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                   WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) &&
               status >= 200 && status < 300;
    }
};

}  // namespace

// @orig none: port's online news
std::optional<std::string> http_get(const std::string& url, int timeout_ms) {
    Request r;
    if (!r.open(url, timeout_ms)) return std::nullopt;
    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(r.request.h, &available)) return std::nullopt;
        if (available == 0) break;
        if (body.size() + available > kMaxBody) return std::nullopt;
        const std::size_t at = body.size();
        body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(r.request.h, body.data() + at, available, &read)) return std::nullopt;
        body.resize(at + read);
    }
    return body;
}

// @orig none: port's self-update
bool http_download(const std::string& url, const std::filesystem::path& file, std::uint64_t expected_size,
                   const std::function<bool(std::uint64_t)>& progress, int timeout_ms) {
    bool ok = false;
    {
        Request r;
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (out && r.open(url, timeout_ms)) {
            std::vector<char> buffer(1 << 16);
            std::uint64_t written = 0;
            ok = true;
            for (;;) {
                DWORD read = 0;
                if (!WinHttpReadData(r.request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
                    ok = false;
                    break;
                }
                if (read == 0) break;
                out.write(buffer.data(), read);
                written += read;
                if (!out || !progress(written)) {
                    ok = false;
                    break;
                }
            }
            out.close();
            ok = ok && !out.fail() && (expected_size == 0 || written == expected_size);
        }
    }
    if (!ok) {
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
    }
    return ok;
}

#else

namespace {

// The few libcurl entry points and option numbers used (curl/curl.h), so the
// game builds without its headers.
using CurlEasyInit = void* (*)();
using CurlEasySetopt = int (*)(void*, int, ...);
using CurlEasyPerform = int (*)(void*);
using CurlEasyCleanup = void (*)(void*);
constexpr int kOptWriteData = 10001, kOptUrl = 10002, kOptUserAgent = 10018, kOptWriteFunction = 20011;
constexpr int kOptFailOnError = 45, kOptFollowLocation = 52, kOptNoSignal = 99, kOptTimeoutMs = 155,
              kOptConnectTimeoutMs = 156, kOptMaxRedirs = 68, kOptLowSpeedLimit = 19, kOptLowSpeedTime = 20;

struct Curl {
    CurlEasyInit init = nullptr;
    CurlEasySetopt setopt = nullptr;
    CurlEasyPerform perform = nullptr;
    CurlEasyCleanup cleanup = nullptr;
};

// Loaded once, never unloaded.
const Curl* curl() {
    static const Curl* loaded = [] () -> const Curl* {
        static Curl c;
        for (const char* name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl-nss.so.4", "libcurl.so",
                                 "libcurl.4.dylib"}) {
            void* lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (!lib) continue;
            c.init = reinterpret_cast<CurlEasyInit>(dlsym(lib, "curl_easy_init"));
            c.setopt = reinterpret_cast<CurlEasySetopt>(dlsym(lib, "curl_easy_setopt"));
            c.perform = reinterpret_cast<CurlEasyPerform>(dlsym(lib, "curl_easy_perform"));
            c.cleanup = reinterpret_cast<CurlEasyCleanup>(dlsym(lib, "curl_easy_cleanup"));
            if (c.init && c.setopt && c.perform && c.cleanup) return &c;
            dlclose(lib);
        }
        return nullptr;
    }();
    return loaded;
}

struct Body {
    std::string text;
    bool too_big = false;
};

std::size_t write_body(char* data, std::size_t size, std::size_t count, void* user) {
    auto* body = static_cast<Body*>(user);
    const std::size_t n = size * count;
    if (body->text.size() + n > kMaxBody) {
        body->too_big = true;
        return 0;  // stops the transfer
    }
    body->text.append(data, n);
    return n;
}

// A download to a file: what the write callback needs.
struct Download {
    std::ofstream out;
    const std::function<bool(std::uint64_t)>* progress = nullptr;
    std::uint64_t written = 0;
    bool stopped = false;
};

std::size_t write_file(char* data, std::size_t size, std::size_t count, void* user) {
    auto* download = static_cast<Download*>(user);
    const std::size_t n = size * count;
    download->out.write(data, static_cast<std::streamsize>(n));
    download->written += n;
    if (!download->out || !(*download->progress)(download->written)) {
        download->stopped = true;
        return 0;  // stops the transfer
    }
    return n;
}

}  // namespace

// @orig none: port's self-update
bool http_download(const std::string& url, const std::filesystem::path& file, std::uint64_t expected_size,
                   const std::function<bool(std::uint64_t)>& progress, int timeout_ms) {
    bool ok = false;
    if (const Curl* c = curl()) {
        Download download;
        download.out.open(file, std::ios::binary | std::ios::trunc);
        download.progress = &progress;
        if (void* handle = download.out ? c->init() : nullptr) {
            c->setopt(handle, kOptUrl, url.c_str());
            c->setopt(handle, kOptUserAgent, kUserAgent);
            c->setopt(handle, kOptWriteFunction, &write_file);
            c->setopt(handle, kOptWriteData, &download);
            c->setopt(handle, kOptFailOnError, 1L);
            c->setopt(handle, kOptFollowLocation, 1L);
            c->setopt(handle, kOptMaxRedirs, 5L);
            c->setopt(handle, kOptNoSignal, 1L);
            c->setopt(handle, kOptConnectTimeoutMs, static_cast<long>(timeout_ms));
            // No total time (an archive takes what it takes), but a transfer
            // under 1 byte a second for `timeout_ms` is dead.
            c->setopt(handle, kOptLowSpeedLimit, 1L);
            c->setopt(handle, kOptLowSpeedTime, static_cast<long>(timeout_ms / 1000 > 0 ? timeout_ms / 1000 : 1));
            const int result = c->perform(handle);
            c->cleanup(handle);
            download.out.close();
            ok = result == 0 && !download.stopped && !download.out.fail() &&
                 (expected_size == 0 || download.written == expected_size);
        }
    }
    if (!ok) {
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
    }
    return ok;
}

// @orig none: port's online news
std::optional<std::string> http_get(const std::string& url, int timeout_ms) {
    const Curl* c = curl();
    if (!c) return std::nullopt;
    void* handle = c->init();
    if (!handle) return std::nullopt;
    Body body;
    c->setopt(handle, kOptUrl, url.c_str());
    c->setopt(handle, kOptUserAgent, kUserAgent);
    c->setopt(handle, kOptWriteFunction, &write_body);
    c->setopt(handle, kOptWriteData, &body);
    c->setopt(handle, kOptFailOnError, 1L);
    c->setopt(handle, kOptFollowLocation, 1L);
    c->setopt(handle, kOptMaxRedirs, 5L);
    c->setopt(handle, kOptNoSignal, 1L);
    c->setopt(handle, kOptConnectTimeoutMs, static_cast<long>(timeout_ms));
    c->setopt(handle, kOptTimeoutMs, static_cast<long>(timeout_ms) * 2);
    const int result = c->perform(handle);
    c->cleanup(handle);
    if (result != 0 || body.too_big) return std::nullopt;
    return std::move(body.text);
}

#endif

}  // namespace crusader
