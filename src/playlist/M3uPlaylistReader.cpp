#include "pcmtp/playlist/M3uPlaylistReader.hpp"

#include "pcmtp/util/MediaUri.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace pcmtp {
namespace {

std::string lower_extension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return std::string();
    }
    const std::size_t query = path.find('?', dot);
    const std::size_t end = query == std::string::npos ? path.size() : query;
    std::string ext = path.substr(dot, end - dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

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
    if (is_remote_media_uri(entry)) {
        return entry;
    }
    return directory_name(playlist_path) + "/" + entry;
}

std::string shell_escape_single_quotes(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    escaped.push_back('\'');
    for (char ch : value) {
        if (ch == '\'') {
            escaped += "'\\''";
        } else {
            escaped.push_back(ch);
        }
    }
    escaped.push_back('\'');
    return escaped;
}

std::string read_playlist_text(const std::string& path) {
    if (is_remote_media_uri(path)) {
        const std::string command = "curl -fsSL --max-time 30 " + shell_escape_single_quotes(path);
        std::array<char, 4096> buffer{};
        std::string output;
        FILE* pipe = popen(command.c_str(), "r");
        if (pipe == nullptr) {
            throw std::runtime_error("Cannot fetch remote playlist: " + path);
        }
        while (std::size_t bytes = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
            output.append(buffer.data(), bytes);
        }
        const int status = pclose(pipe);
        if (status != 0 || output.empty()) {
            throw std::runtime_error("Cannot fetch remote playlist: " + path);
        }
        return output;
    }

    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open playlist: " + path);
    }
    std::ostringstream ss;
    ss << input.rdbuf();
    return ss.str();
}

bool parse_extinf_line(const std::string& line, int* duration_seconds, std::string* title, std::string* artist) {
    if (!starts_with_ci(line, "#EXTINF:")) {
        return false;
    }
    const std::string payload = trim_copy(line.substr(8));
    const std::size_t comma = payload.find(',');
    if (comma == std::string::npos) {
        return false;
    }

    const std::string duration_text = trim_copy(payload.substr(0, comma));
    std::string info = trim_copy(payload.substr(comma + 1));
    try {
        *duration_seconds = static_cast<int>(std::llround(std::stod(duration_text)));
    } catch (...) {
        *duration_seconds = -1;
    }

    const std::size_t separator = info.find(" - ");
    if (separator != std::string::npos) {
        *artist = trim_copy(info.substr(0, separator));
        *title = trim_copy(info.substr(separator + 3));
    } else {
        *title = trim_copy(info);
        artist->clear();
    }
    if (artist->empty() && !title->empty() && (*title)[0] == '-') {
        std::size_t start = 1;
        while (start < title->size() && std::isspace(static_cast<unsigned char>((*title)[start])) != 0) {
            ++start;
        }
        *title = trim_copy(title->substr(start));
    }
    return true;
}

std::vector<M3uPlaylistEntry> parse_playlist_text(const std::string& playlist_path, const std::string& text) {
    std::string normalized = text;
    if (normalized.size() >= 3 && static_cast<unsigned char>(normalized[0]) == 0xEF &&
        static_cast<unsigned char>(normalized[1]) == 0xBB &&
        static_cast<unsigned char>(normalized[2]) == 0xBF) {
        normalized.erase(0, 3);
    }

    std::vector<M3uPlaylistEntry> entries;
    M3uPlaylistEntry pending{};
    bool have_pending = false;

    std::istringstream lines(normalized);
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
            if (starts_with_ci(line, "#EXTINF:")) {
                have_pending = parse_extinf_line(line, &pending.duration_seconds, &pending.title, &pending.artist);
            }
            continue;
        }

        std::string resolved;
        if (starts_with_ci(line, "file://")) {
            if (!local_file_uri_to_path(line, resolved)) {
                continue;
            }
        } else {
            resolved = resolve_local_entry(playlist_path, line);
        }
        if (resolved.empty() || resolved == playlist_path) {
            continue;
        }

        M3uPlaylistEntry entry;
        entry.location = std::move(resolved);
        if (have_pending) {
            entry.title = pending.title;
            entry.artist = pending.artist;
            entry.duration_seconds = pending.duration_seconds;
            pending = M3uPlaylistEntry{};
            have_pending = false;
        }
        entries.push_back(std::move(entry));
    }

    return entries;
}

} // namespace

bool M3uPlaylistReader::looks_like_playlist_path(const std::string& path) {
    if (is_remote_media_uri(path)) {
        const std::string ext = lower_extension(path);
        return ext == ".m3u" || ext == ".m3u8";
    }
    const std::string ext = lower_extension(path);
    return ext == ".m3u" || ext == ".m3u8";
}

std::vector<M3uPlaylistEntry> M3uPlaylistReader::read_entries(const std::string& path) {
    return parse_playlist_text(path, read_playlist_text(path));
}

std::vector<std::string> M3uPlaylistReader::read_local_paths(const std::string& path) {
    std::vector<std::string> paths;
    for (const M3uPlaylistEntry& entry : read_entries(path)) {
        paths.push_back(entry.location);
    }
    return paths;
}

} // namespace pcmtp
