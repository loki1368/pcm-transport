#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pcmtp {

struct PlaylistSessionTrack {
    std::string audio_file_path;
    int track_number = 0;
    std::string title;
    std::string performer;
    std::uint64_t start_sample = 0;
    std::uint64_t end_sample = 0;
    std::string source_label;
    std::uint32_t decoded_sample_rate = 44100;
    std::uint16_t decoded_channels = 2;
    std::uint16_t decoded_bits_per_sample = 16;
    std::uint32_t source_sample_rate = 0;
    std::uint16_t source_bits_per_sample = 0;
    bool native_decode = false;
    bool lossless_source = false;
    bool lossy_source = false;
    bool resampled = false;
    std::uint32_t resampled_from_rate = 0;
    bool bitdepth_converted = false;
    bool processed_by_ffmpeg = false;
    std::string codec_name;
    bool cue_track = false;
    std::uint64_t cue_album_end_sample = 0;
    bool is_stream = false;
};

struct PlaylistSessionSnapshot {
    static constexpr int kFormatVersion = 1;

    std::size_t current_track_index = 0;
    std::vector<PlaylistSessionTrack> tracks;
};

class PlaylistSession {
public:
    static std::string session_path();

    bool load(PlaylistSessionSnapshot& out) const;
    bool save(const PlaylistSessionSnapshot& snapshot) const;
};

} // namespace pcmtp
