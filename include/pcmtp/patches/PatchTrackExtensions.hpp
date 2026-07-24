#pragma once

#include <cstdint>

namespace pcmtp {

// PATCH: fork-specific playlist entry fields (internet radio, stream probe).
struct PatchTrackExtensions {
    bool is_stream = false;
    bool stream_format_probed = false;
    std::uint32_t source_bit_rate = 0;
};

} // namespace pcmtp
