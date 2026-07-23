#include "pcmtp/patches/PlaylistSessionController.hpp"

#include <gtk/gtk.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unistd.h>

#include "pcmtp/patches/StreamAudioDecoder.hpp"
#include "pcmtp/util/MediaUri.hpp"

namespace pcmtp {
namespace {

bool starts_with_ci(const std::string& text, const char* prefix) {
    const std::size_t len = std::strlen(prefix);
    if (text.size() < len) {
        return false;
    }
    for (std::size_t i = 0; i < len; ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

bool path_is_remote_session_uri(const std::string& path) {
    return is_remote_media_uri(path) || starts_with_ci(path, "ftp://");
}

struct FocusRestoreData {
    PlaylistSessionController::Delegate* delegate = nullptr;
    std::size_t index = 0;
};

} // namespace

gboolean playlist_session_focus_idle_cb(gpointer data) {
    auto* focus_data = static_cast<FocusRestoreData*>(data);
    if (focus_data != nullptr && focus_data->delegate != nullptr && !focus_data->delegate->ui_closing()) {
        focus_data->delegate->finalize_focus_restore(focus_data->index);
    }
    delete focus_data;
    return G_SOURCE_REMOVE;
}

PlaylistSessionController::PlaylistSessionController(Delegate& delegate) : delegate_(delegate) {}

void PlaylistSessionController::save(const std::vector<PlaylistSessionEntryData>& entries,
                                     std::size_t current_index) const {
    PlaylistSessionSnapshot snapshot;
    snapshot.tracks.reserve(entries.size());
    for (const PlaylistSessionEntryData& entry : entries) {
        snapshot.tracks.push_back(track_from_entry(entry));
    }
    if (!entries.empty()) {
        snapshot.current_track_index = current_index;
    }
    PlaylistSession().save(snapshot);
}

bool PlaylistSessionController::restore() {
    PlaylistSessionRestoreResult result;
    if (!load_restore_result(result)) {
        return false;
    }

    delegate_.apply_restored_session(result);

    auto* focus_data = new FocusRestoreData{&delegate_, result.current_index};
    g_idle_add(playlist_session_focus_idle_cb, focus_data);
    return true;
}

PlaylistSessionTrack PlaylistSessionController::track_from_entry(const PlaylistSessionEntryData& entry) {
    PlaylistSessionTrack track;
    track.audio_file_path = entry.audio_file_path;
    track.track_number = entry.track_number;
    track.title = entry.title;
    track.performer = entry.performer;
    track.start_sample = entry.start_sample;
    track.end_sample = entry.end_sample;
    track.source_label = entry.source_label;
    track.decoded_sample_rate = entry.decoded_sample_rate;
    track.decoded_channels = entry.decoded_channels;
    track.decoded_bits_per_sample = entry.decoded_bits_per_sample;
    track.source_sample_rate = entry.source_sample_rate;
    track.source_bits_per_sample = entry.source_bits_per_sample;
    track.native_decode = entry.native_decode;
    track.lossless_source = entry.lossless_source;
    track.lossy_source = entry.lossy_source;
    track.resampled = entry.resampled;
    track.resampled_from_rate = entry.resampled_from_rate;
    track.bitdepth_converted = entry.bitdepth_converted;
    track.processed_by_ffmpeg = entry.processed_by_ffmpeg;
    track.codec_name = entry.codec_name;
    track.cue_track = entry.cue_track;
    track.cue_album_end_sample = entry.cue_album_end_sample;
    track.is_stream = entry.is_stream || path_is_remote_session_uri(entry.audio_file_path);
    return track;
}

PlaylistSessionEntryData PlaylistSessionController::entry_from_track(const PlaylistSessionTrack& track) {
    PlaylistSessionEntryData entry;
    entry.audio_file_path = track.audio_file_path;
    entry.track_number = track.track_number;
    entry.title = track.title;
    entry.performer = track.performer;
    entry.start_sample = track.start_sample;
    entry.end_sample = track.end_sample;
    entry.source_label = track.source_label;
    entry.decoded_sample_rate = track.decoded_sample_rate;
    entry.decoded_channels = track.decoded_channels;
    entry.decoded_bits_per_sample = track.decoded_bits_per_sample;
    entry.source_sample_rate = track.source_sample_rate;
    entry.source_bits_per_sample = track.source_bits_per_sample;
    entry.native_decode = track.native_decode;
    entry.lossless_source = track.lossless_source;
    entry.lossy_source = track.lossy_source;
    entry.resampled = track.resampled;
    entry.resampled_from_rate = track.resampled_from_rate;
    entry.bitdepth_converted = track.bitdepth_converted;
    entry.processed_by_ffmpeg = track.processed_by_ffmpeg;
    entry.codec_name = track.codec_name;
    entry.cue_track = track.cue_track;
    entry.cue_album_end_sample = track.cue_album_end_sample;
    entry.is_stream = track.is_stream || StreamAudioDecoder::is_stream_uri(track.audio_file_path) ||
                      path_is_remote_session_uri(track.audio_file_path);
    return entry;
}

bool PlaylistSessionController::track_restorable(const PlaylistSessionTrack& track) {
    if (track.audio_file_path.empty()) {
        return false;
    }
    if (track.is_stream || path_is_remote_session_uri(track.audio_file_path) ||
        StreamAudioDecoder::is_stream_uri(track.audio_file_path)) {
        return true;
    }
    return access(track.audio_file_path.c_str(), F_OK) == 0;
}

bool PlaylistSessionController::load_restore_result(PlaylistSessionRestoreResult& out) {
    PlaylistSessionSnapshot snapshot;
    if (!PlaylistSession().load(snapshot)) {
        return false;
    }

    out.entries.clear();
    out.current_index = 0;
    out.entries.reserve(snapshot.tracks.size());
    for (const PlaylistSessionTrack& track : snapshot.tracks) {
        if (!track_restorable(track)) {
            continue;
        }
        out.entries.push_back(entry_from_track(track));
    }
    if (out.entries.empty()) {
        return false;
    }

    if (!snapshot.tracks.empty()) {
        const std::size_t saved_index = std::min(snapshot.current_track_index, snapshot.tracks.size() - 1);
        const PlaylistSessionTrack& target = snapshot.tracks[saved_index];
        bool found = false;
        for (std::size_t i = 0; i < out.entries.size(); ++i) {
            const PlaylistSessionEntryData& entry = out.entries[i];
            if (entry.audio_file_path == target.audio_file_path && entry.start_sample == target.start_sample &&
                entry.cue_track == target.cue_track && entry.track_number == target.track_number) {
                out.current_index = i;
                found = true;
                break;
            }
        }
        if (!found) {
            out.current_index = std::min(snapshot.current_track_index, out.entries.size() - 1);
        }
    }

    return true;
}

} // namespace pcmtp
