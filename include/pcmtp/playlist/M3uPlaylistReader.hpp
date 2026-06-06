#pragma once

#include <string>
#include <vector>

namespace pcmtp {

class M3uPlaylistReader {
public:
    static bool looks_like_playlist_path(const std::string& path);
    static std::vector<std::string> read_local_paths(const std::string& path);
};

} // namespace pcmtp
