#include "pcmtp/backend/AlsaPcmBackend.hpp"

#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <sstream>
#include <vector>


#include "pcmtp/util/Logger.hpp"

namespace pcmtp {

namespace {

void check_alsa(int result, const char* message) {
    if (result < 0) {
        throw std::runtime_error(std::string(message) + ": " + snd_strerror(result));
    }
}

bool hw_format_supported(snd_pcm_t* handle,
                         snd_pcm_hw_params_t* hw_params,
                         snd_pcm_format_t fmt) {
    return snd_pcm_hw_params_test_format(handle, hw_params, fmt) >= 0;
}

void push_unique_format(std::vector<snd_pcm_format_t>& candidates, snd_pcm_format_t fmt) {
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i] == fmt) {
            return;
        }
    }
    candidates.push_back(fmt);
}

std::vector<snd_pcm_format_t> format_candidates_for_bits(std::uint16_t bits_per_sample,
                                                          Alsa24BitContainerPreference preference) {
    std::vector<snd_pcm_format_t> candidates;
    if (bits_per_sample <= 16) {
        candidates.push_back(SND_PCM_FORMAT_S16_LE);
    } else if (bits_per_sample <= 24) {
        switch (preference) {
            case Alsa24BitContainerPreference::PreferS24LE:
                push_unique_format(candidates, SND_PCM_FORMAT_S24_LE);
                break;
            case Alsa24BitContainerPreference::PreferS24_3LE:
                push_unique_format(candidates, SND_PCM_FORMAT_S24_3LE);
                break;
            case Alsa24BitContainerPreference::PreferS32LE:
                push_unique_format(candidates, SND_PCM_FORMAT_S32_LE);
                break;
            case Alsa24BitContainerPreference::Auto:
            default:
                break;
        }
        push_unique_format(candidates, SND_PCM_FORMAT_S24_LE);
        push_unique_format(candidates, SND_PCM_FORMAT_S24_3LE);
        push_unique_format(candidates, SND_PCM_FORMAT_S32_LE);
    } else {
        candidates.push_back(SND_PCM_FORMAT_S32_LE);
    }
    return candidates;
}

const char* format_name_or_unknown(snd_pcm_format_t fmt) {
    const char* name = snd_pcm_format_name(fmt);
    return name != nullptr ? name : "unknown";
}

std::string preference_name(Alsa24BitContainerPreference preference) {
    switch (preference) {
        case Alsa24BitContainerPreference::PreferS24LE: return "Prefer S24_LE";
        case Alsa24BitContainerPreference::PreferS24_3LE: return "Prefer S24_3LE";
        case Alsa24BitContainerPreference::PreferS32LE: return "Prefer S32_LE";
        case Alsa24BitContainerPreference::Auto:
        default:
            return "Auto";
    }
}

void convert_to_s16_scalar(const PcmSample* samples, std::size_t count, std::uint16_t bits_per_sample, std::int16_t* out) {
    for (std::size_t i = 0; i < count; ++i) {
        std::int64_t v = static_cast<std::int64_t>(samples[i]);
        if (bits_per_sample > 16) {
            v >>= (bits_per_sample - 16);
        }
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out[i] = static_cast<std::int16_t>(v);
    }
}

} // namespace

AlsaPcmBackend::~AlsaPcmBackend() {
    close();
}

void AlsaPcmBackend::open(const std::string& device_name, const AudioFormat& format) {
    close();
    format_ = format;
    pcm_container_format_ = SND_PCM_FORMAT_UNKNOWN;
    device_name_ = device_name;
    accepted_sample_rate_ = 0;
    accepted_channels_ = 0;

    Logger::instance().info("Opening ALSA device: " + device_name + " format=" + format.to_string());
    check_alsa(snd_pcm_open(&handle_, device_name.c_str(), SND_PCM_STREAM_PLAYBACK, 0),
               "snd_pcm_open failed");

    snd_pcm_hw_params_t* hw_params = nullptr;
    snd_pcm_hw_params_alloca(&hw_params);

    check_alsa(snd_pcm_hw_params_any(handle_, hw_params), "snd_pcm_hw_params_any failed");
    Logger::instance().debug("ALSA hw_params_any ok");
    check_alsa(snd_pcm_hw_params_set_access(handle_, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED),
               "snd_pcm_hw_params_set_access failed");
    Logger::instance().debug("ALSA access set: RW_INTERLEAVED");
    check_alsa(snd_pcm_hw_params_set_channels(handle_, hw_params, format.channels),
               "snd_pcm_hw_params_set_channels failed");
    Logger::instance().debug("ALSA channels requested: " + std::to_string(format.channels));

    unsigned sample_rate = format.sample_rate;
    check_alsa(snd_pcm_hw_params_set_rate_near(handle_, hw_params, &sample_rate, nullptr),
               "snd_pcm_hw_params_set_rate_near failed");
    Logger::instance().debug("ALSA rate requested=" + std::to_string(format.sample_rate) + " accepted_near=" + std::to_string(sample_rate));
    if (sample_rate != format.sample_rate) {
        throw std::runtime_error("ALSA device does not accept requested sample rate exactly");
    }

    snd_pcm_uframes_t requested_period = 588;
    snd_pcm_uframes_t requested_buffer = 2352;
    check_alsa(snd_pcm_hw_params_set_period_size_near(handle_, hw_params, &requested_period, nullptr),
               "snd_pcm_hw_params_set_period_size_near failed");
    check_alsa(snd_pcm_hw_params_set_buffer_size_near(handle_, hw_params, &requested_buffer),
               "snd_pcm_hw_params_set_buffer_size_near failed");
    Logger::instance().debug("ALSA period requested_near=" + std::to_string(static_cast<unsigned long long>(requested_period)) +
                             " buffer requested_near=" + std::to_string(static_cast<unsigned long long>(requested_buffer)));

    Logger::instance().debug(std::string("ALSA test_format S16_LE => ") + (hw_format_supported(handle_, hw_params, SND_PCM_FORMAT_S16_LE) ? "supported" : "unsupported"));
    const std::vector<snd_pcm_format_t> candidates = format_candidates_for_bits(format.bits_per_sample, format_24bit_preference_);
    Logger::instance().debug("ALSA 24-bit container preference: " + preference_name(format_24bit_preference_));
    bool opened = false;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        snd_pcm_hw_params_t* trial = nullptr;
        snd_pcm_hw_params_alloca(&trial);
        snd_pcm_hw_params_copy(trial, hw_params);
        const char* candidate_name = format_name_or_unknown(candidates[i]);
        const bool supported = hw_format_supported(handle_, trial, candidates[i]);
        Logger::instance().debug(std::string("ALSA test_format ") + candidate_name + (supported ? " => supported" : " => unsupported"));
        if (!supported) {
            continue;
        }
        const int set_format_result = snd_pcm_hw_params_set_format(handle_, trial, candidates[i]);
        Logger::instance().debug(std::string("ALSA set_format ") + candidate_name + " => " + std::to_string(set_format_result));
        if (set_format_result < 0) {
            continue;
        }
        const int commit_result = snd_pcm_hw_params(handle_, trial);
        Logger::instance().debug(std::string("ALSA hw_params commit ") + candidate_name + " => " + std::to_string(commit_result));
        if (commit_result < 0) {
            continue;
        }
        opened = true;
        pcm_container_format_ = candidates[i];
        check_alsa(snd_pcm_hw_params_current(handle_, hw_params), "snd_pcm_hw_params_current failed");
        snd_pcm_hw_params_get_period_size(hw_params, &period_frames_, nullptr);
        snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_frames_);
        snd_pcm_format_t accepted = SND_PCM_FORMAT_UNKNOWN;
        if (snd_pcm_hw_params_get_format(hw_params, &accepted) >= 0) {
            pcm_container_format_ = accepted;
        }
        Logger::instance().info(std::string("ALSA format negotiation: requested bits=") + std::to_string(format.bits_per_sample) +
                                " container=" + candidate_name +
                                " accepted=" + format_name_or_unknown(pcm_container_format_));
        break;
    }

    if (!opened) {
        const bool s16_supported = hw_format_supported(handle_, hw_params, SND_PCM_FORMAT_S16_LE);
        if (format.bits_per_sample <= 16) {
            throw std::runtime_error("ALSA device does not accept requested 16-bit PCM format");
        }
        if (format.bits_per_sample <= 24) {
            if (s16_supported) {
                throw std::runtime_error("ALSA device does not accept requested 24-bit PCM format (S16_LE is supported in current hw mode)");
            }
            throw std::runtime_error("ALSA device does not accept requested 24-bit PCM format");
        }
        throw std::runtime_error("ALSA device does not accept requested 32-bit PCM format");
    }

    accepted_sample_rate_ = sample_rate;
    accepted_channels_ = format.channels;
    Logger::instance().debug("ALSA PCM container selected: " + std::string(format_name_or_unknown(pcm_container_format_)));

    snd_pcm_sw_params_t* sw_params = nullptr;
    snd_pcm_sw_params_alloca(&sw_params);
    check_alsa(snd_pcm_sw_params_current(handle_, sw_params), "snd_pcm_sw_params_current failed");
    check_alsa(snd_pcm_sw_params_set_start_threshold(handle_, sw_params, period_frames_),
               "snd_pcm_sw_params_set_start_threshold failed");
    check_alsa(snd_pcm_sw_params_set_avail_min(handle_, sw_params, period_frames_),
               "snd_pcm_sw_params_set_avail_min failed");
    check_alsa(snd_pcm_sw_params(handle_, sw_params), "snd_pcm_sw_params failed");

    check_alsa(snd_pcm_prepare(handle_), "snd_pcm_prepare failed");
    Logger::instance().debug("ALSA opened period=" + std::to_string(static_cast<unsigned long long>(period_frames_)) +
                             " buffer=" + std::to_string(static_cast<unsigned long long>(buffer_frames_)) +
                             " start_threshold=" + std::to_string(static_cast<unsigned long long>(period_frames_)) +
                             " avail_min=" + std::to_string(static_cast<unsigned long long>(period_frames_)) +
                             " model=ALSA PCM ring buffer");
}

std::size_t AlsaPcmBackend::write_samples(const PcmSample* samples, std::size_t sample_count) {
    if (handle_ == nullptr) {
        throw std::runtime_error("ALSA backend not opened");
    }

    std::size_t written_samples = 0;
    const std::size_t channels = format_.channels;
    std::vector<std::int32_t> temp32;
    std::vector<std::int16_t> temp16;
    std::vector<unsigned char> temp24;

    while (written_samples < sample_count) {
        const snd_pcm_uframes_t frames_to_write =
            static_cast<snd_pcm_uframes_t>((sample_count - written_samples) / channels);
        if (frames_to_write == 0) {
            break;
        }

        const void* write_ptr = nullptr;
        if (pcm_container_format_ == SND_PCM_FORMAT_S16_LE) {
            temp16.resize(static_cast<std::size_t>(frames_to_write) * channels);
            convert_to_s16_scalar(samples + written_samples, temp16.size(), format_.bits_per_sample, temp16.data());
            write_ptr = temp16.data();
        } else if (pcm_container_format_ == SND_PCM_FORMAT_S24_3LE) {
            temp24.resize(static_cast<std::size_t>(frames_to_write) * channels * 3u);
            const std::int64_t hi = 8388607;
            const std::int64_t lo = -8388608;
            for (std::size_t i = 0; i < static_cast<std::size_t>(frames_to_write) * channels; ++i) {
                std::int64_t v = static_cast<std::int64_t>(samples[written_samples + i]);
                if (format_.bits_per_sample > 24) {
                    v >>= (format_.bits_per_sample - 24);
                }
                if (v > hi) v = hi;
                if (v < lo) v = lo;
                const std::uint32_t u = static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
                temp24[i * 3u + 0u] = static_cast<unsigned char>(u & 0xFFu);
                temp24[i * 3u + 1u] = static_cast<unsigned char>((u >> 8) & 0xFFu);
                temp24[i * 3u + 2u] = static_cast<unsigned char>((u >> 16) & 0xFFu);
            }
            write_ptr = temp24.data();
        } else {
            temp32.resize(static_cast<std::size_t>(frames_to_write) * channels);
            const bool shift_to_container = (pcm_container_format_ == SND_PCM_FORMAT_S32_LE && format_.bits_per_sample <= 24);
            for (std::size_t i = 0; i < temp32.size(); ++i) {
                std::int64_t v = static_cast<std::int64_t>(samples[written_samples + i]);
                if (pcm_container_format_ == SND_PCM_FORMAT_S24_LE) {
                    if (format_.bits_per_sample > 24) {
                        v >>= (format_.bits_per_sample - 24);
                    }
                    if (v > 8388607) v = 8388607;
                    if (v < -8388608) v = -8388608;
                } else if (shift_to_container) {
                    v <<= (32 - format_.bits_per_sample);
                }
                if (v > INT32_MAX) v = INT32_MAX;
                if (v < INT32_MIN) v = INT32_MIN;
                temp32[i] = static_cast<std::int32_t>(v);
            }
            write_ptr = temp32.data();
        }

        const snd_pcm_sframes_t result = snd_pcm_writei(handle_, write_ptr, frames_to_write);
        if (result == -EPIPE) {
            Logger::instance().debug("ALSA underrun, preparing again");
            snd_pcm_prepare(handle_);
            continue;
        }
        if (result == -ESTRPIPE) {
            Logger::instance().debug("ALSA suspended stream, trying resume");
            while (snd_pcm_resume(handle_) == -EAGAIN) {
            }
            snd_pcm_prepare(handle_);
            continue;
        }
        if (result < 0) {
            throw std::runtime_error(std::string("snd_pcm_writei failed: ") +
                                     snd_strerror(static_cast<int>(result)));
        }

        written_samples += static_cast<std::size_t>(result) * channels;
    }

    return written_samples;
}

void AlsaPcmBackend::drain() {
    if (handle_ != nullptr) {
        Logger::instance().debug("Draining ALSA device");
        snd_pcm_drain(handle_);
    }
}

void AlsaPcmBackend::close() {
    if (handle_ != nullptr) {
        Logger::instance().debug("Closing ALSA device (drop + close)");
        snd_pcm_drop(handle_);
        snd_pcm_close(handle_);
        handle_ = nullptr;
        pcm_container_format_ = SND_PCM_FORMAT_UNKNOWN;
        accepted_sample_rate_ = 0;
        accepted_channels_ = 0;
    }
}

snd_pcm_uframes_t AlsaPcmBackend::period_frames() const {
    return period_frames_;
}

snd_pcm_uframes_t AlsaPcmBackend::buffer_frames() const {
    return buffer_frames_;
}

snd_pcm_format_t AlsaPcmBackend::pcm_container_format() const {
    return pcm_container_format_;
}

void AlsaPcmBackend::set_24bit_container_preference(Alsa24BitContainerPreference preference) {
    format_24bit_preference_ = preference;
}

std::string AlsaPcmBackend::active_output_report() const {
    if (pcm_container_format_ == SND_PCM_FORMAT_UNKNOWN || accepted_sample_rate_ == 0) {
        return std::string();
    }
    std::ostringstream ss;
    ss << "Device: " << (device_name_.empty() ? std::string("unknown") : device_name_) << '\n';
    ss << "Source/requested: " << format_.bits_per_sample << "-bit / "
       << format_.sample_rate << " Hz / " << static_cast<unsigned>(format_.channels) << " ch" << '\n';
    ss << "ALSA container: " << format_name_or_unknown(pcm_container_format_)
       << " (24-bit preference: " << preference_name(format_24bit_preference_) << ")" << '\n';
    ss << "ALSA rate: " << accepted_sample_rate_
       << (accepted_sample_rate_ == format_.sample_rate ? " Hz exact" : " Hz near") << '\n';
    ss << "Period / buffer: " << static_cast<unsigned long long>(period_frames_)
       << " / " << static_cast<unsigned long long>(buffer_frames_);
    return ss.str();
}


AlsaProbeMatrix AlsaPcmBackend::probe_device_format_matrix(const std::string& device_name) {
    const std::vector<unsigned> rates = {44100, 48000, 88200, 96000, 176400, 192000};
    const std::vector<snd_pcm_format_t> formats = {
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_FORMAT_S24_LE,
        SND_PCM_FORMAT_S24_3LE,
        SND_PCM_FORMAT_S32_LE
    };

    AlsaProbeMatrix matrix;
    matrix.device_name = device_name.empty() ? std::string("default") : device_name;
    matrix.sample_rates = rates;
    for (std::size_t f = 0; f < formats.size(); ++f) {
        matrix.format_names.push_back(format_name_or_unknown(formats[f]));
    }

    for (std::size_t f = 0; f < formats.size(); ++f) {
        const snd_pcm_format_t fmt = formats[f];
        for (std::size_t r = 0; r < rates.size(); ++r) {
            bool ok = false;
            snd_pcm_t* handle = nullptr;
            const int open_result = snd_pcm_open(&handle, matrix.device_name.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
            if (open_result >= 0 && handle != nullptr) {
                snd_pcm_hw_params_t* hw_params = nullptr;
                snd_pcm_hw_params_alloca(&hw_params);
                if (snd_pcm_hw_params_any(handle, hw_params) >= 0 &&
                    snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED) >= 0 &&
                    snd_pcm_hw_params_set_channels(handle, hw_params, 2) >= 0) {
                    unsigned rate = rates[r];
                    if (snd_pcm_hw_params_set_rate_near(handle, hw_params, &rate, nullptr) >= 0 &&
                        rate == rates[r] &&
                        snd_pcm_hw_params_set_format(handle, hw_params, fmt) >= 0 &&
                        snd_pcm_hw_params(handle, hw_params) >= 0) {
                        ok = true;
                    }
                }
                snd_pcm_close(handle);
            }
            AlsaProbeCell cell;
            cell.format_name = format_name_or_unknown(fmt);
            cell.sample_rate = rates[r];
            cell.supported = ok;
            matrix.cells.push_back(cell);
        }
    }

    return matrix;
}

std::string AlsaPcmBackend::probe_device_formats(const std::string& device_name) {
    const AlsaProbeMatrix matrix = probe_device_format_matrix(device_name);
    std::ostringstream out;
    out << "ALSA device probe\n";
    out << "Device: " << matrix.device_name << "\n";
    out << "Mode: playback, RW_INTERLEAVED, stereo\n\n";
    out << "Format        44.1k    48k      88.2k    96k      176.4k   192k\n";
    out << "----------------------------------------------------------------\n";

    for (std::size_t f = 0; f < matrix.format_names.size(); ++f) {
        out << matrix.format_names[f];
        for (std::size_t pad = matrix.format_names[f].size(); pad < 14; ++pad) out << ' ';
        for (std::size_t r = 0; r < matrix.sample_rates.size(); ++r) {
            const std::size_t idx = f * matrix.sample_rates.size() + r;
            const bool ok = idx < matrix.cells.size() && matrix.cells[idx].supported;
            out << (ok ? "yes" : "no ");
            if (r + 1 < matrix.sample_rates.size()) out << "      ";
        }
        out << '\n';
    }

    out << "\nNotes:\n";
    out << "- This probe tests the selected ALSA PCM device directly.\n";
    out << "- Other players may use plug/dmix or a different subdevice.\n";
    out << "- If playback is active, the device may be busy and the probe may show false negatives.\n";
    return out.str();
}

} // namespace pcmtp
