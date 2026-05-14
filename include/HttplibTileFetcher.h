#pragma once

#include "TileFetcher.h"
#include <string>

// TileFetcher backed by cpp-httplib. Supports HTTP and HTTPS (built with
// CPPHTTPLIB_OPENSSL_SUPPORT and linked against OpenSSL).
class HttplibTileFetcher : public TileFetcher {
public:
    explicit HttplibTileFetcher(std::string user_agent = "simplemap/0.2",
                                int timeout_seconds = 10);
    ~HttplibTileFetcher() override = default;

    // Optional: set a CA bundle path. On most desktop systems the OpenSSL
    // defaults find the system trust store automatically; on Android you
    // typically need to point at /system/etc/security/cacerts or a bundled
    // cacert.pem in your app's assets.
    void setCaCertPath(std::string path);

    std::vector<uint8_t> fetch(const std::string& url) override;

private:
    std::string user_agent_;
    int timeout_seconds_;
    std::string ca_cert_path_;
};
