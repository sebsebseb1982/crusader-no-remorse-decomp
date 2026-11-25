#pragma once
// Port addition (@orig none): one HTTP(S) GET, for the port's online news and
// update notice (core/news.hpp).
//
// Windows: WinHTTP, part of the system (proxy settings of the system, TLS of
// the system). Elsewhere: libcurl, loaded when first needed (dlopen
// "libcurl.so.4"...), so the game neither links nor ships it; without it,
// every request fails.
//
// Blocking: the game calls it from a thread of its own (Game::enable_online).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace crusader {

// The body of a successful answer (2xx, redirects followed), none on any
// failure: no network, timeout (`timeout_ms` for each step), an HTTP error,
// or a body over 1 MB.
std::optional<std::string> http_get(const std::string& url, int timeout_ms = 8000);

// The body at `url` written to `file` (2xx, redirects followed), for the
// files that http_get's 1 MB does not allow: the archive of an update.
// `progress` gets the bytes written so far, as they come, and returns false to
// stop. False on any failure (no network, a step over `timeout_ms`, an HTTP
// error, stopped, a disk error, or a body that is not `expected_size` bytes
// when that is not 0): the file is then removed. Blocking, like http_get.
bool http_download(const std::string& url, const std::filesystem::path& file, std::uint64_t expected_size,
                   const std::function<bool(std::uint64_t)>& progress, int timeout_ms = 15000);

}  // namespace crusader
