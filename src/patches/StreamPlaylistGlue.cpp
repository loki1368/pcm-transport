#include "pcmtp/patches/StreamPlaylistGlue.hpp"

#include <gtk/gtk.h>

#include <algorithm>
#include <cctype>

#include "pcmtp/gui/GtkPlayerWindow.hpp"
#include "pcmtp/patches/PlaylistStreamViewPatches.hpp"
#include "pcmtp/patches/StreamAudioDecoder.hpp"
#include "pcmtp/patches/StreamPlaylistUtils.hpp"
#include "pcmtp/util/Logger.hpp"
#include "pcmtp/util/MediaUri.hpp"

namespace pcmtp {
namespace {

constexpr std::size_t kMaxStreamReconnectAttempts = 5;

enum PlaylistColumns {
    COL_INDEX = 0,
    COL_TRACKNO,
    COL_ARTIST,
    COL_TITLE,
    COL_SOURCE,
    COL_STREAM_BROKEN = patches::playlist_stream_broken_column(),
};

} // namespace

StreamPlaylistGlue::StreamPlaylistGlue(Delegate& delegate) : delegate_(delegate) {}

void StreamPlaylistGlue::append_stream_entry(const std::string& path,
                                             const std::string& hint_title,
                                             const std::string& hint_artist) {
    GtkPlayerWindow& host = delegate_.host();
    GtkPlayerWindow::PlaylistEntry entry;
    entry.audio_file_path = path;
    entry.patch.is_stream = true;
    entry.track_number = static_cast<int>(host.playlist_.size() + 1);
    entry.title = !hint_title.empty() ? hint_title : stream_display_label(path);
    entry.performer = hint_artist;
    entry.start_sample = 0;
    entry.end_sample = 0;
    entry.source_label = stream_display_label(path);
    const std::string stream_hint = !hint_title.empty() ? hint_title : path;
    const std::uint32_t hinted_rate = stream_sample_rate_hint(stream_hint);
    const std::uint16_t hinted_bits = stream_bits_per_sample_hint(stream_hint);
    entry.decoded_format.sample_rate = hinted_rate > 0 ? hinted_rate : 44100;
    entry.decoded_format.channels = 2;
    entry.decoded_format.bits_per_sample = hinted_bits > 0 ? hinted_bits : 16;
    entry.source_sample_rate = entry.decoded_format.sample_rate;
    entry.source_bits_per_sample = entry.decoded_format.bits_per_sample;
    const std::uint32_t target_rate = delegate_.target_sample_rate_for(entry.source_sample_rate);
    const std::uint16_t target_bits = delegate_.target_bits_for(entry.source_bits_per_sample);
    entry.resampled = (target_rate > 0 && target_rate != entry.source_sample_rate);
    entry.resampled_from_rate = entry.resampled ? entry.source_sample_rate : 0;
    entry.bitdepth_converted = (target_bits > 0 && target_bits != entry.source_bits_per_sample);
    entry.native_decode = false;
    entry.processed_by_ffmpeg = true;
    if (entry.resampled) {
        entry.decoded_format.sample_rate = target_rate;
    }
    if (entry.bitdepth_converted) {
        entry.decoded_format.bits_per_sample = target_bits;
    }
    const std::string stream_hint_lower = [&stream_hint]() {
        std::string lower = stream_hint;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return lower;
    }();
    const bool hinted_flac = stream_hint_lower.find("flac") != std::string::npos;
    entry.codec_name = hinted_flac ? "flac" : (is_hls_media_uri(path) ? "hls" : "stream");
    entry.lossless_source = hinted_flac;
    entry.lossy_source = !hinted_flac;
    entry.title = delegate_.safe_utf8_for_display(entry.title);
    entry.performer = delegate_.safe_utf8_for_display(entry.performer);
    entry.source_label = delegate_.safe_utf8_for_display(entry.source_label);
    entry.metadata_state = GtkPlayerWindow::MetadataState::Ready;
    host.playlist_.push_back(entry);
}

void StreamPlaylistGlue::handle_stream_probe_result(StreamPlaybackManager::ProbeResult result) {
    if (delegate_.ui_closing()) {
        return;
    }

    GtkPlayerWindow& host = delegate_.host();
    const std::size_t playlist_index = delegate_.find_playlist_index_by_url(result.url);
    const bool connect_failed = !result.probe_ok || !result.info.live_format_probed;

    if (connect_failed) {
        const std::string error = !result.probe_ok
            ? (result.error.empty() ? std::string("Stream probe failed") : result.error)
            : "Stream unavailable";
        delegate_.stream_manager().note_broken(result.url, error);
        delegate_.track_switch_in_progress() = false;
        delegate_.finish_handled() = false;
        delegate_.stream_manager().set_status_override(std::string());
        Logger::instance().error(std::string("Failed to probe stream: ") + error);
        GtkWidget* msg = gtk_message_dialog_new(GTK_WINDOW(host.window_),
                                                GTK_DIALOG_MODAL,
                                                GTK_MESSAGE_ERROR,
                                                GTK_BUTTONS_CLOSE,
                                                "%s",
                                                error.c_str());
        gtk_dialog_run(GTK_DIALOG(msg));
        gtk_widget_destroy(msg);
        delegate_.notify_mpris_state_changed();
        return;
    }

    if (playlist_index != static_cast<std::size_t>(-1)) {
        GtkPlayerWindow::PlaylistEntry& entry = host.playlist_[playlist_index];
        entry.source_sample_rate = result.info.source_format.sample_rate > 0 ? result.info.source_format.sample_rate : result.info.format.sample_rate;
        entry.source_bits_per_sample = result.info.source_format.bits_per_sample > 0 ? result.info.source_format.bits_per_sample : result.info.format.bits_per_sample;
        entry.decoded_format = result.info.format;
        if (entry.decoded_format.channels == 0) {
            entry.decoded_format.channels = 2;
        }
        if (!result.info.codec_name.empty()) {
            entry.codec_name = result.info.codec_name;
        }
        entry.patch.source_bit_rate = result.info.bit_rate;
        entry.lossless_source = result.info.lossless;
        entry.lossy_source = !result.info.lossless;
        const std::uint32_t target_rate = delegate_.target_sample_rate_for(entry.source_sample_rate);
        const std::uint16_t target_bits = delegate_.target_bits_for(entry.source_bits_per_sample);
        entry.resampled = (target_rate > 0 && target_rate != entry.source_sample_rate);
        entry.resampled_from_rate = entry.resampled ? entry.source_sample_rate : 0;
        entry.bitdepth_converted = (target_bits > 0 && target_bits != entry.source_bits_per_sample);
        if (entry.resampled) {
            entry.decoded_format.sample_rate = target_rate;
        }
        if (entry.bitdepth_converted) {
            entry.decoded_format.bits_per_sample = target_bits;
        }
        entry.patch.stream_format_probed = true;
    }
    if (result.info.live_format_probed) {
        Logger::instance().info("Stream format probed: " + result.url + " -> " +
                                std::to_string(result.info.source_format.sample_rate) + " Hz / " +
                                std::to_string(result.info.source_format.bits_per_sample) + "-bit / " +
                                std::to_string(result.info.source_format.channels) + " ch");
    }

    if (playlist_index == static_cast<std::size_t>(-1)) {
        delegate_.track_switch_in_progress() = false;
        delegate_.finish_handled() = false;
        return;
    }

    delegate_.stream_manager().set_status_override(std::string());
    delegate_.play_track_index_at_offset(playlist_index,
                                         result.playback.offset_samples,
                                         true,
                                         result.playback.preserve_paused,
                                         result.playback.update_mpris_track,
                                         true);
}

void StreamPlaylistGlue::begin_async_stream_probe_and_play(std::size_t index,
                                                           std::uint64_t offset_samples,
                                                           bool preserve_paused,
                                                           bool update_mpris_track,
                                                           bool skip_engine_stop,
                                                           std::uint64_t probe_generation) {
    GtkPlayerWindow& host = delegate_.host();
    if (index >= host.playlist_.size()) {
        delegate_.track_switch_in_progress() = false;
        delegate_.finish_handled() = false;
        return;
    }

    const GtkPlayerWindow::PlaylistEntry& entry = host.playlist_[index];
    const std::uint32_t source_rate = entry.source_sample_rate > 0 ? entry.source_sample_rate : entry.decoded_format.sample_rate;
    const std::uint16_t source_bits = entry.source_bits_per_sample > 0 ? entry.source_bits_per_sample : entry.decoded_format.bits_per_sample;
    const std::uint32_t target_rate = delegate_.target_sample_rate_for(source_rate);
    const std::uint16_t target_bits = delegate_.target_bits_for(source_bits);
    const std::uint32_t forced_rate = (target_rate > 0 && target_rate != source_rate) ? target_rate : 0;
    const std::uint16_t forced_bits = (target_bits > 0 && target_bits != source_bits) ? target_bits : 0;

    delegate_.stream_manager().set_status_override("Probing stream...");
    delegate_.refresh_display();

    StreamPlaybackManager::ProbePlaybackRequest request;
    request.index = index;
    request.offset_samples = offset_samples;
    request.preserve_paused = preserve_paused;
    request.update_mpris_track = update_mpris_track;
    request.skip_engine_stop = skip_engine_stop;
    delegate_.stream_manager().begin_async_probe(request,
                                                 entry.audio_file_path,
                                                 forced_rate,
                                                 forced_bits,
                                                 probe_generation);
}

void StreamPlaylistGlue::refresh_stream_health_rows_for_url(const std::string& url) {
    GtkPlayerWindow& host = delegate_.host();
    if (host.playlist_store_ == nullptr || host.playlist_.empty()) {
        return;
    }

    const std::string normalized = normalize_stream_url(url);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(host.playlist_store_), &iter);
    while (valid) {
        int index = 0;
        gtk_tree_model_get(GTK_TREE_MODEL(host.playlist_store_), &iter, COL_INDEX, &index, -1);
        if (index >= 0 && static_cast<std::size_t>(index) < host.playlist_.size()) {
            const GtkPlayerWindow::PlaylistEntry& entry = host.playlist_[static_cast<std::size_t>(index)];
            if (normalize_stream_url(entry.audio_file_path) == normalized) {
                const bool stream_broken = entry.patch.is_stream && delegate_.stream_manager().is_broken(entry.audio_file_path);
                const std::string trackno = stream_broken
                    ? ("× " + std::to_string(entry.track_number))
                    : std::to_string(entry.track_number);
                gtk_list_store_set(host.playlist_store_, &iter,
                                   COL_TRACKNO, trackno.c_str(),
                                   COL_STREAM_BROKEN, stream_broken ? TRUE : FALSE,
                                   -1);
            }
        }
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(host.playlist_store_), &iter);
    }

    if (host.playlist_view_ != nullptr) {
        gtk_widget_queue_draw(host.playlist_view_);
    }
}

bool StreamPlaylistGlue::prepare_play_track_preamble(std::size_t index,
                                                     std::uint64_t offset_samples,
                                                     bool start_playback,
                                                     bool preserve_paused,
                                                     bool update_mpris_track,
                                                     bool skip_engine_stop,
                                                     std::uint64_t probe_generation) {
    (void)probe_generation;
    GtkPlayerWindow& host = delegate_.host();
    StreamPlaybackManager& manager = delegate_.stream_manager();

    const bool reconnecting_same_stream =
        manager.should_keep_sidecar_for_play(index, host.playlist_[index].patch.is_stream);
    if (!reconnecting_same_stream) {
        manager.stop_sidecar();
    }
    if (!manager.is_reconnect_target(index)) {
        manager.cancel_reconnect();
    }

    if (!reconnecting_same_stream || !start_playback || skip_engine_stop) {
        return false;
    }

    delegate_.track_switch_in_progress() = true;
    delegate_.finish_handled() = true;
    host.engine_.stop();
    host.clear_gapless_chain();
    host.current_track_index_ = index;
    host.select_playlist_row(host.current_track_index_);
    delegate_.refresh_display();

    struct StreamPlaybackResume {
        GtkPlayerWindow* self = nullptr;
        std::size_t index = 0;
        std::uint64_t offset_samples = 0;
        bool preserve_paused = false;
        bool update_mpris_track = true;
    };
    auto* resume = new StreamPlaybackResume{&host, index, offset_samples, preserve_paused, update_mpris_track};
    g_idle_add(+[](gpointer user_data) -> gboolean {
        std::unique_ptr<StreamPlaybackResume> resume(static_cast<StreamPlaybackResume*>(user_data));
        if (resume->self != nullptr && !resume->self->ui_closing_) {
            resume->self->play_track_index_at_offset(resume->index,
                                                     resume->offset_samples,
                                                     true,
                                                     resume->preserve_paused,
                                                     resume->update_mpris_track,
                                                     true);
        }
        return G_SOURCE_REMOVE;
    }, resume);
    return true;
}

std::unique_ptr<IAudioDecoder> StreamPlaylistGlue::open_stream_decoder(std::size_t index,
                                                                       std::uint64_t initial_offset,
                                                                       bool preserve_paused,
                                                                       bool update_mpris_track,
                                                                       bool skip_engine_stop,
                                                                       std::uint64_t probe_generation,
                                                                       bool* async_started) {
    if (async_started != nullptr) {
        *async_started = false;
    }

    GtkPlayerWindow& host = delegate_.host();
    GtkPlayerWindow::PlaylistEntry& entry = host.playlist_[index];
    if (!entry.patch.stream_format_probed) {
        begin_async_stream_probe_and_play(index,
                                          initial_offset,
                                          preserve_paused,
                                          update_mpris_track,
                                          skip_engine_stop,
                                          probe_generation);
        if (async_started != nullptr) {
            *async_started = true;
        }
        return nullptr;
    }

    std::unique_ptr<IAudioDecoder> decoder = host.create_decoder_for_entry(entry, false);
    decoder->open(entry.audio_file_path);
    const AudioFormat stream_format = decoder->format();
    entry.decoded_format = stream_format;
    entry.source_sample_rate = stream_format.sample_rate;
    entry.source_bits_per_sample = stream_format.bits_per_sample;
    entry.patch.stream_format_probed = true;
    Logger::instance().info("Streaming playback: " + entry.audio_file_path + " (" +
                            std::to_string(stream_format.sample_rate) + " Hz)");
    delegate_.stream_manager().start_sidecar(entry.audio_file_path);
    return decoder;
}

void StreamPlaylistGlue::on_stream_play_succeeded(std::size_t index) {
    GtkPlayerWindow& host = delegate_.host();
    if (index < host.playlist_.size() && host.playlist_[index].patch.is_stream) {
        delegate_.stream_manager().clear_reconnect_after_success();
    }
}

void StreamPlaylistGlue::on_stream_play_failed(const std::string& url, const std::string& error) {
    delegate_.stream_manager().note_broken(url, error);
}

void StreamPlaylistGlue::on_ui_timer_tick(const PlaybackStatusSnapshot& status) {
    GtkPlayerWindow& host = delegate_.host();
    if (!host.playlist_.empty() && host.current_track_index_ < host.playlist_.size()) {
        const GtkPlayerWindow::PlaylistEntry& current = host.playlist_[host.current_track_index_];
        if (current.patch.is_stream) {
            delegate_.stream_manager().update_from_playback(current.audio_file_path, status.playing, status.paused);
        } else {
            delegate_.stream_manager().reset_health_tracking();
        }
    } else {
        delegate_.stream_manager().reset_health_tracking();
    }

    delegate_.stream_manager().tick_reconnect(std::chrono::steady_clock::now());
}

void StreamPlaylistGlue::on_transport_finished(std::size_t finished_index, bool* should_advance) {
    GtkPlayerWindow& host = delegate_.host();
    if (should_advance == nullptr ||
        finished_index >= host.playlist_.size() ||
        !host.playlist_[finished_index].patch.is_stream) {
        return;
    }

    *should_advance = false;
    const std::string& stream_url = host.playlist_[finished_index].audio_file_path;
    StreamPlaybackManager& manager = delegate_.stream_manager();
    if (manager.reconnect_attempts() < kMaxStreamReconnectAttempts) {
        manager.note_broken(stream_url, "Stream unavailable");
        manager.schedule_reconnect(finished_index, "Reconnecting...");
    } else {
        manager.set_status_override("Stream unavailable");
        manager.cancel_reconnect();
        manager.note_broken(stream_url, "Stream unavailable");
    }
}

bool StreamPlaylistGlue::try_load_stream_source(GtkPlayerWindow& window,
                                                const std::string& path,
                                                std::vector<std::string>* accepted_sources,
                                                bool quiet) {
    if (!StreamAudioDecoder::is_stream_uri(path)) {
        return false;
    }

    const std::size_t before = window.playlist_.size();
    try {
        window.stream_glue_->append_stream_entry(path);
    } catch (const std::exception& ex) {
        const std::string message = std::string("Cannot load stream: ") + path + " (" + ex.what() + ")";
        if (quiet) {
            Logger::instance().debug(message);
        } else {
            Logger::instance().error(message);
        }
    }

    if (accepted_sources != nullptr && window.playlist_.size() > before) {
        accepted_sources->push_back(path);
    }
    return true;
}

bool StreamPlaylistGlue::entry_playable(bool is_stream, bool metadata_ready) const {
    return is_stream || metadata_ready;
}

std::size_t StreamPlaylistGlue::append_source_stream_placeholder(GtkPlayerWindow& host,
                                                               const std::string& path,
                                                               const std::string& hint_title,
                                                               const std::string& hint_artist) {
    if (!StreamAudioDecoder::is_stream_uri(path)) {
        return 0;
    }
    if (host.stream_glue_ != nullptr) {
        host.stream_glue_->append_stream_entry(path, hint_title, hint_artist);
    }
    return 1;
}

bool StreamPlaylistGlue::begin_play_track(std::size_t index,
                                          std::uint64_t offset_samples,
                                          bool start_playback,
                                          bool preserve_paused,
                                          bool update_mpris_track,
                                          bool skip_engine_stop,
                                          std::uint64_t* probe_generation_out) {
    GtkPlayerWindow& host = delegate_.host();
    if (probe_generation_out != nullptr) {
        *probe_generation_out = 0;
    }
    if (host.playlist_loading_ || index >= host.playlist_.size()) {
        return true;
    }
    const GtkPlayerWindow::PlaylistEntry& entry = host.playlist_[index];
    const bool metadata_ready = entry.metadata_state == GtkPlayerWindow::MetadataState::Ready;
    if (!entry_playable(entry.patch.is_stream, metadata_ready)) {
        return true;
    }

    const std::uint64_t probe_generation = delegate_.stream_manager().bump_probe_generation();
    if (probe_generation_out != nullptr) {
        *probe_generation_out = probe_generation;
    }
    return prepare_play_track_preamble(index,
                                       offset_samples,
                                       start_playback,
                                       preserve_paused,
                                       update_mpris_track,
                                       skip_engine_stop,
                                       probe_generation);
}

void StreamPlaylistGlue::handle_playback_error(GtkPlayerWindow& host,
                                               bool is_stream,
                                               const std::string& audio_file_path,
                                               const std::string& error) {
    if (is_stream) {
        on_stream_play_failed(audio_file_path, error);
    }
    GtkWidget* msg = gtk_message_dialog_new(GTK_WINDOW(host.window_),
                                            GTK_DIALOG_MODAL,
                                            GTK_MESSAGE_ERROR,
                                            GTK_BUTTONS_CLOSE,
                                            "%s",
                                            error.c_str());
    gtk_dialog_run(GTK_DIALOG(msg));
    gtk_widget_destroy(msg);
}

} // namespace pcmtp
