#include "pcmtp/mpris/MprisService.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace pcmtp {
namespace {

constexpr const char* kBusName = "org.mpris.MediaPlayer2.pcmtransport";
constexpr const char* kObjectPath = "/org/mpris/MediaPlayer2";
constexpr const char* kNoTrackObjectPath = "/org/mpris/MediaPlayer2/TrackList/NoTrack";
constexpr const char* kIdentity = "PCM Transport";
constexpr const char* kDesktopEntry = "pcm_transport";

constexpr const char* kIntrospectionXml =
    "<node>"
    "  <interface name='org.freedesktop.DBus.Properties'>"
    "    <method name='Get'>"
    "      <arg type='s' name='interface_name' direction='in'/>"
    "      <arg type='s' name='property_name' direction='in'/>"
    "      <arg type='v' name='value' direction='out'/>"
    "    </method>"
    "    <method name='GetAll'>"
    "      <arg type='s' name='interface_name' direction='in'/>"
    "      <arg type='a{sv}' name='properties' direction='out'/>"
    "    </method>"
    "    <method name='Set'>"
    "      <arg type='s' name='interface_name' direction='in'/>"
    "      <arg type='s' name='property_name' direction='in'/>"
    "      <arg type='v' name='value' direction='in'/>"
    "    </method>"
    "    <signal name='PropertiesChanged'>"
    "      <arg type='s' name='interface_name'/>"
    "      <arg type='a{sv}' name='changed_properties'/>"
    "      <arg type='as' name='invalidated_properties'/>"
    "    </signal>"
    "  </interface>"
    "  <interface name='org.mpris.MediaPlayer2'>"
    "    <method name='Raise'/>"
    "    <method name='Quit'/>"
    "    <property name='CanQuit' type='b' access='read'/>"
    "    <property name='CanRaise' type='b' access='read'/>"
    "    <property name='CanSetFullscreen' type='b' access='read'/>"
    "    <property name='Fullscreen' type='b' access='readwrite'/>"
    "    <property name='HasTrackList' type='b' access='read'/>"
    "    <property name='Identity' type='s' access='read'/>"
    "    <property name='DesktopEntry' type='s' access='read'/>"
    "    <property name='SupportedUriSchemes' type='as' access='read'/>"
    "    <property name='SupportedMimeTypes' type='as' access='read'/>"
    "  </interface>"
    "  <interface name='org.mpris.MediaPlayer2.Player'>"
    "    <method name='Next'/>"
    "    <method name='Previous'/>"
    "    <method name='Pause'/>"
    "    <method name='PlayPause'/>"
    "    <method name='Stop'/>"
    "    <method name='Play'/>"
    "    <method name='Seek'>"
    "      <arg type='x' name='Offset' direction='in'/>"
    "    </method>"
    "    <method name='SetPosition'>"
    "      <arg type='o' name='TrackId' direction='in'/>"
    "      <arg type='x' name='Position' direction='in'/>"
    "    </method>"
    "    <method name='OpenUri'>"
    "      <arg type='s' name='Uri' direction='in'/>"
    "    </method>"
    "    <signal name='Seeked'>"
    "      <arg type='x' name='Position'/>"
    "    </signal>"
    "    <property name='PlaybackStatus' type='s' access='read'/>"
    "    <property name='LoopStatus' type='s' access='readwrite'/>"
    "    <property name='Rate' type='d' access='readwrite'/>"
    "    <property name='Metadata' type='a{sv}' access='read'/>"
    "    <property name='Volume' type='d' access='readwrite'/>"
    "    <property name='Position' type='x' access='read'/>"
    "    <property name='MinimumRate' type='d' access='read'/>"
    "    <property name='MaximumRate' type='d' access='read'/>"
    "    <property name='CanGoNext' type='b' access='read'/>"
    "    <property name='CanGoPrevious' type='b' access='read'/>"
    "    <property name='CanPlay' type='b' access='read'/>"
    "    <property name='CanPause' type='b' access='read'/>"
    "    <property name='CanSeek' type='b' access='read'/>"
    "    <property name='CanControl' type='b' access='read'/>"
    "    <property name='Shuffle' type='b' access='readwrite'/>"
    "  </interface>"
    "</node>";

class VariantRef {
public:
    explicit VariantRef(GVariant* variant)
        : variant_(variant != nullptr ? g_variant_ref(variant) : nullptr) {}

    VariantRef(const VariantRef&) = delete;
    VariantRef& operator=(const VariantRef&) = delete;

    ~VariantRef() {
        if (variant_ != nullptr) {
            g_variant_unref(variant_);
        }
    }

    GVariant* get() const {
        return variant_;
    }

private:
    GVariant* variant_ = nullptr;
};

GVariant* metadata_variant(const MprisPlayerState& state) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

    const char* track_id = state.has_track && !state.track_id.empty() ? state.track_id.c_str() : kNoTrackObjectPath;
    g_variant_builder_add(&builder, "{sv}", "mpris:trackid", g_variant_new_object_path(track_id));

    if (state.has_track) {
        g_variant_builder_add(&builder, "{sv}", "xesam:title", g_variant_new_string(state.title.c_str()));

        GVariantBuilder artist_builder;
        g_variant_builder_init(&artist_builder, G_VARIANT_TYPE("as"));
        if (!state.artist.empty()) {
            g_variant_builder_add(&artist_builder, "s", state.artist.c_str());
        }
        g_variant_builder_add(&builder, "{sv}", "xesam:artist", g_variant_builder_end(&artist_builder));

        if (!state.url.empty()) {
            g_variant_builder_add(&builder, "{sv}", "xesam:url", g_variant_new_string(state.url.c_str()));
        }
        if (!state.art_url.empty()) {
            g_variant_builder_add(&builder, "{sv}", "mpris:artUrl", g_variant_new_string(state.art_url.c_str()));
        }
        if (state.length_usec > 0) {
            g_variant_builder_add(&builder, "{sv}", "mpris:length", g_variant_new_int64(state.length_usec));
        }
        if (state.track_number > 0) {
            g_variant_builder_add(&builder, "{sv}", "xesam:trackNumber", g_variant_new_int32(state.track_number));
        }
    }

    return g_variant_builder_end(&builder);
}

std::string metadata_signature(const MprisPlayerState& state) {
    return state.title + "|" + state.artist + "|" + state.url + "|" + state.art_url + "|" +
           std::to_string(state.length_usec) + "|" + std::to_string(state.track_number) + "|" +
           state.track_id + "|" + std::to_string(state.track_epoch) + "|" + (state.has_track ? "1" : "0");
}

std::string capabilities_signature(const MprisPlayerState& state) {
    return std::to_string(state.can_control) + "|" + std::to_string(state.can_play) + "|" +
           std::to_string(state.can_pause) + "|" + std::to_string(state.can_seek) + "|" +
           std::to_string(state.can_go_next) + "|" + std::to_string(state.can_go_previous);
}

void emit_properties_changed(GDBusConnection* connection,
                             const char* interface_name,
                             GVariantBuilder* changed_builder) {
    if (connection == nullptr || changed_builder == nullptr) {
        return;
    }

    GVariantBuilder invalidated_builder;
    g_variant_builder_init(&invalidated_builder, G_VARIANT_TYPE("as"));

    g_dbus_connection_emit_signal(connection,
                                  nullptr,
                                  kObjectPath,
                                  "org.freedesktop.DBus.Properties",
                                  "PropertiesChanged",
                                  g_variant_new("(sa{sv}as)",
                                                interface_name,
                                                changed_builder,
                                                &invalidated_builder),
                                  nullptr);
}

void add_capability_properties(GVariantBuilder* changed_builder, const MprisPlayerState& state) {
    g_variant_builder_add(changed_builder, "{sv}", "CanGoNext", g_variant_new_boolean(state.can_go_next));
    g_variant_builder_add(changed_builder, "{sv}", "CanGoPrevious", g_variant_new_boolean(state.can_go_previous));
    g_variant_builder_add(changed_builder, "{sv}", "CanSeek", g_variant_new_boolean(state.can_seek));
    g_variant_builder_add(changed_builder, "{sv}", "CanPlay", g_variant_new_boolean(state.can_play));
    g_variant_builder_add(changed_builder, "{sv}", "CanPause", g_variant_new_boolean(state.can_pause));
    g_variant_builder_add(changed_builder, "{sv}", "CanControl", g_variant_new_boolean(state.can_control));
}

} // namespace

MprisService::MprisService(Actions actions)
    : actions_(std::move(actions)) {}

MprisService::~MprisService() {
    stop();
}

MprisPlayerState MprisService::current_state() const {
    if (actions_.get_state) {
        return actions_.get_state();
    }
    return MprisPlayerState{};
}

GVariant* MprisService::root_property_from_state(const char* property_name, const MprisPlayerState& state) const {
    if (std::strcmp(property_name, "CanQuit") == 0) {
        return g_variant_new_boolean(FALSE);
    }
    if (std::strcmp(property_name, "CanRaise") == 0) {
        return g_variant_new_boolean(TRUE);
    }
    if (std::strcmp(property_name, "CanSetFullscreen") == 0) {
        return g_variant_new_boolean(FALSE);
    }
    if (std::strcmp(property_name, "Fullscreen") == 0) {
        return g_variant_new_boolean(state.fullscreen ? TRUE : FALSE);
    }
    if (std::strcmp(property_name, "HasTrackList") == 0) {
        return g_variant_new_boolean(FALSE);
    }
    if (std::strcmp(property_name, "Identity") == 0) {
        return g_variant_new_string(kIdentity);
    }
    if (std::strcmp(property_name, "DesktopEntry") == 0) {
        return g_variant_new_string(kDesktopEntry);
    }
    if (std::strcmp(property_name, "SupportedUriSchemes") == 0) {
        const char* schemes[] = {"file", "http", "https"};
        return g_variant_new_strv(schemes, 3);
    }
    if (std::strcmp(property_name, "SupportedMimeTypes") == 0) {
        const char* mime_types[] = {
            "audio/flac",
            "audio/mpeg",
            "audio/x-flac",
            "audio/wav",
            "audio/x-wav",
            "application/x-cue",
            "audio/x-mpegurl",
        };
        return g_variant_new_strv(mime_types, G_N_ELEMENTS(mime_types));
    }
    return nullptr;
}

GVariant* MprisService::player_property_from_state(const char* property_name, const MprisPlayerState& state) const {
    if (std::strcmp(property_name, "PlaybackStatus") == 0) {
        return g_variant_new_string(state.playback_status.c_str());
    }
    if (std::strcmp(property_name, "LoopStatus") == 0) {
        return g_variant_new_string(state.loop_status.c_str());
    }
    if (std::strcmp(property_name, "Rate") == 0) {
        return g_variant_new_double(1.0);
    }
    if (std::strcmp(property_name, "Metadata") == 0) {
        return metadata_variant(state);
    }
    if (std::strcmp(property_name, "Volume") == 0) {
        return g_variant_new_double(state.volume);
    }
    if (std::strcmp(property_name, "Position") == 0) {
        return g_variant_new_int64(state.position_usec);
    }
    if (std::strcmp(property_name, "MinimumRate") == 0) {
        return g_variant_new_double(1.0);
    }
    if (std::strcmp(property_name, "MaximumRate") == 0) {
        return g_variant_new_double(1.0);
    }
    if (std::strcmp(property_name, "CanGoNext") == 0) {
        return g_variant_new_boolean(state.can_go_next);
    }
    if (std::strcmp(property_name, "CanGoPrevious") == 0) {
        return g_variant_new_boolean(state.can_go_previous);
    }
    if (std::strcmp(property_name, "CanPlay") == 0) {
        return g_variant_new_boolean(state.can_play);
    }
    if (std::strcmp(property_name, "CanPause") == 0) {
        return g_variant_new_boolean(state.can_pause);
    }
    if (std::strcmp(property_name, "CanSeek") == 0) {
        return g_variant_new_boolean(state.can_seek);
    }
    if (std::strcmp(property_name, "CanControl") == 0) {
        return g_variant_new_boolean(state.can_control);
    }
    if (std::strcmp(property_name, "Shuffle") == 0) {
        return g_variant_new_boolean(state.shuffle);
    }
    return nullptr;
}

void MprisService::emit_player_properties(GVariantBuilder* changed_builder) {
    if (!bus_connected_ || connection_ == nullptr) {
        return;
    }
    emit_properties_changed(connection_, "org.mpris.MediaPlayer2.Player", changed_builder);
}

void MprisService::emit_seeked(std::int64_t position_usec) {
    if (!bus_connected_ || connection_ == nullptr) {
        return;
    }

    g_dbus_connection_emit_signal(connection_,
                                  nullptr,
                                  kObjectPath,
                                  "org.mpris.MediaPlayer2.Player",
                                  "Seeked",
                                  g_variant_new("(x)", position_usec),
                                  nullptr);
}

void MprisService::notify_seeked(std::int64_t position_usec) {
    emit_seeked(position_usec);
}

void MprisService::handle_method_call(GDBusConnection*,
                                      const gchar*,
                                      const gchar*,
                                      const gchar* interface_name,
                                      const gchar* method_name,
                                      GVariant* parameters,
                                      GDBusMethodInvocation* invocation,
                                      gpointer user_data) {
    auto* service = static_cast<MprisService*>(user_data);
    if (service == nullptr) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Missing service");
        return;
    }

    if (std::strcmp(interface_name, "org.freedesktop.DBus.Properties") == 0) {
        if (std::strcmp(method_name, "Get") == 0) {
            const char* property_interface = nullptr;
            const char* property_name = nullptr;
            g_variant_get(parameters, "(&s&s)", &property_interface, &property_name);

            const MprisPlayerState state = service->current_state();
            GVariant* value = nullptr;
            if (std::strcmp(property_interface, "org.mpris.MediaPlayer2") == 0) {
                value = service->root_property_from_state(property_name, state);
            } else if (std::strcmp(property_interface, "org.mpris.MediaPlayer2.Player") == 0) {
                value = service->player_property_from_state(property_name, state);
            }

            if (value == nullptr) {
                g_dbus_method_invocation_return_error(invocation,
                                                      G_DBUS_ERROR,
                                                      G_DBUS_ERROR_UNKNOWN_PROPERTY,
                                                      "Unknown property %s",
                                                      property_name);
                return;
            }

            g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
            return;
        }

        if (std::strcmp(method_name, "GetAll") == 0) {
            const char* property_interface = nullptr;
            g_variant_get(parameters, "(&s)", &property_interface);

            const MprisPlayerState state = service->current_state();
            GVariantBuilder builder;
            g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

            if (std::strcmp(property_interface, "org.mpris.MediaPlayer2") == 0) {
                const char* names[] = {
                    "CanQuit", "CanRaise", "CanSetFullscreen", "Fullscreen", "HasTrackList", "Identity",
                    "DesktopEntry", "SupportedUriSchemes", "SupportedMimeTypes",
                };
                for (const char* name : names) {
                    GVariant* prop_value = service->root_property_from_state(name, state);
                    if (prop_value != nullptr) {
                        g_variant_builder_add(&builder, "{sv}", name, prop_value);
                    }
                }
            } else if (std::strcmp(property_interface, "org.mpris.MediaPlayer2.Player") == 0) {
                const char* names[] = {
                    "PlaybackStatus", "LoopStatus", "Rate", "Metadata", "Volume", "Position",
                    "MinimumRate", "MaximumRate", "CanGoNext", "CanGoPrevious", "CanPlay",
                    "CanPause", "CanSeek", "CanControl", "Shuffle",
                };
                for (const char* name : names) {
                    GVariant* prop_value = service->player_property_from_state(name, state);
                    if (prop_value != nullptr) {
                        g_variant_builder_add(&builder, "{sv}", name, prop_value);
                    }
                }
            }

            GVariant* properties = g_variant_builder_end(&builder);
            g_dbus_method_invocation_return_value(invocation, g_variant_new_tuple(&properties, 1));
            return;
        }

        if (std::strcmp(method_name, "Set") == 0) {
            const char* property_interface = nullptr;
            const char* property_name = nullptr;
            GVariant* value = nullptr;
            g_variant_get(parameters, "(&s&sv)", &property_interface, &property_name, &value);
            VariantRef value_ref(value);

            if (value_ref.get() == nullptr) {
                g_dbus_method_invocation_return_error(invocation,
                                                      G_DBUS_ERROR,
                                                      G_DBUS_ERROR_INVALID_ARGS,
                                                      "Missing property value");
                return;
            }

            if (std::strcmp(property_interface, "org.mpris.MediaPlayer2") == 0) {
                if (std::strcmp(property_name, "Fullscreen") == 0) {
                    if (!g_variant_is_of_type(value_ref.get(), G_VARIANT_TYPE_BOOLEAN)) {
                        g_dbus_method_invocation_return_error(invocation,
                                                              G_DBUS_ERROR,
                                                              G_DBUS_ERROR_INVALID_ARGS,
                                                              "Invalid Fullscreen value type");
                        return;
                    }
                    const bool fullscreen = g_variant_get_boolean(value_ref.get()) != FALSE;
                    if (service->actions_.set_fullscreen) {
                        service->actions_.set_fullscreen(fullscreen);
                    }
                    g_dbus_method_invocation_return_value(invocation, nullptr);
                    service->notify_state_changed();
                    return;
                }

                g_dbus_method_invocation_return_error(invocation,
                                                      G_DBUS_ERROR,
                                                      G_DBUS_ERROR_PROPERTY_READ_ONLY,
                                                      "Property %s is read-only",
                                                      property_name);
                return;
            }

            if (std::strcmp(property_interface, "org.mpris.MediaPlayer2.Player") == 0) {
                if (std::strcmp(property_name, "Volume") == 0) {
                    if (!g_variant_is_of_type(value_ref.get(), G_VARIANT_TYPE("d"))) {
                        g_dbus_method_invocation_return_error(invocation,
                                                              G_DBUS_ERROR,
                                                              G_DBUS_ERROR_INVALID_ARGS,
                                                              "Invalid Volume value type");
                        return;
                    }
                    gdouble volume = 0.0;
                    g_variant_get(value_ref.get(), "d", &volume);
                    volume = std::max(0.0, std::min(1.0, volume));
                    if (service->actions_.set_volume) {
                        service->actions_.set_volume(volume);
                    }
                    g_dbus_method_invocation_return_value(invocation, nullptr);
                    service->notify_state_changed();
                    return;
                }
                if (std::strcmp(property_name, "LoopStatus") == 0) {
                    if (!g_variant_is_of_type(value_ref.get(), G_VARIANT_TYPE_STRING)) {
                        g_dbus_method_invocation_return_error(invocation,
                                                              G_DBUS_ERROR,
                                                              G_DBUS_ERROR_INVALID_ARGS,
                                                              "Invalid LoopStatus value type");
                        return;
                    }
                    const char* loop_status = g_variant_get_string(value_ref.get(), nullptr);
                    if (loop_status == nullptr ||
                        (std::strcmp(loop_status, "None") != 0 && std::strcmp(loop_status, "Track") != 0 &&
                         std::strcmp(loop_status, "Playlist") != 0)) {
                        g_dbus_method_invocation_return_error(invocation,
                                                              G_DBUS_ERROR,
                                                              G_DBUS_ERROR_INVALID_ARGS,
                                                              "Invalid LoopStatus value");
                        return;
                    }
                    if (service->actions_.set_loop_status) {
                        service->actions_.set_loop_status(loop_status);
                    }
                    g_dbus_method_invocation_return_value(invocation, nullptr);
                    service->notify_state_changed();
                    return;
                }
                if (std::strcmp(property_name, "Rate") == 0) {
                    if (!g_variant_is_of_type(value_ref.get(), G_VARIANT_TYPE("d"))) {
                        g_dbus_method_invocation_return_error(invocation,
                                                              G_DBUS_ERROR,
                                                              G_DBUS_ERROR_INVALID_ARGS,
                                                              "Invalid Rate value type");
                        return;
                    }
                    gdouble rate = 0.0;
                    g_variant_get(value_ref.get(), "d", &rate);
                    if (service->actions_.set_rate) {
                        service->actions_.set_rate(rate);
                    }
                    g_dbus_method_invocation_return_value(invocation, nullptr);
                    return;
                }
                if (std::strcmp(property_name, "Shuffle") == 0) {
                    if (!g_variant_is_of_type(value_ref.get(), G_VARIANT_TYPE_BOOLEAN)) {
                        g_dbus_method_invocation_return_error(invocation,
                                                              G_DBUS_ERROR,
                                                              G_DBUS_ERROR_INVALID_ARGS,
                                                              "Invalid Shuffle value type");
                        return;
                    }
                    const bool shuffle = g_variant_get_boolean(value_ref.get()) != FALSE;
                    if (service->actions_.set_shuffle) {
                        service->actions_.set_shuffle(shuffle);
                    }
                    g_dbus_method_invocation_return_value(invocation, nullptr);
                    service->notify_state_changed();
                    return;
                }
            }

            g_dbus_method_invocation_return_error(invocation,
                                                  G_DBUS_ERROR,
                                                  G_DBUS_ERROR_PROPERTY_READ_ONLY,
                                                  "Property %s is read-only",
                                                  property_name);
            return;
        }
    }

    if (std::strcmp(interface_name, "org.mpris.MediaPlayer2") == 0) {
        if (std::strcmp(method_name, "Raise") == 0) {
            if (service->actions_.raise) {
                service->actions_.raise();
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }
        if (std::strcmp(method_name, "Quit") == 0) {
            g_dbus_method_invocation_return_error(invocation,
                                                  G_DBUS_ERROR,
                                                  G_DBUS_ERROR_NOT_SUPPORTED,
                                                  "Quit is not supported");
            return;
        }
    }

    if (std::strcmp(interface_name, "org.mpris.MediaPlayer2.Player") == 0) {
        if (std::strcmp(method_name, "Play") == 0) {
            if (service->actions_.play) {
                service->actions_.play();
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            service->notify_state_changed();
            return;
        }
        if (std::strcmp(method_name, "Pause") == 0) {
            if (service->actions_.pause) {
                service->actions_.pause();
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            service->notify_state_changed();
            return;
        }
        if (std::strcmp(method_name, "PlayPause") == 0) {
            if (service->actions_.play_pause) {
                service->actions_.play_pause();
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            service->notify_state_changed();
            return;
        }
        if (std::strcmp(method_name, "Stop") == 0) {
            if (service->actions_.stop) {
                service->actions_.stop();
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            service->notify_state_changed();
            return;
        }
        if (std::strcmp(method_name, "Next") == 0) {
            if (service->actions_.next) {
                service->actions_.next();
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            service->notify_state_changed();
            return;
        }
        if (std::strcmp(method_name, "Previous") == 0) {
            if (service->actions_.previous) {
                service->actions_.previous();
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            service->notify_state_changed();
            return;
        }
        if (std::strcmp(method_name, "Seek") == 0) {
            gint64 offset = 0;
            g_variant_get(parameters, "(x)", &offset);
            if (service->actions_.seek) {
                const std::int64_t position_usec = service->actions_.seek(static_cast<std::int64_t>(offset));
                if (position_usec >= 0) {
                    service->notify_seeked(position_usec);
                }
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }
        if (std::strcmp(method_name, "SetPosition") == 0) {
            const char* track_id = nullptr;
            gint64 position = 0;
            g_variant_get(parameters, "(&ox)", &track_id, &position);
            if (service->actions_.set_position) {
                const std::string track_id_value = track_id != nullptr ? track_id : std::string{};
                const std::int64_t position_usec =
                    service->actions_.set_position(static_cast<std::int64_t>(position), track_id_value);
                if (position_usec >= 0) {
                    service->notify_seeked(position_usec);
                }
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }
        if (std::strcmp(method_name, "OpenUri") == 0) {
            const char* uri = nullptr;
            g_variant_get(parameters, "(&s)", &uri);
            if (uri == nullptr || *uri == '\0') {
                g_dbus_method_invocation_return_error(invocation,
                                                      G_DBUS_ERROR,
                                                      G_DBUS_ERROR_INVALID_ARGS,
                                                      "Missing URI");
                return;
            }
            if (!service->actions_.open_uri || !service->actions_.open_uri(uri)) {
                g_dbus_method_invocation_return_error(invocation,
                                                      G_DBUS_ERROR,
                                                      G_DBUS_ERROR_FAILED,
                                                      "Failed to open URI");
                return;
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            service->notify_state_changed();
            return;
        }
    }

    g_dbus_method_invocation_return_error(invocation,
                                          G_DBUS_ERROR,
                                          G_DBUS_ERROR_UNKNOWN_METHOD,
                                          "Unknown method %s",
                                          method_name);
}

GVariant* MprisService::handle_get_property(GDBusConnection*,
                                              const gchar*,
                                              const gchar*,
                                              const gchar* interface_name,
                                              const gchar* property_name,
                                              GError**,
                                              gpointer user_data) {
    auto* service = static_cast<MprisService*>(user_data);
    if (service == nullptr) {
        return nullptr;
    }

    const MprisPlayerState state = service->current_state();
    if (std::strcmp(interface_name, "org.mpris.MediaPlayer2") == 0) {
        return service->root_property_from_state(property_name, state);
    }
    if (std::strcmp(interface_name, "org.mpris.MediaPlayer2.Player") == 0) {
        return service->player_property_from_state(property_name, state);
    }
    return nullptr;
}

void MprisService::register_object(GDBusConnection* connection) {
    GError* error = nullptr;
    GDBusNodeInfo* introspection = g_dbus_node_info_new_for_xml(kIntrospectionXml, &error);
    if (introspection == nullptr) {
        if (error != nullptr) {
            g_error_free(error);
        }
        return;
    }

    static const GDBusInterfaceVTable interface_vtable = {
        handle_method_call,
        handle_get_property,
        nullptr,
        {0},
    };

    if (registration_ids_.empty()) {
        for (GDBusInterfaceInfo** iface = introspection->interfaces; *iface != nullptr; ++iface) {
            GError* register_error = nullptr;
            const unsigned int registration_id = g_dbus_connection_register_object(connection,
                                                                                   kObjectPath,
                                                                                   *iface,
                                                                                   &interface_vtable,
                                                                                   this,
                                                                                   nullptr,
                                                                                   &register_error);
            if (registration_id == 0) {
                if (register_error != nullptr) {
                    g_error_free(register_error);
                }
                continue;
            }
            registration_ids_.push_back(registration_id);
        }
    }

    g_dbus_node_info_unref(introspection);
}

void MprisService::disconnect_bus() {
    bus_connected_ = false;
    unregister_object();

    if (connection_ != nullptr) {
        g_object_unref(connection_);
        connection_ = nullptr;
    }
}

void MprisService::unregister_object() {
    if (connection_ == nullptr) {
        registration_ids_.clear();
        return;
    }

    for (unsigned int registration_id : registration_ids_) {
        g_dbus_connection_unregister_object(connection_, registration_id);
    }
    registration_ids_.clear();
}

void MprisService::on_bus_acquired(GDBusConnection* connection, const gchar*, gpointer user_data) {
    auto* service = static_cast<MprisService*>(user_data);
    if (service == nullptr) {
        return;
    }

    if (service->connection_ != nullptr) {
        service->unregister_object();
        g_object_unref(service->connection_);
        service->connection_ = nullptr;
    }

    service->connection_ = connection;
    g_object_ref(service->connection_);
    service->register_object(connection);

    if (service->registration_ids_.empty()) {
        return;
    }

    service->bus_connected_ = true;
}

void MprisService::on_name_acquired(GDBusConnection*, const gchar*, gpointer) {
}

void MprisService::on_name_lost(GDBusConnection*, const gchar*, gpointer user_data) {
    auto* service = static_cast<MprisService*>(user_data);
    if (service == nullptr) {
        return;
    }

    service->disconnect_bus();
}

void MprisService::start() {
    if (bus_owner_id_ != 0) {
        return;
    }

    bus_owner_id_ = g_bus_own_name(G_BUS_TYPE_SESSION,
                                   kBusName,
                                   G_BUS_NAME_OWNER_FLAGS_NONE,
                                   on_bus_acquired,
                                   on_name_acquired,
                                   on_name_lost,
                                   this,
                                   nullptr);
}

void MprisService::stop() {
    if (bus_owner_id_ != 0) {
        const unsigned int owner_id = bus_owner_id_;
        bus_owner_id_ = 0;
        g_bus_unown_name(owner_id);
    }

    disconnect_bus();
}

void MprisService::notify_state_changed() {
    if (!bus_connected_ || connection_ == nullptr || !actions_.get_state) {
        return;
    }

    const MprisPlayerState state = current_state();
    const std::string metadata_signature_value = metadata_signature(state);
    const std::string capabilities_signature_value = capabilities_signature(state);
    const bool metadata_changed = metadata_signature_value != last_metadata_signature_;
    const bool playback_changed = state.playback_status != last_playback_status_;
    const bool initial = last_metadata_signature_.empty();
    const bool volume_changed = initial || std::abs(state.volume - last_volume_) > 1e-9;
    const bool loop_changed = initial || state.loop_status != last_loop_status_;
    const bool shuffle_changed = initial || state.shuffle != last_shuffle_;
    const bool fullscreen_changed = initial || state.fullscreen != last_fullscreen_;
    const bool capabilities_changed = last_capabilities_signature_.empty() ||
                                      capabilities_signature_value != last_capabilities_signature_;

    if (!metadata_changed && !playback_changed && !volume_changed && !loop_changed && !shuffle_changed &&
        !fullscreen_changed && !capabilities_changed) {
        return;
    }

    GVariantBuilder changed_builder;
    g_variant_builder_init(&changed_builder, G_VARIANT_TYPE("a{sv}"));

    if (metadata_changed) {
        last_metadata_signature_ = metadata_signature_value;
        GVariant* metadata = metadata_variant(state);
        if (metadata != nullptr) {
            g_variant_builder_add(&changed_builder, "{sv}", "Metadata", metadata);
        }
    }

    if (playback_changed) {
        last_playback_status_ = state.playback_status;
        g_variant_builder_add(&changed_builder,
                              "{sv}",
                              "PlaybackStatus",
                              g_variant_new_string(state.playback_status.c_str()));
    }

    if (volume_changed) {
        last_volume_ = state.volume;
        g_variant_builder_add(&changed_builder, "{sv}", "Volume", g_variant_new_double(state.volume));
    }

    if (loop_changed) {
        last_loop_status_ = state.loop_status;
        g_variant_builder_add(&changed_builder,
                              "{sv}",
                              "LoopStatus",
                              g_variant_new_string(state.loop_status.c_str()));
    }

    if (shuffle_changed) {
        last_shuffle_ = state.shuffle;
        g_variant_builder_add(&changed_builder, "{sv}", "Shuffle", g_variant_new_boolean(state.shuffle));
    }

    if (fullscreen_changed) {
        last_fullscreen_ = state.fullscreen;
        GVariantBuilder root_builder;
        g_variant_builder_init(&root_builder, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&root_builder, "{sv}", "Fullscreen", g_variant_new_boolean(state.fullscreen));
        emit_properties_changed(connection_, "org.mpris.MediaPlayer2", &root_builder);
    }

    if (capabilities_changed) {
        last_capabilities_signature_ = capabilities_signature_value;
        add_capability_properties(&changed_builder, state);
    }

    if (metadata_changed || playback_changed || volume_changed || loop_changed || shuffle_changed ||
        capabilities_changed) {
        emit_player_properties(&changed_builder);
    }
}

} // namespace pcmtp
