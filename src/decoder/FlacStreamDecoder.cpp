#include "pcmtp/decoder/FlacStreamDecoder.hpp"

#include <FLAC/metadata.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "pcmtp/util/Logger.hpp"

namespace pcmtp {
namespace {

std::string to_upper_ascii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return value;
}


} // namespace

FlacStreamDecoder::~FlacStreamDecoder() {
    reset_decoder();
}

void FlacStreamDecoder::reset_decoder() {
    if (decoder_ != nullptr) {
        FLAC__stream_decoder_finish(decoder_);
        FLAC__stream_decoder_delete(decoder_);
        decoder_ = nullptr;
    }
    opened_ = false;
}

void FlacStreamDecoder::open(const std::string& path) {
    reset_decoder();

    path_ = path;
    blocks_.clear();
    block_offset_ = 0;
    queued_samples_ = 0;
    reached_eof_ = false;
    metadata_seen_ = false;
    total_samples_per_channel_ = 0;
    format_ = AudioFormat{};

    decoder_ = FLAC__stream_decoder_new();
    if (decoder_ == nullptr) {
        throw std::runtime_error("Failed to allocate FLAC decoder");
    }


    const auto init_status = FLAC__stream_decoder_init_file(
        decoder_,
        path.c_str(),
        &FlacStreamDecoder::write_callback,
        &FlacStreamDecoder::metadata_callback,
        &FlacStreamDecoder::error_callback,
        this);
    if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        reset_decoder();
        throw std::runtime_error("FLAC init failed");
    }

    if (FLAC__stream_decoder_process_until_end_of_metadata(decoder_) == 0) {
        reset_decoder();
        throw std::runtime_error("Failed to read FLAC metadata");
    }

    if (!metadata_seen_) {
        reset_decoder();
        throw std::runtime_error("FLAC metadata not available");
    }

    opened_ = true;
    Logger::instance().info("Opened FLAC stream: " + path);
}

const AudioFormat& FlacStreamDecoder::format() const {
    return format_;
}

std::size_t FlacStreamDecoder::read_samples(PcmSample* destination, std::size_t max_samples) {
    if (!opened_) {
        throw std::runtime_error("Decoder not opened");
    }

    fill_queue_if_needed();

    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t copied = 0;
    while (copied < max_samples && !blocks_.empty()) {
        PcmBuffer& front = blocks_.front();
        if (block_offset_ >= front.size()) {
            blocks_.pop_front();
            block_offset_ = 0;
            continue;
        }
        const std::size_t available = front.size() - block_offset_;
        const std::size_t needed = max_samples - copied;
        const std::size_t take = std::min(available, needed);
        std::copy(front.data() + block_offset_, front.data() + block_offset_ + take, destination + copied);
        copied += take;
        block_offset_ += take;
        queued_samples_ -= take;
        if (block_offset_ >= front.size()) {
            blocks_.pop_front();
            block_offset_ = 0;
        }
    }
    return copied;
}

bool FlacStreamDecoder::eof() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reached_eof_ && queued_samples_ == 0;
}

std::uint64_t FlacStreamDecoder::total_samples_per_channel() const {
    return total_samples_per_channel_;
}

std::string FlacStreamDecoder::source_path() const {
    return path_;
}

bool FlacStreamDecoder::seek_to_sample(std::uint64_t sample_index) {
    if (!opened_ || decoder_ == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        blocks_.clear();
        block_offset_ = 0;
        queued_samples_ = 0;
        reached_eof_ = false;
    }
    return FLAC__stream_decoder_seek_absolute(decoder_, sample_index) != 0;
}

FlacFileProbe FlacStreamDecoder::probe_file(const std::string& path) {
    FlacFileProbe result;
    FLAC__Metadata_Chain* chain = FLAC__metadata_chain_new();
    if (chain == nullptr) {
        return result;
    }

    if (!FLAC__metadata_chain_read(chain, path.c_str())) {
        FLAC__metadata_chain_delete(chain);
        return result;
    }

    FLAC__Metadata_Iterator* iterator = FLAC__metadata_iterator_new();
    if (iterator == nullptr) {
        FLAC__metadata_chain_delete(chain);
        return result;
    }

    FLAC__metadata_iterator_init(iterator, chain);
    do {
        FLAC__StreamMetadata* block = FLAC__metadata_iterator_get_block(iterator);
        if (block == nullptr) {
            continue;
        }
        if (block->type == FLAC__METADATA_TYPE_STREAMINFO) {
            result.format.sample_rate = block->data.stream_info.sample_rate;
            result.format.channels = block->data.stream_info.channels;
            result.format.bits_per_sample = static_cast<std::uint16_t>(block->data.stream_info.bits_per_sample);
            result.total_samples_per_channel = block->data.stream_info.total_samples;
        } else if (block->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
            const FLAC__StreamMetadata_VorbisComment& vc = block->data.vorbis_comment;
            for (unsigned i = 0; i < vc.num_comments; ++i) {
                const FLAC__StreamMetadata_VorbisComment_Entry& entry = vc.comments[i];
                const std::string text(reinterpret_cast<const char*>(entry.entry), entry.length);
                const std::size_t eq = text.find('=');
                if (eq == std::string::npos) {
                    continue;
                }
                const std::string key = to_upper_ascii(text.substr(0, eq));
                const std::string value = text.substr(eq + 1);
                if (key == "TITLE") {
                    result.tags.title = value;
                } else if (key == "ARTIST") {
                    result.tags.artist = value;
                } else if (key == "TRACKNUMBER") {
                    try {
                        result.tags.track_number = std::stoi(value);
                    } catch (...) {
                    }
                }
            }
        }
    } while (FLAC__metadata_iterator_next(iterator));

    FLAC__metadata_iterator_delete(iterator);
    FLAC__metadata_chain_delete(chain);
    result.valid = result.format.sample_rate > 0 && result.format.channels > 0;
    return result;
}

FlacTags FlacStreamDecoder::read_tags(const std::string& path) {
    return probe_file(path).tags;
}

::FLAC__StreamDecoderWriteStatus FlacStreamDecoder::write_callback(const ::FLAC__StreamDecoder*,
                                                                   const ::FLAC__Frame* frame,
                                                                   const ::FLAC__int32* const buffer[],
                                                                   void* client_data) {
    auto* self = static_cast<FlacStreamDecoder*>(client_data);
    self->handle_write(frame, buffer);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void FlacStreamDecoder::metadata_callback(const ::FLAC__StreamDecoder*,
                                          const ::FLAC__StreamMetadata* metadata,
                                          void* client_data) {
    auto* self = static_cast<FlacStreamDecoder*>(client_data);
    self->handle_metadata(metadata);
}

void FlacStreamDecoder::error_callback(const ::FLAC__StreamDecoder*,
                                       ::FLAC__StreamDecoderErrorStatus status,
                                       void* client_data) {
    auto* self = static_cast<FlacStreamDecoder*>(client_data);
    self->handle_error(status);
}

void FlacStreamDecoder::handle_write(const ::FLAC__Frame* frame, const ::FLAC__int32* const buffer[]) {
    const unsigned blocksize = frame->header.blocksize;
    const unsigned channels = format_.channels;
    PcmBuffer block;
    block.resize(static_cast<std::size_t>(blocksize) * channels);
    if (block.empty()) {
        return;
    }
    std::size_t out = 0;
    for (unsigned i = 0; i < blocksize; ++i) {
        for (unsigned ch = 0; ch < channels; ++ch) {
            block[out++] = static_cast<PcmSample>(buffer[ch][i]);
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    queued_samples_ += block.size();
    blocks_.push_back(std::move(block));
}

void FlacStreamDecoder::handle_metadata(const ::FLAC__StreamMetadata* metadata) {
    if (metadata->type != FLAC__METADATA_TYPE_STREAMINFO) {
        return;
    }
    format_.sample_rate = metadata->data.stream_info.sample_rate;
    format_.channels = metadata->data.stream_info.channels;
    format_.bits_per_sample = static_cast<std::uint16_t>(metadata->data.stream_info.bits_per_sample);
    total_samples_per_channel_ = metadata->data.stream_info.total_samples;
    metadata_seen_ = true;
}

void FlacStreamDecoder::handle_error(::FLAC__StreamDecoderErrorStatus status) {
    const std::string message = FLAC__StreamDecoderErrorStatusString[status];
    Logger::instance().error("FLAC decoder error: " + message);
    std::cerr << "FLAC decoder error: " << message << '\n';
}

void FlacStreamDecoder::fill_queue_if_needed() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reached_eof_ || queued_samples_ >= 8192) {
            return;
        }
    }

    if (Logger::instance().debug_enabled()) {
        Logger::instance().debug("FLAC queue low, decoding next frame");
    }
    if (decoder_ == nullptr || FLAC__stream_decoder_process_single(decoder_) == 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        reached_eof_ = true;
        Logger::instance().debug("FLAC process_single returned false, marking EOF");
        return;
    }

    const auto state = FLAC__stream_decoder_get_state(decoder_);
    if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
        std::lock_guard<std::mutex> lock(mutex_);
        reached_eof_ = true;
        Logger::instance().debug("FLAC decoder reached end of stream");
    }
}

} // namespace pcmtp
