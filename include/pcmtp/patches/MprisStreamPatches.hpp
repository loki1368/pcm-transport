#pragma once

#include <string>

#include "pcmtp/gui/GtkPlayerWindow.hpp"
#include "pcmtp/mpris/MprisService.hpp"
#include "pcmtp/patches/StreamPlaybackManager.hpp"

namespace pcmtp {

class GtkPlayerWindow;

namespace patches {

void apply_fork_mpris_action_patches(GtkPlayerWindow& window, MprisService::Actions& actions);

void apply_stream_fields_to_mpris_state(MprisPlayerState& state,
                                        const GtkPlayerWindow::PlaylistEntry& track,
                                        const StreamPlaybackManager& manager);
std::string display_title_for_entry(const GtkPlayerWindow::PlaylistEntry& entry,
                                    const StreamPlaybackManager& manager);

} // namespace patches
} // namespace pcmtp
