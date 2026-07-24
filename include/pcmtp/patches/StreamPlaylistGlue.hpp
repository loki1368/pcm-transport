#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pcmtp/decoder/IAudioDecoder.hpp"
#include "pcmtp/core/PlaybackEngine.hpp"
#include "pcmtp/patches/StreamPlaybackManager.hpp"

namespace pcmtp {

class GtkPlayerWindow;

// PATCH: internet-radio — GTK playlist glue for stream entries and probes.
class StreamPlaylistGlue {
public:
    class Delegate {
    public:
        virtual ~Delegate() = default;
        virtual GtkPlayerWindow& host() = 0;
        virtual bool ui_closing() const = 0;
        virtual StreamPlaybackManager& stream_manager() = 0;
        virtual std::uint32_t target_sample_rate_for(std::uint32_t source_rate) const = 0;
        virtual std::uint16_t target_bits_for(std::uint16_t source_bits) const = 0;
        virtual std::size_t find_playlist_index_by_url(const std::string& url) const = 0;
        virtual void play_track_index_at_offset(std::size_t index,
                                                std::uint64_t offset_samples,
                                                bool start_playback,
                                                bool preserve_paused,
                                                bool update_mpris_track,
                                                bool skip_engine_stop) = 0;
        virtual void notify_mpris_state_changed() = 0;
        virtual void refresh_display() = 0;
        virtual std::string safe_utf8_for_display(const std::string& text) const = 0;
        virtual bool& track_switch_in_progress() = 0;
        virtual bool& finish_handled() = 0;
    };

    explicit StreamPlaylistGlue(Delegate& delegate);

    void append_stream_entry(const std::string& path,
                             const std::string& hint_title = std::string(),
                             const std::string& hint_artist = std::string());
    void handle_stream_probe_result(StreamPlaybackManager::ProbeResult result);
    void refresh_stream_health_rows_for_url(const std::string& url);
    void begin_async_stream_probe_and_play(std::size_t index,
                                           std::uint64_t offset_samples,
                                           bool preserve_paused,
                                           bool update_mpris_track,
                                           bool skip_engine_stop,
                                           std::uint64_t probe_generation);

    // Returns true when play_track_index_at_offset should return immediately.
    bool prepare_play_track_preamble(std::size_t index,
                                     std::uint64_t offset_samples,
                                     bool start_playback,
                                     bool preserve_paused,
                                     bool update_mpris_track,
                                     bool skip_engine_stop,
                                     std::uint64_t probe_generation);

    // Opens a probed stream decoder, or starts async probe (returns nullptr with *async_started).
    std::unique_ptr<IAudioDecoder> open_stream_decoder(std::size_t index,
                                                         std::uint64_t initial_offset,
                                                         bool preserve_paused,
                                                         bool update_mpris_track,
                                                         bool skip_engine_stop,
                                                         std::uint64_t probe_generation,
                                                         bool* async_started);

    void on_stream_play_succeeded(std::size_t index);
    void on_stream_play_failed(const std::string& url, const std::string& error);

    void on_ui_timer_tick(const PlaybackStatusSnapshot& status);
    void on_transport_finished(std::size_t finished_index, bool* should_advance);
    static bool try_load_stream_source(GtkPlayerWindow& window,
                                       const std::string& path,
                                       std::vector<std::string>* accepted_sources,
                                       bool quiet);

    bool entry_playable(bool is_stream, bool metadata_ready) const;
    std::size_t append_source_stream_placeholder(GtkPlayerWindow& host,
                                               const std::string& path,
                                               const std::string& hint_title = std::string(),
                                               const std::string& hint_artist = std::string());
    bool begin_play_track(std::size_t index,
                          std::uint64_t offset_samples,
                          bool start_playback,
                          bool preserve_paused,
                          bool update_mpris_track,
                          bool skip_engine_stop,
                          std::uint64_t* probe_generation_out);
    void handle_playback_error(GtkPlayerWindow& host,
                               bool is_stream,
                               const std::string& audio_file_path,
                               const std::string& error);

private:
    Delegate& delegate_;
};

} // namespace pcmtp
