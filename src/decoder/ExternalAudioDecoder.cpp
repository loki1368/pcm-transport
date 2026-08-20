// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/decoder/ExternalAudioDecoder.hpp"
#include "pcmtp/decoder/ContainerBoundaryVerifier.hpp"
#include "pcmtp/util/MediaUri.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavformat/version.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
#include <libswresample/version.h>
}

#include "pcmtp/util/Logger.hpp"
#include "pcmtp/util/ProbeCancellation.hpp"
#include "pcmtp/util/TextEncoding.hpp"

namespace pcmtp {
namespace {

#define PCMTP_FFMPEG_HAS_MODERN_CHANNEL_LAYOUT \
    (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100) && \
     LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100) && \
     LIBSWRESAMPLE_VERSION_INT >= AV_VERSION_INT(4, 5, 100))

#define PCMTP_FFMPEG_HAS_MOV_AAC_INDEX_API \
    (LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(58, 78, 100))

constexpr auto kProbeTimeout = std::chrono::seconds(30);
constexpr auto kVorbisOriginProbeTimeout = std::chrono::seconds(2);
constexpr std::size_t kVorbisOriginMaximumDemuxPackets = 64;
constexpr std::size_t kVorbisOriginMaximumAudioPackets = 16;
constexpr std::uint64_t kVorbisOriginMaximumPayloadBytes = 4u * 1024u * 1024u;
#if PCMTP_FFMPEG_HAS_MOV_AAC_INDEX_API
constexpr auto kMovAacBoundaryProbeTimeout = std::chrono::seconds(2);
constexpr std::size_t kMovAacMaximumHeadPackets = 16;
constexpr std::int64_t kMovAacMaximumLeadingSkipSamples = 16384;
constexpr std::size_t kMovAacMaximumTailPackets = 64;
constexpr std::uint64_t kMovAacMaximumTailPayloadBytes = 4u * 1024u * 1024u;
constexpr int kMovAacMaximumIndexEntries = 2000000;
#endif
constexpr std::size_t kOggMaximumPageSize = 27u + 255u + (255u * 255u);
constexpr double kApeSeekPrerollSeconds = 3.0;
constexpr double kRawAacSeekPrerollSeconds = 1.5;
constexpr double kAlacSeekPrerollSeconds = 1.0;
constexpr double kDefaultSeekPrerollSeconds = 0.5;


std::string trim_ffmpeg_log_line(std::string message) {
    while (!message.empty() &&
           (message.back() == '\n' || message.back() == '\r' ||
            std::isspace(static_cast<unsigned char>(message.back())) != 0)) {
        message.pop_back();
    }
    std::size_t start = 0;
    while (start < message.size() &&
           std::isspace(static_cast<unsigned char>(message[start])) != 0) {
        ++start;
    }
    return message.substr(start);
}

class FfmpegLogDispatcher {
public:
    static FfmpegLogDispatcher& instance() {
        static FfmpegLogDispatcher dispatcher;
        return dispatcher;
    }

    void enqueue(int level, std::string message) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return;
            }
            if (queue_.size() >= kMaximumQueuedMessages) {
                queue_.pop_front();
                ++dropped_messages_;
            }
            queue_.push_back(Message{level, std::move(message)});
        }
        condition_.notify_one();
    }

private:
    struct Message {
        int level = AV_LOG_WARNING;
        std::string text;
    };

    static constexpr std::size_t kMaximumQueuedMessages = 256;

    FfmpegLogDispatcher()
        : worker_(&FfmpegLogDispatcher::run, this) {}

    ~FfmpegLogDispatcher() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    FfmpegLogDispatcher(const FfmpegLogDispatcher&) = delete;
    FfmpegLogDispatcher& operator=(const FfmpegLogDispatcher&) = delete;

    void run() {
        std::string last_message;
        std::chrono::steady_clock::time_point last_emitted{};
        for (;;) {
            Message message;
            std::size_t dropped = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this]() {
                    return stopping_ || !queue_.empty();
                });
                if (queue_.empty()) {
                    if (stopping_) {
                        break;
                    }
                    continue;
                }
                message = std::move(queue_.front());
                queue_.pop_front();
                dropped = dropped_messages_;
                dropped_messages_ = 0;
            }

            if (dropped > 0) {
                Logger::instance().warning(
                    "FFmpeg API log queue dropped " +
                    std::to_string(dropped) + " messages");
            }

            const auto now = std::chrono::steady_clock::now();
            if (message.text == last_message &&
                now - last_emitted < std::chrono::seconds(1)) {
                continue;
            }
            last_message = message.text;
            last_emitted = now;

            if (message.level <= AV_LOG_ERROR) {
                Logger::instance().error("FFmpeg API: " + message.text);
            } else {
                Logger::instance().warning("FFmpeg API: " + message.text);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Message> queue_;
    std::thread worker_;
    bool stopping_ = false;
    std::size_t dropped_messages_ = 0;
};

void ffmpeg_log_callback(void* context, int level, const char* format, va_list arguments) {
    if (format == nullptr || level > AV_LOG_WARNING) {
        return;
    }

    char buffer[1024]{};
    int print_prefix = 1;
    va_list copy;
    va_copy(copy, arguments);
    av_log_format_line2(context, level, format, copy, buffer, sizeof(buffer), &print_prefix);
    va_end(copy);

    std::string message = trim_ffmpeg_log_line(buffer);
    if (!message.empty()) {
        FfmpegLogDispatcher::instance().enqueue(level, std::move(message));
    }
}

void initialize_ffmpeg_logging() {
    static std::once_flag once;
    std::call_once(once, []() {
        (void)Logger::instance();
        (void)FfmpegLogDispatcher::instance();
        av_log_set_level(AV_LOG_WARNING);
        av_log_set_callback(ffmpeg_log_callback);
    });
}

std::string av_error_string(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return std::string(buffer);
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim_value_copy(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

bool starts_with(const std::string& text, const char* prefix) {
    const std::size_t length = std::char_traits<char>::length(prefix);
    return text.size() >= length && text.compare(0, length, prefix) == 0;
}

bool format_name_has_token(const std::string& names, const std::string& token) {
    std::size_t start = 0;
    while (start <= names.size()) {
        const std::size_t comma = names.find(',', start);
        const std::size_t length = comma == std::string::npos
            ? names.size() - start
            : comma - start;
        if (length == token.size() && names.compare(start, length, token) == 0) {
            return true;
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return false;
}

bool is_mov_family_format_name(const std::string& names) {
    return format_name_has_token(names, "mov") ||
           format_name_has_token(names, "mp4") ||
           format_name_has_token(names, "m4a") ||
           format_name_has_token(names, "3gp") ||
           format_name_has_token(names, "3g2") ||
           format_name_has_token(names, "mj2");
}

bool is_dsd_codec_name(const std::string& codec_name) {
    return starts_with(codec_name, "dsd_") || codec_name == "dst";
}

std::uint32_t wavpack_read_le32(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

bool wavpack_stream_is_lossless(const std::string& path) {
    constexpr std::size_t kWavPackHeaderSize = 32;
    constexpr std::uint32_t kWavPackHybridFlag = 0x00000008U;

    std::ifstream input(path.c_str(), std::ios::binary);
    std::array<unsigned char, kWavPackHeaderSize> header{};
    if (!input.read(reinterpret_cast<char*>(header.data()),
                    static_cast<std::streamsize>(header.size()))) {
        return false;
    }
    if (header[0] != 'w' || header[1] != 'v' ||
        header[2] != 'p' || header[3] != 'k') {
        return false;
    }
    const std::uint32_t flags = wavpack_read_le32(header.data() + 24U);
    return (flags & kWavPackHybridFlag) == 0U;
}

bool codec_is_lossless(const std::string& codec_name, const std::string& path) {
    const std::string codec = lower_copy(codec_name);
    if (is_dsd_codec_name(codec)) {
        return true;
    }
    if (codec == "wavpack") {
        // The WavPack codec supports both pure lossless and hybrid/lossy streams;
        // the first block header identifies the mode of this concrete file.
        return wavpack_stream_is_lossless(path);
    }

    const AVCodecDescriptor* descriptor = avcodec_descriptor_get_by_name(codec.c_str());
    if (descriptor == nullptr) {
        return false;
    }
    const bool supports_lossless = (descriptor->props & AV_CODEC_PROP_LOSSLESS) != 0;
    const bool supports_lossy = (descriptor->props & AV_CODEC_PROP_LOSSY) != 0;
    return supports_lossless && !supports_lossy;
}

std::uint16_t normalize_bits(int bits, AVSampleFormat sample_format, const std::string& codec_name) {
    if (bits == 16 || bits == 24 || bits == 32) {
        return static_cast<std::uint16_t>(bits);
    }
    const int sample_bits = av_get_bytes_per_sample(sample_format) * 8;
    if (codec_name == "alac" && sample_bits == 32) {
        return 32;
    }
    if (sample_bits == 16 || sample_bits == 24) {
        return static_cast<std::uint16_t>(sample_bits);
    }
    if (sample_bits == 32 && starts_with(codec_name, "pcm_")) {
        return 32;
    }
    return 16;
}

int stream_channels(const AVCodecParameters* parameters) {
#if PCMTP_FFMPEG_HAS_MODERN_CHANNEL_LAYOUT
    return parameters != nullptr ? parameters->ch_layout.nb_channels : 0;
#else
    return parameters != nullptr ? parameters->channels : 0;
#endif
}

int frame_channels(const AVFrame* frame, const AVCodecContext* codec_context) {
#if PCMTP_FFMPEG_HAS_MODERN_CHANNEL_LAYOUT
    if (frame != nullptr && frame->ch_layout.nb_channels > 0) {
        return frame->ch_layout.nb_channels;
    }
    return codec_context != nullptr ? codec_context->ch_layout.nb_channels : 0;
#else
    if (frame != nullptr && frame->channels > 0) {
        return frame->channels;
    }
    return codec_context != nullptr ? codec_context->channels : 0;
#endif
}

std::string rational_string(AVRational value) {
    if (value.num <= 0 || value.den <= 0) {
        return std::string();
    }
    return std::to_string(value.num) + "/" + std::to_string(value.den);
}

std::string dictionary_value(AVDictionary* dictionary, const char* first, const char* second = nullptr) {
    if (dictionary == nullptr) {
        return std::string();
    }
    AVDictionaryEntry* entry = av_dict_get(dictionary, first, nullptr, 0);
    if ((entry == nullptr || entry->value == nullptr || entry->value[0] == '\0') && second != nullptr) {
        entry = av_dict_get(dictionary, second, nullptr, 0);
    }
    return entry != nullptr && entry->value != nullptr ? std::string(entry->value) : std::string();
}

GenericTags extract_tags(AVDictionary* format_metadata, AVDictionary* stream_metadata) {
    GenericTags tags;
    auto select = [&](const char* first, const char* second = nullptr) {
        std::string value = dictionary_value(format_metadata, first, second);
        if (trim_value_copy(value).empty()) {
            value = dictionary_value(stream_metadata, first, second);
        }
        return value;
    };

    const std::string title = select("title");
    const std::string artist = select("artist");
    const std::string album = select("album");
    const std::string track = select("track", "tracknumber");
    if (!title.empty()) {
        tags.title = pcmtp::text::normalize_metadata_value(title);
    }
    if (!artist.empty()) {
        tags.artist = pcmtp::text::normalize_metadata_value(artist);
    }
    if (!album.empty()) {
        tags.album = pcmtp::text::normalize_metadata_value(album);
    }
    if (!track.empty()) {
        try {
            tags.track_number = std::stoi(track);
        } catch (...) {}
    }
    return tags;
}

struct InterruptState {
    ProbeCancellation* cancellation = nullptr;
    std::uint64_t cancellation_token = 0;
    std::atomic<bool>* abort_requested = nullptr;
    std::chrono::steady_clock::time_point deadline{};
    bool use_deadline = false;
    std::chrono::steady_clock::time_point bounded_deadline{};
    bool use_bounded_deadline = false;
    bool global_timed_out() const {
        return use_deadline && std::chrono::steady_clock::now() >= deadline;
    }

    bool bounded_timed_out() const {
        return use_bounded_deadline &&
               std::chrono::steady_clock::now() >= bounded_deadline;
    }

    bool timed_out() const {
        return global_timed_out() || bounded_timed_out();
    }

    bool cancelled() const {
        return (abort_requested != nullptr && abort_requested->load(std::memory_order_acquire)) ||
               (cancellation != nullptr && cancellation->cancelled_since(cancellation_token));
    }

    bool interrupted() const {
        return cancelled() || timed_out();
    }
};

class ScopedInterruptBudget {
public:
    ScopedInterruptBudget(InterruptState* interrupt,
                          std::chrono::steady_clock::duration duration)
        : interrupt_(interrupt) {
        if (interrupt_ == nullptr) {
            return;
        }
        previous_deadline_ = interrupt_->bounded_deadline;
        previous_enabled_ = interrupt_->use_bounded_deadline;
        const std::chrono::steady_clock::time_point requested_deadline =
            std::chrono::steady_clock::now() + duration;
        if (!previous_enabled_ || requested_deadline < previous_deadline_) {
            interrupt_->bounded_deadline = requested_deadline;
        }
        interrupt_->use_bounded_deadline = true;
    }

    ~ScopedInterruptBudget() {
        if (interrupt_ != nullptr) {
            interrupt_->bounded_deadline = previous_deadline_;
            interrupt_->use_bounded_deadline = previous_enabled_;
        }
    }

    ScopedInterruptBudget(const ScopedInterruptBudget&) = delete;
    ScopedInterruptBudget& operator=(const ScopedInterruptBudget&) = delete;

private:
    InterruptState* interrupt_ = nullptr;
    std::chrono::steady_clock::time_point previous_deadline_{};
    bool previous_enabled_ = false;
};

int interrupt_callback(void* opaque) {
    const InterruptState* state = static_cast<const InterruptState*>(opaque);
    return state != nullptr && state->interrupted() ? 1 : 0;
}

void throw_if_global_probe_interrupted(const InterruptState* interrupt) {
    if (interrupt != nullptr &&
        (interrupt->cancelled() || interrupt->global_timed_out())) {
        throw std::runtime_error(interrupt->global_timed_out()
            ? "metadata probe timed out"
            : "metadata probe cancelled");
    }
}

struct AvFormatInputCloser {
    void operator()(AVFormatContext* context) const {
        if (context == nullptr) {
            return;
        }
        AVFormatContext* closing = context;
        avformat_close_input(&closing);
    }
};

using ScopedAvFormatInput = std::unique_ptr<AVFormatContext, AvFormatInputCloser>;

ScopedAvFormatInput open_bounded_input_context(const std::string& path,
                                               InterruptState* interrupt) {
    initialize_ffmpeg_logging();
    AVFormatContext* context = avformat_alloc_context();
    if (context == nullptr) {
        return ScopedAvFormatInput{};
    }
    if (interrupt != nullptr) {
        context->interrupt_callback.callback = interrupt_callback;
        context->interrupt_callback.opaque = interrupt;
    }

    AVDictionary* options = nullptr;
    if (is_remote_media_uri(path)) {
        av_dict_set(&options, "reconnect", "1", 0);
        av_dict_set(&options, "reconnect_at_eof", "1", 0);
        av_dict_set(&options, "reconnect_streamed", "1", 0);
        av_dict_set(&options, "reconnect_delay_max", "30", 0);
        av_dict_set(&options, "timeout", "8000000", 0);
        av_dict_set(&options, "rw_timeout", "8000000", 0);
        av_dict_set(&options, "user_agent", "pcm-transport/0.9", 0);
        if (is_hls_media_uri(path)) {
            av_dict_set(&options, "live_start_index", "-3", 0);
            av_dict_set(&options, "hls_allow_cache", "1", 0);
        }
    }
    const int result = avformat_open_input(&context, path.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (result < 0) {
        if (context != nullptr) {
            avformat_close_input(&context);
        }
        throw_if_global_probe_interrupted(interrupt);
        return ScopedAvFormatInput{};
    }
    return ScopedAvFormatInput(context);
}

AVFormatContext* open_input_context(const std::string& path,
                                    InterruptState* interrupt,
                                    bool find_stream_info) {
    initialize_ffmpeg_logging();
    AVFormatContext* context = avformat_alloc_context();
    if (context == nullptr) {
        throw std::runtime_error("Cannot allocate FFmpeg input context");
    }
    const std::size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && lower_copy(path.substr(dot)) == ".aac") {
        context->flags |= AVFMT_FLAG_GENPTS;
    }
    if (interrupt != nullptr) {
        context->interrupt_callback.callback = interrupt_callback;
        context->interrupt_callback.opaque = interrupt;
    }
    AVDictionary* options = nullptr;
    if (is_remote_media_uri(path)) {
        av_dict_set(&options, "reconnect", "1", 0);
        av_dict_set(&options, "reconnect_at_eof", "1", 0);
        av_dict_set(&options, "reconnect_streamed", "1", 0);
        av_dict_set(&options, "reconnect_delay_max", "30", 0);
        av_dict_set(&options, "timeout", "8000000", 0);
        av_dict_set(&options, "rw_timeout", "8000000", 0);
        av_dict_set(&options, "user_agent", "pcm-transport/0.9", 0);
        if (is_hls_media_uri(path)) {
            av_dict_set(&options, "live_start_index", "-3", 0);
            av_dict_set(&options, "hls_allow_cache", "1", 0);
        }
    }
    int result = avformat_open_input(&context, path.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (result < 0) {
        if (context != nullptr) {
            avformat_close_input(&context);
        }
        if (interrupt != nullptr && interrupt->interrupted()) {
            throw std::runtime_error(interrupt->timed_out()
                ? "FFmpeg input operation timed out"
                : "FFmpeg input operation cancelled");
        }
        throw std::runtime_error("Cannot open media input: " + av_error_string(result));
    }
    if (find_stream_info) {
        result = avformat_find_stream_info(context, nullptr);
        if (result < 0) {
            avformat_close_input(&context);
            if (interrupt != nullptr && interrupt->interrupted()) {
                throw std::runtime_error(interrupt->timed_out()
                    ? "metadata probe timed out"
                    : "metadata probe cancelled");
            }
            throw std::runtime_error("Cannot read media stream information: " + av_error_string(result));
        }
    }
    return context;
}

int first_audio_stream(AVFormatContext* context) {
    if (context == nullptr) {
        throw std::runtime_error("No media input context");
    }
    for (unsigned int index = 0; index < context->nb_streams; ++index) {
        AVStream* stream = context->streams[index];
        if (stream != nullptr && stream->codecpar != nullptr &&
            stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            return static_cast<int>(index);
        }
    }
    throw std::runtime_error("No audio stream found");
}

std::uint16_t read_le16(const unsigned char* p) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t read_le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

struct MovAacBoundaryEvidence {
    bool exact_presentation = false;
    std::uint64_t presentation_samples = 0;
    ExactPresentationDrainPolicy drain_policy =
        ExactPresentationDrainPolicy::ExactRangeRequired;
    bool terminal_discard = false;
};

#if PCMTP_FFMPEG_HAS_MOV_AAC_INDEX_API

struct PacketSkipSamples {
    bool present = false;
    std::uint32_t leading = 0;
    std::uint32_t trailing = 0;
    std::uint8_t leading_reason = 0;
    std::uint8_t trailing_reason = 0;
};

PacketSkipSamples packet_skip_samples(const AVPacket* packet) {
    PacketSkipSamples result;
    if (packet == nullptr) {
        return result;
    }
    std::size_t side_data_size = 0;
    const std::uint8_t* side_data = av_packet_get_side_data(
        packet, AV_PKT_DATA_SKIP_SAMPLES, &side_data_size);
    if (side_data == nullptr || side_data_size < 10) {
        return result;
    }
    result.present = true;
    result.leading = read_le32(side_data);
    result.trailing = read_le32(side_data + 4);
    result.leading_reason = side_data[8];
    result.trailing_reason = side_data[9];
    return result;
}

struct ItunSmpbInfo {
    bool present = false;
    bool valid = false;
    std::uint64_t priming = 0;
    std::uint64_t remainder = 0;
    std::uint64_t samples = 0;
};

bool parse_hex_u64_token(const char** cursor, std::uint64_t* value) {
    if (cursor == nullptr || *cursor == nullptr || value == nullptr) {
        return false;
    }
    const char* current = *cursor;
    while (*current != '\0' &&
           std::isspace(static_cast<unsigned char>(*current)) != 0) {
        ++current;
    }
    if (*current == '\0') {
        return false;
    }

    std::uint64_t parsed = 0;
    std::size_t digits = 0;
    while (*current != '\0' &&
           std::isspace(static_cast<unsigned char>(*current)) == 0) {
        unsigned int nibble = 0;
        const unsigned char ch = static_cast<unsigned char>(*current);
        if (ch >= '0' && ch <= '9') {
            nibble = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
            nibble = ch - 'a' + 10U;
        } else if (ch >= 'A' && ch <= 'F') {
            nibble = ch - 'A' + 10U;
        } else {
            return false;
        }
        if (digits >= 16 ||
            parsed > (std::numeric_limits<std::uint64_t>::max() - nibble) / 16U) {
            return false;
        }
        parsed = parsed * 16U + nibble;
        ++digits;
        ++current;
    }
    if (digits == 0) {
        return false;
    }
    *cursor = current;
    *value = parsed;
    return true;
}

ItunSmpbInfo itun_smpb_info(const AVDictionary* metadata) {
    ItunSmpbInfo result;
    if (metadata == nullptr) {
        return result;
    }
    const AVDictionaryEntry* entry = av_dict_get(
        metadata, "iTunSMPB", nullptr, 0);
    if (entry == nullptr || entry->value == nullptr) {
        return result;
    }
    result.present = true;

    const char* cursor = entry->value;
    std::uint64_t reserved = 0;
    if (!parse_hex_u64_token(&cursor, &reserved) ||
        !parse_hex_u64_token(&cursor, &result.priming) ||
        !parse_hex_u64_token(&cursor, &result.remainder) ||
        !parse_hex_u64_token(&cursor, &result.samples)) {
        return result;
    }
    // Match FFmpeg's conservative priming sanity range. The remaining fields
    // are corroborated against packet timing before they become evidence.
    result.valid = result.priming > 0 &&
                   result.priming <
                       static_cast<std::uint64_t>(kMovAacMaximumLeadingSkipSamples) &&
                   result.samples > 0 &&
                   result.samples <=
                       static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max());
    return result;
}

ItunSmpbInfo combined_itun_smpb_info(const AVDictionary* container_metadata,
                                      const AVDictionary* stream_metadata) {
    const ItunSmpbInfo container = itun_smpb_info(container_metadata);
    const ItunSmpbInfo stream = itun_smpb_info(stream_metadata);
    if (!container.present) {
        return stream;
    }
    if (!stream.present) {
        return container;
    }

    ItunSmpbInfo combined = container;
    combined.valid = container.valid && stream.valid &&
                     container.priming == stream.priming &&
                     container.remainder == stream.remainder &&
                     container.samples == stream.samples;
    return combined;
}

bool checked_add_int64(std::int64_t left,
                       std::int64_t right,
                       std::int64_t* result) {
    if (result == nullptr ||
        (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        return false;
    }
    *result = left + right;
    return true;
}

struct MovAacHeadEvidence {
    bool valid = false;
    std::int64_t nominal_frame_samples = 0;
    std::int64_t first_packet_timestamp = AV_NOPTS_VALUE;
};

MovAacHeadEvidence probe_mov_aac_head_evidence(
    const std::string& path,
    int stream_index,
    const AVStream* reference_stream,
    AVCodecParameters* reference_parameters,
    const ItunSmpbInfo& smpb,
    InterruptState* interrupt) {
    MovAacHeadEvidence evidence;
    if (path.empty() || stream_index < 0 || reference_stream == nullptr ||
        reference_parameters == nullptr) {
        return evidence;
    }

    ScopedAvFormatInput head_context = open_bounded_input_context(path, interrupt);
    if (!head_context || head_context->iformat == nullptr ||
        head_context->iformat->name == nullptr ||
        !is_mov_family_format_name(lower_copy(head_context->iformat->name)) ||
        static_cast<unsigned int>(stream_index) >= head_context->nb_streams) {
        return evidence;
    }

    AVStream* head_stream = head_context->streams[stream_index];
    AVCodecParameters* head_parameters =
        head_stream != nullptr ? head_stream->codecpar : nullptr;
    if (head_stream == nullptr || head_parameters == nullptr ||
        head_parameters->codec_type != AVMEDIA_TYPE_AUDIO ||
        head_parameters->codec_id != AV_CODEC_ID_AAC ||
        head_stream->id != reference_stream->id ||
        head_parameters->sample_rate != reference_parameters->sample_rate ||
        head_stream->time_base.num != reference_stream->time_base.num ||
        head_stream->time_base.den != reference_stream->time_base.den) {
        return evidence;
    }

    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        return evidence;
    }

    for (std::size_t packet_count = 0;
         packet_count < kMovAacMaximumHeadPackets;
         ++packet_count) {
        const int read_result = av_read_frame(head_context.get(), packet);
        if (read_result < 0) {
            av_packet_free(&packet);
            throw_if_global_probe_interrupted(interrupt);
            return evidence;
        }
        if (packet->stream_index != stream_index) {
            av_packet_unref(packet);
            continue;
        }
        if ((packet->flags & AV_PKT_FLAG_CORRUPT) != 0 ||
            packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE ||
            packet->dts != packet->pts || packet->duration <= 0 ||
            packet->size <= 0) {
            av_packet_free(&packet);
            return evidence;
        }

        const PacketSkipSamples skip = packet_skip_samples(packet);
        if (!skip.present || skip.leading == 0 || skip.trailing != 0 ||
            skip.leading_reason != 0) {
            av_packet_free(&packet);
            return evidence;
        }
        std::int64_t presentation_start = 0;
        if (!checked_add_int64(packet->pts,
                               static_cast<std::int64_t>(skip.leading),
                               &presentation_start) ||
            presentation_start != 0) {
            av_packet_free(&packet);
            return evidence;
        }

        const int nominal_frame_samples =
            av_get_audio_frame_duration2(reference_parameters, packet->size);
        if (nominal_frame_samples <= 0 || nominal_frame_samples > 4096 ||
            packet->duration != nominal_frame_samples) {
            av_packet_free(&packet);
            return evidence;
        }
        if (reference_parameters->frame_size > 0 &&
            reference_parameters->frame_size != nominal_frame_samples) {
            av_packet_free(&packet);
            return evidence;
        }
        if (reference_parameters->initial_padding > 0 &&
            static_cast<std::uint32_t>(reference_parameters->initial_padding) !=
                skip.leading) {
            av_packet_free(&packet);
            return evidence;
        }
        if (smpb.valid && smpb.priming != skip.leading) {
            av_packet_free(&packet);
            return evidence;
        }

        evidence.valid = true;
        evidence.nominal_frame_samples = nominal_frame_samples;
        evidence.first_packet_timestamp = packet->pts;
        av_packet_free(&packet);
        return evidence;
    }

    av_packet_free(&packet);
    throw_if_global_probe_interrupted(interrupt);
    return evidence;
}

bool mov_aac_index_is_contiguous(
    AVStream* stream,
    std::int64_t nominal_frame_samples,
    std::int64_t first_packet_timestamp,
    std::int64_t last_packet_timestamp,
    const InterruptState* interrupt) {
    if (stream == nullptr || nominal_frame_samples <= 0 ||
        first_packet_timestamp == AV_NOPTS_VALUE ||
        last_packet_timestamp == AV_NOPTS_VALUE ||
        last_packet_timestamp < first_packet_timestamp) {
        return false;
    }

    // AVIndexEntry timestamps are an implementation-facing seek index and MOV
    // edit-list handling can shift their absolute origin relative to demuxed
    // packet PTS. Do not assign either timeline a physical/presentation meaning
    // that FFmpeg's public API does not promise. For this conservative proof we
    // require only an offset-independent invariant: the index must contain one
    // dense entry per nominal AAC access unit across the packet timestamp span,
    // and adjacent index entries must have the same nominal AAC cadence.
    std::int64_t packet_span_signed = 0;
    if (first_packet_timestamp < 0 &&
        last_packet_timestamp >
            std::numeric_limits<std::int64_t>::max() + first_packet_timestamp) {
        return false;
    }
    packet_span_signed = last_packet_timestamp - first_packet_timestamp;
    const std::uint64_t packet_span =
        static_cast<std::uint64_t>(packet_span_signed);
    const std::uint64_t nominal = static_cast<std::uint64_t>(nominal_frame_samples);
    if (packet_span % nominal != 0) {
        return false;
    }
    const std::uint64_t expected_entry_count = packet_span / nominal + 1;
    if (expected_entry_count == 0 ||
        expected_entry_count > static_cast<std::uint64_t>(kMovAacMaximumIndexEntries)) {
        return false;
    }

    const int entry_count = avformat_index_get_entries_count(stream);
    if (entry_count <= 0 || entry_count > kMovAacMaximumIndexEntries) {
        return false;
    }
    if (static_cast<std::uint64_t>(entry_count) != expected_entry_count) {
        return false;
    }

    const AVIndexEntry* first_entry = avformat_index_get_entry(stream, 0);
    if (first_entry == nullptr) {
        return false;
    }
    std::int64_t previous_timestamp = first_entry->timestamp;
    for (int index = 1; index < entry_count; ++index) {
        if ((index & 4095) == 0 && interrupt != nullptr && interrupt->interrupted()) {
            return false;
        }
        const AVIndexEntry* entry = avformat_index_get_entry(stream, index);
        std::int64_t expected_timestamp = 0;
        if (entry == nullptr ||
            !checked_add_int64(previous_timestamp,
                               nominal_frame_samples,
                               &expected_timestamp) ||
            entry->timestamp != expected_timestamp) {
            return false;
        }
        previous_timestamp = entry->timestamp;
    }
    return true;
}

bool mov_aac_auxiliary_streams_supported(const AVFormatContext* context,
                                         int audio_stream_index) {
    if (context == nullptr || audio_stream_index < 0 ||
        static_cast<unsigned int>(audio_stream_index) >= context->nb_streams) {
        return false;
    }
    for (unsigned int index = 0; index < context->nb_streams; ++index) {
        if (static_cast<int>(index) == audio_stream_index) {
            continue;
        }
        const AVStream* stream = context->streams[index];
        if (stream == nullptr || stream->codecpar == nullptr ||
            stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
            (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
            return false;
        }
    }
    return true;
}

// Prove only AAC-LC/MOV presentations whose demuxed presentation timeline is
// corroborated by bounded packet evidence at both ends of the stream. The
// leading packet trim anchors the MOV presentation origin, while the terminal
// nominal AAC access unit must reach a clean demux EOF and contain the MOV
// presentation end. AVPacket::duration is only a bounded consistency check: it
// must cover that presentation end and cannot extend beyond the nominal access
// unit. The AVStream index is checked only for offset-independent dense AAC
// cadence, never treated as a physical or presentation timestamp origin.
// Optional iTunSMPB/trailing-padding data must agree with that packet proof.
// Explicit terminal packet discard remains a separate decoder-EOF proof.
// Any missing or conflicting evidence deliberately falls back to ET.
MovAacBoundaryEvidence probe_mov_aac_boundary_evidence(
    const std::string& path,
    AVFormatContext* context,
    int stream_index,
    InterruptState* interrupt) {
    MovAacBoundaryEvidence evidence;
    if (context == nullptr || stream_index < 0 ||
        static_cast<unsigned int>(stream_index) >= context->nb_streams) {
        return evidence;
    }
    if (!mov_aac_auxiliary_streams_supported(context, stream_index)) {
        return evidence;
    }

    AVStream* stream = context->streams[stream_index];
    AVCodecParameters* parameters = stream != nullptr ? stream->codecpar : nullptr;
    if (stream == nullptr || parameters == nullptr ||
        parameters->codec_type != AVMEDIA_TYPE_AUDIO ||
        parameters->codec_id != AV_CODEC_ID_AAC ||
        parameters->profile != AV_PROFILE_AAC_LOW ||
        parameters->sample_rate <= 0 ||
        parameters->initial_padding < 0 || parameters->trailing_padding < 0 ||
        stream->time_base.num != 1 ||
        stream->time_base.den != parameters->sample_rate ||
        stream->start_time != 0 || stream->duration <= 0) {
        return evidence;
    }

    const std::uint64_t stream_presentation_samples =
        static_cast<std::uint64_t>(stream->duration);
    if (context->duration > 0) {
        const std::int64_t container_samples = av_rescale_q_rnd(
            context->duration,
            AVRational{1, AV_TIME_BASE},
            AVRational{1, parameters->sample_rate},
            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        if (container_samples <= 0) {
            return evidence;
        }
        const std::int64_t duration_difference =
            container_samples > stream->duration
                ? container_samples - stream->duration
                : stream->duration - container_samples;
        if (duration_difference > 1) {
            return evidence;
        }
    }

    const ItunSmpbInfo smpb =
        combined_itun_smpb_info(context->metadata, stream->metadata);
    if (smpb.present && !smpb.valid) {
        return evidence;
    }

    // FFmpeg may attach AV_PKT_DATA_SKIP_SAMPLES when packet iteration starts
    // from a fresh MOV demux state. Seeking the already-open main context can
    // preserve the negative packet timestamp while dropping that leading-trim
    // side data. Read the first packets from a fresh bounded context without an
    // application-level seek; keep the main context and its sample-table index
    // for tail/index proof.
    MovAacHeadEvidence head;
    {
        ScopedInterruptBudget head_budget(interrupt, kMovAacBoundaryProbeTimeout);
        head = probe_mov_aac_head_evidence(
            path, stream_index, stream, parameters, smpb, interrupt);
    }
    if (!head.valid) {
        return evidence;
    }
    const std::int64_t nominal_frame_samples = head.nominal_frame_samples;
    const std::int64_t first_packet_timestamp = head.first_packet_timestamp;

    // Preserve the existing bounded tail/index budget independently of the
    // additional bounded head open so accepted files do not lose verification
    // time merely because the leading packet is now read from a fresh context.
    ScopedInterruptBudget bounded_budget(interrupt, kMovAacBoundaryProbeTimeout);

    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        return evidence;
    }

    const std::int64_t tail_window = nominal_frame_samples * 16;
    const std::int64_t seek_target = stream->duration > tail_window
        ? stream->duration - tail_window
        : 0;
    const int seek_result = avformat_seek_file(
        context,
        stream_index,
        std::numeric_limits<std::int64_t>::min(),
        seek_target,
        stream->duration,
        AVSEEK_FLAG_BACKWARD);
    if (seek_result < 0) {
        av_packet_free(&packet);
        throw_if_global_probe_interrupted(interrupt);
        return evidence;
    }
    avformat_flush(context);

    bool found_tail_audio_packet = false;
    bool saw_clean_eof = false;
    bool checked_seek_locality = false;
    std::int64_t last_pts = AV_NOPTS_VALUE;
    std::int64_t last_duration = 0;
    std::int64_t last_frame_samples = 0;
    PacketSkipSamples last_skip;
    std::uint64_t payload_bytes = 0;
    for (std::size_t packet_count = 0;
         packet_count < kMovAacMaximumTailPackets;
         ++packet_count) {
        const int read_result = av_read_frame(context, packet);
        if (read_result == AVERROR_EOF) {
            saw_clean_eof = true;
            break;
        }
        if (read_result < 0) {
            av_packet_free(&packet);
            throw_if_global_probe_interrupted(interrupt);
            return evidence;
        }
        if (packet->stream_index != stream_index) {
            av_packet_unref(packet);
            continue;
        }
        if (packet->size > 0) {
            const std::uint64_t packet_size = static_cast<std::uint64_t>(packet->size);
            if (packet_size > kMovAacMaximumTailPayloadBytes - payload_bytes) {
                av_packet_free(&packet);
                return evidence;
            }
            payload_bytes += packet_size;
        }
        if ((packet->flags & AV_PKT_FLAG_CORRUPT) != 0 ||
            packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE ||
            packet->dts != packet->pts || packet->duration <= 0 ||
            packet->size <= 0) {
            av_packet_free(&packet);
            return evidence;
        }
        if (!checked_seek_locality) {
            const std::int64_t locality_floor = seek_target > nominal_frame_samples * 4
                ? seek_target - nominal_frame_samples * 4
                : std::numeric_limits<std::int64_t>::min();
            if (packet->pts < locality_floor) {
                av_packet_free(&packet);
                return evidence;
            }
            checked_seek_locality = true;
        }
        const int frame_samples = av_get_audio_frame_duration2(parameters, packet->size);
        if (frame_samples <= 0 || frame_samples != nominal_frame_samples ||
            packet->duration > nominal_frame_samples) {
            av_packet_free(&packet);
            return evidence;
        }
        found_tail_audio_packet = true;
        last_pts = packet->pts;
        last_duration = packet->duration;
        last_frame_samples = frame_samples;
        last_skip = packet_skip_samples(packet);
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    throw_if_global_probe_interrupted(interrupt);

    if (!saw_clean_eof || !found_tail_audio_packet ||
        last_pts == AV_NOPTS_VALUE || last_duration <= 0 ||
        last_frame_samples <= 0) {
        return evidence;
    }


    std::int64_t packet_duration_end = 0;
    std::int64_t physical_end = 0;
    if (!checked_add_int64(last_pts, last_duration, &packet_duration_end) ||
        !checked_add_int64(last_pts, last_frame_samples, &physical_end) ||
        physical_end <= 0) {
        return evidence;
    }

    // AVPacket::duration is demuxer timing metadata, not the authoritative
    // presentation boundary. Supported FFmpeg/MOV combinations may expose a
    // duration between the presentation remainder and the nominal AAC frame.
    // Require only that it is self-consistent with the independently proven
    // boundary: it must cover the MOV presentation end and, as already checked
    // above, cannot exceed the nominal access-unit duration.
    if (physical_end < stream->duration) {
        return evidence;
    }
    const std::uint64_t trailing_padding = static_cast<std::uint64_t>(
        physical_end - stream->duration);
    if (trailing_padding >= static_cast<std::uint64_t>(nominal_frame_samples)) {
        return evidence;
    }
    if (packet_duration_end < stream->duration || packet_duration_end > physical_end) {
        return evidence;
    }

    if (!mov_aac_index_is_contiguous(
            stream,
            nominal_frame_samples,
            first_packet_timestamp,
            last_pts,
            interrupt)) {
        throw_if_global_probe_interrupted(interrupt);
        return evidence;
    }

    // The exact span is the MOV presentation span established by the bounded
    // packet timeline above. Optional gapless fields are corroborating facts,
    // never substitutes for that packet proof.
    if (smpb.valid &&
        (smpb.samples != stream_presentation_samples ||
         smpb.remainder != trailing_padding)) {
        return evidence;
    }
    if (parameters->trailing_padding > 0 &&
        static_cast<std::uint64_t>(parameters->trailing_padding) != trailing_padding) {
        return evidence;
    }
    if (last_skip.present && last_skip.trailing > 0 &&
        (last_skip.trailing_reason != 0 ||
         static_cast<std::uint64_t>(last_skip.trailing) != trailing_padding)) {
        return evidence;
    }

    evidence.exact_presentation = true;
    evidence.presentation_samples = stream_presentation_samples;
    evidence.drain_policy = trailing_padding == 0
        ? ExactPresentationDrainPolicy::DecoderEofMatchesPresentation
        : ExactPresentationDrainPolicy::ExactRangeRequired;
    evidence.terminal_discard = trailing_padding > 0 &&
        last_skip.present && last_skip.trailing_reason == 0 &&
        static_cast<std::uint64_t>(last_skip.trailing) == trailing_padding;
    return evidence;
}

#else

MovAacBoundaryEvidence probe_mov_aac_boundary_evidence(
    const std::string&,
    AVFormatContext*,
    int,
    InterruptState*) {
    // FFmpeg 4.4 predates the public AVStream index-entry accessor API used
    // by the strict MOV/AAC proof. Keep normal AAC/MOV playback available and
    // conservatively leave the presentation boundary unknown on this baseline.
    return MovAacBoundaryEvidence{};
}

#endif

bool read_exact(std::ifstream& input, unsigned char* data, std::size_t size) {
    input.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(input.gcount()) == size;
}

struct OggPageInfo {
    std::uint8_t flags = 0;
    std::uint32_t serial = 0;
    std::uint32_t sequence = 0;
    std::uint64_t granule_position = 0;
    std::uint32_t checksum = 0;
    std::size_t segment_count = 0;
    std::size_t completed_packet_count = 0;
    std::uint8_t last_lacing_value = 0;
    std::size_t header_size = 0;
    std::size_t page_size = 0;
};

std::uint64_t read_le64(const unsigned char* data) {
    std::uint64_t value = 0;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(data[shift / 8]) << shift;
    }
    return value;
}

bool parse_ogg_page(const unsigned char* data,
                    std::size_t available,
                    OggPageInfo* page) {
    if (data == nullptr || page == nullptr || available < 27 ||
        std::memcmp(data, "OggS", 4) != 0 || data[4] != 0) {
        return false;
    }
    const std::size_t segment_count = data[26];
    const std::size_t header_size = 27u + segment_count;
    if (header_size > available) {
        return false;
    }
    std::size_t payload_size = 0;
    std::size_t completed_packet_count = 0;
    for (std::size_t index = 0; index < segment_count; ++index) {
        const std::uint8_t lacing_value = data[27u + index];
        payload_size += lacing_value;
        if (lacing_value < 255U) {
            ++completed_packet_count;
        }
    }
    if (payload_size > available - header_size) {
        return false;
    }
    page->flags = data[5];
    page->granule_position = read_le64(data + 6);
    page->serial = read_le32(data + 14);
    page->sequence = read_le32(data + 18);
    page->checksum = read_le32(data + 22);
    page->segment_count = segment_count;
    page->completed_packet_count = completed_packet_count;
    page->last_lacing_value = segment_count == 0
        ? 0
        : data[27u + segment_count - 1u];
    page->header_size = header_size;
    page->page_size = header_size + payload_size;
    return page->page_size >= 27 && page->page_size <= kOggMaximumPageSize;
}

std::uint32_t ogg_page_crc(const unsigned char* data, std::size_t size) {
    static const std::array<std::uint32_t, 256> table = []() {
        std::array<std::uint32_t, 256> result{};
        for (std::size_t index = 0; index < result.size(); ++index) {
            std::uint32_t value = static_cast<std::uint32_t>(index) << 24U;
            for (unsigned int bit = 0; bit < 8; ++bit) {
                value = (value & 0x80000000U) != 0U
                    ? (value << 1U) ^ 0x04c11db7U
                    : (value << 1U);
            }
            result[index] = value;
        }
        return result;
    }();

    std::uint32_t crc = 0;
    for (std::size_t index = 0; index < size; ++index) {
        const unsigned char byte = index >= 22 && index < 26 ? 0 : data[index];
        const std::uint8_t table_index = static_cast<std::uint8_t>(
            ((crc >> 24U) & 0xffU) ^ byte);
        crc = (crc << 8U) ^ table[table_index];
    }
    return crc;
}

bool ogg_page_crc_matches(const unsigned char* data,
                          const OggPageInfo& page) {
    return page.page_size > 0 &&
           ogg_page_crc(data, page.page_size) == page.checksum;
}

bool ogg_identification_payload_matches(const unsigned char* payload,
                                        std::size_t payload_size,
                                        const std::string& codec_name) {
    if (codec_name == "vorbis") {
        return payload_size >= 7 && payload[0] == 0x01U &&
               std::memcmp(payload + 1, "vorbis", 6) == 0;
    }
    if (codec_name == "opus") {
        return payload_size >= 8 && std::memcmp(payload, "OpusHead", 8) == 0;
    }
    return false;
}

bool verify_ogg_terminal_eos(const std::string& path,
                             const std::string& codec_name,
                             InterruptState* interrupt) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff file_size_value = input.tellg();
    if (file_size_value < 27) {
        return false;
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(file_size_value);
    const std::size_t head_bytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(file_size, kOggMaximumPageSize));
    std::vector<unsigned char> head(head_bytes);
    input.seekg(0, std::ios::beg);
    if (!read_exact(input, head.data(), head.size())) {
        return false;
    }
    if (interrupt != nullptr && interrupt->interrupted()) {
        throw std::runtime_error(interrupt->timed_out()
            ? "metadata probe timed out"
            : "metadata probe cancelled");
    }

    OggPageInfo first_page;
    if (!parse_ogg_page(head.data(), head.size(), &first_page) ||
        (first_page.flags & 0x02U) == 0U || (first_page.flags & 0x01U) != 0U ||
        first_page.sequence != 0 || first_page.segment_count == 0 ||
        first_page.completed_packet_count != 1 ||
        first_page.last_lacing_value == 255U ||
        !ogg_page_crc_matches(head.data(), first_page)) {
        return false;
    }
    const unsigned char* identification_payload =
        head.data() + first_page.header_size;
    const std::size_t identification_payload_size =
        first_page.page_size - first_page.header_size;
    if (!ogg_identification_payload_matches(identification_payload,
                                            identification_payload_size,
                                            codec_name)) {
        return false;
    }

    constexpr std::uint64_t kOggTailProbeBytes =
        static_cast<std::uint64_t>(kOggMaximumPageSize) * 2U;
    const std::size_t tail_bytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(file_size, kOggTailProbeBytes));
    std::vector<unsigned char> tail(tail_bytes);
    input.clear();
    input.seekg(file_size_value - static_cast<std::streamoff>(tail_bytes),
                std::ios::beg);
    if (!read_exact(input, tail.data(), tail.size())) {
        return false;
    }

    for (std::size_t offset = 0; offset + 27 <= tail.size(); ++offset) {
        if (tail[offset] != 'O' ||
            std::memcmp(tail.data() + offset, "OggS", 4) != 0) {
            continue;
        }
        OggPageInfo last_page;
        if (!parse_ogg_page(tail.data() + offset,
                            tail.size() - offset,
                            &last_page) ||
            offset + last_page.page_size != tail.size() ||
            !ogg_page_crc_matches(tail.data() + offset, last_page)) {
            continue;
        }
        if (last_page.serial != first_page.serial ||
            (last_page.flags & 0x04U) == 0U ||
            last_page.granule_position ==
                std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        if (interrupt != nullptr && interrupt->interrupted()) {
            throw std::runtime_error(interrupt->timed_out()
                ? "metadata probe timed out"
                : "metadata probe cancelled");
        }

        if (last_page.segment_count > 0) {
            return last_page.last_lacing_value < 255U;
        }
        if ((last_page.flags & 0x01U) != 0U || offset == 0) {
            return false;
        }

        for (std::size_t previous_offset = 0;
             previous_offset + 27 <= offset;
             ++previous_offset) {
            if (tail[previous_offset] != 'O' ||
                std::memcmp(tail.data() + previous_offset, "OggS", 4) != 0) {
                continue;
            }
            OggPageInfo previous_page;
            if (!parse_ogg_page(tail.data() + previous_offset,
                                offset - previous_offset,
                                &previous_page) ||
                previous_offset + previous_page.page_size != offset ||
                !ogg_page_crc_matches(tail.data() + previous_offset,
                                      previous_page)) {
                continue;
            }
            return previous_page.serial == last_page.serial &&
                   previous_page.sequence + 1U == last_page.sequence &&
                   previous_page.segment_count > 0 &&
                   previous_page.last_lacing_value < 255U;
        }
        return false;
    }
    return false;
}

std::optional<std::int64_t> probe_vorbis_presentation_origin_sample(
    AVFormatContext* context,
    int stream_index,
    std::uint32_t sample_rate,
    InterruptState* interrupt) {
    if (context == nullptr || stream_index < 0 ||
        static_cast<unsigned int>(stream_index) >= context->nb_streams ||
        sample_rate == 0 ||
        sample_rate > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }

    ScopedInterruptBudget bounded_budget(interrupt, kVorbisOriginProbeTimeout);

    AVCodecContext* codec_context = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    const auto cleanup = [&]() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec_context);
    };

    try {
        AVStream* stream = context->streams[stream_index];
        if (stream == nullptr || stream->codecpar == nullptr ||
            stream->codecpar->codec_id != AV_CODEC_ID_VORBIS ||
            stream->time_base.num <= 0 || stream->time_base.den <= 0) {
            cleanup();
            return std::nullopt;
        }

        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (decoder == nullptr) {
            cleanup();
            return std::nullopt;
        }
        codec_context = avcodec_alloc_context3(decoder);
        if (codec_context == nullptr) {
            cleanup();
            return std::nullopt;
        }
        int result = avcodec_parameters_to_context(codec_context, stream->codecpar);
        if (result < 0) {
            cleanup();
            return std::nullopt;
        }
        codec_context->pkt_timebase = stream->time_base;
        result = avcodec_open2(codec_context, decoder, nullptr);
        if (result < 0 || codec_context->sample_rate <= 0 ||
            static_cast<std::uint32_t>(codec_context->sample_rate) != sample_rate) {
            cleanup();
            return std::nullopt;
        }

        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (packet == nullptr || frame == nullptr) {
            cleanup();
            return std::nullopt;
        }

        const auto decoded_frame_start = [&]() -> std::optional<std::int64_t> {
            if (frame->sample_rate > 0 &&
                static_cast<std::uint32_t>(frame->sample_rate) != sample_rate) {
                return std::nullopt;
            }
            const std::int64_t timestamp = frame->best_effort_timestamp != AV_NOPTS_VALUE
                ? frame->best_effort_timestamp
                : frame->pts;
            if (timestamp == AV_NOPTS_VALUE) {
                return std::nullopt;
            }
            const AVRational sample_time_base{1, static_cast<int>(sample_rate)};
            const std::int64_t sample_position = av_rescale_q_rnd(
                timestamp,
                stream->time_base,
                sample_time_base,
                static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            const std::int64_t round_trip_timestamp = av_rescale_q_rnd(
                sample_position,
                sample_time_base,
                stream->time_base,
                static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            if (round_trip_timestamp != timestamp) {
                return std::nullopt;
            }
            return sample_position;
        };

        bool decoder_or_proof_failed = false;
        const auto receive_available_frame =
            [&](bool* made_progress) -> std::optional<std::int64_t> {
            if (made_progress != nullptr) {
                *made_progress = false;
            }
            while (true) {
                const int receive_result = avcodec_receive_frame(codec_context, frame);
                if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                    return std::nullopt;
                }
                if (receive_result < 0) {
                    decoder_or_proof_failed = true;
                    return std::nullopt;
                }
                if (made_progress != nullptr) {
                    *made_progress = true;
                }
                if (frame->nb_samples <= 0) {
                    av_frame_unref(frame);
                    continue;
                }
                const std::optional<std::int64_t> position = decoded_frame_start();
                av_frame_unref(frame);
                if (position.has_value()) {
                    return position;
                }
                decoder_or_proof_failed = true;
                return std::nullopt;
            }
        };

        std::size_t demux_packets = 0;
        std::size_t audio_packets = 0;
        std::uint64_t payload_bytes = 0;
        bool clean_eof = false;
        while (interrupt == nullptr || !interrupt->interrupted()) {
            if (demux_packets >= kVorbisOriginMaximumDemuxPackets ||
                audio_packets >= kVorbisOriginMaximumAudioPackets ||
                payload_bytes >= kVorbisOriginMaximumPayloadBytes) {
                cleanup();
                return std::nullopt;
            }
            result = av_read_frame(context, packet);
            if (result == AVERROR_EOF) {
                clean_eof = true;
                break;
            }
            if (result < 0) {
                if (interrupt != nullptr &&
                    (interrupt->cancelled() || interrupt->global_timed_out())) {
                    cleanup();
                    throw std::runtime_error(interrupt->global_timed_out()
                        ? "metadata probe timed out"
                        : "metadata probe cancelled");
                }
                cleanup();
                return std::nullopt;
            }
            ++demux_packets;
            const std::uint64_t packet_payload_bytes = packet->size >= 0
                ? static_cast<std::uint64_t>(packet->size)
                : kVorbisOriginMaximumPayloadBytes + 1u;
            if (packet_payload_bytes > kVorbisOriginMaximumPayloadBytes ||
                payload_bytes > kVorbisOriginMaximumPayloadBytes - packet_payload_bytes) {
                av_packet_unref(packet);
                cleanup();
                return std::nullopt;
            }
            payload_bytes += packet_payload_bytes;
            if (packet->stream_index != stream_index) {
                av_packet_unref(packet);
                continue;
            }
            ++audio_packets;

            while (true) {
                result = avcodec_send_packet(codec_context, packet);
                if (result != AVERROR(EAGAIN)) {
                    break;
                }
                bool receive_made_progress = false;
                const std::optional<std::int64_t> position =
                    receive_available_frame(&receive_made_progress);
                if (position.has_value()) {
                    av_packet_unref(packet);
                    cleanup();
                    return position;
                }
                if (decoder_or_proof_failed || !receive_made_progress) {
                    av_packet_unref(packet);
                    cleanup();
                    return std::nullopt;
                }
            }
            av_packet_unref(packet);
            if (result < 0) {
                cleanup();
                return std::nullopt;
            }
            const std::optional<std::int64_t> position = receive_available_frame(nullptr);
            if (position.has_value()) {
                cleanup();
                return position;
            }
            if (decoder_or_proof_failed) {
                cleanup();
                return std::nullopt;
            }
        }

        if (interrupt != nullptr && interrupt->interrupted()) {
            if (interrupt->bounded_timed_out() && !interrupt->cancelled() &&
                !interrupt->global_timed_out()) {
                cleanup();
                return std::nullopt;
            }
            cleanup();
            throw std::runtime_error(interrupt->timed_out()
                ? "metadata probe timed out"
                : "metadata probe cancelled");
        }
        if (clean_eof && avcodec_send_packet(codec_context, nullptr) >= 0) {
            const std::optional<std::int64_t> position = receive_available_frame(nullptr);
            cleanup();
            return position;
        }
        cleanup();
        return std::nullopt;
    } catch (...) {
        cleanup();
        throw;
    }
}

std::uint64_t read_id3v2_size(const unsigned char* h) {
    return (static_cast<std::uint64_t>(h[6] & 0x7Fu) << 21) |
           (static_cast<std::uint64_t>(h[7] & 0x7Fu) << 14) |
           (static_cast<std::uint64_t>(h[8] & 0x7Fu) << 7) |
           static_cast<std::uint64_t>(h[9] & 0x7Fu);
}

bool probe_adts_aac_headers(const std::string& path,
                            ExternalAudioInfo& info,
                            InterruptState* interrupt) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        return false;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff file_size = input.tellg();
    if (file_size <= 0) {
        return false;
    }
    input.seekg(0, std::ios::beg);

    unsigned char first[10]{};
    input.read(reinterpret_cast<char*>(first), sizeof(first));
    const std::size_t got_first = static_cast<std::size_t>(input.gcount());
    if (got_first >= 10 && std::memcmp(first, "ID3", 3) == 0) {
        const std::uint64_t tag_size = read_id3v2_size(first);
        const bool footer = (first[5] & 0x10u) != 0;
        const std::uint64_t audio_offset = 10u + tag_size + (footer ? 10u : 0u);
        if (audio_offset >= static_cast<std::uint64_t>(file_size) ||
            audio_offset > static_cast<std::uint64_t>(
                std::numeric_limits<std::streamoff>::max())) {
            return false;
        }
        input.clear();
        input.seekg(static_cast<std::streamoff>(audio_offset), std::ios::beg);
    } else {
        input.clear();
        input.seekg(0, std::ios::beg);
    }

    static const std::uint32_t kSampleRates[16] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 8000, 7350, 0, 0, 0
    };
    static const std::uint16_t kChannelCounts[8] = {
        0, 1, 2, 3, 4, 5, 6, 8
    };

    constexpr std::size_t kMaximumProbeFrames = 8;
    std::size_t validated_frames = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    while (input && validated_frames < kMaximumProbeFrames) {
        if (interrupt != nullptr && interrupt->interrupted()) {
            throw std::runtime_error(interrupt->timed_out()
                ? "metadata probe timed out"
                : "metadata probe cancelled");
        }
        unsigned char h[7]{};
        const std::streampos frame_pos = input.tellg();
        if (frame_pos == std::streampos(-1)) {
            return false;
        }
        const std::streamoff frame_offset = static_cast<std::streamoff>(frame_pos);
        input.read(reinterpret_cast<char*>(h), sizeof(h));
        if (static_cast<std::size_t>(input.gcount()) != sizeof(h)) {
            break;
        }
        if (h[0] != 0xFFu || (h[1] & 0xF0u) != 0xF0u ||
            (h[1] & 0x06u) != 0) {
            return false;
        }
        const unsigned int sf_index = (h[2] >> 2) & 0x0Fu;
        if (sf_index >= 16 || kSampleRates[sf_index] == 0) {
            return false;
        }
        const std::uint16_t channel_config = static_cast<std::uint16_t>(
            ((h[2] & 0x01u) << 2) | ((h[3] & 0xC0u) >> 6));
        if (channel_config >= 8 || kChannelCounts[channel_config] == 0) {
            return false;
        }
        const std::uint32_t frame_length =
            (static_cast<std::uint32_t>(h[3] & 0x03u) << 11) |
            (static_cast<std::uint32_t>(h[4]) << 3) |
            ((static_cast<std::uint32_t>(h[5] & 0xE0u)) >> 5);
        const std::uint32_t header_size = (h[1] & 0x01u) != 0 ? 7u : 9u;
        if (frame_length < header_size) {
            return false;
        }
        if (frame_offset < 0 ||
            static_cast<std::uint64_t>(frame_offset) >
                std::numeric_limits<std::uint64_t>::max() - frame_length) {
            return false;
        }
        const std::uint64_t frame_end =
            static_cast<std::uint64_t>(frame_offset) + frame_length;
        if (frame_end > static_cast<std::uint64_t>(file_size) ||
            frame_end > static_cast<std::uint64_t>(
                std::numeric_limits<std::streamoff>::max())) {
            return false;
        }
        const std::uint32_t current_sample_rate = kSampleRates[sf_index];
        const std::uint16_t current_channels = kChannelCounts[channel_config];
        if (sample_rate == 0) {
            sample_rate = current_sample_rate;
            channels = current_channels;
        } else if (sample_rate != current_sample_rate || channels != current_channels) {
            return false;
        }
        ++validated_frames;
        input.clear();
        input.seekg(static_cast<std::streamoff>(frame_end), std::ios::beg);
    }

    if (validated_frames == 0 || sample_rate == 0) {
        return false;
    }
    info.format.sample_rate = sample_rate;
    info.format.channels = channels == 0 ? 2 : channels;
    info.format.bits_per_sample = 16;
    info.codec_name = "aac";
    info.lossless = false;
    info.raw_aac = true;
    Logger::instance().debug("ExternalAudioDecoder bounded ADTS AAC header probe: " + path +
                             " frames=" + std::to_string(validated_frames));
    return true;
}

bool probe_wav_header_fast(const std::string& path,
                           ExternalAudioInfo& info,
                           bool* has_embedded_id3) {
    if (has_embedded_id3 != nullptr) {
        *has_embedded_id3 = false;
    }
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        return false;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff file_size_value = input.tellg();
    if (file_size_value < 12) {
        return false;
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(file_size_value);
    input.clear();
    input.seekg(0, std::ios::beg);

    unsigned char riff[12]{};
    if (!read_exact(input, riff, sizeof(riff))) {
        return false;
    }
    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        return false;
    }

    const std::uint64_t riff_payload_size = read_le32(riff + 4);
    if (riff_payload_size < 4 || riff_payload_size > file_size - 8) {
        return false;
    }
    const std::uint64_t riff_end = 8 + riff_payload_size;

    bool have_fmt = false;
    bool have_data = false;
    std::uint16_t audio_format = 0;
    bool wave_format_extensible = false;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t byte_rate = 0;
    std::uint16_t block_align = 0;
    std::uint16_t bits_per_sample = 0;
    std::uint16_t valid_bits_per_sample = 0;
    std::uint64_t data_bytes = 0;
    constexpr std::uint32_t kMaxInfoChunkBytes = 4U * 1024U * 1024U;
    constexpr std::uint64_t kMaximumMetadataBytes = 4U * 1024U * 1024U;
    constexpr std::size_t kMaximumChunks = 4096;
    constexpr std::array<unsigned char, 12> kWaveSubtypeGuidTail{{
        0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
        0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
    }};

    std::uint64_t chunk_offset = 12;
    std::uint64_t metadata_bytes_read = 0;
    std::size_t chunk_count = 0;
    while (chunk_offset + 8 <= riff_end) {
        if (++chunk_count > kMaximumChunks ||
            chunk_offset > static_cast<std::uint64_t>(
                std::numeric_limits<std::streamoff>::max())) {
            return false;
        }
        input.clear();
        input.seekg(static_cast<std::streamoff>(chunk_offset), std::ios::beg);
        if (!input) {
            return false;
        }

        unsigned char header[8]{};
        if (!read_exact(input, header, sizeof(header))) {
            return false;
        }
        const std::string chunk_id(reinterpret_cast<const char*>(header), 4);
        const std::uint64_t chunk_size = read_le32(header + 4);
        const std::uint64_t payload_offset = chunk_offset + 8;
        if (chunk_size > std::numeric_limits<std::uint64_t>::max() - payload_offset) {
            return false;
        }
        const std::uint64_t payload_end = payload_offset + chunk_size;
        const std::uint64_t padding = chunk_size & 1U;
        if (payload_end > std::numeric_limits<std::uint64_t>::max() - padding) {
            return false;
        }
        const std::uint64_t next_chunk_offset = payload_end + padding;
        if (payload_end > riff_end || next_chunk_offset > riff_end ||
            payload_end > file_size || next_chunk_offset > file_size) {
            return false;
        }

        if (chunk_id == "fmt ") {
            if (have_fmt || chunk_size < 16 || chunk_size > kMaxInfoChunkBytes) {
                return false;
            }
            std::array<unsigned char, 40> fmt{};
            const std::size_t bytes_to_read = static_cast<std::size_t>(
                std::min<std::uint64_t>(chunk_size, fmt.size()));
            if (!read_exact(input, fmt.data(), bytes_to_read)) {
                return false;
            }

            audio_format = read_le16(fmt.data());
            channels = read_le16(fmt.data() + 2);
            sample_rate = read_le32(fmt.data() + 4);
            byte_rate = read_le32(fmt.data() + 8);
            block_align = read_le16(fmt.data() + 12);
            bits_per_sample = read_le16(fmt.data() + 14);
            valid_bits_per_sample = bits_per_sample;

            if (audio_format == 0xFFFE) {
                wave_format_extensible = true;
                if (chunk_size < 40) {
                    return false;
                }
                const std::uint16_t extension_size = read_le16(fmt.data() + 16);
                if (extension_size < 22 ||
                    static_cast<std::uint64_t>(18) + extension_size > chunk_size) {
                    return false;
                }
                valid_bits_per_sample = read_le16(fmt.data() + 18);
                if (std::memcmp(fmt.data() + 28,
                                kWaveSubtypeGuidTail.data(),
                                kWaveSubtypeGuidTail.size()) != 0) {
                    return false;
                }
                const std::uint32_t subtype = read_le32(fmt.data() + 24);
                if (subtype == 1U) {
                    audio_format = 1;
                } else if (subtype == 3U) {
                    audio_format = 3;
                } else {
                    return false;
                }
            }
            have_fmt = true;
        } else if (chunk_id == "data") {
            if (have_data) {
                return false;
            }
            data_bytes = chunk_size;
            have_data = true;
        } else if (chunk_id == "LIST" &&
                   chunk_size >= 4 &&
                   chunk_size <= kMaxInfoChunkBytes &&
                   chunk_size <= kMaximumMetadataBytes - metadata_bytes_read) {
            metadata_bytes_read += chunk_size;
            std::vector<unsigned char> list_data(static_cast<std::size_t>(chunk_size));
            if (!read_exact(input, list_data.data(), list_data.size())) {
                return false;
            }
            if (std::memcmp(list_data.data(), "INFO", 4) == 0) {
                std::size_t offset = 4;
                while (offset + 8 <= list_data.size()) {
                    const std::string info_id(
                        reinterpret_cast<const char*>(list_data.data() + offset), 4);
                    const std::uint32_t value_size = read_le32(list_data.data() + offset + 4);
                    offset += 8;
                    if (value_size > list_data.size() - offset) {
                        break;
                    }
                    std::string value(
                        reinterpret_cast<const char*>(list_data.data() + offset),
                        static_cast<std::size_t>(value_size));
                    const std::size_t nul = value.find('\0');
                    if (nul != std::string::npos) {
                        value.resize(nul);
                    }
                    value = trim_value_copy(value);
                    if (!value.empty()) {
                        const std::string normalized =
                            pcmtp::text::normalize_metadata_value(value);
                        if (info_id == "IART" && info.tags.artist.empty()) {
                            info.tags.artist = normalized;
                        } else if (info_id == "INAM" && info.tags.title.empty()) {
                            info.tags.title = normalized;
                        } else if ((info_id == "IPRD" || info_id == "IALB" ||
                                    info_id == "ALBM") && info.tags.album.empty()) {
                            info.tags.album = normalized;
                        } else if ((info_id == "ITRK" || info_id == "IPRT") &&
                                   info.tags.track_number == 0) {
                            try {
                                info.tags.track_number = std::stoi(value);
                            } catch (...) {}
                        }
                    }
                    offset += value_size;
                    if ((value_size & 1U) != 0 && offset < list_data.size()) {
                        ++offset;
                    }
                }
            }
        } else if (chunk_id == "ID3 " || chunk_id == "id3 ") {
            if (has_embedded_id3 != nullptr) {
                *has_embedded_id3 = true;
            }
        }

        chunk_offset = next_chunk_offset;
    }

    if (chunk_offset != riff_end || !have_fmt || !have_data || channels == 0 ||
        sample_rate == 0 || block_align == 0 || bits_per_sample == 0) {
        return false;
    }
    if (!(audio_format == 1 || audio_format == 3)) {
        return false;
    }
    if (!(bits_per_sample == 16 || bits_per_sample == 24 || bits_per_sample == 32)) {
        return false;
    }
    if (valid_bits_per_sample == 0 || valid_bits_per_sample > bits_per_sample ||
        (wave_format_extensible && valid_bits_per_sample != bits_per_sample) ||
        (audio_format == 3 &&
         (bits_per_sample != 32 || valid_bits_per_sample != bits_per_sample))) {
        return false;
    }

    const std::uint64_t bytes_per_sample = bits_per_sample / 8;
    if (bytes_per_sample == 0 ||
        channels > std::numeric_limits<std::uint64_t>::max() / bytes_per_sample) {
        return false;
    }
    const std::uint64_t expected_block_align =
        static_cast<std::uint64_t>(channels) * bytes_per_sample;
    if (expected_block_align != block_align || data_bytes % block_align != 0) {
        return false;
    }
    if (sample_rate > std::numeric_limits<std::uint64_t>::max() / block_align ||
        static_cast<std::uint64_t>(sample_rate) * block_align != byte_rate) {
        return false;
    }

    info.format.sample_rate = sample_rate;
    info.format.channels = channels;
    info.format.bits_per_sample = bits_per_sample;
    info.total_samples_per_channel = data_bytes / block_align;
    info.codec_name = audio_format == 3 ? "pcm_f" + std::to_string(bits_per_sample) + "le"
                                        : "pcm_s" + std::to_string(bits_per_sample) + "le";
    info.lossless = true;
    info.sample_extent_kind = SampleExtentKind::ExactPresentationSpan;
    info.sample_extent_source = SampleExtentSource::PcmDataSize;
    Logger::instance().debug("ExternalAudioDecoder fast WAV probe: " + path);
    return true;
}


struct LibavProbeResult {
    ExternalAudioInfo info;
};

LibavProbeResult probe_with_libav(const std::string& path,
                                  ProbeCancellation* probe_cancellation) {
    InterruptState interrupt;
    interrupt.cancellation = probe_cancellation;
    interrupt.cancellation_token = probe_cancellation != nullptr ? probe_cancellation->token() : 0;
    interrupt.deadline = std::chrono::steady_clock::now() + kProbeTimeout;
    interrupt.use_deadline = true;

    AVFormatContext* context = open_input_context(path, &interrupt, true);
    try {
        const int stream_index = first_audio_stream(context);
        AVStream* stream = context->streams[stream_index];
        AVCodecParameters* parameters = stream->codecpar;

        ExternalAudioInfo info;
        info.format.sample_rate = parameters->sample_rate > 0
            ? static_cast<std::uint32_t>(parameters->sample_rate) : 44100;
        const int channels = stream_channels(parameters);
        info.format.channels = static_cast<std::uint16_t>(channels > 0 ? channels : 2);
        const char* codec_name = avcodec_get_name(parameters->codec_id);
        info.codec_name = codec_name != nullptr ? lower_copy(codec_name) : std::string();
        if (parameters->bit_rate > 0) {
            info.bit_rate = static_cast<std::uint32_t>(parameters->bit_rate);
        } else if (context->bit_rate > 0) {
            info.bit_rate = static_cast<std::uint32_t>(context->bit_rate);
        }
        int bits = parameters->bits_per_raw_sample;
        if (bits <= 0) bits = parameters->bits_per_coded_sample;
        if (bits <= 0) bits = av_get_exact_bits_per_sample(parameters->codec_id);
        info.format.bits_per_sample = normalize_bits(
            bits, static_cast<AVSampleFormat>(parameters->format), info.codec_name);
        info.duration_ts = stream->duration;
        info.time_base = rational_string(stream->time_base);
        info.tags = extract_tags(context->metadata, stream->metadata);
        LibavStreamBoundaryFacts boundary_facts;
        boundary_facts.demuxer_name = context->iformat != nullptr && context->iformat->name != nullptr
            ? lower_copy(context->iformat->name)
            : std::string();
        boundary_facts.codec_name = info.codec_name;
        boundary_facts.duration = stream->duration;
        boundary_facts.time_base_num = stream->time_base.num;
        boundary_facts.time_base_den = stream->time_base.den;
        boundary_facts.sample_rate = info.format.sample_rate;
        boundary_facts.initial_padding = parameters->initial_padding;
        boundary_facts.trailing_padding = parameters->trailing_padding;
        boundary_facts.stream_count = context->nb_streams;
        for (unsigned int index = 0; index < context->nb_streams; ++index) {
            const AVStream* candidate_stream = context->streams[index];
            if (candidate_stream != nullptr && candidate_stream->codecpar != nullptr &&
                candidate_stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                ++boundary_facts.audio_stream_count;
            }
        }
        boundary_facts.stream_info_complete = true;
        if (is_mov_family_format_name(boundary_facts.demuxer_name) &&
            info.codec_name == "aac") {
            const MovAacBoundaryEvidence aac_evidence =
                probe_mov_aac_boundary_evidence(path, context, stream_index, &interrupt);
            if (aac_evidence.exact_presentation &&
                aac_evidence.presentation_samples ==
                    static_cast<std::uint64_t>(stream->duration)) {
                boundary_facts.mov_aac_exact_presentation_evidence = true;
                boundary_facts.mov_aac_exact_presentation_drain_policy =
                    aac_evidence.drain_policy;
                if (aac_evidence.terminal_discard) {
                    boundary_facts.decoder_eof_evidence_source =
                        DecoderEofEvidenceSource::MovAacTerminalDiscard;
                }
            }
        }
        if (format_name_has_token(boundary_facts.demuxer_name, "ogg") &&
            (info.codec_name == "vorbis" || info.codec_name == "opus") &&
            boundary_facts.stream_count == 1 &&
            boundary_facts.audio_stream_count == 1) {
            if (verify_ogg_terminal_eos(path, info.codec_name, &interrupt)) {
                boundary_facts.decoder_eof_evidence_source =
                    DecoderEofEvidenceSource::OggTerminalEos;
            }
        }
        info.source_supports_trusted_decoder_eof =
            libav_stream_supports_trusted_decoder_eof(boundary_facts);

        const SampleExtent stream_extent = classify_libav_stream_extent(boundary_facts);
        ContainerBoundaryFacts container_boundary_facts;
        container_boundary_facts.demuxer_name = boundary_facts.demuxer_name;
        container_boundary_facts.codec_name = info.codec_name;
        container_boundary_facts.sample_rate = info.format.sample_rate;
        container_boundary_facts.channels = info.format.channels;
        container_boundary_facts.block_align = parameters->block_align > 0
            ? static_cast<std::uint32_t>(parameters->block_align) : 0;
        int boundary_bits = parameters->bits_per_coded_sample;
        if (boundary_bits <= 0) {
            boundary_bits = av_get_exact_bits_per_sample(parameters->codec_id);
        }
        if (boundary_bits > 0 && boundary_bits <=
                static_cast<int>(std::numeric_limits<std::uint16_t>::max())) {
            container_boundary_facts.bits_per_coded_sample =
                static_cast<std::uint16_t>(boundary_bits);
        }
        container_boundary_facts.initial_padding = parameters->initial_padding;
        container_boundary_facts.trailing_padding = parameters->trailing_padding;
        const SampleExtent verified_file_extent = verify_container_sample_extent(
            path, container_boundary_facts);
        const SampleExtent container_extent = estimated_container_extent(
            context->duration, 1, AV_TIME_BASE, info.format.sample_rate);
        SampleExtent selected_extent = select_stronger_sample_extent(
            stream_extent, verified_file_extent);
        if (format_name_has_token(boundary_facts.demuxer_name, "ogg") &&
            info.codec_name == "vorbis") {
            const std::optional<std::int64_t> presentation_start =
                probe_vorbis_presentation_origin_sample(
                    context, stream_index, info.format.sample_rate, &interrupt);
            if (presentation_start.has_value()) {
                std::int64_t stream_start_sample = 0;
                bool stream_start_exact = true;
                if (stream->start_time != AV_NOPTS_VALUE) {
                    const AVRational sample_time_base{
                        1, static_cast<int>(info.format.sample_rate)};
                    stream_start_sample = av_rescale_q_rnd(
                        stream->start_time,
                        stream->time_base,
                        sample_time_base,
                        static_cast<AVRounding>(
                            AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
                    const std::int64_t round_trip_start = av_rescale_q_rnd(
                        stream_start_sample,
                        sample_time_base,
                        stream->time_base,
                        static_cast<AVRounding>(
                            AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
                    stream_start_exact = round_trip_start == stream->start_time;
                }
                if (stream_start_exact && *presentation_start >= stream_start_sample) {
                    info.source_presentation_start_known = true;
                    info.source_presentation_start_sample =
                        static_cast<std::uint64_t>(
                            *presentation_start - stream_start_sample);
                }
            }
        }
        if (!sample_extent_supports_bounded_transport(selected_extent.kind) &&
            container_extent.samples > selected_extent.samples) {
            selected_extent = container_extent;
        }
        info.total_samples_per_channel = selected_extent.samples;
        info.sample_extent_kind = selected_extent.kind;
        info.sample_extent_source = selected_extent.source;
        info.sample_extent_drain_policy =
            selected_extent.exact_presentation_drain_policy;
        info.probe_backend = "libavformat";
        avformat_close_input(&context);
        return LibavProbeResult{info};
    } catch (...) {
        avformat_close_input(&context);
        throw;
    }
}

void merge_missing_tags(GenericTags& destination, const GenericTags& source) {
    if (destination.title.empty()) destination.title = source.title;
    if (destination.artist.empty()) destination.artist = source.artist;
    if (destination.album.empty()) destination.album = source.album;
    if (destination.track_number == 0) destination.track_number = source.track_number;
}

int soxr_precision(const std::string& quality) {
    if (quality == "high") return 28;
    if (quality == "balanced") return 20;
    if (quality == "fast") return 16;
    return 33;
}

const char* dither_method(const std::string& quality) {
    if (quality == "tpdf") return "triangular";
    if (quality == "rectangular") return "rectangular";
    return "triangular_hp";
}

void set_swr_option(SwrContext* context, const char* name, const char* value) {
    const int result = av_opt_set(context, name, value, 0);
    if (result < 0) {
        throw std::runtime_error(std::string("Cannot set FFmpeg resampler option ") +
                                 name + ": " + av_error_string(result));
    }
}

void set_swr_option_int(SwrContext* context, const char* name, std::int64_t value) {
    const int result = av_opt_set_int(context, name, value, 0);
    if (result < 0) {
        throw std::runtime_error(std::string("Cannot set FFmpeg resampler option ") +
                                 name + ": " + av_error_string(result));
    }
}

} // namespace

struct ExternalAudioDecoder::Impl {
    AVFormatContext* format_context = nullptr;
    AVCodecContext* codec_context = nullptr;
    SwrContext* swr_context = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    int audio_stream_index = -1;
    bool packet_pending = false;
    bool input_eof = false;
    bool decoder_flush_sent = false;
    bool decoder_eof = false;
    bool swr_drained = false;
    std::atomic<bool> abort_requested{false};
    InterruptState interrupt{};
    std::vector<PcmSample> pending_samples;
    std::size_t pending_offset = 0;
    std::vector<std::int16_t> conversion_buffer_s16;
    std::vector<std::int32_t> conversion_buffer_s32;
    std::vector<const std::uint8_t*> input_planes;
    std::atomic<ResamplerRuntimeKind> resampler_runtime_kind{
        ResamplerRuntimeKind::NotUsed};
    bool seeking = false;
    std::uint64_t seek_target_sample = 0;
    std::uint64_t output_timeline_sample = 0;
    bool output_timeline_initialized = false;
    std::uint64_t expected_input_timeline_sample = 0;
    bool expected_input_timeline_known = false;
    bool timestamp_discontinuity_reported = false;
    int configured_input_rate = 0;
    AVSampleFormat configured_input_format = AV_SAMPLE_FMT_NONE;
    int configured_channels = 0;
#if PCMTP_FFMPEG_HAS_MODERN_CHANNEL_LAYOUT
    AVChannelLayout configured_input_layout{};
    bool configured_input_layout_valid = false;
#else
    std::uint64_t configured_input_layout = 0;
#endif
    unsigned int consecutive_decode_errors = 0;

    ~Impl() {
        clear_configured_input_layout();
    }

    void clear_configured_input_layout() {
#if PCMTP_FFMPEG_HAS_MODERN_CHANNEL_LAYOUT
        if (configured_input_layout_valid) {
            av_channel_layout_uninit(&configured_input_layout);
            configured_input_layout_valid = false;
        }
#else
        configured_input_layout = 0;
#endif
    }

    void reset_decode_state() {
        packet_pending = false;
        input_eof = false;
        decoder_flush_sent = false;
        decoder_eof = false;
        swr_drained = false;
        pending_samples.clear();
        pending_offset = 0;
        resampler_runtime_kind.store(
            ResamplerRuntimeKind::NotUsed, std::memory_order_release);
        seeking = false;
        seek_target_sample = 0;
        output_timeline_sample = 0;
        output_timeline_initialized = false;
        expected_input_timeline_sample = 0;
        expected_input_timeline_known = false;
        timestamp_discontinuity_reported = false;
        consecutive_decode_errors = 0;
    }
};

ExternalAudioDecoder::ExternalAudioDecoder(std::uint32_t forced_output_sample_rate,
                                           std::uint16_t forced_output_bits_per_sample,
                                           const std::string& resample_quality,
                                           const std::string& bitdepth_quality)
    : forced_output_sample_rate_(forced_output_sample_rate),
      forced_output_bits_per_sample_(forced_output_bits_per_sample),
      resample_quality_(resample_quality),
      bitdepth_quality_(bitdepth_quality),
      impl_(new Impl()) {}

ExternalAudioDecoder::~ExternalAudioDecoder() {
    close_decoder();
}

std::string ExternalAudioDecoder::to_lower_extension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return std::string();
    }
    return lower_copy(path.substr(dot));
}

bool ExternalAudioDecoder::looks_supported(const std::string& path) {
    if (is_remote_media_uri(path)) {
        return true;
    }
    static const std::array<const char*, 39> extensions = {{
        ".mp3", ".mp2", ".m4a", ".m4r", ".aac", ".ac3", ".dts", ".ogg", ".oga",
        ".opus", ".spx", ".wav", ".wave", ".w64", ".bwf", ".au", ".snd", ".caf",
        ".voc", ".ra", ".ape", ".wv", ".flac", ".aiff", ".aif", ".tak", ".tta",
        ".wma", ".asf", ".xwma", ".wmv", ".oma", ".aa3", ".at3", ".mpc", ".mp+",
        ".mpp", ".dsf", ".dff"
    }};
    const std::string extension = to_lower_extension(path);
    return std::find_if(extensions.begin(), extensions.end(), [&](const char* item) {
        return extension == item;
    }) != extensions.end();
}

ExternalAudioInfo ExternalAudioDecoder::probe_metadata(const std::string& path,
                                                        std::uint32_t forced_output_sample_rate,
                                                        std::uint16_t forced_output_bits_per_sample,
                                                        ProbeCancellation* probe_cancellation) {
    if (!looks_supported(path)) {
        throw std::runtime_error("ExternalAudioDecoder does not support this file type");
    }

    ExternalAudioInfo info;
    info.format.sample_rate = 44100;
    info.format.channels = 2;
    info.format.bits_per_sample = 16;
    const std::string extension = to_lower_extension(path);

    InterruptState interrupt;
    interrupt.cancellation = probe_cancellation;
    interrupt.cancellation_token = probe_cancellation != nullptr ? probe_cancellation->token() : 0;
    interrupt.deadline = std::chrono::steady_clock::now() + kProbeTimeout;
    interrupt.use_deadline = true;

    bool have_info = false;
    bool wav_has_embedded_id3 = false;
    const bool fast_wav = extension == ".wav" || extension == ".wave" || extension == ".bwf";
    if (fast_wav) {
        have_info = probe_wav_header_fast(path, info, &wav_has_embedded_id3);
        if (have_info) {
            info.probe_backend = "wav-fast";
        }
    }

    LibavProbeResult libav_result;
    if (!have_info || wav_has_embedded_id3) {
        try {
            libav_result = probe_with_libav(path, probe_cancellation);
            if (!have_info) {
                info = libav_result.info;
                have_info = true;
            } else {
                merge_missing_tags(info.tags, libav_result.info.tags);
                info.probe_backend = "wav-fast+libav-tags";
            }
        } catch (const std::exception& error) {
            if (!have_info) {
                throw;
            }
            Logger::instance().debug("WAV embedded metadata fallback failed for " + path +
                                     ": " + error.what());
        }
    }

    if (extension == ".aac") {
        ExternalAudioInfo aac_info = info;
        if (probe_adts_aac_headers(path, aac_info, &interrupt)) {
            aac_info.tags = info.tags;
            aac_info.probe_backend = "libavformat+adts-bounded";
            info = aac_info;
        } else {
            info.raw_aac = true;
        }
    }

    if (!have_info && info.codec_name.empty()) {
        throw std::runtime_error("Cannot determine audio stream information");
    }
    if (interrupt.interrupted()) {
        throw std::runtime_error(interrupt.timed_out()
            ? "metadata probe timed out"
            : "metadata probe cancelled");
    }
    if (info.format.channels == 0) info.format.channels = 2;
    if (info.format.bits_per_sample != 16 && info.format.bits_per_sample != 24 &&
        info.format.bits_per_sample != 32) {
        info.format.bits_per_sample = 16;
    }
    info.dsd_source = is_dsd_codec_name(info.codec_name);
    if (info.dsd_source && info.format.sample_rate > 0 &&
        info.format.sample_rate <= std::numeric_limits<std::uint32_t>::max() / 8U) {
        info.dsd_sample_rate = info.format.sample_rate * 8U;
    }
    info.lossless = info.lossless || codec_is_lossless(info.codec_name, path);
    info.source_format = info.format;
    info.source_total_samples_per_channel = info.total_samples_per_channel;
    info.source_sample_extent_kind = info.sample_extent_kind;
    info.source_sample_extent_source = info.sample_extent_source;
    info.source_exact_presentation_drain_policy = info.sample_extent_drain_policy;

    const std::uint32_t source_rate = info.format.sample_rate;
    SampleExtent source_extent;
    source_extent.samples = info.total_samples_per_channel;
    source_extent.kind = info.sample_extent_kind;
    source_extent.source = info.sample_extent_source;
    source_extent.exact_presentation_drain_policy = info.sample_extent_drain_policy;
    SampleExtent output_extent = source_extent;
    if (forced_output_sample_rate > 0 && source_rate > 0 &&
        forced_output_sample_rate != source_rate) {
        output_extent = transform_sample_extent_for_output(
            source_extent, source_rate, forced_output_sample_rate);
        info.total_samples_per_channel = output_extent.samples;
        info.sample_extent_kind = output_extent.kind;
        info.sample_extent_source = output_extent.source;
        info.sample_extent_drain_policy = output_extent.exact_presentation_drain_policy;
    }
    info.presentation_end_kind =
        presentation_end_kind_for_output(
            source_extent,
            output_extent,
            info.source_supports_trusted_decoder_eof);
    if (forced_output_sample_rate > 0) {
        info.format.sample_rate = forced_output_sample_rate;
    }
    if (forced_output_bits_per_sample == 16 || forced_output_bits_per_sample == 24 ||
        forced_output_bits_per_sample == 32) {
        info.format.bits_per_sample = forced_output_bits_per_sample;
    }
    return info;
}

ExternalAudioInfo ExternalAudioDecoder::probe_info(const std::string& path,
                                                    std::uint32_t forced_output_sample_rate,
                                                    std::uint16_t forced_output_bits_per_sample) {
    ExternalAudioInfo info = probe_metadata(path, forced_output_sample_rate,
                                            forced_output_bits_per_sample, nullptr);
    info.tags = GenericTags{};
    return info;
}

void ExternalAudioDecoder::set_known_info(const ExternalAudioInfo& info) {
    known_info_ = info;
    have_known_info_ = true;
}

ExternalAudioInfo ExternalAudioDecoder::effective_probe_info(const std::string& path) const {
    if (have_known_info_) {
        return known_info_;
    }
    return probe_info(path, forced_output_sample_rate_, forced_output_bits_per_sample_);
}

void ExternalAudioDecoder::close_decoder() {
    if (!impl_) return;
    if (impl_->swr_context != nullptr) {
        swr_free(&impl_->swr_context);
    }
    if (impl_->frame != nullptr) {
        av_frame_free(&impl_->frame);
    }
    if (impl_->packet != nullptr) {
        av_packet_free(&impl_->packet);
    }
    if (impl_->codec_context != nullptr) {
        avcodec_free_context(&impl_->codec_context);
    }
    if (impl_->format_context != nullptr) {
        avformat_close_input(&impl_->format_context);
    }
    impl_->audio_stream_index = -1;
    impl_->configured_input_rate = 0;
    impl_->configured_input_format = AV_SAMPLE_FMT_NONE;
    impl_->configured_channels = 0;
    impl_->clear_configured_input_layout();
    impl_->reset_decode_state();
    opened_ = false;
}

namespace {

template <typename DecoderImpl>
std::uint8_t* prepare_conversion_buffer(DecoderImpl& impl,
                                        const AudioFormat& output_format,
                                        std::size_t sample_count) {
    if (output_format.bits_per_sample <= 16) {
        impl.conversion_buffer_s16.resize(sample_count);
        return reinterpret_cast<std::uint8_t*>(impl.conversion_buffer_s16.data());
    }
    impl.conversion_buffer_s32.resize(sample_count);
    return reinterpret_cast<std::uint8_t*>(impl.conversion_buffer_s32.data());
}

template <typename DecoderImpl>
void append_conversion_buffer(DecoderImpl& impl,
                              const AudioFormat& output_format,
                              std::size_t sample_count) {
    const std::size_t old_size = impl.pending_samples.size();
    impl.pending_samples.resize(old_size + sample_count);
    if (output_format.bits_per_sample <= 16) {
        for (std::size_t i = 0; i < sample_count; ++i) {
            impl.pending_samples[old_size + i] = impl.conversion_buffer_s16[i];
        }
    } else {
        const int shift = output_format.bits_per_sample == 24 ? 8 : 0;
        for (std::size_t i = 0; i < sample_count; ++i) {
            const std::int32_t input = impl.conversion_buffer_s32[i];
            impl.pending_samples[old_size + i] = shift > 0 ? (input >> shift) : input;
        }
    }
}

template <typename DecoderImpl>
int drain_resampler_chunk(DecoderImpl& impl, const AudioFormat& output_format) {
    if (impl.swr_context == nullptr) {
        return 0;
    }
    const int input_rate = impl.configured_input_rate > 0
        ? impl.configured_input_rate
        : static_cast<int>(output_format.sample_rate);
    const int capacity = static_cast<int>(av_rescale_rnd(
        swr_get_delay(impl.swr_context, input_rate),
        output_format.sample_rate,
        input_rate,
        AV_ROUND_UP));
    if (capacity <= 0) {
        return 0;
    }

    const std::size_t sample_capacity =
        static_cast<std::size_t>(capacity) * output_format.channels;
    std::uint8_t* output_planes[1] = {
        prepare_conversion_buffer(impl, output_format, sample_capacity)
    };
    const int frames = swr_convert(
        impl.swr_context, output_planes, capacity, nullptr, 0);
    if (frames < 0) {
        throw std::runtime_error(
            "Cannot drain FFmpeg API resampler: " + av_error_string(frames));
    }
    if (frames > 0) {
        append_conversion_buffer(
            impl,
            output_format,
            static_cast<std::size_t>(frames) * output_format.channels);
    }
    return frames;
}

template <typename DecoderImpl>
void drain_resampler_fully(DecoderImpl& impl, const AudioFormat& output_format) {
    if (impl.swr_context == nullptr) {
        impl.swr_drained = true;
        return;
    }
    constexpr unsigned int kMaximumDrainIterations = 1024;
    for (unsigned int iteration = 0; iteration < kMaximumDrainIterations; ++iteration) {
        if (drain_resampler_chunk(impl, output_format) == 0) {
            impl.swr_drained = true;
            return;
        }
    }
    throw std::runtime_error("FFmpeg API resampler did not finish draining");
}

class SwrContextGuard final {
public:
    SwrContextGuard() = default;
    ~SwrContextGuard() {
        swr_free(&context_);
    }

    SwrContextGuard(const SwrContextGuard&) = delete;
    SwrContextGuard& operator=(const SwrContextGuard&) = delete;

    SwrContext* get() const noexcept {
        return context_;
    }

    void reset(SwrContext* context) noexcept {
        if (context_ != context) {
            swr_free(&context_);
            context_ = context;
        }
    }

    SwrContext* release() noexcept {
        SwrContext* context = context_;
        context_ = nullptr;
        return context;
    }

private:
    SwrContext* context_ = nullptr;
};

#if PCMTP_FFMPEG_HAS_MODERN_CHANNEL_LAYOUT
class ChannelLayoutGuard final {
public:
    ChannelLayoutGuard() = default;
    explicit ChannelLayoutGuard(AVChannelLayout layout) noexcept
        : layout_(layout) {}

    ~ChannelLayoutGuard() {
        av_channel_layout_uninit(&layout_);
    }

    ChannelLayoutGuard(const ChannelLayoutGuard&) = delete;
    ChannelLayoutGuard& operator=(const ChannelLayoutGuard&) = delete;

    AVChannelLayout* get() noexcept {
        return &layout_;
    }

    const AVChannelLayout* get() const noexcept {
        return &layout_;
    }

    AVChannelLayout release() noexcept {
        AVChannelLayout layout = layout_;
        layout_ = AVChannelLayout{};
        return layout;
    }

private:
    AVChannelLayout layout_{};
};

AVChannelLayout resolved_input_layout(const AVFrame* frame,
                                      const AVCodecContext* codec_context,
                                      int channels) {
    AVChannelLayout layout{};
    int result = 0;
    if (frame != nullptr && frame->ch_layout.nb_channels > 0) {
        result = av_channel_layout_copy(&layout, &frame->ch_layout);
    } else if (codec_context != nullptr && codec_context->ch_layout.nb_channels > 0) {
        result = av_channel_layout_copy(&layout, &codec_context->ch_layout);
    } else {
        av_channel_layout_default(&layout, channels);
    }
    if (result < 0) {
        av_channel_layout_uninit(&layout);
        throw std::runtime_error(
            "Cannot copy FFmpeg API channel layout: " + av_error_string(result));
    }
    return layout;
}
#else
std::uint64_t resolved_input_layout(const AVFrame* frame,
                                    const AVCodecContext* codec_context,
                                    int channels) {
    std::uint64_t layout = frame != nullptr
        ? static_cast<std::uint64_t>(frame->channel_layout)
        : 0;
    if (layout == 0 && codec_context != nullptr) {
        layout = static_cast<std::uint64_t>(codec_context->channel_layout);
    }
    if (layout == 0) {
        layout = static_cast<std::uint64_t>(av_get_default_channel_layout(channels));
    }
    return layout;
}
#endif

template <typename DecoderImpl>
void configure_resampler(DecoderImpl& impl,
                         const AVFrame* frame,
                         const AudioFormat& output_format,
                         const AudioFormat& source_format,
                         bool dsd_source,
                         std::uint32_t forced_output_sample_rate,
                         std::uint16_t forced_output_bits,
                         const std::string& resample_quality,
                         const std::string& bitdepth_quality) {
    const int input_rate =
        frame->sample_rate > 0 ? frame->sample_rate : impl.codec_context->sample_rate;
    const AVSampleFormat input_format = static_cast<AVSampleFormat>(frame->format);
    const int channels = frame_channels(frame, impl.codec_context);
    if (input_rate <= 0 || channels <= 0 || input_format == AV_SAMPLE_FMT_NONE) {
        throw std::runtime_error("Invalid decoded audio frame format");
    }

#if PCMTP_FFMPEG_HAS_MODERN_CHANNEL_LAYOUT
    ChannelLayoutGuard input_layout(
        resolved_input_layout(frame, impl.codec_context, channels));
    const bool layout_matches =
        impl.configured_input_layout_valid &&
        av_channel_layout_compare(
            &impl.configured_input_layout, input_layout.get()) == 0;
#else
    const std::uint64_t input_layout =
        resolved_input_layout(frame, impl.codec_context, channels);
    const bool layout_matches =
        impl.configured_input_layout != 0 &&
        impl.configured_input_layout == input_layout;
#endif

    if (impl.swr_context != nullptr &&
        impl.configured_input_rate == input_rate &&
        impl.configured_input_format == input_format &&
        impl.configured_channels == channels &&
        layout_matches) {
        return;
    }

    if (impl.swr_context != nullptr) {
        if (!impl.seeking) {
            drain_resampler_fully(impl, output_format);
        }
    }

    const AVSampleFormat output_sample_format =
        output_format.bits_per_sample <= 16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
#if PCMTP_FFMPEG_HAS_MODERN_CHANNEL_LAYOUT
    ChannelLayoutGuard output_layout;
    av_channel_layout_default(output_layout.get(), output_format.channels);
    const auto allocate_resampler_context = [&]() -> SwrContext* {
        SwrContext* context = nullptr;
        const int result = swr_alloc_set_opts2(
            &context,
            output_layout.get(),
            output_sample_format,
            static_cast<int>(output_format.sample_rate),
            input_layout.get(),
            input_format,
            input_rate,
            0,
            nullptr);
        if (result < 0) {
            swr_free(&context);
            throw std::runtime_error(
                "Cannot configure FFmpeg API resampler: " + av_error_string(result));
        }
        return context;
    };
#else
    const std::int64_t output_layout =
        av_get_default_channel_layout(output_format.channels);
    const auto allocate_resampler_context = [&]() -> SwrContext* {
        SwrContext* context = swr_alloc_set_opts(
            nullptr,
            output_layout,
            output_sample_format,
            static_cast<int>(output_format.sample_rate),
            static_cast<std::int64_t>(input_layout),
            input_format,
            input_rate,
            0,
            nullptr);
        if (context == nullptr) {
            throw std::runtime_error("Cannot configure FFmpeg API resampler");
        }
        return context;
    };
#endif

    const bool forced_rate_active =
        forced_output_sample_rate > 0 &&
        forced_output_sample_rate != source_format.sample_rate;
    const bool forced_bits_active =
        forced_output_bits > 0 &&
        forced_output_bits != source_format.bits_per_sample;
    const bool quality_filter_active =
        forced_rate_active ||
        (dsd_source ? output_format.bits_per_sample <= 16 : forced_bits_active);

    const auto apply_output_options = [&](SwrContext* context) {
        set_swr_option_int(
            context, "output_sample_bits", output_format.bits_per_sample);
        if (output_format.bits_per_sample <= 16) {
            set_swr_option(
                context, "dither_method", dither_method(bitdepth_quality));
        }
    };

    SwrContextGuard new_context;
    new_context.reset(allocate_resampler_context());
    bool initialized = false;
    ResamplerRuntimeKind runtime_kind = ResamplerRuntimeKind::NotUsed;
    int soxr_failure = 0;
    if (quality_filter_active) {
        const std::string precision =
            std::to_string(soxr_precision(resample_quality));
        int option_result = av_opt_set(new_context.get(), "resampler", "soxr", 0);
        if (option_result >= 0) {
            option_result = av_opt_set(
                new_context.get(), "precision", precision.c_str(), 0);
        }
        if (option_result >= 0) {
            option_result = av_opt_set_int(new_context.get(), "cheby", 0, 0);
        }
        if (option_result >= 0) {
            apply_output_options(new_context.get());
            const int init_result = swr_init(new_context.get());
            if (init_result >= 0) {
                initialized = true;
                runtime_kind = ResamplerRuntimeKind::SoXr;
            } else {
                soxr_failure = init_result;
            }
        } else {
            soxr_failure = option_result;
        }

        if (!initialized) {
            std::string warning =
                "FFmpeg SoXr resampler unavailable; falling back to the "
                "built-in SWR engine";
            if (soxr_failure < 0) {
                warning += ": " + av_error_string(soxr_failure);
            }
            Logger::instance().warning(warning);
            new_context.reset(allocate_resampler_context());
            apply_output_options(new_context.get());
        }
    }

    if (!initialized) {
        const int init_result = swr_init(new_context.get());
        if (init_result < 0) {
            throw std::runtime_error(
                "Cannot initialize FFmpeg API resampler: " +
                av_error_string(init_result));
        }
        if (quality_filter_active) {
            runtime_kind = ResamplerRuntimeKind::FfmpegSwr;
        }
    }

#if PCMTP_FFMPEG_HAS_MODERN_CHANNEL_LAYOUT
    ChannelLayoutGuard retained_input_layout;
    const int copy_result =
        av_channel_layout_copy(retained_input_layout.get(), input_layout.get());
    if (copy_result < 0) {
        throw std::runtime_error(
            "Cannot retain FFmpeg API channel layout: " +
            av_error_string(copy_result));
    }
#endif

    swr_free(&impl.swr_context);
    impl.clear_configured_input_layout();
    impl.swr_context = new_context.release();
    impl.configured_input_rate = input_rate;
    impl.configured_input_format = input_format;
    impl.configured_channels = channels;
#if PCMTP_FFMPEG_HAS_MODERN_CHANNEL_LAYOUT
    impl.configured_input_layout = retained_input_layout.release();
    impl.configured_input_layout_valid = true;
#else
    impl.configured_input_layout = input_layout;
#endif
    impl.swr_drained = false;
    impl.resampler_runtime_kind.store(runtime_kind, std::memory_order_release);
}

std::optional<std::uint64_t> frame_timestamp_sample(
    const AVFrame* frame,
    const AVStream* stream,
    std::uint32_t output_rate,
    std::uint64_t presentation_timeline_origin_sample) {
    if (frame == nullptr || stream == nullptr || output_rate == 0 ||
        output_rate > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const std::int64_t timestamp = frame->best_effort_timestamp != AV_NOPTS_VALUE
        ? frame->best_effort_timestamp
        : frame->pts;
    if (timestamp == AV_NOPTS_VALUE) {
        return std::nullopt;
    }
    std::int64_t adjusted = timestamp;
    if (stream->start_time != AV_NOPTS_VALUE) {
        adjusted -= stream->start_time;
    }
    if (adjusted < 0) {
        adjusted = 0;
    }
    const std::int64_t sample = av_rescale_q(
        adjusted,
        stream->time_base,
        AVRational{1, static_cast<int>(output_rate)});
    if (sample < 0) {
        return std::nullopt;
    }
    const std::uint64_t absolute_sample = static_cast<std::uint64_t>(sample);
    if (absolute_sample < presentation_timeline_origin_sample) {
        return 0;
    }
    return absolute_sample - presentation_timeline_origin_sample;
}

std::uint64_t frame_duration_at_output_rate(const AVFrame* frame,
                                            const AVCodecContext* codec_context,
                                            std::uint32_t output_rate) {
    if (frame == nullptr || frame->nb_samples <= 0 || output_rate == 0) {
        return 0;
    }
    const int input_rate = frame->sample_rate > 0
        ? frame->sample_rate
        : (codec_context != nullptr ? codec_context->sample_rate : 0);
    if (input_rate <= 0 ||
        output_rate > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return 0;
    }
    const std::int64_t duration = av_rescale_q(
        frame->nb_samples,
        AVRational{1, input_rate},
        AVRational{1, static_cast<int>(output_rate)});
    return duration > 0 ? static_cast<std::uint64_t>(duration) : 0;
}

template <typename DecoderImpl>
std::size_t append_converted_frame(DecoderImpl& impl,
                            const AVFrame* frame,
                            const AudioFormat& output_format,
                            const AudioFormat& source_format,
                            bool dsd_source,
                            std::uint32_t forced_output_sample_rate,
                            std::uint16_t forced_output_bits,
                            const std::string& resample_quality,
                            const std::string& bitdepth_quality) {
    configure_resampler(
        impl,
        frame,
        output_format,
        source_format,
        dsd_source,
        forced_output_sample_rate,
        forced_output_bits,
        resample_quality,
        bitdepth_quality);
    const int input_rate =
        frame->sample_rate > 0 ? frame->sample_rate : impl.codec_context->sample_rate;
    const AVSampleFormat input_format = static_cast<AVSampleFormat>(frame->format);
    const int channels = frame_channels(frame, impl.codec_context);
    const std::int64_t delay = swr_get_delay(impl.swr_context, input_rate);
    const int output_capacity = static_cast<int>(av_rescale_rnd(
        delay + frame->nb_samples,
        output_format.sample_rate,
        input_rate,
        AV_ROUND_UP));
    if (output_capacity <= 0) {
        return 0;
    }

    const std::size_t sample_capacity =
        static_cast<std::size_t>(output_capacity) * output_format.channels;
    std::uint8_t* output_planes[1] = {
        prepare_conversion_buffer(impl, output_format, sample_capacity)
    };

    const int input_plane_count =
        av_sample_fmt_is_planar(input_format) != 0 ? channels : 1;
    impl.input_planes.resize(static_cast<std::size_t>(input_plane_count));
    for (int plane = 0; plane < input_plane_count; ++plane) {
        impl.input_planes[static_cast<std::size_t>(plane)] =
            frame->extended_data[plane];
    }

    const int output_frames = swr_convert(
        impl.swr_context,
        output_planes,
        output_capacity,
        impl.input_planes.data(),
        frame->nb_samples);
    if (output_frames < 0) {
        throw std::runtime_error(
            "FFmpeg API audio conversion failed: " +
            av_error_string(output_frames));
    }
    const std::size_t sample_count =
        static_cast<std::size_t>(output_frames) * output_format.channels;
    append_conversion_buffer(impl, output_format, sample_count);
    return sample_count;
}

template <typename DecoderImpl>
void append_resampler_drain(DecoderImpl& impl, const AudioFormat& output_format) {
    if (impl.swr_context == nullptr || impl.swr_drained) {
        impl.swr_drained = true;
        return;
    }
    drain_resampler_fully(impl, output_format);
}

constexpr unsigned int kMaxConsecutiveRecoverableDecodeErrors = 32;

template <typename DecoderImpl>
bool recover_from_invalid_data(DecoderImpl& impl,
                               const char* stage,
                               int error_code,
                               const std::string& path) {
    if (error_code != AVERROR_INVALIDDATA) {
        return false;
    }
    ++impl.consecutive_decode_errors;
    if (impl.consecutive_decode_errors == 1 ||
        (impl.consecutive_decode_errors % 8) == 0) {
        Logger::instance().warning(
            std::string("FFmpeg API recovered from invalid ") + stage +
            " data (" + std::to_string(impl.consecutive_decode_errors) +
            "/" + std::to_string(kMaxConsecutiveRecoverableDecodeErrors) +
            "): " + path);
    }
    return impl.consecutive_decode_errors <=
           kMaxConsecutiveRecoverableDecodeErrors;
}
} // namespace

void ExternalAudioDecoder::open_decoder(std::uint64_t sample_index) {
    close_decoder();
    try {
        impl_->interrupt.abort_requested = &impl_->abort_requested;
        impl_->format_context = open_input_context(path_, &impl_->interrupt, true);
        impl_->audio_stream_index = first_audio_stream(impl_->format_context);
        AVStream* stream = impl_->format_context->streams[impl_->audio_stream_index];
        AVCodecParameters* parameters = stream->codecpar;
        const AVCodec* decoder = avcodec_find_decoder(parameters->codec_id);
        if (decoder == nullptr) {
            close_decoder();
            throw std::runtime_error("Decoder for " + codec_name_ +
                                     " is unavailable in the installed FFmpeg build");
        }
        impl_->codec_context = avcodec_alloc_context3(decoder);
        if (impl_->codec_context == nullptr) {
            close_decoder();
            throw std::runtime_error("Cannot allocate FFmpeg decoder context");
        }
        int result = avcodec_parameters_to_context(impl_->codec_context, parameters);
        if (result < 0) {
            close_decoder();
            throw std::runtime_error("Cannot initialize FFmpeg decoder parameters: " + av_error_string(result));
        }
        impl_->codec_context->pkt_timebase = stream->time_base;
        impl_->codec_context->thread_count = 0;
        result = avcodec_open2(impl_->codec_context, decoder, nullptr);
        if (result < 0) {
            close_decoder();
            throw std::runtime_error("Cannot open FFmpeg decoder: " + av_error_string(result));
        }
        impl_->packet = av_packet_alloc();
        impl_->frame = av_frame_alloc();
        if (impl_->packet == nullptr || impl_->frame == nullptr) {
            close_decoder();
            throw std::runtime_error("Cannot allocate FFmpeg packet/frame buffers");
        }
        impl_->reset_decode_state();
        opened_ = true;
        reached_eof_ = false;
        current_samples_per_channel_ = 0;
        if (sample_index > 0 && !seek_to_sample(sample_index)) {
            close_decoder();
            throw std::runtime_error("Cannot seek FFmpeg decoder to requested sample");
        }
    } catch (...) {
        close_decoder();
        throw;
    }
}

void ExternalAudioDecoder::open(const std::string& path) {
    open_at_sample(path, 0);
}

void ExternalAudioDecoder::open_at_sample(const std::string& path, std::uint64_t sample_index) {
    path_ = path;
    const ExternalAudioInfo info = effective_probe_info(path);
    format_ = info.format;
    source_format_ = info.source_format.sample_rate > 0 ? info.source_format : info.format;
    presentation_timeline_origin_sample_ = 0;
    if (info.source_presentation_start_known &&
        source_format_.sample_rate > 0 && format_.sample_rate > 0 &&
        format_.sample_rate <=
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
        source_format_.sample_rate <=
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
        info.source_presentation_start_sample <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        const std::int64_t origin = av_rescale_q(
            static_cast<std::int64_t>(info.source_presentation_start_sample),
            AVRational{1, static_cast<int>(source_format_.sample_rate)},
            AVRational{1, static_cast<int>(format_.sample_rate)});
        if (origin > 0) {
            presentation_timeline_origin_sample_ =
                static_cast<std::uint64_t>(origin);
        }
    }
    codec_name_ = info.codec_name;
    presentation_end_kind_ = info.presentation_end_kind;
    dsd_source_ = info.dsd_source;
    total_samples_per_channel_ = info.total_samples_per_channel;
    Logger::instance().debug("ExternalAudioDecoder direct libav format: " +
                             std::to_string(format_.sample_rate) + " Hz / " +
                             std::to_string(format_.bits_per_sample) + "-bit / " +
                             std::to_string(format_.channels) + " ch, total samples/ch=" +
                             std::to_string(total_samples_per_channel_) +
                             ", extent=" + sample_extent_kind_name(info.sample_extent_kind) +
                             " (" + sample_extent_source_name(info.sample_extent_source) + ")" +
                             (sample_index > 0 ? ", start sample/ch=" + std::to_string(sample_index)
                                               : std::string()));
    open_decoder(sample_index);
}

const AudioFormat& ExternalAudioDecoder::format() const {
    return format_;
}

std::size_t ExternalAudioDecoder::read_samples(PcmSample* destination, std::size_t max_samples) {
    if (!opened_ || impl_->format_context == nullptr || impl_->codec_context == nullptr) {
        throw std::runtime_error("Decoder not opened");
    }
    if (destination == nullptr || max_samples == 0) {
        return 0;
    }
    if (impl_->abort_requested.load(std::memory_order_acquire)) {
        reached_eof_ = true;
        return 0;
    }

    const std::size_t channels = std::max<std::size_t>(1, format_.channels);
    max_samples -= max_samples % channels;
    if (max_samples == 0) {
        return 0;
    }
    std::size_t written = 0;
    auto copy_pending = [&]() {
        if (impl_->pending_offset >= impl_->pending_samples.size()) {
            impl_->pending_samples.clear();
            impl_->pending_offset = 0;
            return;
        }
        const std::size_t available = impl_->pending_samples.size() - impl_->pending_offset;
        if (impl_->pending_offset % channels != 0 || available % channels != 0) {
            throw std::runtime_error(
                "FFmpeg decoder queued an incomplete PCM frame");
        }
        std::size_t count = std::min(max_samples - written, available);
        count -= count % channels;
        if (count == 0) return;
        std::copy_n(impl_->pending_samples.data() + impl_->pending_offset, count,
                    destination + written);
        impl_->pending_offset += count;
        written += count;
        current_samples_per_channel_ += count / channels;
        if (impl_->pending_offset >= impl_->pending_samples.size()) {
            impl_->pending_samples.clear();
            impl_->pending_offset = 0;
        }
    };

    copy_pending();
    auto apply_seek_discard = [&](std::size_t pending_before,
                                  std::size_t produced_samples,
                                  std::uint64_t start_sample) {
        const std::uint64_t produced_frames = produced_samples / channels;
        if (!impl_->seeking || produced_frames == 0) {
            return;
        }
        const std::uint64_t frame_end = produced_frames >
                std::numeric_limits<std::uint64_t>::max() - start_sample
            ? std::numeric_limits<std::uint64_t>::max()
            : start_sample + produced_frames;
        if (frame_end <= impl_->seek_target_sample) {
            impl_->pending_samples.resize(pending_before);
            return;
        }
        if (start_sample < impl_->seek_target_sample) {
            const std::uint64_t drop_frames =
                impl_->seek_target_sample - start_sample;
            if (drop_frames > static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max() / channels)) {
                impl_->pending_samples.resize(pending_before);
                return;
            }
            const std::size_t drop_samples =
                static_cast<std::size_t>(drop_frames) * channels;
            if (drop_samples < produced_samples) {
                impl_->pending_samples.erase(
                    impl_->pending_samples.begin() +
                        static_cast<std::ptrdiff_t>(pending_before),
                    impl_->pending_samples.begin() +
                        static_cast<std::ptrdiff_t>(pending_before + drop_samples));
            } else {
                impl_->pending_samples.resize(pending_before);
                return;
            }
        }
        impl_->seeking = false;
        current_samples_per_channel_ = impl_->seek_target_sample;
    };
    auto drain_resampler_output = [&]() {
        const std::size_t pending_before = impl_->pending_samples.size();
        const std::uint64_t start_sample = impl_->output_timeline_sample;
        append_resampler_drain(*impl_, format_);
        const std::size_t drained_samples =
            impl_->pending_samples.size() - pending_before;
        const std::uint64_t drained_frames = drained_samples / channels;
        impl_->output_timeline_sample = drained_frames >
                std::numeric_limits<std::uint64_t>::max() - start_sample
            ? std::numeric_limits<std::uint64_t>::max()
            : start_sample + drained_frames;
        apply_seek_discard(pending_before, drained_samples, start_sample);
    };

    while (written < max_samples && !reached_eof_) {
        if (impl_->abort_requested.load(std::memory_order_acquire)) {
            reached_eof_ = true;
            break;
        }
        int receive_result = avcodec_receive_frame(impl_->codec_context, impl_->frame);
        if (receive_result == 0) {
            if (impl_->abort_requested.load(std::memory_order_acquire)) {
                av_frame_unref(impl_->frame);
                reached_eof_ = true;
                break;
            }
            impl_->consecutive_decode_errors = 0;
            AVStream* stream = impl_->format_context->streams[impl_->audio_stream_index];
            const std::optional<std::uint64_t> timestamp_sample =
                frame_timestamp_sample(impl_->frame,
                                       stream,
                                       format_.sample_rate,
                                       presentation_timeline_origin_sample_);
            if (!impl_->output_timeline_initialized) {
                if (timestamp_sample.has_value()) {
                    impl_->output_timeline_sample = *timestamp_sample;
                }
                impl_->output_timeline_initialized = true;
            }
            if (timestamp_sample.has_value()) {
                if (impl_->expected_input_timeline_known) {
                    const std::uint64_t expected = impl_->expected_input_timeline_sample;
                    const std::uint64_t actual = *timestamp_sample;
                    const std::uint64_t difference = actual > expected
                        ? actual - expected
                        : expected - actual;
                    if (difference > 2 && !impl_->timestamp_discontinuity_reported) {
                        Logger::instance().warning(
                            "FFmpeg API input timestamp discontinuity; "
                            "continuing the established output PCM timeline: " + path_);
                        impl_->timestamp_discontinuity_reported = true;
                    }
                }
                const std::uint64_t duration = frame_duration_at_output_rate(
                    impl_->frame, impl_->codec_context, format_.sample_rate);
                impl_->expected_input_timeline_sample =
                    duration > std::numeric_limits<std::uint64_t>::max() - *timestamp_sample
                    ? std::numeric_limits<std::uint64_t>::max()
                    : *timestamp_sample + duration;
                impl_->expected_input_timeline_known = duration > 0;
            } else {
                impl_->expected_input_timeline_known = false;
            }
            const std::uint64_t start_sample = impl_->output_timeline_sample;
            const std::size_t pending_before = impl_->pending_samples.size();
            const std::size_t produced_samples = append_converted_frame(
                *impl_,
                impl_->frame,
                format_,
                source_format_,
                dsd_source_,
                forced_output_sample_rate_,
                forced_output_bits_per_sample_,
                resample_quality_,
                bitdepth_quality_);
            const std::uint64_t produced_frames = produced_samples / channels;
            impl_->output_timeline_sample = produced_frames >
                    std::numeric_limits<std::uint64_t>::max() - start_sample
                ? std::numeric_limits<std::uint64_t>::max()
                : start_sample + produced_frames;
            apply_seek_discard(pending_before, produced_samples, start_sample);
            av_frame_unref(impl_->frame);
            copy_pending();
            continue;
        }
        if (receive_result == AVERROR_EOF) {
            impl_->decoder_eof = true;
            drain_resampler_output();
            copy_pending();
            if (impl_->pending_samples.empty() && impl_->swr_drained) {
                reached_eof_ = true;
            }
            continue;
        }
        if (receive_result != AVERROR(EAGAIN)) {
            av_frame_unref(impl_->frame);
            if (impl_->abort_requested.load(std::memory_order_acquire)) {
                reached_eof_ = true;
                break;
            }
            if (recover_from_invalid_data(
                    *impl_, "decoded frame", receive_result, path_)) {
                continue;
            }
            throw std::runtime_error(
                "FFmpeg API decode failed: " + av_error_string(receive_result));
        }

        if (impl_->input_eof) {
            if (!impl_->decoder_flush_sent) {
                const int flush_result = avcodec_send_packet(impl_->codec_context, nullptr);
                if (flush_result == 0 || flush_result == AVERROR_EOF) {
                    impl_->decoder_flush_sent = true;
                } else if (flush_result != AVERROR(EAGAIN)) {
                    throw std::runtime_error("Cannot flush FFmpeg decoder: " + av_error_string(flush_result));
                }
                continue;
            }
            impl_->decoder_eof = true;
            drain_resampler_output();
            copy_pending();
            if (impl_->pending_samples.empty() && impl_->swr_drained) {
                reached_eof_ = true;
            }
            continue;
        }

        if (!impl_->packet_pending) {
            while (!impl_->packet_pending && !impl_->input_eof) {
                if (impl_->abort_requested.load(std::memory_order_acquire)) {
                    reached_eof_ = true;
                    break;
                }
                const int read_result = av_read_frame(impl_->format_context, impl_->packet);
                if (read_result == AVERROR_EOF) {
                    impl_->input_eof = true;
                    break;
                }
                if (read_result < 0) {
                    if (impl_->abort_requested.load(std::memory_order_acquire)) {
                        av_packet_unref(impl_->packet);
                        reached_eof_ = true;
                        break;
                    }
                    av_packet_unref(impl_->packet);
                    if (recover_from_invalid_data(
                            *impl_, "demux", read_result, path_)) {
                        continue;
                    }
                    throw std::runtime_error(
                        "FFmpeg API demux failed: " + av_error_string(read_result));
                }
                if (impl_->packet->stream_index != impl_->audio_stream_index) {
                    av_packet_unref(impl_->packet);
                    continue;
                }
                impl_->packet_pending = true;
            }
        }
        if (impl_->packet_pending) {
            const int send_result = avcodec_send_packet(impl_->codec_context, impl_->packet);
            if (send_result == 0) {
                av_packet_unref(impl_->packet);
                impl_->packet_pending = false;
            } else if (send_result == AVERROR_EOF) {
                av_packet_unref(impl_->packet);
                impl_->packet_pending = false;
                impl_->input_eof = true;
            } else if (send_result != AVERROR(EAGAIN)) {
                av_packet_unref(impl_->packet);
                impl_->packet_pending = false;
                if (impl_->abort_requested.load(std::memory_order_acquire)) {
                    reached_eof_ = true;
                    break;
                }
                if (recover_from_invalid_data(
                        *impl_, "packet", send_result, path_)) {
                    continue;
                }
                throw std::runtime_error(
                    "FFmpeg API packet decode failed: " +
                    av_error_string(send_result));
            }
        }
    }
    return written;
}

bool ExternalAudioDecoder::eof() const {
    return reached_eof_;
}

std::uint64_t ExternalAudioDecoder::total_samples_per_channel() const {
    return total_samples_per_channel_;
}

std::string ExternalAudioDecoder::source_path() const {
    return path_;
}

PresentationEndKind ExternalAudioDecoder::presentation_end_kind() const noexcept {
    if (have_known_info_) {
        return known_info_.presentation_end_kind;
    }
    return presentation_end_kind_;
}

ResamplerRuntimeKind ExternalAudioDecoder::resampler_runtime_kind() const noexcept {
    if (!opened_ || impl_ == nullptr) {
        return ResamplerRuntimeKind::NotUsed;
    }
    const ResamplerRuntimeKind runtime_kind =
        impl_->resampler_runtime_kind.load(std::memory_order_acquire);
    if (runtime_kind != ResamplerRuntimeKind::NotUsed) {
        return runtime_kind;
    }

    const bool forced_rate_active =
        forced_output_sample_rate_ > 0 &&
        forced_output_sample_rate_ != source_format_.sample_rate;
    const bool forced_bits_active =
        forced_output_bits_per_sample_ > 0 &&
        forced_output_bits_per_sample_ != source_format_.bits_per_sample;
    const bool quality_filter_active =
        forced_rate_active ||
        (dsd_source_ ? format_.bits_per_sample <= 16 : forced_bits_active);
    return quality_filter_active
        ? ResamplerRuntimeKind::Initializing
        : ResamplerRuntimeKind::NotUsed;
}

void ExternalAudioDecoder::request_abort() {
    if (impl_ != nullptr) {
        impl_->abort_requested.store(true, std::memory_order_release);
    }
}

bool ExternalAudioDecoder::seek_to_sample(std::uint64_t sample_index) {
    if (!opened_ || impl_->format_context == nullptr || impl_->codec_context == nullptr ||
        impl_->audio_stream_index < 0 || format_.sample_rate == 0) {
        return false;
    }
    if (total_samples_per_channel_ > 0) {
        sample_index = std::min(sample_index, total_samples_per_channel_);
    }
    const std::string extension = to_lower_extension(path_);
    double preroll_seconds = kDefaultSeekPrerollSeconds;
    if (extension == ".ape") preroll_seconds = kApeSeekPrerollSeconds;
    else if (extension == ".aac") preroll_seconds = kRawAacSeekPrerollSeconds;
    else if ((extension == ".m4a" || extension == ".m4r") && codec_name_ == "alac") {
        preroll_seconds = kAlacSeekPrerollSeconds;
    }
    const std::uint64_t preroll_samples = static_cast<std::uint64_t>(
        std::llround(preroll_seconds * static_cast<double>(format_.sample_rate)));
    const std::uint64_t seek_start_sample = sample_index > preroll_samples
        ? sample_index - preroll_samples : 0;
    AVStream* stream = impl_->format_context->streams[impl_->audio_stream_index];
    if (seek_start_sample >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        presentation_timeline_origin_sample_ >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                seek_start_sample) {
        return false;
    }
    const std::uint64_t absolute_seek_start =
        presentation_timeline_origin_sample_ + seek_start_sample;
    std::int64_t timestamp = av_rescale_q(
        static_cast<std::int64_t>(absolute_seek_start),
        AVRational{1, static_cast<int>(format_.sample_rate)},
        stream->time_base);
    if (stream->start_time != AV_NOPTS_VALUE) {
        if ((stream->start_time > 0 &&
             timestamp > std::numeric_limits<std::int64_t>::max() -
                 stream->start_time) ||
            (stream->start_time < 0 &&
             timestamp < std::numeric_limits<std::int64_t>::min() -
                 stream->start_time)) {
            return false;
        }
        timestamp += stream->start_time;
    }
    const int result = avformat_seek_file(
        impl_->format_context, impl_->audio_stream_index,
        std::numeric_limits<std::int64_t>::min(), timestamp, timestamp, AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
        Logger::instance().debug("Direct libav demux seek unavailable for " + path_ +
                                 "; falling back to decoded discard from the beginning: " +
                                 av_error_string(result));
        open_decoder(0);
        impl_->seeking = sample_index > 0;
        impl_->seek_target_sample = sample_index;
        impl_->output_timeline_sample = 0;
        impl_->output_timeline_initialized = false;
        current_samples_per_channel_ = sample_index;
        reached_eof_ = false;
        return true;
    }
    avcodec_flush_buffers(impl_->codec_context);
    if (impl_->packet != nullptr) {
        av_packet_unref(impl_->packet);
    }
    if (impl_->swr_context != nullptr) {
        swr_free(&impl_->swr_context);
    }
    impl_->configured_input_rate = 0;
    impl_->configured_input_format = AV_SAMPLE_FMT_NONE;
    impl_->configured_channels = 0;
    impl_->clear_configured_input_layout();
    impl_->reset_decode_state();
    impl_->seeking = true;
    impl_->seek_target_sample = sample_index;
    impl_->output_timeline_sample = seek_start_sample;
    impl_->output_timeline_initialized = false;
    current_samples_per_channel_ = sample_index;
    reached_eof_ = false;
    Logger::instance().debug("ExternalAudioDecoder direct libav seek at sample/ch=" +
                             std::to_string(sample_index) + " for: " + path_);
    return true;
}

#undef PCMTP_FFMPEG_HAS_MOV_AAC_INDEX_API
#undef PCMTP_FFMPEG_HAS_MODERN_CHANNEL_LAYOUT

} // namespace pcmtp
