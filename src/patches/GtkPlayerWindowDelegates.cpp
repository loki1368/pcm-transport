#include "pcmtp/gui/GtkPlayerWindow.hpp"
#include "pcmtp/patches/PlaylistStreamViewPatches.hpp"
#include "pcmtp/patches/StreamPlaylistGlue.hpp"

#include <gtk/gtk.h>

#include "pcmtp/util/TextEncoding.hpp"

namespace pcmtp {

namespace {

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
}

GtkPlayerWindow::~GtkPlayerWindow() {
    ui_closing_ = true;
    if (stream_manager_ != nullptr) {
        stream_manager_->shutdown();
    }
    stop_ui_updates();
    cancel_pending_seek();
    stop_metadata_worker();
    mpris_service_.reset();
    stop_playback();
}

} // namespace pcmtp
