#pragma once

#include <gtk/gtk.h>

namespace pcmtp {

class GtkPlayerWindow;

namespace patches {

void update_current_track_from_playlist_ui(GtkPlayerWindow& window, int index_column);
gboolean on_playlist_focus_in(GtkWidget* widget, GdkEventFocus* event, gpointer user_data);
gboolean on_playlist_view_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data);
bool find_playlist_view_path_for_index(GtkTreeView* playlist_view,
                                       std::size_t index,
                                       int index_column,
                                       GtkTreePath** out_path);

} // namespace patches
} // namespace pcmtp
