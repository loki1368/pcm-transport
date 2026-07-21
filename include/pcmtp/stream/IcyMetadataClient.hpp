#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace pcmtp {

class IcyMetadataClient {
public:
    using MetadataHandler = std::function<void(const std::string& stream_title)>;

    static bool supports_url(const std::string& url);
    static void stream_until_stopped(const std::string& url,
                                     MetadataHandler handler,
                                     const std::atomic<bool>& stop_requested);
};

} // namespace pcmtp
