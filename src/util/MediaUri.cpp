#include "pcmtp/util/MediaUri.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

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

std::string path_without_query_lower(const std::string& path) {
    std::size_t end = path.size();
    const std::size_t query = path.find('?');
    if (query != std::string::npos) {
        end = query;
    }
    std::string out = path.substr(0, end);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

} // namespace

bool is_http_media_uri(const std::string& path) {
    return starts_with_ci(path, "http://") || starts_with_ci(path, "https://");
}

bool is_remote_media_uri(const std::string& path) {
    return is_http_media_uri(path) || starts_with_ci(path, "icy://") || starts_with_ci(path, "rtsp://") ||
           starts_with_ci(path, "rtmp://");
}

bool is_hls_media_uri(const std::string& path) {
    return path_without_query_lower(path).find(".m3u8") != std::string::npos;
}

bool file_looks_like_hls_playlist(const std::string& path) {
    if (is_remote_media_uri(path)) {
        return is_hls_media_uri(path);
    }
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        return false;
    }
    std::string sample;
    sample.resize(4096);
    input.read(&sample[0], static_cast<std::streamsize>(sample.size()));
    sample.resize(static_cast<std::size_t>(input.gcount()));
    return sample.find("#EXT-X-") != std::string::npos;
}

} // namespace pcmtp
