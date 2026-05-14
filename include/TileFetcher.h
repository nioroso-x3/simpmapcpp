#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Abstract interface for fetching tile bytes from a URL.
// Implementations can be: cpp-httplib + OpenSSL (HttplibTileFetcher), a null
// fetcher (for cache-only mode), or any custom transport (e.g. a JNI bridge to
// a host-app HTTP client).
class TileFetcher {
public:
    virtual ~TileFetcher() = default;

    // Fetches the URL and returns the raw response body (PNG, JPEG, etc.).
    // Returns an empty vector on any failure. Implementations must not throw.
    virtual std::vector<uint8_t> fetch(const std::string& url) = 0;
};
