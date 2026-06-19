#include "AsyncTileLoader.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

AsyncTileLoader::AsyncTileLoader(std::unique_ptr<TileFetcher> fetcher,
                                 OnComplete on_complete,
                                 int num_threads,
                                 std::chrono::seconds failure_ttl)
    : fetcher_(std::move(fetcher)),
      on_complete_(std::move(on_complete)),
      failure_ttl_(failure_ttl) {
    if (num_threads < 1) num_threads = 1;
    workers_.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

AsyncTileLoader::~AsyncTileLoader() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

bool AsyncTileLoader::isFailing(const TileKey& key) const {
    auto it = failed_.find(key);
    if (it == failed_.end()) return false;
    return std::chrono::steady_clock::now() - it->second < failure_ttl_;
}

void AsyncTileLoader::request(const std::string& url, const TileKey& coord) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) return;
        if (in_flight_.count(coord)) return;
        if (isFailing(coord)) return;
        in_flight_.insert(coord);
        queue_.push_back(Job{url, coord});
    }
    cv_.notify_one();
}

void AsyncTileLoader::workerLoop() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            // On shutdown, abandon any remaining queued jobs so destruction
            // is fast — we don't want to block app exit fetching tiles nobody
            // will see. In-flight fetches (already past this point in another
            // worker) still finish and call back.
            if (stop_) return;
            // LIFO: take from the back so the most recent request wins.
            job = std::move(queue_.back());
            queue_.pop_back();
        }

        std::vector<uint8_t> bytes = fetcher_->fetch(job.url);

        cv::Mat tile;
        if (!bytes.empty()) {
            tile = cv::imdecode(cv::Mat(bytes), cv::IMREAD_COLOR);
            if (!tile.empty()) {
                cv::cvtColor(tile, tile, cv::COLOR_BGR2RGBA);
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            in_flight_.erase(job.coord);
            if (tile.empty()) {
                failed_[job.coord] = std::chrono::steady_clock::now();
            } else {
                failed_.erase(job.coord);
            }
        }

        if (!tile.empty() && on_complete_) {
            on_complete_(job.coord, tile);
        }
    }
}
