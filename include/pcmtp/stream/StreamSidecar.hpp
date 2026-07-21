#pragma once

#include <atomic>
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

    std::thread thread_;
    std::mutex mutex_;
    MetadataHandler handler_;
    std::string url_;
    std::atomic<bool> stop_requested_{false};
};

} // namespace pcmtp
