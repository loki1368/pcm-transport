// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/core/PlaybackEngine.hpp"
#include "pcmtp/core/Pcm16Quantizer.hpp"
#include "pcmtp/core/Pcm16Dither.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cerrno>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/eventfd.h>
#include <sched.h>
#include <unistd.h>
#include <gio/gio.h>
#include "pcmtp/dsp/ToneControlDesign.hpp"
#include "pcmtp/util/Logger.hpp"

namespace pcmtp {
namespace {

struct ShelfState {
    double z1 = 0.0;
    double z2 = 0.0;
};

using tone::ShelfCoefficients;

double process_sample(double input, const ShelfCoefficients& c, ShelfState& s) {
    const double out = c.b0 * input + s.z1;
    s.z1 = c.b1 * input - c.a1 * out + s.z2;
    s.z2 = c.b2 * input - c.a2 * out;
    return out;
}


std::int64_t pcm_minimum_sample(std::uint16_t bits_per_sample) {
    if (bits_per_sample >= 32) return std::numeric_limits<std::int32_t>::min();
    if (bits_per_sample <= 1) return -1;
    return -(1LL << (bits_per_sample - 1));
}

double clamp_sample_to_bits(double sample, std::uint16_t bits_per_sample) {
    const double maximum = static_cast<double>(pcm_full_scale(bits_per_sample));
    const double minimum = static_cast<double>(pcm_minimum_sample(bits_per_sample));
    if (maximum <= 0.0 || minimum >= 0.0) return sample;
    if (sample > maximum) return maximum;
    if (sample < minimum) return minimum;
    return sample;
}

PcmSample quantize_processed_sample(double sample,
                                    std::uint16_t bits_per_sample,
                                    Pcm16QuantizationMode pcm16_mode,
                                    double pcm16_dither_code_units = 0.0) {
    if (bits_per_sample == 16) {
        return quantize_pcm16_code_units(
            sample + pcm16_dither_code_units, pcm16_mode);
    }
    const double clamped = clamp_sample_to_bits(sample, bits_per_sample);
    return static_cast<PcmSample>(std::llround(clamped));
}

PcmSample quantize_working_sample_to_bits(double sample,
                                           std::uint16_t working_bits,
                                           std::uint16_t output_bits,
                                           Pcm16QuantizationMode pcm16_mode,
                                           double pcm16_dither_code_units = 0.0) {
    if (working_bits < output_bits || output_bits < 16 || output_bits > 32 ||
        working_bits > 32) {
        throw std::runtime_error("Unsupported PCM precision conversion");
    }
    const unsigned shift = static_cast<unsigned>(working_bits - output_bits);
    const double divisor = static_cast<double>(std::uint64_t{1} << shift);
    double scaled = sample / divisor;
    if (output_bits == 16) {
        return quantize_pcm16_code_units(
            scaled + pcm16_dither_code_units, pcm16_mode);
    }
    const double maximum = static_cast<double>(pcm_full_scale(output_bits));
    const double minimum = static_cast<double>(pcm_minimum_sample(output_bits));
    if (scaled > maximum) scaled = maximum;
    if (scaled < minimum) scaled = minimum;
    return static_cast<PcmSample>(std::llround(scaled));
}

PcmSample narrow_exact_working_sample(PcmSample sample,
                                      std::uint16_t working_bits,
                                      std::uint16_t output_bits) {
    if (working_bits <= output_bits || output_bits < 16 || working_bits > 32) {
        throw std::runtime_error("Unsupported exact PCM precision conversion");
    }
    const unsigned shift = static_cast<unsigned>(working_bits - output_bits);
    const std::int64_t divisor = std::int64_t{1} << shift;
    const std::int64_t value = static_cast<std::int64_t>(sample);
    if (value % divisor != 0) {
        throw std::runtime_error(
            "PCM working sample is not an exact widened destination value");
    }
    return static_cast<PcmSample>(value / divisor);
}

void widen_pcm_block_exact(PcmSample* samples,
                           std::size_t count,
                           std::uint16_t source_bits,
                           std::uint16_t working_bits) {
    if (source_bits >= working_bits || source_bits < 16 || working_bits > 32) {
        if (source_bits == working_bits) return;
        throw std::runtime_error("Unsupported exact PCM widening");
    }
    const unsigned shift = static_cast<unsigned>(working_bits - source_bits);
    for (std::size_t i = 0; i < count; ++i) {
        const std::int64_t value =
            static_cast<std::int64_t>(samples[i]) * (std::int64_t{1} << shift);
        if (value > std::numeric_limits<std::int32_t>::max() ||
            value < std::numeric_limits<std::int32_t>::min()) {
            throw std::runtime_error("Exact PCM widening overflow");
        }
        samples[i] = static_cast<PcmSample>(value);
    }
}

std::uint64_t make_pcm16_dither_seed(std::uint64_t transport_generation) noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::uint64_t time_seed = static_cast<std::uint64_t>(now);
    const std::uint64_t seed =
        time_seed ^ (transport_generation << 32U) ^ transport_generation;
    return seed != 0 ? seed : std::uint64_t{1};
}

double headroom_gain_from_tenths_db(int tenths_db) {
    const double db = static_cast<double>(std::max(0, tenths_db)) / 10.0;
    return std::pow(10.0, -db / 20.0);
}

struct ToneFilterTarget {
    int bass_db = 0;
    int treble_db = 0;
    int bass_hz = 100;
    int treble_hz = 10000;
    int headroom_tenths_db = 0;
};

bool same_filter_target(const ToneFilterTarget& a, const ToneFilterTarget& b) {
    return a.bass_db == b.bass_db &&
           a.treble_db == b.treble_db &&
           a.bass_hz == b.bass_hz &&
           a.treble_hz == b.treble_hz;
}

bool same_tone_target(const ToneFilterTarget& a, const ToneFilterTarget& b) {
    return same_filter_target(a, b) &&
           a.headroom_tenths_db == b.headroom_tenths_db;
}

bool tone_filter_has_processing(const ToneFilterTarget& target) {
    return target.bass_db != 0 || target.treble_db != 0;
}

bool tone_target_has_processing(const ToneFilterTarget& target) {
    return tone_filter_has_processing(target) || target.headroom_tenths_db > 0;
}

struct ToneFilterPath {
    ToneFilterTarget target{};
    ShelfCoefficients low{};
    ShelfCoefficients high{};
    ShelfState low_l{};
    ShelfState low_r{};
    ShelfState high_l{};
    ShelfState high_r{};
};

ToneFilterPath make_tone_filter_path(std::uint32_t sample_rate,
                                     const ToneFilterTarget& target) {
    ToneFilterPath path;
    path.target = target;
    path.low = tone::make_low_shelf(
        sample_rate, static_cast<double>(target.bass_db),
        static_cast<double>(target.bass_hz));
    path.high = tone::make_high_shelf(
        sample_rate, static_cast<double>(target.treble_db),
        static_cast<double>(target.treble_hz));
    return path;
}

double process_tone_filter_path(double input,
                                bool left,
                                double headroom_gain,
                                ToneFilterPath& path) {
    double sample = input * headroom_gain;
    if (path.target.bass_db != 0) {
        sample = process_sample(sample, path.low, left ? path.low_l : path.low_r);
    }
    if (path.target.treble_db != 0) {
        sample = process_sample(sample, path.high, left ? path.high_l : path.high_r);
    }
    return sample;
}

void rescale_shelf_state(ShelfState& state, double factor) {
    state.z1 *= factor;
    state.z2 *= factor;
}

void rescale_tone_filter_path(ToneFilterPath& path, double factor) {
    rescale_shelf_state(path.low_l, factor);
    rescale_shelf_state(path.low_r, factor);
    rescale_shelf_state(path.high_l, factor);
    rescale_shelf_state(path.high_r, factor);
}

class ToneFilterCrossfade {
public:
    ToneFilterCrossfade(std::uint32_t sample_rate,
                        const ToneFilterTarget& initial_target)
        : sample_rate_(sample_rate),
          transition_frames_(std::max<std::uint32_t>(
              1U, static_cast<std::uint32_t>(std::lround(
                  static_cast<double>(sample_rate) * 0.004)))),
          active_(make_tone_filter_path(sample_rate, initial_target)) {
        active_gain_ = cached_headroom_gain(initial_target.headroom_tenths_db);
        active_gain_target_ = active_gain_;
    }

    void request(const ToneFilterTarget& target) {
        if (!transitioning_) {
            if (same_filter_target(target, active_.target)) {
                active_.target.headroom_tenths_db = target.headroom_tenths_db;
                request_active_gain(cached_headroom_gain(
                    target.headroom_tenths_db));
                return;
            }
            if (!tone_filter_has_processing(active_.target) &&
                !tone_filter_has_processing(target)) {
                const double requested_gain = cached_headroom_gain(
                    target.headroom_tenths_db);
                active_.target = target;
                request_active_gain(requested_gain);
                return;
            }
            start_transition(target);
            return;
        }

        if (same_tone_target(target, incoming_.target)) {
            pending_valid_ = false;
            return;
        }

        const double requested_gain = cached_headroom_gain(
            target.headroom_tenths_db);
        if (requested_gain < active_gain_ - 1.0e-15) {
            active_gain_ = requested_gain;
            active_gain_target_ = requested_gain;
            active_gain_step_ = 0.0;
            active_gain_remaining_frames_ = 0;
        }
        if (requested_gain < incoming_gain_ - 1.0e-15) {
            incoming_gain_ = requested_gain;
        }

        pending_target_ = target;
        pending_valid_ = true;
    }

    double process(double input, bool left) {
        const double active_output = process_tone_filter_path(
            input, left, active_gain_, active_);
        if (!transitioning_) return active_output;

        const double incoming_output = process_tone_filter_path(
            input, left, incoming_gain_, incoming_);
        const double t = std::min(
            1.0, static_cast<double>(transition_position_ + 1U) /
                     static_cast<double>(transition_frames_));
        return active_output * (1.0 - t) + incoming_output * t;
    }

    void advance_frame() {
        if (transitioning_) {
            ++transition_position_;
            if (transition_position_ < transition_frames_) return;

            active_ = incoming_;
            active_gain_ = incoming_gain_;
            active_gain_target_ = active_gain_;
            active_gain_step_ = 0.0;
            active_gain_remaining_frames_ = 0;
            transitioning_ = false;
            transition_position_ = 0;
            if (pending_valid_) {
                const ToneFilterTarget pending = pending_target_;
                pending_valid_ = false;
                request(pending);
            }
            return;
        }

        if (active_gain_remaining_frames_ == 0) return;
        active_gain_ += active_gain_step_;
        --active_gain_remaining_frames_;
        if (active_gain_remaining_frames_ == 0) {
            active_gain_ = active_gain_target_;
        }
    }

    bool requires_processing() const {
        if (tone_target_has_processing(active_.target) ||
            std::fabs(active_gain_ - 1.0) > 1.0e-15 ||
            active_gain_remaining_frames_ != 0) {
            return true;
        }
        if (transitioning_ && tone_target_has_processing(incoming_.target)) {
            return true;
        }
        return pending_valid_ && tone_target_has_processing(pending_target_);
    }

    void rescale_state(double factor) {
        if (std::fabs(factor - 1.0) < 1.0e-15) {
            return;
        }
        rescale_tone_filter_path(active_, factor);
        if (transitioning_) {
            rescale_tone_filter_path(incoming_, factor);
        }
    }

private:
    double cached_headroom_gain(int tenths_db) {
        const int normalized_tenths_db = std::max(0, tenths_db);
        if (!cached_headroom_gain_valid_ ||
            normalized_tenths_db != cached_headroom_tenths_db_) {
            cached_headroom_tenths_db_ = normalized_tenths_db;
            cached_headroom_gain_ =
                headroom_gain_from_tenths_db(normalized_tenths_db);
            cached_headroom_gain_valid_ = true;
        }
        return cached_headroom_gain_;
    }

    void request_active_gain(double target_gain) {
        if (target_gain < active_gain_ - 1.0e-15) {
            active_gain_ = target_gain;
            active_gain_target_ = target_gain;
            active_gain_step_ = 0.0;
            active_gain_remaining_frames_ = 0;
            return;
        }
        if (std::fabs(target_gain - active_gain_target_) < 1.0e-15) return;
        active_gain_target_ = target_gain;
        active_gain_remaining_frames_ = transition_frames_;
        active_gain_step_ =
            (active_gain_target_ - active_gain_) /
            static_cast<double>(active_gain_remaining_frames_);
    }

    void start_transition(const ToneFilterTarget& target) {
        active_gain_target_ = active_gain_;
        active_gain_step_ = 0.0;
        active_gain_remaining_frames_ = 0;
        incoming_ = make_tone_filter_path(sample_rate_, target);
        incoming_gain_ = cached_headroom_gain(target.headroom_tenths_db);
        transition_position_ = 0;
        transitioning_ = true;
    }

    std::uint32_t sample_rate_ = 0;
    std::uint32_t transition_frames_ = 1;
    std::uint32_t transition_position_ = 0;
    ToneFilterPath active_{};
    ToneFilterPath incoming_{};
    ToneFilterTarget pending_target_{};
    int cached_headroom_tenths_db_ = 0;
    double cached_headroom_gain_ = 1.0;
    bool cached_headroom_gain_valid_ = false;
    double active_gain_ = 1.0;
    double active_gain_target_ = 1.0;
    double active_gain_step_ = 0.0;
    double incoming_gain_ = 1.0;
    std::uint32_t active_gain_remaining_frames_ = 0;
    bool transitioning_ = false;
    bool pending_valid_ = false;
};


constexpr int kPreEqHeadroomMaxTenthsDb = 320;
constexpr std::uint32_t kNoMeterMeasurement = ~std::uint32_t{0};
constexpr std::uint32_t kMaxMeterPeakUnits = kNoMeterMeasurement - 1;
constexpr float kMeterPeakScale = 16777216.0f;
constexpr std::uint32_t kPlaybackEventSegmentChanged = 1U << 0;
constexpr std::uint32_t kPlaybackEventProcessingStateChanged = 1U << 1;
constexpr std::uint32_t kPlaybackEventFinished = 1U << 2;
constexpr std::uint32_t kPlaybackEventError = 1U << 3;

std::uint32_t meter_peak_to_units(float peak) {
    if (!(peak > 0.0f)) {
        return 0;
    }
    const float max_peak =
        static_cast<float>(kMaxMeterPeakUnits) / kMeterPeakScale;
    if (peak >= max_peak) {
        return kMaxMeterPeakUnits;
    }
    return static_cast<std::uint32_t>((peak * kMeterPeakScale) + 0.5f);
}

float meter_peak_from_units(std::uint32_t units) {
    return static_cast<float>(units) / kMeterPeakScale;
}

void publish_meter_peak(std::atomic<std::uint32_t>& slot, float peak) {
    const std::uint32_t peak_units = meter_peak_to_units(peak);
    std::uint32_t current_units = slot.load(std::memory_order_relaxed);
    while (true) {
        if (current_units != kNoMeterMeasurement &&
            current_units >= peak_units) {
            return;
        }
        if (slot.compare_exchange_weak(
                current_units,
                peak_units,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            return;
        }
    }
}

} // namespace

PlaybackEngine::PlaybackEngine() {
    playback_event_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (playback_event_fd_ < 0) {
        throw std::runtime_error(
            std::string("Cannot create playback event mailbox: ") +
            std::strerror(errno));
    }
}

PlaybackEngine::~PlaybackEngine() {
    stop();
    if (playback_event_fd_ >= 0) {
        close(playback_event_fd_);
        playback_event_fd_ = -1;
    }
}

void PlaybackEngine::start(std::unique_ptr<IAudioDecoder> decoder,
                           std::unique_ptr<IAudioBackend> backend,
                           const std::string& device_name,
                           const std::vector<std::uint16_t>& output_precision_candidates,
                           std::uint64_t initial_samples_per_channel,
                           std::vector<std::uint64_t> logical_segment_offsets,
                           Pcm16QuantizationMode pcm16_quantization_mode,
                           Pcm16DitherMode pcm16_dither_mode) {
    stop();
    const std::uint64_t transport_generation =
        transport_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::uint64_t pcm16_dither_seed =
        make_pcm16_dither_seed(transport_generation);
    if (!decoder || !backend) {
        throw std::invalid_argument("PlaybackEngine::start received null decoder/backend");
    }
    if (!logical_segment_offsets.empty()) {
        if (logical_segment_offsets.size() < 2 ||
            logical_segment_offsets.front() != 0) {
            throw std::invalid_argument(
                "PlaybackEngine::start received an invalid logical segment timeline");
        }
        for (std::size_t i = 1; i < logical_segment_offsets.size(); ++i) {
            if (logical_segment_offsets[i] < logical_segment_offsets[i - 1]) {
                throw std::invalid_argument(
                    "PlaybackEngine::start received a non-monotonic logical segment timeline");
            }
        }
    }

    const AudioFormat decoder_working_format = decoder->format();
    if (decoder_working_format.sample_rate == 0 ||
        decoder_working_format.channels == 0 ||
        decoder_working_format.bits_per_sample < 16 ||
        decoder_working_format.bits_per_sample > 32) {
        throw std::invalid_argument(
            "PlaybackEngine::start received an invalid working PCM format");
    }
    AudioFormat output_base_format = decoder_working_format;
    const AudioFormat opened_format = backend->open_with_precision_candidates(
        device_name, output_base_format, output_precision_candidates);
    AudioFormat working_format = decoder_working_format;
    working_format.bits_per_sample = std::max(
        decoder_working_format.bits_per_sample, opened_format.bits_per_sample);
    const std::string opened_report = backend->active_output_report();
    const std::uint64_t total_samples_per_channel =
        decoder->total_samples_per_channel();
    const DecoderSegmentPosition segment = decoder->segment_position();
    const TransportTruncationKind transport_truncation_kind =
        decoder->transport_truncation_kind();
    const DecoderRuntimeStateSnapshot initial_decoder_runtime_state =
        decoder->runtime_state_snapshot();

    PlaybackStatusSnapshot initial_snapshot;
    initial_snapshot.playing = true;
    initial_snapshot.paused = false;
    initial_snapshot.finished = false;
    initial_snapshot.format = opened_format;
    initial_snapshot.total_samples_per_channel = total_samples_per_channel;
    initial_snapshot.segment_position_valid = segment.valid;
    initial_snapshot.segment_index = segment.index;
    initial_snapshot.segment_samples_per_channel = segment.samples_per_channel;
    initial_snapshot.transport_truncation_kind = transport_truncation_kind;
    initial_snapshot.message = "Playing";
    initial_snapshot.active_output_report = opened_report;
    initial_snapshot.current_samples_per_channel = initial_samples_per_channel;

    std::string opened_device_name = device_name;
    std::string runtime_output_report = opened_report;

    try {
        decoder_ = std::move(decoder);
        backend_ = std::move(backend);
        device_name_ = std::move(opened_device_name);
        stop_requested_ = false;
        pause_requested_ = false;
        level_meter_peak_units_.store(kNoMeterMeasurement, std::memory_order_relaxed);
        clipped_samples_pending_.store(0, std::memory_order_relaxed);
        meter_transport_active_.store(true, std::memory_order_release);
        initial_samples_per_channel_ = initial_samples_per_channel;
        logical_segment_offsets_ = std::move(logical_segment_offsets);
        format_ = working_format;
        output_format_ = opened_format;
        pcm16_quantization_mode_ = pcm16_quantization_mode;
        pcm16_dither_mode_ = pcm16_dither_mode;
        pcm16_dither_ = std::make_unique<Pcm16Dither>(
            pcm16_dither_mode_, working_format.channels, pcm16_dither_seed);
        resampler_runtime_kind_.store(
            initial_decoder_runtime_state.resampler_runtime_kind,
            std::memory_order_release);
        pcm16_quantization_runtime_kind_.store(
            initial_decoder_runtime_state.pcm16_quantization_runtime_kind,
            std::memory_order_release);
        pcm16_quantization_stage_count_.store(
            initial_decoder_runtime_state.pcm16_quantization_stage_count,
            std::memory_order_release);
        pcm16_dither_runtime_kind_.store(
            Pcm16DitherRuntimeKind::NotUsed, std::memory_order_release);
        decoded_pcm_sample_kind_.store(
            initial_decoder_runtime_state.decoded_pcm_sample_kind,
            std::memory_order_release);
        decoded_pcm_significant_bits_.store(
            initial_decoder_runtime_state.decoded_pcm_significant_bits,
            std::memory_order_release);
        encoded_bitrate_bps_.store(
            initial_decoder_runtime_state.encoded_bitrate_bps,
            std::memory_order_release);
        {
            std::lock_guard<std::mutex> runtime_lock(runtime_mutex_);
            last_active_output_report_ = std::move(runtime_output_report);
            source_codec_name_ = initial_decoder_runtime_state.source_codec_name;
            decoded_codec_name_ =
                initial_decoder_runtime_state.decoder_implementation_name;
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            snapshot_ = std::move(initial_snapshot);
            transport_working_format_ = working_format;
            last_error_.clear();
            publish_live_transport_position(initial_samples_per_channel, segment);
        }
        playback_thread_ = std::thread(
            &PlaybackEngine::playback_loop, this, transport_generation);
    } catch (...) {
        stop_requested_ = true;
        pause_requested_ = false;
        meter_transport_active_.store(false, std::memory_order_release);
        if (backend_) {
            try {
                backend_->close();
            } catch (...) {}
        }
        decoder_.reset();
        backend_.reset();
        resampler_runtime_kind_.store(
            ResamplerRuntimeKind::NotUsed, std::memory_order_release);
        pcm16_quantization_runtime_kind_.store(
            Pcm16QuantizationRuntimeKind::NotUsed, std::memory_order_release);
        pcm16_quantization_stage_count_.store(0, std::memory_order_release);
        pcm16_dither_runtime_kind_.store(
            Pcm16DitherRuntimeKind::NotUsed, std::memory_order_release);
        decoded_pcm_sample_kind_.store(
            DecoderPcmSampleKind::Unknown, std::memory_order_release);
        decoded_pcm_significant_bits_.store(0, std::memory_order_release);
        encoded_bitrate_bps_.store(0, std::memory_order_release);
        pcm16_quantization_mode_ = Pcm16QuantizationMode::RoundToNearestEven;
        pcm16_dither_mode_ = Pcm16DitherMode::Off;
        pcm16_dither_.reset();
        device_name_.clear();
        format_ = AudioFormat{};
        output_format_ = AudioFormat{};
        initial_samples_per_channel_ = 0;
        logical_segment_offsets_.clear();
        {
            std::lock_guard<std::mutex> runtime_lock(runtime_mutex_);
            last_active_output_report_.clear();
            source_codec_name_.clear();
            decoded_codec_name_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            snapshot_ = PlaybackStatusSnapshot{};
            transport_working_format_ = AudioFormat{};
            snapshot_.message = "Stopped";
            last_error_.clear();
            publish_live_transport_position(0, DecoderSegmentPosition{});
        }
        throw;
    }

    Logger::instance().info("Playback started on device: " + device_name_);
}

void PlaybackEngine::stop() {
    transport_generation_.fetch_add(1, std::memory_order_acq_rel);
    meter_transport_active_.store(false, std::memory_order_release);
    stop_requested_ = true;
    pause_requested_ = false;
    if (decoder_ != nullptr) {
        decoder_->request_abort();
    }
    pause_cv_.notify_all();
    join_threads();
    playback_thread_tid_.store(0, std::memory_order_relaxed);
    clear_pending_playback_events();
    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = RealtimePriorityStatusSnapshot{};
        realtime_priority_status_.enabled =
            realtime_priority_enabled_.load(std::memory_order_relaxed);
        realtime_priority_status_.text = realtime_priority_status_.enabled
            ? std::string("Realtime priority: inactive, playback stopped")
            : std::string("Realtime priority: disabled");
    }
    if (backend_) {
        try { backend_->close(); } catch (...) {}
    }
    decoder_.reset();
    backend_.reset();
    resampler_runtime_kind_.store(
        ResamplerRuntimeKind::NotUsed, std::memory_order_release);
    pcm16_quantization_runtime_kind_.store(
        Pcm16QuantizationRuntimeKind::NotUsed, std::memory_order_release);
    pcm16_quantization_stage_count_.store(0, std::memory_order_release);
    pcm16_dither_runtime_kind_.store(
        Pcm16DitherRuntimeKind::NotUsed, std::memory_order_release);
    decoded_pcm_sample_kind_.store(
        DecoderPcmSampleKind::Unknown, std::memory_order_release);
    decoded_pcm_significant_bits_.store(0, std::memory_order_release);
    encoded_bitrate_bps_.store(0, std::memory_order_release);
    pcm16_quantization_mode_ = Pcm16QuantizationMode::RoundToNearestEven;
    pcm16_dither_mode_ = Pcm16DitherMode::Off;
    pcm16_dither_.reset();
    format_ = AudioFormat{};
    output_format_ = AudioFormat{};
    logical_segment_offsets_.clear();
    level_meter_peak_units_.store(kNoMeterMeasurement, std::memory_order_relaxed);
    clipped_samples_pending_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> runtime_lock(runtime_mutex_);
        last_active_output_report_.clear();
        source_codec_name_.clear();
        decoded_codec_name_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        snapshot_.playing = false;
        snapshot_.paused = false;
        transport_working_format_ = AudioFormat{};
        snapshot_.active_output_report.clear();
        if (!snapshot_.finished) {
            snapshot_.current_samples_per_channel = 0;
            snapshot_.segment_position_valid = false;
            snapshot_.segment_index = 0;
            snapshot_.segment_samples_per_channel = 0;
            publish_live_transport_position(0, DecoderSegmentPosition{});
            snapshot_.message = last_error_.empty() ? "Stopped" : last_error_;
        }
    }
}

void PlaybackEngine::pause() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!snapshot_.playing) {
        return;
    }
    pause_requested_.store(true, std::memory_order_release);
    meter_transport_active_.store(false, std::memory_order_release);
    snapshot_.paused = true;
    snapshot_.message = "Paused";
}

void PlaybackEngine::resume() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!snapshot_.playing) {
            return;
        }
        pause_requested_.store(false, std::memory_order_release);
        meter_transport_active_.store(true, std::memory_order_release);
        snapshot_.paused = false;
        snapshot_.message = "Playing";
    }
    pause_cv_.notify_all();
}

void PlaybackEngine::request_stop_after_current_segment(std::uint64_t segment_end_sample) {
    if (decoder_ != nullptr) {
        decoder_->request_stop_after_current_segment(segment_end_sample);
    }
}

void PlaybackEngine::request_stop_after_segment(std::size_t segment_index) {
    if (decoder_ != nullptr) {
        decoder_->request_stop_after_segment(segment_index);
    }
}

bool PlaybackEngine::is_playing() const { std::lock_guard<std::mutex> lock(state_mutex_); return snapshot_.playing; }
bool PlaybackEngine::is_paused() const { std::lock_guard<std::mutex> lock(state_mutex_); return snapshot_.paused; }
void PlaybackEngine::set_soft_volume_percent(int percent) { soft_volume_percent_.store(std::max(0, std::min(100, percent)), std::memory_order_relaxed); }
int PlaybackEngine::soft_volume_percent() const { return soft_volume_percent_.load(std::memory_order_relaxed); }
void PlaybackEngine::set_soft_eq(int bass_db, int treble_db) { bass_db_.store(std::max(-12, std::min(12, bass_db)), std::memory_order_relaxed); treble_db_.store(std::max(-12, std::min(12, treble_db)), std::memory_order_relaxed); }
void PlaybackEngine::set_pre_eq_headroom_tenths_db(int tenths_db) { pre_eq_headroom_tenths_db_.store(std::max(0, std::min(kPreEqHeadroomMaxTenthsDb, tenths_db)), std::memory_order_relaxed); }
int PlaybackEngine::pre_eq_headroom_tenths_db() const { return pre_eq_headroom_tenths_db_.load(std::memory_order_relaxed); }
void PlaybackEngine::set_soft_eq_profile(int bass_hz, int treble_hz) { bass_hz_.store(tone::clamp_bass_hz(bass_hz), std::memory_order_relaxed); treble_hz_.store(tone::clamp_treble_hz(treble_hz), std::memory_order_relaxed); }
void PlaybackEngine::set_level_meter_enabled(bool enabled) {
    level_meter_enabled_.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        level_meter_peak_units_.store(kNoMeterMeasurement, std::memory_order_relaxed);
    }
}
void PlaybackEngine::set_clip_detection_enabled(bool enabled) {
    clip_detection_enabled_.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        clipped_samples_pending_.store(0, std::memory_order_relaxed);
    }
}
int PlaybackEngine::bass_db() const { return bass_db_.load(std::memory_order_relaxed); }
int PlaybackEngine::treble_db() const { return treble_db_.load(std::memory_order_relaxed); }
ResamplerRuntimeKind PlaybackEngine::resampler_runtime_kind() const noexcept {
    return resampler_runtime_kind_.load(std::memory_order_acquire);
}

Pcm16QuantizationRuntimeKind
PlaybackEngine::pcm16_quantization_runtime_kind() const noexcept {
    return pcm16_quantization_runtime_kind_.load(std::memory_order_acquire);
}

std::uint32_t PlaybackEngine::pcm16_quantization_stage_count() const noexcept {
    return pcm16_quantization_stage_count_.load(std::memory_order_acquire);
}

Pcm16DitherRuntimeKind PlaybackEngine::pcm16_dither_runtime_kind() const noexcept {
    return pcm16_dither_runtime_kind_.load(std::memory_order_acquire);
}

std::string PlaybackEngine::source_codec_name() const {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    return source_codec_name_;
}

std::uint64_t PlaybackEngine::encoded_bitrate_bps() const noexcept {
    return encoded_bitrate_bps_.load(std::memory_order_acquire);
}

std::string PlaybackEngine::decoded_codec_name() const {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    return decoded_codec_name_;
}

DecoderPcmSampleKind PlaybackEngine::decoded_pcm_sample_kind() const noexcept {
    return decoded_pcm_sample_kind_.load(std::memory_order_acquire);
}

std::uint16_t PlaybackEngine::decoded_pcm_significant_bits() const noexcept {
    return decoded_pcm_significant_bits_.load(std::memory_order_acquire);
}
PlaybackStatusSnapshot PlaybackEngine::snapshot() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    PlaybackStatusSnapshot status = snapshot_;
    const LiveTransportPosition live = read_live_transport_position();
    status.current_samples_per_channel = live.current_samples_per_channel;
    status.segment_position_valid = live.segment_position_valid;
    status.segment_index = live.segment_index;
    status.segment_samples_per_channel = live.segment_samples_per_channel;
    return status;
}
PlaybackTransportSnapshot PlaybackEngine::transport_snapshot() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const LiveTransportPosition live = read_live_transport_position();
    PlaybackTransportSnapshot transport;
    transport.playing = snapshot_.playing;
    transport.paused = snapshot_.paused;
    transport.finished = snapshot_.finished;
    transport.format = snapshot_.format;
    transport.working_format = transport_working_format_;
    transport.current_samples_per_channel = live.current_samples_per_channel;
    transport.total_samples_per_channel = snapshot_.total_samples_per_channel;
    transport.segment_position_valid = live.segment_position_valid;
    transport.segment_index = live.segment_index;
    transport.segment_samples_per_channel = live.segment_samples_per_channel;
    transport.transport_truncation_kind = snapshot_.transport_truncation_kind;
    return transport;
}
PlaybackMeterSnapshot PlaybackEngine::consume_meter_snapshot() {
    PlaybackMeterSnapshot meter;
    const std::uint32_t peak_units = level_meter_peak_units_.exchange(
        kNoMeterMeasurement, std::memory_order_acq_rel);
    if (peak_units != kNoMeterMeasurement) {
        meter.peak_measured = true;
        meter.peak_level = meter_peak_from_units(peak_units);
    }
    meter.clipped_samples = clipped_samples_pending_.exchange(0, std::memory_order_relaxed);
    meter.transport_active = meter_transport_active_.load(std::memory_order_acquire);
    return meter;
}
void PlaybackEngine::publish_live_transport_position(
    std::uint64_t current_samples_per_channel,
    const DecoderSegmentPosition& segment) noexcept {
    // One writer is active at a time: start() publishes before the playback
    // thread starts, the playback thread publishes while active, and stop()
    // publishes only after join().  The odd sequence marks an update in
    // progress; release/acquire field operations make a reader that observes
    // any new field also observe the changed sequence before accepting it.
    const std::uint64_t previous_sequence = live_position_sequence_.fetch_add(
        1, std::memory_order_relaxed);

    live_current_samples_per_channel_.store(
        current_samples_per_channel, std::memory_order_release);
    live_segment_position_valid_.store(segment.valid, std::memory_order_release);
    live_segment_index_.store(segment.index, std::memory_order_release);
    live_segment_samples_per_channel_.store(
        segment.samples_per_channel, std::memory_order_release);

    live_position_sequence_.store(previous_sequence + 2,
                                  std::memory_order_release);
}

PlaybackEngine::LiveTransportPosition
PlaybackEngine::read_live_transport_position() const noexcept {
    while (true) {
        const std::uint64_t before = live_position_sequence_.load(
            std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue;
        }

        LiveTransportPosition live;
        live.current_samples_per_channel =
            live_current_samples_per_channel_.load(std::memory_order_acquire);
        live.segment_position_valid =
            live_segment_position_valid_.load(std::memory_order_acquire);
        live.segment_index =
            live_segment_index_.load(std::memory_order_acquire);
        live.segment_samples_per_channel =
            live_segment_samples_per_channel_.load(std::memory_order_acquire);

        const std::uint64_t after = live_position_sequence_.load(
            std::memory_order_acquire);
        if (before == after && (after & 1U) == 0) {
            return live;
        }
    }
}

int PlaybackEngine::playback_event_fd() const noexcept {
    return playback_event_fd_;
}

std::size_t PlaybackEngine::drain_playback_events(
    std::array<PlaybackEvent, 4>& events) noexcept {
    if (playback_event_fd_ < 0) {
        return 0;
    }

    std::uint64_t wake_count = 0;
    while (true) {
        const ssize_t result = read(playback_event_fd_,
                                    &wake_count,
                                    sizeof(wake_count));
        if (result == static_cast<ssize_t>(sizeof(wake_count))) {
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    const std::uint32_t pending = pending_playback_event_bits_.exchange(
        0, std::memory_order_acq_rel);
    std::size_t count = 0;
    const auto append = [&](PlaybackEventKind kind,
                            std::uint64_t generation) {
        events[count++] = PlaybackEvent{kind, generation};
    };
    if ((pending & kPlaybackEventSegmentChanged) != 0) {
        append(PlaybackEventKind::SegmentChanged,
               pending_segment_generation_.load(std::memory_order_acquire));
    }
    if ((pending & kPlaybackEventProcessingStateChanged) != 0) {
        append(PlaybackEventKind::ProcessingStateChanged,
               pending_processing_state_generation_.load(std::memory_order_acquire));
    }
    if ((pending & kPlaybackEventFinished) != 0) {
        append(PlaybackEventKind::Finished,
               pending_finished_generation_.load(std::memory_order_acquire));
    }
    if ((pending & kPlaybackEventError) != 0) {
        append(PlaybackEventKind::Error,
               pending_error_generation_.load(std::memory_order_acquire));
    }
    return count;
}

bool PlaybackEngine::consume_finished_transport() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (snapshot_.playing || !snapshot_.finished) {
            return false;
        }
    }

    stop();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        snapshot_.finished = false;
        snapshot_.current_samples_per_channel = 0;
        snapshot_.segment_position_valid = false;
        snapshot_.segment_index = 0;
        snapshot_.segment_samples_per_channel = 0;
        publish_live_transport_position(0, DecoderSegmentPosition{});
        snapshot_.message = last_error_.empty() ? "Stopped" : last_error_;
    }
    return true;
}

std::uint64_t PlaybackEngine::transport_generation() const noexcept {
    return transport_generation_.load(std::memory_order_acquire);
}

void PlaybackEngine::emit_playback_event(
    PlaybackEventKind kind,
    std::uint64_t transport_generation) noexcept {
    std::uint32_t bit = 0;
    switch (kind) {
        case PlaybackEventKind::SegmentChanged:
            pending_segment_generation_.store(transport_generation,
                                              std::memory_order_release);
            bit = kPlaybackEventSegmentChanged;
            break;
        case PlaybackEventKind::ProcessingStateChanged:
            pending_processing_state_generation_.store(
                transport_generation, std::memory_order_release);
            bit = kPlaybackEventProcessingStateChanged;
            break;
        case PlaybackEventKind::Finished:
            pending_finished_generation_.store(transport_generation,
                                               std::memory_order_release);
            bit = kPlaybackEventFinished;
            break;
        case PlaybackEventKind::Error:
            pending_error_generation_.store(transport_generation,
                                            std::memory_order_release);
            bit = kPlaybackEventError;
            break;
    }
    pending_playback_event_bits_.fetch_or(bit, std::memory_order_release);

    if (playback_event_fd_ < 0) {
        return;
    }
    const std::uint64_t wake = 1;
    while (write(playback_event_fd_, &wake, sizeof(wake)) < 0 && errno == EINTR) {
    }
}

void PlaybackEngine::clear_pending_playback_events() noexcept {
    pending_playback_event_bits_.store(0, std::memory_order_release);
    if (playback_event_fd_ < 0) {
        return;
    }
    std::uint64_t wake_count = 0;
    while (true) {
        const ssize_t result = read(playback_event_fd_,
                                    &wake_count,
                                    sizeof(wake_count));
        if (result == static_cast<ssize_t>(sizeof(wake_count))) {
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

void PlaybackEngine::set_realtime_priority_enabled(bool enabled) {
    realtime_priority_enabled_.store(enabled, std::memory_order_relaxed);

    const long tid = playback_thread_tid_.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    if (!enabled) {
        if (tid > 0 && realtime_priority_status_.tid == tid) {
            realtime_priority_status_.enabled = false;
            realtime_priority_status_.error.clear();
            format_realtime_priority_status(realtime_priority_status_);
        } else {
            realtime_priority_status_ = RealtimePriorityStatusSnapshot{};
        }
    } else if (!realtime_priority_status_.enabled) {
        realtime_priority_status_ = RealtimePriorityStatusSnapshot{};
        realtime_priority_status_.enabled = true;
        realtime_priority_status_.text = "Realtime priority: inactive, playback stopped";
    }
}

void PlaybackEngine::set_realtime_priority(int priority) {
    realtime_priority_.store(std::max(1, std::min(80, priority)), std::memory_order_relaxed);
}

namespace {

std::string gerror_message(const char* prefix, GError* error) {
    std::string message = prefix != nullptr ? std::string(prefix) : std::string("error");
    if (error != nullptr && error->message != nullptr) {
        message += ": ";
        message += error->message;
    }
    return message;
}

GDBusConnection* rtkit_system_bus(std::string& error_message) {
    GError* error = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (connection == nullptr) {
        error_message = gerror_message("system D-Bus unavailable", error);
        if (error != nullptr) g_error_free(error);
        return nullptr;
    }
    return connection;
}

std::string concise_rtkit_error(GError* error) {
    if (error == nullptr || error->message == nullptr) {
        return "request failed";
    }
    const std::string msg(error->message);
    if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
        g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
        msg.find("ServiceUnknown") != std::string::npos ||
        msg.find("NameHasNoOwner") != std::string::npos ||
        msg.find("Name has no owner") != std::string::npos ||
        msg.find("was not provided by any .service files") != std::string::npos ||
        msg.find("No such interface") != std::string::npos) {
        return "service not available";
    }
    if (msg.find("AccessDenied") != std::string::npos ||
        msg.find("not permitted") != std::string::npos ||
        msg.find("Operation not permitted") != std::string::npos) {
        return "access denied by system policy";
    }
    if (msg.find("Failed to activate service") != std::string::npos ||
        msg.find("Spawn.ChildExited") != std::string::npos) {
        return "service activation failed";
    }
    return msg;
}

std::string rtkit_query_error(const char* context, GError* error) {
    const std::string concise = concise_rtkit_error(error);
    if (concise == "service not available") {
        return "RTKit service not available";
    }
    std::string message = context != nullptr ? std::string(context) : std::string("RTKit request failed");
    if (!concise.empty()) {
        message += ": ";
        message += concise;
    }
    return message;
}

bool rtkit_get_max_realtime_priority(GDBusConnection* connection, int& max_priority, std::string& error_message) {
    max_priority = 0;
    if (connection == nullptr) {
        error_message = "invalid D-Bus connection";
        return false;
    }
    GError* error = nullptr;
    GVariant* result = g_dbus_connection_call_sync(connection,
                                                   "org.freedesktop.RealtimeKit1",
                                                   "/org/freedesktop/RealtimeKit1",
                                                   "org.freedesktop.DBus.Properties",
                                                   "Get",
                                                   g_variant_new("(ss)", "org.freedesktop.RealtimeKit1", "MaxRealtimePriority"),
                                                   G_VARIANT_TYPE("(v)"),
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   1500,
                                                   nullptr,
                                                   &error);
    if (result == nullptr) {
        error_message = rtkit_query_error("RTKit MaxRealtimePriority query failed", error);
        if (error != nullptr) g_error_free(error);
        return false;
    }

    GVariant* value = nullptr;
    g_variant_get(result, "(v)", &value);
    if (value != nullptr) {
        const GVariantType* type = g_variant_get_type(value);
        if (g_variant_type_equal(type, G_VARIANT_TYPE_INT32)) {
            max_priority = static_cast<int>(g_variant_get_int32(value));
        } else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT32)) {
            max_priority = static_cast<int>(g_variant_get_uint32(value));
        } else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT64)) {
            max_priority = static_cast<int>(g_variant_get_int64(value));
        } else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT64)) {
            max_priority = static_cast<int>(g_variant_get_uint64(value));
        }
        g_variant_unref(value);
    }
    g_variant_unref(result);

    if (max_priority <= 0) {
        error_message = "RTKit MaxRealtimePriority is not usable";
        return false;
    }
    return true;
}

bool rtkit_get_rttime_usec_max(GDBusConnection* connection, rlim_t& rttime_usec_max, std::string& error_message) {
    rttime_usec_max = 0;
    if (connection == nullptr) {
        error_message = "invalid D-Bus connection";
        return false;
    }

    GError* error = nullptr;
    GVariant* result = g_dbus_connection_call_sync(connection,
                                                   "org.freedesktop.RealtimeKit1",
                                                   "/org/freedesktop/RealtimeKit1",
                                                   "org.freedesktop.DBus.Properties",
                                                   "Get",
                                                   g_variant_new("(ss)", "org.freedesktop.RealtimeKit1", "RTTimeUSecMax"),
                                                   G_VARIANT_TYPE("(v)"),
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   1500,
                                                   nullptr,
                                                   &error);
    if (result == nullptr) {
        error_message = rtkit_query_error("RTKit RTTimeUSecMax query failed", error);
        if (error != nullptr) g_error_free(error);
        return false;
    }

    GVariant* value = nullptr;
    g_variant_get(result, "(v)", &value);
    gint64 service_value = 0;
    if (value != nullptr) {
        if (g_variant_type_equal(g_variant_get_type(value), G_VARIANT_TYPE_INT64)) {
            service_value = g_variant_get_int64(value);
        }
        g_variant_unref(value);
    }
    g_variant_unref(result);

    if (service_value <= 0) {
        error_message = "RTKit RTTimeUSecMax is not usable";
        return false;
    }

    const guint64 unsigned_value = static_cast<guint64>(service_value);
    if (unsigned_value > static_cast<guint64>(std::numeric_limits<rlim_t>::max())) {
        error_message = "RTKit RTTimeUSecMax exceeds the local rlimit range";
        return false;
    }

    rttime_usec_max = static_cast<rlim_t>(unsigned_value);
    return true;
}

bool prepare_rtkit_rttime_limit(rlim_t rttime_usec_max, std::string& error_message) {
    if (rttime_usec_max == 0 || rttime_usec_max == RLIM_INFINITY) {
        error_message = "RTKit RTTimeUSecMax is not usable";
        return false;
    }

    rlimit current{};
    if (getrlimit(RLIMIT_RTTIME, &current) != 0) {
        error_message = std::string("RTKit RLIMIT_RTTIME query failed: ") + std::strerror(errno);
        return false;
    }

    if (current.rlim_max == 0) {
        error_message = "RTKit RLIMIT_RTTIME hard limit is zero";
        return false;
    }

    rlimit adjusted = current;
    if (current.rlim_max == RLIM_INFINITY || current.rlim_max > rttime_usec_max) {
        adjusted.rlim_max = rttime_usec_max;
    }
    if (adjusted.rlim_cur == RLIM_INFINITY ||
        adjusted.rlim_cur == 0 ||
        adjusted.rlim_cur > adjusted.rlim_max) {
        adjusted.rlim_cur = adjusted.rlim_max;
    }

    if (adjusted.rlim_cur == current.rlim_cur &&
        adjusted.rlim_max == current.rlim_max) {
        return true;
    }

    if (setrlimit(RLIMIT_RTTIME, &adjusted) != 0) {
        error_message = std::string("RTKit RLIMIT_RTTIME setup failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

int base_scheduler_policy(int policy) {
#ifdef SCHED_RESET_ON_FORK
    return policy & ~SCHED_RESET_ON_FORK;
#else
    return policy;
#endif
}

const char* policy_display_name(int policy) {
    switch (base_scheduler_policy(policy)) {
        case SCHED_RR: return "SCHED_RR";
        case SCHED_FIFO: return "SCHED_FIFO";
        case SCHED_OTHER: return "TS";
#ifdef SCHED_BATCH
        case SCHED_BATCH: return "BATCH";
#endif
#ifdef SCHED_IDLE
        case SCHED_IDLE: return "IDLE";
#endif
        default: return "UNKNOWN";
    }
}

bool direct_make_thread_realtime(long tid, int requested_priority, std::string& error_message) {
    if (tid <= 0) {
        error_message = "invalid audio thread TID";
        return false;
    }
    sched_param param{};
    param.sched_priority = std::max(1, std::min(80, requested_priority));

#ifdef SCHED_RESET_ON_FORK
    if (sched_setscheduler(static_cast<pid_t>(tid),
                           SCHED_RR | SCHED_RESET_ON_FORK,
                           &param) == 0) {
        return true;
    }
    const int reset_err = errno;
    if (reset_err != EINVAL) {
        switch (reset_err) {
            case EPERM:
                error_message = "Direct scheduler request failed: permission required";
                break;
            case ESRCH:
                error_message = "Direct scheduler request failed: playback thread not found";
                break;
            default:
                error_message = std::string("Direct scheduler request failed: ") +
                                std::strerror(reset_err);
                break;
        }
        return false;
    }
#endif

    if (sched_setscheduler(static_cast<pid_t>(tid), SCHED_RR, &param) == 0) {
        return true;
    }
    const int err = errno;
    switch (err) {
        case EPERM:
            error_message = "Direct scheduler request failed: permission required";
            break;
        case ESRCH:
            error_message = "Direct scheduler request failed: playback thread not found";
            break;
        case EINVAL:
            error_message = "Direct scheduler request failed: invalid priority or scheduler";
            break;
        default:
            error_message = std::string("Direct scheduler request failed: ") + std::strerror(err);
            break;
    }
    return false;
}

bool demote_thread_from_realtime(long tid, std::string& error_message) {
    if (tid <= 0) {
        error_message = "invalid audio thread TID";
        return false;
    }

    sched_param param{};
    param.sched_priority = 0;
#ifdef SCHED_RESET_ON_FORK
    if (sched_setscheduler(static_cast<pid_t>(tid),
                           SCHED_OTHER | SCHED_RESET_ON_FORK,
                           &param) == 0) {
        return true;
    }
    const int reset_err = errno;
    if (reset_err != EINVAL) {
        error_message = reset_err == ESRCH
            ? std::string("playback thread not found")
            : std::string("scheduler demotion failed: ") + std::strerror(reset_err);
        return false;
    }
#endif

    if (sched_setscheduler(static_cast<pid_t>(tid), SCHED_OTHER, &param) == 0) {
        return true;
    }
    const int err = errno;
    error_message = err == ESRCH
        ? std::string("playback thread not found")
        : std::string("scheduler demotion failed: ") + std::strerror(err);
    return false;
}

bool rtkit_make_thread_realtime(long tid, int requested_priority, int& effective_priority, int& max_priority, std::string& error_message) {
    effective_priority = 0;
    max_priority = 0;
    if (tid <= 0) {
        error_message = "invalid audio thread TID";
        return false;
    }
    std::string bus_error;
    GDBusConnection* connection = rtkit_system_bus(bus_error);
    if (connection == nullptr) {
        error_message = bus_error;
        return false;
    }

    std::string max_error;
    if (!rtkit_get_max_realtime_priority(connection, max_priority, max_error)) {
        g_object_unref(connection);
        error_message = max_error.empty()
            ? std::string("RTKit MaxRealtimePriority query failed")
            : max_error;
        return false;
    }

    rlim_t rttime_usec_max = 0;
    std::string rttime_error;
    if (!rtkit_get_rttime_usec_max(connection, rttime_usec_max, rttime_error)) {
        g_object_unref(connection);
        error_message = rttime_error.empty()
            ? std::string("RTKit RTTimeUSecMax query failed")
            : rttime_error;
        return false;
    }
    if (!prepare_rtkit_rttime_limit(rttime_usec_max, rttime_error)) {
        g_object_unref(connection);
        error_message = rttime_error.empty()
            ? std::string("RTKit realtime limit setup failed")
            : rttime_error;
        return false;
    }

    effective_priority = std::max(1, std::min(requested_priority, max_priority));
    GError* error = nullptr;
    GVariant* result = g_dbus_connection_call_sync(connection,
                                                   "org.freedesktop.RealtimeKit1",
                                                   "/org/freedesktop/RealtimeKit1",
                                                   "org.freedesktop.RealtimeKit1",
                                                   "MakeThreadRealtime",
                                                   g_variant_new("(tu)", static_cast<guint64>(tid), static_cast<guint32>(effective_priority)),
                                                   nullptr,
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   1500,
                                                   nullptr,
                                                   &error);
    g_object_unref(connection);
    if (result == nullptr) {
        std::ostringstream ss;
        ss << "RTKit: " << concise_rtkit_error(error)
           << "; requested " << effective_priority
           << "; service max " << max_priority
           << "; TID " << tid;
        error_message = ss.str();
        if (error != nullptr) g_error_free(error);
        return false;
    }
    g_variant_unref(result);
    return true;
}

} // namespace

void PlaybackEngine::format_realtime_priority_status(RealtimePriorityStatusSnapshot& status) {
    if (status.tid <= 0) {
        status.text = status.enabled
            ? std::string("Realtime priority: inactive, playback stopped")
            : std::string("Realtime priority: disabled");
        return;
    }

    std::ostringstream ss;
    if (!status.enabled) {
        if (status.active) {
            ss << "Realtime priority: disable failed, still active, "
               << policy_display_name(status.scheduler_policy) << ' '
               << status.priority << ", TID " << status.tid;
            if (status.source == RealtimePrioritySource::Direct) {
                ss << ", via direct scheduling";
            } else if (status.source == RealtimePrioritySource::Rtkit) {
                ss << ", via RTKit";
            }
        } else if (status.scheduler_policy >= 0) {
            ss << "Realtime priority: disabled";
        } else {
            ss << "Realtime priority: disabled, scheduler status unavailable";
        }
    } else if (status.active) {
        ss << "Realtime priority: active, "
           << policy_display_name(status.scheduler_policy) << ' '
           << status.priority << ", TID " << status.tid;
        if (status.source == RealtimePrioritySource::Direct) {
            ss << ", via direct scheduling";
        } else if (status.source == RealtimePrioritySource::Rtkit) {
            ss << ", via RTKit";
        }
    } else if (status.scheduler_policy >= 0) {
        ss << "Realtime priority: not active, scheduler "
           << policy_display_name(status.scheduler_policy)
           << ", priority " << status.priority
           << ", TID " << status.tid;
    } else {
        ss << "Realtime priority: status unavailable, TID " << status.tid;
    }
    if (!status.error.empty()) {
        ss << '\n' << status.error;
    }
    status.text = ss.str();
}

RealtimePriorityStatusSnapshot PlaybackEngine::verified_realtime_priority_status(long tid) const {
    RealtimePriorityStatusSnapshot status;
    status.enabled = realtime_priority_enabled_.load(std::memory_order_relaxed);
    status.tid = tid;
    status.scheduler_policy = -1;

    if (tid <= 0) {
        format_realtime_priority_status(status);
        return status;
    }

    sched_param param{};
    const int policy = sched_getscheduler(static_cast<pid_t>(tid));
    if (policy < 0) {
        const int err = errno;
        status.error = err == ESRCH
            ? std::string("Playback thread not found")
            : std::string("Scheduler query failed: ") + std::strerror(err);
        format_realtime_priority_status(status);
        return status;
    }

    status.scheduler_policy = base_scheduler_policy(policy);
    if (sched_getparam(static_cast<pid_t>(tid), &param) != 0) {
        status.error = std::string("Scheduler priority query failed: ") + std::strerror(errno);
        format_realtime_priority_status(status);
        return status;
    }

    status.priority = param.sched_priority;
    status.active = status.scheduler_policy == SCHED_RR ||
                    status.scheduler_policy == SCHED_FIFO;
    format_realtime_priority_status(status);
    return status;
}

RealtimePriorityStatusSnapshot PlaybackEngine::realtime_priority_status_snapshot() const {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    return realtime_priority_status_;
}

std::string PlaybackEngine::request_realtime_priority_for_playback_thread() {
    std::lock_guard<std::mutex> transition_lock(realtime_transition_mutex_);
    if (!realtime_priority_enabled_.load(std::memory_order_relaxed)) {
        RealtimePriorityStatusSnapshot status;
        format_realtime_priority_status(status);
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = status;
        return realtime_priority_status_.text;
    }

    const long tid = playback_thread_tid_.load(std::memory_order_relaxed);
    if (tid <= 0) {
        RealtimePriorityStatusSnapshot status;
        status.enabled = true;
        format_realtime_priority_status(status);
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = status;
        return realtime_priority_status_.text;
    }

    RealtimePriorityStatusSnapshot current = verified_realtime_priority_status(tid);
    if (current.active) {
        {
            std::lock_guard<std::mutex> lock(runtime_mutex_);
            if (realtime_priority_status_.active && realtime_priority_status_.tid == tid) {
                current.source = realtime_priority_status_.source;
            }
        }
        current.error.clear();
        format_realtime_priority_status(current);
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = current;
        return realtime_priority_status_.text;
    }

    const int priority = realtime_priority_.load(std::memory_order_relaxed);
    std::string direct_error;
    if (direct_make_thread_realtime(tid, priority, direct_error)) {
        RealtimePriorityStatusSnapshot verified = verified_realtime_priority_status(tid);
        if (verified.active) {
            verified.source = RealtimePrioritySource::Direct;
            verified.error.clear();
        } else {
            verified.source = RealtimePrioritySource::None;
            verified.error = "Direct scheduler request returned but scheduler was not changed";
        }
        format_realtime_priority_status(verified);
        {
            std::lock_guard<std::mutex> lock(runtime_mutex_);
            realtime_priority_status_ = verified;
        }
        Logger::instance().info(verified.text);
        return verified.text;
    }

    int effective_priority = 0;
    int max_priority = 0;
    std::string rtkit_error;
    RealtimePriorityStatusSnapshot result;
    if (rtkit_make_thread_realtime(tid, priority, effective_priority, max_priority, rtkit_error)) {
        result = verified_realtime_priority_status(tid);
        if (result.active) {
            result.source = RealtimePrioritySource::Rtkit;
            result.error.clear();
        } else {
            std::ostringstream ss;
            ss << "RTKit request returned but scheduler was not changed; requested "
               << effective_priority << "; service max " << max_priority
               << "; TID " << tid;
            result.error = ss.str();
        }
    } else {
        result = current;
        result.source = RealtimePrioritySource::None;
        result.error = direct_error;
        if (!rtkit_error.empty()) {
            if (!result.error.empty()) {
                result.error += '\n';
            }
            result.error += rtkit_error;
        }
    }

    format_realtime_priority_status(result);
    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = result;
    }
    Logger::instance().info(result.text);
    return result.text;
}


std::string PlaybackEngine::disable_realtime_priority_for_playback_thread() {
    // Publish the desired OFF state before waiting for an older acquisition.
    // The transition lock then guarantees that this demotion is the last RT
    // operation applied for this user action.
    realtime_priority_enabled_.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> transition_lock(realtime_transition_mutex_);
    const long tid = playback_thread_tid_.load(std::memory_order_relaxed);
    if (tid <= 0) {
        RealtimePriorityStatusSnapshot status;
        status.enabled = false;
        format_realtime_priority_status(status);
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = status;
        return realtime_priority_status_.text;
    }

    RealtimePrioritySource previous_source = RealtimePrioritySource::None;
    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        if (realtime_priority_status_.tid == tid && realtime_priority_status_.active) {
            previous_source = realtime_priority_status_.source;
        }
    }

    RealtimePriorityStatusSnapshot before = verified_realtime_priority_status(tid);
    before.source = before.active ? previous_source : RealtimePrioritySource::None;
    if (before.active) {
        std::string demote_error;
        if (!demote_thread_from_realtime(tid, demote_error)) {
            RealtimePriorityStatusSnapshot failed = verified_realtime_priority_status(tid);
            failed.source = failed.active ? previous_source : RealtimePrioritySource::None;
            failed.error = "Realtime disable failed";
            if (!demote_error.empty()) {
                failed.error += ": " + demote_error;
            }
            format_realtime_priority_status(failed);
            {
                std::lock_guard<std::mutex> lock(runtime_mutex_);
                realtime_priority_status_ = failed;
            }
            Logger::instance().error(failed.text);
            return failed.text;
        }
    }

    RealtimePriorityStatusSnapshot result = verified_realtime_priority_status(tid);
    result.source = result.active ? previous_source : RealtimePrioritySource::None;
    if (result.active) {
        result.error = "Realtime disable request returned but scheduler is still realtime";
    } else if (result.scheduler_policy >= 0) {
        result.error.clear();
    }
    format_realtime_priority_status(result);
    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = result;
    }
    if (before.active || !result.error.empty()) {
        Logger::instance().info(result.text);
    }
    return result.text;
}

std::string PlaybackEngine::refresh_realtime_priority_status() {
    const long tid = playback_thread_tid_.load(std::memory_order_relaxed);
    RealtimePriorityStatusSnapshot refreshed = verified_realtime_priority_status(tid);

    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        if (refreshed.tid > 0 && realtime_priority_status_.tid == refreshed.tid) {
            if (refreshed.active && realtime_priority_status_.active) {
                refreshed.source = realtime_priority_status_.source;
            }
            const bool preserve_error = refreshed.error.empty() &&
                !realtime_priority_status_.error.empty() &&
                ((refreshed.enabled && !refreshed.active) ||
                 (!refreshed.enabled && refreshed.active));
            if (preserve_error) {
                refreshed.error = realtime_priority_status_.error;
            }
        }
        format_realtime_priority_status(refreshed);
        realtime_priority_status_ = refreshed;
        return realtime_priority_status_.text;
    }
}

std::string PlaybackEngine::active_output_report() const {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    return last_active_output_report_;
}

void PlaybackEngine::playback_loop(std::uint64_t transport_generation) {
    const long playback_tid = static_cast<long>(syscall(SYS_gettid));
    playback_thread_tid_.store(playback_tid, std::memory_order_relaxed);
    if (realtime_priority_enabled_.load(std::memory_order_relaxed)) {
        request_realtime_priority_for_playback_thread();
    } else {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = RealtimePriorityStatusSnapshot{};
    }
    try {
        const std::uint16_t ch = std::max<std::uint16_t>(1, format_.channels);
        const bool stereo_tonal_dsp_allowed = ch <= 2;
        const std::size_t block_samples = std::max<std::size_t>(
            ch, (static_cast<std::size_t>(4096) / ch) * ch);
        std::vector<PcmSample> block(block_samples);
        std::vector<PcmSample> output_block(block_samples);
        std::uint64_t played_samples_per_channel = initial_samples_per_channel_;
        std::size_t next_logical_boundary = logical_segment_offsets_.size();
        if (logical_segment_offsets_.size() >= 2) {
            next_logical_boundary = static_cast<std::size_t>(
                std::upper_bound(logical_segment_offsets_.begin(),
                                 logical_segment_offsets_.end(),
                                 played_samples_per_channel) -
                logical_segment_offsets_.begin());
        }
        DecoderSegmentPosition last_published_segment = decoder_->segment_position();
        DecoderRuntimeStateSnapshot decoder_runtime_state =
            decoder_->runtime_state_snapshot();
        std::uint64_t last_decoder_runtime_generation =
            decoder_runtime_state.generation;
        Pcm16QuantizationRuntimeKind last_published_pcm16_quantization_runtime_kind =
            pcm16_quantization_runtime_kind_.load(std::memory_order_acquire);
        std::uint32_t last_published_pcm16_quantization_stage_count =
            pcm16_quantization_stage_count_.load(std::memory_order_acquire);
        Pcm16DitherRuntimeKind last_published_pcm16_dither_runtime_kind =
            pcm16_dither_runtime_kind_.load(std::memory_order_acquire);
        if (pcm16_dither_ == nullptr) {
            throw std::runtime_error("PCM16 dither state is unavailable");
        }
        Pcm16Dither& pcm16_dither = *pcm16_dither_;
        const ToneFilterTarget initial_tone_target{
            stereo_tonal_dsp_allowed
                ? bass_db_.load(std::memory_order_relaxed)
                : 0,
            stereo_tonal_dsp_allowed
                ? treble_db_.load(std::memory_order_relaxed)
                : 0,
            bass_hz_.load(std::memory_order_relaxed),
            treble_hz_.load(std::memory_order_relaxed),
            stereo_tonal_dsp_allowed
                ? pre_eq_headroom_tenths_db_.load(std::memory_order_relaxed)
                : 0};
        ToneFilterCrossfade tone_filter(format_.sample_rate, initial_tone_target);
        while (!stop_requested_ && !decoder_->eof()) {
            wait_if_paused();
            if (stop_requested_) break;

            const std::size_t got = decoder_->read_samples(block.data(), block.size());
            bool processing_state_changed = false;
            const std::uint64_t decoder_runtime_generation =
                decoder_->runtime_state_generation();
            if (decoder_runtime_generation != last_decoder_runtime_generation) {
                decoder_runtime_state = decoder_->runtime_state_snapshot();
                last_decoder_runtime_generation = decoder_runtime_state.generation;
                resampler_runtime_kind_.store(
                    decoder_runtime_state.resampler_runtime_kind,
                    std::memory_order_release);
                decoded_pcm_sample_kind_.store(
                    decoder_runtime_state.decoded_pcm_sample_kind,
                    std::memory_order_release);
                decoded_pcm_significant_bits_.store(
                    decoder_runtime_state.decoded_pcm_significant_bits,
                    std::memory_order_release);
                encoded_bitrate_bps_.store(
                    decoder_runtime_state.encoded_bitrate_bps,
                    std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lock(runtime_mutex_);
                    source_codec_name_ = decoder_runtime_state.source_codec_name;
                    decoded_codec_name_ =
                        decoder_runtime_state.decoder_implementation_name;
                }
                processing_state_changed = true;
            }
            if (got > block.size()) {
                throw std::runtime_error("Decoder returned more PCM samples than requested");
            }
            if (got % ch != 0) {
                throw std::runtime_error("Decoder returned an incomplete PCM frame");
            }
            if (got == 0) break;

            const AudioFormat decoder_block_format = decoder_->format();
            if (decoder_block_format.sample_rate != format_.sample_rate ||
                decoder_block_format.channels != format_.channels ||
                decoder_block_format.bits_per_sample < 16 ||
                decoder_block_format.bits_per_sample > 32) {
                throw std::runtime_error(
                    "Decoder changed to an incompatible working PCM format");
            }
            const std::uint16_t block_processing_bits = std::max(
                decoder_block_format.bits_per_sample,
                output_format_.bits_per_sample);
            if (block_processing_bits != format_.bits_per_sample) {
                const int bit_delta =
                    static_cast<int>(block_processing_bits) -
                    static_cast<int>(format_.bits_per_sample);
                tone_filter.rescale_state(std::ldexp(1.0, bit_delta));
                format_.bits_per_sample = block_processing_bits;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    transport_working_format_ = format_;
                }
                processing_state_changed = true;
            }
            if (decoder_block_format.bits_per_sample < format_.bits_per_sample) {
                widen_pcm_block_exact(
                    block.data(),
                    got,
                    decoder_block_format.bits_per_sample,
                    format_.bits_per_sample);
            }

            const int current_soft_volume_percent =
                soft_volume_percent_.load(std::memory_order_relaxed);
            const int current_bass_db = stereo_tonal_dsp_allowed
                ? bass_db_.load(std::memory_order_relaxed)
                : 0;
            const int current_treble_db = stereo_tonal_dsp_allowed
                ? treble_db_.load(std::memory_order_relaxed)
                : 0;
            const int current_bass_hz = bass_hz_.load(std::memory_order_relaxed);
            const int current_treble_hz = treble_hz_.load(std::memory_order_relaxed);
            const int current_pre_eq_headroom_tenths_db = stereo_tonal_dsp_allowed
                ? pre_eq_headroom_tenths_db_.load(std::memory_order_relaxed)
                : 0;
            const ToneFilterTarget requested_tone_target{
                current_bass_db,
                current_treble_db,
                current_bass_hz,
                current_treble_hz,
                current_pre_eq_headroom_tenths_db};
            tone_filter.request(requested_tone_target);

            bool tone_processing_for_frame = tone_filter.requires_processing();
            const bool dsp_active =
                current_soft_volume_percent < 100 ||
                tone_processing_for_frame;
            const bool narrowing_required =
                output_format_.bits_per_sample < format_.bits_per_sample;
            const DecoderPcmSampleKind decoded_sample_kind =
                decoder_runtime_state.decoded_pcm_sample_kind;
            const bool decoded_s16 =
                decoded_sample_kind == DecoderPcmSampleKind::S16 ||
                decoded_sample_kind == DecoderPcmSampleKind::S16Planar;
            const bool exact_s16_repack =
                output_format_.bits_per_sample == 16 &&
                narrowing_required && !dsp_active &&
                decoder_runtime_state.pcm16_quantization_stage_count > 0 &&
                decoded_s16 &&
                decoder_runtime_state.resampler_runtime_kind ==
                    ResamplerRuntimeKind::NotUsed;
            const bool final_output_conversion =
                narrowing_required && !exact_s16_repack;
            const bool final_s16_quantization =
                output_format_.bits_per_sample == 16 &&
                (final_output_conversion || dsp_active);
            const Pcm16QuantizationRuntimeKind current_pcm16_quantization_runtime_kind =
                final_s16_quantization
                    ? (pcm16_quantization_mode_ == Pcm16QuantizationMode::Truncate
                           ? Pcm16QuantizationRuntimeKind::Truncate
                           : (pcm16_quantization_mode_ == Pcm16QuantizationMode::RoundHalfUp
                                  ? Pcm16QuantizationRuntimeKind::RoundHalfUp
                                  : Pcm16QuantizationRuntimeKind::RoundToNearestEven))
                    : decoder_runtime_state.pcm16_quantization_runtime_kind;
            const std::uint32_t current_pcm16_quantization_stage_count =
                decoder_runtime_state.pcm16_quantization_stage_count +
                (final_s16_quantization ? 1U : 0U);
            const Pcm16DitherRuntimeKind current_pcm16_dither_runtime_kind =
                pcmtp::pcm16_dither_runtime_kind(
                    pcm16_dither_mode_, final_s16_quantization);
            const bool final_s16_dither =
                current_pcm16_dither_runtime_kind !=
                Pcm16DitherRuntimeKind::NotUsed;
            const double user_volume =
                static_cast<double>(current_soft_volume_percent) / 100.0;
            const bool measure_level =
                level_meter_enabled_.load(std::memory_order_relaxed);
            const bool detect_clip =
                clip_detection_enabled_.load(std::memory_order_relaxed);
            const double full_scale =
                static_cast<double>(pcm_full_scale(format_.bits_per_sample));
            const double minimum_scale =
                static_cast<double>(pcm_minimum_sample(format_.bits_per_sample));

            float peak = 0.0f;
            std::uint32_t clipped_samples = 0;
            if (dsp_active) {
                for (std::size_t i = 0; i < got; ++i) {
                    const std::size_t channel_index = i % ch;
                    const bool left = (ch == 1) || (channel_index == 0);
                    double sample = static_cast<double>(block[i]);

                    if (tone_processing_for_frame) {
                        sample = tone_filter.process(sample, left);
                        if (channel_index + 1 == ch) {
                            tone_filter.advance_frame();
                            tone_processing_for_frame = tone_filter.requires_processing();
                        }
                    }
                    sample *= user_volume;

                    if (measure_level) {
                        const double meter_mag = full_scale > 0.0
                            ? std::fabs(sample) / full_scale
                            : 0.0;
                        if (meter_mag > static_cast<double>(peak)) {
                            peak = static_cast<float>(meter_mag);
                        }
                    }
                    if (detect_clip &&
                        (sample > full_scale || sample < minimum_scale)) {
                        ++clipped_samples;
                    }

                    const double pcm16_dither_code_units = final_s16_dither
                        ? pcm16_dither.next_code_units(channel_index)
                        : 0.0;
                    if (output_format_.bits_per_sample < format_.bits_per_sample) {
                        output_block[i] = quantize_working_sample_to_bits(
                            sample,
                            format_.bits_per_sample,
                            output_format_.bits_per_sample,
                            pcm16_quantization_mode_,
                            pcm16_dither_code_units);
                    } else {
                        block[i] = quantize_processed_sample(
                            sample,
                            format_.bits_per_sample,
                            pcm16_quantization_mode_,
                            pcm16_dither_code_units);
                    }
                }
            } else {
                if (measure_level) {
                    std::int64_t peak_magnitude = 0;
                    for (std::size_t i = 0; i < got; ++i) {
                        const std::int64_t sample = static_cast<std::int64_t>(block[i]);
                        const std::int64_t magnitude = sample < 0 ? -sample : sample;
                        if (magnitude > peak_magnitude) {
                            peak_magnitude = magnitude;
                        }
                    }
                    if (full_scale > 0.0) {
                        peak = static_cast<float>(
                            static_cast<double>(peak_magnitude) / full_scale);
                    }
                }
                if (narrowing_required) {
                    for (std::size_t i = 0; i < got; ++i) {
                        if (exact_s16_repack) {
                            output_block[i] = narrow_exact_working_sample(
                                block[i],
                                format_.bits_per_sample,
                                output_format_.bits_per_sample);
                        } else if (output_format_.bits_per_sample == 16) {
                            if (final_s16_dither) {
                                const double pcm16_dither_code_units =
                                    pcm16_dither.next_code_units(i % ch);
                                output_block[i] =
                                    quantize_integer_working_sample_to_pcm16_with_dither(
                                        block[i],
                                        format_.bits_per_sample,
                                        pcm16_quantization_mode_,
                                        pcm16_dither_code_units);
                            } else {
                                output_block[i] = quantize_integer_working_sample_to_pcm16(
                                    block[i],
                                    format_.bits_per_sample,
                                    pcm16_quantization_mode_);
                            }
                        } else {
                            output_block[i] = quantize_working_sample_to_bits(
                                static_cast<double>(block[i]),
                                format_.bits_per_sample,
                                output_format_.bits_per_sample,
                                pcm16_quantization_mode_);
                        }
                    }
                }
            }

            backend_->write_samples(
                narrowing_required ? output_block.data() : block.data(),
                got);

            // Publish visualization facts only after the PCM block has been
            // accepted by the output backend.  The GUI owns the display
            // ballistics; the playback thread only accumulates raw facts.
            if (measure_level) {
                publish_meter_peak(level_meter_peak_units_, peak);
            }
            if (detect_clip && clipped_samples > 0) {
                std::uint32_t published_clips =
                    clipped_samples_pending_.load(std::memory_order_relaxed);
                while (true) {
                    const std::uint32_t remaining =
                        std::numeric_limits<std::uint32_t>::max() - published_clips;
                    const std::uint32_t next = clipped_samples > remaining
                        ? std::numeric_limits<std::uint32_t>::max()
                        : published_clips + clipped_samples;
                    if (clipped_samples_pending_.compare_exchange_weak(
                            published_clips,
                            next,
                            std::memory_order_relaxed,
                            std::memory_order_relaxed)) {
                        break;
                    }
                }
            }

            played_samples_per_channel += got / ch;
            const DecoderSegmentPosition segment = decoder_->segment_position();
            publish_live_transport_position(played_samples_per_channel, segment);
            const bool decoder_segment_changed = segment.valid &&
                (!last_published_segment.valid ||
                 segment.index != last_published_segment.index);

            const bool pcm16_quantization_state_changed =
                current_pcm16_quantization_runtime_kind !=
                last_published_pcm16_quantization_runtime_kind;
            const bool pcm16_quantization_stage_count_changed =
                current_pcm16_quantization_stage_count !=
                last_published_pcm16_quantization_stage_count;
            if (pcm16_quantization_state_changed) {
                pcm16_quantization_runtime_kind_.store(
                    current_pcm16_quantization_runtime_kind,
                    std::memory_order_release);
                last_published_pcm16_quantization_runtime_kind =
                    current_pcm16_quantization_runtime_kind;
            }
            if (pcm16_quantization_stage_count_changed) {
                pcm16_quantization_stage_count_.store(
                    current_pcm16_quantization_stage_count,
                    std::memory_order_release);
                last_published_pcm16_quantization_stage_count =
                    current_pcm16_quantization_stage_count;
            }
            const bool pcm16_dither_state_changed =
                current_pcm16_dither_runtime_kind !=
                last_published_pcm16_dither_runtime_kind;
            if (pcm16_dither_state_changed) {
                pcm16_dither_runtime_kind_.store(
                    current_pcm16_dither_runtime_kind,
                    std::memory_order_release);
                last_published_pcm16_dither_runtime_kind =
                    current_pcm16_dither_runtime_kind;
            }
            processing_state_changed = processing_state_changed ||
                pcm16_quantization_state_changed ||
                pcm16_quantization_stage_count_changed ||
                pcm16_dither_state_changed;

            bool logical_segment_changed = false;
            while (next_logical_boundary + 1 < logical_segment_offsets_.size() &&
                   played_samples_per_channel >=
                       logical_segment_offsets_[next_logical_boundary]) {
                logical_segment_changed = true;
                ++next_logical_boundary;
            }
            const bool segment_changed =
                decoder_segment_changed || logical_segment_changed;
            if (segment_changed) {
                if (segment.valid) {
                    last_published_segment = segment;
                }
                emit_playback_event(PlaybackEventKind::SegmentChanged,
                                    transport_generation);
            } else if (segment.valid) {
                last_published_segment = segment;
            }
            if (processing_state_changed) {
                emit_playback_event(PlaybackEventKind::ProcessingStateChanged,
                                    transport_generation);
            }
        }

        if (backend_ && !stop_requested_) backend_->drain();
        bool naturally_finished = false;
        {
            const DecoderSegmentPosition segment = decoder_->segment_position();
            publish_live_transport_position(played_samples_per_channel, segment);
            std::lock_guard<std::mutex> lock(state_mutex_);
            snapshot_.current_samples_per_channel = played_samples_per_channel;
            snapshot_.segment_position_valid = segment.valid;
            snapshot_.segment_index = segment.index;
            snapshot_.segment_samples_per_channel = segment.samples_per_channel;
            snapshot_.finished = !stop_requested_ && last_error_.empty();
            snapshot_.playing = false;
            snapshot_.paused = false;
            if (!last_error_.empty()) {
                snapshot_.message = last_error_;
            } else if (!stop_requested_ &&
                       decoder_->total_samples_per_channel() == 0 &&
                       played_samples_per_channel <= initial_samples_per_channel_) {
                snapshot_.message = "Stream unavailable";
            } else {
                snapshot_.message = "Stopped";
            }
            naturally_finished = snapshot_.finished;
        }
        meter_transport_active_.store(false, std::memory_order_release);
        level_meter_peak_units_.store(kNoMeterMeasurement, std::memory_order_relaxed);
        if (naturally_finished) {
            emit_playback_event(PlaybackEventKind::Finished,
                                transport_generation);
        }
    } catch (const std::exception& ex) {
        set_error(ex.what());
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            snapshot_.playing = false;
            snapshot_.paused = false;
            snapshot_.message = last_error_;
        }
        emit_playback_event(PlaybackEventKind::Error,
                            transport_generation);
    } catch (...) {
        set_error("Unknown playback error");
        emit_playback_event(PlaybackEventKind::Error,
                            transport_generation);
    }

    long expected_tid = playback_tid;
    playback_thread_tid_.compare_exchange_strong(
        expected_tid, 0, std::memory_order_relaxed);
}

void PlaybackEngine::wait_if_paused() {
    if (!pause_requested_) return;
    std::unique_lock<std::mutex> lock(pause_mutex_);
    pause_cv_.wait(lock, [this]() { return !pause_requested_ || stop_requested_; });
}
void PlaybackEngine::set_error(const std::string& message) {
    meter_transport_active_.store(false, std::memory_order_release);
    level_meter_peak_units_.store(kNoMeterMeasurement, std::memory_order_relaxed);
    clipped_samples_pending_.store(0, std::memory_order_relaxed);
    { std::lock_guard<std::mutex> lock(state_mutex_); last_error_ = message; snapshot_.playing = false; snapshot_.paused = false; snapshot_.finished = false; snapshot_.message = message; }
    Logger::instance().error(message);
    stop_requested_ = true;
    pause_cv_.notify_all();
}
void PlaybackEngine::join_threads() { if (playback_thread_.joinable()) playback_thread_.join(); }

} // namespace pcmtp
