#pragma once

#include <gtk/gtk.h>

#include <string>

namespace pcmtp {

class GtkPlayerWindow;

namespace patches {

void setup_media_keys(GtkApplication* app, GtkPlayerWindow* window);

void handle_media_play(GtkPlayerWindow* window);
void handle_media_pause(GtkPlayerWindow* window);
void handle_media_stop(GtkPlayerWindow* window);
void handle_media_next(GtkPlayerWindow* window);
void handle_media_previous(GtkPlayerWindow* window);
void handle_media_play_pause(GtkPlayerWindow* window);
bool handle_media_key(GtkPlayerWindow* window, guint keyval);

void on_media_play(GSimpleAction* action, GVariant* parameter, gpointer user_data);
void on_media_pause(GSimpleAction* action, GVariant* parameter, gpointer user_data);
void on_media_play_pause(GSimpleAction* action, GVariant* parameter, gpointer user_data);
void on_media_stop(GSimpleAction* action, GVariant* parameter, gpointer user_data);
void on_media_next(GSimpleAction* action, GVariant* parameter, gpointer user_data);
void on_media_previous(GSimpleAction* action, GVariant* parameter, gpointer user_data);
gboolean on_window_key_press(GtkWidget* widget, GdkEvent* event, gpointer user_data);

} // namespace patches
} // namespace pcmtp
