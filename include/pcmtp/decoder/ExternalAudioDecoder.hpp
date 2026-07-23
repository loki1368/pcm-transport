#pragma once

#include <cstdio>
#include <string>
#include <sys/types.h>
#include <vector>

#include "pcmtp/decoder/IAudioDecoder.hpp"

namespace pcmtp {

class ManagedSubprocess;

struct GenericTags {
    std::string title;
    std::string artist;
    int track_number = 0;
};

struct ExternalAudioInfo {
    AudioFormat format{};
    AudioFormat source_format{};
    std::uint64_t total_samples_per_channel = 0;
    std::uint64_t source_total_samples_per_channel = 0;
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
};

class ExternalAudioDecoder final : public IAudioDecoder {
public:
    explicit ExternalAudioDecoder(std::uint32_t forced_output_sample_rate = 0, std::uint16_t forced_output_bits_per_sample = 0, const std::string& resample_quality = "maximum", const std::string& bitdepth_quality = "tpdf_hp");
    ~ExternalAudioDecoder() override;

    void open(const std::string& path) override;
    void open_at_sample(const std::string& path, std::uint64_t sample_index) override;
    void set_known_info(const ExternalAudioInfo& info);
    const AudioFormat& format() const override;
    std::size_t read_samples(PcmSample* destination, std::size_t max_samples) override;
    bool eof() const override;
    std::uint64_t total_samples_per_channel() const override;
    std::string source_path() const override;
    bool seek_to_sample(std::uint64_t sample_index) override;
    void interrupt() override;

    static bool looks_supported(const std::string& path);
    static bool is_stream_uri(const std::string& path);
    static ExternalAudioInfo probe_metadata(const std::string& path,
                                            std::uint32_t forced_output_sample_rate = 0,
                                            std::uint16_t forced_output_bits_per_sample = 0,
                                            bool background_priority = false,
                                            ManagedSubprocess* probe_process = nullptr);
    static ExternalAudioInfo probe_info(const std::string& path, std::uint32_t forced_output_sample_rate = 0, std::uint16_t forced_output_bits_per_sample = 0);
    static bool verify_stream_playback(const std::string& path,
                                       const ExternalAudioInfo& probed_info,
                                       std::uint32_t forced_output_sample_rate = 0,
                                       std::uint16_t forced_output_bits_per_sample = 0);
    static GenericTags read_tags(const std::string& path);

private:
    static std::string to_lower_extension(const std::string& path);
    static std::string shell_escape(const std::string& value);
    static std::string trim_copy(const std::string& value);
    std::string decode_command(double seconds) const;
    std::size_t bytes_per_sample() const;
    void close_pipe(bool log_stderr, const std::string& context);
    bool start_decode_pipe(double seconds, const std::string& context);
    ExternalAudioInfo effective_probe_info(const std::string& path) const;

    std::uint32_t forced_output_sample_rate_ = 0;
    std::uint16_t forced_output_bits_per_sample_ = 0;
    std::string resample_quality_ = "maximum";
    std::string bitdepth_quality_ = "tpdf_hp";
    FILE* pipe_ = nullptr;
    bool have_known_info_ = false;
    ExternalAudioInfo known_info_{};
    std::string stderr_path_;
    AudioFormat format_{};
    AudioFormat source_format_{};
    std::uint64_t total_samples_per_channel_ = 0;
    std::string path_;
    std::string codec_name_;
    bool dsd_source_ = false;
    bool opened_ = false;
    bool reached_eof_ = false;
    bool zero_read_logged_ = false;
    bool interrupt_requested_ = false;
    pid_t child_pid_ = 0;
    std::uint64_t current_samples_per_channel_ = 0;
    std::vector<unsigned char> raw_buffer_;
};

} // namespace pcmtp
