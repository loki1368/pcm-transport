#include "pcmtp/patches/StreamPlaybackManager.hpp"

#include <gtk/gtk.h>

#include <algorithm>
#include <utility>

#include "pcmtp/util/Logger.hpp"
#include "pcmtp/util/MediaUri.hpp"

namespace pcmtp {
namespace {

constexpr int kMaxStreamReconnectAttempts = 5;
constexpr int kStreamReconnectDelaysSec[] = {3, 5, 8, 12, 20};
constexpr int kStreamHealthOkSeconds = 10;

struct MetadataUpdate {
    StreamPlaybackManager* manager = nullptr;
    std::string title;
};

gboolean on_metadata_idle(gpointer user_data) {
    std::unique_ptr<MetadataUpdate> update(static_cast<MetadataUpdate*>(user_data));
    if (update == nullptr || update->manager == nullptr) {
        return G_SOURCE_REMOVE;
    }
    if (update->title.empty() || update->title == update->manager->now_playing()) {
        return G_SOURCE_REMOVE;
    }
    update->manager->notify_metadata_title(update->title);
    return G_SOURCE_REMOVE;
}

} // namespace

gboolean stream_probe_idle_cb(gpointer data) {
    auto* result = static_cast<StreamPlaybackManager::ProbeTaskResult*>(data);
    if (result != nullptr && result->manager != nullptr) {
        result->manager->deliver_probe_result(result);
    }
    return G_SOURCE_REMOVE;
}

StreamPlaybackManager::StreamPlaybackManager(Delegate& delegate)
    : delegate_(delegate) {}

StreamPlaybackManager::~StreamPlaybackManager() {
    shutdown();
}

void StreamPlaybackManager::shutdown() {
    invalidate_probes();
    stop_sidecar(true);
}

void StreamPlaybackManager::load_health_registry() {
    health_.load();
}

void StreamPlaybackManager::notify_metadata_title(const std::string& title) {
    if (title.empty() || title == now_playing_) {
        return;
    }
    now_playing_ = title;
    delegate_.on_now_playing_changed(title);
}

void StreamPlaybackManager::set_status_override(std::string text) {
    status_override_ = std::move(text);
    delegate_.on_status_override_changed();
}

void StreamPlaybackManager::start_sidecar(const std::string& stream_url) {
    if (sidecar_ != nullptr && sidecar_url_ == stream_url) {
        return;
    }

    stop_sidecar(false);
    now_playing_.clear();
    sidecar_url_ = stream_url;
    sidecar_ = std::make_unique<StreamSidecar>();
    StreamPlaybackManager* manager = this;
    sidecar_->start(stream_url, [manager](const std::string& title) {
        if (manager == nullptr || manager->delegate_.ui_closing()) {
            return;
        }
        auto* update = new MetadataUpdate{manager, title};
        g_idle_add(on_metadata_idle, update);
    });
}

void StreamPlaybackManager::stop_sidecar(bool wait_for_exit) {
    if (sidecar_ == nullptr) {
        now_playing_.clear();
        sidecar_url_.clear();
        return;
    }

    std::unique_ptr<StreamSidecar> sidecar = std::move(sidecar_);
    sidecar_url_.clear();
    now_playing_.clear();
    if (wait_for_exit) {
        sidecar->stop();
        return;
    }

    std::thread([sidecar = std::move(sidecar)]() mutable {
        sidecar->stop();
    }).detach();
}

bool StreamPlaybackManager::is_broken(const std::string& url) const {
    return health_.is_broken(url);
}

void StreamPlaybackManager::note_broken(const std::string& url, const std::string& error) {
    health_.mark_broken(url, error);
    if (!delegate_.ui_closing()) {
        delegate_.on_stream_health_changed(url);
    }
}

void StreamPlaybackManager::update_from_playback(const std::string& url, bool playing, bool paused) {
    if (url.empty()) {
        reset_health_tracking();
        return;
    }

    if (url != health_track_url_) {
        health_track_url_ = url;
        health_playing_ = false;
    }

    if (playing && !paused) {
        if (!health_playing_) {
            health_playing_ = true;
            health_playing_since_ = std::chrono::steady_clock::now();
            return;
        }

        const auto elapsed = std::chrono::steady_clock::now() - health_playing_since_;
        if (elapsed >= std::chrono::seconds(kStreamHealthOkSeconds)) {
            if (health_.mark_ok(url)) {
                delegate_.on_stream_health_changed(url);
            }
            health_playing_ = false;
        }
    } else {
        health_playing_ = false;
    }
}

void StreamPlaybackManager::reset_health_tracking() {
    health_track_url_.clear();
    health_playing_ = false;
}

bool StreamPlaybackManager::tick_reconnect(std::chrono::steady_clock::time_point now) {
    if (!reconnect_pending_ || now < reconnect_due_) {
        return false;
    }
    reconnect_pending_ = false;
    if (reconnect_target_index_ != static_cast<std::size_t>(-1)) {
        delegate_.on_reconnect_requested(reconnect_target_index_);
    }
    return true;
}

void StreamPlaybackManager::schedule_reconnect(std::size_t index, const std::string& status_message) {
    if (reconnect_attempts_ >= kMaxStreamReconnectAttempts) {
        set_status_override("Stream unavailable");
        return;
    }

    const int delay_index = std::min(reconnect_attempts_,
                                     static_cast<int>(sizeof(kStreamReconnectDelaysSec) / sizeof(kStreamReconnectDelaysSec[0])) - 1);
    reconnect_target_index_ = index;
    reconnect_pending_ = true;
    reconnect_due_ = std::chrono::steady_clock::now() + std::chrono::seconds(kStreamReconnectDelaysSec[delay_index]);
    set_status_override(status_message.empty() ? "Reconnecting..." : status_message);
    ++reconnect_attempts_;
}

void StreamPlaybackManager::cancel_reconnect() {
    reconnect_pending_ = false;
    reconnect_target_index_ = static_cast<std::size_t>(-1);
    reconnect_attempts_ = 0;
    status_override_.clear();
    delegate_.on_status_override_changed();
}

bool StreamPlaybackManager::is_reconnect_target(std::size_t index) const {
    return reconnect_target_index_ == index;
}

void StreamPlaybackManager::clear_reconnect_after_success() {
    cancel_reconnect();
    status_override_.clear();
    delegate_.on_status_override_changed();
}

std::uint64_t StreamPlaybackManager::bump_probe_generation() {
    std::lock_guard<std::mutex> lock(probe_mutex_);
    return ++probe_generation_;
}

void StreamPlaybackManager::invalidate_probes() {
    {
        std::lock_guard<std::mutex> lock(probe_mutex_);
        ++probe_generation_;
        probe_shutdown_ = true;
    }
    probe_cv_.notify_all();
    if (probe_thread_.joinable()) {
        probe_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(probe_mutex_);
        probe_queue_.clear();
        probe_shutdown_ = false;
    }
}

bool StreamPlaybackManager::should_keep_sidecar_for_play(std::size_t index, bool is_stream) const {
    return is_stream && is_reconnect_target(index);
}

void StreamPlaybackManager::on_playback_stopped() {
    stop_sidecar();
    cancel_reconnect();
    reset_health_tracking();
    invalidate_probes();
}

void StreamPlaybackManager::ensure_probe_worker() {
    std::lock_guard<std::mutex> lock(probe_mutex_);
    if (probe_thread_.joinable()) {
        return;
    }
    probe_shutdown_ = false;
    probe_thread_ = std::thread(&StreamPlaybackManager::probe_worker_loop, this);
}

void StreamPlaybackManager::begin_async_probe(const ProbePlaybackRequest& request,
                                              const std::string& url,
                                              std::uint32_t forced_output_sample_rate,
                                              std::uint16_t forced_output_bits_per_sample,
                                              std::uint64_t probe_generation) {
    ensure_probe_worker();

    ProbeTask task;
    task.manager = this;
    task.generation = probe_generation;
    task.playback = request;
    task.url = url;
    task.forced_output_sample_rate = forced_output_sample_rate;
    task.forced_output_bits_per_sample = forced_output_bits_per_sample;

    {
        std::lock_guard<std::mutex> lock(probe_mutex_);
        const auto stale = std::remove_if(probe_queue_.begin(),
                                          probe_queue_.end(),
                                          [&](const ProbeTask& pending) { return pending.url == task.url; });
        probe_queue_.erase(stale, probe_queue_.end());
        probe_queue_.push_front(std::move(task));
    }
    probe_cv_.notify_one();
}

void StreamPlaybackManager::probe_worker_loop() {
    for (;;) {
        ProbeTask task;
        {
            std::unique_lock<std::mutex> lock(probe_mutex_);
            probe_cv_.wait(lock, [this]() {
                return probe_shutdown_ || !probe_queue_.empty();
            });
            if (probe_shutdown_ && probe_queue_.empty()) {
                return;
            }
            task = std::move(probe_queue_.front());
            probe_queue_.pop_front();
        }

        std::thread([task = std::move(task), this]() mutable {
            auto* result = new StreamPlaybackManager::ProbeTaskResult;
            static_cast<StreamPlaybackManager::ProbeTask&>(*result) = std::move(task);
            const std::uint64_t generation = result->generation;
            StreamPlaybackManager* manager = result->manager;

            try {
                result->info = ExternalAudioDecoder::probe_info(result->url,
                                                                result->forced_output_sample_rate,
                                                                result->forced_output_bits_per_sample);
                result->probe_ok = true;
            } catch (const std::exception& ex) {
                result->error = ex.what();
            } catch (...) {
                result->error = "Stream probe failed";
            }

            bool generation_current = false;
            if (manager != nullptr) {
                generation_current = manager->probe_is_current(generation);
            }

            if (!generation_current && result->probe_ok && result->info.live_format_probed) {
                if (!ExternalAudioDecoder::verify_stream_playback(result->url,
                                                                  result->info,
                                                                  result->forced_output_sample_rate,
                                                                  result->forced_output_bits_per_sample)) {
                    result->info.live_format_probed = false;
                }
            }

            g_idle_add(stream_probe_idle_cb, result);
        }).detach();
    }
}

void StreamPlaybackManager::deliver_probe_result(ProbeTaskResult* raw) {
    std::unique_ptr<ProbeTaskResult> request(raw);
    if (request == nullptr || delegate_.ui_closing()) {
        return;
    }
    if (!probe_is_current(request->generation)) {
        return;
    }

    ProbeResult result;
    result.url = request->url;
    result.generation = request->generation;
    result.info = request->info;
    result.probe_ok = request->probe_ok;
    result.error = request->error;
    result.playback = request->playback;
    delegate_.on_probe_finished(std::move(result));
}

bool StreamPlaybackManager::probe_is_current(std::uint64_t generation) const {
    std::lock_guard<std::mutex> lock(probe_mutex_);
    return generation == probe_generation_;
}

} // namespace pcmtp
