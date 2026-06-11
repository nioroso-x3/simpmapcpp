#include "HttplibTileFetcher.h"

#include "httplib.h"  // single header, CPPHTTPLIB_OPENSSL_SUPPORT set via CMake

namespace sm_http = simplemap_internal_httplib;
#include <iostream>

HttplibTileFetcher::HttplibTileFetcher(std::string user_agent, int timeout_seconds)
    : user_agent_(std::move(user_agent)), timeout_seconds_(timeout_seconds) {}

void HttplibTileFetcher::setCaCertPath(std::string path) {
    ca_cert_path_ = std::move(path);
}

namespace {

struct ParsedUrl {
    bool ok = false;
    std::string base;   // "https://host:port"
    std::string path;   // "/foo/bar?baz"
};

ParsedUrl parseUrl(const std::string& url) {
    ParsedUrl out;
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return out;

    auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
        out.base = url;
        out.path = "/";
    } else {
        out.base = url.substr(0, path_start);
        out.path = url.substr(path_start);
    }
    out.ok = true;
    return out;
}

}  // namespace

std::vector<uint8_t> HttplibTileFetcher::fetch(const std::string& url) {
    ParsedUrl parsed = parseUrl(url);
    if (!parsed.ok) {
        std::cerr << "[HttplibTileFetcher] malformed URL: " << url << "\n";
        return {};
    }

    sm_http::Client client(parsed.base);
    client.set_connection_timeout(timeout_seconds_, 0);
    client.set_read_timeout(timeout_seconds_, 0);
    client.set_follow_location(true);
    client.set_default_headers({{"User-Agent", user_agent_}});
    client.enable_server_certificate_verification(false);

    if (!ca_cert_path_.empty()) {
        client.set_ca_cert_path(ca_cert_path_.c_str());
    }

    auto res = client.Get(parsed.path.c_str());
    if (!res) {
        std::cerr << "[HttplibTileFetcher] request failed: "
                  << sm_http::to_string(res.error()) << " (" << url << ")\n";
        return {};
    }
    if (res->status != 200) {
        std::cerr << "[HttplibTileFetcher] HTTP " << res->status
                  << " for " << url << "\n";
        return {};
    }

    return std::vector<uint8_t>(res->body.begin(), res->body.end());
}
