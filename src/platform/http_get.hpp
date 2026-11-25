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

#include <optional>
#include <string>

namespace crusader {

// The body of a successful answer (2xx, redirects followed), none on any
// failure: no network, timeout (`timeout_ms` for each step), an HTTP error,
// or a body over 1 MB.
std::optional<std::string> http_get(const std::string& url, int timeout_ms = 8000);

}  // namespace crusader
