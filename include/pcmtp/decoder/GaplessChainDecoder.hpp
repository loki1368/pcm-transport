#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pcmtp/core/PcmTypes.hpp"
#include "pcmtp/decoder/ExternalAudioDecoder.hpp"
#include "pcmtp/decoder/IAudioDecoder.hpp"

namespace pcmtp {

struct GaplessTrackSpec {
    std::string path;
    AudioFormat format{};
    std::uint64_t start_sample = 0;
    std::uint64_t end_sample = 0;
    bool range_limited = false;
    bool native_flac = false;
    std::uint32_t forced_output_sample_rate = 0;
    std::uint16_t forced_output_bits_per_sample = 0;
    std::string resample_quality = "maximum";
    std::string bitdepth_quality = "tpdf_hp";
    ExternalAudioInfo known_external_info{};
    bool has_known_external_info = false;
};

class GaplessChainDecoder final : public IAudioDecoder {
public:
    explicit GaplessChainDecoder(std::vector<GaplessTrackSpec> tracks, std::uint64_t first_track_offset = 0);
    ~GaplessChainDecoder() override;

    void open(const std::string& path) override;
    void open_at_sample(const std::string& path, std::uint64_t sample_index) override;
    const AudioFormat& format() const override;
    std::size_t read_samples(PcmSample* destination, std::size_t max_samples) override;
    bool eof() const override;
    std::uint64_t total_samples_per_channel() const override;
    std::string source_path() const override;
    bool seek_to_sample(std::uint64_t sample_index) override;

private:
    struct PreparedNext {
        std::size_t index = 0;
        std::unique_ptr<IAudioDecoder> decoder;
        PcmBuffer prebuffer;
        std::size_t prebuffer_offset = 0;
        bool ready = false;
        bool failed = false;
    };

    std::unique_ptr<IAudioDecoder> create_decoder_for_track(const GaplessTrackSpec& spec) const;
    void open_current_decoder(std::uint64_t offset);
    void close_prepare_thread();
    void maybe_prepare_next();
    void prepare_next_worker(std::size_t index);
    enum class SwitchResult { Switched, NoNext, NotReady };

    SwitchResult switch_to_next_track();
    std::size_t fill_keepalive_silence(PcmSample* destination, std::size_t max_samples);
    std::uint64_t track_length(std::size_t index) const;
    std::uint64_t prepare_threshold_frames(std::size_t index) const;
    std::size_t prebuffer_samples(std::size_t index) const;
    std::size_t keepalive_silence_samples() const;

    std::vector<GaplessTrackSpec> tracks_;
    std::size_t current_index_ = 0;
    std::uint64_t current_track_position_ = 0;
    std::uint64_t first_track_offset_ = 0;
    std::uint64_t total_samples_per_channel_ = 0;
    AudioFormat format_{};
    std::unique_ptr<IAudioDecoder> current_decoder_;
    bool opened_ = false;
    bool reached_eof_ = false;

    mutable std::mutex prepare_mutex_;
    std::thread prepare_thread_;
    PreparedNext prepared_;
    bool preparing_ = false;
    bool keepalive_logged_ = false;
};

} // namespace pcmtp
