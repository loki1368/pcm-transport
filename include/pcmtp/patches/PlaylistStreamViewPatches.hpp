#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <string>

namespace pcmtp {

class StreamPlaybackManager;

namespace patches {

constexpr int playlist_stream_broken_column() { return 5; }

struct PlaylistStreamRowValues {
    std::string track_number;
    gboolean stream_broken = FALSE;
};

PlaylistStreamRowValues playlist_stream_row_values(const StreamPlaybackManager& manager,
                                                   bool is_stream,
                                                   const std::string& audio_file_path,
                                                   int track_number);

bool find_playlist_view_path_for_index(GtkTreeView* playlist_view,
                                       std::size_t index,
                                       int index_column,
                                       GtkTreePath** out_path);

void install_playlist_stream_styling(GtkTreeView* view,
                                     GtkTreeViewColumn* col_track,
                                     GtkTreeViewColumn* col_artist,
                                     GtkTreeViewColumn* col_title,
                                     GtkTreeViewColumn* col_source,
                                     int col_trackno_id,
                                     int col_artist_id,
                                     int col_title_id,
                                     int col_source_id);

} // namespace patches
} // namespace pcmtp
