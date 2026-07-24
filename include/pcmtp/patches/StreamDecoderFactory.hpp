#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "pcmtp/gui/GtkPlayerWindow.hpp"

namespace pcmtp::patches {

struct StreamDecoderFactoryOptions {
    std::string resample_quality;
    std::string bitdepth_quality;
};

bool entry_uses_stream_decoder(const GtkPlayerWindow::PlaylistEntry& entry);
bool blocks_native_flac_decoder(const GtkPlayerWindow::PlaylistEntry& entry);

std::unique_ptr<IAudioDecoder> create_stream_decoder_for_entry(
    const GtkPlayerWindow::PlaylistEntry& entry,
    std::uint32_t target_rate,
    std::uint16_t target_bits,
    bool resample_needed,
    bool bitdepth_needed,
    const StreamDecoderFactoryOptions& options);

} // namespace pcmtp::patches
