#include "pcmtp/gui/GtkPlayerWindow.hpp"
#include <climits>
#include <cmath>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <dirent.h>
#include <cerrno>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <functional>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <pango/pango.h>
#include <gdk/gdkkeysyms.h>
#include <gio/gio.h>
#include "pcmtp/backend/AlsaPcmBackend.hpp"
#include "pcmtp/decoder/ExternalAudioDecoder.hpp"
#include "pcmtp/decoder/GaplessChainDecoder.hpp"
#include "pcmtp/decoder/FlacStreamDecoder.hpp"
#include "pcmtp/decoder/MemoryAudioDecoder.hpp"
#include "pcmtp/decoder/RangeLimitedDecoder.hpp"
#include "pcmtp/dsp/ToneControlDesign.hpp"
#include "pcmtp/playlist/M3uPlaylistReader.hpp"
#include "pcmtp/mpris/MprisService.hpp"
#include "pcmtp/session/PlaylistSession.hpp"
#include "pcmtp/util/Logger.hpp"
#include "pcmtp/util/MediaUri.hpp"

namespace pcmtp {

namespace {

constexpr int kMaxStreamReconnectAttempts = 5;
constexpr int kStreamReconnectDelaysSec[] = {3, 5, 8, 12, 20};

struct StreamMetadataUpdate {
    GtkPlayerWindow* self = nullptr;
    std::string title;
};

} // namespace
namespace {

constexpr int kClipIndicatorHoldMs = 700;

std::string path_from_mpris_uri(const std::string& uri) {
    if (uri.compare(0, 7, "file://") != 0) {
        return {};
    }

    GError* error = nullptr;
    gchar* path = g_filename_from_uri(uri.c_str(), nullptr, &error);
    if (path == nullptr) {
        if (error != nullptr) {
            g_error_free(error);
        }
        return {};
    }

    const std::string result(path);
    g_free(path);
    return result;
}

std::string file_uri_for_path(const std::string& path) {
    GError* error = nullptr;
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, &error);
    if (uri == nullptr) {
        if (error != nullptr) {
            g_error_free(error);
        }
        return "file://" + path;
    }

    const std::string result(uri);
    g_free(uri);
    return result;
}

bool is_cover_art_extension(const std::string& extension) {
    std::string lower = extension;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == ".jpg" || lower == ".jpeg" || lower == ".png" || lower == ".tif";
}

int cover_art_stem_priority(const std::string& stem) {
    std::string lower = stem;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "cover") {
        return 0;
    }
    if (lower == "folder") {
        return 1;
    }
    if (lower == "front") {
        return 2;
    }
    return 100;
}

int cover_art_extension_priority(const std::string& extension) {
    std::string lower = extension;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == ".jpg") {
        return 0;
    }
    if (lower == ".jpeg") {
        return 1;
    }
    if (lower == ".png") {
        return 2;
    }
    if (lower == ".tif") {
        return 3;
    }
    return 100;
}

struct CoverCandidate {
    std::string path;
    std::string filename;
    std::string stem;
    std::string extension;
};

bool cover_art_path_preferred(const CoverCandidate& left, const CoverCandidate& right) {
    const int left_stem = cover_art_stem_priority(left.stem);
    const int right_stem = cover_art_stem_priority(right.stem);
    if (left_stem != right_stem) {
        return left_stem < right_stem;
    }
    const int left_ext = cover_art_extension_priority(left.extension);
    const int right_ext = cover_art_extension_priority(right.extension);
    if (left_ext != right_ext) {
        return left_ext < right_ext;
    }
    return left.filename < right.filename;
}

std::string directory_of_path(const std::string& path) {
    const std::size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return ".";
    }
    if (pos == 0) {
        return path.substr(0, 1);
    }
    return path.substr(0, pos);
}

std::string split_filename_parts(const std::string& filename, std::string* extension_out) {
    const std::size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
        if (extension_out != nullptr) {
            *extension_out = {};
        }
        return filename;
    }
    if (extension_out != nullptr) {
        *extension_out = filename.substr(dot);
    }
    return filename.substr(0, dot);
}

std::string find_cover_art_in_directory(const std::string& audio_file_path) {
    const std::string directory = directory_of_path(audio_file_path);
    if (directory.empty()) {
        return {};
    }

    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr) {
        return {};
    }

    std::vector<CoverCandidate> candidates;
    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        std::string full_path = directory;
        if (!full_path.empty() && full_path.back() != '/') {
            full_path.push_back('/');
        }
        full_path += entry->d_name;

        struct stat st {};
        if (stat(full_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        std::string extension;
        const std::string stem = split_filename_parts(entry->d_name, &extension);
        if (!is_cover_art_extension(extension)) {
            continue;
        }

        candidates.push_back(CoverCandidate{full_path, entry->d_name, stem, extension});
    }
    closedir(dir);

    if (candidates.empty()) {
        return {};
    }

    std::sort(candidates.begin(), candidates.end(), cover_art_path_preferred);
    return candidates.front().path;
}

bool is_supported_media_path(const std::string& path) {
    if (ExternalAudioDecoder::is_stream_uri(path)) {
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
        ".flac", ".mp3", ".wav", ".wave", ".bwf", ".aiff", ".aif", ".au", ".snd", ".caf", ".ape", ".wv", ".tak",
        ".tta", ".dsf", ".m4a", ".aac", ".ogg", ".oga", ".opus", ".wma", ".asf", ".xwma", ".oma", ".aa3", ".at3",
        ".mpc", ".mp+", ".mpp",
    };
    for (const char* supported : kSupportedExtensions) {
        if (ext == supported) {
            return true;
        }
    }
    return false;
}

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

    std::sort(entries.begin(), entries.end(), [](const DirEntry& left, const DirEntry& right) {
        return left.name < right.name;
    });

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

std::string stream_display_label(const std::string& url) {
    if (url.empty()) {
        return "Stream";
    }
    const std::size_t scheme = url.find("://");
    std::size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    while (start < url.size() && url[start] == '/') {
        ++start;
    }
    std::size_t end = url.find('/', start);
    if (end == std::string::npos) {
        end = url.size();
    }
    const std::size_t query = url.find('?', start);
    if (query != std::string::npos && query < end) {
        end = query;
    }
    if (end <= start) {
        return url;
    }
    return url.substr(start, end - start);
}

std::uint32_t stream_sample_rate_hint(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    struct RateHint {
        const char* token;
        std::uint32_t rate;
    };
    static const RateHint hints[] = {
        {"192 khz", 192000}, {"192khz", 192000},
        {"176.4 khz", 176400}, {"176.4khz", 176400},
        {"96 khz", 96000}, {"96khz", 96000},
        {"88.2 khz", 88200}, {"88.2khz", 88200},
        {"48 khz", 48000}, {"48khz", 48000},
        {"44.1 khz", 44100}, {"44.1khz", 44100},
    };
    for (const RateHint& hint : hints) {
        if (lower.find(hint.token) != std::string::npos) {
            return hint.rate;
        }
    }
    return 0;
}

std::uint16_t stream_bits_per_sample_hint(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower.find("24-bit") != std::string::npos || lower.find("24 bit") != std::string::npos) {
        return 24;
    }
    if (lower.find("32-bit") != std::string::npos || lower.find("32 bit") != std::string::npos) {
        return 32;
    }
    if (lower.find("flac") != std::string::npos) {
        return 16;
    }
    return 0;
}

constexpr const char* kMprisNoTrackObjectPath = "/org/mpris/MediaPlayer2/TrackList/NoTrack";

std::int64_t samples_to_usec_safe(std::uint64_t samples, std::uint32_t sample_rate) {
    if (sample_rate == 0) {
        return 0;
    }

    const std::uint64_t whole_seconds = samples / sample_rate;
    const std::uint64_t remainder_samples = samples % sample_rate;
    if (whole_seconds > static_cast<std::uint64_t>(INT64_MAX / 1000000)) {
        return INT64_MAX;
    }

    return static_cast<std::int64_t>(whole_seconds * 1000000ULL +
                                     (remainder_samples * 1000000ULL) / sample_rate);
}

bool usec_to_samples_safe(std::int64_t usec, std::uint32_t sample_rate, std::uint64_t* out_samples) {
    if (out_samples == nullptr || usec < 0 || sample_rate == 0) {
        return false;
    }

    const std::uint64_t usec_u = static_cast<std::uint64_t>(usec);
    const std::uint64_t whole_seconds = usec_u / 1000000ULL;
    const std::uint64_t remainder_usec = usec_u % 1000000ULL;
    if (whole_seconds > UINT64_MAX / sample_rate) {
        return false;
    }

    const std::uint64_t whole_samples = whole_seconds * sample_rate;
    const std::uint64_t remainder_samples = (remainder_usec * sample_rate) / 1000000ULL;
    if (whole_samples > UINT64_MAX - remainder_samples) {
        return false;
    }

    *out_samples = whole_samples + remainder_samples;
    return true;
}

constexpr int kUiRefreshIntervalMs = 20;
constexpr unsigned int kUiProgressRefreshTicks = 5;
constexpr unsigned int kUiTextRefreshTicks = 25;
constexpr int kUiPreEqHeadroomMaxTenthsDb = 150;
constexpr double kUiHeadroomSafetyMarginDb = 0.0;

struct ToneGraphData {
    GtkPlayerWindow* self = nullptr;
};

struct DeepBassPresetEntry { const char* id; const char* label; };

const std::array<DeepBassPresetEntry, 2> kDeepBassPresets{{
    {"focused", "Reference"},
    {"punchy", "Punch"}
}};

int clamp_deep_bass_preset_ui(int preset) { return std::max(0, std::min(1, preset)); }

int deep_bass_internal_from_ui(int preset) {
    switch (clamp_deep_bass_preset_ui(preset)) {
        case 1: return static_cast<int>(tone::DeepBassPreset::Punchy);
        default: return static_cast<int>(tone::DeepBassPreset::Focused);
    }
}

int deep_bass_ui_from_internal(int preset) {
    switch (preset) {
        case static_cast<int>(tone::DeepBassPreset::Punchy): return 1;
        default: return 0;
    }
}

std::string format_headroom_db_text(double db) {
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss << std::setprecision(1) << db;
    return ss.str();
}

std::string format_signed_step(int value) {
    if (value > 0) return "+" + std::to_string(value);
    return std::to_string(value);
}

int clamp_deep_bass_amount_ui(int amount) {
    return std::max(-1, std::min(1, amount));
}

int deep_bass_dsp_amount_from_ui(int amount) {
    return clamp_deep_bass_amount_ui(amount);
}

bool is_absolute_path(const std::string& path) {
    return !path.empty() && path[0] == '/';
}

std::string current_working_directory() {
    char buffer[4096];
    if (getcwd(buffer, sizeof(buffer)) == nullptr) {
        return std::string(".");
    }
    return std::string(buffer);
}

std::string effective_log_path_for_display(const std::string& path) {
    if (path.empty()) {
        return current_working_directory() + "/pcm_transport.log";
    }
    if (is_absolute_path(path)) {
        return path;
    }
    return current_working_directory() + "/" + path;
}

void update_log_path_tooltip(GtkWidget* entry) {
    if (entry == nullptr) {
        return;
    }
    const gchar* text = gtk_entry_get_text(GTK_ENTRY(entry));
    const std::string path = text != nullptr ? std::string(text) : std::string();
    const std::string tip = "Current path: " + effective_log_path_for_display(path);
    gtk_widget_set_tooltip_text(entry, tip.c_str());
}

GtkWidget* create_symbolic_button(const char* primary_icon,
                                  const char* fallback_icon,
                                  const char* fallback_label) {
    GtkWidget* button = gtk_button_new();
    GtkIconTheme* theme = gtk_icon_theme_get_default();
    const char* icon_name = primary_icon;
    if (theme != nullptr && icon_name != nullptr && !gtk_icon_theme_has_icon(theme, icon_name)) {
        if (fallback_icon != nullptr && gtk_icon_theme_has_icon(theme, fallback_icon)) {
            icon_name = fallback_icon;
        } else {
            icon_name = nullptr;
        }
    }

    if (icon_name != nullptr) {
        GtkWidget* image = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
        gtk_image_set_pixel_size(GTK_IMAGE(image), 18);
        gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
        gtk_button_set_image(GTK_BUTTON(button), image);
        gtk_button_set_always_show_image(GTK_BUTTON(button), TRUE);
    } else {
        gtk_button_set_label(GTK_BUTTON(button), fallback_label != nullptr ? fallback_label : "");
    }
    return button;
}


enum PlaylistColumns {
    COL_INDEX = 0,
    COL_TRACKNO,
    COL_ARTIST,
    COL_TITLE,
    COL_SOURCE,
    COL_STREAM_BROKEN,
    COL_COUNT
};

constexpr std::size_t kBulkPlaylistImportThreshold = 8;

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

constexpr int kStreamHealthOkSeconds = 10;

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
    gtk_tree_model_get(model, iter, COL_STREAM_BROKEN, &broken, -1);

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

std::string base_name(const std::string& path) {
    const std::size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

std::string directory_name(const std::string& path) {
    const std::size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return ".";
    }
    if (pos == 0) {
        return path.substr(0, 1);
    }
    return path.substr(0, pos);
}

std::string current_executable_path() {
    char buffer[4096];
    const ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n <= 0) return std::string();
    buffer[n] = '\0';
    return std::string(buffer);
}

std::string preferred_tool_path(const char* primary, const char* fallback, const char* name) {
    if (primary != nullptr && access(primary, X_OK) == 0) return std::string(primary);
    if (fallback != nullptr && access(fallback, X_OK) == 0) return std::string(fallback);
    return name != nullptr ? std::string(name) : std::string();
}

struct SpawnResult {
    bool ok = false;
    int exit_code = -1;
    std::string output;
    std::string error;
};

SpawnResult run_argv_sync(const std::vector<std::string>& args) {
    SpawnResult result;
    if (args.empty()) {
        result.error = "empty command";
        return result;
    }
    std::vector<gchar*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        argv.push_back(const_cast<gchar*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    gchar* stdout_text = nullptr;
    gchar* stderr_text = nullptr;
    gint wait_status = 0;
    GError* error = nullptr;
    const gboolean spawned = g_spawn_sync(nullptr,
                                          argv.data(),
                                          nullptr,
                                          G_SPAWN_SEARCH_PATH,
                                          nullptr,
                                          nullptr,
                                          &stdout_text,
                                          &stderr_text,
                                          &wait_status,
                                          &error);
    if (!spawned) {
        result.error = error != nullptr && error->message != nullptr ? std::string(error->message) : std::string("spawn failed");
        if (error != nullptr) g_error_free(error);
    } else {
        if (WIFEXITED(wait_status)) {
            result.exit_code = WEXITSTATUS(wait_status);
            result.ok = (result.exit_code == 0);
        } else {
            result.error = "command did not exit normally";
        }
    }
    if (stdout_text != nullptr) {
        result.output = stdout_text;
        g_free(stdout_text);
    }
    if (stderr_text != nullptr) {
        result.error = stderr_text;
        g_free(stderr_text);
    }
    if (!result.ok && result.error.empty() && result.exit_code >= 0) {
        result.error = "exit code " + std::to_string(result.exit_code);
    }
    return result;
}

std::string persistent_rt_permission_status() {
    const std::string exe = current_executable_path();
    if (exe.empty()) return "Persistent RT permission: executable path unavailable";
    const std::string getcap = preferred_tool_path("/usr/sbin/getcap", "/usr/bin/getcap", "getcap");
    const SpawnResult result = run_argv_sync({getcap, exe});
    if (!result.ok) {
        return "Persistent RT permission: unknown (getcap unavailable)";
    }
    if (result.output.find("cap_sys_nice") != std::string::npos) {
        return "Persistent RT permission: granted; restart required if granted during this session";
    }
    return "Persistent RT permission: not granted";
}

std::string apply_persistent_rt_permission(bool grant) {
    const std::string exe = current_executable_path();
    if (exe.empty()) return "Persistent RT permission: executable path unavailable";
    const std::string setcap = preferred_tool_path("/usr/sbin/setcap", "/usr/bin/setcap", "setcap");
    std::vector<std::string> args;
    args.push_back("pkexec");
    args.push_back(setcap);
    if (grant) {
        args.push_back("cap_sys_nice=eip");
    } else {
        args.push_back("-r");
    }
    args.push_back(exe);
    const SpawnResult result = run_argv_sync(args);
    if (result.ok) {
        return grant
            ? "Persistent RT permission: granted. Restart PCM Transport to use it."
            : "Persistent RT permission: revoked for next start. Restart PCM Transport.";
    }
    std::string msg = grant ? "Persistent RT permission grant failed" : "Persistent RT permission revoke failed";
    if (!result.error.empty()) msg += ": " + result.error;
    return msg;
}

std::string lower_extension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return std::string();
    }
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}


void style_dialog_action_area(GtkWidget* dialog, guint border, gint spacing);

Alsa24BitContainerPreference alsa_24bit_preference_from_id(const std::string& id) {
    if (id == "s24le") return Alsa24BitContainerPreference::PreferS24LE;
    if (id == "s24_3le") return Alsa24BitContainerPreference::PreferS24_3LE;
    if (id == "s32le") return Alsa24BitContainerPreference::PreferS32LE;
    return Alsa24BitContainerPreference::Auto;
}

std::string normalize_alsa_24bit_preference_id(const std::string& id) {
    if (id == "s24le" || id == "s24_3le" || id == "s32le") {
        return id;
    }
    return "auto";
}

int alsa_24bit_preference_combo_index(const std::string& id) {
    const std::string normalized = normalize_alsa_24bit_preference_id(id);
    if (normalized == "s24le") return 1;
    if (normalized == "s24_3le") return 2;
    if (normalized == "s32le") return 3;
    return 0;
}

void show_runtime_message(GtkWindow* parent, const char* title, const std::string& message, GtkMessageType type = GTK_MESSAGE_INFO) {
    GtkWidget* msg = gtk_message_dialog_new(parent,
                                            GTK_DIALOG_MODAL,
                                            type,
                                            GTK_BUTTONS_CLOSE,
                                            "%s",
                                            message.c_str());
    gtk_window_set_title(GTK_WINDOW(msg), title != nullptr ? title : "PCM Transport");
    style_dialog_action_area(msg, 2, 6);
    gtk_dialog_run(GTK_DIALOG(msg));
    gtk_widget_destroy(msg);
}

void show_text_report_dialog(GtkWindow* parent, const char* title, const std::string& text) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons(title != nullptr ? title : "PCM Transport report",
                                                    parent,
                                                    GTK_DIALOG_MODAL,
                                                    "_Close", GTK_RESPONSE_CLOSE,
                                                    NULL);
    style_dialog_action_area(dialog, 2, 6);
    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(area), 14);

    GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_size_request(scrolled, 760, 420);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    GtkWidget* view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_NONE);
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(buffer, text.c_str(), -1);

    gtk_container_add(GTK_CONTAINER(scrolled), view);
    gtk_box_pack_start(GTK_BOX(area), scrolled, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

std::string realtime_status_markup(const std::string& status) {
    gchar* escaped = g_markup_escape_text(status.c_str(), -1);
    std::string safe = escaped != nullptr ? std::string(escaped) : status;
    if (escaped != nullptr) g_free(escaped);
    if (status.find("active, SCHED_") != std::string::npos) {
        return std::string("<b><span foreground=\"#1a7f37\">") + safe + "</span></b>";
    }
    if (status.find("not available") != std::string::npos ||
        status.find("failed") != std::string::npos ||
        status.find("not active") != std::string::npos ||
        status.find("permission required") != std::string::npos ||
        status.find("access denied") != std::string::npos) {
        return std::string("<span foreground=\"#9a3412\">") + safe + "</span>";
    }
    return safe;
}

void set_realtime_status_label(GtkWidget* label, const std::string& status) {
    if (label == nullptr || !GTK_IS_LABEL(label)) return;
    const std::string markup = realtime_status_markup(status);
    gtk_label_set_markup(GTK_LABEL(label), markup.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 92);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_widget_set_tooltip_text(label, status.c_str());
}

std::string realtime_settings_status_text(PlaybackEngine& engine) {
    const std::string rt = engine.refresh_realtime_priority_status();
    return rt + "\n" + persistent_rt_permission_status();
}


void add_probe_cell(GtkGrid* grid, GtkWidget* child, int column, int row, bool header, bool ok = false, bool fail = false) {
    gtk_widget_set_hexpand(child, TRUE);
    gtk_widget_set_vexpand(child, FALSE);
    gtk_widget_set_halign(child, GTK_ALIGN_FILL);
    gtk_widget_set_valign(child, GTK_ALIGN_FILL);
    gtk_style_context_add_class(gtk_widget_get_style_context(child), "alsa-probe-cell");
    if (header) {
        gtk_style_context_add_class(gtk_widget_get_style_context(child), "alsa-probe-header");
    }
    if (ok) {
        gtk_style_context_add_class(gtk_widget_get_style_context(child), "alsa-probe-ok");
    }
    if (fail) {
        gtk_style_context_add_class(gtk_widget_get_style_context(child), "alsa-probe-fail");
    }
    gtk_grid_attach(grid, child, column, row, 1, 1);
}


void show_alsa_probe_table_dialog(GtkWindow* parent, const AlsaProbeMatrix& matrix) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons("ALSA device probe",
                                                    parent,
                                                    GTK_DIALOG_MODAL,
                                                    "_Close", GTK_RESPONSE_CLOSE,
                                                    NULL);
    style_dialog_action_area(dialog, 2, 6);
    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(area), 14);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(area), box, FALSE, FALSE, 0);

    GtkWidget* title = gtk_label_new(nullptr);
    gchar* escaped_device = g_markup_escape_text(matrix.device_name.c_str(), -1);
    const std::string safe_device = escaped_device != nullptr ? std::string(escaped_device) : matrix.device_name;
    if (escaped_device != nullptr) {
        g_free(escaped_device);
    }
    const std::string title_text = std::string("<b>ALSA device probe</b>\nDevice: ") + safe_device +
                                   "\nMode: playback, RW_INTERLEAVED, stereo";
    gtk_label_set_markup(GTK_LABEL(title), title_text.c_str());
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

    GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_size_request(scrolled, 720, 156);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_widget_set_margin_top(scrolled, 2);
    gtk_widget_set_margin_bottom(scrolled, 2);
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 0);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 0);
    gtk_container_add(GTK_CONTAINER(scrolled), grid);

    GtkWidget* first = gtk_label_new("Format");
    gtk_label_set_xalign(GTK_LABEL(first), 0.0f);
    add_probe_cell(GTK_GRID(grid), first, 0, 0, true);
    for (std::size_t r = 0; r < matrix.sample_rates.size(); ++r) {
        std::string rate_label;
        if (matrix.sample_rates[r] % 1000 == 0) {
            rate_label = std::to_string(matrix.sample_rates[r] / 1000) + " kHz";
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f kHz", static_cast<double>(matrix.sample_rates[r]) / 1000.0);
            rate_label = buf;
        }
        GtkWidget* label = gtk_label_new(rate_label.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.5f);
        add_probe_cell(GTK_GRID(grid), label, static_cast<int>(r + 1), 0, true);
    }
    for (std::size_t f = 0; f < matrix.format_names.size(); ++f) {
        GtkWidget* format = gtk_label_new(matrix.format_names[f].c_str());
        gtk_label_set_xalign(GTK_LABEL(format), 0.0f);
        add_probe_cell(GTK_GRID(grid), format, 0, static_cast<int>(f + 1), true);
        for (std::size_t r = 0; r < matrix.sample_rates.size(); ++r) {
            const std::size_t idx = f * matrix.sample_rates.size() + r;
            const bool ok = idx < matrix.cells.size() && matrix.cells[idx].supported;
            GtkWidget* value = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(value), ok ? "<b>OK</b>" : "<b>✗</b>");
            gtk_label_set_xalign(GTK_LABEL(value), 0.5f);
            add_probe_cell(GTK_GRID(grid), value, static_cast<int>(r + 1), static_cast<int>(f + 1), false, ok, !ok);
        }
    }
    gtk_box_pack_start(GTK_BOX(box), scrolled, FALSE, FALSE, 0);

    GtkWidget* note = gtk_label_new("This probe tests the selected ALSA PCM device directly. Other players may use plug/dmix or a different subdevice. Stop playback before probing for reliable results.");
    gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(note), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(note), 92);
    gtk_widget_set_margin_top(note, 2);
    gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}



std::string codec_family_from_name(const std::string& codec_name, const std::string& path) {
    std::string codec = codec_name;
    std::transform(codec.begin(), codec.end(), codec.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const std::string ext = lower_extension(path);
    if (codec == "mp3" || ext == ".mp3") return "mp3";
    if (codec == "aac" || ext == ".aac" || (ext == ".m4a" && codec != "alac")) return "aac";
    if (codec == "vorbis") return "vorbis";
    if (codec == "opus" || ext == ".opus") return "opus";
    if (codec == "mpc" || ext == ".mpc" || ext == ".mp+" || ext == ".mpp") return "mpc";
    if (codec.find("wmav") == 0 || codec == "wmapro" || codec == "wmalossless" || ext == ".wma" || ext == ".asf" || ext == ".xwma") return "wma";
    if (codec.find("atrac") == 0 || ext == ".oma" || ext == ".aa3" || ext == ".at3") return "atrac";
    if (codec == "alac") return "alac";
    if (codec == "flac" || ext == ".flac") return "flac";
    if (codec == "ape" || ext == ".ape") return "ape";
    if (codec == "wavpack" || ext == ".wv") return "wavpack";
    if (codec == "tta" || ext == ".tta") return "tta";
    if (codec == "tak" || ext == ".tak") return "tak";
    if (codec.find("dsd_") == 0 || ext == ".dsf") return "dsd";
    if (codec.size() >= 4 && codec.compare(0, 4, "pcm_") == 0) return "pcm";
    if (ext == ".wav" || ext == ".wave" || ext == ".bwf" || ext == ".aiff" || ext == ".aif" || ext == ".au" || ext == ".snd" || ext == ".caf") return "pcm";
    return codec.empty() ? ext : codec;
}

bool lossy_gapless_family(const std::string& family) {
    return family == "mp3" || family == "aac" || family == "vorbis" || family == "opus";
}


std::string safe_utf8_for_display(const std::string& text) {
    if (text.empty()) return text;
    if (g_utf8_validate(text.c_str(), -1, nullptr)) return text;

    auto convert_from = [&text](const char* from_codeset) -> std::string {
        GError* error = nullptr;
        gchar* converted = g_convert(text.c_str(), static_cast<gssize>(text.size()),
                                     "UTF-8", from_codeset, nullptr, nullptr, &error);
        if (error != nullptr) g_error_free(error);
        if (converted == nullptr) return std::string();
        std::string out(converted);
        g_free(converted);
        return g_utf8_validate(out.c_str(), -1, nullptr) ? out : std::string();
    };

    GError* locale_error = nullptr;
    gchar* locale_converted = g_locale_to_utf8(text.c_str(), static_cast<gssize>(text.size()),
                                               nullptr, nullptr, &locale_error);
    if (locale_error != nullptr) g_error_free(locale_error);
    if (locale_converted != nullptr) {
        std::string out(locale_converted);
        g_free(locale_converted);
        if (g_utf8_validate(out.c_str(), -1, nullptr)) return out;
    }

    const char* fallbacks[] = {"WINDOWS-1251", "CP1251", "ISO-8859-1", nullptr};
    for (const char** enc = fallbacks; *enc != nullptr; ++enc) {
        std::string converted = convert_from(*enc);
        if (!converted.empty()) return converted;
    }

    std::string out;
    out.reserve(text.size());
    const char* cur = text.data();
    const char* end = cur + text.size();
    while (cur < end) {
        const gunichar ch = g_utf8_get_char_validated(cur, end - cur);
        if (ch == static_cast<gunichar>(-1) || ch == static_cast<gunichar>(-2)) {
            out += '?';
            ++cur;
            continue;
        }
        const char* next = g_utf8_next_char(cur);
        out.append(cur, static_cast<std::size_t>(next - cur));
        cur = next;
    }
    return out;
}

void set_label_text_if_changed(GtkWidget* label, const std::string& text) {
    if (label == nullptr || !GTK_IS_LABEL(label)) return;
    const char* current = gtk_label_get_text(GTK_LABEL(label));
    if (current != nullptr && text == current) return;
    gtk_label_set_text(GTK_LABEL(label), text.c_str());
}

void set_widget_opacity_if_changed(GtkWidget* widget, double opacity) {
    if (widget == nullptr) return;
    if (std::fabs(gtk_widget_get_opacity(widget) - opacity) < 0.0001) return;
    gtk_widget_set_opacity(widget, opacity);
}

std::unique_ptr<IAudioDecoder> create_decoder_for_path(const std::string& path) {
    const std::string ext = lower_extension(path);
    if (ext == ".flac") {
        return std::unique_ptr<IAudioDecoder>(new FlacStreamDecoder());
    }
    if (ExternalAudioDecoder::looks_supported(path)) {
        return std::unique_ptr<IAudioDecoder>(new ExternalAudioDecoder());
    }
    throw std::runtime_error("Unsupported audio file type: " + ext);
}

std::string shell_quote(const std::string& value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') out += "'\\''";
        else out += ch;
    }
    out += "'";
    return out;
}

void append_text_view(GtkWidget* view, const std::string& text) {
    if (view == nullptr || !GTK_IS_TEXT_VIEW(view)) return;
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, text.c_str(), -1);
    GtkTextMark* mark = gtk_text_buffer_get_insert(buffer);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(view), mark);
}

struct DiagnosticsUiUpdate {
    GtkWidget* text_view = nullptr;
    GtkWidget* progress_bar = nullptr;
    GtkWidget* close_button = nullptr;
    std::string text;
    double fraction = -1.0;
    bool finished = false;
};

gboolean diagnostics_ui_update_cb(gpointer user_data) {
    std::unique_ptr<DiagnosticsUiUpdate> update(static_cast<DiagnosticsUiUpdate*>(user_data));
    if (update->text_view != nullptr && !update->text.empty()) {
        append_text_view(update->text_view, update->text);
    }
    if (update->progress_bar != nullptr && update->fraction >= 0.0) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(update->progress_bar), std::max(0.0, std::min(1.0, update->fraction)));
    }
    if (update->finished && update->close_button != nullptr) {
        gtk_widget_set_sensitive(update->close_button, TRUE);
        GtkWidget* toplevel = gtk_widget_get_toplevel(update->close_button);
        if (toplevel != nullptr && GTK_IS_WINDOW(toplevel)) {
            gtk_window_set_deletable(GTK_WINDOW(toplevel), TRUE);
        }
    }
    return G_SOURCE_REMOVE;
}

void post_diagnostics_update(GtkWidget* text_view, GtkWidget* progress_bar, GtkWidget* close_button,
                             const std::string& text, double fraction, bool finished) {
    DiagnosticsUiUpdate* update = new DiagnosticsUiUpdate();
    update->text_view = text_view;
    update->progress_bar = progress_bar;
    update->close_button = close_button;
    update->text = text;
    update->fraction = fraction;
    update->finished = finished;
    g_idle_add(diagnostics_ui_update_cb, update);
    if (!text.empty()) Logger::instance().info(text);
}

void write_le16(std::ofstream& out, std::uint16_t v) {
    char b[2] = {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff)};
    out.write(b, 2);
}

void write_le32(std::ofstream& out, std::uint32_t v) {
    char b[4] = {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff), static_cast<char>((v >> 16) & 0xff), static_cast<char>((v >> 24) & 0xff)};
    out.write(b, 4);
}

std::int16_t clamp_i16(int value) {
    return static_cast<std::int16_t>(std::max(-32768, std::min(32767, value)));
}

void write_test_wav(const std::string& path, int duration_seconds) {
    constexpr std::uint32_t sample_rate = 44100;
    constexpr std::uint16_t channels = 2;
    constexpr std::uint16_t bits = 16;
    const std::uint32_t frames = static_cast<std::uint32_t>(std::max(1, duration_seconds) * sample_rate);
    const std::uint32_t data_size = frames * channels * (bits / 8);
    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out) throw std::runtime_error("Cannot create test WAV");
    out.write("RIFF", 4); write_le32(out, 36 + data_size); out.write("WAVE", 4);
    out.write("fmt ", 4); write_le32(out, 16); write_le16(out, 1); write_le16(out, channels);
    write_le32(out, sample_rate); write_le32(out, sample_rate * channels * (bits / 8));
    write_le16(out, channels * (bits / 8)); write_le16(out, bits);
    out.write("data", 4); write_le32(out, data_size);
    std::uint32_t rng = 0x12345678u;
    for (std::uint32_t i = 0; i < frames; ++i) {
        std::int16_t left = 0;
        std::int16_t right = 0;
        const std::uint32_t section = (i * 8u) / std::max(1u, frames);
        switch (section) {
            case 0: left = 0; right = 0; break;
            case 1: left = (i & 1u) ? 1 : -1; right = (i & 2u) ? 2 : -2; break;
            case 2: left = 32767; right = -32768; break;
            case 3: left = (i & 1u) ? 32767 : -32768; right = (i & 1u) ? -32768 : 32767; break;
            case 4: left = (i % 997u == 0u) ? 30000 : 0; right = (i % 991u == 0u) ? -30000 : 0; break;
            case 5:
                rng = rng * 1664525u + 1013904223u;
                left = static_cast<std::int16_t>(rng >> 16);
                rng = rng * 1664525u + 1013904223u;
                right = static_cast<std::int16_t>(rng >> 16);
                break;
            case 6: {
                const double t = static_cast<double>(i) / static_cast<double>(sample_rate);
                left = clamp_i16(static_cast<int>(std::lrint(std::sin(2.0 * M_PI * 37.0 * t) * 18000.0)));
                right = clamp_i16(static_cast<int>(std::lrint(std::sin(2.0 * M_PI * 53.0 * t + 0.35) * 12000.0)));
                break;
            }
            default:
                left = clamp_i16(static_cast<int>((static_cast<int>(i % 65536u) - 32768) / 2));
                right = clamp_i16(static_cast<int>(32767 - static_cast<int>(i % 65536u)) / 2);
                break;
        }
        write_le16(out, static_cast<std::uint16_t>(left));
        write_le16(out, static_cast<std::uint16_t>(right));
    }
}

struct WavPcm16Data {
    std::vector<std::int16_t> samples;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
};

std::uint16_t read_u16_le(const std::vector<unsigned char>& b, std::size_t p) {
    return static_cast<std::uint16_t>(b[p] | (b[p + 1] << 8));
}

std::uint32_t read_u32_le(const std::vector<unsigned char>& b, std::size_t p) {
    return static_cast<std::uint32_t>(b[p] | (b[p + 1] << 8) | (b[p + 2] << 16) | (b[p + 3] << 24));
}

WavPcm16Data read_wav_pcm16(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open WAV for comparison");
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
        throw std::runtime_error("Invalid WAV file");
    }
    WavPcm16Data out;
    std::size_t pos = 12;
    std::size_t data_pos = 0, data_size = 0;
    while (pos + 8 <= b.size()) {
        const std::uint32_t sz = read_u32_le(b, pos + 4);
        const std::size_t payload = pos + 8;
        if (payload + sz > b.size()) break;
        if (std::memcmp(b.data() + pos, "fmt ", 4) == 0 && sz >= 16) {
            const std::uint16_t audio_format = read_u16_le(b, payload);
            out.channels = read_u16_le(b, payload + 2);
            out.sample_rate = read_u32_le(b, payload + 4);
            out.bits = read_u16_le(b, payload + 14);
            if (audio_format != 1 || out.bits != 16) throw std::runtime_error("Expected 16-bit PCM WAV");
        } else if (std::memcmp(b.data() + pos, "data", 4) == 0) {
            data_pos = payload; data_size = sz;
        }
        pos = payload + sz + (sz & 1u);
    }
    if (data_pos == 0 || out.channels == 0) throw std::runtime_error("WAV data chunk not found");
    out.samples.resize(data_size / 2);
    for (std::size_t i = 0; i < out.samples.size(); ++i) {
        out.samples[i] = static_cast<std::int16_t>(read_u16_le(b, data_pos + i * 2));
    }
    return out;
}

struct DiagnosticShelfState { double z1 = 0.0; double z2 = 0.0; };

double diagnostic_process_sample(double input, const tone::ShelfCoefficients& c, DiagnosticShelfState& s) {
    const double out = c.b0 * input + s.z1;
    s.z1 = c.b1 * input - c.a1 * out + s.z2;
    s.z2 = c.b2 * input - c.a2 * out;
    return out;
}

double diagnostic_clamp_to_bits(double sample, std::uint16_t bits_per_sample) {
    const double limit = static_cast<double>(pcm_full_scale(bits_per_sample));
    if (limit <= 0.0) return sample;
    if (sample > limit) return limit;
    if (sample < -limit) return -limit;
    return sample;
}

std::vector<std::int16_t> render_internal_path_16(const std::string& flac_path,
                                                  int soft_volume_percent,
                                                  int bass_db,
                                                  int treble_db,
                                                  int pre_eq_headroom_tenths_db,
                                                  bool deep_bass_enabled,
                                                  int deep_bass_preset,
                                                  int deep_bass_amount,
                                                  int bass_hz,
                                                  int treble_hz) {
    FlacStreamDecoder decoder;
    decoder.open(flac_path);
    const AudioFormat fmt = decoder.format();
    if (fmt.sample_rate != 44100 || fmt.channels != 2 || fmt.bits_per_sample != 16) {
        throw std::runtime_error("Diagnostic test expects generated 16-bit / 44.1 kHz / stereo FLAC");
    }
    std::vector<PcmSample> block(4096);
    std::vector<std::int16_t> out;
    out.reserve(static_cast<std::size_t>(decoder.total_samples_per_channel() * fmt.channels));
    DiagnosticShelfState low_l{}, low_r{}, high_l{}, high_r{};
    tone::DeepBassState deep_l{}, deep_r{};
    const auto low = tone::make_low_shelf(fmt.sample_rate, static_cast<double>(bass_db), static_cast<double>(bass_hz));
    const auto high = tone::make_high_shelf(fmt.sample_rate, static_cast<double>(treble_db), static_cast<double>(treble_hz));
    const bool dsp_active = soft_volume_percent < 100 || bass_db != 0 || treble_db != 0 || pre_eq_headroom_tenths_db > 0 || deep_bass_enabled;
    const double user_volume = static_cast<double>(soft_volume_percent) / 100.0;
    const double pre_eq_gain = std::pow(10.0, -(static_cast<double>(pre_eq_headroom_tenths_db) / 10.0) / 20.0);
    const double full_scale = static_cast<double>(pcm_full_scale(fmt.bits_per_sample));
    const double inv_full_scale = full_scale > 0.0 ? 1.0 / full_scale : 0.0;
    const double deep_bass_amount_gain = tone::deep_bass_amount_gain_from_steps(deep_bass_amount);
    while (!decoder.eof()) {
        const std::size_t got = decoder.read_samples(block.data(), block.size());
        if (got == 0) break;
        for (std::size_t i = 0; i < got; ++i) {
            double sample = static_cast<double>(block[i]);
            if (dsp_active) {
                const bool left = ((i % 2) == 0);
                sample *= pre_eq_gain;
                if (bass_db != 0) sample = diagnostic_process_sample(sample, low, left ? low_l : low_r);
                if (treble_db != 0) sample = diagnostic_process_sample(sample, high, left ? high_l : high_r);
                if (deep_bass_enabled && inv_full_scale > 0.0) {
                    sample = tone::process_deep_bass_normalized(sample * inv_full_scale, fmt.sample_rate,
                                                               static_cast<tone::DeepBassPreset>(deep_bass_preset),
                                                               left ? deep_l : deep_r,
                                                               deep_bass_amount_gain) * full_scale;
                }
                sample *= user_volume;
                sample = diagnostic_clamp_to_bits(sample, fmt.bits_per_sample);
                sample = std::llround(sample);
            }
            out.push_back(static_cast<std::int16_t>(static_cast<PcmSample>(sample)));
        }
    }
    return out;
}

struct CompareResult {
    bool pass = true;
    std::size_t compared = 0;
    std::size_t first_mismatch = 0;
    std::int16_t expected = 0;
    std::int16_t actual = 0;
    int max_diff = 0;
};

CompareResult compare_samples(const std::vector<std::int16_t>& expected, const std::vector<std::int16_t>& actual) {
    CompareResult r;
    r.compared = std::min(expected.size(), actual.size());
    if (expected.size() != actual.size()) {
        r.pass = false;
    }
    for (std::size_t i = 0; i < r.compared; ++i) {
        const int diff = std::abs(static_cast<int>(expected[i]) - static_cast<int>(actual[i]));
        if (diff > r.max_diff) r.max_diff = diff;
        if (expected[i] != actual[i] && r.pass) {
            r.pass = false;
            r.first_mismatch = i;
            r.expected = expected[i];
            r.actual = actual[i];
        }
    }
    if (!r.pass && r.compared == expected.size() && r.compared == actual.size() && expected[r.first_mismatch] == actual[r.first_mismatch]) {
        r.first_mismatch = r.compared;
    }
    return r;
}


int current_card_index(const std::vector<CardProfileInfo>& cards, const std::string& device_name) {
    for (const auto& card : cards) {
        if (card.hw_device == device_name) return card.card_index;
    }
    return -1;
}


struct DeleteRateRuleData {
    GtkPlayerWindow* self;
    GtkWidget* dialog;
    GtkWidget* row;
    std::uint32_t from_rate;
    std::uint32_t to_rate;
};

struct DeleteBitRuleData {
    GtkPlayerWindow* self;
    GtkWidget* dialog;
    GtkWidget* row;
    std::uint16_t from_bits;
    std::uint16_t to_bits;
};

struct AddRateRuleData {
    GtkPlayerWindow* self;
    GtkWidget* dialog;
    GtkWidget* from_combo;
    GtkWidget* to_combo;
    GtkWidget* list;
};

struct AddBitRuleData {
    GtkPlayerWindow* self;
    GtkWidget* dialog;
    GtkWidget* from_combo;
    GtkWidget* to_combo;
    GtkWidget* list;
};

void destroy_delete_rate_rule_data(gpointer data, GClosure*) { delete static_cast<DeleteRateRuleData*>(data); }
void destroy_delete_bit_rule_data(gpointer data, GClosure*) { delete static_cast<DeleteBitRuleData*>(data); }
void destroy_add_rate_rule_data(gpointer data, GClosure*) { delete static_cast<AddRateRuleData*>(data); }
void destroy_add_bit_rule_data(gpointer data, GClosure*) { delete static_cast<AddBitRuleData*>(data); }

std::string serialize_resample_rules(const std::vector<GtkPlayerWindow::ResampleRule>& rules) {
    std::string out;
    for (std::size_t i = 0; i < rules.size(); ++i) {
        if (!out.empty()) out += ',';
        out += std::to_string(rules[i].from_rate) + '>' + std::to_string(rules[i].to_rate);
    }
    return out;
}

std::vector<GtkPlayerWindow::ResampleRule> parse_resample_rules(const std::string& text) {
    std::vector<GtkPlayerWindow::ResampleRule> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find(',', start);
        const std::string token = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const std::size_t sep = token.find('>');
        if (sep != std::string::npos) {
            try {
                GtkPlayerWindow::ResampleRule rule;
                rule.from_rate = static_cast<std::uint32_t>(std::stoul(token.substr(0, sep)));
                rule.to_rate = static_cast<std::uint32_t>(std::stoul(token.substr(sep + 1)));
                if (rule.from_rate > 0 && rule.to_rate > 0 && rule.from_rate != rule.to_rate) out.push_back(rule);
            } catch (...) {}
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

std::string serialize_bitdepth_rules(const std::vector<GtkPlayerWindow::BitDepthRule>& rules) {
    std::string out;
    for (std::size_t i = 0; i < rules.size(); ++i) {
        if (!out.empty()) out += ',';
        out += std::to_string(rules[i].from_bits) + '>' + std::to_string(rules[i].to_bits);
    }
    return out;
}

std::vector<GtkPlayerWindow::BitDepthRule> parse_bitdepth_rules(const std::string& text) {
    std::vector<GtkPlayerWindow::BitDepthRule> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find(',', start);
        const std::string token = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const std::size_t sep = token.find('>');
        if (sep != std::string::npos) {
            try {
                GtkPlayerWindow::BitDepthRule rule;
                rule.from_bits = static_cast<std::uint16_t>(std::stoul(token.substr(0, sep)));
                rule.to_bits = static_cast<std::uint16_t>(std::stoul(token.substr(sep + 1)));
                if ((rule.from_bits == 16 || rule.from_bits == 24 || rule.from_bits == 32) &&
                    (rule.to_bits == 16 || rule.to_bits == 24 || rule.to_bits == 32) &&
                    rule.from_bits != rule.to_bits) out.push_back(rule);
            } catch (...) {}
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

std::string shell_escape_cmd(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''"; else out.push_back(c);
    }
    out += "'";
    return out;
}

static const char* kEmbeddedGplV3Text = R"GPLTEXT(                    GNU GENERAL PUBLIC LICENSE
                       Version 3, 29 June 2007

 Copyright (C) 2007 Free Software Foundation, Inc. <https://fsf.org/>
 Everyone is permitted to copy and distribute verbatim copies
 of this license document, but changing it is not allowed.

                            Preamble

  The GNU General Public License is a free, copyleft license for
software and other kinds of works.

  The licenses for most software and other practical works are designed
to take away your freedom to share and change the works.  By contrast,
the GNU General Public License is intended to guarantee your freedom to
share and change all versions of a program--to make sure it remains free
software for all its users.  We, the Free Software Foundation, use the
GNU General Public License for most of our software; it applies also to
any other work released this way by its authors.  You can apply it to
your programs, too.

  When we speak of free software, we are referring to freedom, not
price.  Our General Public Licenses are designed to make sure that you
have the freedom to distribute copies of free software (and charge for
them if you wish), that you receive source code or can get it if you
want it, that you can change the software or use pieces of it in new
free programs, and that you know you can do these things.

  To protect your rights, we need to prevent others from denying you
these rights or asking you to surrender the rights.  Therefore, you have
certain responsibilities if you distribute copies of the software, or if
you modify it: responsibilities to respect the freedom of others.

  For example, if you distribute copies of such a program, whether
gratis or for a fee, you must pass on to the recipients the same
freedoms that you received.  You must make sure that they, too, receive
or can get the source code.  And you must show them these terms so they
know their rights.

  Developers that use the GNU GPL protect your rights with two steps:
(1) assert copyright on the software, and (2) offer you this License
giving you legal permission to copy, distribute and/or modify it.

  For the developers' and authors' protection, the GPL clearly explains
that there is no warranty for this free software.  For both users' and
authors' sake, the GPL requires that modified versions be marked as
changed, so that their problems will not be attributed erroneously to
authors of previous versions.

  Some devices are designed to deny users access to install or run
modified versions of the software inside them, although the manufacturer
can do so.  This is fundamentally incompatible with the aim of
protecting users' freedom to change the software.  The systematic
pattern of such abuse occurs in the area of products for individuals to
use, which is precisely where it is most unacceptable.  Therefore, we
have designed this version of the GPL to prohibit the practice for those
products.  If such problems arise substantially in other domains, we
stand ready to extend this provision to those domains in future versions
of the GPL, as needed to protect the freedom of users.

  Finally, every program is threatened constantly by software patents.
States should not allow patents to restrict development and use of
software on general-purpose computers, but in those that do, we wish to
avoid the special danger that patents applied to a free program could
make it effectively proprietary.  To prevent this, the GPL assures that
patents cannot be used to render the program non-free.

  The precise terms and conditions for copying, distribution and
modification follow.

                       TERMS AND CONDITIONS

  0. Definitions.

  "This License" refers to version 3 of the GNU General Public License.

  "Copyright" also means copyright-like laws that apply to other kinds of
works, such as semiconductor masks.

  "The Program" refers to any copyrightable work licensed under this
License.  Each licensee is addressed as "you".  "Licensees" and
"recipients" may be individuals or organizations.

  To "modify" a work means to copy from or adapt all or part of the work
in a fashion requiring copyright permission, other than the making of an
exact copy.  The resulting work is called a "modified version" of the
earlier work or a work "based on" the earlier work.

  A "covered work" means either the unmodified Program or a work based
on the Program.

  To "propagate" a work means to do anything with it that, without
permission, would make you directly or secondarily liable for
infringement under applicable copyright law, except executing it on a
computer or modifying a private copy.  Propagation includes copying,
distribution (with or without modification), making available to the
public, and in some countries other activities as well.

  To "convey" a work means any kind of propagation that enables other
parties to make or receive copies.  Mere interaction with a user through
a computer network, with no transfer of a copy, is not conveying.

  An interactive user interface displays "Appropriate Legal Notices"
to the extent that it includes a convenient and prominently visible
feature that (1) displays an appropriate copyright notice, and (2)
tells the user that there is no warranty for the work (except to the
extent that warranties are provided), that licensees may convey the
work under this License, and how to view a copy of this License.  If
the interface presents a list of user commands or options, such as a
menu, a prominent item in the list meets this criterion.

  1. Source Code.

  The "source code" for a work means the preferred form of the work
for making modifications to it.  "Object code" means any non-source
form of a work.

  A "Standard Interface" means an interface that either is an official
standard defined by a recognized standards body, or, in the case of
interfaces specified for a particular programming language, one that
is widely used among developers working in that language.

  The "System Libraries" of an executable work include anything, other
than the work as a whole, that (a) is included in the normal form of
packaging a Major Component, but which is not part of that Major
Component, and (b) serves only to enable use of the work with that
Major Component, or to implement a Standard Interface for which an
implementation is available to the public in source code form.  A
"Major Component", in this context, means a major essential component
(kernel, window system, and so on) of the specific operating system
(if any) on which the executable work runs, or a compiler used to
produce the work, or an object code interpreter used to run it.

  The "Corresponding Source" for a work in object code form means all
the source code needed to generate, install, and (for an executable
work) run the object code and to modify the work, including scripts to
control those activities.  However, it does not include the work's
System Libraries, or general-purpose tools or generally available free
programs which are used unmodified in performing those activities but
which are not part of the work.  For example, Corresponding Source
includes interface definition files associated with source files for
the work, and the source code for shared libraries and dynamically
linked subprograms that the work is specifically designed to require,
such as by intimate data communication or control flow between those
subprograms and other parts of the work.

  The Corresponding Source need not include anything that users
can regenerate automatically from other parts of the Corresponding
Source.

  The Corresponding Source for a work in source code form is that
same work.

  2. Basic Permissions.

  All rights granted under this License are granted for the term of
copyright on the Program, and are irrevocable provided the stated
conditions are met.  This License explicitly affirms your unlimited
permission to run the unmodified Program.  The output from running a
covered work is covered by this License only if the output, given its
content, constitutes a covered work.  This License acknowledges your
rights of fair use or other equivalent, as provided by copyright law.

  You may make, run and propagate covered works that you do not
convey, without conditions so long as your license otherwise remains
in force.  You may convey covered works to others for the sole purpose
of having them make modifications exclusively for you, or provide you
with facilities for running those works, provided that you comply with
the terms of this License in conveying all material for which you do
not control copyright.  Those thus making or running the covered works
for you must do so exclusively on your behalf, under your direction
and control, on terms that prohibit them from making any copies of
your copyrighted material outside their relationship with you.

  Conveying under any other circumstances is permitted solely under
the conditions stated below.  Sublicensing is not allowed; section 10
makes it unnecessary.

  3. Protecting Users' Legal Rights From Anti-Circumvention Law.

  No covered work shall be deemed part of an effective technological
measure under any applicable law fulfilling obligations under article
11 of the WIPO copyright treaty adopted on 20 December 1996, or
similar laws prohibiting or restricting circumvention of such
measures.

  When you convey a covered work, you waive any legal power to forbid
circumvention of technological measures to the extent such circumvention
is effected by exercising rights under this License with respect to
the covered work, and you disclaim any intention to limit operation or
modification of the work as a means of enforcing, against the work's
users, your or third parties' legal rights to forbid circumvention of
technological measures.

  4. Conveying Verbatim Copies.

  You may convey verbatim copies of the Program's source code as you
receive it, in any medium, provided that you conspicuously and
appropriately publish on each copy an appropriate copyright notice;
keep intact all notices stating that this License and any
non-permissive terms added in accord with section 7 apply to the code;
keep intact all notices of the absence of any warranty; and give all
recipients a copy of this License along with the Program.

  You may charge any price or no price for each copy that you convey,
and you may offer support or warranty protection for a fee.

  5. Conveying Modified Source Versions.

  You may convey a work based on the Program, or the modifications to
produce it from the Program, in the form of source code under the
terms of section 4, provided that you also meet all of these conditions:

    a) The work must carry prominent notices stating that you modified
    it, and giving a relevant date.

    b) The work must carry prominent notices stating that it is
    released under this License and any conditions added under section
    7.  This requirement modifies the requirement in section 4 to
    "keep intact all notices".

    c) You must license the entire work, as a whole, under this
    License to anyone who comes into possession of a copy.  This
    License will therefore apply, along with any applicable section 7
    additional terms, to the whole of the work, and all its parts,
    regardless of how they are packaged.  This License gives no
    permission to license the work in any other way, but it does not
    invalidate such permission if you have separately received it.

    d) If the work has interactive user interfaces, each must display
    Appropriate Legal Notices; however, if the Program has interactive
    interfaces that do not display Appropriate Legal Notices, your
    work need not make them do so.

  A compilation of a covered work with other separate and independent
works, which are not by their nature extensions of the covered work,
and which are not combined with it such as to form a larger program,
in or on a volume of a storage or distribution medium, is called an
"aggregate" if the compilation and its resulting copyright are not
used to limit the access or legal rights of the compilation's users
beyond what the individual works permit.  Inclusion of a covered work
in an aggregate does not cause this License to apply to the other
parts of the aggregate.

  6. Conveying Non-Source Forms.

  You may convey a covered work in object code form under the terms
of sections 4 and 5, provided that you also convey the
machine-readable Corresponding Source under the terms of this License,
in one of these ways:

    a) Convey the object code in, or embodied in, a physical product
    (including a physical distribution medium), accompanied by the
    Corresponding Source fixed on a durable physical medium
    customarily used for software interchange.

    b) Convey the object code in, or embodied in, a physical product
    (including a physical distribution medium), accompanied by a
    written offer, valid for at least three years and valid for as
    long as you offer spare parts or customer support for that product
    model, to give anyone who possesses the object code either (1) a
    copy of the Corresponding Source for all the software in the
    product that is covered by this License, on a durable physical
    medium customarily used for software interchange, for a price no
    more than your reasonable cost of physically performing this
    conveying of source, or (2) access to copy the
    Corresponding Source from a network server at no charge.

    c) Convey individual copies of the object code with a copy of the
    written offer to provide the Corresponding Source.  This
    alternative is allowed only occasionally and noncommercially, and
    only if you received the object code with such an offer, in accord
    with subsection 6b.

    d) Convey the object code by offering access from a designated
    place (gratis or for a charge), and offer equivalent access to the
    Corresponding Source in the same way through the same place at no
    further charge.  You need not require recipients to copy the
    Corresponding Source along with the object code.  If the place to
    copy the object code is a network server, the Corresponding Source
    may be on a different server (operated by you or a third party)
    that supports equivalent copying facilities, provided you maintain
    clear directions next to the object code saying where to find the
    Corresponding Source.  Regardless of what server hosts the
    Corresponding Source, you remain obligated to ensure that it is
    available for as long as needed to satisfy these requirements.

    e) Convey the object code using peer-to-peer transmission, provided
    you inform other peers where the object code and Corresponding
    Source of the work are being offered to the general public at no
    charge under subsection 6d.

  A separable portion of the object code, whose source code is excluded
from the Corresponding Source as a System Library, need not be
included in conveying the object code work.

  A "User Product" is either (1) a "consumer product", which means any
tangible personal property which is normally used for personal, family,
or household purposes, or (2) anything designed or sold for incorporation
into a dwelling.  In determining whether a product is a consumer product,
doubtful cases shall be resolved in favor of coverage.  For a particular
product received by a particular user, "normally used" refers to a
typical or common use of that class of product, regardless of the status
of the particular user or of the way in which the particular user
actually uses, or expects or is expected to use, the product.  A product
is a consumer product regardless of whether the product has substantial
commercial, industrial or non-consumer uses, unless such uses represent
the only significant mode of use of the product.

  "Installation Information" for a User Product means any methods,
procedures, authorization keys, or other information required to install
and execute modified versions of a covered work in that User Product from
a modified version of its Corresponding Source.  The information must
suffice to ensure that the continued functioning of the modified object
code is in no case prevented or interfered with solely because
modification has been made.

  If you convey an object code work under this section in, or with, or
specifically for use in, a User Product, and the conveying occurs as
part of a transaction in which the right of possession and use of the
User Product is transferred to the recipient in perpetuity or for a
fixed term (regardless of how the transaction is characterized), the
Corresponding Source conveyed under this section must be accompanied
by the Installation Information.  But this requirement does not apply
if neither you nor any third party retains the ability to install
modified object code on the User Product (for example, the work has
been installed in ROM).

  The requirement to provide Installation Information does not include a
requirement to continue to provide support service, warranty, or updates
for a work that has been modified or installed by the recipient, or for
the User Product in which it has been modified or installed.  Access to a
network may be denied when the modification itself materially and
adversely affects the operation of the network or violates the rules and
protocols for communication across the network.

  Corresponding Source conveyed, and Installation Information provided,
in accord with this section must be in a format that is publicly
documented (and with an implementation available to the public in
source code form), and must require no special password or key for
unpacking, reading or copying.

  7. Additional Terms.

  "Additional permissions" are terms that supplement the terms of this
License by making exceptions from one or more of its conditions.
Additional permissions that are applicable to the entire Program shall
be treated as though they were included in this License, to the extent
that they are valid under applicable law.  If additional permissions
apply only to part of the Program, that part may be used separately
under those permissions, but the entire Program remains governed by
this License without regard to the additional permissions.

  When you convey a copy of a covered work, you may at your option
remove any additional permissions from that copy, or from any part of
it.  (Additional permissions may be written to require their own
removal in certain cases when you modify the work.)  You may place
additional permissions on material, added by you to a covered work,
for which you have or can give appropriate copyright permission.

  Notwithstanding any other provision of this License, for material you
add to a covered work, you may (if authorized by the copyright holders of
that material) supplement the terms of this License with terms:

    a) Disclaiming warranty or limiting liability differently from the
    terms of sections 15 and 16 of this License; or

    b) Requiring preservation of specified reasonable legal notices or
    author attributions in that material or in the Appropriate Legal
    Notices displayed by works containing it; or

    c) Prohibiting misrepresentation of the origin of that material, or
    requiring that modified versions of such material be marked in
    reasonable ways as different from the original version; or

    d) Limiting the use for publicity purposes of names of licensors or
    authors of the material; or

    e) Declining to grant rights under trademark law for use of some
    trade names, trademarks, or service marks; or

    f) Requiring indemnification of licensors and authors of that
    material by anyone who conveys the material (or modified versions of
    it) with contractual assumptions of liability to the recipient, for
    any liability that these contractual assumptions directly impose on
    those licensors and authors.

  All other non-permissive additional terms are considered "further
restrictions" within the meaning of section 10.  If the Program as you
received it, or any part of it, contains a notice stating that it is
governed by this License along with a term that is a further
restriction, you may remove that term.  If a license document contains
a further restriction but permits relicensing or conveying under this
License, you may add to a covered work material governed by the terms
of that license document, provided that the further restriction does
not survive such relicensing or conveying.

  If you add terms to a covered work in accord with this section, you
must place, in the relevant source files, a statement of the
additional terms that apply to those files, or a notice indicating
where to find the applicable terms.

  Additional terms, permissive or non-permissive, may be stated in the
form of a separately written license, or stated as exceptions;
the above requirements apply either way.

  8. Termination.

  You may not propagate or modify a covered work except as expressly
provided under this License.  Any attempt otherwise to propagate or
modify it is void, and will automatically terminate your rights under
this License (including any patent licenses granted under the third
paragraph of section 11).

  However, if you cease all violation of this License, then your
license from a particular copyright holder is reinstated (a)
provisionally, unless and until the copyright holder explicitly and
finally terminates your license, and (b) permanently, if the copyright
holder fails to notify you of the violation by some reasonable means
prior to 60 days after the cessation.

  Moreover, your license from a particular copyright holder is
reinstated permanently if the copyright holder notifies you of the
violation by some reasonable means, this is the first time you have
received notice of violation of this License (for any work) from that
copyright holder, and you cure the violation prior to 30 days after
your receipt of the notice.

  Termination of your rights under this section does not terminate the
licenses of parties who have received copies or rights from you under
this License.  If your rights have been terminated and not permanently
reinstated, you do not qualify to receive new licenses for the same
material under section 10.

  9. Acceptance Not Required for Having Copies.

  You are not required to accept this License in order to receive or
run a copy of the Program.  Ancillary propagation of a covered work
occurring solely as a consequence of using peer-to-peer transmission
to receive a copy likewise does not require acceptance.  However,
nothing other than this License grants you permission to propagate or
modify any covered work.  These actions infringe copyright if you do
not accept this License.  Therefore, by modifying or propagating a
covered work, you indicate your acceptance of this License to do so.

  10. Automatic Licensing of Downstream Recipients.

  Each time you convey a covered work, the recipient automatically
receives a license from the original licensors, to run, modify and
propagate that work, subject to this License.  You are not responsible
for enforcing compliance by third parties with this License.

  An "entity transaction" is a transaction transferring control of an
organization, or substantially all assets of one, or subdividing an
organization, or merging organizations.  If propagation of a covered
work results from an entity transaction, each party to that
transaction who receives a copy of the work also receives whatever
licenses to the work the party's predecessor in interest had or could
give under the previous paragraph, plus a right to possession of the
Corresponding Source of the work from the predecessor in interest, if
the predecessor has it or can get it with reasonable efforts.

  You may not impose any further restrictions on the exercise of the
rights granted or affirmed under this License.  For example, you may
not impose a license fee, royalty, or other charge for exercise of
rights granted under this License, and you may not initiate litigation
(including a cross-claim or counterclaim in a lawsuit) alleging that
any patent claim is infringed by making, using, selling, offering for
sale, or importing the Program or any portion of it.

  11. Patents.

  A "contributor" is a copyright holder who authorizes use under this
License of the Program or a work on which the Program is based.  The
work thus licensed is called the contributor's "contributor version".

  A contributor's "essential patent claims" are all patent claims
owned or controlled by the contributor, whether already acquired or
hereafter acquired, that would be infringed by some manner, permitted
by this License, of making, using, or selling its contributor version,
but do not include claims that would be infringed only as a
consequence of further modification of the contributor version.  For
purposes of this definition, "control" includes the right to grant
patent sublicenses in a manner consistent with the requirements of
this License.

  Each contributor grants you a non-exclusive, worldwide, royalty-free
patent license under the contributor's essential patent claims, to
make, use, sell, offer for sale, import and otherwise run, modify and
propagate the contents of its contributor version.

  In the following three paragraphs, a "patent license" is any express
agreement or commitment, however denominated, not to enforce a patent
(such as an express permission to practice a patent or covenant not to
sue for patent infringement).  To "grant" such a patent license to a
party means to make such an agreement or commitment not to enforce a
patent against the party.

  If you convey a covered work, knowingly relying on a patent license,
and the Corresponding Source of the work is not available for anyone
to copy, free of charge and under the terms of this License, through a
publicly available network server or other readily accessible means,
then you must either (1) cause the Corresponding Source to be so
available, or (2) arrange to deprive yourself of the benefit of the
patent license for this particular work, or (3) arrange, in a manner
consistent with the requirements of this License, to extend the patent
license to downstream recipients.  "Knowingly relying" means you have
actual knowledge that, but for the patent license, your conveying the
covered work in a country, or your recipient's use of the covered work
in a country, would infringe one or more identifiable patents in that
country that you have reason to believe are valid.

  If, pursuant to or in connection with a single transaction or
arrangement, you convey, or propagate by procuring conveyance of, a
covered work, and grant a patent license to some of the parties
receiving the covered work authorizing them to use, propagate, modify
or convey a specific copy of the covered work, then the patent license
you grant is automatically extended to all recipients of the covered
work and works based on it.

  A patent license is "discriminatory" if it does not include within
the scope of its coverage, prohibits the exercise of, or is
conditioned on the non-exercise of one or more of the rights that are
specifically granted under this License.  You may not convey a covered
work if you are a party to an arrangement with a third party that is
in the business of distributing software, under which you make payment
to the third party based on the extent of your activity of conveying
the work, and under which the third party grants, to any of the
parties who would receive the covered work from you, a discriminatory
patent license (a) in connection with copies of the covered work
conveyed by you (or copies made from those copies), or (b) primarily
for and in connection with specific products or compilations that
contain the covered work, unless you entered into that arrangement,
or that patent license was granted, prior to 28 March 2007.

  Nothing in this License shall be construed as excluding or limiting
any implied license or other defenses to infringement that may
otherwise be available to you under applicable patent law.

  12. No Surrender of Others' Freedom.

  If conditions are imposed on you (whether by court order, agreement or
otherwise) that contradict the conditions of this License, they do not
excuse you from the conditions of this License.  If you cannot convey a
covered work so as to satisfy simultaneously your obligations under this
License and any other pertinent obligations, then as a consequence you may
not convey it at all.  For example, if you agree to terms that obligate you
to collect a royalty for further conveying from those to whom you convey
the Program, the only way you could satisfy both those terms and this
License would be to refrain entirely from conveying the Program.

  13. Use with the GNU Affero General Public License.

  Notwithstanding any other provision of this License, you have
permission to link or combine any covered work with a work licensed
under version 3 of the GNU Affero General Public License into a single
combined work, and to convey the resulting work.  The terms of this
License will continue to apply to the part which is the covered work,
but the special requirements of the GNU Affero General Public License,
section 13, concerning interaction through a network will apply to the
combination as such.

  14. Revised Versions of this License.

  The Free Software Foundation may publish revised and/or new versions of
the GNU General Public License from time to time.  Such new versions will
be similar in spirit to the present version, but may differ in detail to
address new problems or concerns.

  Each version is given a distinguishing version number.  If the
Program specifies that a certain numbered version of the GNU General
Public License "or any later version" applies to it, you have the
option of following the terms and conditions either of that numbered
version or of any later version published by the Free Software
Foundation.  If the Program does not specify a version number of the
GNU General Public License, you may choose any version ever published
by the Free Software Foundation.

  If the Program specifies that a proxy can decide which future
versions of the GNU General Public License can be used, that proxy's
public statement of acceptance of a version permanently authorizes you
to choose that version for the Program.

  Later license versions may give you additional or different
permissions.  However, no additional obligations are imposed on any
author or copyright holder as a result of your choosing to follow a
later version.

  15. Disclaimer of Warranty.

  THERE IS NO WARRANTY FOR THE PROGRAM, TO THE EXTENT PERMITTED BY
APPLICABLE LAW.  EXCEPT WHEN OTHERWISE STATED IN WRITING THE COPYRIGHT
HOLDERS AND/OR OTHER PARTIES PROVIDE THE PROGRAM "AS IS" WITHOUT WARRANTY
OF ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE.  THE ENTIRE RISK AS TO THE QUALITY AND PERFORMANCE OF THE PROGRAM
IS WITH YOU.  SHOULD THE PROGRAM PROVE DEFECTIVE, YOU ASSUME THE COST OF
ALL NECESSARY SERVICING, REPAIR OR CORRECTION.

  16. Limitation of Liability.

  IN NO EVENT UNLESS REQUIRED BY APPLICABLE LAW OR AGREED TO IN WRITING
WILL ANY COPYRIGHT HOLDER, OR ANY OTHER PARTY WHO MODIFIES AND/OR CONVEYS
THE PROGRAM AS PERMITTED ABOVE, BE LIABLE TO YOU FOR DAMAGES, INCLUDING ANY
GENERAL, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES ARISING OUT OF THE
USE OR INABILITY TO USE THE PROGRAM (INCLUDING BUT NOT LIMITED TO LOSS OF
DATA OR DATA BEING RENDERED INACCURATE OR LOSSES SUSTAINED BY YOU OR THIRD
PARTIES OR A FAILURE OF THE PROGRAM TO OPERATE WITH ANY OTHER PROGRAMS),
EVEN IF SUCH HOLDER OR OTHER PARTY HAS BEEN ADVISED OF THE POSSIBILITY OF
SUCH DAMAGES.

  17. Interpretation of Sections 15 and 16.

  If the disclaimer of warranty and limitation of liability provided
above cannot be given local legal effect according to their terms,
reviewing courts shall apply local law that most closely approximates
an absolute waiver of all civil liability in connection with the
Program, unless a warranty or assumption of liability accompanies a
copy of the Program in return for a fee.

                     END OF TERMS AND CONDITIONS

            How to Apply These Terms to Your New Programs

  If you develop a new program, and you want it to be of the greatest
possible use to the public, the best way to achieve this is to make it
free software which everyone can redistribute and change under these terms.

  To do so, attach the following notices to the program.  It is safest
to attach them to the start of each source file to most effectively
state the exclusion of warranty; and each file should have at least
the "copyright" line and a pointer to where the full notice is found.

    <one line to give the program's name and a brief idea of what it does.>
    Copyright (C) <year>  <name of author>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.

Also add information on how to contact you by electronic and paper mail.

  If the program does terminal interaction, make it output a short
notice like this when it starts in an interactive mode:

    <program>  Copyright (C) <year>  <name of author>
    This program comes with ABSOLUTELY NO WARRANTY; for details type `show w'.
    This is free software, and you are welcome to redistribute it
    under certain conditions; type `show c' for details.

The hypothetical commands `show w' and `show c' should show the appropriate
parts of the General Public License.  Of course, your program's commands
might be different; for a GUI interface, you would use an "about box".

  You should also get your employer (if you work as a programmer) or school,
if any, to sign a "copyright disclaimer" for the program, if necessary.
For more information on this, and how to apply and follow the GNU GPL, see
<https://www.gnu.org/licenses/>.

  The GNU General Public License does not permit incorporating your program
into proprietary programs.  If your program is a subroutine library, you
may consider it more useful to permit linking proprietary applications with
the library.  If this is what you want to do, use the GNU Lesser General
Public License instead of this License.  But first, please read
<https://www.gnu.org/licenses/why-not-lgpl.html>.
)GPLTEXT";

void style_dialog_action_area(GtkWidget* dialog, guint border = 2, gint spacing = 6) {

    GtkWidget* action = gtk_dialog_get_action_area(GTK_DIALOG(dialog));
    if (action == nullptr) {
        return;
    }
    gtk_container_set_border_width(GTK_CONTAINER(action), border);
    gtk_widget_set_margin_top(action, 12);
    gtk_box_set_spacing(GTK_BOX(action), spacing);
    gtk_box_set_homogeneous(GTK_BOX(action), FALSE);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(action), GTK_BUTTONBOX_END);
    GList* children = gtk_container_get_children(GTK_CONTAINER(action));
    for (GList* node = children; node != nullptr; node = node->next) {
        GtkWidget* button = GTK_WIDGET(node->data);
        if (button != nullptr) {
            gtk_style_context_add_class(gtk_widget_get_style_context(button), "dialog-action-button");
            gtk_widget_set_margin_start(button, 0);
            gtk_widget_set_margin_end(button, 0);
            gtk_widget_set_margin_top(button, 6);
            gtk_widget_set_margin_bottom(button, 0);
        }
    }
    g_list_free(children);
}

} // namespace

GtkPlayerWindow::GtkPlayerWindow(std::size_t transport_buffer_ms)
    : transport_buffer_ms_(transport_buffer_ms),
      engine_(transport_buffer_ms),
      log_path_("pcm_transport.log") {
    load_preferences();
    stream_health_.load();
    repeat_enabled_ = false;
    Logger::instance().configure(logging_enabled_, log_path_, log_errors_only_);
    engine_.set_soft_volume_percent(soft_volume_percent_);
    engine_.set_soft_eq(bass_db_, treble_db_);
    engine_.set_pre_eq_headroom_tenths_db(pre_eq_headroom_tenths_db_);
    engine_.set_soft_eq_profile(bass_shelf_hz_, treble_shelf_hz_);
    engine_.set_deep_bass_enabled(deep_bass_enabled_);
    engine_.set_deep_bass_preset(deep_bass_internal_from_ui(deep_bass_preset_));
}

GtkPlayerWindow::~GtkPlayerWindow() {
    ui_closing_ = true;
    save_playlist_session();
    shutdown_stream_probe_worker();
    stop_ui_updates();
    cancel_pending_seek();
    mpris_service_.reset();
    stop_playback();
}

void GtkPlayerWindow::show() {
    app_ = gtk_application_new("org.berestov.pcmtransport", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app_, "activate", G_CALLBACK(GtkPlayerWindow::on_activate), this);
    g_application_run(G_APPLICATION(app_), 0, nullptr);
    g_object_unref(app_);
    app_ = nullptr;
}

void GtkPlayerWindow::on_activate(GtkApplication* app, gpointer user_data) {
    static_cast<GtkPlayerWindow*>(user_data)->build_ui(app);
}

void GtkPlayerWindow::build_ui(GtkApplication* app) {
    window_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window_), "PCM Transport v0.9.109");
    gtk_window_set_default_size(GTK_WINDOW(window_), 900, 660);
    gtk_container_set_border_width(GTK_CONTAINER(window_), 16);

    GtkCssProvider* provider = gtk_css_provider_new();
    const char* css =
        "window { background: #2b2f34; }"
        ".rack { background: linear-gradient(to bottom, #9aa0a6, #8f949a); border-radius: 12px; padding: 18px; }"
        ".display { background: #0f2413; color: #9cff9c; border-radius: 8px; padding: 12px; font-family: monospace; }"
        ".display-track { font-size: 18px; }"
        ".display-time { font-size: 24px; font-weight: bold; }"
        ".transport-button { min-height: 42px; min-width: 46px; font-weight: bold; padding: 2px 8px; }"
        ".transport-button-thin { min-height: 19px; min-width: 86px; font-weight: bold; padding: 1px 8px; }"
        ".transport-icon { font-size: 18px; color: #25313a; }"
        "treeview.view:selected, treeview.view:selected:focus { background-color: #6f7780; color: #ffffff; }"
        "treeview.view:selected:hover { background-color: #78818a; color: #ffffff; }"
        "#playlist-view.view:selected, #playlist-view.view:selected:focus { background-color: transparent; color: inherit; }"
        "#playlist-view.view:selected:hover { background-color: transparent; color: inherit; }"
        "#playlist-view row:selected { background-color: transparent; color: inherit; }"
        "#playlist-view row:nth-child(even):selected { background-color: transparent; color: inherit; }"
        "notebook > header > tabs > tab:checked { box-shadow: inset 0 -3px #6f7780; }"
        "scale trough highlight { background-color: #6f7780; background-image: none; border-color: #6f7780; }"
        "scale slider { background-color: #eeeeee; background-image: none; border-color: #9a9a9a; }"
        "scale slider:hover { background-color: #ffffff; border-color: #7c838a; }"
        "checkbutton check { background-image: none; border-color: #9a9a9a; }"
        "checkbutton check:checked { background-color: #6f7780; background-image: none; border-color: #6f7780; color: #ffffff; }"
        "checkbutton check:checked:hover { background-color: #78818a; border-color: #78818a; }"
        ".small-label { color: #e8ebee; }"
        ".display-badge { color: #ff5959; border: 1px solid #ff5959; border-radius: 3px; padding: 2px 6px; font-size: 10px; font-weight: bold; margin-left: 6px; }"
        ".meter-caption { color: #d6d8db; font-size: 10px; font-weight: bold; }"
        ".section-label { font-weight: bold; }"
        ".progress-area { min-height: 14px; }"
        ".dialog-action-button { padding: 4px 0; margin: 0; min-height: 0; min-width: 0; }"
        ".alsa-probe-cell { border: 1px solid #9aa1a8; padding: 4px 8px; color: #25313a; background-color: #f7f7f7; }"
        ".alsa-probe-header { font-weight: bold; color: #25313a; background-color: #e6e9ec; }"
        ".alsa-probe-ok { color: #1a7f37; font-weight: bold; }"
        ".alsa-probe-fail { color: #a12a2a; font-weight: bold; }";
    gtk_css_provider_load_from_data(provider, css, -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(window_), outer);
    gtk_style_context_add_class(gtk_widget_get_style_context(outer), "rack");

    GtkWidget* display_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(display_box), "display");
    gtk_box_pack_start(GTK_BOX(outer), display_box, FALSE, FALSE, 0);

    GtkWidget* display_left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_box_pack_start(GTK_BOX(display_box), display_left, TRUE, TRUE, 0);
    gtk_widget_set_hexpand(display_left, TRUE);

    display_track_ = gtk_label_new("Track: --");
    display_time_ = gtk_label_new("00:00 / 00:00");
    display_status_ = gtk_label_new("Open FLAC or CUE files");
    display_source_ = gtk_label_new("Device: default");
    display_path_ = gtk_label_new("Path: --");
    display_reserve_ = gtk_label_new(" ");

    gtk_style_context_add_class(gtk_widget_get_style_context(display_track_), "display-track");
    gtk_style_context_add_class(gtk_widget_get_style_context(display_time_), "display-time");

    GtkWidget* left_labels[] = {display_track_, display_time_, display_status_, display_source_};
    for (GtkWidget* label : left_labels) {
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
        gtk_box_pack_start(GTK_BOX(display_left), label, FALSE, FALSE, 0);
    }

    gtk_label_set_xalign(GTK_LABEL(display_path_), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(display_path_), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(display_path_), TRUE);
    gtk_widget_set_hexpand(display_path_, TRUE);
    gtk_box_pack_start(GTK_BOX(display_left), display_path_, FALSE, FALSE, 0);

    gtk_label_set_xalign(GTK_LABEL(display_reserve_), 0.0f);
    gtk_label_set_single_line_mode(GTK_LABEL(display_reserve_), TRUE);
    gtk_widget_set_hexpand(display_reserve_, TRUE);
    gtk_box_pack_start(GTK_BOX(display_left), display_reserve_, FALSE, FALSE, 0);
    GtkWidget* display_right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(display_right, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(display_box), display_right, FALSE, FALSE, 0);

    GtkWidget* meter_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_halign(meter_frame, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(display_right), meter_frame, FALSE, FALSE, 0);

    display_meter_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(display_meter_, 46, 108);
    gtk_widget_set_margin_top(display_meter_, 8);
    gtk_widget_set_halign(display_meter_, GTK_ALIGN_END);
    gtk_widget_set_valign(display_meter_, GTK_ALIGN_FILL);
    g_signal_connect(display_meter_, "draw", G_CALLBACK(GtkPlayerWindow::on_meter_draw), this);
    gtk_box_pack_start(GTK_BOX(meter_frame), display_meter_, TRUE, TRUE, 0);

    GtkWidget* meter_caption = gtk_label_new("LEVEL");
    gtk_widget_set_size_request(meter_caption, 46, -1);
    gtk_widget_set_halign(meter_caption, GTK_ALIGN_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(meter_caption), "meter-caption");
    gtk_box_pack_end(GTK_BOX(meter_frame), meter_caption, FALSE, FALSE, 0);

    GtkWidget* badge_wrap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(badge_wrap, TRUE);
    gtk_box_pack_start(GTK_BOX(display_left), badge_wrap, FALSE, FALSE, 0);

    GtkWidget* clip_slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(clip_slot, TRUE);
    gtk_box_pack_start(GTK_BOX(badge_wrap), clip_slot, TRUE, TRUE, 0);

    badge_clip_ = gtk_label_new("CLIP");
    gtk_style_context_add_class(gtk_widget_get_style_context(badge_clip_), "display-badge");
    gtk_widget_set_halign(badge_clip_, GTK_ALIGN_END);
    gtk_widget_set_size_request(badge_clip_, 84, -1);
    gtk_label_set_xalign(GTK_LABEL(badge_clip_), 0.5f);
    gtk_box_pack_end(GTK_BOX(clip_slot), badge_clip_, FALSE, FALSE, 0);
    gtk_widget_set_margin_end(badge_clip_, 10);
    gtk_widget_set_opacity(badge_clip_, 0.0);

    badge_box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_set_spacing(GTK_BOX(badge_box_), 6);
    gtk_widget_set_halign(badge_box_, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(badge_wrap), badge_box_, FALSE, FALSE, 0);

    badge_lossless_ = gtk_label_new("LOSSLESS");
    badge_redbook_ = gtk_label_new("RED BOOK PCM");
    badge_native_ = gtk_label_new("NATIVE DECODE");
    badge_dsp_ = gtk_label_new("DSP");
    badge_repeat_ = gtk_label_new("REPEAT");
    GtkWidget* badges[] = {badge_lossless_, badge_redbook_, badge_native_, badge_dsp_, badge_repeat_};
    for (GtkWidget* badge : badges) {
        gtk_style_context_add_class(gtk_widget_get_style_context(badge), "display-badge");
        gtk_box_pack_end(GTK_BOX(badge_box_), badge, FALSE, FALSE, 0);
        gtk_widget_set_opacity(badge, 0.0);
    }

    progress_bar_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(progress_bar_, -1, 14);
    gtk_widget_add_events(progress_bar_, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(progress_bar_, "draw", G_CALLBACK(GtkPlayerWindow::on_progress_draw), this);
    g_signal_connect(progress_bar_, "button-press-event", G_CALLBACK(GtkPlayerWindow::on_progress_button_press), this);
    gtk_box_pack_start(GTK_BOX(outer), progress_bar_, FALSE, FALSE, 0);

    controls_wrap_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_hexpand(controls_wrap_, TRUE);
    gtk_box_pack_start(GTK_BOX(outer), controls_wrap_, FALSE, FALSE, 0);

    GtkWidget* controls_transport = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(controls_transport), FALSE);
    GtkWidget* controls_text = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(controls_text), 4);
    gtk_grid_set_column_spacing(GTK_GRID(controls_text), 6);
    gtk_box_pack_start(GTK_BOX(controls_wrap_), controls_transport, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(controls_wrap_), controls_text, FALSE, FALSE, 0);

    btn_prev_ = create_symbolic_button("media-skip-backward-symbolic", nullptr, "<<");
    btn_play_ = create_symbolic_button("media-playback-start-symbolic", nullptr, ">");
    btn_pause_ = create_symbolic_button("media-playback-pause-symbolic", nullptr, "||");
    btn_stop_ = create_symbolic_button("media-playback-stop-symbolic", nullptr, "[]");
    btn_next_ = create_symbolic_button("media-skip-forward-symbolic", nullptr, ">>");
    btn_open_ = create_symbolic_button("media-eject-symbolic", "document-open-symbolic", "OPEN");
    btn_repeat_ = create_symbolic_button("media-playlist-repeat-symbolic", "view-refresh-symbolic", "Repeat");
    btn_settings_ = gtk_button_new_with_label("Settings");
    btn_eq_ = gtk_button_new_with_label("DSP Studio");
    btn_alsamixer_ = gtk_button_new_with_label("alsamixer");
    btn_about_ = gtk_button_new_with_label("About");

    GtkWidget* transport_buttons_all[] = {btn_prev_, btn_play_, btn_pause_, btn_stop_, btn_next_, btn_open_, btn_repeat_};
    for (GtkWidget* button : transport_buttons_all) {
        gtk_style_context_add_class(gtk_widget_get_style_context(button), "transport-button");
        gtk_style_context_add_class(gtk_widget_get_style_context(button), "transport-icon");
        gtk_box_pack_start(GTK_BOX(controls_transport), button, FALSE, FALSE, 0);
    }

    GtkWidget* text_buttons[] = {btn_settings_, btn_eq_, btn_alsamixer_, btn_about_};
    for (GtkWidget* button : text_buttons) {
        gtk_style_context_add_class(gtk_widget_get_style_context(button), "transport-button");
        gtk_style_context_add_class(gtk_widget_get_style_context(button), "transport-button-thin");
    }
    gtk_grid_attach(GTK_GRID(controls_text), btn_settings_, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(controls_text), btn_alsamixer_, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(controls_text), btn_eq_, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(controls_text), btn_about_, 1, 1, 1, 1);
    gtk_widget_set_tooltip_text(btn_open_, "Open files");
    gtk_widget_set_tooltip_text(btn_repeat_, "Repeat playlist");
    gtk_widget_set_tooltip_text(btn_settings_, "Settings");
    gtk_widget_set_tooltip_text(btn_eq_, "DSP Studio");
    gtk_widget_set_tooltip_text(btn_alsamixer_, "Open alsamixer");
    gtk_widget_set_tooltip_text(btn_about_, "About");
    GtkWidget* icon_buttons[] = {btn_open_, btn_repeat_};
    for (GtkWidget* button : icon_buttons) {
        gtk_widget_set_size_request(button, 62, 42);
    }
    GtkWidget* text_buttons_sized[] = {btn_settings_, btn_eq_, btn_alsamixer_, btn_about_};
    for (GtkWidget* button : text_buttons_sized) {
        gtk_widget_set_size_request(button, 116, 19);
    }
    GtkWidget* transport_buttons[] = {btn_prev_, btn_play_, btn_pause_, btn_stop_, btn_next_};
    for (GtkWidget* button : transport_buttons) {
        gtk_widget_set_size_request(button, 62, 42);
    }

    GtkWidget* content_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(outer), content_row, TRUE, TRUE, 0);

    GtkWidget* playlist_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(content_row), playlist_panel, TRUE, TRUE, 0);

    playlist_search_entry_ = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(playlist_search_entry_), "Search title or artist");
    gtk_widget_set_margin_bottom(playlist_search_entry_, 2);
    gtk_box_pack_start(GTK_BOX(playlist_panel), playlist_search_entry_, FALSE, FALSE, 0);

    GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    playlist_scrolled_ = scrolled;
    gtk_widget_set_size_request(scrolled, -1, 285);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(playlist_panel), scrolled, TRUE, TRUE, 0);

    GtkWidget* softvol_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(softvol_box, 58, -1);
    gtk_box_pack_end(GTK_BOX(content_row), softvol_box, FALSE, FALSE, 0);

    soft_volume_scale_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(soft_volume_scale_, 52, 285);
    gtk_widget_add_events(soft_volume_scale_, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON1_MOTION_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(soft_volume_scale_, "draw", G_CALLBACK(GtkPlayerWindow::on_softvol_draw), this);
    g_signal_connect(soft_volume_scale_, "button-press-event", G_CALLBACK(GtkPlayerWindow::on_softvol_button_press), this);
    g_signal_connect(soft_volume_scale_, "motion-notify-event", G_CALLBACK(GtkPlayerWindow::on_softvol_motion_notify), this);
    g_signal_connect(soft_volume_scale_, "button-release-event", G_CALLBACK(GtkPlayerWindow::on_softvol_button_release), this);
    gtk_box_pack_start(GTK_BOX(softvol_box), soft_volume_scale_, TRUE, TRUE, 0);

    GtkWidget* softvol_caption = gtk_label_new("SOFT VOL");
    gtk_widget_set_halign(softvol_caption, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(softvol_box), softvol_caption, FALSE, FALSE, 0);
    GtkWidget* softvol_hint = gtk_label_new("100%=OFF");
    gtk_widget_set_halign(softvol_hint, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(softvol_box), softvol_hint, FALSE, FALSE, 0);

    playlist_store_ = gtk_list_store_new(COL_COUNT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
    playlist_filter_ = GTK_TREE_MODEL_FILTER(gtk_tree_model_filter_new(GTK_TREE_MODEL(playlist_store_), nullptr));
    gtk_tree_model_filter_set_visible_func(playlist_filter_,
                                           GtkPlayerWindow::on_playlist_filter_visible,
                                           this,
                                           nullptr);
    playlist_view_ = gtk_tree_view_new_with_model(GTK_TREE_MODEL(playlist_filter_));
    gtk_widget_set_name(playlist_view_, "playlist-view");
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(playlist_view_)), GTK_SELECTION_BROWSE);
    gtk_container_add(GTK_CONTAINER(scrolled), playlist_view_);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(playlist_view_), TRUE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(playlist_view_), FALSE);
    gtk_widget_add_events(playlist_view_, GDK_KEY_PRESS_MASK);
    g_signal_connect(playlist_search_entry_, "changed", G_CALLBACK(GtkPlayerWindow::on_playlist_search_changed), this);

    GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "xpad", 6, "ypad", 2, nullptr);
    GtkTreeViewColumn* col_track = gtk_tree_view_column_new_with_attributes("#", renderer, "text", COL_TRACKNO, nullptr);
    gtk_tree_view_column_set_resizable(col_track, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(playlist_view_), col_track);

    renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "xpad", 6, "ypad", 2, nullptr);
    GtkTreeViewColumn* col_artist = gtk_tree_view_column_new_with_attributes("Artist", renderer, "text", COL_ARTIST, nullptr);
    gtk_tree_view_column_set_resizable(col_artist, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(playlist_view_), col_artist);

    renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "xpad", 6, "ypad", 2, nullptr);
    GtkTreeViewColumn* col_title = gtk_tree_view_column_new_with_attributes("Title", renderer, "text", COL_TITLE, nullptr);
    gtk_tree_view_column_set_expand(col_title, TRUE);
    gtk_tree_view_column_set_resizable(col_title, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(playlist_view_), col_title);

    renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "xpad", 6, "ypad", 2, nullptr);
    GtkTreeViewColumn* col_source = gtk_tree_view_column_new_with_attributes("Source", renderer, "text", COL_SOURCE, nullptr);
    gtk_tree_view_column_set_resizable(col_source, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(playlist_view_), col_source);

    gtk_tree_view_append_column(GTK_TREE_VIEW(playlist_view_), col_source);

    set_playlist_column_cell_styler(col_track, COL_TRACKNO);
    set_playlist_column_cell_styler(col_artist, COL_ARTIST);
    set_playlist_column_cell_styler(col_title, COL_TITLE);
    set_playlist_column_cell_styler(col_source, COL_SOURCE);

    GtkTreeSelection* playlist_selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(playlist_view_));
    g_signal_connect(playlist_selection, "changed", G_CALLBACK(on_playlist_selection_changed), nullptr);

    g_signal_connect(playlist_view_, "key-press-event", G_CALLBACK(GtkPlayerWindow::on_playlist_view_key_press), this);

    playlist_typeahead_popup_ = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_decorated(GTK_WINDOW(playlist_typeahead_popup_), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(playlist_typeahead_popup_), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(playlist_typeahead_popup_), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(playlist_typeahead_popup_), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(playlist_typeahead_popup_), GDK_WINDOW_TYPE_HINT_TOOLTIP);
    gtk_window_set_transient_for(GTK_WINDOW(playlist_typeahead_popup_), GTK_WINDOW(window_));
    gtk_container_set_border_width(GTK_CONTAINER(playlist_typeahead_popup_), 0);

    playlist_typeahead_entry_ = gtk_entry_new();
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(playlist_typeahead_entry_),
                                      GTK_ENTRY_ICON_PRIMARY,
                                      "edit-find-symbolic");
    gtk_editable_set_editable(GTK_EDITABLE(playlist_typeahead_entry_), FALSE);
    gtk_widget_set_can_focus(playlist_typeahead_entry_, FALSE);
    gtk_container_add(GTK_CONTAINER(playlist_typeahead_popup_), playlist_typeahead_entry_);

    g_signal_connect(btn_open_, "clicked", G_CALLBACK(GtkPlayerWindow::on_open_clicked), this);
    g_signal_connect(btn_play_, "clicked", G_CALLBACK(GtkPlayerWindow::on_play_clicked), this);
    g_signal_connect(btn_pause_, "clicked", G_CALLBACK(GtkPlayerWindow::on_pause_clicked), this);
    g_signal_connect(btn_stop_, "clicked", G_CALLBACK(GtkPlayerWindow::on_stop_clicked), this);
    g_signal_connect(btn_prev_, "clicked", G_CALLBACK(GtkPlayerWindow::on_prev_clicked), this);
    g_signal_connect(btn_next_, "clicked", G_CALLBACK(GtkPlayerWindow::on_next_clicked), this);
    g_signal_connect(btn_repeat_, "clicked", G_CALLBACK(GtkPlayerWindow::on_repeat_clicked), this);
    g_signal_connect(btn_settings_, "clicked", G_CALLBACK(GtkPlayerWindow::on_settings_clicked), this);
    g_signal_connect(btn_eq_, "clicked", G_CALLBACK(GtkPlayerWindow::on_eq_clicked), this);
    g_signal_connect(btn_alsamixer_, "clicked", G_CALLBACK(GtkPlayerWindow::on_open_alsamixer_clicked), this);
    g_signal_connect(btn_about_, "clicked", G_CALLBACK(GtkPlayerWindow::on_about_clicked), this);
    g_signal_connect(playlist_view_, "row-activated", G_CALLBACK(GtkPlayerWindow::on_playlist_row_activated), this);
    g_signal_connect(playlist_view_, "focus-in-event", G_CALLBACK(GtkPlayerWindow::on_playlist_focus_in), this);
    g_signal_connect(window_, "delete-event", G_CALLBACK(GtkPlayerWindow::on_window_delete_event), this);
    g_signal_connect(window_, "destroy", G_CALLBACK(GtkPlayerWindow::on_window_destroy), this);
    refresh_device_list();
    refresh_dsp_info_for_current_device();
    refresh_display();
    ui_timer_id_ = g_timeout_add(kUiRefreshIntervalMs, GtkPlayerWindow::on_timer_tick, this);
    setup_media_keys(app);
    setup_mpris();

    gtk_widget_show_all(window_);
    restore_playlist_session();
}


gboolean GtkPlayerWindow::on_timer_tick(gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr) {
        return G_SOURCE_REMOVE;
    }
    if (self->ui_closing_) {
        self->ui_timer_id_ = 0;
        return G_SOURCE_REMOVE;
    }

    const PlaybackStatusSnapshot status = self->engine_.snapshot();
    self->update_gapless_chain_track_from_status(status);
    self->update_stream_health_from_playback(status);

    if (self->stream_reconnect_pending_ &&
        std::chrono::steady_clock::now() >= self->stream_reconnect_due_) {
        self->stream_reconnect_pending_ = false;
        const std::size_t index = self->stream_reconnect_target_index_;
        if (index < self->playlist_.size() && self->playlist_[index].is_stream) {
            struct StreamReconnectRequest {
                GtkPlayerWindow* self = nullptr;
                std::size_t index = 0;
            };
            auto* request = new StreamReconnectRequest{self, index};
            g_idle_add(+[](gpointer user_data) -> gboolean {
                std::unique_ptr<StreamReconnectRequest> request(static_cast<StreamReconnectRequest*>(user_data));
                if (request->self != nullptr && !request->self->ui_closing_) {
                    request->self->play_track_index(request->index);
                }
                return G_SOURCE_REMOVE;
            }, request);
        }
    }

    bool transport_finished = !self->track_switch_in_progress_ && status.finished && !status.playing;
    if (!transport_finished && !self->track_switch_in_progress_ && !self->playlist_.empty() && self->current_track_index_ < self->playlist_.size()) {
        const PlaylistEntry& current = self->playlist_[self->current_track_index_];
        const std::uint64_t track_length = self->track_length_samples(current);
        if (track_length > 0 && !status.playing && status.current_samples_per_channel >= track_length) {
            transport_finished = true;
        }
    }

    if (transport_finished && !self->finish_handled_) {
        self->finish_handled_ = true;

        if (!self->playlist_.empty()) {
            const std::size_t finished_index = self->current_track_index_;
            bool should_advance = false;
            std::size_t next_index = finished_index;

            if (finished_index + 1 < self->playlist_.size()) {
                next_index = finished_index + 1;
                should_advance = true;
            } else if (self->repeat_enabled_) {
                next_index = 0;
                should_advance = true;
            }

            if (finished_index < self->playlist_.size() && self->playlist_[finished_index].is_stream) {
                should_advance = false;
                if (self->stream_reconnect_attempts_ < kMaxStreamReconnectAttempts) {
                    self->schedule_stream_reconnect(finished_index);
                } else {
                    self->stream_status_override_ = "Stream unavailable";
                    self->stream_reconnect_attempts_ = 0;
                    self->stream_reconnect_target_index_ = static_cast<std::size_t>(-1);
                    self->note_stream_broken(self->playlist_[finished_index].audio_file_path, "Stream unavailable");
                }
            }

            if (should_advance) {
                self->play_track_index(next_index);
            } else {
                self->select_playlist_row(self->current_track_index_);
            }
        }
    } else if (!transport_finished) {
        self->finish_handled_ = false;
    }

    ++self->ui_refresh_tick_;
    const bool update_meter = self->level_meter_enabled_;
    const unsigned int progress_refresh_ticks = self->progress_blink_enabled_ ? kUiProgressRefreshTicks : kUiTextRefreshTicks;
    const bool update_progress = (self->ui_refresh_tick_ % progress_refresh_ticks) == 0;
    const bool update_text = (self->ui_refresh_tick_ % kUiTextRefreshTicks) == 0;
    self->refresh_display(update_text, update_progress, update_meter);

    return G_SOURCE_CONTINUE;
}

gboolean GtkPlayerWindow::on_window_delete_event(GtkWidget*, GdkEvent*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self != nullptr) {
        self->save_playlist_session();
        self->ui_closing_ = true;
        self->stop_ui_updates();
        self->cancel_pending_seek();
    }
    return FALSE;
}

void GtkPlayerWindow::on_window_destroy(GtkWidget*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self != nullptr) {
        self->ui_closing_ = true;
        self->stop_ui_updates();
        self->cancel_pending_seek();
        self->window_ = nullptr;
        self->display_track_ = nullptr;
        self->display_time_ = nullptr;
        self->display_status_ = nullptr;
        self->display_source_ = nullptr;
        self->display_path_ = nullptr;
        self->display_mode_ = nullptr;
        self->display_meter_ = nullptr;
        self->badge_clip_ = nullptr;
        self->progress_bar_ = nullptr;
        self->badge_box_ = nullptr;
        self->badge_lossless_ = nullptr;
        self->badge_redbook_ = nullptr;
        self->badge_native_ = nullptr;
        self->badge_dsp_ = nullptr;
        self->badge_repeat_ = nullptr;
        self->btn_prev_ = nullptr;
        self->btn_play_ = nullptr;
        self->btn_pause_ = nullptr;
        self->btn_stop_ = nullptr;
        self->btn_next_ = nullptr;
        self->btn_open_ = nullptr;
        self->btn_repeat_ = nullptr;
        self->btn_settings_ = nullptr;
        self->btn_alsamixer_ = nullptr;
        self->btn_about_ = nullptr;
        self->btn_eq_ = nullptr;
        self->controls_wrap_ = nullptr;
        self->soft_volume_scale_ = nullptr;
        self->playlist_view_ = nullptr;
        self->playlist_filter_ = nullptr;
        self->playlist_store_ = nullptr;
        self->playlist_search_entry_ = nullptr;
        self->playlist_typeahead_popup_ = nullptr;
        self->playlist_typeahead_entry_ = nullptr;
        self->playlist_scrolled_ = nullptr;
    }
}

void GtkPlayerWindow::stop_ui_updates() {
    if (ui_timer_id_ != 0) {
        g_source_remove(ui_timer_id_);
        ui_timer_id_ = 0;
    }
    cancel_playlist_metadata_probe();
}

void GtkPlayerWindow::cancel_pending_seek() {
    pending_seek_valid_ = false;
    if (pending_seek_timer_id_ != 0) {
        g_source_remove(pending_seek_timer_id_);
        pending_seek_timer_id_ = 0;
    }
}

void GtkPlayerWindow::on_open_clicked(GtkButton*, gpointer user_data) {
    static_cast<GtkPlayerWindow*>(user_data)->open_file_dialog();
}

void GtkPlayerWindow::on_play_clicked(GtkButton*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    self->start_current_track(true);
    self->notify_mpris_state_changed();
}

void GtkPlayerWindow::on_pause_clicked(GtkButton*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self->engine_.is_paused()) {
        self->engine_.resume();
    } else {
        self->engine_.pause();
    }
    self->notify_mpris_state_changed();
}

void GtkPlayerWindow::on_stop_clicked(GtkButton*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    self->stop_playback();
    self->notify_mpris_state_changed();
}

void GtkPlayerWindow::on_prev_clicked(GtkButton*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self->playlist_.empty()) {
        return;
    }
    if (self->current_track_index_ > 0) {
        self->play_track_index(self->current_track_index_ - 1);
    } else {
        self->play_track_index(0);
    }
    self->notify_mpris_state_changed();
}

void GtkPlayerWindow::on_next_clicked(GtkButton*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self->playlist_.empty()) {
        return;
    }
    if (self->current_track_index_ + 1 < self->playlist_.size()) {
        self->play_track_index(self->current_track_index_ + 1);
    }
    self->notify_mpris_state_changed();
}

void GtkPlayerWindow::on_settings_clicked(GtkButton*, gpointer user_data) {
    static_cast<GtkPlayerWindow*>(user_data)->open_settings_dialog();
}

void GtkPlayerWindow::on_about_clicked(GtkButton*, gpointer user_data) {
    static_cast<GtkPlayerWindow*>(user_data)->open_about_dialog();
}

void GtkPlayerWindow::on_eq_clicked(GtkButton*, gpointer user_data) {
    static_cast<GtkPlayerWindow*>(user_data)->open_eq_dialog();
}

void GtkPlayerWindow::on_open_alsamixer_clicked(GtkButton*, gpointer user_data) {
    static_cast<GtkPlayerWindow*>(user_data)->open_alsamixer_for_current_device();
}

void GtkPlayerWindow::on_repeat_clicked(GtkButton*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    self->repeat_enabled_ = !self->repeat_enabled_;
    self->mpris_loop_status_ = self->repeat_enabled_ ? "Playlist" : "None";
    self->save_preferences();
    self->refresh_display();
    self->notify_mpris_state_changed();
}


gboolean GtkPlayerWindow::on_softvol_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    GtkAllocation alloc{};
    gtk_widget_get_allocation(widget, &alloc);
    const double width = alloc.width;
    const double height = alloc.height;
    const double value = std::max(0.0, std::min(100.0, static_cast<double>(self->soft_volume_percent_)));

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    const double rail_w = 8.0;
    const double rail_x = std::floor(width * 0.50) - rail_w / 2.0;
    const double rail_y = 8.0;
    const double rail_h = height - 16.0;
    const double rail_r = 2.6;

    cairo_new_path(cr);
    cairo_move_to(cr, rail_x, rail_y);
    cairo_line_to(cr, rail_x + rail_w, rail_y);
    cairo_line_to(cr, rail_x + rail_w, rail_y + rail_h - rail_r);
    cairo_arc(cr, rail_x + rail_w - rail_r, rail_y + rail_h - rail_r, rail_r, 0.0, M_PI / 2.0);
    cairo_line_to(cr, rail_x + rail_r, rail_y + rail_h);
    cairo_arc(cr, rail_x + rail_r, rail_y + rail_h - rail_r, rail_r, M_PI / 2.0, M_PI);
    cairo_line_to(cr, rail_x, rail_y);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.18, 0.18, 0.19);
    cairo_fill(cr);

    cairo_new_path(cr);
    cairo_move_to(cr, rail_x + 1.0, rail_y + 1.0);
    cairo_line_to(cr, rail_x + rail_w - 1.0, rail_y + 1.0);
    cairo_line_to(cr, rail_x + rail_w - 1.0, rail_y + rail_h - rail_r - 1.0);
    cairo_arc(cr, rail_x + rail_w - 1.0 - rail_r, rail_y + rail_h - 1.0 - rail_r, rail_r, 0.0, M_PI / 2.0);
    cairo_line_to(cr, rail_x + 1.0 + rail_r, rail_y + rail_h - 1.0);
    cairo_arc(cr, rail_x + 1.0 + rail_r, rail_y + rail_h - 1.0 - rail_r, rail_r, M_PI / 2.0, M_PI);
    cairo_line_to(cr, rail_x + 1.0, rail_y + 1.0);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.08, 0.08, 0.09);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
    cairo_rectangle(cr, rail_x + 1.0, rail_y + 1.0, 1.0, rail_h - 3.0);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.34);
    cairo_set_line_width(cr, 1.0);
    const int tick_count = 16;
    for (int i = 0; i <= tick_count; ++i) {
        const double y = rail_y + rail_h * i / static_cast<double>(tick_count);
        const bool major = (i % 2 == 0);
        const double tick = major ? 8.0 : 5.0;
        cairo_move_to(cr, rail_x + rail_w + 5.0, y);
        cairo_line_to(cr, rail_x + rail_w + 5.0 + tick, y);
        cairo_stroke(cr);
    }

    const double knob_h = 28.0;
    const double knob_w = 40.0;
    const double knob_x = rail_x - (knob_w - rail_w) / 2.0;
    const double knob_y = rail_y + (100.0 - value) * (rail_h - knob_h) / 100.0;
    const double r = 5.0;

    cairo_new_path(cr);
    cairo_arc(cr, knob_x + knob_w - r, knob_y + r, r, -M_PI / 2.0, 0.0);
    cairo_arc(cr, knob_x + knob_w - r, knob_y + knob_h - r, r, 0.0, M_PI / 2.0);
    cairo_arc(cr, knob_x + r, knob_y + knob_h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, knob_x + r, knob_y + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);

    cairo_pattern_t* pat = cairo_pattern_create_linear(0, knob_y, 0, knob_y + knob_h);
    cairo_pattern_add_color_stop_rgb(pat, 0.0, 1.0, 0.33, 0.31);
    cairo_pattern_add_color_stop_rgb(pat, 0.18, 1.0, 0.18, 0.16);
    cairo_pattern_add_color_stop_rgb(pat, 0.55, 0.93, 0.05, 0.05);
    cairo_pattern_add_color_stop_rgb(pat, 1.0, 0.76, 0.02, 0.02);
    cairo_set_source(cr, pat);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(pat);

    cairo_set_source_rgb(cr, 0.42, 0.02, 0.02);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.34);
    cairo_rectangle(cr, knob_x + 5.0, knob_y + 5.0, knob_w - 10.0, 2.0);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.42);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, knob_x + 7.0, knob_y + knob_h * 0.52);
    cairo_line_to(cr, knob_x + knob_w - 7.0, knob_y + knob_h * 0.52);
    cairo_stroke(cr);

    return FALSE;
}

gboolean GtkPlayerWindow::on_softvol_button_press(GtkWidget* widget, GdkEventButton* event, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (event == nullptr || event->button != 1) {
        return FALSE;
    }
    GtkAllocation alloc{};
    gtk_widget_get_allocation(widget, &alloc);
    const double track_y = 10.0;
    const double track_h = alloc.height - 20.0;
    const double knob_h = 30.0;
    double y = std::max(track_y, std::min(track_y + track_h - knob_h, static_cast<double>(event->y) - knob_h * 0.5));
    const double ratio = 1.0 - ((y - track_y) / std::max(1.0, track_h - knob_h));
    self->soft_volume_percent_ = static_cast<int>(std::round(std::max(0.0, std::min(1.0, ratio)) * 100.0));
    self->engine_.set_soft_volume_percent(self->soft_volume_percent_);
    self->save_preferences();
    self->refresh_display();
    gtk_widget_queue_draw(widget);
    self->softvol_dragging_ = true;
    return TRUE;
}

gboolean GtkPlayerWindow::on_softvol_motion_notify(GtkWidget* widget, GdkEventMotion* event, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (!self->softvol_dragging_ || event == nullptr || (event->state & GDK_BUTTON1_MASK) == 0) {
        return FALSE;
    }
    GtkAllocation alloc{};
    gtk_widget_get_allocation(widget, &alloc);
    const double track_y = 10.0;
    const double track_h = alloc.height - 20.0;
    const double knob_h = 30.0;
    double y = std::max(track_y, std::min(track_y + track_h - knob_h, static_cast<double>(event->y) - knob_h * 0.5));
    const double ratio = 1.0 - ((y - track_y) / std::max(1.0, track_h - knob_h));
    self->soft_volume_percent_ = static_cast<int>(std::round(std::max(0.0, std::min(1.0, ratio)) * 100.0));
    self->engine_.set_soft_volume_percent(self->soft_volume_percent_);
    self->save_preferences();
    self->refresh_display();
    gtk_widget_queue_draw(widget);
    return TRUE;
}

gboolean GtkPlayerWindow::on_softvol_button_release(GtkWidget*, GdkEventButton* event, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (event != nullptr && event->button == 1) {
        self->softvol_dragging_ = false;
        self->notify_mpris_state_changed();
        return TRUE;
    }
    return FALSE;
}


gboolean GtkPlayerWindow::on_time_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data) {
    (void)widget;
    (void)cr;
    (void)user_data;
    return FALSE;
}

gboolean GtkPlayerWindow::on_meter_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    const double value = std::max(0.0, std::min(1.18, self->meter_level_));
    GtkAllocation alloc{};
    gtk_widget_get_allocation(widget, &alloc);
    const double width = alloc.width;
    const double height = alloc.height;

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    const double meter_y = 10.0;
    const double meter_w = width - 4.0;
    const double meter_h = height - 14.0;
    const double meter_x = width - meter_w - 2.0;

    cairo_set_source_rgb(cr, 0.15, 0.22, 0.20);
    cairo_rectangle(cr, meter_x - 1.0, meter_y - 1.0, meter_w + 2.0, meter_h + 2.0);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.07, 0.14, 0.12);
    cairo_rectangle(cr, meter_x, meter_y, meter_w, meter_h);
    cairo_fill(cr);

    const int segments = 21;
    const int red_segments = 3;
    const double gap = 2.0;
    const double usable_h = meter_h - 6.0;
    const double seg_h = (usable_h - gap * (segments - 1)) / segments;
    const double fill_ratio = value / 1.18;
    const double top_fill_y = meter_y + meter_h - (fill_ratio * meter_h);

    for (int i = 0; i < segments; ++i) {
        const double y = meter_y + meter_h - 3.0 - (i + 1) * seg_h - i * gap;
        const bool on = y + seg_h >= top_fill_y;
        const bool in_red_zone = i >= (segments - red_segments);
        if (on) {
            if (in_red_zone) {
                cairo_set_source_rgb(cr, 1.0, 0.34, 0.28);
            } else {
                cairo_set_source_rgb(cr, 0.6118, 1.0, 0.6118);
            }
        } else {
            if (in_red_zone) {
                cairo_set_source_rgb(cr, 0.28, 0.16, 0.16);
            } else {
                cairo_set_source_rgb(cr, 0.22, 0.30, 0.27);
            }
        }
        cairo_rectangle(cr, meter_x + 2.0, y, meter_w - 4.0, seg_h);
        cairo_fill(cr);
    }

    return FALSE;
}

gboolean GtkPlayerWindow::on_progress_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    GtkAllocation alloc{};
    gtk_widget_get_allocation(widget, &alloc);
    const double width = alloc.width;
    const double height = alloc.height;

    cairo_set_source_rgb(cr, 0.10, 0.18, 0.12);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.22, 0.32, 0.24);
    cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
    cairo_stroke(cr);

    const double ratio = std::max(0.0, std::min(1.0, self->display_progress_ratio_));

    const int segments = 56;
    const double gap = 1.0;
    const double inner_x = 2.0;
    const double inner_y = 2.0;
    const double inner_w = width - 4.0;
    const double inner_h = height - 4.0;
    const double seg_w = (inner_w - gap * (segments - 1)) / segments;
    const int filled = static_cast<int>(ratio * segments);
    const bool blink_on = self->progress_blink_enabled_ && ((g_get_monotonic_time() / 600000) % 2 == 0);
    const int blink_index = std::min(segments - 1, filled);

    for (int i = 0; i < segments; ++i) {
        const double x = inner_x + i * (seg_w + gap);
        if (i < filled) {
            cairo_set_source_rgb(cr, 0.62, 0.62, 0.62);
        } else if (i == blink_index && ratio > 0.0 && ratio < 1.0 && blink_on) {
            cairo_set_source_rgb(cr, 0.70, 0.70, 0.70);
        } else {
            cairo_set_source_rgb(cr, 0.24, 0.30, 0.26);
        }
        cairo_rectangle(cr, x, inner_y, seg_w, inner_h);
        cairo_fill(cr);
    }
    return FALSE;
}

gboolean GtkPlayerWindow::on_progress_button_press(GtkWidget* widget, GdkEventButton* event, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (event == nullptr || event->button != 1 || self->playlist_.empty() || self->current_track_index_ >= self->playlist_.size()) {
        return FALSE;
    }
    GtkAllocation alloc{};
    gtk_widget_get_allocation(widget, &alloc);
    if (alloc.width <= 0) {
        return FALSE;
    }
    const PlaylistEntry& track = self->playlist_[self->current_track_index_];
    const std::uint64_t track_length = self->track_length_samples(track);
    if (track_length == 0) {
        return FALSE;
    }
    const double ratio = std::max(0.0, std::min(1.0, event->x / static_cast<double>(alloc.width)));
    const std::uint64_t target = static_cast<std::uint64_t>(ratio * static_cast<double>(track_length));

    if (track.processed_by_ffmpeg || track.resampled || track.bitdepth_converted) {
        self->pending_seek_index_ = self->current_track_index_;
        self->pending_seek_offset_ = target;
        self->pending_seek_valid_ = true;
        if (self->pending_seek_timer_id_ == 0) {
            self->pending_seek_timer_id_ = g_timeout_add(140, GtkPlayerWindow::on_pending_seek_timer, self);
        }
    } else {
        self->play_track_index_at_offset(self->current_track_index_, target);
    }
    return TRUE;
}

gboolean GtkPlayerWindow::on_pending_seek_timer(gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || self->ui_closing_) {
        return G_SOURCE_REMOVE;
    }
    self->pending_seek_timer_id_ = 0;
    if (!self->pending_seek_valid_) {
        return G_SOURCE_REMOVE;
    }
    const std::size_t index = self->pending_seek_index_;
    const std::uint64_t offset = self->pending_seek_offset_;
    self->pending_seek_valid_ = false;
    if (self->track_switch_in_progress_) {
        return G_SOURCE_REMOVE;
    }
    if (index < self->playlist_.size()) {
        self->play_track_index_at_offset(index, offset);
    }
    return G_SOURCE_REMOVE;
}

void GtkPlayerWindow::on_playlist_row_activated(GtkTreeView*, GtkTreePath*, GtkTreeViewColumn*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || self->playlist_.empty()) {
        return;
    }
    // Enter activates the keyboard cursor, which can lag behind the highlighted row after session restore.
    self->update_playlist_selection_from_ui();
    self->sync_playlist_cursor_to_selection();
    self->play_track_index(self->current_track_index_);
}

gboolean GtkPlayerWindow::on_playlist_focus_in(GtkWidget*, GdkEventFocus*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || self->ui_closing_) {
        return FALSE;
    }
    self->sync_playlist_cursor_to_selection();
    return FALSE;
}

void GtkPlayerWindow::on_playlist_search_changed(GtkEditable* editable, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    const gchar* text = gtk_entry_get_text(GTK_ENTRY(editable));
    self->playlist_filter_text_ = text != nullptr ? utf8_casefold_copy(text) : std::string();
    if (self->playlist_filter_ != nullptr) {
        gtk_tree_model_filter_refilter(self->playlist_filter_);
    }
}

gboolean GtkPlayerWindow::on_playlist_filter_visible(GtkTreeModel* model, GtkTreeIter* iter, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self->playlist_filter_text_.empty()) {
        return TRUE;
    }
    gchar* artist = nullptr;
    gchar* title = nullptr;
    gtk_tree_model_get(model, iter, COL_ARTIST, &artist, COL_TITLE, &title, -1);
    const bool match = utf8_contains_casefold(artist != nullptr ? artist : "", self->playlist_filter_text_) ||
                       utf8_contains_casefold(title != nullptr ? title : "", self->playlist_filter_text_);
    g_free(artist);
    g_free(title);
    return match ? TRUE : FALSE;
}

void GtkPlayerWindow::update_playlist_typeahead_popup() {
    if (playlist_typeahead_popup_ == nullptr || playlist_typeahead_entry_ == nullptr || playlist_scrolled_ == nullptr) {
        return;
    }
    if (playlist_typeahead_text_.empty()) {
        gtk_widget_hide(playlist_typeahead_popup_);
        return;
    }

    gtk_entry_set_text(GTK_ENTRY(playlist_typeahead_entry_), playlist_typeahead_text_.c_str());
    gtk_widget_show_all(playlist_typeahead_popup_);

    if (!gtk_widget_get_realized(playlist_scrolled_)) {
        return;
    }

    GtkAllocation allocation{};
    gtk_widget_get_allocation(playlist_scrolled_, &allocation);

    gint anchor_x = 0;
    gint anchor_y = 0;
    GdkWindow* anchor_window = gtk_widget_get_window(playlist_scrolled_);
    if (anchor_window == nullptr) {
        return;
    }
    gdk_window_get_origin(anchor_window, &anchor_x, &anchor_y);

    GdkRectangle anchor_rect{};
    anchor_rect.x = anchor_x;
    anchor_rect.y = anchor_y;
    anchor_rect.width = allocation.width;
    anchor_rect.height = allocation.height;

    if (!gtk_widget_get_realized(playlist_typeahead_popup_)) {
        gtk_widget_realize(playlist_typeahead_popup_);
    }
    GdkWindow* popup_window = gtk_widget_get_window(playlist_typeahead_popup_);
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

void GtkPlayerWindow::reset_playlist_typeahead() {
    playlist_typeahead_text_.clear();
    if (playlist_typeahead_timeout_id_ != 0) {
        g_source_remove(playlist_typeahead_timeout_id_);
        playlist_typeahead_timeout_id_ = 0;
    }
    update_playlist_typeahead_popup();
}

gboolean GtkPlayerWindow::on_playlist_typeahead_clear_timeout(gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr) {
        return G_SOURCE_REMOVE;
    }
    self->playlist_typeahead_timeout_id_ = 0;
    self->playlist_typeahead_text_.clear();
    self->update_playlist_typeahead_popup();
    return G_SOURCE_REMOVE;
}

void GtkPlayerWindow::apply_playlist_typeahead_selection() {
    if (playlist_typeahead_text_.empty() || playlist_view_ == nullptr) {
        return;
    }

    const std::string key_folded = utf8_casefold_copy(playlist_typeahead_text_);
    GtkTreeModel* model = gtk_tree_view_get_model(GTK_TREE_VIEW(playlist_view_));
    if (model == nullptr) {
        return;
    }

    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_first(model, &iter)) {
        return;
    }

    do {
        gchar* artist = nullptr;
        gchar* title = nullptr;
        gtk_tree_model_get(model, &iter, COL_ARTIST, &artist, COL_TITLE, &title, -1);
        const bool match = playlist_row_matches_typeahead(artist, title, key_folded);
        g_free(artist);
        g_free(title);
        if (!match) {
            continue;
        }

        GtkTreePath* path = gtk_tree_model_get_path(model, &iter);
        GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(playlist_view_));
        gtk_tree_selection_unselect_all(selection);
        gtk_tree_selection_select_path(selection, path);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(playlist_view_), path, nullptr, FALSE);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(playlist_view_), path, nullptr, TRUE, 0.5f, 0.0f);
        gtk_tree_path_free(path);
        return;
    } while (gtk_tree_model_iter_next(model, &iter));
}

gboolean GtkPlayerWindow::on_playlist_view_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || event == nullptr) {
        return FALSE;
    }
    if (gtk_widget_is_focus(self->playlist_search_entry_)) {
        return FALSE;
    }

    if (event->keyval == GDK_KEY_Escape) {
        if (!self->playlist_typeahead_text_.empty()) {
            self->reset_playlist_typeahead();
            return TRUE;
        }
        return FALSE;
    }

    if (event->keyval == GDK_KEY_BackSpace) {
        if (self->playlist_typeahead_text_.empty()) {
            return FALSE;
        }
        const char* text = self->playlist_typeahead_text_.c_str();
        const char* prev = g_utf8_find_prev_char(text, text + self->playlist_typeahead_text_.size());
        if (prev != nullptr) {
            self->playlist_typeahead_text_.resize(static_cast<std::size_t>(prev - text));
        } else {
            self->playlist_typeahead_text_.clear();
        }
        if (self->playlist_typeahead_timeout_id_ != 0) {
            g_source_remove(self->playlist_typeahead_timeout_id_);
        }
        self->playlist_typeahead_timeout_id_ = g_timeout_add(1500, GtkPlayerWindow::on_playlist_typeahead_clear_timeout, self);
        self->update_playlist_typeahead_popup();
        self->apply_playlist_typeahead_selection();
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
    self->playlist_typeahead_text_.append(buffer, static_cast<std::size_t>(written));
    if (self->playlist_typeahead_timeout_id_ != 0) {
        g_source_remove(self->playlist_typeahead_timeout_id_);
    }
    self->playlist_typeahead_timeout_id_ = g_timeout_add(1500, GtkPlayerWindow::on_playlist_typeahead_clear_timeout, self);
    self->update_playlist_typeahead_popup();
    self->apply_playlist_typeahead_selection();
    (void)widget;
    return TRUE;
}

void GtkPlayerWindow::clear_playlist_search() {
    reset_playlist_typeahead();
    playlist_filter_text_.clear();
    if (playlist_search_entry_ != nullptr) {
        gtk_entry_set_text(GTK_ENTRY(playlist_search_entry_), "");
    }
    if (playlist_filter_ != nullptr) {
        gtk_tree_model_filter_refilter(playlist_filter_);
    }
}

bool GtkPlayerWindow::find_playlist_view_path_for_index(std::size_t index, GtkTreePath** out_path) const {
    if (out_path == nullptr || playlist_view_ == nullptr) {
        return false;
    }
    *out_path = nullptr;
    GtkTreeModel* model = gtk_tree_view_get_model(GTK_TREE_VIEW(playlist_view_));
    if (model == nullptr) {
        return false;
    }
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    while (valid) {
        int row_index = -1;
        gtk_tree_model_get(model, &iter, COL_INDEX, &row_index, -1);
        if (row_index >= 0 && static_cast<std::size_t>(row_index) == index) {
            *out_path = gtk_tree_model_get_path(model, &iter);
            return true;
        }
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return false;
}

std::uint32_t GtkPlayerWindow::target_sample_rate_for(std::uint32_t source_rate) const {
    for (std::size_t i = 0; i < resample_rules_.size(); ++i) {
        if (resample_rules_[i].from_rate == source_rate) {
            return resample_rules_[i].to_rate;
        }
    }
    return 0;
}

std::uint16_t GtkPlayerWindow::target_bits_for(std::uint16_t source_bits) const {
    for (std::size_t i = 0; i < bitdepth_rules_.size(); ++i) {
        if (bitdepth_rules_[i].from_bits == source_bits) {
            return bitdepth_rules_[i].to_bits;
        }
    }
    return 0;
}

void GtkPlayerWindow::refresh_playlist_processing_metadata() {
    for (auto& entry : playlist_) {
        entry.decoded_format.sample_rate = entry.source_sample_rate;
        entry.decoded_format.bits_per_sample = entry.source_bits_per_sample;
        entry.end_sample = entry.start_sample == 0 ? entry.end_sample : entry.end_sample;
        const std::uint32_t target_rate = target_sample_rate_for(entry.source_sample_rate);
        const std::uint16_t target_bits = target_bits_for(entry.source_bits_per_sample);
        entry.resampled = (target_rate > 0 && target_rate != entry.source_sample_rate);
        entry.resampled_from_rate = entry.resampled ? entry.source_sample_rate : 0;
        entry.bitdepth_converted = (target_bits > 0 && target_bits != entry.source_bits_per_sample);
        entry.native_decode = lower_extension(entry.audio_file_path) == ".flac" && !entry.resampled && !entry.bitdepth_converted;
        entry.processed_by_ffmpeg = (!entry.native_decode);
        const std::uint64_t source_length = entry.start_sample == 0 ? (entry.end_sample > 0 ? entry.end_sample : 0) : (entry.end_sample - entry.start_sample);
        if (entry.resampled && entry.source_sample_rate > 0) {
            const std::uint64_t new_length = static_cast<std::uint64_t>(std::llround(static_cast<double>(source_length) * static_cast<double>(target_rate) / static_cast<double>(entry.source_sample_rate)));
            entry.decoded_format.sample_rate = target_rate;
            if (entry.start_sample == 0) entry.end_sample = new_length;
            else entry.end_sample = entry.start_sample + new_length;
        }
        if (entry.bitdepth_converted) entry.decoded_format.bits_per_sample = target_bits;
    }
}


GaplessTrackSpec GtkPlayerWindow::gapless_spec_for_entry(const PlaylistEntry& entry) const {
    GaplessTrackSpec spec;
    spec.path = entry.audio_file_path;
    spec.format = entry.decoded_format;
    spec.start_sample = entry.start_sample;
    spec.end_sample = entry.end_sample;
    const std::string ext = lower_extension(entry.audio_file_path);
    spec.native_flac = (ext == ".flac" && entry.native_decode);
    spec.range_limited = entry.start_sample > 0 || entry.end_sample > entry.start_sample;
    spec.forced_output_bits_per_sample = entry.bitdepth_converted ? entry.decoded_format.bits_per_sample : 0;
    spec.resample_quality = resample_quality_;
    spec.bitdepth_quality = bitdepth_quality_;
    if (!spec.native_flac) {
        spec.forced_output_sample_rate = entry.resampled ? entry.decoded_format.sample_rate : 0;
        spec.known_external_info.format = entry.decoded_format;
        spec.known_external_info.source_format = entry.decoded_format;
        spec.known_external_info.source_format.sample_rate = entry.source_sample_rate > 0 ? entry.source_sample_rate : entry.decoded_format.sample_rate;
        spec.known_external_info.source_format.bits_per_sample = entry.source_bits_per_sample > 0 ? entry.source_bits_per_sample : entry.decoded_format.bits_per_sample;
        spec.known_external_info.total_samples_per_channel = entry.cue_album_end_sample > 0 ? entry.cue_album_end_sample : entry.end_sample;
        spec.known_external_info.source_total_samples_per_channel = spec.known_external_info.total_samples_per_channel;
        spec.known_external_info.codec_name = entry.codec_name;
        spec.known_external_info.lossless = entry.lossless_source;
        spec.known_external_info.raw_aac = (ext == ".aac");
        spec.known_external_info.duration_reliable = entry.end_sample > entry.start_sample;
        spec.has_known_external_info = true;
    }
    return spec;
}

bool GtkPlayerWindow::entries_share_playback_format(const PlaylistEntry& a, const PlaylistEntry& b) const {
    return a.decoded_format.sample_rate == b.decoded_format.sample_rate &&
           a.decoded_format.channels == b.decoded_format.channels &&
           a.decoded_format.bits_per_sample == b.decoded_format.bits_per_sample;
}

std::uint64_t GtkPlayerWindow::track_length_samples(const PlaylistEntry& entry) const {
    if (entry.is_stream) {
        return 0;
    }
    if (entry.end_sample > entry.start_sample) {
        return entry.end_sample - entry.start_sample;
    }
    return entry.end_sample;
}

std::size_t GtkPlayerWindow::cue_chain_end_index(std::size_t index) const {
    if (index >= playlist_.size() || !playlist_[index].cue_track) {
        return index + 1;
    }
    std::size_t end = index + 1;
    while (end < playlist_.size()) {
        const PlaylistEntry& prev = playlist_[end - 1];
        const PlaylistEntry& next = playlist_[end];
        if (!next.cue_track || prev.audio_file_path != next.audio_file_path) {
            break;
        }
        if (!entries_share_playback_format(prev, next)) {
            break;
        }
        if (prev.end_sample != next.start_sample) {
            break;
        }
        ++end;
    }
    return end;
}

std::size_t GtkPlayerWindow::file_chain_end_index(std::size_t index) const {
    if (index >= playlist_.size() || playlist_[index].cue_track || playlist_[index].is_stream) {
        return index + 1;
    }
    std::size_t end = index + 1;
    while (end < playlist_.size()) {
        const PlaylistEntry& prev = playlist_[end - 1];
        const PlaylistEntry& next = playlist_[end];
        if (next.cue_track || !entries_share_playback_format(prev, next)) {
            break;
        }
        const bool lossless_chain = prev.lossless_source && next.lossless_source;
        const std::string prev_family = codec_family_from_name(prev.codec_name, prev.audio_file_path);
        const std::string next_family = codec_family_from_name(next.codec_name, next.audio_file_path);
        const bool prev_raw_aac = lower_extension(prev.audio_file_path) == ".aac" && prev_family == "aac" &&
                                  prev.lossy_source && prev.end_sample > prev.start_sample;
        const bool next_raw_aac = lower_extension(next.audio_file_path) == ".aac" && next_family == "aac" &&
                                  next.lossy_source && next.end_sample > next.start_sample;
        const bool raw_aac_chain = prev_raw_aac && next_raw_aac;
        const bool lossy_chain = raw_aac_chain ||
                                 (prev.lossy_source && next.lossy_source &&
                                  prev_family == next_family && lossy_gapless_family(prev_family));
        if (!lossless_chain && !lossy_chain) {
            break;
        }
        ++end;
    }
    return end;
}

void GtkPlayerWindow::activate_gapless_chain(std::size_t start_index, std::size_t end_index) {
    gapless_chain_offsets_.clear();
    gapless_chain_total_samples_ = 0;
    if (start_index >= playlist_.size() || end_index <= start_index || end_index > playlist_.size()) {
        clear_gapless_chain();
        return;
    }
    for (std::size_t i = start_index; i < end_index; ++i) {
        gapless_chain_offsets_.push_back(gapless_chain_total_samples_);
        gapless_chain_total_samples_ += track_length_samples(playlist_[i]);
    }
    gapless_chain_active_ = (end_index > start_index + 1);
    gapless_chain_start_index_ = start_index;
    gapless_chain_end_index_ = end_index;
    if (!gapless_chain_active_) {
        gapless_chain_offsets_.clear();
        gapless_chain_total_samples_ = 0;
    }
}

void GtkPlayerWindow::clear_gapless_chain() {
    gapless_chain_active_ = false;
    gapless_chain_start_index_ = 0;
    gapless_chain_end_index_ = 0;
    gapless_chain_offsets_.clear();
    gapless_chain_total_samples_ = 0;
}

void GtkPlayerWindow::update_gapless_chain_track_from_status(const PlaybackStatusSnapshot& status) {
    if (!gapless_chain_active_ || gapless_chain_offsets_.empty() || gapless_chain_end_index_ <= gapless_chain_start_index_) {
        return;
    }
    const std::uint64_t pos = std::min(status.current_samples_per_channel, gapless_chain_total_samples_);
    std::size_t active = gapless_chain_start_index_;
    for (std::size_t i = 0; i < gapless_chain_offsets_.size(); ++i) {
        const std::uint64_t begin = gapless_chain_offsets_[i];
        const std::uint64_t end = (i + 1 < gapless_chain_offsets_.size()) ? gapless_chain_offsets_[i + 1] : gapless_chain_total_samples_;
        if (pos >= begin && (pos < end || i + 1 == gapless_chain_offsets_.size())) {
            active = gapless_chain_start_index_ + i;
            break;
        }
    }
    if (active != current_track_index_ && active < playlist_.size()) {
        current_track_index_ = active;
        select_playlist_row(current_track_index_);
        mark_mpris_track_changed();
    }
}

std::uint64_t GtkPlayerWindow::current_track_position_from_status(const PlaybackStatusSnapshot& status) const {
    std::uint64_t pos = status.current_samples_per_channel;
    if (gapless_chain_active_ && current_track_index_ >= gapless_chain_start_index_ && current_track_index_ < gapless_chain_end_index_) {
        const std::size_t rel = current_track_index_ - gapless_chain_start_index_;
        if (rel < gapless_chain_offsets_.size()) {
            const std::uint64_t offset = gapless_chain_offsets_[rel];
            pos = pos > offset ? pos - offset : 0;
        }
    }
    if (!playlist_.empty() && current_track_index_ < playlist_.size()) {
        const std::uint64_t length = track_length_samples(playlist_[current_track_index_]);
        if (length > 0 && pos > length) {
            pos = length;
        }
    }
    return pos;
}

std::unique_ptr<IAudioDecoder> GtkPlayerWindow::create_decoder_for_entry(const PlaylistEntry& entry, bool) const {
    const std::string ext = lower_extension(entry.audio_file_path);
    const std::uint32_t source_rate = entry.source_sample_rate > 0 ? entry.source_sample_rate : entry.decoded_format.sample_rate;
    const std::uint16_t source_bits = entry.source_bits_per_sample > 0 ? entry.source_bits_per_sample : entry.decoded_format.bits_per_sample;
    const std::uint32_t target_rate = target_sample_rate_for(source_rate);
    const std::uint16_t target_bits = target_bits_for(source_bits);
    const bool resample_needed = (target_rate > 0 && target_rate != source_rate);
    const bool bitdepth_needed = (target_bits > 0 && target_bits != source_bits);
    if (!entry.is_stream && ext == ".flac" && entry.native_decode && !resample_needed && !bitdepth_needed) {
        return std::unique_ptr<IAudioDecoder>(new FlacStreamDecoder());
    }
    if (entry.is_stream || ExternalAudioDecoder::is_stream_uri(entry.audio_file_path)) {
        std::unique_ptr<ExternalAudioDecoder> decoder;
        if (resample_needed || bitdepth_needed) {
            decoder.reset(new ExternalAudioDecoder(target_rate, target_bits, resample_quality_, bitdepth_quality_));
        } else {
            decoder.reset(new ExternalAudioDecoder());
        }
        ExternalAudioInfo known;
        known.format = entry.decoded_format;
        known.source_format = entry.decoded_format;
        known.source_format.sample_rate = entry.source_sample_rate > 0 ? entry.source_sample_rate : entry.decoded_format.sample_rate;
        known.source_format.bits_per_sample = entry.source_bits_per_sample > 0 ? entry.source_bits_per_sample : entry.decoded_format.bits_per_sample;
        known.total_samples_per_channel = 0;
        known.source_total_samples_per_channel = 0;
        known.duration_reliable = false;
        known.codec_name = entry.codec_name;
        known.lossless = entry.lossless_source;
        known.live_format_probed = entry.stream_format_probed;
        decoder->set_known_info(known);
        return std::unique_ptr<IAudioDecoder>(decoder.release());
    }
    if (ExternalAudioDecoder::looks_supported(entry.audio_file_path)) {
        std::unique_ptr<ExternalAudioDecoder> decoder;
        if (resample_needed || bitdepth_needed) {
            decoder.reset(new ExternalAudioDecoder(target_rate, target_bits, resample_quality_, bitdepth_quality_));
        } else {
            decoder.reset(new ExternalAudioDecoder());
        }
        ExternalAudioInfo known;
        known.format = entry.decoded_format;
        known.source_format = entry.decoded_format;
        known.source_format.sample_rate = entry.source_sample_rate > 0 ? entry.source_sample_rate : entry.decoded_format.sample_rate;
        known.source_format.bits_per_sample = entry.source_bits_per_sample > 0 ? entry.source_bits_per_sample : entry.decoded_format.bits_per_sample;
        known.total_samples_per_channel = entry.cue_album_end_sample > 0 ? entry.cue_album_end_sample : entry.end_sample;
        known.source_total_samples_per_channel = known.total_samples_per_channel;
        known.codec_name = entry.codec_name;
        known.lossless = entry.lossless_source;
        decoder->set_known_info(known);
        return std::unique_ptr<IAudioDecoder>(decoder.release());
    }
    throw std::runtime_error("Unsupported audio file type: " + ext);
}

void GtkPlayerWindow::invalidate_normalized_playlist() {
    for (std::size_t i = 0; i < playlist_.size(); ++i) {
        playlist_[i].normalized_pcm.reset();
        playlist_[i].normalized_format = AudioFormat{};
        playlist_[i].normalization_matches_current = false;
    }
}

void GtkPlayerWindow::normalize_playlist(GtkWidget* progress_bar) {
    (void)progress_bar;
}

std::uint32_t GtkPlayerWindow::current_tone_control_sample_rate() const {
    if (current_track_index_ < playlist_.size()) {
        const PlaylistEntry& entry = playlist_[current_track_index_];
        if (entry.source_sample_rate > 0) {
            const std::uint32_t target_rate = target_sample_rate_for(entry.source_sample_rate);
            if (target_rate > 0) {
                return target_rate;
            }
        }
        if (entry.decoded_format.sample_rate > 0) {
            return entry.decoded_format.sample_rate;
        }
        if (entry.normalized_format.sample_rate > 0) {
            return entry.normalized_format.sample_rate;
        }
    }
    return 44100;
}

int GtkPlayerWindow::effective_pre_eq_headroom_tenths_db() const {
    return std::max(0, std::min(kUiPreEqHeadroomMaxTenthsDb, pre_eq_headroom_tenths_db_));
}

int GtkPlayerWindow::compute_auto_pre_eq_headroom_tenths_db() const {
    if (bass_db_ == 0 && treble_db_ == 0 && !deep_bass_enabled_) {
        return 0;
    }
    double reserve_db = tone::estimate_total_processing_max_gain_db(
        current_tone_control_sample_rate(),
        bass_db_,
        bass_shelf_hz_,
        treble_db_,
        treble_shelf_hz_,
        deep_bass_enabled_,
        static_cast<tone::DeepBassPreset>(deep_bass_internal_from_ui(deep_bass_preset_)),
        deep_bass_dsp_amount_from_ui(deep_bass_amount_));
    if (reserve_db > 0.0001) {
        reserve_db += kUiHeadroomSafetyMarginDb;
    } else {
        reserve_db = 0.0;
    }
    int tenths = static_cast<int>(std::lround(std::max(0.0, reserve_db) * 10.0));
    if (tenths < 1) tenths = 0;
    if (tenths > kUiPreEqHeadroomMaxTenthsDb) tenths = kUiPreEqHeadroomMaxTenthsDb;
    return tenths;
}

void GtkPlayerWindow::apply_auto_pre_eq_headroom(bool save_preferences_after) {
    pre_eq_headroom_tenths_db_ = compute_auto_pre_eq_headroom_tenths_db();
    engine_.set_pre_eq_headroom_tenths_db(pre_eq_headroom_tenths_db_);
    if (save_preferences_after) save_preferences();
}

void GtkPlayerWindow::draw_tone_response_graph(cairo_t* cr, int width, int height) const {
    cairo_set_source_rgb(cr, 0.09, 0.09, 0.10);
    cairo_paint(cr);

    const double left = 24.0;
    const double top = 12.0;
    const double right = 24.0;
    const double bottom = 28.0;
    const double graph_w = std::max(1.0, static_cast<double>(width) - left - right);
    const double graph_h = std::max(1.0, static_cast<double>(height) - top - bottom);
    const double min_hz = 20.0;
    const double max_hz = 20000.0;
    const double min_db = -15.0;
    const double max_db = 15.0;
    auto map_x = [&](double hz) {
        const double t = (std::log(hz) - std::log(min_hz)) / (std::log(max_hz) - std::log(min_hz));
        return left + std::max(0.0, std::min(1.0, t)) * graph_w;
    };
    auto map_y = [&](double db) {
        const double t = (db - min_db) / (max_db - min_db);
        return top + (1.0 - std::max(0.0, std::min(1.0, t))) * graph_h;
    };

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);

    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
    const double db_lines[] = {-12.0, -6.0, 0.0, 6.0, 12.0};
    for (double db : db_lines) {
        const double y = std::floor(map_y(db)) + 0.5;
        cairo_move_to(cr, left, y);
        cairo_line_to(cr, left + graph_w, y);
        cairo_stroke(cr);
    }

    const struct Tick { double hz; const char* label; } hz_ticks[] = {
        {20.0, "20"}, {50.0, "50"}, {100.0, "100"}, {200.0, "200"}, {500.0, "500"},
        {1000.0, "1k"}, {2000.0, "2k"}, {5000.0, "5k"}, {10000.0, "10k"}, {20000.0, "20k"}
    };
    for (const auto& tick : hz_ticks) {
        const double x = std::floor(map_x(tick.hz)) + 0.5;
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, tick.hz == 1000.0 || tick.hz == 10000.0 ? 0.16 : 0.10);
        cairo_move_to(cr, x, top);
        cairo_line_to(cr, x, top + graph_h);
        cairo_stroke(cr);
        cairo_move_to(cr, x, top + graph_h);
        cairo_line_to(cr, x, top + graph_h + 3.0);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, 0.88, 0.90, 0.92, 0.78);
        cairo_text_extents_t ext{};
        cairo_text_extents(cr, tick.label, &ext);
        cairo_move_to(cr, x - (ext.width * 0.5) - ext.x_bearing, top + graph_h + 14.0);
        cairo_show_text(cr, tick.label);
    }

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
    cairo_rectangle(cr, std::floor(left) + 0.5, std::floor(top) + 0.5, std::floor(graph_w) + 0.0, std::floor(graph_h) + 0.0);
    cairo_stroke(cr);

    cairo_set_line_width(cr, 1.2);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.26);
    const double y0 = std::floor(map_y(0.0)) + 0.5;
    cairo_move_to(cr, left, y0);
    cairo_line_to(cr, left + graph_w, y0);
    cairo_stroke(cr);

    cairo_set_line_width(cr, 2.0);
    cairo_set_source_rgb(cr, 0.98, 0.38, 0.18);
    bool started = false;
    const std::uint32_t sample_rate = current_tone_control_sample_rate();
    for (int i = 0; i < static_cast<int>(graph_w); ++i) {
        const double t = graph_w > 1.0 ? static_cast<double>(i) / (graph_w - 1.0) : 0.0;
        const double hz = std::exp(std::log(min_hz) + (std::log(max_hz) - std::log(min_hz)) * t);
        const double db = tone::cascaded_shelf_response_db(sample_rate, bass_db_, bass_shelf_hz_, treble_db_, treble_shelf_hz_, hz);
        const double x = left + t * graph_w;
        const double y = map_y(db);
        if (!started) { cairo_move_to(cr, x, y); started = true; }
        else { cairo_line_to(cr, x, y); }
    }
    cairo_stroke(cr);
}

void GtkPlayerWindow::update_clip_indicator(bool clip_detected, std::uint32_t clipped_samples) {
    if (clip_detected) {
        clip_hold_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(kClipIndicatorHoldMs);
        clip_hold_samples_ = clipped_samples;
    }

    if (badge_clip_ == nullptr) {
        return;
    }

    const bool hold_active = std::chrono::steady_clock::now() < clip_hold_until_;
    if (hold_active) {
        if (clip_hold_samples_ > 0) {
            const std::string text = std::string("CLIP ") + std::to_string(clip_hold_samples_);
            set_label_text_if_changed(badge_clip_, text);
        } else {
            set_label_text_if_changed(badge_clip_, "CLIP");
        }
        set_widget_opacity_if_changed(badge_clip_, 1.0);
    } else {
        set_label_text_if_changed(badge_clip_, "CLIP");
        set_widget_opacity_if_changed(badge_clip_, 0.0);
        clip_hold_samples_ = 0;
    }
}

ExternalAudioInfo GtkPlayerWindow::probe_external_cached(const std::string& audio_path, bool background_priority) {
    {
        std::lock_guard<std::mutex> lock(external_probe_cache_mutex_);
        std::unordered_map<std::string, ExternalAudioInfo>::const_iterator cached = external_probe_cache_.find(audio_path);
        if (cached != external_probe_cache_.end()) {
            return cached->second;
        }
    }
    ExternalAudioInfo info = ExternalAudioDecoder::probe_metadata(audio_path, 0, 0, background_priority);
    std::lock_guard<std::mutex> lock(external_probe_cache_mutex_);
    external_probe_cache_[audio_path] = info;
    return info;
}

void GtkPlayerWindow::probe_playlist_entry(PlaylistEntry& entry, bool background_priority) {
    if (entry.metadata_probed || entry.cue_track) {
        return;
    }

    const std::string& path = entry.audio_file_path;
    const std::string ext = lower_extension(path);
    AudioFormat probed_format{};
    std::uint64_t probed_total_samples = 0;
    bool native_decode = false;
    ExternalAudioInfo external_info{};
    bool have_external_info = false;
    FlacTags flac_tags;
    GenericTags generic_tags;

    if (ext == ".flac") {
        const FlacFileProbe flac_probe = FlacStreamDecoder::probe_file(path);
        if (flac_probe.valid) {
            probed_format = flac_probe.format;
            probed_total_samples = flac_probe.total_samples_per_channel;
            native_decode = true;
            flac_tags = flac_probe.tags;
        } else {
            external_info = probe_external_cached(path, background_priority);
            have_external_info = true;
            probed_format = external_info.format;
            probed_total_samples = external_info.total_samples_per_channel;
            generic_tags = external_info.tags;
        }
    } else {
        external_info = probe_external_cached(path, background_priority);
        have_external_info = true;
        probed_format = external_info.format;
        probed_total_samples = external_info.total_samples_per_channel;
        generic_tags = external_info.tags;
    }

    if (ext == ".flac" && native_decode) {
        entry.track_number = flac_tags.track_number > 0 ? flac_tags.track_number : entry.track_number;
        if (entry.title.empty() || entry.title == base_name(path)) {
            entry.title = flac_tags.title.empty() ? base_name(path) : flac_tags.title;
        }
        if (entry.performer.empty()) {
            entry.performer = flac_tags.artist;
        }
    } else {
        entry.track_number = generic_tags.track_number > 0 ? generic_tags.track_number : entry.track_number;
        if (entry.title.empty() || entry.title == base_name(path)) {
            entry.title = generic_tags.title.empty() ? base_name(path) : generic_tags.title;
        }
        if (entry.performer.empty()) {
            entry.performer = generic_tags.artist;
        }
    }
    entry.start_sample = 0;
    entry.end_sample = probed_total_samples;
    if (entry.source_label.empty()) {
        entry.source_label = base_name(path);
    }
    entry.decoded_format = probed_format;
    entry.source_sample_rate = entry.decoded_format.sample_rate;
    entry.source_bits_per_sample = entry.decoded_format.bits_per_sample;
    const std::uint32_t target_rate = target_sample_rate_for(entry.source_sample_rate);
    const std::uint16_t target_bits = target_bits_for(entry.source_bits_per_sample);
    entry.resampled = (target_rate > 0 && target_rate != entry.source_sample_rate);
    entry.resampled_from_rate = entry.resampled ? entry.source_sample_rate : 0;
    entry.bitdepth_converted = (target_bits > 0 && target_bits != entry.source_bits_per_sample);
    entry.native_decode = native_decode && !entry.resampled && !entry.bitdepth_converted;
    entry.processed_by_ffmpeg = (!entry.native_decode);
    if (entry.resampled) {
        entry.end_sample = static_cast<std::uint64_t>(std::llround(static_cast<double>(entry.end_sample) * static_cast<double>(target_rate) / static_cast<double>(entry.source_sample_rate)));
        entry.decoded_format.sample_rate = target_rate;
    }
    if (entry.bitdepth_converted) {
        entry.decoded_format.bits_per_sample = target_bits;
    }
    entry.codec_name = native_decode ? std::string("flac") : external_info.codec_name;
    entry.lossless_source = native_decode || (have_external_info && external_info.lossless) ||
                            (ext == ".flac" || ext == ".wav" || ext == ".wave" || ext == ".bwf" || ext == ".aiff" || ext == ".aif" || ext == ".au" || ext == ".snd" || ext == ".caf" || ext == ".ape" || ext == ".wv" || ext == ".tak" || ext == ".tta" || ext == ".dsf");
    entry.lossy_source = (!entry.lossless_source && (ext == ".mp3" || ext == ".m4a" || ext == ".aac" || ext == ".ogg" || ext == ".oga" || ext == ".opus" || ext == ".wma" || ext == ".asf" || ext == ".xwma" || ext == ".oma" || ext == ".aa3" || ext == ".at3" || ext == ".mpc" || ext == ".mp+" || ext == ".mpp"));
    entry.title = safe_utf8_for_display(entry.title);
    entry.performer = safe_utf8_for_display(entry.performer);
    entry.source_label = safe_utf8_for_display(entry.source_label);
    entry.metadata_probed = true;
}

void GtkPlayerWindow::ensure_playlist_entry_probed(std::size_t index) {
    if (index >= playlist_.size()) {
        return;
    }
    PlaylistEntry& entry = playlist_[index];
    if (entry.metadata_probed) {
        return;
    }
    probe_playlist_entry(entry);
    update_playlist_view_row(index);
}

void GtkPlayerWindow::ensure_gapless_neighbors_probed(std::size_t index) {
    if (index >= playlist_.size()) {
        return;
    }
    ensure_playlist_entry_probed(index);
    if (!playlist_[index].metadata_probed) {
        return;
    }

    std::size_t chain_end = index + 1;
    if (playlist_[index].cue_track) {
        chain_end = cue_chain_end_index(index);
    } else {
        chain_end = file_chain_end_index(index);
    }
    for (std::size_t i = index + 1; i < chain_end; ++i) {
        ensure_playlist_entry_probed(i);
    }
}

void GtkPlayerWindow::cancel_playlist_metadata_probe() {
    playlist_metadata_probe_cancel_.store(true, std::memory_order_release);
    if (playlist_metadata_probe_thread_.joinable()) {
        playlist_metadata_probe_thread_.join();
    }
    playlist_metadata_probe_cancel_.store(false, std::memory_order_release);
    playlist_metadata_probe_index_ = 0;
    if (playlist_metadata_probe_ui_idle_id_ != 0) {
        g_source_remove(playlist_metadata_probe_ui_idle_id_);
        playlist_metadata_probe_ui_idle_id_ = 0;
    }
    playlist_metadata_probe_pending_ui_.clear();
}

gboolean GtkPlayerWindow::on_playlist_metadata_probe_ui_idle(gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr) {
        return G_SOURCE_REMOVE;
    }
    self->flush_playlist_metadata_probe_ui_updates();
    return G_SOURCE_REMOVE;
}

void GtkPlayerWindow::flush_playlist_metadata_probe_ui_updates() {
    playlist_metadata_probe_ui_idle_id_ = 0;
    if (playlist_metadata_probe_pending_ui_.empty()) {
        return;
    }
    for (std::size_t index : playlist_metadata_probe_pending_ui_) {
        update_playlist_view_row(index);
    }
    playlist_metadata_probe_pending_ui_.clear();
}

void GtkPlayerWindow::schedule_playlist_metadata_probe_if_needed() {
    if (playlist_.empty() || ui_closing_) {
        return;
    }
    if (playlist_metadata_probe_thread_.joinable()) {
        return;
    }
    for (const PlaylistEntry& entry : playlist_) {
        if (!entry.metadata_probed) {
            schedule_playlist_metadata_probe();
            return;
        }
    }
}

void GtkPlayerWindow::schedule_playlist_metadata_probe() {
    cancel_playlist_metadata_probe();
    if (playlist_.empty()) {
        return;
    }
    playlist_metadata_probe_index_ = 0;
    while (playlist_metadata_probe_index_ < playlist_.size() && playlist_[playlist_metadata_probe_index_].metadata_probed) {
        ++playlist_metadata_probe_index_;
    }
    if (playlist_metadata_probe_index_ >= playlist_.size()) {
        return;
    }
    playlist_metadata_probe_thread_ = std::thread(&GtkPlayerWindow::playlist_metadata_probe_worker, this);
}

gboolean GtkPlayerWindow::apply_playlist_metadata_probe(gpointer user_data) {
    std::unique_ptr<PlaylistMetadataProbeInvoke> payload(static_cast<PlaylistMetadataProbeInvoke*>(user_data));
    GtkPlayerWindow* self = payload->window;
    if (self == nullptr || self->playlist_metadata_probe_cancel_.load(std::memory_order_acquire)) {
        return G_SOURCE_REMOVE;
    }
    if (payload->apply.index < self->playlist_.size()) {
        self->playlist_[payload->apply.index] = std::move(payload->apply.entry);
        self->playlist_metadata_probe_pending_ui_.push_back(payload->apply.index);
        if (self->playlist_metadata_probe_ui_idle_id_ == 0) {
            self->playlist_metadata_probe_ui_idle_id_ =
                g_idle_add(GtkPlayerWindow::on_playlist_metadata_probe_ui_idle, self);
        }
    }
    return G_SOURCE_REMOVE;
}

void GtkPlayerWindow::playlist_metadata_probe_worker() {
    while (!playlist_metadata_probe_cancel_.load(std::memory_order_acquire)) {
        PlaylistEntry entry;
        std::size_t index = 0;
        {
            std::lock_guard<std::mutex> lock(playlist_metadata_probe_mutex_);
            index = playlist_metadata_probe_index_;
            if (index >= playlist_.size()) {
                break;
            }
            playlist_metadata_probe_index_ = index + 1;
            if (playlist_[index].metadata_probed) {
                continue;
            }
            entry = playlist_[index];
        }

        probe_playlist_entry(entry, true);
        if (playlist_metadata_probe_cancel_.load(std::memory_order_acquire)) {
            break;
        }
        auto* payload = new PlaylistMetadataProbeInvoke{this, PlaylistMetadataProbeApply{index, std::move(entry)}};
        g_main_context_invoke_full(g_main_context_default(),
                                   G_PRIORITY_LOW,
                                   apply_playlist_metadata_probe,
                                   payload,
                                   nullptr);
    }

    if (!playlist_metadata_probe_cancel_.load(std::memory_order_acquire)) {
        g_main_context_invoke_full(g_main_context_default(),
                                   G_PRIORITY_LOW,
                                   +[](gpointer user_data) -> gboolean {
                                       auto* self = static_cast<GtkPlayerWindow*>(user_data);
                                       self->flush_playlist_metadata_probe_ui_updates();
                                       self->refresh_display();
                                       return G_SOURCE_REMOVE;
                                   },
                                   this,
                                   nullptr);
    }
}

void GtkPlayerWindow::update_playlist_view_row(std::size_t index) {
    if (index >= playlist_.size() || playlist_store_ == nullptr) {
        return;
    }
    GtkTreePath* path = gtk_tree_path_new_from_indices(static_cast<int>(index), -1);
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(playlist_store_), &iter, path)) {
        const PlaylistEntry& entry = playlist_[index];
        const bool stream_broken = entry.is_stream && stream_health_.is_broken(entry.audio_file_path);
        const std::string trackno = stream_broken
            ? ("× " + std::to_string(entry.track_number))
            : std::to_string(entry.track_number);
        gtk_list_store_set(playlist_store_, &iter,
                           COL_INDEX, static_cast<int>(index),
                           COL_TRACKNO, trackno.c_str(),
                           COL_ARTIST, safe_utf8_for_display(entry.performer).c_str(),
                           COL_TITLE, safe_utf8_for_display(entry.title).c_str(),
                           COL_SOURCE, safe_utf8_for_display(entry.source_label).c_str(),
                           COL_STREAM_BROKEN, stream_broken ? TRUE : FALSE,
                           -1);
    }
    gtk_tree_path_free(path);
}

void GtkPlayerWindow::append_path_to_playlist(const std::string& path, bool defer_metadata_probe) {
    append_media_to_playlist(path, std::string(), std::string(), defer_metadata_probe);
}

gboolean GtkPlayerWindow::on_stream_metadata_idle(gpointer user_data) {
    std::unique_ptr<StreamMetadataUpdate> update(static_cast<StreamMetadataUpdate*>(user_data));
    if (update != nullptr && update->self != nullptr && !update->self->ui_closing_) {
        update->self->apply_stream_metadata(update->title);
    }
    return G_SOURCE_REMOVE;
}

void GtkPlayerWindow::apply_stream_metadata(const std::string& title) {
    if (title.empty()) {
        return;
    }
    stream_now_playing_ = title;
    refresh_display();
    notify_mpris_state_changed();
}

void GtkPlayerWindow::start_stream_sidecar(const std::string& stream_url) {
    if (stream_sidecar_ != nullptr && stream_sidecar_url_ == stream_url) {
        return;
    }

    stop_stream_sidecar(false);
    stream_now_playing_.clear();
    stream_sidecar_url_ = stream_url;
    stream_sidecar_ = std::make_unique<StreamSidecar>();
    GtkPlayerWindow* self = this;
    stream_sidecar_->start(stream_url, [self](const std::string& title) {
        if (self == nullptr || self->ui_closing_) {
            return;
        }
        auto* update = new StreamMetadataUpdate{self, title};
        g_idle_add(&GtkPlayerWindow::on_stream_metadata_idle, update);
    });
}

void GtkPlayerWindow::stop_stream_sidecar(bool wait_for_exit) {
    if (stream_sidecar_ == nullptr) {
        stream_now_playing_.clear();
        stream_sidecar_url_.clear();
        return;
    }

    std::unique_ptr<StreamSidecar> sidecar = std::move(stream_sidecar_);
    stream_sidecar_url_.clear();
    stream_now_playing_.clear();
    if (wait_for_exit) {
        sidecar->stop();
        return;
    }

    std::thread([sidecar = std::move(sidecar)]() mutable {
        sidecar->stop();
    }).detach();
}

void GtkPlayerWindow::cancel_stream_reconnect() {
    stream_reconnect_pending_ = false;
    stream_reconnect_target_index_ = static_cast<std::size_t>(-1);
    stream_reconnect_attempts_ = 0;
    stream_status_override_.clear();
}

void GtkPlayerWindow::note_stream_broken(const std::string& url, const std::string& error) {
    stream_health_.mark_broken(url, error);
    if (ui_closing_) {
        return;
    }
    refresh_stream_health_rows_for_url(url);
}

void GtkPlayerWindow::reset_stream_health_tracking() {
    stream_health_track_url_.clear();
    stream_health_playing_ = false;
}

void GtkPlayerWindow::update_stream_health_from_playback(const PlaybackStatusSnapshot& status) {
    if (playlist_.empty() || current_track_index_ >= playlist_.size()) {
        reset_stream_health_tracking();
        return;
    }

    const PlaylistEntry& track = playlist_[current_track_index_];
    if (!track.is_stream) {
        reset_stream_health_tracking();
        return;
    }

    const std::string& url = track.audio_file_path;
    if (url != stream_health_track_url_) {
        stream_health_track_url_ = url;
        stream_health_playing_ = false;
    }

    if (status.playing && !status.paused) {
        if (!stream_health_playing_) {
            stream_health_playing_ = true;
            stream_health_playing_since_ = std::chrono::steady_clock::now();
            return;
        }

        const auto elapsed = std::chrono::steady_clock::now() - stream_health_playing_since_;
        if (elapsed >= std::chrono::seconds(kStreamHealthOkSeconds)) {
            if (stream_health_.mark_ok(url)) {
                refresh_stream_health_rows_for_url(url);
            }
            stream_health_playing_ = false;
        }
    } else {
        stream_health_playing_ = false;
    }
}

void GtkPlayerWindow::schedule_stream_reconnect(std::size_t index) {
    if (index >= playlist_.size() || !playlist_[index].is_stream) {
        return;
    }

    const std::string& url = playlist_[index].audio_file_path;
    const std::string error = stream_status_override_.empty() ? "Stream unavailable" : stream_status_override_;
    note_stream_broken(url, error);

    if (stream_reconnect_attempts_ >= kMaxStreamReconnectAttempts) {
        stream_status_override_ = "Stream unavailable";
        return;
    }

    const int delay_index = std::min(stream_reconnect_attempts_,
                                     static_cast<int>(sizeof(kStreamReconnectDelaysSec) / sizeof(kStreamReconnectDelaysSec[0])) - 1);
    stream_reconnect_target_index_ = index;
    stream_reconnect_pending_ = true;
    stream_reconnect_due_ = std::chrono::steady_clock::now() + std::chrono::seconds(kStreamReconnectDelaysSec[delay_index]);
    stream_status_override_ = "Reconnecting...";
    ++stream_reconnect_attempts_;
    refresh_display();
}

void GtkPlayerWindow::append_stream_entry(const std::string& path,
                                          const std::string& hint_title,
                                          const std::string& hint_artist) {
    PlaylistEntry entry;
    entry.audio_file_path = path;
    entry.is_stream = true;
    entry.track_number = static_cast<int>(playlist_.size() + 1);
    entry.title = !hint_title.empty() ? hint_title : stream_display_label(path);
    entry.performer = hint_artist;
    entry.start_sample = 0;
    entry.end_sample = 0;
    entry.source_label = stream_display_label(path);
    const std::string stream_hint = !hint_title.empty() ? hint_title : path;
    const std::uint32_t hinted_rate = stream_sample_rate_hint(stream_hint);
    const std::uint16_t hinted_bits = stream_bits_per_sample_hint(stream_hint);
    entry.decoded_format.sample_rate = hinted_rate > 0 ? hinted_rate : 44100;
    entry.decoded_format.channels = 2;
    entry.decoded_format.bits_per_sample = hinted_bits > 0 ? hinted_bits : 16;
    entry.source_sample_rate = entry.decoded_format.sample_rate;
    entry.source_bits_per_sample = entry.decoded_format.bits_per_sample;
    const std::uint32_t target_rate = target_sample_rate_for(entry.source_sample_rate);
    const std::uint16_t target_bits = target_bits_for(entry.source_bits_per_sample);
    entry.resampled = (target_rate > 0 && target_rate != entry.source_sample_rate);
    entry.resampled_from_rate = entry.resampled ? entry.source_sample_rate : 0;
    entry.bitdepth_converted = (target_bits > 0 && target_bits != entry.source_bits_per_sample);
    entry.native_decode = false;
    entry.processed_by_ffmpeg = true;
    if (entry.resampled) {
        entry.decoded_format.sample_rate = target_rate;
    }
    if (entry.bitdepth_converted) {
        entry.decoded_format.bits_per_sample = target_bits;
    }
    const std::string stream_hint_lower = [&stream_hint]() {
        std::string lower = stream_hint;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return lower;
    }();
    const bool hinted_flac = stream_hint_lower.find("flac") != std::string::npos;
    entry.codec_name = hinted_flac ? "flac" : (is_hls_media_uri(path) ? "hls" : "stream");
    entry.lossless_source = hinted_flac;
    entry.lossy_source = !hinted_flac;
    entry.title = safe_utf8_for_display(entry.title);
    entry.performer = safe_utf8_for_display(entry.performer);
    entry.source_label = safe_utf8_for_display(entry.source_label);
    playlist_.push_back(entry);
}

void GtkPlayerWindow::append_media_to_playlist(const std::string& path,
                                                 const std::string& hint_title,
                                                 const std::string& hint_artist,
                                                 bool defer_metadata_probe) {
    if (file_looks_like_hls_playlist(path)) {
        append_stream_entry(path, hint_title, hint_artist);
        return;
    }

    if (M3uPlaylistReader::looks_like_playlist_path(path)) {
        const std::vector<M3uPlaylistEntry> entries = M3uPlaylistReader::read_entries(path);
        const bool defer_children = defer_metadata_probe || entries.size() >= kBulkPlaylistImportThreshold;
        Logger::instance().info("Importing playlist: " + path + " entries=" + std::to_string(entries.size()));
        for (const M3uPlaylistEntry& item : entries) {
            if (M3uPlaylistReader::looks_like_playlist_path(item.location)) {
                Logger::instance().debug("Skipping nested playlist entry: " + item.location);
                continue;
            }
            try {
                append_media_to_playlist(item.location, item.title, item.artist, defer_children);
            } catch (const std::exception& ex) {
                Logger::instance().debug(std::string("Skipping playlist entry: ") + item.location + " (" + ex.what() + ")");
            }
        }
        return;
    }

    if (ExternalAudioDecoder::is_stream_uri(path)) {
        append_stream_entry(path, hint_title, hint_artist);
        return;
    }

    if (CueParser::looks_like_cue_path(path)) {
        CueSheet sheet;
        AudioFormat cue_format{};
        ExternalAudioInfo cue_external_info{};
        bool cue_external_info_ready = false;
        bool cue_native = false;
        bool cue_format_ready = false;
        std::unordered_map<std::string, CueSheet>::const_iterator cached = cue_cache_.find(path);
        if (cached != cue_cache_.end()) {
            sheet = cached->second;
        } else {
            const std::string audio_path = CueParser::resolve_audio_file_path(path);
            const std::string audio_ext = lower_extension(audio_path);
            std::uint64_t total_samples = 0;
            if (audio_ext == ".flac") {
                const FlacFileProbe flac_probe = FlacStreamDecoder::probe_file(audio_path);
                if (flac_probe.valid) {
                    total_samples = flac_probe.total_samples_per_channel;
                    cue_format = flac_probe.format;
                    cue_native = true;
                    cue_format_ready = true;
                } else {
                    cue_external_info = probe_external_cached(audio_path);
                    cue_external_info_ready = true;
                    total_samples = cue_external_info.total_samples_per_channel;
                    cue_format = cue_external_info.format;
                    cue_format_ready = true;
                }
            } else {
                cue_external_info = probe_external_cached(audio_path);
                cue_external_info_ready = true;
                total_samples = cue_external_info.total_samples_per_channel;
                cue_format = cue_external_info.format;
                cue_format_ready = true;
            }
            sheet = CueParser::parse_file(path, total_samples);
            cue_cache_[path] = sheet;
        }

        const std::string cue_audio_ext = lower_extension(sheet.audio_file_path);
        if (!cue_format_ready) {
            if (cue_audio_ext == ".flac") {
                const FlacFileProbe flac_probe = FlacStreamDecoder::probe_file(sheet.audio_file_path);
                if (flac_probe.valid) {
                    cue_format = flac_probe.format;
                    cue_native = true;
                } else {
                    cue_external_info = probe_external_cached(sheet.audio_file_path);
                    cue_external_info_ready = true;
                    cue_format = cue_external_info.format;
                }
            } else {
                cue_external_info = probe_external_cached(sheet.audio_file_path);
                cue_external_info_ready = true;
                cue_format = cue_external_info.format;
            }
        }

        for (std::size_t i = 0; i < sheet.tracks.size(); ++i) {
            PlaylistEntry entry;
            entry.audio_file_path = sheet.audio_file_path;
            entry.track_number = sheet.tracks[i].number;
            entry.title = sheet.tracks[i].title;
            entry.performer = sheet.tracks[i].performer.empty() ? sheet.performer : sheet.tracks[i].performer;
            entry.start_sample = sheet.tracks[i].start_sample;
            entry.end_sample = sheet.tracks[i].end_sample;
            entry.source_label = base_name(path);
            entry.cue_track = true;
            entry.cue_album_end_sample = cue_format.sample_rate > 0 ? sheet.tracks.back().end_sample : 0;
            entry.source_sample_rate = cue_format.sample_rate;
            entry.source_bits_per_sample = cue_format.bits_per_sample;
            const std::uint32_t cue_target_rate = target_sample_rate_for(cue_format.sample_rate);
            entry.decoded_format = cue_format;
            const std::uint16_t cue_target_bits = target_bits_for(cue_format.bits_per_sample);
            entry.resampled = (cue_target_rate > 0 && cue_target_rate != cue_format.sample_rate);
            entry.resampled_from_rate = entry.resampled ? cue_format.sample_rate : 0;
            entry.bitdepth_converted = (cue_target_bits > 0 && cue_target_bits != cue_format.bits_per_sample);
            entry.native_decode = cue_native && !entry.resampled && !entry.bitdepth_converted;
            entry.processed_by_ffmpeg = (!entry.native_decode);
            if (entry.resampled) {
                entry.decoded_format.sample_rate = cue_target_rate;
                entry.start_sample = static_cast<std::uint64_t>(std::llround(static_cast<double>(entry.start_sample) * static_cast<double>(cue_target_rate) / static_cast<double>(cue_format.sample_rate)));
                entry.end_sample = static_cast<std::uint64_t>(std::llround(static_cast<double>(entry.end_sample) * static_cast<double>(cue_target_rate) / static_cast<double>(cue_format.sample_rate)));
            }
            if (entry.bitdepth_converted) {
                entry.decoded_format.bits_per_sample = cue_target_bits;
            }
            entry.codec_name = cue_native ? std::string("flac") : cue_external_info.codec_name;
            entry.lossless_source = cue_native || (cue_external_info_ready && cue_external_info.lossless) ||
                                    (cue_audio_ext == ".flac" || cue_audio_ext == ".wav" || cue_audio_ext == ".wave" || cue_audio_ext == ".bwf" || cue_audio_ext == ".aiff" || cue_audio_ext == ".aif" || cue_audio_ext == ".au" || cue_audio_ext == ".snd" || cue_audio_ext == ".caf" || cue_audio_ext == ".ape" || cue_audio_ext == ".wv" || cue_audio_ext == ".tak" || cue_audio_ext == ".tta" || cue_audio_ext == ".dsf");
            entry.lossy_source = (!entry.lossless_source && (cue_audio_ext == ".mp3" || cue_audio_ext == ".m4a" || cue_audio_ext == ".aac" || cue_audio_ext == ".ogg" || cue_audio_ext == ".oga" || cue_audio_ext == ".opus" || cue_audio_ext == ".wma" || cue_audio_ext == ".asf" || cue_audio_ext == ".xwma" || cue_audio_ext == ".oma" || cue_audio_ext == ".aa3" || cue_audio_ext == ".at3" || cue_audio_ext == ".mpc" || cue_audio_ext == ".mp+" || cue_audio_ext == ".mpp"));
            entry.title = safe_utf8_for_display(entry.title);
            entry.performer = safe_utf8_for_display(entry.performer);
            entry.source_label = safe_utf8_for_display(entry.source_label);
            entry.metadata_probed = true;
            playlist_.push_back(entry);
        }
        return;
    }

    if (defer_metadata_probe) {
        PlaylistEntry entry;
        entry.audio_file_path = path;
        entry.track_number = static_cast<int>(playlist_.size() + 1);
        entry.title = !hint_title.empty() ? hint_title : safe_utf8_for_display(base_name(path));
        entry.performer = hint_artist;
        entry.source_label = entry.title;
        entry.metadata_probed = false;
        playlist_.push_back(entry);
        return;
    }

    PlaylistEntry entry;
    entry.audio_file_path = path;
    entry.track_number = static_cast<int>(playlist_.size() + 1);
    entry.title = base_name(path);
    entry.source_label = entry.title;
    probe_playlist_entry(entry);
    if (!hint_title.empty()) {
        entry.title = hint_title;
    }
    if (!hint_artist.empty()) {
        entry.performer = hint_artist;
    }
    playlist_.push_back(entry);
}

void GtkPlayerWindow::start_current_track(bool restart_if_paused) {
    update_playlist_selection_from_ui();
    if (playlist_.empty()) {
        return;
    }

    if (engine_.is_paused() && restart_if_paused) {
        engine_.resume();
        return;
    }

    play_track_index(current_track_index_);
}

void GtkPlayerWindow::stop_playback() {
    cancel_pending_seek();
    stop_stream_sidecar();
    cancel_stream_reconnect();
    reset_stream_health_tracking();
    {
        std::lock_guard<std::mutex> lock(stream_probe_mutex_);
        ++stream_probe_generation_;
    }
    track_switch_in_progress_ = false;
    finish_handled_ = false;
    softvol_dragging_ = false;
    clear_gapless_chain();
    engine_.stop();
    if (!ui_closing_) {
        refresh_display();
        notify_mpris_state_changed();
    }
}

void GtkPlayerWindow::play_track_index(std::size_t index) {
    play_track_index_at_offset(index, 0);
}

void GtkPlayerWindow::apply_stream_probe_to_entry(PlaylistEntry& entry, const ExternalAudioInfo& info) {
    entry.source_sample_rate = info.source_format.sample_rate > 0 ? info.source_format.sample_rate : info.format.sample_rate;
    entry.source_bits_per_sample = info.source_format.bits_per_sample > 0 ? info.source_format.bits_per_sample : info.format.bits_per_sample;
    entry.decoded_format = info.format;
    if (entry.decoded_format.channels == 0) {
        entry.decoded_format.channels = 2;
    }
    if (!info.codec_name.empty()) {
        entry.codec_name = info.codec_name;
    }
    entry.lossless_source = info.lossless;
    entry.lossy_source = !info.lossless;
    const std::uint32_t target_rate = target_sample_rate_for(entry.source_sample_rate);
    const std::uint16_t target_bits = target_bits_for(entry.source_bits_per_sample);
    entry.resampled = (target_rate > 0 && target_rate != entry.source_sample_rate);
    entry.resampled_from_rate = entry.resampled ? entry.source_sample_rate : 0;
    entry.bitdepth_converted = (target_bits > 0 && target_bits != entry.source_bits_per_sample);
    if (entry.resampled) {
        entry.decoded_format.sample_rate = target_rate;
    }
    if (entry.bitdepth_converted) {
        entry.decoded_format.bits_per_sample = target_bits;
    }
    entry.stream_format_probed = true;
}

bool GtkPlayerWindow::stream_probe_is_current(std::uint64_t generation) const {
    std::lock_guard<std::mutex> lock(stream_probe_mutex_);
    return generation == stream_probe_generation_;
}

std::size_t GtkPlayerWindow::find_playlist_index_by_url(const std::string& url) const {
    const std::string normalized = normalize_stream_url(url);
    for (std::size_t i = 0; i < playlist_.size(); ++i) {
        if (normalize_stream_url(playlist_[i].audio_file_path) == normalized) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

void GtkPlayerWindow::mark_stream_broken_from_probe(const std::string& url, const std::string& error) {
    stream_health_.mark_broken(url, error);
    if (ui_closing_) {
        return;
    }
    refresh_stream_health_rows_for_url(url);
}

void GtkPlayerWindow::ensure_stream_probe_worker() {
    std::lock_guard<std::mutex> lock(stream_probe_mutex_);
    if (stream_probe_thread_.joinable()) {
        return;
    }
    stream_probe_shutdown_ = false;
    stream_probe_thread_ = std::thread(&GtkPlayerWindow::stream_probe_worker_loop, this);
}

void GtkPlayerWindow::enqueue_stream_probe(std::size_t index,
                                           std::uint64_t offset_samples,
                                           bool preserve_paused,
                                           bool update_mpris_track,
                                           bool skip_engine_stop,
                                           std::uint64_t probe_generation,
                                           const std::string& url,
                                           std::uint32_t forced_output_sample_rate,
                                           std::uint16_t forced_output_bits_per_sample) {
    ensure_stream_probe_worker();

    StreamProbeTask task;
    task.self = this;
    task.generation = probe_generation;
    task.index = index;
    task.offset_samples = offset_samples;
    task.preserve_paused = preserve_paused;
    task.update_mpris_track = update_mpris_track;
    task.skip_engine_stop = skip_engine_stop;
    task.url = url;
    task.forced_output_sample_rate = forced_output_sample_rate;
    task.forced_output_bits_per_sample = forced_output_bits_per_sample;

    {
        std::lock_guard<std::mutex> lock(stream_probe_mutex_);
        const auto stale = std::remove_if(stream_probe_queue_.begin(),
                                          stream_probe_queue_.end(),
                                          [&](const StreamProbeTask& pending) { return pending.url == task.url; });
        stream_probe_queue_.erase(stale, stream_probe_queue_.end());
        stream_probe_queue_.push_front(std::move(task));
    }
    stream_probe_cv_.notify_one();
}

void GtkPlayerWindow::stream_probe_worker_loop() {
    for (;;) {
        StreamProbeTask task;
        {
            std::unique_lock<std::mutex> lock(stream_probe_mutex_);
            stream_probe_cv_.wait(lock, [this]() {
                return stream_probe_shutdown_ || !stream_probe_queue_.empty();
            });
            if (stream_probe_shutdown_ && stream_probe_queue_.empty()) {
                return;
            }
            task = std::move(stream_probe_queue_.front());
            stream_probe_queue_.pop_front();
        }

        std::thread([task = std::move(task)]() mutable {
            auto* result = new StreamProbeResult;
            static_cast<StreamProbeTask&>(*result) = std::move(task);
            const std::uint64_t generation = result->generation;
            GtkPlayerWindow* self = result->self;

            try {
                result->info = ExternalAudioDecoder::probe_info(result->url,
                                                                result->forced_output_sample_rate,
                                                                result->forced_output_bits_per_sample);
                result->probe_ok = true;
            } catch (const std::exception& ex) {
                result->error = ex.what();
            } catch (...) {
                result->error = "Stream probe failed";
            }

            if (self != nullptr && !self->stream_probe_is_current(generation) &&
                result->probe_ok && result->info.live_format_probed) {
                if (!ExternalAudioDecoder::verify_stream_playback(result->url,
                                                                  result->info,
                                                                  result->forced_output_sample_rate,
                                                                  result->forced_output_bits_per_sample)) {
                    result->info.live_format_probed = false;
                }
            }

            g_idle_add(&GtkPlayerWindow::on_stream_probe_idle, result);
        }).detach();
    }
}

void GtkPlayerWindow::shutdown_stream_probe_worker() {
    {
        std::lock_guard<std::mutex> lock(stream_probe_mutex_);
        stream_probe_shutdown_ = true;
    }
    stream_probe_cv_.notify_all();
    if (stream_probe_thread_.joinable()) {
        stream_probe_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(stream_probe_mutex_);
        stream_probe_queue_.clear();
    }
}

void GtkPlayerWindow::begin_async_stream_probe_and_play(std::size_t index,
                                                        std::uint64_t offset_samples,
                                                        bool preserve_paused,
                                                        bool update_mpris_track,
                                                        bool skip_engine_stop,
                                                        std::uint64_t probe_generation) {
    if (index >= playlist_.size()) {
        track_switch_in_progress_ = false;
        finish_handled_ = false;
        return;
    }

    const PlaylistEntry& entry = playlist_[index];
    const std::uint32_t source_rate = entry.source_sample_rate > 0 ? entry.source_sample_rate : entry.decoded_format.sample_rate;
    const std::uint16_t source_bits = entry.source_bits_per_sample > 0 ? entry.source_bits_per_sample : entry.decoded_format.bits_per_sample;
    const std::uint32_t target_rate = target_sample_rate_for(source_rate);
    const std::uint16_t target_bits = target_bits_for(source_bits);
    const std::uint32_t forced_rate = (target_rate > 0 && target_rate != source_rate) ? target_rate : 0;
    const std::uint16_t forced_bits = (target_bits > 0 && target_bits != source_bits) ? target_bits : 0;

    stream_status_override_ = "Probing stream...";
    refresh_display();

    enqueue_stream_probe(index,
                         offset_samples,
                         preserve_paused,
                         update_mpris_track,
                         skip_engine_stop,
                         probe_generation,
                         entry.audio_file_path,
                         forced_rate,
                         forced_bits);
}

gboolean GtkPlayerWindow::on_stream_probe_idle(gpointer user_data) {
    std::unique_ptr<StreamProbeResult> request(static_cast<StreamProbeResult*>(user_data));
    GtkPlayerWindow* self = request->self;
    if (self == nullptr || self->ui_closing_) {
        return G_SOURCE_REMOVE;
    }

    const bool stale = !self->stream_probe_is_current(request->generation);
    const std::size_t playlist_index = self->find_playlist_index_by_url(request->url);
    const bool connect_failed = !request->probe_ok || !request->info.live_format_probed;

    if (connect_failed) {
        const std::string error = !request->probe_ok
            ? (request->error.empty() ? std::string("Stream probe failed") : request->error)
            : "Stream unavailable";
        self->mark_stream_broken_from_probe(request->url, error);

        if (stale) {
            return G_SOURCE_REMOVE;
        }

        self->track_switch_in_progress_ = false;
        self->finish_handled_ = false;
        self->stream_status_override_.clear();
        Logger::instance().error(std::string("Failed to probe stream: ") + error);
        GtkWidget* msg = gtk_message_dialog_new(GTK_WINDOW(self->window_),
                                                GTK_DIALOG_MODAL,
                                                GTK_MESSAGE_ERROR,
                                                GTK_BUTTONS_CLOSE,
                                                "%s",
                                                error.c_str());
        gtk_dialog_run(GTK_DIALOG(msg));
        gtk_widget_destroy(msg);
        self->notify_mpris_state_changed();
        return G_SOURCE_REMOVE;
    }

    if (playlist_index != static_cast<std::size_t>(-1)) {
        self->apply_stream_probe_to_entry(self->playlist_[playlist_index], request->info);
    }
    if (request->info.live_format_probed) {
        Logger::instance().info("Stream format probed: " + request->url + " -> " +
                                std::to_string(request->info.source_format.sample_rate) + " Hz / " +
                                std::to_string(request->info.source_format.bits_per_sample) + "-bit / " +
                                std::to_string(request->info.source_format.channels) + " ch");
    }

    if (stale) {
        return G_SOURCE_REMOVE;
    }
    if (playlist_index == static_cast<std::size_t>(-1)) {
        self->track_switch_in_progress_ = false;
        self->finish_handled_ = false;
        return G_SOURCE_REMOVE;
    }

    self->stream_status_override_.clear();
    self->play_track_index_at_offset(playlist_index,
                                     request->offset_samples,
                                     true,
                                     request->preserve_paused,
                                     request->update_mpris_track,
                                     true);
    return G_SOURCE_REMOVE;
}

void GtkPlayerWindow::play_track_index_at_offset(std::size_t index,
                                                 std::uint64_t offset_samples,
                                                 bool start_playback,
                                                 bool preserve_paused,
                                                 bool update_mpris_track,
                                                 bool skip_engine_stop) {
    if (index >= playlist_.size()) {
        return;
    }

    cancel_playlist_metadata_probe();
    ensure_gapless_neighbors_probed(index);

    std::uint64_t probe_generation = 0;
    {
        std::lock_guard<std::mutex> lock(stream_probe_mutex_);
        ++stream_probe_generation_;
        probe_generation = stream_probe_generation_;
    }

    const bool reconnecting_same_stream = stream_reconnect_target_index_ == index &&
                                          index < playlist_.size() &&
                                          playlist_[index].is_stream;
    if (!reconnecting_same_stream) {
        stop_stream_sidecar();
    }
    if (stream_reconnect_target_index_ != index) {
        cancel_stream_reconnect();
    }

    if (reconnecting_same_stream && start_playback && !skip_engine_stop) {
        track_switch_in_progress_ = true;
        finish_handled_ = true;
        engine_.stop();
        clear_gapless_chain();
        current_track_index_ = index;
        select_playlist_row(current_track_index_);
        refresh_display();

        struct StreamPlaybackResume {
            GtkPlayerWindow* self = nullptr;
            std::size_t index = 0;
            std::uint64_t offset_samples = 0;
            bool preserve_paused = false;
            bool update_mpris_track = true;
        };
        auto* resume = new StreamPlaybackResume{this, index, offset_samples, preserve_paused, update_mpris_track};
        g_idle_add(+[](gpointer user_data) -> gboolean {
            std::unique_ptr<StreamPlaybackResume> resume(static_cast<StreamPlaybackResume*>(user_data));
            if (resume->self != nullptr && !resume->self->ui_closing_) {
                resume->self->play_track_index_at_offset(resume->index,
                                                         resume->offset_samples,
                                                         true,
                                                         resume->preserve_paused,
                                                         resume->update_mpris_track,
                                                         true);
            }
            return G_SOURCE_REMOVE;
        }, resume);
        return;
    }

    track_switch_in_progress_ = true;
    finish_handled_ = true;
    if (!skip_engine_stop) {
        engine_.stop();
    }
    clear_gapless_chain();

    current_track_index_ = index;
    const PlaylistEntry track = playlist_[current_track_index_];
    const std::uint64_t track_length = track_length_samples(track);
    const std::uint64_t initial_offset = std::min<std::uint64_t>(offset_samples, track_length);
    select_playlist_row(current_track_index_);

    if (!start_playback) {
        track_switch_in_progress_ = false;
        finish_handled_ = false;
        refresh_display();
        if (update_mpris_track) {
            mark_mpris_track_changed();
        } else {
            notify_mpris_state_changed();
        }
        schedule_playlist_metadata_probe_if_needed();
        return;
    }

    try {
        std::unique_ptr<IAudioDecoder> decoder;
        std::size_t chain_end = index + 1;

        if (track.cue_track) {
            chain_end = cue_chain_end_index(index);
            const std::uint64_t chain_end_sample = chain_end > index
                ? playlist_[chain_end - 1].end_sample
                : track.end_sample;
            std::unique_ptr<IAudioDecoder> base = create_decoder_for_entry(track, false);
            decoder.reset(new RangeLimitedDecoder(std::move(base), track.start_sample, chain_end_sample));
            decoder->open_at_sample(track.audio_file_path, initial_offset);
            activate_gapless_chain(index, chain_end);
            if (chain_end > index + 1) {
                Logger::instance().info("Continuous CUE playback enabled for " + std::to_string(chain_end - index) + " tracks: " + track.audio_file_path);
            }
        } else if (track.is_stream) {
            if (!playlist_[current_track_index_].stream_format_probed) {
                begin_async_stream_probe_and_play(index,
                                                  initial_offset,
                                                  preserve_paused,
                                                  update_mpris_track,
                                                  skip_engine_stop,
                                                  probe_generation);
                return;
            }
            decoder = create_decoder_for_entry(track, false);
            decoder->open(track.audio_file_path);
            const AudioFormat stream_format = decoder->format();
            playlist_[current_track_index_].decoded_format = stream_format;
            playlist_[current_track_index_].source_sample_rate = stream_format.sample_rate;
            playlist_[current_track_index_].source_bits_per_sample = stream_format.bits_per_sample;
            playlist_[current_track_index_].stream_format_probed = true;
            Logger::instance().info("Streaming playback: " + track.audio_file_path + " (" +
                                    std::to_string(stream_format.sample_rate) + " Hz)");
            start_stream_sidecar(track.audio_file_path);
        } else {
            chain_end = file_chain_end_index(index);
            if (chain_end > index + 1) {
                std::vector<GaplessTrackSpec> specs;
                specs.reserve(chain_end - index);
                for (std::size_t i = index; i < chain_end; ++i) {
                    specs.push_back(gapless_spec_for_entry(playlist_[i]));
                }
                decoder.reset(new GaplessChainDecoder(std::move(specs), initial_offset));
                decoder->open(track.audio_file_path);
                activate_gapless_chain(index, chain_end);
                Logger::instance().info("Gapless file chain enabled for " + std::to_string(chain_end - index) + " tracks");
            } else {
                decoder = create_decoder_for_entry(track, false);

                if (track.start_sample > 0 || track.end_sample > track.start_sample) {
                    Logger::instance().debug("Legacy bounded transport enabled");
                    decoder.reset(new RangeLimitedDecoder(std::move(decoder), track.start_sample, track.end_sample));
                }
                decoder->open_at_sample(track.audio_file_path, initial_offset);
            }
        }

        engine_.set_soft_volume_percent(soft_volume_percent_);
        engine_.set_soft_eq(bass_db_, treble_db_);
        engine_.set_pre_eq_headroom_tenths_db(effective_pre_eq_headroom_tenths_db());
        engine_.set_soft_eq_profile(bass_shelf_hz_, treble_shelf_hz_);
        engine_.set_deep_bass_enabled(deep_bass_enabled_);
        engine_.set_deep_bass_preset(deep_bass_internal_from_ui(deep_bass_preset_));
        engine_.set_deep_bass_amount(deep_bass_dsp_amount_from_ui(deep_bass_amount_));
        auto alsa_backend = std::make_unique<AlsaPcmBackend>();
        alsa_backend->set_24bit_container_preference(alsa_24bit_preference_from_id(alsa_24bit_container_preference_));
        engine_.set_realtime_priority_enabled(realtime_audio_priority_enabled_);
        engine_.start(std::move(decoder),
                      std::move(alsa_backend),
                      current_device_,
                      PlaybackStatusCallback(),
                      initial_offset);
        if (preserve_paused) {
            engine_.pause();
        }
        if (track.is_stream) {
            cancel_stream_reconnect();
            stream_status_override_.clear();
        }
        track_switch_in_progress_ = false;
        finish_handled_ = false;
        refresh_display();
        if (update_mpris_track) {
            mark_mpris_track_changed();
        } else {
            notify_mpris_state_changed();
        }
        schedule_playlist_metadata_probe_if_needed();
    } catch (const std::exception& ex) {
        clear_gapless_chain();
        track_switch_in_progress_ = false;
        finish_handled_ = false;
        Logger::instance().error(std::string("Failed to play track: ") + ex.what());
        if (track.is_stream) {
            note_stream_broken(track.audio_file_path, ex.what());
        }
        GtkWidget* msg = gtk_message_dialog_new(GTK_WINDOW(window_),
                                                GTK_DIALOG_MODAL,
                                                GTK_MESSAGE_ERROR,
                                                GTK_BUTTONS_CLOSE,
                                                "%s",
                                                ex.what());
        gtk_dialog_run(GTK_DIALOG(msg));
        gtk_widget_destroy(msg);
        notify_mpris_state_changed();
        schedule_playlist_metadata_probe_if_needed();
    }
}

void GtkPlayerWindow::open_file_dialog() {
    GtkWidget* dialog = gtk_dialog_new_with_buttons("Open audio files",
                                                    GTK_WINDOW(window_),
                                                    GTK_DIALOG_MODAL,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Open", GTK_RESPONSE_ACCEPT,
                                                    nullptr);

    GtkWidget* chooser = gtk_file_chooser_widget_new(GTK_FILE_CHOOSER_ACTION_OPEN);
    GtkFileChooser* file_chooser = GTK_FILE_CHOOSER(chooser);
    gtk_file_chooser_set_select_multiple(file_chooser, TRUE);
    if (!last_open_directory_.empty()) {
        gtk_file_chooser_set_current_folder(file_chooser, last_open_directory_.c_str());
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
            last_open_directory_ = current_folder;
            g_free(current_folder);
        }

        const std::vector<std::string> selected_paths = collect_file_chooser_paths(file_chooser);
        if (!selected_paths.empty()) {
            last_open_directory_ = directory_name(selected_paths.front());
            stop_playback();
            cancel_playlist_metadata_probe();
            playlist_.clear();
            clear_playlist_search();
            cue_cache_.clear();
            {
                std::lock_guard<std::mutex> lock(external_probe_cache_mutex_);
                external_probe_cache_.clear();
            }

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

            std::size_t plain_audio_count = 0;
            for (const std::string& path : paths_to_add) {
                if (!M3uPlaylistReader::looks_like_playlist_path(path) && !CueParser::looks_like_cue_path(path)) {
                    ++plain_audio_count;
                }
            }
            const bool defer_metadata_probe = plain_audio_count >= kBulkPlaylistImportThreshold;

            for (const std::string& path : paths_to_add) {
                try {
                    append_path_to_playlist(path, defer_metadata_probe);
                } catch (const std::exception& ex) {
                    Logger::instance().error(ex.what());
                }
            }
            save_preferences();
            current_track_index_ = 0;
            rebuild_playlist_view();
            bool needs_metadata_probe = false;
            for (const PlaylistEntry& entry : playlist_) {
                if (!entry.metadata_probed) {
                    needs_metadata_probe = true;
                    break;
                }
            }
            if (needs_metadata_probe) {
                schedule_playlist_metadata_probe_if_needed();
            }
            if (!playlist_.empty()) {
                select_playlist_row(current_track_index_);
            }
            track_switch_in_progress_ = false;
            finish_handled_ = true;
            refresh_display();
            mark_mpris_track_changed();
            save_playlist_session();
        }
    }

    gtk_widget_destroy(dialog);
}


void GtkPlayerWindow::open_settings_dialog() {
    refresh_device_list();
    refresh_dsp_info_for_current_device();

    enum { RESPONSE_APPLY_CLOSE = 1002 };
    GtkWidget* dialog = gtk_dialog_new_with_buttons("Audio settings",
                                                    GTK_WINDOW(window_),
                                                    GTK_DIALOG_MODAL,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Save", RESPONSE_APPLY_CLOSE,
                                                    NULL);
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 14);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_pack_start(GTK_BOX(content), grid, FALSE, FALSE, 0);

    style_dialog_action_area(dialog);

    GtkWidget* lbl_device = gtk_label_new("Device:");
    gtk_label_set_xalign(GTK_LABEL(lbl_device), 0.0f);

    GtkWidget* combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "default", "default");

    int active_index = 0;
    int combo_index = 1;
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        const CardProfileInfo& card = cards_[i];
        const std::string label = card.hw_device + " — " + card.short_name +
                                  (!card.pcm_device_name.empty() ? " [" + card.pcm_device_name + "]" : "");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), card.hw_device.c_str(), label.c_str());
        if (card.hw_device == current_device_) {
            active_index = combo_index;
        }
        ++combo_index;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active_index);

    GtkWidget* dsp_title = gtk_label_new("Hardware mixer / low-level controls:");
    gtk_label_set_xalign(GTK_LABEL(dsp_title), 0.0f);

    GtkWidget* dsp_status = gtk_label_new(current_dsp_info_.status_text.empty() ? "No DSP information" : current_dsp_info_.status_text.c_str());
    gtk_label_set_xalign(GTK_LABEL(dsp_status), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(dsp_status), TRUE);

    GtkWidget* dsp_diag = gtk_label_new(current_dsp_info_.diagnostics_text.empty() ? "" : current_dsp_info_.diagnostics_text.c_str());
    gtk_label_set_xalign(GTK_LABEL(dsp_diag), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(dsp_diag), TRUE);

    int selected_card = current_card_index(cards_, current_device_);
    if (selected_card < 0 && !cards_.empty()) {
        selected_card = cards_.front().card_index;
    }
    std::string mixer_cmd = selected_card >= 0 ? ("alsamixer -c " + std::to_string(selected_card)) : std::string("alsamixer");
    GtkWidget* mixer_info = gtk_label_new((std::string("For safe low-level control use: ") + mixer_cmd).c_str());
    gtk_label_set_xalign(GTK_LABEL(mixer_info), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(mixer_info), TRUE);

    GtkWidget* advanced_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(advanced_sep, 8);
    gtk_widget_set_margin_bottom(advanced_sep, 8);
    GtkWidget* advanced_title = gtk_label_new("Advanced audio:");
    gtk_label_set_xalign(GTK_LABEL(advanced_title), 0.0f);

    GtkWidget* lbl_alsa_24 = gtk_label_new("24-bit ALSA container:");
    gtk_label_set_xalign(GTK_LABEL(lbl_alsa_24), 0.0f);
    GtkWidget* alsa_24_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(alsa_24_combo), "auto", "Auto");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(alsa_24_combo), "s24le", "Prefer S24_LE");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(alsa_24_combo), "s24_3le", "Prefer S24_3LE");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(alsa_24_combo), "s32le", "Prefer S32_LE");
    gtk_combo_box_set_active(GTK_COMBO_BOX(alsa_24_combo), alsa_24bit_preference_combo_index(alsa_24bit_container_preference_));

    const std::string rt_status_text = realtime_settings_status_text(engine_);
    GtkWidget* rt_check = gtk_check_button_new_with_label("Use realtime audio thread priority");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rt_check), realtime_audio_priority_enabled_ ? TRUE : FALSE);
    gtk_widget_set_tooltip_text(rt_check, "Tries direct SCHED_RR first. Uses RTKit as a runtime fallback when available. Persistent permission can be granted with cap_sys_nice.");
    GtkWidget* rt_status = gtk_label_new(nullptr);
    set_realtime_status_label(rt_status, rt_status_text);
    gtk_widget_set_hexpand(rt_status, FALSE);

    GtkWidget* rt_grant_button = gtk_button_new_with_label("Grant persistent RT permission");
    GtkWidget* rt_revoke_button = gtk_button_new_with_label("Revoke RT permission");
    gtk_widget_set_tooltip_text(rt_grant_button, "Runs pkexec setcap cap_sys_nice=eip on the current PCM Transport executable. Restart required.");
    gtk_widget_set_tooltip_text(rt_revoke_button, "Runs pkexec setcap -r on the current PCM Transport executable. Restart required.");

    GtkWidget* alsa24_row = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(alsa24_row), 8);
    gtk_grid_attach(GTK_GRID(alsa24_row), lbl_alsa_24, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(alsa24_row), alsa_24_combo, 1, 0, 1, 1);

    GtkWidget* rt_row = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(rt_row), 8);
    gtk_grid_attach(GTK_GRID(rt_row), rt_check, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(rt_row), rt_grant_button, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(rt_row), rt_revoke_button, 2, 0, 1, 1);

    GtkWidget* ui_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(ui_sep, 8);
    gtk_widget_set_margin_bottom(ui_sep, 8);
    GtkWidget* ui_title = gtk_label_new("UI / display:");
    gtk_label_set_xalign(GTK_LABEL(ui_title), 0.0f);
    GtkWidget* level_meter_check = gtk_check_button_new_with_label("Enable level meter measurement");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(level_meter_check), level_meter_enabled_ ? TRUE : FALSE);
    GtkWidget* clip_detect_check = gtk_check_button_new_with_label("Enable clip detection");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(clip_detect_check), clip_detection_enabled_ ? TRUE : FALSE);
    GtkWidget* progress_blink_check = gtk_check_button_new_with_label("Animate progress bar cell");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(progress_blink_check), progress_blink_enabled_ ? TRUE : FALSE);
    GtkWidget* log_title = gtk_label_new("Logging:");
    gtk_label_set_xalign(GTK_LABEL(log_title), 0.0f);
    GtkWidget* log_check = gtk_check_button_new_with_label("Enable log");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(log_check), logging_enabled_ ? TRUE : FALSE);
    GtkWidget* log_mode_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(log_mode_combo), "All events");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(log_mode_combo), "Errors only");
    gtk_combo_box_set_active(GTK_COMBO_BOX(log_mode_combo), log_errors_only_ ? 1 : 0);
    gtk_widget_set_hexpand(log_mode_combo, TRUE);

    GtkWidget* log_path_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(log_path_entry), log_path_.c_str());
    gtk_widget_set_hexpand(log_path_entry, TRUE);
    GtkWidget* log_browse_button = gtk_button_new_with_label("Browse");
    update_log_path_tooltip(log_path_entry);

    GtkWidget* lbl_logmode = gtk_label_new("Log mode:");
    gtk_label_set_xalign(GTK_LABEL(lbl_logmode), 0.0f);
    GtkWidget* lbl_logfile = gtk_label_new("Log file:");
    gtk_label_set_xalign(GTK_LABEL(lbl_logfile), 0.0f);

    GtkWidget* log_row_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(log_row_grid), 8);
    gtk_widget_set_hexpand(log_row_grid, TRUE);
    gtk_grid_attach(GTK_GRID(log_row_grid), lbl_logmode, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(log_row_grid), log_mode_combo, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(log_row_grid), lbl_logfile, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(log_row_grid), log_path_entry, 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(log_row_grid), log_browse_button, 4, 0, 1, 1);
    gtk_widget_set_hexpand(log_path_entry, TRUE);

    GtkWidget* log_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(log_sep, 8);
    gtk_widget_set_margin_bottom(log_sep, 8);

    gtk_grid_attach(GTK_GRID(grid), lbl_device, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), combo, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), dsp_title, 0, 1, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), dsp_status, 0, 2, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), dsp_diag, 0, 3, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), mixer_info, 0, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), advanced_sep, 0, 5, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), advanced_title, 0, 6, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), alsa24_row, 0, 7, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), rt_row, 0, 8, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), rt_status, 0, 9, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), ui_sep, 0, 10, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), ui_title, 0, 11, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), level_meter_check, 0, 12, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), clip_detect_check, 0, 13, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), progress_blink_check, 0, 14, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), log_sep, 0, 15, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), log_title, 0, 16, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), log_check, 0, 17, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), log_row_grid, 0, 18, 2, 1);

    g_signal_connect(log_path_entry, "changed", G_CALLBACK(+[](GtkEditable* editable, gpointer) {
        update_log_path_tooltip(GTK_WIDGET(editable));
    }), nullptr);
    g_object_set_data(G_OBJECT(log_browse_button), "log-path-entry", log_path_entry);
    g_signal_connect(log_browse_button, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        if (self == nullptr) {
            return;
        }
        GtkWidget* entry = GTK_WIDGET(g_object_get_data(G_OBJECT(button), "log-path-entry"));
        GtkWidget* chooser = gtk_file_chooser_dialog_new("Select log folder",
                                                         GTK_WINDOW(self->window_),
                                                         GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                         "_Cancel", GTK_RESPONSE_CANCEL,
                                                         "_Select", GTK_RESPONSE_ACCEPT,
                                                         NULL);
        const gchar* current = entry != nullptr ? gtk_entry_get_text(GTK_ENTRY(entry)) : nullptr;
        const std::string current_text = current != nullptr ? std::string(current) : std::string("pcm_transport.log");
        const std::string current_path = effective_log_path_for_display(current_text);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), directory_name(current_path).c_str());
        const int response = gtk_dialog_run(GTK_DIALOG(chooser));
        if (response == GTK_RESPONSE_ACCEPT) {
            char* folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
            if (folder != nullptr && entry != nullptr) {
                const std::string name = base_name(current_text.empty() ? std::string("pcm_transport.log") : current_text);
                const std::string selected = std::string(folder) + "/" + (name.empty() ? std::string("pcm_transport.log") : name);
                gtk_entry_set_text(GTK_ENTRY(entry), selected.c_str());
                g_free(folder);
            }
        }
        gtk_widget_destroy(chooser);
    }), this);


    g_object_set_data(G_OBJECT(rt_grant_button), "rt-status-label", rt_status);
    g_signal_connect(rt_grant_button, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        if (self == nullptr) return;
        const std::string result = apply_persistent_rt_permission(true);
        GtkWidget* status_label = GTK_WIDGET(g_object_get_data(G_OBJECT(button), "rt-status-label"));
        if (status_label != nullptr) {
            set_realtime_status_label(status_label, self->engine_.refresh_realtime_priority_status() + "\n" + persistent_rt_permission_status() + "\n" + result);
        }
        show_runtime_message(GTK_WINDOW(self->window_), "Realtime permission", result, result.find("failed") == std::string::npos ? GTK_MESSAGE_INFO : GTK_MESSAGE_WARNING);
    }), this);

    g_object_set_data(G_OBJECT(rt_revoke_button), "rt-status-label", rt_status);
    g_signal_connect(rt_revoke_button, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        if (self == nullptr) return;
        const std::string result = apply_persistent_rt_permission(false);
        GtkWidget* status_label = GTK_WIDGET(g_object_get_data(G_OBJECT(button), "rt-status-label"));
        if (status_label != nullptr) {
            set_realtime_status_label(status_label, self->engine_.refresh_realtime_priority_status() + "\n" + persistent_rt_permission_status() + "\n" + result);
        }
        show_runtime_message(GTK_WINDOW(self->window_), "Realtime permission", result, result.find("failed") == std::string::npos ? GTK_MESSAGE_INFO : GTK_MESSAGE_WARNING);
    }), this);

    gtk_widget_show_all(dialog);

    auto apply_settings_from_dialog = [&]() {
        const gchar* selected = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
        if (selected != nullptr) {
            current_device_ = std::string(selected);
        }
        const gchar* alsa24_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(alsa_24_combo));
        alsa_24bit_container_preference_ = normalize_alsa_24bit_preference_id(alsa24_id != nullptr ? std::string(alsa24_id) : std::string("auto"));
        realtime_audio_priority_enabled_ = (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rt_check)) != 0);
        engine_.set_realtime_priority_enabled(realtime_audio_priority_enabled_);
        engine_.set_realtime_priority(60);
        if (realtime_audio_priority_enabled_) {
            engine_.request_realtime_priority_for_playback_thread();
        } else {
            engine_.refresh_realtime_priority_status();
        }
        level_meter_enabled_ = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(level_meter_check)) != 0;
        clip_detection_enabled_ = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(clip_detect_check)) != 0;
        progress_blink_enabled_ = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(progress_blink_check)) != 0;
        engine_.set_level_meter_enabled(level_meter_enabled_);
        engine_.set_clip_detection_enabled(clip_detection_enabled_);
        logging_enabled_ = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(log_check)) != 0;
        log_errors_only_ = gtk_combo_box_get_active(GTK_COMBO_BOX(log_mode_combo)) == 1;
        log_path_ = gtk_entry_get_text(GTK_ENTRY(log_path_entry));
        Logger::instance().configure(logging_enabled_, log_path_, log_errors_only_);
        save_preferences();
        refresh_device_list();
        refresh_dsp_info_for_current_device();
        refresh_display();
    };

    while (true) {
        const int response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (response == RESPONSE_APPLY_CLOSE) {
            apply_settings_from_dialog();
            break;
        }
        break;
    }

    gtk_widget_destroy(dialog);
}

void GtkPlayerWindow::open_about_dialog() {
    enum { RESPONSE_DONATE = 1001, RESPONSE_LICENSE = 1002, RESPONSE_COPY = 1003 };
    GtkWidget* dialog = gtk_dialog_new_with_buttons("About PCM Transport",
                                                    GTK_WINDOW(window_),
                                                    GTK_DIALOG_MODAL,
                                                    "_Donate", RESPONSE_DONATE,
                                                    "_License", RESPONSE_LICENSE,
                                                    "_Close", GTK_RESPONSE_CLOSE,
                                                    NULL);
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    style_dialog_action_area(dialog);
    gtk_container_set_border_width(GTK_CONTAINER(content), 14);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(content), box, TRUE, TRUE, 0);

    GtkWidget* title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<b>PCM Transport 0.9.109</b>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.5f);
    GtkWidget* subtitle = gtk_label_new("Digital Audio Player");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.5f);

    GtkWidget* author = gtk_label_new("Author: Andrey Berestov");
    gtk_label_set_xalign(GTK_LABEL(author), 0.5f);
    gtk_label_set_justify(GTK_LABEL(author), GTK_JUSTIFY_CENTER);

    GtkWidget* website = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(website), "<a href=\"https://andreyberestov.github.io/pcm-transport/\">https://andreyberestov.github.io/pcm-transport/</a>");
    gtk_label_set_xalign(GTK_LABEL(website), 0.5f);
    gtk_label_set_justify(GTK_LABEL(website), GTK_JUSTIFY_CENTER);
    gtk_label_set_selectable(GTK_LABEL(website), TRUE);

    GtkWidget* details = gtk_label_new("License: GNU GPL v3\nTechnologies: C++17, GTK3, Cairo, ALSA, libFLAC, FFmpeg/FFprobe, SoXr resampling, CUE parsing");
    gtk_label_set_xalign(GTK_LABEL(details), 0.5f);
    gtk_label_set_justify(GTK_LABEL(details), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(details), TRUE);

    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), author, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), website, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), details, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);

    while (true) {
        const int response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (response == RESPONSE_DONATE) {
            GtkWidget* msg = gtk_dialog_new_with_buttons("Donate",
                                                         GTK_WINDOW(dialog),
                                                         GTK_DIALOG_MODAL,
                                                         "_Copy", RESPONSE_COPY,
                                                         "_Close", GTK_RESPONSE_CLOSE,
                                                         NULL);
            GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(msg));
            style_dialog_action_area(msg);
            gtk_container_set_border_width(GTK_CONTAINER(area), 14);
            GtkWidget* label = gtk_label_new("If you enjoy PCM Transport, you can buy the author a cup of coffee.\n\nETH / USDT (ERC-20):\n0x37385DA1388F2921583d4750FB44Def7D76cAb23");
            gtk_label_set_xalign(GTK_LABEL(label), 0.5f);
            gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
            gtk_label_set_selectable(GTK_LABEL(label), TRUE);
            gtk_box_pack_start(GTK_BOX(area), label, FALSE, FALSE, 0);
            gtk_widget_show_all(msg);
            while (true) {
                const int r = gtk_dialog_run(GTK_DIALOG(msg));
                if (r == RESPONSE_COPY) {
                    GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
                    gtk_clipboard_set_text(cb, "0x37385DA1388F2921583d4750FB44Def7D76cAb23", -1);
                    gtk_clipboard_store(cb);
                    continue;
                }
                break;
            }
            gtk_widget_destroy(msg);
            continue;
        }
        if (response == RESPONSE_LICENSE) {
            GtkWidget* msg = gtk_dialog_new_with_buttons("GNU GPL v3",
                                                         GTK_WINDOW(dialog),
                                                         GTK_DIALOG_MODAL,
                                                         "_Close", GTK_RESPONSE_CLOSE,
                                                         NULL);
            GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(msg));
            style_dialog_action_area(msg);
            gtk_container_set_border_width(GTK_CONTAINER(area), 14);
            GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
            gtk_widget_set_size_request(scroll, 760, 420);
            gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
            GtkWidget* view = gtk_text_view_new();
            gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
            gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
            gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
            GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
            gtk_text_buffer_set_text(buf, kEmbeddedGplV3Text, -1);
            gtk_container_add(GTK_CONTAINER(scroll), view);
            gtk_box_pack_start(GTK_BOX(area), scroll, TRUE, TRUE, 0);
            gtk_widget_show_all(msg);
            gtk_dialog_run(GTK_DIALOG(msg));
            gtk_widget_destroy(msg);
            continue;
        }
        break;
    }
    gtk_widget_destroy(dialog);
}


struct BitPerfectButtonData {
    GtkPlayerWindow* self = nullptr;
    GtkWidget* parent_dialog = nullptr;
    GtkWidget* duration_combo = nullptr;
};

void destroy_bitperfect_button_data(gpointer data, GClosure*) {
    delete static_cast<BitPerfectButtonData*>(data);
}

void GtkPlayerWindow::on_run_bitperfect_test_clicked(GtkButton*, gpointer user_data) {
    auto* data = static_cast<BitPerfectButtonData*>(user_data);
    if (data == nullptr || data->self == nullptr) return;
    int duration_seconds = 240;
    if (data->duration_combo != nullptr) {
        const gchar* id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(data->duration_combo));
        if (id != nullptr) {
            try { duration_seconds = std::max(1, std::stoi(id)); } catch (...) { duration_seconds = 240; }
        }
    }
    data->self->open_bitperfect_test_dialog(data->parent_dialog, duration_seconds);
}


void GtkPlayerWindow::open_bitperfect_test_dialog(GtkWidget* parent_dialog, int duration_seconds) {
    if (engine_.is_playing()) {
        GtkWidget* ask = gtk_message_dialog_new(GTK_WINDOW(parent_dialog), GTK_DIALOG_MODAL,
                                                GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                                "Playback is currently active. Stop playback and run the test?");
        gtk_dialog_add_button(GTK_DIALOG(ask), "_Cancel", GTK_RESPONSE_CANCEL);
        gtk_dialog_add_button(GTK_DIALOG(ask), "_Stop and Run", GTK_RESPONSE_ACCEPT);
        style_dialog_action_area(ask);
        const int answer = gtk_dialog_run(GTK_DIALOG(ask));
        gtk_widget_destroy(ask);
        if (answer != GTK_RESPONSE_ACCEPT) return;
        stop_playback();
    }

    GtkWidget* dialog = gtk_dialog_new_with_buttons("FLAC bit-perfect test",
                                                    GTK_WINDOW(parent_dialog),
                                                    GTK_DIALOG_MODAL,
                                                    NULL);
    GtkWidget* close_button = gtk_dialog_add_button(GTK_DIALOG(dialog), "_Close", GTK_RESPONSE_CLOSE);
    gtk_widget_set_sensitive(close_button, FALSE);
    gtk_window_set_deletable(GTK_WINDOW(dialog), FALSE);
    style_dialog_action_area(dialog);
    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(area), 14);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_size_request(box, 760, 420);
    gtk_box_pack_start(GTK_BOX(area), box, TRUE, TRUE, 0);

    GtkWidget* title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<b>FLAC bit-perfect diagnostic</b>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

    GtkWidget* note = gtk_label_new("libFLAC / flac CLI only. FFmpeg is not used. The comparison is made before ALSA output.");
    gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(note), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(note), 74);
    gtk_widget_set_hexpand(note, FALSE);
    gtk_widget_set_vexpand(note, FALSE);
    gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);

    GtkWidget* progress = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(box), progress, FALSE, FALSE, 0);

    GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(box), scrolled, TRUE, TRUE, 0);
    GtkWidget* text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scrolled), text_view);

    const int soft_volume = soft_volume_percent_;
    const int bass_db = bass_db_;
    const int treble_db = treble_db_;
    const int headroom = effective_pre_eq_headroom_tenths_db();
    const bool deep_bass = deep_bass_enabled_;
    const int deep_bass_preset = deep_bass_internal_from_ui(deep_bass_preset_);
    const int deep_bass_amount = deep_bass_amount_;
    const int deep_bass_amount_dsp = deep_bass_dsp_amount_from_ui(deep_bass_amount_);
    const int bass_hz = bass_shelf_hz_;
    const int treble_hz = treble_shelf_hz_;
    const bool level_meter = level_meter_enabled_;
    const bool clip_detection = clip_detection_enabled_;

    gtk_widget_show_all(dialog);

    std::thread([=]() {
        std::string tmp_dir;
        auto cleanup = [&]() {
            if (!tmp_dir.empty()) {
                const std::string cmd = std::string("rm -rf -- ") + shell_quote(tmp_dir);
                std::system(cmd.c_str());
            }
        };
        try {
            post_diagnostics_update(text_view, progress, close_button,
                "PCM Transport FLAC bit-perfect test\nVersion: 0.9.109\nMode: current player processing path before ALSA\nFFmpeg: not used\n", 0.02, false);
            std::ostringstream ctx;
            ctx << "Duration: " << duration_seconds << " sec\n"
                << "Generated signal: deterministic 16-bit / 44.1 kHz / stereo stress pattern\n"
                << "Processing context:\n"
                << "  Soft volume: " << soft_volume << "%\n"
                << "  Bass: " << bass_db << " dB @ " << bass_hz << " Hz\n"
                << "  Treble: " << treble_db << " dB @ " << treble_hz << " Hz\n"
                << "  Pre-EQ headroom: " << (static_cast<double>(headroom) / 10.0) << " dB\n"
                << "  Deep Bass: " << (deep_bass ? "enabled" : "disabled") << "\n"
                << "  Deep Bass amount: " << format_signed_step(deep_bass_amount) << "\n"
                << "  Level meter: " << (level_meter ? "enabled" : "disabled") << "\n"
                << "  Clip detection: " << (clip_detection ? "enabled" : "disabled") << "\n\n";
            post_diagnostics_update(text_view, progress, close_button, ctx.str(), 0.05, false);

            char tmpl[] = "/tmp/pcm_transport_bitperfect_XXXXXX";
            char* made = g_mkdtemp(tmpl);
            if (made == nullptr) throw std::runtime_error(std::string("Cannot create temp directory: ") + std::strerror(errno));
            tmp_dir = made;
            const std::string source_wav = tmp_dir + "/source.wav";
            const std::string test_flac = tmp_dir + "/source.flac";
            const std::string reference_wav = tmp_dir + "/reference.wav";

            post_diagnostics_update(text_view, progress, close_button, "Generating deterministic WAV...\n", 0.12, false);
            write_test_wav(source_wav, duration_seconds);

            gchar* flac_cli = g_find_program_in_path("flac");
            if (flac_cli == nullptr) throw std::runtime_error("External flac CLI was not found in PATH");
            g_free(flac_cli);

            post_diagnostics_update(text_view, progress, close_button, "Encoding WAV to FLAC using flac CLI...\n", 0.25, false);
            std::string cmd = "flac -f -s " + shell_quote(source_wav) + " -o " + shell_quote(test_flac);
            if (std::system(cmd.c_str()) != 0) throw std::runtime_error("flac CLI encode failed");

            post_diagnostics_update(text_view, progress, close_button, "Decoding reference using flac -d -c...\n", 0.42, false);
            cmd = "flac -d -c -s " + shell_quote(test_flac) + " > " + shell_quote(reference_wav);
            if (std::system(cmd.c_str()) != 0) throw std::runtime_error("flac CLI reference decode failed");
            const WavPcm16Data ref = read_wav_pcm16(reference_wav);

            post_diagnostics_update(text_view, progress, close_button, "Decoding with PCM Transport internal libFLAC path...\n", 0.62, false);
            const std::vector<std::int16_t> internal = render_internal_path_16(test_flac, soft_volume, bass_db, treble_db, headroom,
                                                                               deep_bass, deep_bass_preset, deep_bass_amount_dsp,
                                                                               bass_hz, treble_hz);

            post_diagnostics_update(text_view, progress, close_button, "Comparing samples...\n", 0.82, false);
            const CompareResult result = compare_samples(ref.samples, internal);
            std::ostringstream out;
            out << "\n" << (result.pass ? "PASS" : "FAIL") << "\n"
                << "Compared samples: " << result.compared << "\n"
                << "Compared frames: " << (result.compared / 2) << "\n"
                << "Max absolute difference: " << result.max_diff << "\n";
            if (!result.pass) {
                out << "First mismatch:\n"
                    << "  Sample index: " << result.first_mismatch << "\n"
                    << "  Frame: " << (result.first_mismatch / 2) << "\n"
                    << "  Channel: " << ((result.first_mismatch % 2) == 0 ? "L" : "R") << "\n"
                    << "  Expected: " << result.expected << "\n"
                    << "  Actual: " << result.actual << "\n"
                    << "  Difference: " << (static_cast<int>(result.actual) - static_cast<int>(result.expected)) << "\n";
                if (soft_volume < 100 || bass_db != 0 || treble_db != 0 || headroom > 0 || deep_bass) {
                    out << "Note: FAIL can be expected when DSP, soft volume, headroom or Deep Bass is enabled.\n";
                } else {
                    out << "Warning: pure path differs from reference. This should be investigated.\n";
                }
            }
            out << "Temporary files removed.\n";
            cleanup();
            post_diagnostics_update(text_view, progress, close_button, out.str(), 1.0, true);
        } catch (const std::exception& ex) {
            cleanup();
            post_diagnostics_update(text_view, progress, close_button, std::string("\nERROR: ") + ex.what() + "\nTemporary files removed.\n", 1.0, true);
        }
    }).detach();

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void GtkPlayerWindow::open_eq_dialog() {
    enum { RESPONSE_RESET = 1001 };
    GtkWidget* dialog = gtk_dialog_new_with_buttons("DSP Studio",
                                                    GTK_WINDOW(window_),
                                                    GTK_DIALOG_MODAL,
                                                    "_Close", GTK_RESPONSE_CLOSE,
                                                    "_Reset", RESPONSE_RESET,
                                                    NULL);
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    style_dialog_action_area(dialog);
    gtk_container_set_border_width(GTK_CONTAINER(content), 14);
    GtkWidget* notebook = gtk_notebook_new();
    gtk_widget_set_hexpand(notebook, TRUE);
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_box_pack_start(GTK_BOX(content), notebook, TRUE, TRUE, 0);

    GtkWidget* root = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(root), 14);
    gtk_grid_set_column_spacing(GTK_GRID(root), 0);
    gtk_widget_set_hexpand(root, TRUE);
    gtk_widget_set_vexpand(root, TRUE);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), root, gtk_label_new("Tone / Deep Bass"));

    GtkWidget* processing_root = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(processing_root), 14);
    gtk_grid_set_column_spacing(GTK_GRID(processing_root), 0);
    gtk_widget_set_hexpand(processing_root, TRUE);
    gtk_widget_set_vexpand(processing_root, TRUE);
    gtk_widget_set_margin_start(processing_root, 12);
    gtk_widget_set_margin_end(processing_root, 12);
    gtk_widget_set_margin_top(processing_root, 12);
    gtk_widget_set_margin_bottom(processing_root, 12);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), processing_root, gtk_label_new("Processing Rules"));


    GtkWidget* diagnostics_root = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(diagnostics_root), 14);
    gtk_grid_set_column_spacing(GTK_GRID(diagnostics_root), 0);
    gtk_widget_set_hexpand(diagnostics_root, TRUE);
    gtk_widget_set_vexpand(diagnostics_root, TRUE);
    gtk_widget_set_margin_start(diagnostics_root, 12);
    gtk_widget_set_margin_end(diagnostics_root, 12);
    gtk_widget_set_margin_top(diagnostics_root, 12);
    gtk_widget_set_margin_bottom(diagnostics_root, 12);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), diagnostics_root, gtk_label_new("Diagnostics / Tests"));
    int root_row = 0;
    int processing_row = 0;
    int diagnostics_row = 0;
    auto attach_root = [&](GtkWidget* widget) {
        gtk_widget_set_hexpand(widget, TRUE);
        gtk_grid_attach(GTK_GRID(root), widget, 0, root_row++, 1, 1);
    };
    auto attach_processing = [&](GtkWidget* widget) {
        gtk_widget_set_hexpand(widget, TRUE);
        gtk_grid_attach(GTK_GRID(processing_root), widget, 0, processing_row++, 1, 1);
    };
    auto attach_diagnostics = [&](GtkWidget* widget) {
        gtk_widget_set_hexpand(widget, TRUE);
        gtk_grid_attach(GTK_GRID(diagnostics_root), widget, 0, diagnostics_row++, 1, 1);
    };

    auto make_header = [](const char* title, const char* desc) -> GtkWidget* {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        GtkWidget* title_label = gtk_label_new(nullptr);
        std::string markup = std::string("<b>") + title + "</b>";
        gtk_label_set_markup(GTK_LABEL(title_label), markup.c_str());
        gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
        GtkWidget* desc_label = gtk_label_new(desc);
        gtk_label_set_xalign(GTK_LABEL(desc_label), 0.0f);
        gtk_label_set_line_wrap(GTK_LABEL(desc_label), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(desc_label), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(desc_label), 74);
        gtk_widget_set_hexpand(desc_label, FALSE);
        gtk_widget_set_vexpand(desc_label, FALSE);
        gtk_box_pack_start(GTK_BOX(box), title_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), desc_label, FALSE, FALSE, 0);
        return box;
    };

    const gint section_indent = 32;
    const gint row_spacing = 10;
    const gint col_spacing = 10;
    const gint section_bottom = 8;
    const gint subsection_gap = 6;

    GtkWidget* volume_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(volume_grid), row_spacing);
    gtk_grid_set_column_spacing(GTK_GRID(volume_grid), col_spacing);
    attach_root(make_header("Master DSP Volume", "Shared soft-volume stage for all DSP processing."));
    GtkWidget* volume_label = gtk_label_new("Volume:");
    gtk_widget_set_valign(volume_label, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(volume_label), 0.0f);
    GtkWidget* volume_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    GtkWidget* pre_eq_headroom_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 15.0, 0.1);
    gtk_scale_set_draw_value(GTK_SCALE(volume_scale), TRUE);
    gtk_widget_set_hexpand(volume_scale, TRUE);
    gtk_range_set_value(GTK_RANGE(volume_scale), static_cast<double>(soft_volume_percent_));
    gtk_scale_set_draw_value(GTK_SCALE(pre_eq_headroom_scale), TRUE);
    gtk_scale_set_digits(GTK_SCALE(pre_eq_headroom_scale), 1);
    gtk_widget_set_hexpand(pre_eq_headroom_scale, TRUE);
    gtk_range_set_value(GTK_RANGE(pre_eq_headroom_scale), static_cast<double>(effective_pre_eq_headroom_tenths_db()) / 10.0);
    gtk_widget_set_tooltip_text(pre_eq_headroom_scale, "Automatically set from Bass/Treble.\nYou can adjust it manually.\nManual adjustment is reset when Bass or Treble changes.");
    gtk_grid_attach(GTK_GRID(volume_grid), volume_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(volume_grid), volume_scale, 1, 0, 1, 1);
    attach_root(volume_grid);
    gtk_widget_set_margin_bottom(volume_grid, subsection_gap);

    GtkWidget* headroom_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(headroom_grid), row_spacing);
    gtk_grid_set_column_spacing(GTK_GRID(headroom_grid), col_spacing);
    GtkWidget* headroom_label = gtk_label_new("Pre-EQ Headroom:");
    gtk_widget_set_valign(headroom_label, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(headroom_label), 0.0f);
    gtk_widget_set_tooltip_text(headroom_label, "Automatically set from Bass/Treble.\nYou can adjust it manually.\nManual adjustment is reset when Bass or Treble changes.");
    gtk_grid_attach(GTK_GRID(headroom_grid), headroom_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(headroom_grid), pre_eq_headroom_scale, 1, 0, 1, 1);
    attach_root(headroom_grid);
    gtk_widget_set_margin_bottom(headroom_grid, subsection_gap);
    GtkWidget* sep_volume_deep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep_volume_deep, 2);
    gtk_widget_set_margin_bottom(sep_volume_deep, 2);
    attach_root(sep_volume_deep);

    attach_root(make_header("Deep Bass", "Adaptive bass enhancement with contour shaping, harmonic reinforcement and controlled cleanup."));
    GtkWidget* deep_bass_row_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(deep_bass_row_grid), row_spacing);
    gtk_grid_set_column_spacing(GTK_GRID(deep_bass_row_grid), col_spacing);
    GtkWidget* deep_bass_check = gtk_check_button_new_with_label("Deep Bass");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(deep_bass_check), deep_bass_enabled_);
    gtk_widget_set_tooltip_text(deep_bass_check, "Deep Bass adds bass shaping, harmonic enhancement and gentle nonlinear reinforcement.\nIt is a separate layer above the clean tone controls.");
    GtkWidget* deep_bass_preset_label = gtk_label_new("Character:");
    gtk_widget_set_valign(deep_bass_preset_label, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(deep_bass_preset_label), 1.0f);
    GtkWidget* deep_bass_preset_combo = gtk_combo_box_text_new();
    for (const auto& preset : kDeepBassPresets) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(deep_bass_preset_combo), preset.id, preset.label);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(deep_bass_preset_combo), clamp_deep_bass_preset_ui(deep_bass_preset_));
    gtk_widget_set_hexpand(deep_bass_preset_combo, TRUE);
    gtk_grid_attach(GTK_GRID(deep_bass_row_grid), deep_bass_check, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(deep_bass_row_grid), deep_bass_preset_label, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(deep_bass_row_grid), deep_bass_preset_combo, 2, 0, 1, 1);
    gtk_widget_set_margin_start(deep_bass_row_grid, section_indent);
    gtk_widget_set_margin_bottom(deep_bass_row_grid, subsection_gap);
    attach_root(deep_bass_row_grid);

    GtkWidget* deep_bass_amount_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(deep_bass_amount_grid), row_spacing);
    gtk_grid_set_column_spacing(GTK_GRID(deep_bass_amount_grid), col_spacing);
    GtkWidget* deep_bass_amount_label = gtk_label_new("Amount:");
    gtk_widget_set_valign(deep_bass_amount_label, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(deep_bass_amount_label), 0.0f);
    GtkWidget* deep_bass_amount_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -1, 1, 1);
    gtk_scale_set_draw_value(GTK_SCALE(deep_bass_amount_scale), TRUE);
    gtk_scale_set_digits(GTK_SCALE(deep_bass_amount_scale), 0);
    gtk_scale_add_mark(GTK_SCALE(deep_bass_amount_scale), -1, GTK_POS_BOTTOM, "-1");
    gtk_scale_add_mark(GTK_SCALE(deep_bass_amount_scale), 0, GTK_POS_BOTTOM, "0");
    gtk_scale_add_mark(GTK_SCALE(deep_bass_amount_scale), 1, GTK_POS_BOTTOM, "+1");
    gtk_widget_set_hexpand(deep_bass_amount_scale, TRUE);
    gtk_range_set_value(GTK_RANGE(deep_bass_amount_scale), static_cast<double>(deep_bass_amount_));
    gtk_widget_set_tooltip_text(deep_bass_amount_scale, "Scales the final Deep Bass contribution without changing the Reference/Punch character. -1 is lighter, 0 is the baseline, +1 is stronger.");
    gtk_grid_attach(GTK_GRID(deep_bass_amount_grid), deep_bass_amount_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(deep_bass_amount_grid), deep_bass_amount_scale, 1, 0, 1, 1);
    gtk_widget_set_margin_start(deep_bass_amount_grid, section_indent);
    gtk_widget_set_margin_bottom(deep_bass_amount_grid, subsection_gap);
    attach_root(deep_bass_amount_grid);
    GtkWidget* sep_deep_tone = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep_deep_tone, 2);
    gtk_widget_set_margin_bottom(sep_deep_tone, 2);
    attach_root(sep_deep_tone);

    attach_root(make_header("Bass / Treble", "Musical Baxandall-like shelves for low and high tone shaping."));
    GtkWidget* tone_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(tone_grid), row_spacing);
    gtk_grid_set_column_spacing(GTK_GRID(tone_grid), col_spacing);
    GtkWidget* bass_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -12, 12, 1);
    GtkWidget* treble_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -12, 12, 1);
    gtk_scale_set_draw_value(GTK_SCALE(bass_scale), TRUE);
    gtk_scale_set_draw_value(GTK_SCALE(treble_scale), TRUE);
    gtk_widget_set_hexpand(bass_scale, TRUE);
    gtk_widget_set_hexpand(treble_scale, TRUE);
    gtk_range_set_value(GTK_RANGE(bass_scale), static_cast<double>(bass_db_));
    gtk_range_set_value(GTK_RANGE(treble_scale), static_cast<double>(treble_db_));
    GtkWidget* preset_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(preset_combo), "Reference Baxandall (100 Hz / 10 kHz)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(preset_combo), "Console Tone (85 Hz / 8 kHz)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(preset_combo), "Broadcast Sweetening (120 Hz / 6.5 kHz)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(preset_combo), "Air & Weight (70 Hz / 12 kHz)");
    int preset_index = 0;
    if (bass_shelf_hz_ == 85 && treble_shelf_hz_ == 8000) preset_index = 1;
    else if (bass_shelf_hz_ == 120 && treble_shelf_hz_ == 6500) preset_index = 2;
    else if (bass_shelf_hz_ == 70 && treble_shelf_hz_ == 12000) preset_index = 3;
    gtk_combo_box_set_active(GTK_COMBO_BOX(preset_combo), preset_index);
    g_object_set_data(G_OBJECT(bass_scale), "pre-eq-headroom-scale", pre_eq_headroom_scale);
    g_object_set_data(G_OBJECT(treble_scale), "pre-eq-headroom-scale", pre_eq_headroom_scale);
    g_object_set_data(G_OBJECT(deep_bass_check), "pre-eq-headroom-scale", pre_eq_headroom_scale);
    GtkWidget* bass_label = gtk_label_new("Bass:");
    gtk_widget_set_valign(bass_label, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(bass_label), 0.0f);
    gtk_grid_attach(GTK_GRID(tone_grid), bass_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(tone_grid), bass_scale, 1, 0, 1, 1);
    GtkWidget* treble_label = gtk_label_new("Treble:");
    gtk_widget_set_valign(treble_label, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(treble_label), 0.0f);
    gtk_grid_attach(GTK_GRID(tone_grid), treble_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(tone_grid), treble_scale, 1, 1, 1, 1);
    GtkWidget* shelf_pair_label = gtk_label_new("Shelf pair:");
    gtk_widget_set_valign(shelf_pair_label, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(shelf_pair_label), 0.0f);
    gtk_grid_attach(GTK_GRID(tone_grid), shelf_pair_label, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(tone_grid), preset_combo, 1, 2, 1, 1);
    attach_root(tone_grid);
    gtk_widget_set_margin_bottom(tone_grid, subsection_gap);

    GtkWidget* tone_graph = gtk_drawing_area_new();
    gtk_widget_set_size_request(tone_graph, 520, 170);
    gtk_widget_set_hexpand(tone_graph, TRUE);
    auto* tone_graph_data = new ToneGraphData{this};
    g_signal_connect_data(tone_graph, "draw", G_CALLBACK(+[](GtkWidget* widget, cairo_t* cr, gpointer user_data) -> gboolean {
        auto* data = static_cast<ToneGraphData*>(user_data);
        GtkAllocation alloc{};
        gtk_widget_get_allocation(widget, &alloc);
data->self->draw_tone_response_graph(cr, alloc.width, alloc.height);
        return FALSE;
    }), tone_graph_data, +[](gpointer data, GClosure*) { delete static_cast<ToneGraphData*>(data); }, static_cast<GConnectFlags>(0));
    attach_root(tone_graph);
    gtk_widget_set_margin_start(tone_graph, section_indent);
    gtk_widget_set_margin_end(tone_graph, section_indent);
    gtk_widget_set_margin_bottom(tone_graph, section_bottom - 2);
    g_object_set_data(G_OBJECT(bass_scale), "tone-graph", tone_graph);
    g_object_set_data(G_OBJECT(treble_scale), "tone-graph", tone_graph);
    g_object_set_data(G_OBJECT(preset_combo), "tone-graph", tone_graph);
    g_object_set_data(G_OBJECT(deep_bass_check), "tone-graph", tone_graph);
    g_object_set_data(G_OBJECT(deep_bass_preset_combo), "tone-graph", tone_graph);
    g_object_set_data(G_OBJECT(deep_bass_amount_scale), "tone-graph", tone_graph);
    g_object_set_data(G_OBJECT(deep_bass_preset_combo), "pre-eq-headroom-scale", pre_eq_headroom_scale);
    g_object_set_data(G_OBJECT(deep_bass_amount_scale), "pre-eq-headroom-scale", pre_eq_headroom_scale);
    g_object_set_data(G_OBJECT(preset_combo), "pre-eq-headroom-scale", pre_eq_headroom_scale);

    attach_processing(make_header("Processing Rules", "Optional SoXr resampling and bit-depth conversion rules. Leave empty for native playback."));
    GtkWidget* rules_columns = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(rules_columns), 0);
    gtk_widget_set_hexpand(rules_columns, TRUE);
    attach_processing(rules_columns);

    GtkWidget* resample_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget* bit_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(resample_col, TRUE);
    gtk_widget_set_hexpand(bit_col, TRUE);
    gtk_widget_set_halign(resample_col, GTK_ALIGN_FILL);
    gtk_widget_set_halign(bit_col, GTK_ALIGN_FILL);
    GtkWidget* rules_separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_size_request(rules_separator, 1, -1);
    gtk_widget_set_margin_start(rules_separator, 10);
    gtk_widget_set_margin_end(rules_separator, 10);
    gtk_grid_attach(GTK_GRID(rules_columns), resample_col, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(rules_columns), rules_separator, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(rules_columns), bit_col, 2, 0, 1, 1);
    GtkSizeGroup* rules_size_group = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    gtk_size_group_add_widget(rules_size_group, resample_col);
    gtk_size_group_add_widget(rules_size_group, bit_col);

    gtk_box_pack_start(GTK_BOX(resample_col), make_header("Resampling Rules", "Optional high-quality SoXr resampling rules."), FALSE, FALSE, 0);
    GtkWidget* quality_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(quality_grid), row_spacing);
    gtk_grid_set_column_spacing(GTK_GRID(quality_grid), col_spacing);
    GtkWidget* resample_quality_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(resample_quality_combo), "maximum", "Maximum (SoXr 33-bit precision)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(resample_quality_combo), "high", "High");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(resample_quality_combo), "balanced", "Balanced");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(resample_quality_combo), "fast", "Fast");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(resample_quality_combo), resample_quality_.c_str());
    GtkWidget* resample_quality_label = gtk_label_new("Resample quality:");
    gtk_widget_set_valign(resample_quality_label, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(resample_quality_label), 0.0f);
    gtk_widget_set_hexpand(resample_quality_combo, TRUE);
    gtk_grid_attach(GTK_GRID(quality_grid), resample_quality_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(quality_grid), resample_quality_combo, 1, 0, 4, 1);
    gtk_widget_set_margin_start(quality_grid, section_indent);
    gtk_box_pack_start(GTK_BOX(resample_col), quality_grid, FALSE, FALSE, 0);

    GtkWidget* rate_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(rate_list, section_indent);
    gtk_widget_set_margin_top(rate_list, 4);
    gtk_widget_set_margin_bottom(rate_list, 8);
    gtk_widget_set_size_request(rate_list, -1, 88);
    auto append_rate_row = [&](std::uint32_t from_rate, std::uint32_t to_rate) {
        GtkWidget* row = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(row), 8);
        gtk_widget_set_hexpand(row, TRUE);
        gtk_widget_set_margin_top(row, 2);
        gtk_widget_set_margin_bottom(row, 2);
        std::string text = std::to_string(from_rate) + " Hz → " + std::to_string(to_rate) + " Hz";
        GtkWidget* label = gtk_label_new(text.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_hexpand(label, TRUE);
        GtkWidget* del = gtk_button_new_with_label("Delete rule");
        gtk_widget_set_valign(del, GTK_ALIGN_CENTER);
        auto* data = new DeleteRateRuleData{this, dialog, row, from_rate, to_rate};
        auto on_delete_rate_clicked = +[](GtkButton*, gpointer user_data) {
            auto* data = static_cast<DeleteRateRuleData*>(user_data);
            for (std::size_t idx = 0; idx < data->self->resample_rules_.size(); ++idx) {
                if (data->self->resample_rules_[idx].from_rate == data->from_rate && data->self->resample_rules_[idx].to_rate == data->to_rate) {
                    data->self->resample_rules_.erase(data->self->resample_rules_.begin() + static_cast<std::ptrdiff_t>(idx));
                    break;
                }
            }
            data->self->refresh_playlist_processing_metadata();
            data->self->save_preferences();
            data->self->refresh_display();
            if (data->row != nullptr) gtk_widget_destroy(data->row);
        };
        g_signal_connect_data(del, "clicked", G_CALLBACK(on_delete_rate_clicked), data, destroy_delete_rate_rule_data, static_cast<GConnectFlags>(0));
        gtk_grid_attach(GTK_GRID(row), label, 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(row), del, 1, 0, 1, 1);
        gtk_box_pack_start(GTK_BOX(rate_list), row, FALSE, FALSE, 0);
        gtk_widget_show_all(row);
    };
    for (const auto& rule : resample_rules_) append_rate_row(rule.from_rate, rule.to_rate);

    GtkWidget* rate_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(rate_grid), row_spacing);
    gtk_grid_set_column_spacing(GTK_GRID(rate_grid), col_spacing);
    GtkWidget* from_combo = gtk_combo_box_text_new();
    GtkWidget* to_combo = gtk_combo_box_text_new();
    const char* rates[] = {"44100", "48000", "88200", "96000", "176400", "192000"};
    for (const char* rate : rates) { gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(from_combo), rate); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(to_combo), rate); }
    gtk_combo_box_set_active(GTK_COMBO_BOX(from_combo), 0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(to_combo), 1);
    GtkWidget* add_rate_btn = gtk_button_new_with_label("Add rule");
    GtkWidget* rate_from_label = gtk_label_new("From:");
    gtk_widget_set_valign(rate_from_label, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(rate_from_label), 0.0f);
    gtk_grid_attach(GTK_GRID(rate_grid), rate_from_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(rate_grid), from_combo, 1, 0, 1, 1);
    GtkWidget* rate_to_label = gtk_label_new("To:");
    gtk_widget_set_valign(rate_to_label, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(rate_to_label), 0.0f);
    gtk_grid_attach(GTK_GRID(rate_grid), rate_to_label, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(rate_grid), to_combo, 3, 0, 1, 1);
    gtk_widget_set_valign(add_rate_btn, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(rate_grid), add_rate_btn, 4, 0, 1, 1);
    gtk_widget_set_hexpand(from_combo, TRUE);
    gtk_widget_set_hexpand(to_combo, TRUE);
    gtk_widget_set_margin_start(rate_grid, section_indent);
    gtk_widget_set_margin_top(rate_grid, 8);
    gtk_widget_set_margin_bottom(rate_grid, 6);
    gtk_box_pack_start(GTK_BOX(resample_col), rate_grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(resample_col), rate_list, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bit_col), make_header("Bit-Depth Rules", "Optional compatibility rules, for example 24-bit → 16-bit."), FALSE, FALSE, 0);
    GtkWidget* bitdepth_quality_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(bitdepth_quality_combo), "tpdf_hp", "TPDF high-pass dither");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(bitdepth_quality_combo), "tpdf", "TPDF dither");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(bitdepth_quality_combo), "rectangular", "Rectangular dither");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(bitdepth_quality_combo), bitdepth_quality_.c_str());
    GtkWidget* bit_quality_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(bit_quality_grid), row_spacing);
    gtk_grid_set_column_spacing(GTK_GRID(bit_quality_grid), col_spacing);
    GtkWidget* conversion_quality_label = gtk_label_new("Bit depth dither:");
    gtk_widget_set_valign(conversion_quality_label, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(conversion_quality_label), 0.0f);
    gtk_widget_set_hexpand(bitdepth_quality_combo, TRUE);
    gtk_grid_attach(GTK_GRID(bit_quality_grid), conversion_quality_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(bit_quality_grid), bitdepth_quality_combo, 1, 0, 4, 1);
    gtk_widget_set_margin_start(bit_quality_grid, section_indent);
    gtk_box_pack_start(GTK_BOX(bit_col), bit_quality_grid, FALSE, FALSE, 0);

    GtkWidget* bit_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(bit_list, section_indent);
    gtk_widget_set_margin_top(bit_list, 4);
    gtk_widget_set_margin_bottom(bit_list, 8);
    gtk_widget_set_size_request(bit_list, -1, 88);

    GtkSizeGroup* rules_list_height_group = gtk_size_group_new(GTK_SIZE_GROUP_VERTICAL);
    gtk_size_group_add_widget(rules_list_height_group, rate_list);
    gtk_size_group_add_widget(rules_list_height_group, bit_list);
    auto append_bit_row = [&](std::uint16_t from_bits, std::uint16_t to_bits) {
        GtkWidget* row = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(row), 8);
        gtk_widget_set_hexpand(row, TRUE);
        gtk_widget_set_margin_top(row, 2);
        gtk_widget_set_margin_bottom(row, 2);
        std::string text = std::to_string(from_bits) + "-bit → " + std::to_string(to_bits) + "-bit";
        GtkWidget* label = gtk_label_new(text.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_hexpand(label, TRUE);
        GtkWidget* del = gtk_button_new_with_label("Delete rule");
        gtk_widget_set_valign(del, GTK_ALIGN_CENTER);
        auto* data = new DeleteBitRuleData{this, dialog, row, from_bits, to_bits};
        auto on_delete_bit_clicked = +[](GtkButton*, gpointer user_data) {
            auto* data = static_cast<DeleteBitRuleData*>(user_data);
            for (std::size_t idx = 0; idx < data->self->bitdepth_rules_.size(); ++idx) {
                if (data->self->bitdepth_rules_[idx].from_bits == data->from_bits && data->self->bitdepth_rules_[idx].to_bits == data->to_bits) {
                    data->self->bitdepth_rules_.erase(data->self->bitdepth_rules_.begin() + static_cast<std::ptrdiff_t>(idx));
                    break;
                }
            }
            data->self->refresh_playlist_processing_metadata();
            data->self->save_preferences();
            data->self->refresh_display();
            if (data->row != nullptr) gtk_widget_destroy(data->row);
        };
        g_signal_connect_data(del, "clicked", G_CALLBACK(on_delete_bit_clicked), data, destroy_delete_bit_rule_data, static_cast<GConnectFlags>(0));
        gtk_grid_attach(GTK_GRID(row), label, 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(row), del, 1, 0, 1, 1);
        gtk_box_pack_start(GTK_BOX(bit_list), row, FALSE, FALSE, 0);
        gtk_widget_show_all(row);
    };
    for (const auto& rule : bitdepth_rules_) append_bit_row(rule.from_bits, rule.to_bits);

    GtkWidget* from_bits_combo = gtk_combo_box_text_new();
    GtkWidget* to_bits_combo = gtk_combo_box_text_new();
    const char* bits[] = {"16", "24", "32"};
    for (const char* bit : bits) { gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(from_bits_combo), bit); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(to_bits_combo), bit); }
    gtk_combo_box_set_active(GTK_COMBO_BOX(from_bits_combo), 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(to_bits_combo), 0);
    GtkWidget* add_bit_btn = gtk_button_new_with_label("Add rule");
    GtkWidget* bit_rule_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(bit_rule_grid), row_spacing);
    gtk_grid_set_column_spacing(GTK_GRID(bit_rule_grid), col_spacing);
    GtkWidget* bit_from_label = gtk_label_new("From:");
    gtk_widget_set_valign(bit_from_label, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(bit_from_label), 0.0f);
    gtk_grid_attach(GTK_GRID(bit_rule_grid), bit_from_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(bit_rule_grid), from_bits_combo, 1, 0, 1, 1);
    GtkWidget* bit_to_label = gtk_label_new("To:");
    gtk_widget_set_valign(bit_to_label, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(bit_to_label), 0.0f);
    gtk_grid_attach(GTK_GRID(bit_rule_grid), bit_to_label, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(bit_rule_grid), to_bits_combo, 3, 0, 1, 1);
    gtk_widget_set_valign(add_bit_btn, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(bit_rule_grid), add_bit_btn, 4, 0, 1, 1);
    gtk_widget_set_hexpand(from_bits_combo, TRUE);
    gtk_widget_set_hexpand(to_bits_combo, TRUE);
    gtk_widget_set_margin_start(bit_rule_grid, section_indent);
    gtk_widget_set_margin_top(bit_rule_grid, 8);
    gtk_widget_set_margin_bottom(bit_rule_grid, 6);
    gtk_box_pack_start(GTK_BOX(bit_col), bit_rule_grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bit_col), bit_list, FALSE, FALSE, 0);

    g_signal_connect(volume_scale, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        self->soft_volume_percent_ = std::max(0, std::min(100, static_cast<int>(std::lround(gtk_range_get_value(range)))));
        self->engine_.set_soft_volume_percent(self->soft_volume_percent_);
        self->save_preferences();
        self->refresh_display();
        self->notify_mpris_state_changed();
        if (self->soft_volume_scale_ != nullptr) gtk_widget_queue_draw(self->soft_volume_scale_);
    }), this);
    g_signal_connect(pre_eq_headroom_scale, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        self->pre_eq_headroom_tenths_db_ = std::max(0, std::min(kUiPreEqHeadroomMaxTenthsDb, static_cast<int>(std::lround(gtk_range_get_value(range) * 10.0))));
        self->engine_.set_pre_eq_headroom_tenths_db(self->pre_eq_headroom_tenths_db_);
        self->save_preferences();
        self->refresh_display();
    }), this);
    g_signal_connect(deep_bass_check, "toggled", G_CALLBACK(+[](GtkToggleButton* btn, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        self->deep_bass_enabled_ = gtk_toggle_button_get_active(btn);
        self->engine_.set_deep_bass_enabled(self->deep_bass_enabled_);
        self->engine_.set_deep_bass_preset(deep_bass_internal_from_ui(self->deep_bass_preset_));
        self->apply_auto_pre_eq_headroom(false);
        GtkWidget* slider = GTK_WIDGET(g_object_get_data(G_OBJECT(btn), "pre-eq-headroom-scale"));
        if (slider != nullptr) gtk_range_set_value(GTK_RANGE(slider), static_cast<double>(self->effective_pre_eq_headroom_tenths_db()) / 10.0);
        GtkWidget* graph = GTK_WIDGET(g_object_get_data(G_OBJECT(btn), "tone-graph"));
        if (graph != nullptr) gtk_widget_queue_draw(graph);
        self->save_preferences();
        self->refresh_display();
    }), this);
    g_signal_connect(deep_bass_preset_combo, "changed", G_CALLBACK(+[](GtkComboBox* combo, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        self->deep_bass_preset_ = clamp_deep_bass_preset_ui(gtk_combo_box_get_active(combo));
        self->engine_.set_deep_bass_preset(deep_bass_internal_from_ui(self->deep_bass_preset_));
        self->apply_auto_pre_eq_headroom(false);
        GtkWidget* slider = GTK_WIDGET(g_object_get_data(G_OBJECT(combo), "pre-eq-headroom-scale"));
        if (slider != nullptr) gtk_range_set_value(GTK_RANGE(slider), static_cast<double>(self->effective_pre_eq_headroom_tenths_db()) / 10.0);
        GtkWidget* graph = GTK_WIDGET(g_object_get_data(G_OBJECT(combo), "tone-graph"));
        if (graph != nullptr) gtk_widget_queue_draw(graph);
        self->save_preferences();
        self->refresh_display();
    }), this);
    g_signal_connect(deep_bass_amount_scale, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        self->deep_bass_amount_ = clamp_deep_bass_amount_ui(static_cast<int>(std::lround(gtk_range_get_value(range))));
        self->engine_.set_deep_bass_amount(deep_bass_dsp_amount_from_ui(self->deep_bass_amount_));
        self->apply_auto_pre_eq_headroom(false);
        GtkWidget* slider = GTK_WIDGET(g_object_get_data(G_OBJECT(range), "pre-eq-headroom-scale"));
        if (slider != nullptr) gtk_range_set_value(GTK_RANGE(slider), static_cast<double>(self->effective_pre_eq_headroom_tenths_db()) / 10.0);
        GtkWidget* graph = GTK_WIDGET(g_object_get_data(G_OBJECT(range), "tone-graph"));
        if (graph != nullptr) gtk_widget_queue_draw(graph);
        self->save_preferences();
        self->refresh_display();
    }), this);
    g_signal_connect(bass_scale, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        self->bass_db_ = static_cast<int>(std::lround(gtk_range_get_value(range)));
        self->engine_.set_soft_eq(self->bass_db_, self->treble_db_);
        self->apply_auto_pre_eq_headroom(false);
        GtkWidget* slider = GTK_WIDGET(g_object_get_data(G_OBJECT(range), "pre-eq-headroom-scale"));
        if (slider != nullptr) gtk_range_set_value(GTK_RANGE(slider), static_cast<double>(self->effective_pre_eq_headroom_tenths_db()) / 10.0);
        GtkWidget* graph = GTK_WIDGET(g_object_get_data(G_OBJECT(range), "tone-graph"));
        if (graph != nullptr) gtk_widget_queue_draw(graph);
        self->save_preferences();
        self->refresh_display();
    }), this);
    g_signal_connect(treble_scale, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        self->treble_db_ = static_cast<int>(std::lround(gtk_range_get_value(range)));
        self->engine_.set_soft_eq(self->bass_db_, self->treble_db_);
        self->apply_auto_pre_eq_headroom(false);
        GtkWidget* slider = GTK_WIDGET(g_object_get_data(G_OBJECT(range), "pre-eq-headroom-scale"));
        if (slider != nullptr) gtk_range_set_value(GTK_RANGE(slider), static_cast<double>(self->effective_pre_eq_headroom_tenths_db()) / 10.0);
        GtkWidget* graph = GTK_WIDGET(g_object_get_data(G_OBJECT(range), "tone-graph"));
        if (graph != nullptr) gtk_widget_queue_draw(graph);
        self->save_preferences();
        self->refresh_display();
    }), this);
    g_signal_connect(preset_combo, "changed", G_CALLBACK(+[](GtkComboBox* combo, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        const int idx = gtk_combo_box_get_active(combo);
        self->bass_shelf_hz_ = 100; self->treble_shelf_hz_ = 10000;
        if (idx == 1) { self->bass_shelf_hz_ = 85; self->treble_shelf_hz_ = 8000; }
        else if (idx == 2) { self->bass_shelf_hz_ = 120; self->treble_shelf_hz_ = 6500; }
        else if (idx == 3) { self->bass_shelf_hz_ = 70; self->treble_shelf_hz_ = 12000; }
        self->engine_.set_soft_eq_profile(self->bass_shelf_hz_, self->treble_shelf_hz_);
        self->apply_auto_pre_eq_headroom(false);
        GtkWidget* graph = GTK_WIDGET(g_object_get_data(G_OBJECT(combo), "tone-graph"));
        if (graph != nullptr) gtk_widget_queue_draw(graph);
        GtkWidget* slider = GTK_WIDGET(g_object_get_data(G_OBJECT(combo), "pre-eq-headroom-scale"));
        if (slider != nullptr) gtk_range_set_value(GTK_RANGE(slider), static_cast<double>(self->effective_pre_eq_headroom_tenths_db()) / 10.0);
        self->save_preferences();
        self->refresh_display();
    }), this);
    g_signal_connect(resample_quality_combo, "changed", G_CALLBACK(+[](GtkComboBox* combo, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        const gchar* id = gtk_combo_box_get_active_id(combo);
        if (id != nullptr) self->resample_quality_ = id;
        self->refresh_playlist_processing_metadata();
        self->save_preferences();
    }), this);
    g_signal_connect(bitdepth_quality_combo, "changed", G_CALLBACK(+[](GtkComboBox* combo, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        const gchar* id = gtk_combo_box_get_active_id(combo);
        if (id != nullptr) self->bitdepth_quality_ = id;
        self->refresh_playlist_processing_metadata();
        self->save_preferences();
    }), this);
    auto on_add_rate_clicked = +[](GtkButton*, gpointer user_data) {
        auto* data = static_cast<AddRateRuleData*>(user_data);
        gchar* from_text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(data->from_combo));
        gchar* to_text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(data->to_combo));
        if (from_text == nullptr || to_text == nullptr) { if (from_text) g_free(from_text); if (to_text) g_free(to_text); return; }
        std::uint32_t from_rate = 0, to_rate = 0; try { from_rate = static_cast<std::uint32_t>(std::stoul(from_text)); to_rate = static_cast<std::uint32_t>(std::stoul(to_text)); } catch (...) {}
        g_free(from_text); g_free(to_text);
        if (from_rate == 0 || to_rate == 0 || from_rate == to_rate) return;
        for (const auto& rule : data->self->resample_rules_) if (rule.from_rate == from_rate && rule.to_rate == to_rate) return;
        data->self->resample_rules_.push_back(GtkPlayerWindow::ResampleRule{from_rate, to_rate});
        data->self->refresh_playlist_processing_metadata();
        data->self->save_preferences();
        data->self->refresh_display();
        GtkWidget* row = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(row), 8);
        gtk_widget_set_hexpand(row, TRUE);
        gtk_widget_set_margin_top(row, 2);
        gtk_widget_set_margin_bottom(row, 2);
        GtkWidget* label = gtk_label_new((std::to_string(from_rate) + " Hz → " + std::to_string(to_rate) + " Hz").c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_hexpand(label, TRUE);
        GtkWidget* del = gtk_button_new_with_label("Delete rule");
        gtk_widget_set_valign(del, GTK_ALIGN_CENTER);
        auto* del_data = new DeleteRateRuleData{data->self, data->dialog, row, from_rate, to_rate};
        auto on_delete_rate_clicked2 = +[](GtkButton*, gpointer user_data2) {
            auto* data2 = static_cast<DeleteRateRuleData*>(user_data2);
            for (std::size_t idx = 0; idx < data2->self->resample_rules_.size(); ++idx) if (data2->self->resample_rules_[idx].from_rate == data2->from_rate && data2->self->resample_rules_[idx].to_rate == data2->to_rate) { data2->self->resample_rules_.erase(data2->self->resample_rules_.begin() + static_cast<std::ptrdiff_t>(idx)); break; }
            data2->self->refresh_playlist_processing_metadata(); data2->self->save_preferences(); data2->self->refresh_display(); if (data2->row) gtk_widget_destroy(data2->row); };
        g_signal_connect_data(del, "clicked", G_CALLBACK(on_delete_rate_clicked2), del_data, destroy_delete_rate_rule_data, static_cast<GConnectFlags>(0));
        gtk_grid_attach(GTK_GRID(row), label, 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(row), del, 1, 0, 1, 1);
        gtk_box_pack_start(GTK_BOX(data->list), row, FALSE, FALSE, 0);
        gtk_widget_show_all(row);
    };
    auto* rate_add = new AddRateRuleData{this, dialog, from_combo, to_combo, rate_list};
    g_signal_connect_data(add_rate_btn, "clicked", G_CALLBACK(on_add_rate_clicked), rate_add, destroy_add_rate_rule_data, static_cast<GConnectFlags>(0));
    auto on_add_bit_clicked = +[](GtkButton*, gpointer user_data) {
        auto* data = static_cast<AddBitRuleData*>(user_data);
        gchar* from_text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(data->from_combo));
        gchar* to_text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(data->to_combo));
        if (from_text == nullptr || to_text == nullptr) { if (from_text) g_free(from_text); if (to_text) g_free(to_text); return; }
        std::uint16_t from_bits = 0, to_bits = 0; try { from_bits = static_cast<std::uint16_t>(std::stoul(from_text)); to_bits = static_cast<std::uint16_t>(std::stoul(to_text)); } catch (...) {}
        g_free(from_text); g_free(to_text);
        if (from_bits == 0 || to_bits == 0 || from_bits == to_bits) return;
        for (const auto& rule : data->self->bitdepth_rules_) if (rule.from_bits == from_bits && rule.to_bits == to_bits) return;
        data->self->bitdepth_rules_.push_back(GtkPlayerWindow::BitDepthRule{from_bits, to_bits});
        data->self->refresh_playlist_processing_metadata();
        data->self->save_preferences();
        data->self->refresh_display();
        GtkWidget* row = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(row), 8);
        gtk_widget_set_hexpand(row, TRUE);
        gtk_widget_set_margin_top(row, 2);
        gtk_widget_set_margin_bottom(row, 2);
        GtkWidget* label = gtk_label_new((std::to_string(from_bits) + "-bit → " + std::to_string(to_bits) + "-bit").c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_hexpand(label, TRUE);
        GtkWidget* del = gtk_button_new_with_label("Delete rule");
        gtk_widget_set_valign(del, GTK_ALIGN_CENTER);
        auto* del_data = new DeleteBitRuleData{data->self, data->dialog, row, from_bits, to_bits};
        auto on_delete_bit_clicked2 = +[](GtkButton*, gpointer user_data2) {
            auto* data2 = static_cast<DeleteBitRuleData*>(user_data2);
            for (std::size_t idx = 0; idx < data2->self->bitdepth_rules_.size(); ++idx) if (data2->self->bitdepth_rules_[idx].from_bits == data2->from_bits && data2->self->bitdepth_rules_[idx].to_bits == data2->to_bits) { data2->self->bitdepth_rules_.erase(data2->self->bitdepth_rules_.begin() + static_cast<std::ptrdiff_t>(idx)); break; }
            data2->self->refresh_playlist_processing_metadata(); data2->self->save_preferences(); data2->self->refresh_display(); if (data2->row) gtk_widget_destroy(data2->row); };
        g_signal_connect_data(del, "clicked", G_CALLBACK(on_delete_bit_clicked2), del_data, destroy_delete_bit_rule_data, static_cast<GConnectFlags>(0));
        gtk_grid_attach(GTK_GRID(row), label, 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(row), del, 1, 0, 1, 1);
        gtk_box_pack_start(GTK_BOX(data->list), row, FALSE, FALSE, 0);
        gtk_widget_show_all(row);
    };
    auto* bit_add = new AddBitRuleData{this, dialog, from_bits_combo, to_bits_combo, bit_list};
    g_signal_connect_data(add_bit_btn, "clicked", G_CALLBACK(on_add_bit_clicked), bit_add, destroy_add_bit_rule_data, static_cast<GConnectFlags>(0));

    attach_diagnostics(make_header("FLAC bit-perfect test", "Generates a deterministic 16-bit / 44.1 kHz / stereo FLAC file, decodes a reference with flac CLI, renders the current PCM Transport path before ALSA, and compares samples."));
    GtkWidget* diag_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(diag_grid), row_spacing);
    gtk_grid_set_column_spacing(GTK_GRID(diag_grid), col_spacing);
    gtk_widget_set_margin_start(diag_grid, section_indent);
    GtkWidget* diag_duration_label = gtk_label_new("Test length:");
    gtk_label_set_xalign(GTK_LABEL(diag_duration_label), 0.0f);
    GtkWidget* diag_duration_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(diag_duration_combo), "30", "30 seconds (default)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(diag_duration_combo), "60", "1 minute");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(diag_duration_combo), "240", "4 minutes");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(diag_duration_combo), "30");
    gtk_widget_set_hexpand(diag_duration_combo, TRUE);
    GtkWidget* diag_run_button = gtk_button_new_with_label("Run FLAC bit-perfect test");
    gtk_widget_set_tooltip_text(diag_run_button, "Runs an offline libFLAC/flac CLI diagnostic. Temporary files are removed after the test.");
    gtk_grid_attach(GTK_GRID(diag_grid), diag_duration_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(diag_grid), diag_duration_combo, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(diag_grid), diag_run_button, 0, 1, 2, 1);
    attach_diagnostics(diag_grid);
    GtkWidget* diag_note = gtk_label_new("The test uses libFLAC / flac CLI only. FFmpeg is not used. FAIL is expected if DSP, soft volume, headroom or Deep Bass changes the signal.");
    gtk_label_set_xalign(GTK_LABEL(diag_note), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(diag_note), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(diag_note), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(diag_note), 74);
    gtk_widget_set_hexpand(diag_note, FALSE);
    gtk_widget_set_vexpand(diag_note, FALSE);
    gtk_widget_set_margin_start(diag_note, section_indent);
    attach_diagnostics(diag_note);
    gtk_widget_set_hexpand(diag_note, FALSE);
    gtk_widget_set_vexpand(diag_note, FALSE);
    auto* diag_btn_data = new BitPerfectButtonData{this, dialog, diag_duration_combo};
    g_signal_connect_data(diag_run_button, "clicked", G_CALLBACK(GtkPlayerWindow::on_run_bitperfect_test_clicked), diag_btn_data, destroy_bitperfect_button_data, static_cast<GConnectFlags>(0));

    GtkWidget* diag_alsa_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(diag_alsa_sep, 8);
    gtk_widget_set_margin_bottom(diag_alsa_sep, 8);
    attach_diagnostics(diag_alsa_sep);

    attach_diagnostics(make_header("ALSA output diagnostics", "Shows the active ALSA output path and probes the selected PCM device for common PCM containers and sample rates."));
    GtkWidget* alsa_diag_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(alsa_diag_grid), row_spacing);
    gtk_grid_set_column_spacing(GTK_GRID(alsa_diag_grid), col_spacing);
    gtk_widget_set_margin_start(alsa_diag_grid, section_indent);

    GtkWidget* active_output_label = gtk_label_new("Active ALSA output:");
    gtk_label_set_xalign(GTK_LABEL(active_output_label), 0.0f);
    GtkWidget* active_output_value = gtk_label_new(nullptr);
    std::string active_output_text = engine_.active_output_report();
    if (active_output_text.empty()) active_output_text = "No active ALSA output yet.";
    gtk_label_set_text(GTK_LABEL(active_output_value), active_output_text.c_str());
    gtk_label_set_xalign(GTK_LABEL(active_output_value), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(active_output_value), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(active_output_value), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(active_output_value), 74);
    gtk_label_set_selectable(GTK_LABEL(active_output_value), TRUE);

    GtkWidget* refresh_active_output_button = gtk_button_new_with_label("Refresh active output");
    GtkWidget* probe_alsa_button = gtk_button_new_with_label("Probe selected ALSA device");
    gtk_widget_set_tooltip_text(probe_alsa_button, "Tests the selected ALSA PCM device for common stereo PCM formats and sample rates. Stop playback first for reliable results.");

    gtk_grid_attach(GTK_GRID(alsa_diag_grid), active_output_label, 0, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(alsa_diag_grid), active_output_value, 0, 1, 2, 1);
    gtk_grid_attach(GTK_GRID(alsa_diag_grid), refresh_active_output_button, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(alsa_diag_grid), probe_alsa_button, 1, 2, 1, 1);
    attach_diagnostics(alsa_diag_grid);

    g_object_set_data(G_OBJECT(refresh_active_output_button), "active-output-label", active_output_value);
    g_signal_connect(refresh_active_output_button, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        if (self == nullptr) return;
        GtkWidget* label = GTK_WIDGET(g_object_get_data(G_OBJECT(button), "active-output-label"));
        if (label == nullptr) return;
        std::string text = self->engine_.active_output_report();
        if (text.empty()) text = "No active ALSA output yet.";
        gtk_label_set_text(GTK_LABEL(label), text.c_str());
    }), this);

    g_signal_connect(probe_alsa_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        if (self == nullptr) return;
        const AlsaProbeMatrix matrix = AlsaPcmBackend::probe_device_format_matrix(self->current_device_);
        show_alsa_probe_table_dialog(GTK_WINDOW(self->window_), matrix);
    }), this);



    gtk_widget_show_all(dialog);
    while (true) {
        const int response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (response == RESPONSE_RESET) {
            gtk_range_set_value(GTK_RANGE(volume_scale), 100.0);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(deep_bass_check), FALSE);
            gtk_combo_box_set_active(GTK_COMBO_BOX(deep_bass_preset_combo), 0);
            gtk_range_set_value(GTK_RANGE(deep_bass_amount_scale), 0.0);
            gtk_range_set_value(GTK_RANGE(bass_scale), 0.0);
            gtk_range_set_value(GTK_RANGE(treble_scale), 0.0);
            gtk_combo_box_set_active(GTK_COMBO_BOX(preset_combo), 0);
            gtk_range_set_value(GTK_RANGE(pre_eq_headroom_scale), 0.0);
            continue;
        }
        break;
    }
    gtk_widget_destroy(dialog);
}

void GtkPlayerWindow::open_alsamixer_for_current_device() {
    refresh_device_list();
    int card = current_card_index(cards_, current_device_);
    const std::string mixer_cmd = card >= 0 ? ("alsamixer -c " + std::to_string(card)) : std::string("alsamixer");

    GError* error = nullptr;
    std::string command = "xfce4-terminal --command=" + shell_escape_cmd(mixer_cmd);
    if (!g_spawn_command_line_async(command.c_str(), &error)) {
        if (error != nullptr) {
            g_error_free(error);
            error = nullptr;
        }
        command = "x-terminal-emulator -e " + mixer_cmd;
        if (!g_spawn_command_line_async(command.c_str(), &error)) {
            if (error != nullptr) {
                g_error_free(error);
                error = nullptr;
            }
            command = "xterm -e " + mixer_cmd;
            if (!g_spawn_command_line_async(command.c_str(), &error) && error != nullptr) {
                Logger::instance().error(std::string("Failed to open alsamixer: ") + error->message);
                g_error_free(error);
            }
        }
    }
}

void GtkPlayerWindow::refresh_device_list() {
    cards_ = CardProfileRegistry::probe_cards();
}

void GtkPlayerWindow::refresh_dsp_info_for_current_device() {
    current_dsp_info_ = DspConnectionInfo{};
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        if (cards_[i].hw_device == current_device_) {
            current_dsp_info_ = AlsaControlBridge::probe(cards_[i].card_index);
            if (current_dsp_info_.status_text.empty()) {
                current_dsp_info_.status_text = cards_[i].dsp_status.empty() ? "ALSA mixer controls available" : cards_[i].dsp_status;
            }
            return;
        }
    }
    current_dsp_info_.status_text = "ALSA mixer controls available";
}

void GtkPlayerWindow::refresh_display(bool update_text, bool update_progress, bool update_meter) {
    if (ui_closing_) {
        return;
    }

    const PlaybackStatusSnapshot status = engine_.snapshot();

    if (update_progress) {
        std::string time_text = "00:00 / 00:00";
        display_progress_ratio_ = 0.0;
        if (!playlist_.empty() && current_track_index_ < playlist_.size()) {
            const PlaylistEntry& track = playlist_[current_track_index_];
            const std::uint64_t track_length = track_length_samples(track);
            const std::uint64_t track_position = current_track_position_from_status(status);
            if (track.is_stream) {
                time_text = format_time(track_position, track.decoded_format.sample_rate) + " / LIVE";
            } else if (track_length > 0) {
                display_progress_ratio_ = std::max(0.0, std::min(1.0, static_cast<double>(track_position) / static_cast<double>(track_length)));
                time_text = format_time(track_position, track.decoded_format.sample_rate) +
                            " / " + format_time(track_length, track.decoded_format.sample_rate);
            }
        }
        display_time_text_ = time_text;
        if (display_time_ != nullptr) {
            set_label_text_if_changed(display_time_, display_time_text_);
        }
        if (progress_bar_ != nullptr) {
            gtk_widget_queue_draw(progress_bar_);
        }
    }

    if (update_meter) {
        meter_level_ = level_meter_enabled_ ? static_cast<double>(status.peak_level) : 0.0;
        if (display_meter_ != nullptr && level_meter_enabled_) {
            gtk_widget_queue_draw(display_meter_);
        }
    }
    if (clip_detection_enabled_ && (update_meter || update_progress)) {
        update_clip_indicator(status.clip_detected, status.clipped_samples);
    } else if (update_text && !clip_detection_enabled_) {
        update_clip_indicator(false, 0);
    }

    if (!update_text) {
        return;
    }

    std::string track_text = "Track: --";
    std::string status_text = status.message.empty() ? "Idle" : status.message;
    if (!stream_status_override_.empty()) {
        status_text = stream_status_override_;
    }
    std::string source_text = "Device: " + current_device_;
    std::string path_text = "Path: --";

    if (!playlist_.empty() && current_track_index_ < playlist_.size()) {
        const PlaylistEntry& track = playlist_[current_track_index_];
        const std::string ext = lower_extension(track.audio_file_path);
        track_text = "Track " + std::to_string(track.track_number) + ": " + display_title_for(track);

        std::string source_name = "File";
        if (ext == ".flac") source_name = "FLAC";
        else if (ext == ".wav" || ext == ".wave") source_name = "WAV";
        else if (ext == ".bwf") source_name = "BWF";
        else if (ext == ".au" || ext == ".snd") source_name = "AU/SND";
        else if (ext == ".caf") source_name = "CAF";
        else if (ext == ".aiff" || ext == ".aif") source_name = "AIFF";
        else if (ext == ".ape") source_name = "APE";
        else if (ext == ".wv") source_name = "WavPack";
        else if (ext == ".m4a") source_name = "M4A";
        else if (ext == ".aac") source_name = "AAC";
        else if (ext == ".ogg" || ext == ".oga") source_name = "OGG";
        else if (ext == ".opus") source_name = "OPUS";
        else if (ext == ".tak") source_name = "TAK";
        else if (ext == ".tta") source_name = "TTA";
        else if (ext == ".wma" || ext == ".asf" || ext == ".xwma") source_name = "WMA";
        else if (ext == ".oma" || ext == ".aa3" || ext == ".at3") source_name = "ATRAC";
        else if (ext == ".mpc" || ext == ".mp+" || ext == ".mpp") source_name = "MPC";
        else if (ext == ".dsf") source_name = "DSF";
        else if (ext == ".mp3") source_name = "MP3";
        const std::uint32_t effective_target_rate = target_sample_rate_for(track.source_sample_rate);
        const std::uint16_t effective_target_bits = target_bits_for(track.source_bits_per_sample);
        const bool effective_resampled = (effective_target_rate > 0 && effective_target_rate != track.source_sample_rate);
        const bool effective_bitdepth = (effective_target_bits > 0 && effective_target_bits != track.source_bits_per_sample);
        const bool uses_external_decoder = !track.native_decode;
        const std::uint32_t shown_rate = effective_resampled ? effective_target_rate : track.decoded_format.sample_rate;
        const std::uint16_t shown_bits = effective_bitdepth ? effective_target_bits : track.decoded_format.bits_per_sample;
        std::string decoder_name = uses_external_decoder ? ((effective_resampled || effective_bitdepth) ? "FFmpeg/SoXr" : "FFmpeg") : "libFLAC";
        path_text = "Path: " + source_name + " → " + decoder_name;
        if (effective_resampled) {
            path_text += " → Resampled " + std::to_string(track.source_sample_rate) + "→" + std::to_string(shown_rate);
        }
        if (effective_bitdepth) {
            path_text += " → Dither " + std::to_string(track.source_bits_per_sample) + "→" + std::to_string(shown_bits);
        }
        path_text += " → PCM " +
                    std::to_string(shown_rate / 1000) + "." +
                    std::to_string((shown_rate % 1000) / 100) + "k/" +
                    std::to_string(shown_bits);
        if (soft_volume_percent_ < 100 || bass_db_ != 0 || treble_db_ != 0 || deep_bass_enabled_ || effective_pre_eq_headroom_tenths_db() > 0) {
            path_text += " → SoftDSP";
            if (bass_db_ != 0) path_text += " Bass " + std::to_string(bass_db_) + "dB";
            if (treble_db_ != 0) path_text += " Treble " + std::to_string(treble_db_) + "dB";
            if (deep_bass_enabled_) {
                path_text += " Deep Bass";
                if (deep_bass_amount_ != 0) path_text += " " + format_signed_step(deep_bass_amount_);
            }
            if (soft_volume_percent_ < 100) path_text += " Vol " + std::to_string(soft_volume_percent_) + "%";
            if (effective_pre_eq_headroom_tenths_db() > 0) path_text += " Pre-EQ Headroom " + format_headroom_db_text(static_cast<double>(effective_pre_eq_headroom_tenths_db()) / 10.0) + "dB";
        }
        path_text += " → ALSA " + current_device_ + " (buffer 588/2352)";
    }

    set_label_text_if_changed(display_track_, safe_utf8_for_display(track_text));
    set_label_text_if_changed(display_status_, safe_utf8_for_display(status_text));
    set_label_text_if_changed(display_source_, safe_utf8_for_display(source_text));
    set_label_text_if_changed(display_path_, safe_utf8_for_display(path_text));
    if (!clip_detection_enabled_) {
        update_clip_indicator(false, 0);
    }

    const bool has_track = !playlist_.empty() && current_track_index_ < playlist_.size();
    set_widget_opacity_if_changed(badge_lossless_, (has_track && playlist_[current_track_index_].lossless_source) ? 1.0 : 0.0);
    set_widget_opacity_if_changed(badge_redbook_, (has_track && playlist_[current_track_index_].decoded_format.is_red_book()) ? 1.0 : 0.0);
    set_widget_opacity_if_changed(badge_native_, (has_track && playlist_[current_track_index_].native_decode) ? 1.0 : 0.0);
    set_widget_opacity_if_changed(badge_dsp_, (soft_volume_percent_ < 100 || bass_db_ != 0 || treble_db_ != 0 || deep_bass_enabled_ || effective_pre_eq_headroom_tenths_db() > 0) ? 1.0 : 0.0);
    set_widget_opacity_if_changed(badge_repeat_, repeat_enabled_ ? 1.0 : 0.0);
}

void GtkPlayerWindow::rebuild_playlist_view() {
    gtk_list_store_clear(playlist_store_);
    for (std::size_t i = 0; i < playlist_.size(); ++i) {
        GtkTreeIter iter;
        gtk_list_store_append(playlist_store_, &iter);
        const PlaylistEntry& entry = playlist_[i];
        const bool stream_broken = entry.is_stream && stream_health_.is_broken(entry.audio_file_path);
        const std::string trackno = stream_broken
            ? ("× " + std::to_string(entry.track_number))
            : std::to_string(entry.track_number);
        const std::string artist = safe_utf8_for_display(entry.performer);
        const std::string title = safe_utf8_for_display(entry.title);
        const std::string source = safe_utf8_for_display(entry.source_label);
        gtk_list_store_set(playlist_store_, &iter,
                           COL_INDEX, static_cast<int>(i),
                           COL_TRACKNO, trackno.c_str(),
                           COL_ARTIST, artist.c_str(),
                           COL_TITLE, title.c_str(),
                           COL_SOURCE, source.c_str(),
                           COL_STREAM_BROKEN, stream_broken ? TRUE : FALSE,
                           -1);
    }
    if (playlist_filter_ != nullptr) {
        gtk_tree_model_filter_refilter(playlist_filter_);
    }
}

void GtkPlayerWindow::refresh_stream_health_rows_for_url(const std::string& url) {
    if (playlist_store_ == nullptr || playlist_.empty()) {
        return;
    }

    const std::string normalized = normalize_stream_url(url);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(playlist_store_), &iter);
    while (valid) {
        int index = 0;
        gtk_tree_model_get(GTK_TREE_MODEL(playlist_store_), &iter, COL_INDEX, &index, -1);
        if (index >= 0 && static_cast<std::size_t>(index) < playlist_.size()) {
            const PlaylistEntry& entry = playlist_[static_cast<std::size_t>(index)];
            if (normalize_stream_url(entry.audio_file_path) == normalized) {
                const bool stream_broken = entry.is_stream && stream_health_.is_broken(entry.audio_file_path);
                const std::string trackno = stream_broken
                    ? ("× " + std::to_string(entry.track_number))
                    : std::to_string(entry.track_number);
                gtk_list_store_set(playlist_store_, &iter,
                                   COL_TRACKNO, trackno.c_str(),
                                   COL_STREAM_BROKEN, stream_broken ? TRUE : FALSE,
                                   -1);
            }
        }
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(playlist_store_), &iter);
    }

    if (playlist_view_ != nullptr) {
        gtk_widget_queue_draw(playlist_view_);
    }
}

void GtkPlayerWindow::select_playlist_row(std::size_t index) {
    if (playlist_.empty() || index >= playlist_.size() || playlist_view_ == nullptr) {
        return;
    }
    GtkTreePath* path = nullptr;
    if (!find_playlist_view_path_for_index(index, &path) || path == nullptr) {
        return;
    }
    GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(playlist_view_));
    gtk_tree_selection_unselect_all(selection);
    gtk_tree_selection_select_path(selection, path);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(playlist_view_), path, nullptr, FALSE);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(playlist_view_), path, nullptr, FALSE, 0.0f, 0.0f);
    gtk_tree_path_free(path);
}

void GtkPlayerWindow::sync_playlist_cursor_to_selection() {
    if (playlist_.empty() || playlist_view_ == nullptr) {
        return;
    }

    GtkTreeView* view = GTK_TREE_VIEW(playlist_view_);
    GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        return;
    }

    GtkTreePath* path = gtk_tree_model_get_path(model, &iter);
    if (path == nullptr) {
        return;
    }
    gtk_tree_view_set_cursor(view, path, nullptr, FALSE);
    gtk_tree_path_free(path);
}

void GtkPlayerWindow::update_playlist_selection_from_ui() {
    if (playlist_.empty() || playlist_view_ == nullptr) {
        return;
    }

    GtkTreeView* view = GTK_TREE_VIEW(playlist_view_);
    GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        int row_index = -1;
        gtk_tree_model_get(model, &iter, COL_INDEX, &row_index, -1);
        if (row_index >= 0 && static_cast<std::size_t>(row_index) < playlist_.size()) {
            current_track_index_ = static_cast<std::size_t>(row_index);
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
            gtk_tree_model_get(cursor_model, &cursor_iter, COL_INDEX, &row_index, -1);
            if (row_index >= 0 && static_cast<std::size_t>(row_index) < playlist_.size()) {
                current_track_index_ = static_cast<std::size_t>(row_index);
            }
        }
        gtk_tree_path_free(cursor_path);
    }
}

std::size_t GtkPlayerWindow::highlighted_playlist_index() const {
    if (playlist_.empty()) {
        return 0;
    }
    if (playlist_view_ == nullptr) {
        return std::min(current_track_index_, playlist_.size() - 1);
    }

    GtkTreeView* view = GTK_TREE_VIEW(playlist_view_);
    GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        int row_index = -1;
        gtk_tree_model_get(model, &iter, COL_INDEX, &row_index, -1);
        if (row_index >= 0 && static_cast<std::size_t>(row_index) < playlist_.size()) {
            return static_cast<std::size_t>(row_index);
        }
    }

    return std::min(current_track_index_, playlist_.size() - 1);
}

std::string GtkPlayerWindow::format_time(std::uint64_t samples_per_channel, std::uint32_t sample_rate) {
    const std::uint64_t safe_rate = sample_rate == 0 ? 44100ULL : static_cast<std::uint64_t>(sample_rate);
    const std::uint64_t total_seconds = samples_per_channel / safe_rate;
    const std::uint64_t minutes = total_seconds / 60ULL;
    const std::uint64_t seconds = total_seconds % 60ULL;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02llu:%02llu",
                  static_cast<unsigned long long>(minutes),
                  static_cast<unsigned long long>(seconds));
    return buffer;
}

std::string GtkPlayerWindow::display_title_for(const PlaylistEntry& entry) const {
    if (entry.is_stream && !stream_now_playing_.empty()) {
        if (!entry.title.empty()) {
            return entry.title + " — " + stream_now_playing_;
        }
        return stream_now_playing_;
    }
    if (!entry.performer.empty()) {
        return entry.performer + " - " + entry.title;
    }
    return entry.title;
}

namespace {

bool session_path_is_remote_uri(const std::string& path) {
    auto starts_with_ci = [](const std::string& text, const char* prefix) -> bool {
        const std::size_t len = std::strlen(prefix);
        if (text.size() < len) {
            return false;
        }
        for (std::size_t i = 0; i < len; ++i) {
            if (std::tolower(static_cast<unsigned char>(text[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i]))) {
                return false;
            }
        }
        return true;
    };
    return starts_with_ci(path, "http://") || starts_with_ci(path, "https://") ||
           starts_with_ci(path, "ftp://") || starts_with_ci(path, "rtsp://") ||
           starts_with_ci(path, "rtmp://") || starts_with_ci(path, "icy://");
}

} // namespace

PlaylistSessionTrack GtkPlayerWindow::session_track_from_entry(const PlaylistEntry& entry) {
    PlaylistSessionTrack track;
    track.audio_file_path = entry.audio_file_path;
    track.track_number = entry.track_number;
    track.title = entry.title;
    track.performer = entry.performer;
    track.start_sample = entry.start_sample;
    track.end_sample = entry.end_sample;
    track.source_label = entry.source_label;
    track.decoded_sample_rate = entry.decoded_format.sample_rate;
    track.decoded_channels = entry.decoded_format.channels;
    track.decoded_bits_per_sample = entry.decoded_format.bits_per_sample;
    track.source_sample_rate = entry.source_sample_rate;
    track.source_bits_per_sample = entry.source_bits_per_sample;
    track.native_decode = entry.native_decode;
    track.lossless_source = entry.lossless_source;
    track.lossy_source = entry.lossy_source;
    track.resampled = entry.resampled;
    track.resampled_from_rate = entry.resampled_from_rate;
    track.bitdepth_converted = entry.bitdepth_converted;
    track.processed_by_ffmpeg = entry.processed_by_ffmpeg;
    track.codec_name = entry.codec_name;
    track.cue_track = entry.cue_track;
    track.cue_album_end_sample = entry.cue_album_end_sample;
    track.is_stream = session_path_is_remote_uri(entry.audio_file_path);
    return track;
}

GtkPlayerWindow::PlaylistEntry GtkPlayerWindow::entry_from_session_track(const PlaylistSessionTrack& track) {
    PlaylistEntry entry;
    entry.audio_file_path = track.audio_file_path;
    entry.track_number = track.track_number;
    entry.title = track.title;
    entry.performer = track.performer;
    entry.start_sample = track.start_sample;
    entry.end_sample = track.end_sample;
    entry.source_label = track.source_label;
    entry.decoded_format.sample_rate = track.decoded_sample_rate;
    entry.decoded_format.channels = track.decoded_channels;
    entry.decoded_format.bits_per_sample = track.decoded_bits_per_sample;
    entry.source_sample_rate = track.source_sample_rate;
    entry.source_bits_per_sample = track.source_bits_per_sample;
    entry.native_decode = track.native_decode;
    entry.lossless_source = track.lossless_source;
    entry.lossy_source = track.lossy_source;
    entry.resampled = track.resampled;
    entry.resampled_from_rate = track.resampled_from_rate;
    entry.bitdepth_converted = track.bitdepth_converted;
    entry.processed_by_ffmpeg = track.processed_by_ffmpeg;
    entry.codec_name = track.codec_name;
    entry.cue_track = track.cue_track;
    entry.cue_album_end_sample = track.cue_album_end_sample;
    return entry;
}

bool GtkPlayerWindow::session_track_restorable(const PlaylistSessionTrack& track) {
    if (track.audio_file_path.empty()) {
        return false;
    }
    if (track.is_stream || session_path_is_remote_uri(track.audio_file_path)) {
        return true;
    }
    return access(track.audio_file_path.c_str(), F_OK) == 0;
}

void GtkPlayerWindow::save_playlist_session() const {
    PlaylistSessionSnapshot snapshot;
    snapshot.tracks.reserve(playlist_.size());
    for (const PlaylistEntry& entry : playlist_) {
        snapshot.tracks.push_back(session_track_from_entry(entry));
    }
    if (!playlist_.empty()) {
        snapshot.current_track_index = highlighted_playlist_index();
    }
    PlaylistSession().save(snapshot);
}

struct RestorePlaylistFocusData {
    GtkPlayerWindow* window = nullptr;
    std::size_t index = 0;
};

bool GtkPlayerWindow::restore_playlist_session() {
    PlaylistSessionSnapshot snapshot;
    if (!PlaylistSession().load(snapshot)) {
        return false;
    }

    std::vector<PlaylistEntry> restored;
    restored.reserve(snapshot.tracks.size());
    for (const PlaylistSessionTrack& track : snapshot.tracks) {
        if (!session_track_restorable(track)) {
            continue;
        }
        restored.push_back(entry_from_session_track(track));
    }
    if (restored.empty()) {
        return false;
    }

    std::size_t restored_index = 0;
    if (!snapshot.tracks.empty()) {
        const std::size_t saved_index = std::min(snapshot.current_track_index, snapshot.tracks.size() - 1);
        const PlaylistSessionTrack& target = snapshot.tracks[saved_index];
        bool found = false;
        for (std::size_t i = 0; i < restored.size(); ++i) {
            const PlaylistEntry& entry = restored[i];
            if (entry.audio_file_path == target.audio_file_path &&
                entry.start_sample == target.start_sample &&
                entry.cue_track == target.cue_track &&
                entry.track_number == target.track_number) {
                restored_index = i;
                found = true;
                break;
            }
        }
        if (!found) {
            restored_index = std::min(snapshot.current_track_index, restored.size() - 1);
        }
    }

    playlist_ = std::move(restored);
    current_track_index_ = restored_index;
    rebuild_playlist_view();
    select_playlist_row(current_track_index_);
    track_switch_in_progress_ = false;
    finish_handled_ = true;
    refresh_display();
    mark_mpris_track_changed();

    auto* focus_data = new RestorePlaylistFocusData{this, current_track_index_};
    g_idle_add(GtkPlayerWindow::on_restore_playlist_focus_idle, focus_data);
    return true;
}

gboolean GtkPlayerWindow::on_restore_playlist_focus_idle(gpointer user_data) {
    auto* data = static_cast<RestorePlaylistFocusData*>(user_data);
    if (data != nullptr && data->window != nullptr) {
        data->window->finalize_restored_playlist_selection(data->index);
    }
    delete data;
    return G_SOURCE_REMOVE;
}

void GtkPlayerWindow::finalize_restored_playlist_selection(std::size_t index) {
    if (ui_closing_ || playlist_.empty()) {
        return;
    }
    select_playlist_row(std::min(index, playlist_.size() - 1));
    gtk_widget_grab_focus(playlist_view_);
}

void GtkPlayerWindow::load_preferences() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return;
    }

    const std::string path = std::string(home) + "/.config/pcm_transport.conf";
    std::ifstream in(path.c_str());
    if (!in) {
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "last_open_directory") {
            last_open_directory_ = value;
        } else if (key == "current_device") {
            current_device_ = value;
        } else if (key == "soft_volume_percent") {
            try { soft_volume_percent_ = std::stoi(value); } catch (...) {}
            if (soft_volume_percent_ < 0) soft_volume_percent_ = 0;
            if (soft_volume_percent_ > 100) soft_volume_percent_ = 100;
        } else if (key == "bass_db") {
            try { bass_db_ = std::stoi(value); } catch (...) {}
            if (bass_db_ < -12) bass_db_ = -12;
            if (bass_db_ > 12) bass_db_ = 12;
        } else if (key == "treble_db") {
            try { treble_db_ = std::stoi(value); } catch (...) {}
            if (treble_db_ < -12) treble_db_ = -12;
            if (treble_db_ > 12) treble_db_ = 12;
        } else if (key == "pre_eq_headroom_tenths_db") {
            try { pre_eq_headroom_tenths_db_ = std::stoi(value); } catch (...) {}
            if (pre_eq_headroom_tenths_db_ < 0) pre_eq_headroom_tenths_db_ = 0;
            if (pre_eq_headroom_tenths_db_ > kUiPreEqHeadroomMaxTenthsDb) pre_eq_headroom_tenths_db_ = kUiPreEqHeadroomMaxTenthsDb;
        } else if (key == "deep_bass_enabled") {
            deep_bass_enabled_ = (value == "1");
        } else if (key == "deep_bass_preset") {
            try { deep_bass_preset_ = std::stoi(value); } catch (...) {}
            deep_bass_preset_ = deep_bass_ui_from_internal(deep_bass_preset_);
        } else if (key == "deep_bass_amount") {
            try { deep_bass_amount_ = std::stoi(value); } catch (...) {}
            deep_bass_amount_ = clamp_deep_bass_amount_ui(deep_bass_amount_);
        } else if (key == "progress_blink_enabled") {
            progress_blink_enabled_ = (value == "1" || value == "true" || value == "yes");
        } else if (key == "level_meter_enabled") {
            level_meter_enabled_ = (value == "1" || value == "true" || value == "yes");
        } else if (key == "clip_detection_enabled") {
            clip_detection_enabled_ = (value == "1" || value == "true" || value == "yes");
        } else if (key == "resample_rules") {
            resample_rules_ = parse_resample_rules(value);
        } else if (key == "bitdepth_rules") {
            bitdepth_rules_ = parse_bitdepth_rules(value);
        } else if (key == "logging_enabled") {
            logging_enabled_ = (value == "1");
        } else if (key == "log_path") {
            log_path_ = value;
        } else if (key == "log_errors_only") {
            log_errors_only_ = (value == "1");
        } else if (key == "bass_shelf_hz") {
            try { bass_shelf_hz_ = std::stoi(value); } catch (...) {}
            bass_shelf_hz_ = tone::clamp_bass_hz(bass_shelf_hz_);
        } else if (key == "treble_shelf_hz") {
            try { treble_shelf_hz_ = std::stoi(value); } catch (...) {}
            treble_shelf_hz_ = tone::clamp_treble_hz(treble_shelf_hz_);
        } else if (key == "resample_quality") {
            resample_quality_ = value;
        } else if (key == "bitdepth_quality") {
            bitdepth_quality_ = value;
        } else if (key == "alsa_24bit_container_preference") {
            alsa_24bit_container_preference_ = normalize_alsa_24bit_preference_id(value);
        } else if (key == "realtime_audio_priority_enabled") {
            realtime_audio_priority_enabled_ = (value == "1" || value == "true" || value == "yes");
        }
    }
    if (bass_db_ == 0 && treble_db_ == 0 && !deep_bass_enabled_) {
        pre_eq_headroom_tenths_db_ = 0;
    } else if (pre_eq_headroom_tenths_db_ == 0) {
        pre_eq_headroom_tenths_db_ = compute_auto_pre_eq_headroom_tenths_db();
    }
    alsa_24bit_container_preference_ = normalize_alsa_24bit_preference_id(alsa_24bit_container_preference_);
    engine_.set_deep_bass_enabled(deep_bass_enabled_);
    engine_.set_deep_bass_preset(deep_bass_internal_from_ui(deep_bass_preset_));
    engine_.set_deep_bass_amount(deep_bass_dsp_amount_from_ui(deep_bass_amount_));
    engine_.set_level_meter_enabled(level_meter_enabled_);
    engine_.set_clip_detection_enabled(clip_detection_enabled_);
    engine_.set_realtime_priority_enabled(realtime_audio_priority_enabled_);
    engine_.set_realtime_priority(60);
}

void GtkPlayerWindow::save_preferences() const {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return;
    }

    const std::string dir = std::string(home) + "/.config";
    const std::string path = dir + "/pcm_transport.conf";
    std::system((std::string("mkdir -p '") + dir + "'").c_str());

    std::ofstream out(path.c_str(), std::ios::trunc);
    if (!out) {
        return;
    }
    out << "last_open_directory=" << last_open_directory_ << '\n';
    out << "current_device=" << current_device_ << '\n';
    out << "soft_volume_percent=" << soft_volume_percent_ << '\n';
    out << "bass_db=" << bass_db_ << '\n';
    out << "treble_db=" << treble_db_ << '\n';
    out << "pre_eq_headroom_tenths_db=" << effective_pre_eq_headroom_tenths_db() << '\n';
    out << "deep_bass_enabled=" << (deep_bass_enabled_ ? 1 : 0) << '\n';
    out << "deep_bass_preset=" << clamp_deep_bass_preset_ui(deep_bass_preset_) << '\n';
    out << "deep_bass_amount=" << deep_bass_amount_ << '\n';
    out << "progress_blink_enabled=" << (progress_blink_enabled_ ? 1 : 0) << '\n';
    out << "level_meter_enabled=" << (level_meter_enabled_ ? 1 : 0) << '\n';
    out << "clip_detection_enabled=" << (clip_detection_enabled_ ? 1 : 0) << '\n';
    out << "resample_rules=" << serialize_resample_rules(resample_rules_) << '\n';
    out << "bitdepth_rules=" << serialize_bitdepth_rules(bitdepth_rules_) << '\n';
    out << "logging_enabled=" << (logging_enabled_ ? 1 : 0) << '\n';
    out << "log_errors_only=" << (log_errors_only_ ? 1 : 0) << '\n';
    out << "log_path=" << log_path_ << '\n';
    out << "bass_shelf_hz=" << bass_shelf_hz_ << '\n';
    out << "treble_shelf_hz=" << treble_shelf_hz_ << '\n';
    out << "resample_quality=" << resample_quality_ << '\n';
    out << "bitdepth_quality=" << bitdepth_quality_ << '\n';
    out << "alsa_24bit_container_preference=" << normalize_alsa_24bit_preference_id(alsa_24bit_container_preference_) << '\n';
    out << "realtime_audio_priority_enabled=" << (realtime_audio_priority_enabled_ ? 1 : 0) << '\n';
}

void GtkPlayerWindow::setup_mpris() {
    MprisService::Actions actions;
    actions.play = [this]() { mpris_play(); };
    actions.pause = [this]() {
        if (engine_.is_playing() && !engine_.is_paused()) {
            engine_.pause();
            notify_mpris_state_changed();
        }
    };
    actions.play_pause = [this]() {
        if (engine_.is_playing() && engine_.is_paused()) {
            engine_.resume();
        } else if (engine_.is_playing()) {
            engine_.pause();
        } else {
            mpris_play();
        }
        notify_mpris_state_changed();
    };
    actions.stop = [this]() { stop_playback(); };
    actions.next = [this]() { mpris_advance_track(1); };
    actions.previous = [this]() { mpris_advance_track(-1); };
    actions.seek = [this](std::int64_t offset_usec) { return mpris_seek(offset_usec); };
    actions.set_position = [this](std::int64_t position_usec, const std::string& track_id) {
        return mpris_set_position(position_usec, track_id);
    };
    actions.open_uri = [this](const std::string& uri) { return mpris_open_uri(uri); };
    actions.set_volume = [this](double volume) { mpris_set_volume(volume); };
    actions.set_loop_status = [this](const std::string& loop_status) { mpris_set_loop_status(loop_status); };
    actions.set_rate = [this](double rate) { mpris_set_rate(rate); };
    actions.set_fullscreen = [this](bool enabled) { mpris_set_fullscreen(enabled); };
    actions.set_shuffle = [this](bool enabled) { mpris_set_shuffle(enabled); };
    actions.raise = [this]() { mpris_raise(); };
    actions.get_state = [this]() {
        if (ui_closing_) {
            return MprisPlayerState{};
        }
        return build_mpris_state();
    };

    mpris_service_ = std::make_unique<MprisService>(std::move(actions));
    mpris_service_->start();
}

void GtkPlayerWindow::notify_mpris_state_changed() {
    if (mpris_service_ != nullptr) {
        mpris_service_->notify_state_changed();
    }
}

void GtkPlayerWindow::mark_mpris_track_changed() {
    ++mpris_track_epoch_;
    notify_mpris_state_changed();
}

void GtkPlayerWindow::invalidate_mpris_cover_cache() {
    mpris_cover_cache_valid_ = false;
    mpris_cover_cache_directory_.clear();
    mpris_cover_cache_art_path_.clear();
}

std::string GtkPlayerWindow::cached_cover_art_for(const std::string& audio_file_path) const {
    if (ExternalAudioDecoder::is_stream_uri(audio_file_path)) {
        return std::string();
    }
    const std::string directory = directory_of_path(audio_file_path);
    if (mpris_cover_cache_valid_ && mpris_cover_cache_directory_ == directory) {
        return mpris_cover_cache_art_path_;
    }

    mpris_cover_cache_directory_ = directory;
    mpris_cover_cache_art_path_ = find_cover_art_in_directory(audio_file_path);
    mpris_cover_cache_valid_ = true;
    return mpris_cover_cache_art_path_;
}

std::string GtkPlayerWindow::current_mpris_track_id() const {
    if (playlist_.empty() || current_track_index_ >= playlist_.size()) {
        return "/org/mpris/MediaPlayer2/TrackList/NoTrack";
    }
    const std::uint64_t id = mpris_track_epoch_ > 0 ? mpris_track_epoch_ : (current_track_index_ + 1);
    return "/org/pcmtransport/mpris/track/" + std::to_string(id);
}

void GtkPlayerWindow::mpris_play() {
    if (playlist_.empty()) {
        return;
    }
    if (engine_.is_playing() && !engine_.is_paused()) {
        return;
    }
    if (engine_.is_paused()) {
        engine_.resume();
        notify_mpris_state_changed();
        return;
    }
    play_track_index(current_track_index_);
}

void GtkPlayerWindow::mpris_advance_track(int direction) {
    if (playlist_.empty()) {
        return;
    }

    const bool was_paused = engine_.is_playing() && engine_.is_paused();
    const bool was_stopped = !engine_.is_playing();

    if (direction > 0) {
        if (current_track_index_ + 1 < playlist_.size()) {
            if (was_stopped) {
                current_track_index_ += 1;
                select_playlist_row(current_track_index_);
                refresh_display();
                mark_mpris_track_changed();
                return;
            }
            play_track_index_at_offset(current_track_index_ + 1, 0, true, was_paused);
            return;
        }
        if (repeat_enabled_) {
            if (was_stopped) {
                current_track_index_ = 0;
                select_playlist_row(current_track_index_);
                refresh_display();
                mark_mpris_track_changed();
                return;
            }
            play_track_index_at_offset(0, 0, true, was_paused);
        }
        return;
    }

    if (current_track_index_ > 0) {
        if (was_stopped) {
            current_track_index_ -= 1;
            select_playlist_row(current_track_index_);
            refresh_display();
            mark_mpris_track_changed();
            return;
        }
        play_track_index_at_offset(current_track_index_ - 1, 0, true, was_paused);
        return;
    }

    if (repeat_enabled_) {
        const std::size_t last_index = playlist_.size() - 1;
        if (was_stopped) {
            current_track_index_ = last_index;
            select_playlist_row(current_track_index_);
            refresh_display();
            mark_mpris_track_changed();
            return;
        }
        play_track_index_at_offset(last_index, 0, true, was_paused);
        return;
    }
}

MprisPlayerState GtkPlayerWindow::build_mpris_state() const {
    MprisPlayerState state;
    state.volume = static_cast<double>(soft_volume_percent_) / 100.0;
    state.loop_status = mpris_loop_status_;
    state.shuffle = mpris_shuffle_;
    state.fullscreen = mpris_fullscreen_;
    state.can_control = !playlist_.empty();
    state.can_play = !playlist_.empty();
    state.can_go_next = !playlist_.empty() &&
                        (current_track_index_ + 1 < playlist_.size() || repeat_enabled_);
    state.can_go_previous = !playlist_.empty() && (current_track_index_ > 0 || repeat_enabled_);
    state.track_epoch = mpris_track_epoch_;
    state.track_id = current_mpris_track_id();

    const bool transport_active = engine_.is_playing();

    if (transport_active && engine_.is_paused()) {
        state.playback_status = "Paused";
        state.can_pause = true;
    } else if (transport_active) {
        state.playback_status = "Playing";
        state.can_pause = true;
    } else {
        state.playback_status = "Stopped";
        state.can_pause = false;
        state.can_seek = false;
        state.position_usec = 0;
    }

    if (transport_active && !playlist_.empty() && current_track_index_ < playlist_.size()) {
        const PlaylistEntry& track = playlist_[current_track_index_];
        const PlaybackStatusSnapshot status = engine_.snapshot();
        const std::uint32_t sample_rate = track.decoded_format.sample_rate > 0
            ? track.decoded_format.sample_rate
            : 44100U;
        const std::uint64_t position_samples = current_track_position_from_status(status);
        const std::uint64_t length_samples = track_length_samples(track);

        state.has_track = true;
        state.title = !stream_now_playing_.empty() ? stream_now_playing_ : track.title;
        state.artist = track.performer;
        state.track_number = track.track_number;
        state.url = track.is_stream ? track.audio_file_path : file_uri_for_path(track.audio_file_path);
        const std::string cover_path = track.is_stream ? std::string() : cached_cover_art_for(track.audio_file_path);
        if (!cover_path.empty()) {
            state.art_url = file_uri_for_path(cover_path);
        }
        state.position_usec = samples_to_usec_safe(position_samples, sample_rate);
        state.length_usec = samples_to_usec_safe(length_samples, sample_rate);
        state.can_seek = length_samples > 0;
    } else {
        state.has_track = false;
        state.track_id = "/org/mpris/MediaPlayer2/TrackList/NoTrack";
    }

    return state;
}

bool GtkPlayerWindow::validate_mpris_open_uri(const std::string& uri, std::string* resolved_location) const {
    if (uri.compare(0, 7, "file://") == 0) {
        const std::string path = path_from_mpris_uri(uri);
        if (path.empty()) {
            return false;
        }
        if (!g_file_test(path.c_str(), G_FILE_TEST_EXISTS) || !g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR)) {
            return false;
        }
        if (!is_supported_media_path(path)) {
            return false;
        }
        if (resolved_location != nullptr) {
            *resolved_location = path;
        }
        return true;
    }

    if (is_http_media_uri(uri) || uri.compare(0, 6, "icy://") == 0) {
        if (resolved_location != nullptr) {
            *resolved_location = uri;
        }
        return true;
    }

    return false;
}

bool GtkPlayerWindow::mpris_open_uri(const std::string& uri) {
    std::string location;
    if (!validate_mpris_open_uri(uri, &location)) {
        Logger::instance().error("MPRIS OpenUri rejected unsupported URI: " + uri);
        return false;
    }

    try {
        const std::size_t before = playlist_.size();
        if (is_http_media_uri(location) || location.compare(0, 6, "icy://") == 0) {
            append_media_to_playlist(location);
        } else {
            append_path_to_playlist(location);
        }
        rebuild_playlist_view();
        if (playlist_.size() > before) {
            play_track_index(before);
        }
        save_playlist_session();
        return true;
    } catch (const std::exception& ex) {
        Logger::instance().error(std::string("MPRIS OpenUri failed: ") + ex.what());
        return false;
    }
}

std::int64_t GtkPlayerWindow::current_mpris_track_position_usec() const {
    if (playlist_.empty() || current_track_index_ >= playlist_.size() || !engine_.is_playing()) {
        return -1;
    }

    const PlaylistEntry& track = playlist_[current_track_index_];
    const std::uint32_t sample_rate = track.decoded_format.sample_rate > 0
        ? track.decoded_format.sample_rate
        : 44100U;
    const PlaybackStatusSnapshot status = engine_.snapshot();
    return samples_to_usec_safe(current_track_position_from_status(status), sample_rate);
}

std::int64_t GtkPlayerWindow::current_mpris_track_length_usec() const {
    if (playlist_.empty() || current_track_index_ >= playlist_.size()) {
        return -1;
    }

    const PlaylistEntry& track = playlist_[current_track_index_];
    const std::uint32_t sample_rate = track.decoded_format.sample_rate > 0
        ? track.decoded_format.sample_rate
        : 44100U;
    return samples_to_usec_safe(track_length_samples(track), sample_rate);
}

std::int64_t GtkPlayerWindow::mpris_seek(std::int64_t offset_usec) {
    if (playlist_.empty() || current_track_index_ >= playlist_.size() || !engine_.is_playing()) {
        return -1;
    }

    const std::int64_t current_usec = current_mpris_track_position_usec();
    const std::int64_t length_usec = current_mpris_track_length_usec();
    if (current_usec < 0 || length_usec < 0) {
        return -1;
    }

    const std::int64_t target_usec = current_usec + offset_usec;
    if (target_usec < 0) {
        return -1;
    }
    if (target_usec > length_usec) {
        const bool can_go_next = current_track_index_ + 1 < playlist_.size() || repeat_enabled_;
        if (!can_go_next) {
            return -1;
        }
        const std::size_t track_index_before = current_track_index_;
        mpris_advance_track(1);
        if (!engine_.is_playing() || current_track_index_ == track_index_before) {
            return -1;
        }
        return current_mpris_track_position_usec();
    }

    return mpris_set_position(target_usec, current_mpris_track_id());
}

std::int64_t GtkPlayerWindow::mpris_set_position(std::int64_t position_usec, const std::string& track_id) {
    if (playlist_.empty() || current_track_index_ >= playlist_.size() || !engine_.is_playing()) {
        return -1;
    }

    if (track_id.empty() || track_id == kMprisNoTrackObjectPath) {
        return -1;
    }

    if (track_id != current_mpris_track_id()) {
        return -1;
    }

    if (position_usec < 0) {
        return -1;
    }

    const std::int64_t length_usec = current_mpris_track_length_usec();
    if (length_usec < 0 || position_usec > length_usec) {
        return -1;
    }

    const PlaylistEntry& track = playlist_[current_track_index_];
    const std::uint32_t sample_rate = track.decoded_format.sample_rate > 0
        ? track.decoded_format.sample_rate
        : 44100U;
    std::uint64_t target_samples = 0;
    if (!usec_to_samples_safe(position_usec, sample_rate, &target_samples)) {
        return -1;
    }

    const PlaybackStatusSnapshot status = engine_.snapshot();
    const std::uint64_t current_samples = current_track_position_from_status(status);
    if (target_samples == current_samples) {
        return -1;
    }

    play_track_index_at_offset(current_track_index_, target_samples, true, engine_.is_paused(), false);
    return samples_to_usec_safe(target_samples, sample_rate);
}

void GtkPlayerWindow::mpris_set_volume(double volume) {
    soft_volume_percent_ = static_cast<int>(std::round(std::max(0.0, std::min(1.0, volume)) * 100.0));
    engine_.set_soft_volume_percent(soft_volume_percent_);
    save_preferences();
    if (!ui_closing_) {
        refresh_display(false, false, false);
    }
    notify_mpris_state_changed();
}

void GtkPlayerWindow::mpris_set_loop_status(const std::string& loop_status) {
    if (loop_status == "Playlist") {
        mpris_loop_status_ = "Playlist";
        repeat_enabled_ = true;
    } else if (loop_status == "Track") {
        mpris_loop_status_ = "Track";
    } else if (loop_status == "None") {
        mpris_loop_status_ = "None";
        repeat_enabled_ = false;
    } else {
        return;
    }

    save_preferences();
    if (!ui_closing_) {
        refresh_display();
    }
    notify_mpris_state_changed();
}

void GtkPlayerWindow::mpris_set_rate(double rate) {
    if (std::abs(rate - 1.0) > 1e-9) {
        Logger::instance().info("MPRIS Rate is not supported; ignoring value " + std::to_string(rate));
    }
}

void GtkPlayerWindow::mpris_set_fullscreen(bool enabled) {
    if (mpris_fullscreen_ == enabled) {
        return;
    }
    mpris_fullscreen_ = enabled;
    notify_mpris_state_changed();
}

void GtkPlayerWindow::mpris_set_shuffle(bool enabled) {
    if (mpris_shuffle_ == enabled) {
        return;
    }
    mpris_shuffle_ = enabled;
    if (enabled) {
        Logger::instance().info("MPRIS Shuffle is not supported; stored value will not affect playback");
    }
    notify_mpris_state_changed();
}

void GtkPlayerWindow::mpris_raise() {
    if (window_ != nullptr) {
        gtk_window_present(GTK_WINDOW(window_));
    }
}

void GtkPlayerWindow::setup_media_keys(GtkApplication* app) {
    const GActionEntry actions[] = {
        {"media-play", on_media_play, nullptr, nullptr, nullptr, {0}},
        {"media-pause", on_media_pause, nullptr, nullptr, nullptr, {0}},
        {"media-stop", on_media_stop, nullptr, nullptr, nullptr, {0}},
        {"media-next", on_media_next, nullptr, nullptr, nullptr, {0}},
        {"media-previous", on_media_previous, nullptr, nullptr, nullptr, {0}},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), actions, G_N_ELEMENTS(actions), this);

    static const char* kPlayKeys[] = {"XF86AudioPlay", nullptr};
    static const char* kPauseKeys[] = {"XF86AudioPause", nullptr};
    static const char* kStopKeys[] = {"XF86AudioStop", nullptr};
    static const char* kNextKeys[] = {"XF86AudioNext", nullptr};
    static const char* kPreviousKeys[] = {"XF86AudioPrev", nullptr};

    gtk_application_set_accels_for_action(app, "app.media-play", kPlayKeys);
    gtk_application_set_accels_for_action(app, "app.media-pause", kPauseKeys);
    gtk_application_set_accels_for_action(app, "app.media-stop", kStopKeys);
    gtk_application_set_accels_for_action(app, "app.media-next", kNextKeys);
    gtk_application_set_accels_for_action(app, "app.media-previous", kPreviousKeys);
}

void GtkPlayerWindow::handle_media_play() {
    mpris_play();
}

void GtkPlayerWindow::handle_media_pause() {
    if (engine_.is_playing() && !engine_.is_paused()) {
        engine_.pause();
        notify_mpris_state_changed();
    }
}

void GtkPlayerWindow::handle_media_stop() {
    stop_playback();
}

void GtkPlayerWindow::handle_media_next() {
    mpris_advance_track(1);
}

void GtkPlayerWindow::handle_media_previous() {
    mpris_advance_track(-1);
}

void GtkPlayerWindow::on_media_play(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GtkPlayerWindow*>(user_data)->handle_media_play();
}

void GtkPlayerWindow::on_media_pause(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GtkPlayerWindow*>(user_data)->handle_media_pause();
}

void GtkPlayerWindow::on_media_stop(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GtkPlayerWindow*>(user_data)->handle_media_stop();
}

void GtkPlayerWindow::on_media_next(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GtkPlayerWindow*>(user_data)->handle_media_next();
}

void GtkPlayerWindow::on_media_previous(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GtkPlayerWindow*>(user_data)->handle_media_previous();
}

} // namespace pcmtp
