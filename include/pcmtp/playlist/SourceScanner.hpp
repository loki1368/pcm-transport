#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace pcmtp {

struct ScannedSourcePath {
    std::string path;
    std::string top_level_source_path;
};

struct SourceScanResult {
    std::vector<ScannedSourcePath> sources;
    std::vector<std::string> errors;
    bool cancelled = false;
};

class SourceScanner {
public:
    static bool is_supported_media_path(const std::string& path);
    static bool is_supported_audio_path(const std::string& path);

    static SourceScanResult scan(const std::vector<std::string>& top_level_paths,
                                 const std::atomic<bool>* cancel_requested);
};

} // namespace pcmtp
