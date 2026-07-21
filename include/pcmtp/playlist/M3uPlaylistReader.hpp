#pragma once

#include <string>
#include <vector>

namespace pcmtp {

struct M3uPlaylistEntry {
    std::string location;
    std::string title;
    std::string artist;
    int duration_seconds = -1;
};

class M3uPlaylistReader {
public:
    static bool looks_like_playlist_path(const std::string& path);
    static std::vector<std::string> read_local_paths(const std::string& path);
    static std::vector<M3uPlaylistEntry> read_entries(const std::string& path);
};

} // namespace pcmtp
