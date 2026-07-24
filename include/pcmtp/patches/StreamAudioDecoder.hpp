#pragma once

#include "pcmtp/decoder/ExternalAudioDecoder.hpp"

namespace pcmtp {

// PATCH: internet-radio — stream probe, reconnect ffmpeg flags, poll-based read.
class StreamAudioDecoder : public ExternalAudioDecoder {
public:
    using ExternalAudioDecoder::ExternalAudioDecoder;

    std::size_t read_samples(PcmSample* destination, std::size_t max_samples) override;

    static bool is_stream_uri(const std::string& path);
    static ExternalAudioInfo probe_metadata(const std::string& path,
                                            std::uint32_t forced_output_sample_rate = 0,
                                            std::uint16_t forced_output_bits_per_sample = 0,
                                            bool background_priority = false,
                                            ManagedSubprocess* probe_process = nullptr);
    static ExternalAudioInfo probe_info(const std::string& path,
                                        std::uint32_t forced_output_sample_rate = 0,
                                        std::uint16_t forced_output_bits_per_sample = 0);
    static bool verify_stream_playback(const std::string& path,
                                       const ExternalAudioInfo& probed_info,
                                       std::uint32_t forced_output_sample_rate = 0,
                                       std::uint16_t forced_output_bits_per_sample = 0);

protected:
    std::string decode_command(double seconds) const override;
    ExternalAudioInfo effective_probe_info(const std::string& path) const override;
};

} // namespace pcmtp
