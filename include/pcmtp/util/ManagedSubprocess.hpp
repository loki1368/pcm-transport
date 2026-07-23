#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <sys/types.h>

namespace pcmtp {

struct ManagedSubprocessResult {
    bool launched = false;
    bool cancelled = false;
    bool timed_out = false;
    int exit_status = -1;
    std::string stdout_text;
    std::string stderr_text;
};

class ManagedSubprocess {
public:
    ManagedSubprocess() = default;
    ~ManagedSubprocess();

    ManagedSubprocess(const ManagedSubprocess&) = delete;
    ManagedSubprocess& operator=(const ManagedSubprocess&) = delete;

    ManagedSubprocessResult run(const std::vector<std::string>& arguments,
                                std::chrono::milliseconds timeout);
    void cancel();

private:
    static void signal_process_group(pid_t pid, int signal_number);

    mutable std::mutex mutex_;
    pid_t active_pid_ = -1;
    std::uint64_t cancellation_generation_ = 0;
};

} // namespace pcmtp
