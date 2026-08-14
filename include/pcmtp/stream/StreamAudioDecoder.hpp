#pragma once

#include <cstdint>
#include <string>

#include "pcmtp/decoder/ExternalAudioDecoder.hpp"
#include "pcmtp/util/ProbeCancellation.hpp"

namespace pcmtp {

class StreamAudioDecoder {
public:
    static bool is_stream_uri(const std::string& path);
    static ExternalAudioInfo probe_metadata(const std::string& path,
                                            std::uint32_t forced_output_sample_rate = 0,
                                            std::uint16_t forced_output_bits_per_sample = 0,
                                            ProbeCancellation* probe_cancellation = nullptr);
    static ExternalAudioInfo probe_info(const std::string& path,
                                        std::uint32_t forced_output_sample_rate = 0,
                                        std::uint16_t forced_output_bits_per_sample = 0);
    static bool verify_stream_playback(const std::string& path,
                                       const ExternalAudioInfo& probed_info,
                                       std::uint32_t forced_output_sample_rate = 0,
                                       std::uint16_t forced_output_bits_per_sample = 0);
};

} // namespace pcmtp
