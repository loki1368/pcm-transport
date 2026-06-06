#pragma once

#include <fstream>
#include <mutex>
#include <string>

namespace pcmtp {

class Logger {
public:
    static Logger& instance();

    void configure(bool enabled, const std::string& path = std::string(), bool errors_only = false);
    bool enabled() const;
    bool debug_enabled() const;
    void info(const std::string& message);
    void error(const std::string& message);
    void debug(const std::string& message);

private:
    Logger() = default;
    void write(const char* level, const std::string& message);

    mutable std::mutex mutex_;
    bool enabled_ = false;
    bool errors_only_ = false;
    std::ofstream stream_;
};

} // namespace pcmtp
