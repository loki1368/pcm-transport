#pragma once

#include <cstddef>
#include <string>

#include "pcmtp/core/PcmTypes.hpp"

namespace pcmtp {

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    virtual void open(const std::string& device_name, const AudioFormat& format) = 0;
    virtual std::size_t write_samples(const PcmSample* samples, std::size_t sample_count) = 0;
    virtual void drain() = 0;
    virtual void close() = 0;
    virtual std::string active_output_report() const { return std::string(); }
};

} // namespace pcmtp
