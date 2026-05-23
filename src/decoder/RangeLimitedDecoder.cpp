#include "pcmtp/decoder/RangeLimitedDecoder.hpp"

#include <algorithm>
#include <stdexcept>

#include "pcmtp/util/Logger.hpp"

namespace pcmtp {

RangeLimitedDecoder::RangeLimitedDecoder(std::unique_ptr<IAudioDecoder> inner, std::uint64_t start_sample, std::uint64_t end_sample)
    : inner_(std::move(inner)), start_sample_(start_sample), end_sample_(end_sample) {
    if (!inner_) throw std::invalid_argument("RangeLimitedDecoder requires a valid decoder");
    if (end_sample_ > start_sample_) track_length_samples_ = end_sample_ - start_sample_;
}
void RangeLimitedDecoder::open(const std::string& path) {
    open_at_sample(path, 0);
}

void RangeLimitedDecoder::open_at_sample(const std::string& path, std::uint64_t sample_index) {
    const std::uint64_t clamped = std::min<std::uint64_t>(sample_index, track_length_samples_);
    inner_->open_at_sample(path, start_sample_ + clamped);
    if (end_sample_ <= start_sample_) {
        const std::uint64_t total = inner_->total_samples_per_channel();
        if (total > start_sample_) { end_sample_ = total; track_length_samples_ = end_sample_ - start_sample_; }
    }
    consumed_samples_per_channel_ = clamped;
    opened_ = true;
    Logger::instance().debug("RangeLimitedDecoder open: start=" + std::to_string(start_sample_) +
                             " end=" + std::to_string(end_sample_) +
                             " length=" + std::to_string(track_length_samples_) +
                             " offset=" + std::to_string(clamped) +
                             " source=" + inner_->source_path());
}
const AudioFormat& RangeLimitedDecoder::format() const { return inner_->format(); }
std::size_t RangeLimitedDecoder::read_samples(PcmSample* destination, std::size_t max_samples) {
    if (!opened_) throw std::runtime_error("RangeLimitedDecoder not opened");
    const std::uint16_t ch = std::max<std::uint16_t>(1, format().channels);
    const std::uint64_t rem = remaining_samples_per_channel();
    if (rem == 0) return 0;
    const std::uint64_t req_frames = static_cast<std::uint64_t>(max_samples / ch);
    const std::size_t req_samples = static_cast<std::size_t>(std::min<std::uint64_t>(rem, req_frames) * ch);
    const std::size_t got = inner_->read_samples(destination, req_samples);
    consumed_samples_per_channel_ += static_cast<std::uint64_t>(got / ch);
    return got;
}
bool RangeLimitedDecoder::eof() const { return remaining_samples_per_channel() == 0 || inner_->eof(); }
std::uint64_t RangeLimitedDecoder::total_samples_per_channel() const { return track_length_samples_; }
std::string RangeLimitedDecoder::source_path() const { return inner_->source_path(); }
bool RangeLimitedDecoder::seek_to_sample(std::uint64_t sample_index) {
    if (!opened_) return false;
    const std::uint64_t clamped = std::min<std::uint64_t>(sample_index, track_length_samples_);
    consumed_samples_per_channel_ = clamped;
    return inner_->seek_to_sample(start_sample_ + clamped);
}
std::uint64_t RangeLimitedDecoder::remaining_samples_per_channel() const {
    return track_length_samples_ > consumed_samples_per_channel_ ? (track_length_samples_ - consumed_samples_per_channel_) : 0;
}

} // namespace pcmtp
