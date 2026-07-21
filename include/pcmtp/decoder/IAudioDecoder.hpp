#pragma once

#include <cstddef>
#include <string>

#include "pcmtp/core/PcmTypes.hpp"

namespace pcmtp {

class IAudioDecoder {
public:
    virtual ~IAudioDecoder() = default;

    virtual void open(const std::string& path) = 0;
    virtual void open_at_sample(const std::string& path, std::uint64_t sample_index) {
        open(path);
        if (sample_index > 0) {
            seek_to_sample(sample_index);
        }
    }
    virtual const AudioFormat& format() const = 0;
    virtual std::size_t read_samples(PcmSample* destination, std::size_t max_samples) = 0;
    virtual bool eof() const = 0;
    virtual std::uint64_t total_samples_per_channel() const = 0;
    virtual std::string source_path() const = 0;
    virtual bool seek_to_sample(std::uint64_t sample_index) { (void)sample_index; return false; }
    virtual void interrupt() {}
};

} // namespace pcmtp
