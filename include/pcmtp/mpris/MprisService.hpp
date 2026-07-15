#pragma once

#include <gio/gio.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pcmtp {

struct MprisPlayerState {
    std::string playback_status;
    bool can_control = false;
    bool can_play = false;
    bool can_pause = false;
    bool can_seek = false;
    bool can_go_next = false;
    bool can_go_previous = false;
    double volume = 1.0;
    std::int64_t position_usec = 0;
    std::int64_t length_usec = 0;
    std::string title;
    std::string artist;
    std::string url;
    std::string art_url;
    int track_number = 0;
    bool repeat_playlist = false;
    bool has_track = false;
};

class MprisService {
public:
    struct Actions {
        std::function<void()> play;
        std::function<void()> pause;
        std::function<void()> play_pause;
        std::function<void()> stop;
        std::function<void()> next;
        std::function<void()> previous;
        std::function<void(std::int64_t offset_usec)> seek;
        std::function<void(std::int64_t position_usec)> set_position;
        std::function<void(const std::string& uri)> open_uri;
        std::function<void(double volume)> set_volume;
        std::function<void(bool repeat_playlist)> set_repeat_playlist;
        std::function<void()> raise;
        std::function<MprisPlayerState()> get_state;
    };

    explicit MprisService(Actions actions);
    ~MprisService();

    MprisService(const MprisService&) = delete;
    MprisService& operator=(const MprisService&) = delete;

    void start();
    void stop();
    void notify_state_changed();
    void notify_metadata_changed();
    void notify_playback_status_changed();

private:
    void disconnect_bus();
    static void on_bus_acquired(GDBusConnection* connection, const gchar* name, gpointer user_data);
    static void on_name_acquired(GDBusConnection* connection, const gchar* name, gpointer user_data);
    static void on_name_lost(GDBusConnection* connection, const gchar* name, gpointer user_data);
    static void handle_method_call(GDBusConnection* connection,
                                   const gchar* sender,
                                   const gchar* object_path,
                                   const gchar* interface_name,
                                   const gchar* method_name,
                                   GVariant* parameters,
                                   GDBusMethodInvocation* invocation,
                                   gpointer user_data);
    static GVariant* handle_get_property(GDBusConnection* connection,
                                         const gchar* sender,
                                         const gchar* object_path,
                                         const gchar* interface_name,
                                         const gchar* property_name,
                                         GError** error,
                                         gpointer user_data);
    static gboolean on_position_timer(gpointer user_data);

    void register_object(GDBusConnection* connection);
    void unregister_object();
    void update_position_timer();
    void emit_player_properties(GVariantBuilder* changed_builder);
    GVariant* player_property(const char* property_name) const;
    MprisPlayerState current_state() const;

    Actions actions_;
    GDBusConnection* connection_ = nullptr;
    std::vector<unsigned int> registration_ids_;
    unsigned int bus_owner_id_ = 0;
    unsigned int position_timer_id_ = 0;
    std::string last_playback_status_;
    std::string last_metadata_signature_;
    bool bus_connected_ = false;
};

} // namespace pcmtp
