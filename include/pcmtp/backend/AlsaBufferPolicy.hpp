#pragma once

#include <cstdint>

namespace pcmtp {

struct AlsaBufferPolicy {
    std::uint64_t period_frames = 588;
    std::uint64_t buffer_frames = 2352;
};

inline constexpr AlsaBufferPolicy alsa_buffer_policy_for_sample_rate(std::uint32_t sample_rate) {
    if (sample_rate <= 192000U) {
        return AlsaBufferPolicy{588U, 2352U};
    }
    if (sample_rate <= 384000U) {
        return AlsaBufferPolicy{1176U, 4704U};
    }
    if (sample_rate <= 768000U) {
        return AlsaBufferPolicy{2352U, 9408U};
    }
    // The highest defined tier is intentionally capped here. No additional
    // scaling policy is introduced for rates above 1.536 MHz.
    return AlsaBufferPolicy{4704U, 18816U};
}

static_assert(alsa_buffer_policy_for_sample_rate(44100U).period_frames == 588U &&
              alsa_buffer_policy_for_sample_rate(44100U).buffer_frames == 2352U,
              "44.1 kHz buffer policy");
static_assert(alsa_buffer_policy_for_sample_rate(48000U).period_frames == 588U &&
              alsa_buffer_policy_for_sample_rate(48000U).buffer_frames == 2352U,
              "48 kHz buffer policy");
static_assert(alsa_buffer_policy_for_sample_rate(176400U).period_frames == 588U &&
              alsa_buffer_policy_for_sample_rate(176400U).buffer_frames == 2352U,
              "176.4 kHz buffer policy");
static_assert(alsa_buffer_policy_for_sample_rate(192000U).period_frames == 588U &&
              alsa_buffer_policy_for_sample_rate(192000U).buffer_frames == 2352U,
              "192 kHz buffer policy");
static_assert(alsa_buffer_policy_for_sample_rate(352800U).period_frames == 1176U &&
              alsa_buffer_policy_for_sample_rate(352800U).buffer_frames == 4704U,
              "352.8 kHz buffer policy");
static_assert(alsa_buffer_policy_for_sample_rate(384000U).period_frames == 1176U &&
              alsa_buffer_policy_for_sample_rate(384000U).buffer_frames == 4704U,
              "384 kHz buffer policy");
static_assert(alsa_buffer_policy_for_sample_rate(705600U).period_frames == 2352U &&
              alsa_buffer_policy_for_sample_rate(705600U).buffer_frames == 9408U,
              "705.6 kHz buffer policy");
static_assert(alsa_buffer_policy_for_sample_rate(768000U).period_frames == 2352U &&
              alsa_buffer_policy_for_sample_rate(768000U).buffer_frames == 9408U,
              "768 kHz buffer policy");
static_assert(alsa_buffer_policy_for_sample_rate(1411200U).period_frames == 4704U &&
              alsa_buffer_policy_for_sample_rate(1411200U).buffer_frames == 18816U,
              "1.4112 MHz buffer policy");
static_assert(alsa_buffer_policy_for_sample_rate(1536000U).period_frames == 4704U &&
              alsa_buffer_policy_for_sample_rate(1536000U).buffer_frames == 18816U,
              "1.536 MHz buffer policy");

} // namespace pcmtp
