#include "pcmtp/stream/StreamAudioDecoder.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>

#include "pcmtp/util/Logger.hpp"
#include "pcmtp/util/MediaUri.hpp"

namespace pcmtp {

bool StreamAudioDecoder::is_stream_uri(const std::string& path) {
    return is_remote_media_uri(path);
}

ExternalAudioInfo StreamAudioDecoder::probe_metadata(const std::string& path,
                                                     std::uint32_t forced_output_sample_rate,
                                                     std::uint16_t forced_output_bits_per_sample,
                                                     ProbeCancellation* probe_cancellation) {
    if (!is_stream_uri(path)) {
        throw std::runtime_error("StreamAudioDecoder does not support this file type");
    }

    Logger::instance().debug("StreamAudioDecoder stream probe: " + path);
    ExternalAudioInfo info = ExternalAudioDecoder::probe_metadata(path,
                                                                  forced_output_sample_rate,
                                                                  forced_output_bits_per_sample,
                                                                  probe_cancellation);
    info.duration_reliable = false;
    info.total_samples_per_channel = 0;
    info.source_total_samples_per_channel = 0;
    info.live_format_probed = info.format.sample_rate > 0 && info.format.channels > 0;
    return info;
}

ExternalAudioInfo StreamAudioDecoder::probe_info(const std::string& path,
                                                 std::uint32_t forced_output_sample_rate,
                                                 std::uint16_t forced_output_bits_per_sample) {
    ExternalAudioInfo info = probe_metadata(path, forced_output_sample_rate, forced_output_bits_per_sample);
    info.tags = GenericTags{};
    return info;
}

bool StreamAudioDecoder::verify_stream_playback(const std::string& path,
                                                const ExternalAudioInfo& probed_info,
                                                std::uint32_t forced_output_sample_rate,
                                                std::uint16_t forced_output_bits_per_sample) {
    if (!is_stream_uri(path)) {
        return true;
    }
    try {
        std::unique_ptr<ExternalAudioDecoder> decoder;
        if (forced_output_sample_rate > 0 || forced_output_bits_per_sample > 0) {
            decoder = std::make_unique<ExternalAudioDecoder>(forced_output_sample_rate,
                                                             forced_output_bits_per_sample);
        } else {
            decoder = std::make_unique<ExternalAudioDecoder>();
        }
        ExternalAudioInfo known = probed_info;
        known.live_format_probed = true;
        decoder->set_known_info(known);
        decoder->open(path);
        PcmSample buffer[2048];
        const std::size_t got = decoder->read_samples(buffer, 2048);
        decoder->request_abort();
        return got > 0;
    } catch (const std::exception& ex) {
        Logger::instance().debug(std::string("Stream verify failed: ") + path + " -> " + ex.what());
        return false;
    } catch (...) {
        Logger::instance().debug(std::string("Stream verify failed: ") + path);
        return false;
    }
}

} // namespace pcmtp
