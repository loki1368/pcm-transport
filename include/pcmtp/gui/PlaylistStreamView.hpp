#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <string>

namespace pcmtp {

class StreamPlaybackManager;

// After INDEX, TRACKNO, ARTIST, ALBUM, TITLE, SOURCE, SEARCH_FOLDED.
constexpr int playlist_stream_broken_column() { return 7; }

struct PlaylistStreamRowValues {
    std::string track_number;
    gboolean stream_broken = FALSE;
};

PlaylistStreamRowValues playlist_stream_row_values(const StreamPlaybackManager& manager,
                                                   bool is_stream,
                                                   const std::string& audio_file_path,
                                                   int track_number);

void install_playlist_stream_styling(GtkTreeView* view,
                                     GtkTreeViewColumn* col_track,
                                     GtkTreeViewColumn* col_artist,
                                     GtkTreeViewColumn* col_title,
                                     GtkTreeViewColumn* col_source,
                                     int col_trackno_id,
                                     int col_artist_id,
                                     int col_title_id,
                                     int col_source_id);

} // namespace pcmtp
