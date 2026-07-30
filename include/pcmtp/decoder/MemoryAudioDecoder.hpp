// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <memory>
#include <string>

#include "pcmtp/decoder/IAudioDecoder.hpp"

namespace pcmtp {

class MemoryAudioDecoder final : public IAudioDecoder {
public:
    MemoryAudioDecoder(std::shared_ptr<PcmBuffer> samples,
                       AudioFormat format,
                       const std::string& label);

    void open(const std::string& path) override;
    const AudioFormat& format() const override;
    std::size_t read_samples(PcmSample* destination, std::size_t max_samples) override;
    bool eof() const override;
    std::uint64_t total_samples_per_channel() const override;
    std::string source_path() const override;
    bool seek_to_sample(std::uint64_t sample_index) override;

private:
    std::shared_ptr<PcmBuffer> samples_;
    AudioFormat format_{};
    std::string label_;
    std::string opened_path_;
    std::size_t cursor_ = 0;
};

} // namespace pcmtp
