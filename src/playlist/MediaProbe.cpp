#include "pcmtp/playlist/MediaProbe.hpp"

#include <algorithm>
#include <cctype>
#include <exception>

#include "pcmtp/decoder/FlacStreamDecoder.hpp"

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

bool is_wav_extension(const std::string& extension) {
    return extension == ".wav" || extension == ".wave" || extension == ".bwf";
}

void fill_result_from_external_info(const ExternalAudioInfo& info, MediaProbeResult* result) {
    result->format = info.format;
    result->total_samples_per_channel = info.total_samples_per_channel;
    result->tags = info.tags;
    result->codec_name = info.codec_name;
    result->native_decode = false;
    result->lossless = info.lossless;
    result->dsd_source = info.dsd_source;
    result->dsd_sample_rate = info.dsd_sample_rate;
}

} // namespace

MediaProbeResult probe_media_file(const std::string& path,
                                  ManagedSubprocess* probe_process,
                                  bool background_priority) {
    MediaProbeResult result;
    try {
        const std::string extension = lower_extension(path);
        if (extension == ".flac") {
            const FlacFileProbe flac_probe = FlacStreamDecoder::probe_file(path);
            if (flac_probe.valid) {
                result.format = flac_probe.format;
                result.total_samples_per_channel = flac_probe.total_samples_per_channel;
                result.tags.title = flac_probe.tags.title;
                result.tags.artist = flac_probe.tags.artist;
                result.tags.track_number = flac_probe.tags.track_number;
                result.codec_name = "flac";
                result.native_decode = true;
                result.lossless = true;
                result.success = true;
                return result;
            }
        }

        const ExternalAudioInfo info = ExternalAudioDecoder::probe_metadata(
            path, 0, 0, background_priority, probe_process);
        fill_result_from_external_info(info, &result);
        if (result.codec_name.empty() || result.format.sample_rate == 0 ||
            result.format.channels == 0) {
            result.error = "metadata probe returned no usable audio stream";
            return result;
        }
        result.success = true;
    } catch (const std::exception& ex) {
        result.error = ex.what();
    } catch (...) {
        result.error = "unknown metadata probe error";
    }
    return result;
}

bool try_probe_media_file_fast(const std::string& path, MediaProbeResult* out_result) {
    if (out_result == nullptr) {
        return false;
    }
    *out_result = MediaProbeResult{};

    const std::string extension = lower_extension(path);
    if (extension == ".flac") {
        const FlacFileProbe flac_probe = FlacStreamDecoder::probe_file(path);
        if (!flac_probe.valid) {
            return false;
        }
        out_result->format = flac_probe.format;
        out_result->total_samples_per_channel = flac_probe.total_samples_per_channel;
        out_result->tags.title = flac_probe.tags.title;
        out_result->tags.artist = flac_probe.tags.artist;
        out_result->tags.track_number = flac_probe.tags.track_number;
        out_result->codec_name = "flac";
        out_result->native_decode = true;
        out_result->lossless = true;
        out_result->success = true;
        return true;
    }

    if (!is_wav_extension(extension)) {
        return false;
    }

    ExternalAudioInfo wav_info;
    if (!ExternalAudioDecoder::try_probe_wav_metadata_fast(path, &wav_info)) {
        return false;
    }
    fill_result_from_external_info(wav_info, out_result);
    if (out_result->format.sample_rate == 0 || out_result->format.channels == 0) {
        *out_result = MediaProbeResult{};
        return false;
    }
    out_result->success = true;
    return true;
}

} // namespace pcmtp
