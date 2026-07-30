// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pcmtp {

struct CueTrack {
    int number = 0;
    std::string title;
    std::string performer;
    std::string audio_file_path;
    std::uint64_t start_sample = 0;
    std::uint64_t end_sample = 0;
    std::uint64_t start_frame_75 = 0;
    std::uint64_t end_frame_75 = 0;
    bool has_index_00 = false;
    bool has_index_01 = false;
    bool has_end_frame_75 = false;
};

struct CueSheet {
    std::string cue_path;
    std::string audio_file_path;
    std::vector<std::string> audio_file_paths;
    std::string title;
    std::string performer;
    std::vector<CueTrack> tracks;
};

class CueParser {
public:
    static bool looks_like_cue_path(const std::string& path);
    static std::string resolve_audio_file_path(const std::string& path);
    static CueSheet parse_file(const std::string& path, std::uint64_t total_samples_per_channel);
    static std::uint64_t frame75_to_samples(std::uint64_t frame_75,
                                            std::uint32_t sample_rate);
};

} // namespace pcmtp
