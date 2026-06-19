#pragma once

#include "TileFetcher.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Background tile fetcher with a worker thread pool. Submitted requests are
// deduped (same tile requested twice while still pending = one fetch). The
// pending queue is LIFO so the most recently requested tiles get serviced
// first — better for interactive panning where stale requests should yield
// to the user's current view.
//
// On successful fetch the on_complete callback is invoked from the worker
// thread with the decoded tile (RGBA cv::Mat). On failure the tile is added
// to a short-TTL failure set so it isn't immediately re-fetched (avoids
// hammering the server when a tile is 404 or the network is down).
//
// The loader owns its TileFetcher and shuts down its workers on destruction.
class AsyncTileLoader {
public:
    struct TileKey {
        int x, y, z;
        bool operator==(const TileKey& o) const {
            return x == o.x && y == o.y && z == o.z;
        }
    };
    struct TileKeyHash {
        std::size_t operator()(const TileKey& k) const {
            std::size_t h = std::hash<int>()(k.x);
            h ^= std::hash<int>()(k.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>()(k.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    // Callback signature: receives the tile coord and the decoded RGBA image.
    // Called from a worker thread.
    using OnComplete = std::function<void(const TileKey&, const cv::Mat&)>;

    AsyncTileLoader(std::unique_ptr<TileFetcher> fetcher,
                    OnComplete on_complete,
                    int num_threads = 3,
                    std::chrono::seconds failure_ttl = std::chrono::seconds(30));
    ~AsyncTileLoader();

    AsyncTileLoader(const AsyncTileLoader&) = delete;
    AsyncTileLoader& operator=(const AsyncTileLoader&) = delete;

    // Submit a tile for fetching. No-op if the tile is already in flight or
    // in the failure backoff window.
    void request(const std::string& url, const TileKey& coord);

private:
    struct Job {
        std::string url;
        TileKey coord;
    };

    void workerLoop();
    bool isFailing(const TileKey& key) const;  // call with mutex_ held

    std::unique_ptr<TileFetcher> fetcher_;
    OnComplete on_complete_;
    std::chrono::seconds failure_ttl_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Job> queue_;  // used as a stack (LIFO via pop_back)
    std::unordered_set<TileKey, TileKeyHash> in_flight_;
    std::unordered_map<TileKey, std::chrono::steady_clock::time_point, TileKeyHash> failed_;
    bool stop_ = false;

    std::vector<std::thread> workers_;
};
