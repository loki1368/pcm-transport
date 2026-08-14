#include "pcmtp/stream/StreamHealthRegistry.hpp"

#include "pcmtp/util/MediaUri.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace pcmtp {
namespace {

constexpr int kFormatVersion = 1;

std::string trim_copy(const std::string& value) {
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

std::string iso8601_now_local() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return buffer;
}

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string json_unescape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            out.push_back(value[i]);
            continue;
        }
        const char next = value[++i];
        switch (next) {
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(next); break;
        }
    }
    return out;
}

std::string read_json_string(const std::string& text, std::size_t quote_pos) {
    if (quote_pos >= text.size() || text[quote_pos] != '"') {
        return std::string();
    }
    std::size_t i = quote_pos + 1;
    std::string raw;
    while (i < text.size()) {
        const char ch = text[i++];
        if (ch == '"') {
            return json_unescape(raw);
        }
        if (ch == '\\' && i < text.size()) {
            raw.push_back(ch);
            raw.push_back(text[i++]);
            continue;
        }
        raw.push_back(ch);
    }
    return std::string();
}

std::size_t find_json_field(const std::string& object_text, const char* field_name, std::size_t from = 0) {
  const std::string needle = std::string("\"") + field_name + "\"";
  return object_text.find(needle, from);
}

StreamHealthStatus status_from_string(const std::string& value) {
    if (value == "ok") {
        return StreamHealthStatus::Ok;
    }
    if (value == "broken") {
        return StreamHealthStatus::Broken;
    }
    return StreamHealthStatus::Unknown;
}

const char* status_to_string(StreamHealthStatus status) {
    switch (status) {
        case StreamHealthStatus::Ok:
            return "ok";
        case StreamHealthStatus::Broken:
            return "broken";
        case StreamHealthStatus::Unknown:
        default:
            return "unknown";
    }
}

int parse_int_field(const std::string& object_text, const char* field_name, int fallback) {
    const std::size_t pos = find_json_field(object_text, field_name);
    if (pos == std::string::npos) {
        return fallback;
    }
    const std::size_t colon = object_text.find(':', pos);
    if (colon == std::string::npos) {
        return fallback;
    }
    std::size_t i = colon + 1;
    while (i < object_text.size() && std::isspace(static_cast<unsigned char>(object_text[i])) != 0) {
        ++i;
    }
    int sign = 1;
    if (i < object_text.size() && object_text[i] == '-') {
        sign = -1;
        ++i;
    }
    int value = 0;
    bool found = false;
    while (i < object_text.size() && std::isdigit(static_cast<unsigned char>(object_text[i])) != 0) {
        value = value * 10 + (object_text[i] - '0');
        ++i;
        found = true;
    }
    return found ? sign * value : fallback;
}

std::string parse_string_field(const std::string& object_text, const char* field_name) {
    const std::size_t pos = find_json_field(object_text, field_name);
    if (pos == std::string::npos) {
        return std::string();
    }
    const std::size_t quote = object_text.find('"', pos + std::strlen(field_name) + 2);
    if (quote == std::string::npos) {
        return std::string();
    }
    return read_json_string(object_text, quote);
}

} // namespace

std::string StreamHealthRegistry::config_path() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return std::string();
    }
    return std::string(home) + "/.config/pcm_transport/stream_health.json";
}

void StreamHealthRegistry::load() {
    records_.clear();
    const std::string path = config_path();
    if (path.empty()) {
        return;
    }

    std::ifstream in(path.c_str());
    if (!in) {
        return;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();

    std::size_t search_from = 0;
    while (true) {
        const std::size_t object_start = text.find('{', search_from);
        if (object_start == std::string::npos) {
            break;
        }
        const std::size_t object_end = text.find('}', object_start);
        if (object_end == std::string::npos) {
            break;
        }
        const std::string object_text = text.substr(object_start, object_end - object_start + 1);
        search_from = object_end + 1;

        const std::string url = parse_string_field(object_text, "url");
        if (url.empty()) {
            continue;
        }

        StreamHealthRecord record;
        record.status = status_from_string(parse_string_field(object_text, "status"));
        record.fail_count = parse_int_field(object_text, "fail_count", 0);
        record.last_failure = parse_string_field(object_text, "last_failure");
        record.last_success = parse_string_field(object_text, "last_success");
        record.last_error = parse_string_field(object_text, "last_error");
        records_[normalize_stream_url(url)] = record;
    }
}

void StreamHealthRegistry::save() const {
    const std::string path = config_path();
    if (path.empty()) {
        return;
    }

    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        const std::string dir = path.substr(0, slash);
        std::system((std::string("mkdir -p '") + dir + "'").c_str());
    }

    std::ofstream out(path.c_str(), std::ios::trunc);
    if (!out) {
        return;
    }

    out << "{\n  \"version\": " << kFormatVersion << ",\n  \"streams\": [\n";
    bool first = true;
    for (const auto& item : records_) {
        const StreamHealthRecord& record = item.second;
        if (!first) {
            out << ",\n";
        }
        first = false;
        out << "    {\n";
        out << "      \"url\": \"" << json_escape(item.first) << "\",\n";
        out << "      \"status\": \"" << status_to_string(record.status) << "\",\n";
        out << "      \"fail_count\": " << record.fail_count << ",\n";
        out << "      \"last_failure\": \"" << json_escape(record.last_failure) << "\",\n";
        out << "      \"last_success\": \"" << json_escape(record.last_success) << "\",\n";
        out << "      \"last_error\": \"" << json_escape(record.last_error) << "\"\n";
        out << "    }";
    }
    out << "\n  ]\n}\n";
}

StreamHealthStatus StreamHealthRegistry::status_for(const std::string& stream_url) const {
    const StreamHealthRecord* record = find_record(stream_url);
    if (record == nullptr) {
        return StreamHealthStatus::Unknown;
    }
    return record->status;
}

bool StreamHealthRegistry::is_broken(const std::string& stream_url) const {
    return status_for(stream_url) == StreamHealthStatus::Broken;
}

const StreamHealthRecord* StreamHealthRegistry::find_record(const std::string& stream_url) const {
    if (!is_remote_media_uri(stream_url)) {
        return nullptr;
    }
    const auto it = records_.find(normalize_stream_url(stream_url));
    if (it == records_.end()) {
        return nullptr;
    }
    return &it->second;
}

StreamHealthRecord* StreamHealthRegistry::record_for(const std::string& stream_url) {
    if (!is_remote_media_uri(stream_url)) {
        return nullptr;
    }
    return &records_[normalize_stream_url(stream_url)];
}

bool StreamHealthRegistry::mark_broken(const std::string& stream_url, const std::string& error) {
    StreamHealthRecord* record = record_for(stream_url);
    if (record == nullptr) {
        return false;
    }

    const std::string message = trim_copy(error);
    const bool changed = record->status != StreamHealthStatus::Broken ||
                         record->last_error != (message.empty() ? "Stream unavailable" : message);

    record->status = StreamHealthStatus::Broken;
    ++record->fail_count;
    record->last_failure = iso8601_now_local();
    record->last_error = message.empty() ? "Stream unavailable" : message;
    save();
    return changed;
}

bool StreamHealthRegistry::mark_ok(const std::string& stream_url) {
    StreamHealthRecord* record = record_for(stream_url);
    if (record == nullptr) {
        return false;
    }

    if (record->status == StreamHealthStatus::Ok && record->last_error.empty()) {
        return false;
    }

    record->status = StreamHealthStatus::Ok;
    record->last_success = iso8601_now_local();
    record->last_error.clear();
    save();
    return true;
}

} // namespace pcmtp
