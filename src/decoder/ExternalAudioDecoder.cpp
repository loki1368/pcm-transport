#include "pcmtp/decoder/ExternalAudioDecoder.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <vector>

#include "pcmtp/util/Logger.hpp"

namespace pcmtp {
namespace {

std::string shell_escape_for_command(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back(static_cast<char>(39));
    for (char ch : value) {
        if (ch == static_cast<char>(39)) {
            out += "'\\''";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back(static_cast<char>(39));
    return out;
}

std::string make_temp_stderr_path(const char* prefix) {
    std::string pattern = std::string("/tmp/") + prefix + "_XXXXXX";
    std::vector<char> path(pattern.begin(), pattern.end());
    path.push_back('\0');
    int fd = mkstemp(path.data());
    if (fd < 0) {
        Logger::instance().debug(std::string("Cannot create temporary stderr log: ") + std::strerror(errno));
        return std::string();
    }
    close(fd);
    return std::string(path.data());
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

void remove_file_quiet(const std::string& path) {
    if (!path.empty()) {
        std::remove(path.c_str());
    }
}

struct CommandCaptureResult {
    std::string stdout_text;
    std::string stderr_text;
    int status = 0;
};

CommandCaptureResult run_command_capture(const std::string& command) {
    CommandCaptureResult result;
    const std::string stderr_path = make_temp_stderr_path("pcm_transport_probe");
    std::string full_command = command;
    if (!stderr_path.empty()) {
        full_command += " 2>" + shell_escape_for_command(stderr_path);
    } else {
        full_command += " 2>/dev/null";
    }

    FILE* pipe = popen(full_command.c_str(), "r");
    if (pipe == nullptr) {
        result.status = -1;
        remove_file_quiet(stderr_path);
        return result;
    }
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result.stdout_text += buffer;
    }
    result.status = pclose(pipe);
    result.stderr_text = read_text_file_limited(stderr_path);
    remove_file_quiet(stderr_path);
    return result;
}

bool starts_with(const std::string& text, const char* prefix) {
    const std::size_t length = std::char_traits<char>::length(prefix);
    return text.size() >= length && text.compare(0, length, prefix) == 0;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim_value_copy(const std::string& value) {
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

double parse_time_base_seconds(const std::string& value) {
    const std::size_t slash = value.find('/');
    if (slash == std::string::npos) {
        return 0.0;
    }
    try {
        const double num = std::stod(value.substr(0, slash));
        const double den = std::stod(value.substr(slash + 1));
        if (num > 0.0 && den > 0.0) {
            return num / den;
        }
    } catch (...) {}
    return 0.0;
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
    if (codec == "aiff" || codec == "aifc" || codec == "dsd_lsbf" || codec == "dsd_msbf" || codec == "dsd_lsbf_planar" || codec == "dsd_msbf_planar") {
        return true;
    }
    if (ext == ".wav" || ext == ".wave" || ext == ".bwf" || ext == ".aiff" || ext == ".aif" || ext == ".ape" || ext == ".wv" || ext == ".flac" || ext == ".tta" || ext == ".tak" || ext == ".dsf") {
        return true;
    }
    return false;
}

std::string format_seconds(double seconds) {
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss << std::setprecision(9) << seconds;
    return ss.str();
}

std::uint16_t read_le16(const unsigned char* p) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t read_le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

bool read_exact(std::ifstream& input, unsigned char* data, std::size_t size) {
    input.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(input.gcount()) == size;
}


std::uint64_t read_id3v2_size(const unsigned char* h) {
    return (static_cast<std::uint64_t>(h[6] & 0x7Fu) << 21) |
           (static_cast<std::uint64_t>(h[7] & 0x7Fu) << 14) |
           (static_cast<std::uint64_t>(h[8] & 0x7Fu) << 7) |
           static_cast<std::uint64_t>(h[9] & 0x7Fu);
}

bool probe_adts_aac_fast(const std::string& path, ExternalAudioInfo& info) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        return false;
    }

    unsigned char first[10]{};
    input.read(reinterpret_cast<char*>(first), sizeof(first));
    const std::size_t got_first = static_cast<std::size_t>(input.gcount());
    if (got_first >= 10 && std::memcmp(first, "ID3", 3) == 0) {
        const std::uint64_t tag_size = read_id3v2_size(first);
        const bool footer = (first[5] & 0x10u) != 0;
        input.clear();
        input.seekg(static_cast<std::streamoff>(10 + tag_size + (footer ? 10 : 0)), std::ios::beg);
    } else {
        input.clear();
        input.seekg(0, std::ios::beg);
    }

    static const std::uint32_t kSampleRates[16] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 8000, 7350, 0, 0, 0
    };

    std::uint64_t total_frames = 0;
    std::uint64_t total_samples = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    while (input) {
        unsigned char h[7]{};
        const std::streampos frame_pos = input.tellg();
        input.read(reinterpret_cast<char*>(h), sizeof(h));
        if (static_cast<std::size_t>(input.gcount()) != sizeof(h)) {
            break;
        }
        if (h[0] != 0xFFu || (h[1] & 0xF0u) != 0xF0u) {
            if (total_frames == 0) {
                return false;
            }
            break;
        }
        const unsigned int sf_index = (h[2] >> 2) & 0x0Fu;
        if (sf_index >= 16 || kSampleRates[sf_index] == 0) {
            return false;
        }
        const std::uint16_t channel_config = static_cast<std::uint16_t>(((h[2] & 0x01u) << 2) | ((h[3] & 0xC0u) >> 6));
        const std::uint32_t frame_length = (static_cast<std::uint32_t>(h[3] & 0x03u) << 11) |
                                           (static_cast<std::uint32_t>(h[4]) << 3) |
                                           ((static_cast<std::uint32_t>(h[5] & 0xE0u)) >> 5);
        if (frame_length < 7) {
            return false;
        }
        if (sample_rate == 0) {
            sample_rate = kSampleRates[sf_index];
            channels = channel_config == 0 ? 2 : channel_config;
        }
        const std::uint32_t raw_blocks = static_cast<std::uint32_t>(h[6] & 0x03u) + 1u;
        total_samples += static_cast<std::uint64_t>(raw_blocks) * 1024u;
        ++total_frames;
        input.clear();
        input.seekg(frame_pos + static_cast<std::streamoff>(frame_length), std::ios::beg);
    }

    if (total_frames == 0 || sample_rate == 0) {
        return false;
    }
    info.format.sample_rate = sample_rate;
    info.format.channels = channels == 0 ? 2 : channels;
    info.format.bits_per_sample = 16;
    info.total_samples_per_channel = total_samples;
    info.codec_name = "aac";
    info.lossless = false;
    info.raw_aac = true;
    info.duration_reliable = true;
    Logger::instance().debug("ExternalAudioDecoder fast ADTS AAC probe: " + path + " frames=" + std::to_string(total_frames));
    return true;
}

bool probe_aac_frame_count_ffprobe(const std::string& path, ExternalAudioInfo& info) {
    const std::string cmd =
        "ffprobe -v error -count_frames -select_streams a:0 "
        "-show_entries stream=nb_read_frames,sample_rate,channels "
        "-of default=nokey=0:noprint_wrappers=1 " + shell_escape_for_command(path);
    const CommandCaptureResult result = run_command_capture(cmd);
    if (result.status != 0 || result.stdout_text.empty()) {
        return false;
    }
    std::istringstream ps(result.stdout_text);
    std::string line;
    std::uint64_t frames = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    while (std::getline(ps, line)) {
        line = trim_value_copy(line);
        const std::size_t pos = line.find('=');
        if (pos == std::string::npos) continue;
        const std::string key = line.substr(0, pos);
        const std::string value = line.substr(pos + 1);
        try {
            if (key == "nb_read_frames" && !value.empty() && value != "N/A") {
                frames = static_cast<std::uint64_t>(std::stoull(value));
            } else if (key == "sample_rate" && !value.empty() && value != "N/A") {
                sample_rate = static_cast<std::uint32_t>(std::stoul(value));
            } else if (key == "channels" && !value.empty() && value != "N/A") {
                channels = static_cast<std::uint16_t>(std::stoul(value));
            }
        } catch (...) {}
    }
    if (frames == 0 || sample_rate == 0) {
        return false;
    }
    info.format.sample_rate = sample_rate;
    info.format.channels = channels == 0 ? 2 : channels;
    info.format.bits_per_sample = 16;
    info.total_samples_per_channel = frames * 1024u;
    info.codec_name = "aac";
    info.lossless = false;
    info.raw_aac = true;
    info.duration_reliable = true;
    Logger::instance().debug("ExternalAudioDecoder ffprobe AAC frame-count probe: " + path + " frames=" + std::to_string(frames));
    return true;
}


bool probe_m4a_packet_duration_ffprobe(const std::string& path, ExternalAudioInfo& info) {
    if (info.format.sample_rate == 0) {
        return false;
    }
    const std::string cmd =
        "ffprobe -v error -select_streams a:0 "
        "-show_packets -show_entries packet=duration_time,duration "
        "-of default=nokey=0:noprint_wrappers=1 " + shell_escape_for_command(path);
    const CommandCaptureResult result = run_command_capture(cmd);
    if (result.status != 0 || result.stdout_text.empty()) {
        return false;
    }

    std::istringstream ps(result.stdout_text);
    std::string line;
    double seconds_sum = 0.0;
    std::uint64_t duration_units_sum = 0;
    while (std::getline(ps, line)) {
        line = trim_value_copy(line);
        const std::size_t pos = line.find('=');
        if (pos == std::string::npos) continue;
        const std::string key = line.substr(0, pos);
        const std::string value = line.substr(pos + 1);
        if (value.empty() || value == "N/A") {
            continue;
        }
        try {
            if (key == "duration_time") {
                const double v = std::stod(value);
                if (v > 0.0 && std::isfinite(v)) {
                    seconds_sum += v;
                }
            } else if (key == "duration") {
                const std::uint64_t v = static_cast<std::uint64_t>(std::stoull(value));
                duration_units_sum += v;
            }
        } catch (...) {}
    }

    double total_seconds = seconds_sum;
    if (total_seconds <= 0.0 && duration_units_sum > 0 && !info.time_base.empty()) {
        const double tb = parse_time_base_seconds(info.time_base);
        if (tb > 0.0) {
            total_seconds = static_cast<double>(duration_units_sum) * tb;
        }
    }
    if (total_seconds <= 0.0 || !std::isfinite(total_seconds)) {
        return false;
    }

    info.total_samples_per_channel = static_cast<std::uint64_t>(std::llround(total_seconds * static_cast<double>(info.format.sample_rate)));
    info.duration_reliable = info.total_samples_per_channel > 0;
    if (info.total_samples_per_channel == 0) {
        return false;
    }
    Logger::instance().debug("ExternalAudioDecoder packet-duration probe: " + path +
                             " samples/ch=" + std::to_string(info.total_samples_per_channel));
    return true;
}

bool probe_wav_header_fast(const std::string& path, ExternalAudioInfo& info) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        return false;
    }

    unsigned char riff[12]{};
    if (!read_exact(input, riff, sizeof(riff))) {
        return false;
    }
    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool have_fmt = false;
    bool have_data = false;
    std::uint16_t audio_format = 0;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t byte_rate = 0;
    std::uint16_t block_align = 0;
    std::uint16_t bits_per_sample = 0;
    std::uint64_t data_bytes = 0;

    while (input) {
        unsigned char header[8]{};
        if (!read_exact(input, header, sizeof(header))) {
            break;
        }
        const std::string chunk_id(reinterpret_cast<const char*>(header), 4);
        const std::uint32_t chunk_size = read_le32(header + 4);
        const std::streampos chunk_data_pos = input.tellg();
        if (chunk_id == "fmt ") {
            if (chunk_size < 16) {
                return false;
            }
            std::vector<unsigned char> fmt(chunk_size);
            if (!read_exact(input, fmt.data(), fmt.size())) {
                return false;
            }
            audio_format = read_le16(fmt.data());
            channels = read_le16(fmt.data() + 2);
            sample_rate = read_le32(fmt.data() + 4);
            byte_rate = read_le32(fmt.data() + 8);
            block_align = read_le16(fmt.data() + 12);
            bits_per_sample = read_le16(fmt.data() + 14);
            (void)byte_rate;
            have_fmt = true;
        } else if (chunk_id == "data") {
            data_bytes = chunk_size;
            have_data = true;
            input.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
        } else {
            input.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
        }
        if (chunk_size % 2 != 0) {
            input.seekg(1, std::ios::cur);
        }
        if (!input && !input.eof()) {
            return false;
        }
        if (have_fmt && have_data) {
            break;
        }
        (void)chunk_data_pos;
    }

    if (!have_fmt || !have_data || channels == 0 || sample_rate == 0 || block_align == 0 || bits_per_sample == 0) {
        return false;
    }
    if (!(audio_format == 1 || audio_format == 3 || audio_format == 0xFFFE)) {
        return false;
    }
    if (!(bits_per_sample == 16 || bits_per_sample == 24 || bits_per_sample == 32)) {
        return false;
    }

    info.format.sample_rate = sample_rate;
    info.format.channels = channels;
    info.format.bits_per_sample = bits_per_sample;
    info.total_samples_per_channel = data_bytes / block_align;
    if (audio_format == 3) {
        info.codec_name = "pcm_f" + std::to_string(bits_per_sample) + "le";
    } else {
        info.codec_name = "pcm_s" + std::to_string(bits_per_sample) + "le";
    }
    info.lossless = true;
    Logger::instance().debug("ExternalAudioDecoder fast WAV probe: " + path);
    return true;
}

} // namespace

ExternalAudioDecoder::ExternalAudioDecoder(std::uint32_t forced_output_sample_rate, std::uint16_t forced_output_bits_per_sample, const std::string& resample_quality, const std::string& bitdepth_quality)
    : forced_output_sample_rate_(forced_output_sample_rate),
      forced_output_bits_per_sample_(forced_output_bits_per_sample),
      resample_quality_(resample_quality),
      bitdepth_quality_(bitdepth_quality) {}

ExternalAudioDecoder::~ExternalAudioDecoder() {
    close_pipe(false, std::string());
}

std::string ExternalAudioDecoder::to_lower_extension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return std::string();
    }
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

bool ExternalAudioDecoder::looks_supported(const std::string& path) {
    static const std::array<const char*, 29> exts = {{".mp3", ".m4a", ".aac", ".ogg", ".opus", ".wav", ".wave", ".bwf", ".au", ".snd", ".caf", ".ape", ".wv", ".flac", ".aiff", ".aif", ".tak", ".tta", ".wma", ".asf", ".xwma", ".oma", ".aa3", ".at3", ".mpc", ".mp+", ".mpp", ".dsf", ".oga"}};
    const std::string ext = to_lower_extension(path);
    for (const char* item : exts) {
        if (ext == item) {
            return true;
        }
    }
    return false;
}

std::string ExternalAudioDecoder::shell_escape(const std::string& value) {
    return shell_escape_for_command(value);
}

std::string ExternalAudioDecoder::trim_copy(const std::string& value) {
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

std::size_t ExternalAudioDecoder::bytes_per_sample() const {
    if (format_.bits_per_sample <= 16) return 2;
    if (format_.bits_per_sample <= 24) return 3;
    return 4;
}

ExternalAudioInfo ExternalAudioDecoder::probe_metadata(const std::string& path, std::uint32_t forced_output_sample_rate, std::uint16_t forced_output_bits_per_sample) {
    if (!looks_supported(path)) {
        throw std::runtime_error("ExternalAudioDecoder does not support this file type");
    }

    ExternalAudioInfo info;
    info.format.sample_rate = 44100;
    info.format.channels = 2;
    info.format.bits_per_sample = 16;

    const std::string ext = to_lower_extension(path);
    const bool can_use_fast_wav = (ext == ".wav") || (ext == ".wave") || (ext == ".bwf");
    bool have_info = false;
    if (can_use_fast_wav) {
        have_info = probe_wav_header_fast(path, info);
    }

    if (!have_info) {
        const std::string probe_cmd =
            "ffprobe -v error -select_streams a:0 "
            "-show_entries stream=codec_name,sample_fmt,sample_rate,channels,bits_per_sample,bits_per_raw_sample,bit_rate,duration,duration_ts,time_base:format=duration:format_tags=title,artist,track "
            "-of default=nokey=0:noprint_wrappers=1 " +
            shell_escape_for_command(path);
        Logger::instance().debug("ExternalAudioDecoder unified probe: " + path);
        const CommandCaptureResult probe = run_command_capture(probe_cmd);
        if (probe.status != 0) {
            Logger::instance().error("ffprobe failed for: " + path + (probe.stderr_text.empty() ? std::string() : ("\nffprobe stderr:\n" + probe.stderr_text)));
        } else if (!probe.stderr_text.empty()) {
            Logger::instance().debug("ffprobe stderr for " + path + ":\n" + probe.stderr_text);
        }
        if (probe.stdout_text.empty()) {
            Logger::instance().debug("ExternalAudioDecoder probe returned no data for: " + path);
        }

        std::istringstream ps(probe.stdout_text);
        std::string line;
        std::string sample_fmt;
        double seconds = 0.0;
        while (std::getline(ps, line)) {
            line = trim_copy(line);
            const std::size_t pos = line.find('=');
            if (pos == std::string::npos) continue;
            const std::string key = line.substr(0, pos);
            const std::string value = line.substr(pos + 1);
            try {
                if (key == "codec_name") {
                    info.codec_name = lower_copy(value);
                } else if (key == "sample_fmt") {
                    sample_fmt = lower_copy(value);
                } else if (key == "sample_rate") {
                    info.format.sample_rate = static_cast<std::uint32_t>(std::stoul(value));
                } else if (key == "channels") {
                    info.format.channels = static_cast<std::uint16_t>(std::stoul(value));
                } else if ((key == "bits_per_sample" || key == "bits_per_raw_sample") && !value.empty() && value != "N/A") {
                    const std::uint16_t bits = static_cast<std::uint16_t>(std::stoul(value));
                    if (bits == 16 || bits == 24 || bits == 32) {
                        info.format.bits_per_sample = bits;
                    }
                } else if (key == "bit_rate" && !value.empty() && value != "N/A") {
                    info.bit_rate = static_cast<std::uint32_t>(std::stoul(value));
                } else if (key == "duration_ts" && !value.empty() && value != "N/A") {
                    info.duration_ts = std::stoll(value);
                } else if (key == "time_base" && !value.empty() && value != "N/A") {
                    info.time_base = value;
                } else if (key == "duration") {
                    const double probed = std::stod(value);
                    if (probed > seconds) {
                        seconds = probed;
                    }
                } else if (key == "TAG:title" || key == "title") {
                    info.tags.title = value;
                } else if (key == "TAG:artist" || key == "artist") {
                    info.tags.artist = value;
                } else if (key == "TAG:track" || key == "track") {
                    info.tags.track_number = std::stoi(value);
                }
            } catch (...) {}
        }
        const std::uint16_t fmt_bits = bits_from_sample_fmt(sample_fmt);
        if ((info.format.bits_per_sample == 0 || info.format.bits_per_sample == 16) && fmt_bits > 0) {
            if (!(info.format.bits_per_sample == 16 && fmt_bits == 32 && info.codec_name != "alac")) {
                info.format.bits_per_sample = fmt_bits;
            }
        }
        const double time_base_seconds = parse_time_base_seconds(info.time_base);
        if (info.duration_ts > 0 && time_base_seconds > 0.0 && info.format.sample_rate > 0) {
            info.total_samples_per_channel = static_cast<std::uint64_t>(std::llround(static_cast<double>(info.duration_ts) * time_base_seconds * static_cast<double>(info.format.sample_rate)));
        } else if (seconds > 0.0 && info.format.sample_rate > 0) {
            info.total_samples_per_channel = static_cast<std::uint64_t>(std::llround(seconds * static_cast<double>(info.format.sample_rate)));
        }
    }

    if (ext == ".m4a" && info.codec_name == "alac" && info.total_samples_per_channel == 0) {
        ExternalAudioInfo m4a_info = info;
        if (probe_m4a_packet_duration_ffprobe(path, m4a_info)) {
            m4a_info.tags = info.tags;
            info = m4a_info;
        } else {
            info.duration_reliable = false;
            Logger::instance().debug("ExternalAudioDecoder ALAC/M4A duration packet probe fallback failed: " + path);
        }
    }

    if (ext == ".aac") {
        ExternalAudioInfo aac_info = info;
        if (probe_adts_aac_fast(path, aac_info) || probe_aac_frame_count_ffprobe(path, aac_info)) {
            aac_info.tags = info.tags;
            info = aac_info;
        } else {
            info.raw_aac = true;
            info.duration_reliable = info.total_samples_per_channel > 0;
            Logger::instance().debug("ExternalAudioDecoder raw AAC duration probe fallback: " + path);
        }
    }

    if (info.format.channels == 0) info.format.channels = 2;
    if (info.format.bits_per_sample != 16 && info.format.bits_per_sample != 24 && info.format.bits_per_sample != 32) {
        info.format.bits_per_sample = 16;
    }
    info.lossless = codec_is_lossless(info.codec_name, ext);
    info.source_format = info.format;
    info.source_total_samples_per_channel = info.total_samples_per_channel;
    const std::uint32_t source_rate = info.format.sample_rate;
    const std::uint64_t source_total = info.total_samples_per_channel;
    if (forced_output_sample_rate > 0 && source_rate > 0 && source_total > 0 && forced_output_sample_rate != source_rate) {
        info.total_samples_per_channel = static_cast<std::uint64_t>(std::llround(static_cast<double>(source_total) * static_cast<double>(forced_output_sample_rate) / static_cast<double>(source_rate)));
    }
    if (forced_output_sample_rate > 0) {
        info.format.sample_rate = forced_output_sample_rate;
    }
    if (forced_output_bits_per_sample == 16 || forced_output_bits_per_sample == 24 || forced_output_bits_per_sample == 32) {
        info.format.bits_per_sample = forced_output_bits_per_sample;
    }
    return info;
}

ExternalAudioInfo ExternalAudioDecoder::probe_info(const std::string& path, std::uint32_t forced_output_sample_rate, std::uint16_t forced_output_bits_per_sample) {
    ExternalAudioInfo info = probe_metadata(path, forced_output_sample_rate, forced_output_bits_per_sample);
    info.tags = GenericTags{};
    return info;
}

GenericTags ExternalAudioDecoder::read_tags(const std::string& path) {
    return probe_metadata(path).tags;
}

std::string ExternalAudioDecoder::decode_command(double seconds) const {
    const bool have_seek = seconds > 0.0;
    const AudioFormat source_format = source_format_.sample_rate > 0 ? source_format_ : format_;
    const std::uint32_t out_rate = forced_output_sample_rate_ > 0 ? forced_output_sample_rate_ : format_.sample_rate;
    const std::uint16_t out_bits = forced_output_bits_per_sample_ > 0 ? forced_output_bits_per_sample_ : format_.bits_per_sample;
    const std::string codec = out_bits <= 16 ? "pcm_s16le"
                             : (out_bits <= 24 ? "pcm_s24le" : "pcm_s32le");
    const std::string raw = out_bits <= 16 ? "s16le"
                           : (out_bits <= 24 ? "s24le" : "s32le");
    const bool forced_rate_active = forced_output_sample_rate_ > 0 && forced_output_sample_rate_ != source_format.sample_rate;
    const bool forced_bits_active = forced_output_bits_per_sample_ > 0 && forced_output_bits_per_sample_ != source_format.bits_per_sample;
    const bool need_filter = forced_rate_active || forced_bits_active;
    const std::string ext = to_lower_extension(path_);
    const bool is_ape = ext == ".ape";
    const bool is_raw_aac = ext == ".aac";
    const bool is_alac_m4a = ext == ".m4a" && codec_name_ == "alac";
    constexpr double kApeHybridPrerollSeconds = 3.0;
    constexpr double kAacHybridPrerollSeconds = 1.5;
    constexpr double kAlacHybridPrerollSeconds = 1.0;
    double input_seek = 0.0;
    double output_seek = 0.0;
    if (have_seek) {
        if (is_ape && seconds > kApeHybridPrerollSeconds) {
            input_seek = seconds - kApeHybridPrerollSeconds;
            output_seek = kApeHybridPrerollSeconds;
        } else if (is_ape) {
            output_seek = seconds;
        } else if (is_raw_aac && seconds > kAacHybridPrerollSeconds) {
            input_seek = seconds - kAacHybridPrerollSeconds;
            output_seek = kAacHybridPrerollSeconds;
        } else if (is_raw_aac) {
            output_seek = seconds;
        } else if (is_alac_m4a && seconds > kAlacHybridPrerollSeconds) {
            input_seek = seconds - kAlacHybridPrerollSeconds;
            output_seek = kAlacHybridPrerollSeconds;
        } else if (is_alac_m4a) {
            output_seek = seconds;
        } else {
            input_seek = seconds;
        }
    }

    std::string cmd = "ffmpeg -v error -nostdin ";
    if (is_raw_aac) {
        cmd += "-fflags +genpts ";
    }
    if (input_seek > 0.0) {
        cmd += "-ss " + format_seconds(input_seek) + " ";
    }
    cmd += "-i " + shell_escape(path_) + " ";
    cmd += "-map 0:a:0 -vn -sn -dn ";
    if (output_seek > 0.0) {
        cmd += "-ss " + format_seconds(output_seek) + " ";
    }
    if (need_filter) {
        int precision = 33;
        if (resample_quality_ == "high") precision = 28;
        else if (resample_quality_ == "balanced") precision = 20;
        else if (resample_quality_ == "fast") precision = 16;
        std::string af = "aresample=resampler=soxr:precision=" + std::to_string(precision) + ":cheby=0:osr=" + std::to_string(out_rate);
        if (out_bits <= 16) {
            std::string method = "triangular_hp";
            if (bitdepth_quality_ == "tpdf") method = "triangular";
            else if (bitdepth_quality_ == "rectangular") method = "rectangular";
            af += ":osf=s16:dither_method=" + method;
        } else if (out_bits == 24) {
            af += ":osf=s32";
        } else if (out_bits >= 32) {
            af += ":osf=s32";
        }
        cmd += "-af " + shell_escape(af) + " ";
    }
    cmd += "-f " + raw + " -acodec " + codec +
           " -ac " + std::to_string(format_.channels) + " -";
    return cmd;
}

void ExternalAudioDecoder::close_pipe(bool log_stderr, const std::string& context) {
    if (pipe_ != nullptr) {
        const int status = pclose(pipe_);
        pipe_ = nullptr;
        if (log_stderr && status != 0) {
            const std::string stderr_text = read_text_file_limited(stderr_path_);
            if (!stderr_text.empty()) {
                Logger::instance().error("ffmpeg exited with non-zero status" + (context.empty() ? std::string() : (" during " + context)) + ":\n" + stderr_text);
            } else {
                Logger::instance().error("ffmpeg exited with non-zero status" + (context.empty() ? std::string() : (" during " + context)));
            }
        }
    }
    remove_file_quiet(stderr_path_);
    stderr_path_.clear();
}

bool ExternalAudioDecoder::start_decode_pipe(double seconds, const std::string& context) {
    close_pipe(false, std::string());
    stderr_path_ = make_temp_stderr_path("pcm_transport_ffmpeg");
    std::string command = decode_command(seconds);
    if (!stderr_path_.empty()) {
        command += " 2>" + shell_escape(stderr_path_);
    } else {
        command += " 2>/dev/null";
    }
    Logger::instance().debug("ExternalAudioDecoder starting ffmpeg decode" + (context.empty() ? std::string() : (" (" + context + ")")) + " for: " + path_);
    pipe_ = popen(command.c_str(), "r");
    if (pipe_ == nullptr) {
        Logger::instance().error("Cannot start ffmpeg decoder for: " + path_);
        remove_file_quiet(stderr_path_);
        stderr_path_.clear();
        return false;
    }
    return true;
}

void ExternalAudioDecoder::set_known_info(const ExternalAudioInfo& info) {
    known_info_ = info;
    have_known_info_ = true;
}

ExternalAudioInfo ExternalAudioDecoder::effective_probe_info(const std::string& path) const {
    if (have_known_info_) {
        return known_info_;
    }
    return probe_info(path, forced_output_sample_rate_, forced_output_bits_per_sample_);
}

void ExternalAudioDecoder::open(const std::string& path) {
    open_at_sample(path, 0);
}

void ExternalAudioDecoder::open_at_sample(const std::string& path, std::uint64_t sample_index) {
    close_pipe(false, std::string());
    opened_ = false;
    path_ = path;
    reached_eof_ = false;
    zero_read_logged_ = false;
    total_samples_per_channel_ = 0;
    current_samples_per_channel_ = sample_index;

    const ExternalAudioInfo info = effective_probe_info(path);
    format_ = info.format;
    source_format_ = info.source_format.sample_rate > 0 ? info.source_format : info.format;
    codec_name_ = info.codec_name;
    total_samples_per_channel_ = info.total_samples_per_channel;

    Logger::instance().debug("ExternalAudioDecoder format: " + std::to_string(format_.sample_rate) + " Hz / " +
                             std::to_string(format_.bits_per_sample) + "-bit / " +
                             std::to_string(format_.channels) + " ch, total samples/ch=" +
                             std::to_string(total_samples_per_channel_) +
                             (sample_index > 0 ? (", start sample/ch=" + std::to_string(sample_index)) : std::string()));
    const double seconds = format_.sample_rate > 0
        ? static_cast<double>(sample_index) / static_cast<double>(format_.sample_rate)
        : 0.0;
    if (!start_decode_pipe(seconds, sample_index > 0 ? "open at sample" : "initial decode")) {
        throw std::runtime_error("Cannot start ffmpeg decoder");
    }
    opened_ = true;
}

const AudioFormat& ExternalAudioDecoder::format() const {
    return format_;
}

std::size_t ExternalAudioDecoder::read_samples(PcmSample* destination, std::size_t max_samples) {
    if (!opened_ || pipe_ == nullptr) {
        throw std::runtime_error("Decoder not opened");
    }

    const std::size_t bps = bytes_per_sample();
    raw_buffer_.resize(max_samples * bps);
    const std::size_t got_bytes = fread(raw_buffer_.data(), 1, raw_buffer_.size(), pipe_);
    const std::size_t got = got_bytes / bps;
    if (got == 0 && !zero_read_logged_) {
        const bool expected_more = (total_samples_per_channel_ == 0) || (current_samples_per_channel_ < total_samples_per_channel_);
        if (expected_more) {
            std::string message = "ExternalAudioDecoder produced no PCM data before expected EOF: " + path_ +
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

bool ExternalAudioDecoder::eof() const {
    return reached_eof_;
}

std::uint64_t ExternalAudioDecoder::total_samples_per_channel() const {
    return total_samples_per_channel_;
}

std::string ExternalAudioDecoder::source_path() const {
    return path_;
}

bool ExternalAudioDecoder::seek_to_sample(std::uint64_t sample_index) {
    if (!opened_ || path_.empty()) return false;
    const double seconds = format_.sample_rate > 0
        ? static_cast<double>(sample_index) / static_cast<double>(format_.sample_rate)
        : 0.0;
    close_pipe(false, std::string());
    reached_eof_ = false;
    zero_read_logged_ = false;
    current_samples_per_channel_ = sample_index;
    const std::string ext = to_lower_extension(path_);
    const bool is_ape = ext == ".ape";
    const bool is_raw_aac = ext == ".aac";
    const std::string seek_mode = is_ape && seconds > 3.0 ? "hybrid input/output seek for APE" :
                                  (is_ape ? "decoded-output seek for short APE offset" :
                                  (is_raw_aac ? "hybrid raw AAC seek" : "input seek"));
    Logger::instance().debug("ExternalAudioDecoder seeking/restarting ffmpeg decode at " + std::to_string(seconds) +
                             " sec (" + seek_mode + ") for: " + path_);
    return start_decode_pipe(seconds, seek_mode);
}

} // namespace pcmtp
