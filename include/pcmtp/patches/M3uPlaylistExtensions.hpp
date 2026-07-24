#pragma once

#include "pcmtp/playlist/M3uPlaylistReader.hpp"

#include <string>
#include <vector>

namespace pcmtp {

class M3uPlaylistExtensions {
public:
    static bool looks_like_playlist_path(const std::string& path);
    static std::vector<M3uPlaylistEntry> read_entries(const std::string& path);
};

} // namespace pcmtp
