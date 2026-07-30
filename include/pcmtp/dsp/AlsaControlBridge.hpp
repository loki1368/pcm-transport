// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>
#include <vector>

namespace pcmtp {

struct DspControlInfo {
    int numid = -1;
    std::string name;
    int channel_count = 1;
    long min_value = 0;
    long max_value = 0;
    long step = 1;
    long value = 0;
    long right_value = 0;
    bool is_boolean = false;
};

struct DspConnectionInfo {
    bool low_level_connected = false;
    std::string status_text;
    std::string diagnostics_text;
    std::vector<DspControlInfo> controls;
    std::vector<DspControlInfo> filtered_controls;
    std::vector<DspControlInfo> bass_treble_controls;
};

class AlsaControlBridge {
public:
    static DspConnectionInfo probe(int card_index);
    static bool set_control_value(int card_index, int numid, long value, std::string* error_message);
};

} // namespace pcmtp
