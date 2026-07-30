// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/decoder/MemoryAudioDecoder.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace pcmtp {

MemoryAudioDecoder::MemoryAudioDecoder(std::shared_ptr<PcmBuffer> samples,
                                       AudioFormat format,
                                       const std::string& label)
    : samples_(std::move(samples)), format_(format), label_(label) {}

void MemoryAudioDecoder::open(const std::string& path) {
    if (!samples_) {
        throw std::runtime_error("MemoryAudioDecoder has no PCM buffer");
    }
    opened_path_ = path;
    cursor_ = 0;
}

const AudioFormat& MemoryAudioDecoder::format() const { return format_; }

std::size_t MemoryAudioDecoder::read_samples(PcmSample* destination, std::size_t max_samples) {
    if (!samples_) return 0;
    const std::size_t remaining = samples_->size() > cursor_ ? (samples_->size() - cursor_) : 0;
    const std::size_t count = std::min(max_samples, remaining);
    if (count > 0) {
        std::memcpy(destination, samples_->data() + cursor_, count * sizeof(PcmSample));
        cursor_ += count;
    }
    return count;
}

bool MemoryAudioDecoder::eof() const { return !samples_ || cursor_ >= samples_->size(); }

std::uint64_t MemoryAudioDecoder::total_samples_per_channel() const {
    if (!samples_ || format_.channels == 0) return 0;
    return static_cast<std::uint64_t>(samples_->size() / format_.channels);
}

std::string MemoryAudioDecoder::source_path() const {
    return opened_path_.empty() ? label_ : opened_path_;
}

bool MemoryAudioDecoder::seek_to_sample(std::uint64_t sample_index) {
    if (!samples_ || format_.channels == 0) return false;
    const std::size_t target = static_cast<std::size_t>(sample_index) * format_.channels;
    if (target > samples_->size()) return false;
    cursor_ = target;
    return true;
}

} // namespace pcmtp
