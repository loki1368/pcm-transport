#include "pcmtp/playlist/M3uPlaylistReader.hpp"

#include "pcmtp/patches/M3uPlaylistExtensions.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "pcmtp/util/TextEncoding.hpp"

namespace pcmtp {
namespace {

std::string directory_name(const std::string& path) {
    const std::size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return ".";
    }
    if (pos == 0) {
        return path.substr(0, 1);
    }
    return path.substr(0, pos);
}

bool is_absolute_path(const std::string& path) {
    return !path.empty() && path[0] == '/';
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

bool is_remote_or_stream_uri(const std::string& value) {
    return starts_with_ci(value, "http://") || starts_with_ci(value, "https://") ||
           starts_with_ci(value, "ftp://") || starts_with_ci(value, "rtsp://") ||
           starts_with_ci(value, "rtmp://") || starts_with_ci(value, "icy://");
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return -1;
}

std::string percent_decode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = hex_value(value[i + 1]);
            const int lo = hex_value(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i]);
    }
    return out;
}

bool local_file_uri_to_path(const std::string& uri, std::string& out_path) {
    if (!starts_with_ci(uri, "file://")) {
        return false;
    }
    std::string rest = uri.substr(7);
    if (starts_with_ci(rest, "localhost/")) {
        rest = rest.substr(9);
        out_path = "/" + percent_decode(rest);
        return true;
    }
    if (!rest.empty() && rest[0] == '/') {
        out_path = percent_decode(rest);
        return true;
    }
    return false;
}

std::string resolve_local_entry(const std::string& playlist_path, const std::string& entry) {
    std::string local_path;
    if (local_file_uri_to_path(entry, local_path)) {
        return local_path;
    }
    if (is_absolute_path(entry)) {
        return entry;
    }
    return directory_name(playlist_path) + "/" + entry;
}

} // namespace

bool M3uPlaylistReader::looks_like_playlist_path(const std::string& path) {
    return M3uPlaylistExtensions::looks_like_playlist_path(path);
}

std::vector<M3uPlaylistEntry> M3uPlaylistReader::read_entries(const std::string& path) {
    return M3uPlaylistExtensions::read_entries(path);
}

std::vector<std::string> M3uPlaylistReader::read_local_paths(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open playlist: " + path);
    }

    std::ostringstream ss;
    ss << input.rdbuf();
    const std::string normalized_text = pcmtp::text::normalize_text_file_bytes(ss.str());

    std::vector<std::string> paths;
    std::istringstream lines(normalized_text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        line = trim_copy(line);
        if (line.empty()) {
            continue;
        }
        if (!line.empty() && line[0] == '#') {
            continue;
        }
        if (is_remote_or_stream_uri(line)) {
            continue;
        }
        std::string resolved;
        if (starts_with_ci(line, "file://")) {
            if (!local_file_uri_to_path(line, resolved)) {
                continue;
            }
        } else {
            resolved = resolve_local_entry(path, line);
        }
        if (!resolved.empty() && resolved != path) {
            paths.push_back(resolved);
        }
    }
    return paths;
}

} // namespace pcmtp
