#include "pcmtp/util/MediaUri.hpp"

#include <cctype>

namespace pcmtp {
namespace {

bool starts_with_ci(const std::string& text, const char* prefix) {
    std::size_t i = 0;
    for (; prefix[i] != '\0'; ++i) {
        if (i >= text.size()) {
            return false;
        }
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

bool is_http_media_uri(const std::string& path) {
    return starts_with_ci(path, "http://") || starts_with_ci(path, "https://");
}

bool is_remote_media_uri(const std::string& path) {
    return is_http_media_uri(path) || starts_with_ci(path, "icy://") || starts_with_ci(path, "rtsp://") ||
           starts_with_ci(path, "rtmp://");
}

} // namespace pcmtp
