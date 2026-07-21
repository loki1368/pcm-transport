#include "pcmtp/core/PlaybackEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sched.h>
#include <unistd.h>
#include <gio/gio.h>
#include "pcmtp/dsp/ToneControlDesign.hpp"
#include "pcmtp/util/Logger.hpp"

namespace pcmtp {
namespace {

struct ShelfState {
    double z1 = 0.0;
    double z2 = 0.0;
};

using tone::DeepBassState;
using tone::ShelfCoefficients;

double process_sample(double input, const ShelfCoefficients& c, ShelfState& s) {
    const double out = c.b0 * input + s.z1;
    s.z1 = c.b1 * input - c.a1 * out + s.z2;
    s.z2 = c.b2 * input - c.a2 * out;
    return out;
}


double clamp_sample_to_bits(double sample, std::uint16_t bits_per_sample) {
    const double limit = static_cast<double>(pcm_full_scale(bits_per_sample));
    if (limit <= 0.0) return sample;
    if (sample > limit) return limit;
    if (sample < -limit) return -limit;
    return sample;
}

bool sample_exceeds_full_scale(double sample, std::uint16_t bits_per_sample) {
    const double limit = static_cast<double>(pcm_full_scale(bits_per_sample));
    if (limit <= 0.0) return false;
    return sample > limit || sample < -limit;
}


constexpr int kPreEqHeadroomMaxTenthsDb = 150;

} // namespace

PlaybackEngine::PlaybackEngine(std::size_t transport_buffer_milliseconds)
    : transport_buffer_milliseconds_(transport_buffer_milliseconds) {
    if (transport_buffer_milliseconds_ == 0) {
        throw std::invalid_argument("transport_buffer_milliseconds must be > 0");
    }
}

PlaybackEngine::~PlaybackEngine() { stop(); }

void PlaybackEngine::start(std::unique_ptr<IAudioDecoder> decoder,
                           std::unique_ptr<IAudioBackend> backend,
                           const std::string& device_name,
                           PlaybackStatusCallback callback,
                           std::uint64_t initial_samples_per_channel) {
    stop();
    if (!decoder || !backend) {
        throw std::invalid_argument("PlaybackEngine::start received null decoder/backend");
    }
    decoder_ = std::move(decoder);
    backend_ = std::move(backend);
    callback_ = std::move(callback);
    device_name_ = device_name;
    stop_requested_ = false;
    pause_requested_ = false;
    playback_completed_ = false;
    initial_samples_per_channel_ = initial_samples_per_channel;
    format_ = decoder_->format();
    backend_->open(device_name_, format_);
    const std::string opened_report = backend_->active_output_report();
    {
        std::lock_guard<std::mutex> runtime_lock(runtime_mutex_);
        last_active_output_report_ = opened_report;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        snapshot_ = PlaybackStatusSnapshot{};
        snapshot_.playing = true;
        snapshot_.paused = false;
        snapshot_.finished = false;
        snapshot_.total_samples_per_channel = decoder_->total_samples_per_channel();
        snapshot_.message = "Playing";
        snapshot_.active_output_report = opened_report;
        snapshot_.current_samples_per_channel = initial_samples_per_channel_;
        last_error_.clear();
    }
    update_status(true);
    Logger::instance().info("Playback started on device: " + device_name_);
    playback_thread_ = std::thread(&PlaybackEngine::playback_loop, this);
}

void PlaybackEngine::stop() {
    stop_requested_ = true;
    pause_requested_ = false;
    pause_cv_.notify_all();
    if (decoder_) {
        decoder_->interrupt();
    }
    join_threads();
    playback_thread_tid_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = realtime_priority_enabled_.load(std::memory_order_relaxed)
            ? std::string("Realtime priority: inactive, playback stopped")
            : std::string("Realtime priority: disabled");
    }
    if (backend_) {
        try { backend_->close(); } catch (...) {}
    }
    decoder_.reset();
    backend_.reset();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        snapshot_.playing = false;
        snapshot_.paused = false;
        if (!snapshot_.finished) {
            snapshot_.current_samples_per_channel = 0;
            snapshot_.message = last_error_.empty() ? "Stopped" : last_error_;
            snapshot_.peak_level = 0.0f;
            snapshot_.clip_detected = false;
            snapshot_.clipped_samples = 0;
        }
    }
    update_status(true);
}

void PlaybackEngine::pause() {
    if (!is_playing()) return;
    pause_requested_ = true;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        snapshot_.paused = true;
        snapshot_.message = "Paused";
    }
    update_status(true);
}

void PlaybackEngine::resume() {
    pause_requested_ = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        snapshot_.paused = false;
        snapshot_.message = "Playing";
    }
    pause_cv_.notify_all();
    update_status(true);
}

bool PlaybackEngine::is_playing() const { std::lock_guard<std::mutex> lock(state_mutex_); return snapshot_.playing; }
bool PlaybackEngine::is_paused() const { std::lock_guard<std::mutex> lock(state_mutex_); return snapshot_.paused; }
void PlaybackEngine::set_soft_volume_percent(int percent) { soft_volume_percent_.store(std::max(0, std::min(100, percent)), std::memory_order_relaxed); }
int PlaybackEngine::soft_volume_percent() const { return soft_volume_percent_.load(std::memory_order_relaxed); }
void PlaybackEngine::set_soft_eq(int bass_db, int treble_db) { bass_db_.store(std::max(-12, std::min(12, bass_db)), std::memory_order_relaxed); treble_db_.store(std::max(-12, std::min(12, treble_db)), std::memory_order_relaxed); }
void PlaybackEngine::set_pre_eq_headroom_tenths_db(int tenths_db) { pre_eq_headroom_tenths_db_.store(std::max(0, std::min(kPreEqHeadroomMaxTenthsDb, tenths_db)), std::memory_order_relaxed); }
int PlaybackEngine::pre_eq_headroom_tenths_db() const { return pre_eq_headroom_tenths_db_.load(std::memory_order_relaxed); }
void PlaybackEngine::set_soft_eq_profile(int bass_hz, int treble_hz) { bass_hz_.store(tone::clamp_bass_hz(bass_hz), std::memory_order_relaxed); treble_hz_.store(tone::clamp_treble_hz(treble_hz), std::memory_order_relaxed); }
void PlaybackEngine::set_deep_bass_enabled(bool enabled) { deep_bass_enabled_.store(enabled, std::memory_order_relaxed); }
bool PlaybackEngine::deep_bass_enabled() const { return deep_bass_enabled_.load(std::memory_order_relaxed); }
void PlaybackEngine::set_deep_bass_preset(int preset) {
    const int clamped = std::max(0, std::min(5, preset));
    deep_bass_preset_.store(clamped, std::memory_order_relaxed);
}
int PlaybackEngine::deep_bass_preset() const { return deep_bass_preset_.load(std::memory_order_relaxed); }
void PlaybackEngine::set_deep_bass_amount(int amount_steps) { deep_bass_amount_.store(std::max(-1, std::min(1, amount_steps)), std::memory_order_relaxed); }
int PlaybackEngine::deep_bass_amount() const { return deep_bass_amount_.load(std::memory_order_relaxed); }
void PlaybackEngine::set_level_meter_enabled(bool enabled) { level_meter_enabled_.store(enabled, std::memory_order_relaxed); }
bool PlaybackEngine::level_meter_enabled() const { return level_meter_enabled_.load(std::memory_order_relaxed); }
void PlaybackEngine::set_clip_detection_enabled(bool enabled) { clip_detection_enabled_.store(enabled, std::memory_order_relaxed); }
bool PlaybackEngine::clip_detection_enabled() const { return clip_detection_enabled_.load(std::memory_order_relaxed); }
int PlaybackEngine::bass_db() const { return bass_db_.load(std::memory_order_relaxed); }
int PlaybackEngine::treble_db() const { return treble_db_.load(std::memory_order_relaxed); }
PlaybackStatusSnapshot PlaybackEngine::snapshot() const { std::lock_guard<std::mutex> lock(state_mutex_); return snapshot_; }
std::string PlaybackEngine::last_error() const { std::lock_guard<std::mutex> lock(state_mutex_); return last_error_; }
std::size_t PlaybackEngine::transport_buffer_milliseconds() const { return transport_buffer_milliseconds_; }

void PlaybackEngine::set_realtime_priority_enabled(bool enabled) {
    realtime_priority_enabled_.store(enabled, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    if (!enabled) {
        realtime_priority_status_ = "Realtime priority: disabled";
        last_realtime_priority_error_.clear();
    }
}

void PlaybackEngine::set_realtime_priority(int priority) {
    realtime_priority_.store(std::max(1, std::min(80, priority)), std::memory_order_relaxed);
}

bool PlaybackEngine::realtime_priority_enabled() const {
    return realtime_priority_enabled_.load(std::memory_order_relaxed);
}

int PlaybackEngine::realtime_priority() const {
    return realtime_priority_.load(std::memory_order_relaxed);
}

long PlaybackEngine::playback_thread_id() const {
    return playback_thread_tid_.load(std::memory_order_relaxed);
}

std::string PlaybackEngine::realtime_priority_status() const {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    return realtime_priority_status_;
}

namespace {

std::string gerror_message(const char* prefix, GError* error) {
    std::string message = prefix != nullptr ? std::string(prefix) : std::string("error");
    if (error != nullptr && error->message != nullptr) {
        message += ": ";
        message += error->message;
    }
    return message;
}

GDBusConnection* rtkit_system_bus(std::string& error_message) {
    GError* error = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (connection == nullptr) {
        error_message = gerror_message("system D-Bus unavailable", error);
        if (error != nullptr) g_error_free(error);
        return nullptr;
    }
    return connection;
}

bool rtkit_name_has_owner(std::string* detail = nullptr) {
    std::string bus_error;
    GDBusConnection* connection = rtkit_system_bus(bus_error);
    if (connection == nullptr) {
        if (detail != nullptr) *detail = bus_error;
        return false;
    }
    GError* error = nullptr;
    GVariant* result = g_dbus_connection_call_sync(connection,
                                                   "org.freedesktop.DBus",
                                                   "/org/freedesktop/DBus",
                                                   "org.freedesktop.DBus",
                                                   "NameHasOwner",
                                                   g_variant_new("(s)", "org.freedesktop.RealtimeKit1"),
                                                   G_VARIANT_TYPE("(b)"),
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   750,
                                                   nullptr,
                                                   &error);
    g_object_unref(connection);
    if (result == nullptr) {
        if (detail != nullptr) *detail = gerror_message("RTKit availability check failed", error);
        if (error != nullptr) g_error_free(error);
        return false;
    }
    gboolean has_owner = FALSE;
    g_variant_get(result, "(b)", &has_owner);
    g_variant_unref(result);
    if (!has_owner && detail != nullptr) {
        *detail = "RTKit service not available";
    }
    return has_owner != FALSE;
}

bool rtkit_get_max_realtime_priority(GDBusConnection* connection, int& max_priority, std::string& error_message) {
    max_priority = 0;
    if (connection == nullptr) {
        error_message = "invalid D-Bus connection";
        return false;
    }
    GError* error = nullptr;
    GVariant* result = g_dbus_connection_call_sync(connection,
                                                   "org.freedesktop.RealtimeKit1",
                                                   "/org/freedesktop/RealtimeKit1",
                                                   "org.freedesktop.DBus.Properties",
                                                   "Get",
                                                   g_variant_new("(ss)", "org.freedesktop.RealtimeKit1", "MaxRealtimePriority"),
                                                   G_VARIANT_TYPE("(v)"),
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   1500,
                                                   nullptr,
                                                   &error);
    if (result == nullptr) {
        error_message = gerror_message("RTKit MaxRealtimePriority query failed", error);
        if (error != nullptr) g_error_free(error);
        return false;
    }

    GVariant* value = nullptr;
    g_variant_get(result, "(v)", &value);
    if (value != nullptr) {
        const GVariantType* type = g_variant_get_type(value);
        if (g_variant_type_equal(type, G_VARIANT_TYPE_INT32)) {
            max_priority = static_cast<int>(g_variant_get_int32(value));
        } else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT32)) {
            max_priority = static_cast<int>(g_variant_get_uint32(value));
        } else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT64)) {
            max_priority = static_cast<int>(g_variant_get_int64(value));
        } else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT64)) {
            max_priority = static_cast<int>(g_variant_get_uint64(value));
        }
        g_variant_unref(value);
    }
    g_variant_unref(result);

    if (max_priority <= 0) {
        error_message = "RTKit MaxRealtimePriority is not usable";
        return false;
    }
    return true;
}

const char* policy_display_name(int policy) {
    switch (policy) {
        case SCHED_RR: return "SCHED_RR";
        case SCHED_FIFO: return "SCHED_FIFO";
        case SCHED_OTHER: return "TS";
#ifdef SCHED_BATCH
        case SCHED_BATCH: return "BATCH";
#endif
#ifdef SCHED_IDLE
        case SCHED_IDLE: return "IDLE";
#endif
        default: return "UNKNOWN";
    }
}

bool direct_make_thread_realtime(long tid, int requested_priority, std::string& error_message) {
    if (tid <= 0) {
        error_message = "invalid audio thread TID";
        return false;
    }
    sched_param param{};
    param.sched_priority = std::max(1, std::min(80, requested_priority));
    if (sched_setscheduler(static_cast<pid_t>(tid), SCHED_RR, &param) == 0) {
        return true;
    }
    const int err = errno;
    switch (err) {
        case EPERM:
            error_message = "Direct scheduler request failed: permission required";
            break;
        case ESRCH:
            error_message = "Direct scheduler request failed: playback thread not found";
            break;
        case EINVAL:
            error_message = "Direct scheduler request failed: invalid priority or scheduler";
            break;
        default:
            error_message = std::string("Direct scheduler request failed: ") + std::strerror(err);
            break;
    }
    return false;
}

std::string concise_rtkit_error(GError* error) {
    if (error == nullptr || error->message == nullptr) {
        return "request failed";
    }
    const std::string msg(error->message);
    if (msg.find("AccessDenied") != std::string::npos ||
        msg.find("not permitted") != std::string::npos ||
        msg.find("Operation not permitted") != std::string::npos) {
        return "access denied by system policy";
    }
    if (msg.find("No such interface") != std::string::npos ||
        msg.find("Name has no owner") != std::string::npos) {
        return "service not available";
    }
    return msg;
}

bool rtkit_make_thread_realtime(long tid, int requested_priority, int& effective_priority, int& max_priority, std::string& error_message) {
    effective_priority = 0;
    max_priority = 0;
    if (tid <= 0) {
        error_message = "invalid audio thread TID";
        return false;
    }
    std::string bus_error;
    GDBusConnection* connection = rtkit_system_bus(bus_error);
    if (connection == nullptr) {
        error_message = bus_error;
        return false;
    }

    std::string max_error;
    const bool have_max_priority = rtkit_get_max_realtime_priority(connection, max_priority, max_error);
    if (!have_max_priority) {
        max_priority = 20;
    }

    effective_priority = std::max(1, std::min(requested_priority, max_priority));
    GError* error = nullptr;
    GVariant* result = g_dbus_connection_call_sync(connection,
                                                   "org.freedesktop.RealtimeKit1",
                                                   "/org/freedesktop/RealtimeKit1",
                                                   "org.freedesktop.RealtimeKit1",
                                                   "MakeThreadRealtime",
                                                   g_variant_new("(tu)", static_cast<guint64>(tid), static_cast<guint32>(effective_priority)),
                                                   nullptr,
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   1500,
                                                   nullptr,
                                                   &error);
    g_object_unref(connection);
    if (result == nullptr) {
        std::ostringstream ss;
        ss << "RTKit: " << concise_rtkit_error(error)
           << "; requested " << effective_priority
           << "; service max " << max_priority
           << "; TID " << tid;
        if (!have_max_priority && !max_error.empty()) {
            ss << "; max query: " << max_error;
        }
        error_message = ss.str();
        if (error != nullptr) g_error_free(error);
        return false;
    }
    g_variant_unref(result);
    return true;
}

} // namespace

std::string PlaybackEngine::verified_realtime_priority_status(long tid) const {
    if (!realtime_priority_enabled_.load(std::memory_order_relaxed)) {
        return "Realtime priority: disabled";
    }
    if (tid <= 0) {
        return "Realtime priority: inactive, playback stopped";
    }
    sched_param param{};
    const int policy = sched_getscheduler(static_cast<pid_t>(tid));
    if (policy < 0) {
        return "Realtime priority: inactive, playback thread not found, TID " + std::to_string(tid);
    }
    if (sched_getparam(static_cast<pid_t>(tid), &param) != 0) {
        return "Realtime priority: status unavailable, TID " + std::to_string(tid);
    }
    if (policy == SCHED_RR || policy == SCHED_FIFO) {
        return std::string("Realtime priority: active, ") + policy_display_name(policy) + " " +
               std::to_string(param.sched_priority) + ", TID " + std::to_string(tid);
    }
    return std::string("Realtime priority: not active, scheduler ") + policy_display_name(policy) +
           ", priority " + std::to_string(param.sched_priority) + ", TID " + std::to_string(tid);
}

bool PlaybackEngine::realtime_priority_service_available() const {
    return rtkit_name_has_owner();
}

std::string PlaybackEngine::request_realtime_priority_for_playback_thread() {
    if (!realtime_priority_enabled_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = "Realtime priority: disabled";
        last_realtime_priority_error_.clear();
        return realtime_priority_status_;
    }

    const long tid = playback_thread_tid_.load(std::memory_order_relaxed);
    if (tid <= 0) {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = "Realtime priority: inactive, playback stopped";
        return realtime_priority_status_;
    }

    const std::string current_status = verified_realtime_priority_status(tid);
    if (current_status.find("active, SCHED_") != std::string::npos) {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = current_status;
        last_realtime_priority_error_.clear();
        return realtime_priority_status_;
    }

    const int priority = realtime_priority_.load(std::memory_order_relaxed);
    std::string direct_error;
    if (direct_make_thread_realtime(tid, priority, direct_error)) {
        const std::string verified = verified_realtime_priority_status(tid);
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = verified;
        if (verified.find("active, SCHED_") != std::string::npos) {
            last_realtime_priority_error_.clear();
        } else {
            last_realtime_priority_error_ = "Direct scheduler request returned but scheduler was not changed";
            realtime_priority_status_ += "\n" + last_realtime_priority_error_;
        }
        Logger::instance().info(realtime_priority_status_);
        return realtime_priority_status_;
    }

    std::string detail;
    if (!rtkit_name_has_owner(&detail)) {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        last_realtime_priority_error_ = detail.empty() ? "RTKit service not available" : detail;
        realtime_priority_status_ = current_status + "\n" + direct_error + "\nRTKit: not available";
        return realtime_priority_status_;
    }

    int effective_priority = 0;
    int max_priority = 0;
    std::string rtkit_error;
    std::string message;
    if (rtkit_make_thread_realtime(tid, priority, effective_priority, max_priority, rtkit_error)) {
        message = verified_realtime_priority_status(tid);
        if (message.find("active, SCHED_") == std::string::npos) {
            std::ostringstream ss;
            ss << "RTKit request returned but scheduler was not changed; requested "
               << effective_priority << "; service max " << max_priority << "; TID " << tid;
            rtkit_error = ss.str();
            message += "\n" + rtkit_error;
        }
    } else {
        message = current_status + "\n" + direct_error + "\n" + rtkit_error;
    }

    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = message;
        if (message.find("active, SCHED_") != std::string::npos) {
            last_realtime_priority_error_.clear();
        } else {
            last_realtime_priority_error_ = rtkit_error.empty() ? direct_error : rtkit_error;
        }
    }
    Logger::instance().info(message);
    return message;
}

std::string PlaybackEngine::refresh_realtime_priority_status() {
    std::string message;
    if (!realtime_priority_enabled_.load(std::memory_order_relaxed)) {
        message = "Realtime priority: disabled";
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = message;
        last_realtime_priority_error_.clear();
        return message;
    }

    const long tid = playback_thread_tid_.load(std::memory_order_relaxed);
    if (tid > 0) {
        message = request_realtime_priority_for_playback_thread();
    } else {
        message = "Realtime priority: inactive, playback stopped";
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = message;
        last_realtime_priority_error_.clear();
    }
    return message;
}

std::string PlaybackEngine::try_set_realtime_priority_for_current_thread() {
    playback_thread_tid_.store(static_cast<long>(syscall(SYS_gettid)), std::memory_order_relaxed);
    return request_realtime_priority_for_playback_thread();
}

std::string PlaybackEngine::active_output_report() const {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    return last_active_output_report_;
}

void PlaybackEngine::playback_loop() {
    playback_thread_tid_.store(static_cast<long>(syscall(SYS_gettid)), std::memory_order_relaxed);
    if (realtime_priority_enabled_.load(std::memory_order_relaxed)) {
        try_set_realtime_priority_for_current_thread();
    } else {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        realtime_priority_status_ = "Realtime priority: disabled";
    }
    try {
        std::vector<PcmSample> block(4096);
        std::uint64_t played_samples_per_channel = initial_samples_per_channel_;
        float smoothed_peak = 0.0f;
        auto last_update = std::chrono::steady_clock::now();
        int active_bass_db = bass_db_.load(std::memory_order_relaxed);
        int active_treble_db = treble_db_.load(std::memory_order_relaxed);
        int active_bass_hz = bass_hz_.load(std::memory_order_relaxed);
        int active_treble_hz = treble_hz_.load(std::memory_order_relaxed);
        ShelfCoefficients low = tone::make_low_shelf(format_.sample_rate, static_cast<double>(active_bass_db), static_cast<double>(active_bass_hz));
        ShelfCoefficients high = tone::make_high_shelf(format_.sample_rate, static_cast<double>(active_treble_db), static_cast<double>(active_treble_hz));
        ShelfState low_l{}, low_r{}, high_l{}, high_r{};
        DeepBassState deep_bass_l{}, deep_bass_r{};
        bool active_deep_bass_enabled = deep_bass_enabled_.load(std::memory_order_relaxed);
        int active_deep_bass_preset = deep_bass_preset_.load(std::memory_order_relaxed);
        int active_deep_bass_amount = deep_bass_amount_.load(std::memory_order_relaxed);
        const std::uint16_t ch = std::max<std::uint16_t>(1, format_.channels);
        while (!stop_requested_ && !decoder_->eof()) {
            wait_if_paused();
            if (stop_requested_) break;
            const std::size_t got = decoder_->read_samples(block.data(), block.size());
            if (got == 0) break;

            const int current_soft_volume_percent = soft_volume_percent_.load(std::memory_order_relaxed);
            const int current_bass_db = bass_db_.load(std::memory_order_relaxed);
            const int current_treble_db = treble_db_.load(std::memory_order_relaxed);
            const int current_bass_hz = bass_hz_.load(std::memory_order_relaxed);
            const int current_treble_hz = treble_hz_.load(std::memory_order_relaxed);
            const int current_pre_eq_headroom_tenths_db = pre_eq_headroom_tenths_db_.load(std::memory_order_relaxed);
            const bool current_deep_bass_enabled = deep_bass_enabled_.load(std::memory_order_relaxed);
            const int current_deep_bass_preset = deep_bass_preset_.load(std::memory_order_relaxed);
            const int current_deep_bass_amount = deep_bass_amount_.load(std::memory_order_relaxed);
            const double current_deep_bass_amount_gain = tone::deep_bass_amount_gain_from_steps(current_deep_bass_amount);
            if (current_deep_bass_enabled != active_deep_bass_enabled || current_deep_bass_preset != active_deep_bass_preset) {
                active_deep_bass_enabled = current_deep_bass_enabled;
                active_deep_bass_preset = current_deep_bass_preset;
                active_deep_bass_amount = current_deep_bass_amount;
                deep_bass_l = DeepBassState{};
                deep_bass_r = DeepBassState{};
            }
            if (current_deep_bass_amount != active_deep_bass_amount) {
                active_deep_bass_amount = current_deep_bass_amount;
            }
            if (current_bass_db != active_bass_db || current_bass_hz != active_bass_hz) {
                active_bass_db = current_bass_db;
                active_bass_hz = current_bass_hz;
                low = tone::make_low_shelf(format_.sample_rate, static_cast<double>(active_bass_db), static_cast<double>(active_bass_hz));
                low_l = ShelfState{};
                low_r = ShelfState{};
            }
            if (current_treble_db != active_treble_db || current_treble_hz != active_treble_hz) {
                active_treble_db = current_treble_db;
                active_treble_hz = current_treble_hz;
                high = tone::make_high_shelf(format_.sample_rate, static_cast<double>(active_treble_db), static_cast<double>(active_treble_hz));
                high_l = ShelfState{};
                high_r = ShelfState{};
            }
            const bool dsp_active = current_soft_volume_percent < 100 || current_bass_db != 0 || current_treble_db != 0 || current_pre_eq_headroom_tenths_db > 0 || current_deep_bass_enabled;
            const double user_volume = static_cast<double>(current_soft_volume_percent) / 100.0;
            const double pre_eq_headroom_db = static_cast<double>(current_pre_eq_headroom_tenths_db) / 10.0;
            const double pre_eq_headroom_gain = std::pow(10.0, -pre_eq_headroom_db / 20.0);
            const bool measure_level = level_meter_enabled_.load(std::memory_order_relaxed);
            const bool detect_clip = clip_detection_enabled_.load(std::memory_order_relaxed);
            const double full_scale = static_cast<double>(pcm_full_scale(format_.bits_per_sample));
            const double inv_full_scale = full_scale > 0.0 ? (1.0 / full_scale) : 0.0;
            float peak = 0.0f;
            std::uint32_t clipped_samples = 0;
            bool clip_detected = false;
            if (dsp_active) {
                for (std::size_t i = 0; i < got; ++i) {
                    const bool left = (ch == 1) || ((i % ch) == 0);
                    double sample = static_cast<double>(block[i]);
                    sample *= pre_eq_headroom_gain;
                    if (current_bass_db != 0) sample = process_sample(sample, low, left ? low_l : low_r);
                    if (current_treble_db != 0) sample = process_sample(sample, high, left ? high_l : high_r);
                    if (current_deep_bass_enabled && inv_full_scale > 0.0) {
                        const double normalized = sample * inv_full_scale;
                        sample = tone::process_deep_bass_normalized(normalized, format_.sample_rate, static_cast<tone::DeepBassPreset>(current_deep_bass_preset), left ? deep_bass_l : deep_bass_r, current_deep_bass_amount_gain) * full_scale;
                    }
                    sample *= user_volume;
                    if (measure_level) {
                        const double meter_mag = full_scale > 0.0 ? (std::fabs(sample) / full_scale) : 0.0;
                        if (meter_mag > static_cast<double>(peak)) peak = static_cast<float>(meter_mag);
                    }
                    if (detect_clip && sample_exceeds_full_scale(sample, format_.bits_per_sample)) {
                        clip_detected = true;
                        ++clipped_samples;
                    }
                    block[i] = static_cast<PcmSample>(std::llround(clamp_sample_to_bits(sample, format_.bits_per_sample)));
                }
            } else {
                if (measure_level) {
                    for (std::size_t i = 0; i < got; ++i) {
                        const double meter_mag = full_scale > 0.0 ? (std::fabs(static_cast<double>(block[i])) / full_scale) : 0.0;
                        if (meter_mag > static_cast<double>(peak)) peak = static_cast<float>(meter_mag);
                    }
                }
            }
            const float instant_peak = peak;
            smoothed_peak = (smoothed_peak * 0.32f) + (instant_peak * 0.68f);
            backend_->write_samples(block.data(), got);
            played_samples_per_channel += got / ch;
            const auto now = std::chrono::steady_clock::now();
            if (now - last_update >= std::chrono::milliseconds(16)) {
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    snapshot_.current_samples_per_channel = played_samples_per_channel;
                    snapshot_.message = pause_requested_ ? "Paused" : "Playing";
                    snapshot_.peak_level = smoothed_peak;
                    snapshot_.clip_detected = clip_detected;
                    snapshot_.clipped_samples = clipped_samples;
                }
                update_status(false);
                last_update = now;
            }
        }
        if (backend_ && !stop_requested_) backend_->drain();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            snapshot_.current_samples_per_channel = played_samples_per_channel;
            snapshot_.finished = !stop_requested_ && last_error_.empty();
            snapshot_.playing = false;
            snapshot_.paused = false;
            if (!last_error_.empty()) {
                snapshot_.message = last_error_;
            } else if (!stop_requested_ &&
                       decoder_->total_samples_per_channel() == 0 &&
                       played_samples_per_channel <= initial_samples_per_channel_) {
                snapshot_.message = "Stream unavailable";
            } else {
                snapshot_.message = "Stopped";
            }
            snapshot_.peak_level = 0.0f;
            snapshot_.clip_detected = false;
            snapshot_.clipped_samples = 0;
        }
        playback_completed_ = true;
        update_status(true);
    } catch (const std::exception& ex) {
        set_error(ex.what());
        { std::lock_guard<std::mutex> lock(state_mutex_); snapshot_.playing = false; snapshot_.paused = false; snapshot_.message = last_error_; }
        playback_completed_ = true;
        update_status(true);
    } catch (...) {
        set_error("Unknown playback error");
        playback_completed_ = true;
        update_status(true);
    }
}

void PlaybackEngine::wait_if_paused() {
    if (!pause_requested_) return;
    std::unique_lock<std::mutex> lock(pause_mutex_);
    pause_cv_.wait(lock, [this]() { return !pause_requested_ || stop_requested_; });
}
void PlaybackEngine::update_status(bool) { if (callback_) callback_(snapshot()); }
void PlaybackEngine::set_error(const std::string& message) {
    { std::lock_guard<std::mutex> lock(state_mutex_); last_error_ = message; snapshot_.playing = false; snapshot_.paused = false; snapshot_.finished = false; snapshot_.message = message; snapshot_.peak_level = 0.0f; snapshot_.clip_detected = false; snapshot_.clipped_samples = 0; }
    Logger::instance().error(message);
    stop_requested_ = true;
    pause_cv_.notify_all();
}
void PlaybackEngine::join_threads() {
    if (playback_thread_.joinable()) {
        playback_thread_.join();
    }
}

} // namespace pcmtp
