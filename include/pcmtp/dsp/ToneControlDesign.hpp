#pragma once

#include <cstdint>

namespace pcmtp {
namespace tone {

struct ShelfCoefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

enum class DeepBassPreset : int {
    Current = 0,
    Focused = 1,
    Aggressive = 2,
    Punchy = 3,
    Delicate = 4,
    Warm = 5
};

struct DeepBassState {
    double foundation_lp = 0.0;
    double contour_lp = 0.0;
    double low_mid_lp = 0.0;
    double harmonic_hp_lp = 0.0;
    double harmonic_band_lp = 0.0;
    double envelope = 0.0;
    double low_mid_envelope = 0.0;
    std::uint32_t cached_sample_rate = 0;
    std::uint32_t control_counter = 0;
    double foundation_alpha = 0.0;
    double contour_alpha = 0.0;
    double low_mid_alpha = 0.0;
    double envelope_attack_alpha = 0.0;
    double envelope_release_alpha = 0.0;
    double low_mid_env_attack_alpha = 0.0;
    double low_mid_env_release_alpha = 0.0;
    double target_foundation_mix = 0.0;
    double target_cleanup_mix = 0.0;
    double target_harmonic_mix = 0.0;
    double target_drive = 0.0;
    double target_shape = 0.0;
    double target_harmonic_hp_alpha = 0.0;
    double target_harmonic_lp_alpha = 0.0;
    double foundation_mix = 0.0;
    double cleanup_mix = 0.0;
    double harmonic_mix = 0.0;
    double drive = 0.0;
    double shape = 0.0;
    double harmonic_hp_alpha = 0.0;
    double harmonic_lp_alpha = 0.0;
};

constexpr int kBassMinHz = 40;
constexpr int kBassMaxHz = 220;
constexpr int kTrebleMinHz = 3000;
constexpr int kTrebleMaxHz = 16000;
constexpr double kBaxandallBassShelfSlope = 1.40;
constexpr double kBaxandallTrebleShelfSlope = 1.08;

int clamp_bass_hz(int hz);
int clamp_treble_hz(int hz);
ShelfCoefficients make_low_shelf(std::uint32_t sample_rate, double gain_db, double cutoff_hz);
ShelfCoefficients make_high_shelf(std::uint32_t sample_rate, double gain_db, double cutoff_hz);
double cascaded_shelf_response_db(std::uint32_t sample_rate,
                                  int bass_db,
                                  int bass_hz,
                                  int treble_db,
                                  int treble_hz,
                                  double hz);
double deep_bass_amount_gain_from_steps(int amount_steps);
double process_deep_bass_normalized(double input,
                                    std::uint32_t sample_rate,
                                    DeepBassPreset preset,
                                    DeepBassState& state,
                                    double amount_gain = 1.0);
void process_deep_bass_normalized_stereo(double& left,
                                         double& right,
                                         std::uint32_t sample_rate,
                                         DeepBassPreset preset,
                                         DeepBassState& left_state,
                                         DeepBassState& right_state,
                                         double amount_gain = 1.0);
double estimate_total_processing_max_gain_db(std::uint32_t sample_rate,
                                             int bass_db,
                                             int bass_hz,
                                             int treble_db,
                                             int treble_hz,
                                             bool deep_bass_enabled,
                                             DeepBassPreset deep_bass_preset,
                                             int deep_bass_amount_steps = 0);
double estimate_cascaded_shelf_max_gain_db(std::uint32_t sample_rate,
                                           int bass_db,
                                           int bass_hz,
                                           int treble_db,
                                           int treble_hz);

} // namespace tone
} // namespace pcmtp
