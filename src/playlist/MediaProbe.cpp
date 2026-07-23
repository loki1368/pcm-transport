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
                                  ManagedSubprocess* probe_process) {
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

        const ExternalAudioInfo info = ExternalAudioDecoder::probe_metadata(path, 0, 0, probe_process);
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

} // namespace pcmtp
