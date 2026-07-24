// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/patches/PlaylistSelectionPatches.hpp"

#include "pcmtp/gui/GtkPlayerWindow.hpp"
#include "pcmtp/patches/GtkPlayerMediaKeys.hpp"

namespace pcmtp::patches {

namespace {

bool playlist_index_from_path(GtkTreeModel* model,
                              GtkTreePath* path,
                              int index_column,
                              std::size_t* out_index) {
    if (model == nullptr || path == nullptr || out_index == nullptr) {
        return false;
    }

    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) {
        return false;
    }

    return playlist_index_from_model_iter(model, &iter, index_column, out_index);
}

} // namespace

void update_current_track_from_playlist_ui(GtkPlayerWindow& window, int index_column) {
    if (window.playlist_.empty() || window.playlist_view_ == nullptr) {
        return;
    }

    GtkTreeView* view = GTK_TREE_VIEW(window.playlist_view_);
    GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        int row_index = -1;
        gtk_tree_model_get(model, &iter, index_column, &row_index, -1);
        if (row_index >= 0 && static_cast<std::size_t>(row_index) < window.playlist_.size()) {
            window.current_track_index_ = static_cast<std::size_t>(row_index);
            if (window.pending_metadata_playback_valid()) {
                window.set_pending_metadata_playback(window.current_track_index_,
                                                     window.pending_metadata_playback_.offset_samples,
                                                     window.pending_metadata_playback_.start_playback,
                                                     window.pending_metadata_playback_.preserve_paused,
                                                     window.pending_metadata_playback_.update_mpris_track);
            }
            return;
        }
    }

    GtkTreePath* cursor_path = nullptr;
    GtkTreeViewColumn* cursor_column = nullptr;
    gtk_tree_view_get_cursor(view, &cursor_path, &cursor_column);
    if (cursor_path != nullptr) {
        GtkTreeModel* cursor_model = gtk_tree_view_get_model(view);
        GtkTreeIter cursor_iter;
        if (cursor_model != nullptr && gtk_tree_model_get_iter(cursor_model, &cursor_iter, cursor_path)) {
            int row_index = -1;
            gtk_tree_model_get(cursor_model, &cursor_iter, index_column, &row_index, -1);
            if (row_index >= 0 && static_cast<std::size_t>(row_index) < window.playlist_.size()) {
                window.current_track_index_ = static_cast<std::size_t>(row_index);
                if (window.pending_metadata_playback_valid()) {
                    window.set_pending_metadata_playback(window.current_track_index_,
                                                         window.pending_metadata_playback_.offset_samples,
                                                         window.pending_metadata_playback_.start_playback,
                                                         window.pending_metadata_playback_.preserve_paused,
                                                         window.pending_metadata_playback_.update_mpris_track);
                }
            }
        }
        gtk_tree_path_free(cursor_path);
    }
}

bool playlist_index_from_model_iter(GtkTreeModel* model,
                                    GtkTreeIter* iter,
                                    int index_column,
                                    std::size_t* out_index) {
    if (model == nullptr || iter == nullptr || out_index == nullptr) {
        return false;
    }

    int row_index = -1;
    gtk_tree_model_get(model, iter, index_column, &row_index, -1);
    if (row_index < 0) {
        return false;
    }
    *out_index = static_cast<std::size_t>(row_index);
    return true;
}

void on_playlist_selection_changed(GtkTreeSelection* selection, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || self->ui_closing_) {
        return;
    }
    self->update_playlist_selection_from_ui();
    (void)selection;
}

gboolean on_playlist_focus_in(GtkWidget*, GdkEventFocus*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || self->ui_closing_) {
        return FALSE;
    }
    self->sync_playlist_cursor_to_selection();
    return FALSE;
}

gboolean on_playlist_view_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || event == nullptr) {
        return FALSE;
    }
    return handle_media_key(self, event->keyval);
}

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

    GtkTreePath* child_path = gtk_tree_path_new_from_indices(static_cast<int>(index), -1);
    if (child_path == nullptr) {
        return false;
    }

    if (GTK_IS_TREE_MODEL_FILTER(model)) {
        GtkTreeModelFilter* filter = GTK_TREE_MODEL_FILTER(model);
        GtkTreePath* filter_path = gtk_tree_model_filter_convert_child_path_to_path(filter, child_path);
        gtk_tree_path_free(child_path);
        if (filter_path == nullptr) {
            return false;
        }
        *out_path = filter_path;
        return true;
    }

    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, child_path)) {
        gtk_tree_path_free(child_path);
        return false;
    }

    int row_index = -1;
    gtk_tree_model_get(model, &iter, index_column, &row_index, -1);
    if (row_index < 0 || static_cast<std::size_t>(row_index) != index) {
        gtk_tree_path_free(child_path);
        return false;
    }

    *out_path = child_path;
    return true;
}

bool playlist_index_from_view_path(GtkTreeView* playlist_view,
                                   GtkTreePath* path,
                                   int index_column,
                                   std::size_t* out_index) {
    if (playlist_view == nullptr) {
        return false;
    }
    GtkTreeModel* model = gtk_tree_view_get_model(playlist_view);
    return playlist_index_from_path(model, path, index_column, out_index);
}

} // namespace pcmtp::patches
