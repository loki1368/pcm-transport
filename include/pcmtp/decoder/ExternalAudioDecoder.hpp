// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <memory>
#include <string>

#include "pcmtp/decoder/IAudioDecoder.hpp"
#include "pcmtp/decoder/SampleBoundary.hpp"

namespace pcmtp {

class ProbeCancellation;

struct GenericTags {
    std::string title;
    std::string artist;
    std::string album;
    int track_number = 0;
};

struct ExternalAudioInfo {
    AudioFormat format{};
    AudioFormat source_format{};
    std::uint64_t total_samples_per_channel = 0;
    std::uint64_t source_total_samples_per_channel = 0;
    bool source_supports_trusted_decoder_eof = false;
    bool source_presentation_start_known = false;
    std::uint64_t source_presentation_start_sample = 0;
    GenericTags tags{};
    std::string codec_name;
    std::uint32_t bit_rate = 0;
    bool dsd_source = false;
    std::uint32_t dsd_sample_rate = 0;
    std::int64_t duration_ts = 0;
    std::string time_base;
    bool lossless = false;
    bool raw_aac = false;
    bool duration_reliable = true;
    bool live_format_probed = false;
    SampleExtentKind sample_extent_kind = SampleExtentKind::Unknown;
    SampleExtentSource sample_extent_source = SampleExtentSource::None;
    SampleExtentKind source_sample_extent_kind = SampleExtentKind::Unknown;
    SampleExtentSource source_sample_extent_source = SampleExtentSource::None;
    PresentationEndKind presentation_end_kind = PresentationEndKind::Unknown;
    std::string probe_backend;
};

class ExternalAudioDecoder final : public IAudioDecoder {
public:
    explicit ExternalAudioDecoder(std::uint32_t forced_output_sample_rate = 0,
                                  std::uint16_t forced_output_bits_per_sample = 0,
                                  const std::string& resample_quality = "maximum",
                                  const std::string& bitdepth_quality = "tpdf_hp");
    ~ExternalAudioDecoder() override;

    ExternalAudioDecoder(const ExternalAudioDecoder&) = delete;
    ExternalAudioDecoder& operator=(const ExternalAudioDecoder&) = delete;

    void open(const std::string& path) override;
    void open_at_sample(const std::string& path, std::uint64_t sample_index) override;
    void set_known_info(const ExternalAudioInfo& info);
    const AudioFormat& format() const override;
    std::size_t read_samples(PcmSample* destination, std::size_t max_samples) override;
    bool eof() const override;
    std::uint64_t total_samples_per_channel() const override;
    std::string source_path() const override;
    PresentationEndKind presentation_end_kind() const noexcept override;
    bool seek_to_sample(std::uint64_t sample_index) override;
    void request_abort() override;

    static bool looks_supported(const std::string& path);
    static ExternalAudioInfo probe_metadata(const std::string& path,
                                            std::uint32_t forced_output_sample_rate = 0,
                                            std::uint16_t forced_output_bits_per_sample = 0,
                                            ProbeCancellation* probe_cancellation = nullptr);
    static ExternalAudioInfo probe_info(const std::string& path,
                                        std::uint32_t forced_output_sample_rate = 0,
                                        std::uint16_t forced_output_bits_per_sample = 0);

private:
    struct Impl;

    static std::string to_lower_extension(const std::string& path);
    ExternalAudioInfo effective_probe_info(const std::string& path) const;
    void close_decoder();
    void open_decoder(std::uint64_t sample_index);

    std::uint32_t forced_output_sample_rate_ = 0;
    std::uint16_t forced_output_bits_per_sample_ = 0;
    std::string resample_quality_ = "maximum";
    std::string bitdepth_quality_ = "tpdf_hp";
    bool have_known_info_ = false;
    ExternalAudioInfo known_info_{};
    AudioFormat format_{};
    AudioFormat source_format_{};
    std::uint64_t total_samples_per_channel_ = 0;
    std::string path_;
    std::string codec_name_;
    PresentationEndKind presentation_end_kind_ = PresentationEndKind::Unknown;
    std::uint64_t presentation_timeline_origin_sample_ = 0;
    bool dsd_source_ = false;
    bool opened_ = false;
    bool reached_eof_ = false;
    std::uint64_t current_samples_per_channel_ = 0;
    std::unique_ptr<Impl> impl_;
};

} // namespace pcmtp
