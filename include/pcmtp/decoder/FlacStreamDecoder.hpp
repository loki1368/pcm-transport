#pragma once

#include <FLAC/stream_decoder.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "pcmtp/decoder/IAudioDecoder.hpp"

namespace pcmtp {

struct FlacTags {
    std::string title;
    std::string artist;
    int track_number = 0;
};

class FlacStreamDecoder final : public IAudioDecoder {
public:
    FlacStreamDecoder() = default;
    ~FlacStreamDecoder() override;

    void open(const std::string& path) override;
    const AudioFormat& format() const override;
    std::size_t read_samples(PcmSample* destination, std::size_t max_samples) override;
    bool eof() const override;
    std::uint64_t total_samples_per_channel() const override;
    std::string source_path() const override;
    bool seek_to_sample(std::uint64_t sample_index) override;

    static FlacTags read_tags(const std::string& path);


private:
    static ::FLAC__StreamDecoderWriteStatus write_callback(const ::FLAC__StreamDecoder* decoder,
                                                           const ::FLAC__Frame* frame,
                                                           const ::FLAC__int32* const buffer[],
                                                           void* client_data);
    static void metadata_callback(const ::FLAC__StreamDecoder* decoder,
                                  const ::FLAC__StreamMetadata* metadata,
                                  void* client_data);
    static void error_callback(const ::FLAC__StreamDecoder* decoder,
                               ::FLAC__StreamDecoderErrorStatus status,
                               void* client_data);

    void handle_write(const ::FLAC__Frame* frame, const ::FLAC__int32* const buffer[]);
    void handle_metadata(const ::FLAC__StreamMetadata* metadata);
    void handle_error(::FLAC__StreamDecoderErrorStatus status);
    void fill_queue_if_needed();
    void reset_decoder();

    AudioFormat format_{};
    bool opened_ = false;
    bool reached_eof_ = false;
    bool metadata_seen_ = false;
    std::deque<PcmBuffer> blocks_;
    std::size_t block_offset_ = 0;
    std::size_t queued_samples_ = 0;
    mutable std::mutex mutex_;
    std::string path_;
    std::uint64_t total_samples_per_channel_ = 0;
    ::FLAC__StreamDecoder* decoder_ = nullptr;
};

} // namespace pcmtp
