#pragma once

#include <gtk/gtk.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <mutex>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pcmtp/core/PcmTypes.hpp"
#include "pcmtp/core/PlaybackEngine.hpp"
#include "pcmtp/cue/CueParser.hpp"
#include "pcmtp/decoder/ExternalAudioDecoder.hpp"
#include "pcmtp/decoder/GaplessChainDecoder.hpp"
#include "pcmtp/dsp/AlsaControlBridge.hpp"
#include "pcmtp/hardware/CardProfileRegistry.hpp"
#include "pcmtp/mpris/MprisService.hpp"
#include "pcmtp/playlist/MediaProbe.hpp"
#include "pcmtp/patches/PlaylistSessionController.hpp"
#include "pcmtp/patches/PlaylistSelectionPatches.hpp"
#include "pcmtp/util/ManagedSubprocess.hpp"

namespace pcmtp {

class GtkPlayerWindow;

namespace patches {
void update_current_track_from_playlist_ui(GtkPlayerWindow& window, int index_column);
void save_playlist_session(GtkPlayerWindow& window);
bool restore_playlist_session(GtkPlayerWindow& window);
gboolean on_playlist_focus_in(GtkWidget* widget, GdkEventFocus* event, gpointer user_data);
} // namespace patches

class GtkPlayerWindow {
    friend void patches::update_current_track_from_playlist_ui(GtkPlayerWindow& window, int index_column);
    friend void patches::save_playlist_session(GtkPlayerWindow& window);
    friend bool patches::restore_playlist_session(GtkPlayerWindow& window);
    friend gboolean patches::on_playlist_focus_in(GtkWidget* widget, GdkEventFocus* event, gpointer user_data);

public:
    struct ResampleRule {
        std::uint32_t from_rate = 0;
        std::uint32_t to_rate = 0;
    };

    struct BitDepthRule {
        std::uint16_t from_bits = 0;
        std::uint16_t to_bits = 0;
    };

    struct DsdPcmRule {
        std::uint32_t dsd_sample_rate = 0;
        // Zero preserves FFmpeg's native DSD/8 PCM output rate.
        std::uint32_t pcm_sample_rate = 0;
    };

    explicit GtkPlayerWindow(std::size_t transport_buffer_ms);
    ~GtkPlayerWindow();

    void show();

    enum class MetadataState {
        Pending,
        Ready,
        Failed
    };

    struct PlaylistEntry {
        std::string audio_file_path;
        std::string top_level_source_path;
        MetadataState metadata_state = MetadataState::Pending;
        std::uint64_t load_generation = 0;
        int track_number = 0;
        std::string title;
        std::string performer;
        std::uint64_t start_sample = 0;
        std::uint64_t end_sample = 0;
        std::uint64_t source_start_sample = 0;
        std::uint64_t source_end_sample = 0;
        std::uint64_t cue_start_frame_75 = 0;
        std::uint64_t cue_end_frame_75 = 0;
        bool cue_has_end_frame_75 = false;
        std::string source_label;
        AudioFormat decoded_format{};
        std::uint32_t source_sample_rate = 0;
        std::uint16_t source_bits_per_sample = 0;
        bool native_source_available = false;
        bool native_decode = false;
        bool lossless_source = false;
        bool lossy_source = false;
        bool resampled = false;
        std::uint32_t resampled_from_rate = 0;
        bool bitdepth_converted = false;
        bool processed_by_ffmpeg = false;
        std::string codec_name;
        bool dsd_source = false;
        std::uint32_t dsd_sample_rate = 0;
        bool cue_track = false;
        std::uint64_t cue_album_end_sample = 0;
        std::uint64_t source_cue_album_end_sample = 0;
        std::shared_ptr<PcmBuffer> normalized_pcm;
        AudioFormat normalized_format{};
        bool normalization_matches_current = false;
    };

private:
    struct MetadataProbeJob {
        std::uint64_t generation = 0;
        std::string path;
    };

    struct MetadataProbeCompletion {
        std::uint64_t generation = 0;
        std::string path;
        MediaProbeResult result;
    };

    struct PendingMetadataPlayback {
        bool active = false;
        std::uint64_t generation = 0;
        std::size_t index = 0;
        std::uint64_t offset_samples = 0;
        bool start_playback = true;
        bool preserve_paused = false;
        bool update_mpris_track = true;
        std::string waiting_path;
    };

    enum class MetadataProbePathState {
        Queued,
        InFlight,
        Completed
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
    static void on_media_play(GSimpleAction* action, GVariant* parameter, gpointer user_data);
    static void on_media_pause(GSimpleAction* action, GVariant* parameter, gpointer user_data);
    static void on_media_stop(GSimpleAction* action, GVariant* parameter, gpointer user_data);
    static void on_media_next(GSimpleAction* action, GVariant* parameter, gpointer user_data);
    static void on_media_previous(GSimpleAction* action, GVariant* parameter, gpointer user_data);
    static gboolean on_restore_last_sources_idle(gpointer user_data);

    void build_ui(GtkApplication* app);
    std::size_t append_source_placeholders(const std::string& path,
                                           const std::string& top_level_source_path,
                                           std::vector<std::string>* probe_paths);
    void start_metadata_worker();
    void stop_metadata_worker();
    void metadata_worker_loop(std::size_t worker_index);
    void enqueue_initial_metadata_probes(const std::vector<std::string>& paths);
    void enqueue_metadata_probe(const std::string& path, bool move_to_front);
    void drain_metadata_probe_results();
    void apply_metadata_probe_result(std::uint64_t generation,
                                    const std::string& path,
                                    const MediaProbeResult& result);
    bool complete_metadata_probe_path(std::uint64_t generation,
                                    const std::string& path,
                                    const MediaProbeResult& result);
    bool prepare_track_for_playback(std::size_t index);
    void prioritize_metadata_probe(const std::string& path);
    void set_pending_metadata_playback(std::size_t index,
                                       std::uint64_t offset_samples,
                                       bool start_playback,
                                       bool preserve_paused,
                                       bool update_mpris_track);
    void clear_pending_metadata_play();
    void advance_pending_metadata_playback(int direction);
    void try_start_pending_metadata_play(const std::string& path);
    bool pending_metadata_playback_valid() const;
    bool metadata_loading_progress_visible() const;
    bool current_track_metadata_ready() const;
    void maybe_finish_metadata_load_session();
    void finish_metadata_load_session();
    void update_loading_controls();
    bool playback_available() const;
    std::vector<std::string> load_source_paths(const std::vector<std::string>& paths,
                                               bool replace_playlist,
                                               bool quiet,
                                               bool record_last_sources,
                                               const std::string& play_after_load_path = std::string());
    void finalize_loaded_playlist(bool rebuild_view = true);
    void schedule_last_sources_restore();
    void start_current_track(bool restart_if_paused = true);
    void halt_active_transport(bool clear_pending_state);
    void remap_playlist_indices_after_failed_removal(
        const std::vector<std::optional<std::size_t>>& index_remap);
    void stop_playback();
    void play_track_index(std::size_t index);
    void play_track_index_at_offset(std::size_t index,
                                    std::uint64_t offset_samples,
                                    bool start_playback = true,
                                    bool preserve_paused = false,
                                    bool update_mpris_track = true);
    void open_file_dialog();
    void open_settings_dialog();
    void open_about_dialog();
    void open_eq_dialog();
    void open_alsamixer_for_current_device();
    void open_bitperfect_test_dialog(GtkWidget* parent_dialog, int duration_seconds);
    void refresh_device_list();
    void load_preferences();
    void save_preferences() const;
    void refresh_dsp_info_for_current_device();
    void refresh_display(bool update_text = true, bool update_progress = true, bool update_meter = true);
    void stop_ui_updates();
    void cancel_pending_seek();
    void rebuild_playlist_view();
    void update_playlist_row(std::size_t index);
    void select_playlist_row(std::size_t index);
    void update_playlist_selection_from_ui();
    void sync_playlist_cursor_to_selection();
    void initialize_playlist_session();
    void save_playlist_session() const;
    bool restore_playlist_session();
    std::size_t highlighted_playlist_index() const;
    static PlaylistSessionEntryData session_entry_data_from(const PlaylistEntry& entry);
    static PlaylistEntry playlist_entry_from(const PlaylistSessionEntryData& data);

    std::unique_ptr<IAudioDecoder> create_decoder_for_entry(const PlaylistEntry& entry, bool for_normalization) const;
    GaplessTrackSpec gapless_spec_for_entry(const PlaylistEntry& entry) const;
    bool entries_share_playback_format(const PlaylistEntry& a, const PlaylistEntry& b) const;
    std::size_t cue_chain_end_index(std::size_t index) const;
    std::size_t file_chain_end_index(std::size_t index) const;
    std::uint64_t track_length_samples(const PlaylistEntry& entry) const;
    void activate_gapless_chain(std::size_t start_index, std::size_t end_index);
    void clear_gapless_chain();
    void update_gapless_chain_track_from_status(const PlaybackStatusSnapshot& status);
    std::uint64_t current_track_position_from_samples(std::uint64_t samples_per_channel) const;
    std::uint64_t current_track_position_from_status(const PlaybackStatusSnapshot& status) const;
    std::uint32_t target_sample_rate_for(std::uint32_t source_rate) const;
    std::uint16_t target_bits_for(std::uint16_t source_bits) const;
    std::uint32_t dsd_target_sample_rate_for(std::uint32_t dsd_sample_rate,
                                             std::uint32_t ffmpeg_pcm_rate) const;
    std::uint32_t output_sample_rate_for_entry(const PlaylistEntry& entry) const;
    std::uint16_t output_bits_for_entry(const PlaylistEntry& entry) const;
    void reset_dsd_pcm_defaults();
    void refresh_playlist_processing_metadata();
    void invalidate_normalized_playlist();
    void normalize_playlist(GtkWidget* progress_bar = nullptr);
    void update_clip_indicator(bool clip_detected, std::uint32_t clipped_samples);
    int effective_pre_eq_headroom_tenths_db() const;
    int compute_auto_pre_eq_headroom_tenths_db() const;
    void apply_auto_pre_eq_headroom(bool save_preferences_after = true);
    void draw_tone_response_graph(cairo_t* cr, int width, int height) const;
    std::uint32_t current_tone_control_sample_rate() const;
    std::string current_dsd_conversion_report() const;
    void refresh_active_alsa_output_diagnostics();
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
    bool validate_mpris_file_uri(const std::string& uri, std::string* local_path) const;
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
    static std::string display_title_for(const PlaylistEntry& entry);

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
    GtkWidget* playlist_view_ = nullptr;
    GtkWidget* diagnostics_active_output_value_ = nullptr;

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
    std::vector<DsdPcmRule> dsd_pcm_rules_;
    std::uint16_t dsd_pcm_output_bits_ = 24;
    bool normalization_in_progress_ = false;
    bool repeat_enabled_ = false;
    std::string mpris_loop_status_ = "None";
    bool finish_handled_ = false;
    bool track_switch_in_progress_ = false;
    bool gapless_chain_active_ = false;
    std::size_t gapless_chain_start_index_ = 0;
    std::size_t gapless_chain_end_index_ = 0;
    std::vector<std::uint64_t> gapless_chain_offsets_;
    std::uint64_t gapless_chain_total_samples_ = 0;
    std::string last_open_directory_;
    std::vector<std::thread> metadata_workers_;
    std::vector<std::unique_ptr<ManagedSubprocess>> metadata_probe_processes_;
    mutable std::mutex metadata_worker_mutex_;
    std::condition_variable metadata_worker_cv_;
    std::deque<MetadataProbeJob> metadata_jobs_;
    std::deque<MetadataProbeCompletion> metadata_completions_;
    bool metadata_worker_stop_ = false;
    bool playlist_loading_ = false;
    std::uint64_t metadata_generation_ = 0;
    std::size_t metadata_total_files_ = 0;
    std::size_t metadata_completed_files_ = 0;
    std::size_t metadata_failed_files_ = 0;
    bool metadata_load_quiet_ = false;
    bool metadata_load_record_sources_ = false;
    bool metadata_load_replace_playlist_ = false;
    std::vector<std::string> metadata_load_requested_sources_;
    std::string play_after_metadata_path_;
    std::uint64_t play_after_metadata_generation_ = 0;
    PendingMetadataPlayback pending_metadata_playback_;
    std::unordered_map<std::string, MetadataProbePathState> metadata_probe_path_states_;
    std::unordered_map<std::string, MediaProbeResult> media_probe_cache_;
    bool restore_last_sources_enabled_ = false;
    std::vector<std::string> last_opened_sources_;
    std::vector<std::string> current_loaded_source_paths_;
    guint restore_sources_idle_id_ = 0;
    std::unordered_map<std::string, CueSheet> cue_cache_;
    std::chrono::steady_clock::time_point clip_hold_until_{};
    std::uint32_t clip_hold_samples_ = 0;
    guint ui_timer_id_ = 0;
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
    struct SessionDelegate;
    std::unique_ptr<SessionDelegate> session_delegate_;
    std::unique_ptr<PlaylistSessionController> session_controller_;
};

} // namespace pcmtp
