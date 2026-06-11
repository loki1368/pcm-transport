#pragma once

#include <alsa/asoundlib.h>
#include <cstdint>

#include <string>
#include <vector>

#include "pcmtp/backend/IAudioBackend.hpp"

namespace pcmtp {

enum class Alsa24BitContainerPreference {
    Auto,
    PreferS24LE,
    PreferS24_3LE,
    PreferS32LE
};

struct AlsaProbeCell {
    std::string format_name;
    unsigned sample_rate = 0;
    bool supported = false;
};

struct AlsaProbeMatrix {
    std::string device_name;
    std::vector<unsigned> sample_rates;
    std::vector<std::string> format_names;
    std::vector<AlsaProbeCell> cells;
};

class AlsaPcmBackend final : public IAudioBackend {
public:
    AlsaPcmBackend() = default;
    ~AlsaPcmBackend() override;

    void open(const std::string& device_name, const AudioFormat& format) override;
    std::size_t write_samples(const PcmSample* samples, std::size_t sample_count) override;
    void drain() override;
    void close() override;
    std::string active_output_report() const override;

    void set_24bit_container_preference(Alsa24BitContainerPreference preference);

    static std::string probe_device_formats(const std::string& device_name);
    static AlsaProbeMatrix probe_device_format_matrix(const std::string& device_name);

    snd_pcm_uframes_t period_frames() const;
    snd_pcm_uframes_t buffer_frames() const;
    snd_pcm_format_t pcm_container_format() const;

private:
    snd_pcm_t* handle_ = nullptr;
    AudioFormat format_{};
    snd_pcm_uframes_t period_frames_ = 588;
    snd_pcm_uframes_t buffer_frames_ = 2352;
    snd_pcm_format_t pcm_container_format_ = SND_PCM_FORMAT_UNKNOWN;
    Alsa24BitContainerPreference format_24bit_preference_ = Alsa24BitContainerPreference::Auto;
    std::string device_name_;
    unsigned accepted_sample_rate_ = 0;
    std::uint16_t accepted_channels_ = 0;
};

} // namespace pcmtp
