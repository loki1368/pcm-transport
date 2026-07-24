#include "pcmtp/patches/StreamDecoderFactory.hpp"

#include <memory>

#include "pcmtp/decoder/ExternalAudioDecoder.hpp"
#include "pcmtp/patches/StreamAudioDecoder.hpp"

namespace pcmtp::patches {

bool entry_uses_stream_decoder(const GtkPlayerWindow::PlaylistEntry& entry) {
    return entry.patch.is_stream || StreamAudioDecoder::is_stream_uri(entry.audio_file_path);
}

bool blocks_native_flac_decoder(const GtkPlayerWindow::PlaylistEntry& entry) {
    return entry.patch.is_stream;
}

std::unique_ptr<IAudioDecoder> create_stream_decoder_for_entry(
    const GtkPlayerWindow::PlaylistEntry& entry,
    std::uint32_t target_rate,
    std::uint16_t target_bits,
    bool resample_needed,
    bool bitdepth_needed,
    const StreamDecoderFactoryOptions& options) {
    std::unique_ptr<StreamAudioDecoder> decoder;
    if (resample_needed || bitdepth_needed) {
        decoder.reset(new StreamAudioDecoder(target_rate,
                                             target_bits,
                                             options.resample_quality,
                                             options.bitdepth_quality));
    } else {
        decoder.reset(new StreamAudioDecoder());
    }

    ExternalAudioInfo known;
    known.format = entry.decoded_format;
    known.source_format = entry.decoded_format;
    known.source_format.sample_rate = entry.source_sample_rate > 0 ? entry.source_sample_rate : entry.decoded_format.sample_rate;
    known.source_format.bits_per_sample = entry.source_bits_per_sample > 0 ? entry.source_bits_per_sample : entry.decoded_format.bits_per_sample;
    known.total_samples_per_channel = 0;
    known.source_total_samples_per_channel = 0;
    known.duration_reliable = false;
    known.codec_name = entry.codec_name;
    known.dsd_source = entry.dsd_source;
    known.dsd_sample_rate = entry.dsd_sample_rate;
    known.lossless = entry.lossless_source;
    known.live_format_probed = entry.patch.stream_format_probed;
    decoder->set_known_info(known);
    return std::unique_ptr<IAudioDecoder>(decoder.release());
}

} // namespace pcmtp::patches
