#include "pcmtp/patches/PlaylistStreamViewPatches.hpp"

#include <gtk/gtk.h>

#include <pango/pango.h>

#include "pcmtp/patches/StreamPlaybackManager.hpp"

namespace pcmtp::patches {
namespace {

void on_playlist_row_cell_data(GtkTreeViewColumn* column,
                               GtkCellRenderer* cell,
                               GtkTreeModel* model,
                               GtkTreeIter* iter,
                               gpointer user_data) {
    GtkTreeView* view = GTK_TREE_VIEW(gtk_tree_view_column_get_tree_view(column));
    GtkTreePath* path = gtk_tree_model_get_path(model, iter);
    const int model_column = GPOINTER_TO_INT(user_data);
    gboolean broken = FALSE;
    gtk_tree_model_get(model, iter, playlist_stream_broken_column(), &broken, -1);
    GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
    gboolean selected = gtk_tree_selection_path_is_selected(selection, path);
    gtk_tree_path_free(path);
    const char* fg = broken ? "#c44" : (selected ? "#ffffff" : nullptr);
    g_object_set(cell, "weight", PANGO_WEIGHT_NORMAL, "weight-set", FALSE, nullptr);
    if (fg != nullptr) {
        g_object_set(cell, "foreground", fg, "foreground-set", TRUE, nullptr);
    } else {
        g_object_set(cell, "foreground-set", FALSE, nullptr);
    }
    if (selected && !broken) {
        g_object_set(cell, "weight", PANGO_WEIGHT_BOLD, "weight-set", TRUE, nullptr);
    }
    (void)model_column;
}

void on_playlist_selection_changed(GtkTreeSelection* selection, gpointer) {
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gtk_widget_queue_draw(GTK_WIDGET(gtk_tree_selection_get_tree_view(selection)));
    }
}

void set_playlist_column_cell_styler(GtkTreeViewColumn* column, int model_column) {
    GtkCellRenderer* renderer = nullptr;
    GList* cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(column));
    if (cells != nullptr) {
        renderer = GTK_CELL_RENDERER(cells->data);
    }
    g_list_free(cells);
    if (renderer != nullptr) {
        gtk_tree_view_column_set_cell_data_func(column,
                                                renderer,
                                                on_playlist_row_cell_data,
                                                GINT_TO_POINTER(model_column),
                                                nullptr);
    }
}

} // namespace

bool find_playlist_view_path_for_index(GtkTreeView* playlist_view,
                                       std::size_t index,
                                       int index_column,
                                       GtkTreePath** out_path) {
    if (out_path == nullptr || playlist_view == nullptr) {
        return false;
    }
    *out_path = nullptr;
    GtkTreeModel* model = gtk_tree_view_get_model(playlist_view);
    if (model == nullptr) {
        return false;
    }
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    while (valid) {
        int row_index = -1;
        gtk_tree_model_get(model, &iter, index_column, &row_index, -1);
        if (row_index >= 0 && static_cast<std::size_t>(row_index) == index) {
            *out_path = gtk_tree_model_get_path(model, &iter);
            return true;
        }
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return false;
}

void install_playlist_stream_styling(GtkTreeView* view,
                                     GtkTreeViewColumn* col_track,
                                     GtkTreeViewColumn* col_artist,
                                     GtkTreeViewColumn* col_title,
                                     GtkTreeViewColumn* col_source,
                                     int col_trackno_id,
                                     int col_artist_id,
                                     int col_title_id,
                                     int col_source_id) {
    set_playlist_column_cell_styler(col_track, col_trackno_id);
    set_playlist_column_cell_styler(col_artist, col_artist_id);
    set_playlist_column_cell_styler(col_title, col_title_id);
    set_playlist_column_cell_styler(col_source, col_source_id);

    GtkTreeSelection* playlist_selection = gtk_tree_view_get_selection(view);
    g_signal_connect(playlist_selection, "changed", G_CALLBACK(on_playlist_selection_changed), nullptr);
}

PlaylistStreamRowValues playlist_stream_row_values(const StreamPlaybackManager& manager,
                                                   bool is_stream,
                                                   const std::string& audio_file_path,
                                                   int track_number) {
    PlaylistStreamRowValues values;
    const bool stream_broken = is_stream && manager.is_broken(audio_file_path);
    values.stream_broken = stream_broken ? TRUE : FALSE;
    values.track_number = stream_broken
        ? ("× " + std::to_string(track_number))
        : std::to_string(track_number);
    return values;
}

} // namespace pcmtp::patches
