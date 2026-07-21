#include "pcmtp/session/PlaylistSession.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace pcmtp {

namespace {

std::string config_directory() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return {};
    }
    return std::string(home) + "/.config/pcm_transport";
}

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buffer[7];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(ch));
                    out += buffer;
                } else {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

void append_json_string(std::ostringstream& out, const std::string& key, const std::string& value) {
    out << '"' << key << "\":\"" << json_escape(value) << '"';
}

void append_json_uint64(std::ostringstream& out, const std::string& key, std::uint64_t value) {
    out << '"' << key << "\":" << value;
}

void append_json_uint32(std::ostringstream& out, const std::string& key, std::uint32_t value) {
    out << '"' << key << "\":" << value;
}

void append_json_uint16(std::ostringstream& out, const std::string& key, std::uint16_t value) {
    out << '"' << key << "\":" << value;
}

void append_json_int(std::ostringstream& out, const std::string& key, int value) {
    out << '"' << key << "\":" << value;
}

void append_json_bool(std::ostringstream& out, const std::string& key, bool value) {
    out << '"' << key << "\":" << (value ? "true" : "false");
}

std::string serialize_track(const PlaylistSessionTrack& track) {
    std::ostringstream out;
    out << '{';
    append_json_string(out, "audio_file_path", track.audio_file_path);
    out << ',';
    append_json_int(out, "track_number", track.track_number);
    out << ',';
    append_json_string(out, "title", track.title);
    out << ',';
    append_json_string(out, "performer", track.performer);
    out << ',';
    append_json_uint64(out, "start_sample", track.start_sample);
    out << ',';
    append_json_uint64(out, "end_sample", track.end_sample);
    out << ',';
    append_json_string(out, "source_label", track.source_label);
    out << ',';
    append_json_uint32(out, "decoded_sample_rate", track.decoded_sample_rate);
    out << ',';
    append_json_uint16(out, "decoded_channels", track.decoded_channels);
    out << ',';
    append_json_uint16(out, "decoded_bits_per_sample", track.decoded_bits_per_sample);
    out << ',';
    append_json_uint32(out, "source_sample_rate", track.source_sample_rate);
    out << ',';
    append_json_uint16(out, "source_bits_per_sample", track.source_bits_per_sample);
    out << ',';
    append_json_bool(out, "native_decode", track.native_decode);
    out << ',';
    append_json_bool(out, "lossless_source", track.lossless_source);
    out << ',';
    append_json_bool(out, "lossy_source", track.lossy_source);
    out << ',';
    append_json_bool(out, "resampled", track.resampled);
    out << ',';
    append_json_uint32(out, "resampled_from_rate", track.resampled_from_rate);
    out << ',';
    append_json_bool(out, "bitdepth_converted", track.bitdepth_converted);
    out << ',';
    append_json_bool(out, "processed_by_ffmpeg", track.processed_by_ffmpeg);
    out << ',';
    append_json_string(out, "codec_name", track.codec_name);
    out << ',';
    append_json_bool(out, "cue_track", track.cue_track);
    out << ',';
    append_json_uint64(out, "cue_album_end_sample", track.cue_album_end_sample);
    out << '}';
    return out.str();
}

class JsonReader {
public:
    explicit JsonReader(std::string text) : text_(std::move(text)) {}

    void skip_whitespace() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    bool consume(char expected) {
        skip_whitespace();
        if (pos_ >= text_.size() || text_[pos_] != expected) {
            return false;
        }
        ++pos_;
        return true;
    }

    bool parse_string(std::string& out) {
        skip_whitespace();
        if (pos_ >= text_.size() || text_[pos_] != '"') {
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            const char ch = text_[pos_++];
            if (ch == '"') {
                return true;
            }
            if (ch != '\\') {
                out += ch;
                continue;
            }
            if (pos_ >= text_.size()) {
                return false;
            }
            const char esc = text_[pos_++];
            switch (esc) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u':
                    if (pos_ + 4 > text_.size()) {
                        return false;
                    }
                    out += '?';
                    pos_ += 4;
                    break;
                default:
                    return false;
            }
        }
        return false;
    }

    bool parse_number(std::uint64_t& out) {
        skip_whitespace();
        const std::size_t begin = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') {
            return false;
        }
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
        if (begin == pos_) {
            return false;
        }
        try {
            out = static_cast<std::uint64_t>(std::stoull(text_.substr(begin, pos_ - begin)));
            return true;
        } catch (...) {
            return false;
        }
    }

    bool parse_bool(bool& out) {
        skip_whitespace();
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out = true;
            return true;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out = false;
            return true;
        }
        return false;
    }

    bool parse_key(std::string& key) {
        if (!parse_string(key)) {
            return false;
        }
        skip_whitespace();
        if (pos_ >= text_.size() || text_[pos_] != ':') {
            return false;
        }
        ++pos_;
        return true;
    }

    bool parse_track(PlaylistSessionTrack& track) {
        if (!consume('{')) {
            return false;
        }
        bool first = true;
        while (true) {
            skip_whitespace();
            if (consume('}')) {
                return true;
            }
            if (!first && !consume(',')) {
                return false;
            }
            first = false;

            std::string key;
            if (!parse_key(key)) {
                return false;
            }

            if (key == "audio_file_path") {
                if (!parse_string(track.audio_file_path)) return false;
            } else if (key == "track_number") {
                std::uint64_t value = 0;
                if (!parse_number(value)) return false;
                track.track_number = static_cast<int>(value);
            } else if (key == "title") {
                if (!parse_string(track.title)) return false;
            } else if (key == "performer") {
                if (!parse_string(track.performer)) return false;
            } else if (key == "start_sample") {
                if (!parse_number(track.start_sample)) return false;
            } else if (key == "end_sample") {
                if (!parse_number(track.end_sample)) return false;
            } else if (key == "source_label") {
                if (!parse_string(track.source_label)) return false;
            } else if (key == "decoded_sample_rate") {
                std::uint64_t value = 0;
                if (!parse_number(value)) return false;
                track.decoded_sample_rate = static_cast<std::uint32_t>(value);
            } else if (key == "decoded_channels") {
                std::uint64_t value = 0;
                if (!parse_number(value)) return false;
                track.decoded_channels = static_cast<std::uint16_t>(value);
            } else if (key == "decoded_bits_per_sample") {
                std::uint64_t value = 0;
                if (!parse_number(value)) return false;
                track.decoded_bits_per_sample = static_cast<std::uint16_t>(value);
            } else if (key == "source_sample_rate") {
                std::uint64_t value = 0;
                if (!parse_number(value)) return false;
                track.source_sample_rate = static_cast<std::uint32_t>(value);
            } else if (key == "source_bits_per_sample") {
                std::uint64_t value = 0;
                if (!parse_number(value)) return false;
                track.source_bits_per_sample = static_cast<std::uint16_t>(value);
            } else if (key == "native_decode") {
                if (!parse_bool(track.native_decode)) return false;
            } else if (key == "lossless_source") {
                if (!parse_bool(track.lossless_source)) return false;
            } else if (key == "lossy_source") {
                if (!parse_bool(track.lossy_source)) return false;
            } else if (key == "resampled") {
                if (!parse_bool(track.resampled)) return false;
            } else if (key == "resampled_from_rate") {
                std::uint64_t value = 0;
                if (!parse_number(value)) return false;
                track.resampled_from_rate = static_cast<std::uint32_t>(value);
            } else if (key == "bitdepth_converted") {
                if (!parse_bool(track.bitdepth_converted)) return false;
            } else if (key == "processed_by_ffmpeg") {
                if (!parse_bool(track.processed_by_ffmpeg)) return false;
            } else if (key == "codec_name") {
                if (!parse_string(track.codec_name)) return false;
            } else if (key == "cue_track") {
                if (!parse_bool(track.cue_track)) return false;
            } else if (key == "cue_album_end_sample") {
                if (!parse_number(track.cue_album_end_sample)) return false;
            } else if (!skip_value()) {
                return false;
            }
        }
    }

    bool parse_snapshot(PlaylistSessionSnapshot& snapshot) {
        if (!consume('{')) {
            return false;
        }

        bool first = true;
        while (true) {
            skip_whitespace();
            if (consume('}')) {
                return true;
            }
            if (!first && !consume(',')) {
                return false;
            }
            first = false;

            std::string key;
            if (!parse_key(key)) {
                return false;
            }

            if (key == "version") {
                std::uint64_t version = 0;
                if (!parse_number(version) || version != PlaylistSessionSnapshot::kFormatVersion) {
                    return false;
                }
            } else if (key == "current_track_index") {
                std::uint64_t index = 0;
                if (!parse_number(index)) {
                    return false;
                }
                snapshot.current_track_index = static_cast<std::size_t>(index);
            } else if (key == "tracks") {
                if (!consume('[')) {
                    return false;
                }
                bool track_first = true;
                while (true) {
                    skip_whitespace();
                    if (consume(']')) {
                        break;
                    }
                    if (!track_first && !consume(',')) {
                        return false;
                    }
                    track_first = false;
                    PlaylistSessionTrack track;
                    if (!parse_track(track)) {
                        return false;
                    }
                    snapshot.tracks.push_back(std::move(track));
                }
            } else if (!skip_value()) {
                return false;
            }
        }
    }

private:
    bool skip_value() {
        skip_whitespace();
        if (pos_ >= text_.size()) {
            return false;
        }
        const char ch = text_[pos_];
        if (ch == '"') {
            std::string ignored;
            return parse_string(ignored);
        }
        if (ch == '{') {
            std::size_t depth = 0;
            do {
                if (text_[pos_] == '{') {
                    ++depth;
                } else if (text_[pos_] == '}') {
                    --depth;
                }
                ++pos_;
            } while (pos_ < text_.size() && depth > 0);
            return depth == 0;
        }
        if (ch == '[') {
            std::size_t depth = 0;
            do {
                if (text_[pos_] == '[') {
                    ++depth;
                } else if (text_[pos_] == ']') {
                    --depth;
                }
                ++pos_;
            } while (pos_ < text_.size() && depth > 0);
            return depth == 0;
        }
        while (pos_ < text_.size() && text_[pos_] != ',' && text_[pos_] != '}' && text_[pos_] != ']') {
            ++pos_;
        }
        return true;
    }

    std::string text_;
    std::size_t pos_ = 0;
};

} // namespace

std::string PlaylistSession::session_path() {
    const std::string dir = config_directory();
    if (dir.empty()) {
        return {};
    }
    return dir + "/session.json";
}

bool PlaylistSession::load(PlaylistSessionSnapshot& out) const {
    out = PlaylistSessionSnapshot{};
    const std::string path = session_path();
    if (path.empty()) {
        return false;
    }

    std::ifstream in(path.c_str());
    if (!in) {
        return false;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    JsonReader reader(buffer.str());
    if (!reader.parse_snapshot(out)) {
        out = PlaylistSessionSnapshot{};
        return false;
    }
    return !out.tracks.empty();
}

bool PlaylistSession::save(const PlaylistSessionSnapshot& snapshot) const {
    const std::string dir = config_directory();
    if (dir.empty()) {
        return false;
    }
    std::system((std::string("mkdir -p '") + dir + "'").c_str());

    const std::string path = session_path();
    std::ofstream out(path.c_str(), std::ios::trunc);
    if (!out) {
        return false;
    }

    out << "{\n";
    out << "  \"version\": " << PlaylistSessionSnapshot::kFormatVersion << ",\n";
    out << "  \"current_track_index\": " << snapshot.current_track_index << ",\n";
    out << "  \"tracks\": [\n";
    for (std::size_t i = 0; i < snapshot.tracks.size(); ++i) {
        out << "    " << serialize_track(snapshot.tracks[i]);
        if (i + 1 < snapshot.tracks.size()) {
            out << ',';
        }
        out << '\n';
    }
    out << "  ]\n";
    out << "}\n";
    return true;
}

} // namespace pcmtp
