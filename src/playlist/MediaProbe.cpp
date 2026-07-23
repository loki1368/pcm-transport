#include "pcmtp/playlist/MediaProbe.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <memory>

#include "pcmtp/decoder/ExternalAudioDecoder.hpp"
#include "pcmtp/decoder/FlacStreamDecoder.hpp"
#include "pcmtp/patches/StreamAudioDecoder.hpp"

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

} // namespace

MediaProbeResult probe_media_file(const std::string& path, ManagedSubprocess* probe_process) {
    MediaProbeResult result;
    try {
        if (lower_extension(path) == ".flac") {
            try {
                std::unique_ptr<IAudioDecoder> decoder(new FlacStreamDecoder());
                decoder->open(path);
                const FlacTags tags = FlacStreamDecoder::read_tags(path);
                result.format = decoder->format();
                result.total_samples_per_channel = decoder->total_samples_per_channel();
                result.tags.title = tags.title;
                result.tags.artist = tags.artist;
                result.tags.track_number = tags.track_number;
                result.codec_name = "flac";
                result.native_decode = true;
                result.lossless = true;
                result.success = true;
                return result;
            } catch (...) {
                // Keep the existing external fallback for nonstandard or damaged FLAC files.
            }
        }

        const ExternalAudioInfo info = StreamAudioDecoder::is_stream_uri(path)
            ? StreamAudioDecoder::probe_metadata(path, 0, 0, false, probe_process)
            : ExternalAudioDecoder::probe_metadata(path, 0, 0, false, probe_process);
        result.format = info.format;
        result.total_samples_per_channel = info.total_samples_per_channel;
        result.tags = info.tags;
        result.codec_name = info.codec_name;
        result.native_decode = false;
        result.lossless = info.lossless;
        result.dsd_source = info.dsd_source;
        result.dsd_sample_rate = info.dsd_sample_rate;
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
