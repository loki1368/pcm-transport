#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace pcmtp {

enum class StreamHealthStatus {
    Unknown,
    Ok,
    Broken,
};

struct StreamHealthRecord {
    StreamHealthStatus status = StreamHealthStatus::Unknown;
    int fail_count = 0;
    std::string last_failure;
    std::string last_success;
    std::string last_error;
};

class StreamHealthRegistry {
public:
    void load();
    void save() const;

    StreamHealthStatus status_for(const std::string& stream_url) const;
    bool is_broken(const std::string& stream_url) const;

    bool mark_broken(const std::string& stream_url, const std::string& error);
    bool mark_ok(const std::string& stream_url);

private:
    StreamHealthRecord* record_for(const std::string& stream_url);
    const StreamHealthRecord* find_record(const std::string& stream_url) const;
    static std::string config_path();

    std::unordered_map<std::string, StreamHealthRecord> records_;
};

} // namespace pcmtp
