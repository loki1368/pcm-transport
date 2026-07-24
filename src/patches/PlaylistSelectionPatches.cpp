#include "pcmtp/patches/PlaylistSelectionPatches.hpp"

#include "pcmtp/gui/GtkPlayerWindow.hpp"
#include "pcmtp/patches/PlaylistSessionController.hpp"

namespace pcmtp::patches {

namespace {

enum PlaylistColumns {
    COL_INDEX = 0,
};

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

gboolean on_playlist_focus_in(GtkWidget*, GdkEventFocus*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || self->ui_closing_) {
        return FALSE;
    }
    self->sync_playlist_cursor_to_selection();
    return FALSE;
}

void save_playlist_session(GtkPlayerWindow& window) {
    std::vector<PlaylistSessionEntryData> entries;
    entries.reserve(window.playlist_.size());
    for (const GtkPlayerWindow::PlaylistEntry& entry : window.playlist_) {
        entries.push_back(GtkPlayerWindow::session_entry_data_from(entry));
    }
    const std::size_t current_index = window.playlist_.empty() ? 0 : window.highlighted_playlist_index();
    window.session_controller_->save(entries, current_index);
}

bool restore_playlist_session(GtkPlayerWindow& window) {
    return window.session_controller_->restore();
}

} // namespace pcmtp::patches
