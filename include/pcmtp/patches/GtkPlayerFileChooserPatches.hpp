#pragma once

#include <gtk/gtk.h>

#include <string>
#include <vector>

namespace pcmtp {

class GtkPlayerWindow;

namespace patches {

bool is_supported_media_path(const std::string& path);

gboolean file_chooser_open_filter(const GtkFileFilterInfo* filter_info, gpointer user_data);

std::vector<std::string> collect_file_chooser_paths(GtkFileChooser* chooser);

void prefer_cue_over_audio_files(std::vector<std::string>* paths);

void collect_audio_files_recursive(const std::string& directory, std::vector<std::string>* out);

void run_open_audio_dialog(GtkPlayerWindow& window);

} // namespace patches
} // namespace pcmtp
