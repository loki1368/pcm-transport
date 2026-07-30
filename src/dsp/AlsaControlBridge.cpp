// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/dsp/AlsaControlBridge.hpp"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace pcmtp {
namespace {

std::string to_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool is_interesting_name(const std::string& name) {
    const std::string lower = to_lower(name);
    return lower.find("playback") != std::string::npos ||
           lower.find("volume") != std::string::npos ||
           lower.find("switch") != std::string::npos ||
           lower.find("bass") != std::string::npos ||
           lower.find("treble") != std::string::npos ||
           lower.find("tone") != std::string::npos ||
           lower.find("master") != std::string::npos ||
           lower.find("front") != std::string::npos ||
           lower.find("line") != std::string::npos ||
           lower.find("pcm") != std::string::npos;
}

bool is_primary_name(const std::string& name) {
    const std::string lower = to_lower(name);
    return lower.find("bass") != std::string::npos ||
           lower.find("treble") != std::string::npos ||
           lower.find("tone") != std::string::npos ||
           lower.find("master playback volume") != std::string::npos ||
           lower.find("front playback volume") != std::string::npos;
}

bool read_control(snd_hctl_elem_t* elem, DspControlInfo& out) {
    if (elem == nullptr) {
        return false;
    }

    snd_ctl_elem_info_t* info = nullptr;
    snd_ctl_elem_id_t* id = nullptr;
    snd_ctl_elem_value_t* value = nullptr;
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_value_alloca(&value);

    if (snd_hctl_elem_info(elem, info) < 0) {
        return false;
    }

    const snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);
    if (type != SND_CTL_ELEM_TYPE_BOOLEAN && type != SND_CTL_ELEM_TYPE_INTEGER) {
        return false;
    }

    snd_hctl_elem_get_id(elem, id);
    out.numid = static_cast<int>(snd_ctl_elem_id_get_numid(id));
    const char* name = snd_ctl_elem_info_get_name(info);
    out.name = name != nullptr ? name : "";
    out.channel_count = static_cast<int>(snd_ctl_elem_info_get_count(info));
    out.is_boolean = (type == SND_CTL_ELEM_TYPE_BOOLEAN);
    out.min_value = out.is_boolean ? 0 : snd_ctl_elem_info_get_min(info);
    out.max_value = out.is_boolean ? 1 : snd_ctl_elem_info_get_max(info);
    out.step = 1;

    snd_ctl_elem_value_set_id(value, id);
    if (snd_hctl_elem_read(elem, value) < 0) {
        return false;
    }

    out.value = out.is_boolean ? snd_ctl_elem_value_get_boolean(value, 0)
                               : snd_ctl_elem_value_get_integer(value, 0);
    if (out.channel_count > 1) {
        out.right_value = out.is_boolean ? snd_ctl_elem_value_get_boolean(value, 1)
                                         : snd_ctl_elem_value_get_integer(value, 1);
    } else {
        out.right_value = out.value;
    }
    return true;
}

} // namespace

DspConnectionInfo AlsaControlBridge::probe(int card_index) {
    DspConnectionInfo result;

    const std::string ctl_name = "hw:" + std::to_string(card_index);
    snd_hctl_t* hctl = nullptr;
    if (snd_hctl_open(&hctl, ctl_name.c_str(), 0) < 0 || hctl == nullptr) {
        result.status_text = "ALSA hardware mixer not available for selected device.";
        result.diagnostics_text = "hw:" + std::to_string(card_index) + " could not be opened via snd_hctl.";
        return result;
    }

    if (snd_hctl_load(hctl) < 0) {
        snd_hctl_close(hctl);
        result.status_text = "ALSA hardware mixer could not be loaded.";
        result.diagnostics_text = "snd_hctl_load failed for hw:" + std::to_string(card_index) + ".";
        return result;
    }

    for (snd_hctl_elem_t* elem = snd_hctl_first_elem(hctl); elem != nullptr; elem = snd_hctl_elem_next(elem)) {
        DspControlInfo info;
        if (!read_control(elem, info)) {
            continue;
        }
        result.controls.push_back(info);
        if (is_interesting_name(info.name)) {
            result.filtered_controls.push_back(info);
        }
        if (is_primary_name(info.name)) {
            result.bass_treble_controls.push_back(info);
        }
    }

    snd_hctl_close(hctl);

    auto dedupe_by_numid = [](std::vector<DspControlInfo>& items) {
        std::vector<DspControlInfo> unique;
        std::vector<int> seen;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (std::find(seen.begin(), seen.end(), items[i].numid) != seen.end()) {
                continue;
            }
            seen.push_back(items[i].numid);
            unique.push_back(items[i]);
        }
        items.swap(unique);
    };
    dedupe_by_numid(result.controls);
    dedupe_by_numid(result.filtered_controls);
    dedupe_by_numid(result.bass_treble_controls);

    result.low_level_connected = !result.controls.empty();
    if (result.low_level_connected) {
        result.status_text = "ALSA hardware mixer controls detected";
    } else {
        result.status_text = "No writable ALSA hardware mixer controls detected";
    }

    std::ostringstream diag;
    diag << "Card " << card_index << ": "
         << result.controls.size() << " writable controls";
    if (!result.bass_treble_controls.empty()) {
        diag << ", " << result.bass_treble_controls.size() << " primary tone/level controls";
    }
    result.diagnostics_text = diag.str();
    return result;
}

bool AlsaControlBridge::set_control_value(int card_index, int numid, long new_value, std::string* error_message) {
    const std::string ctl_name = "hw:" + std::to_string(card_index);
    snd_ctl_t* ctl = nullptr;
    if (snd_ctl_open(&ctl, ctl_name.c_str(), 0) < 0 || ctl == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Cannot open ALSA control interface for " + ctl_name;
        }
        return false;
    }

    snd_ctl_elem_id_t* id = nullptr;
    snd_ctl_elem_info_t* info = nullptr;
    snd_ctl_elem_value_t* value = nullptr;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&value);

    snd_ctl_elem_id_set_numid(id, static_cast<unsigned int>(numid));
    snd_ctl_elem_info_set_id(info, id);
    if (snd_ctl_elem_info(ctl, info) < 0) {
        snd_ctl_close(ctl);
        if (error_message != nullptr) {
            *error_message = "Cannot query ALSA control numid=" + std::to_string(numid);
        }
        return false;
    }

    const snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);
    const unsigned int count = snd_ctl_elem_info_get_count(info);
    const long min_value = (type == SND_CTL_ELEM_TYPE_BOOLEAN) ? 0 : snd_ctl_elem_info_get_min(info);
    const long max_value = (type == SND_CTL_ELEM_TYPE_BOOLEAN) ? 1 : snd_ctl_elem_info_get_max(info);
    if (new_value < min_value) {
        new_value = min_value;
    }
    if (new_value > max_value) {
        new_value = max_value;
    }

    snd_ctl_elem_value_set_id(value, id);
    for (unsigned int i = 0; i < count; ++i) {
        if (type == SND_CTL_ELEM_TYPE_BOOLEAN) {
            snd_ctl_elem_value_set_boolean(value, i, new_value != 0 ? 1 : 0);
        } else if (type == SND_CTL_ELEM_TYPE_INTEGER) {
            snd_ctl_elem_value_set_integer(value, i, new_value);
        }
    }

    const int rc = snd_ctl_elem_write(ctl, value);
    snd_ctl_close(ctl);
    if (rc < 0) {
        if (error_message != nullptr) {
            *error_message = "snd_ctl_elem_write failed for numid=" + std::to_string(numid);
        }
        return false;
    }
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

} // namespace pcmtp
