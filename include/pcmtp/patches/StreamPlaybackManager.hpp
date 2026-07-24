#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct _GMainLoop;
typedef struct _GMainLoop GMainLoop;
typedef int gboolean;
typedef void* gpointer;

#include "pcmtp/decoder/ExternalAudioDecoder.hpp"
#include "pcmtp/stream/StreamHealthRegistry.hpp"
#include "pcmtp/stream/StreamSidecar.hpp"

namespace pcmtp {

// PATCH: internet-radio — stream probe, reconnect, ICY metadata, health tracking.
class StreamPlaybackManager {
    friend gboolean stream_probe_idle_cb(gpointer data);

public:
    struct ProbePlaybackRequest {
        std::size_t index = 0;
        std::uint64_t offset_samples = 0;
        bool preserve_paused = false;
        bool update_mpris_track = true;
        bool skip_engine_stop = false;
    };

    struct ProbeResult {
        std::string url;
        std::uint64_t generation = 0;
        ExternalAudioInfo info;
        bool probe_ok = false;
        std::string error;
        ProbePlaybackRequest playback;
    };

    class Delegate {
    public:
        virtual ~Delegate() = default;
        virtual bool ui_closing() const = 0;
        virtual void on_now_playing_changed(const std::string& title) = 0;
        virtual void on_status_override_changed() = 0;
        virtual void on_stream_health_changed(const std::string& url) = 0;
        virtual void on_probe_finished(ProbeResult result) = 0;
        virtual void on_reconnect_requested(std::size_t index) = 0;
        virtual std::size_t find_playlist_index_by_url(const std::string& url) const = 0;
    };

    explicit StreamPlaybackManager(Delegate& delegate);
    ~StreamPlaybackManager();

    StreamPlaybackManager(const StreamPlaybackManager&) = delete;
    StreamPlaybackManager& operator=(const StreamPlaybackManager&) = delete;

    void shutdown();
    void load_health_registry();

    void start_sidecar(const std::string& stream_url);
    void stop_sidecar(bool wait_for_exit = true);
    const std::string& now_playing() const { return now_playing_; }

    const std::string& status_override() const { return status_override_; }
    void set_status_override(std::string text);

    bool is_broken(const std::string& url) const;
    void note_broken(const std::string& url, const std::string& error);
    void update_from_playback(const std::string& url, bool playing, bool paused);
    void reset_health_tracking();

    bool reconnect_pending() const { return reconnect_pending_; }
    bool tick_reconnect(std::chrono::steady_clock::time_point now);
    void schedule_reconnect(std::size_t index, const std::string& status_message);
    void cancel_reconnect();
    bool is_reconnect_target(std::size_t index) const;
    std::size_t reconnect_target_index() const { return reconnect_target_index_; }
    int reconnect_attempts() const { return reconnect_attempts_; }
    void clear_reconnect_after_success();

    std::uint64_t bump_probe_generation();
    void invalidate_probes();
    void begin_async_probe(const ProbePlaybackRequest& request,
                           const std::string& url,
                           std::uint32_t forced_output_sample_rate,
                           std::uint16_t forced_output_bits_per_sample,
                           std::uint64_t probe_generation);

    bool should_keep_sidecar_for_play(std::size_t index, bool is_stream) const;
    void on_playback_stopped();
    void notify_metadata_title(const std::string& title);
    bool probe_is_current(std::uint64_t generation) const;

private:
    struct ProbeTask {
        StreamPlaybackManager* manager = nullptr;
        std::uint64_t generation = 0;
        ProbePlaybackRequest playback;
        std::string url;
        std::uint32_t forced_output_sample_rate = 0;
        std::uint16_t forced_output_bits_per_sample = 0;
    };

    struct ProbeTaskResult : ProbeTask {
        ExternalAudioInfo info;
        bool probe_ok = false;
        std::string error;
    };

    void deliver_probe_result(ProbeTaskResult* raw);
    void ensure_probe_worker();
    void probe_worker_loop();

    Delegate& delegate_;
    std::unique_ptr<StreamSidecar> sidecar_;
    StreamHealthRegistry health_;
    std::string now_playing_;
    std::string sidecar_url_;
    std::string status_override_;
    std::string health_track_url_;
    bool health_playing_ = false;
    std::chrono::steady_clock::time_point health_playing_since_{};
    std::size_t reconnect_target_index_ = static_cast<std::size_t>(-1);
    int reconnect_attempts_ = 0;
    bool reconnect_pending_ = false;
    std::chrono::steady_clock::time_point reconnect_due_{};
    std::uint64_t probe_generation_ = 0;
    mutable std::mutex probe_mutex_;
    std::condition_variable probe_cv_;
    std::deque<ProbeTask> probe_queue_;
    std::thread probe_thread_;
    bool probe_shutdown_ = false;
};

} // namespace pcmtp
