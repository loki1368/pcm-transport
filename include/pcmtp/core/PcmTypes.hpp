// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pcmtp {

struct AudioFormat {
    std::uint32_t sample_rate = 44100;
    std::uint16_t channels = 2;
    std::uint16_t bits_per_sample = 16;

    bool is_red_book() const {
        return sample_rate == 44100 && channels == 2 && bits_per_sample == 16;
    }

    std::string to_string() const {
        return std::to_string(sample_rate) + " Hz / " +
               std::to_string(bits_per_sample) + "-bit / " +
               (channels == 2 ? std::string("stereo") : std::to_string(channels) + " ch");
    }
};

using PcmSample = std::int32_t;

inline std::int64_t pcm_full_scale(std::uint16_t bits_per_sample) {
    if (bits_per_sample >= 32) return 2147483647LL;
    if (bits_per_sample <= 1) return 1LL;
    return (1LL << (bits_per_sample - 1)) - 1LL;
}

inline std::int64_t pcm_min_value(std::uint16_t bits_per_sample) {
    if (bits_per_sample >= 32) return static_cast<std::int64_t>(INT32_MIN);
    if (bits_per_sample <= 1) return -1LL;
    return -(1LL << (bits_per_sample - 1));
}

using PcmBuffer = std::vector<PcmSample>;

} // namespace pcmtp
