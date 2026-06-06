#include "pcmtp/util/Logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cstring>

namespace pcmtp {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::configure(bool enabled, const std::string& path, bool errors_only) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
    errors_only_ = errors_only;
    if (stream_.is_open()) {
        stream_.close();
    }
    if (enabled_) {
        const std::string final_path = path.empty() ? "pcm_transport.log" : path;
        stream_.open(final_path.c_str(), std::ios::out | std::ios::app);
        if (!stream_) {
            enabled_ = false;
        }
    }
}

bool Logger::enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

bool Logger::debug_enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_ && !errors_only_ && stream_.is_open();
}

void Logger::info(const std::string& message) {
    write("INFO", message);
}

void Logger::error(const std::string& message) {
    write("ERROR", message);
}

void Logger::debug(const std::string& message) {
    write("DEBUG", message);
}

void Logger::write(const char* level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || !stream_) {
        return;
    }
    if (errors_only_ && std::string(level) != "ERROR") {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm;
#if defined(_WIN32)
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif

    stream_ << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S")
            << " [" << level << "] " << message << '\n';
    if (std::strcmp(level, "ERROR") == 0) {
        stream_.flush();
    }
}

} // namespace pcmtp
