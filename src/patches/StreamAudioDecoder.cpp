#include "pcmtp/patches/StreamAudioDecoder.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "pcmtp/util/Logger.hpp"
#include "pcmtp/util/ManagedSubprocess.hpp"
#include "pcmtp/util/MediaUri.hpp"
#include "pcmtp/util/TextEncoding.hpp"

namespace pcmtp {
namespace {

struct CommandCaptureResult {
    std::string stdout_text;
    std::string stderr_text;
    int status = 0;
    bool cancelled = false;
    bool timed_out = false;
};

std::vector<std::string> with_low_priority_prefix(const std::vector<std::string>& arguments) {
    if (arguments.empty()) {
        return arguments;
    }
    if (access("/usr/bin/ionice", X_OK) == 0 || access("/bin/ionice", X_OK) == 0) {
        std::vector<std::string> prefixed = {"nice", "-n", "19", "ionice", "-c3"};
        prefixed.insert(prefixed.end(), arguments.begin(), arguments.end());
        return prefixed;
    }
    std::vector<std::string> prefixed = {"nice", "-n", "19"};
    prefixed.insert(prefixed.end(), arguments.begin(), arguments.end());
    return prefixed;
}

CommandCaptureResult run_command_capture(const std::vector<std::string>& arguments,
                                         bool background_priority,
                                         ManagedSubprocess* managed_process) {
    constexpr auto kProbeTimeout = std::chrono::seconds(30);
    ManagedSubprocess local_process;
    ManagedSubprocess* process = managed_process != nullptr ? managed_process : &local_process;
    const std::vector<std::string>& launch_arguments =
        (background_priority && managed_process == nullptr) ? with_low_priority_prefix(arguments) : arguments;
    const ManagedSubprocessResult managed_result = process->run(launch_arguments, kProbeTimeout);

    CommandCaptureResult result;
    result.stdout_text = managed_result.stdout_text;
    result.stderr_text = managed_result.stderr_text;
    result.status = managed_result.exit_status;
    result.cancelled = managed_result.cancelled;
    result.timed_out = managed_result.timed_out;
    return result;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

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

std::uint16_t bits_from_sample_fmt(const std::string& sample_fmt) {
    const std::string fmt = lower_copy(sample_fmt);
    if (fmt.find("s16") != std::string::npos || fmt.find("u16") != std::string::npos) return 16;
    if (fmt.find("s24") != std::string::npos || fmt.find("u24") != std::string::npos) return 24;
    if (fmt.find("s32") != std::string::npos || fmt.find("u32") != std::string::npos) return 32;
    if (fmt == "flt" || fmt == "fltp") return 32;
    return 0;
}

bool codec_is_lossless(const std::string& codec_name, const std::string& ext) {
    const std::string codec = lower_copy(codec_name);
    if (codec == "alac" || codec == "flac" || codec == "ape" || codec == "wavpack" || codec == "tta" || codec == "tak" || codec == "wmalossless") {
        return true;
    }
    if (codec.size() >= 4 && codec.compare(0, 4, "pcm_") == 0) {
        return true;
    }
    if (codec == "aiff" || codec == "aifc" || codec == "dsd_lsbf" || codec == "dsd_msbf" || codec == "dsd_lsbf_planar" || codec == "dsd_msbf_planar" || codec == "dst") {
        return true;
    }
    if (ext == ".wav" || ext == ".wave" || ext == ".bwf" || ext == ".aiff" || ext == ".aif" || ext == ".ape" || ext == ".wv" || ext == ".flac" || ext == ".tta" || ext == ".tak" || ext == ".dsf" || ext == ".dff") {
        return true;
    }
    return false;
}

std::string read_text_file_limited(const std::string& path, std::size_t max_bytes = 8192) {
    if (path.empty()) {
        return std::string();
    }
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        return std::string();
    }
    std::string data;
    data.resize(max_bytes);
    input.read(&data[0], static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<std::size_t>(input.gcount()));
    return data;
}

void kill_subprocess_tree(pid_t pid) {
    if (pid <= 0) {
        return;
    }
    const pid_t pgid = getpgid(pid);
    if (pgid > 0) {
        kill(-pgid, SIGKILL);
    } else {
        kill(pid, SIGKILL);
    }
}

} // namespace

bool StreamAudioDecoder::is_stream_uri(const std::string& path) {
    return is_remote_media_uri(path);
}

ExternalAudioInfo StreamAudioDecoder::probe_metadata(const std::string& path,
                                                     std::uint32_t forced_output_sample_rate,
                                                     std::uint16_t forced_output_bits_per_sample,
                                                     bool background_priority,
                                                     ManagedSubprocess* probe_process) {
    if (!is_stream_uri(path)) {
        throw std::runtime_error("StreamAudioDecoder does not support this file type");
    }

    ExternalAudioInfo info;
    info.format.sample_rate = 44100;
    info.format.channels = 2;
    info.format.bits_per_sample = 16;

    const std::vector<std::string> probe_arguments = {
        "ffprobe", "-v", "error", "-select_streams", "a:0",
        "-timeout", "8000000", "-rw_timeout", "8000000",
        "-analyzeduration", "2000000", "-probesize", "200000",
        "-user_agent", "pcm-transport/0.9",
        "-show_entries",
        "stream=codec_name,sample_fmt,sample_rate,channels,bits_per_sample,bits_per_raw_sample,bit_rate:"
        "format=duration:format_tags=title,artist",
        "-of", "default=nokey=0:noprint_wrappers=1",
        path
    };
    Logger::instance().debug("StreamAudioDecoder stream probe: " + path);
    const CommandCaptureResult probe = run_command_capture(probe_arguments, background_priority, probe_process);
    if (probe.cancelled) {
        throw std::runtime_error("metadata probe cancelled");
    }
    if (probe.timed_out) {
        throw std::runtime_error("metadata probe timed out");
    }
    if (probe.status != 0) {
        Logger::instance().error("ffprobe failed for stream: " + path +
                                 (probe.stderr_text.empty() ? std::string() : ("\nffprobe stderr:\n" + probe.stderr_text)));
    }

    std::istringstream ps(probe.stdout_text);
    std::string line;
    std::string sample_fmt;
    bool saw_sample_rate = false;
    while (std::getline(ps, line)) {
        line = trim_copy(line);
        const std::size_t pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, pos);
        const std::string value = line.substr(pos + 1);
        try {
            if (key == "codec_name") {
                info.codec_name = lower_copy(value);
            } else if (key == "sample_fmt") {
                sample_fmt = lower_copy(value);
            } else if (key == "sample_rate") {
                info.format.sample_rate = static_cast<std::uint32_t>(std::stoul(value));
                saw_sample_rate = true;
            } else if (key == "channels") {
                info.format.channels = static_cast<std::uint16_t>(std::stoul(value));
            } else if ((key == "bits_per_sample" || key == "bits_per_raw_sample") && !value.empty() && value != "N/A") {
                const std::uint16_t bits = static_cast<std::uint16_t>(std::stoul(value));
                if (bits == 16 || bits == 24 || bits == 32) {
                    info.format.bits_per_sample = bits;
                }
            } else if (key == "bit_rate" && !value.empty() && value != "N/A") {
                info.bit_rate = static_cast<std::uint32_t>(std::stoul(value));
            } else if (key == "TAG:title" || key == "title") {
                info.tags.title = pcmtp::text::normalize_metadata_value(value);
            } else if (key == "TAG:artist" || key == "artist") {
                info.tags.artist = pcmtp::text::normalize_metadata_value(value);
            }
        } catch (...) {
        }
    }

    const std::uint16_t fmt_bits = bits_from_sample_fmt(sample_fmt);
    if ((info.format.bits_per_sample == 0 || info.format.bits_per_sample == 16) && fmt_bits > 0) {
        info.format.bits_per_sample = fmt_bits;
    }
    if (info.format.channels == 0) {
        info.format.channels = 2;
    }
    if (info.format.bits_per_sample != 16 && info.format.bits_per_sample != 24 && info.format.bits_per_sample != 32) {
        info.format.bits_per_sample = 16;
    }
    info.total_samples_per_channel = 0;
    info.source_total_samples_per_channel = 0;
    info.duration_reliable = false;
    info.lossless = codec_is_lossless(info.codec_name, std::string());
    info.source_format = info.format;
    info.live_format_probed = probe.status == 0 && saw_sample_rate && info.format.sample_rate > 0;
    if (forced_output_sample_rate > 0) {
        info.format.sample_rate = forced_output_sample_rate;
    }
    if (forced_output_bits_per_sample == 16 || forced_output_bits_per_sample == 24 ||
        forced_output_bits_per_sample == 32) {
        info.format.bits_per_sample = forced_output_bits_per_sample;
    }
    return info;
}

ExternalAudioInfo StreamAudioDecoder::probe_info(const std::string& path,
                                                 std::uint32_t forced_output_sample_rate,
                                                 std::uint16_t forced_output_bits_per_sample) {
    ExternalAudioInfo info = probe_metadata(path, forced_output_sample_rate, forced_output_bits_per_sample);
    info.tags = GenericTags{};
    return info;
}

bool StreamAudioDecoder::verify_stream_playback(const std::string& path,
                                                const ExternalAudioInfo& probed_info,
                                                std::uint32_t forced_output_sample_rate,
                                                std::uint16_t forced_output_bits_per_sample) {
    if (!is_stream_uri(path)) {
        return true;
    }
    try {
        std::unique_ptr<StreamAudioDecoder> decoder(new StreamAudioDecoder(forced_output_sample_rate,
                                                                           forced_output_bits_per_sample));
        ExternalAudioInfo known = probed_info;
        known.live_format_probed = true;
        decoder->set_known_info(known);
        decoder->open(path);
        PcmSample buffer[2048];
        const std::size_t got = decoder->read_samples(buffer, 2048);
        decoder->interrupt();
        return got > 0;
    } catch (const std::exception& ex) {
        Logger::instance().debug(std::string("Stream verify failed: ") + path + " -> " + ex.what());
        return false;
    } catch (...) {
        Logger::instance().debug(std::string("Stream verify failed: ") + path);
        return false;
    }
}

std::string StreamAudioDecoder::decode_command(double seconds) const {
    std::string cmd = ExternalAudioDecoder::decode_command(seconds);
    const std::string prefix = "ffmpeg -v error -nostdin ";
    std::string stream_opts = "-reconnect 1 -reconnect_at_eof 1 -reconnect_streamed 1 -reconnect_delay_max 30 "
                              "-timeout 8000000 -rw_timeout 8000000 -multiple_requests 1 "
                              "-user_agent 'pcm-transport/0.9' ";
    if (is_hls_media_uri(source_path())) {
        stream_opts += "-live_start_index -3 -hls_allow_cache 1 ";
    }
    const std::size_t pos = cmd.find(prefix);
    if (pos != std::string::npos) {
        cmd.insert(pos + prefix.size(), stream_opts);
    }
    return cmd;
}

ExternalAudioInfo StreamAudioDecoder::effective_probe_info(const std::string& path) const {
    if (have_known_info_ && known_info_.live_format_probed) {
        ExternalAudioInfo cached = known_info_;
        if (forced_output_sample_rate_ > 0) {
            cached.format.sample_rate = forced_output_sample_rate_;
        }
        if (forced_output_bits_per_sample_ == 16 || forced_output_bits_per_sample_ == 24 ||
            forced_output_bits_per_sample_ == 32) {
            cached.format.bits_per_sample = forced_output_bits_per_sample_;
        }
        Logger::instance().debug("Using cached stream format for: " + path + " -> " +
                                 std::to_string(cached.source_format.sample_rate) + " Hz");
        return cached;
    }

    ExternalAudioInfo probed = probe_info(path, forced_output_sample_rate_, forced_output_bits_per_sample_);
    if (probed.live_format_probed) {
        Logger::instance().info("Stream format probed: " + path + " -> " +
                                std::to_string(probed.source_format.sample_rate) + " Hz / " +
                                std::to_string(probed.source_format.bits_per_sample) + "-bit / " +
                                std::to_string(probed.source_format.channels) + " ch");
        return probed;
    }
    if (have_known_info_) {
        Logger::instance().debug("Stream probe unavailable, using playlist hints for: " + path);
        ExternalAudioInfo fallback = known_info_;
        if (forced_output_sample_rate_ > 0) {
            fallback.format.sample_rate = forced_output_sample_rate_;
        }
        if (forced_output_bits_per_sample_ == 16 || forced_output_bits_per_sample_ == 24 ||
            forced_output_bits_per_sample_ == 32) {
            fallback.format.bits_per_sample = forced_output_bits_per_sample_;
        }
        return fallback;
    }
    return probed;
}

std::size_t StreamAudioDecoder::read_samples(PcmSample* destination, std::size_t max_samples) {
    if (!opened_ || pipe_ == nullptr || interrupt_requested_) {
        reached_eof_ = true;
        return 0;
    }

    const std::size_t bps = bytes_per_sample();
    raw_buffer_.resize(max_samples * bps);

    const int fd = fileno(pipe_);
    if (fd < 0) {
        reached_eof_ = true;
        return 0;
    }
    constexpr int kPollMs = 200;
    constexpr int kConnectWaitMs = 8000;
    constexpr int kReadWaitMs = 15000;
    const int max_wait_ms = current_samples_per_channel_ == 0 ? kConnectWaitMs : kReadWaitMs;
    int waited_ms = 0;
    for (;;) {
        if (interrupt_requested_) {
            reached_eof_ = true;
            return 0;
        }
        struct pollfd pfd = {fd, POLLIN, 0};
        const int ready = poll(&pfd, 1, kPollMs);
        if (ready > 0) {
            break;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            reached_eof_ = true;
            return 0;
        }
        if (child_pid_ > 0) {
            int status = 0;
            if (waitpid(child_pid_, &status, WNOHANG) > 0) {
                child_pid_ = 0;
                reached_eof_ = true;
                return 0;
            }
        } else {
            reached_eof_ = true;
            return 0;
        }
        waited_ms += kPollMs;
        if (waited_ms >= max_wait_ms) {
            Logger::instance().error("Stream read timed out: " + path_);
            if (child_pid_ > 0) {
                kill_subprocess_tree(child_pid_);
                int status = 0;
                waitpid(child_pid_, &status, 0);
                child_pid_ = 0;
            }
            reached_eof_ = true;
            return 0;
        }
    }

    if (pipe_ == nullptr || interrupt_requested_) {
        reached_eof_ = true;
        return 0;
    }

    const std::size_t got_bytes = fread(raw_buffer_.data(), 1, raw_buffer_.size(), pipe_);
    const std::size_t got = got_bytes / bps;
    if (got == 0 && !zero_read_logged_) {
        const bool expected_more = (total_samples_per_channel_ == 0) || (current_samples_per_channel_ < total_samples_per_channel_);
        if (expected_more) {
            std::string message = "StreamAudioDecoder produced no PCM data before expected EOF: " + path_ +
                                  " current_samples/ch=" + std::to_string(current_samples_per_channel_) +
                                  " total_samples/ch=" + std::to_string(total_samples_per_channel_) +
                                  " feof=" + std::to_string(feof(pipe_) != 0) +
                                  " ferror=" + std::to_string(ferror(pipe_) != 0);
            const std::string stderr_text = read_text_file_limited(stderr_path_);
            if (!stderr_text.empty()) {
                message += "\nffmpeg stderr:\n" + stderr_text;
            }
            Logger::instance().error(message);
        }
        zero_read_logged_ = true;
    }
    for (std::size_t i = 0; i < got; ++i) {
        const unsigned char* src = raw_buffer_.data() + i * bps;
        std::int32_t value = 0;
        if (bps == 2) {
            value = static_cast<std::int16_t>(static_cast<std::uint16_t>(src[0]) | (static_cast<std::uint16_t>(src[1]) << 8));
        } else if (bps == 3) {
            std::uint32_t u = static_cast<std::uint32_t>(src[0]) |
                              (static_cast<std::uint32_t>(src[1]) << 8) |
                              (static_cast<std::uint32_t>(src[2]) << 16);
            if ((u & 0x00800000u) != 0) {
                u |= 0xFF000000u;
            }
            value = static_cast<std::int32_t>(u);
        } else {
            std::uint32_t u = static_cast<std::uint32_t>(src[0]) |
                              (static_cast<std::uint32_t>(src[1]) << 8) |
                              (static_cast<std::uint32_t>(src[2]) << 16) |
                              (static_cast<std::uint32_t>(src[3]) << 24);
            value = static_cast<std::int32_t>(u);
        }
        destination[i] = value;
    }
    current_samples_per_channel_ += got / std::max<std::uint16_t>(1, format_.channels);
    if (got < max_samples && feof(pipe_) != 0) {
        reached_eof_ = true;
        if (got > 0) {
            const std::string stderr_text = read_text_file_limited(stderr_path_);
            if (!stderr_text.empty()) {
                Logger::instance().error("ffmpeg stderr at decoder EOF for " + path_ + ":\n" + stderr_text);
            }
        }
    }
    return got;
}

} // namespace pcmtp
