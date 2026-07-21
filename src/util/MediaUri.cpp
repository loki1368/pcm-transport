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

std::string trim_copy(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
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

std::string normalize_stream_url(const std::string& url) {
    std::string normalized = trim_copy(url);
    if (!is_remote_media_uri(normalized)) {
        return normalized;
    }

    const std::size_t scheme_end = normalized.find("://");
    if (scheme_end == std::string::npos) {
        return normalized;
    }

    std::string scheme = lowercase_copy(normalized.substr(0, scheme_end));
    const std::size_t authority_start = scheme_end + 3;
    std::size_t path_start = normalized.find('/', authority_start);
    if (path_start == std::string::npos) {
        path_start = normalized.size();
    }

    std::string authority = normalized.substr(authority_start, path_start - authority_start);
    std::string path_and_query = normalized.substr(path_start);

    const std::size_t at = authority.rfind('@');
    std::string userinfo;
    if (at != std::string::npos) {
        userinfo = authority.substr(0, at + 1);
        authority = authority.substr(at + 1);
    }

    std::string host;
    std::string port;
    if (!authority.empty() && authority.front() == '[') {
        const std::size_t bracket_end = authority.find(']');
        if (bracket_end != std::string::npos) {
            host = lowercase_copy(authority.substr(0, bracket_end + 1));
            if (bracket_end + 1 < authority.size() && authority[bracket_end + 1] == ':') {
                port = authority.substr(bracket_end + 2);
            }
        } else {
            host = lowercase_copy(authority);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            host = lowercase_copy(authority.substr(0, colon));
            port = authority.substr(colon + 1);
        } else {
            host = lowercase_copy(authority);
        }
    }

    if (!port.empty()) {
        const bool default_http = (scheme == "http" || scheme == "icy") && port == "80";
        const bool default_https = scheme == "https" && port == "443";
        if (default_http || default_https) {
            port.clear();
        }
    }

    while (path_and_query.size() > 1 && path_and_query.back() == '/') {
        path_and_query.pop_back();
    }

    normalized = scheme + "://" + userinfo + host;
    if (!port.empty()) {
        normalized += ":" + port;
    }
    normalized += path_and_query;
    return normalized;
}

} // namespace pcmtp
