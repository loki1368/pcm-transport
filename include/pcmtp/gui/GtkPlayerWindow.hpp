// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <gtk/gtk.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <mutex>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pcmtp/core/PcmTypes.hpp"
#include "pcmtp/core/PlaybackEngine.hpp"
#include "pcmtp/cue/CueParser.hpp"
#include "pcmtp/decoder/ExternalAudioDecoder.hpp"
#include "pcmtp/decoder/GaplessChainDecoder.hpp"
#include "pcmtp/hardware/CardProfileRegistry.hpp"
#include "pcmtp/mpris/MprisService.hpp"
#include "pcmtp/playlist/MediaProbe.hpp"
#include "pcmtp/playlist/SourceScanner.hpp"
#include "pcmtp/patches/PlaylistSearchController.hpp"
#include "pcmtp/stream/StreamPlaybackManager.hpp"
#include "pcmtp/util/ProbeCancellation.hpp"

namespace pcmtp {

class GtkPlayerWindow;

namespace patches {
gboolean on_playlist_focus_in(GtkWidget* widget, GdkEventFocus* event, gpointer user_data);
gboolean on_playlist_view_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data);
void on_playlist_selection_changed(GtkTreeSelection* selection, gpointer user_data);
} // namespace patches

class GtkPlayerWindow {
    friend gboolean patches::on_playlist_focus_in(GtkWidget* widget, GdkEventFocus* event, gpointer user_data);
    friend gboolean patches::on_playlist_view_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data);
    friend void patches::on_playlist_selection_changed(GtkTreeSelection* selection, gpointer user_data);

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

    GtkPlayerWindow();
    ~GtkPlayerWindow();

    void show(const std::string& program_name,
              const std::vector<std::string>& source_paths);

    enum class MetadataState {
        Pending,
        Ready,
        Failed
    };

    struct PlaylistEntry {
        std::string audio_file_path;
        std::string top_level_source_path;
        std::string cue_source_path;
        std::uint64_t original_order = 0;
        MetadataState metadata_state = MetadataState::Pending;
        std::uint64_t load_generation = 0;
        int track_number = 0;
        std::string title;
        std::string performer;
        std::string album;
        std::uint64_t start_sample = 0;
        std::uint64_t end_sample = 0;
        std::uint64_t source_start_sample = 0;
        std::uint64_t source_end_sample = 0;
        bool source_supports_trusted_decoder_eof = false;
        ExactPresentationDrainPolicy source_exact_presentation_drain_policy =
            ExactPresentationDrainPolicy::DecoderEofMatchesPresentation;
        bool source_presentation_start_known = false;
        std::uint64_t source_presentation_start_sample = 0;
        SampleExtentKind sample_extent_kind = SampleExtentKind::Unknown;
        SampleExtentSource sample_extent_source = SampleExtentSource::None;
        ExactPresentationDrainPolicy sample_extent_drain_policy =
            ExactPresentationDrainPolicy::DecoderEofMatchesPresentation;
        SampleExtentKind source_sample_extent_kind = SampleExtentKind::Unknown;
        SampleExtentSource source_sample_extent_source = SampleExtentSource::None;
        PresentationEndKind presentation_end_kind = PresentationEndKind::Unknown;
        bool start_sample_known = true;
        bool end_sample_known = false;
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
        bool is_stream = false;
        bool stream_format_probed = false;
        std::uint32_t source_bit_rate = 0;
    };

private:
    struct ActiveTrackTransportState {
        std::uint64_t planned_length_samples = 0;
        bool range_limited = false;
        bool native_decode = false;
        std::string processing_report;
        std::string processing_path;
        bool resampler_runtime_reported = false;
        std::string soxr_runtime_description;
    };

    struct MetadataProbeJob {
        std::uint64_t generation = 0;
        std::string path;
    };

    struct MetadataProbeCompletion {
        std::uint64_t generation = 0;
        std::string path;
        std::string file_identity;
        MediaProbeResult result;
        bool cache_hit = false;
    };

    struct CachedMediaProbe {
        std::string file_identity;
        MediaProbeResult result;
        std::uint64_t last_used_serial = 0;
    };

    struct SourceScanJob {
        std::uint64_t generation = 0;
        std::vector<std::string> source_paths;
        bool replace_playlist = true;
        bool quiet = false;
        bool record_last_sources = false;
        bool restore_saved_sources = false;
        std::string play_after_load_path;
        std::shared_ptr<std::atomic<bool>> cancel_requested;
    };

    struct SourceScanCompletion {
        SourceScanJob job;
        SourceScanResult result;
    };

    enum class PlaylistSelectionMode {
        ExplicitUser,
        FollowTransport,
        FilterCandidate
    };

    enum class PlaylistScrollPolicy {
        PreserveViewport,
        EnsureVisible,
        Center
    };

    enum class PlaylistSortKey {
        None,
        TrackNumber,
        Artist,
        Title,
        Album,
        Source
    };

    enum class PlaylistSortDirection {
        Original,
        Ascending,
        Descending
    };

    enum class PlaybackStartReason {
        Manual,
        Automatic,
        HistoryNavigation,
        PreserveHistory,
        StoppedRandomPreview
    };

    struct RandomNavigationAvailability {
        bool can_go_next = false;
        bool can_go_previous = false;
    };

    struct PendingMetadataPlayback {
        bool active = false;
        std::uint64_t generation = 0;
        std::size_t index = 0;
        std::uint64_t offset_samples = 0;
        bool start_playback = true;
        bool preserve_paused = false;
        bool update_mpris_track = true;
        bool preserve_explicit_selection = false;
        PlaybackStartReason start_reason = PlaybackStartReason::Manual;
        std::string waiting_path;
    };

    struct LastActiveTrackLocator {
        bool valid = false;
        std::string audio_file_path;
        std::uint64_t source_start_sample = 0;
        bool cue_track = false;
    };

    enum class MetadataProbePathState {
        Queued,
        InFlight,
        Completed
    };

    static void on_activate(GtkApplication* app, gpointer user_data);
    static void on_open(GApplication* application,
                        GFile** files,
                        gint file_count,
                        const gchar* hint,
                        gpointer user_data);
    static void on_open_clicked(GtkButton* button, gpointer user_data);
    static gboolean on_open_button_press(GtkWidget* widget, GdkEventButton* event, gpointer user_data);
    static void on_playlist_drag_data_received(GtkWidget* widget,
                                               GdkDragContext* context,
                                               gint x,
                                               gint y,
                                               GtkSelectionData* selection_data,
                                               guint info,
                                               guint time,
                                               gpointer user_data);
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
    static gboolean on_progress_deadline(gpointer user_data);
    static gboolean on_meter_tick(gpointer user_data);
    static gboolean on_playlist_vertical_position_restore_idle(gpointer user_data);
    static void on_playlist_scrolled_size_allocate(GtkWidget* widget,
                                                   GtkAllocation* allocation,
                                                   gpointer user_data);
    static void on_playlist_field_cell_data(GtkTreeViewColumn* column,
                                            GtkCellRenderer* renderer,
                                            GtkTreeModel* model,
                                            GtkTreeIter* iter,
                                            gpointer user_data);
    static gboolean on_playlist_search_window_resize_idle(gpointer user_data);
    static gboolean on_window_configure_event(GtkWidget* widget,
                                              GdkEventConfigure* event,
                                              gpointer user_data);
    static gboolean on_window_delete_event(GtkWidget* widget, GdkEvent* event, gpointer user_data);
    static void on_window_destroy(GtkWidget* widget, gpointer user_data);
    static gboolean on_meter_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data);
    static gboolean on_playback_event_ready(gint fd,
                                            GIOCondition condition,
                                            gpointer user_data);
    static gboolean on_source_scan_completion_dispatch(gpointer user_data);
    static gboolean on_metadata_completion_dispatch(gpointer user_data);
    static gboolean on_progress_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data);
    static gboolean on_progress_button_press(GtkWidget* widget, GdkEventButton* event, gpointer user_data);
    static gboolean on_pending_seek_idle(gpointer user_data);
    static gboolean on_softvol_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data);
    static gboolean on_softvol_button_press(GtkWidget* widget, GdkEventButton* event, gpointer user_data);
    static gboolean on_softvol_motion_notify(GtkWidget* widget, GdkEventMotion* event, gpointer user_data);
    static gboolean on_softvol_button_release(GtkWidget* widget, GdkEventButton* event, gpointer user_data);
    static void on_playlist_row_activated(GtkTreeView* tree_view,
                                          GtkTreePath* path,
                                          GtkTreeViewColumn* column,
                                          gpointer user_data);
    static void on_playlist_column_clicked(GtkTreeViewColumn* column, gpointer user_data);
    static void on_playlist_column_width_notify(GObject* object, GParamSpec* pspec, gpointer user_data);
    static gboolean on_playlist_column_width_save_idle(gpointer user_data);
    static void on_media_play(GSimpleAction* action, GVariant* parameter, gpointer user_data);
    static void on_media_pause(GSimpleAction* action, GVariant* parameter, gpointer user_data);
    static void on_media_play_pause(GSimpleAction* action, GVariant* parameter, gpointer user_data);
    static void on_media_stop(GSimpleAction* action, GVariant* parameter, gpointer user_data);
    static void on_media_next(GSimpleAction* action, GVariant* parameter, gpointer user_data);
    static void on_media_previous(GSimpleAction* action, GVariant* parameter, gpointer user_data);
    static gboolean on_restore_last_sources_idle(gpointer user_data);
    static gboolean on_preferences_save_timeout(gpointer user_data);
    static gboolean on_window_key_press(GtkWidget* widget, GdkEvent* event, gpointer user_data);
    static gboolean on_playlist_view_media_key_press(GtkWidget* widget, GdkEvent* event, gpointer user_data);

    void build_ui(GtkApplication* app);
    void initialize_stream_subsystem();
    std::size_t find_playlist_index_by_url(const std::string& url) const;
    void ensure_stream_service_timer_running();
    void stop_stream_service_timer();
    bool stream_service_timer_needed() const;
    static gboolean on_stream_service_tick(gpointer user_data);

    void append_stream_entry(const std::string& path,
                             const std::string& hint_title = std::string(),
                             const std::string& hint_artist = std::string(),
                             const std::string& top_level_source_path = std::string());
    void handle_stream_probe_result(StreamPlaybackManager::ProbeResult result);
    void begin_async_stream_probe_and_play(std::size_t index,
                                           std::uint64_t offset_samples,
                                           bool preserve_paused,
                                           bool update_mpris_track,
                                           bool skip_engine_stop,
                                           std::uint64_t probe_generation);
    void refresh_stream_health_rows_for_url(const std::string& url);
    bool prepare_play_track_preamble(std::size_t index,
                                     std::uint64_t offset_samples,
                                     bool start_playback,
                                     bool preserve_paused,
                                     bool update_mpris_track,
                                     bool skip_engine_stop,
                                     std::uint64_t probe_generation);
    std::unique_ptr<IAudioDecoder> open_stream_decoder(std::size_t index,
                                                       std::uint64_t initial_offset,
                                                       bool preserve_paused,
                                                       bool update_mpris_track,
                                                       bool skip_engine_stop,
                                                       std::uint64_t probe_generation,
                                                       bool* async_started);
    void on_stream_play_succeeded(std::size_t index);
    void on_stream_play_failed(const std::string& url, const std::string& error);
    void on_stream_ui_timer_tick(const PlaybackStatusSnapshot& status);
    void on_stream_transport_finished(std::size_t finished_index, bool* should_advance);
    bool try_load_stream_source(const std::string& path,
                                std::vector<std::string>* accepted_sources,
                                bool quiet,
                                const std::string& top_level_source_path = std::string());
    bool entry_playable(bool is_stream, bool metadata_ready) const;
    std::size_t append_source_stream_placeholder(const std::string& path,
                                                 const std::string& top_level_source_path,
                                                 const std::string& hint_title = std::string(),
                                                 const std::string& hint_artist = std::string());
    bool begin_play_track_stream_preamble(std::size_t index,
                                          std::uint64_t offset_samples,
                                          bool start_playback,
                                          bool preserve_paused,
                                          bool update_mpris_track,
                                          bool skip_engine_stop,
                                          std::uint64_t* probe_generation_out);
    void handle_stream_playback_error(bool is_stream,
                                      const std::string& audio_file_path,
                                      const std::string& error);
    void apply_stream_mpris_action_wrappers(MprisService::Actions& actions);
    void apply_stream_fields_to_mpris_state(MprisPlayerState& state, const PlaylistEntry& track) const;
    void start_source_scan_worker();
    void stop_source_scan_worker();
    void source_scan_worker_loop();
    void schedule_source_scan_completion_dispatch();
    void remember_open_directory_from_sources(const std::vector<std::string>& paths);
    bool open_source_paths(const std::vector<std::string>& paths,
                           bool replace_playlist,
                           bool quiet,
                           bool record_last_sources,
                           const std::string& play_after_load_path = std::string(),
                           bool restore_saved_sources = false);
    void enqueue_source_scan(const std::vector<std::string>& paths,
                             bool replace_playlist,
                             bool quiet,
                             bool record_last_sources,
                             const std::string& play_after_load_path,
                             bool restore_saved_sources);
    void cancel_source_scan();
    void drain_source_scan_results();
    std::size_t append_source_placeholders(const std::string& path,
                                           const std::string& top_level_source_path,
                                           std::vector<std::string>* probe_paths);
    void start_metadata_worker();
    void stop_metadata_worker();
    void metadata_worker_loop(std::size_t worker_index);
    void schedule_metadata_completion_dispatch();
    void enqueue_initial_metadata_probes(const std::vector<std::string>& paths);
    void enqueue_metadata_probe(const std::string& path, bool move_to_front);
    void drain_metadata_probe_results();
    void apply_metadata_probe_result(std::uint64_t generation,
                                    const std::string& path,
                                    const MediaProbeResult& result);
    bool complete_metadata_probe_path(std::uint64_t generation,
                                    const std::string& path,
                                    const std::string& file_identity,
                                    const MediaProbeResult& result,
                                    bool cache_hit);
    bool prepare_track_for_playback(std::size_t index);
    void prioritize_metadata_probe(const std::string& path);
    void set_pending_metadata_playback(std::size_t index,
                                       std::uint64_t offset_samples,
                                       bool start_playback,
                                       bool preserve_paused,
                                       bool update_mpris_track,
                                       bool preserve_explicit_selection,
                                       PlaybackStartReason start_reason);
    void clear_pending_metadata_play();
    bool advance_pending_metadata_playback(int direction);
    void try_start_pending_metadata_play(const std::string& path);
    bool pending_metadata_playback_valid() const;
    bool metadata_loading_progress_visible(bool transport_playing) const;
    bool current_track_metadata_ready() const;
    void maybe_finish_metadata_load_session();
    void finish_metadata_load_session();
    void update_loading_controls();
    bool playback_available() const;
    std::vector<std::string> load_source_paths(const std::vector<std::string>& paths,
                                               bool replace_playlist,
                                               bool quiet,
                                               bool record_last_sources,
                                               const std::string& play_after_load_path = std::string(),
                                               bool restore_saved_sources = false);
    std::vector<std::string> load_resolved_source_paths(
        const std::vector<ScannedSourcePath>& paths,
        bool replace_playlist,
        bool quiet,
        bool record_last_sources,
        const std::string& play_after_load_path,
        bool restore_saved_sources);
    void finalize_loaded_playlist(bool rebuild_view = true);
    void schedule_last_sources_restore();
    void remember_last_active_track(std::size_t index);
    void cancel_pending_last_active_track_restore();
    void prepare_last_active_track_restore(bool restore_saved_sources,
                                           bool replace_playlist);
    void resolve_pending_last_active_track_restore();
    void apply_last_active_track_restore_centering();
    void commit_recovery_checkpoint();
    bool last_active_track_locator_matches(const PlaylistEntry& entry) const;
    void start_current_track(bool restart_if_paused = true);
    void halt_active_transport(bool clear_pending_state);
    void remap_playlist_indices_after_failed_removal(
        const std::vector<std::optional<std::size_t>>& index_remap);
    void stop_playback();
    void play_track_index(std::size_t index,
                          bool preserve_explicit_selection = false,
                          PlaybackStartReason start_reason = PlaybackStartReason::Manual);
    void play_track_index_at_offset(std::size_t index,
                                    std::uint64_t offset_samples,
                                    bool start_playback = true,
                                    bool preserve_paused = false,
                                    bool update_mpris_track = true,
                                    bool preserve_explicit_selection = false,
                                    PlaybackStartReason start_reason = PlaybackStartReason::Manual);
    void open_file_dialog();
    void open_directory_dialog();
    void open_settings_dialog();
    void open_about_dialog();
    void open_eq_dialog();
    void open_alsamixer_for_current_device();
    void open_bitperfect_test_dialog(GtkWidget* parent_dialog, int duration_seconds);
    void stop_bitperfect_test_worker();
    void refresh_device_list();
    void load_preferences();
    std::string serialize_preferences() const;
    void save_preferences();
    void save_preferences_now();
    void flush_preferences_save();
    void begin_continuous_preferences_interaction();
    void mark_continuous_preferences_dirty();
    void commit_continuous_preferences();
    void refresh_display(bool update_text = true,
                         bool update_progress = true);
    void refresh_display(const PlaybackStatusSnapshot& status,
                         bool update_text,
                         bool update_progress);
    void refresh_progress_display(const PlaybackStatusSnapshot& status);
    void pause_playback();
    void resume_playback();
    void schedule_progress_deadline();
    void cancel_progress_deadline();
    void reschedule_progress_deadline();
    guint progress_deadline_delay_ms(const PlaybackTransportSnapshot& transport) const;
    void install_playback_event_bridge();
    void uninstall_playback_event_bridge();
    void handle_playback_event(const PlaybackEvent& event);
    void handle_transport_finished_event();
    void close_finished_transport_for_manual_navigation();
    void ensure_meter_timer_running();
    void sync_meter_timer_to_settings();
    void settle_meter_timer_after_stop();
    void stop_ui_updates();
    void cancel_pending_seek();
    void rebuild_playlist_view(bool reset_column_widths = true);
    void reset_playlist_column_widths();
    bool has_saved_playlist_column_widths() const;
    void apply_saved_playlist_column_widths();
    void capture_playlist_column_widths_from_view();
    void schedule_playlist_column_width_persist();
    void apply_playlist_field_width_limit(bool reset_column_widths);
    void sync_playlist_field_renderer_binding();
    void update_playlist_sort_headers();
    void reset_playlist_sort_state();
    void cycle_playlist_sort(PlaylistSortKey key);
    void apply_playlist_sort(PlaylistSortKey key,
                             PlaylistSortDirection direction,
                             bool center_selection);
    bool playlist_sort_available() const;
    std::optional<std::size_t> selected_playlist_view_index() const;
    void remap_playlist_indices_after_reorder(const std::vector<std::size_t>& index_remap,
                                              std::size_t old_current_index,
                                              bool truncate_active_chain);
    void begin_playlist_selection_sync();
    void end_playlist_selection_sync();
    void rebuild_playlist_search_cache();
    void clear_playlist_search_cache();
    void update_playlist_row(std::size_t index);
    bool select_playlist_row(std::size_t index,
                             PlaylistScrollPolicy scroll_policy = PlaylistScrollPolicy::EnsureVisible);
    void reset_playlist_selection_state(std::size_t index = 0);
    void set_explicit_playlist_selection(std::size_t index);
    void set_filter_candidate_selection(std::size_t index);
    void sync_playlist_selection_after_transport_change(std::size_t index,
                                                        bool preserve_explicit_selection,
                                                        PlaylistScrollPolicy scroll_policy = PlaylistScrollPolicy::EnsureVisible);
    std::size_t playlist_play_target_index() const;
    std::size_t playlist_selection_index_without_filter_candidate() const;
    PlaylistSelectionMode playlist_selection_mode_without_filter_candidate() const;
    void begin_playlist_filter_session();
    void mark_playlist_filter_playback_committed(std::size_t index);
    void finish_playlist_filter_session();
    bool capture_playlist_vertical_position(double* value) const;
    void restore_playlist_vertical_position(double value);
    void cancel_playlist_vertical_position_restore();
    void select_first_filter_candidate();
    void update_playlist_selection_from_ui();
    void update_selected_playlist_index_from_ui();
    void sync_playlist_cursor_to_selection();
    void sync_playlist_selection_to_filter();
    void activate_filtered_playlist_selection();
    void play_filtered_track_index(std::size_t index);
    void apply_playlist_search_handler_connections();
    void apply_playlist_search_ui_state();
    void adjust_playlist_search_window_height(bool enabled,
                                              int preserved_viewport_height = 0);
    void account_playlist_search_window_resize_height(int window_height);
    void cancel_playlist_search_window_resize();
    void complete_playlist_search_window_resize();
    void queue_playlist_layout_reflow();
    void schedule_playlist_search_window_resize();

    std::unique_ptr<IAudioDecoder> create_decoder_for_entry(const PlaylistEntry& entry) const;
    GaplessTrackSpec gapless_spec_for_entry(const PlaylistEntry& entry) const;
    bool entries_share_playback_format(const PlaylistEntry& a, const PlaylistEntry& b) const;
    bool entry_supports_separate_gapless(const PlaylistEntry& entry) const;
    bool entries_share_gapless_transport(const PlaylistEntry& a,
                                         const PlaylistEntry& b,
                                         bool allow_noncontiguous_cue) const;
    bool entries_share_split_cue_file_transport(const PlaylistEntry& a,
                                                const PlaylistEntry& b) const;
    std::size_t cue_chain_end_index(std::size_t index) const;
    std::size_t split_cue_file_chain_end_index(std::size_t index) const;
    std::size_t file_chain_end_index(std::size_t index) const;
    std::uint64_t track_length_samples(const PlaylistEntry& entry) const;
    void activate_gapless_chain(std::size_t start_index, std::size_t end_index);
    void activate_gapless_chain(const std::vector<std::size_t>& playlist_indices);
    void set_gapless_chain_mapping(const std::vector<std::size_t>& playlist_indices,
                                   const std::vector<std::uint64_t>& offsets,
                                   std::uint64_t total_samples,
                                   std::size_t active_segment,
                                   std::size_t decoder_segment_base = 0);
    void clear_gapless_chain();
    void update_gapless_chain_track_from_status(const PlaybackStatusSnapshot& status);
    void update_active_gapless_future_for_playback_mode();
    std::uint64_t current_track_position_from_samples(
        std::uint64_t samples_per_channel,
        std::uint64_t track_length_samples) const;
    std::uint64_t current_track_position_from_status(const PlaybackStatusSnapshot& status) const;
    std::uint64_t current_track_position_from_transport(const PlaybackTransportSnapshot& transport) const;
    std::uint32_t target_sample_rate_for(std::uint32_t source_rate) const;
    std::uint16_t target_bits_for(std::uint16_t source_bits) const;
    std::uint32_t dsd_target_sample_rate_for(std::uint32_t dsd_sample_rate,
                                             std::uint32_t ffmpeg_pcm_rate) const;
    std::uint32_t output_sample_rate_for_entry(const PlaylistEntry& entry) const;
    std::uint32_t playback_sample_rate_for_entry(const PlaylistEntry& entry) const;
    std::uint32_t active_transport_sample_rate(
        bool transport_active,
        const AudioFormat& transport_format,
        const PlaylistEntry& entry) const;
    std::uint64_t active_track_length_samples(
        bool transport_active,
        std::uint64_t decoder_total_samples,
        const PlaylistEntry& entry) const;
    const ActiveTrackTransportState* active_track_transport_state() const;
    std::uint16_t output_bits_for_entry(const PlaylistEntry& entry) const;
    void reset_dsd_pcm_defaults();
    void refresh_entry_processing_metadata(PlaylistEntry& entry);
    void refresh_playlist_processing_metadata();
    void update_clip_indicator(bool clip_detected, std::uint32_t clipped_samples);
    int effective_pre_eq_headroom_tenths_db() const;
    int compute_auto_pre_eq_headroom_tenths_db() const;
    void apply_auto_pre_eq_headroom(bool save_preferences_after = true);
    void draw_tone_response_graph(cairo_t* cr, int width, int height) const;
    std::uint32_t current_tone_control_sample_rate() const;
    std::string processing_rules_report_for_entry(
        const PlaylistEntry& entry,
        const AudioFormat& active_output_format) const;
    std::string processing_path_for_entry(
        const PlaylistEntry& entry,
        const AudioFormat& active_output_format) const;
    std::string current_transport_processing_report() const;
    void refresh_active_alsa_output_diagnostics();
    void refresh_stereo_tonal_dsp_controls(bool playing,
                                           std::uint16_t channels);
    void setup_mpris();
    void setup_media_keys(GtkApplication* app);
    void initialize_playlist_search();
    void handle_media_play();
    void handle_media_pause();
    void handle_media_stop();
    void handle_media_next();
    void handle_media_previous();
    bool handle_media_key(guint keyval);
    void handle_media_play_pause();
    void notify_mpris_state_changed();
    void mark_mpris_track_changed();
    void invalidate_mpris_cover_cache();
    std::string cached_cover_art_for(const std::string& audio_file_path) const;
    std::size_t mpris_playlist_index(bool transport_active) const;
    std::string mpris_track_id_for_index(std::size_t index) const;
    std::string current_mpris_track_id() const;
    MprisPlayerState build_mpris_state() const;
    void mpris_play();
    bool mpris_advance_track(int direction);
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

    void cycle_playback_mode();
    void set_playback_mode(bool repeat_enabled, bool random_enabled);
    void update_playback_mode_ui();
    void reset_random_transport_state(bool clear_played_history);
    void initialize_random_pass_if_needed();
    void begin_new_random_pass(std::uint64_t avoid_first_entry_id = 0);
    void synchronize_random_remaining_with_playlist();
    void record_track_started(std::size_t index, PlaybackStartReason reason);
    void record_random_chain_transition(std::size_t index);
    void anchor_random_stopped_navigation();
    void record_random_stopped_selection(std::size_t index,
                                         PlaybackStartReason reason);
    void clear_random_stopped_preview();
    RandomNavigationAvailability random_navigation_availability(
        bool anchor_to_stopped_selection) const;
    bool random_next_track(std::size_t* index, PlaybackStartReason* reason);
    bool random_previous_track(std::size_t* index);
    std::vector<std::uint64_t> random_future_entry_ids() const;
    std::vector<std::size_t> random_gapless_chain_indices(std::size_t index,
                                                           PlaybackStartReason reason) const;
    std::optional<std::size_t> playlist_index_for_entry_id(std::uint64_t entry_id) const;
    std::uint64_t playlist_entry_id(std::size_t index) const;
    void index_playlist_entry(std::size_t index);
    void rebuild_playlist_entry_indexes();
    void clear_playlist_entry_indexes();
    void remove_random_remaining_entry(std::uint64_t entry_id);
    void trim_playback_history();
    void trim_random_history();
    bool playlist_row_visible(std::size_t index) const;
    PlaylistScrollPolicy automatic_transport_scroll_policy(std::size_t index) const;

    static std::string format_time_seconds(std::uint64_t total_seconds);
    std::string display_title_for(const PlaylistEntry& entry) const;
    std::string media_source_summary(const PlaylistEntry& entry) const;

    GtkApplication* app_ = nullptr;
    GtkWidget* window_ = nullptr;
    GtkWidget* display_track_ = nullptr;
    GtkWidget* display_time_ = nullptr;
    std::string display_time_text_ = "00:00 / 00:00";
    std::uint64_t display_elapsed_seconds_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t display_total_seconds_ = std::numeric_limits<std::uint64_t>::max();
    GtkWidget* display_status_ = nullptr;
    GtkWidget* display_source_ = nullptr;
    GtkWidget* display_path_ = nullptr;
    GtkWidget* display_reserve_ = nullptr;
    GtkWidget* display_meter_ = nullptr;
    GtkWidget* badge_clip_ = nullptr;
    GtkWidget* progress_bar_ = nullptr;
    double meter_level_ = 0.0;
    double meter_target_level_ = 0.0;
    double display_progress_ratio_ = 0.0;
    GtkWidget* badge_box_ = nullptr;
    GtkWidget* badge_lossless_ = nullptr;
    GtkWidget* badge_redbook_ = nullptr;
    GtkWidget* badge_native_ = nullptr;
    GtkWidget* badge_dsp_ = nullptr;
    GtkWidget* badge_random_ = nullptr;
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
    GtkWidget* playlist_panel_ = nullptr;
    GtkWidget* playlist_scrolled_ = nullptr;
    GtkWidget* playlist_view_ = nullptr;
    GtkTreeViewColumn* playlist_expand_column_ = nullptr;
    std::array<GtkTreeViewColumn*, 5> playlist_sort_columns_{{nullptr, nullptr, nullptr, nullptr, nullptr}};
    std::array<GtkWidget*, 5> playlist_sort_header_labels_{{nullptr, nullptr, nullptr, nullptr, nullptr}};
    GtkWidget* diagnostics_active_output_value_ = nullptr;
    std::vector<GtkWidget*> stereo_tonal_dsp_controls_;
    std::optional<bool> applied_stereo_tonal_dsp_controls_enabled_;

    PlaybackEngine engine_;
    std::vector<PlaylistEntry> playlist_;
    std::unordered_map<std::uint64_t, std::size_t> playlist_index_by_entry_id_;
    std::unordered_map<std::string, std::vector<std::uint64_t>> metadata_entry_ids_by_path_;
    std::uint64_t next_playlist_original_order_ = 1;
    PlaylistSortKey playlist_sort_key_ = PlaylistSortKey::None;
    PlaylistSortDirection playlist_sort_direction_ = PlaylistSortDirection::Original;
    std::size_t current_track_index_ = 0;
    std::size_t selected_playlist_index_ = 0;
    PlaylistSelectionMode playlist_selection_mode_ = PlaylistSelectionMode::FollowTransport;
    PlaylistSelectionMode playlist_selection_mode_before_filter_candidate_ =
        PlaylistSelectionMode::FollowTransport;
    std::size_t playlist_selection_index_before_filter_candidate_ = 0;
    bool playlist_filter_candidate_valid_ = false;
    bool playlist_filter_session_active_ = false;
    PlaylistSelectionMode playlist_filter_session_selection_mode_ =
        PlaylistSelectionMode::FollowTransport;
    std::size_t playlist_filter_session_selection_index_ = 0;
    bool playlist_filter_session_scroll_valid_ = false;
    double playlist_filter_session_scroll_value_ = 0.0;
    bool playlist_filter_session_playback_committed_ = false;
    std::size_t playlist_filter_session_committed_index_ = 0;
    guint playlist_vertical_position_restore_idle_id_ = 0;
    double playlist_vertical_position_restore_value_ = 0.0;
    std::string current_device_ = "default";
    std::vector<CardProfileInfo> cards_;
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
    bool repeat_enabled_ = false;
    bool random_enabled_ = false;
    std::string mpris_loop_status_ = "None";
    bool finish_handled_ = false;
    bool track_switch_in_progress_ = false;
    bool active_range_limited_transport_ = false;
    std::string active_gapless_transport_kind_;
    std::string active_output_device_;
    std::vector<ActiveTrackTransportState> active_track_transport_states_;
    bool gapless_chain_active_ = false;
    std::vector<std::size_t> gapless_chain_playlist_indices_;
    std::size_t gapless_chain_active_segment_ = 0;
    std::size_t gapless_chain_decoder_segment_base_ = 0;
    std::vector<std::uint64_t> gapless_chain_offsets_;
    std::uint64_t gapless_chain_total_samples_ = 0;
    std::unordered_set<std::uint64_t> played_entry_ids_;
    std::vector<std::uint64_t> playback_history_entry_ids_;
    bool random_pass_initialized_ = false;
    std::unordered_set<std::uint64_t> random_visited_entry_ids_;
    std::vector<std::uint64_t> random_remaining_entry_ids_;
    std::vector<std::uint64_t> random_history_entry_ids_;
    std::size_t random_history_position_ = std::numeric_limits<std::size_t>::max();
    std::optional<std::uint64_t> random_stopped_preview_entry_id_;
    std::mt19937_64 random_generator_;
    std::thread bitperfect_test_worker_;
    std::shared_ptr<std::atomic<bool>> bitperfect_test_cancel_;
    std::string last_open_directory_;
    std::string saved_last_open_directory_;
    std::thread source_scan_worker_;
    mutable std::mutex source_scan_mutex_;
    std::condition_variable source_scan_cv_;
    std::deque<SourceScanJob> source_scan_jobs_;
    std::deque<SourceScanCompletion> source_scan_completions_;
    std::atomic<bool> source_scan_completion_pending_{false};
    std::atomic<bool> source_scan_dispatch_scheduled_{false};
    std::shared_ptr<std::atomic<bool>> active_source_scan_cancel_;
    bool source_scan_worker_stop_ = false;
    bool source_scan_active_ = false;
    std::uint64_t source_scan_generation_ = 0;
    std::vector<std::thread> metadata_workers_;
    std::vector<std::unique_ptr<ProbeCancellation>> metadata_probe_cancellations_;
    mutable std::mutex metadata_worker_mutex_;
    std::condition_variable metadata_worker_cv_;
    std::deque<MetadataProbeJob> metadata_jobs_;
    std::deque<MetadataProbeCompletion> metadata_completions_;
    std::atomic<bool> metadata_completion_pending_{false};
    std::atomic<bool> metadata_dispatch_scheduled_{false};
    bool metadata_worker_stop_ = false;
    bool playlist_loading_ = false;
    std::uint64_t metadata_generation_ = 0;
    std::size_t metadata_total_files_ = 0;
    std::size_t metadata_completed_files_ = 0;
    std::size_t metadata_failed_files_ = 0;
    std::chrono::steady_clock::time_point metadata_display_last_refresh_{};
    bool metadata_load_quiet_ = false;
    bool metadata_load_replace_playlist_ = false;
    std::vector<std::string> metadata_load_requested_sources_;
    std::string play_after_metadata_path_;
    std::uint64_t play_after_metadata_generation_ = 0;
    PendingMetadataPlayback pending_metadata_playback_;
    std::unordered_map<std::string, MetadataProbePathState> metadata_probe_path_states_;
    std::unordered_map<std::string, CachedMediaProbe> media_probe_cache_;
    std::uint64_t media_probe_cache_serial_ = 0;
    bool restore_last_sources_enabled_ = false;
    bool restore_last_active_track_enabled_ = false;
    std::vector<std::string> last_opened_sources_;
    std::vector<std::string> current_loaded_source_paths_;
    bool current_loaded_sources_initialized_ = false;
    // Recovery values are updated in memory during playback. They are committed
    // only when another preference write is already required or during shutdown.
    LastActiveTrackLocator saved_last_active_track_;
    LastActiveTrackLocator runtime_last_active_track_;
    bool pending_last_active_track_restore_ = false;
    std::uint64_t pending_last_active_track_restore_generation_ = 0;
    bool last_active_track_restore_center_pending_ = false;
    std::size_t last_active_track_restore_center_index_ = 0;
    guint restore_sources_idle_id_ = 0;
    guint preferences_save_timeout_id_ = 0;
    std::string persisted_preferences_snapshot_;
    bool bulk_preferences_update_ = false;
    bool continuous_preferences_dirty_ = false;
    bool continuous_preferences_interaction_active_ = false;
    bool preferences_save_deferred_for_continuous_ = false;
    std::unordered_map<std::string, CueSheet> cue_cache_;
    std::chrono::steady_clock::time_point clip_hold_until_{};
    std::uint32_t clip_hold_samples_ = 0;
    guint progress_deadline_id_ = 0;
    guint meter_timer_id_ = 0;
    guint stream_service_timer_id_ = 0;
    guint playback_event_source_id_ = 0;
    std::chrono::steady_clock::time_point meter_last_update_{};
    bool progress_blink_enabled_ = true;
    std::shared_ptr<std::atomic<bool>> ui_dispatch_lifetime_;
    bool playlist_search_enabled_ = false;
    static constexpr int kDefaultPlaylistFieldWidthChars = 25;
    bool playlist_field_width_limit_enabled_ = false;
    int playlist_field_width_chars_ = kDefaultPlaylistFieldWidthChars;
    GtkCellRenderer* playlist_artist_renderer_ = nullptr;
    GtkCellRenderer* playlist_title_renderer_ = nullptr;
    GtkCellRenderer* playlist_album_renderer_ = nullptr;
    GtkCellRenderer* playlist_source_renderer_ = nullptr;
    GtkTreeViewColumn* playlist_track_column_ = nullptr;
    GtkTreeViewColumn* playlist_artist_column_ = nullptr;
    GtkTreeViewColumn* playlist_title_column_ = nullptr;
    GtkTreeViewColumn* playlist_album_column_ = nullptr;
    GtkTreeViewColumn* playlist_field_width_spacer_column_ = nullptr;
    GtkTreeViewColumn* playlist_source_column_ = nullptr;
    std::array<int, 4> playlist_field_width_initial_caps_ = {{-1, -1, -1, -1}};
    // Persisted pixel widths for # / Artist / Title / Album / Source. 0 = unset.
    std::array<int, 5> playlist_column_widths_ = {{0, 0, 0, 0, 0}};
    bool playlist_column_widths_applying_ = false;
    guint playlist_column_width_save_idle_id_ = 0;
    bool playlist_field_cell_data_active_ = false;
    int playlist_rows_at_startup_ = 12;
    bool playlist_search_window_height_adjusted_ = false;
    int playlist_search_unrealized_height_delta_ = 0;
    bool playlist_search_window_resize_pending_ = false;
    bool playlist_search_window_resize_enabling_ = false;
    int playlist_search_preserved_viewport_height_ = 0;
    int playlist_search_window_resize_last_height_ = 0;
    int playlist_search_window_resize_min_height_ = 0;
    int playlist_search_runtime_height_compensation_ = 0;
    unsigned int playlist_search_window_resize_attempts_ = 0;
    bool playlist_search_window_resize_waiting_for_window_event_ = false;
    guint playlist_search_window_resize_idle_id_ = 0;
    bool playlist_selection_syncing_ = false;
    bool playlist_selection_handler_blocked_ = false;
    unsigned int playlist_selection_sync_depth_ = 0;
    gulong playlist_selection_changed_handler_id_ = 0;
    gulong playlist_key_press_handler_id_ = 0;
    gulong playlist_focus_in_handler_id_ = 0;
    std::string alsa_24bit_container_preference_ = "auto";
    bool realtime_audio_priority_enabled_ = false;
    guint pending_seek_source_id_ = 0;
    bool pending_seek_valid_ = false;
    std::size_t pending_seek_index_ = 0;
    std::uint64_t pending_seek_offset_ = 0;
    bool ui_closing_ = false;
    std::uint64_t mpris_track_epoch_ = 0;
    mutable std::string mpris_cover_cache_directory_;
    mutable std::string mpris_cover_cache_art_path_;
    mutable bool mpris_cover_cache_valid_ = false;
    std::unique_ptr<MprisService> mpris_service_;
    class PlaylistSelectionSignalBlocker;
    struct SearchDelegate;
    std::unique_ptr<SearchDelegate> search_delegate_;
    std::unique_ptr<PlaylistSearchController> search_controller_;
    struct StreamDelegate;
    std::unique_ptr<StreamDelegate> stream_delegate_;
    std::unique_ptr<StreamPlaybackManager> stream_manager_;
};

} // namespace pcmtp
