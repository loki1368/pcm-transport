#include "pcmtp/patches/MprisStreamPatches.hpp"

#include <functional>

#include "pcmtp/gui/GtkPlayerWindow.hpp"
#include "pcmtp/util/MediaUri.hpp"

namespace pcmtp::patches {

void apply_fork_mpris_action_patches(GtkPlayerWindow& window, MprisService::Actions& actions) {
    auto wrap_selection = [&window](std::function<void()> action) {
        return [&window, action = std::move(action)]() {
            window.update_playlist_selection_from_ui();
            action();
        };
    };

    auto play = std::move(actions.play);
    actions.play = wrap_selection(std::move(play));

    auto play_pause = std::move(actions.play_pause);
    actions.play_pause = wrap_selection(std::move(play_pause));

    auto next = std::move(actions.next);
    actions.next = wrap_selection(std::move(next));

    auto previous = std::move(actions.previous);
    actions.previous = wrap_selection(std::move(previous));
}

void apply_stream_fields_to_mpris_state(MprisPlayerState& state,
                                        const GtkPlayerWindow::PlaylistEntry& track,
                                        const StreamPlaybackManager& manager) {
    if (!track.patch.is_stream) {
        return;
    }
    if (!manager.now_playing().empty()) {
        state.title = manager.now_playing();
    }
    state.url = track.audio_file_path;
    state.art_url.clear();
}

std::string display_title_for_entry(const GtkPlayerWindow::PlaylistEntry& entry,
                                    const StreamPlaybackManager& manager) {
    if (entry.patch.is_stream && !manager.now_playing().empty()) {
        if (!entry.title.empty()) {
            return entry.title + " — " + manager.now_playing();
        }
        return manager.now_playing();
    }
    return entry.title;
}

} // namespace pcmtp::patches
