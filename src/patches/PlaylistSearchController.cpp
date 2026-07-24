#include "pcmtp/patches/PlaylistSearchController.hpp"

#include <gtk/gtk.h>

namespace pcmtp {
namespace {

std::string utf8_casefold_copy(const std::string& text) {
    gchar* folded = g_utf8_casefold(text.c_str(), -1);
    if (folded == nullptr) {
        return text;
    }
    std::string result(folded);
    g_free(folded);
    return result;
}

bool utf8_contains_casefold(const std::string& haystack, const std::string& needle_folded) {
    if (needle_folded.empty()) {
        return true;
    }
    return utf8_casefold_copy(haystack).find(needle_folded) != std::string::npos;
}

bool utf8_starts_with_casefold(const std::string& haystack, const std::string& prefix_folded) {
    if (prefix_folded.empty()) {
        return true;
    }
    const std::string hay_folded = utf8_casefold_copy(haystack);
    return hay_folded.size() >= prefix_folded.size() &&
           hay_folded.compare(0, prefix_folded.size(), prefix_folded) == 0;
}

bool playlist_row_matches_typeahead(const char* artist, const char* title, const std::string& key_folded) {
    const std::string artist_text = artist != nullptr ? artist : "";
    const std::string title_text = title != nullptr ? title : "";
    return utf8_starts_with_casefold(artist_text, key_folded) ||
           utf8_starts_with_casefold(title_text, key_folded) ||
           utf8_contains_casefold(artist_text, key_folded) ||
           utf8_contains_casefold(title_text, key_folded);
}

} // namespace

PlaylistSearchController::PlaylistSearchController(Delegate& delegate) : delegate_(delegate) {}

void PlaylistSearchController::install_in_panel(GtkBox* playlist_panel) {
    if (playlist_panel == nullptr) {
        return;
    }

    search_entry_ = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry_), "Search title or artist");
    gtk_widget_set_margin_bottom(search_entry_, 2);
    gtk_box_pack_start(playlist_panel, search_entry_, FALSE, FALSE, 0);

    GtkListStore* store = delegate_.playlist_store();
    if (store != nullptr) {
        filter_ = GTK_TREE_MODEL_FILTER(gtk_tree_model_filter_new(GTK_TREE_MODEL(store), nullptr));
        gtk_tree_model_filter_set_visible_func(filter_,
                                               PlaylistSearchController::on_filter_visible,
                                               this,
                                               nullptr);
    }

    g_signal_connect(search_entry_, "changed", G_CALLBACK(PlaylistSearchController::on_search_changed), this);

    GtkWidget* window = delegate_.window();
    typeahead_popup_ = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_decorated(GTK_WINDOW(typeahead_popup_), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(typeahead_popup_), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(typeahead_popup_), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(typeahead_popup_), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(typeahead_popup_), GDK_WINDOW_TYPE_HINT_TOOLTIP);
    if (window != nullptr) {
        gtk_window_set_transient_for(GTK_WINDOW(typeahead_popup_), GTK_WINDOW(window));
    }
    gtk_container_set_border_width(GTK_CONTAINER(typeahead_popup_), 0);

    typeahead_entry_ = gtk_entry_new();
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(typeahead_entry_),
                                      GTK_ENTRY_ICON_PRIMARY,
                                      "edit-find-symbolic");
    gtk_editable_set_editable(GTK_EDITABLE(typeahead_entry_), FALSE);
    gtk_widget_set_can_focus(typeahead_entry_, FALSE);
    gtk_container_add(GTK_CONTAINER(typeahead_popup_), typeahead_entry_);
}

void PlaylistSearchController::clear_search() {
    reset_typeahead();
    filter_text_.clear();
    if (search_entry_ != nullptr) {
        gtk_entry_set_text(GTK_ENTRY(search_entry_), "");
    }
    refilter();
}

void PlaylistSearchController::refilter() {
    if (filter_ != nullptr) {
        gtk_tree_model_filter_refilter(filter_);
    }
}

gboolean PlaylistSearchController::on_key_press(GtkWidget* widget, GdkEventKey* event) {
    if (event == nullptr || delegate_.ui_closing()) {
        return FALSE;
    }
    if (search_entry_ != nullptr && gtk_widget_is_focus(search_entry_)) {
        return FALSE;
    }

    if (event->keyval == GDK_KEY_Escape) {
        if (!typeahead_text_.empty()) {
            reset_typeahead();
            return TRUE;
        }
        return FALSE;
    }

    if (event->keyval == GDK_KEY_BackSpace) {
        if (typeahead_text_.empty()) {
            return FALSE;
        }
        const char* text = typeahead_text_.c_str();
        const char* prev = g_utf8_find_prev_char(text, text + typeahead_text_.size());
        if (prev != nullptr) {
            typeahead_text_.resize(static_cast<std::size_t>(prev - text));
        } else {
            typeahead_text_.clear();
        }
        if (typeahead_timeout_id_ != 0) {
            g_source_remove(typeahead_timeout_id_);
        }
        typeahead_timeout_id_ = g_timeout_add(1500, PlaylistSearchController::on_typeahead_clear_timeout, this);
        update_typeahead_popup();
        apply_typeahead_selection();
        return TRUE;
    }

    if ((event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK)) != 0) {
        return FALSE;
    }

    guint32 unicode = gdk_keyval_to_unicode(event->keyval);
    if (unicode == 0 || !g_unichar_isprint(unicode)) {
        return FALSE;
    }

    char buffer[8];
    const gint written = g_unichar_to_utf8(unicode, buffer);
    if (written <= 0) {
        return FALSE;
    }
    typeahead_text_.append(buffer, static_cast<std::size_t>(written));
    if (typeahead_timeout_id_ != 0) {
        g_source_remove(typeahead_timeout_id_);
    }
    typeahead_timeout_id_ = g_timeout_add(1500, PlaylistSearchController::on_typeahead_clear_timeout, this);
    update_typeahead_popup();
    apply_typeahead_selection();
    (void)widget;
    return TRUE;
}

void PlaylistSearchController::invalidate_ui() {
    if (typeahead_timeout_id_ != 0) {
        g_source_remove(typeahead_timeout_id_);
        typeahead_timeout_id_ = 0;
    }
    filter_ = nullptr;
    search_entry_ = nullptr;
    typeahead_popup_ = nullptr;
    typeahead_entry_ = nullptr;
}

void PlaylistSearchController::on_search_changed(GtkEditable* editable, gpointer user_data) {
    auto* self = static_cast<PlaylistSearchController*>(user_data);
    if (self == nullptr) {
        return;
    }
    const gchar* text = gtk_entry_get_text(GTK_ENTRY(editable));
    self->filter_text_ = text != nullptr ? utf8_casefold_copy(text) : std::string();
    self->refilter();
}

gboolean PlaylistSearchController::on_filter_visible(GtkTreeModel* model, GtkTreeIter* iter, gpointer user_data) {
    auto* self = static_cast<PlaylistSearchController*>(user_data);
    if (self == nullptr || self->filter_text_.empty()) {
        return TRUE;
    }
    gchar* artist = nullptr;
    gchar* title = nullptr;
    gtk_tree_model_get(model,
                       iter,
                       self->delegate_.col_artist(), &artist,
                       self->delegate_.col_title(), &title,
                       -1);
    const bool match = utf8_contains_casefold(artist != nullptr ? artist : "", self->filter_text_) ||
                       utf8_contains_casefold(title != nullptr ? title : "", self->filter_text_);
    g_free(artist);
    g_free(title);
    return match ? TRUE : FALSE;
}

void PlaylistSearchController::update_typeahead_popup() {
    GtkWidget* scrolled = delegate_.playlist_scrolled();
    if (typeahead_popup_ == nullptr || typeahead_entry_ == nullptr || scrolled == nullptr) {
        return;
    }
    if (typeahead_text_.empty()) {
        gtk_widget_hide(typeahead_popup_);
        return;
    }

    gtk_entry_set_text(GTK_ENTRY(typeahead_entry_), typeahead_text_.c_str());
    gtk_widget_show_all(typeahead_popup_);

    if (!gtk_widget_get_realized(scrolled)) {
        return;
    }

    GtkAllocation allocation{};
    gtk_widget_get_allocation(scrolled, &allocation);

    gint anchor_x = 0;
    gint anchor_y = 0;
    GdkWindow* anchor_window = gtk_widget_get_window(scrolled);
    if (anchor_window == nullptr) {
        return;
    }
    gdk_window_get_origin(anchor_window, &anchor_x, &anchor_y);

    GdkRectangle anchor_rect{};
    anchor_rect.x = anchor_x;
    anchor_rect.y = anchor_y;
    anchor_rect.width = allocation.width;
    anchor_rect.height = allocation.height;

    if (!gtk_widget_get_realized(typeahead_popup_)) {
        gtk_widget_realize(typeahead_popup_);
    }
    GdkWindow* popup_window = gtk_widget_get_window(typeahead_popup_);
    if (popup_window == nullptr) {
        return;
    }

    gdk_window_move_to_rect(popup_window,
                            &anchor_rect,
                            GDK_GRAVITY_SOUTH_EAST,
                            GDK_GRAVITY_SOUTH_EAST,
                            static_cast<GdkAnchorHints>(GDK_ANCHOR_SLIDE | GDK_ANCHOR_FLIP_X | GDK_ANCHOR_FLIP_Y),
                            -4,
                            -4);
}

void PlaylistSearchController::reset_typeahead() {
    typeahead_text_.clear();
    if (typeahead_timeout_id_ != 0) {
        g_source_remove(typeahead_timeout_id_);
        typeahead_timeout_id_ = 0;
    }
    update_typeahead_popup();
}

gboolean PlaylistSearchController::on_typeahead_clear_timeout(gpointer user_data) {
    auto* self = static_cast<PlaylistSearchController*>(user_data);
    if (self == nullptr) {
        return G_SOURCE_REMOVE;
    }
    self->typeahead_timeout_id_ = 0;
    self->typeahead_text_.clear();
    self->update_typeahead_popup();
    return G_SOURCE_REMOVE;
}

void PlaylistSearchController::apply_typeahead_selection() {
    if (typeahead_text_.empty()) {
        return;
    }
    GtkWidget* view_widget = delegate_.playlist_view();
    if (view_widget == nullptr) {
        return;
    }

    const std::string key_folded = utf8_casefold_copy(typeahead_text_);
    GtkTreeModel* model = gtk_tree_view_get_model(GTK_TREE_VIEW(view_widget));
    if (model == nullptr) {
        return;
    }

    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_first(model, &iter)) {
        return;
    }

    const int col_artist = delegate_.col_artist();
    const int col_title = delegate_.col_title();
    do {
        gchar* artist = nullptr;
        gchar* title = nullptr;
        gtk_tree_model_get(model, &iter, col_artist, &artist, col_title, &title, -1);
        const bool match = playlist_row_matches_typeahead(artist, title, key_folded);
        g_free(artist);
        g_free(title);
        if (!match) {
            continue;
        }

        GtkTreePath* path = gtk_tree_model_get_path(model, &iter);
        delegate_.select_and_scroll_playlist_path(path, true);
        gtk_tree_path_free(path);
        return;
    } while (gtk_tree_model_iter_next(model, &iter));
}

} // namespace pcmtp
