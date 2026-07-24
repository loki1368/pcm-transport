#pragma once

#include <string>

#include "pcmtp/patches/StreamPlaybackManager.hpp"

namespace pcmtp::patches {

void on_playback_stopped(StreamPlaybackManager& manager);
std::string status_text_with_stream_override(const StreamPlaybackManager& manager,
                                             const std::string& fallback);

} // namespace pcmtp::patches
