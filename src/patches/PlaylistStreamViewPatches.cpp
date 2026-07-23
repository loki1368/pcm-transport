#include "pcmtp/patches/PlaylistStreamViewPatches.hpp"

#include <gtk/gtk.h>

#include <string>

#include "pcmtp/patches/StreamPlaybackManager.hpp"

namespace pcmtp::patches {
namespace {

std::string escape_pango_markup_text(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

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

    gboolean selected = FALSE;
    if (view != nullptr && path != nullptr) {
        GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
        selected = gtk_tree_selection_path_is_selected(selection, path);
    }

    const GdkRGBA normal_selected_bg = {0.435f, 0.467f, 0.502f, 1.0f};
    const GdkRGBA normal_selected_fg = {1.0f, 1.0f, 1.0f, 1.0f};
    const char* broken_color = selected ? "#ff9a9a" : "#c44";

    gchar* text = nullptr;
    if (model_column >= 0) {
        gtk_tree_model_get(model, iter, model_column, &text, -1);
    }
    const std::string cell_text = text != nullptr ? text : std::string();

    if (broken) {
        const std::string markup = std::string("<span foreground='") + broken_color + "'>" +
                                   escape_pango_markup_text(cell_text) + "</span>";
        g_object_set(G_OBJECT(cell),
                       "markup", markup.c_str(),
                       "foreground-set", FALSE,
                       "weight-set", FALSE,
                       "cell-background-set", selected ? TRUE : FALSE,
                       nullptr);
        if (selected) {
            g_object_set(G_OBJECT(cell), "cell-background-rgba", &normal_selected_bg, nullptr);
        }
    } else if (selected) {
        g_object_set(G_OBJECT(cell),
                       "markup", nullptr,
                       "text", cell_text.c_str(),
                       "foreground-rgba", &normal_selected_fg,
                       "foreground-set", TRUE,
                       "cell-background-rgba", &normal_selected_bg,
                       "cell-background-set", TRUE,
                       "weight-set", FALSE,
                       nullptr);
    } else {
        g_object_set(G_OBJECT(cell),
                       "markup", nullptr,
                       "text", cell_text.c_str(),
                       "foreground-set", FALSE,
                       "cell-background-set", FALSE,
                       "weight-set", FALSE,
                       nullptr);
    }

    g_free(text);
    if (path != nullptr) {
        gtk_tree_path_free(path);
    }
}

void on_playlist_selection_changed(GtkTreeSelection* selection, gpointer /*user_data*/) {
    GtkTreeView* view = gtk_tree_selection_get_tree_view(selection);
    if (view != nullptr) {
        gtk_widget_queue_draw(GTK_WIDGET(view));
    }
}

void set_playlist_column_cell_styler(GtkTreeViewColumn* column, int model_column) {
    GList* renderers = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(column));
    for (GList* node = renderers; node != nullptr; node = node->next) {
        gtk_tree_view_column_set_cell_data_func(column,
                                                GTK_CELL_RENDERER(node->data),
                                                on_playlist_row_cell_data,
                                                GINT_TO_POINTER(model_column),
                                                nullptr);
    }
    g_list_free(renderers);
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
