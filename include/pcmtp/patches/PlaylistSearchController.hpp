#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <string>

namespace pcmtp {

// PATCH: playlist search/filter/typeahead UI and keyboard handling.
class PlaylistSearchController {
public:
    class Delegate {
    public:
        virtual ~Delegate() = default;

        virtual GtkWidget* window() = 0;
        virtual GtkListStore* playlist_store() = 0;
        virtual GtkWidget* playlist_view() = 0;
        virtual GtkWidget* playlist_scrolled() = 0;
        virtual int col_artist() const = 0;
        virtual int col_title() const = 0;
        virtual bool ui_closing() const = 0;
        virtual void select_playlist_row(std::size_t index) = 0;
        virtual void select_and_scroll_playlist_path(GtkTreePath* path, bool center_vertically) = 0;
    };

    explicit PlaylistSearchController(Delegate& delegate);

    void install_in_panel(GtkBox* playlist_panel);
    GtkTreeModelFilter* filter_model() const { return filter_; }

    void clear_search();
    void refilter();
    gboolean on_key_press(GtkWidget* widget, GdkEventKey* event);
    void invalidate_ui();

private:
    static void on_search_changed(GtkEditable* editable, gpointer user_data);
    static gboolean on_filter_visible(GtkTreeModel* model, GtkTreeIter* iter, gpointer user_data);
    static gboolean on_typeahead_clear_timeout(gpointer user_data);

    void reset_typeahead();
    void apply_typeahead_selection();
    void update_typeahead_popup();

    Delegate& delegate_;
    GtkTreeModelFilter* filter_ = nullptr;
    GtkWidget* search_entry_ = nullptr;
    GtkWidget* typeahead_popup_ = nullptr;
    GtkWidget* typeahead_entry_ = nullptr;
    std::string filter_text_;
    std::string typeahead_text_;
    guint typeahead_timeout_id_ = 0;
};

} // namespace pcmtp
