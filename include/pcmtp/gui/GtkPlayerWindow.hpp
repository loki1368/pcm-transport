#pragma once

#include <gtk/gtk.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "pcmtp/core/PcmTypes.hpp"
#include "pcmtp/core/PlaybackEngine.hpp"
#include "pcmtp/cue/CueParser.hpp"
#include "pcmtp/session/PlaylistSession.hpp"
#include "pcmtp/decoder/ExternalAudioDecoder.hpp"
#include "pcmtp/decoder/GaplessChainDecoder.hpp"
#include "pcmtp/dsp/AlsaControlBridge.hpp"
#include "pcmtp/hardware/CardProfileRegistry.hpp"
#include "pcmtp/mpris/MprisService.hpp"
#include "pcmtp/stream/StreamHealthRegistry.hpp"
#include "pcmtp/stream/StreamSidecar.hpp"

namespace pcmtp {

class GtkPlayerWindow {
public:
    struct ResampleRule {
        std::uint32_t from_rate = 0;
        std::uint32_t to_rate = 0;
    };

    struct BitDepthRule {
        std::uint16_t from_bits = 0;
        std::uint16_t to_bits = 0;
    };

    explicit GtkPlayerWindow(std::size_t transport_buffer_ms);
    ~GtkPlayerWindow();

    void show();

private:
    struct PlaylistEntry {
        std::string audio_file_path;
        int track_number = 0;
        std::string title;
        std::string performer;
        std::uint64_t start_sample = 0;
        std::uint64_t end_sample = 0;
        std::string source_label;
        AudioFormat decoded_format{};
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
        bool is_stream = false;
        bool stream_format_probed = false;
        std::uint64_t cue_album_end_sample = 0;
        std::shared_ptr<PcmBuffer> normalized_pcm;
        AudioFormat normalized_format{};
        bool normalization_matches_current = false;
        bool metadata_probed = true;
    };

    struct PlaylistMetadataProbeApply {
        std::size_t index = 0;
        PlaylistEntry entry;
    };

    struct PlaylistMetadataProbeInvoke {
        GtkPlayerWindow* window = nullptr;
        PlaylistMetadataProbeApply apply;
    };

    static void on_activate(GtkApplication* app, gpointer user_data);
    static void on_open_clicked(GtkButton* button, gpointer user_data);
    static void on_play_clicked(GtkButton* button, gpointer user_data);
    static void on_pause_clicked(GtkButton* button, gpointer user_data);
    static void on_stop_clicked(GtkButton* button, gpointer user_data);
    static void on_prev_clicked(GtkButton* button, gpointer user_data);
    static void on_next_clicked(GtkButton* button, gpointer user_data);
    static void on_settings_clicked(GtkButton* button, gpointer user_data);
    static void on_about_clicked(GtkButton* button, gpointer user_data);
    static void on_eq_clicked(GtkButton* button, gpointer user_data);
    static void on_open_alsamixer_clicked(GtkButton* button, gpointer user_data);
    static void on_repeat_clicked(GtkButton* button, gpointer user_data);
    static void on_run_bitperfect_test_clicked(GtkButton* button, gpointer user_data);
    static gboolean on_timer_tick(gpointer user_data);
    static gboolean on_window_delete_event(GtkWidget* widget, GdkEvent* event, gpointer user_data);
    static void on_window_destroy(GtkWidget* widget, gpointer user_data);
    static gboolean on_time_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data);
    static gboolean on_meter_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data);
    static gboolean on_progress_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data);
    static gboolean on_progress_button_press(GtkWidget* widget, GdkEventButton* event, gpointer user_data);
    static gboolean on_pending_seek_timer(gpointer user_data);
    static gboolean on_softvol_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data);
    static gboolean on_softvol_button_press(GtkWidget* widget, GdkEventButton* event, gpointer user_data);
    static gboolean on_softvol_motion_notify(GtkWidget* widget, GdkEventMotion* event, gpointer user_data);
    static gboolean on_softvol_button_release(GtkWidget* widget, GdkEventButton* event, gpointer user_data);
    static void on_playlist_row_activated(GtkTreeView* tree_view,
                                          GtkTreePath* path,
                                          GtkTreeViewColumn* column,
                                          gpointer user_data);
    static void on_playlist_search_changed(GtkEditable* editable, gpointer user_data);
    static gboolean on_playlist_filter_visible(GtkTreeModel* model, GtkTreeIter* iter, gpointer user_data);
    static gboolean on_playlist_view_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data);
    static gboolean on_playlist_typeahead_clear_timeout(gpointer user_data);
    static gboolean on_playlist_focus_in(GtkWidget* widget, GdkEventFocus* event, gpointer user_data);
    static void on_media_play(GSimpleAction* action, GVariant* parameter, gpointer user_data);
    static void on_media_pause(GSimpleAction* action, GVariant* parameter, gpointer user_data);
    static void on_media_stop(GSimpleAction* action, GVariant* parameter, gpointer user_data);
    static void on_media_next(GSimpleAction* action, GVariant* parameter, gpointer user_data);
    static void on_media_previous(GSimpleAction* action, GVariant* parameter, gpointer user_data);

    void build_ui(GtkApplication* app);
    void append_path_to_playlist(const std::string& path, bool defer_metadata_probe = false);
    void probe_playlist_entry(PlaylistEntry& entry, bool background_priority = false);
    void ensure_playlist_entry_probed(std::size_t index);
    void ensure_gapless_neighbors_probed(std::size_t index);
    ExternalAudioInfo probe_external_cached(const std::string& audio_path, bool background_priority = false);
    void schedule_playlist_metadata_probe();
    void schedule_playlist_metadata_probe_if_needed();
    void cancel_playlist_metadata_probe();
    void flush_playlist_metadata_probe_ui_updates();
    static gboolean on_playlist_metadata_probe_ui_idle(gpointer user_data);
    void playlist_metadata_probe_worker();
    static gboolean apply_playlist_metadata_probe(gpointer user_data);
    void update_playlist_view_row(std::size_t index);
    bool find_playlist_view_path_for_index(std::size_t index, GtkTreePath** out_path) const;
    void clear_playlist_search();
    void reset_playlist_typeahead();
    void apply_playlist_typeahead_selection();
    void update_playlist_typeahead_popup();
    void append_media_to_playlist(const std::string& path,
                                  const std::string& hint_title = std::string(),
                                  const std::string& hint_artist = std::string(),
                                  bool defer_metadata_probe = false);
    void append_stream_entry(const std::string& path,
                             const std::string& hint_title = std::string(),
                             const std::string& hint_artist = std::string());
    void start_stream_sidecar(const std::string& stream_url);
    void stop_stream_sidecar(bool wait_for_exit = true);
    void apply_stream_metadata(const std::string& title);
    void schedule_stream_reconnect(std::size_t index);
    void cancel_stream_reconnect();
    void note_stream_broken(const std::string& url, const std::string& error);
    void reset_stream_health_tracking();
    void update_stream_health_from_playback(const PlaybackStatusSnapshot& status);
    static gboolean on_stream_metadata_idle(gpointer user_data);
    void start_current_track(bool restart_if_paused = true);
    void stop_playback();
    void play_track_index(std::size_t index);
    void play_track_index_at_offset(std::size_t index,
                                    std::uint64_t offset_samples,
                                    bool start_playback = true,
                                    bool preserve_paused = false,
                                    bool update_mpris_track = true,
                                    bool skip_engine_stop = false);
    void begin_async_stream_probe_and_play(std::size_t index,
                                           std::uint64_t offset_samples,
                                           bool preserve_paused,
                                           bool update_mpris_track,
                                           bool skip_engine_stop,
                                           std::uint64_t probe_generation);
    void apply_stream_probe_to_entry(PlaylistEntry& entry, const ExternalAudioInfo& info);
    void ensure_stream_probe_worker();
    void enqueue_stream_probe(std::size_t index,
                              std::uint64_t offset_samples,
                              bool preserve_paused,
                              bool update_mpris_track,
                              bool skip_engine_stop,
                              std::uint64_t probe_generation,
                              const std::string& url,
                              std::uint32_t forced_output_sample_rate,
                              std::uint16_t forced_output_bits_per_sample);
    void stream_probe_worker_loop();
    void shutdown_stream_probe_worker();
    bool stream_probe_is_current(std::uint64_t generation) const;
    std::size_t find_playlist_index_by_url(const std::string& url) const;
    void mark_stream_broken_from_probe(const std::string& url, const std::string& error);
    static gboolean on_stream_probe_idle(gpointer user_data);

    struct StreamProbeTask {
        GtkPlayerWindow* self = nullptr;
        std::uint64_t generation = 0;
        std::size_t index = 0;
        std::uint64_t offset_samples = 0;
        bool preserve_paused = false;
        bool update_mpris_track = true;
        bool skip_engine_stop = false;
        std::string url;
        std::uint32_t forced_output_sample_rate = 0;
        std::uint16_t forced_output_bits_per_sample = 0;
    };

    struct StreamProbeResult : StreamProbeTask {
        ExternalAudioInfo info;
        bool probe_ok = false;
        std::string error;
    };

    void open_file_dialog();
    void open_settings_dialog();
    void open_about_dialog();
    void open_eq_dialog();
    void open_alsamixer_for_current_device();
    void open_bitperfect_test_dialog(GtkWidget* parent_dialog, int duration_seconds);
    void refresh_device_list();
    void load_preferences();
    void save_preferences() const;
    void save_playlist_session() const;
    bool restore_playlist_session();
    void finalize_restored_playlist_selection(std::size_t index);
    static gboolean on_restore_playlist_focus_idle(gpointer user_data);
    static PlaylistSessionTrack session_track_from_entry(const PlaylistEntry& entry);
    static PlaylistEntry entry_from_session_track(const PlaylistSessionTrack& track);
    static bool session_track_restorable(const PlaylistSessionTrack& track);
    void refresh_dsp_info_for_current_device();
    void refresh_display(bool update_text = true, bool update_progress = true, bool update_meter = true);
    void stop_ui_updates();
    void cancel_pending_seek();
    void rebuild_playlist_view();
    void refresh_stream_health_rows_for_url(const std::string& url);
    void select_playlist_row(std::size_t index);
    void sync_playlist_cursor_to_selection();
    void update_playlist_selection_from_ui();
    std::size_t highlighted_playlist_index() const;

    std::unique_ptr<IAudioDecoder> create_decoder_for_entry(const PlaylistEntry& entry, bool for_normalization) const;
    GaplessTrackSpec gapless_spec_for_entry(const PlaylistEntry& entry) const;
    bool entries_share_playback_format(const PlaylistEntry& a, const PlaylistEntry& b) const;
    std::size_t cue_chain_end_index(std::size_t index) const;
    std::size_t file_chain_end_index(std::size_t index) const;
    std::uint64_t track_length_samples(const PlaylistEntry& entry) const;
    void activate_gapless_chain(std::size_t start_index, std::size_t end_index);
    void clear_gapless_chain();
    void update_gapless_chain_track_from_status(const PlaybackStatusSnapshot& status);
    std::uint64_t current_track_position_from_status(const PlaybackStatusSnapshot& status) const;
    std::uint32_t target_sample_rate_for(std::uint32_t source_rate) const;
    std::uint16_t target_bits_for(std::uint16_t source_bits) const;
    void refresh_playlist_processing_metadata();
    void invalidate_normalized_playlist();
    void normalize_playlist(GtkWidget* progress_bar = nullptr);
    void update_clip_indicator(bool clip_detected, std::uint32_t clipped_samples);
    int effective_pre_eq_headroom_tenths_db() const;
    int compute_auto_pre_eq_headroom_tenths_db() const;
    void apply_auto_pre_eq_headroom(bool save_preferences_after = true);
    void draw_tone_response_graph(cairo_t* cr, int width, int height) const;
    std::uint32_t current_tone_control_sample_rate() const;
    void setup_mpris();
    void setup_media_keys(GtkApplication* app);
    void handle_media_play();
    void handle_media_pause();
    void handle_media_stop();
    void handle_media_next();
    void handle_media_previous();
    void notify_mpris_state_changed();
    void mark_mpris_track_changed();
    void invalidate_mpris_cover_cache();
    std::string cached_cover_art_for(const std::string& audio_file_path) const;
    std::string current_mpris_track_id() const;
    MprisPlayerState build_mpris_state() const;
    void mpris_play();
    void mpris_advance_track(int direction);
    bool mpris_open_uri(const std::string& uri);
    bool validate_mpris_open_uri(const std::string& uri, std::string* resolved_location) const;
    std::int64_t mpris_seek(std::int64_t offset_usec);
    std::int64_t mpris_set_position(std::int64_t position_usec, const std::string& track_id);
    std::int64_t current_mpris_track_length_usec() const;
    std::int64_t current_mpris_track_position_usec() const;
    void mpris_set_volume(double volume);
    void mpris_set_loop_status(const std::string& loop_status);
    void mpris_set_rate(double rate);
    void mpris_set_fullscreen(bool enabled);
    void mpris_set_shuffle(bool enabled);
    void mpris_raise();

    static std::string format_time(std::uint64_t samples_per_channel, std::uint32_t sample_rate = 44100);
    std::string display_title_for(const PlaylistEntry& entry) const;

    const std::size_t transport_buffer_ms_;
    GtkApplication* app_ = nullptr;
    GtkWidget* window_ = nullptr;
    GtkWidget* display_track_ = nullptr;
    GtkWidget* display_time_ = nullptr;
    std::string display_time_text_ = "00:00 / 00:00";
    GtkWidget* display_status_ = nullptr;
    GtkWidget* display_source_ = nullptr;
    GtkWidget* display_path_ = nullptr;
    GtkWidget* display_reserve_ = nullptr;
    GtkWidget* display_mode_ = nullptr;
    GtkWidget* display_meter_ = nullptr;
    GtkWidget* badge_clip_ = nullptr;
    GtkWidget* progress_bar_ = nullptr;
    double meter_level_ = 0.0;
    double display_progress_ratio_ = 0.0;
    GtkWidget* badge_box_ = nullptr;
    GtkWidget* badge_lossless_ = nullptr;
    GtkWidget* badge_redbook_ = nullptr;
    GtkWidget* badge_native_ = nullptr;
    GtkWidget* badge_dsp_ = nullptr;
    GtkWidget* badge_repeat_ = nullptr;
    GtkWidget* btn_prev_ = nullptr;
    GtkWidget* btn_play_ = nullptr;
    GtkWidget* btn_pause_ = nullptr;
    GtkWidget* btn_stop_ = nullptr;
    GtkWidget* btn_next_ = nullptr;
    GtkWidget* btn_open_ = nullptr;
    GtkWidget* btn_repeat_ = nullptr;
    GtkWidget* btn_settings_ = nullptr;
    GtkWidget* btn_alsamixer_ = nullptr;
    GtkWidget* btn_about_ = nullptr;
    GtkWidget* btn_eq_ = nullptr;
    GtkWidget* controls_wrap_ = nullptr;
    GtkWidget* soft_volume_scale_ = nullptr;
    bool softvol_dragging_ = false;
    GtkListStore* playlist_store_ = nullptr;
    GtkTreeModelFilter* playlist_filter_ = nullptr;
    GtkWidget* playlist_search_entry_ = nullptr;
    GtkWidget* playlist_typeahead_popup_ = nullptr;
    GtkWidget* playlist_typeahead_entry_ = nullptr;
    GtkWidget* playlist_scrolled_ = nullptr;
    GtkWidget* playlist_view_ = nullptr;
    std::string playlist_filter_text_;
    std::string playlist_typeahead_text_;
    guint playlist_typeahead_timeout_id_ = 0;

    PlaybackEngine engine_;
    std::vector<PlaylistEntry> playlist_;
    std::size_t current_track_index_ = 0;
    std::string current_device_ = "default";
    std::vector<CardProfileInfo> cards_;
    DspConnectionInfo current_dsp_info_{};
    bool logging_enabled_ = false;
    bool log_errors_only_ = false;
    std::string log_path_;
    int soft_volume_percent_ = 100;
    int bass_db_ = 0;
    int treble_db_ = 0;
    int pre_eq_headroom_tenths_db_ = 0;
    bool deep_bass_enabled_ = false;
    int deep_bass_preset_ = 0;
    int deep_bass_amount_ = 0;
    bool level_meter_enabled_ = true;
    bool clip_detection_enabled_ = true;
    int bass_shelf_hz_ = 110;
    int treble_shelf_hz_ = 10000;
    std::string resample_quality_ = "maximum";
    std::string bitdepth_quality_ = "tpdf_hp";
    std::vector<ResampleRule> resample_rules_;
    std::vector<BitDepthRule> bitdepth_rules_;
    bool normalization_in_progress_ = false;
    bool repeat_enabled_ = false;
    std::string mpris_loop_status_ = "None";
    bool mpris_shuffle_ = false;
    bool mpris_fullscreen_ = false;
    bool finish_handled_ = false;
    bool track_switch_in_progress_ = false;
    bool gapless_chain_active_ = false;
    std::size_t gapless_chain_start_index_ = 0;
    std::size_t gapless_chain_end_index_ = 0;
    std::vector<std::uint64_t> gapless_chain_offsets_;
    std::uint64_t gapless_chain_total_samples_ = 0;
    std::string last_open_directory_;
    std::unordered_map<std::string, CueSheet> cue_cache_;
    std::unordered_map<std::string, ExternalAudioInfo> external_probe_cache_;
    mutable std::mutex external_probe_cache_mutex_;
    std::thread playlist_metadata_probe_thread_;
    std::atomic<bool> playlist_metadata_probe_cancel_{false};
    std::mutex playlist_metadata_probe_mutex_;
    std::vector<std::size_t> playlist_metadata_probe_pending_ui_;
    guint playlist_metadata_probe_ui_idle_id_ = 0;
    std::chrono::steady_clock::time_point clip_hold_until_{};
    std::uint32_t clip_hold_samples_ = 0;
    guint ui_timer_id_ = 0;
    std::size_t playlist_metadata_probe_index_ = 0;
    unsigned int ui_refresh_tick_ = 0;
    bool progress_blink_enabled_ = true;
    std::string alsa_24bit_container_preference_ = "auto";
    bool realtime_audio_priority_enabled_ = false;
    guint pending_seek_timer_id_ = 0;
    bool pending_seek_valid_ = false;
    std::size_t pending_seek_index_ = 0;
    std::uint64_t pending_seek_offset_ = 0;
    bool ui_closing_ = false;
    std::uint64_t mpris_track_epoch_ = 0;
    mutable std::string mpris_cover_cache_directory_;
    mutable std::string mpris_cover_cache_art_path_;
    mutable bool mpris_cover_cache_valid_ = false;
    std::unique_ptr<MprisService> mpris_service_;
    std::unique_ptr<StreamSidecar> stream_sidecar_;
    StreamHealthRegistry stream_health_;
    std::string stream_now_playing_;
    std::string stream_sidecar_url_;
    std::string stream_status_override_;
    std::string stream_health_track_url_;
    bool stream_health_playing_ = false;
    std::chrono::steady_clock::time_point stream_health_playing_since_{};
    std::size_t stream_reconnect_target_index_ = static_cast<std::size_t>(-1);
    int stream_reconnect_attempts_ = 0;
    bool stream_reconnect_pending_ = false;
    std::chrono::steady_clock::time_point stream_reconnect_due_{};
    std::uint64_t stream_probe_generation_ = 0;
    mutable std::mutex stream_probe_mutex_;
    std::condition_variable stream_probe_cv_;
    std::deque<StreamProbeTask> stream_probe_queue_;
    std::thread stream_probe_thread_;
    bool stream_probe_shutdown_ = false;
};

} // namespace pcmtp
