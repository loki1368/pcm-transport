#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pcmtp {

struct CueTrack {
    int number = 0;
    std::string title;
    std::string performer;
    std::uint64_t start_sample = 0;
    std::uint64_t end_sample = 0;
};

struct CueSheet {
    std::string cue_path;
    std::string audio_file_path;
    std::string title;
    std::string performer;
    std::vector<CueTrack> tracks;
};

class CueParser {
public:
    static bool looks_like_cue_path(const std::string& path);
    static std::string resolve_audio_file_path(const std::string& path);
    static CueSheet parse_file(const std::string& path, std::uint64_t total_samples_per_channel);
};

} // namespace pcmtp
