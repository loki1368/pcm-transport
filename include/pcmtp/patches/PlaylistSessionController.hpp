#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

typedef int gboolean;
typedef void* gpointer;

#include "pcmtp/session/PlaylistSession.hpp"

namespace pcmtp {

// PATCH: playlist session — save/restore orchestration and track conversion.
struct PlaylistSessionEntryData {
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

struct PlaylistSessionRestoreResult {
    std::vector<PlaylistSessionEntryData> entries;
    std::size_t current_index = 0;
};

class PlaylistSessionController {
    friend gboolean playlist_session_focus_idle_cb(gpointer data);

public:
    class Delegate {
    public:
        virtual ~Delegate() = default;
        virtual bool ui_closing() const = 0;
        virtual void apply_restored_session(const PlaylistSessionRestoreResult& result) = 0;
        virtual void finalize_focus_restore(std::size_t index) = 0;
    };

    explicit PlaylistSessionController(Delegate& delegate);

    void save(const std::vector<PlaylistSessionEntryData>& entries, std::size_t current_index) const;
    bool restore();

    static PlaylistSessionTrack track_from_entry(const PlaylistSessionEntryData& entry);
    static PlaylistSessionEntryData entry_from_track(const PlaylistSessionTrack& track);
    static bool track_restorable(const PlaylistSessionTrack& track);
    static bool load_restore_result(PlaylistSessionRestoreResult& out);

private:
    Delegate& delegate_;
};

} // namespace pcmtp
