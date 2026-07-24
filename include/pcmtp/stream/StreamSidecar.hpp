#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace pcmtp {

class StreamSidecar {
public:
    using MetadataHandler = std::function<void(const std::string& stream_title)>;

    ~StreamSidecar();

    void start(const std::string& stream_url, MetadataHandler handler);
    void stop();

private:
    void worker();
    void interruptible_sleep(std::chrono::milliseconds duration);
    void interrupt_active_socket();

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_cv_;
    MetadataHandler handler_;
    std::string url_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<int> active_socket_{-1};
};

} // namespace pcmtp
