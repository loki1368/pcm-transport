// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/playlist/MediaProbe.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>

#include "pcmtp/decoder/ExternalAudioDecoder.hpp"
#include "pcmtp/decoder/FlacStreamDecoder.hpp"
#include "pcmtp/stream/StreamAudioDecoder.hpp"
#include "pcmtp/playlist/Mp3FastProbe.hpp"
#include "pcmtp/util/Logger.hpp"

namespace pcmtp {
namespace {

std::string lower_extension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    std::string extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension;
}

void fill_result_from_external_info(const ExternalAudioInfo& info, MediaProbeResult* result) {
    result->format = info.format;
    result->total_samples_per_channel = info.total_samples_per_channel;
    result->source_supports_trusted_decoder_eof =
        info.source_supports_trusted_decoder_eof;
    result->sample_extent_drain_policy = info.sample_extent_drain_policy;
    result->source_presentation_start_known =
        info.source_presentation_start_known;
    result->source_presentation_start_sample =
        info.source_presentation_start_sample;
    result->sample_extent_kind = info.sample_extent_kind;
    result->sample_extent_source = info.sample_extent_source;
    result->tags = info.tags;
    result->codec_name = info.codec_name;
    result->native_decode = false;
    result->lossless = info.lossless;
    result->dsd_source = info.dsd_source;
    result->dsd_sample_rate = info.dsd_sample_rate;
    result->probe_backend = info.probe_backend.empty() ? "libavformat" : info.probe_backend;
}

std::uint64_t elapsed_microseconds(std::chrono::steady_clock::time_point started) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    return elapsed.count() > 0 ? static_cast<std::uint64_t>(elapsed.count()) : 0;
}

} // namespace

MediaProbeResult probe_media_file(const std::string& path,
                                  ProbeCancellation* probe_cancellation) {
    const auto started = std::chrono::steady_clock::now();
    MediaProbeResult result;
    try {
        const std::string extension = lower_extension(path);
        if (extension == ".flac") {
            const FlacFileProbe flac_probe = FlacStreamDecoder::probe_file(path);
            if (flac_probe.valid) {
                result.format = flac_probe.format;
                result.total_samples_per_channel = flac_probe.total_samples_per_channel;
                result.sample_extent_kind = flac_probe.total_samples_per_channel > 0
                    ? SampleExtentKind::ExactPresentationSpan
                    : SampleExtentKind::Unknown;
                result.sample_extent_source = flac_probe.total_samples_per_channel > 0
                    ? SampleExtentSource::NativeHeader
                    : SampleExtentSource::None;
                result.tags.title = flac_probe.tags.title;
                result.tags.artist = flac_probe.tags.artist;
                result.tags.album = flac_probe.tags.album;
                result.tags.track_number = flac_probe.tags.track_number;
                result.codec_name = "flac";
                result.native_decode = true;
                result.lossless = true;
                result.probe_backend = "flac-native";
                result.success = true;
                result.probe_elapsed_microseconds = elapsed_microseconds(started);
                return result;
            }
        }

        if (extension == ".mp3") {
            const Mp3FastProbeResult mp3_probe = probe_mp3_fast(path);
            if (mp3_probe.status == Mp3FastProbeStatus::Complete) {
                fill_result_from_external_info(mp3_probe.info, &result);
                result.success = true;
                result.probe_elapsed_microseconds = elapsed_microseconds(started);
                return result;
            }
            if (!mp3_probe.diagnostic.empty()) {
                Logger::instance().debug("MP3 fast probe fallback: " + path +
                                         " (" + mp3_probe.diagnostic + ")");
            }
        }

        const ExternalAudioInfo info = StreamAudioDecoder::is_stream_uri(path)
            ? StreamAudioDecoder::probe_metadata(path, 0, 0, probe_cancellation)
            : ExternalAudioDecoder::probe_metadata(path, 0, 0, probe_cancellation);
        fill_result_from_external_info(info, &result);
        if (result.codec_name.empty() || result.format.sample_rate == 0 ||
            result.format.channels == 0) {
            result.error = "metadata probe returned no usable audio stream";
            result.probe_elapsed_microseconds = elapsed_microseconds(started);
            return result;
        }
        result.success = true;
    } catch (const std::exception& ex) {
        result.error = ex.what();
    } catch (...) {
        result.error = "unknown metadata probe error";
    }
    result.probe_elapsed_microseconds = elapsed_microseconds(started);
    return result;
}

} // namespace pcmtp
