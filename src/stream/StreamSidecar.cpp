#include "pcmtp/stream/StreamSidecar.hpp"

#include <chrono>
#include <utility>

#include "pcmtp/stream/IcyMetadataClient.hpp"

namespace pcmtp {

StreamSidecar::~StreamSidecar() {
    stop();
}

void StreamSidecar::start(const std::string& stream_url, MetadataHandler handler) {
    stop();
    if (stream_url.empty() || !handler) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        url_ = stream_url;
        handler_ = std::move(handler);
        stop_requested_.store(false, std::memory_order_relaxed);
    }

    thread_ = std::thread(&StreamSidecar::worker, this);
}

void StreamSidecar::stop() {
    stop_requested_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    url_.clear();
    handler_ = MetadataHandler();
    stop_requested_.store(false, std::memory_order_relaxed);
}

void StreamSidecar::worker() {
    MetadataHandler handler;
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = handler_;
        url = url_;
    }

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        if (!IcyMetadataClient::supports_url(url)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (stop_requested_.load(std::memory_order_relaxed)) {
                return;
            }
            continue;
        }

        IcyMetadataClient::stream_until_stopped(url, handler, stop_requested_);
        if (stop_requested_.load(std::memory_order_relaxed)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
}

} // namespace pcmtp
