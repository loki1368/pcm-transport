#include "pcmtp/playlist/SourceScanner.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <iterator>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pcmtp/cue/CueParser.hpp"
#include "pcmtp/playlist/M3uPlaylistReader.hpp"

namespace pcmtp {
namespace {

struct DirectoryCandidate {
    enum class Type {
        Audio,
        Cue
    };

    Type type = Type::Audio;
    std::string path;
    std::string identity;
};

struct DirectoryFrame {
    std::string path;
    std::vector<std::string> names;
    std::size_t next_name = 0;
};

bool cancelled(const std::atomic<bool>* cancel_requested) {
    return cancel_requested != nullptr && cancel_requested->load(std::memory_order_relaxed);
}

std::string lower_extension(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size() ||
        (slash != std::string::npos && dot < slash)) {
        return {};
    }
    std::string extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return extension;
}

bool is_regular_file_following_symlink(const std::string& path) {
    struct stat status {};
    return stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}

bool is_directory_without_following_symlink(const std::string& path) {
    struct stat status {};
    if (lstat(path.c_str(), &status) != 0 || S_ISLNK(status.st_mode)) {
        return false;
    }
    return S_ISDIR(status.st_mode);
}

std::string join_path(const std::string& directory, const std::string& name) {
    if (directory.empty() || directory == ".") {
        return name;
    }
    if (directory.back() == '/') {
        return directory + name;
    }
    return directory + "/" + name;
}

std::string status_identity(const struct stat& status) {
    std::ostringstream stream;
    stream << static_cast<unsigned long long>(status.st_dev)
           << ':'
           << static_cast<unsigned long long>(status.st_ino);
    return stream.str();
}

std::string file_identity(const std::string& path) {
    struct stat status {};
    if (stat(path.c_str(), &status) == 0) {
        return status_identity(status);
    }

    char* resolved = realpath(path.c_str(), nullptr);
    if (resolved != nullptr) {
        const std::string result(resolved);
        std::free(resolved);
        return result;
    }
    return path;
}

int compare_numeric_runs(const std::string& left,
                         std::size_t* left_offset,
                         const std::string& right,
                         std::size_t* right_offset) {
    std::size_t left_begin = *left_offset;
    std::size_t right_begin = *right_offset;
    while (left_begin < left.size() && left[left_begin] == '0') {
        ++left_begin;
    }
    while (right_begin < right.size() && right[right_begin] == '0') {
        ++right_begin;
    }

    std::size_t left_end = left_begin;
    std::size_t right_end = right_begin;
    while (left_end < left.size() && std::isdigit(static_cast<unsigned char>(left[left_end])) != 0) {
        ++left_end;
    }
    while (right_end < right.size() && std::isdigit(static_cast<unsigned char>(right[right_end])) != 0) {
        ++right_end;
    }

    const std::size_t left_digits = left_end - left_begin;
    const std::size_t right_digits = right_end - right_begin;
    if (left_digits != right_digits) {
        return left_digits < right_digits ? -1 : 1;
    }
    if (left_digits > 0) {
        const int comparison = left.compare(left_begin, left_digits, right, right_begin, right_digits);
        if (comparison != 0) {
            return comparison < 0 ? -1 : 1;
        }
    }

    std::size_t left_full_end = *left_offset;
    std::size_t right_full_end = *right_offset;
    while (left_full_end < left.size() &&
           std::isdigit(static_cast<unsigned char>(left[left_full_end])) != 0) {
        ++left_full_end;
    }
    while (right_full_end < right.size() &&
           std::isdigit(static_cast<unsigned char>(right[right_full_end])) != 0) {
        ++right_full_end;
    }
    const std::size_t left_full_digits = left_full_end - *left_offset;
    const std::size_t right_full_digits = right_full_end - *right_offset;
    *left_offset = left_full_end;
    *right_offset = right_full_end;
    if (left_full_digits != right_full_digits) {
        return left_full_digits < right_full_digits ? -1 : 1;
    }
    return 0;
}

bool natural_filename_less(const std::string& left, const std::string& right) {
    std::size_t left_offset = 0;
    std::size_t right_offset = 0;
    while (left_offset < left.size() && right_offset < right.size()) {
        const unsigned char left_value = static_cast<unsigned char>(left[left_offset]);
        const unsigned char right_value = static_cast<unsigned char>(right[right_offset]);
        if (std::isdigit(left_value) != 0 && std::isdigit(right_value) != 0) {
            const int numeric_comparison = compare_numeric_runs(
                left, &left_offset, right, &right_offset);
            if (numeric_comparison != 0) {
                return numeric_comparison < 0;
            }
            continue;
        }

        const unsigned char left_folded = left_value < 128
            ? static_cast<unsigned char>(std::tolower(left_value))
            : left_value;
        const unsigned char right_folded = right_value < 128
            ? static_cast<unsigned char>(std::tolower(right_value))
            : right_value;
        if (left_folded != right_folded) {
            return left_folded < right_folded;
        }
        if (left_value != right_value) {
            return left_value < right_value;
        }
        ++left_offset;
        ++right_offset;
    }
    return left.size() < right.size();
}

bool read_directory_names(const std::string& directory,
                          const std::atomic<bool>* cancel_requested,
                          std::vector<std::string>* names,
                          std::vector<std::string>* errors) {
    if (names == nullptr || errors == nullptr || cancelled(cancel_requested)) {
        return false;
    }

    DIR* handle = opendir(directory.c_str());
    if (handle == nullptr) {
        errors->push_back("Cannot scan directory: " + directory + " (" +
                          std::strerror(errno) + ")");
        return false;
    }

    names->clear();
    errno = 0;
    while (!cancelled(cancel_requested)) {
        dirent* entry = readdir(handle);
        if (entry == nullptr) {
            break;
        }
        const std::string name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        names->push_back(name);
    }
    const int read_error = errno;
    closedir(handle);
    if (read_error != 0 && !cancelled(cancel_requested)) {
        errors->push_back("Cannot read directory: " + directory + " (" +
                          std::strerror(read_error) + ")");
    }
    if (cancelled(cancel_requested)) {
        return false;
    }

    std::stable_sort(names->begin(), names->end(), natural_filename_less);
    return read_error == 0;
}

bool push_directory_frame(const std::string& directory,
                          dev_t root_device,
                          const std::atomic<bool>* cancel_requested,
                          std::unordered_set<std::string>* visited_directories,
                          std::vector<DirectoryFrame>* stack,
                          std::vector<std::string>* errors) {
    if (visited_directories == nullptr || stack == nullptr || errors == nullptr ||
        cancelled(cancel_requested)) {
        return false;
    }

    struct stat status {};
    if (lstat(directory.c_str(), &status) != 0) {
        errors->push_back("Cannot inspect directory: " + directory + " (" +
                          std::strerror(errno) + ")");
        return false;
    }
    if (S_ISLNK(status.st_mode) || !S_ISDIR(status.st_mode)) {
        return false;
    }
    if (status.st_dev != root_device) {
        return false;
    }

    if (!visited_directories->insert(status_identity(status)).second) {
        return false;
    }

    DirectoryFrame frame;
    frame.path = directory;
    if (!read_directory_names(directory, cancel_requested, &frame.names, errors)) {
        return false;
    }
    stack->push_back(std::move(frame));
    return true;
}

void collect_directory_candidates(const std::string& directory,
                                  const std::atomic<bool>* cancel_requested,
                                  std::unordered_set<std::string>* visited_directories,
                                  std::vector<DirectoryCandidate>* candidates,
                                  std::vector<std::string>* errors) {
    if (visited_directories == nullptr || candidates == nullptr || errors == nullptr ||
        cancelled(cancel_requested)) {
        return;
    }

    struct stat root_status {};
    if (lstat(directory.c_str(), &root_status) != 0) {
        errors->push_back("Cannot inspect directory: " + directory + " (" +
                          std::strerror(errno) + ")");
        return;
    }
    if (S_ISLNK(root_status.st_mode) || !S_ISDIR(root_status.st_mode)) {
        errors->push_back("Cannot scan directory: " + directory);
        return;
    }

    std::vector<DirectoryFrame> stack;
    if (!push_directory_frame(directory,
                              root_status.st_dev,
                              cancel_requested,
                              visited_directories,
                              &stack,
                              errors)) {
        return;
    }

    while (!stack.empty()) {
        if (cancelled(cancel_requested)) {
            return;
        }

        DirectoryFrame& frame = stack.back();
        if (frame.next_name >= frame.names.size()) {
            stack.pop_back();
            continue;
        }

        const std::string path = join_path(frame.path, frame.names[frame.next_name]);
        ++frame.next_name;
        struct stat link_status {};
        if (lstat(path.c_str(), &link_status) != 0) {
            errors->push_back("Cannot inspect source: " + path + " (" +
                              std::strerror(errno) + ")");
            continue;
        }

        if (S_ISDIR(link_status.st_mode)) {
            push_directory_frame(path,
                                 root_status.st_dev,
                                 cancel_requested,
                                 visited_directories,
                                 &stack,
                                 errors);
            continue;
        }
        if (S_ISLNK(link_status.st_mode)) {
            struct stat target_status {};
            if (stat(path.c_str(), &target_status) != 0 || S_ISDIR(target_status.st_mode)) {
                continue;
            }
            if (!S_ISREG(target_status.st_mode)) {
                continue;
            }
        } else if (!S_ISREG(link_status.st_mode)) {
            continue;
        }

        if (M3uPlaylistReader::looks_like_playlist_path(path)) {
            continue;
        }
        if (CueParser::looks_like_cue_path(path)) {
            candidates->push_back(DirectoryCandidate{
                DirectoryCandidate::Type::Cue, path, file_identity(path)});
            continue;
        }
        if (SourceScanner::is_supported_audio_path(path)) {
            candidates->push_back(DirectoryCandidate{
                DirectoryCandidate::Type::Audio, path, file_identity(path)});
        }
    }
}

std::vector<ScannedSourcePath> expand_directory(
    const std::string& directory,
    const std::atomic<bool>* cancel_requested,
    std::unordered_set<std::string>* visited_directories,
    std::unordered_set<std::string>* consumed_audio_identities,
    std::unordered_set<std::string>* emitted_source_identities,
    std::vector<std::string>* errors) {
    std::vector<ScannedSourcePath> result;
    if (visited_directories == nullptr || consumed_audio_identities == nullptr ||
        emitted_source_identities == nullptr || errors == nullptr ||
        cancelled(cancel_requested)) {
        return result;
    }

    std::vector<DirectoryCandidate> candidates;
    collect_directory_candidates(directory,
                                 cancel_requested,
                                 visited_directories,
                                 &candidates,
                                 errors);
    if (cancelled(cancel_requested)) {
        return result;
    }

    std::unordered_set<std::string> available_audio_identities;
    for (const DirectoryCandidate& candidate : candidates) {
        if (candidate.type == DirectoryCandidate::Type::Audio) {
            available_audio_identities.insert(candidate.identity);
        }
    }

    std::unordered_map<std::string, DirectoryCandidate> cue_for_audio;
    for (const DirectoryCandidate& candidate : candidates) {
        if (cancelled(cancel_requested)) {
            return {};
        }
        if (candidate.type != DirectoryCandidate::Type::Cue) {
            continue;
        }
        try {
            const CueSheet sheet = CueParser::parse_file(candidate.path, 0);
            if (sheet.audio_file_paths.size() != 1) {
                continue;
            }
            const std::string& audio_path = sheet.audio_file_paths.front();
            if (!is_regular_file_following_symlink(audio_path) ||
                !SourceScanner::is_supported_audio_path(audio_path)) {
                continue;
            }
            const std::string audio_identity = file_identity(audio_path);
            if (available_audio_identities.find(audio_identity) ==
                available_audio_identities.end()) {
                continue;
            }
            if (cue_for_audio.find(audio_identity) == cue_for_audio.end()) {
                cue_for_audio.emplace(audio_identity, candidate);
            }
        } catch (const std::exception& error) {
            errors->push_back("Ignoring CUE during directory scan: " + candidate.path +
                              " (" + error.what() + ")");
        }
    }

    std::unordered_set<std::string> emitted_cues;
    for (const DirectoryCandidate& candidate : candidates) {
        if (cancelled(cancel_requested)) {
            return {};
        }
        if (candidate.type != DirectoryCandidate::Type::Audio) {
            continue;
        }
        if (!consumed_audio_identities->insert(candidate.identity).second) {
            continue;
        }

        const auto cue = cue_for_audio.find(candidate.identity);
        if (cue != cue_for_audio.end()) {
            const std::string cue_identity = "cue:" + cue->second.identity;
            if (emitted_cues.insert(cue_identity).second &&
                emitted_source_identities->insert(cue_identity).second) {
                result.push_back(ScannedSourcePath{cue->second.path, directory});
            }
            continue;
        }

        const std::string source_identity = "audio:" + candidate.identity;
        if (emitted_source_identities->insert(source_identity).second) {
            result.push_back(ScannedSourcePath{candidate.path, directory});
        }
    }
    return result;
}

void remember_direct_cue_audio(const std::string& cue_path,
                               std::unordered_set<std::string>* consumed_audio_identities) {
    if (consumed_audio_identities == nullptr) {
        return;
    }
    try {
        const CueSheet sheet = CueParser::parse_file(cue_path, 0);
        for (const std::string& audio_path : sheet.audio_file_paths) {
            if (is_regular_file_following_symlink(audio_path)) {
                consumed_audio_identities->insert(file_identity(audio_path));
            }
        }
    } catch (...) {
        // Direct CUE validation remains the responsibility of the existing loader.
    }
}

} // namespace

bool SourceScanner::is_supported_audio_path(const std::string& path) {
    const std::string extension = lower_extension(path);
    static const char* kSupportedExtensions[] = {
        ".flac", ".mp3", ".mp2", ".wav", ".wave", ".w64", ".bwf",
        ".aiff", ".aif", ".au", ".snd", ".caf", ".voc", ".ra",
        ".ape", ".wv", ".tak", ".tta", ".dsf", ".dff", ".m4a",
        ".m4r", ".aac", ".ac3", ".dts", ".ogg", ".oga", ".opus",
        ".spx", ".wma", ".asf", ".xwma", ".wmv", ".oma", ".aa3",
        ".at3", ".mpc", ".mp+", ".mpp",
    };
    for (const char* supported : kSupportedExtensions) {
        if (extension == supported) {
            return true;
        }
    }
    return false;
}

bool SourceScanner::is_supported_media_path(const std::string& path) {
    return is_supported_audio_path(path) ||
           CueParser::looks_like_cue_path(path) ||
           M3uPlaylistReader::looks_like_playlist_path(path);
}

SourceScanResult SourceScanner::scan(
    const std::vector<std::string>& top_level_paths,
    const std::atomic<bool>* cancel_requested) {
    SourceScanResult result;
    std::unordered_set<std::string> visited_directories;
    std::unordered_set<std::string> consumed_audio_identities;
    std::unordered_set<std::string> emitted_source_identities;

    for (const std::string& top_level_path : top_level_paths) {
        if (cancelled(cancel_requested)) {
            result.cancelled = true;
            result.sources.clear();
            return result;
        }
        if (top_level_path.empty()) {
            continue;
        }

        if (is_directory_without_following_symlink(top_level_path)) {
            std::vector<ScannedSourcePath> expanded = expand_directory(
                top_level_path,
                cancel_requested,
                &visited_directories,
                &consumed_audio_identities,
                &emitted_source_identities,
                &result.errors);
            if (cancelled(cancel_requested)) {
                result.cancelled = true;
                result.sources.clear();
                return result;
            }
            result.sources.insert(result.sources.end(),
                                  std::make_move_iterator(expanded.begin()),
                                  std::make_move_iterator(expanded.end()));
            continue;
        }

        if (!is_regular_file_following_symlink(top_level_path)) {
            result.errors.push_back("Source is unavailable: " + top_level_path);
            continue;
        }
        if (!is_supported_media_path(top_level_path)) {
            result.errors.push_back("Unsupported source: " + top_level_path);
            continue;
        }

        std::string identity_prefix = "source:";
        if (is_supported_audio_path(top_level_path)) {
            const std::string audio_identity = file_identity(top_level_path);
            if (!consumed_audio_identities.insert(audio_identity).second) {
                continue;
            }
            identity_prefix = "audio:";
        } else if (CueParser::looks_like_cue_path(top_level_path)) {
            identity_prefix = "cue:";
            remember_direct_cue_audio(top_level_path, &consumed_audio_identities);
        } else if (M3uPlaylistReader::looks_like_playlist_path(top_level_path)) {
            identity_prefix = "playlist:";
        }

        const std::string source_identity = identity_prefix + file_identity(top_level_path);
        if (emitted_source_identities.insert(source_identity).second) {
            result.sources.push_back(ScannedSourcePath{top_level_path, top_level_path});
        }
    }

    return result;
}

} // namespace pcmtp
