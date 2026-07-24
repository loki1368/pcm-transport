#include "pcmtp/patches/StreamPlaybackHooks.hpp"

#include "pcmtp/patches/StreamPlaybackManager.hpp"

namespace pcmtp::patches {

void on_playback_stopped(StreamPlaybackManager& manager) {
    manager.on_playback_stopped();
}

std::string status_text_with_stream_override(const StreamPlaybackManager& manager,
                                             const std::string& fallback) {
    if (!manager.status_override().empty()) {
        return manager.status_override();
    }
    return fallback;
}

} // namespace pcmtp::patches
