#include "pcmtp/stream/StreamSidecar.hpp"

#include <chrono>
#include <utility>
#include <sys/socket.h>
#include <unistd.h>

#include "pcmtp/stream/IcyMetadataClient.hpp"

namespace pcmtp {

StreamSidecar::~StreamSidecar() {
    stop();
}

void StreamSidecar::interrupt_active_socket() {
    const int fd = active_socket_.exchange(-1);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
}

void StreamSidecar::interruptible_sleep(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_cv_.wait_for(lock, duration, [this]() {
        return stop_requested_.load(std::memory_order_relaxed);
    });
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
    wake_cv_.notify_all();
    interrupt_active_socket();
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
            interruptible_sleep(std::chrono::seconds(5));
            if (stop_requested_.load(std::memory_order_relaxed)) {
                return;
            }
            continue;
        }

        IcyMetadataClient::stream_until_stopped(url, handler, stop_requested_, &active_socket_);
        if (stop_requested_.load(std::memory_order_relaxed)) {
            return;
        }
        interruptible_sleep(std::chrono::seconds(3));
    }
}

} // namespace pcmtp
