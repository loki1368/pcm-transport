#include "pcmtp/patches/GtkPlayerMediaKeys.hpp"
#include "pcmtp/patches/GtkPlayerFileChooserPatches.hpp"

#include <gdk/gdkkeysyms.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cctype>
#include <sstream>

#include "pcmtp/gui/GtkPlayerWindow.hpp"
#include "pcmtp/util/MediaUri.hpp"

namespace pcmtp {
namespace {

std::string path_from_mpris_uri(const std::string& uri) {
    if (uri.compare(0, 7, "file://") != 0) {
        return {};
    }

    GError* error = nullptr;
    gchar* path = g_filename_from_uri(uri.c_str(), nullptr, &error);
    if (path == nullptr) {
        if (error != nullptr) {
            g_error_free(error);
        }
        return {};
    }

    const std::string result(path);
    g_free(path);
    return result;
}

std::string lower_extension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) {
        return {};
    }
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string codec_display_name(const std::string& codec_name) {
    if (codec_name.empty()) {
        return "File";
    }
    if (codec_name == "stream") return "Stream";
    if (codec_name == "hls") return "HLS";
    if (codec_name == "mp3") return "MP3";
    if (codec_name == "flac") return "FLAC";
    if (codec_name == "aac") return "AAC";
    if (codec_name == "vorbis") return "Vorbis";
    if (codec_name == "opus") return "Opus";
    if (codec_name == "alac") return "ALAC";
    if (codec_name == "ape") return "APE";
    if (codec_name == "wavpack") return "WavPack";
    if (codec_name == "tta") return "TTA";
    if (codec_name == "tak") return "TAK";
    std::string out = codec_name;
    if (!out.empty()) {
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    }
    return out;
}

std::string codec_label_for_entry(const std::string& codec_name, const std::string& path) {
    if (!codec_name.empty()) {
        if (codec_name.size() >= 4 && codec_name.compare(0, 4, "pcm_") == 0) {
            const std::string ext = lower_extension(path);
            if (ext == ".wav" || ext == ".wave") return "WAV";
            if (ext == ".bwf") return "BWF";
            if (ext == ".aiff" || ext == ".aif") return "AIFF";
            if (ext == ".au" || ext == ".snd") return "AU/SND";
            if (ext == ".caf") return "CAF";
            return "PCM";
        }
        return codec_display_name(codec_name);
    }

    const std::string ext = lower_extension(path);
    if (ext == ".flac") return "FLAC";
    if (ext == ".wav" || ext == ".wave") return "WAV";
    if (ext == ".bwf") return "BWF";
    if (ext == ".au" || ext == ".snd") return "AU/SND";
    if (ext == ".caf") return "CAF";
    if (ext == ".aiff" || ext == ".aif") return "AIFF";
    if (ext == ".ape") return "APE";
    if (ext == ".wv") return "WavPack";
    if (ext == ".m4a") return "M4A";
    if (ext == ".aac") return "AAC";
    if (ext == ".ogg" || ext == ".oga") return "OGG";
    if (ext == ".opus") return "OPUS";
    if (ext == ".tak") return "TAK";
    if (ext == ".tta") return "TTA";
    if (ext == ".wma" || ext == ".asf" || ext == ".xwma") return "WMA";
    if (ext == ".oma" || ext == ".aa3" || ext == ".at3") return "ATRAC";
    if (ext == ".mpc" || ext == ".mp+" || ext == ".mpp") return "MPC";
    if (ext == ".dsf") return "DSF";
    if (ext == ".mp3") return "MP3";
    return "File";
}

std::string channels_layout_label(std::uint16_t channels) {
    if (channels == 1) {
        return "mono";
    }
    if (channels == 2) {
        return {};
    }
    if (channels > 0) {
        return std::to_string(channels) + " ch";
    }
    return {};
}

} // namespace

namespace patches {

void setup_media_keys(GtkApplication* app, GtkPlayerWindow* window) {
    const GActionEntry actions[] = {
        {"media-play", on_media_play, nullptr, nullptr, nullptr, {0}},
        {"media-pause", on_media_pause, nullptr, nullptr, nullptr, {0}},
        {"media-play-pause", on_media_play_pause, nullptr, nullptr, nullptr, {0}},
        {"media-stop", on_media_stop, nullptr, nullptr, nullptr, {0}},
        {"media-next", on_media_next, nullptr, nullptr, nullptr, {0}},
        {"media-previous", on_media_previous, nullptr, nullptr, nullptr, {0}},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), actions, G_N_ELEMENTS(actions), window);

    static const char* kPlayPauseKeys[] = {"XF86AudioPlay", "XF86AudioPause", nullptr};
    static const char* kStopKeys[] = {"XF86AudioStop", nullptr};
    static const char* kNextKeys[] = {"XF86AudioNext", nullptr};
    static const char* kPreviousKeys[] = {"XF86AudioPrev", nullptr};

    gtk_application_set_accels_for_action(app, "app.media-play-pause", kPlayPauseKeys);
    gtk_application_set_accels_for_action(app, "app.media-stop", kStopKeys);
    gtk_application_set_accels_for_action(app, "app.media-next", kNextKeys);
    gtk_application_set_accels_for_action(app, "app.media-previous", kPreviousKeys);
}

void handle_media_play(GtkPlayerWindow* window) {
    if (window != nullptr && window->playback_available()) {
        window->mpris_play();
    }
}

void handle_media_pause(GtkPlayerWindow* window) {
    if (window == nullptr) {
        return;
    }
    if (window->engine_.is_playing() && !window->engine_.is_paused()) {
        window->engine_.pause();
        window->notify_mpris_state_changed();
    }
}

void handle_media_stop(GtkPlayerWindow* window) {
    if (window != nullptr) {
        window->stop_playback();
    }
}

void handle_media_next(GtkPlayerWindow* window) {
    if (window != nullptr && window->playback_available()) {
        window->mpris_advance_track(1);
    }
}

void handle_media_previous(GtkPlayerWindow* window) {
    if (window != nullptr && window->playback_available()) {
        window->mpris_advance_track(-1);
    }
}

void handle_media_play_pause(GtkPlayerWindow* window) {
    if (window == nullptr) {
        return;
    }
    window->update_playlist_selection_from_ui();
    if (window->engine_.is_playing() && window->engine_.is_paused()) {
        window->engine_.resume();
    } else if (window->engine_.is_playing()) {
        window->engine_.pause();
    } else {
        window->mpris_play();
    }
    window->notify_mpris_state_changed();
}

bool handle_media_key(GtkPlayerWindow* window, guint keyval) {
    if (window == nullptr) {
        return false;
    }
    switch (keyval) {
        case GDK_KEY_AudioPlay:
        case GDK_KEY_AudioPause:
            handle_media_play_pause(window);
            return true;
        case GDK_KEY_AudioStop:
            handle_media_stop(window);
            return true;
        case GDK_KEY_AudioNext:
            handle_media_next(window);
            return true;
        case GDK_KEY_AudioPrev:
            handle_media_previous(window);
            return true;
        default:
            return false;
    }
}

void on_media_play(GSimpleAction*, GVariant*, gpointer user_data) {
    handle_media_play(static_cast<GtkPlayerWindow*>(user_data));
}

void on_media_pause(GSimpleAction*, GVariant*, gpointer user_data) {
    handle_media_pause(static_cast<GtkPlayerWindow*>(user_data));
}

void on_media_stop(GSimpleAction*, GVariant*, gpointer user_data) {
    handle_media_stop(static_cast<GtkPlayerWindow*>(user_data));
}

void on_media_next(GSimpleAction*, GVariant*, gpointer user_data) {
    handle_media_next(static_cast<GtkPlayerWindow*>(user_data));
}

void on_media_previous(GSimpleAction*, GVariant*, gpointer user_data) {
    handle_media_previous(static_cast<GtkPlayerWindow*>(user_data));
}

void on_media_play_pause(GSimpleAction*, GVariant*, gpointer user_data) {
    handle_media_play_pause(static_cast<GtkPlayerWindow*>(user_data));
}

gboolean on_window_key_press(GtkWidget*, GdkEvent* event, gpointer user_data) {
    auto* window = static_cast<GtkPlayerWindow*>(user_data);
    if (window == nullptr || event == nullptr || event->type != GDK_KEY_PRESS) {
        return FALSE;
    }
    const auto* key_event = reinterpret_cast<GdkEventKey*>(event);
    return handle_media_key(window, key_event->keyval) ? TRUE : FALSE;
}

} // namespace patches

void GtkPlayerWindow::setup_media_keys(GtkApplication* app) {
    patches::setup_media_keys(app, this);
}

void GtkPlayerWindow::handle_media_play() {
    patches::handle_media_play(this);
}

void GtkPlayerWindow::handle_media_pause() {
    patches::handle_media_pause(this);
}

void GtkPlayerWindow::handle_media_stop() {
    patches::handle_media_stop(this);
}

void GtkPlayerWindow::handle_media_next() {
    patches::handle_media_next(this);
}

void GtkPlayerWindow::handle_media_previous() {
    patches::handle_media_previous(this);
}

void GtkPlayerWindow::handle_media_play_pause() {
    patches::handle_media_play_pause(this);
}

bool GtkPlayerWindow::handle_media_key(guint keyval) {
    return patches::handle_media_key(this, keyval);
}

std::size_t GtkPlayerWindow::find_playlist_index_by_url(const std::string& url) const {
    const std::string normalized = normalize_stream_url(url);
    for (std::size_t i = 0; i < playlist_.size(); ++i) {
        if (normalize_stream_url(playlist_[i].audio_file_path) == normalized) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

void GtkPlayerWindow::sync_playlist_cursor_to_selection() {
    if (playlist_.empty() || playlist_view_ == nullptr) {
        return;
    }

    GtkTreeView* view = GTK_TREE_VIEW(playlist_view_);
    GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        return;
    }

    GtkTreePath* path = gtk_tree_model_get_path(model, &iter);
    if (path == nullptr) {
        return;
    }
    gtk_tree_view_set_cursor(view, path, nullptr, FALSE);
    gtk_tree_path_free(path);
}

std::size_t GtkPlayerWindow::highlighted_playlist_index() const {
    if (playlist_.empty()) {
        return 0;
    }
    if (playlist_view_ == nullptr) {
        return std::min(current_track_index_, playlist_.size() - 1);
    }

    GtkTreeView* view = GTK_TREE_VIEW(playlist_view_);
    GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        int row_index = -1;
        gtk_tree_model_get(model, &iter, 0, &row_index, -1);
        if (row_index >= 0 && static_cast<std::size_t>(row_index) < playlist_.size()) {
            return static_cast<std::size_t>(row_index);
        }
    }

    return std::min(current_track_index_, playlist_.size() - 1);
}

std::string GtkPlayerWindow::media_source_summary(const PlaylistEntry& entry) const {
    std::ostringstream summary;
    summary << codec_label_for_entry(entry.codec_name, entry.audio_file_path);

    const std::uint32_t rate = entry.source_sample_rate > 0 ? entry.source_sample_rate : entry.decoded_format.sample_rate;
    const std::uint16_t channels = entry.decoded_format.channels > 0 ? entry.decoded_format.channels : 2;
    const std::uint16_t bits = entry.source_bits_per_sample > 0 ? entry.source_bits_per_sample : entry.decoded_format.bits_per_sample;
    const bool format_known = entry.patch.is_stream ? entry.patch.stream_format_probed
                                                    : entry.metadata_state == MetadataState::Ready;

    if (format_known || rate > 0) {
        if (rate > 0) {
            summary << ", " << rate << " Hz";
        }
        const std::string layout = channels_layout_label(channels);
        if (!layout.empty()) {
            summary << ", " << layout;
        }
        if (entry.lossy_source && entry.patch.source_bit_rate >= 1000) {
            summary << ", " << ((entry.patch.source_bit_rate + 500) / 1000) << " kb/s";
        } else if (bits > 0 && !entry.lossy_source) {
            summary << ", " << bits << " bit";
        }
    }

    return summary.str();
}

bool GtkPlayerWindow::validate_mpris_open_uri(const std::string& uri, std::string* resolved_location) const {
    if (uri.compare(0, 7, "file://") == 0) {
        const std::string path = path_from_mpris_uri(uri);
        if (path.empty()) {
            return false;
        }
        if (!g_file_test(path.c_str(), G_FILE_TEST_EXISTS) || !g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR)) {
            return false;
        }
        if (!patches::is_supported_media_path(path)) {
            return false;
        }
        if (resolved_location != nullptr) {
            *resolved_location = path;
        }
        return true;
    }

    if (is_http_media_uri(uri) || uri.compare(0, 6, "icy://") == 0) {
        if (resolved_location != nullptr) {
            *resolved_location = uri;
        }
        return true;
    }

    return false;
}

} // namespace pcmtp
