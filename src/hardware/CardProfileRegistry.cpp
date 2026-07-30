// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/hardware/CardProfileRegistry.hpp"

#include <alsa/asoundlib.h>

#include "pcmtp/dsp/AlsaControlBridge.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace pcmtp {

namespace {

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool looks_like_legacy_audigy(const std::string& short_name, const std::string& long_name) {
    const std::string a = to_lower(short_name);
    const std::string b = to_lower(long_name);
    return a.find("audigy") != std::string::npos ||
           b.find("audigy") != std::string::npos ||
           b.find("sb0090") != std::string::npos ||
           b.find("sb0092") != std::string::npos;
}

std::string pcm_device_display_name(snd_ctl_t* ctl, int device) {
    snd_pcm_info_t* info = nullptr;
    snd_pcm_info_alloca(&info);
    snd_pcm_info_set_device(info, device);
    snd_pcm_info_set_subdevice(info, 0);
    snd_pcm_info_set_stream(info, SND_PCM_STREAM_PLAYBACK);
    if (snd_ctl_pcm_info(ctl, info) >= 0) {
        const char* name = snd_pcm_info_get_name(info);
        if (name != nullptr && *name != '\0') {
            return name;
        }
    }
    return "PCM " + std::to_string(device);
}

} // namespace

std::vector<CardProfileInfo> CardProfileRegistry::probe_cards() {
    std::vector<CardProfileInfo> result;

    int card = -1;
    if (snd_card_next(&card) < 0) {
        return result;
    }

    while (card >= 0) {
        char* name = nullptr;
        char* long_name = nullptr;
        std::string short_name;
        std::string full_name;
        if (snd_card_get_name(card, &name) >= 0 && snd_card_get_longname(card, &long_name) >= 0) {
            short_name = name != nullptr ? name : "";
            full_name = long_name != nullptr ? long_name : "";
        }

        const bool is_legacy_audigy = looks_like_legacy_audigy(short_name, full_name);
        const DspConnectionInfo dsp = AlsaControlBridge::probe(card);

        snd_ctl_t* ctl = nullptr;
        const std::string ctl_name = "hw:" + std::to_string(card);
        if (snd_ctl_open(&ctl, ctl_name.c_str(), 0) >= 0 && ctl != nullptr) {
            int device = -1;
            while (true) {
                if (snd_ctl_pcm_next_device(ctl, &device) < 0 || device < 0) {
                    break;
                }

                snd_pcm_info_t* info = nullptr;
                snd_pcm_info_alloca(&info);
                snd_pcm_info_set_device(info, device);
                snd_pcm_info_set_subdevice(info, 0);
                snd_pcm_info_set_stream(info, SND_PCM_STREAM_PLAYBACK);
                if (snd_ctl_pcm_info(ctl, info) < 0) {
                    continue;
                }

                CardProfileInfo entry;
                entry.card_index = card;
                entry.pcm_device_index = device;
                entry.short_name = short_name;
                entry.long_name = full_name;
                entry.pcm_device_name = pcm_device_display_name(ctl, device);
                entry.hw_device = "hw:" + std::to_string(card) + "," + std::to_string(device);
                entry.plughw_device = "plughw:" + std::to_string(card) + "," + std::to_string(device);
                entry.legacy_audigy_like = is_legacy_audigy;
                entry.alsa_hw_profile = is_legacy_audigy ? "legacy_audigy_alsa_profile" : "generic_alsa_hw_profile";
                entry.low_level_features_available = dsp.low_level_connected || !dsp.controls.empty();
                entry.dsp_low_level_connected = dsp.low_level_connected;
                entry.dsp_status = dsp.status_text.empty() ? "ALSA mixer controls available" : dsp.status_text;
                result.push_back(entry);
            }
            snd_ctl_close(ctl);
        }

        if (name != nullptr) {
            std::free(name);
        }
        if (long_name != nullptr) {
            std::free(long_name);
        }

        if (snd_card_next(&card) < 0) {
            break;
        }
    }

    return result;
}

} // namespace pcmtp
