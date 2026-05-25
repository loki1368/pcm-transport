#include "pcmtp/decoder/GaplessChainDecoder.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "pcmtp/decoder/FlacStreamDecoder.hpp"
#include "pcmtp/decoder/RangeLimitedDecoder.hpp"
#include "pcmtp/util/Logger.hpp"

namespace pcmtp {
namespace {

constexpr std::uint64_t kMinimumPrepareThresholdFrames = 32768;
constexpr std::uint64_t kNativePrepareSeconds = 2;
constexpr std::uint64_t kExternalPrepareSeconds = 5;
constexpr std::uint64_t kNativePrefetchMillis = 500;
constexpr std::uint64_t kExternalPrefetchMillis = 1000;
constexpr std::uint64_t kKeepaliveSilenceMillis = 120;

bool same_format(const AudioFormat& a, const AudioFormat& b) {
    return a.sample_rate == b.sample_rate &&
           a.channels == b.channels &&
           a.bits_per_sample == b.bits_per_sample;
}

} // namespace

GaplessChainDecoder::GaplessChainDecoder(std::vector<GaplessTrackSpec> tracks, std::uint64_t first_track_offset)
    : tracks_(std::move(tracks)), first_track_offset_(first_track_offset) {
    if (tracks_.empty()) {
        throw std::invalid_argument("GaplessChainDecoder requires at least one track");
    }
    format_ = tracks_.front().format;
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        if (!same_format(format_, tracks_[i].format)) {
            throw std::invalid_argument("GaplessChainDecoder requires matching output formats");
        }
        total_samples_per_channel_ += track_length(i);
    }
    if (first_track_offset_ > track_length(0)) {
        first_track_offset_ = track_length(0);
    }
}

GaplessChainDecoder::~GaplessChainDecoder() {
    close_prepare_thread();
}

std::unique_ptr<IAudioDecoder> GaplessChainDecoder::create_decoder_for_track(const GaplessTrackSpec& spec) const {
    std::unique_ptr<IAudioDecoder> decoder;
    if (spec.native_flac) {
        decoder.reset(new FlacStreamDecoder());
    } else {
        std::unique_ptr<ExternalAudioDecoder> external(new ExternalAudioDecoder(spec.forced_output_sample_rate,
                                                                                spec.forced_output_bits_per_sample,
                                                                                spec.resample_quality,
                                                                                spec.bitdepth_quality));
        if (spec.has_known_external_info) {
            external->set_known_info(spec.known_external_info);
        }
        decoder.reset(external.release());
    }
    if (spec.range_limited) {
        Logger::instance().debug("Legacy bounded transport enabled for gapless chain track");
        decoder.reset(new RangeLimitedDecoder(std::move(decoder), spec.start_sample, spec.end_sample));
    }
    return decoder;
}

void GaplessChainDecoder::open_current_decoder(std::uint64_t offset) {
    current_decoder_ = create_decoder_for_track(tracks_[current_index_]);
    current_decoder_->open_at_sample(tracks_[current_index_].path, offset);
    current_track_position_ = offset;
    keepalive_logged_ = false;
    Logger::instance().debug("GaplessChainDecoder opened track index=" + std::to_string(current_index_) +
                             " offset=" + std::to_string(offset) +
                             " path=" + tracks_[current_index_].path);
}

void GaplessChainDecoder::open(const std::string&) {
    open_at_sample(std::string(), first_track_offset_);
}

void GaplessChainDecoder::open_at_sample(const std::string&, std::uint64_t sample_index) {
    close_prepare_thread();
    current_index_ = 0;
    current_track_position_ = 0;
    reached_eof_ = false;
    opened_ = false;
    open_current_decoder(std::min<std::uint64_t>(sample_index, track_length(0)));
    opened_ = true;
    maybe_prepare_next();
}

const AudioFormat& GaplessChainDecoder::format() const {
    return format_;
}

std::size_t GaplessChainDecoder::read_samples(PcmSample* destination, std::size_t max_samples) {
    if (!opened_ || !current_decoder_) {
        throw std::runtime_error("GaplessChainDecoder not opened");
    }
    if (max_samples == 0 || reached_eof_) {
        return 0;
    }

    const std::uint16_t channels = std::max<std::uint16_t>(1, format_.channels);
    std::size_t copied = 0;
    while (copied < max_samples && !reached_eof_) {
        {
            std::lock_guard<std::mutex> lock(prepare_mutex_);
            if (prepared_.ready && prepared_.index == current_index_ && prepared_.prebuffer_offset < prepared_.prebuffer.size()) {
                const std::size_t available = prepared_.prebuffer.size() - prepared_.prebuffer_offset;
                const std::size_t take = std::min(available, max_samples - copied);
                std::copy(prepared_.prebuffer.data() + prepared_.prebuffer_offset,
                          prepared_.prebuffer.data() + prepared_.prebuffer_offset + take,
                          destination + copied);
                prepared_.prebuffer_offset += take;
                copied += take;
                current_track_position_ += static_cast<std::uint64_t>(take / channels);
                if (copied >= max_samples) {
                    break;
                }
            }
        }

        const std::uint64_t length = track_length(current_index_);
        const bool range_limited = tracks_[current_index_].range_limited;
        if ((range_limited && current_track_position_ >= length) || current_decoder_->eof()) {
            const SwitchResult sw = switch_to_next_track();
            if (sw == SwitchResult::Switched) {
                continue;
            }
            if (sw == SwitchResult::NotReady) {
                copied += fill_keepalive_silence(destination + copied, max_samples - copied);
                break;
            }
            reached_eof_ = true;
            break;
        }

        std::size_t request_samples = max_samples - copied;
        if (range_limited) {
            const std::uint64_t remaining_frames = length > current_track_position_ ? (length - current_track_position_) : 0;
            request_samples = static_cast<std::size_t>(std::min<std::uint64_t>(
                remaining_frames * channels,
                static_cast<std::uint64_t>(request_samples)));
        }
        if (request_samples == 0) {
            const SwitchResult sw = switch_to_next_track();
            if (sw == SwitchResult::Switched) {
                continue;
            }
            if (sw == SwitchResult::NotReady) {
                copied += fill_keepalive_silence(destination + copied, max_samples - copied);
                break;
            }
            reached_eof_ = true;
            break;
        }

        const std::size_t got = current_decoder_->read_samples(destination + copied, request_samples);
        if (got == 0) {
            const SwitchResult sw = switch_to_next_track();
            if (sw == SwitchResult::Switched) {
                continue;
            }
            if (sw == SwitchResult::NotReady) {
                copied += fill_keepalive_silence(destination + copied, max_samples - copied);
                break;
            }
            reached_eof_ = true;
            break;
        }
        copied += got;
        current_track_position_ += static_cast<std::uint64_t>(got / channels);
        maybe_prepare_next();
    }
    return copied;
}

bool GaplessChainDecoder::eof() const {
    return reached_eof_;
}

std::uint64_t GaplessChainDecoder::total_samples_per_channel() const {
    return total_samples_per_channel_;
}

std::string GaplessChainDecoder::source_path() const {
    if (current_index_ < tracks_.size()) {
        return tracks_[current_index_].path;
    }
    return std::string();
}

bool GaplessChainDecoder::seek_to_sample(std::uint64_t sample_index) {
    if (!opened_) {
        return false;
    }
    close_prepare_thread();
    reached_eof_ = false;
    std::uint64_t pos = sample_index;
    current_index_ = 0;
    while (current_index_ + 1 < tracks_.size()) {
        const std::uint64_t length = track_length(current_index_);
        if (pos < length) {
            break;
        }
        pos -= length;
        ++current_index_;
    }
    open_current_decoder(pos);
    maybe_prepare_next();
    return true;
}

void GaplessChainDecoder::close_prepare_thread() {
    if (prepare_thread_.joinable()) {
        prepare_thread_.join();
    }
    std::lock_guard<std::mutex> lock(prepare_mutex_);
    preparing_ = false;
    prepared_ = PreparedNext{};
    keepalive_logged_ = false;
}

void GaplessChainDecoder::maybe_prepare_next() {
    if (current_index_ + 1 >= tracks_.size()) {
        return;
    }
    const std::uint64_t length = track_length(current_index_);
    const std::uint64_t threshold = prepare_threshold_frames(current_index_);
    if (length > current_track_position_ && (length - current_track_position_) > threshold) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(prepare_mutex_);
        if (preparing_ || (prepared_.ready && prepared_.index == current_index_ + 1)) {
            return;
        }
        preparing_ = true;
    }
    if (prepare_thread_.joinable()) {
        prepare_thread_.join();
    }
    prepare_thread_ = std::thread(&GaplessChainDecoder::prepare_next_worker, this, current_index_ + 1);
}

void GaplessChainDecoder::prepare_next_worker(std::size_t index) {
    PreparedNext prepared;
    prepared.index = index;
    try {
        prepared.decoder = create_decoder_for_track(tracks_[index]);
        prepared.decoder->open_at_sample(tracks_[index].path, 0);
        prepared.prebuffer.resize(prebuffer_samples(index));
        const std::size_t got = prepared.decoder->read_samples(prepared.prebuffer.data(), prepared.prebuffer.size());
        prepared.prebuffer.resize(got);
        prepared.ready = true;
        Logger::instance().debug("GaplessChainDecoder prebuffered next track index=" + std::to_string(index) +
                                 " samples=" + std::to_string(got));
    } catch (const std::exception& ex) {
        prepared.failed = true;
        Logger::instance().error(std::string("GaplessChainDecoder failed to prebuffer next track: ") + ex.what());
    }
    std::lock_guard<std::mutex> lock(prepare_mutex_);
    prepared_ = std::move(prepared);
    preparing_ = false;
}

GaplessChainDecoder::SwitchResult GaplessChainDecoder::switch_to_next_track() {
    if (current_index_ + 1 >= tracks_.size()) {
        return SwitchResult::NoNext;
    }

    const std::size_t next_index = current_index_ + 1;
    bool still_preparing = false;
    {
        std::lock_guard<std::mutex> lock(prepare_mutex_);
        still_preparing = preparing_;
    }

    if (still_preparing) {
        if (!keepalive_logged_) {
            Logger::instance().debug("Gapless prebuffer was not ready; keeping ALSA stream alive with silence");
            keepalive_logged_ = true;
        }
        return SwitchResult::NotReady;
    }

    if (prepare_thread_.joinable()) {
        prepare_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(prepare_mutex_);
        if (prepared_.ready && prepared_.index == next_index && prepared_.decoder) {
            current_decoder_ = std::move(prepared_.decoder);
            current_index_ = next_index;
            current_track_position_ = 0;
            prepared_.index = current_index_;
            keepalive_logged_ = false;
            Logger::instance().debug("GaplessChainDecoder switched to prebuffered track index=" + std::to_string(current_index_));
            return SwitchResult::Switched;
        }
        if (prepared_.failed && prepared_.index == next_index) {
            Logger::instance().debug("GaplessChainDecoder prebuffer failed; opening next track synchronously");
        }
    }

    current_index_ = next_index;
    open_current_decoder(0);
    {
        std::lock_guard<std::mutex> lock(prepare_mutex_);
        prepared_ = PreparedNext{};
        preparing_ = false;
    }
    maybe_prepare_next();
    return SwitchResult::Switched;
}

std::size_t GaplessChainDecoder::fill_keepalive_silence(PcmSample* destination, std::size_t max_samples) {
    if (destination == nullptr || max_samples == 0) {
        return 0;
    }
    const std::size_t silence = std::min(max_samples, keepalive_silence_samples());
    std::fill(destination, destination + silence, 0);
    return silence;
}

std::uint64_t GaplessChainDecoder::track_length(std::size_t index) const {
    if (index >= tracks_.size()) {
        return 0;
    }
    const GaplessTrackSpec& spec = tracks_[index];
    if (spec.range_limited && spec.end_sample > spec.start_sample) {
        return spec.end_sample - spec.start_sample;
    }
    if (spec.end_sample > 0) {
        return spec.end_sample;
    }
    if (spec.known_external_info.total_samples_per_channel > 0) {
        return spec.known_external_info.total_samples_per_channel;
    }
    return 0;
}

std::uint64_t GaplessChainDecoder::prepare_threshold_frames(std::size_t index) const {
    if (index >= tracks_.size()) {
        return kMinimumPrepareThresholdFrames;
    }
    const std::uint32_t rate = std::max<std::uint32_t>(1, tracks_[index].format.sample_rate);
    const std::uint64_t seconds = tracks_[index].native_flac ? kNativePrepareSeconds : kExternalPrepareSeconds;
    return std::max<std::uint64_t>(kMinimumPrepareThresholdFrames, static_cast<std::uint64_t>(rate) * seconds);
}

std::size_t GaplessChainDecoder::prebuffer_samples(std::size_t index) const {
    if (index >= tracks_.size()) {
        return 16384;
    }
    const GaplessTrackSpec& spec = tracks_[index];
    const std::uint32_t rate = std::max<std::uint32_t>(1, spec.format.sample_rate);
    const std::uint16_t channels = std::max<std::uint16_t>(1, spec.format.channels);
    const std::uint64_t millis = spec.native_flac ? kNativePrefetchMillis : kExternalPrefetchMillis;
    const std::uint64_t frames = std::max<std::uint64_t>(8192, (static_cast<std::uint64_t>(rate) * millis) / 1000);
    const std::uint64_t samples = frames * static_cast<std::uint64_t>(channels);
    return static_cast<std::size_t>(std::min<std::uint64_t>(samples, static_cast<std::uint64_t>(1024 * 1024)));
}

std::size_t GaplessChainDecoder::keepalive_silence_samples() const {
    const std::uint32_t rate = std::max<std::uint32_t>(1, format_.sample_rate);
    const std::uint16_t channels = std::max<std::uint16_t>(1, format_.channels);
    const std::uint64_t frames = std::max<std::uint64_t>(512, (static_cast<std::uint64_t>(rate) * kKeepaliveSilenceMillis) / 1000);
    return static_cast<std::size_t>(frames * static_cast<std::uint64_t>(channels));
}

} // namespace pcmtp
