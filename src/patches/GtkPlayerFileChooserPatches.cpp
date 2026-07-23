#include "pcmtp/patches/GtkPlayerFileChooserPatches.hpp"

#include <gtk/gtk.h>

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unordered_set>
#include <vector>

#include "pcmtp/cue/CueParser.hpp"
#include "pcmtp/gui/GtkPlayerWindow.hpp"
#include "pcmtp/patches/PlaylistSelectionPatches.hpp"
#include "pcmtp/patches/StreamAudioDecoder.hpp"
#include "pcmtp/playlist/M3uPlaylistReader.hpp"

namespace pcmtp::patches {
namespace {

bool is_audio_file_path(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) {
        return false;
    }
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* kAudioExtensions[] = {
        ".flac", ".mp3", ".wav", ".wave", ".bwf", ".aiff", ".aif", ".au", ".snd", ".caf", ".ape", ".wv", ".tak",
        ".tta", ".dsf", ".m4a", ".aac", ".ogg", ".oga", ".opus", ".wma", ".asf", ".xwma", ".oma", ".aa3", ".at3",
        ".mpc", ".mp+", ".mpp",
    };
    for (const char* supported : kAudioExtensions) {
        if (ext == supported) {
            return true;
        }
    }
    return false;
}

std::string join_directory_entry(const std::string& directory, const char* name) {
    if (directory.empty()) {
        return name;
    }
    if (directory.back() == '/') {
        return directory + name;
    }
    return directory + "/" + name;
}

std::string media_path_key(const std::string& path) {
    std::string key = path;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return key;
}

std::string parent_directory(const std::string& path) {
    gchar* dir = g_path_get_dirname(path.c_str());
    std::string result = dir != nullptr ? dir : "";
    g_free(dir);
    return result;
}

} // namespace

bool is_supported_media_path(const std::string& path) {
    if (StreamAudioDecoder::is_stream_uri(path)) {
        return true;
    }
    if (M3uPlaylistReader::looks_like_playlist_path(path) || CueParser::looks_like_cue_path(path)) {
        return true;
    }

    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) {
        return false;
    }

    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static const char* kSupportedExtensions[] = {
        ".flac", ".mp3", ".mp2", ".wav", ".wave", ".w64", ".bwf", ".aiff", ".aif", ".au", ".snd", ".caf", ".voc", ".ra",
        ".ape", ".wv", ".tak", ".tta", ".dsf", ".dff", ".m4a", ".m4r", ".aac", ".ac3", ".dts", ".ogg", ".oga", ".opus", ".spx",
        ".wma", ".asf", ".xwma", ".wmv", ".oma", ".aa3", ".at3", ".mpc", ".mp+", ".mpp",
    };
    for (const char* supported : kSupportedExtensions) {
        if (ext == supported) {
            return true;
        }
    }
    return false;
}

void prefer_cue_over_audio_files(std::vector<std::string>* paths) {
    if (paths == nullptr || paths->empty()) {
        return;
    }
    std::unordered_set<std::string> audio_paths_from_cues;
    for (const std::string& path : *paths) {
        if (!CueParser::looks_like_cue_path(path)) {
            continue;
        }
        try {
            audio_paths_from_cues.insert(media_path_key(CueParser::resolve_audio_file_path(path)));
        } catch (...) {
        }
    }
    if (audio_paths_from_cues.empty()) {
        return;
    }
    paths->erase(std::remove_if(paths->begin(), paths->end(),
                                [&](const std::string& path) {
                                    if (CueParser::looks_like_cue_path(path) ||
                                        M3uPlaylistReader::looks_like_playlist_path(path)) {
                                        return false;
                                    }
                                    if (!is_audio_file_path(path)) {
                                        return false;
                                    }
                                    return audio_paths_from_cues.count(media_path_key(path)) != 0;
                                }),
                 paths->end());
}

void collect_audio_files_recursive(const std::string& directory, std::vector<std::string>* out) {
    if (out == nullptr) {
        return;
    }
    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr) {
        return;
    }
    struct DirEntry {
        std::string name;
        bool is_directory = false;
    };
    std::vector<DirEntry> entries;
    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }
        const std::string full_path = join_directory_entry(directory, entry->d_name);
        struct stat st {};
        if (stat(full_path.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            entries.push_back(DirEntry{entry->d_name, true});
        } else if (S_ISREG(st.st_mode) &&
                   (is_audio_file_path(full_path) || CueParser::looks_like_cue_path(full_path))) {
            entries.push_back(DirEntry{entry->d_name, false});
        }
    }
    closedir(dir);
    std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    for (const DirEntry& entry : entries) {
        const std::string full_path = join_directory_entry(directory, entry.name.c_str());
        if (entry.is_directory) {
            collect_audio_files_recursive(full_path, out);
        } else {
            out->push_back(full_path);
        }
    }
}

gboolean file_chooser_open_filter(const GtkFileFilterInfo* filter_info, gpointer) {
    if (filter_info == nullptr || filter_info->filename == nullptr || filter_info->filename[0] == '\0') {
        return FALSE;
    }
    if (g_file_test(filter_info->filename, G_FILE_TEST_IS_DIR)) {
        return TRUE;
    }
    return is_supported_media_path(filter_info->filename) ? TRUE : FALSE;
}

std::vector<std::string> collect_file_chooser_paths(GtkFileChooser* chooser) {
    std::vector<std::string> paths;
    if (chooser == nullptr) {
        return paths;
    }
    GSList* uris = gtk_file_chooser_get_uris(chooser);
    for (GSList* node = uris; node != nullptr; node = node->next) {
        const char* uri = static_cast<const char*>(node->data);
        if (uri == nullptr || *uri == '\0') {
            g_free(node->data);
            continue;
        }
        GError* error = nullptr;
        gchar* path = g_filename_from_uri(uri, nullptr, &error);
        if (path != nullptr) {
            paths.emplace_back(path);
            g_free(path);
        } else if (error != nullptr) {
            g_error_free(error);
        }
        g_free(node->data);
    }
    g_slist_free(uris);
    if (!paths.empty()) {
        return paths;
    }
    GSList* filenames = gtk_file_chooser_get_filenames(chooser);
    for (GSList* node = filenames; node != nullptr; node = node->next) {
        char* filename = static_cast<char*>(node->data);
        if (filename != nullptr && *filename != '\0') {
            paths.emplace_back(filename);
        }
        g_free(filename);
    }
    g_slist_free(filenames);
    if (!paths.empty()) {
        return paths;
    }
    gchar* filename = gtk_file_chooser_get_filename(chooser);
    if (filename != nullptr && *filename != '\0') {
        paths.emplace_back(filename);
        g_free(filename);
    }
    return paths;
}

void run_open_audio_dialog(GtkPlayerWindow& window) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons("Open audio files",
                                                    GTK_WINDOW(window.window_),
                                                    GTK_DIALOG_MODAL,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Open", GTK_RESPONSE_ACCEPT,
                                                    nullptr);

    GtkWidget* chooser = gtk_file_chooser_widget_new(GTK_FILE_CHOOSER_ACTION_OPEN);
    GtkFileChooser* file_chooser = GTK_FILE_CHOOSER(chooser);
    gtk_file_chooser_set_select_multiple(file_chooser, TRUE);
    if (!window.last_open_directory_.empty()) {
        gtk_file_chooser_set_current_folder(file_chooser, window.last_open_directory_.c_str());
    }

    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Audio, cue, playlist files and folders");
    gtk_file_filter_add_custom(filter, GTK_FILE_FILTER_FILENAME, file_chooser_open_filter, nullptr, nullptr);
    gtk_file_chooser_add_filter(file_chooser, filter);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_pack_start(GTK_BOX(content), chooser, TRUE, TRUE, 0);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 900, 600);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* current_folder = gtk_file_chooser_get_current_folder(file_chooser);
        if (current_folder != nullptr && *current_folder != '\0') {
            window.last_open_directory_ = current_folder;
            g_free(current_folder);
        }

        const std::vector<std::string> selected_paths = collect_file_chooser_paths(file_chooser);
        if (!selected_paths.empty()) {
            window.last_open_directory_ = parent_directory(selected_paths.front());
            std::vector<std::string> paths_to_add;
            paths_to_add.reserve(selected_paths.size());
            for (const std::string& path : selected_paths) {
                if (g_file_test(path.c_str(), G_FILE_TEST_IS_DIR)) {
                    collect_audio_files_recursive(path, &paths_to_add);
                } else {
                    paths_to_add.push_back(path);
                }
            }
            prefer_cue_over_audio_files(&paths_to_add);
            window.search_controller_->clear_search();
            window.load_source_paths(paths_to_add, true, false, true);
            patches::save_playlist_session(window);
        }
    }

    gtk_widget_destroy(dialog);
}

} // namespace pcmtp::patches
