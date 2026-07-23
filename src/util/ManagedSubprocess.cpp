#include "pcmtp/util/ManagedSubprocess.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace pcmtp {
namespace {

constexpr std::size_t kMaximumCapturedStderrBytes = 8192;
constexpr std::chrono::milliseconds kPollInterval(50);
constexpr std::chrono::milliseconds kTerminateGracePeriod(250);

void close_fd(int* fd) {
    if (fd != nullptr && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void append_limited(std::string* output,
                    const char* data,
                    std::size_t size,
                    std::size_t limit) {
    if (output == nullptr || data == nullptr || size == 0 || output->size() >= limit) {
        return;
    }
    const std::size_t available = limit - output->size();
    output->append(data, std::min(size, available));
}

void drain_fd(int* fd, std::string* output, std::size_t limit) {
    if (fd == nullptr || *fd < 0 || output == nullptr) {
        return;
    }

    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = read(*fd, buffer.data(), buffer.size());
        if (count > 0) {
            append_limited(output, buffer.data(), static_cast<std::size_t>(count), limit);
            continue;
        }
        if (count == 0) {
            close_fd(fd);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        close_fd(fd);
        return;
    }
}

std::vector<char*> build_argv(const std::vector<std::string>& arguments) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

} // namespace

ManagedSubprocess::~ManagedSubprocess() {
    cancel();
}

void ManagedSubprocess::signal_process_group(pid_t pid, int signal_number) {
    if (pid <= 0) {
        return;
    }
    if (kill(-pid, signal_number) != 0 && errno == ESRCH) {
        kill(pid, signal_number);
    }
}

void ManagedSubprocess::cancel() {
    pid_t pid = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++cancellation_generation_;
        pid = active_pid_;
    }
    signal_process_group(pid, SIGTERM);
}

ManagedSubprocessResult ManagedSubprocess::run(
    const std::vector<std::string>& arguments,
    std::chrono::milliseconds timeout) {
    ManagedSubprocessResult result;
    if (arguments.empty() || arguments.front().empty()) {
        result.stderr_text = "empty subprocess command";
        return result;
    }

    std::uint64_t run_generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_pid_ > 0) {
            result.stderr_text = "managed subprocess is already active";
            return result;
        }
        run_generation = cancellation_generation_;
    }

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        const std::string error = std::string("cannot create subprocess pipe: ") + std::strerror(errno);
        close_fd(&stdout_pipe[0]);
        close_fd(&stdout_pipe[1]);
        close_fd(&stderr_pipe[0]);
        close_fd(&stderr_pipe[1]);
        result.stderr_text = error;
        return result;
    }

    std::vector<char*> argv = build_argv(arguments);
    const pid_t pid = fork();
    if (pid < 0) {
        const std::string error = std::string("cannot fork subprocess: ") + std::strerror(errno);
        close_fd(&stdout_pipe[0]);
        close_fd(&stdout_pipe[1]);
        close_fd(&stderr_pipe[0]);
        close_fd(&stderr_pipe[1]);
        result.stderr_text = error;
        return result;
    }

    if (pid == 0) {
        setpgid(0, 0);
        if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close_fd(&stdout_pipe[0]);
        close_fd(&stdout_pipe[1]);
        close_fd(&stderr_pipe[0]);
        close_fd(&stderr_pipe[1]);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    result.launched = true;
    close_fd(&stdout_pipe[1]);
    close_fd(&stderr_pipe[1]);
    setpgid(pid, pid);
    if (!set_nonblocking(stdout_pipe[0]) || !set_nonblocking(stderr_pipe[0])) {
        const std::string error = std::string("cannot configure subprocess pipe: ") +
                                  std::strerror(errno);
        signal_process_group(pid, SIGKILL);
        int failed_status = 0;
        while (waitpid(pid, &failed_status, 0) < 0 && errno == EINTR) {}
        close_fd(&stdout_pipe[0]);
        close_fd(&stderr_pipe[0]);
        result.stderr_text = error;
        result.exit_status = -1;
        return result;
    }

    bool cancelled_before_registration = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_before_registration = cancellation_generation_ != run_generation;
        if (!cancelled_before_registration) {
            active_pid_ = pid;
        }
    }

    if (cancelled_before_registration) {
        result.cancelled = true;
        signal_process_group(pid, SIGTERM);
    }

    const auto started_at = std::chrono::steady_clock::now();
    auto terminate_sent_at = cancelled_before_registration
        ? started_at
        : std::chrono::steady_clock::time_point{};
    bool terminate_sent = cancelled_before_registration;
    bool kill_sent = false;
    bool child_reaped = false;
    bool wait_status_valid = false;
    int wait_status = 0;

    while (!child_reaped || stdout_pipe[0] >= 0 || stderr_pipe[0] >= 0) {
        const auto now = std::chrono::steady_clock::now();
        bool externally_cancelled = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            externally_cancelled = cancellation_generation_ != run_generation;
        }

        if (externally_cancelled && !result.timed_out) {
            result.cancelled = true;
        }
        if (!result.cancelled && timeout.count() > 0 && now - started_at >= timeout) {
            result.timed_out = true;
        }

        if ((result.cancelled || result.timed_out) && !terminate_sent) {
            signal_process_group(pid, SIGTERM);
            terminate_sent = true;
            terminate_sent_at = now;
        } else if (terminate_sent && !kill_sent &&
                   now - terminate_sent_at >= kTerminateGracePeriod) {
            signal_process_group(pid, SIGKILL);
            kill_sent = true;
        }

        std::array<pollfd, 2> poll_fds{};
        nfds_t poll_count = 0;
        if (stdout_pipe[0] >= 0) {
            poll_fds[poll_count].fd = stdout_pipe[0];
            poll_fds[poll_count].events = POLLIN | POLLHUP | POLLERR;
            ++poll_count;
        }
        if (stderr_pipe[0] >= 0) {
            poll_fds[poll_count].fd = stderr_pipe[0];
            poll_fds[poll_count].events = POLLIN | POLLHUP | POLLERR;
            ++poll_count;
        }

        const int poll_result = poll(poll_fds.data(), poll_count,
                                     static_cast<int>(kPollInterval.count()));
        if (poll_result < 0 && errno != EINTR) {
            result.stderr_text = std::string("subprocess poll failed: ") + std::strerror(errno);
            result.cancelled = true;
            if (!terminate_sent) {
                signal_process_group(pid, SIGTERM);
                terminate_sent = true;
                terminate_sent_at = now;
            }
        }

        drain_fd(&stdout_pipe[0], &result.stdout_text,
                 std::numeric_limits<std::size_t>::max());
        drain_fd(&stderr_pipe[0], &result.stderr_text,
                 kMaximumCapturedStderrBytes);

        if (!child_reaped) {
            const pid_t waited = waitpid(pid, &wait_status, WNOHANG);
            if (waited == pid) {
                child_reaped = true;
                wait_status_valid = true;
                std::lock_guard<std::mutex> lock(mutex_);
                if (active_pid_ == pid) {
                    active_pid_ = -1;
                }
            } else if (waited < 0 && errno != EINTR) {
                child_reaped = true;
                result.stderr_text += std::string("\nwaitpid failed: ") + std::strerror(errno);
                std::lock_guard<std::mutex> lock(mutex_);
                if (active_pid_ == pid) {
                    active_pid_ = -1;
                }
            }
        }
    }

    close_fd(&stdout_pipe[0]);
    close_fd(&stderr_pipe[0]);

    if (!child_reaped) {
        pid_t waited = -1;
        do {
            waited = waitpid(pid, &wait_status, 0);
        } while (waited < 0 && errno == EINTR);
        wait_status_valid = waited == pid;
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_pid_ == pid) {
            active_pid_ = -1;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancellation_generation_ != run_generation && !result.timed_out) {
            result.cancelled = true;
        }
    }

    if (result.cancelled) {
        result.exit_status = -2;
    } else if (result.timed_out) {
        result.exit_status = -3;
    } else if (wait_status_valid && WIFEXITED(wait_status)) {
        result.exit_status = WEXITSTATUS(wait_status);
    } else if (wait_status_valid && WIFSIGNALED(wait_status)) {
        result.exit_status = 128 + WTERMSIG(wait_status);
    } else {
        result.exit_status = -1;
    }

    return result;
}

} // namespace pcmtp
