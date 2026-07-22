#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <pthread.h>

#include "pcmtp/backend/IAudioBackend.hpp"
#include "pcmtp/decoder/IAudioDecoder.hpp"
#include "pcmtp/dsp/ToneControlDesign.hpp"

namespace pcmtp {

struct PlaybackStatusSnapshot {
    bool playing = false;
    bool paused = false;
    bool finished = false;
    std::uint64_t current_samples_per_channel = 0;
    std::uint64_t total_samples_per_channel = 0;
    std::string message;
    std::string active_output_report;
    float peak_level = 0.0f;
    bool clip_detected = false;
    std::uint32_t clipped_samples = 0;
};

struct PlaybackTransportSnapshot {
    bool playing = false;
    bool paused = false;
    std::uint64_t current_samples_per_channel = 0;
};

using PlaybackStatusCallback = std::function<void(const PlaybackStatusSnapshot&)>;

class PlaybackEngine {
public:
    explicit PlaybackEngine(std::size_t transport_buffer_milliseconds);
    ~PlaybackEngine();

    void start(std::unique_ptr<IAudioDecoder> decoder,
               std::unique_ptr<IAudioBackend> backend,
               const std::string& device_name,
               PlaybackStatusCallback callback = PlaybackStatusCallback(),
               std::uint64_t initial_samples_per_channel = 0);

    void stop();
    void pause();
    void resume();

    bool is_playing() const;
    bool is_paused() const;
    void set_soft_volume_percent(int percent);
    int soft_volume_percent() const;
    void set_soft_eq(int bass_db, int treble_db);
    void set_pre_eq_headroom_tenths_db(int tenths_db);
    int pre_eq_headroom_tenths_db() const;
    void set_soft_eq_profile(int bass_hz, int treble_hz);
    void set_deep_bass_enabled(bool enabled);
    bool deep_bass_enabled() const;
    void set_deep_bass_preset(int preset);
    int deep_bass_preset() const;
    void set_deep_bass_amount(int amount_steps);
    int deep_bass_amount() const;
    void set_level_meter_enabled(bool enabled);
    bool level_meter_enabled() const;
    void set_clip_detection_enabled(bool enabled);
    bool clip_detection_enabled() const;
    int bass_db() const;
    int treble_db() const;
    PlaybackStatusSnapshot snapshot() const;
    PlaybackTransportSnapshot transport_snapshot() const;
    std::string last_error() const;
    std::size_t transport_buffer_milliseconds() const;

    void set_realtime_priority_enabled(bool enabled);
    void set_realtime_priority(int priority);
    bool realtime_priority_enabled() const;
    int realtime_priority() const;
    long playback_thread_id() const;
    std::string realtime_priority_status() const;
    std::string refresh_realtime_priority_status();
    std::string request_realtime_priority_for_playback_thread();
    bool realtime_priority_service_available() const;

    std::string active_output_report() const;

private:
    void playback_loop();
    std::string try_set_realtime_priority_for_current_thread();
    std::string verified_realtime_priority_status(long tid) const;
    void wait_if_paused();
    void update_status(bool force = false);
    void set_error(const std::string& message);
    void join_threads();

    const std::size_t transport_buffer_milliseconds_;

    mutable std::mutex state_mutex_;
    PlaybackStatusSnapshot snapshot_{};
    std::string last_error_;

    std::unique_ptr<IAudioDecoder> decoder_;
    std::unique_ptr<IAudioBackend> backend_;
    AudioFormat format_{};
    std::string device_name_;
    PlaybackStatusCallback callback_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> pause_requested_{false};
    std::atomic<bool> playback_completed_{false};
    std::thread playback_thread_;
    std::mutex pause_mutex_;
    std::condition_variable pause_cv_;
    std::uint64_t initial_samples_per_channel_ = 0;
    std::atomic<int> soft_volume_percent_{100};
    std::atomic<int> bass_db_{0};
    std::atomic<int> treble_db_{0};
    std::atomic<int> pre_eq_headroom_tenths_db_{0};
    std::atomic<int> bass_hz_{110};
    std::atomic<int> treble_hz_{10000};
    std::atomic<bool> deep_bass_enabled_{false};
    std::atomic<int> deep_bass_preset_{static_cast<int>(tone::DeepBassPreset::Focused)};
    std::atomic<int> deep_bass_amount_{0};
    std::atomic<bool> level_meter_enabled_{true};
    std::atomic<bool> clip_detection_enabled_{true};
    std::atomic<bool> realtime_priority_enabled_{false};
    std::atomic<int> realtime_priority_{60};
    std::atomic<long> playback_thread_tid_{0};
    mutable std::mutex runtime_mutex_;
    std::string realtime_priority_status_ = "Realtime priority: disabled";
    std::string last_realtime_priority_error_;
    std::string last_active_output_report_;
};

} // namespace pcmtp
