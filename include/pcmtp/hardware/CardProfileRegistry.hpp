// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>
#include <vector>

namespace pcmtp {

struct CardProfileInfo {
    int card_index = -1;
    std::string short_name;
    std::string long_name;
    int pcm_device_index = 0;
    std::string pcm_device_name;
    std::string hw_device;
    std::string plughw_device;
    std::string alsa_hw_profile;
    bool low_level_features_available = false;
    bool legacy_audigy_like = false;
    bool dsp_low_level_connected = false;
    std::string dsp_status;
};

class CardProfileRegistry {
public:
    static std::vector<CardProfileInfo> probe_cards();
};

} // namespace pcmtp
