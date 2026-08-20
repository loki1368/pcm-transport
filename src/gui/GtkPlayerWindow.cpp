// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/gui/GtkPlayerWindow.hpp"
#include <climits>
#include <cmath>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <dirent.h>
#include <cerrno>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include <pango/pango.h>
#include <gdk/gdkkeysyms.h>
#include <gio/gio.h>
#include <glib-unix.h>

extern "C" {
#include <FLAC/format.h>
#include <alsa/asoundlib.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}
#include "pcmtp/backend/AlsaBufferPolicy.hpp"
#include "pcmtp/backend/AlsaPcmBackend.hpp"
#include "pcmtp/decoder/ExternalAudioDecoder.hpp"
#include "pcmtp/decoder/GaplessChainDecoder.hpp"
#include "pcmtp/decoder/FlacStreamDecoder.hpp"
#include "pcmtp/decoder/RangeLimitedDecoder.hpp"
#include "pcmtp/dsp/ToneControlDesign.hpp"
#include "pcmtp/playlist/M3uPlaylistReader.hpp"
#include "pcmtp/playlist/MediaProbe.hpp"
#include "pcmtp/patches/PlaylistSelectionPatches.hpp"
#include "pcmtp/gui/PlaylistStreamView.hpp"
#include "pcmtp/stream/StreamAudioDecoder.hpp"
#include "pcmtp/stream/StreamDisplay.hpp"
#include "pcmtp/mpris/MprisService.hpp"
#include "pcmtp/util/Logger.hpp"
#include "pcmtp/util/MediaUri.hpp"
#include "pcmtp/util/TextEncoding.hpp"

namespace pcmtp {

namespace {

constexpr int kClipIndicatorHoldMs = 700;
constexpr guint kPreferencesSaveDebounceMs = 350;
constexpr int kDefaultWindowWidth = 900;
constexpr int kDefaultWindowHeight = 660;
constexpr int kMinPlaylistRows = 10;
constexpr int kMaxPlaylistRows = 20;
constexpr int kMinPlaylistFieldWidthChars = 10;
constexpr int kMaxPlaylistFieldWidthChars = 200;
constexpr int kPlaylistFieldWidthSpacerMinPixels = 6;
constexpr gint kDialogOuterMargin = 14;
constexpr gint kDialogContentFooterSpacing = 8;
constexpr gint kDialogButtonSpacing = 6;
constexpr const char* kApplicationId = "org.berestov.pcmtransport";
constexpr const char* kIconThemeResourceRoot = "/org/berestov/pcmtransport/icons";
constexpr const char* kAboutIconResource =
    "/org/berestov/pcmtransport/icons/hicolor/128x128/apps/org.berestov.pcmtransport.png";
constexpr std::array<const char*, 4> kEmbeddedApplicationIconResources = {{
    "/org/berestov/pcmtransport/icons/hicolor/16x16/apps/org.berestov.pcmtransport.png",
    "/org/berestov/pcmtransport/icons/hicolor/32x32/apps/org.berestov.pcmtransport.png",
    "/org/berestov/pcmtransport/icons/hicolor/48x48/apps/org.berestov.pcmtransport.png",
    "/org/berestov/pcmtransport/icons/hicolor/128x128/apps/org.berestov.pcmtransport.png"
}};

int playlist_field_width_spacer_min_width(GtkTreeViewColumn* column) {
    int width = kPlaylistFieldWidthSpacerMinPixels;
    if (column == nullptr) {
        return width;
    }

    GtkWidget* header_button = gtk_tree_view_column_get_button(column);
    if (header_button == nullptr) {
        return width;
    }

    int minimum_width = 0;
    int natural_width = 0;
    gtk_widget_get_preferred_width(header_button, &minimum_width, &natural_width);
    return std::max(width, minimum_width);
}

struct DsdRateDefinition {
    std::uint32_t dsd_sample_rate;
    std::uint32_t ffmpeg_pcm_rate;
    std::uint32_t default_pcm_rate;
    const char* source_label;
    const char* ffmpeg_label;
    bool family_441;
};

constexpr std::array<DsdRateDefinition, 10> kDsdRateDefinitions = {{
    {2822400U, 352800U, 176400U, "DSD64 · 2.8224 MHz", "352.8 kHz", true},
    {5644800U, 705600U, 176400U, "DSD128 · 5.6448 MHz", "705.6 kHz", true},
    {11289600U, 1411200U, 176400U, "DSD256 · 11.2896 MHz", "1411.2 kHz", true},
    {22579200U, 2822400U, 176400U, "DSD512 · 22.5792 MHz", "2822.4 kHz", true},
    {45158400U, 5644800U, 176400U, "DSD1024 · 45.1584 MHz", "5644.8 kHz", true},
    {3072000U, 384000U, 192000U, "DSD64 · 3.072 MHz", "384 kHz", false},
    {6144000U, 768000U, 192000U, "DSD128 · 6.144 MHz", "768 kHz", false},
    {12288000U, 1536000U, 192000U, "DSD256 · 12.288 MHz", "1536 kHz", false},
    {24576000U, 3072000U, 192000U, "DSD512 · 24.576 MHz", "3072 kHz", false},
    {49152000U, 6144000U, 192000U, "DSD1024 · 49.152 MHz", "6144 kHz", false}
}};

constexpr std::array<std::uint32_t, 12> kSelectablePcmRates = {{
    44100U, 48000U, 88200U, 96000U, 176400U, 192000U,
    352800U, 384000U, 705600U, 768000U, 1411200U, 1536000U
}};

const DsdRateDefinition* find_dsd_rate_definition(std::uint32_t dsd_sample_rate) {
    for (const DsdRateDefinition& definition : kDsdRateDefinitions) {
        if (definition.dsd_sample_rate == dsd_sample_rate) {
            return &definition;
        }
    }
    return nullptr;
}

std::vector<GtkPlayerWindow::DsdPcmRule> default_dsd_pcm_rules() {
    std::vector<GtkPlayerWindow::DsdPcmRule> rules;
    rules.reserve(kDsdRateDefinitions.size());
    for (const DsdRateDefinition& definition : kDsdRateDefinitions) {
        rules.push_back(GtkPlayerWindow::DsdPcmRule{
            definition.dsd_sample_rate,
            definition.default_pcm_rate
        });
    }
    return rules;
}

std::string format_rate_khz(std::uint32_t sample_rate) {
    std::ostringstream ss;
    const double khz = static_cast<double>(sample_rate) / 1000.0;
    if (sample_rate % 1000U == 0U) {
        ss << static_cast<unsigned long long>(sample_rate / 1000U);
    } else {
        ss << std::fixed << std::setprecision(1) << khz;
    }
    ss << " kHz";
    return ss.str();
}

std::string format_dsd_rate_mhz(std::uint32_t sample_rate) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision((sample_rate % 1000000U) == 0U ? 3 : 4)
       << (static_cast<double>(sample_rate) / 1000000.0) << " MHz";
    return ss.str();
}

int soxr_precision_for_quality(const std::string& quality) {
    if (quality == "high") return 28;
    if (quality == "balanced") return 20;
    if (quality == "fast") return 16;
    return 33;
}

std::string resample_quality_label(const std::string& quality) {
    if (quality == "high") return "High";
    if (quality == "balanced") return "Balanced";
    if (quality == "fast") return "Fast";
    return "Maximum";
}

std::string dither_quality_label(const std::string& quality) {
    if (quality == "tpdf") return "TPDF";
    if (quality == "rectangular") return "Rectangular";
    return "TPDF high-pass";
}

std::string replace_resampler_runtime_line(
    const std::string& report,
    ResamplerRuntimeKind runtime_kind,
    const std::string& soxr_description) {
    constexpr const char* prefix = "Resampler: ";
    std::size_t line_start = report.find(prefix);
    if (line_start == std::string::npos) {
        return report;
    }
    const std::size_t line_end = report.find('\n', line_start);

    std::string value;
    switch (runtime_kind) {
    case ResamplerRuntimeKind::SoXr:
        value = soxr_description.empty() ? "SoXr" : soxr_description;
        break;
    case ResamplerRuntimeKind::FfmpegSwr:
        value = "FFmpeg SWR (SoXr unavailable)";
        break;
    case ResamplerRuntimeKind::Initializing:
        value = "initializing";
        break;
    case ResamplerRuntimeKind::NotUsed:
    default:
        value = "not used by a processing rule";
        break;
    }

    std::string updated = report;
    updated.replace(
        line_start,
        (line_end == std::string::npos ? report.size() : line_end) - line_start,
        std::string(prefix) + value);
    return updated;
}

bool has_exact_sample_range(bool start_known,
                            bool end_known,
                            std::uint64_t start_sample,
                            std::uint64_t end_sample) {
    return start_known && end_known && end_sample >= start_sample;
}

int clamp_window_height_to_workarea(GtkWidget* window, int requested_height) {
    GdkScreen* screen = window != nullptr ? gtk_widget_get_screen(window) : nullptr;
    if (screen == nullptr) {
        screen = gdk_screen_get_default();
    }
    if (screen == nullptr) {
        return requested_height;
    }

    gint monitor = -1;
    if (window != nullptr && gtk_widget_get_realized(window)) {
        GdkWindow* gdk_window = gtk_widget_get_window(window);
        if (gdk_window != nullptr) {
            monitor = gdk_screen_get_monitor_at_window(screen, gdk_window);
        }
    }
    if (monitor < 0) {
        monitor = gdk_screen_get_primary_monitor(screen);
    }
    if (monitor < 0) {
        monitor = 0;
    }
    GdkRectangle workarea{};
    gdk_screen_get_monitor_workarea(screen, monitor, &workarea);
    if (workarea.height <= 0) {
        return requested_height;
    }

    const int safe_height = std::max(
        1,
        static_cast<int>(std::floor(static_cast<double>(workarea.height) * 0.95)));
    return std::min(requested_height, safe_height);
}

std::uint64_t decoder_open_offset(bool exact_range,
                                  bool start_known,
                                  std::uint64_t start_sample,
                                  std::uint64_t local_offset) {
    if (exact_range || !start_known) {
        return local_offset;
    }
    if (local_offset > std::numeric_limits<std::uint64_t>::max() - start_sample) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return start_sample + local_offset;
}

GdkPixbuf* load_embedded_pixbuf(const char* resource_path) {
    if (resource_path == nullptr || *resource_path == '\0') {
        return nullptr;
    }

    GError* error = nullptr;
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_resource(resource_path, &error);
    if (pixbuf == nullptr) {
        g_warning("PCM Transport: failed to load embedded icon %s: %s",
                  resource_path,
                  error != nullptr ? error->message : "unknown error");
    }
    if (error != nullptr) {
        g_error_free(error);
    }
    return pixbuf;
}

void install_default_application_icons() {
    static bool attempted = false;
    if (attempted) {
        return;
    }
    attempted = true;

    GtkIconTheme* icon_theme = gtk_icon_theme_get_default();
    if (icon_theme != nullptr) {
        gtk_icon_theme_add_resource_path(icon_theme, kIconThemeResourceRoot);
    }

    GList* icons = nullptr;
    for (const char* resource_path : kEmbeddedApplicationIconResources) {
        GdkPixbuf* pixbuf = load_embedded_pixbuf(resource_path);
        if (pixbuf != nullptr) {
            icons = g_list_append(icons, pixbuf);
        }
    }

    if (icons == nullptr) {
        g_warning("PCM Transport: no embedded application icons were loaded");
        return;
    }

    gtk_window_set_default_icon_list(icons);
    for (GList* node = icons; node != nullptr; node = node->next) {
        g_object_unref(node->data);
    }
    g_list_free(icons);
}

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
        return {};
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
    return SourceScanner::is_supported_media_path(path);
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

constexpr int kProgressSegments = 56;
constexpr gint64 kProgressBlinkPeriodUsec = 600000;
constexpr int kMeterRefreshIntervalMs = 33;
constexpr double kMeterReleaseDbPerSecond = 24.0;
constexpr double kMeterInactiveReleaseDbPerSecond = 48.0;
constexpr double kMeterFloorDb = -80.0;
constexpr double kMeterMaximumLevel = 1.18;
constexpr int kUiPreEqHeadroomMaxTenthsDb = 150;
constexpr double kUiHeadroomSafetyMarginDb = 0.0;

struct AsyncUiDispatch {
    GtkPlayerWindow* window = nullptr;
    std::shared_ptr<std::atomic<bool>> lifetime;
};

void destroy_async_ui_dispatch(gpointer data) {
    delete static_cast<AsyncUiDispatch*>(data);
}

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

int deep_bass_ui_from_config(int preset) {
    switch (preset) {
        case 1: // Current UI index: Punch.
        case static_cast<int>(tone::DeepBassPreset::Punchy): // Legacy internal value.
            return 1;
        case 0: // Current UI index: Reference.
        default:
            return 0;
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
    COL_ALBUM,
    COL_TITLE,
    COL_SOURCE,
    COL_SEARCH_FOLDED,
    COL_STREAM_BROKEN = playlist_stream_broken_column(),
    COL_COUNT
};

std::string build_playlist_search_folded(const std::string& artist,
                                           const std::string& album,
                                           const std::string& title) {
    gchar* folded_artist = g_utf8_casefold(artist.c_str(), -1);
    gchar* folded_album = g_utf8_casefold(album.c_str(), -1);
    gchar* folded_title = g_utf8_casefold(title.c_str(), -1);
    std::string result;
    if (folded_artist != nullptr) {
        result.append(folded_artist);
        g_free(folded_artist);
    }
    result.push_back('\n');
    if (folded_album != nullptr) {
        result.append(folded_album);
        g_free(folded_album);
    }
    result.push_back('\n');
    if (folded_title != nullptr) {
        result.append(folded_title);
        g_free(folded_title);
    }
    return result;
}

constexpr std::size_t kMetadataProbeWorkerCount = 3;
constexpr std::size_t kMetadataDisplayRefreshStride = 8;
constexpr int kMetadataDisplayRefreshIntervalMs = 500;
constexpr std::size_t kMediaProbeCacheMaxEntries = 4096;
constexpr std::size_t kMaxRandomHistoryEntries = 100;

std::string metadata_file_identity(const std::string& path) {
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
        return {};
    }

    std::ostringstream identity;
    identity << static_cast<unsigned long long>(status.st_dev) << ':'
             << static_cast<unsigned long long>(status.st_ino) << ':'
             << static_cast<unsigned long long>(status.st_size) << ':'
             << static_cast<long long>(status.st_mtim.tv_sec) << ':'
             << static_cast<long long>(status.st_mtim.tv_nsec);
    return identity.str();
}

std::string metadata_probe_extension(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size() ||
        (slash != std::string::npos && dot < slash)) {
        return "<none>";
    }
    std::string extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension;
}

void log_metadata_probe_timing(const std::string& path,
                               const MediaProbeResult& result) {
    const std::string backend = result.probe_backend.empty()
        ? "unknown"
        : result.probe_backend;
    Logger::instance().debug(
        "Metadata probe timing: backend=" + backend +
        " extension=" + metadata_probe_extension(path) +
        " elapsed_us=" + std::to_string(result.probe_elapsed_microseconds) +
        " path=" + path);
}

std::vector<std::string> prioritize_probe_path(std::vector<std::string> paths,
                                               const std::string& priority_path) {
    if (priority_path.empty()) {
        return paths;
    }
    const auto it = std::find(paths.begin(), paths.end(), priority_path);
    if (it != paths.end() && it != paths.begin()) {
        paths.erase(it);
        paths.insert(paths.begin(), priority_path);
    }
    return paths;
}

std::string base_name(const std::string& path) {
    const std::size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

std::string temporary_title_from_path(const std::string& path) {
    std::string title = base_name(path);

    const std::size_t extension_pos = title.find_last_of('.');
    if (extension_pos != std::string::npos && extension_pos != 0) {
        title.resize(extension_pos);
    }

    std::size_t digit_end = 0;
    while (digit_end < title.size() && digit_end < 3 &&
           std::isdigit(static_cast<unsigned char>(title[digit_end]))) {
        ++digit_end;
    }

    if (digit_end > 0 && digit_end < title.size() &&
        !(digit_end == 3 && std::isdigit(static_cast<unsigned char>(title[digit_end])))) {
        std::size_t content_pos = std::string::npos;
        if (title.compare(digit_end, 3, " - ") == 0) {
            content_pos = digit_end + 3;
        } else if (title.compare(digit_end, 2, ". ") == 0) {
            content_pos = digit_end + 2;
        } else if (title[digit_end] == '_') {
            content_pos = digit_end + 1;
        } else if (title[digit_end] == ' ') {
            content_pos = digit_end + 1;
        }

        if (content_pos != std::string::npos) {
            while (content_pos < title.size() &&
                   std::isspace(static_cast<unsigned char>(title[content_pos]))) {
                ++content_pos;
            }
            if (content_pos < title.size()) {
                title.erase(0, content_pos);
            }
        }
    }

    if (title.empty()) {
        return base_name(path);
    }
    return title;
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

bool executable_tool_available(const char* primary,
                               const char* fallback,
                               const char* name) {
    if (primary != nullptr && access(primary, X_OK) == 0) return true;
    if (fallback != nullptr && access(fallback, X_OK) == 0) return true;
    if (name == nullptr || *name == '\0') return false;
    gchar* resolved = g_find_program_in_path(name);
    const bool available = resolved != nullptr;
    g_free(resolved);
    return available;
}

std::string ffmpeg_component_version(unsigned version) {
    std::ostringstream ss;
    ss << ((version >> 16U) & 0xffU) << '.'
       << ((version >> 8U) & 0xffU) << '.'
       << (version & 0xffU);
    return ss.str();
}

std::string runtime_environment_text(const char* rtkit_status) {
    const char* ffmpeg_version = av_version_info();
    const char* flac_version = FLAC__VERSION_STRING;
    const char* alsa_version = snd_asoundlib_version();
    const bool pkexec_available = executable_tool_available(
        "/usr/bin/pkexec", "/bin/pkexec", "pkexec");
    const bool setcap_available = executable_tool_available(
        "/usr/sbin/setcap", "/usr/bin/setcap", "setcap");

    std::ostringstream ss;
    ss << "Runtime environment:\n"
       << "FFmpeg: " << (ffmpeg_version != nullptr ? ffmpeg_version : "unknown") << '\n'
       << "libavformat: " << ffmpeg_component_version(avformat_version())
       << " · libavcodec: " << ffmpeg_component_version(avcodec_version()) << '\n'
       << "libavutil: " << ffmpeg_component_version(avutil_version())
       << " · libswresample: " << ffmpeg_component_version(swresample_version()) << '\n'
       << "libFLAC: " << (flac_version != nullptr ? flac_version : "unknown")
       << " · GTK: " << gtk_get_major_version() << '.' << gtk_get_minor_version() << '.'
       << gtk_get_micro_version()
       << " · ALSA: " << (alsa_version != nullptr ? alsa_version : "unknown") << '\n'
       << "RTKit: " << (rtkit_status != nullptr ? rtkit_status : "checking...") << '\n'
       << "Realtime tools: pkexec " << (pkexec_available ? "available" : "not available")
       << " · setcap " << (setcap_available ? "available" : "not available");
    return ss.str();
}

void set_runtime_environment_label(GtkWidget* label, const char* rtkit_status) {
    if (label == nullptr || !GTK_IS_LABEL(label)) return;
    const std::string text = runtime_environment_text(rtkit_status);
    gtk_label_set_text(GTK_LABEL(label), text.c_str());
}

void on_rtkit_name_appeared(GDBusConnection*,
                            const gchar*,
                            const gchar*,
                            gpointer user_data) {
    set_runtime_environment_label(GTK_WIDGET(user_data), "available");
}

void on_rtkit_name_vanished(GDBusConnection*,
                            const gchar*,
                            gpointer user_data) {
    set_runtime_environment_label(GTK_WIDGET(user_data), "not available");
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

std::string codec_display_name(const std::string& codec_name) {
    if (codec_name.empty()) {
        return "File";
    }
    if (codec_name == "mp3") return "MP3";
    if (codec_name == "flac") return "FLAC";
    if (codec_name == "aac") return "AAC";
    if (codec_name == "vorbis") return "Vorbis";
    if (codec_name == "opus") return "Opus";
    if (codec_name == "alac") return "ALAC";
    if (codec_name == "ape") return "APE";
    if (codec_name == "wavpack") return "WavPack";
    if (codec_name == "tta") return "TTA";
    if (codec_name == "tak") return "TAK";
    if (codec_name == "hls") return "HLS";
    if (codec_name == "stream") return "Stream";
    std::string out = codec_name;
    if (!out.empty()) {
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    }
    return out;
}

std::string codec_label_for_entry(const std::string& codec_name, const std::string& path) {
    if (!codec_name.empty()) {
        if (codec_name.size() >= 4 && codec_name.compare(0, 4, "pcm_") == 0) {
            const std::string ext = lower_extension(path);
            if (ext == ".wav" || ext == ".wave") return "WAV";
            if (ext == ".bwf") return "BWF";
            if (ext == ".aiff" || ext == ".aif") return "AIFF";
            if (ext == ".au" || ext == ".snd") return "AU/SND";
            if (ext == ".caf") return "CAF";
            if (ext == ".w64") return "W64";
            if (ext == ".voc") return "VOC";
            return "PCM";
        }
        return codec_display_name(codec_name);
    }

    const std::string ext = lower_extension(path);
    if (ext == ".flac") return "FLAC";
    if (ext == ".wav" || ext == ".wave") return "WAV";
    if (ext == ".bwf") return "BWF";
    if (ext == ".au" || ext == ".snd") return "AU/SND";
    if (ext == ".caf") return "CAF";
    if (ext == ".aiff" || ext == ".aif") return "AIFF";
    if (ext == ".ape") return "APE";
    if (ext == ".wv") return "WavPack";
    if (ext == ".w64") return "W64";
    if (ext == ".voc") return "VOC";
    if (ext == ".ra") return "RA";
    if (ext == ".m4a") return "M4A";
    if (ext == ".m4r") return "M4R";
    if (ext == ".aac") return "AAC";
    if (ext == ".mp2") return "MP2";
    if (ext == ".ac3") return "AC3";
    if (ext == ".dts") return "DTS";
    if (ext == ".ogg" || ext == ".oga") return "OGG";
    if (ext == ".opus") return "OPUS";
    if (ext == ".spx") return "SPX";
    if (ext == ".tak") return "TAK";
    if (ext == ".tta") return "TTA";
    if (ext == ".wmv") return "WMV";
    if (ext == ".wma" || ext == ".asf" || ext == ".xwma") return "WMA";
    if (ext == ".oma" || ext == ".aa3" || ext == ".at3") return "ATRAC";
    if (ext == ".mpc" || ext == ".mp+" || ext == ".mpp") return "MPC";
    if (ext == ".dsf") return "DSF";
    if (ext == ".dff") return "DFF";
    if (ext == ".mp3") return "MP3";
    return "File";
}

std::string channels_layout_label(std::uint16_t channels) {
    if (channels == 1) {
        return "mono";
    }
    if (channels == 2) {
        return {};
    }
    if (channels > 0) {
        return std::to_string(channels) + " ch";
    }
    return {};
}

enum class PcmDialogLayoutMode {
    Compact,
    Expandable
};

struct PcmDialogLayout {
    GtkWidget* root = nullptr;
    GtkWidget* content = nullptr;
    GtkWidget* footer = nullptr;
};

PcmDialogLayout create_pcm_dialog_layout(GtkWidget* dialog,
                                         PcmDialogLayoutMode mode);
GtkWidget* add_pcm_dialog_button(GtkWidget* dialog,
                                 GtkWidget* footer,
                                 const char* label,
                                 gint response_id);
void add_pcm_message_content(GtkWidget* content,
                             const std::string& message,
                             GtkMessageType type);

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
    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        title != nullptr ? title : "PCM Transport",
        parent,
        GTK_DIALOG_MODAL,
        NULL);
    const PcmDialogLayout layout = create_pcm_dialog_layout(
        dialog, PcmDialogLayoutMode::Compact);
    add_pcm_message_content(layout.content, message, type);
    add_pcm_dialog_button(dialog, layout.footer, "_Close", GTK_RESPONSE_CLOSE);
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
                                                    NULL);
    const PcmDialogLayout layout = create_pcm_dialog_layout(
        dialog, PcmDialogLayoutMode::Expandable);
    add_pcm_dialog_button(dialog, layout.footer, "_Close", GTK_RESPONSE_CLOSE);
    GtkWidget* area = layout.content;

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

std::string safe_utf8_for_display(const std::string& text_value) {
    if (text_value.empty() ||
        g_utf8_validate(text_value.data(), static_cast<gssize>(text_value.size()), nullptr)) {
        return text_value;
    }
    return pcmtp::text::normalize_metadata_value(text_value);
}

std::string playlist_field_limited_presentation(const std::string& text_value,
                                                int max_chars) {
    const std::string display = safe_utf8_for_display(text_value);
    if (max_chars < 0) {
        return display;
    }

    const glong chars = g_utf8_strlen(display.c_str(), -1);
    if (chars <= max_chars) {
        return display;
    }

    const char* end = g_utf8_offset_to_pointer(display.c_str(), max_chars);
    std::string limited(display.c_str(), static_cast<std::size_t>(end - display.c_str()));
    limited += "\xE2\x80\xA6"; // U+2026 HORIZONTAL ELLIPSIS.
    return limited;
}

int compare_playlist_text(const std::string& left,
                          const std::string& right,
                          bool natural,
                          bool descending) {
    const bool left_empty = left.empty();
    const bool right_empty = right.empty();
    if (left_empty || right_empty) {
        if (left_empty == right_empty) {
            return 0;
        }
        return left_empty ? 1 : -1;
    }

    const std::string safe_left = safe_utf8_for_display(left);
    const std::string safe_right = safe_utf8_for_display(right);
    gchar* folded_left = g_utf8_casefold(safe_left.c_str(), -1);
    gchar* folded_right = g_utf8_casefold(safe_right.c_str(), -1);
    const char* left_value = folded_left != nullptr ? folded_left : safe_left.c_str();
    const char* right_value = folded_right != nullptr ? folded_right : safe_right.c_str();

    int comparison = 0;
    if (natural) {
        gchar* left_key = g_utf8_collate_key_for_filename(left_value, -1);
        gchar* right_key = g_utf8_collate_key_for_filename(right_value, -1);
        if (left_key != nullptr && right_key != nullptr) {
            comparison = std::strcmp(left_key, right_key);
        } else {
            comparison = g_utf8_collate(left_value, right_value);
        }
        g_free(left_key);
        g_free(right_key);
    } else {
        comparison = g_utf8_collate(left_value, right_value);
    }

    g_free(folded_left);
    g_free(folded_right);
    if (comparison == 0) {
        return 0;
    }
    const int normalized = comparison < 0 ? -1 : 1;
    return descending ? -normalized : normalized;
}

int compare_playlist_track_number(int left, int right, bool descending) {
    const bool left_missing = left <= 0;
    const bool right_missing = right <= 0;
    if (left_missing || right_missing) {
        if (left_missing == right_missing) {
            return 0;
        }
        return left_missing ? 1 : -1;
    }
    if (left == right) {
        return 0;
    }
    const int comparison = left < right ? -1 : 1;
    return descending ? -comparison : comparison;
}

std::string encode_config_path(const std::string& path) {
    gchar* encoded = g_base64_encode(reinterpret_cast<const guchar*>(path.data()), path.size());
    if (encoded == nullptr) {
        return std::string();
    }
    std::string result(encoded);
    g_free(encoded);
    return result;
}

bool decode_config_path(const std::string& encoded, std::string* path) {
    if (path == nullptr || encoded.empty()) {
        return false;
    }
    gsize decoded_size = 0;
    guchar* decoded = g_base64_decode(encoded.c_str(), &decoded_size);
    if (decoded == nullptr) {
        return false;
    }
    path->assign(reinterpret_cast<const char*>(decoded), decoded_size);
    g_free(decoded);
    return !path->empty();
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

void destroy_diagnostics_ui_update(gpointer user_data) {
    DiagnosticsUiUpdate* update = static_cast<DiagnosticsUiUpdate*>(user_data);
    if (update == nullptr) {
        return;
    }
    if (update->text_view != nullptr) {
        g_object_unref(update->text_view);
    }
    if (update->progress_bar != nullptr) {
        g_object_unref(update->progress_bar);
    }
    if (update->close_button != nullptr) {
        g_object_unref(update->close_button);
    }
    delete update;
}

gboolean diagnostics_ui_update_cb(gpointer user_data) {
    DiagnosticsUiUpdate* update = static_cast<DiagnosticsUiUpdate*>(user_data);
    if (update == nullptr) {
        return G_SOURCE_REMOVE;
    }
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
    update->text_view = text_view != nullptr
        ? GTK_WIDGET(g_object_ref(text_view))
        : nullptr;
    update->progress_bar = progress_bar != nullptr
        ? GTK_WIDGET(g_object_ref(progress_bar))
        : nullptr;
    update->close_button = close_button != nullptr
        ? GTK_WIDGET(g_object_ref(close_button))
        : nullptr;
    update->text = text;
    update->fraction = fraction;
    update->finished = finished;
    const guint source_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                            diagnostics_ui_update_cb,
                                            update,
                                            destroy_diagnostics_ui_update);
    if (source_id == 0) {
        destroy_diagnostics_ui_update(update);
    }
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

bool write_test_wav(const std::string& path,
                    int duration_seconds,
                    const std::atomic<bool>* cancel_requested) {
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
        if ((i & 4095u) == 0u && cancel_requested != nullptr &&
            cancel_requested->load(std::memory_order_relaxed)) {
            return false;
        }
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
    return static_cast<bool>(out);
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
                                                  int treble_hz,
                                                  const std::atomic<bool>* cancel_requested) {
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
        if (cancel_requested != nullptr &&
            cancel_requested->load(std::memory_order_relaxed)) {
            return std::vector<std::int16_t>();
        }
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

CompareResult compare_samples(const std::vector<std::int16_t>& expected,
                              const std::vector<std::int16_t>& actual,
                              const std::atomic<bool>* cancel_requested) {
    CompareResult r;
    r.compared = std::min(expected.size(), actual.size());
    if (expected.size() != actual.size()) {
        r.pass = false;
    }
    for (std::size_t i = 0; i < r.compared; ++i) {
        if ((i & 4095u) == 0u && cancel_requested != nullptr &&
            cancel_requested->load(std::memory_order_relaxed)) {
            return r;
        }
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

std::string serialize_dsd_pcm_rules(const std::vector<GtkPlayerWindow::DsdPcmRule>& rules) {
    std::string out;
    for (const GtkPlayerWindow::DsdPcmRule& rule : rules) {
        if (!out.empty()) out += ',';
        out += std::to_string(rule.dsd_sample_rate) + '>' + std::to_string(rule.pcm_sample_rate);
    }
    return out;
}

std::vector<GtkPlayerWindow::DsdPcmRule> parse_dsd_pcm_rules(const std::string& text) {
    std::vector<GtkPlayerWindow::DsdPcmRule> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find(',', start);
        const std::string token = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const std::size_t sep = token.find('>');
        if (sep != std::string::npos) {
            try {
                const unsigned long long parsed_dsd_rate = std::stoull(token.substr(0, sep));
                const unsigned long long parsed_pcm_rate = std::stoull(token.substr(sep + 1));
                if (parsed_dsd_rate > std::numeric_limits<std::uint32_t>::max() ||
                    parsed_pcm_rate > std::numeric_limits<std::uint32_t>::max()) {
                    throw std::out_of_range("DSD rule value exceeds uint32 range");
                }
                const std::uint32_t dsd_rate = static_cast<std::uint32_t>(parsed_dsd_rate);
                const std::uint32_t pcm_rate = static_cast<std::uint32_t>(parsed_pcm_rate);
                const DsdRateDefinition* definition = find_dsd_rate_definition(dsd_rate);
                if (definition != nullptr && (pcm_rate == 0 || pcm_rate <= definition->ffmpeg_pcm_rate)) {
                    out.push_back(GtkPlayerWindow::DsdPcmRule{dsd_rate, pcm_rate});
                }
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

void clear_widget_margins(GtkWidget* widget) {
    if (widget == nullptr) {
        return;
    }
    gtk_widget_set_margin_start(widget, 0);
    gtk_widget_set_margin_end(widget, 0);
    gtk_widget_set_margin_top(widget, 0);
    gtk_widget_set_margin_bottom(widget, 0);
}

void on_pcm_dialog_button_clicked(GtkButton* button, gpointer) {
    if (button == nullptr) {
        return;
    }
    auto* dialog = GTK_DIALOG(g_object_get_data(G_OBJECT(button), "pcm-dialog"));
    const gint response_id = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(button), "pcm-response-id"));
    if (dialog != nullptr) {
        gtk_dialog_response(dialog, response_id);
    }
}

PcmDialogLayout create_pcm_dialog_layout(GtkWidget* dialog,
                                         PcmDialogLayoutMode mode) {
    PcmDialogLayout layout;
    if (dialog == nullptr || !GTK_IS_DIALOG(dialog)) {
        return layout;
    }

    GtkWidget* native_content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    if (native_content == nullptr) {
        return layout;
    }

    gtk_container_set_border_width(GTK_CONTAINER(native_content), 0);
    clear_widget_margins(native_content);
    gtk_box_set_spacing(GTK_BOX(native_content), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(native_content),
                                "pcm-dialog-host");

    GtkWidget* native_actions = gtk_dialog_get_action_area(GTK_DIALOG(dialog));
    if (native_actions != nullptr) {
        gtk_container_set_border_width(GTK_CONTAINER(native_actions), 0);
        clear_widget_margins(native_actions);
        gtk_widget_set_no_show_all(native_actions, TRUE);
        gtk_widget_hide(native_actions);
    }

    const gboolean expandable = mode == PcmDialogLayoutMode::Expandable;
    if (!expandable) {
        gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    }

    layout.root = gtk_box_new(GTK_ORIENTATION_VERTICAL,
                              kDialogContentFooterSpacing);
    gtk_widget_set_hexpand(layout.root, TRUE);
    gtk_widget_set_vexpand(layout.root, expandable);
    gtk_widget_set_margin_start(layout.root, kDialogOuterMargin);
    gtk_widget_set_margin_end(layout.root, kDialogOuterMargin);
    gtk_widget_set_margin_top(layout.root, kDialogOuterMargin);
    gtk_widget_set_margin_bottom(layout.root, kDialogOuterMargin);
    gtk_style_context_add_class(gtk_widget_get_style_context(layout.root),
                                "pcm-dialog-root");

    layout.content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(layout.content, TRUE);
    gtk_widget_set_vexpand(layout.content, expandable);
    gtk_style_context_add_class(gtk_widget_get_style_context(layout.content),
                                "pcm-dialog-body");

    layout.footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,
                                kDialogButtonSpacing);
    gtk_widget_set_hexpand(layout.footer, TRUE);
    gtk_widget_set_vexpand(layout.footer, FALSE);
    gtk_widget_set_halign(layout.footer, GTK_ALIGN_END);
    gtk_widget_set_valign(layout.footer, GTK_ALIGN_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(layout.footer),
                                "pcm-dialog-footer");

    gtk_box_pack_start(GTK_BOX(layout.root),
                       layout.content,
                       expandable,
                       expandable,
                       0);
    gtk_box_pack_start(GTK_BOX(layout.root), layout.footer, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(native_content),
                       layout.root,
                       expandable,
                       expandable,
                       0);
    return layout;
}

GtkWidget* add_pcm_dialog_button(GtkWidget* dialog,
                                 GtkWidget* footer,
                                 const char* label,
                                 gint response_id) {
    if (dialog == nullptr || !GTK_IS_DIALOG(dialog) ||
        footer == nullptr || !GTK_IS_BOX(footer)) {
        return nullptr;
    }

    GtkWidget* button = gtk_button_new_with_mnemonic(label != nullptr ? label : "");
    clear_widget_margins(button);
    g_object_set_data(G_OBJECT(button), "pcm-dialog", dialog);
    g_object_set_data(G_OBJECT(button), "pcm-response-id",
                      GINT_TO_POINTER(response_id));
    g_signal_connect(button, "clicked",
                     G_CALLBACK(on_pcm_dialog_button_clicked), nullptr);
    gtk_box_pack_start(GTK_BOX(footer), button, FALSE, FALSE, 0);
    return button;
}

const char* pcm_message_icon_name(GtkMessageType type) {
    switch (type) {
        case GTK_MESSAGE_WARNING:
            return "dialog-warning";
        case GTK_MESSAGE_ERROR:
            return "dialog-error";
        case GTK_MESSAGE_QUESTION:
            return "dialog-question";
        default:
            return nullptr;
    }
}

void add_pcm_message_content(GtkWidget* content,
                             const std::string& message,
                             GtkMessageType type) {
    if (content == nullptr || !GTK_IS_BOX(content)) {
        return;
    }

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_hexpand(row, TRUE);
    gtk_widget_set_vexpand(row, FALSE);
    gtk_widget_set_halign(row, GTK_ALIGN_FILL);
    gtk_widget_set_valign(row, GTK_ALIGN_START);

    const char* icon_name = pcm_message_icon_name(type);
    if (icon_name != nullptr) {
        GtkWidget* icon = gtk_image_new_from_icon_name(icon_name,
                                                       GTK_ICON_SIZE_DIALOG);
        gtk_widget_set_halign(icon, GTK_ALIGN_START);
        gtk_widget_set_valign(icon, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
    }

    GtkWidget* label = gtk_label_new(message.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 76);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_vexpand(label, FALSE);
    gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), row, FALSE, FALSE, 0);
}

} // namespace

class GtkPlayerWindow::PlaylistSelectionSignalBlocker final {
public:
    explicit PlaylistSelectionSignalBlocker(GtkPlayerWindow& owner)
        : owner_(&owner) {
        owner_->begin_playlist_selection_sync();
    }

    ~PlaylistSelectionSignalBlocker() {
        if (owner_ != nullptr) {
            owner_->end_playlist_selection_sync();
        }
    }

    PlaylistSelectionSignalBlocker(const PlaylistSelectionSignalBlocker&) = delete;
    PlaylistSelectionSignalBlocker& operator=(const PlaylistSelectionSignalBlocker&) = delete;

private:
    GtkPlayerWindow* owner_ = nullptr;
};

struct GtkPlayerWindow::SearchDelegate final : PlaylistSearchController::Delegate {
    GtkPlayerWindow* self = nullptr;
    std::unique_ptr<PlaylistSelectionSignalBlocker> refilter_selection_blocker;

    explicit SearchDelegate(GtkPlayerWindow* window) : self(window) {}

    GtkListStore* playlist_store() override {
        return self != nullptr ? self->playlist_store_ : nullptr;
    }

    int col_search_folded() const override {
        return COL_SEARCH_FOLDED;
    }

    bool ui_closing() const override {
        return self == nullptr || self->ui_closing_;
    }

    void on_search_filter_started() override {
        if (self != nullptr) {
            self->begin_playlist_filter_session();
            self->update_playlist_sort_headers();
        }
    }

    void on_search_filter_cleared() override {
        if (self != nullptr) {
            self->finish_playlist_filter_session();
            self->update_playlist_sort_headers();
        }
    }

    void on_search_filtered() override {
        if (self != nullptr) {
            self->sync_playlist_selection_to_filter();
            self->update_playlist_sort_headers();
        }
    }

    void on_search_cancelled() override {
        if (self == nullptr || self->playlist_view_ == nullptr) {
            return;
        }
        gtk_widget_grab_focus(self->playlist_view_);
    }

    void activate_filtered_playlist_selection() override {
        if (self != nullptr) {
            self->activate_filtered_playlist_selection();
        }
    }

    void begin_refilter() override {
        refilter_selection_blocker.reset();
        if (self != nullptr) {
            refilter_selection_blocker =
                std::make_unique<PlaylistSelectionSignalBlocker>(*self);
        }
    }

    void end_refilter() override {
        refilter_selection_blocker.reset();
    }
};

struct GtkPlayerWindow::StreamDelegate final : StreamPlaybackManager::Delegate {
    GtkPlayerWindow* self = nullptr;

    explicit StreamDelegate(GtkPlayerWindow* window) : self(window) {}

    bool ui_closing() const override {
        return self == nullptr || self->ui_closing_;
    }

    void on_now_playing_changed(const std::string& title) override {
        (void)title;
        if (self == nullptr || self->ui_closing_) {
            return;
        }
        if (!self->playlist_.empty() && self->current_track_index_ < self->playlist_.size()) {
            self->update_playlist_row(self->current_track_index_);
        }
        self->refresh_display();
        self->notify_mpris_state_changed();
    }

    void on_status_override_changed() override {
        if (self != nullptr && !self->ui_closing_) {
            self->refresh_display();
            self->ensure_stream_service_timer_running();
        }
    }

    void on_stream_health_changed(const std::string& url) override {
        if (self != nullptr && !self->ui_closing_) {
            self->refresh_stream_health_rows_for_url(url);
        }
    }

    void on_probe_finished(StreamPlaybackManager::ProbeResult result) override {
        if (self != nullptr) {
            self->handle_stream_probe_result(std::move(result));
        }
    }

    void on_reconnect_requested(std::size_t index) override {
        if (self != nullptr && !self->ui_closing_) {
            self->play_track_index(index);
        }
    }

    std::size_t find_playlist_index_by_url(const std::string& url) const override {
        return self != nullptr ? self->find_playlist_index_by_url(url) : static_cast<std::size_t>(-1);
    }
};

void GtkPlayerWindow::initialize_stream_subsystem() {
    stream_delegate_ = std::make_unique<StreamDelegate>(this);
    stream_manager_ = std::make_unique<StreamPlaybackManager>(*stream_delegate_);
}

void GtkPlayerWindow::append_stream_entry(const std::string& path,
                                         const std::string& hint_title,
                                         const std::string& hint_artist,
                                         const std::string& top_level_source_path) {
    PlaylistEntry entry;
    entry.audio_file_path = path;
    entry.top_level_source_path = top_level_source_path.empty() ? path : top_level_source_path;
    entry.original_order = next_playlist_original_order_++;
    entry.load_generation = metadata_generation_;
    entry.is_stream = true;
    entry.start_sample_known = true;
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
    entry.title = safe_utf8_for_display(entry.title);
    entry.performer = safe_utf8_for_display(entry.performer);
    entry.source_label = safe_utf8_for_display(entry.source_label);
    entry.metadata_state = MetadataState::Ready;
    playlist_.push_back(entry);
}

void GtkPlayerWindow::handle_stream_probe_result(StreamPlaybackManager::ProbeResult result) {
    if (ui_closing_ || stream_manager_ == nullptr) {
        return;
    }

    const std::size_t playlist_index = find_playlist_index_by_url(result.url);
    const bool connect_failed = !result.probe_ok || !result.info.live_format_probed;

    if (connect_failed) {
        const std::string error = !result.probe_ok
            ? (result.error.empty() ? std::string("Stream probe failed") : result.error)
            : "Stream unavailable";
        stream_manager_->note_broken(result.url, error);
        Logger::instance().error(std::string("Failed to probe stream: ") + error);
        if (result.stale) {
            return;
        }
        track_switch_in_progress_ = false;
        finish_handled_ = false;
        stream_manager_->set_status_override(std::string());
        GtkWidget* msg = gtk_message_dialog_new(GTK_WINDOW(window_),
                                                GTK_DIALOG_MODAL,
                                                GTK_MESSAGE_ERROR,
                                                GTK_BUTTONS_CLOSE,
                                                "%s",
                                                error.c_str());
        gtk_dialog_run(GTK_DIALOG(msg));
        gtk_widget_destroy(msg);
        notify_mpris_state_changed();
        return;
    }

    if (playlist_index != static_cast<std::size_t>(-1)) {
        PlaylistEntry& entry = playlist_[playlist_index];
        entry.source_sample_rate = result.info.source_format.sample_rate > 0 ? result.info.source_format.sample_rate : result.info.format.sample_rate;
        entry.source_bits_per_sample = result.info.source_format.bits_per_sample > 0 ? result.info.source_format.bits_per_sample : result.info.format.bits_per_sample;
        entry.decoded_format = result.info.format;
        if (entry.decoded_format.channels == 0) {
            entry.decoded_format.channels = 2;
        }
        if (!result.info.codec_name.empty()) {
            entry.codec_name = result.info.codec_name;
        }
        entry.source_bit_rate = result.info.bit_rate;
        entry.lossless_source = result.info.lossless;
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
    if (result.info.live_format_probed) {
        Logger::instance().info("Stream format probed: " + result.url + " -> " +
                                std::to_string(result.info.source_format.sample_rate) + " Hz / " +
                                std::to_string(result.info.source_format.bits_per_sample) + "-bit / " +
                                std::to_string(result.info.source_format.channels) + " ch");
    }

    if (result.stale) {
        return;
    }

    if (playlist_index == static_cast<std::size_t>(-1)) {
        track_switch_in_progress_ = false;
        finish_handled_ = false;
        return;
    }

    stream_manager_->set_status_override(std::string());
    play_track_index_at_offset(playlist_index,
                               result.playback.offset_samples,
                               true,
                               result.playback.preserve_paused,
                               result.playback.update_mpris_track);
}

void GtkPlayerWindow::begin_async_stream_probe_and_play(std::size_t index,
                                                       std::uint64_t offset_samples,
                                                       bool preserve_paused,
                                                       bool update_mpris_track,
                                                       bool skip_engine_stop,
                                                       std::uint64_t probe_generation) {
    if (stream_manager_ == nullptr || index >= playlist_.size()) {
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

    stream_manager_->set_status_override("Probing stream...");
    refresh_display();

    StreamPlaybackManager::ProbePlaybackRequest request;
    request.index = index;
    request.offset_samples = offset_samples;
    request.preserve_paused = preserve_paused;
    request.update_mpris_track = update_mpris_track;
    request.skip_engine_stop = skip_engine_stop;
    stream_manager_->begin_async_probe(request,
                                       entry.audio_file_path,
                                       forced_rate,
                                       forced_bits,
                                       probe_generation);
}

void GtkPlayerWindow::refresh_stream_health_rows_for_url(const std::string& url) {
    if (playlist_store_ == nullptr || playlist_.empty() || stream_manager_ == nullptr) {
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
                const bool stream_broken = entry.is_stream && stream_manager_->is_broken(entry.audio_file_path);
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

bool GtkPlayerWindow::prepare_play_track_preamble(std::size_t index,
                                                 std::uint64_t offset_samples,
                                                 bool start_playback,
                                                 bool preserve_paused,
                                                 bool update_mpris_track,
                                                 bool skip_engine_stop,
                                                 std::uint64_t probe_generation) {
    (void)probe_generation;
    if (stream_manager_ == nullptr || index >= playlist_.size()) {
        return false;
    }

    StreamPlaybackManager& manager = *stream_manager_;
    const bool reconnecting_same_stream =
        manager.should_keep_sidecar_for_play(index, playlist_[index].is_stream);
    if (!reconnecting_same_stream) {
        manager.stop_sidecar();
    }
    if (!manager.is_reconnect_target(index)) {
        manager.cancel_reconnect();
    }

    if (!reconnecting_same_stream || !start_playback || skip_engine_stop) {
        return false;
    }

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
                                                     resume->update_mpris_track);
        }
        return G_SOURCE_REMOVE;
    }, resume);
    return true;
}

std::unique_ptr<IAudioDecoder> GtkPlayerWindow::open_stream_decoder(std::size_t index,
                                                                   std::uint64_t initial_offset,
                                                                   bool preserve_paused,
                                                                   bool update_mpris_track,
                                                                   bool skip_engine_stop,
                                                                   std::uint64_t probe_generation,
                                                                   bool* async_started) {
    if (async_started != nullptr) {
        *async_started = false;
    }
    if (stream_manager_ == nullptr || index >= playlist_.size()) {
        return nullptr;
    }

    PlaylistEntry& entry = playlist_[index];
    if (!entry.stream_format_probed) {
        begin_async_stream_probe_and_play(index,
                                          initial_offset,
                                          preserve_paused,
                                          update_mpris_track,
                                          skip_engine_stop,
                                          probe_generation);
        if (async_started != nullptr) {
            *async_started = true;
        }
        return nullptr;
    }

    std::unique_ptr<IAudioDecoder> decoder = create_decoder_for_entry(entry);
    decoder->open(entry.audio_file_path);
    const AudioFormat stream_format = decoder->format();
    entry.decoded_format = stream_format;
    entry.source_sample_rate = stream_format.sample_rate;
    entry.source_bits_per_sample = stream_format.bits_per_sample;
    entry.stream_format_probed = true;
    Logger::instance().info("Streaming playback: " + entry.audio_file_path + " (" +
                            std::to_string(stream_format.sample_rate) + " Hz)");
    stream_manager_->start_sidecar(entry.audio_file_path);
    return decoder;
}

void GtkPlayerWindow::on_stream_play_succeeded(std::size_t index) {
    if (stream_manager_ != nullptr &&
        index < playlist_.size() &&
        playlist_[index].is_stream) {
        stream_manager_->clear_reconnect_after_success();
    }
}

void GtkPlayerWindow::on_stream_play_failed(const std::string& url, const std::string& error) {
    if (stream_manager_ != nullptr) {
        stream_manager_->note_broken(url, error);
    }
}

void GtkPlayerWindow::on_stream_ui_timer_tick(const PlaybackStatusSnapshot& status) {
    if (stream_manager_ == nullptr) {
        return;
    }
    if (!playlist_.empty() && current_track_index_ < playlist_.size()) {
        const PlaylistEntry& current = playlist_[current_track_index_];
        if (current.is_stream) {
            stream_manager_->update_from_playback(current.audio_file_path, status.playing, status.paused);
        } else {
            stream_manager_->reset_health_tracking();
        }
    } else {
        stream_manager_->reset_health_tracking();
    }

    stream_manager_->tick_reconnect(std::chrono::steady_clock::now());
}

void GtkPlayerWindow::on_stream_transport_finished(std::size_t finished_index, bool* should_advance) {
    constexpr int kMaxStreamReconnectAttempts = 5;
    if (should_advance == nullptr ||
        stream_manager_ == nullptr ||
        finished_index >= playlist_.size() ||
        !playlist_[finished_index].is_stream) {
        return;
    }

    *should_advance = false;
    const std::string& stream_url = playlist_[finished_index].audio_file_path;
    StreamPlaybackManager& manager = *stream_manager_;
    if (manager.reconnect_attempts() < kMaxStreamReconnectAttempts) {
        manager.note_broken(stream_url, "Stream unavailable");
        manager.schedule_reconnect(finished_index, "Reconnecting...");
    } else {
        manager.set_status_override("Stream unavailable");
        manager.cancel_reconnect();
        manager.note_broken(stream_url, "Stream unavailable");
    }
}

bool GtkPlayerWindow::try_load_stream_source(const std::string& path,
                                            std::vector<std::string>* accepted_sources,
                                            bool quiet,
                                            const std::string& top_level_source_path) {
    if (!StreamAudioDecoder::is_stream_uri(path)) {
        return false;
    }

    const std::string top_level = top_level_source_path.empty() ? path : top_level_source_path;
    const std::size_t before = playlist_.size();
    try {
        append_stream_entry(path, std::string(), std::string(), top_level);
    } catch (const std::exception& ex) {
        const std::string message = std::string("Cannot load stream: ") + path + " (" + ex.what() + ")";
        if (quiet) {
            Logger::instance().debug(message);
        } else {
            Logger::instance().error(message);
        }
    }

    if (accepted_sources != nullptr && playlist_.size() > before) {
        accepted_sources->push_back(top_level);
    }
    return true;
}

bool GtkPlayerWindow::entry_playable(bool is_stream, bool metadata_ready) const {
    return is_stream || metadata_ready;
}

std::size_t GtkPlayerWindow::append_source_stream_placeholder(const std::string& path,
                                                             const std::string& top_level_source_path,
                                                             const std::string& hint_title,
                                                             const std::string& hint_artist) {
    if (!StreamAudioDecoder::is_stream_uri(path)) {
        return 0;
    }
    append_stream_entry(path, hint_title, hint_artist, top_level_source_path);
    return 1;
}

bool GtkPlayerWindow::begin_play_track_stream_preamble(std::size_t index,
                                                      std::uint64_t offset_samples,
                                                      bool start_playback,
                                                      bool preserve_paused,
                                                      bool update_mpris_track,
                                                      bool skip_engine_stop,
                                                      std::uint64_t* probe_generation_out) {
    if (probe_generation_out != nullptr) {
        *probe_generation_out = 0;
    }
    if (stream_manager_ == nullptr || index >= playlist_.size()) {
        return stream_manager_ == nullptr ? false : true;
    }
    const PlaylistEntry& entry = playlist_[index];
    const bool metadata_ready = entry.metadata_state == MetadataState::Ready;
    if (!entry_playable(entry.is_stream, metadata_ready)) {
        return false;
    }

    const std::uint64_t probe_generation = stream_manager_->bump_probe_generation();
    if (probe_generation_out != nullptr) {
        *probe_generation_out = probe_generation;
    }
    return prepare_play_track_preamble(index,
                                       offset_samples,
                                       start_playback,
                                       preserve_paused,
                                       update_mpris_track,
                                       skip_engine_stop,
                                       probe_generation);
}

void GtkPlayerWindow::handle_stream_playback_error(bool is_stream,
                                                   const std::string& audio_file_path,
                                                   const std::string& error) {
    if (is_stream) {
        on_stream_play_failed(audio_file_path, error);
    }
    GtkWidget* msg = gtk_message_dialog_new(GTK_WINDOW(window_),
                                            GTK_DIALOG_MODAL,
                                            GTK_MESSAGE_ERROR,
                                            GTK_BUTTONS_CLOSE,
                                            "%s",
                                            error.c_str());
    gtk_dialog_run(GTK_DIALOG(msg));
    gtk_widget_destroy(msg);
}

void GtkPlayerWindow::apply_stream_mpris_action_wrappers(MprisService::Actions& actions) {
    auto wrap_selection = [this](std::function<void()> action, bool skip_when_searching) {
        return [this, action = std::move(action), skip_when_searching]() {
            if (!skip_when_searching || !playlist_search_enabled_) {
                update_playlist_selection_from_ui();
            }
            action();
        };
    };

    auto play = std::move(actions.play);
    actions.play = wrap_selection(std::move(play), true);

    auto play_pause = std::move(actions.play_pause);
    actions.play_pause = wrap_selection(std::move(play_pause), true);

    auto next = std::move(actions.next);
    actions.next = wrap_selection(std::move(next), false);

    auto previous = std::move(actions.previous);
    actions.previous = wrap_selection(std::move(previous), false);
}

void GtkPlayerWindow::apply_stream_fields_to_mpris_state(MprisPlayerState& state,
                                                        const PlaylistEntry& track) const {
    if (!track.is_stream || stream_manager_ == nullptr) {
        return;
    }
    if (!stream_manager_->now_playing().empty()) {
        state.title = stream_manager_->now_playing();
    }
    state.url = track.audio_file_path;
    state.art_url.clear();
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

bool GtkPlayerWindow::stream_service_timer_needed() const {
    if (ui_closing_ || stream_manager_ == nullptr) {
        return false;
    }
    if (stream_manager_->reconnect_pending()) {
        return true;
    }
    if (playlist_.empty() || current_track_index_ >= playlist_.size()) {
        return false;
    }
    if (!playlist_[current_track_index_].is_stream) {
        return false;
    }
    const PlaybackStatusSnapshot status = engine_.snapshot();
    return status.playing || status.paused || status.finished;
}

void GtkPlayerWindow::ensure_stream_service_timer_running() {
    if (ui_closing_ || stream_service_timer_id_ != 0 || !stream_service_timer_needed()) {
        return;
    }
    stream_service_timer_id_ = g_timeout_add(
        500,
        GtkPlayerWindow::on_stream_service_tick,
        this);
}

void GtkPlayerWindow::stop_stream_service_timer() {
    if (stream_service_timer_id_ == 0) {
        return;
    }
    g_source_remove(stream_service_timer_id_);
    stream_service_timer_id_ = 0;
}

gboolean GtkPlayerWindow::on_stream_service_tick(gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || self->ui_closing_) {
        if (self != nullptr) {
            self->stream_service_timer_id_ = 0;
        }
        return G_SOURCE_REMOVE;
    }

    self->on_stream_ui_timer_tick(self->engine_.snapshot());

    if (!self->stream_service_timer_needed()) {
        self->stream_service_timer_id_ = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void GtkPlayerWindow::initialize_playlist_search() {
    search_delegate_ = std::make_unique<SearchDelegate>(this);
    search_controller_ = std::make_unique<PlaylistSearchController>(*search_delegate_);
}

void GtkPlayerWindow::begin_playlist_selection_sync() {
    ++playlist_selection_sync_depth_;
    if (playlist_selection_sync_depth_ != 1) {
        return;
    }

    playlist_selection_syncing_ = true;
    playlist_selection_handler_blocked_ = false;
    if (playlist_view_ == nullptr || playlist_selection_changed_handler_id_ == 0) {
        return;
    }

    GtkTreeSelection* selection =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(playlist_view_));
    g_signal_handler_block(selection, playlist_selection_changed_handler_id_);
    playlist_selection_handler_blocked_ = true;
}

void GtkPlayerWindow::end_playlist_selection_sync() {
    if (playlist_selection_sync_depth_ == 0) {
        return;
    }
    --playlist_selection_sync_depth_;
    if (playlist_selection_sync_depth_ != 0) {
        return;
    }

    if (playlist_selection_handler_blocked_ && playlist_view_ != nullptr &&
        playlist_selection_changed_handler_id_ != 0) {
        GtkTreeSelection* selection =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(playlist_view_));
        g_signal_handler_unblock(selection, playlist_selection_changed_handler_id_);
    }
    playlist_selection_handler_blocked_ = false;
    playlist_selection_syncing_ = false;
}

void GtkPlayerWindow::apply_playlist_search_handler_connections() {
    if (playlist_view_ == nullptr) {
        return;
    }

    if (playlist_key_press_handler_id_ != 0) {
        g_signal_handler_disconnect(playlist_view_, playlist_key_press_handler_id_);
        playlist_key_press_handler_id_ = 0;
    }
    if (playlist_focus_in_handler_id_ != 0) {
        g_signal_handler_disconnect(playlist_view_, playlist_focus_in_handler_id_);
        playlist_focus_in_handler_id_ = 0;
    }
    if (playlist_selection_changed_handler_id_ != 0) {
        GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(playlist_view_));
        g_signal_handler_disconnect(selection, playlist_selection_changed_handler_id_);
        playlist_selection_changed_handler_id_ = 0;
    }

    GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(playlist_view_));
    playlist_selection_changed_handler_id_ =
        g_signal_connect(selection, "changed", G_CALLBACK(patches::on_playlist_selection_changed), this);
    if (playlist_search_enabled_) {
        playlist_key_press_handler_id_ =
            g_signal_connect(playlist_view_, "key-press-event", G_CALLBACK(patches::on_playlist_view_key_press), this);
        playlist_focus_in_handler_id_ =
            g_signal_connect(playlist_view_, "focus-in-event", G_CALLBACK(patches::on_playlist_focus_in), this);
    }
}

void GtkPlayerWindow::account_playlist_search_window_resize_height(
    int window_height) {
    if (!playlist_search_window_resize_pending_ ||
        playlist_search_window_resize_last_height_ <= 0 ||
        window_height <= 0) {
        return;
    }

    const int delta = window_height - playlist_search_window_resize_last_height_;
    if (playlist_search_window_resize_enabling_ && delta > 0) {
        const std::int64_t accumulated =
            static_cast<std::int64_t>(playlist_search_runtime_height_compensation_) +
            static_cast<std::int64_t>(delta);
        playlist_search_runtime_height_compensation_ = static_cast<int>(
            std::min<std::int64_t>(accumulated, std::numeric_limits<int>::max()));
    } else if (!playlist_search_window_resize_enabling_ && delta < 0) {
        playlist_search_runtime_height_compensation_ = std::max(
            0,
            playlist_search_runtime_height_compensation_ + delta);
    }
    playlist_search_window_resize_last_height_ = window_height;
}

void GtkPlayerWindow::cancel_playlist_search_window_resize() {
    if (playlist_search_window_resize_idle_id_ != 0) {
        g_source_remove(playlist_search_window_resize_idle_id_);
        playlist_search_window_resize_idle_id_ = 0;
    }
    playlist_search_window_resize_pending_ = false;
    playlist_search_window_resize_enabling_ = false;
    playlist_search_preserved_viewport_height_ = 0;
    playlist_search_window_resize_last_height_ = 0;
    playlist_search_window_resize_min_height_ = 0;
    playlist_search_window_resize_attempts_ = 0;
    playlist_search_window_resize_waiting_for_window_event_ = false;
}

void GtkPlayerWindow::queue_playlist_layout_reflow() {
    // GtkTreeView can keep the child allocation from the pre-search layout on
    // an empty or partially populated model even after the top-level window
    // has accepted its new size. Invalidate the complete playlist allocation
    // chain once the resize transaction has settled; no synthetic input or
    // secondary window resize is required.
    if (playlist_view_ != nullptr) {
        gtk_widget_queue_resize(playlist_view_);
    }
    if (playlist_scrolled_ != nullptr) {
        gtk_widget_queue_resize(playlist_scrolled_);
    }
    if (playlist_panel_ != nullptr) {
        gtk_widget_queue_resize(playlist_panel_);
    }
}

void GtkPlayerWindow::complete_playlist_search_window_resize() {
    cancel_playlist_search_window_resize();
    queue_playlist_layout_reflow();
}

void GtkPlayerWindow::schedule_playlist_search_window_resize() {
    if (!playlist_search_window_resize_pending_ ||
        playlist_search_window_resize_waiting_for_window_event_ ||
        playlist_search_window_resize_idle_id_ != 0 ||
        ui_closing_) {
        return;
    }
    playlist_search_window_resize_idle_id_ = g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        GtkPlayerWindow::on_playlist_search_window_resize_idle,
        this,
        nullptr);
}

void GtkPlayerWindow::on_playlist_scrolled_size_allocate(
    GtkWidget*,
    GtkAllocation*,
    gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self != nullptr) {
        self->schedule_playlist_search_window_resize();
    }
}

gboolean GtkPlayerWindow::on_window_configure_event(
    GtkWidget*,
    GdkEventConfigure* event,
    gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || !self->playlist_search_window_resize_pending_) {
        return FALSE;
    }

    if (event != nullptr && event->height > 0) {
        self->account_playlist_search_window_resize_height(event->height);
    }
    self->playlist_search_window_resize_waiting_for_window_event_ = false;
    self->schedule_playlist_search_window_resize();
    return FALSE;
}

void GtkPlayerWindow::on_playlist_field_cell_data(GtkTreeViewColumn* column,
                                                   GtkCellRenderer* renderer,
                                                   GtkTreeModel* model,
                                                   GtkTreeIter* iter,
                                                   gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || column == nullptr || renderer == nullptr ||
        model == nullptr || iter == nullptr) {
        return;
    }

    std::size_t slot = 0;
    int model_column = -1;
    if (column == self->playlist_artist_column_) {
        slot = 0;
        model_column = COL_ARTIST;
    } else if (column == self->playlist_title_column_) {
        slot = 1;
        model_column = COL_TITLE;
    } else if (column == self->playlist_album_column_) {
        slot = 2;
        model_column = COL_ALBUM;
    } else if (column == self->playlist_source_column_) {
        slot = 3;
        model_column = COL_SOURCE;
    } else {
        return;
    }

    gchar* model_text = nullptr;
    gtk_tree_model_get(model, iter, model_column, &model_text, -1);
    const std::string full_text = model_text != nullptr ? model_text : "";
    g_free(model_text);

    std::string presentation = full_text;
    PangoEllipsizeMode ellipsize = PANGO_ELLIPSIZE_NONE;
    if (self->playlist_field_width_limit_enabled_) {
        const int initial_cap = self->playlist_field_width_initial_caps_[slot];
        const int fixed_width = gtk_tree_view_column_get_fixed_width(column);
        if (initial_cap > 0) {
            // Limited columns start with a strict character-capped presentation.
            // GtkTreeViewColumn::fixed-width is the durable user-resize state:
            // expanding beyond the configured initial cap reveals the full model
            // value, while shrinking keeps the capped presentation and lets Pango
            // ellipsize it further if necessary.
            if (fixed_width <= initial_cap) {
                const int max_chars = std::max(
                    kMinPlaylistFieldWidthChars,
                    std::min(kMaxPlaylistFieldWidthChars, self->playlist_field_width_chars_));
                presentation = playlist_field_limited_presentation(full_text, max_chars);
            }
            ellipsize = PANGO_ELLIPSIZE_END;
        } else if (fixed_width > 0) {
            // A field that is already within the configured character limit uses
            // normal GTK GROW_ONLY sizing. Once the user explicitly resizes that
            // column, GTK records the width in fixed-width; only then enable end
            // ellipsis for further manual shrink.
            ellipsize = PANGO_ELLIPSIZE_END;
        }
    }

    g_object_set(renderer,
                 "text", presentation.c_str(),
                 "ellipsize", ellipsize,
                 "weight",
                 playlist_row_is_playing(GTK_TREE_VIEW(self->playlist_view_), model, iter)
                     ? PANGO_WEIGHT_BOLD
                     : PANGO_WEIGHT_NORMAL,
                 "weight-set", TRUE,
                 nullptr);
}

gboolean GtkPlayerWindow::on_playlist_search_window_resize_idle(gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr) {
        return G_SOURCE_REMOVE;
    }
    self->playlist_search_window_resize_idle_id_ = 0;
    if (!self->playlist_search_window_resize_pending_ || self->ui_closing_ ||
        self->window_ == nullptr || self->playlist_scrolled_ == nullptr ||
        !gtk_widget_get_realized(self->window_)) {
        self->cancel_playlist_search_window_resize();
        return G_SOURCE_REMOVE;
    }

    int width = 0;
    int height = 0;
    gtk_window_get_size(GTK_WINDOW(self->window_), &width, &height);
    if (width <= 0 || height <= 0) {
        self->cancel_playlist_search_window_resize();
        return G_SOURCE_REMOVE;
    }

    self->account_playlist_search_window_resize_height(height);

    const int actual_viewport_height =
        gtk_widget_get_allocated_height(self->playlist_scrolled_);
    const int correction = self->playlist_search_preserved_viewport_height_ -
                           actual_viewport_height;

    int adjusted_height = height;
    if (self->playlist_search_window_resize_enabling_) {
        // Adding search is allowed to grow the top-level window only.  If the
        // current work area cannot provide the requested space, keep the
        // existing size instead of shrinking a user-sized window.
        if (correction <= 1) {
            self->complete_playlist_search_window_resize();
            return G_SOURCE_REMOVE;
        }
        const std::int64_t requested_height_value =
            static_cast<std::int64_t>(height) + static_cast<std::int64_t>(correction);
        const int requested_height = static_cast<int>(std::max<std::int64_t>(
            1,
            std::min<std::int64_t>(
                requested_height_value,
                std::numeric_limits<int>::max())));
        adjusted_height = clamp_window_height_to_workarea(
            self->window_, requested_height);
        if (adjusted_height <= height) {
            self->complete_playlist_search_window_resize();
            return G_SOURCE_REMOVE;
        }
    } else {
        // Removing search only reverses height that PCM Transport actually
        // added when search was enabled.  A failed or partial grow therefore
        // cannot make the window smaller than its pre-search/user-sized state.
        if (correction >= -1 || self->playlist_search_window_resize_min_height_ <= 0 ||
            height <= self->playlist_search_window_resize_min_height_ + 1) {
            self->complete_playlist_search_window_resize();
            return G_SOURCE_REMOVE;
        }
        const std::int64_t requested_height_value =
            static_cast<std::int64_t>(height) + static_cast<std::int64_t>(correction);
        const int requested_height = static_cast<int>(std::max<std::int64_t>(
            self->playlist_search_window_resize_min_height_,
            std::min<std::int64_t>(
                requested_height_value,
                std::numeric_limits<int>::max())));
        adjusted_height = requested_height;
        if (adjusted_height >= height) {
            self->complete_playlist_search_window_resize();
            return G_SOURCE_REMOVE;
        }
    }

    if (self->playlist_search_window_resize_attempts_ >= 4) {
        self->complete_playlist_search_window_resize();
        return G_SOURCE_REMOVE;
    }

    ++self->playlist_search_window_resize_attempts_;
    // The next correction is driven by the top-level configure event that
    // acknowledges the window manager's actual size. Child size-allocate
    // notifications are ignored while that acknowledgement is pending.
    self->playlist_search_window_resize_waiting_for_window_event_ = true;
    gtk_window_resize(GTK_WINDOW(self->window_), width, adjusted_height);
    return G_SOURCE_REMOVE;
}

void GtkPlayerWindow::adjust_playlist_search_window_height(
    bool enabled,
    int preserved_viewport_height) {
    if (window_ == nullptr || playlist_search_window_height_adjusted_ == enabled) {
        return;
    }

    if (!gtk_widget_get_realized(window_)) {
        int width = 0;
        int height = 0;
        gtk_window_get_default_size(GTK_WINDOW(window_), &width, &height);
        if (width <= 0) {
            width = kDefaultWindowWidth;
        }
        if (height <= 0) {
            height = kDefaultWindowHeight;
        }
        int delta = playlist_search_unrealized_height_delta_;
        if (enabled) {
            const int entry_height = search_controller_ != nullptr
                ? search_controller_->search_entry_natural_height()
                : 0;
            const int spacing = playlist_panel_ != nullptr
                ? gtk_box_get_spacing(GTK_BOX(playlist_panel_))
                : 0;
            delta = std::max(0, entry_height + spacing);
        }
        const int requested_height = enabled
            ? height + delta
            : std::max(1, height - delta);
        const int adjusted_height =
            clamp_window_height_to_workarea(window_, requested_height);
        gtk_window_set_default_size(GTK_WINDOW(window_), width, adjusted_height);
        playlist_search_unrealized_height_delta_ = enabled
            ? std::max(0, adjusted_height - height)
            : 0;
        playlist_search_runtime_height_compensation_ = enabled
            ? playlist_search_unrealized_height_delta_
            : 0;
        playlist_search_window_height_adjusted_ = enabled;
        return;
    }

    int window_width = 0;
    int window_height = 0;
    gtk_window_get_size(GTK_WINDOW(window_), &window_width, &window_height);
    // Capture any resize already accepted by the window manager before a
    // rapid search toggle cancels the previous transaction.
    account_playlist_search_window_resize_height(window_height);
    const int compensation_to_reverse = enabled
        ? 0
        : playlist_search_runtime_height_compensation_;

    cancel_playlist_search_window_resize();
    playlist_search_window_resize_enabling_ = enabled;
    playlist_search_window_resize_last_height_ = std::max(0, window_height);
    if (!enabled && window_height > 0 && compensation_to_reverse > 0) {
        playlist_search_window_resize_min_height_ = std::max(
            1,
            window_height - compensation_to_reverse);
    }

    if (playlist_scrolled_ != nullptr) {
        const int allocated_height = preserved_viewport_height > 0
            ? preserved_viewport_height
            : gtk_widget_get_allocated_height(playlist_scrolled_);
        const bool resize_allowed = enabled ||
            playlist_search_window_resize_min_height_ > 0;
        if (allocated_height > 0 && resize_allowed) {
            playlist_search_preserved_viewport_height_ = allocated_height;
            playlist_search_window_resize_pending_ = true;
            if (playlist_panel_ != nullptr) {
                gtk_widget_queue_resize(playlist_panel_);
            } else {
                gtk_widget_queue_resize(window_);
            }
            // Coalesce the first layout observation at the end of the current
            // main-loop iteration. Further steps are driven only by GTK/GDK
            // allocation/configure events, not by elapsed time.
            schedule_playlist_search_window_resize();
        }
    }
    playlist_search_window_height_adjusted_ = enabled;
}

void GtkPlayerWindow::apply_playlist_search_ui_state() {
    if (playlist_view_ == nullptr || playlist_store_ == nullptr) {
        apply_playlist_search_handler_connections();
        update_playlist_sort_headers();
        return;
    }

    GtkTreeView* view = GTK_TREE_VIEW(playlist_view_);
    GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
    const bool search_was_enabled = search_controller_ != nullptr;
    const bool had_filter_session = playlist_filter_session_active_;

    double preserved_scroll_value = 0.0;
    const bool preserved_scroll_valid =
        capture_playlist_vertical_position(&preserved_scroll_value);
    const int preserved_viewport_height =
        playlist_scrolled_ != nullptr && gtk_widget_get_realized(window_)
            ? gtk_widget_get_allocated_height(playlist_scrolled_)
            : 0;

    PlaylistSelectionMode preserved_mode = playlist_selection_mode_without_filter_candidate();
    std::size_t preserved_index = playlist_selection_index_without_filter_candidate();

    if (!search_was_enabled) {
        preserved_index = current_track_index_;
        GtkTreeModel* selected_model = nullptr;
        GtkTreeIter selected_iter;
        if (gtk_tree_selection_get_selected(selection, &selected_model, &selected_iter)) {
            std::size_t selected_index = 0;
            if (patches::playlist_index_from_model_iter(selected_model,
                                                        &selected_iter,
                                                        COL_INDEX,
                                                        &selected_index) &&
                selected_index < playlist_.size()) {
                preserved_index = selected_index;
            }
        }
        preserved_mode = preserved_index == current_track_index_
            ? PlaylistSelectionMode::FollowTransport
            : PlaylistSelectionMode::ExplicitUser;
    }

    if (playlist_search_enabled_) {
        playlist_selection_mode_ = preserved_mode;
        selected_playlist_index_ = preserved_mode == PlaylistSelectionMode::FollowTransport
            ? current_track_index_
            : preserved_index;
        playlist_selection_mode_before_filter_candidate_ = playlist_selection_mode_;
        playlist_selection_index_before_filter_candidate_ = selected_playlist_index_;
        playlist_filter_candidate_valid_ = false;

        rebuild_playlist_search_cache();

        if (search_controller_ == nullptr && playlist_panel_ != nullptr) {
            initialize_playlist_search();
            search_controller_->install_in_panel(GTK_BOX(playlist_panel_));
        }

        adjust_playlist_search_window_height(true, preserved_viewport_height);

        if (search_controller_ != nullptr && search_controller_->filter_model() != nullptr) {
            gtk_tree_view_set_model(view, GTK_TREE_MODEL(search_controller_->filter_model()));
        }
        gtk_tree_view_set_enable_search(view, FALSE);
        apply_playlist_search_handler_connections();
        if (search_controller_ != nullptr && search_controller_->is_filter_active()) {
            search_controller_->refilter();
        } else if (!playlist_.empty()) {
            const std::size_t target = playlist_selection_mode_ == PlaylistSelectionMode::FollowTransport
                ? current_track_index_
                : selected_playlist_index_;
            select_playlist_row(target, PlaylistScrollPolicy::PreserveViewport);
        }
        if (preserved_scroll_valid) {
            restore_playlist_vertical_position(preserved_scroll_value);
        }
    } else {
        apply_playlist_search_handler_connections();
        if (search_controller_ != nullptr) {
            search_controller_->invalidate();
        }
        gtk_tree_view_set_model(view, GTK_TREE_MODEL(playlist_store_));
        gtk_tree_view_set_enable_search(view, TRUE);
        search_controller_.reset();
        search_delegate_.reset();
        clear_playlist_search_cache();
        adjust_playlist_search_window_height(false, preserved_viewport_height);

        if (had_filter_session) {
            finish_playlist_filter_session();
        } else {
            playlist_selection_mode_ = preserved_mode;
            selected_playlist_index_ = preserved_mode == PlaylistSelectionMode::FollowTransport
                ? current_track_index_
                : preserved_index;
            playlist_selection_mode_before_filter_candidate_ = playlist_selection_mode_;
            playlist_selection_index_before_filter_candidate_ = selected_playlist_index_;
            playlist_filter_candidate_valid_ = false;

            if (!playlist_.empty()) {
                const std::size_t target = playlist_selection_mode_ == PlaylistSelectionMode::FollowTransport
                    ? current_track_index_
                    : selected_playlist_index_;
                select_playlist_row(target, PlaylistScrollPolicy::PreserveViewport);
            }
            if (preserved_scroll_valid) {
                restore_playlist_vertical_position(preserved_scroll_value);
            }
        }
    }
    update_playlist_sort_headers();
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

GtkPlayerWindow::GtkPlayerWindow()
    : log_path_("pcm_transport.log"),
      random_generator_(static_cast<std::uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count())) {
    initialize_stream_subsystem();
    reset_dsd_pcm_defaults();
    load_preferences();
    repeat_enabled_ = false;
    random_enabled_ = false;
    Logger::instance().configure(logging_enabled_, log_path_, log_errors_only_);
    engine_.set_soft_volume_percent(soft_volume_percent_);
    engine_.set_soft_eq(bass_db_, treble_db_);
    engine_.set_pre_eq_headroom_tenths_db(pre_eq_headroom_tenths_db_);
    engine_.set_soft_eq_profile(bass_shelf_hz_, treble_shelf_hz_);
    engine_.set_deep_bass_enabled(deep_bass_enabled_);
    engine_.set_deep_bass_preset(deep_bass_internal_from_ui(deep_bass_preset_));
    if (stream_manager_ != nullptr) {
        stream_manager_->load_health_registry();
    }
    install_playback_event_bridge();
}

GtkPlayerWindow::~GtkPlayerWindow() {
    ui_closing_ = true;
    uninstall_playback_event_bridge();
    stop_bitperfect_test_worker();
    flush_preferences_save();
    search_controller_.reset();
    search_delegate_.reset();
    if (stream_manager_ != nullptr) {
        stream_manager_->shutdown();
    }
    stop_stream_service_timer();
    stop_ui_updates();
    cancel_pending_seek();
    stop_source_scan_worker();
    stop_metadata_worker();
    mpris_service_.reset();
    stop_playback();
}

void GtkPlayerWindow::show(const std::string& program_name,
                           const std::vector<std::string>& source_paths) {
    app_ = gtk_application_new(kApplicationId, G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app_, "activate", G_CALLBACK(GtkPlayerWindow::on_activate), this);
    g_signal_connect(app_, "open", G_CALLBACK(GtkPlayerWindow::on_open), this);

    std::vector<std::string> arguments;
    arguments.reserve(source_paths.size() + 1);
    arguments.push_back(program_name.empty() ? std::string("pcm_transport") : program_name);
    for (const std::string& source_path : source_paths) {
        if (!source_path.empty() && source_path[0] == '-') {
            arguments.push_back("./" + source_path);
        } else {
            arguments.push_back(source_path);
        }
    }

    std::vector<char*> argument_pointers;
    argument_pointers.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) {
        argument_pointers.push_back(argument.data());
    }
    argument_pointers.push_back(nullptr);

    g_application_run(G_APPLICATION(app_),
                      static_cast<int>(arguments.size()),
                      argument_pointers.data());
    g_object_unref(app_);
    app_ = nullptr;
}

void GtkPlayerWindow::on_activate(GtkApplication* app, gpointer user_data) {
    install_default_application_icons();
    GtkPlayerWindow* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self->window_ == nullptr) {
        self->build_ui(app);
    }
    if (self->window_ != nullptr) {
        gtk_window_present(GTK_WINDOW(self->window_));
    }
}

void GtkPlayerWindow::on_open(GApplication* application,
                              GFile** files,
                              gint file_count,
                              const gchar*,
                              gpointer user_data) {
    GtkPlayerWindow* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || self->ui_closing_) {
        return;
    }

    install_default_application_icons();
    if (self->window_ == nullptr) {
        self->build_ui(GTK_APPLICATION(application));
    }

    std::vector<std::string> source_paths;
    if (files != nullptr && file_count > 0) {
        source_paths.reserve(static_cast<std::size_t>(file_count));
        for (gint index = 0; index < file_count; ++index) {
            if (files[index] == nullptr) {
                continue;
            }
            gchar* local_path = g_file_get_path(files[index]);
            if (local_path == nullptr) {
                Logger::instance().error("Cannot open a non-local source");
                continue;
            }
            const std::string path(local_path);
            g_free(local_path);
            const bool regular_file = g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR);
            const bool directory = g_file_test(path.c_str(), G_FILE_TEST_IS_DIR);
            if (!regular_file && !directory) {
                Logger::instance().error("Cannot open unavailable source: " + path);
                continue;
            }
            if (regular_file && !is_supported_media_path(path)) {
                Logger::instance().error("Cannot open unsupported source: " + path);
                continue;
            }
            source_paths.push_back(path);
        }
    }

    if (!source_paths.empty() &&
        self->open_source_paths(source_paths, true, false, true)) {
        self->remember_open_directory_from_sources(source_paths);
    }
    if (self->window_ != nullptr) {
        gtk_window_present(GTK_WINDOW(self->window_));
    }
}

void GtkPlayerWindow::build_ui(GtkApplication* app) {
    start_metadata_worker();
    window_ = gtk_application_window_new(app);
    gtk_window_set_icon_name(GTK_WINDOW(window_), kApplicationId);
    gtk_window_set_title(GTK_WINDOW(window_), "PCM Transport v0.9.114");
    gtk_window_set_default_size(GTK_WINDOW(window_), kDefaultWindowWidth, kDefaultWindowHeight);
    gtk_container_set_border_width(GTK_CONTAINER(window_), 16);

    GtkCssProvider* provider = gtk_css_provider_new();
    const char* css =
        "window { background: #2b2f34; }"
        ".rack { background: linear-gradient(to bottom, #9aa0a6, #8f949a); border-radius: 12px; padding: 18px; }"
        ".display { background: #0f2413; color: #9cff9c; border-radius: 8px; padding: 12px; font-family: monospace; }"
        ".display-track { font-size: 18px; }"
        ".display-time { font-size: 24px; font-weight: bold; }"
        ".transport-button { min-height: 42px; min-width: 46px; font-weight: bold; padding: 2px 8px; }"
        ".playlist-search-entry { border-radius: 6px; min-height: 32px; }"
        ".transport-button-thin { min-height: 19px; min-width: 86px; font-weight: bold; padding: 1px 8px; }"
        ".transport-icon { font-size: 18px; color: #25313a; }"
        ".transport-mode { font-size: 11px; font-weight: bold; padding: 2px 6px; color: #25313a; }"
        "treeview.view:selected, treeview.view:selected:focus { background-color: #6f7780; color: #ffffff; }"
        "treeview.view:selected:hover { background-color: #78818a; color: #ffffff; }"
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
        ".pcm-dialog-host { margin: 0; padding: 0; border-width: 0; }"
        ".alsa-probe-cell { border: 1px solid #9aa1a8; padding: 3px 5px; color: #25313a; background-color: #f7f7f7; }"
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
    gtk_widget_set_margin_end(badge_clip_, 6);
    gtk_widget_set_opacity(badge_clip_, 0.0);

    badge_box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_set_spacing(GTK_BOX(badge_box_), 6);
    gtk_widget_set_halign(badge_box_, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(badge_wrap), badge_box_, FALSE, FALSE, 0);

    badge_lossless_ = gtk_label_new("LOSSLESS");
    badge_redbook_ = gtk_label_new("RED BOOK PCM");
    badge_native_ = gtk_label_new("NATIVE DECODE");
    badge_dsp_ = gtk_label_new("DSP");
    badge_random_ = gtk_label_new("RANDOM");
    badge_repeat_ = gtk_label_new("REPEAT");
    GtkWidget* badges[] = {
        badge_lossless_, badge_redbook_, badge_native_, badge_dsp_, badge_repeat_, badge_random_
    };
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
    btn_repeat_ = gtk_button_new_with_label("MODE");
    btn_settings_ = gtk_button_new_with_label("Settings");
    btn_eq_ = gtk_button_new_with_label("DSP Studio");
    btn_alsamixer_ = gtk_button_new_with_label("alsamixer");
    btn_about_ = gtk_button_new_with_label("About");

    GtkWidget* transport_icon_buttons[] = {btn_prev_, btn_play_, btn_pause_, btn_stop_, btn_next_, btn_open_};
    for (GtkWidget* button : transport_icon_buttons) {
        gtk_style_context_add_class(gtk_widget_get_style_context(button), "transport-button");
        gtk_style_context_add_class(gtk_widget_get_style_context(button), "transport-icon");
        gtk_box_pack_start(GTK_BOX(controls_transport), button, FALSE, FALSE, 0);
    }
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_repeat_), "transport-button");
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_repeat_), "transport-mode");
    gtk_box_pack_start(GTK_BOX(controls_transport), btn_repeat_, FALSE, FALSE, 0);

    GtkWidget* text_buttons[] = {btn_settings_, btn_eq_, btn_alsamixer_, btn_about_};
    for (GtkWidget* button : text_buttons) {
        gtk_style_context_add_class(gtk_widget_get_style_context(button), "transport-button");
        gtk_style_context_add_class(gtk_widget_get_style_context(button), "transport-button-thin");
    }
    gtk_grid_attach(GTK_GRID(controls_text), btn_settings_, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(controls_text), btn_alsamixer_, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(controls_text), btn_eq_, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(controls_text), btn_about_, 1, 1, 1, 1);
    gtk_widget_set_tooltip_text(btn_open_, "Left-click: Open files\nRight-click: Open directory");
    gtk_widget_set_tooltip_text(btn_repeat_, "Playback mode: Off\nNext: Repeat");
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

    GtkWidget* playlist_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    playlist_panel_ = playlist_panel;
    gtk_box_pack_start(GTK_BOX(content_row), playlist_panel, TRUE, TRUE, 0);

    playlist_store_ = gtk_list_store_new(COL_COUNT,
                                          G_TYPE_INT,
                                          G_TYPE_STRING,
                                          G_TYPE_STRING,
                                          G_TYPE_STRING,
                                          G_TYPE_STRING,
                                          G_TYPE_STRING,
                                          G_TYPE_STRING,
                                          G_TYPE_BOOLEAN);
    if (playlist_search_enabled_) {
        initialize_playlist_search();
        search_controller_->install_in_panel(GTK_BOX(playlist_panel));
    }

    GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    playlist_scrolled_ = scrolled;
    g_signal_connect(scrolled,
                     "size-allocate",
                     G_CALLBACK(GtkPlayerWindow::on_playlist_scrolled_size_allocate),
                     this);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(playlist_panel), scrolled, TRUE, TRUE, 0);

    GtkWidget* softvol_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(softvol_box, 58, -1);
    gtk_widget_set_vexpand(softvol_box, TRUE);
    gtk_widget_set_valign(softvol_box, GTK_ALIGN_FILL);
    gtk_box_pack_end(GTK_BOX(content_row), softvol_box, FALSE, FALSE, 0);

    soft_volume_scale_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(soft_volume_scale_, 52, -1);
    gtk_widget_set_vexpand(soft_volume_scale_, TRUE);
    gtk_widget_set_valign(soft_volume_scale_, GTK_ALIGN_FILL);
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

    if (playlist_search_enabled_ && search_controller_->filter_model() != nullptr) {
        playlist_view_ = gtk_tree_view_new_with_model(GTK_TREE_MODEL(search_controller_->filter_model()));
    } else {
        playlist_view_ = gtk_tree_view_new_with_model(GTK_TREE_MODEL(playlist_store_));
    }
    gtk_widget_set_name(playlist_view_, "playlist-view");
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(playlist_view_)), GTK_SELECTION_SINGLE);
    gtk_container_add(GTK_CONTAINER(scrolled), playlist_view_);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(playlist_view_), TRUE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(playlist_view_),
                                    playlist_search_enabled_ ? FALSE : TRUE);
    gtk_widget_add_events(playlist_view_, GDK_KEY_PRESS_MASK);
    gtk_drag_dest_set(playlist_view_,
                      GTK_DEST_DEFAULT_ALL,
                      nullptr,
                      0,
                      GDK_ACTION_COPY);
    gtk_drag_dest_add_uri_targets(playlist_view_);
    g_signal_connect(playlist_view_,
                     "drag-data-received",
                     G_CALLBACK(GtkPlayerWindow::on_playlist_drag_data_received),
                     this);

    GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "xpad", 6, "ypad", 2, nullptr);
    GtkTreeViewColumn* col_track = gtk_tree_view_column_new_with_attributes("#", renderer, "text", COL_TRACKNO, nullptr);
    gtk_tree_view_column_set_resizable(col_track, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(playlist_view_), col_track);

    renderer = gtk_cell_renderer_text_new();
    playlist_artist_renderer_ = renderer;
    g_object_set(renderer, "xpad", 6, "ypad", 2, nullptr);
    GtkTreeViewColumn* col_artist = gtk_tree_view_column_new_with_attributes("Artist", renderer, "text", COL_ARTIST, nullptr);
    playlist_artist_column_ = col_artist;
    gtk_tree_view_column_set_resizable(col_artist, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(playlist_view_), col_artist);

    renderer = gtk_cell_renderer_text_new();
    playlist_title_renderer_ = renderer;
    g_object_set(renderer, "xpad", 6, "ypad", 2, nullptr);
    GtkTreeViewColumn* col_title = gtk_tree_view_column_new_with_attributes("Title", renderer, "text", COL_TITLE, nullptr);
    playlist_title_column_ = col_title;
    gtk_tree_view_column_set_resizable(col_title, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(playlist_view_), col_title);

    renderer = gtk_cell_renderer_text_new();
    playlist_album_renderer_ = renderer;
    g_object_set(renderer, "xpad", 6, "ypad", 2, nullptr);
    GtkTreeViewColumn* col_album = gtk_tree_view_column_new_with_attributes("Album", renderer, "text", COL_ALBUM, nullptr);
    playlist_album_column_ = col_album;
    gtk_tree_view_column_set_expand(col_album, TRUE);
    gtk_tree_view_column_set_resizable(col_album, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(playlist_view_), col_album);
    playlist_expand_column_ = col_album;

    // When field limiting is active, this presentation-only spacer takes the
    // same role that Album expansion has in the normal layout: it receives any
    // spare viewport width and therefore keeps Source at the right edge. GTK
    // redistributes that space directly as real columns are resized, so no
    // geometry polling, delayed correction or manual viewport arithmetic is
    // required. The fixed width is only the minimum visual gap.
    GtkTreeViewColumn* field_width_spacer = gtk_tree_view_column_new();
    playlist_field_width_spacer_column_ = field_width_spacer;
    gtk_tree_view_column_set_sizing(field_width_spacer, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(field_width_spacer,
                                         kPlaylistFieldWidthSpacerMinPixels);
    gtk_tree_view_column_set_min_width(field_width_spacer,
                                       kPlaylistFieldWidthSpacerMinPixels);
    gtk_tree_view_column_set_expand(field_width_spacer, FALSE);
    gtk_tree_view_column_set_resizable(field_width_spacer, FALSE);
    gtk_tree_view_column_set_clickable(field_width_spacer, FALSE);
    gtk_tree_view_column_set_visible(field_width_spacer, FALSE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(playlist_view_), field_width_spacer);

    renderer = gtk_cell_renderer_text_new();
    playlist_source_renderer_ = renderer;
    g_object_set(renderer, "xpad", 6, "ypad", 2, nullptr);
    GtkTreeViewColumn* col_source = gtk_tree_view_column_new_with_attributes("Source", renderer, "text", COL_SOURCE, nullptr);
    playlist_source_column_ = col_source;
    gtk_tree_view_column_set_resizable(col_source, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(playlist_view_), col_source);

    const auto configure_sort_column = [this](GtkTreeViewColumn* column,
                                               const char* title,
                                               PlaylistSortKey key,
                                               std::size_t slot) {
        GtkWidget* label = gtk_label_new(title);
        gtk_widget_show(label);
        gtk_tree_view_column_set_widget(column, label);
        gtk_tree_view_column_set_clickable(column, TRUE);
        g_object_set_data(G_OBJECT(column),
                          "pcm-playlist-sort-key",
                          GINT_TO_POINTER(static_cast<int>(key)));
        g_signal_connect(column,
                         "clicked",
                         G_CALLBACK(GtkPlayerWindow::on_playlist_column_clicked),
                         this);
        playlist_sort_columns_[slot] = column;
        playlist_sort_header_labels_[slot] = label;
    };
    configure_sort_column(col_track, "#", PlaylistSortKey::TrackNumber, 0);
    configure_sort_column(col_artist, "Artist", PlaylistSortKey::Artist, 1);
    configure_sort_column(col_title, "Title", PlaylistSortKey::Title, 2);
    configure_sort_column(col_album, "Album", PlaylistSortKey::Album, 3);
    configure_sort_column(col_source, "Source", PlaylistSortKey::Source, 4);
    install_playlist_stream_styling(GTK_TREE_VIEW(playlist_view_),
                                             col_track,
                                             col_artist,
                                             col_title,
                                             col_album,
                                             col_source,
                                             COL_TRACKNO,
                                             COL_ARTIST,
                                             COL_TITLE,
                                             COL_ALBUM,
                                             COL_SOURCE,
                                             &current_track_index_);
    apply_playlist_field_width_limit(false);
    update_playlist_sort_headers();

    // Make child visibility and GTK theme metrics available without mapping the
    // top-level window. Derive the row step from the real renderers and the
    // GtkTreeView style properties so the startup geometry follows the active
    // font and theme without temporary model rows or a post-map resize.
    gtk_widget_show_all(outer);
    const bool startup_search_visible =
        playlist_search_enabled_ && search_controller_ != nullptr;
    if (startup_search_visible) {
        search_controller_->set_search_entry_visible(false);
    }

    const auto preferred_widget_minimum_height = [](GtkWidget* widget) {
        gint minimum_height = 0;
        gint natural_height = 0;
        gtk_widget_get_preferred_height(widget,
                                        &minimum_height,
                                        &natural_height);
        return std::max(0, static_cast<int>(minimum_height));
    };
    const auto preferred_widget_natural_height = [](GtkWidget* widget) {
        gint minimum_height = 0;
        gint natural_height = 0;
        gtk_widget_get_preferred_height(widget,
                                        &minimum_height,
                                        &natural_height);
        return std::max(
            0,
            static_cast<int>(natural_height > 0
                                 ? natural_height
                                 : minimum_height));
    };

    // Measure the header controls directly. The preferred height of an empty
    // GtkTreeView is theme-defined and may include a minimum body allocation.
    int renderer_height = 0;
    int header_height = 0;
    GList* columns = gtk_tree_view_get_columns(GTK_TREE_VIEW(playlist_view_));
    for (GList* column_node = columns;
         column_node != nullptr;
         column_node = column_node->next) {
        auto* column = GTK_TREE_VIEW_COLUMN(column_node->data);
        if (gtk_tree_view_column_get_visible(column)) {
            GtkWidget* header_control = gtk_tree_view_column_get_button(column);
            if (header_control == nullptr) {
                header_control = gtk_tree_view_column_get_widget(column);
            }
            if (header_control != nullptr) {
                header_height = std::max(
                    header_height,
                    preferred_widget_natural_height(header_control));
            }
        }

        GList* cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(column));
        for (GList* cell_node = cells;
             cell_node != nullptr;
             cell_node = cell_node->next) {
            auto* cell = GTK_CELL_RENDERER(cell_node->data);
            gboolean visible = TRUE;
            g_object_get(cell, "visible", &visible, nullptr);
            if (!visible) {
                continue;
            }

            gchar* saved_text = nullptr;
            const bool text_renderer = GTK_IS_CELL_RENDERER_TEXT(cell);
            if (text_renderer) {
                g_object_get(cell, "text", &saved_text, nullptr);
                g_object_set(cell, "text", "Ag", nullptr);
            }

            gint minimum_cell_height = 0;
            gint natural_cell_height = 0;
            gtk_cell_renderer_get_preferred_height(cell,
                                                   playlist_view_,
                                                   &minimum_cell_height,
                                                   &natural_cell_height);
            renderer_height = std::max(
                renderer_height,
                natural_cell_height > 0
                    ? natural_cell_height
                    : minimum_cell_height);

            if (text_renderer) {
                g_object_set(cell, "text", saved_text, nullptr);
                g_free(saved_text);
            }
        }
        g_list_free(cells);
    }
    g_list_free(columns);

    if (renderer_height <= 0) {
        GtkCellRenderer* fallback_renderer = gtk_cell_renderer_text_new();
        g_object_set(fallback_renderer,
                     "text", "Ag",
                     "xpad", 6,
                     "ypad", 2,
                     nullptr);
        gint fallback_minimum_height = 0;
        gint fallback_natural_height = 0;
        gtk_cell_renderer_get_preferred_height(fallback_renderer,
                                               playlist_view_,
                                               &fallback_minimum_height,
                                               &fallback_natural_height);
        g_object_unref(fallback_renderer);
        renderer_height = fallback_natural_height > 0
            ? fallback_natural_height
            : fallback_minimum_height;
    }

    gint vertical_separator = 0;
    gint expander_size = 0;
    gtk_widget_style_get(playlist_view_,
                         "vertical-separator", &vertical_separator,
                         "expander-size", &expander_size,
                         nullptr);

    const int row_height = std::max(
        1,
        std::max(renderer_height,
                 std::max(0, static_cast<int>(expander_size))) +
            std::max(0, static_cast<int>(vertical_separator)));
    const int ten_row_content_height =
        header_height + kMinPlaylistRows * row_height;

    gtk_scrolled_window_set_min_content_height(
        GTK_SCROLLED_WINDOW(scrolled),
        ten_row_content_height);
    gtk_widget_queue_resize(scrolled);

    const int measured_playlist_minimum_height =
        preferred_widget_minimum_height(scrolled);
    const int minimum_playlist_height = measured_playlist_minimum_height > 0
        ? measured_playlist_minimum_height
        : ten_row_content_height;

    const int scale_minimum_height =
        preferred_widget_minimum_height(soft_volume_scale_);
    const int softvol_box_minimum_height = std::max(
        scale_minimum_height,
        preferred_widget_minimum_height(softvol_box));
    const int softvol_chrome_height = std::max(
        0,
        softvol_box_minimum_height - scale_minimum_height);
    gtk_widget_set_size_request(
        soft_volume_scale_,
        52,
        std::max(1, minimum_playlist_height - softvol_chrome_height));

    playlist_rows_at_startup_ = std::max(
        kMinPlaylistRows,
        std::min(kMaxPlaylistRows, playlist_rows_at_startup_));

    // Size the logical window from its root content. GtkWindow requisitions
    // may include backend- or decoration-dependent additions.
    gtk_widget_queue_resize(softvol_box);
    gtk_widget_queue_resize(outer);
    const int outer_natural_height = preferred_widget_natural_height(outer);
    const int window_border_width = static_cast<int>(
        gtk_container_get_border_width(GTK_CONTAINER(window_)));
    const int ten_row_window_height = outer_natural_height > 0
        ? outer_natural_height + 2 * std::max(0, window_border_width)
        : kDefaultWindowHeight;

    if (startup_search_visible) {
        search_controller_->set_search_entry_visible(true);
    }

    const int requested_window_height_without_search =
        ten_row_window_height +
        (playlist_rows_at_startup_ - kMinPlaylistRows) * row_height;
    const int applied_window_height_without_search =
        clamp_window_height_to_workarea(window_,
            requested_window_height_without_search);

    int applied_window_height = applied_window_height_without_search;
    playlist_search_window_height_adjusted_ = false;
    playlist_search_unrealized_height_delta_ = 0;
    playlist_search_runtime_height_compensation_ = 0;
    if (startup_search_visible) {
        const int entry_height = search_controller_->search_entry_natural_height();
        const int spacing = playlist_panel_ != nullptr
                                ? gtk_box_get_spacing(GTK_BOX(playlist_panel_))
                                : 0;
        const int requested_search_delta = std::max(0, entry_height + spacing);
        applied_window_height = clamp_window_height_to_workarea(window_,
            requested_window_height_without_search + requested_search_delta);
        playlist_search_unrealized_height_delta_ = std::max(
            0,
            applied_window_height - applied_window_height_without_search);
        playlist_search_runtime_height_compensation_ =
            playlist_search_unrealized_height_delta_;
        playlist_search_window_height_adjusted_ = true;
    }

    gtk_window_set_default_size(GTK_WINDOW(window_),
                                kDefaultWindowWidth,
                                applied_window_height);

    gtk_widget_add_events(btn_open_, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(btn_open_, "button-press-event",
                     G_CALLBACK(GtkPlayerWindow::on_open_button_press), this);
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
    apply_playlist_search_handler_connections();
    g_signal_connect(playlist_view_, "key-press-event", G_CALLBACK(GtkPlayerWindow::on_playlist_view_media_key_press), this);
    gtk_widget_add_events(window_, GDK_KEY_PRESS_MASK);
    g_signal_connect(window_, "key-press-event", G_CALLBACK(GtkPlayerWindow::on_window_key_press), this);
    g_signal_connect(window_, "configure-event",
                     G_CALLBACK(GtkPlayerWindow::on_window_configure_event), this);
    g_signal_connect(window_, "delete-event", G_CALLBACK(GtkPlayerWindow::on_window_delete_event), this);
    g_signal_connect(window_, "destroy", G_CALLBACK(GtkPlayerWindow::on_window_destroy), this);
    refresh_device_list();
    refresh_display();
    update_loading_controls();
    setup_media_keys(app);
    setup_mpris();

    gtk_widget_show_all(window_);
    if (playlist_search_enabled_) {
        adjust_playlist_search_window_height(true);
    }
    schedule_last_sources_restore();
}

void GtkPlayerWindow::ensure_meter_timer_running() {
    if (ui_closing_ || meter_timer_id_ != 0 ||
        (!level_meter_enabled_ && !clip_detection_enabled_) ||
        !engine_.is_playing()) {
        return;
    }
    meter_last_update_ = std::chrono::steady_clock::now();
    meter_timer_id_ = g_timeout_add(kMeterRefreshIntervalMs,
                                    GtkPlayerWindow::on_meter_tick,
                                    this);
}

void GtkPlayerWindow::sync_meter_timer_to_settings() {
    if (level_meter_enabled_ || clip_detection_enabled_) {
        ensure_meter_timer_running();
        return;
    }

    if (meter_timer_id_ != 0) {
        g_source_remove(meter_timer_id_);
        meter_timer_id_ = 0;
    }
    meter_target_level_ = 0.0;
    meter_level_ = 0.0;
    clip_hold_until_ = std::chrono::steady_clock::time_point{};
    clip_hold_samples_ = 0;
    update_clip_indicator(false, 0);
    if (display_meter_ != nullptr) {
        gtk_widget_queue_draw(display_meter_);
    }
}

void GtkPlayerWindow::settle_meter_timer_after_stop() {
    meter_target_level_ = 0.0;
    if (meter_timer_id_ == 0) {
        meter_level_ = 0.0;
        clip_hold_until_ = std::chrono::steady_clock::time_point{};
        clip_hold_samples_ = 0;
        update_clip_indicator(false, 0);
        if (display_meter_ != nullptr) {
            gtk_widget_queue_draw(display_meter_);
        }
    }
}

void GtkPlayerWindow::pause_playback() {
    const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
    if (!transport.playing || transport.paused) {
        return;
    }
    engine_.pause();
    cancel_progress_deadline();
    refresh_display(true, true);
    notify_mpris_state_changed();
}

void GtkPlayerWindow::resume_playback() {
    const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
    if (!transport.playing || !transport.paused) {
        return;
    }
    engine_.resume();
    ensure_meter_timer_running();
    refresh_display(true, false);
    schedule_progress_deadline();
    notify_mpris_state_changed();
}

guint GtkPlayerWindow::progress_deadline_delay_ms(
    const PlaybackTransportSnapshot& transport) const {
    if (!transport.playing || transport.paused || playlist_.empty() ||
        current_track_index_ >= playlist_.size() ||
        !current_track_metadata_ready()) {
        return 0;
    }

    const PlaylistEntry& track = playlist_[current_track_index_];
    const std::uint32_t playback_rate = active_transport_sample_rate(
        true, transport.format, track);
    const std::uint64_t safe_rate = playback_rate == 0
        ? 44100ULL
        : static_cast<std::uint64_t>(playback_rate);
    const std::uint64_t track_position =
        current_track_position_from_transport(transport);
    const std::uint64_t track_length = active_track_length_samples(
        true, transport.total_samples_per_channel, track);

    // The time label changes only at whole-second boundaries. This also gives
    // us a hard upper bound of one second for the next visible presentation
    // change without polling the transport at a fixed cadence.
    std::uint64_t next_sample_delta =
        safe_rate - (track_position % safe_rate);

    // The progress bar has a fixed number of cells. Find the next exact cell
    // boundary without multiplying track_length by kProgressSegments, which
    // keeps the arithmetic portable and overflow-safe in C++17.
    if (track_length > 0 && track_position < track_length) {
        const std::uint64_t segment_count =
            static_cast<std::uint64_t>(kProgressSegments);
        const std::uint64_t quotient = track_length / segment_count;
        const std::uint64_t remainder = track_length % segment_count;
        for (std::uint64_t cell = 1; cell <= segment_count; ++cell) {
            const std::uint64_t remainder_product = remainder * cell;
            const std::uint64_t boundary =
                quotient * cell +
                (remainder_product + segment_count - 1) / segment_count;
            if (boundary > track_position) {
                next_sample_delta = std::min(
                    next_sample_delta, boundary - track_position);
                break;
            }
        }
    }

    std::uint64_t delay_ms =
        (next_sample_delta * 1000ULL + safe_rate - 1) / safe_rate;
    delay_ms = std::max<std::uint64_t>(1, delay_ms);

    if (progress_blink_enabled_ && track_length > 0 &&
        track_position < track_length) {
        const gint64 now_usec = g_get_monotonic_time();
        const gint64 phase_usec = now_usec % kProgressBlinkPeriodUsec;
        const gint64 remaining_usec = kProgressBlinkPeriodUsec - phase_usec;
        const std::uint64_t blink_delay_ms = static_cast<std::uint64_t>(
            std::max<gint64>(1, (remaining_usec + 999) / 1000));
        delay_ms = std::min(delay_ms, blink_delay_ms);
    }

    return static_cast<guint>(std::min<std::uint64_t>(
        delay_ms, static_cast<std::uint64_t>(std::numeric_limits<guint>::max())));
}

void GtkPlayerWindow::schedule_progress_deadline() {
    if (ui_closing_ || progress_deadline_id_ != 0) {
        return;
    }
    const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
    const guint delay_ms = progress_deadline_delay_ms(transport);
    if (delay_ms == 0) {
        return;
    }
    progress_deadline_id_ = g_timeout_add(
        delay_ms,
        GtkPlayerWindow::on_progress_deadline,
        this);
}

void GtkPlayerWindow::cancel_progress_deadline() {
    if (progress_deadline_id_ != 0) {
        g_source_remove(progress_deadline_id_);
        progress_deadline_id_ = 0;
    }
}

void GtkPlayerWindow::reschedule_progress_deadline() {
    cancel_progress_deadline();
    schedule_progress_deadline();
}

void GtkPlayerWindow::install_playback_event_bridge() {
    if (ui_dispatch_lifetime_ == nullptr ||
        !ui_dispatch_lifetime_->load(std::memory_order_acquire)) {
        ui_dispatch_lifetime_ = std::make_shared<std::atomic<bool>>(true);
    }
    if (playback_event_source_id_ != 0) {
        return;
    }

    const int event_fd = engine_.playback_event_fd();
    if (event_fd < 0) {
        throw std::runtime_error("Playback event mailbox is unavailable");
    }
    playback_event_source_id_ = g_unix_fd_add_full(
        G_PRIORITY_DEFAULT,
        event_fd,
        static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP),
        GtkPlayerWindow::on_playback_event_ready,
        this,
        nullptr);
    if (playback_event_source_id_ == 0) {
        throw std::runtime_error("Cannot install playback event source");
    }
}

void GtkPlayerWindow::uninstall_playback_event_bridge() {
    if (ui_dispatch_lifetime_ != nullptr) {
        ui_dispatch_lifetime_->store(false, std::memory_order_release);
    }
    if (playback_event_source_id_ != 0) {
        g_source_remove(playback_event_source_id_);
        playback_event_source_id_ = 0;
    }
}

gboolean GtkPlayerWindow::on_playback_event_ready(gint,
                                                  GIOCondition,
                                                  gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || self->ui_closing_) {
        if (self != nullptr) {
            self->playback_event_source_id_ = 0;
        }
        return G_SOURCE_REMOVE;
    }

    std::array<PlaybackEvent, 4> events{};
    const std::size_t event_count = self->engine_.drain_playback_events(events);
    for (std::size_t i = 0; i < event_count && !self->ui_closing_; ++i) {
        self->handle_playback_event(events[i]);
    }
    return G_SOURCE_CONTINUE;
}

void GtkPlayerWindow::handle_playback_event(const PlaybackEvent& event) {
    if (ui_closing_ ||
        event.transport_generation != engine_.transport_generation()) {
        return;
    }

    switch (event.kind) {
        case PlaybackEventKind::SegmentChanged: {
            const PlaybackStatusSnapshot status = engine_.snapshot();
            update_gapless_chain_track_from_status(status);
            refresh_display(status, true, true);
            reschedule_progress_deadline();
            break;
        }
        case PlaybackEventKind::ProcessingStateChanged:
            refresh_active_alsa_output_diagnostics();
            break;
        case PlaybackEventKind::Finished:
            handle_transport_finished_event();
            break;
        case PlaybackEventKind::Error:
            cancel_progress_deadline();
            settle_meter_timer_after_stop();
            refresh_display(true, true);
            notify_mpris_state_changed();
            break;
    }
}

void GtkPlayerWindow::handle_transport_finished_event() {
    if (ui_closing_ || track_switch_in_progress_ || finish_handled_) {
        return;
    }

    const PlaybackStatusSnapshot status = engine_.snapshot();
    if (!status.finished || status.playing) {
        return;
    }

    cancel_progress_deadline();

    // Preserve the old ordering: logical gapless/CUE transitions are committed
    // before Random/Repeat/Next handles the physical transport completion.
    update_gapless_chain_track_from_status(status);
    finish_handled_ = true;
    if (playlist_.empty() || current_track_index_ >= playlist_.size()) {
        return;
    }

    const std::size_t finished_index = current_track_index_;
    bool should_advance = false;
    std::size_t next_index = finished_index;
    PlaybackStartReason next_reason = PlaybackStartReason::Automatic;

    if (random_enabled_) {
        should_advance = random_next_track(&next_index, &next_reason);
    } else if (finished_index + 1 < playlist_.size()) {
        next_index = finished_index + 1;
        should_advance = true;
    } else if (repeat_enabled_) {
        next_index = 0;
        should_advance = true;
    }

    on_stream_transport_finished(finished_index, &should_advance);

    if (should_advance) {
        play_track_index(next_index, true, next_reason);
        return;
    }

    const bool stream_finished =
        finished_index < playlist_.size() && playlist_[finished_index].is_stream;
    if (!stream_finished) {
        stop_playback();
        // The natural completion belongs to the generation that just ended.
        // engine_.stop() invalidates that generation, so no queued completion can
        // be applied again. Preserve the visible selection at the finished track.
        finish_handled_ = true;
        sync_playlist_selection_after_transport_change(
            current_track_index_,
            true,
            PlaylistScrollPolicy::PreserveViewport);
        return;
    }

    finish_handled_ = true;
    ensure_stream_service_timer_running();
    refresh_display();
    sync_playlist_selection_after_transport_change(
        current_track_index_,
        true,
        PlaylistScrollPolicy::PreserveViewport);
}

void GtkPlayerWindow::close_finished_transport_for_manual_navigation() {
    const PlaybackStatusSnapshot status = engine_.snapshot();
    if (status.playing || !status.finished) {
        return;
    }

    // Commit any final logical CUE position before closing the completed
    // physical transport.  The generation invalidation below makes a queued
    // Finished event stale without starting another transport.
    update_gapless_chain_track_from_status(status);
    if (!engine_.consume_finished_transport()) {
        return;
    }

    cancel_progress_deadline();
    settle_meter_timer_after_stop();
    clear_gapless_chain();
    active_range_limited_transport_ = false;
    active_gapless_transport_kind_.clear();
    active_output_device_.clear();
    active_track_transport_states_.clear();
    finish_handled_ = true;
    refresh_active_alsa_output_diagnostics();
    if (!ui_closing_) {
        refresh_display(true, true);
        notify_mpris_state_changed();
    }
}

gboolean GtkPlayerWindow::on_meter_tick(gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr) {
        return G_SOURCE_REMOVE;
    }
    if (self->ui_closing_) {
        self->meter_timer_id_ = 0;
        return G_SOURCE_REMOVE;
    }

    const PlaybackMeterSnapshot meter = self->engine_.consume_meter_snapshot();
    const auto now = std::chrono::steady_clock::now();
    double elapsed_seconds = std::chrono::duration<double>(
        now - self->meter_last_update_).count();
    if (!(elapsed_seconds > 0.0)) {
        elapsed_seconds = static_cast<double>(kMeterRefreshIntervalMs) / 1000.0;
    }
    self->meter_last_update_ = now;

    const double previous_level = self->meter_level_;
    if (!self->level_meter_enabled_) {
        self->meter_target_level_ = 0.0;
        self->meter_level_ = 0.0;
    } else {
        if (!meter.transport_active) {
            self->meter_target_level_ = 0.0;
        } else if (meter.peak_measured) {
            self->meter_target_level_ = std::max(
                0.0,
                std::min(kMeterMaximumLevel, static_cast<double>(meter.peak_level)));
        }

        if (self->meter_target_level_ >= self->meter_level_) {
            self->meter_level_ = self->meter_target_level_;
        } else if (self->meter_level_ > 0.0) {
            const double floor_amplitude = std::pow(10.0, kMeterFloorDb / 20.0);
            const double current_db = 20.0 * std::log10(
                std::max(self->meter_level_, floor_amplitude));
            const double target_db = self->meter_target_level_ > floor_amplitude
                ? 20.0 * std::log10(self->meter_target_level_)
                : kMeterFloorDb;
            const double release_db_per_second = meter.transport_active
                ? kMeterReleaseDbPerSecond
                : kMeterInactiveReleaseDbPerSecond;
            const double released_db = current_db -
                (release_db_per_second * elapsed_seconds);
            const double next_db = std::max(target_db, released_db);
            self->meter_level_ = next_db <= kMeterFloorDb
                ? 0.0
                : std::pow(10.0, next_db / 20.0);
        }
    }

    if (self->display_meter_ != nullptr &&
        std::fabs(self->meter_level_ - previous_level) > 0.000001) {
        gtk_widget_queue_draw(self->display_meter_);
    }

    if (self->clip_detection_enabled_) {
        self->update_clip_indicator(meter.clipped_samples > 0,
                                    meter.clipped_samples);
    } else {
        self->update_clip_indicator(false, 0);
    }

    if (!meter.transport_active &&
        self->meter_level_ <= 0.000001 &&
        now >= self->clip_hold_until_) {
        self->meter_timer_id_ = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

gboolean GtkPlayerWindow::on_progress_deadline(gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr) {
        return G_SOURCE_REMOVE;
    }

    // This is a one-shot presentation deadline, not a repeating transport
    // poll. Clear the source id before refreshing so the next visible deadline
    // can be scheduled from the fresh coherent transport position.
    self->progress_deadline_id_ = 0;
    if (self->ui_closing_) {
        return G_SOURCE_REMOVE;
    }

    const PlaybackTransportSnapshot transport = self->engine_.transport_snapshot();
    if (!transport.playing || transport.paused) {
        return G_SOURCE_REMOVE;
    }

    PlaybackStatusSnapshot status;
    status.playing = transport.playing;
    status.paused = transport.paused;
    status.finished = transport.finished;
    status.format = transport.format;
    status.current_samples_per_channel = transport.current_samples_per_channel;
    status.total_samples_per_channel = transport.total_samples_per_channel;
    status.segment_position_valid = transport.segment_position_valid;
    status.segment_index = transport.segment_index;
    status.segment_samples_per_channel = transport.segment_samples_per_channel;
    status.transport_truncation_kind = transport.transport_truncation_kind;
    self->refresh_progress_display(status);
    self->schedule_progress_deadline();

    return G_SOURCE_REMOVE;
}

gboolean GtkPlayerWindow::on_window_delete_event(GtkWidget*, GdkEvent*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self != nullptr) {
        self->ui_closing_ = true;
        self->uninstall_playback_event_bridge();
        self->flush_preferences_save();
        self->stop_ui_updates();
        self->cancel_pending_seek();
    }
    return FALSE;
}

void GtkPlayerWindow::on_window_destroy(GtkWidget*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self != nullptr) {
        self->ui_closing_ = true;
        self->uninstall_playback_event_bridge();
        self->stop_ui_updates();
        self->cancel_pending_seek();
        self->window_ = nullptr;
        self->display_track_ = nullptr;
        self->display_time_ = nullptr;
        self->display_status_ = nullptr;
        self->display_source_ = nullptr;
        self->display_path_ = nullptr;
        self->display_meter_ = nullptr;
        self->badge_clip_ = nullptr;
        self->progress_bar_ = nullptr;
        self->badge_box_ = nullptr;
        self->badge_lossless_ = nullptr;
        self->badge_redbook_ = nullptr;
        self->badge_native_ = nullptr;
        self->badge_dsp_ = nullptr;
        self->badge_random_ = nullptr;
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
        if (self->search_controller_ != nullptr) {
            self->search_controller_->invalidate();
        }
        self->playlist_view_ = nullptr;
        self->playlist_artist_renderer_ = nullptr;
        self->playlist_title_renderer_ = nullptr;
        self->playlist_album_renderer_ = nullptr;
        self->playlist_source_renderer_ = nullptr;
        self->playlist_artist_column_ = nullptr;
        self->playlist_title_column_ = nullptr;
        self->playlist_album_column_ = nullptr;
        self->playlist_source_column_ = nullptr;
        if (self->playlist_store_ != nullptr) {
            g_object_unref(self->playlist_store_);
            self->playlist_store_ = nullptr;
        }
        self->playlist_panel_ = nullptr;
        self->playlist_scrolled_ = nullptr;
        self->diagnostics_active_output_value_ = nullptr;
        self->stereo_tonal_dsp_controls_.clear();
        self->applied_stereo_tonal_dsp_controls_enabled_.reset();
    }
}

void GtkPlayerWindow::stop_ui_updates() {
    cancel_playlist_vertical_position_restore();
    cancel_playlist_search_window_resize();
    cancel_progress_deadline();
    stop_stream_service_timer();
    if (meter_timer_id_ != 0) {
        g_source_remove(meter_timer_id_);
        meter_timer_id_ = 0;
    }
    if (restore_sources_idle_id_ != 0) {
        g_source_remove(restore_sources_idle_id_);
        restore_sources_idle_id_ = 0;
    }
}

void GtkPlayerWindow::cancel_pending_seek() {
    pending_seek_valid_ = false;
    if (pending_seek_source_id_ != 0) {
        g_source_remove(pending_seek_source_id_);
        pending_seek_source_id_ = 0;
    }
}

void GtkPlayerWindow::on_open_clicked(GtkButton*, gpointer user_data) {
    static_cast<GtkPlayerWindow*>(user_data)->open_file_dialog();
}

gboolean GtkPlayerWindow::on_open_button_press(GtkWidget*, GdkEventButton* event, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || event == nullptr ||
        event->type != GDK_BUTTON_PRESS || event->button != 3) {
        return FALSE;
    }

    self->open_directory_dialog();
    return TRUE;
}

void GtkPlayerWindow::on_playlist_drag_data_received(GtkWidget* widget,
                                                    GdkDragContext* context,
                                                    gint,
                                                    gint,
                                                    GtkSelectionData* selection_data,
                                                    guint,
                                                    guint time,
                                                    gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    bool opened = false;

    if (self != nullptr && !self->ui_closing_ && selection_data != nullptr) {
        gchar** uris = gtk_selection_data_get_uris(selection_data);
        if (uris != nullptr) {
            std::vector<std::string> source_paths;
            std::unordered_set<std::string> seen_paths;
            for (gchar** uri = uris; *uri != nullptr; ++uri) {
                GFile* file = g_file_new_for_uri(*uri);
                if (file == nullptr) {
                    continue;
                }
                gchar* local_path = g_file_get_path(file);
                g_object_unref(file);
                if (local_path == nullptr) {
                    Logger::instance().debug("Ignoring non-local dropped URI");
                    continue;
                }

                const std::string path(local_path);
                g_free(local_path);
                const bool directory = g_file_test(path.c_str(), G_FILE_TEST_IS_DIR);
                const bool regular_file = g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR);
                if (!directory && (!regular_file || !is_supported_media_path(path))) {
                    Logger::instance().debug("Ignoring unsupported dropped source: " + path);
                    continue;
                }
                if (seen_paths.insert(path).second) {
                    source_paths.push_back(path);
                }
            }
            g_strfreev(uris);

            if (!source_paths.empty()) {
                opened = self->open_source_paths(source_paths, true, false, true);
                if (opened) {
                    self->remember_open_directory_from_sources(source_paths);
                }
            }
        }
    }

    if (context != nullptr) {
        gtk_drag_finish(context, opened ? TRUE : FALSE, FALSE, time);
    }
    if (widget != nullptr) {
        g_signal_stop_emission_by_name(widget, "drag-data-received");
    }
}

void GtkPlayerWindow::on_play_clicked(GtkButton*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (!self->playback_available()) {
        return;
    }
    self->cancel_pending_last_active_track_restore();
    self->start_current_track(true);
}

void GtkPlayerWindow::on_pause_clicked(GtkButton*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (!self->playback_available()) {
        return;
    }
    if (self->engine_.is_paused()) {
        self->resume_playback();
    } else {
        self->pause_playback();
    }
}

void GtkPlayerWindow::on_stop_clicked(GtkButton*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (!self->playback_available() && !self->engine_.is_playing()) {
        return;
    }
    self->stop_playback();
}

void GtkPlayerWindow::on_prev_clicked(GtkButton*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (!self->playback_available()) {
        return;
    }
    self->mpris_advance_track(-1);
}

void GtkPlayerWindow::on_next_clicked(GtkButton*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (!self->playback_available()) {
        return;
    }
    self->mpris_advance_track(1);
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
    self->cycle_playback_mode();
}

void GtkPlayerWindow::cycle_playback_mode() {
    if (!repeat_enabled_ && !random_enabled_) {
        set_playback_mode(true, false);
    } else if (repeat_enabled_ && !random_enabled_) {
        set_playback_mode(false, true);
    } else if (!repeat_enabled_ && random_enabled_) {
        set_playback_mode(true, true);
    } else {
        set_playback_mode(false, false);
    }
}

void GtkPlayerWindow::set_playback_mode(bool repeat_enabled, bool random_enabled) {
    const bool random_changed = random_enabled_ != random_enabled;
    repeat_enabled_ = repeat_enabled;
    random_enabled_ = random_enabled;
    mpris_loop_status_ = repeat_enabled_ ? "Playlist" : "None";

    if (random_enabled_) {
        initialize_random_pass_if_needed();
    } else {
        clear_random_stopped_preview();
    }
    if (random_changed) {
        update_active_gapless_future_for_playback_mode();
    }

    update_playback_mode_ui();
    refresh_display();
    notify_mpris_state_changed();
}

void GtkPlayerWindow::update_playback_mode_ui() {
    if (btn_repeat_ == nullptr) {
        return;
    }

    const char* current = "Off";
    const char* next = "Repeat";
    if (repeat_enabled_ && !random_enabled_) {
        current = "Repeat";
        next = "Random";
    } else if (!repeat_enabled_ && random_enabled_) {
        current = "Random";
        next = "Random + Repeat";
    } else if (repeat_enabled_ && random_enabled_) {
        current = "Random + Repeat";
        next = "Off";
    }
    const std::string tooltip = std::string("Playback mode: ") + current + "\nNext: " + next;
    gtk_widget_set_tooltip_text(btn_repeat_, tooltip.c_str());
}

std::uint64_t GtkPlayerWindow::playlist_entry_id(std::size_t index) const {
    return index < playlist_.size() ? playlist_[index].original_order : 0;
}

std::optional<std::size_t> GtkPlayerWindow::playlist_index_for_entry_id(
    std::uint64_t entry_id) const {
    if (entry_id == 0) {
        return std::nullopt;
    }
    const auto found = playlist_index_by_entry_id_.find(entry_id);
    if (found == playlist_index_by_entry_id_.end() ||
        found->second >= playlist_.size() ||
        playlist_[found->second].original_order != entry_id) {
        return std::nullopt;
    }
    return found->second;
}

void GtkPlayerWindow::index_playlist_entry(std::size_t index) {
    if (index >= playlist_.size()) {
        return;
    }
    const PlaylistEntry& entry = playlist_[index];
    if (entry.original_order == 0 || entry.audio_file_path.empty()) {
        return;
    }
    playlist_index_by_entry_id_[entry.original_order] = index;
    metadata_entry_ids_by_path_[entry.audio_file_path].push_back(entry.original_order);
}

void GtkPlayerWindow::rebuild_playlist_entry_indexes() {
    playlist_index_by_entry_id_.clear();
    metadata_entry_ids_by_path_.clear();
    playlist_index_by_entry_id_.reserve(playlist_.size());
    metadata_entry_ids_by_path_.reserve(playlist_.size());
    for (std::size_t index = 0; index < playlist_.size(); ++index) {
        index_playlist_entry(index);
    }
}

void GtkPlayerWindow::clear_playlist_entry_indexes() {
    playlist_index_by_entry_id_.clear();
    metadata_entry_ids_by_path_.clear();
}

void GtkPlayerWindow::remove_random_remaining_entry(std::uint64_t entry_id) {
    random_remaining_entry_ids_.erase(
        std::remove(random_remaining_entry_ids_.begin(),
                    random_remaining_entry_ids_.end(),
                    entry_id),
        random_remaining_entry_ids_.end());
}

void GtkPlayerWindow::trim_playback_history() {
    if (playback_history_entry_ids_.size() <= kMaxRandomHistoryEntries) {
        return;
    }

    const std::size_t remove_count =
        playback_history_entry_ids_.size() - kMaxRandomHistoryEntries;
    playback_history_entry_ids_.erase(
        playback_history_entry_ids_.begin(),
        playback_history_entry_ids_.begin() +
            static_cast<std::ptrdiff_t>(remove_count));
}

void GtkPlayerWindow::trim_random_history() {
    if (random_history_entry_ids_.size() <= kMaxRandomHistoryEntries) {
        return;
    }

    const std::size_t remove_count =
        random_history_entry_ids_.size() - kMaxRandomHistoryEntries;
    random_history_entry_ids_.erase(
        random_history_entry_ids_.begin(),
        random_history_entry_ids_.begin() +
            static_cast<std::ptrdiff_t>(remove_count));

    if (random_history_position_ == std::numeric_limits<std::size_t>::max()) {
        return;
    }
    if (random_history_position_ >= remove_count) {
        random_history_position_ -= remove_count;
    } else {
        random_history_position_ = 0;
    }
}

void GtkPlayerWindow::reset_random_transport_state(bool clear_played_history) {
    random_pass_initialized_ = false;
    random_visited_entry_ids_.clear();
    random_remaining_entry_ids_.clear();
    random_history_entry_ids_.clear();
    random_history_position_ = std::numeric_limits<std::size_t>::max();
    clear_random_stopped_preview();
    if (clear_played_history) {
        played_entry_ids_.clear();
        playback_history_entry_ids_.clear();
    }
}

void GtkPlayerWindow::synchronize_random_remaining_with_playlist() {
    std::unordered_set<std::uint64_t> valid_ids;
    valid_ids.reserve(playlist_.size());
    for (const PlaylistEntry& entry : playlist_) {
        if (entry.original_order != 0) {
            valid_ids.insert(entry.original_order);
        }
    }

    if (random_stopped_preview_entry_id_.has_value() &&
        valid_ids.find(*random_stopped_preview_entry_id_) == valid_ids.end()) {
        clear_random_stopped_preview();
    }

    const auto prune_vector = [&valid_ids](std::vector<std::uint64_t>* values) {
        values->erase(std::remove_if(values->begin(), values->end(),
                                    [&valid_ids](std::uint64_t id) {
                                        return valid_ids.find(id) == valid_ids.end();
                                    }),
                      values->end());
    };
    const auto prune_random_history = [this, &valid_ids]() {
        if (random_history_entry_ids_.empty()) {
            random_history_position_ = std::numeric_limits<std::size_t>::max();
            return;
        }

        const bool position_valid =
            random_history_position_ < random_history_entry_ids_.size();
        const std::size_t old_position = position_valid
            ? random_history_position_
            : random_history_entry_ids_.size() - 1;
        std::size_t removed_before_position = 0;
        bool positioned_entry_removed = false;

        std::vector<std::uint64_t> retained;
        retained.reserve(random_history_entry_ids_.size());
        for (std::size_t index = 0; index < random_history_entry_ids_.size(); ++index) {
            const std::uint64_t id = random_history_entry_ids_[index];
            if (valid_ids.find(id) != valid_ids.end()) {
                retained.push_back(id);
                continue;
            }
            if (index < old_position) {
                ++removed_before_position;
            } else if (index == old_position) {
                positioned_entry_removed = true;
            }
        }

        random_history_entry_ids_.swap(retained);
        if (random_history_entry_ids_.empty()) {
            random_history_position_ = std::numeric_limits<std::size_t>::max();
            return;
        }

        const std::size_t mapped_position = old_position - removed_before_position;
        if (positioned_entry_removed) {
            random_history_position_ =
                std::min(mapped_position, random_history_entry_ids_.size() - 1);
        } else {
            random_history_position_ = mapped_position;
        }
    };
    const auto prune_set = [&valid_ids](std::unordered_set<std::uint64_t>* values) {
        for (auto it = values->begin(); it != values->end();) {
            if (valid_ids.find(*it) == valid_ids.end()) {
                it = values->erase(it);
            } else {
                ++it;
            }
        }
    };

    prune_set(&played_entry_ids_);
    prune_vector(&playback_history_entry_ids_);
    prune_set(&random_visited_entry_ids_);
    prune_random_history();
    prune_vector(&random_remaining_entry_ids_);

    if (!random_pass_initialized_) {
        return;
    }

    random_remaining_entry_ids_.erase(
        std::remove_if(random_remaining_entry_ids_.begin(),
                       random_remaining_entry_ids_.end(),
                       [this](std::uint64_t id) {
                           return random_visited_entry_ids_.find(id) !=
                                  random_visited_entry_ids_.end();
                       }),
        random_remaining_entry_ids_.end());

    std::unordered_set<std::uint64_t> queued(random_remaining_entry_ids_.begin(),
                                             random_remaining_entry_ids_.end());
    std::vector<std::uint64_t> added;
    for (const PlaylistEntry& entry : playlist_) {
        const std::uint64_t id = entry.original_order;
        if (id != 0 && random_visited_entry_ids_.find(id) == random_visited_entry_ids_.end() &&
            queued.insert(id).second) {
            added.push_back(id);
        }
    }
    std::shuffle(added.begin(), added.end(), random_generator_);
    random_remaining_entry_ids_.insert(random_remaining_entry_ids_.end(),
                                       added.begin(),
                                       added.end());

    if (random_history_entry_ids_.empty()) {
        random_history_position_ = std::numeric_limits<std::size_t>::max();
    } else if (random_history_position_ >= random_history_entry_ids_.size()) {
        random_history_position_ = random_history_entry_ids_.size() - 1;
    }
}

void GtkPlayerWindow::initialize_random_pass_if_needed() {
    if (random_pass_initialized_) {
        synchronize_random_remaining_with_playlist();
        return;
    }

    random_visited_entry_ids_ = played_entry_ids_;
    random_history_entry_ids_ = playback_history_entry_ids_;
    random_history_position_ = random_history_entry_ids_.empty()
        ? std::numeric_limits<std::size_t>::max()
        : random_history_entry_ids_.size() - 1;
    trim_random_history();
    random_remaining_entry_ids_.clear();
    for (const PlaylistEntry& entry : playlist_) {
        if (entry.original_order != 0 &&
            random_visited_entry_ids_.find(entry.original_order) ==
                random_visited_entry_ids_.end()) {
            random_remaining_entry_ids_.push_back(entry.original_order);
        }
    }
    std::shuffle(random_remaining_entry_ids_.begin(),
                 random_remaining_entry_ids_.end(),
                 random_generator_);
    random_pass_initialized_ = true;
}

void GtkPlayerWindow::begin_new_random_pass(std::uint64_t avoid_first_entry_id) {
    random_pass_initialized_ = true;
    random_visited_entry_ids_.clear();
    played_entry_ids_.clear();
    random_remaining_entry_ids_.clear();
    for (const PlaylistEntry& entry : playlist_) {
        if (entry.original_order != 0) {
            random_remaining_entry_ids_.push_back(entry.original_order);
        }
    }
    std::shuffle(random_remaining_entry_ids_.begin(),
                 random_remaining_entry_ids_.end(),
                 random_generator_);
    if (random_remaining_entry_ids_.size() > 1 &&
        random_remaining_entry_ids_.front() == avoid_first_entry_id) {
        const auto replacement = std::find_if(
            random_remaining_entry_ids_.begin() + 1,
            random_remaining_entry_ids_.end(),
            [avoid_first_entry_id](std::uint64_t id) {
                return id != avoid_first_entry_id;
            });
        if (replacement != random_remaining_entry_ids_.end()) {
            std::iter_swap(random_remaining_entry_ids_.begin(), replacement);
        }
    }
    random_history_position_ = random_history_entry_ids_.empty()
        ? std::numeric_limits<std::size_t>::max()
        : random_history_entry_ids_.size() - 1;
}

void GtkPlayerWindow::record_track_started(std::size_t index,
                                           PlaybackStartReason reason) {
    remember_last_active_track(index);
    const std::uint64_t id = playlist_entry_id(index);
    if (id == 0) {
        return;
    }

    played_entry_ids_.insert(id);
    if (reason != PlaybackStartReason::HistoryNavigation &&
        reason != PlaybackStartReason::PreserveHistory) {
        playback_history_entry_ids_.push_back(id);
        trim_playback_history();
    }

    if (!random_pass_initialized_) {
        return;
    }

    random_visited_entry_ids_.insert(id);
    remove_random_remaining_entry(id);

    if (reason == PlaybackStartReason::HistoryNavigation ||
        reason == PlaybackStartReason::PreserveHistory ||
        reason == PlaybackStartReason::StoppedRandomPreview) {
        return;
    }

    if (random_history_position_ != std::numeric_limits<std::size_t>::max() &&
        random_history_position_ + 1 < random_history_entry_ids_.size()) {
        random_history_entry_ids_.erase(
            random_history_entry_ids_.begin() +
                static_cast<std::ptrdiff_t>(random_history_position_ + 1),
            random_history_entry_ids_.end());
    }
    random_history_entry_ids_.push_back(id);
    random_history_position_ = random_history_entry_ids_.size() - 1;
    trim_random_history();
}

void GtkPlayerWindow::record_random_chain_transition(std::size_t index) {
    remember_last_active_track(index);
    const std::uint64_t id = playlist_entry_id(index);
    if (id == 0) {
        return;
    }

    played_entry_ids_.insert(id);
    playback_history_entry_ids_.push_back(id);
    trim_playback_history();
    initialize_random_pass_if_needed();
    random_visited_entry_ids_.insert(id);
    remove_random_remaining_entry(id);

    if (random_history_position_ != std::numeric_limits<std::size_t>::max() &&
        random_history_position_ + 1 < random_history_entry_ids_.size() &&
        random_history_entry_ids_[random_history_position_ + 1] == id) {
        ++random_history_position_;
        return;
    }

    if (random_history_position_ != std::numeric_limits<std::size_t>::max() &&
        random_history_position_ + 1 < random_history_entry_ids_.size()) {
        random_history_entry_ids_.erase(
            random_history_entry_ids_.begin() +
                static_cast<std::ptrdiff_t>(random_history_position_ + 1),
            random_history_entry_ids_.end());
    }
    random_history_entry_ids_.push_back(id);
    random_history_position_ = random_history_entry_ids_.size() - 1;
    trim_random_history();
}

void GtkPlayerWindow::anchor_random_stopped_navigation() {
    if (playlist_.empty() || current_track_index_ >= playlist_.size()) {
        return;
    }
    initialize_random_pass_if_needed();
    const std::uint64_t id = playlist_entry_id(current_track_index_);
    if (id == 0) {
        return;
    }

    random_visited_entry_ids_.insert(id);
    remove_random_remaining_entry(id);
    if (random_history_position_ != std::numeric_limits<std::size_t>::max() &&
        random_history_position_ < random_history_entry_ids_.size() &&
        random_history_entry_ids_[random_history_position_] == id) {
        return;
    }
    if (random_history_position_ != std::numeric_limits<std::size_t>::max() &&
        random_history_position_ + 1 < random_history_entry_ids_.size()) {
        random_history_entry_ids_.erase(
            random_history_entry_ids_.begin() +
                static_cast<std::ptrdiff_t>(random_history_position_ + 1),
            random_history_entry_ids_.end());
    }
    random_history_entry_ids_.push_back(id);
    random_history_position_ = random_history_entry_ids_.size() - 1;
    trim_random_history();
}

void GtkPlayerWindow::record_random_stopped_selection(
    std::size_t index,
    PlaybackStartReason reason) {
    const std::uint64_t id = playlist_entry_id(index);
    if (id == 0) {
        clear_random_stopped_preview();
        return;
    }
    random_stopped_preview_entry_id_ = id;
    random_visited_entry_ids_.insert(id);
    remove_random_remaining_entry(id);
    if (reason == PlaybackStartReason::HistoryNavigation) {
        return;
    }

    if (random_history_position_ != std::numeric_limits<std::size_t>::max() &&
        random_history_position_ + 1 < random_history_entry_ids_.size()) {
        random_history_entry_ids_.erase(
            random_history_entry_ids_.begin() +
                static_cast<std::ptrdiff_t>(random_history_position_ + 1),
            random_history_entry_ids_.end());
    }
    random_history_entry_ids_.push_back(id);
    random_history_position_ = random_history_entry_ids_.size() - 1;
    trim_random_history();
}

void GtkPlayerWindow::clear_random_stopped_preview() {
    random_stopped_preview_entry_id_.reset();
}

GtkPlayerWindow::RandomNavigationAvailability
GtkPlayerWindow::random_navigation_availability(
    bool anchor_to_stopped_selection) const {
    RandomNavigationAvailability availability;
    if (playlist_.empty()) {
        return availability;
    }

    const std::vector<std::uint64_t>& history = random_pass_initialized_
        ? random_history_entry_ids_
        : playback_history_entry_ids_;
    const std::size_t history_position = random_pass_initialized_
        ? random_history_position_
        : (history.empty()
               ? std::numeric_limits<std::size_t>::max()
               : history.size() - 1);
    const auto history_contains_valid_entry =
        [this, &history](std::size_t begin, std::size_t end) {
            const std::size_t bounded_end = std::min(end, history.size());
            for (std::size_t position = std::min(begin, bounded_end);
                 position < bounded_end;
                 ++position) {
                if (playlist_index_for_entry_id(history[position]).has_value()) {
                    return true;
                }
            }
            return false;
        };
    const auto has_remaining_candidate = [this](std::uint64_t excluded_id) {
        if (random_pass_initialized_) {
            return std::any_of(
                random_remaining_entry_ids_.begin(),
                random_remaining_entry_ids_.end(),
                [this, excluded_id](std::uint64_t id) {
                    return id != excluded_id &&
                           playlist_index_for_entry_id(id).has_value();
                });
        }
        return std::any_of(
            playlist_.begin(),
            playlist_.end(),
            [this, excluded_id](const PlaylistEntry& entry) {
                return entry.original_order != 0 &&
                       entry.original_order != excluded_id &&
                       played_entry_ids_.find(entry.original_order) ==
                           played_entry_ids_.end();
            });
    };

    if (!anchor_to_stopped_selection) {
        if (history_position != std::numeric_limits<std::size_t>::max()) {
            availability.can_go_next = history_contains_valid_entry(
                history_position + 1,
                history.size());
            availability.can_go_previous = history_contains_valid_entry(
                0,
                history_position);
        }
        if (!availability.can_go_next) {
            availability.can_go_next = has_remaining_candidate(0);
        }
    } else {
        const std::uint64_t selected_id =
            playlist_entry_id(mpris_playlist_index(false));
        const bool already_anchored =
            selected_id != 0 &&
            history_position != std::numeric_limits<std::size_t>::max() &&
            history_position < history.size() &&
            history[history_position] == selected_id;
        if (already_anchored) {
            availability.can_go_next = history_contains_valid_entry(
                history_position + 1,
                history.size());
            availability.can_go_previous = history_contains_valid_entry(
                0,
                history_position);
        } else if (selected_id != 0) {
            const std::size_t retained_history_end =
                history_position != std::numeric_limits<std::size_t>::max() &&
                        history_position < history.size()
                    ? history_position + 1
                    : history.size();
            availability.can_go_previous = history_contains_valid_entry(
                0,
                retained_history_end);
        }
        if (!availability.can_go_next) {
            availability.can_go_next = has_remaining_candidate(selected_id);
        }
    }

    if (!availability.can_go_next && repeat_enabled_) {
        availability.can_go_next = true;
    }
    return availability;
}

bool GtkPlayerWindow::random_next_track(std::size_t* index,
                                        PlaybackStartReason* reason) {
    if (index == nullptr || reason == nullptr || playlist_.empty()) {
        return false;
    }
    initialize_random_pass_if_needed();

    while (random_history_position_ != std::numeric_limits<std::size_t>::max() &&
           random_history_position_ + 1 < random_history_entry_ids_.size()) {
        ++random_history_position_;
        const std::optional<std::size_t> found =
            playlist_index_for_entry_id(random_history_entry_ids_[random_history_position_]);
        if (found.has_value()) {
            *index = *found;
            *reason = PlaybackStartReason::HistoryNavigation;
            return true;
        }
    }

    while (true) {
        while (!random_remaining_entry_ids_.empty()) {
            const std::optional<std::size_t> found =
                playlist_index_for_entry_id(random_remaining_entry_ids_.front());
            if (found.has_value()) {
                *index = *found;
                *reason = PlaybackStartReason::Automatic;
                return true;
            }
            random_remaining_entry_ids_.erase(random_remaining_entry_ids_.begin());
        }

        if (!repeat_enabled_) {
            return false;
        }
        begin_new_random_pass(playlist_entry_id(current_track_index_));
        if (random_remaining_entry_ids_.empty()) {
            return false;
        }
    }
}

bool GtkPlayerWindow::random_previous_track(std::size_t* index) {
    if (index == nullptr || playlist_.empty()) {
        return false;
    }
    initialize_random_pass_if_needed();
    while (random_history_position_ != std::numeric_limits<std::size_t>::max() &&
           random_history_position_ > 0) {
        --random_history_position_;
        const std::optional<std::size_t> found =
            playlist_index_for_entry_id(random_history_entry_ids_[random_history_position_]);
        if (found.has_value()) {
            *index = *found;
            return true;
        }
    }
    return false;
}

std::vector<std::uint64_t> GtkPlayerWindow::random_future_entry_ids() const {
    std::vector<std::uint64_t> future;
    if (!random_pass_initialized_) {
        return future;
    }
    if (random_history_position_ != std::numeric_limits<std::size_t>::max() &&
        random_history_position_ + 1 < random_history_entry_ids_.size()) {
        future.insert(future.end(),
                      random_history_entry_ids_.begin() +
                          static_cast<std::ptrdiff_t>(random_history_position_ + 1),
                      random_history_entry_ids_.end());
    }
    future.insert(future.end(),
                  random_remaining_entry_ids_.begin(),
                  random_remaining_entry_ids_.end());
    return future;
}

std::vector<std::size_t> GtkPlayerWindow::random_gapless_chain_indices(
    std::size_t index,
    PlaybackStartReason reason) const {
    std::vector<std::size_t> indices;
    if (index >= playlist_.size()) {
        return indices;
    }
    indices.push_back(index);
    if (!entry_supports_separate_gapless(playlist_[index])) {
        return indices;
    }

    std::vector<std::uint64_t> future_ids;
    if (reason == PlaybackStartReason::Manual ||
        reason == PlaybackStartReason::Automatic) {
        future_ids = random_remaining_entry_ids_;
    } else {
        future_ids = random_future_entry_ids();
    }

    const std::uint64_t current_id = playlist_entry_id(index);
    bool skipped_current = false;
    std::size_t previous_index = index;
    for (const std::uint64_t id : future_ids) {
        if (!skipped_current && id == current_id) {
            skipped_current = true;
            continue;
        }
        const std::optional<std::size_t> next_index = playlist_index_for_entry_id(id);
        if (!next_index.has_value()) {
            continue;
        }
        if (!entry_supports_separate_gapless(playlist_[*next_index]) ||
            !entries_share_gapless_transport(playlist_[previous_index],
                                             playlist_[*next_index],
                                             true)) {
            break;
        }
        indices.push_back(*next_index);
        previous_index = *next_index;
    }
    return indices;
}

void GtkPlayerWindow::update_active_gapless_future_for_playback_mode() {
    const PlaybackStatusSnapshot status = engine_.snapshot();
    update_gapless_chain_track_from_status(status);
    if (!status.playing || !gapless_chain_active_ ||
        gapless_chain_active_segment_ >= gapless_chain_offsets_.size() ||
        gapless_chain_active_segment_ >= gapless_chain_playlist_indices_.size()) {
        return;
    }

    const bool can_stop_after_decoder_segment =
        status.transport_truncation_kind ==
            TransportTruncationKind::DecoderSegmentBoundary &&
        status.segment_position_valid;
    const bool can_stop_at_exact_sample =
        status.transport_truncation_kind ==
            TransportTruncationKind::ExactSampleBoundary;
    if (!can_stop_after_decoder_segment && !can_stop_at_exact_sample) {
        Logger::instance().debug(
            "Playback mode change will take effect after the active continuous "
            "single-source chain; the decoder has no enforceable segment boundary");
        return;
    }

    const std::size_t current_segment = gapless_chain_active_segment_;
    const std::uint64_t current_begin = gapless_chain_offsets_[current_segment];
    const std::uint64_t current_end =
        current_segment + 1 < gapless_chain_offsets_.size()
            ? gapless_chain_offsets_[current_segment + 1]
            : gapless_chain_total_samples_;
    if (current_end <= current_begin) {
        return;
    }

    const std::size_t current_index =
        gapless_chain_playlist_indices_[current_segment];
    if (current_index >= playlist_.size()) {
        return;
    }

    std::vector<std::size_t> retained_indices;
    std::vector<std::uint64_t> retained_offsets;
    std::vector<ActiveTrackTransportState> retained_states;
    retained_indices.push_back(current_index);
    retained_offsets.push_back(current_begin);
    if (current_segment >= active_track_transport_states_.size()) {
        return;
    }
    retained_states.push_back(active_track_transport_states_[current_segment]);

    std::uint64_t stop_at = current_end;
    std::size_t decoder_stop_segment = status.segment_index;
    const std::size_t next_segment = current_segment + 1;
    if (next_segment < gapless_chain_playlist_indices_.size() &&
        next_segment < gapless_chain_offsets_.size()) {
        const std::size_t next_index =
            gapless_chain_playlist_indices_[next_segment];
        const std::uint64_t next_begin =
            gapless_chain_offsets_[next_segment];
        const std::uint64_t next_end =
            next_segment + 1 < gapless_chain_offsets_.size()
                ? gapless_chain_offsets_[next_segment + 1]
                : gapless_chain_total_samples_;
        if (next_index < playlist_.size() && next_end > next_begin) {
            retained_indices.push_back(next_index);
            retained_offsets.push_back(next_begin);
            if (next_segment >= active_track_transport_states_.size()) {
                return;
            }
            retained_states.push_back(
                active_track_transport_states_[next_segment]);
            stop_at = next_end;
            if (can_stop_after_decoder_segment &&
                decoder_stop_segment < std::numeric_limits<std::size_t>::max()) {
                ++decoder_stop_segment;
            }
        }
    }

    if (can_stop_after_decoder_segment) {
        engine_.request_stop_after_segment(decoder_stop_segment);
    } else {
        engine_.request_stop_after_current_segment(stop_at);
    }
    set_gapless_chain_mapping(retained_indices,
                              retained_offsets,
                              stop_at,
                              0,
                              can_stop_after_decoder_segment
                                  ? status.segment_index
                                  : 0);
    active_track_transport_states_ = std::move(retained_states);
    refresh_active_alsa_output_diagnostics();

    if (retained_indices.size() > 1) {
        Logger::instance().debug(
            "Playback mode change will take effect after the committed next "
            "gapless track");
    } else {
        active_gapless_transport_kind_.clear();
        Logger::instance().debug(
            "Playback mode change will take effect after the current "
            "gapless track");
    }
}

bool GtkPlayerWindow::playlist_row_visible(std::size_t index) const {
    if (playlist_view_ == nullptr || index >= playlist_.size()) {
        return false;
    }
    GtkTreePath* target = nullptr;
    if (!patches::find_playlist_view_path_for_index(GTK_TREE_VIEW(playlist_view_),
                                                    index,
                                                    COL_INDEX,
                                                    &target) ||
        target == nullptr) {
        return false;
    }

    GtkTreePath* first = nullptr;
    GtkTreePath* last = nullptr;
    const gboolean has_range = gtk_tree_view_get_visible_range(
        GTK_TREE_VIEW(playlist_view_), &first, &last);
    const bool visible = has_range && first != nullptr && last != nullptr &&
                         gtk_tree_path_compare(target, first) >= 0 &&
                         gtk_tree_path_compare(target, last) <= 0;
    if (first != nullptr) gtk_tree_path_free(first);
    if (last != nullptr) gtk_tree_path_free(last);
    gtk_tree_path_free(target);
    return visible;
}

GtkPlayerWindow::PlaylistScrollPolicy
GtkPlayerWindow::automatic_transport_scroll_policy(std::size_t index) const {
    return playlist_row_visible(index)
        ? PlaylistScrollPolicy::PreserveViewport
        : PlaylistScrollPolicy::Center;
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
    self->begin_continuous_preferences_interaction();
    GtkAllocation alloc{};
    gtk_widget_get_allocation(widget, &alloc);
    const double track_y = 10.0;
    const double track_h = alloc.height - 20.0;
    const double knob_h = 30.0;
    double y = std::max(track_y, std::min(track_y + track_h - knob_h, static_cast<double>(event->y) - knob_h * 0.5));
    const double ratio = 1.0 - ((y - track_y) / std::max(1.0, track_h - knob_h));
    self->soft_volume_percent_ = static_cast<int>(std::round(std::max(0.0, std::min(1.0, ratio)) * 100.0));
    self->engine_.set_soft_volume_percent(self->soft_volume_percent_);
    self->mark_continuous_preferences_dirty();
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
    self->mark_continuous_preferences_dirty();
    self->refresh_display();
    gtk_widget_queue_draw(widget);
    return TRUE;
}

gboolean GtkPlayerWindow::on_softvol_button_release(GtkWidget*, GdkEventButton* event, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (event != nullptr && event->button == 1) {
        self->softvol_dragging_ = false;
        self->commit_continuous_preferences();
        self->notify_mpris_state_changed();
        return TRUE;
    }
    return FALSE;
}

gboolean GtkPlayerWindow::on_meter_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    const double value = std::max(0.0, std::min(kMeterMaximumLevel, self->meter_level_));
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
    const double fill_ratio = value / kMeterMaximumLevel;
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

    const int segments = kProgressSegments;
    const double gap = 1.0;
    const double inner_x = 2.0;
    const double inner_y = 2.0;
    const double inner_w = width - 4.0;
    const double inner_h = height - 4.0;
    const double seg_w = (inner_w - gap * (segments - 1)) / segments;
    const int filled = static_cast<int>(ratio * segments);
    const bool blink_on = self->progress_blink_enabled_ &&
        ((g_get_monotonic_time() / kProgressBlinkPeriodUsec) % 2 == 0);
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
    if (event == nullptr || event->button != 1 || !self->current_track_metadata_ready() ||
        self->playlist_.empty() || self->current_track_index_ >= self->playlist_.size()) {
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
        if (self->pending_seek_source_id_ == 0) {
            self->pending_seek_source_id_ = g_idle_add(GtkPlayerWindow::on_pending_seek_idle, self);
        }
    } else {
        self->play_track_index_at_offset(self->current_track_index_,
                                         target,
                                         true,
                                         false,
                                         true,
                                         true,
                                         PlaybackStartReason::PreserveHistory);
    }
    return TRUE;
}

gboolean GtkPlayerWindow::on_pending_seek_idle(gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || self->ui_closing_) {
        return G_SOURCE_REMOVE;
    }
    self->pending_seek_source_id_ = 0;
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
        self->play_track_index_at_offset(index,
                                         offset,
                                         true,
                                         false,
                                         true,
                                         true,
                                         PlaybackStartReason::PreserveHistory);
    }
    return G_SOURCE_REMOVE;
}

void GtkPlayerWindow::on_playlist_row_activated(GtkTreeView* view, GtkTreePath* path, GtkTreeViewColumn*, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || view == nullptr || path == nullptr) {
        return;
    }
    std::size_t index = 0;
    if (!patches::playlist_index_from_view_path(view, path, COL_INDEX, &index) ||
        index >= self->playlist_.size()) {
        return;
    }
    self->play_filtered_track_index(index);
}

void GtkPlayerWindow::on_playlist_column_clicked(GtkTreeViewColumn* column, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || column == nullptr || self->ui_closing_) {
        return;
    }
    const int raw_key = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(column), "pcm-playlist-sort-key"));
    const PlaylistSortKey key = static_cast<PlaylistSortKey>(raw_key);
    if (key == PlaylistSortKey::None) {
        return;
    }
    self->cancel_pending_last_active_track_restore();
    self->cycle_playlist_sort(key);
}

std::uint32_t GtkPlayerWindow::target_sample_rate_for(std::uint32_t source_rate) const {
    for (const ResampleRule& rule : resample_rules_) {
        if (rule.from_rate == source_rate) {
            return rule.to_rate;
        }
    }
    return 0;
}

std::uint16_t GtkPlayerWindow::target_bits_for(std::uint16_t source_bits) const {
    for (const BitDepthRule& rule : bitdepth_rules_) {
        if (rule.from_bits == source_bits) {
            return rule.to_bits;
        }
    }
    return 0;
}

std::uint32_t GtkPlayerWindow::dsd_target_sample_rate_for(std::uint32_t dsd_sample_rate,
                                                          std::uint32_t ffmpeg_pcm_rate) const {
    for (const DsdPcmRule& rule : dsd_pcm_rules_) {
        if (rule.dsd_sample_rate == dsd_sample_rate) {
            return rule.pcm_sample_rate;
        }
    }

    const bool family_441 = ffmpeg_pcm_rate > 0 && (ffmpeg_pcm_rate % 44100U) == 0U;
    const bool family_48 = ffmpeg_pcm_rate > 0 && (ffmpeg_pcm_rate % 48000U) == 0U;
    if (family_441 && !family_48) {
        return 176400U;
    }
    if (family_48 && !family_441) {
        return 192000U;
    }
    return 0;
}

std::uint32_t GtkPlayerWindow::output_sample_rate_for_entry(const PlaylistEntry& entry) const {
    if (entry.dsd_source) {
        return dsd_target_sample_rate_for(entry.dsd_sample_rate, entry.source_sample_rate);
    }
    return target_sample_rate_for(entry.source_sample_rate);
}

std::uint32_t GtkPlayerWindow::playback_sample_rate_for_entry(
    const PlaylistEntry& entry) const {
    if (entry.resampled) {
        const std::uint32_t target_rate = output_sample_rate_for_entry(entry);
        if (target_rate > 0) {
            return target_rate;
        }
    }
    if (entry.decoded_format.sample_rate > 0) {
        return entry.decoded_format.sample_rate;
    }
    return entry.source_sample_rate;
}

std::uint32_t GtkPlayerWindow::active_transport_sample_rate(
    bool transport_active,
    const AudioFormat& transport_format,
    const PlaylistEntry& entry) const {
    if (transport_active && transport_format.sample_rate > 0) {
        return transport_format.sample_rate;
    }
    return playback_sample_rate_for_entry(entry);
}

const GtkPlayerWindow::ActiveTrackTransportState*
GtkPlayerWindow::active_track_transport_state() const {
    if (active_track_transport_states_.empty()) {
        return nullptr;
    }
    const std::size_t state_index = gapless_chain_active_
        ? gapless_chain_active_segment_
        : 0;
    if (state_index >= active_track_transport_states_.size()) {
        return nullptr;
    }
    return &active_track_transport_states_[state_index];
}

std::uint64_t GtkPlayerWindow::active_track_length_samples(
    bool transport_active,
    std::uint64_t decoder_total_samples,
    const PlaylistEntry& entry) const {
    if (transport_active) {
        const ActiveTrackTransportState* state = active_track_transport_state();
        if (state != nullptr && state->planned_length_samples > 0) {
            return state->planned_length_samples;
        }
        if (!gapless_chain_active_ && decoder_total_samples > 0) {
            return decoder_total_samples;
        }
    }
    return track_length_samples(entry);
}

std::uint16_t GtkPlayerWindow::output_bits_for_entry(const PlaylistEntry& entry) const {
    if (entry.dsd_source) {
        return dsd_pcm_output_bits_;
    }
    return target_bits_for(entry.source_bits_per_sample);
}

void GtkPlayerWindow::reset_dsd_pcm_defaults() {
    dsd_pcm_rules_ = default_dsd_pcm_rules();
    dsd_pcm_output_bits_ = 24;
}

void GtkPlayerWindow::refresh_entry_processing_metadata(PlaylistEntry& entry) {
    entry.decoded_format.sample_rate = entry.source_sample_rate;
    entry.decoded_format.bits_per_sample = entry.source_bits_per_sample;

    const std::uint32_t target_rate = output_sample_rate_for_entry(entry);
    const std::uint16_t target_bits = output_bits_for_entry(entry);
    entry.resampled = target_rate > 0 && target_rate != entry.source_sample_rate;
    entry.resampled_from_rate = entry.resampled ? entry.source_sample_rate : 0;
    entry.bitdepth_converted = entry.dsd_source ||
                               (target_bits > 0 && target_bits != entry.source_bits_per_sample);
    entry.native_decode = entry.native_source_available &&
                          !entry.dsd_source && !entry.resampled && !entry.bitdepth_converted;
    entry.processed_by_ffmpeg = !entry.native_decode;

    SampleExtent source_extent;
    source_extent.samples = entry.cue_track
        ? entry.source_cue_album_end_sample
        : entry.source_end_sample;
    source_extent.kind = entry.source_sample_extent_kind;
    source_extent.source = entry.source_sample_extent_source;
    source_extent.exact_presentation_drain_policy =
        entry.source_exact_presentation_drain_policy;

    const std::uint32_t output_rate = entry.resampled
        ? target_rate
        : entry.source_sample_rate;
    if (entry.resampled) {
        // decoded_format is the final PCM domain consumed by PlaybackEngine.
        // Keep source properties exclusively in source_* fields.
        entry.decoded_format.sample_rate = output_rate;
    }
    const SampleExtent output_extent = transform_sample_extent_for_output(
        source_extent, entry.source_sample_rate, output_rate);
    entry.sample_extent_kind = output_extent.kind;
    entry.sample_extent_source = output_extent.source;
    entry.sample_extent_drain_policy = output_extent.exact_presentation_drain_policy;
    entry.presentation_end_kind =
        presentation_end_kind_for_output(
            source_extent,
            output_extent,
            entry.source_supports_trusted_decoder_eof);

    entry.start_sample_known = true;
    if (entry.cue_track) {
        if (entry.resampled) {
            entry.start_sample = CueParser::frame75_to_samples(
                entry.cue_start_frame_75, target_rate);
            entry.end_sample = entry.cue_has_end_frame_75
                ? CueParser::frame75_to_samples(entry.cue_end_frame_75, target_rate)
                : output_extent.samples;
            entry.cue_album_end_sample = output_extent.samples;
            if (entry.cue_album_end_sample > 0) {
                entry.start_sample = std::min(entry.start_sample,
                                              entry.cue_album_end_sample);
                entry.end_sample = std::min(entry.end_sample,
                                            entry.cue_album_end_sample);
            }
            // CUE indices remain exact in the source domain, but the number of
            // output frames after streaming resampling is not proven exact.
            entry.end_sample_known = false;
            // Decoder EOF is a valid logical track end only when this CUE entry
            // covers a complete physical file from sample zero through EOF.
            if (entry.cue_has_end_frame_75 || entry.source_start_sample != 0) {
                entry.presentation_end_kind = PresentationEndKind::Unknown;
            }
        } else {
            entry.start_sample = entry.source_start_sample;
            entry.end_sample = entry.source_end_sample;
            entry.cue_album_end_sample = entry.source_cue_album_end_sample;
            entry.end_sample_known = entry.cue_has_end_frame_75 ||
                (sample_extent_supports_bounded_transport(
                     entry.source_sample_extent_kind) &&
                 entry.source_cue_album_end_sample > 0);
            if (entry.cue_has_end_frame_75) {
                entry.sample_extent_kind = SampleExtentKind::ExactPresentationSpan;
                entry.sample_extent_source = SampleExtentSource::CueIndex;
                entry.presentation_end_kind = PresentationEndKind::ExactSampleSpan;
            } else if (entry.end_sample_known) {
                entry.presentation_end_kind = PresentationEndKind::ExactSampleSpan;
            }
        }
        if (entry.end_sample < entry.start_sample) {
            entry.end_sample = entry.start_sample;
        }
    } else {
        entry.start_sample = entry.source_start_sample;
        entry.end_sample = output_extent.samples;
        entry.cue_album_end_sample = 0;
        entry.end_sample_known =
            sample_extent_supports_bounded_transport(entry.sample_extent_kind) &&
            entry.end_sample > entry.start_sample;
    }

    if (target_bits == 16 || target_bits == 24 || target_bits == 32) {
        entry.decoded_format.bits_per_sample = target_bits;
    }
}

void GtkPlayerWindow::refresh_playlist_processing_metadata() {
    if (bulk_preferences_update_) {
        return;
    }
    for (PlaylistEntry& entry : playlist_) {
        refresh_entry_processing_metadata(entry);
    }
}

GaplessTrackSpec GtkPlayerWindow::gapless_spec_for_entry(const PlaylistEntry& entry) const {
    GaplessTrackSpec spec;
    spec.path = entry.audio_file_path;
    spec.format = entry.decoded_format;
    spec.format.sample_rate = playback_sample_rate_for_entry(entry);
    spec.start_sample = entry.start_sample;
    spec.planned_end_sample = entry.end_sample;
    const std::string ext = lower_extension(entry.audio_file_path);
    spec.native_flac = (ext == ".flac" && entry.native_decode);
    spec.start_sample_known = entry.start_sample_known;
    spec.exact_end_sample_known = entry.end_sample_known;
    spec.boundary_mode =
        entry.presentation_end_kind == PresentationEndKind::TrustedDecoderEof
            ? GaplessBoundaryMode::DecoderEof
            : GaplessBoundaryMode::ExactRange;
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
        spec.known_external_info.source_total_samples_per_channel = entry.source_cue_album_end_sample > 0
            ? entry.source_cue_album_end_sample
            : entry.source_end_sample;
        spec.known_external_info.source_supports_trusted_decoder_eof =
            entry.source_supports_trusted_decoder_eof;
        spec.known_external_info.source_exact_presentation_drain_policy =
            entry.source_exact_presentation_drain_policy;
        spec.known_external_info.source_presentation_start_known =
            entry.source_presentation_start_known;
        spec.known_external_info.source_presentation_start_sample =
            entry.source_presentation_start_sample;
        spec.known_external_info.codec_name = entry.codec_name;
        spec.known_external_info.dsd_source = entry.dsd_source;
        spec.known_external_info.dsd_sample_rate = entry.dsd_sample_rate;
        spec.known_external_info.lossless = entry.lossless_source;
        spec.known_external_info.raw_aac = (ext == ".aac");
        spec.known_external_info.sample_extent_kind = entry.sample_extent_kind;
        spec.known_external_info.sample_extent_source = entry.sample_extent_source;
        spec.known_external_info.sample_extent_drain_policy =
            entry.sample_extent_drain_policy;
        spec.known_external_info.source_sample_extent_kind =
            entry.source_sample_extent_kind;
        spec.known_external_info.source_sample_extent_source =
            entry.source_sample_extent_source;
        spec.known_external_info.presentation_end_kind =
            entry.presentation_end_kind;
        spec.has_known_external_info = true;
    }
    return spec;
}

bool GtkPlayerWindow::entries_share_playback_format(const PlaylistEntry& a, const PlaylistEntry& b) const {
    return a.decoded_format.sample_rate == b.decoded_format.sample_rate &&
           a.decoded_format.channels == b.decoded_format.channels &&
           a.decoded_format.bits_per_sample == b.decoded_format.bits_per_sample;
}

bool GtkPlayerWindow::entry_supports_separate_gapless(
    const PlaylistEntry& entry) const {
    // DSD-to-PCM conversion carries decoder filter state across samples.
    // Separate physical DSD files are intentionally not chained through fresh
    // decoders; single-file CUE continuity is handled before this check.
    if (entry.dsd_source ||
        !presentation_end_supports_gapless(entry.presentation_end_kind)) {
        return false;
    }
    if (entry.presentation_end_kind == PresentationEndKind::ExactSampleSpan) {
        return has_exact_sample_range(entry.start_sample_known,
                                      entry.end_sample_known,
                                      entry.start_sample,
                                      entry.end_sample) &&
               sample_extent_supports_gapless_presentation(
                   entry.sample_extent_kind);
    }
    // A trusted EOF is intentionally limited to a complete physical file.
    // Partial resampled CUE ranges still require an exact processed span.
    return entry.start_sample_known && entry.start_sample == 0 &&
           (!entry.cue_track || !entry.cue_has_end_frame_75);
}

bool GtkPlayerWindow::entries_share_gapless_transport(const PlaylistEntry& a,
                                                       const PlaylistEntry& b,
                                                       bool allow_noncontiguous_cue) const {
    if (a.metadata_state != MetadataState::Ready || b.metadata_state != MetadataState::Ready ||
        !entries_share_playback_format(a, b)) {
        return false;
    }

    if (!allow_noncontiguous_cue) {
        if (a.cue_track != b.cue_track) {
            return false;
        }
        if (a.cue_track) {
            return a.audio_file_path == b.audio_file_path && a.end_sample == b.start_sample;
        }
    }

    if (!entry_supports_separate_gapless(a) ||
        !entry_supports_separate_gapless(b)) {
        return false;
    }

    // A separate-file transition may use either an exact numerical span or a
    // trusted fully drained decoder EOF. The planner remains format-agnostic.
    return true;
}

bool GtkPlayerWindow::entries_share_split_cue_file_transport(
    const PlaylistEntry& a,
    const PlaylistEntry& b) const {
    return a.cue_track && b.cue_track &&
           !a.cue_source_path.empty() &&
           a.cue_source_path == b.cue_source_path &&
           a.audio_file_path != b.audio_file_path &&
           entries_share_gapless_transport(a, b, true);
}

std::uint64_t GtkPlayerWindow::track_length_samples(const PlaylistEntry& entry) const {
    return entry.end_sample >= entry.start_sample
        ? entry.end_sample - entry.start_sample
        : 0;
}

std::size_t GtkPlayerWindow::cue_chain_end_index(std::size_t index) const {
    if (index >= playlist_.size() || !playlist_[index].cue_track) {
        return index + 1;
    }
    std::size_t end = index + 1;
    while (end < playlist_.size() &&
           entries_share_gapless_transport(playlist_[end - 1], playlist_[end], false)) {
        ++end;
    }
    return end;
}

std::size_t GtkPlayerWindow::split_cue_file_chain_end_index(std::size_t index) const {
    if (index >= playlist_.size() || !playlist_[index].cue_track ||
        playlist_[index].cue_source_path.empty() ||
        !entry_supports_separate_gapless(playlist_[index])) {
        return index + 1;
    }

    std::size_t end = index + 1;
    while (end < playlist_.size()) {
        const PlaylistEntry& previous = playlist_[end - 1];
        const PlaylistEntry& next = playlist_[end];
        if (!entry_supports_separate_gapless(next) ||
            !entries_share_split_cue_file_transport(previous, next)) {
            break;
        }
        ++end;
    }
    return end;
}

std::size_t GtkPlayerWindow::file_chain_end_index(std::size_t index) const {
    if (index >= playlist_.size() || playlist_[index].cue_track ||
        !entry_supports_separate_gapless(playlist_[index])) {
        return index + 1;
    }
    std::size_t end = index + 1;
    while (end < playlist_.size() &&
           entry_supports_separate_gapless(playlist_[end]) &&
           entries_share_gapless_transport(playlist_[end - 1], playlist_[end], false)) {
        ++end;
    }
    return end;
}

void GtkPlayerWindow::activate_gapless_chain(std::size_t start_index, std::size_t end_index) {
    std::vector<std::size_t> indices;
    if (start_index < playlist_.size() && end_index > start_index && end_index <= playlist_.size()) {
        indices.reserve(end_index - start_index);
        for (std::size_t i = start_index; i < end_index; ++i) {
            indices.push_back(i);
        }
    }
    activate_gapless_chain(indices);
}

void GtkPlayerWindow::activate_gapless_chain(const std::vector<std::size_t>& playlist_indices) {
    if (playlist_indices.size() <= 1) {
        clear_gapless_chain();
        return;
    }

    std::vector<std::uint64_t> offsets;
    offsets.reserve(playlist_indices.size());
    std::uint64_t total = 0;
    for (const std::size_t index : playlist_indices) {
        if (index >= playlist_.size()) {
            clear_gapless_chain();
            return;
        }
        offsets.push_back(total);
        total += track_length_samples(playlist_[index]);
    }
    set_gapless_chain_mapping(playlist_indices, offsets, total, 0);
}

void GtkPlayerWindow::set_gapless_chain_mapping(
    const std::vector<std::size_t>& playlist_indices,
    const std::vector<std::uint64_t>& offsets,
    std::uint64_t total_samples,
    std::size_t active_segment,
    std::size_t decoder_segment_base) {
    if (playlist_indices.empty() || playlist_indices.size() != offsets.size() ||
        active_segment >= playlist_indices.size()) {
        clear_gapless_chain();
        return;
    }
    gapless_chain_active_ = true;
    gapless_chain_playlist_indices_ = playlist_indices;
    gapless_chain_offsets_ = offsets;
    gapless_chain_total_samples_ = total_samples;
    gapless_chain_active_segment_ = active_segment;
    gapless_chain_decoder_segment_base_ = decoder_segment_base;
}

void GtkPlayerWindow::clear_gapless_chain() {
    gapless_chain_active_ = false;
    gapless_chain_playlist_indices_.clear();
    gapless_chain_active_segment_ = 0;
    gapless_chain_decoder_segment_base_ = 0;
    gapless_chain_offsets_.clear();
    gapless_chain_total_samples_ = 0;
}

void GtkPlayerWindow::update_gapless_chain_track_from_status(const PlaybackStatusSnapshot& status) {
    if (!gapless_chain_active_ || gapless_chain_offsets_.empty() ||
        gapless_chain_playlist_indices_.size() != gapless_chain_offsets_.size()) {
        return;
    }
    std::size_t active_segment = gapless_chain_active_segment_;
    if (status.segment_position_valid) {
        if (status.segment_index < gapless_chain_decoder_segment_base_) {
            return;
        }
        const std::size_t relative_segment =
            status.segment_index - gapless_chain_decoder_segment_base_;
        if (relative_segment >= gapless_chain_playlist_indices_.size()) {
            return;
        }
        active_segment = relative_segment;
    } else {
        const std::uint64_t pos = std::min(status.current_samples_per_channel,
                                           gapless_chain_total_samples_);
        for (std::size_t i = 0; i < gapless_chain_offsets_.size(); ++i) {
            const std::uint64_t begin = gapless_chain_offsets_[i];
            const std::uint64_t end = i + 1 < gapless_chain_offsets_.size()
                ? gapless_chain_offsets_[i + 1]
                : gapless_chain_total_samples_;
            if (pos >= begin && (pos < end || i + 1 == gapless_chain_offsets_.size())) {
                active_segment = i;
                break;
            }
        }
    }

    if (active_segment == gapless_chain_active_segment_) {
        return;
    }

    const std::size_t previous_segment = gapless_chain_active_segment_;
    if (active_segment > previous_segment) {
        for (std::size_t segment = previous_segment + 1;
             segment <= active_segment;
             ++segment) {
            const std::size_t crossed_index = gapless_chain_playlist_indices_[segment];
            if (crossed_index >= playlist_.size()) {
                continue;
            }
            if (random_enabled_) {
                record_random_chain_transition(crossed_index);
            } else {
                record_track_started(crossed_index, PlaybackStartReason::Automatic);
            }
        }
    }

    gapless_chain_active_segment_ = active_segment;
    const std::size_t active_index = gapless_chain_playlist_indices_[active_segment];
    if (active_index >= playlist_.size()) {
        return;
    }

    const std::size_t previous_playing_index = current_track_index_;
    current_track_index_ = active_index;
    if (active_segment < active_track_transport_states_.size()) {
        active_range_limited_transport_ =
            active_track_transport_states_[active_segment].range_limited;
    }
    sync_playlist_selection_after_transport_change(
        current_track_index_,
        true,
        random_enabled_
            ? automatic_transport_scroll_policy(current_track_index_)
            : PlaylistScrollPolicy::EnsureVisible);
    if (previous_playing_index != current_track_index_) {
        refresh_playlist_row_styles(playlist_view_);
    }
    mark_mpris_track_changed();
    refresh_active_alsa_output_diagnostics();
}

std::uint64_t GtkPlayerWindow::current_track_position_from_samples(
    std::uint64_t samples_per_channel,
    std::uint64_t track_length) const {
    std::uint64_t pos = samples_per_channel;
    if (gapless_chain_active_ && gapless_chain_active_segment_ < gapless_chain_offsets_.size() &&
        gapless_chain_active_segment_ < gapless_chain_playlist_indices_.size() &&
        gapless_chain_playlist_indices_[gapless_chain_active_segment_] == current_track_index_) {
        const std::uint64_t offset = gapless_chain_offsets_[gapless_chain_active_segment_];
        pos = pos > offset ? pos - offset : 0;
    }
    if (track_length > 0 && pos > track_length) {
        pos = track_length;
    }
    return pos;
}

std::uint64_t GtkPlayerWindow::current_track_position_from_status(
    const PlaybackStatusSnapshot& status) const {
    if (gapless_chain_active_ && status.segment_position_valid &&
        status.segment_index >= gapless_chain_decoder_segment_base_) {
        const std::size_t relative_segment =
            status.segment_index - gapless_chain_decoder_segment_base_;
        if (relative_segment == gapless_chain_active_segment_ &&
            relative_segment < gapless_chain_playlist_indices_.size() &&
            gapless_chain_playlist_indices_[relative_segment] == current_track_index_) {
            return status.segment_samples_per_channel;
        }
    }
    if (playlist_.empty() || current_track_index_ >= playlist_.size()) {
        return 0;
    }
    const std::uint64_t track_length = active_track_length_samples(
        status.playing,
        status.total_samples_per_channel,
        playlist_[current_track_index_]);
    return current_track_position_from_samples(
        status.current_samples_per_channel,
        track_length);
}

std::uint64_t GtkPlayerWindow::current_track_position_from_transport(
    const PlaybackTransportSnapshot& transport) const {
    if (gapless_chain_active_ && transport.segment_position_valid &&
        transport.segment_index >= gapless_chain_decoder_segment_base_) {
        const std::size_t relative_segment =
            transport.segment_index - gapless_chain_decoder_segment_base_;
        if (relative_segment == gapless_chain_active_segment_ &&
            relative_segment < gapless_chain_playlist_indices_.size() &&
            gapless_chain_playlist_indices_[relative_segment] == current_track_index_) {
            return transport.segment_samples_per_channel;
        }
    }
    if (playlist_.empty() || current_track_index_ >= playlist_.size()) {
        return 0;
    }
    const std::uint64_t track_length = active_track_length_samples(
        transport.playing,
        transport.total_samples_per_channel,
        playlist_[current_track_index_]);
    return current_track_position_from_samples(
        transport.current_samples_per_channel,
        track_length);
}

std::unique_ptr<IAudioDecoder> GtkPlayerWindow::create_decoder_for_entry(const PlaylistEntry& entry) const {
    const std::string ext = lower_extension(entry.audio_file_path);
    const std::uint32_t source_rate = entry.source_sample_rate > 0 ? entry.source_sample_rate : entry.decoded_format.sample_rate;
    const std::uint16_t source_bits = entry.source_bits_per_sample > 0 ? entry.source_bits_per_sample : entry.decoded_format.bits_per_sample;
    const std::uint32_t target_rate = output_sample_rate_for_entry(entry);
    const std::uint16_t target_bits = output_bits_for_entry(entry);
    const bool resample_needed = (target_rate > 0 && target_rate != source_rate);
    const bool bitdepth_needed = entry.dsd_source ||
                                 (target_bits > 0 && target_bits != source_bits);
    if (ext == ".flac" && entry.native_decode && !resample_needed && !bitdepth_needed &&
        !entry.is_stream) {
        return std::unique_ptr<IAudioDecoder>(new FlacStreamDecoder());
    }
    if (entry.is_stream || StreamAudioDecoder::is_stream_uri(entry.audio_file_path)) {
        std::unique_ptr<ExternalAudioDecoder> decoder;
        if (resample_needed || bitdepth_needed) {
            decoder = std::make_unique<ExternalAudioDecoder>(target_rate,
                                                             target_bits,
                                                             resample_quality_,
                                                             bitdepth_quality_);
        } else {
            decoder = std::make_unique<ExternalAudioDecoder>();
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
        known.dsd_source = entry.dsd_source;
        known.dsd_sample_rate = entry.dsd_sample_rate;
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
        known.source_total_samples_per_channel = entry.source_cue_album_end_sample > 0
            ? entry.source_cue_album_end_sample
            : entry.source_end_sample;
        known.source_supports_trusted_decoder_eof =
            entry.source_supports_trusted_decoder_eof;
        known.source_exact_presentation_drain_policy =
            entry.source_exact_presentation_drain_policy;
        known.source_presentation_start_known =
            entry.source_presentation_start_known;
        known.source_presentation_start_sample =
            entry.source_presentation_start_sample;
        known.codec_name = entry.codec_name;
        known.dsd_source = entry.dsd_source;
        known.dsd_sample_rate = entry.dsd_sample_rate;
        known.lossless = entry.lossless_source;
        known.sample_extent_kind = entry.sample_extent_kind;
        known.sample_extent_source = entry.sample_extent_source;
        known.sample_extent_drain_policy = entry.sample_extent_drain_policy;
        known.source_sample_extent_kind = entry.source_sample_extent_kind;
        known.source_sample_extent_source = entry.source_sample_extent_source;
        known.presentation_end_kind = entry.presentation_end_kind;
        decoder->set_known_info(known);
        return std::unique_ptr<IAudioDecoder>(decoder.release());
    }
    throw std::runtime_error("Unsupported audio file type: " + ext);
}

std::uint32_t GtkPlayerWindow::current_tone_control_sample_rate() const {
    const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
    if (transport.playing && transport.format.sample_rate > 0) {
        return transport.format.sample_rate;
    }
    if (current_track_index_ < playlist_.size()) {
        const PlaylistEntry& entry = playlist_[current_track_index_];
        if (entry.source_sample_rate > 0) {
            const std::uint32_t target_rate = output_sample_rate_for_entry(entry);
            if (target_rate > 0) {
                return target_rate;
            }
        }
        if (entry.decoded_format.sample_rate > 0) {
            return entry.decoded_format.sample_rate;
        }
    }
    return 44100;
}

std::string GtkPlayerWindow::processing_rules_report_for_entry(
    const PlaylistEntry& entry,
    const AudioFormat& active_output_format) const {
    std::ostringstream out;
    out << "Processing rules:\n";

    const std::uint32_t final_rate = active_output_format.sample_rate > 0
        ? active_output_format.sample_rate
        : (entry.decoded_format.sample_rate > 0
               ? entry.decoded_format.sample_rate
               : entry.source_sample_rate);
    const std::uint16_t final_bits = active_output_format.bits_per_sample > 0
        ? active_output_format.bits_per_sample
        : (entry.decoded_format.bits_per_sample > 0
               ? entry.decoded_format.bits_per_sample
               : entry.source_bits_per_sample);

    if (entry.dsd_source) {
        const DsdRateDefinition* definition =
            find_dsd_rate_definition(entry.dsd_sample_rate);
        const bool additional_resampling =
            final_rate > 0 && final_rate != entry.source_sample_rate;
        const bool dither_active = final_bits <= 16;
        const bool quality_filter_active =
            additional_resampling || dither_active;

        out << "Active: yes (DSD to PCM)\n";
        out << "DSD source: "
            << (definition != nullptr
                    ? definition->source_label
                    : format_dsd_rate_mhz(entry.dsd_sample_rate))
            << '\n';
        out << "FFmpeg API PCM: "
            << format_rate_khz(entry.source_sample_rate) << '\n';
        out << "Final PCM: " << format_rate_khz(final_rate)
            << " / " << final_bits << "-bit\n";
        if (quality_filter_active) {
            out << "Resampler: initializing\n";
        } else {
            out << "Resampler: not used by a processing rule\n";
        }
        if (dither_active) {
            out << "Dither: " << dither_quality_label(bitdepth_quality_)
                << " (applied at 16-bit DSD-to-PCM output)";
        } else {
            out << "Dither: not applied";
        }
        return out.str();
    }

    const bool processing_active = entry.resampled || entry.bitdepth_converted;
    const bool quality_filter_active = processing_active;
    const bool dither_active = quality_filter_active && final_bits <= 16;

    out << "Active: " << (processing_active ? "yes" : "no") << '\n';
    if (entry.resampled) {
        out << "Resampling: " << format_rate_khz(entry.source_sample_rate)
            << " -> " << format_rate_khz(final_rate) << '\n';
    } else {
        out << "Resampling: inactive\n";
    }
    if (quality_filter_active) {
        out << "Resampler: initializing\n";
    } else {
        out << "Resampler: not used by a processing rule\n";
    }
    if (entry.bitdepth_converted) {
        out << "Bit-depth conversion: " << entry.source_bits_per_sample
            << "-bit -> " << final_bits << "-bit\n";
    } else {
        out << "Bit-depth conversion: inactive (sample width unchanged)\n";
    }
    if (dither_active) {
        out << "Dither: " << dither_quality_label(bitdepth_quality_);
        if (entry.resampled && !entry.bitdepth_converted) {
            out << " (applied at 16-bit resampling output)";
        } else if (!entry.resampled && entry.bitdepth_converted) {
            out << " (applied during bit-depth conversion)";
        } else {
            out << " (applied at 16-bit processing output)";
        }
    } else {
        out << "Dither: not applied";
    }
    return out.str();
}

std::string GtkPlayerWindow::processing_path_for_entry(
    const PlaylistEntry& entry,
    const AudioFormat& active_output_format) const {
    const std::string ext = lower_extension(entry.audio_file_path);
    const std::string source_name = media_source_summary(entry);

    const std::uint32_t shown_rate = active_output_format.sample_rate > 0
        ? active_output_format.sample_rate
        : playback_sample_rate_for_entry(entry);
    const std::uint16_t shown_bits = active_output_format.bits_per_sample > 0
        ? active_output_format.bits_per_sample
        : output_bits_for_entry(entry);

    std::string path;
    if (entry.dsd_source) {
        const DsdRateDefinition* definition = find_dsd_rate_definition(entry.dsd_sample_rate);
        const std::string container_name = ext == ".dsf"
            ? "DSF"
            : (ext == ".dff" ? "DFF" : codec_label_for_entry(entry.codec_name, entry.audio_file_path));
        const std::string dsd_name = definition != nullptr
            ? std::string(definition->source_label)
            : (std::string("DSD · ") + format_dsd_rate_mhz(entry.dsd_sample_rate));
        path = "Path: " + container_name + " " + dsd_name +
               " → FFmpeg API DSD decoder";
        if (entry.resampled) {
            path += " → FFmpeg resampler " + format_rate_khz(shown_rate);
        }
    } else {
        const bool uses_external_decoder = !entry.native_decode;
        const std::string decoder_name = uses_external_decoder
            ? ((entry.resampled || entry.bitdepth_converted)
                   ? "FFmpeg API processing"
                   : "FFmpeg API")
            : "libFLAC";
        path = "Path: " + source_name + " → " + decoder_name;
        if (entry.resampled) {
            path += " → Resampled " + std::to_string(entry.source_sample_rate) +
                    "→" + std::to_string(shown_rate);
        }
        if (entry.bitdepth_converted) {
            path += " → Bit-depth " + std::to_string(entry.source_bits_per_sample) +
                    "→" + std::to_string(shown_bits);
        }
    }
    path += " → PCM " +
            std::to_string(shown_rate / 1000) + "." +
            std::to_string((shown_rate % 1000) / 100) + "k/" +
            std::to_string(shown_bits);
    return path;
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

std::string GtkPlayerWindow::current_transport_processing_report() const {
    std::ostringstream out;
    const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
    const std::uint16_t channels = transport.format.channels;
    if (transport.playing && channels > 0) {
        out << "Channel mode: native " << channels << "-channel PCM\n";
        if (channels > 2) {
            out << "Stereo tonal DSP: bypassed for multichannel transport\n";
        } else {
            const bool tonal_dsp_active =
                engine_.bass_db() != 0 || engine_.treble_db() != 0 ||
                engine_.pre_eq_headroom_tenths_db() > 0 ||
                engine_.deep_bass_enabled();
            out << "Stereo tonal DSP: "
                << (tonal_dsp_active ? "active" : "inactive") << '\n';
        }
        const int soft_volume_percent = engine_.soft_volume_percent();
        out << "Soft volume: "
            << (soft_volume_percent < 100 ? "active" : "inactive")
            << " (" << soft_volume_percent << "%)\n\n";
    }
    const bool gapless_active =
        gapless_chain_active_ && gapless_chain_playlist_indices_.size() > 1;
    const bool next_gapless_expected =
        gapless_active &&
        gapless_chain_active_segment_ + 1 < gapless_chain_playlist_indices_.size();

    out << "Playback transport:\n";
    out << "RangeLimitedDecoder: "
        << (active_range_limited_transport_ ? "active" : "inactive") << '\n';

    if (!playlist_.empty() && current_track_index_ < playlist_.size() &&
        playlist_[current_track_index_].metadata_state == MetadataState::Ready) {
        const PlaylistEntry& entry = playlist_[current_track_index_];
        const auto extent_short_name = [](SampleExtentKind kind) {
            switch (kind) {
            case SampleExtentKind::ExactPresentationSpan: return "EP";
            case SampleExtentKind::ExactDecodedSpan: return "ED";
            case SampleExtentKind::EstimatedTimeline: return "ET";
            case SampleExtentKind::Unknown:
            default: return "U";
            }
        };
        const auto append_extent = [&out, &extent_short_name](
            SampleExtentKind kind, SampleExtentSource source) {
            out << extent_short_name(kind);
            if (source != SampleExtentSource::None) {
                out << " / " << sample_extent_source_name(source);
            }
        };

        out << "Boundary: ";
        append_extent(entry.source_sample_extent_kind,
                      entry.source_sample_extent_source);
        if (entry.sample_extent_kind != entry.source_sample_extent_kind ||
            entry.sample_extent_source != entry.source_sample_extent_source) {
            out << " → ";
            append_extent(entry.sample_extent_kind, entry.sample_extent_source);
        }
        if (entry.presentation_end_kind == PresentationEndKind::ExactSampleSpan) {
            out << " → ExactSampleSpan";
        } else if (entry.presentation_end_kind == PresentationEndKind::TrustedDecoderEof) {
            out << " → TrustedDecoderEof";
        } else if (entry.sample_extent_kind == SampleExtentKind::ExactDecodedSpan &&
                   entry.end_sample_known) {
            out << " → ExactDecodedSpan";
        } else {
            out << " → Unknown";
        }
        out << '\n';
        out << "Gapless capability: ";
        if (entry.dsd_source) {
            out << "none (separate-file DSD disabled)";
        } else if (entry_supports_separate_gapless(entry)) {
            out << (entry.presentation_end_kind == PresentationEndKind::ExactSampleSpan
                        ? "ExactRange"
                        : "DecoderEof");
        } else if (entry.source_sample_extent_kind == SampleExtentKind::ExactPresentationSpan &&
                   entry.source_exact_presentation_drain_policy ==
                       ExactPresentationDrainPolicy::ExactRangeRequired &&
                   entry.presentation_end_kind == PresentationEndKind::Unknown) {
            out << "none (ExactRangeRequired)";
        } else if (entry.sample_extent_kind == SampleExtentKind::ExactDecodedSpan) {
            out << "none (decoded span only)";
        } else if (entry.presentation_end_kind == PresentationEndKind::TrustedDecoderEof) {
            out << "none (full-file EOF required)";
        } else {
            out << "none";
        }
        out << '\n';
    }

    out << "Gapless chain: " << (gapless_active ? "active" : "inactive");
    if (gapless_active) {
        std::string chain_detail;
        if (!playlist_.empty() && current_track_index_ < playlist_.size()) {
            const PlaylistEntry& entry = playlist_[current_track_index_];
            if (active_gapless_transport_kind_ == "Continuous single-file CUE") {
                chain_detail = active_gapless_transport_kind_;
            } else if (entry.presentation_end_kind == PresentationEndKind::TrustedDecoderEof) {
                chain_detail = "DecoderEof";
            } else if (entry.presentation_end_kind == PresentationEndKind::ExactSampleSpan) {
                chain_detail = "ExactRange";
            }
        }
        if (chain_detail.empty()) {
            chain_detail = active_gapless_transport_kind_;
        }
        if (!chain_detail.empty()) {
            out << " (" << chain_detail << ")";
        }
    }
    out << '\n';
    out << "Next gapless transition: "
        << (next_gapless_expected ? "expected" : "not expected");

    std::size_t report_index = 0;
    if (gapless_active) {
        report_index = gapless_chain_active_segment_;
    }
    if (report_index < active_track_transport_states_.size() &&
        !active_track_transport_states_[report_index].processing_report.empty()) {
        const ActiveTrackTransportState& state =
            active_track_transport_states_[report_index];
        std::string processing_report = state.processing_report;
        if (state.resampler_runtime_reported) {
            processing_report = replace_resampler_runtime_line(
                processing_report,
                engine_.resampler_runtime_kind(),
                state.soxr_runtime_description);
        }
        out << "\n\n"
            << processing_report;
    }
    return out.str();
}

void GtkPlayerWindow::refresh_active_alsa_output_diagnostics() {
    if (diagnostics_active_output_value_ == nullptr ||
        !GTK_IS_LABEL(diagnostics_active_output_value_)) {
        return;
    }

    std::string text_value = engine_.active_output_report();
    if (text_value.empty()) {
        text_value = "No active ALSA output yet.";
    } else {
        const std::string pipeline_report = current_transport_processing_report();
        if (!pipeline_report.empty()) {
            text_value += "\n\n" + pipeline_report;
        }
    }
    gtk_label_set_text(GTK_LABEL(diagnostics_active_output_value_), text_value.c_str());
}

void GtkPlayerWindow::refresh_stereo_tonal_dsp_controls(
    bool playing,
    std::uint16_t channels) {
    if (stereo_tonal_dsp_controls_.empty()) {
        return;
    }
    channels = playing ? channels : 0;
    if (!playing) {
        const std::size_t target_index = playlist_play_target_index();
        if (target_index < playlist_.size() &&
            playlist_[target_index].metadata_state == MetadataState::Ready) {
            channels = playlist_[target_index].decoded_format.channels;
        }
    }
    const bool enabled = channels == 0 || channels <= 2;
    if (applied_stereo_tonal_dsp_controls_enabled_.has_value() &&
        *applied_stereo_tonal_dsp_controls_enabled_ == enabled) {
        return;
    }
    for (GtkWidget* widget : stereo_tonal_dsp_controls_) {
        if (widget != nullptr && GTK_IS_WIDGET(widget)) {
            gtk_widget_set_sensitive(widget, enabled);
        }
    }
    applied_stereo_tonal_dsp_controls_enabled_ = enabled;
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

void GtkPlayerWindow::start_source_scan_worker() {
    if (source_scan_worker_.joinable()) {
        return;
    }
    source_scan_worker_stop_ = false;
    source_scan_worker_ = std::thread(&GtkPlayerWindow::source_scan_worker_loop, this);
}

void GtkPlayerWindow::stop_source_scan_worker() {
    {
        std::lock_guard<std::mutex> lock(source_scan_mutex_);
        source_scan_worker_stop_ = true;
        if (active_source_scan_cancel_) {
            active_source_scan_cancel_->store(true, std::memory_order_relaxed);
        }
        source_scan_jobs_.clear();
    }
    source_scan_cv_.notify_all();
    if (source_scan_worker_.joinable()) {
        source_scan_worker_.join();
    }
    {
        std::lock_guard<std::mutex> lock(source_scan_mutex_);
        source_scan_completions_.clear();
        source_scan_completion_pending_.store(false, std::memory_order_release);
        active_source_scan_cancel_.reset();
    }
    source_scan_active_ = false;
}

void GtkPlayerWindow::schedule_source_scan_completion_dispatch() {
    bool expected = false;
    if (!source_scan_dispatch_scheduled_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }
    const std::shared_ptr<std::atomic<bool>> lifetime = ui_dispatch_lifetime_;
    if (lifetime == nullptr || !lifetime->load(std::memory_order_acquire)) {
        source_scan_dispatch_scheduled_.store(false, std::memory_order_release);
        return;
    }
    auto* dispatch = new (std::nothrow) AsyncUiDispatch;
    if (dispatch == nullptr) {
        source_scan_dispatch_scheduled_.store(false, std::memory_order_release);
        Logger::instance().error("Cannot allocate source-scan completion dispatch");
        return;
    }
    dispatch->window = this;
    dispatch->lifetime = lifetime;
    g_main_context_invoke_full(nullptr,
                               G_PRIORITY_DEFAULT,
                               GtkPlayerWindow::on_source_scan_completion_dispatch,
                               dispatch,
                               destroy_async_ui_dispatch);
}

gboolean GtkPlayerWindow::on_source_scan_completion_dispatch(gpointer user_data) {
    auto* dispatch = static_cast<AsyncUiDispatch*>(user_data);
    if (dispatch == nullptr || dispatch->window == nullptr ||
        dispatch->lifetime == nullptr ||
        !dispatch->lifetime->load(std::memory_order_acquire)) {
        return G_SOURCE_REMOVE;
    }
    GtkPlayerWindow* self = dispatch->window;
    self->source_scan_dispatch_scheduled_.store(false, std::memory_order_release);
    if (!self->ui_closing_) {
        self->drain_source_scan_results();
    }
    return G_SOURCE_REMOVE;
}

void GtkPlayerWindow::source_scan_worker_loop() {
    for (;;) {
        SourceScanJob job;
        {
            std::unique_lock<std::mutex> lock(source_scan_mutex_);
            source_scan_cv_.wait(lock, [this]() {
                return source_scan_worker_stop_ || !source_scan_jobs_.empty();
            });
            if (source_scan_worker_stop_) {
                return;
            }
            job = std::move(source_scan_jobs_.front());
            source_scan_jobs_.pop_front();
        }

        SourceScanCompletion completion;
        completion.job = job;
        completion.result = SourceScanner::scan(
            job.source_paths,
            job.cancel_requested ? job.cancel_requested.get() : nullptr);

        {
            std::lock_guard<std::mutex> lock(source_scan_mutex_);
            if (source_scan_worker_stop_) {
                return;
            }
            source_scan_completions_.push_back(std::move(completion));
            source_scan_completion_pending_.store(true, std::memory_order_release);
        }
        schedule_source_scan_completion_dispatch();
    }
}

void GtkPlayerWindow::cancel_source_scan() {
    std::lock_guard<std::mutex> lock(source_scan_mutex_);
    if (active_source_scan_cancel_) {
        active_source_scan_cancel_->store(true, std::memory_order_relaxed);
    }
    ++source_scan_generation_;
    source_scan_jobs_.clear();
    source_scan_completions_.clear();
    source_scan_completion_pending_.store(false, std::memory_order_release);
    active_source_scan_cancel_.reset();
    source_scan_active_ = false;
}

void GtkPlayerWindow::remember_open_directory_from_sources(
    const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
        if (path.empty()) {
            continue;
        }
        if (g_file_test(path.c_str(), G_FILE_TEST_IS_DIR)) {
            last_open_directory_ = path;
            return;
        }
        if (g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR) &&
            is_supported_media_path(path)) {
            last_open_directory_ = directory_name(path);
            return;
        }
    }
}

bool GtkPlayerWindow::open_source_paths(const std::vector<std::string>& paths,
                                        bool replace_playlist,
                                        bool quiet,
                                        bool record_last_sources,
                                        const std::string& play_after_load_path,
                                        bool restore_saved_sources) {
    if (paths.empty() || ui_closing_) {
        return false;
    }
    if (!restore_saved_sources) {
        cancel_pending_last_active_track_restore();
    }

    bool contains_directory = false;
    bool contains_usable_source = false;
    for (const std::string& path : paths) {
        if (path.empty()) {
            continue;
        }
        if (g_file_test(path.c_str(), G_FILE_TEST_IS_DIR)) {
            contains_directory = true;
            contains_usable_source = true;
            continue;
        }
        if (StreamAudioDecoder::is_stream_uri(path)) {
            contains_usable_source = true;
            continue;
        }
        if (g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR) &&
            is_supported_media_path(path)) {
            contains_usable_source = true;
        }
    }
    if (!contains_usable_source) {
        return false;
    }

    if (!contains_directory) {
        return !load_source_paths(paths,
                                  replace_playlist,
                                  quiet,
                                  record_last_sources,
                                  play_after_load_path,
                                  restore_saved_sources).empty();
    }

    enqueue_source_scan(paths,
                        replace_playlist,
                        quiet,
                        record_last_sources,
                        play_after_load_path,
                        restore_saved_sources);
    return true;
}

void GtkPlayerWindow::enqueue_source_scan(const std::vector<std::string>& paths,
                                          bool replace_playlist,
                                          bool quiet,
                                          bool record_last_sources,
                                          const std::string& play_after_load_path,
                                          bool restore_saved_sources) {
    if (paths.empty() || ui_closing_) {
        return;
    }
    start_source_scan_worker();

    if (record_last_sources && restore_sources_idle_id_ != 0) {
        g_source_remove(restore_sources_idle_id_);
        restore_sources_idle_id_ = 0;
    }

    SourceScanJob job;
    job.source_paths = paths;
    job.replace_playlist = replace_playlist;
    job.quiet = quiet;
    job.record_last_sources = record_last_sources;
    job.restore_saved_sources = restore_saved_sources;
    job.play_after_load_path = play_after_load_path;
    job.cancel_requested = std::make_shared<std::atomic<bool>>(false);

    {
        std::lock_guard<std::mutex> lock(source_scan_mutex_);
        if (active_source_scan_cancel_) {
            active_source_scan_cancel_->store(true, std::memory_order_relaxed);
        }
        ++source_scan_generation_;
        job.generation = source_scan_generation_;
        source_scan_jobs_.clear();
        source_scan_completions_.clear();
        source_scan_completion_pending_.store(false, std::memory_order_release);
        active_source_scan_cancel_ = job.cancel_requested;
        source_scan_jobs_.push_back(job);
        source_scan_active_ = true;
    }
    source_scan_cv_.notify_one();

    update_loading_controls();
    refresh_display();
    notify_mpris_state_changed();
}

void GtkPlayerWindow::drain_source_scan_results() {
    if (!source_scan_completion_pending_.load(std::memory_order_acquire)) {
        return;
    }
    std::deque<SourceScanCompletion> completions;
    {
        std::lock_guard<std::mutex> lock(source_scan_mutex_);
        completions.swap(source_scan_completions_);
        source_scan_completion_pending_.store(false, std::memory_order_release);
    }

    for (SourceScanCompletion& completion : completions) {
        if (completion.job.generation != source_scan_generation_) {
            continue;
        }

        source_scan_active_ = false;
        {
            std::lock_guard<std::mutex> lock(source_scan_mutex_);
            if (completion.job.generation == source_scan_generation_) {
                active_source_scan_cancel_.reset();
            }
        }

        if (completion.result.cancelled || ui_closing_) {
            continue;
        }

        for (const std::string& error : completion.result.errors) {
            if (completion.job.quiet) {
                Logger::instance().debug(error);
            } else {
                Logger::instance().error(error);
            }
        }

        if (completion.result.sources.empty()) {
            const std::string message = "No supported audio sources were found";
            if (completion.job.quiet) {
                Logger::instance().debug(message);
            } else {
                Logger::instance().error(message);
            }
            update_loading_controls();
            refresh_display();
            notify_mpris_state_changed();
            continue;
        }

        load_resolved_source_paths(completion.result.sources,
                                   completion.job.replace_playlist,
                                   completion.job.quiet,
                                   completion.job.record_last_sources,
                                   completion.job.play_after_load_path,
                                   completion.job.restore_saved_sources);
    }
}

std::size_t GtkPlayerWindow::append_source_placeholders(const std::string& path,
                                                        const std::string& top_level_source_path,
                                                        std::vector<std::string>* probe_paths) {
    if (probe_paths == nullptr) {
        return 0;
    }

    if (StreamAudioDecoder::is_stream_uri(path)) {
        return append_source_stream_placeholder(path, top_level_source_path);
    }

    if (M3uPlaylistReader::looks_like_playlist_path(path)) {
        const std::vector<M3uPlaylistEntry> entries = M3uPlaylistReader::read_entries(path);
        Logger::instance().info("Importing playlist: " + path + " entries=" + std::to_string(entries.size()));
        std::size_t appended = 0;
        for (const M3uPlaylistEntry& playlist_entry : entries) {
            const std::string& item = playlist_entry.location;
            if (M3uPlaylistReader::looks_like_playlist_path(item)) {
                Logger::instance().debug("Skipping nested playlist entry: " + item);
                continue;
            }
            if (StreamAudioDecoder::is_stream_uri(item)) {
                appended += append_source_stream_placeholder(item,
                                                            top_level_source_path,
                                                            playlist_entry.title,
                                                            playlist_entry.artist);
                continue;
            }
            if (!g_file_test(item.c_str(), G_FILE_TEST_IS_REGULAR)) {
                Logger::instance().debug("Skipping unavailable playlist entry: " + item);
                continue;
            }
            try {
                appended += append_source_placeholders(item, top_level_source_path, probe_paths);
            } catch (const std::exception& ex) {
                Logger::instance().debug(std::string("Skipping playlist entry: ") + item + " (" + ex.what() + ")");
            }
        }
        return appended;
    }

    if (CueParser::looks_like_cue_path(path)) {
        CueSheet sheet;
        const auto cached = cue_cache_.find(path);
        if (cached != cue_cache_.end()) {
            sheet = cached->second;
        } else {
            sheet = CueParser::parse_file(path, 0);
            cue_cache_[path] = sheet;
        }

        for (const std::string& audio_file_path : sheet.audio_file_paths) {
            if (!g_file_test(audio_file_path.c_str(), G_FILE_TEST_IS_REGULAR)) {
                throw std::runtime_error("CUE audio file is unavailable: " + audio_file_path);
            }
            if (!is_supported_media_path(audio_file_path) ||
                CueParser::looks_like_cue_path(audio_file_path) ||
                M3uPlaylistReader::looks_like_playlist_path(audio_file_path)) {
                throw std::runtime_error("Unsupported CUE audio file type: " + audio_file_path);
            }
            probe_paths->push_back(audio_file_path);
        }

        for (const CueTrack& cue_track : sheet.tracks) {
            if (cue_track.audio_file_path.empty()) {
                throw std::runtime_error("CUE track does not reference an audio file");
            }
            PlaylistEntry entry;
            entry.audio_file_path = cue_track.audio_file_path;
            entry.top_level_source_path = top_level_source_path;
            entry.cue_source_path = path;
            entry.original_order = next_playlist_original_order_++;
            entry.load_generation = metadata_generation_;
            entry.track_number = cue_track.number;
            entry.title = safe_utf8_for_display(cue_track.title);
            entry.performer = safe_utf8_for_display(
                cue_track.performer.empty() ? sheet.performer : cue_track.performer);
            entry.album = safe_utf8_for_display(sheet.title);
            entry.start_sample = cue_track.start_sample;
            entry.end_sample = cue_track.end_sample;
            entry.source_start_sample = cue_track.start_sample;
            entry.source_end_sample = cue_track.end_sample;
            entry.cue_start_frame_75 = cue_track.start_frame_75;
            entry.cue_end_frame_75 = cue_track.end_frame_75;
            entry.cue_has_end_frame_75 = cue_track.has_end_frame_75;
            entry.source_label = safe_utf8_for_display(base_name(path));
            entry.cue_track = true;
            playlist_.push_back(std::move(entry));
            index_playlist_entry(playlist_.size() - 1);
        }
        return sheet.tracks.size();
    }

    if (!is_supported_media_path(path) ||
        M3uPlaylistReader::looks_like_playlist_path(path) ||
        CueParser::looks_like_cue_path(path)) {
        throw std::runtime_error("Unsupported audio file type: " + path);
    }

    PlaylistEntry entry;
    entry.audio_file_path = path;
    entry.top_level_source_path = top_level_source_path;
    entry.original_order = next_playlist_original_order_++;
    entry.load_generation = metadata_generation_;
    entry.track_number = static_cast<int>(playlist_.size() + 1);
    entry.title = safe_utf8_for_display(temporary_title_from_path(path));
    entry.source_label = safe_utf8_for_display(base_name(path));
    playlist_.push_back(std::move(entry));
    index_playlist_entry(playlist_.size() - 1);
    probe_paths->push_back(path);
    return 1;
}

void GtkPlayerWindow::start_metadata_worker() {
    if (!metadata_workers_.empty()) {
        return;
    }

    const std::size_t worker_count = std::max<std::size_t>(
        1,
        std::min<std::size_t>(kMetadataProbeWorkerCount,
                              static_cast<std::size_t>(std::thread::hardware_concurrency())));

    std::vector<std::unique_ptr<ProbeCancellation>> cancellations;
    cancellations.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        cancellations.push_back(std::make_unique<ProbeCancellation>());
    }
    metadata_probe_cancellations_ = std::move(cancellations);

    metadata_worker_stop_ = false;
    metadata_workers_.reserve(worker_count);
    for (std::size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
        metadata_workers_.emplace_back(&GtkPlayerWindow::metadata_worker_loop, this, worker_index);
    }
}

void GtkPlayerWindow::stop_metadata_worker() {
    clear_pending_metadata_play();
    {
        std::lock_guard<std::mutex> lock(metadata_worker_mutex_);
        metadata_worker_stop_ = true;
        metadata_jobs_.clear();
        metadata_probe_path_states_.clear();
    }
    for (const std::unique_ptr<ProbeCancellation>& probe_cancellation : metadata_probe_cancellations_) {
        if (probe_cancellation) {
            probe_cancellation->cancel();
        }
    }
    metadata_worker_cv_.notify_all();
    for (std::thread& worker : metadata_workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    metadata_workers_.clear();
    metadata_probe_cancellations_.clear();
    std::lock_guard<std::mutex> lock(metadata_worker_mutex_);
    metadata_completions_.clear();
    metadata_completion_pending_.store(false, std::memory_order_release);
}

void GtkPlayerWindow::schedule_metadata_completion_dispatch() {
    bool expected = false;
    if (!metadata_dispatch_scheduled_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }
    const std::shared_ptr<std::atomic<bool>> lifetime = ui_dispatch_lifetime_;
    if (lifetime == nullptr || !lifetime->load(std::memory_order_acquire)) {
        metadata_dispatch_scheduled_.store(false, std::memory_order_release);
        return;
    }
    auto* dispatch = new (std::nothrow) AsyncUiDispatch;
    if (dispatch == nullptr) {
        metadata_dispatch_scheduled_.store(false, std::memory_order_release);
        Logger::instance().error("Cannot allocate metadata completion dispatch");
        return;
    }
    dispatch->window = this;
    dispatch->lifetime = lifetime;
    g_main_context_invoke_full(nullptr,
                               G_PRIORITY_DEFAULT,
                               GtkPlayerWindow::on_metadata_completion_dispatch,
                               dispatch,
                               destroy_async_ui_dispatch);
}

gboolean GtkPlayerWindow::on_metadata_completion_dispatch(gpointer user_data) {
    auto* dispatch = static_cast<AsyncUiDispatch*>(user_data);
    if (dispatch == nullptr || dispatch->window == nullptr ||
        dispatch->lifetime == nullptr ||
        !dispatch->lifetime->load(std::memory_order_acquire)) {
        return G_SOURCE_REMOVE;
    }
    GtkPlayerWindow* self = dispatch->window;
    self->metadata_dispatch_scheduled_.store(false, std::memory_order_release);
    if (!self->ui_closing_) {
        self->drain_metadata_probe_results();
    }
    return G_SOURCE_REMOVE;
}

void GtkPlayerWindow::metadata_worker_loop(std::size_t worker_index) {
    if (worker_index >= metadata_probe_cancellations_.size()) {
        return;
    }
    ProbeCancellation* const probe_cancellation = metadata_probe_cancellations_[worker_index].get();

    for (;;) {
        MetadataProbeJob job;
        {
            std::unique_lock<std::mutex> lock(metadata_worker_mutex_);
            metadata_worker_cv_.wait(lock, [this]() {
                return metadata_worker_stop_ || !metadata_jobs_.empty();
            });
            if (metadata_worker_stop_) {
                return;
            }
            job = std::move(metadata_jobs_.front());
            metadata_jobs_.pop_front();

            if (job.generation != metadata_generation_) {
                continue;
            }
            const auto state_it = metadata_probe_path_states_.find(job.path);
            if (state_it == metadata_probe_path_states_.end() ||
                state_it->second != MetadataProbePathState::Queued) {
                continue;
            }
            state_it->second = MetadataProbePathState::InFlight;
        }

        MetadataProbeCompletion completion;
        completion.generation = job.generation;
        completion.path = job.path;
        completion.file_identity = metadata_file_identity(job.path);

        const auto cache_started = std::chrono::steady_clock::now();
        if (!completion.file_identity.empty()) {
            std::lock_guard<std::mutex> lock(metadata_worker_mutex_);
            const auto cached = media_probe_cache_.find(job.path);
            if (cached != media_probe_cache_.end() &&
                cached->second.file_identity == completion.file_identity) {
                completion.result = cached->second.result;
                completion.result.probe_backend = "cache/" +
                    (cached->second.result.probe_backend.empty()
                         ? std::string("unknown")
                         : cached->second.result.probe_backend);
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - cache_started);
                completion.result.probe_elapsed_microseconds = elapsed.count() > 0
                    ? static_cast<std::uint64_t>(elapsed.count())
                    : 0;
                cached->second.last_used_serial = ++media_probe_cache_serial_;
                completion.cache_hit = true;
            }
        }

        if (!completion.cache_hit) {
            const std::string identity_before = completion.file_identity;
            completion.result = probe_media_file(job.path, probe_cancellation);
            const std::string identity_after = metadata_file_identity(job.path);
            if (identity_before.empty() || identity_after != identity_before) {
                completion.file_identity.clear();
            }
        }
        log_metadata_probe_timing(job.path, completion.result);

        {
            std::lock_guard<std::mutex> lock(metadata_worker_mutex_);
            if (metadata_worker_stop_) {
                return;
            }
            metadata_completions_.push_back(std::move(completion));
            metadata_completion_pending_.store(true, std::memory_order_release);
        }
        schedule_metadata_completion_dispatch();
    }
}

void GtkPlayerWindow::enqueue_metadata_probe(const std::string& path, bool move_to_front) {
    if (path.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(metadata_worker_mutex_);
    const auto state_it = metadata_probe_path_states_.find(path);
    if (state_it != metadata_probe_path_states_.end()) {
        if (state_it->second == MetadataProbePathState::InFlight ||
            state_it->second == MetadataProbePathState::Completed) {
            return;
        }
        if (move_to_front) {
            for (auto it = metadata_jobs_.begin(); it != metadata_jobs_.end();) {
                if (it->generation == metadata_generation_ && it->path == path) {
                    it = metadata_jobs_.erase(it);
                } else {
                    ++it;
                }
            }
            metadata_jobs_.push_front(MetadataProbeJob{metadata_generation_, path});
            metadata_worker_cv_.notify_all();
        }
        return;
    }

    metadata_probe_path_states_[path] = MetadataProbePathState::Queued;
    if (move_to_front) {
        metadata_jobs_.push_front(MetadataProbeJob{metadata_generation_, path});
    } else {
        metadata_jobs_.push_back(MetadataProbeJob{metadata_generation_, path});
    }
    metadata_worker_cv_.notify_all();
}

void GtkPlayerWindow::enqueue_initial_metadata_probes(const std::vector<std::string>& paths) {
    std::string priority_path;
    if (pending_last_active_track_restore_ &&
        pending_last_active_track_restore_generation_ == metadata_generation_ &&
        saved_last_active_track_.valid) {
        priority_path = saved_last_active_track_.audio_file_path;
    } else if (!playlist_.empty()) {
        const std::size_t index = std::min(current_track_index_, playlist_.size() - 1);
        priority_path = playlist_[index].audio_file_path;
    }
    const std::vector<std::string> ordered_paths = prioritize_probe_path(paths, priority_path);
    for (const std::string& path : ordered_paths) {
        enqueue_metadata_probe(path, false);
    }
}

void GtkPlayerWindow::apply_metadata_probe_result(std::uint64_t generation,
                                                  const std::string& path,
                                                  const MediaProbeResult& result) {
    const auto path_entries = metadata_entry_ids_by_path_.find(path);
    if (path_entries == metadata_entry_ids_by_path_.end()) {
        return;
    }

    const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
    const std::size_t mpris_index = mpris_playlist_index(transport.playing);
    bool mpris_metadata_changed = false;
    for (const std::uint64_t entry_id : path_entries->second) {
        const std::optional<std::size_t> indexed =
            playlist_index_for_entry_id(entry_id);
        if (!indexed.has_value()) {
            continue;
        }
        const std::size_t index = *indexed;
        PlaylistEntry& entry = playlist_[index];
        if (entry.load_generation != generation ||
            entry.metadata_state != MetadataState::Pending) {
            continue;
        }

        if (!result.success || result.format.sample_rate == 0 || result.format.channels == 0) {
            entry.metadata_state = MetadataState::Failed;
            update_playlist_row(index);
            mpris_metadata_changed =
                mpris_metadata_changed || index == mpris_index;
            continue;
        }

        entry.source_sample_rate = result.format.sample_rate;
        entry.source_bits_per_sample = result.format.bits_per_sample;
        entry.source_supports_trusted_decoder_eof =
            result.source_supports_trusted_decoder_eof;
        entry.source_exact_presentation_drain_policy =
            result.sample_extent_drain_policy;
        entry.source_presentation_start_known =
            result.source_presentation_start_known;
        entry.source_presentation_start_sample =
            result.source_presentation_start_sample;
        entry.dsd_source = result.dsd_source;
        entry.dsd_sample_rate = result.dsd_source ? result.dsd_sample_rate : 0;
        entry.decoded_format = result.format;
        entry.codec_name = result.codec_name;
        entry.source_bit_rate = result.bit_rate;
        entry.source_sample_extent_kind = result.sample_extent_kind;
        entry.source_sample_extent_source = result.sample_extent_source;
        entry.sample_extent_drain_policy = result.sample_extent_drain_policy;
        entry.lossless_source = result.lossless;
        entry.native_source_available = result.native_decode;

        if (entry.cue_track) {
            const std::uint64_t source_album_end = result.total_samples_per_channel;
            const bool exact_source_duration =
                sample_extent_supports_bounded_transport(result.sample_extent_kind) &&
                source_album_end > 0;
            entry.source_start_sample = CueParser::frame75_to_samples(
                entry.cue_start_frame_75, entry.source_sample_rate);
            entry.source_end_sample = entry.cue_has_end_frame_75
                ? CueParser::frame75_to_samples(entry.cue_end_frame_75,
                                                entry.source_sample_rate)
                : source_album_end;
            if (exact_source_duration) {
                entry.source_start_sample = std::min(entry.source_start_sample,
                                                     source_album_end);
                entry.source_end_sample = std::min(entry.source_end_sample,
                                                   source_album_end);
            }
            if (entry.source_end_sample < entry.source_start_sample) {
                entry.source_end_sample = entry.source_start_sample;
            }
            entry.source_cue_album_end_sample = source_album_end;
            if (entry.album.empty() && !result.tags.album.empty()) {
                entry.album = safe_utf8_for_display(result.tags.album);
            }
        } else {
            entry.source_start_sample = 0;
            entry.source_end_sample = result.total_samples_per_channel;
            entry.source_cue_album_end_sample = 0;
            if (result.tags.track_number > 0) {
                entry.track_number = result.tags.track_number;
            }
            if (!result.tags.title.empty()) {
                entry.title = safe_utf8_for_display(result.tags.title);
            }
            entry.performer = safe_utf8_for_display(result.tags.artist);
            entry.album = safe_utf8_for_display(result.tags.album);
        }

        refresh_entry_processing_metadata(entry);
        entry.metadata_state = MetadataState::Ready;
        update_playlist_row(index);
        mpris_metadata_changed =
            mpris_metadata_changed || index == mpris_index;
    }
    if (mpris_metadata_changed) {
        mark_mpris_track_changed();
    }
}

bool GtkPlayerWindow::complete_metadata_probe_path(std::uint64_t generation,
                                                   const std::string& path,
                                                   const std::string& file_identity,
                                                   const MediaProbeResult& result,
                                                   bool cache_hit) {
    if (generation != metadata_generation_) {
        return false;
    }

    const bool valid = result.success &&
                       result.format.sample_rate > 0 &&
                       result.format.channels > 0;
    {
        std::lock_guard<std::mutex> lock(metadata_worker_mutex_);
        const auto state_it = metadata_probe_path_states_.find(path);
        if (state_it != metadata_probe_path_states_.end() &&
            state_it->second == MetadataProbePathState::Completed) {
            return false;
        }
        if (state_it != metadata_probe_path_states_.end() &&
            state_it->second == MetadataProbePathState::Queued) {
            for (auto it = metadata_jobs_.begin(); it != metadata_jobs_.end();) {
                if (it->generation == metadata_generation_ && it->path == path) {
                    it = metadata_jobs_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        metadata_probe_path_states_[path] = MetadataProbePathState::Completed;

        if (valid && !cache_hit && !file_identity.empty()) {
            CachedMediaProbe cached;
            cached.file_identity = file_identity;
            cached.result = result;
            cached.last_used_serial = ++media_probe_cache_serial_;
            media_probe_cache_[path] = std::move(cached);

            while (media_probe_cache_.size() > kMediaProbeCacheMaxEntries) {
                const auto oldest = std::min_element(
                    media_probe_cache_.begin(),
                    media_probe_cache_.end(),
                    [](const auto& left, const auto& right) {
                        return left.second.last_used_serial < right.second.last_used_serial;
                    });
                if (oldest == media_probe_cache_.end()) {
                    break;
                }
                media_probe_cache_.erase(oldest);
            }
        }
    }

    apply_metadata_probe_result(generation, path, result);

    if (playlist_loading_) {
        ++metadata_completed_files_;
        if (!valid) {
            ++metadata_failed_files_;
            Logger::instance().debug("Metadata probe failed: " + path +
                                     (result.error.empty()
                                          ? std::string()
                                          : std::string(" (") + result.error + ")"));
        }
    }

    const bool final_completion = playlist_loading_ &&
                                  metadata_completed_files_ >= metadata_total_files_;
    if (!final_completion) {
        try_start_pending_metadata_play(path);
    }
    return true;
}

bool GtkPlayerWindow::prepare_track_for_playback(std::size_t index) {
    if (index >= playlist_.size()) {
        return false;
    }

    const PlaylistEntry& entry = playlist_[index];
    if (entry.metadata_state == MetadataState::Ready) {
        return true;
    }
    if (entry.metadata_state == MetadataState::Failed) {
        return false;
    }

    prioritize_metadata_probe(entry.audio_file_path);
    return false;
}

void GtkPlayerWindow::prioritize_metadata_probe(const std::string& path) {
    if (path.empty()) {
        return;
    }

    std::uint64_t entry_generation = metadata_generation_;
    bool has_pending_entry = false;
    const auto path_entries = metadata_entry_ids_by_path_.find(path);
    if (path_entries != metadata_entry_ids_by_path_.end()) {
        bool generation_found = false;
        for (const std::uint64_t entry_id : path_entries->second) {
            const std::optional<std::size_t> indexed =
                playlist_index_for_entry_id(entry_id);
            if (!indexed.has_value()) {
                continue;
            }
            const PlaylistEntry& entry = playlist_[*indexed];
            if (!generation_found) {
                entry_generation = entry.load_generation;
                generation_found = true;
            }
            has_pending_entry = has_pending_entry ||
                (entry.load_generation == metadata_generation_ &&
                 entry.metadata_state == MetadataState::Pending);
        }
    }

    enum class PriorityAction {
        None,
        ApplyCompleted,
        TryPendingWithoutEntry,
    };
    PriorityAction action = PriorityAction::None;
    MediaProbeResult completed_result;

    {
        std::lock_guard<std::mutex> lock(metadata_worker_mutex_);
        const auto state_it = metadata_probe_path_states_.find(path);
        if (state_it != metadata_probe_path_states_.end()) {
            if (state_it->second == MetadataProbePathState::Completed) {
                const auto cached_result = media_probe_cache_.find(path);
                if (cached_result != media_probe_cache_.end()) {
                    completed_result = cached_result->second.result;
                    action = PriorityAction::ApplyCompleted;
                } else {
                    action = PriorityAction::TryPendingWithoutEntry;
                }
            } else if (state_it->second == MetadataProbePathState::InFlight) {
                return;
            } else if (state_it->second == MetadataProbePathState::Queued) {
                for (auto it = metadata_jobs_.begin(); it != metadata_jobs_.end();) {
                    if (it->generation == metadata_generation_ && it->path == path) {
                        it = metadata_jobs_.erase(it);
                    } else {
                        ++it;
                    }
                }
                metadata_jobs_.push_front(MetadataProbeJob{metadata_generation_, path});
                metadata_worker_cv_.notify_all();
                return;
            }
        }
    }

    if (action == PriorityAction::ApplyCompleted) {
        apply_metadata_probe_result(entry_generation, path, completed_result);
        try_start_pending_metadata_play(path);
        return;
    }
    if (action == PriorityAction::TryPendingWithoutEntry) {
        try_start_pending_metadata_play(path);
        return;
    }

    if (!has_pending_entry) {
        try_start_pending_metadata_play(path);
        return;
    }

    enqueue_metadata_probe(path, true);
}

void GtkPlayerWindow::set_pending_metadata_playback(std::size_t index,
                                                    std::uint64_t offset_samples,
                                                    bool start_playback,
                                                    bool preserve_paused,
                                                    bool update_mpris_track,
                                                    bool preserve_explicit_selection,
                                                    PlaybackStartReason start_reason) {
    if (index >= playlist_.size()) {
        return;
    }

    halt_active_transport(false);

    pending_metadata_playback_.active = true;
    pending_metadata_playback_.generation = metadata_generation_;
    pending_metadata_playback_.index = index;
    pending_metadata_playback_.offset_samples = offset_samples;
    pending_metadata_playback_.start_playback = start_playback;
    pending_metadata_playback_.preserve_paused = preserve_paused;
    pending_metadata_playback_.update_mpris_track = update_mpris_track;
    pending_metadata_playback_.preserve_explicit_selection = preserve_explicit_selection;
    pending_metadata_playback_.start_reason = start_reason;
    pending_metadata_playback_.waiting_path = playlist_[index].audio_file_path;
    current_track_index_ = index;
    sync_playlist_selection_after_transport_change(index, preserve_explicit_selection);
    prioritize_metadata_probe(pending_metadata_playback_.waiting_path);
    refresh_display(true, true);
    notify_mpris_state_changed();
}

void GtkPlayerWindow::clear_pending_metadata_play() {
    pending_metadata_playback_ = PendingMetadataPlayback{};
}

bool GtkPlayerWindow::pending_metadata_playback_valid() const {
    if (!pending_metadata_playback_.active ||
        pending_metadata_playback_.generation != metadata_generation_ ||
        ui_closing_) {
        return false;
    }
    const std::size_t index = pending_metadata_playback_.index;
    if (index >= playlist_.size()) {
        return false;
    }
    const PlaylistEntry& entry = playlist_[index];
    return entry.load_generation == pending_metadata_playback_.generation &&
           entry.audio_file_path == pending_metadata_playback_.waiting_path;
}

bool GtkPlayerWindow::advance_pending_metadata_playback(int direction) {
    if (!pending_metadata_playback_valid()) {
        return false;
    }

    std::size_t target_index = pending_metadata_playback_.index;
    PlaybackStartReason target_reason = pending_metadata_playback_.start_reason;
    if (random_enabled_) {
        initialize_random_pass_if_needed();
        if (direction > 0) {
            const std::uint64_t pending_id = playlist_entry_id(target_index);
            const auto found = std::find(random_remaining_entry_ids_.begin(),
                                         random_remaining_entry_ids_.end(),
                                         pending_id);
            if (found != random_remaining_entry_ids_.end() &&
                random_remaining_entry_ids_.size() > 1) {
                const std::uint64_t skipped = *found;
                random_remaining_entry_ids_.erase(found);
                random_remaining_entry_ids_.push_back(skipped);
            }
            if (!random_next_track(&target_index, &target_reason)) {
                return false;
            }
        } else if (!random_previous_track(&target_index)) {
            return false;
        } else {
            target_reason = PlaybackStartReason::HistoryNavigation;
        }
        set_pending_metadata_playback(target_index,
                                      0,
                                      pending_metadata_playback_.start_playback,
                                      pending_metadata_playback_.preserve_paused,
                                      pending_metadata_playback_.update_mpris_track,
                                      false,
                                      target_reason);
        return true;
    }

    if (direction > 0) {
        if (target_index + 1 < playlist_.size()) {
            target_index += 1;
        } else if (repeat_enabled_) {
            target_index = 0;
        } else {
            return false;
        }
    } else if (target_index > 0) {
        target_index -= 1;
    } else {
        return false;
    }

    set_pending_metadata_playback(target_index,
                                   0,
                                   pending_metadata_playback_.start_playback,
                                   pending_metadata_playback_.preserve_paused,
                                   pending_metadata_playback_.update_mpris_track,
                                   false,
                                   target_reason);
    return true;
}

void GtkPlayerWindow::try_start_pending_metadata_play(const std::string& path) {
    if (ui_closing_ || !pending_metadata_playback_valid() ||
        pending_metadata_playback_.waiting_path != path) {
        return;
    }

    const std::size_t index = pending_metadata_playback_.index;
    const MetadataState state = playlist_[index].metadata_state;
    if (state == MetadataState::Failed) {
        clear_pending_metadata_play();
        refresh_display();
        notify_mpris_state_changed();
        return;
    }
    if (state != MetadataState::Ready) {
        return;
    }

    const PendingMetadataPlayback pending = pending_metadata_playback_;
    clear_pending_metadata_play();
    play_track_index_at_offset(pending.index,
                               pending.offset_samples,
                               pending.start_playback,
                               pending.preserve_paused,
                               pending.update_mpris_track,
                               pending.preserve_explicit_selection,
                               pending.start_reason);
}

bool GtkPlayerWindow::current_track_metadata_ready() const {
    if (playlist_.empty() || current_track_index_ >= playlist_.size()) {
        return false;
    }
    return playlist_[current_track_index_].metadata_state == MetadataState::Ready;
}

bool GtkPlayerWindow::metadata_loading_progress_visible(bool transport_playing) const {
    if (!playlist_loading_ || pending_metadata_playback_valid()) {
        return false;
    }
    return !transport_playing;
}

void GtkPlayerWindow::maybe_finish_metadata_load_session() {
    if (!playlist_loading_ || metadata_completed_files_ < metadata_total_files_) {
        return;
    }
    finish_metadata_load_session();
}

void GtkPlayerWindow::drain_metadata_probe_results() {
    if (!metadata_completion_pending_.load(std::memory_order_acquire)) {
        return;
    }
    std::deque<MetadataProbeCompletion> completions;
    {
        std::lock_guard<std::mutex> lock(metadata_worker_mutex_);
        if (metadata_completions_.empty()) {
            metadata_completion_pending_.store(false, std::memory_order_release);
            return;
        }
        completions.swap(metadata_completions_);
        metadata_completion_pending_.store(false, std::memory_order_release);
    }

    bool changed = false;
    bool loading_progress_changed = false;
    for (MetadataProbeCompletion& completion : completions) {
        if (completion.generation != metadata_generation_) {
            continue;
        }

        bool should_apply = false;
        {
            std::lock_guard<std::mutex> lock(metadata_worker_mutex_);
            const auto state_it = metadata_probe_path_states_.find(completion.path);
            if (state_it == metadata_probe_path_states_.end() ||
                state_it->second != MetadataProbePathState::InFlight) {
                continue;
            }
            should_apply = true;
        }
        if (!should_apply) {
            continue;
        }

        if (!complete_metadata_probe_path(completion.generation,
                                          completion.path,
                                          completion.file_identity,
                                          completion.result,
                                          completion.cache_hit)) {
            continue;
        }

        changed = true;
        if (playlist_loading_) {
            loading_progress_changed = true;
        }
    }
    if (playlist_loading_) {
        const auto now = std::chrono::steady_clock::now();
        const bool display_refresh_due =
            metadata_display_last_refresh_ == std::chrono::steady_clock::time_point{} ||
            now - metadata_display_last_refresh_ >=
                std::chrono::milliseconds(kMetadataDisplayRefreshIntervalMs);
        if (loading_progress_changed &&
            (display_refresh_due ||
             metadata_completed_files_ % kMetadataDisplayRefreshStride == 0 ||
             metadata_completed_files_ >= metadata_total_files_)) {
            metadata_display_last_refresh_ = now;
            refresh_display();
            update_loading_controls();
        }
        maybe_finish_metadata_load_session();
    } else if (changed) {
        update_loading_controls();
        refresh_display();
    }
}

void GtkPlayerWindow::finish_metadata_load_session() {
    if (!playlist_loading_) {
        return;
    }

    const std::vector<std::string> requested_sources = metadata_load_requested_sources_;
    const bool replace_playlist = metadata_load_replace_playlist_;
    const bool quiet = metadata_load_quiet_;
    const std::string play_after_path = play_after_metadata_path_;
    const std::uint64_t play_after_generation = play_after_metadata_generation_;

    playlist_loading_ = false;
    {
        std::lock_guard<std::mutex> lock(metadata_worker_mutex_);
        metadata_probe_path_states_.clear();
    }
    play_after_metadata_path_.clear();
    play_after_metadata_generation_ = 0;
    metadata_load_requested_sources_.clear();
    metadata_load_quiet_ = false;
    metadata_load_replace_playlist_ = false;

    std::vector<std::optional<std::size_t>> index_remap;
    index_remap.reserve(playlist_.size());
    std::size_t next_index = 0;
    bool has_failed_entries = false;
    for (const PlaylistEntry& entry : playlist_) {
        if (entry.metadata_state == MetadataState::Failed) {
            index_remap.push_back(std::nullopt);
            has_failed_entries = true;
        } else {
            index_remap.push_back(next_index++);
        }
    }

    if (has_failed_entries) {
        const bool was_gapless = gapless_chain_active_;
        const std::size_t old_current_track_index = current_track_index_;
        const bool current_track_removed =
            old_current_track_index < index_remap.size() &&
            !index_remap[old_current_track_index].has_value();
        const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
        const bool engine_active = transport.playing || transport.paused;

        playlist_.erase(std::remove_if(playlist_.begin(), playlist_.end(), [](const PlaylistEntry& entry) {
            return entry.metadata_state == MetadataState::Failed;
        }), playlist_.end());
        rebuild_playlist_entry_indexes();

        remap_playlist_indices_after_failed_removal(index_remap);

        if (engine_active && (current_track_removed || (was_gapless && !gapless_chain_active_))) {
            halt_active_transport(false);
            if (!playlist_.empty()) {
                current_track_index_ = std::min(current_track_index_, playlist_.size() - 1);
            }
        }
        synchronize_random_remaining_with_playlist();
    }

    resolve_pending_last_active_track_restore();

    std::vector<std::string> successful_sources;
    for (const std::string& source : requested_sources) {
        const bool has_ready_entry = std::any_of(
            playlist_.begin(), playlist_.end(), [this, &source](const PlaylistEntry& entry) {
                return entry.metadata_state == MetadataState::Ready &&
                       entry.load_generation == metadata_generation_ &&
                       entry.top_level_source_path == source;
            });
        if (has_ready_entry) {
            successful_sources.push_back(source);
        }
    }

    if (replace_playlist) {
        current_loaded_source_paths_ = successful_sources;
        current_loaded_sources_initialized_ = !successful_sources.empty();
    } else if (!successful_sources.empty()) {
        current_loaded_source_paths_.insert(current_loaded_source_paths_.end(),
                                            successful_sources.begin(),
                                            successful_sources.end());
        current_loaded_sources_initialized_ = true;
    }

    if (metadata_failed_files_ > 0) {
        const std::string message = std::to_string(metadata_failed_files_) +
                                    (metadata_failed_files_ == 1
                                         ? " file could not be loaded"
                                         : " files could not be loaded");
        if (quiet) {
            Logger::instance().debug(message);
        } else {
            Logger::instance().error(message);
        }
    }

    refresh_playlist_processing_metadata();
    update_loading_controls();
    finalize_loaded_playlist(metadata_failed_files_ > 0);
    apply_last_active_track_restore_centering();
    if (random_enabled_) {
        initialize_random_pass_if_needed();
    } else {
        synchronize_random_remaining_with_playlist();
    }

    if (pending_metadata_playback_valid()) {
        const std::string pending_path = pending_metadata_playback_.waiting_path;
        try_start_pending_metadata_play(pending_path);
        return;
    }

    if (!play_after_path.empty()) {
        for (std::size_t index = 0; index < playlist_.size(); ++index) {
            if (playlist_[index].load_generation == play_after_generation &&
                (playlist_[index].audio_file_path == play_after_path ||
                 playlist_[index].top_level_source_path == play_after_path) &&
                playlist_[index].metadata_state == MetadataState::Ready) {
                play_track_index(index);
                break;
            }
        }
    }
}

void GtkPlayerWindow::update_loading_controls() {
    const bool can_play = playback_available();
    if (btn_play_ != nullptr) gtk_widget_set_sensitive(btn_play_, can_play);
    if (btn_pause_ != nullptr) gtk_widget_set_sensitive(btn_pause_, can_play);
    if (btn_stop_ != nullptr) gtk_widget_set_sensitive(btn_stop_, can_play);
    if (btn_prev_ != nullptr) gtk_widget_set_sensitive(btn_prev_, can_play);
    if (btn_next_ != nullptr) gtk_widget_set_sensitive(btn_next_, can_play);
    if (playlist_view_ != nullptr) gtk_widget_set_sensitive(playlist_view_, TRUE);
    if (progress_bar_ != nullptr) gtk_widget_set_sensitive(progress_bar_, current_track_metadata_ready());
}

bool GtkPlayerWindow::playback_available() const {
    return !playlist_.empty();
}

std::vector<std::string> GtkPlayerWindow::load_source_paths(const std::vector<std::string>& paths,
                                                            bool replace_playlist,
                                                            bool quiet,
                                                            bool record_last_sources,
                                                            const std::string& play_after_load_path,
                                                            bool restore_saved_sources) {
    cancel_source_scan();
    std::vector<ScannedSourcePath> resolved_paths;
    resolved_paths.reserve(paths.size());
    for (const std::string& path : paths) {
        resolved_paths.push_back(ScannedSourcePath{path, path});
    }
    return load_resolved_source_paths(resolved_paths,
                                      replace_playlist,
                                      quiet,
                                      record_last_sources,
                                      play_after_load_path,
                                      restore_saved_sources);
}

std::vector<std::string> GtkPlayerWindow::load_resolved_source_paths(
    const std::vector<ScannedSourcePath>& paths,
    bool replace_playlist,
    bool quiet,
    bool record_last_sources,
    const std::string& play_after_load_path,
    bool restore_saved_sources) {
    std::vector<std::string> accepted_sources;
    if (paths.empty() || ui_closing_) {
        return accepted_sources;
    }

    if (playlist_loading_ && !replace_playlist) {
        Logger::instance().debug("Ignoring appended source while metadata loading is active");
        return accepted_sources;
    }

    if (record_last_sources && restore_sources_idle_id_ != 0) {
        g_source_remove(restore_sources_idle_id_);
        restore_sources_idle_id_ = 0;
    }

    stop_playback();
    clear_pending_metadata_play();
    {
        std::lock_guard<std::mutex> lock(metadata_worker_mutex_);
        ++metadata_generation_;
        metadata_jobs_.clear();
        metadata_completions_.clear();
        metadata_completion_pending_.store(false, std::memory_order_release);
        metadata_probe_path_states_.clear();
    }
    if (!metadata_probe_cancellations_.empty()) {
        for (const std::unique_ptr<ProbeCancellation>& probe_cancellation : metadata_probe_cancellations_) {
            if (probe_cancellation) {
                probe_cancellation->cancel();
            }
        }
    }

    prepare_last_active_track_restore(restore_saved_sources, replace_playlist);

    if (replace_playlist) {
        current_loaded_sources_initialized_ = false;
        reset_random_transport_state(true);
        playlist_.clear();
        clear_playlist_entry_indexes();
        cue_cache_.clear();
        reset_playlist_sort_state();
        current_track_index_ = 0;
        reset_playlist_selection_state();
    }

    std::vector<std::string> probe_paths;
    std::unordered_set<std::string> accepted_source_set;
    for (const ScannedSourcePath& source : paths) {
        const std::string& path = source.path;
        const std::string top_level_source = source.top_level_source_path.empty()
            ? path
            : source.top_level_source_path;
        if (path.empty()) {
            continue;
        }
        if (try_load_stream_source(path, &accepted_sources, quiet, top_level_source)) {
            continue;
        }
        if (!g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR)) {
            const std::string message = "Source is unavailable: " + path;
            if (quiet) Logger::instance().debug(message);
            else Logger::instance().error(message);
            continue;
        }

        const std::size_t before = playlist_.size();
        try {
            append_source_placeholders(path, top_level_source, &probe_paths);
        } catch (const std::exception& ex) {
            const std::string message = std::string("Cannot load source: ") + path +
                                        " (" + ex.what() + ")";
            if (quiet) Logger::instance().debug(message);
            else Logger::instance().error(message);
        }
        if (playlist_.size() > before && accepted_source_set.insert(top_level_source).second) {
            accepted_sources.push_back(top_level_source);
        }
    }

    if (accepted_sources.empty()) {
        playlist_loading_ = false;
        play_after_metadata_path_.clear();
        play_after_metadata_generation_ = 0;
        metadata_load_requested_sources_.clear();
        metadata_total_files_ = 0;
        metadata_completed_files_ = 0;
        metadata_failed_files_ = 0;
        if (replace_playlist) {
            current_loaded_source_paths_.clear();
            current_loaded_sources_initialized_ = false;
        }
        resolve_pending_last_active_track_restore();
        update_loading_controls();
        finalize_loaded_playlist();
        apply_last_active_track_restore_centering();
        return accepted_sources;
    }

    std::vector<std::string> unique_probe_paths;
    std::unordered_set<std::string> seen_paths;
    for (const std::string& path : probe_paths) {
        if (seen_paths.insert(path).second) {
            unique_probe_paths.push_back(path);
        }
    }

    playlist_loading_ = true;
    metadata_total_files_ = unique_probe_paths.size();
    metadata_completed_files_ = 0;
    metadata_failed_files_ = 0;
    metadata_display_last_refresh_ = std::chrono::steady_clock::time_point{};
    metadata_load_quiet_ = quiet;
    metadata_load_replace_playlist_ = replace_playlist;
    metadata_load_requested_sources_ = accepted_sources;
    play_after_metadata_path_ = play_after_load_path;
    play_after_metadata_generation_ = play_after_load_path.empty()
        ? 0
        : metadata_generation_;

    if (!replace_playlist &&
        playlist_sort_direction_ != PlaylistSortDirection::Original &&
        playlist_sort_key_ != PlaylistSortKey::None) {
        apply_playlist_sort(playlist_sort_key_, playlist_sort_direction_, false);
    } else {
        rebuild_playlist_view();
    }
    if (!playlist_.empty()) {
        current_track_index_ = std::min(current_track_index_, playlist_.size() - 1);
        sync_playlist_selection_after_transport_change(current_track_index_, true);
    }
    update_loading_controls();
    refresh_display();
    notify_mpris_state_changed();

    if (metadata_total_files_ == 0) {
        maybe_finish_metadata_load_session();
    } else {
        enqueue_initial_metadata_probes(unique_probe_paths);
    }
    return accepted_sources;
}

void GtkPlayerWindow::finalize_loaded_playlist(bool rebuild_view) {
    rebuild_playlist_entry_indexes();
    if (playlist_.empty()) {
        current_track_index_ = 0;
    } else if (current_track_index_ >= playlist_.size()) {
        current_track_index_ = 0;
    }

    if (rebuild_view) {
        rebuild_playlist_view();
    } else if (playlist_field_width_limit_enabled_) {
        apply_playlist_field_width_limit(true);
    }
    if (!playlist_.empty()) {
        sync_playlist_selection_after_transport_change(current_track_index_, true);
    } else {
        reset_playlist_selection_state();
    }
    track_switch_in_progress_ = false;
    finish_handled_ = true;
    update_loading_controls();
    refresh_display();
    invalidate_mpris_cover_cache();
    mark_mpris_track_changed();
}

void GtkPlayerWindow::schedule_last_sources_restore() {
    if (!restore_last_sources_enabled_ || last_opened_sources_.empty() || restore_sources_idle_id_ != 0) {
        return;
    }
    restore_sources_idle_id_ = g_idle_add(GtkPlayerWindow::on_restore_last_sources_idle, this);
}

gboolean GtkPlayerWindow::on_restore_last_sources_idle(gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr) {
        return G_SOURCE_REMOVE;
    }
    self->restore_sources_idle_id_ = 0;
    if (self->ui_closing_ || self->window_ == nullptr || !self->restore_last_sources_enabled_) {
        return G_SOURCE_REMOVE;
    }

    const std::vector<std::string> saved_sources = self->last_opened_sources_;
    if (!self->open_source_paths(saved_sources, true, true, false, std::string(), true)) {
        Logger::instance().debug("No saved source could be scheduled for restoration");
    } else {
        Logger::instance().info("Scheduled saved sources for restoration: " +
                                std::to_string(saved_sources.size()));
    }
    return G_SOURCE_REMOVE;
}

void GtkPlayerWindow::remember_last_active_track(std::size_t index) {
    if (index >= playlist_.size()) {
        return;
    }
    const PlaylistEntry& entry = playlist_[index];
    if (entry.metadata_state != MetadataState::Ready ||
        !entry.start_sample_known || entry.audio_file_path.empty()) {
        return;
    }

    runtime_last_active_track_.valid = true;
    runtime_last_active_track_.audio_file_path = entry.audio_file_path;
    runtime_last_active_track_.source_start_sample = entry.source_start_sample;
    runtime_last_active_track_.cue_track = entry.cue_track;
}

void GtkPlayerWindow::cancel_pending_last_active_track_restore() {
    pending_last_active_track_restore_ = false;
    pending_last_active_track_restore_generation_ = 0;
    last_active_track_restore_center_pending_ = false;
}

void GtkPlayerWindow::prepare_last_active_track_restore(bool restore_saved_sources,
                                                        bool replace_playlist) {
    if (!replace_playlist) {
        return;
    }

    cancel_pending_last_active_track_restore();
    if (restore_saved_sources && restore_last_sources_enabled_ &&
        restore_last_active_track_enabled_ && saved_last_active_track_.valid) {
        pending_last_active_track_restore_ = true;
        pending_last_active_track_restore_generation_ = metadata_generation_;
        runtime_last_active_track_ = saved_last_active_track_;
        return;
    }

    if (!restore_saved_sources) {
        runtime_last_active_track_ = LastActiveTrackLocator{};
    }
}

bool GtkPlayerWindow::last_active_track_locator_matches(const PlaylistEntry& entry) const {
    return saved_last_active_track_.valid &&
           entry.metadata_state == MetadataState::Ready &&
           entry.start_sample_known &&
           entry.audio_file_path == saved_last_active_track_.audio_file_path &&
           entry.cue_track == saved_last_active_track_.cue_track &&
           entry.source_start_sample == saved_last_active_track_.source_start_sample;
}

void GtkPlayerWindow::resolve_pending_last_active_track_restore() {
    if (!pending_last_active_track_restore_ ||
        pending_last_active_track_restore_generation_ != metadata_generation_) {
        return;
    }

    for (std::size_t index = 0; index < playlist_.size(); ++index) {
        if (!last_active_track_locator_matches(playlist_[index])) {
            continue;
        }

        current_track_index_ = index;
        reset_playlist_selection_state(index);
        runtime_last_active_track_ = saved_last_active_track_;
        pending_last_active_track_restore_ = false;
        pending_last_active_track_restore_generation_ = 0;
        last_active_track_restore_center_pending_ = true;
        last_active_track_restore_center_index_ = index;
        Logger::instance().info("Restored last active track selection");
        return;
    }

    pending_last_active_track_restore_ = false;
    pending_last_active_track_restore_generation_ = 0;
    saved_last_active_track_ = LastActiveTrackLocator{};
    runtime_last_active_track_ = LastActiveTrackLocator{};
    if (!playlist_.empty()) {
        current_track_index_ = 0;
        reset_playlist_selection_state(0);
    }
    Logger::instance().debug("Saved active track is unavailable; using the first playlist entry");
}

void GtkPlayerWindow::apply_last_active_track_restore_centering() {
    if (!last_active_track_restore_center_pending_) {
        return;
    }
    last_active_track_restore_center_pending_ = false;
    if (playlist_.empty()) {
        return;
    }

    const std::size_t index = std::min(last_active_track_restore_center_index_,
                                       playlist_.size() - 1);
    current_track_index_ = index;
    reset_playlist_selection_state(index);
    select_playlist_row(index, PlaylistScrollPolicy::Center);
}

void GtkPlayerWindow::commit_recovery_checkpoint() {
    saved_last_open_directory_ = last_open_directory_;

    if (!restore_last_sources_enabled_) {
        last_opened_sources_.clear();
        saved_last_active_track_ = LastActiveTrackLocator{};
        return;
    }

    if (current_loaded_sources_initialized_) {
        last_opened_sources_ = current_loaded_source_paths_;
        if (restore_last_active_track_enabled_) {
            saved_last_active_track_ = runtime_last_active_track_.valid
                ? runtime_last_active_track_
                : LastActiveTrackLocator{};
        } else {
            saved_last_active_track_ = LastActiveTrackLocator{};
        }
        return;
    }

    if (!restore_last_active_track_enabled_) {
        saved_last_active_track_ = LastActiveTrackLocator{};
    }
}

void GtkPlayerWindow::start_current_track(bool restart_if_paused) {
    if (!playback_available() || playlist_.empty()) {
        return;
    }

    std::size_t target_index = current_track_index_;
    if (playlist_search_enabled_) {
        target_index = std::min(playlist_play_target_index(), playlist_.size() - 1);
    } else {
        update_playlist_selection_from_ui();
        target_index = current_track_index_;
    }

    if (engine_.is_paused() && restart_if_paused && target_index == current_track_index_) {
        resume_playback();
        return;
    }

    const bool explicit_target =
        playlist_selection_mode_ == PlaylistSelectionMode::ExplicitUser ||
        (playlist_selection_mode_ == PlaylistSelectionMode::FilterCandidate &&
         playlist_filter_candidate_valid_);
    const bool stopped_random_preview =
        random_enabled_ && !engine_.is_playing() &&
        random_stopped_preview_entry_id_.has_value() &&
        playlist_entry_id(target_index) == *random_stopped_preview_entry_id_;
    if (random_enabled_ && !engine_.is_playing() && !explicit_target &&
        !stopped_random_preview) {
        initialize_random_pass_if_needed();
        const bool history_at_end =
            random_history_position_ == std::numeric_limits<std::size_t>::max() ||
            random_history_position_ + 1 >= random_history_entry_ids_.size();
        if (random_remaining_entry_ids_.empty() && history_at_end &&
            !random_visited_entry_ids_.empty()) {
            begin_new_random_pass(playlist_entry_id(current_track_index_));
        }
        if (played_entry_ids_.empty() ||
            (random_remaining_entry_ids_.size() == playlist_.size())) {
            if (!random_remaining_entry_ids_.empty()) {
                const std::optional<std::size_t> random_index =
                    playlist_index_for_entry_id(random_remaining_entry_ids_.front());
                if (random_index.has_value()) {
                    target_index = *random_index;
                    play_track_index(target_index, false, PlaybackStartReason::Automatic);
                    return;
                }
            }
        }
    }

    if (playlist_search_enabled_ &&
        playlist_filter_session_active_ &&
        search_controller_ != nullptr &&
        search_controller_->is_filter_active()) {
        play_filtered_track_index(target_index);
        return;
    }
    play_track_index(target_index,
                     false,
                     stopped_random_preview
                         ? PlaybackStartReason::StoppedRandomPreview
                         : PlaybackStartReason::Manual);
}

void GtkPlayerWindow::halt_active_transport(bool clear_pending_state) {
    if (clear_pending_state) {
        clear_pending_metadata_play();
    }
    cancel_pending_seek();
    if (stream_manager_ != nullptr) {
        stream_manager_->on_playback_stopped();
    }
    stop_stream_service_timer();
    track_switch_in_progress_ = false;
    finish_handled_ = false;
    softvol_dragging_ = false;
    clear_gapless_chain();
    cancel_progress_deadline();
    engine_.stop();
    settle_meter_timer_after_stop();
    active_range_limited_transport_ = false;
    active_gapless_transport_kind_.clear();
    active_output_device_.clear();
    active_track_transport_states_.clear();
    refresh_active_alsa_output_diagnostics();
}

void GtkPlayerWindow::remap_playlist_indices_after_failed_removal(
    const std::vector<std::optional<std::size_t>>& index_remap) {
    const auto remap_index = [&index_remap](std::size_t old_index) -> std::optional<std::size_t> {
        if (old_index >= index_remap.size()) {
            return std::nullopt;
        }
        return index_remap[old_index];
    };

    if (const std::optional<std::size_t> remapped = remap_index(current_track_index_)) {
        current_track_index_ = *remapped;
    } else if (!playlist_.empty()) {
        current_track_index_ = 0;
    }

    if (playlist_filter_session_active_) {
        if (playlist_filter_session_selection_mode_ == PlaylistSelectionMode::ExplicitUser) {
            if (const std::optional<std::size_t> remapped =
                    remap_index(playlist_filter_session_selection_index_)) {
                playlist_filter_session_selection_index_ = *remapped;
            } else {
                playlist_filter_session_selection_mode_ = PlaylistSelectionMode::FollowTransport;
                playlist_filter_session_selection_index_ = current_track_index_;
            }
        } else {
            playlist_filter_session_selection_index_ = current_track_index_;
        }

        if (playlist_filter_session_playback_committed_) {
            if (const std::optional<std::size_t> remapped =
                    remap_index(playlist_filter_session_committed_index_)) {
                playlist_filter_session_committed_index_ = *remapped;
            } else {
                playlist_filter_session_playback_committed_ = false;
                playlist_filter_session_committed_index_ = current_track_index_;
            }
        }
    }

    if (playlist_selection_mode_ == PlaylistSelectionMode::FilterCandidate) {
        if (playlist_filter_candidate_valid_) {
            if (const std::optional<std::size_t> remapped = remap_index(selected_playlist_index_)) {
                selected_playlist_index_ = *remapped;
            } else {
                playlist_filter_candidate_valid_ = false;
            }
        }

        if (playlist_selection_mode_before_filter_candidate_ == PlaylistSelectionMode::ExplicitUser) {
            if (const std::optional<std::size_t> remapped =
                    remap_index(playlist_selection_index_before_filter_candidate_)) {
                playlist_selection_index_before_filter_candidate_ = *remapped;
            } else {
                playlist_selection_mode_before_filter_candidate_ =
                    PlaylistSelectionMode::FollowTransport;
                playlist_selection_index_before_filter_candidate_ = current_track_index_;
            }
        } else {
            playlist_selection_index_before_filter_candidate_ = current_track_index_;
        }

        if (playlist_filter_session_active_) {
            playlist_selection_mode_before_filter_candidate_ =
                playlist_filter_session_selection_mode_;
            playlist_selection_index_before_filter_candidate_ =
                playlist_filter_session_selection_index_;
        }
    } else if (playlist_selection_mode_ == PlaylistSelectionMode::ExplicitUser) {
        if (const std::optional<std::size_t> remapped = remap_index(selected_playlist_index_)) {
            selected_playlist_index_ = *remapped;
        } else {
            playlist_selection_mode_ = PlaylistSelectionMode::FollowTransport;
            selected_playlist_index_ = current_track_index_;
        }
    } else {
        selected_playlist_index_ = current_track_index_;
    }

    if (pending_metadata_playback_.active) {
        if (const std::optional<std::size_t> remapped = remap_index(pending_metadata_playback_.index)) {
            pending_metadata_playback_.index = *remapped;
        } else {
            clear_pending_metadata_play();
        }
    }

    if (pending_seek_source_id_ != 0) {
        if (const std::optional<std::size_t> remapped = remap_index(pending_seek_index_)) {
            pending_seek_index_ = *remapped;
        } else {
            cancel_pending_seek();
        }
    }

    if (playlist_.empty()) {
        current_track_index_ = 0;
        reset_playlist_selection_state();
    }

    if (!gapless_chain_active_) {
        return;
    }

    if (gapless_chain_playlist_indices_.empty() ||
        gapless_chain_playlist_indices_.size() != gapless_chain_offsets_.size()) {
        clear_gapless_chain();
        return;
    }

    std::vector<std::size_t> remapped_chain;
    remapped_chain.reserve(gapless_chain_playlist_indices_.size());
    for (const std::size_t index : gapless_chain_playlist_indices_) {
        if (index >= index_remap.size() || !index_remap[index].has_value()) {
            clear_gapless_chain();
            return;
        }
        remapped_chain.push_back(*index_remap[index]);
    }
    gapless_chain_playlist_indices_ = std::move(remapped_chain);
}

void GtkPlayerWindow::stop_playback() {
    halt_active_transport(true);
    if (!ui_closing_) {
        refresh_display();
        notify_mpris_state_changed();
    }
}

void GtkPlayerWindow::play_track_index(std::size_t index,
                                         bool preserve_explicit_selection,
                                         PlaybackStartReason start_reason) {
    play_track_index_at_offset(index,
                               0,
                               true,
                               false,
                               true,
                               preserve_explicit_selection,
                               start_reason);
}

void GtkPlayerWindow::play_track_index_at_offset(std::size_t index,
                                                 std::uint64_t offset_samples,
                                                 bool start_playback,
                                                 bool preserve_paused,
                                                 bool update_mpris_track,
                                                 bool preserve_explicit_selection,
                                                 PlaybackStartReason start_reason) {
    if (index >= playlist_.size()) {
        return;
    }

    const std::uint64_t requested_entry_id = playlist_entry_id(index);
    const bool current_random_history_entry =
        random_history_position_ != std::numeric_limits<std::size_t>::max() &&
        random_history_position_ < random_history_entry_ids_.size() &&
        random_history_entry_ids_[random_history_position_] == requested_entry_id;
    if (random_enabled_ && start_reason == PlaybackStartReason::Manual &&
        index == current_track_index_ && current_random_history_entry) {
        start_reason = random_stopped_preview_entry_id_.has_value() &&
                *random_stopped_preview_entry_id_ == requested_entry_id
            ? PlaybackStartReason::StoppedRandomPreview
            : PlaybackStartReason::PreserveHistory;
    }

    clear_pending_metadata_play();

    std::uint64_t probe_generation = 0;
    if (begin_play_track_stream_preamble(index,
                                         offset_samples,
                                         start_playback,
                                         preserve_paused,
                                         update_mpris_track,
                                         false,
                                         &probe_generation)) {
        return;
    }

    if (!prepare_track_for_playback(index)) {
        set_pending_metadata_playback(index,
                                      offset_samples,
                                      start_playback,
                                      preserve_paused,
                                      update_mpris_track,
                                      preserve_explicit_selection,
                                      start_reason);
        return;
    }
    if (playlist_[index].metadata_state == MetadataState::Failed) {
        return;
    }
    update_loading_controls();

    track_switch_in_progress_ = true;
    finish_handled_ = true;
    cancel_progress_deadline();
    engine_.stop();
    active_range_limited_transport_ = false;
    active_gapless_transport_kind_.clear();
    active_output_device_.clear();
    active_track_transport_states_.clear();
    refresh_active_alsa_output_diagnostics();
    clear_gapless_chain();

    const std::size_t previous_playing_index = current_track_index_;
    current_track_index_ = index;
    const PlaylistScrollPolicy scroll_policy =
        (random_enabled_ && (start_reason == PlaybackStartReason::Automatic ||
                             start_reason == PlaybackStartReason::HistoryNavigation))
            ? automatic_transport_scroll_policy(index)
            : PlaylistScrollPolicy::EnsureVisible;
    sync_playlist_selection_after_transport_change(index,
                                                   preserve_explicit_selection,
                                                   scroll_policy);
    if (previous_playing_index != current_track_index_) {
        refresh_playlist_row_styles(playlist_view_);
    }
    const PlaylistEntry track = playlist_[current_track_index_];
    const std::uint64_t track_length = track_length_samples(track);
    const std::uint64_t initial_offset = std::min<std::uint64_t>(offset_samples, track_length);

    if (!start_playback) {
        settle_meter_timer_after_stop();
        track_switch_in_progress_ = false;
        finish_handled_ = false;
        refresh_display();
        if (update_mpris_track) {
            mark_mpris_track_changed();
        } else {
            notify_mpris_state_changed();
        }
        return;
    }

    if (random_enabled_) {
        initialize_random_pass_if_needed();
    }

    try {
        std::unique_ptr<IAudioDecoder> decoder;
        bool range_limited_transport = false;
        std::string gapless_transport_kind;
        const bool gapless_allowed = !playlist_loading_;

        if (track.is_stream) {
            bool async_started = false;
            decoder = open_stream_decoder(index,
                                          initial_offset,
                                          preserve_paused,
                                          update_mpris_track,
                                          false,
                                          probe_generation,
                                          &async_started);
            if (async_started) {
                return;
            }
        } else if (random_enabled_ && gapless_allowed) {
            const std::vector<std::size_t> chain_indices =
                random_gapless_chain_indices(index, start_reason);
            if (chain_indices.size() > 1) {
                std::vector<GaplessTrackSpec> specs;
                specs.reserve(chain_indices.size());
                for (const std::size_t chain_index : chain_indices) {
                    specs.push_back(gapless_spec_for_entry(playlist_[chain_index]));
                }
                const bool first_track_uses_range =
                    specs.front().boundary_mode == GaplessBoundaryMode::ExactRange;
                decoder.reset(new GaplessChainDecoder(std::move(specs), initial_offset));
                decoder->open(track.audio_file_path);
                activate_gapless_chain(chain_indices);
                range_limited_transport = first_track_uses_range;
                gapless_transport_kind = "Random file chain";
                Logger::instance().info("Random gapless chain enabled for " +
                                        std::to_string(chain_indices.size()) + " tracks");
            } else {
                decoder = create_decoder_for_entry(track);
                if (has_exact_sample_range(track.start_sample_known,
                                       track.end_sample_known,
                                       track.start_sample,
                                       track.end_sample)) {
                    Logger::instance().debug("Bounded transport enabled");
                    decoder.reset(new RangeLimitedDecoder(std::move(decoder),
                                                          track.start_sample,
                                                          track.end_sample));
                    range_limited_transport = true;
                }
                const bool exact_range = has_exact_sample_range(
                    track.start_sample_known,
                    track.end_sample_known,
                    track.start_sample,
                    track.end_sample);
                const std::uint64_t source_offset = decoder_open_offset(
                    exact_range,
                    track.start_sample_known,
                    track.start_sample,
                    initial_offset);
                decoder->open_at_sample(track.audio_file_path, source_offset);
            }
        } else if (track.cue_track) {
            const std::size_t continuous_chain_end = cue_chain_end_index(index);
            if (continuous_chain_end > index + 1 || !gapless_allowed) {
                const PlaylistEntry& chain_last =
                    playlist_[continuous_chain_end - 1];
                const std::uint64_t chain_end_sample = chain_last.end_sample;
                std::unique_ptr<IAudioDecoder> base =
                    create_decoder_for_entry(track);
                if (has_exact_sample_range(track.start_sample_known,
                                           chain_last.end_sample_known,
                                           track.start_sample,
                                           chain_end_sample)) {
                    decoder.reset(new RangeLimitedDecoder(
                        std::move(base), track.start_sample, chain_end_sample));
                    decoder->open_at_sample(track.audio_file_path, initial_offset);
                    range_limited_transport = true;
                } else {
                    decoder = std::move(base);
                    decoder->open_at_sample(
                        track.audio_file_path,
                        decoder_open_offset(false,
                                            track.start_sample_known,
                                            track.start_sample,
                                            initial_offset));
                    Logger::instance().debug(
                        "CUE final boundary is estimated; playback continues to decoder EOF");
                }
                activate_gapless_chain(index, continuous_chain_end);
                if (continuous_chain_end > index + 1) {
                    gapless_transport_kind = "Continuous single-file CUE";
                    Logger::instance().info(
                        "Continuous CUE playback enabled for " +
                        std::to_string(continuous_chain_end - index) +
                        " tracks: " + track.audio_file_path);
                }
            } else {
                const std::size_t split_chain_end =
                    split_cue_file_chain_end_index(index);
                if (split_chain_end > index + 1) {
                    std::vector<GaplessTrackSpec> specs;
                    specs.reserve(split_chain_end - index);
                    for (std::size_t i = index; i < split_chain_end; ++i) {
                        specs.push_back(gapless_spec_for_entry(playlist_[i]));
                    }
                    const bool first_track_uses_range =
                        specs.front().boundary_mode == GaplessBoundaryMode::ExactRange;
                    decoder.reset(new GaplessChainDecoder(std::move(specs),
                                                          initial_offset));
                    decoder->open(track.audio_file_path);
                    activate_gapless_chain(index, split_chain_end);
                    range_limited_transport = first_track_uses_range;
                    gapless_transport_kind = "Split CUE file chain";
                    Logger::instance().info(
                        "Split CUE gapless chain enabled for " +
                        std::to_string(split_chain_end - index) + " tracks");
                } else {
                    decoder = create_decoder_for_entry(track);
                    if (has_exact_sample_range(track.start_sample_known,
                                               track.end_sample_known,
                                               track.start_sample,
                                               track.end_sample)) {
                        Logger::instance().debug("Bounded transport enabled");
                        decoder.reset(new RangeLimitedDecoder(
                            std::move(decoder),
                            track.start_sample,
                            track.end_sample));
                        range_limited_transport = true;
                    }
                    const bool exact_range = has_exact_sample_range(
                        track.start_sample_known,
                        track.end_sample_known,
                        track.start_sample,
                        track.end_sample);
                    const std::uint64_t source_offset = decoder_open_offset(
                        exact_range,
                        track.start_sample_known,
                        track.start_sample,
                        initial_offset);
                    decoder->open_at_sample(track.audio_file_path, source_offset);
                }
            }
        } else if (gapless_allowed) {
            const std::size_t chain_end = file_chain_end_index(index);
            if (chain_end > index + 1) {
                std::vector<GaplessTrackSpec> specs;
                specs.reserve(chain_end - index);
                for (std::size_t i = index; i < chain_end; ++i) {
                    specs.push_back(gapless_spec_for_entry(playlist_[i]));
                }
                const bool first_track_uses_range =
                    specs.front().boundary_mode == GaplessBoundaryMode::ExactRange;
                decoder.reset(new GaplessChainDecoder(std::move(specs), initial_offset));
                decoder->open(track.audio_file_path);
                activate_gapless_chain(index, chain_end);
                range_limited_transport = first_track_uses_range;
                gapless_transport_kind = "Separate-file chain";
                Logger::instance().info("Gapless file chain enabled for " +
                                        std::to_string(chain_end - index) + " tracks");
            } else {
                decoder = create_decoder_for_entry(track);
                if (has_exact_sample_range(track.start_sample_known,
                                       track.end_sample_known,
                                       track.start_sample,
                                       track.end_sample)) {
                    Logger::instance().debug("Bounded transport enabled");
                    decoder.reset(new RangeLimitedDecoder(std::move(decoder),
                                                          track.start_sample,
                                                          track.end_sample));
                    range_limited_transport = true;
                }
                const bool exact_range = has_exact_sample_range(
                    track.start_sample_known,
                    track.end_sample_known,
                    track.start_sample,
                    track.end_sample);
                const std::uint64_t source_offset = decoder_open_offset(
                    exact_range,
                    track.start_sample_known,
                    track.start_sample,
                    initial_offset);
                decoder->open_at_sample(track.audio_file_path, source_offset);
            }
        } else {
            decoder = create_decoder_for_entry(track);
            if (has_exact_sample_range(track.start_sample_known,
                                           track.end_sample_known,
                                           track.start_sample,
                                           track.end_sample)) {
                Logger::instance().debug("Bounded transport enabled");
                decoder.reset(new RangeLimitedDecoder(std::move(decoder),
                                                      track.start_sample,
                                                      track.end_sample));
                range_limited_transport = true;
            }
            const bool exact_range = has_exact_sample_range(
                track.start_sample_known,
                track.end_sample_known,
                track.start_sample,
                track.end_sample);
            const std::uint64_t source_offset = decoder_open_offset(
                exact_range,
                track.start_sample_known,
                track.start_sample,
                initial_offset);
            decoder->open_at_sample(track.audio_file_path, source_offset);
        }

        const AudioFormat active_output_format = decoder->format();
        const bool continuous_single_file_transport =
            gapless_transport_kind == "Continuous single-file CUE";
        const std::string active_soxr_runtime_description =
            "SoXr " + resample_quality_label(resample_quality_) +
            " (precision " +
            std::to_string(soxr_precision_for_quality(resample_quality_)) +
            ")";
        const auto finalize_processing_state =
            [&active_soxr_runtime_description](ActiveTrackTransportState& state) {
                state.resampler_runtime_reported =
                    state.processing_report.find("Resampler: initializing") !=
                    std::string::npos;
                if (state.resampler_runtime_reported) {
                    state.soxr_runtime_description =
                        active_soxr_runtime_description;
                }
            };
        std::vector<ActiveTrackTransportState> active_states;
        if (gapless_chain_active_) {
            active_states.reserve(gapless_chain_playlist_indices_.size());
            for (const std::size_t chain_index : gapless_chain_playlist_indices_) {
                if (chain_index >= playlist_.size()) {
                    throw std::runtime_error(
                        "Active gapless mapping contains an invalid playlist index");
                }
                const PlaylistEntry& active_entry = playlist_[chain_index];
                ActiveTrackTransportState state;
                state.planned_length_samples = track_length_samples(active_entry);
                state.range_limited = continuous_single_file_transport
                    ? range_limited_transport
                    : active_entry.presentation_end_kind ==
                          PresentationEndKind::ExactSampleSpan;
                state.native_decode = active_entry.native_decode;
                state.processing_report = processing_rules_report_for_entry(
                    active_entry, active_output_format);
                state.processing_path = processing_path_for_entry(
                    active_entry, active_output_format);
                finalize_processing_state(state);
                active_states.push_back(std::move(state));
            }
        } else {
            ActiveTrackTransportState state;
            state.planned_length_samples = track_length_samples(track);
            state.range_limited = range_limited_transport;
            state.native_decode = track.native_decode;
            state.processing_report = processing_rules_report_for_entry(
                track, active_output_format);
            state.processing_path = processing_path_for_entry(
                track, active_output_format);
            finalize_processing_state(state);
            active_states.push_back(std::move(state));
        }

        engine_.set_soft_volume_percent(soft_volume_percent_);
        engine_.set_soft_eq(bass_db_, treble_db_);
        engine_.set_pre_eq_headroom_tenths_db(effective_pre_eq_headroom_tenths_db());
        engine_.set_soft_eq_profile(bass_shelf_hz_, treble_shelf_hz_);
        engine_.set_deep_bass_enabled(deep_bass_enabled_);
        engine_.set_deep_bass_preset(deep_bass_internal_from_ui(deep_bass_preset_));
        engine_.set_deep_bass_amount(deep_bass_dsp_amount_from_ui(deep_bass_amount_));
        std::vector<std::uint64_t> logical_segment_offsets;
        if (continuous_single_file_transport && gapless_chain_active_ &&
            gapless_chain_offsets_.size() > 1) {
            logical_segment_offsets = gapless_chain_offsets_;
            logical_segment_offsets.push_back(gapless_chain_total_samples_);
        }
        auto alsa_backend = std::make_unique<AlsaPcmBackend>();
        alsa_backend->set_24bit_container_preference(
            alsa_24bit_preference_from_id(alsa_24bit_container_preference_));
        engine_.set_realtime_priority_enabled(realtime_audio_priority_enabled_);
        engine_.start(std::move(decoder),
                      std::move(alsa_backend),
                      current_device_,
                      initial_offset,
                      std::move(logical_segment_offsets));
        ensure_meter_timer_running();
        active_range_limited_transport_ = range_limited_transport;
        active_gapless_transport_kind_ = gapless_transport_kind;
        active_output_device_ = current_device_;
        active_track_transport_states_ = std::move(active_states);
        clear_random_stopped_preview();
        record_track_started(index, start_reason);
        refresh_active_alsa_output_diagnostics();
        if (preserve_paused) {
            engine_.pause();
        }
        on_stream_play_succeeded(index);
        ensure_stream_service_timer_running();
        track_switch_in_progress_ = false;
        finish_handled_ = false;
        refresh_display();
        if (!preserve_paused) {
            schedule_progress_deadline();
        }
        if (update_mpris_track) {
            mark_mpris_track_changed();
        } else {
            notify_mpris_state_changed();
        }
    } catch (const std::exception& ex) {
        cancel_progress_deadline();
        clear_gapless_chain();
        active_range_limited_transport_ = false;
        active_gapless_transport_kind_.clear();
        active_output_device_.clear();
        active_track_transport_states_.clear();
        track_switch_in_progress_ = false;
        finish_handled_ = false;
        settle_meter_timer_after_stop();
        Logger::instance().error(std::string("Failed to play track: ") + ex.what());
        handle_stream_playback_error(track.is_stream, track.audio_file_path, ex.what());
        notify_mpris_state_changed();
    }
}

void GtkPlayerWindow::open_file_dialog() {
    GtkWidget* dialog = gtk_file_chooser_dialog_new("Open audio files",
                                                    GTK_WINDOW(window_),
                                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Open", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);
    if (!last_open_directory_.empty()) {
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), last_open_directory_.c_str());
    }

    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Audio, cue and playlist files");
    gtk_file_filter_add_pattern(filter, "*.flac");
    gtk_file_filter_add_pattern(filter, "*.FLAC");
    gtk_file_filter_add_pattern(filter, "*.mp3");
    gtk_file_filter_add_pattern(filter, "*.MP3");
    gtk_file_filter_add_pattern(filter, "*.mp2");
    gtk_file_filter_add_pattern(filter, "*.MP2");
    gtk_file_filter_add_pattern(filter, "*.m4a");
    gtk_file_filter_add_pattern(filter, "*.M4A");
    gtk_file_filter_add_pattern(filter, "*.m4r");
    gtk_file_filter_add_pattern(filter, "*.M4R");
    gtk_file_filter_add_pattern(filter, "*.aac");
    gtk_file_filter_add_pattern(filter, "*.AAC");
    gtk_file_filter_add_pattern(filter, "*.ac3");
    gtk_file_filter_add_pattern(filter, "*.AC3");
    gtk_file_filter_add_pattern(filter, "*.dts");
    gtk_file_filter_add_pattern(filter, "*.DTS");
    gtk_file_filter_add_pattern(filter, "*.ogg");
    gtk_file_filter_add_pattern(filter, "*.OGG");
    gtk_file_filter_add_pattern(filter, "*.opus");
    gtk_file_filter_add_pattern(filter, "*.spx");
    gtk_file_filter_add_pattern(filter, "*.oga");
    gtk_file_filter_add_pattern(filter, "*.au");
    gtk_file_filter_add_pattern(filter, "*.snd");
    gtk_file_filter_add_pattern(filter, "*.caf");
    gtk_file_filter_add_pattern(filter, "*.voc");
    gtk_file_filter_add_pattern(filter, "*.ra");
    gtk_file_filter_add_pattern(filter, "*.w64");
    gtk_file_filter_add_pattern(filter, "*.bwf");
    gtk_file_filter_add_pattern(filter, "*.tak");
    gtk_file_filter_add_pattern(filter, "*.tta");
    gtk_file_filter_add_pattern(filter, "*.wma");
    gtk_file_filter_add_pattern(filter, "*.asf");
    gtk_file_filter_add_pattern(filter, "*.xwma");
    gtk_file_filter_add_pattern(filter, "*.wmv");
    gtk_file_filter_add_pattern(filter, "*.oma");
    gtk_file_filter_add_pattern(filter, "*.aa3");
    gtk_file_filter_add_pattern(filter, "*.at3");
    gtk_file_filter_add_pattern(filter, "*.mpc");
    gtk_file_filter_add_pattern(filter, "*.mp+");
    gtk_file_filter_add_pattern(filter, "*.mpp");
    gtk_file_filter_add_pattern(filter, "*.dsf");
    gtk_file_filter_add_pattern(filter, "*.dff");
    gtk_file_filter_add_pattern(filter, "*.OGA");
    gtk_file_filter_add_pattern(filter, "*.AU");
    gtk_file_filter_add_pattern(filter, "*.SND");
    gtk_file_filter_add_pattern(filter, "*.CAF");
    gtk_file_filter_add_pattern(filter, "*.VOC");
    gtk_file_filter_add_pattern(filter, "*.RA");
    gtk_file_filter_add_pattern(filter, "*.W64");
    gtk_file_filter_add_pattern(filter, "*.SPX");
    gtk_file_filter_add_pattern(filter, "*.BWF");
    gtk_file_filter_add_pattern(filter, "*.TAK");
    gtk_file_filter_add_pattern(filter, "*.TTA");
    gtk_file_filter_add_pattern(filter, "*.WMA");
    gtk_file_filter_add_pattern(filter, "*.ASF");
    gtk_file_filter_add_pattern(filter, "*.XWMA");
    gtk_file_filter_add_pattern(filter, "*.WMV");
    gtk_file_filter_add_pattern(filter, "*.OMA");
    gtk_file_filter_add_pattern(filter, "*.AA3");
    gtk_file_filter_add_pattern(filter, "*.AT3");
    gtk_file_filter_add_pattern(filter, "*.MPC");
    gtk_file_filter_add_pattern(filter, "*.MP+");
    gtk_file_filter_add_pattern(filter, "*.MPP");
    gtk_file_filter_add_pattern(filter, "*.DSF");
    gtk_file_filter_add_pattern(filter, "*.DFF");
    gtk_file_filter_add_pattern(filter, "*.OPUS");
    gtk_file_filter_add_pattern(filter, "*.wav");
    gtk_file_filter_add_pattern(filter, "*.WAV");
    gtk_file_filter_add_pattern(filter, "*.wave");
    gtk_file_filter_add_pattern(filter, "*.WAVE");
    gtk_file_filter_add_pattern(filter, "*.aiff");
    gtk_file_filter_add_pattern(filter, "*.AIFF");
    gtk_file_filter_add_pattern(filter, "*.aif");
    gtk_file_filter_add_pattern(filter, "*.AIF");
    gtk_file_filter_add_pattern(filter, "*.ape");
    gtk_file_filter_add_pattern(filter, "*.APE");
    gtk_file_filter_add_pattern(filter, "*.wv");
    gtk_file_filter_add_pattern(filter, "*.WV");
    gtk_file_filter_add_pattern(filter, "*.cue");
    gtk_file_filter_add_pattern(filter, "*.CUE");
    gtk_file_filter_add_pattern(filter, "*.m3u");
    gtk_file_filter_add_pattern(filter, "*.M3U");
    gtk_file_filter_add_pattern(filter, "*.m3u8");
    gtk_file_filter_add_pattern(filter, "*.M3U8");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        GSList* files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
        if (files != nullptr) {
            std::vector<std::string> selected_paths;
            for (GSList* node = files; node != nullptr; node = node->next) {
                char* filename = static_cast<char*>(node->data);
                if (filename != nullptr) {
                    if (*filename != '\0') {
                        selected_paths.emplace_back(filename);
                    }
                    g_free(filename);
                }
            }
            g_slist_free(files);

            if (!selected_paths.empty() &&
                !load_source_paths(selected_paths, true, false, true).empty()) {
                remember_open_directory_from_sources(selected_paths);
            }
        }
    }

    gtk_widget_destroy(dialog);
}

void GtkPlayerWindow::open_directory_dialog() {
    GtkWidget* dialog = gtk_file_chooser_dialog_new("Open directory",
                                                    GTK_WINDOW(window_),
                                                    GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Open", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    if (!last_open_directory_.empty()) {
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog),
                                            last_open_directory_.c_str());
    }

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* selected_directory =
            gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (selected_directory != nullptr && *selected_directory != '\0') {
            const std::vector<std::string> source_paths{selected_directory};
            if (open_source_paths(source_paths, true, false, true)) {
                remember_open_directory_from_sources(source_paths);
            }
        }
        g_free(selected_directory);
    }

    gtk_widget_destroy(dialog);
}

void GtkPlayerWindow::open_settings_dialog() {
    refresh_device_list();

    enum { RESPONSE_APPLY_CLOSE = 1002 };
    GtkWidget* dialog = gtk_dialog_new_with_buttons("Audio settings",
                                                    GTK_WINDOW(window_),
                                                    GTK_DIALOG_MODAL,
                                                    NULL);
    const PcmDialogLayout layout = create_pcm_dialog_layout(
        dialog, PcmDialogLayoutMode::Expandable);
    add_pcm_dialog_button(dialog, layout.footer, "_Cancel", GTK_RESPONSE_CANCEL);
    add_pcm_dialog_button(dialog, layout.footer, "_Save", RESPONSE_APPLY_CLOSE);
    GtkWidget* content = layout.content;

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_pack_start(GTK_BOX(content), grid, FALSE, FALSE, 0);

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
    GtkWidget* playlist_search_check =
        gtk_check_button_new_with_label("Enable playlist search filter");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(playlist_search_check), playlist_search_enabled_ ? TRUE : FALSE);

    GtkWidget* playlist_field_width_check =
        gtk_check_button_new_with_label("Limit playlist field width");
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(playlist_field_width_check),
        playlist_field_width_limit_enabled_ ? TRUE : FALSE);
    gtk_widget_set_tooltip_text(
        playlist_field_width_check,
        "Limits only displayed playlist cell width. Search, sorting and metadata keep the full text.");

    GtkWidget* playlist_field_width_row = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(playlist_field_width_row), 8);
    gtk_widget_set_margin_start(playlist_field_width_row, 22);
    GtkWidget* playlist_field_width_label = gtk_label_new("Maximum width:");
    gtk_label_set_xalign(GTK_LABEL(playlist_field_width_label), 0.0f);
    GtkWidget* playlist_field_width_spin = gtk_spin_button_new_with_range(
        kMinPlaylistFieldWidthChars,
        kMaxPlaylistFieldWidthChars,
        1.0);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(playlist_field_width_spin), TRUE);
    gtk_spin_button_set_value(
        GTK_SPIN_BUTTON(playlist_field_width_spin),
        static_cast<double>(playlist_field_width_chars_));
    gtk_entry_set_width_chars(GTK_ENTRY(playlist_field_width_spin), 4);
    gtk_widget_set_halign(playlist_field_width_spin, GTK_ALIGN_START);
    GtkWidget* playlist_field_width_units = gtk_label_new("characters");
    gtk_label_set_xalign(GTK_LABEL(playlist_field_width_units), 0.0f);
    gtk_grid_attach(GTK_GRID(playlist_field_width_row), playlist_field_width_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(playlist_field_width_row), playlist_field_width_spin, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(playlist_field_width_row), playlist_field_width_units, 2, 0, 1, 1);
    gtk_widget_set_sensitive(
        playlist_field_width_row,
        playlist_field_width_limit_enabled_ ? TRUE : FALSE);
    g_signal_connect(playlist_field_width_check, "toggled", G_CALLBACK(+[](
        GtkToggleButton* button, gpointer user_data) {
        GtkWidget* dependent = GTK_WIDGET(user_data);
        if (dependent != nullptr) {
            gtk_widget_set_sensitive(
                dependent,
                gtk_toggle_button_get_active(button));
        }
    }), playlist_field_width_row);

    GtkWidget* playlist_rows_row = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(playlist_rows_row), 8);
    GtkWidget* playlist_rows_label = gtk_label_new("Playlist rows at startup:");
    gtk_label_set_xalign(GTK_LABEL(playlist_rows_label), 0.0f);
    GtkWidget* playlist_rows_spin = gtk_spin_button_new_with_range(
        kMinPlaylistRows,
        kMaxPlaylistRows,
        1.0);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(playlist_rows_spin), TRUE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(playlist_rows_spin),
                              static_cast<double>(playlist_rows_at_startup_));
    gtk_entry_set_width_chars(GTK_ENTRY(playlist_rows_spin), 3);
    gtk_widget_set_halign(playlist_rows_spin, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(
        playlist_rows_row,
        "Sets the initial playlist height from 10 to 20 rows. Applied on the next start.");
    gtk_grid_attach(GTK_GRID(playlist_rows_row), playlist_rows_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(playlist_rows_row), playlist_rows_spin, 1, 0, 1, 1);
    GtkWidget* restore_sources_check = gtk_check_button_new_with_label("Restore last opened sources on startup");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(restore_sources_check), restore_last_sources_enabled_ ? TRUE : FALSE);
    GtkWidget* restore_active_track_check =
        gtk_check_button_new_with_label("Restore last active track");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(restore_active_track_check),
                                 restore_last_active_track_enabled_ ? TRUE : FALSE);
    gtk_widget_set_margin_start(restore_active_track_check, 22);
    gtk_widget_set_sensitive(restore_active_track_check,
                             restore_last_sources_enabled_ ? TRUE : FALSE);
    gtk_widget_set_tooltip_text(
        restore_active_track_check,
        "Restores and centers the last played track without starting playback.");
    g_signal_connect(restore_sources_check, "toggled", G_CALLBACK(+[](
        GtkToggleButton* button, gpointer user_data) {
        GtkWidget* dependent = GTK_WIDGET(user_data);
        const gboolean enabled = gtk_toggle_button_get_active(button);
        gtk_widget_set_sensitive(dependent, enabled);
        if (!enabled) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dependent), FALSE);
        }
    }), restore_active_track_check);
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
    gtk_grid_attach(GTK_GRID(grid), advanced_sep, 0, 1, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), advanced_title, 0, 2, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), alsa24_row, 0, 3, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), rt_row, 0, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), rt_status, 0, 5, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), ui_sep, 0, 6, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), ui_title, 0, 7, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), level_meter_check, 0, 8, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), clip_detect_check, 0, 9, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), progress_blink_check, 0, 10, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), playlist_search_check, 0, 11, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), playlist_field_width_check, 0, 12, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), playlist_field_width_row, 0, 13, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), playlist_rows_row, 0, 14, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), restore_sources_check, 0, 15, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), restore_active_track_check, 0, 16, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), log_sep, 0, 17, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), log_title, 0, 18, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), log_check, 0, 19, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), log_row_grid, 0, 20, 2, 1);

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
        const bool new_progress_blink_enabled =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(progress_blink_check)) != 0;
        if (new_progress_blink_enabled != progress_blink_enabled_) {
            progress_blink_enabled_ = new_progress_blink_enabled;
            reschedule_progress_deadline();
        }
        const bool playlist_search_requested =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(playlist_search_check)) != 0;
        if (playlist_search_requested != playlist_search_enabled_) {
            playlist_search_enabled_ = playlist_search_requested;
            apply_playlist_search_ui_state();
        }
        const bool playlist_field_width_limit_requested =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(playlist_field_width_check)) != 0;
        const int playlist_field_width_chars_requested = std::max(
            kMinPlaylistFieldWidthChars,
            std::min(kMaxPlaylistFieldWidthChars,
                     gtk_spin_button_get_value_as_int(
                         GTK_SPIN_BUTTON(playlist_field_width_spin))));
        if (playlist_field_width_limit_requested != playlist_field_width_limit_enabled_ ||
            playlist_field_width_chars_requested != playlist_field_width_chars_) {
            playlist_field_width_limit_enabled_ = playlist_field_width_limit_requested;
            playlist_field_width_chars_ = playlist_field_width_chars_requested;
            apply_playlist_field_width_limit(true);
        }
        playlist_rows_at_startup_ = std::max(
            kMinPlaylistRows,
            std::min(kMaxPlaylistRows,
                     gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(playlist_rows_spin))));
        const bool restore_sources_requested =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(restore_sources_check)) != 0;
        const bool restore_active_track_requested =
            restore_sources_requested &&
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(restore_active_track_check)) != 0;
        if (!restore_sources_requested) {
            restore_last_sources_enabled_ = false;
            restore_last_active_track_enabled_ = false;
            saved_last_active_track_ = LastActiveTrackLocator{};
            runtime_last_active_track_ = LastActiveTrackLocator{};
            cancel_pending_last_active_track_restore();
        } else {
            const bool track_restore_was_enabled = restore_last_active_track_enabled_;
            restore_last_sources_enabled_ = true;
            restore_last_active_track_enabled_ = restore_active_track_requested;
            if (!restore_last_active_track_enabled_) {
                saved_last_active_track_ = LastActiveTrackLocator{};
                runtime_last_active_track_ = LastActiveTrackLocator{};
                cancel_pending_last_active_track_restore();
            } else if (!track_restore_was_enabled) {
                saved_last_active_track_ = LastActiveTrackLocator{};
                const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
                if (transport.playing && current_track_index_ < playlist_.size()) {
                    remember_last_active_track(current_track_index_);
                }
            }
        }
        engine_.set_level_meter_enabled(level_meter_enabled_);
        engine_.set_clip_detection_enabled(clip_detection_enabled_);
        sync_meter_timer_to_settings();
        logging_enabled_ = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(log_check)) != 0;
        log_errors_only_ = gtk_combo_box_get_active(GTK_COMBO_BOX(log_mode_combo)) == 1;
        log_path_ = gtk_entry_get_text(GTK_ENTRY(log_path_entry));
        Logger::instance().configure(logging_enabled_, log_path_, log_errors_only_);
        save_preferences();
        refresh_device_list();
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
                                                    NULL);
    const PcmDialogLayout layout = create_pcm_dialog_layout(
        dialog, PcmDialogLayoutMode::Compact);
    add_pcm_dialog_button(dialog, layout.footer, "_Donate", RESPONSE_DONATE);
    add_pcm_dialog_button(dialog, layout.footer, "_License", RESPONSE_LICENSE);
    GtkWidget* about_close_button =
        add_pcm_dialog_button(dialog, layout.footer, "_Close", GTK_RESPONSE_CLOSE);
    GtkWidget* content = layout.content;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(content), box, FALSE, FALSE, 0);

    GdkPixbuf* about_pixbuf = load_embedded_pixbuf(kAboutIconResource);
    GtkWidget* about_icon = about_pixbuf != nullptr
        ? gtk_image_new_from_pixbuf(about_pixbuf)
        : nullptr;
    if (about_pixbuf != nullptr) {
        g_object_unref(about_pixbuf);
    }
    if (about_icon != nullptr) {
        gtk_widget_set_halign(about_icon, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(box), about_icon, FALSE, FALSE, 0);
    }

    GtkWidget* title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<b>PCM Transport 0.9.114</b>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.5f);
    GtkWidget* subtitle = gtk_label_new("Digital Audio Player");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.5f);

    GtkWidget* author = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(author),
                         "Author:\n<a href=\"https://github.com/andreyberestov\">Andrey Berestov</a>\n"
                         "andrey.berestov@gmail.com");
    gtk_label_set_xalign(GTK_LABEL(author), 0.5f);
    gtk_label_set_justify(GTK_LABEL(author), GTK_JUSTIFY_CENTER);
    gtk_label_set_selectable(GTK_LABEL(author), TRUE);

    GtkWidget* website = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(website), "<a href=\"https://andreyberestov.github.io/pcm-transport/\">https://andreyberestov.github.io/pcm-transport/</a>");
    gtk_label_set_xalign(GTK_LABEL(website), 0.5f);
    gtk_label_set_justify(GTK_LABEL(website), GTK_JUSTIFY_CENTER);
    gtk_label_set_selectable(GTK_LABEL(website), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(website), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(website), PANGO_WRAP_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(website), 56);

    GtkWidget* contributors = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(contributors),
                         "Contributors:\n<a href=\"https://github.com/loki1368\">loki1368</a>");
    gtk_label_set_xalign(GTK_LABEL(contributors), 0.5f);
    gtk_label_set_justify(GTK_LABEL(contributors), GTK_JUSTIFY_CENTER);
    gtk_label_set_selectable(GTK_LABEL(contributors), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(contributors), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(contributors), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(contributors), 64);

    GtkWidget* details = gtk_label_new(
        "License: GNU GPL v3\n"
        "Technologies: C++17, GTK3, Cairo, ALSA, libFLAC, FFmpeg libraries,\n"
        "FFmpeg resampling (SoXr when available), CUE parsing, MPRIS");
    gtk_label_set_xalign(GTK_LABEL(details), 0.5f);
    gtk_label_set_justify(GTK_LABEL(details), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(details), TRUE);

    GtkWidget* runtime_environment = gtk_label_new(nullptr);
    set_runtime_environment_label(runtime_environment, "checking...");
    gtk_label_set_xalign(GTK_LABEL(runtime_environment), 0.5f);
    gtk_label_set_justify(GTK_LABEL(runtime_environment), GTK_JUSTIFY_CENTER);
    gtk_label_set_selectable(GTK_LABEL(runtime_environment), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(runtime_environment), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(runtime_environment), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(runtime_environment), 60);

    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), author, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), contributors, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), website, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), details, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), runtime_environment, FALSE, FALSE, 0);
    const guint rtkit_watch_id = g_bus_watch_name(G_BUS_TYPE_SYSTEM,
                                                  "org.freedesktop.RealtimeKit1",
                                                  G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                  on_rtkit_name_appeared,
                                                  on_rtkit_name_vanished,
                                                  runtime_environment,
                                                  nullptr);
    gtk_widget_show_all(dialog);
    if (about_close_button != nullptr) {
        gtk_widget_grab_focus(about_close_button);
    }

    while (true) {
        const int response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (response == RESPONSE_DONATE) {
            GtkWidget* msg = gtk_dialog_new_with_buttons("Donate",
                                                         GTK_WINDOW(dialog),
                                                         GTK_DIALOG_MODAL,
                                                         NULL);
            const PcmDialogLayout message_layout = create_pcm_dialog_layout(
                msg, PcmDialogLayoutMode::Compact);
            add_pcm_dialog_button(msg, message_layout.footer, "_Copy", RESPONSE_COPY);
            add_pcm_dialog_button(msg, message_layout.footer, "_Close", GTK_RESPONSE_CLOSE);
            GtkWidget* area = message_layout.content;
            GtkWidget* label = gtk_label_new("If you enjoy PCM Transport, you can buy the author a cup of coffee.\n\nETH / USDT (ERC-20):\n0x985f490A569B1Cc08c5b157eA044387801BeD939");
            gtk_label_set_xalign(GTK_LABEL(label), 0.5f);
            gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
            gtk_label_set_selectable(GTK_LABEL(label), TRUE);
            gtk_box_pack_start(GTK_BOX(area), label, FALSE, FALSE, 0);
            gtk_widget_show_all(msg);
            while (true) {
                const int r = gtk_dialog_run(GTK_DIALOG(msg));
                if (r == RESPONSE_COPY) {
                    GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
                    gtk_clipboard_set_text(cb, "0x985f490A569B1Cc08c5b157eA044387801BeD939", -1);
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
                                                         NULL);
            const PcmDialogLayout message_layout = create_pcm_dialog_layout(
                msg, PcmDialogLayoutMode::Expandable);
            add_pcm_dialog_button(msg, message_layout.footer, "_Close", GTK_RESPONSE_CLOSE);
            GtkWidget* area = message_layout.content;
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
    if (rtkit_watch_id != 0U) {
        g_bus_unwatch_name(rtkit_watch_id);
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

void GtkPlayerWindow::stop_bitperfect_test_worker() {
    if (bitperfect_test_cancel_ != nullptr) {
        bitperfect_test_cancel_->store(true, std::memory_order_relaxed);
    }
    if (bitperfect_test_worker_.joinable()) {
        bitperfect_test_worker_.join();
    }
    bitperfect_test_cancel_.reset();
}

void GtkPlayerWindow::open_bitperfect_test_dialog(GtkWidget* parent_dialog, int duration_seconds) {
    if (engine_.is_playing()) {
        GtkWidget* ask = gtk_dialog_new_with_buttons("FLAC bit-perfect test",
                                                     GTK_WINDOW(parent_dialog),
                                                     GTK_DIALOG_MODAL,
                                                     NULL);
        const PcmDialogLayout question_layout = create_pcm_dialog_layout(
            ask, PcmDialogLayoutMode::Compact);
        add_pcm_message_content(question_layout.content,
                                "Playback is currently active. Stop playback and run the test?",
                                GTK_MESSAGE_QUESTION);
        add_pcm_dialog_button(ask, question_layout.footer, "_Cancel", GTK_RESPONSE_CANCEL);
        add_pcm_dialog_button(ask, question_layout.footer, "_Stop and Run", GTK_RESPONSE_ACCEPT);
        gtk_widget_show_all(ask);
        const int answer = gtk_dialog_run(GTK_DIALOG(ask));
        gtk_widget_destroy(ask);
        if (answer != GTK_RESPONSE_ACCEPT) return;
        stop_playback();
    }

    GtkWidget* dialog = gtk_dialog_new_with_buttons("FLAC bit-perfect test",
                                                    GTK_WINDOW(parent_dialog),
                                                    GTK_DIALOG_MODAL,
                                                    NULL);
    const PcmDialogLayout layout = create_pcm_dialog_layout(
        dialog, PcmDialogLayoutMode::Expandable);
    GtkWidget* close_button = add_pcm_dialog_button(dialog,
                                                    layout.footer,
                                                    "_Close",
                                                    GTK_RESPONSE_CLOSE);
    gtk_widget_set_sensitive(close_button, FALSE);
    gtk_window_set_deletable(GTK_WINDOW(dialog), FALSE);
    GtkWidget* area = layout.content;

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_size_request(box, 760, 420);
    gtk_box_pack_start(GTK_BOX(area), box, TRUE, TRUE, 0);

    GtkWidget* title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<b>FLAC bit-perfect diagnostic</b>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

    GtkWidget* note = gtk_label_new("libFLAC / flac CLI only. FFmpeg libraries are not used. The comparison is made before ALSA output.");
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

    stop_bitperfect_test_worker();
    bitperfect_test_cancel_ = std::make_shared<std::atomic<bool>>(false);
    const std::shared_ptr<std::atomic<bool>> cancel_requested = bitperfect_test_cancel_;
    bitperfect_test_worker_ = std::thread([
        duration_seconds,
        soft_volume,
        bass_db,
        treble_db,
        headroom,
        deep_bass,
        deep_bass_preset,
        deep_bass_amount,
        deep_bass_amount_dsp,
        bass_hz,
        treble_hz,
        level_meter,
        clip_detection,
        text_view,
        progress,
        close_button,
        cancel_requested]() {
        std::string tmp_dir;
        auto cleanup = [&]() {
            if (!tmp_dir.empty()) {
                const std::string cmd = std::string("rm -rf -- ") + shell_quote(tmp_dir);
                std::system(cmd.c_str());
            }
        };
        const auto cancelled = [&cancel_requested]() {
            return cancel_requested != nullptr &&
                   cancel_requested->load(std::memory_order_relaxed);
        };
        try {
            if (cancelled()) {
                return;
            }
            post_diagnostics_update(text_view, progress, close_button,
                "PCM Transport FLAC bit-perfect test\nVersion: 0.9.114\nMode: current player processing path before ALSA\nFFmpeg libraries: not used\n", 0.02, false);
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
            if (!write_test_wav(source_wav, duration_seconds, cancel_requested.get()) ||
                cancelled()) {
                cleanup();
                return;
            }

            gchar* flac_cli = g_find_program_in_path("flac");
            if (flac_cli == nullptr) throw std::runtime_error("External flac CLI was not found in PATH");
            g_free(flac_cli);

            post_diagnostics_update(text_view, progress, close_button, "Encoding WAV to FLAC using flac CLI...\n", 0.25, false);
            std::string cmd = "flac -f -s " + shell_quote(source_wav) + " -o " + shell_quote(test_flac);
            if (std::system(cmd.c_str()) != 0) throw std::runtime_error("flac CLI encode failed");
            if (cancelled()) {
                cleanup();
                return;
            }

            post_diagnostics_update(text_view, progress, close_button, "Decoding reference using flac -d -c...\n", 0.42, false);
            cmd = "flac -d -c -s " + shell_quote(test_flac) + " > " + shell_quote(reference_wav);
            if (std::system(cmd.c_str()) != 0) throw std::runtime_error("flac CLI reference decode failed");
            if (cancelled()) {
                cleanup();
                return;
            }
            const WavPcm16Data ref = read_wav_pcm16(reference_wav);

            post_diagnostics_update(text_view, progress, close_button, "Decoding with PCM Transport internal libFLAC path...\n", 0.62, false);
            const std::vector<std::int16_t> internal = render_internal_path_16(test_flac, soft_volume, bass_db, treble_db, headroom,
                                                                               deep_bass, deep_bass_preset, deep_bass_amount_dsp,
                                                                               bass_hz, treble_hz, cancel_requested.get());
            if (cancelled()) {
                cleanup();
                return;
            }

            post_diagnostics_update(text_view, progress, close_button, "Comparing samples...\n", 0.82, false);
            const CompareResult result = compare_samples(ref.samples,
                                                         internal,
                                                         cancel_requested.get());
            if (cancelled()) {
                cleanup();
                return;
            }
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
            if (!cancelled()) {
                post_diagnostics_update(text_view, progress, close_button, std::string("\nERROR: ") + ex.what() + "\nTemporary files removed.\n", 1.0, true);
            }
        }
    });

    gtk_dialog_run(GTK_DIALOG(dialog));
    stop_bitperfect_test_worker();
    gtk_widget_destroy(dialog);
}

void GtkPlayerWindow::open_eq_dialog() {
    enum { RESPONSE_RESET = 1001 };
    GtkWidget* dialog = gtk_dialog_new_with_buttons("DSP Studio",
                                                    GTK_WINDOW(window_),
                                                    GTK_DIALOG_MODAL,
                                                    NULL);
    const PcmDialogLayout layout = create_pcm_dialog_layout(
        dialog, PcmDialogLayoutMode::Expandable);
    add_pcm_dialog_button(dialog, layout.footer, "_Close", GTK_RESPONSE_CLOSE);
    add_pcm_dialog_button(dialog, layout.footer, "_Reset", RESPONSE_RESET);
    GtkWidget* content = layout.content;
    GtkWidget* notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
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

    GtkWidget* dsd_scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(dsd_scrolled),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(dsd_scrolled, TRUE);
    gtk_widget_set_vexpand(dsd_scrolled, TRUE);
    GtkWidget* dsd_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(dsd_root, 14);
    gtk_widget_set_margin_end(dsd_root, 14);
    gtk_widget_set_margin_top(dsd_root, 14);
    gtk_widget_set_margin_bottom(dsd_root, 14);
    gtk_container_add(GTK_CONTAINER(dsd_scrolled), dsd_root);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), dsd_scrolled, gtk_label_new("DSD"));

    auto create_notebook_grid = [&](const char* title) -> GtkWidget* {
        GtkWidget* page = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(page), 14);
        gtk_grid_set_column_spacing(GTK_GRID(page), 0);
        gtk_widget_set_hexpand(page, TRUE);
        gtk_widget_set_vexpand(page, TRUE);
        gtk_widget_set_margin_start(page, 12);
        gtk_widget_set_margin_end(page, 12);
        gtk_widget_set_margin_top(page, 12);
        gtk_widget_set_margin_bottom(page, 12);
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page, gtk_label_new(title));
        return page;
    };
    GtkWidget* alsa_diagnostics_root =
        create_notebook_grid("ALSA Output Diagnostics");
    GtkWidget* tests_root = create_notebook_grid("Tests");
    int root_row = 0;
    int processing_row = 0;
    int alsa_diagnostics_row = 0;
    int tests_row = 0;
    auto attach_root = [&](GtkWidget* widget) {
        gtk_widget_set_hexpand(widget, TRUE);
        gtk_grid_attach(GTK_GRID(root), widget, 0, root_row++, 1, 1);
    };
    auto attach_processing = [&](GtkWidget* widget) {
        gtk_widget_set_hexpand(widget, TRUE);
        gtk_grid_attach(GTK_GRID(processing_root), widget, 0, processing_row++, 1, 1);
    };
    auto attach_alsa_diagnostics = [&](GtkWidget* widget) {
        gtk_widget_set_hexpand(widget, TRUE);
        gtk_grid_attach(GTK_GRID(alsa_diagnostics_root),
                        widget,
                        0,
                        alsa_diagnostics_row++,
                        1,
                        1);
    };
    auto attach_tests = [&](GtkWidget* widget) {
        gtk_widget_set_hexpand(widget, TRUE);
        gtk_grid_attach(GTK_GRID(tests_root), widget, 0, tests_row++, 1, 1);
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

    GtkWidget* dsd_header = make_header(
        "DSD to PCM Conversion",
        "Select the final PCM sample rate used when the FFmpeg API decodes DSD. Native DSD and DoP output are not used.");
    gtk_box_pack_start(GTK_BOX(dsd_root), dsd_header, FALSE, FALSE, 0);

    std::vector<std::pair<std::uint32_t, GtkWidget*>> dsd_rate_combos;
    GtkSizeGroup* dsd_source_size_group = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    GtkSizeGroup* dsd_intermediate_size_group = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    GtkSizeGroup* dsd_combo_size_group = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    g_object_set_data_full(G_OBJECT(dialog), "pcm-dsd-source-size-group", dsd_source_size_group, g_object_unref);
    g_object_set_data_full(G_OBJECT(dialog), "pcm-dsd-intermediate-size-group", dsd_intermediate_size_group, g_object_unref);
    g_object_set_data_full(G_OBJECT(dialog), "pcm-dsd-combo-size-group", dsd_combo_size_group, g_object_unref);

    auto current_dsd_target = [this](std::uint32_t dsd_rate) -> std::uint32_t {
        for (const DsdPcmRule& rule : dsd_pcm_rules_) {
            if (rule.dsd_sample_rate == dsd_rate) {
                return rule.pcm_sample_rate;
            }
        }
        const DsdRateDefinition* definition = find_dsd_rate_definition(dsd_rate);
        return definition != nullptr ? definition->default_pcm_rate : 0U;
    };

    auto append_dsd_family = [&](bool family_441, const char* family_title) {
        GtkWidget* family_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
        GtkWidget* family_label = gtk_label_new(nullptr);
        const std::string family_markup = std::string("<b>") + family_title + "</b>";
        gtk_label_set_markup(GTK_LABEL(family_label), family_markup.c_str());
        gtk_label_set_xalign(GTK_LABEL(family_label), 0.0f);
        gtk_box_pack_start(GTK_BOX(family_box), family_label, FALSE, FALSE, 0);

        GtkWidget* table = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(table), 7);
        gtk_grid_set_column_spacing(GTK_GRID(table), 12);
        gtk_widget_set_hexpand(table, TRUE);

        const char* headings[] = {"DSD source", "FFmpeg API PCM", "PCM output"};
        for (int column = 0; column < 3; ++column) {
            GtkWidget* heading = gtk_label_new(nullptr);
            const std::string markup = std::string("<b>") + headings[column] + "</b>";
            gtk_label_set_markup(GTK_LABEL(heading), markup.c_str());
            gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
            if (column == 0) gtk_size_group_add_widget(dsd_source_size_group, heading);
            else if (column == 1) gtk_size_group_add_widget(dsd_intermediate_size_group, heading);
            else gtk_size_group_add_widget(dsd_combo_size_group, heading);
            gtk_grid_attach(GTK_GRID(table), heading, column, 0, 1, 1);
        }

        int row = 1;
        for (const DsdRateDefinition& definition : kDsdRateDefinitions) {
            if (definition.family_441 != family_441) {
                continue;
            }

            GtkWidget* source = gtk_label_new(definition.source_label);
            gtk_label_set_xalign(GTK_LABEL(source), 0.0f);
            gtk_size_group_add_widget(dsd_source_size_group, source);
            GtkWidget* intermediate = gtk_label_new(definition.ffmpeg_label);
            gtk_label_set_xalign(GTK_LABEL(intermediate), 0.0f);
            gtk_size_group_add_widget(dsd_intermediate_size_group, intermediate);
            GtkWidget* combo = gtk_combo_box_text_new();
            gtk_widget_set_tooltip_text(
                combo,
                "Matching the original 44.1/48 kHz rate family is recommended. Cross-family conversion is available for device compatibility.");
            gtk_size_group_add_widget(dsd_combo_size_group, combo);

            const std::uint32_t selected_rate = current_dsd_target(definition.dsd_sample_rate);
            bool selected_in_standard_list = selected_rate == 0;
            auto append_rate = [&](std::uint32_t rate) {
                std::string label = format_rate_khz(rate);
                if (rate == definition.default_pcm_rate) {
                    label += " — recommended";
                }
                const std::string id = std::to_string(rate);
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), id.c_str(), label.c_str());
                if (rate == selected_rate) {
                    selected_in_standard_list = true;
                }
            };

            for (std::uint32_t rate : kSelectablePcmRates) {
                if (rate > definition.ffmpeg_pcm_rate) continue;
                const bool matching = definition.family_441
                    ? ((rate % 44100U) == 0U)
                    : ((rate % 48000U) == 0U);
                if (matching) append_rate(rate);
            }
            for (std::uint32_t rate : kSelectablePcmRates) {
                if (rate > definition.ffmpeg_pcm_rate) continue;
                const bool matching = definition.family_441
                    ? ((rate % 44100U) == 0U)
                    : ((rate % 48000U) == 0U);
                if (!matching) append_rate(rate);
            }
            if (!selected_in_standard_list && selected_rate <= definition.ffmpeg_pcm_rate) {
                append_rate(selected_rate);
            }
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "0", "FFmpeg API output — no additional resampling");
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), std::to_string(selected_rate).c_str());

            g_object_set_data(G_OBJECT(combo), "pcm-dsd-rate", GUINT_TO_POINTER(definition.dsd_sample_rate));
            g_signal_connect(combo, "changed", G_CALLBACK(+[](GtkComboBox* widget, gpointer user_data) {
                auto* self = static_cast<GtkPlayerWindow*>(user_data);
                const gchar* active_id = gtk_combo_box_get_active_id(widget);
                if (self == nullptr || active_id == nullptr) return;
                const std::uint32_t dsd_rate = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(widget), "pcm-dsd-rate"));
                std::uint32_t target_rate = 0;
                try { target_rate = static_cast<std::uint32_t>(std::stoul(active_id)); } catch (...) { return; }
                for (DsdPcmRule& rule : self->dsd_pcm_rules_) {
                    if (rule.dsd_sample_rate == dsd_rate) {
                        rule.pcm_sample_rate = target_rate;
                        if (!self->bulk_preferences_update_) {
                            self->refresh_playlist_processing_metadata();
                            self->save_preferences();
                            self->refresh_display();
                        }
                        return;
                    }
                }
            }), this);

            gtk_grid_attach(GTK_GRID(table), source, 0, row, 1, 1);
            gtk_grid_attach(GTK_GRID(table), intermediate, 1, row, 1, 1);
            gtk_grid_attach(GTK_GRID(table), combo, 2, row, 1, 1);
            dsd_rate_combos.push_back(std::make_pair(definition.dsd_sample_rate, combo));
            ++row;
        }
        gtk_box_pack_start(GTK_BOX(family_box), table, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(dsd_root), family_box, FALSE, FALSE, 0);
    };

    append_dsd_family(true, "44.1 kHz family");
    append_dsd_family(false, "48 kHz family");

    GtkWidget* dsd_separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(dsd_root), dsd_separator, FALSE, FALSE, 0);

    GtkWidget* dsd_depth_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(dsd_depth_grid), row_spacing);
    gtk_grid_set_column_spacing(GTK_GRID(dsd_depth_grid), col_spacing);
    GtkWidget* dsd_depth_label = gtk_label_new("PCM output bit depth:");
    gtk_label_set_xalign(GTK_LABEL(dsd_depth_label), 0.0f);
    GtkWidget* dsd_depth_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dsd_depth_combo), "24", "24-bit — recommended");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dsd_depth_combo), "32", "32-bit");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(dsd_depth_combo), "16", "16-bit — compatibility");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(dsd_depth_combo), std::to_string(dsd_pcm_output_bits_).c_str());
    gtk_widget_set_hexpand(dsd_depth_combo, TRUE);
    gtk_grid_attach(GTK_GRID(dsd_depth_grid), dsd_depth_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(dsd_depth_grid), dsd_depth_combo, 1, 0, 1, 1);
    gtk_box_pack_start(GTK_BOX(dsd_root), dsd_depth_grid, FALSE, FALSE, 0);

    g_signal_connect(dsd_depth_combo, "changed", G_CALLBACK(+[](GtkComboBox* widget, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        const gchar* active_id = gtk_combo_box_get_active_id(widget);
        if (self == nullptr || active_id == nullptr) return;
        std::uint16_t bits = 0;
        try { bits = static_cast<std::uint16_t>(std::stoul(active_id)); } catch (...) { return; }
        if (bits != 16 && bits != 24 && bits != 32) return;
        self->dsd_pcm_output_bits_ = bits;
        if (!self->bulk_preferences_update_) {
            self->refresh_playlist_processing_metadata();
            self->save_preferences();
            self->refresh_display();
        }
    }), this);

    GtkWidget* dsd_quality_note = gtk_label_new(
        "Conversion quality follows the settings in Processing Rules. Dither is applied only for 16-bit PCM output.");
    gtk_label_set_xalign(GTK_LABEL(dsd_quality_note), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(dsd_quality_note), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(dsd_quality_note), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(dsd_quality_note), 74);
    gtk_box_pack_start(GTK_BOX(dsd_root), dsd_quality_note, FALSE, FALSE, 0);

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

    applied_stereo_tonal_dsp_controls_enabled_.reset();
    stereo_tonal_dsp_controls_ = {
        headroom_grid,
        deep_bass_row_grid,
        deep_bass_amount_grid,
        tone_grid,
        tone_graph
    };
    {
        const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
        refresh_stereo_tonal_dsp_controls(transport.playing, transport.format.channels);
    }

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
    g_object_set_data_full(G_OBJECT(dialog),
                           "pcm-rules-size-group",
                           rules_size_group,
                           g_object_unref);
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
    const char* rates[] = {
        "44100", "48000", "88200", "96000", "176400", "192000",
        "352800", "384000", "705600", "768000"
    };
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
    g_object_set_data_full(G_OBJECT(dialog),
                           "pcm-rules-list-height-group",
                           rules_list_height_group,
                           g_object_unref);
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
        if (!self->bulk_preferences_update_) {
            self->mark_continuous_preferences_dirty();
            self->refresh_display();
            self->notify_mpris_state_changed();
            if (self->soft_volume_scale_ != nullptr) gtk_widget_queue_draw(self->soft_volume_scale_);
        }
    }), this);
    g_signal_connect(pre_eq_headroom_scale, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        self->pre_eq_headroom_tenths_db_ = std::max(0, std::min(kUiPreEqHeadroomMaxTenthsDb, static_cast<int>(std::lround(gtk_range_get_value(range) * 10.0))));
        self->engine_.set_pre_eq_headroom_tenths_db(self->pre_eq_headroom_tenths_db_);
        if (!self->bulk_preferences_update_) {
            self->mark_continuous_preferences_dirty();
            self->refresh_display();
        }
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
        if (!self->bulk_preferences_update_) {
            if (graph != nullptr) gtk_widget_queue_draw(graph);
            self->save_preferences();
            self->refresh_display();
        }
    }), this);
    g_signal_connect(deep_bass_preset_combo, "changed", G_CALLBACK(+[](GtkComboBox* combo, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        self->deep_bass_preset_ = clamp_deep_bass_preset_ui(gtk_combo_box_get_active(combo));
        self->engine_.set_deep_bass_preset(deep_bass_internal_from_ui(self->deep_bass_preset_));
        self->apply_auto_pre_eq_headroom(false);
        GtkWidget* slider = GTK_WIDGET(g_object_get_data(G_OBJECT(combo), "pre-eq-headroom-scale"));
        if (slider != nullptr) gtk_range_set_value(GTK_RANGE(slider), static_cast<double>(self->effective_pre_eq_headroom_tenths_db()) / 10.0);
        GtkWidget* graph = GTK_WIDGET(g_object_get_data(G_OBJECT(combo), "tone-graph"));
        if (!self->bulk_preferences_update_) {
            if (graph != nullptr) gtk_widget_queue_draw(graph);
            self->save_preferences();
            self->refresh_display();
        }
    }), this);
    g_signal_connect(deep_bass_amount_scale, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        self->deep_bass_amount_ = clamp_deep_bass_amount_ui(static_cast<int>(std::lround(gtk_range_get_value(range))));
        self->engine_.set_deep_bass_amount(deep_bass_dsp_amount_from_ui(self->deep_bass_amount_));
        self->apply_auto_pre_eq_headroom(false);
        GtkWidget* slider = GTK_WIDGET(g_object_get_data(G_OBJECT(range), "pre-eq-headroom-scale"));
        if (slider != nullptr) gtk_range_set_value(GTK_RANGE(slider), static_cast<double>(self->effective_pre_eq_headroom_tenths_db()) / 10.0);
        GtkWidget* graph = GTK_WIDGET(g_object_get_data(G_OBJECT(range), "tone-graph"));
        if (!self->bulk_preferences_update_) {
            if (graph != nullptr) gtk_widget_queue_draw(graph);
            self->mark_continuous_preferences_dirty();
            self->refresh_display();
        }
    }), this);
    g_signal_connect(bass_scale, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        self->bass_db_ = static_cast<int>(std::lround(gtk_range_get_value(range)));
        self->engine_.set_soft_eq(self->bass_db_, self->treble_db_);
        self->apply_auto_pre_eq_headroom(false);
        GtkWidget* slider = GTK_WIDGET(g_object_get_data(G_OBJECT(range), "pre-eq-headroom-scale"));
        if (slider != nullptr) gtk_range_set_value(GTK_RANGE(slider), static_cast<double>(self->effective_pre_eq_headroom_tenths_db()) / 10.0);
        GtkWidget* graph = GTK_WIDGET(g_object_get_data(G_OBJECT(range), "tone-graph"));
        if (!self->bulk_preferences_update_) {
            if (graph != nullptr) gtk_widget_queue_draw(graph);
            self->mark_continuous_preferences_dirty();
            self->refresh_display();
        }
    }), this);
    g_signal_connect(treble_scale, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        self->treble_db_ = static_cast<int>(std::lround(gtk_range_get_value(range)));
        self->engine_.set_soft_eq(self->bass_db_, self->treble_db_);
        self->apply_auto_pre_eq_headroom(false);
        GtkWidget* slider = GTK_WIDGET(g_object_get_data(G_OBJECT(range), "pre-eq-headroom-scale"));
        if (slider != nullptr) gtk_range_set_value(GTK_RANGE(slider), static_cast<double>(self->effective_pre_eq_headroom_tenths_db()) / 10.0);
        GtkWidget* graph = GTK_WIDGET(g_object_get_data(G_OBJECT(range), "tone-graph"));
        if (!self->bulk_preferences_update_) {
            if (graph != nullptr) gtk_widget_queue_draw(graph);
            self->mark_continuous_preferences_dirty();
            self->refresh_display();
        }
    }), this);
    const auto connect_continuous_preference_scale = [this](GtkWidget* scale) {
        gtk_widget_add_events(scale,
                              GDK_BUTTON_PRESS_MASK |
                              GDK_BUTTON_RELEASE_MASK |
                              GDK_KEY_PRESS_MASK |
                              GDK_KEY_RELEASE_MASK |
                              GDK_SCROLL_MASK);
        g_signal_connect(scale, "button-press-event", G_CALLBACK(+[](
            GtkWidget*, GdkEventButton* event, gpointer user_data) -> gboolean {
            auto* self = static_cast<GtkPlayerWindow*>(user_data);
            if (self != nullptr && event != nullptr && event->button == 1) {
                self->begin_continuous_preferences_interaction();
            }
            return FALSE;
        }), this);
        g_signal_connect(scale, "key-press-event", G_CALLBACK(+[](
            GtkWidget*, GdkEventKey*, gpointer user_data) -> gboolean {
            auto* self = static_cast<GtkPlayerWindow*>(user_data);
            if (self != nullptr) {
                self->begin_continuous_preferences_interaction();
            }
            return FALSE;
        }), this);
        g_signal_connect_after(scale, "button-release-event", G_CALLBACK(+[](
            GtkWidget*, GdkEventButton* event, gpointer user_data) -> gboolean {
            auto* self = static_cast<GtkPlayerWindow*>(user_data);
            if (self != nullptr && event != nullptr && event->button == 1) {
                self->commit_continuous_preferences();
            }
            return FALSE;
        }), this);
        g_signal_connect_after(scale, "key-release-event", G_CALLBACK(+[](
            GtkWidget*, GdkEventKey*, gpointer user_data) -> gboolean {
            auto* self = static_cast<GtkPlayerWindow*>(user_data);
            if (self != nullptr) {
                self->commit_continuous_preferences();
            }
            return FALSE;
        }), this);
        g_signal_connect_after(scale, "scroll-event", G_CALLBACK(+[](
            GtkWidget*, GdkEventScroll*, gpointer user_data) -> gboolean {
            auto* self = static_cast<GtkPlayerWindow*>(user_data);
            if (self != nullptr && self->continuous_preferences_dirty_) {
                // GtkRange has already applied the scroll step and emitted
                // value-changed. Reuse the single preferences write debounce;
                // no separate gesture timeout is needed for wheel/touchpad.
                self->save_preferences();
            }
            return FALSE;
        }), this);
    };
    connect_continuous_preference_scale(volume_scale);
    connect_continuous_preference_scale(pre_eq_headroom_scale);
    connect_continuous_preference_scale(deep_bass_amount_scale);
    connect_continuous_preference_scale(bass_scale);
    connect_continuous_preference_scale(treble_scale);

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
        GtkWidget* slider = GTK_WIDGET(g_object_get_data(G_OBJECT(combo), "pre-eq-headroom-scale"));
        if (slider != nullptr) gtk_range_set_value(GTK_RANGE(slider), static_cast<double>(self->effective_pre_eq_headroom_tenths_db()) / 10.0);
        if (!self->bulk_preferences_update_) {
            if (graph != nullptr) gtk_widget_queue_draw(graph);
            self->save_preferences();
            self->refresh_display();
        }
    }), this);
    g_signal_connect(resample_quality_combo, "changed", G_CALLBACK(+[](GtkComboBox* combo, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        const gchar* id = gtk_combo_box_get_active_id(combo);
        if (id != nullptr) self->resample_quality_ = id;
        if (!self->bulk_preferences_update_) {
            self->refresh_playlist_processing_metadata();
            self->save_preferences();
        }
    }), this);
    g_signal_connect(bitdepth_quality_combo, "changed", G_CALLBACK(+[](GtkComboBox* combo, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        const gchar* id = gtk_combo_box_get_active_id(combo);
        if (id != nullptr) self->bitdepth_quality_ = id;
        if (!self->bulk_preferences_update_) {
            self->refresh_playlist_processing_metadata();
            self->save_preferences();
        }
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

    attach_tests(make_header("FLAC bit-perfect test", "Generates a deterministic 16-bit / 44.1 kHz / stereo FLAC file, decodes a reference with flac CLI, renders the current PCM Transport path before ALSA, and compares samples."));
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
    attach_tests(diag_grid);
    GtkWidget* diag_note = gtk_label_new("The test uses libFLAC / flac CLI only. FFmpeg libraries are not used. FAIL is expected if DSP, soft volume, headroom or Deep Bass changes the signal.");
    gtk_label_set_xalign(GTK_LABEL(diag_note), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(diag_note), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(diag_note), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(diag_note), 74);
    gtk_widget_set_hexpand(diag_note, FALSE);
    gtk_widget_set_vexpand(diag_note, FALSE);
    gtk_widget_set_margin_start(diag_note, section_indent);
    attach_tests(diag_note);
    gtk_widget_set_hexpand(diag_note, FALSE);
    gtk_widget_set_vexpand(diag_note, FALSE);
    auto* diag_btn_data = new BitPerfectButtonData{this, dialog, diag_duration_combo};
    g_signal_connect_data(diag_run_button, "clicked", G_CALLBACK(GtkPlayerWindow::on_run_bitperfect_test_clicked), diag_btn_data, destroy_bitperfect_button_data, static_cast<GConnectFlags>(0));

    attach_alsa_diagnostics(make_header("ALSA output diagnostics", "Shows the active ALSA path, playback transport, gapless state, processing rules and selected PCM device capabilities."));
    GtkWidget* alsa_diag_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(alsa_diag_grid), row_spacing);
    gtk_grid_set_column_spacing(GTK_GRID(alsa_diag_grid), col_spacing);
    gtk_widget_set_margin_start(alsa_diag_grid, section_indent);

    GtkWidget* active_output_label = gtk_label_new("Active ALSA output:");
    gtk_label_set_xalign(GTK_LABEL(active_output_label), 0.0f);
    GtkWidget* active_output_value = gtk_label_new(nullptr);
    diagnostics_active_output_value_ = active_output_value;
    refresh_active_alsa_output_diagnostics();
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
    attach_alsa_diagnostics(alsa_diag_grid);

    g_signal_connect(refresh_active_output_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer user_data) {
        auto* self = static_cast<GtkPlayerWindow*>(user_data);
        if (self != nullptr) {
            self->refresh_active_alsa_output_diagnostics();
        }
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
            if (preferences_save_timeout_id_ != 0) {
                g_source_remove(preferences_save_timeout_id_);
                preferences_save_timeout_id_ = 0;
            }
            continuous_preferences_dirty_ = false;
            continuous_preferences_interaction_active_ = false;
            preferences_save_deferred_for_continuous_ = false;
            bulk_preferences_update_ = true;

            gtk_range_set_value(GTK_RANGE(volume_scale), 100.0);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(deep_bass_check), FALSE);
            gtk_combo_box_set_active(GTK_COMBO_BOX(deep_bass_preset_combo), 0);
            gtk_range_set_value(GTK_RANGE(deep_bass_amount_scale), 0.0);
            gtk_range_set_value(GTK_RANGE(bass_scale), 0.0);
            gtk_range_set_value(GTK_RANGE(treble_scale), 0.0);
            gtk_combo_box_set_active(GTK_COMBO_BOX(preset_combo), 0);
            gtk_range_set_value(GTK_RANGE(pre_eq_headroom_scale), 0.0);

            resample_rules_.clear();
            bitdepth_rules_.clear();
            resample_quality_ = "maximum";
            bitdepth_quality_ = "tpdf_hp";
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(resample_quality_combo),
                                        resample_quality_.c_str());
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(bitdepth_quality_combo),
                                        bitdepth_quality_.c_str());

            const auto clear_rule_rows = [](GtkWidget* list) {
                GList* children = gtk_container_get_children(GTK_CONTAINER(list));
                for (GList* child = children; child != nullptr; child = child->next) {
                    gtk_widget_destroy(GTK_WIDGET(child->data));
                }
                g_list_free(children);
            };
            clear_rule_rows(rate_list);
            clear_rule_rows(bit_list);

            reset_dsd_pcm_defaults();
            for (const auto& item : dsd_rate_combos) {
                const DsdRateDefinition* definition = find_dsd_rate_definition(item.first);
                if (definition != nullptr) {
                    gtk_combo_box_set_active_id(GTK_COMBO_BOX(item.second),
                                                std::to_string(definition->default_pcm_rate).c_str());
                }
            }
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(dsd_depth_combo), "24");

            bulk_preferences_update_ = false;
            continuous_preferences_dirty_ = false;
            continuous_preferences_interaction_active_ = false;
            preferences_save_deferred_for_continuous_ = false;
            refresh_playlist_processing_metadata();
            refresh_display();
            notify_mpris_state_changed();
            if (soft_volume_scale_ != nullptr) {
                gtk_widget_queue_draw(soft_volume_scale_);
            }
            gtk_widget_queue_draw(tone_graph);
            save_preferences();
            continue;
        }
        break;
    }
    commit_continuous_preferences();
    diagnostics_active_output_value_ = nullptr;
    stereo_tonal_dsp_controls_.clear();
    applied_stereo_tonal_dsp_controls_enabled_.reset();
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

void GtkPlayerWindow::refresh_display(bool update_text,
                                      bool update_progress) {
    if (ui_closing_) {
        return;
    }
    const PlaybackStatusSnapshot status = engine_.snapshot();
    refresh_display(status, update_text, update_progress);
}

void GtkPlayerWindow::refresh_display(const PlaybackStatusSnapshot& status,
                                      bool update_text,
                                      bool update_progress) {
    if (ui_closing_) {
        return;
    }

    refresh_stereo_tonal_dsp_controls(status.playing, status.format.channels);

    if (update_progress) {
        refresh_progress_display(status);
    }

    if (!update_text) {
        return;
    }

    std::string track_text = "Track: --";
    std::string status_text = status.message.empty() ? "Idle" : status.message;
    if (stream_manager_ != nullptr && !stream_manager_->status_override().empty()) {
        status_text = stream_manager_->status_override();
    }
    const std::string& displayed_device =
        status.playing && !active_output_device_.empty()
            ? active_output_device_
            : current_device_;
    std::string source_text = "Device: " + displayed_device;
    std::string path_text = "Path: --";

    if (source_scan_active_) {
        status_text = "Scanning folder...";
        path_text = "Path: discovering supported audio sources";
    } else if (pending_metadata_playback_valid()) {
        status_text = "Preparing track for playback...";
        path_text = "Path: probing metadata";
        if (!playlist_.empty() && pending_metadata_playback_.index < playlist_.size()) {
            const PlaylistEntry& pending = playlist_[pending_metadata_playback_.index];
            track_text = "Track " + std::to_string(pending.track_number) + ": " + display_title_for(pending);
        }
    } else if (metadata_loading_progress_visible(status.playing)) {
        const std::size_t percent = metadata_total_files_ == 0
            ? 100
            : std::min<std::size_t>(100, (metadata_completed_files_ * 100) / metadata_total_files_);
        status_text = "Loading metadata: " + std::to_string(metadata_completed_files_) + "/" +
                      std::to_string(metadata_total_files_) + " (" + std::to_string(percent) + "%)";
        path_text = "Path: metadata pending";
        if (!playlist_.empty() && current_track_index_ < playlist_.size()) {
            const PlaylistEntry& pending = playlist_[current_track_index_];
            track_text = "Track " + std::to_string(pending.track_number) + ": " + display_title_for(pending);
        }
    } else if (!playlist_.empty() && current_track_index_ < playlist_.size() && current_track_metadata_ready()) {
        const PlaylistEntry& track = playlist_[current_track_index_];
        track_text = "Track " + std::to_string(track.track_number) + ": " + display_title_for(track);

        const std::uint32_t shown_rate = active_transport_sample_rate(
            status.playing, status.format, track);
        const std::uint16_t shown_bits = status.playing && status.format.bits_per_sample > 0
            ? status.format.bits_per_sample
            : (track.decoded_format.bits_per_sample > 0
                   ? track.decoded_format.bits_per_sample
                   : track.source_bits_per_sample);
        const ActiveTrackTransportState* active_state =
            status.playing ? active_track_transport_state() : nullptr;
        if (active_state != nullptr && !active_state->processing_path.empty()) {
            path_text = active_state->processing_path;
        } else {
            AudioFormat configured_output = track.decoded_format;
            configured_output.sample_rate = shown_rate;
            configured_output.bits_per_sample = shown_bits;
            path_text = processing_path_for_entry(track, configured_output);
        }

        if (soft_volume_percent_ < 100 || bass_db_ != 0 || treble_db_ != 0 ||
            deep_bass_enabled_ || effective_pre_eq_headroom_tenths_db() > 0) {
            path_text += " → SoftDSP";
            if (bass_db_ != 0) path_text += " Bass " + std::to_string(bass_db_) + "dB";
            if (treble_db_ != 0) path_text += " Treble " + std::to_string(treble_db_) + "dB";
            if (deep_bass_enabled_) {
                path_text += " Deep Bass";
                if (deep_bass_amount_ != 0) {
                    path_text += " " + format_signed_step(deep_bass_amount_);
                }
            }
            if (soft_volume_percent_ < 100) {
                path_text += " Vol " + std::to_string(soft_volume_percent_) + "%";
            }
            if (effective_pre_eq_headroom_tenths_db() > 0) {
                path_text += " Pre-EQ Headroom " +
                    format_headroom_db_text(
                        static_cast<double>(effective_pre_eq_headroom_tenths_db()) / 10.0) +
                    "dB";
            }
        }
        const AlsaBufferPolicy buffer_policy =
            alsa_buffer_policy_for_sample_rate(shown_rate);
        const std::string& shown_device =
            status.playing && !active_output_device_.empty()
                ? active_output_device_
                : current_device_;
        path_text += " → ALSA " + shown_device + " (buffer " +
                     std::to_string(static_cast<unsigned long long>(buffer_policy.period_frames)) + "/" +
                     std::to_string(static_cast<unsigned long long>(buffer_policy.buffer_frames)) + ")";
    }

    set_label_text_if_changed(display_track_, safe_utf8_for_display(track_text));
    set_label_text_if_changed(display_status_, safe_utf8_for_display(status_text));
    set_label_text_if_changed(display_source_, safe_utf8_for_display(source_text));
    set_label_text_if_changed(display_path_, safe_utf8_for_display(path_text));
    if (!clip_detection_enabled_) {
        update_clip_indicator(false, 0);
    }

    const bool has_track = !source_scan_active_ &&
                           !metadata_loading_progress_visible(status.playing) &&
                           !pending_metadata_playback_valid() && current_track_metadata_ready();
    const ActiveTrackTransportState* badge_active_state =
        status.playing ? active_track_transport_state() : nullptr;
    const bool shown_redbook = status.playing
        ? status.format.is_red_book()
        : (has_track && playlist_[current_track_index_].decoded_format.is_red_book());
    const bool shown_native = badge_active_state != nullptr
        ? badge_active_state->native_decode
        : (has_track && playlist_[current_track_index_].native_decode);
    set_widget_opacity_if_changed(badge_lossless_, (has_track && playlist_[current_track_index_].lossless_source) ? 1.0 : 0.0);
    set_widget_opacity_if_changed(badge_redbook_, (has_track && shown_redbook) ? 1.0 : 0.0);
    set_widget_opacity_if_changed(badge_native_, (has_track && shown_native) ? 1.0 : 0.0);
    set_widget_opacity_if_changed(badge_dsp_, (soft_volume_percent_ < 100 || bass_db_ != 0 || treble_db_ != 0 || deep_bass_enabled_ || effective_pre_eq_headroom_tenths_db() > 0) ? 1.0 : 0.0);
    set_widget_opacity_if_changed(badge_random_, random_enabled_ ? 1.0 : 0.0);
    set_widget_opacity_if_changed(badge_repeat_, repeat_enabled_ ? 1.0 : 0.0);
}

void GtkPlayerWindow::refresh_progress_display(const PlaybackStatusSnapshot& status) {
    if (ui_closing_) {
        return;
    }

    display_progress_ratio_ = 0.0;
    std::uint64_t elapsed_seconds = 0;
    std::uint64_t total_seconds = 0;
    bool live_stream = false;
    if (!metadata_loading_progress_visible(status.playing) && !playlist_.empty() &&
        current_track_index_ < playlist_.size() && current_track_metadata_ready()) {
        const PlaylistEntry& track = playlist_[current_track_index_];
        const std::uint64_t track_length = active_track_length_samples(
            status.playing, status.total_samples_per_channel, track);
        const std::uint64_t track_position = current_track_position_from_status(status);
        const std::uint32_t playback_rate = active_transport_sample_rate(
            status.playing, status.format, track);
        const std::uint64_t safe_rate = playback_rate == 0
            ? 44100ULL
            : static_cast<std::uint64_t>(playback_rate);
        elapsed_seconds = track_position / safe_rate;
        if (track.is_stream) {
            live_stream = true;
        } else {
            if (track_length > 0) {
                display_progress_ratio_ = std::max(
                    0.0,
                    std::min(1.0,
                             static_cast<double>(track_position) /
                                 static_cast<double>(track_length)));
            }
            total_seconds = track_length / safe_rate;
        }
    }

    if (live_stream) {
        const std::string live_text = format_time_seconds(elapsed_seconds) + " / LIVE";
        if (live_text != display_time_text_ ||
            elapsed_seconds != display_elapsed_seconds_ ||
            display_total_seconds_ != std::numeric_limits<std::uint64_t>::max()) {
            display_elapsed_seconds_ = elapsed_seconds;
            display_total_seconds_ = std::numeric_limits<std::uint64_t>::max();
            display_time_text_ = live_text;
            if (display_time_ != nullptr) {
                set_label_text_if_changed(display_time_, display_time_text_);
            }
        }
    } else if (elapsed_seconds != display_elapsed_seconds_ ||
        total_seconds != display_total_seconds_) {
        display_elapsed_seconds_ = elapsed_seconds;
        display_total_seconds_ = total_seconds;
        display_time_text_ = format_time_seconds(elapsed_seconds) +
                             " / " + format_time_seconds(total_seconds);
        if (display_time_ != nullptr) {
            set_label_text_if_changed(display_time_, display_time_text_);
        }
    }
    if (progress_bar_ != nullptr) {
        gtk_widget_queue_draw(progress_bar_);
    }
}

bool GtkPlayerWindow::playlist_sort_available() const {
    return !playlist_filter_session_active_ &&
           (search_controller_ == nullptr || !search_controller_->is_filter_active());
}

std::optional<std::size_t> GtkPlayerWindow::selected_playlist_view_index() const {
    if (playlist_view_ == nullptr) {
        return std::nullopt;
    }
    GtkTreeSelection* selection =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(playlist_view_));
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        return std::nullopt;
    }
    std::size_t index = 0;
    if (!patches::playlist_index_from_model_iter(model, &iter, COL_INDEX, &index) ||
        index >= playlist_.size()) {
        return std::nullopt;
    }
    return index;
}

void GtkPlayerWindow::update_playlist_sort_headers() {
    const bool available = playlist_sort_available();
    const std::array<PlaylistSortKey, 5> keys{{
        PlaylistSortKey::TrackNumber,
        PlaylistSortKey::Artist,
        PlaylistSortKey::Title,
        PlaylistSortKey::Album,
        PlaylistSortKey::Source
    }};

    for (std::size_t i = 0; i < playlist_sort_columns_.size(); ++i) {
        GtkTreeViewColumn* column = playlist_sort_columns_[i];
        GtkWidget* label = playlist_sort_header_labels_[i];
        if (column == nullptr) {
            continue;
        }
        gtk_tree_view_column_set_clickable(column, available ? TRUE : FALSE);
        const bool active = playlist_sort_key_ == keys[i] &&
                            playlist_sort_direction_ != PlaylistSortDirection::Original;
        gtk_tree_view_column_set_sort_indicator(column, active ? TRUE : FALSE);
        if (active) {
            gtk_tree_view_column_set_sort_order(
                column,
                playlist_sort_direction_ == PlaylistSortDirection::Descending
                    ? GTK_SORT_DESCENDING
                    : GTK_SORT_ASCENDING);
        }
        if (label != nullptr) {
            gtk_widget_set_sensitive(label, available ? TRUE : FALSE);
            gtk_widget_set_opacity(label, available ? 1.0 : 0.48);
            gtk_widget_set_tooltip_text(
                label,
                available ? nullptr : "Sorting is unavailable while search is active");
        }
    }
}

void GtkPlayerWindow::reset_playlist_sort_state() {
    playlist_sort_key_ = PlaylistSortKey::None;
    playlist_sort_direction_ = PlaylistSortDirection::Original;
    update_playlist_sort_headers();
}

void GtkPlayerWindow::cycle_playlist_sort(PlaylistSortKey key) {
    if (!playlist_sort_available() || playlist_.empty() || key == PlaylistSortKey::None) {
        update_playlist_sort_headers();
        return;
    }

    PlaylistSortDirection next_direction = PlaylistSortDirection::Ascending;
    PlaylistSortKey next_key = key;
    if (playlist_sort_key_ == key) {
        if (playlist_sort_direction_ == PlaylistSortDirection::Ascending) {
            next_direction = PlaylistSortDirection::Descending;
        } else if (playlist_sort_direction_ == PlaylistSortDirection::Descending) {
            next_direction = PlaylistSortDirection::Original;
            next_key = PlaylistSortKey::None;
        }
    }

    apply_playlist_sort(next_key, next_direction, true);
}

void GtkPlayerWindow::remap_playlist_indices_after_reorder(
    const std::vector<std::size_t>& index_remap,
    std::size_t old_current_index,
    bool truncate_active_chain) {
    if (index_remap.empty()) {
        current_track_index_ = 0;
        reset_playlist_selection_state();
        clear_gapless_chain();
        return;
    }

    const auto remap = [&index_remap](std::size_t old_index) {
        return old_index < index_remap.size() ? index_remap[old_index]
                                              : std::size_t{0};
    };

    std::uint64_t current_segment_begin = 0;
    std::uint64_t current_segment_end = 0;
    std::size_t current_decoder_segment = 0;
    bool current_decoder_segment_valid = false;
    bool preserve_current_segment_mapping = false;
    bool preserve_full_chain_mapping = false;
    ActiveTrackTransportState preserved_transport_state;
    bool preserved_transport_state_valid = false;
    std::vector<std::size_t> preserved_chain_indices;
    std::vector<std::uint64_t> preserved_chain_offsets;
    std::vector<ActiveTrackTransportState> preserved_chain_states;
    std::uint64_t preserved_chain_total = 0;
    std::size_t preserved_chain_active_segment = 0;
    std::size_t preserved_chain_decoder_segment_base = 0;

    if (truncate_active_chain && gapless_chain_active_ &&
        gapless_chain_active_segment_ < gapless_chain_playlist_indices_.size() &&
        gapless_chain_active_segment_ < gapless_chain_offsets_.size() &&
        gapless_chain_playlist_indices_[gapless_chain_active_segment_] ==
            old_current_index) {
        const std::size_t relative = gapless_chain_active_segment_;
        current_segment_begin = gapless_chain_offsets_[relative];
        current_segment_end =
            relative + 1 < gapless_chain_offsets_.size()
                ? gapless_chain_offsets_[relative + 1]
                : gapless_chain_total_samples_;
        if (current_segment_end > current_segment_begin) {
            const PlaybackStatusSnapshot status = engine_.snapshot();
            const bool can_stop_after_decoder_segment =
                status.transport_truncation_kind ==
                    TransportTruncationKind::DecoderSegmentBoundary &&
                status.segment_position_valid;
            const bool can_stop_at_exact_sample =
                status.transport_truncation_kind ==
                    TransportTruncationKind::ExactSampleBoundary;

            if (can_stop_after_decoder_segment || can_stop_at_exact_sample) {
                if (relative < active_track_transport_states_.size()) {
                    preserved_transport_state =
                        active_track_transport_states_[relative];
                    preserved_transport_state_valid = true;
                }
                if (can_stop_after_decoder_segment) {
                    current_decoder_segment = status.segment_index;
                    current_decoder_segment_valid = true;
                    engine_.request_stop_after_segment(current_decoder_segment);
                } else {
                    engine_.request_stop_after_current_segment(
                        current_segment_end);
                }
                preserve_current_segment_mapping = true;
            } else {
                preserved_chain_indices = gapless_chain_playlist_indices_;
                preserved_chain_offsets = gapless_chain_offsets_;
                preserved_chain_states = active_track_transport_states_;
                preserved_chain_total = gapless_chain_total_samples_;
                preserved_chain_active_segment = gapless_chain_active_segment_;
                preserved_chain_decoder_segment_base =
                    gapless_chain_decoder_segment_base_;
                preserve_full_chain_mapping = true;
                Logger::instance().debug(
                    "Playlist sorting keeps the complete active continuous "
                    "single-source chain; the new order applies after its EOF");
            }
        }
    }

    current_track_index_ = remap(old_current_index);
    selected_playlist_index_ = remap(selected_playlist_index_);
    playlist_selection_index_before_filter_candidate_ =
        remap(playlist_selection_index_before_filter_candidate_);
    playlist_filter_session_selection_index_ =
        remap(playlist_filter_session_selection_index_);
    playlist_filter_session_committed_index_ =
        remap(playlist_filter_session_committed_index_);

    if (pending_metadata_playback_.active) {
        pending_metadata_playback_.index = remap(pending_metadata_playback_.index);
    }
    if (pending_seek_source_id_ != 0 || pending_seek_valid_) {
        pending_seek_index_ = remap(pending_seek_index_);
    }

    if (preserve_full_chain_mapping) {
        for (std::size_t& index : preserved_chain_indices) {
            index = remap(index);
        }
        set_gapless_chain_mapping(preserved_chain_indices,
                                  preserved_chain_offsets,
                                  preserved_chain_total,
                                  preserved_chain_active_segment,
                                  preserved_chain_decoder_segment_base);
        active_track_transport_states_ = std::move(preserved_chain_states);
    } else if (preserve_current_segment_mapping) {
        set_gapless_chain_mapping({current_track_index_},
                                  {current_segment_begin},
                                  current_segment_end,
                                  0,
                                  current_decoder_segment_valid
                                      ? current_decoder_segment
                                      : 0);
        if (preserved_transport_state_valid) {
            active_track_transport_states_ = {preserved_transport_state};
        }
    } else {
        clear_gapless_chain();
    }
}

void GtkPlayerWindow::apply_playlist_sort(PlaylistSortKey key,
                                          PlaylistSortDirection direction,
                                          bool center_selection) {
    if (playlist_.empty()) {
        playlist_sort_key_ = key;
        playlist_sort_direction_ = direction;
        update_playlist_sort_headers();
        return;
    }
    if (!playlist_sort_available() && center_selection) {
        update_playlist_sort_headers();
        return;
    }

    for (PlaylistEntry& entry : playlist_) {
        if (entry.original_order == 0) {
            entry.original_order = next_playlist_original_order_++;
        }
    }

    const std::optional<std::size_t> selected_before = selected_playlist_view_index();
    const std::size_t old_current_index = current_track_index_;
    const std::vector<std::uint64_t> old_order = [&]() {
        std::vector<std::uint64_t> result;
        result.reserve(playlist_.size());
        for (const PlaylistEntry& entry : playlist_) {
            result.push_back(entry.original_order);
        }
        return result;
    }();

    const bool descending = direction == PlaylistSortDirection::Descending;
    const auto compare_entries = [key, direction, descending](const PlaylistEntry& left,
                                                              const PlaylistEntry& right) {
        if (direction == PlaylistSortDirection::Original || key == PlaylistSortKey::None) {
            return left.original_order < right.original_order;
        }

        int comparison = 0;
        const auto compare_text = [descending](const std::string& a,
                                               const std::string& b,
                                               bool natural = false) {
            return compare_playlist_text(a, b, natural, descending);
        };
        const auto compare_track = [descending](int a, int b) {
            return compare_playlist_track_number(a, b, descending);
        };

        switch (key) {
            case PlaylistSortKey::TrackNumber:
                comparison = compare_track(left.track_number, right.track_number);
                break;
            case PlaylistSortKey::Artist:
                comparison = compare_text(left.performer, right.performer);
                if (comparison == 0) comparison = compare_text(left.album, right.album);
                if (comparison == 0) comparison = compare_track(left.track_number, right.track_number);
                if (comparison == 0) comparison = compare_text(left.title, right.title);
                break;
            case PlaylistSortKey::Title:
                comparison = compare_text(left.title, right.title);
                if (comparison == 0) comparison = compare_text(left.performer, right.performer);
                if (comparison == 0) comparison = compare_text(left.album, right.album);
                break;
            case PlaylistSortKey::Album:
                comparison = compare_text(left.album, right.album);
                if (comparison == 0) comparison = compare_track(left.track_number, right.track_number);
                if (comparison == 0) comparison = compare_text(left.title, right.title);
                break;
            case PlaylistSortKey::Source:
                comparison = compare_text(left.audio_file_path, right.audio_file_path, true);
                break;
            case PlaylistSortKey::None:
                break;
        }
        return comparison < 0;
    };

    std::stable_sort(playlist_.begin(), playlist_.end(), compare_entries);
    rebuild_playlist_entry_indexes();

    std::vector<std::size_t> index_remap(old_order.size(), 0);
    for (std::size_t old_index = 0; old_index < old_order.size(); ++old_index) {
        const auto found = playlist_index_by_entry_id_.find(old_order[old_index]);
        if (found != playlist_index_by_entry_id_.end()) {
            index_remap[old_index] = found->second;
        }
    }

    remap_playlist_indices_after_reorder(index_remap,
                                         old_current_index,
                                         gapless_chain_active_);
    playlist_sort_key_ = key;
    playlist_sort_direction_ = direction;
    rebuild_playlist_view(!center_selection);

    std::size_t target_index = current_track_index_;
    if (selected_before.has_value() && *selected_before < index_remap.size()) {
        target_index = index_remap[*selected_before];
    }
    if (target_index < playlist_.size()) {
        select_playlist_row(target_index,
                            center_selection
                                ? PlaylistScrollPolicy::Center
                                : PlaylistScrollPolicy::PreserveViewport);
    }
    update_playlist_sort_headers();
    refresh_display();
    notify_mpris_state_changed();
}

void GtkPlayerWindow::rebuild_playlist_view(bool reset_column_widths) {
    {
        PlaylistSelectionSignalBlocker selection_blocker(*this);
        gtk_list_store_clear(playlist_store_);
        for (std::size_t i = 0; i < playlist_.size(); ++i) {
            GtkTreeIter iter;
            gtk_list_store_append(playlist_store_, &iter);
            const PlaylistEntry& entry = playlist_[i];
            const PlaylistStreamRowValues row =
                stream_manager_ != nullptr
                    ? playlist_stream_row_values(*stream_manager_,
                                                          entry.is_stream,
                                                          entry.audio_file_path,
                                                          entry.track_number)
                    : PlaylistStreamRowValues{std::to_string(entry.track_number), FALSE};
            const std::string artist = safe_utf8_for_display(entry.performer);
            const std::string album = safe_utf8_for_display(entry.album);
            const std::string title = safe_utf8_for_display(entry.title);
            const std::string source = safe_utf8_for_display(entry.source_label);
            std::string search_folded;
            const gchar* search_folded_value = nullptr;
            if (playlist_search_enabled_) {
                search_folded = build_playlist_search_folded(artist, album, title);
                search_folded_value = search_folded.c_str();
            }
            gtk_list_store_set(playlist_store_, &iter,
                               COL_INDEX, static_cast<int>(i),
                               COL_TRACKNO, row.track_number.c_str(),
                               COL_ARTIST, artist.c_str(),
                               COL_ALBUM, album.c_str(),
                               COL_TITLE, title.c_str(),
                               COL_SOURCE, source.c_str(),
                               COL_SEARCH_FOLDED, search_folded_value,
                               COL_STREAM_BROKEN, row.stream_broken,
                               -1);
        }
    }

    if (search_controller_ != nullptr && playlist_search_enabled_ &&
        search_controller_->is_filter_active()) {
        sync_playlist_selection_to_filter();
    }
    if (playlist_field_width_limit_enabled_) {
        apply_playlist_field_width_limit(reset_column_widths);
    } else if (reset_column_widths) {
        reset_playlist_column_widths();
    }
}

void GtkPlayerWindow::sync_playlist_field_renderer_binding() {
    const bool use_cell_data = playlist_field_width_limit_enabled_;
    if (playlist_field_cell_data_active_ == use_cell_data) {
        return;
    }

    const std::array<GtkCellRenderer*, 4> renderers = {{
        playlist_artist_renderer_,
        playlist_title_renderer_,
        playlist_album_renderer_,
        playlist_source_renderer_
    }};
    const std::array<GtkTreeViewColumn*, 4> columns = {{
        playlist_artist_column_,
        playlist_title_column_,
        playlist_album_column_,
        playlist_source_column_
    }};

    for (std::size_t index = 0; index < columns.size(); ++index) {
        GtkTreeViewColumn* column = columns[index];
        GtkCellRenderer* renderer = renderers[index];
        if (column == nullptr || renderer == nullptr) {
            continue;
        }

        // The normal stream/playing styler and the limited presentation callback are
        // two alternative renderer bindings. Keep them mutually exclusive so
        // changing the setting on an already realized TreeView takes effect
        // immediately without rebuilding the playlist model.
        gtk_tree_view_column_set_cell_data_func(column, renderer, nullptr, nullptr, nullptr);
        gtk_tree_view_column_clear_attributes(column, renderer);
        if (use_cell_data) {
            gtk_tree_view_column_set_cell_data_func(
                column,
                renderer,
                GtkPlayerWindow::on_playlist_field_cell_data,
                this,
                nullptr);
        }
        gtk_tree_view_column_queue_resize(column);
    }

    if (!use_cell_data && playlist_view_ != nullptr) {
        install_playlist_stream_styling(GTK_TREE_VIEW(playlist_view_),
                                        nullptr,
                                        playlist_artist_column_,
                                        playlist_title_column_,
                                        playlist_album_column_,
                                        playlist_source_column_,
                                        -1,
                                        COL_ARTIST,
                                        COL_TITLE,
                                        COL_ALBUM,
                                        COL_SOURCE,
                                        &current_track_index_);
    }

    playlist_field_cell_data_active_ = use_cell_data;
}

void GtkPlayerWindow::apply_playlist_field_width_limit(bool reset_column_widths) {
    const int user_limit_chars = std::max(
        kMinPlaylistFieldWidthChars,
        std::min(kMaxPlaylistFieldWidthChars, playlist_field_width_chars_));

    const std::array<GtkCellRenderer*, 4> renderers = {{
        playlist_artist_renderer_,
        playlist_title_renderer_,
        playlist_album_renderer_,
        playlist_source_renderer_
    }};
    const std::array<GtkTreeViewColumn*, 4> columns = {{
        playlist_artist_column_,
        playlist_title_column_,
        playlist_album_column_,
        playlist_source_column_
    }};

    sync_playlist_field_renderer_binding();

    const auto set_renderer_presentation = [](GtkCellRenderer* renderer,
                                              PangoEllipsizeMode ellipsize) {
        if (renderer != nullptr) {
            g_object_set(renderer,
                         "width-chars", -1,
                         "max-width-chars", -1,
                         "ellipsize", ellipsize,
                         nullptr);
        }
    };

    if (!playlist_field_width_limit_enabled_) {
        playlist_field_width_initial_caps_.fill(-1);
        for (GtkCellRenderer* renderer : renderers) {
            set_renderer_presentation(renderer, PANGO_ELLIPSIZE_NONE);
        }
        if (playlist_field_width_spacer_column_ != nullptr) {
            gtk_tree_view_column_set_visible(playlist_field_width_spacer_column_, FALSE);
            gtk_tree_view_column_set_expand(playlist_field_width_spacer_column_, FALSE);
        }
        if (reset_column_widths) {
            reset_playlist_column_widths();
        }
        return;
    }

    // Limiting is a presentation rule only. Full strings stay in the model for
    // search, sorting, metadata, MPRIS and playback. The cell-data function
    // enables ellipsis only for an actually limited column or after an explicit
    // user resize; fields already within the configured limit therefore retain
    // the same natural GTK sizing as the normal layout.
    for (GtkCellRenderer* renderer : renderers) {
        set_renderer_presentation(renderer, PANGO_ELLIPSIZE_NONE);
    }

    std::array<int, 4> longest_chars = {{6, 5, 5, 6}}; // Header text lengths.
    const auto display_chars = [](const std::string& value) {
        const std::string display = safe_utf8_for_display(value);
        const glong length = g_utf8_strlen(display.c_str(), -1);
        return length > 0
            ? static_cast<int>(std::min<glong>(length, std::numeric_limits<int>::max()))
            : 0;
    };
    for (const PlaylistEntry& entry : playlist_) {
        longest_chars[0] = std::max(longest_chars[0], display_chars(entry.performer));
        int title_chars = display_chars(entry.title);
        if (entry.metadata_state == MetadataState::Failed) {
            title_chars += 14; // " [unavailable]"
        }
        longest_chars[1] = std::max(longest_chars[1], title_chars);
        longest_chars[2] = std::max(longest_chars[2], display_chars(entry.album));
        longest_chars[3] = std::max(longest_chars[3], display_chars(entry.source_label));
    }

    std::array<bool, 4> limited = {{false, false, false, false}};
    bool any_limited = false;
    for (std::size_t index = 0; index < limited.size(); ++index) {
        limited[index] = longest_chars[index] > user_limit_chars;
        any_limited = any_limited || limited[index];
    }

    const bool use_spacer = any_limited && playlist_field_width_spacer_column_ != nullptr;
    if (playlist_field_width_spacer_column_ != nullptr) {
        if (use_spacer) {
            // A hidden TreeView column can report a zero preferred width for
            // its GTK-owned header button. Restore normal sizing before making
            // the spacer visible, then measure the realized theme geometry and
            // only afterwards lock the spacer to a safe minimum width. This
            // avoids a transient 6 px fixed allocation during live OFF -> ON.
            gtk_tree_view_column_set_expand(playlist_field_width_spacer_column_, FALSE);
            gtk_tree_view_column_set_sizing(
                playlist_field_width_spacer_column_, GTK_TREE_VIEW_COLUMN_GROW_ONLY);
            gtk_tree_view_column_set_fixed_width(playlist_field_width_spacer_column_, -1);
            gtk_tree_view_column_set_min_width(
                playlist_field_width_spacer_column_, kPlaylistFieldWidthSpacerMinPixels);
            gtk_tree_view_column_set_visible(playlist_field_width_spacer_column_, TRUE);

            const int spacer_min_width = playlist_field_width_spacer_min_width(
                playlist_field_width_spacer_column_);
            gtk_tree_view_column_set_sizing(
                playlist_field_width_spacer_column_, GTK_TREE_VIEW_COLUMN_FIXED);
            gtk_tree_view_column_set_fixed_width(
                playlist_field_width_spacer_column_, spacer_min_width);
            gtk_tree_view_column_set_min_width(
                playlist_field_width_spacer_column_, spacer_min_width);
            gtk_tree_view_column_set_expand(playlist_field_width_spacer_column_, TRUE);
        } else {
            gtk_tree_view_column_set_visible(playlist_field_width_spacer_column_, FALSE);
            gtk_tree_view_column_set_expand(playlist_field_width_spacer_column_, FALSE);
        }
        gtk_tree_view_column_queue_resize(playlist_field_width_spacer_column_);
    }

    // Sorting rebuilds the same model values and must preserve any manual
    // column widths. A new/opened playlist or settings change explicitly
    // requests a reset and establishes the configured initial geometry again.
    if (!reset_column_widths) {
        if (playlist_view_ != nullptr) {
            gtk_widget_queue_draw(playlist_view_);
            gtk_widget_queue_resize(playlist_view_);
        }
        return;
    }

    std::array<int, 4> initial_pixel_widths = {{-1, -1, -1, -1}};
    if (playlist_view_ != nullptr) {
        // Only columns that actually exceed the configured character limit need
        // a custom initial width. Fields already within the limit stay on normal
        // GTK GROW_ONLY sizing, exactly like the limit-off path. For a limited
        // column, measure the complete GtkTreeViewColumn cell layout rather than
        // a standalone PangoLayout so renderer padding and column cell geometry
        // are included by GTK itself.
        const auto measure_column_text = [this](GtkTreeViewColumn* column,
                                                GtkCellRenderer* renderer,
                                                const std::string& value) {
            if (column == nullptr || renderer == nullptr || playlist_view_ == nullptr) {
                return -1;
            }

            g_object_set(renderer,
                         "text", value.c_str(),
                         "ellipsize", PANGO_ELLIPSIZE_NONE,
                         nullptr);
            int width = 0;
            gtk_tree_view_column_cell_get_size(
                column, nullptr, nullptr, nullptr, &width, nullptr);
            return std::max(0, width);
        };

        const auto presentation_for = [user_limit_chars](const std::string& value) {
            return playlist_field_limited_presentation(value, user_limit_chars);
        };

        for (const PlaylistEntry& entry : playlist_) {
            if (limited[0]) {
                initial_pixel_widths[0] = std::max(
                    initial_pixel_widths[0],
                    measure_column_text(
                        columns[0], renderers[0], presentation_for(entry.performer)));
            }

            if (limited[1]) {
                std::string title = entry.title;
                if (entry.metadata_state == MetadataState::Failed) {
                    title += " [unavailable]";
                }
                initial_pixel_widths[1] = std::max(
                    initial_pixel_widths[1],
                    measure_column_text(columns[1], renderers[1], presentation_for(title)));
            }

            if (limited[2]) {
                initial_pixel_widths[2] = std::max(
                    initial_pixel_widths[2],
                    measure_column_text(
                        columns[2], renderers[2], presentation_for(entry.album)));
            }

            if (limited[3]) {
                initial_pixel_widths[3] = std::max(
                    initial_pixel_widths[3],
                    measure_column_text(
                        columns[3], renderers[3], presentation_for(entry.source_label)));
            }
        }
    }

    playlist_field_width_initial_caps_.fill(-1);
    for (std::size_t index = 0; index < renderers.size(); ++index) {
        GtkCellRenderer* renderer = renderers[index];
        GtkTreeViewColumn* column = columns[index];
        if (renderer == nullptr || column == nullptr) {
            continue;
        }

        if (!limited[index]) {
            // No value in this field exceeds the configured character limit.
            // Leave sizing to GTK exactly as in the normal layout; the cell-data
            // function will only enable ellipsis if the user later resizes the
            // column explicitly (GTK records that action in fixed-width).
            gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_GROW_ONLY);
            gtk_tree_view_column_set_fixed_width(column, -1);
            gtk_tree_view_column_set_max_width(column, -1);
            gtk_tree_view_column_set_expand(
                column, !use_spacer && column == playlist_expand_column_ ? TRUE : FALSE);
            gtk_tree_view_column_queue_resize(column);
            continue;
        }

        int requested_width = initial_pixel_widths[index];

        // Preserve the same header padding and theme geometry as the normal
        // TreeView path. The header button is a GTK-owned widget, so its natural
        // width already includes the current theme's padding and sort-button
        // decoration; no guessed pixel constant is required.
        GtkWidget* header_button = gtk_tree_view_column_get_button(column);
        if (header_button != nullptr) {
            int header_minimum = 0;
            int header_natural = 0;
            gtk_widget_get_preferred_width(header_button, &header_minimum, &header_natural);
            requested_width = std::max(requested_width, header_natural);
        }

        if (requested_width > 0) {
            gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
            gtk_tree_view_column_set_fixed_width(column, requested_width);
            gtk_tree_view_column_set_max_width(column, -1);
            gtk_tree_view_column_set_expand(column, FALSE);
            playlist_field_width_initial_caps_[index] = requested_width;
        } else {
            // If GTK cannot measure the limited cell layout, preserve normal
            // sizing rather than inventing a pixel width or truncating content.
            gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_GROW_ONLY);
            gtk_tree_view_column_set_fixed_width(column, -1);
            gtk_tree_view_column_set_max_width(column, -1);
            gtk_tree_view_column_set_expand(
                column, !use_spacer && column == playlist_expand_column_ ? TRUE : FALSE);
        }
        gtk_tree_view_column_queue_resize(column);
    }

    if (playlist_scrolled_ != nullptr) {
        GtkAdjustment* adjustment =
            gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(playlist_scrolled_));
        if (adjustment != nullptr) {
            gtk_adjustment_set_value(adjustment, gtk_adjustment_get_lower(adjustment));
        }
    }
    if (playlist_view_ != nullptr) {
        gtk_widget_queue_draw(playlist_view_);
        gtk_widget_queue_resize(playlist_view_);
    }
}

void GtkPlayerWindow::reset_playlist_column_widths() {
    if (playlist_view_ == nullptr) {
        return;
    }

    playlist_field_width_initial_caps_.fill(-1);

    if (playlist_field_width_spacer_column_ != nullptr) {
        gtk_tree_view_column_set_visible(playlist_field_width_spacer_column_, FALSE);
        gtk_tree_view_column_set_expand(playlist_field_width_spacer_column_, FALSE);
        gtk_tree_view_column_set_sizing(
            playlist_field_width_spacer_column_, GTK_TREE_VIEW_COLUMN_GROW_ONLY);
        gtk_tree_view_column_set_fixed_width(playlist_field_width_spacer_column_, -1);
    }

    GList* columns = gtk_tree_view_get_columns(GTK_TREE_VIEW(playlist_view_));
    for (GList* node = columns; node != nullptr; node = node->next) {
        GtkTreeViewColumn* column = GTK_TREE_VIEW_COLUMN(node->data);
        gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_GROW_ONLY);
        gtk_tree_view_column_set_fixed_width(column, -1);
        gtk_tree_view_column_set_expand(column, column == playlist_expand_column_ ? TRUE : FALSE);
        gtk_tree_view_column_queue_resize(column);
    }
    g_list_free(columns);

    if (playlist_scrolled_ != nullptr) {
        GtkAdjustment* adjustment =
            gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(playlist_scrolled_));
        if (adjustment != nullptr) {
            gtk_adjustment_set_value(adjustment, gtk_adjustment_get_lower(adjustment));
        }
    }
    gtk_widget_queue_resize(playlist_view_);
}

void GtkPlayerWindow::rebuild_playlist_search_cache() {
    if (!playlist_search_enabled_ || playlist_store_ == nullptr) {
        return;
    }

    PlaylistSelectionSignalBlocker selection_blocker(*this);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(playlist_store_), &iter);
    while (valid) {
        int index = -1;
        gtk_tree_model_get(GTK_TREE_MODEL(playlist_store_), &iter, COL_INDEX, &index, -1);
        if (index >= 0 && static_cast<std::size_t>(index) < playlist_.size()) {
            const PlaylistEntry& entry = playlist_[static_cast<std::size_t>(index)];
            const std::string artist = safe_utf8_for_display(entry.performer);
            const std::string album = safe_utf8_for_display(entry.album);
            std::string title = safe_utf8_for_display(entry.title);
            if (entry.metadata_state == MetadataState::Failed) {
                title += " [unavailable]";
            }
            const std::string folded = build_playlist_search_folded(artist, album, title);
            gtk_list_store_set(playlist_store_, &iter,
                               COL_SEARCH_FOLDED, folded.c_str(),
                               -1);
        }
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(playlist_store_), &iter);
    }
}

void GtkPlayerWindow::clear_playlist_search_cache() {
    if (playlist_store_ == nullptr) {
        return;
    }

    PlaylistSelectionSignalBlocker selection_blocker(*this);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(playlist_store_), &iter);
    while (valid) {
        gtk_list_store_set(playlist_store_, &iter,
                           COL_SEARCH_FOLDED, static_cast<const gchar*>(nullptr),
                           -1);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(playlist_store_), &iter);
    }
}

void GtkPlayerWindow::update_playlist_row(std::size_t index) {
    if (playlist_store_ == nullptr || index >= playlist_.size()) {
        return;
    }
    GtkTreePath* tree_path = gtk_tree_path_new_from_indices(static_cast<int>(index), -1);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(playlist_store_), &iter, tree_path)) {
        gtk_tree_path_free(tree_path);
        return;
    }
    gtk_tree_path_free(tree_path);

    const PlaylistEntry& entry = playlist_[index];
    const PlaylistStreamRowValues row =
        stream_manager_ != nullptr
            ? playlist_stream_row_values(*stream_manager_,
                                                  entry.is_stream,
                                                  entry.audio_file_path,
                                                  entry.track_number)
            : PlaylistStreamRowValues{std::to_string(entry.track_number), FALSE};
    const std::string artist = safe_utf8_for_display(entry.performer);
    const std::string album = safe_utf8_for_display(entry.album);
    std::string title = safe_utf8_for_display(entry.title);
    if (entry.metadata_state == MetadataState::Failed) {
        title += " [unavailable]";
    }
    const std::string source = safe_utf8_for_display(entry.source_label);
    std::string search_folded;
    const gchar* search_folded_value = nullptr;
    if (playlist_search_enabled_) {
        search_folded = build_playlist_search_folded(artist, album, title);
        search_folded_value = search_folded.c_str();
    }

    {
        PlaylistSelectionSignalBlocker selection_blocker(*this);
        gtk_list_store_set(playlist_store_, &iter,
                           COL_INDEX, static_cast<int>(index),
                           COL_TRACKNO, row.track_number.c_str(),
                           COL_ARTIST, artist.c_str(),
                           COL_ALBUM, album.c_str(),
                           COL_TITLE, title.c_str(),
                           COL_SOURCE, source.c_str(),
                           COL_SEARCH_FOLDED, search_folded_value,
                           COL_STREAM_BROKEN, row.stream_broken,
                           -1);
    }

    if (search_controller_ != nullptr && playlist_search_enabled_ &&
        search_controller_->is_filter_active()) {
        sync_playlist_selection_to_filter();
    }
}

bool GtkPlayerWindow::capture_playlist_vertical_position(double* value) const {
    if (value == nullptr || playlist_scrolled_ == nullptr) {
        return false;
    }
    GtkAdjustment* adjustment =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(playlist_scrolled_));
    if (adjustment == nullptr) {
        return false;
    }
    *value = gtk_adjustment_get_value(adjustment);
    return true;
}

void GtkPlayerWindow::restore_playlist_vertical_position(double value) {
    playlist_vertical_position_restore_value_ = value;

    if (playlist_scrolled_ != nullptr) {
        GtkAdjustment* adjustment =
            gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(playlist_scrolled_));
        if (adjustment != nullptr) {
            const double lower = gtk_adjustment_get_lower(adjustment);
            const double upper = gtk_adjustment_get_upper(adjustment);
            const double page_size = gtk_adjustment_get_page_size(adjustment);
            const double maximum = std::max(lower, upper - page_size);
            gtk_adjustment_set_value(adjustment,
                                     std::min(std::max(value, lower), maximum));
        }
    }

    if (playlist_vertical_position_restore_idle_id_ == 0 && !ui_closing_) {
        playlist_vertical_position_restore_idle_id_ =
            g_idle_add_full(G_PRIORITY_LOW,
                            GtkPlayerWindow::on_playlist_vertical_position_restore_idle,
                            this,
                            nullptr);
    }
}

gboolean GtkPlayerWindow::on_playlist_vertical_position_restore_idle(gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr) {
        return G_SOURCE_REMOVE;
    }

    self->playlist_vertical_position_restore_idle_id_ = 0;
    if (self->ui_closing_ || self->playlist_scrolled_ == nullptr) {
        return G_SOURCE_REMOVE;
    }

    GtkAdjustment* adjustment =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->playlist_scrolled_));
    if (adjustment == nullptr) {
        return G_SOURCE_REMOVE;
    }

    const double lower = gtk_adjustment_get_lower(adjustment);
    const double upper = gtk_adjustment_get_upper(adjustment);
    const double page_size = gtk_adjustment_get_page_size(adjustment);
    const double maximum = std::max(lower, upper - page_size);
    gtk_adjustment_set_value(adjustment,
                             std::min(std::max(self->playlist_vertical_position_restore_value_, lower),
                                      maximum));
    return G_SOURCE_REMOVE;
}

void GtkPlayerWindow::cancel_playlist_vertical_position_restore() {
    if (playlist_vertical_position_restore_idle_id_ != 0) {
        g_source_remove(playlist_vertical_position_restore_idle_id_);
        playlist_vertical_position_restore_idle_id_ = 0;
    }
}

bool GtkPlayerWindow::select_playlist_row(std::size_t index,
                                          PlaylistScrollPolicy scroll_policy) {
    if (playlist_view_ == nullptr) {
        return false;
    }

    GtkTreePath* path = nullptr;
    if (!patches::find_playlist_view_path_for_index(GTK_TREE_VIEW(playlist_view_), index, COL_INDEX, &path) ||
        path == nullptr) {
        return false;
    }

    double preserved_scroll_value = 0.0;
    const bool preserve_scroll = scroll_policy == PlaylistScrollPolicy::PreserveViewport;
    const bool preserved_scroll_valid =
        preserve_scroll && capture_playlist_vertical_position(&preserved_scroll_value);
    if (!preserve_scroll) {
        cancel_playlist_vertical_position_restore();
    }

    GtkTreeView* view = GTK_TREE_VIEW(playlist_view_);
    GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
    const auto apply_selection = [&]() {
        gtk_tree_selection_unselect_all(selection);
        gtk_tree_selection_select_path(selection, path);
        gtk_tree_view_set_cursor(view, path, nullptr, FALSE);
        if (scroll_policy == PlaylistScrollPolicy::Center) {
            gtk_tree_view_scroll_to_cell(view, path, nullptr, TRUE, 0.5f, 0.0f);
        } else if (scroll_policy == PlaylistScrollPolicy::EnsureVisible) {
            gtk_tree_view_scroll_to_cell(view, path, nullptr, FALSE, 0.0f, 0.0f);
        }
    };

    PlaylistSelectionSignalBlocker selection_blocker(*this);
    apply_selection();

    gtk_tree_path_free(path);
    if (preserved_scroll_valid) {
        restore_playlist_vertical_position(preserved_scroll_value);
    }
    return true;
}

void GtkPlayerWindow::reset_playlist_selection_state(std::size_t index) {
    playlist_selection_mode_ = PlaylistSelectionMode::FollowTransport;
    selected_playlist_index_ = index;
    playlist_selection_mode_before_filter_candidate_ = PlaylistSelectionMode::FollowTransport;
    playlist_selection_index_before_filter_candidate_ = index;
    playlist_filter_candidate_valid_ = false;
    if (playlist_filter_session_active_) {
        playlist_filter_session_selection_mode_ = PlaylistSelectionMode::FollowTransport;
        playlist_filter_session_selection_index_ = index;
        playlist_filter_session_scroll_valid_ = false;
        playlist_filter_session_playback_committed_ = false;
        playlist_filter_session_committed_index_ = index;
    }
}

void GtkPlayerWindow::set_explicit_playlist_selection(std::size_t index) {
    if (index >= playlist_.size()) {
        return;
    }
    const bool transport_stopped = !engine_.is_playing();
    const bool mpris_selection_changed =
        transport_stopped && mpris_playlist_index(false) != index;
    cancel_pending_last_active_track_restore();
    clear_random_stopped_preview();
    playlist_selection_mode_ = PlaylistSelectionMode::ExplicitUser;
    selected_playlist_index_ = index;
    playlist_selection_mode_before_filter_candidate_ = PlaylistSelectionMode::ExplicitUser;
    playlist_selection_index_before_filter_candidate_ = index;
    playlist_filter_candidate_valid_ = false;

    if (pending_metadata_playback_valid()) {
        pending_metadata_playback_.preserve_explicit_selection = true;
    }
    if (mpris_selection_changed) {
        mark_mpris_track_changed();
    }
    if (transport_stopped) {
        refresh_stereo_tonal_dsp_controls(false, 0);
    }
}

void GtkPlayerWindow::set_filter_candidate_selection(std::size_t index) {
    if (index >= playlist_.size()) {
        return;
    }
    const bool transport_stopped = !engine_.is_playing();
    const bool mpris_selection_changed =
        transport_stopped && mpris_playlist_index(false) != index;
    cancel_pending_last_active_track_restore();
    clear_random_stopped_preview();
    if (!playlist_filter_session_active_) {
        begin_playlist_filter_session();
    }

    playlist_selection_mode_before_filter_candidate_ =
        playlist_filter_session_selection_mode_;
    playlist_selection_index_before_filter_candidate_ =
        playlist_filter_session_selection_index_;
    playlist_selection_mode_ = PlaylistSelectionMode::FilterCandidate;
    selected_playlist_index_ = index;
    playlist_filter_candidate_valid_ = true;
    if (mpris_selection_changed) {
        mark_mpris_track_changed();
    }
    if (transport_stopped) {
        refresh_stereo_tonal_dsp_controls(false, 0);
    }
}

GtkPlayerWindow::PlaylistSelectionMode
GtkPlayerWindow::playlist_selection_mode_without_filter_candidate() const {
    if (playlist_selection_mode_ == PlaylistSelectionMode::FilterCandidate) {
        if (playlist_filter_session_active_) {
            return playlist_filter_session_selection_mode_;
        }
        return playlist_selection_mode_before_filter_candidate_;
    }
    return playlist_selection_mode_;
}

std::size_t GtkPlayerWindow::playlist_selection_index_without_filter_candidate() const {
    if (playlist_.empty()) {
        return 0;
    }

    if (playlist_selection_mode_ == PlaylistSelectionMode::FilterCandidate) {
        const PlaylistSelectionMode mode = playlist_filter_session_active_
            ? playlist_filter_session_selection_mode_
            : playlist_selection_mode_before_filter_candidate_;
        const std::size_t index = playlist_filter_session_active_
            ? playlist_filter_session_selection_index_
            : playlist_selection_index_before_filter_candidate_;
        if (mode == PlaylistSelectionMode::ExplicitUser && index < playlist_.size()) {
            return index;
        }
        return std::min(current_track_index_, playlist_.size() - 1);
    }

    if (playlist_selection_mode_ == PlaylistSelectionMode::ExplicitUser &&
        selected_playlist_index_ < playlist_.size()) {
        return selected_playlist_index_;
    }
    return std::min(current_track_index_, playlist_.size() - 1);
}

std::size_t GtkPlayerWindow::playlist_play_target_index() const {
    if (playlist_.empty()) {
        return 0;
    }
    if (playlist_selection_mode_ == PlaylistSelectionMode::FilterCandidate &&
        playlist_filter_candidate_valid_ &&
        selected_playlist_index_ < playlist_.size()) {
        return selected_playlist_index_;
    }
    return playlist_selection_index_without_filter_candidate();
}

void GtkPlayerWindow::begin_playlist_filter_session() {
    if (playlist_filter_session_active_) {
        return;
    }

    cancel_pending_last_active_track_restore();
    playlist_filter_session_selection_mode_ =
        playlist_selection_mode_without_filter_candidate();
    playlist_filter_session_selection_index_ =
        playlist_selection_index_without_filter_candidate();
    playlist_filter_session_scroll_valid_ =
        capture_playlist_vertical_position(&playlist_filter_session_scroll_value_);
    playlist_filter_session_playback_committed_ = false;
    playlist_filter_session_committed_index_ = playlist_filter_session_selection_index_;
    playlist_filter_session_active_ = true;

    playlist_selection_mode_before_filter_candidate_ =
        playlist_filter_session_selection_mode_;
    playlist_selection_index_before_filter_candidate_ =
        playlist_filter_session_selection_index_;
    playlist_filter_candidate_valid_ = false;
}

void GtkPlayerWindow::mark_playlist_filter_playback_committed(std::size_t index) {
    if (!playlist_filter_session_active_ || index >= playlist_.size()) {
        return;
    }
    playlist_filter_session_playback_committed_ = true;
    playlist_filter_session_committed_index_ = index;
}

void GtkPlayerWindow::finish_playlist_filter_session() {
    if (!playlist_filter_session_active_) {
        return;
    }

    const PlaylistSelectionMode saved_mode =
        playlist_filter_session_selection_mode_;
    const std::size_t saved_index =
        playlist_filter_session_selection_index_;
    const bool restore_scroll = playlist_filter_session_scroll_valid_;
    const double saved_scroll_value = playlist_filter_session_scroll_value_;
    const bool playback_committed = playlist_filter_session_playback_committed_;
    const std::size_t committed_index = playlist_filter_session_committed_index_;

    playlist_filter_session_active_ = false;
    playlist_filter_session_scroll_valid_ = false;
    playlist_filter_session_playback_committed_ = false;

    if (playlist_.empty()) {
        reset_playlist_selection_state();
        return;
    }

    if (playback_committed) {
        const std::size_t target = std::min(committed_index, playlist_.size() - 1);
        playlist_selection_mode_ = target == current_track_index_
            ? PlaylistSelectionMode::FollowTransport
            : PlaylistSelectionMode::ExplicitUser;
        selected_playlist_index_ = target;
        playlist_selection_mode_before_filter_candidate_ = playlist_selection_mode_;
        playlist_selection_index_before_filter_candidate_ = target;
        playlist_filter_candidate_valid_ = false;
        select_playlist_row(target, PlaylistScrollPolicy::Center);
        return;
    }

    playlist_selection_mode_ = saved_mode;
    selected_playlist_index_ = saved_mode == PlaylistSelectionMode::FollowTransport
        ? std::min(current_track_index_, playlist_.size() - 1)
        : std::min(saved_index, playlist_.size() - 1);
    playlist_selection_mode_before_filter_candidate_ = playlist_selection_mode_;
    playlist_selection_index_before_filter_candidate_ = selected_playlist_index_;
    playlist_filter_candidate_valid_ = false;
    select_playlist_row(selected_playlist_index_, PlaylistScrollPolicy::PreserveViewport);
    if (restore_scroll) {
        restore_playlist_vertical_position(saved_scroll_value);
    }
}

void GtkPlayerWindow::select_first_filter_candidate() {
    if (!playlist_search_enabled_ || playlist_.empty() || playlist_view_ == nullptr) {
        return;
    }
    if (!playlist_filter_session_active_) {
        begin_playlist_filter_session();
    }

    GtkTreeView* view = GTK_TREE_VIEW(playlist_view_);
    GtkTreeModel* model = gtk_tree_view_get_model(view);
    if (model == nullptr) {
        return;
    }

    double preserved_scroll_value = 0.0;
    const bool preserved_scroll_valid =
        capture_playlist_vertical_position(&preserved_scroll_value);

    GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
    GtkTreeIter iter;
    PlaylistSelectionSignalBlocker selection_blocker(*this);
    gtk_tree_selection_unselect_all(selection);
    if (!gtk_tree_model_get_iter_first(model, &iter)) {
        return;
    }

    std::size_t index = 0;
    if (!patches::playlist_index_from_model_iter(model, &iter, COL_INDEX, &index) ||
        index >= playlist_.size()) {
        return;
    }

    GtkTreePath* path = gtk_tree_model_get_path(model, &iter);
    if (path == nullptr) {
        return;
    }

    set_filter_candidate_selection(index);
    gtk_tree_selection_select_iter(selection, &iter);
    gtk_tree_view_set_cursor(view, path, nullptr, FALSE);
    gtk_tree_path_free(path);
    if (preserved_scroll_valid) {
        restore_playlist_vertical_position(preserved_scroll_value);
    }
}

void GtkPlayerWindow::sync_playlist_selection_after_transport_change(
    std::size_t index,
    bool preserve_explicit_selection,
    PlaylistScrollPolicy scroll_policy) {
    if (index >= playlist_.size()) {
        return;
    }

    if (playlist_filter_session_active_) {
        if (playlist_filter_session_playback_committed_) {
            playlist_filter_session_committed_index_ = index;
        }
        return;
    }

    if (!playlist_search_enabled_) {
        if (random_enabled_ && preserve_explicit_selection &&
            playlist_selection_mode_ == PlaylistSelectionMode::ExplicitUser) {
            return;
        }
        reset_playlist_selection_state(index);
        select_playlist_row(index, scroll_policy);
        return;
    }

    if (playlist_selection_mode_ == PlaylistSelectionMode::FilterCandidate) {
        if (preserve_explicit_selection) {
            if (playlist_selection_mode_before_filter_candidate_ ==
                PlaylistSelectionMode::FollowTransport) {
                playlist_selection_index_before_filter_candidate_ = index;
            }
            return;
        }

        playlist_selection_mode_before_filter_candidate_ = PlaylistSelectionMode::FollowTransport;
        playlist_selection_index_before_filter_candidate_ = index;
        if (select_playlist_row(index, scroll_policy)) {
            reset_playlist_selection_state(index);
        }
        return;
    }

    if (preserve_explicit_selection &&
        playlist_selection_mode_ == PlaylistSelectionMode::ExplicitUser) {
        return;
    }

    playlist_selection_mode_ = PlaylistSelectionMode::FollowTransport;
    selected_playlist_index_ = index;
    playlist_selection_mode_before_filter_candidate_ = PlaylistSelectionMode::FollowTransport;
    playlist_selection_index_before_filter_candidate_ = index;
    playlist_filter_candidate_valid_ = false;

    if (!select_playlist_row(index, scroll_policy) &&
        search_controller_ != nullptr && search_controller_->is_filter_active()) {
        select_first_filter_candidate();
    }
}

void GtkPlayerWindow::update_playlist_selection_from_ui() {
    if (playlist_.empty() || playlist_view_ == nullptr) {
        return;
    }

    GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(playlist_view_));
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        int index = 0;
        gtk_tree_model_get(model, &iter, COL_INDEX, &index, -1);
        if (index >= 0 && static_cast<std::size_t>(index) < playlist_.size()) {
            current_track_index_ = static_cast<std::size_t>(index);
        }
    }
}

void GtkPlayerWindow::update_selected_playlist_index_from_ui() {
    if (playlist_selection_syncing_ || playlist_.empty() || playlist_view_ == nullptr) {
        return;
    }

    GtkTreeView* view = GTK_TREE_VIEW(playlist_view_);
    GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        std::size_t row_index = 0;
        if (patches::playlist_index_from_model_iter(model, &iter, COL_INDEX, &row_index) &&
            row_index < playlist_.size()) {
            if (playlist_filter_session_active_ && search_controller_ != nullptr &&
                search_controller_->is_filter_active()) {
                set_filter_candidate_selection(row_index);
            } else {
                set_explicit_playlist_selection(row_index);
            }
            return;
        }
    }

    GtkTreePath* cursor_path = nullptr;
    GtkTreeViewColumn* cursor_column = nullptr;
    gtk_tree_view_get_cursor(view, &cursor_path, &cursor_column);
    if (cursor_path != nullptr) {
        GtkTreeModel* cursor_model = gtk_tree_view_get_model(view);
        std::size_t row_index = 0;
        if (cursor_model != nullptr &&
            patches::playlist_index_from_view_path(view, cursor_path, COL_INDEX, &row_index) &&
            row_index < playlist_.size()) {
            if (playlist_filter_session_active_ && search_controller_ != nullptr &&
                search_controller_->is_filter_active()) {
                set_filter_candidate_selection(row_index);
            } else {
                set_explicit_playlist_selection(row_index);
            }
        }
        gtk_tree_path_free(cursor_path);
    }
}

void GtkPlayerWindow::sync_playlist_selection_to_filter() {
    if (!playlist_search_enabled_ || playlist_.empty() || playlist_view_ == nullptr) {
        return;
    }

    GtkTreeView* view = GTK_TREE_VIEW(playlist_view_);
    GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
    GtkTreeModel* selected_model = nullptr;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &selected_model, &iter)) {
        if (playlist_selection_mode_ == PlaylistSelectionMode::FilterCandidate) {
            std::size_t index = 0;
            if (patches::playlist_index_from_model_iter(selected_model, &iter, COL_INDEX, &index) &&
                index < playlist_.size()) {
                selected_playlist_index_ = index;
                playlist_filter_candidate_valid_ = true;
            }
        }
        return;
    }

    const PlaylistSelectionMode semantic_mode =
        playlist_selection_mode_without_filter_candidate();
    const std::size_t semantic_index =
        playlist_selection_index_without_filter_candidate();
    if (semantic_index < playlist_.size() &&
        select_playlist_row(semantic_index, PlaylistScrollPolicy::PreserveViewport)) {
        playlist_selection_mode_ = semantic_mode;
        selected_playlist_index_ = semantic_index;
        playlist_selection_mode_before_filter_candidate_ = semantic_mode;
        playlist_selection_index_before_filter_candidate_ = semantic_index;
        playlist_filter_candidate_valid_ = false;
        return;
    }

    select_first_filter_candidate();
}

void GtkPlayerWindow::activate_filtered_playlist_selection() {
    if (!playlist_search_enabled_ || playlist_.empty() || playlist_view_ == nullptr) {
        return;
    }

    if (search_controller_ != nullptr) {
        search_controller_->flush_pending_refilter();
    }

    GtkTreeView* view = GTK_TREE_VIEW(playlist_view_);
    GtkTreeModel* model = gtk_tree_view_get_model(view);
    if (model == nullptr) {
        return;
    }

    GtkTreeSelection* selection = gtk_tree_view_get_selection(view);
    GtkTreeIter iter;
    GtkTreeModel* selected_model = nullptr;
    if (gtk_tree_selection_get_selected(selection, &selected_model, &iter)) {
        std::size_t index = 0;
        if (patches::playlist_index_from_model_iter(selected_model, &iter, COL_INDEX, &index) &&
            index < playlist_.size()) {
            play_filtered_track_index(index);
        }
        return;
    }

    if (search_controller_ == nullptr || !search_controller_->is_filter_active()) {
        return;
    }

    if (!gtk_tree_model_get_iter_first(model, &iter)) {
        return;
    }

    std::size_t index = 0;
    if (patches::playlist_index_from_model_iter(model, &iter, COL_INDEX, &index) &&
        index < playlist_.size()) {
        play_filtered_track_index(index);
    }
}

void GtkPlayerWindow::play_filtered_track_index(std::size_t index) {
    if (playlist_filter_session_active_ && search_controller_ != nullptr &&
        search_controller_->is_filter_active()) {
        mark_playlist_filter_playback_committed(index);
    }
    play_track_index(index);
}

std::string GtkPlayerWindow::format_time_seconds(std::uint64_t total_seconds) {
    const std::uint64_t minutes = total_seconds / 60ULL;
    const std::uint64_t seconds = total_seconds % 60ULL;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02llu:%02llu",
                  static_cast<unsigned long long>(minutes),
                  static_cast<unsigned long long>(seconds));
    return buffer;
}

std::string GtkPlayerWindow::display_title_for(const PlaylistEntry& entry) const {
    if (entry.is_stream && stream_manager_ != nullptr && !stream_manager_->now_playing().empty()) {
        if (!entry.title.empty()) {
            return entry.title + " — " + stream_manager_->now_playing();
        }
        return stream_manager_->now_playing();
    }
    if (!entry.performer.empty()) {
        return entry.performer + " - " + entry.title;
    }
    return entry.title;
}

std::string GtkPlayerWindow::media_source_summary(const PlaylistEntry& entry) const {
    std::ostringstream summary;
    if (entry.is_stream && entry.codec_name.empty()) {
        summary << "Stream";
    } else {
        summary << codec_label_for_entry(entry.codec_name, entry.audio_file_path);
    }

    const std::uint32_t rate = entry.source_sample_rate > 0 ? entry.source_sample_rate : entry.decoded_format.sample_rate;
    const std::uint16_t channels = entry.decoded_format.channels > 0 ? entry.decoded_format.channels : 2;
    const std::uint16_t bits = entry.source_bits_per_sample > 0 ? entry.source_bits_per_sample : entry.decoded_format.bits_per_sample;

    if (rate > 0) {
        summary << ", " << rate << " Hz";
        const std::string layout = channels_layout_label(channels);
        if (!layout.empty()) {
            summary << ", " << layout;
        }
        const bool lossy = !entry.lossless_source;
        if (entry.source_bit_rate >= 1000 && lossy) {
            summary << ", " << ((entry.source_bit_rate + 500) / 1000) << " kb/s";
        } else if (bits > 0 && !lossy) {
            summary << ", " << bits << " bit";
        }
    }

    return summary.str();
}

void GtkPlayerWindow::load_preferences() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return;
    }

    const std::string path = std::string(home) + "/.config/pcm_transport.conf";
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        return;
    }

    std::ostringstream raw_preferences;
    raw_preferences << in.rdbuf();
    persisted_preferences_snapshot_ = raw_preferences.str();
    std::istringstream parsed_preferences(persisted_preferences_snapshot_);

    last_opened_sources_.clear();
    saved_last_active_track_ = LastActiveTrackLocator{};
    runtime_last_active_track_ = LastActiveTrackLocator{};
    bool have_last_active_track_start_sample = false;
    bool have_last_active_track_cue_flag = false;
    bool have_dsd_pcm_rules = false;
    std::string line;
    while (std::getline(parsed_preferences, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "last_open_directory") {
            last_open_directory_ = value;
        } else if (key == "playlist_rows_at_startup") {
            try { playlist_rows_at_startup_ = std::stoi(value); } catch (...) {}
            playlist_rows_at_startup_ = std::max(
                kMinPlaylistRows,
                std::min(kMaxPlaylistRows, playlist_rows_at_startup_));
        } else if (key == "restore_last_sources_enabled") {
            restore_last_sources_enabled_ = (value == "1" || value == "true" || value == "yes");
        } else if (key == "last_opened_source_b64") {
            std::string decoded_path;
            if (decode_config_path(value, &decoded_path)) {
                last_opened_sources_.push_back(decoded_path);
            }
        } else if (key == "restore_last_active_track_enabled") {
            restore_last_active_track_enabled_ =
                (value == "1" || value == "true" || value == "yes");
        } else if (key == "last_active_track_path_b64") {
            std::string decoded_path;
            if (decode_config_path(value, &decoded_path)) {
                saved_last_active_track_.audio_file_path = decoded_path;
            }
        } else if (key == "last_active_track_source_start_sample") {
            try {
                saved_last_active_track_.source_start_sample = std::stoull(value);
                have_last_active_track_start_sample = true;
            } catch (...) {
                saved_last_active_track_.source_start_sample = 0;
                have_last_active_track_start_sample = false;
            }
        } else if (key == "last_active_track_is_cue") {
            saved_last_active_track_.cue_track =
                (value == "1" || value == "true" || value == "yes");
            have_last_active_track_cue_flag = true;
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
            deep_bass_preset_ = deep_bass_ui_from_config(deep_bass_preset_);
        } else if (key == "deep_bass_amount") {
            try { deep_bass_amount_ = std::stoi(value); } catch (...) {}
            deep_bass_amount_ = clamp_deep_bass_amount_ui(deep_bass_amount_);
        } else if (key == "progress_blink_enabled") {
            progress_blink_enabled_ = (value == "1" || value == "true" || value == "yes");
        } else if (key == "playlist_search_enabled") {
            playlist_search_enabled_ = (value == "1" || value == "true" || value == "yes");
        } else if (key == "playlist_field_width_limit_enabled") {
            playlist_field_width_limit_enabled_ =
                (value == "1" || value == "true" || value == "yes");
        } else if (key == "playlist_field_width_chars") {
            try {
                playlist_field_width_chars_ = std::stoi(value);
            } catch (...) {
                playlist_field_width_chars_ = kDefaultPlaylistFieldWidthChars;
            }
            playlist_field_width_chars_ = std::max(
                kMinPlaylistFieldWidthChars,
                std::min(kMaxPlaylistFieldWidthChars, playlist_field_width_chars_));
        } else if (key == "level_meter_enabled") {
            level_meter_enabled_ = (value == "1" || value == "true" || value == "yes");
        } else if (key == "clip_detection_enabled") {
            clip_detection_enabled_ = (value == "1" || value == "true" || value == "yes");
        } else if (key == "resample_rules") {
            resample_rules_ = parse_resample_rules(value);
        } else if (key == "bitdepth_rules") {
            bitdepth_rules_ = parse_bitdepth_rules(value);
        } else if (key == "dsd_pcm_rules") {
            const std::vector<DsdPcmRule> parsed = parse_dsd_pcm_rules(value);
            for (const DsdPcmRule& parsed_rule : parsed) {
                for (DsdPcmRule& rule : dsd_pcm_rules_) {
                    if (rule.dsd_sample_rate == parsed_rule.dsd_sample_rate) {
                        rule.pcm_sample_rate = parsed_rule.pcm_sample_rate;
                        break;
                    }
                }
            }
            have_dsd_pcm_rules = true;
        } else if (key == "dsd_pcm_output_bits") {
            try {
                const std::uint16_t bits = static_cast<std::uint16_t>(std::stoul(value));
                if (bits == 16 || bits == 24 || bits == 32) {
                    dsd_pcm_output_bits_ = bits;
                }
            } catch (...) {}
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
    if (!restore_last_sources_enabled_) {
        restore_last_active_track_enabled_ = false;
        last_opened_sources_.clear();
    }
    if (!restore_last_active_track_enabled_ ||
        saved_last_active_track_.audio_file_path.empty() ||
        !have_last_active_track_start_sample ||
        !have_last_active_track_cue_flag) {
        saved_last_active_track_ = LastActiveTrackLocator{};
    } else {
        saved_last_active_track_.valid = true;
    }
    runtime_last_active_track_ = saved_last_active_track_;
    saved_last_open_directory_ = last_open_directory_;
    current_loaded_sources_initialized_ = false;

    if (!have_dsd_pcm_rules) {
        for (DsdPcmRule& dsd_rule : dsd_pcm_rules_) {
            const DsdRateDefinition* definition = find_dsd_rate_definition(dsd_rule.dsd_sample_rate);
            if (definition == nullptr) {
                continue;
            }
            for (const ResampleRule& legacy_rule : resample_rules_) {
                if (legacy_rule.from_rate == definition->ffmpeg_pcm_rate &&
                    legacy_rule.to_rate > 0 &&
                    legacy_rule.to_rate <= definition->ffmpeg_pcm_rate) {
                    dsd_rule.pcm_sample_rate = legacy_rule.to_rate;
                    break;
                }
            }
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

std::string GtkPlayerWindow::serialize_preferences() const {
    std::ostringstream out;
    out << "last_open_directory=" << saved_last_open_directory_ << '\n';
    out << "playlist_rows_at_startup=" << playlist_rows_at_startup_ << '\n';
    out << "restore_last_sources_enabled=" << (restore_last_sources_enabled_ ? 1 : 0) << '\n';
    out << "restore_last_active_track_enabled="
        << (restore_last_sources_enabled_ && restore_last_active_track_enabled_ ? 1 : 0)
        << '\n';
    if (restore_last_sources_enabled_) {
        for (const std::string& source_path : last_opened_sources_) {
            const std::string encoded = encode_config_path(source_path);
            if (!encoded.empty()) {
                out << "last_opened_source_b64=" << encoded << '\n';
            }
        }
        if (restore_last_active_track_enabled_ && saved_last_active_track_.valid) {
            const std::string encoded_track_path =
                encode_config_path(saved_last_active_track_.audio_file_path);
            if (!encoded_track_path.empty()) {
                out << "last_active_track_path_b64=" << encoded_track_path << '\n';
                out << "last_active_track_source_start_sample="
                    << saved_last_active_track_.source_start_sample << '\n';
                out << "last_active_track_is_cue="
                    << (saved_last_active_track_.cue_track ? 1 : 0) << '\n';
            }
        }
    }
    out << "current_device=" << current_device_ << '\n';
    out << "soft_volume_percent=" << soft_volume_percent_ << '\n';
    out << "bass_db=" << bass_db_ << '\n';
    out << "treble_db=" << treble_db_ << '\n';
    out << "pre_eq_headroom_tenths_db=" << effective_pre_eq_headroom_tenths_db() << '\n';
    out << "deep_bass_enabled=" << (deep_bass_enabled_ ? 1 : 0) << '\n';
    out << "deep_bass_preset=" << clamp_deep_bass_preset_ui(deep_bass_preset_) << '\n';
    out << "deep_bass_amount=" << deep_bass_amount_ << '\n';
    out << "progress_blink_enabled=" << (progress_blink_enabled_ ? 1 : 0) << '\n';
    out << "playlist_search_enabled=" << (playlist_search_enabled_ ? 1 : 0) << '\n';
    out << "playlist_field_width_limit_enabled="
        << (playlist_field_width_limit_enabled_ ? 1 : 0) << '\n';
    out << "playlist_field_width_chars=" << playlist_field_width_chars_ << '\n';
    out << "level_meter_enabled=" << (level_meter_enabled_ ? 1 : 0) << '\n';
    out << "clip_detection_enabled=" << (clip_detection_enabled_ ? 1 : 0) << '\n';
    out << "resample_rules=" << serialize_resample_rules(resample_rules_) << '\n';
    out << "bitdepth_rules=" << serialize_bitdepth_rules(bitdepth_rules_) << '\n';
    out << "dsd_pcm_rules=" << serialize_dsd_pcm_rules(dsd_pcm_rules_) << '\n';
    out << "dsd_pcm_output_bits=" << dsd_pcm_output_bits_ << '\n';
    out << "logging_enabled=" << (logging_enabled_ ? 1 : 0) << '\n';
    out << "log_errors_only=" << (log_errors_only_ ? 1 : 0) << '\n';
    out << "log_path=" << log_path_ << '\n';
    out << "bass_shelf_hz=" << bass_shelf_hz_ << '\n';
    out << "treble_shelf_hz=" << treble_shelf_hz_ << '\n';
    out << "resample_quality=" << resample_quality_ << '\n';
    out << "bitdepth_quality=" << bitdepth_quality_ << '\n';
    out << "alsa_24bit_container_preference="
        << normalize_alsa_24bit_preference_id(alsa_24bit_container_preference_) << '\n';
    out << "realtime_audio_priority_enabled=" << (realtime_audio_priority_enabled_ ? 1 : 0) << '\n';
    return out.str();
}

gboolean GtkPlayerWindow::on_preferences_save_timeout(gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr) {
        return G_SOURCE_REMOVE;
    }
    self->preferences_save_timeout_id_ = 0;
    self->save_preferences_now();
    return G_SOURCE_REMOVE;
}

void GtkPlayerWindow::begin_continuous_preferences_interaction() {
    continuous_preferences_interaction_active_ = true;
}

void GtkPlayerWindow::mark_continuous_preferences_dirty() {
    if (bulk_preferences_update_) {
        return;
    }
    continuous_preferences_dirty_ = true;
}

void GtkPlayerWindow::commit_continuous_preferences() {
    continuous_preferences_interaction_active_ = false;
    if (!continuous_preferences_dirty_ && !preferences_save_deferred_for_continuous_) {
        return;
    }
    preferences_save_deferred_for_continuous_ = false;
    save_preferences();
}

void GtkPlayerWindow::save_preferences() {
    if (bulk_preferences_update_) {
        return;
    }
    if (preferences_save_timeout_id_ != 0) {
        g_source_remove(preferences_save_timeout_id_);
        preferences_save_timeout_id_ = 0;
    }

    if (ui_closing_) {
        save_preferences_now();
        return;
    }

    preferences_save_timeout_id_ = g_timeout_add(
        kPreferencesSaveDebounceMs,
        GtkPlayerWindow::on_preferences_save_timeout,
        this);
    if (preferences_save_timeout_id_ == 0) {
        save_preferences_now();
    }
}

void GtkPlayerWindow::flush_preferences_save() {
    if (preferences_save_timeout_id_ != 0) {
        g_source_remove(preferences_save_timeout_id_);
        preferences_save_timeout_id_ = 0;
    }
    save_preferences_now();
}

void GtkPlayerWindow::save_preferences_now() {
    if (continuous_preferences_interaction_active_ && !ui_closing_) {
        preferences_save_deferred_for_continuous_ = true;
        return;
    }

    commit_recovery_checkpoint();

    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return;
    }

    const std::string serialized = serialize_preferences();
    if (serialized == persisted_preferences_snapshot_) {
        continuous_preferences_dirty_ = false;
        preferences_save_deferred_for_continuous_ = false;
        return;
    }

    const std::string dir = std::string(home) + "/.config";
    const std::string path = dir + "/pcm_transport.conf";
    if (g_mkdir_with_parents(dir.c_str(), 0700) != 0) {
        Logger::instance().error("Cannot create configuration directory: " + dir +
                                 " (" + std::strerror(errno) + ")");
        return;
    }

    std::string temporary_template = dir + "/.pcm_transport.conf.tmp.XXXXXX";
    std::vector<char> temporary_path(temporary_template.begin(), temporary_template.end());
    temporary_path.push_back('\0');

    const int fd = ::mkstemp(temporary_path.data());
    if (fd < 0) {
        Logger::instance().error("Cannot create temporary configuration file: " +
                                 std::string(std::strerror(errno)));
        return;
    }

    int write_error = 0;
    std::size_t offset = 0;
    while (offset < serialized.size()) {
        const ssize_t written = ::write(fd,
                                        serialized.data() + offset,
                                        serialized.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            write_error = errno != 0 ? errno : EIO;
            break;
        }
    }

    if (::close(fd) != 0 && write_error == 0) {
        write_error = errno;
    }

    if (write_error != 0) {
        ::unlink(temporary_path.data());
        Logger::instance().error("Cannot write configuration file: " +
                                 std::string(std::strerror(write_error)));
        return;
    }

    if (::rename(temporary_path.data(), path.c_str()) != 0) {
        const int saved_errno = errno;
        ::unlink(temporary_path.data());
        Logger::instance().error("Cannot replace configuration file: " +
                                 std::string(std::strerror(saved_errno)));
        return;
    }

    persisted_preferences_snapshot_ = serialized;
    continuous_preferences_dirty_ = false;
    preferences_save_deferred_for_continuous_ = false;
}

void GtkPlayerWindow::setup_mpris() {
    MprisService::Actions actions;
    actions.play = [this]() { mpris_play(); };
    actions.pause = [this]() { pause_playback(); };
    actions.play_pause = [this]() {
        const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
        if (transport.playing && transport.paused) {
            resume_playback();
        } else if (transport.playing) {
            pause_playback();
        } else {
            mpris_play();
        }
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
    actions.get_position = [this]() {
        if (ui_closing_) {
            return std::int64_t{0};
        }
        const std::int64_t position_usec = current_mpris_track_position_usec();
        return position_usec >= 0 ? position_usec : 0;
    };
    actions.get_state = [this]() {
        if (ui_closing_) {
            return MprisPlayerState{};
        }
        return build_mpris_state();
    };

    apply_stream_mpris_action_wrappers(actions);
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
    if (StreamAudioDecoder::is_stream_uri(audio_file_path)) {
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

std::size_t GtkPlayerWindow::mpris_playlist_index(bool transport_active) const {
    if (playlist_.empty()) {
        return playlist_.size();
    }
    if (transport_active) {
        return std::min(current_track_index_, playlist_.size() - 1);
    }
    return std::min(playlist_play_target_index(), playlist_.size() - 1);
}

std::string GtkPlayerWindow::mpris_track_id_for_index(std::size_t index) const {
    const std::uint64_t id = playlist_entry_id(index);
    if (id == 0) {
        return "/org/mpris/MediaPlayer2/TrackList/NoTrack";
    }
    return "/org/pcmtransport/mpris/track/" + std::to_string(id);
}

std::string GtkPlayerWindow::current_mpris_track_id() const {
    const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
    return mpris_track_id_for_index(mpris_playlist_index(transport.playing));
}

void GtkPlayerWindow::mpris_play() {
    if (!playback_available()) {
        return;
    }
    cancel_pending_last_active_track_restore();
    const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
    if (transport.playing && !transport.paused) {
        return;
    }
    if (transport.paused) {
        resume_playback();
        return;
    }
    start_current_track(false);
}

bool GtkPlayerWindow::mpris_advance_track(int direction) {
    if (!playback_available()) {
        return false;
    }
    cancel_pending_last_active_track_restore();

    if (pending_metadata_playback_valid()) {
        return advance_pending_metadata_playback(direction);
    }

    PlaybackTransportSnapshot transport = engine_.transport_snapshot();
    if (!transport.playing && transport.finished) {
        close_finished_transport_for_manual_navigation();
        transport = engine_.transport_snapshot();
    }
    const bool was_paused = transport.playing && transport.paused;
    const bool was_stopped = !transport.playing;
    if (was_stopped && !playlist_.empty()) {
        current_track_index_ = mpris_playlist_index(false);
    }

    if (random_enabled_) {
        const RandomNavigationAvailability availability =
            random_navigation_availability(was_stopped);
        if ((direction > 0 && !availability.can_go_next) ||
            (direction < 0 && !availability.can_go_previous)) {
            return false;
        }
        if (was_stopped) {
            anchor_random_stopped_navigation();
        }
        std::size_t target_index = current_track_index_;
        PlaybackStartReason reason = PlaybackStartReason::Automatic;
        const bool found = direction > 0
            ? random_next_track(&target_index, &reason)
            : random_previous_track(&target_index);
        if (!found) {
            return false;
        }
        if (direction < 0) {
            reason = PlaybackStartReason::HistoryNavigation;
        }
        if (was_stopped) {
            record_random_stopped_selection(target_index, reason);
            current_track_index_ = target_index;
            sync_playlist_selection_after_transport_change(
                current_track_index_,
                false,
                automatic_transport_scroll_policy(current_track_index_));
            refresh_display();
            mark_mpris_track_changed();
            return true;
        }
        play_track_index_at_offset(target_index,
                                   0,
                                   true,
                                   was_paused,
                                   true,
                                   false,
                                   reason);
        return true;
    }

    if (direction > 0) {
        if (current_track_index_ + 1 < playlist_.size()) {
            if (was_stopped) {
                current_track_index_ += 1;
                sync_playlist_selection_after_transport_change(current_track_index_, false);
                refresh_display();
                mark_mpris_track_changed();
                return true;
            }
            play_track_index_at_offset(current_track_index_ + 1, 0, true, was_paused);
            return true;
        }
        if (repeat_enabled_) {
            if (was_stopped) {
                current_track_index_ = 0;
                sync_playlist_selection_after_transport_change(current_track_index_, false);
                refresh_display();
                mark_mpris_track_changed();
                return true;
            }
            play_track_index_at_offset(0, 0, true, was_paused);
            return true;
        }
        return false;
    }

    if (current_track_index_ > 0) {
        if (was_stopped) {
            current_track_index_ -= 1;
            sync_playlist_selection_after_transport_change(current_track_index_, false);
            refresh_display();
            mark_mpris_track_changed();
            return true;
        }
        play_track_index_at_offset(current_track_index_ - 1, 0, true, was_paused);
        return true;
    }

    return false;
}

MprisPlayerState GtkPlayerWindow::build_mpris_state() const {
    MprisPlayerState state;
    state.volume = static_cast<double>(soft_volume_percent_) / 100.0;
    state.loop_status = mpris_loop_status_;
    state.shuffle = random_enabled_;
    state.fullscreen = false;
    state.can_control = true;
    state.can_play = playback_available();

    const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
    const bool transport_active = transport.playing;
    const std::size_t mpris_index = mpris_playlist_index(transport_active);
    if (random_enabled_) {
        const RandomNavigationAvailability availability =
            random_navigation_availability(!transport_active);
        state.can_go_next = playback_available() && availability.can_go_next;
        state.can_go_previous = playback_available() && availability.can_go_previous;
    } else {
        state.can_go_next = playback_available() &&
                            (mpris_index + 1 < playlist_.size() ||
                             repeat_enabled_);
        state.can_go_previous = playback_available() && mpris_index > 0;
    }
    state.track_epoch = mpris_track_epoch_;
    state.track_id = mpris_track_id_for_index(mpris_index);

    if (transport_active && transport.paused) {
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

    if (mpris_index >= playlist_.size()) {
        state.has_track = false;
        state.track_id = "/org/mpris/MediaPlayer2/TrackList/NoTrack";
        return state;
    }

    const PlaylistEntry& track = playlist_[mpris_index];
    state.has_track = true;
    state.title = track.title.empty() ? display_title_for(track) : track.title;
    state.artist = track.performer;
    state.album = track.album;
    state.track_number = track.track_number;
    state.url = file_uri_for_path(track.audio_file_path);
    apply_stream_fields_to_mpris_state(state, track);
    const std::string cover_path = cached_cover_art_for(track.audio_file_path);
    if (!cover_path.empty()) {
        state.art_url = file_uri_for_path(cover_path);
    }

    if (track.metadata_state != MetadataState::Ready) {
        return state;
    }

    const std::uint32_t sample_rate = std::max<std::uint32_t>(
        1U, active_transport_sample_rate(transport_active, transport.format, track));
    const std::uint64_t length_samples = active_track_length_samples(
        transport_active, transport.total_samples_per_channel, track);
    state.length_usec = samples_to_usec_safe(length_samples, sample_rate);

    if (transport_active) {
        const std::uint64_t position_samples =
            current_track_position_from_transport(transport);
        state.position_usec = samples_to_usec_safe(position_samples, sample_rate);
        state.can_seek = length_samples > 0;
    }

    return state;
}

bool GtkPlayerWindow::validate_mpris_file_uri(const std::string& uri, std::string* local_path) const {
    if (uri.compare(0, 7, "file://") != 0) {
        return false;
    }

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

    if (local_path != nullptr) {
        *local_path = path;
    }
    return true;
}

bool GtkPlayerWindow::mpris_open_uri(const std::string& uri) {
    std::string path;
    if (!validate_mpris_file_uri(uri, &path)) {
        Logger::instance().error("MPRIS OpenUri rejected unsupported URI: " + uri);
        return false;
    }

    if (playlist_loading_) {
        Logger::instance().debug("MPRIS OpenUri ignored while metadata loading is active");
        return false;
    }
    const std::vector<std::string> source_paths{path};
    const std::vector<std::string> loaded =
        load_source_paths(source_paths, false, false, true, path);
    if (loaded.empty()) {
        return false;
    }
    remember_open_directory_from_sources(source_paths);
    return true;
}

std::int64_t GtkPlayerWindow::current_mpris_track_position_usec() const {
    if (!current_track_metadata_ready()) {
        return -1;
    }

    const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
    if (!transport.playing) {
        return -1;
    }

    const PlaylistEntry& track = playlist_[current_track_index_];
    const std::uint32_t sample_rate = std::max<std::uint32_t>(
        1U, active_transport_sample_rate(
                transport.playing, transport.format, track));
    return samples_to_usec_safe(
        current_track_position_from_transport(transport),
        sample_rate);
}

std::int64_t GtkPlayerWindow::current_mpris_track_length_usec() const {
    if (!current_track_metadata_ready()) {
        return -1;
    }

    const PlaylistEntry& track = playlist_[current_track_index_];
    const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
    const std::uint32_t sample_rate = std::max<std::uint32_t>(
        1U, active_transport_sample_rate(
                transport.playing, transport.format, track));
    const std::uint64_t length_samples = active_track_length_samples(
        transport.playing, transport.total_samples_per_channel, track);
    return samples_to_usec_safe(length_samples, sample_rate);
}

std::int64_t GtkPlayerWindow::mpris_seek(std::int64_t offset_usec) {
    if (!current_track_metadata_ready()) {
        return -1;
    }

    const std::int64_t current_usec = current_mpris_track_position_usec();
    const std::int64_t length_usec = current_mpris_track_length_usec();
    if (current_usec < 0 || length_usec < 0) {
        return -1;
    }

    const std::int64_t bounded_current_usec = std::min(current_usec, length_usec);
    std::int64_t target_usec = 0;
    if (offset_usec < 0) {
        if (offset_usec <= -bounded_current_usec) {
            target_usec = 0;
        } else {
            target_usec = bounded_current_usec + offset_usec;
        }
    } else if (offset_usec > length_usec - bounded_current_usec) {
        const bool can_go_next = random_enabled_
            ? random_navigation_availability(false).can_go_next
            : (current_track_index_ + 1 < playlist_.size() || repeat_enabled_);
        if (!can_go_next || !mpris_advance_track(1)) {
            return -1;
        }
        return current_mpris_track_position_usec();
    } else {
        target_usec = bounded_current_usec + offset_usec;
    }

    return mpris_set_position(target_usec, current_mpris_track_id());
}

std::int64_t GtkPlayerWindow::mpris_set_position(std::int64_t position_usec, const std::string& track_id) {
    if (!current_track_metadata_ready()) {
        return -1;
    }

    const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
    if (!transport.playing) {
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

    const std::int64_t current_position_usec =
        current_mpris_track_position_usec();
    if (current_position_usec == position_usec) {
        return -1;
    }

    const PlaylistEntry& track = playlist_[current_track_index_];
    // Seeking starts a new decoder. Convert the requested time into the
    // configured sample domain that will apply to that new transport, while
    // position and length above remain tied to the currently active transport.
    const std::uint32_t configured_sample_rate = std::max<std::uint32_t>(
        1U, playback_sample_rate_for_entry(track));
    std::uint64_t target_samples = 0;
    if (!usec_to_samples_safe(
            position_usec, configured_sample_rate, &target_samples)) {
        return -1;
    }

    cancel_pending_seek();
    play_track_index_at_offset(current_track_index_,
                               target_samples,
                               true,
                               transport.paused,
                               false,
                               true,
                               PlaybackStartReason::PreserveHistory);
    return position_usec;
}

void GtkPlayerWindow::mpris_set_volume(double volume) {
    soft_volume_percent_ = static_cast<int>(std::round(std::max(0.0, std::min(1.0, volume)) * 100.0));
    engine_.set_soft_volume_percent(soft_volume_percent_);
    save_preferences();
    if (!ui_closing_) {
        refresh_display(true, false);
    }
    notify_mpris_state_changed();
}

void GtkPlayerWindow::mpris_set_loop_status(const std::string& loop_status) {
    if (loop_status == "Playlist") {
        set_playback_mode(true, random_enabled_);
    } else if (loop_status == "Track") {
        return;
    } else if (loop_status == "None") {
        set_playback_mode(false, random_enabled_);
    } else {
        return;
    }
}

void GtkPlayerWindow::mpris_set_rate(double rate) {
    if (std::abs(rate) <= 1e-9) {
        const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
        if (transport.playing && !transport.paused) {
            pause_playback();
        }
        return;
    }
    if (std::abs(rate - 1.0) > 1e-9) {
        Logger::instance().info("MPRIS Rate is not supported; ignoring value " + std::to_string(rate));
    }
}

void GtkPlayerWindow::mpris_set_fullscreen(bool enabled) {
    (void)enabled;
}

void GtkPlayerWindow::mpris_set_shuffle(bool enabled) {
    set_playback_mode(repeat_enabled_, enabled);
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
        {"media-play-pause", on_media_play_pause, nullptr, nullptr, nullptr, {0}},
        {"media-stop", on_media_stop, nullptr, nullptr, nullptr, {0}},
        {"media-next", on_media_next, nullptr, nullptr, nullptr, {0}},
        {"media-previous", on_media_previous, nullptr, nullptr, nullptr, {0}},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), actions, G_N_ELEMENTS(actions), this);

    static const char* kPlayPauseKeys[] = {"XF86AudioPlay", "XF86AudioPause", nullptr};
    static const char* kStopKeys[] = {"XF86AudioStop", nullptr};
    static const char* kNextKeys[] = {"XF86AudioNext", nullptr};
    static const char* kPreviousKeys[] = {"XF86AudioPrev", nullptr};

    gtk_application_set_accels_for_action(app, "app.media-play-pause", kPlayPauseKeys);
    gtk_application_set_accels_for_action(app, "app.media-stop", kStopKeys);
    gtk_application_set_accels_for_action(app, "app.media-next", kNextKeys);
    gtk_application_set_accels_for_action(app, "app.media-previous", kPreviousKeys);
}

void GtkPlayerWindow::handle_media_play() {
    if (!playback_available()) {
        return;
    }
    if (!playlist_search_enabled_) {
        update_playlist_selection_from_ui();
    }
    mpris_play();
}

void GtkPlayerWindow::handle_media_pause() {
    pause_playback();
}

void GtkPlayerWindow::handle_media_play_pause() {
    if (!playlist_search_enabled_) {
        update_playlist_selection_from_ui();
    }
    const PlaybackTransportSnapshot transport = engine_.transport_snapshot();
    if (transport.playing && transport.paused) {
        resume_playback();
    } else if (transport.playing) {
        pause_playback();
    } else {
        mpris_play();
    }
    notify_mpris_state_changed();
}

void GtkPlayerWindow::handle_media_stop() {
    stop_playback();
}

void GtkPlayerWindow::handle_media_next() {
    if (!playback_available()) {
        return;
    }
    if (!playlist_search_enabled_) {
        update_playlist_selection_from_ui();
    }
    mpris_advance_track(1);
}

void GtkPlayerWindow::handle_media_previous() {
    if (!playback_available()) {
        return;
    }
    if (!playlist_search_enabled_) {
        update_playlist_selection_from_ui();
    }
    mpris_advance_track(-1);
}

bool GtkPlayerWindow::handle_media_key(guint keyval) {
    switch (keyval) {
        case GDK_KEY_AudioPlay:
        case GDK_KEY_AudioPause:
            handle_media_play_pause();
            return true;
        case GDK_KEY_AudioStop:
            handle_media_stop();
            return true;
        case GDK_KEY_AudioNext:
            handle_media_next();
            return true;
        case GDK_KEY_AudioPrev:
            handle_media_previous();
            return true;
        default:
            return false;
    }
}

gboolean GtkPlayerWindow::on_window_key_press(GtkWidget*, GdkEvent* event, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || event == nullptr || event->type != GDK_KEY_PRESS) {
        return FALSE;
    }
    const auto* key_event = reinterpret_cast<GdkEventKey*>(event);
    return self->handle_media_key(key_event->keyval) ? TRUE : FALSE;
}

gboolean GtkPlayerWindow::on_playlist_view_media_key_press(GtkWidget*, GdkEvent* event, gpointer user_data) {
    auto* self = static_cast<GtkPlayerWindow*>(user_data);
    if (self == nullptr || event == nullptr || event->type != GDK_KEY_PRESS) {
        return FALSE;
    }
    const auto* key_event = reinterpret_cast<GdkEventKey*>(event);
    return self->handle_media_key(key_event->keyval) ? TRUE : FALSE;
}

void GtkPlayerWindow::on_media_play(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GtkPlayerWindow*>(user_data)->handle_media_play();
}

void GtkPlayerWindow::on_media_pause(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GtkPlayerWindow*>(user_data)->handle_media_pause();
}

void GtkPlayerWindow::on_media_play_pause(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GtkPlayerWindow*>(user_data)->handle_media_play_pause();
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
