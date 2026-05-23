#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "pcmtp/decoder/IAudioDecoder.hpp"

namespace pcmtp {

class RangeLimitedDecoder final : public IAudioDecoder {
public:
    RangeLimitedDecoder(std::unique_ptr<IAudioDecoder> inner, std::uint64_t start_sample, std::uint64_t end_sample);
    void open(const std::string& path) override;
    void open_at_sample(const std::string& path, std::uint64_t sample_index) override;
    const AudioFormat& format() const override;
    std::size_t read_samples(PcmSample* destination, std::size_t max_samples) override;
    bool eof() const override;
    std::uint64_t total_samples_per_channel() const override;
    std::string source_path() const override;
    bool seek_to_sample(std::uint64_t sample_index) override;
private:
    std::uint64_t remaining_samples_per_channel() const;
    std::unique_ptr<IAudioDecoder> inner_;
    std::uint64_t start_sample_ = 0;
    std::uint64_t end_sample_ = 0;
    std::uint64_t track_length_samples_ = 0;
    std::uint64_t consumed_samples_per_channel_ = 0;
    bool opened_ = false;
};

} // namespace pcmtp
