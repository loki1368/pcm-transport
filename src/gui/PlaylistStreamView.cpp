#include "pcmtp/gui/PlaylistStreamView.hpp"

#include <gtk/gtk.h>
#include <pango/pango.h>

#include <string>

#include "pcmtp/stream/StreamPlaybackManager.hpp"

namespace pcmtp {
namespace {

constexpr const char* kPlaylistPlayingIndexKey = "pcmtp-playing-index";

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

bool playlist_row_is_playing_impl(GtkTreeView* view, GtkTreeModel* model, GtkTreeIter* iter) {
    if (view == nullptr || model == nullptr || iter == nullptr) {
        return false;
    }
    const auto* playing_index = static_cast<const std::size_t*>(
        g_object_get_data(G_OBJECT(view), kPlaylistPlayingIndexKey));
    if (playing_index == nullptr) {
        return false;
    }
    int row_index = -1;
    gtk_tree_model_get(model, iter, 0, &row_index, -1);
    return row_index >= 0 && static_cast<std::size_t>(row_index) == *playing_index;
}

void on_playlist_row_cell_data(GtkTreeViewColumn* column,
                               GtkCellRenderer* cell,
                               GtkTreeModel* model,
                               GtkTreeIter* iter,
                               gpointer user_data) {
    GtkTreeView* view = GTK_TREE_VIEW(gtk_tree_view_column_get_tree_view(column));
    GtkTreePath* path = gtk_tree_model_get_path(model, iter);
    const int model_column = GPOINTER_TO_INT(user_data);
    const bool playing = playlist_row_is_playing_impl(view, model, iter);

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
        std::string span_attrs = std::string("foreground='") + broken_color + "'";
        if (playing) {
            span_attrs += " weight='bold'";
        }
        const std::string markup =
            "<span " + span_attrs + ">" + escape_pango_markup_text(cell_text) + "</span>";
        g_object_set(G_OBJECT(cell),
                       "markup", markup.c_str(),
                       "ellipsize", PANGO_ELLIPSIZE_END,
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
                       "ellipsize", PANGO_ELLIPSIZE_END,
                       "foreground-rgba", &normal_selected_fg,
                       "foreground-set", TRUE,
                       "cell-background-rgba", &normal_selected_bg,
                       "cell-background-set", TRUE,
                       "weight", playing ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                       "weight-set", TRUE,
                       nullptr);
    } else {
        g_object_set(G_OBJECT(cell),
                       "markup", nullptr,
                       "text", cell_text.c_str(),
                       "ellipsize", PANGO_ELLIPSIZE_END,
                       "foreground-set", FALSE,
                       "cell-background-set", FALSE,
                       "weight", playing ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                       "weight-set", TRUE,
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
    if (column == nullptr || model_column < 0) {
        return;
    }
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

bool playlist_row_is_playing(GtkTreeView* view, GtkTreeModel* model, GtkTreeIter* iter) {
    return playlist_row_is_playing_impl(view, model, iter);
}

void refresh_playlist_row_styles(GtkWidget* playlist_view) {
    if (playlist_view != nullptr) {
        gtk_widget_queue_draw(playlist_view);
    }
}

void install_playlist_stream_styling(GtkTreeView* view,
                                     GtkTreeViewColumn* col_track,
                                     GtkTreeViewColumn* col_artist,
                                     GtkTreeViewColumn* col_title,
                                     GtkTreeViewColumn* col_album,
                                     GtkTreeViewColumn* col_source,
                                     int col_trackno_id,
                                     int col_artist_id,
                                     int col_title_id,
                                     int col_album_id,
                                     int col_source_id,
                                     const std::size_t* playing_index) {
    if (view != nullptr && playing_index != nullptr) {
        g_object_set_data(G_OBJECT(view),
                          kPlaylistPlayingIndexKey,
                          const_cast<std::size_t*>(playing_index));
    }

    set_playlist_column_cell_styler(col_track, col_trackno_id);
    set_playlist_column_cell_styler(col_artist, col_artist_id);
    set_playlist_column_cell_styler(col_title, col_title_id);
    set_playlist_column_cell_styler(col_album, col_album_id);
    set_playlist_column_cell_styler(col_source, col_source_id);

    if (view != nullptr &&
        g_object_get_data(G_OBJECT(view), "pcmtp-stream-selection-styled") == nullptr) {
        GtkTreeSelection* playlist_selection = gtk_tree_view_get_selection(view);
        g_signal_connect(playlist_selection, "changed", G_CALLBACK(on_playlist_selection_changed), nullptr);
        g_object_set_data(G_OBJECT(view), "pcmtp-stream-selection-styled", GINT_TO_POINTER(1));
    }
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

} // namespace pcmtp
