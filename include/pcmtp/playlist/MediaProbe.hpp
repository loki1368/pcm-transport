#pragma once

#include <cstdint>
#include <string>

#include "pcmtp/decoder/ExternalAudioDecoder.hpp"
#include "pcmtp/util/ManagedSubprocess.hpp"

namespace pcmtp {

struct MediaProbeResult {
    bool success = false;
    std::string error;
    AudioFormat format{};
    std::uint64_t total_samples_per_channel = 0;
    GenericTags tags{};
    std::string codec_name;
    bool native_decode = false;
    bool lossless = false;
    bool dsd_source = false;
    std::uint32_t dsd_sample_rate = 0;
};

MediaProbeResult probe_media_file(const std::string& path,
                                  ManagedSubprocess* probe_process = nullptr);

} // namespace pcmtp
