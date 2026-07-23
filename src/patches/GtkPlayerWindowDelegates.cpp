#include "pcmtp/gui/GtkPlayerWindow.hpp"
#include "pcmtp/patches/PlaylistSearchController.hpp"
#include "pcmtp/patches/PlaylistStreamViewPatches.hpp"
#include "pcmtp/patches/StreamPlaylistGlue.hpp"

#include <gtk/gtk.h>

#include "pcmtp/util/TextEncoding.hpp"

namespace pcmtp {

namespace {

enum PlaylistColumns {
    COL_INDEX = 0,
    COL_TRACKNO,
    COL_ARTIST,
    COL_TITLE,
    COL_SOURCE,
    COL_STREAM_BROKEN = patches::playlist_stream_broken_column(),
};

std::string safe_utf8_for_display(const std::string& text_value) {
    if (text_value.empty() ||
        g_utf8_validate(text_value.data(), static_cast<gssize>(text_value.size()), nullptr)) {
        return text_value;
    }
    return text::normalize_metadata_value(text_value);
}

} // namespace

struct GtkPlayerWindow::StreamDelegate final : StreamPlaybackManager::Delegate {
    GtkPlayerWindow* self = nullptr;

    explicit StreamDelegate(GtkPlayerWindow* window) : self(window) {}

    bool ui_closing() const override {
        return self == nullptr || self->ui_closing_;
    }

    void on_now_playing_changed(const std::string& title) override {
        (void)title;
        if (self == nullptr || self->ui_closing_) {
            return;
        }
        if (!self->playlist_.empty() && self->current_track_index_ < self->playlist_.size()) {
            self->update_playlist_row(self->current_track_index_);
        }
        self->refresh_display();
        self->notify_mpris_state_changed();
    }

    void on_status_override_changed() override {
        if (self != nullptr && !self->ui_closing_) {
            self->refresh_display();
        }
    }

    void on_stream_health_changed(const std::string& url) override {
        if (self != nullptr && !self->ui_closing_ && self->stream_glue_ != nullptr) {
            self->stream_glue_->refresh_stream_health_rows_for_url(url);
        }
    }

    void on_probe_finished(StreamPlaybackManager::ProbeResult result) override {
        if (self != nullptr && self->stream_glue_ != nullptr) {
            self->stream_glue_->handle_stream_probe_result(std::move(result));
        }
    }

    void on_reconnect_requested(std::size_t index) override {
        if (self != nullptr && !self->ui_closing_) {
            self->play_track_index(index);
        }
    }

    std::size_t find_playlist_index_by_url(const std::string& url) const override {
        return self != nullptr ? self->find_playlist_index_by_url(url) : static_cast<std::size_t>(-1);
    }
};

struct GtkPlayerWindow::StreamGlueDelegate final : StreamPlaylistGlue::Delegate {
    GtkPlayerWindow* self = nullptr;

    explicit StreamGlueDelegate(GtkPlayerWindow* window) : self(window) {}

    GtkPlayerWindow& host() override { return *self; }

    bool ui_closing() const override {
        return self == nullptr || self->ui_closing_;
    }

    StreamPlaybackManager& stream_manager() override { return *self->stream_manager_; }

    std::uint32_t target_sample_rate_for(std::uint32_t source_rate) const override {
        return self->target_sample_rate_for(source_rate);
    }

    std::uint16_t target_bits_for(std::uint16_t source_bits) const override {
        return self->target_bits_for(source_bits);
    }

    std::size_t find_playlist_index_by_url(const std::string& url) const override {
        return self->find_playlist_index_by_url(url);
    }

    void play_track_index_at_offset(std::size_t index,
                                    std::uint64_t offset_samples,
                                    bool start_playback,
                                    bool preserve_paused,
                                    bool update_mpris_track,
                                    bool skip_engine_stop) override {
        self->play_track_index_at_offset(index,
                                         offset_samples,
                                         start_playback,
                                         preserve_paused,
                                         update_mpris_track,
                                         skip_engine_stop);
    }

    void notify_mpris_state_changed() override {
        self->notify_mpris_state_changed();
    }

    void refresh_display() override {
        self->refresh_display();
    }

    std::string safe_utf8_for_display(const std::string& text) const override {
        return pcmtp::safe_utf8_for_display(text);
    }

    bool& track_switch_in_progress() override {
        return self->track_switch_in_progress_;
    }

    bool& finish_handled() override {
        return self->finish_handled_;
    }
};

struct GtkPlayerWindow::SearchDelegate final : PlaylistSearchController::Delegate {
    GtkPlayerWindow* self = nullptr;

    explicit SearchDelegate(GtkPlayerWindow* window) : self(window) {}

    GtkWidget* window() override {
        return self != nullptr ? self->window_ : nullptr;
    }

    GtkListStore* playlist_store() override {
        return self != nullptr ? self->playlist_store_ : nullptr;
    }

    GtkWidget* playlist_view() override {
        return self != nullptr ? self->playlist_view_ : nullptr;
    }

    GtkWidget* playlist_scrolled() override {
        return self != nullptr ? self->playlist_scrolled_ : nullptr;
    }

    int col_artist() const override {
        return COL_ARTIST;
    }

    int col_title() const override {
        return COL_TITLE;
    }

    bool ui_closing() const override {
        return self == nullptr || self->ui_closing_;
    }

    void select_playlist_row(std::size_t index) override {
        if (self != nullptr) {
            self->select_playlist_row(index);
        }
    }

    void select_and_scroll_playlist_path(GtkTreePath* path, bool center_vertically) override {
        if (self == nullptr || self->playlist_view_ == nullptr || path == nullptr) {
            return;
        }
        GtkTreeView* view = GTK_TREE_VIEW(self->playlist_view_);
        GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
        gtk_tree_selection_unselect_all(selection);
        gtk_tree_selection_select_path(selection, path);
        gtk_tree_view_set_cursor(view, path, nullptr, FALSE);
        gtk_tree_view_scroll_to_cell(view,
                                     path,
                                     nullptr,
                                     TRUE,
                                     center_vertically ? 0.5f : 0.0f,
                                     0.0f);
    }
};

struct GtkPlayerWindow::SessionDelegate final : PlaylistSessionController::Delegate {
    GtkPlayerWindow* self = nullptr;

    explicit SessionDelegate(GtkPlayerWindow* window) : self(window) {}

    bool ui_closing() const override {
        return self == nullptr || self->ui_closing_;
    }

    void apply_restored_session(const PlaylistSessionRestoreResult& result) override {
        if (self == nullptr || self->ui_closing_) {
            return;
        }
        self->playlist_.clear();
        self->playlist_.reserve(result.entries.size());
        for (const PlaylistSessionEntryData& data : result.entries) {
            self->playlist_.push_back(GtkPlayerWindow::playlist_entry_from(data));
        }
        self->current_track_index_ = result.current_index;
        self->rebuild_playlist_view();
        self->select_playlist_row(self->current_track_index_);
        self->track_switch_in_progress_ = false;
        self->finish_handled_ = true;
        self->update_loading_controls();
        self->refresh_display();
        self->mark_mpris_track_changed();
    }

    void finalize_focus_restore(std::size_t index) override {
        if (self == nullptr || self->ui_closing_ || self->playlist_.empty()) {
            return;
        }
        self->select_playlist_row(std::min(index, self->playlist_.size() - 1));
        gtk_widget_grab_focus(self->playlist_view_);
    }
};

GtkPlayerWindow::GtkPlayerWindow(std::size_t transport_buffer_ms)
    : transport_buffer_ms_(transport_buffer_ms),
      engine_(transport_buffer_ms),
      log_path_("pcm_transport.log") {
    initialize_patch_subsystems();
    finish_initialization_after_patch_subsystems();
}

void GtkPlayerWindow::initialize_patch_subsystems() {
    stream_delegate_ = std::make_unique<StreamDelegate>(this);
    stream_manager_ = std::make_unique<StreamPlaybackManager>(*stream_delegate_);
    stream_glue_delegate_ = std::make_unique<StreamGlueDelegate>(this);
    stream_glue_ = std::make_unique<StreamPlaylistGlue>(*stream_glue_delegate_);
    session_delegate_ = std::make_unique<SessionDelegate>(this);
    session_controller_ = std::make_unique<PlaylistSessionController>(*session_delegate_);
    search_delegate_ = std::make_unique<SearchDelegate>(this);
    search_controller_ = std::make_unique<PlaylistSearchController>(*search_delegate_);
}

GtkPlayerWindow::~GtkPlayerWindow() {
    ui_closing_ = true;
    save_playlist_session();
    if (stream_manager_ != nullptr) {
        stream_manager_->shutdown();
    }
    stop_ui_updates();
    cancel_pending_seek();
    stop_metadata_worker();
    mpris_service_.reset();
    stop_playback();
}

PlaylistSessionEntryData GtkPlayerWindow::session_entry_data_from(const GtkPlayerWindow::PlaylistEntry& entry) {
    PlaylistSessionEntryData data;
    data.audio_file_path = entry.audio_file_path;
    data.track_number = entry.track_number;
    data.title = entry.title;
    data.performer = entry.performer;
    data.start_sample = entry.start_sample;
    data.end_sample = entry.end_sample;
    data.source_label = entry.source_label;
    data.decoded_sample_rate = entry.decoded_format.sample_rate;
    data.decoded_channels = entry.decoded_format.channels;
    data.decoded_bits_per_sample = entry.decoded_format.bits_per_sample;
    data.source_sample_rate = entry.source_sample_rate;
    data.source_bits_per_sample = entry.source_bits_per_sample;
    data.native_decode = entry.native_decode;
    data.lossless_source = entry.lossless_source;
    data.lossy_source = entry.lossy_source;
    data.resampled = entry.resampled;
    data.resampled_from_rate = entry.resampled_from_rate;
    data.bitdepth_converted = entry.bitdepth_converted;
    data.processed_by_ffmpeg = entry.processed_by_ffmpeg;
    data.codec_name = entry.codec_name;
    data.cue_track = entry.cue_track;
    data.cue_album_end_sample = entry.cue_album_end_sample;
    data.is_stream = entry.patch.is_stream;
    return data;
}

GtkPlayerWindow::PlaylistEntry GtkPlayerWindow::playlist_entry_from(const PlaylistSessionEntryData& data) {
    GtkPlayerWindow::PlaylistEntry entry;
    entry.audio_file_path = data.audio_file_path;
    entry.track_number = data.track_number;
    entry.title = data.title;
    entry.performer = data.performer;
    entry.start_sample = data.start_sample;
    entry.end_sample = data.end_sample;
    entry.source_label = data.source_label;
    entry.decoded_format.sample_rate = data.decoded_sample_rate;
    entry.decoded_format.channels = data.decoded_channels;
    entry.decoded_format.bits_per_sample = data.decoded_bits_per_sample;
    entry.source_sample_rate = data.source_sample_rate;
    entry.source_bits_per_sample = data.source_bits_per_sample;
    entry.native_decode = data.native_decode;
    entry.lossless_source = data.lossless_source;
    entry.lossy_source = data.lossy_source;
    entry.resampled = data.resampled;
    entry.resampled_from_rate = data.resampled_from_rate;
    entry.bitdepth_converted = data.bitdepth_converted;
    entry.processed_by_ffmpeg = data.processed_by_ffmpeg;
    entry.codec_name = data.codec_name;
    entry.cue_track = data.cue_track;
    entry.cue_album_end_sample = data.cue_album_end_sample;
    entry.patch.is_stream = data.is_stream;
    entry.metadata_state = MetadataState::Ready;
    return entry;
}

} // namespace pcmtp
