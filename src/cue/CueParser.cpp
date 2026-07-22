#include "pcmtp/cue/CueParser.hpp"

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "pcmtp/util/TextEncoding.hpp"

namespace pcmtp {

namespace {


std::string read_normalized_cue_text(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open CUE file: " + path);
    }
    std::ostringstream ss;
    ss << input.rdbuf();
    return pcmtp::text::normalize_text_file_bytes(ss.str());
}

std::string trim(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string unquote(std::string value) {
    value = trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::string directory_of(const std::string& path) {
    const std::size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return ".";
    }
    if (pos == 0) {
        return path.substr(0, 1);
    }
    return path.substr(0, pos);
}

std::string join_path(const std::string& base_dir, const std::string& file_name) {
    if (file_name.empty()) {
        return file_name;
    }
    if (!file_name.empty() && (file_name[0] == '/' || file_name[0] == '\\')) {
        return file_name;
    }
    if (file_name.size() > 2 && std::isalpha(static_cast<unsigned char>(file_name[0])) != 0 && file_name[1] == ':') {
        return file_name;
    }
    if (base_dir.empty() || base_dir == ".") {
        return file_name;
    }
    if (base_dir.back() == '/' || base_dir.back() == '\\') {
        return base_dir + file_name;
    }
    return base_dir + "/" + file_name;
}

bool path_exists(const std::string& path) {
    struct stat st {};
    return !path.empty() && stat(path.c_str(), &st) == 0;
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::vector<std::string> split_components(const std::string& path) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start < path.size()) {
        while (start < path.size() && (path[start] == '/' || path[start] == '\\')) {
            ++start;
        }
        std::size_t end = start;
        while (end < path.size() && path[end] != '/' && path[end] != '\\') {
            ++end;
        }
        if (end > start) {
            parts.push_back(path.substr(start, end - start));
        }
        start = end;
    }
    return parts;
}

std::string find_child_case_insensitive(const std::string& dir, const std::string& name) {
    DIR* d = opendir(dir.empty() ? "." : dir.c_str());
    if (d == nullptr) {
        return std::string();
    }
    const std::string wanted = lower_ascii(name);
    std::string result;
    while (dirent* ent = readdir(d)) {
        const std::string item = ent->d_name;
        if (item == "." || item == "..") {
            continue;
        }
        if (lower_ascii(item) == wanted) {
            result = item;
            break;
        }
    }
    closedir(d);
    return result;
}

std::string resolve_case_insensitive_path(const std::string& base_dir, const std::string& file_name) {
    if (file_name.empty()) {
        return file_name;
    }
    const bool absolute = !file_name.empty() && (file_name[0] == '/' || file_name[0] == '\\');
    const std::string exact = absolute ? file_name : join_path(base_dir, file_name);
    if (path_exists(exact)) {
        return exact;
    }

    std::string current = absolute ? std::string("/") : (base_dir.empty() ? std::string(".") : base_dir);
    const std::vector<std::string> parts = split_components(file_name);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string exact_child = join_path(current, parts[i]);
        if (path_exists(exact_child)) {
            current = exact_child;
            continue;
        }
        const std::string found = find_child_case_insensitive(current, parts[i]);
        if (found.empty()) {
            std::string tail;
            for (std::size_t j = i; j < parts.size(); ++j) {
                if (!tail.empty()) tail += "/";
                tail += parts[j];
            }
            return join_path(current, tail);
        }
        current = join_path(current, found);
    }
    return current;
}

bool starts_with_keyword(const std::string& line, const std::string& keyword) {
    if (line.size() < keyword.size()) {
        return false;
    }
    for (std::size_t i = 0; i < keyword.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(line[i])) != std::toupper(static_cast<unsigned char>(keyword[i]))) {
            return false;
        }
    }
    return true;
}

std::string cue_file_entry_value(const std::string& line) {
    const std::size_t first_quote = line.find('"');
    const std::size_t second_quote = line.find('"', first_quote == std::string::npos ? first_quote : first_quote + 1);
    if (first_quote != std::string::npos && second_quote != std::string::npos && second_quote > first_quote) {
        return line.substr(first_quote + 1, second_quote - first_quote - 1);
    }
    std::istringstream ss(line.substr(5));
    std::string file_name;
    ss >> file_name;
    return file_name;
}

std::uint64_t mmssff_to_frame75(const std::string& value) {
    std::istringstream stream(value);
    std::string mm;
    std::string ss;
    std::string ff;
    if (!std::getline(stream, mm, ':') || !std::getline(stream, ss, ':') || !std::getline(stream, ff, ':')) {
        throw std::runtime_error("Invalid CUE time format: " + value);
    }

    const int minutes = std::stoi(mm);
    const int seconds = std::stoi(ss);
    const int frames = std::stoi(ff);
    if (minutes < 0 || seconds < 0 || frames < 0) {
        throw std::runtime_error("Invalid CUE time value: " + value);
    }
    const std::uint64_t minute_value = static_cast<std::uint64_t>(minutes);
    const std::uint64_t second_value = static_cast<std::uint64_t>(seconds);
    const std::uint64_t frame_value = static_cast<std::uint64_t>(frames);
    if (minute_value > (std::numeric_limits<std::uint64_t>::max() -
                        second_value) / 60ULL) {
        throw std::runtime_error("CUE time value is too large: " + value);
    }
    const std::uint64_t total_seconds = minute_value * 60ULL + second_value;
    if (total_seconds > (std::numeric_limits<std::uint64_t>::max() -
                         frame_value) / 75ULL) {
        throw std::runtime_error("CUE time value is too large: " + value);
    }
    return total_seconds * 75ULL + frame_value;
}

} // namespace

std::uint64_t CueParser::frame75_to_samples(std::uint64_t frame_75,
                                            std::uint32_t sample_rate) {
    if (sample_rate == 0) {
        return 0;
    }
    const std::uint64_t whole_seconds = frame_75 / 75ULL;
    const std::uint64_t remaining_frames = frame_75 % 75ULL;
    if (whole_seconds > std::numeric_limits<std::uint64_t>::max() /
                            static_cast<std::uint64_t>(sample_rate)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const std::uint64_t whole_samples = whole_seconds *
                                        static_cast<std::uint64_t>(sample_rate);
    const std::uint64_t fractional_samples =
        (remaining_frames * static_cast<std::uint64_t>(sample_rate)) / 75ULL;
    if (whole_samples > std::numeric_limits<std::uint64_t>::max() -
                            fractional_samples) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return whole_samples + fractional_samples;
}

bool CueParser::looks_like_cue_path(const std::string& path) {
    if (path.size() < 4) {
        return false;
    }
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower.substr(lower.size() - 4) == ".cue";
}

std::string CueParser::resolve_audio_file_path(const std::string& path) {
    const std::string text_bytes = read_normalized_cue_text(path);
    std::istringstream input(text_bytes);
    const std::string base_dir = directory_of(path);
    std::string raw_line;
    while (std::getline(input, raw_line)) {
        const std::string line = trim(raw_line);
        if (!starts_with_keyword(line, "FILE ")) {
            continue;
        }
        const std::string referenced = cue_file_entry_value(line);
        if (referenced.empty()) {
            break;
        }
        return resolve_case_insensitive_path(base_dir, referenced);
    }
    throw std::runtime_error("CUE file does not contain FILE entry");
}

CueSheet CueParser::parse_file(const std::string& path, std::uint64_t total_samples_per_channel) {
    const std::string text_bytes = read_normalized_cue_text(path);
    std::istringstream input(text_bytes);

    CueSheet sheet;
    sheet.cue_path = path;
    const std::string base_dir = directory_of(path);

    CueTrack* current_track = nullptr;
    bool seen_audio_file = false;

    std::string raw_line;
    while (std::getline(input, raw_line)) {
        const std::string line = trim(raw_line);
        if (line.empty()) {
            continue;
        }

        if (starts_with_keyword(line, "FILE ")) {
            const std::string referenced = cue_file_entry_value(line);
            if (!referenced.empty()) {
                sheet.audio_file_path = resolve_case_insensitive_path(base_dir, referenced);
                seen_audio_file = true;
            }
            continue;
        }

        if (starts_with_keyword(line, "TITLE ")) {
            const std::string value = unquote(line.substr(6));
            if (current_track != nullptr) {
                current_track->title = value;
            } else {
                sheet.title = value;
            }
            continue;
        }

        if (starts_with_keyword(line, "PERFORMER ")) {
            const std::string value = unquote(line.substr(10));
            if (current_track != nullptr) {
                current_track->performer = value;
            } else {
                sheet.performer = value;
            }
            continue;
        }

        if (starts_with_keyword(line, "TRACK ")) {
            std::istringstream stream(line.substr(6));
            int number = 0;
            std::string type;
            stream >> number >> type;
            std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (type == "audio") {
                CueTrack track;
                track.number = number;
                sheet.tracks.push_back(track);
                current_track = &sheet.tracks.back();
            }
            continue;
        }

        if (starts_with_keyword(line, "INDEX 01 ")) {
            if (current_track == nullptr) {
                continue;
            }
            current_track->start_frame_75 = mmssff_to_frame75(trim(line.substr(9)));
            current_track->start_sample = CueParser::frame75_to_samples(
                current_track->start_frame_75, 44100U);
            continue;
        }
    }

    if (!seen_audio_file) {
        throw std::runtime_error("CUE file does not contain FILE entry");
    }

    if (sheet.tracks.empty()) {
        CueTrack track;
        track.number = 1;
        track.start_sample = 0;
        sheet.tracks.push_back(track);
    }

    for (std::size_t i = 0; i < sheet.tracks.size(); ++i) {
        if (i + 1 < sheet.tracks.size()) {
            sheet.tracks[i].end_frame_75 = sheet.tracks[i + 1].start_frame_75;
            sheet.tracks[i].has_end_frame_75 = true;
            sheet.tracks[i].end_sample = sheet.tracks[i + 1].start_sample;
        } else {
            sheet.tracks[i].end_sample = total_samples_per_channel;
        }
        if (sheet.tracks[i].title.empty()) {
            sheet.tracks[i].title = "Track " + std::to_string(sheet.tracks[i].number);
        }
    }

    return sheet;
}

} // namespace pcmtp
