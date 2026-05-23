#include "pcmtp/dsp/ToneControlDesign.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace pcmtp {
namespace tone {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kMinAuditedHz = 10.0;
constexpr int kResponseAuditPoints = 2048;
constexpr double kHeadroomEstimateFloorDb = 0.0;
constexpr double kDeepBassTestDurationSeconds = 0.30;
constexpr double kDeepBassTestSettleSeconds = 0.12;
constexpr int kDeepBassLowFreqTests = 20;
constexpr int kDeepBassMultiToneTests = 6;
constexpr std::uint32_t kDeepBassControlPeriodSamples = 24;
constexpr std::uint32_t kDeepBassEnvelopePeriodSamples = 8;

ShelfCoefficients make_identity() {
    return ShelfCoefficients{};
}

double clamp_frequency(double hz, int min_hz, int max_hz) {
    return std::max(static_cast<double>(min_hz), std::min(static_cast<double>(max_hz), hz));
}

ShelfCoefficients normalize_coefficients(double b0,
                                         double b1,
                                         double b2,
                                         double a0,
                                         double a1,
                                         double a2) {
    ShelfCoefficients c;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}

std::complex<double> frequency_response(const ShelfCoefficients& c, double w) {
    const std::complex<double> z1 = std::exp(std::complex<double>(0.0, -w));
    const std::complex<double> z2 = std::exp(std::complex<double>(0.0, -2.0 * w));
    const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
    const std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;
    return num / den;
}

ShelfCoefficients make_shelf(bool high,
                             std::uint32_t sample_rate,
                             double gain_db,
                             double cutoff_hz,
                             int min_hz,
                             int max_hz,
                             double slope) {
    if (sample_rate == 0 || std::fabs(gain_db) < 0.001) {
        return make_identity();
    }

    const double hz = clamp_frequency(cutoff_hz, min_hz, max_hz);
    const double nyquist_hz = static_cast<double>(sample_rate) * 0.5;
    if (hz <= 0.0 || hz >= nyquist_hz) {
        return make_identity();
    }

    const double A = std::pow(10.0, gain_db / 40.0);
    const double w0 = kTwoPi * hz / static_cast<double>(sample_rate);
    const double cos_w0 = std::cos(w0);
    const double sin_w0 = std::sin(w0);
    const double alpha = (sin_w0 * 0.5) * std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);
    const double beta = 2.0 * std::sqrt(A) * alpha;

    if (!high) {
        const double b0 = A * ((A + 1.0) - (A - 1.0) * cos_w0 + beta);
        const double b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_w0);
        const double b2 = A * ((A + 1.0) - (A - 1.0) * cos_w0 - beta);
        const double a0 = (A + 1.0) + (A - 1.0) * cos_w0 + beta;
        const double a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_w0);
        const double a2 = (A + 1.0) + (A - 1.0) * cos_w0 - beta;
        return normalize_coefficients(b0, b1, b2, a0, a1, a2);
    }

    const double b0 = A * ((A + 1.0) + (A - 1.0) * cos_w0 + beta);
    const double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w0);
    const double b2 = A * ((A + 1.0) + (A - 1.0) * cos_w0 - beta);
    const double a0 = (A + 1.0) - (A - 1.0) * cos_w0 + beta;
    const double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w0);
    const double a2 = (A + 1.0) - (A - 1.0) * cos_w0 - beta;
    return normalize_coefficients(b0, b1, b2, a0, a1, a2);
}

double one_pole_alpha(std::uint32_t sample_rate, double cutoff_hz) {
    if (sample_rate == 0 || cutoff_hz <= 0.0) return 1.0;
    const double x = 1.0 - std::exp((-2.0 * 3.14159265358979323846 * cutoff_hz) / static_cast<double>(sample_rate));
    return std::max(0.0, std::min(1.0, x));
}

double process_lowpass(double input, double alpha, double& state) {
    state += alpha * (input - state);
    return state;
}

double process_highpass(double input, double alpha, double& state) {
    return input - process_lowpass(input, alpha, state);
}


double process_envelope(double input_abs,
                        double attack_alpha,
                        double release_alpha,
                        double& state) {
    const double alpha = (input_abs > state) ? attack_alpha : release_alpha;
    state += alpha * (input_abs - state);
    return state;
}

double clamp01(double v) {
    return std::max(0.0, std::min(1.0, v));
}




struct DeepBassPresetParams {
    double foundation_bias;
    double foundation_gain;
    double harmonic_bias;
    double harmonic_gain;
    double cleanup_bias;
    double cleanup_gain;
    double contour_fill_bias;
    double contour_fill_gain;
    double drive_bias;
    double drive_gain;
    double shape_bias;
    double shape_gain;
    double env_scale;
    double low_mid_scale;
    double contour_floor;
    double contour_ceiling;
    double hp_bias_hz;
    double hp_lowmid_hz;
    double hp_contour_hz;
    double lp_bias_hz;
    double lp_contour_hz;
    double lp_lowmid_hz;
};

const DeepBassPresetParams& preset_params(DeepBassPreset preset) {
    static const std::array<DeepBassPresetParams, 6> kPresets{{
        {0.118, 0.154, 0.186, 0.126, 0.013, 0.020, 0.024, 0.032, 1.86, 1.86, 0.34, 0.26, 0.76, 0.11, 0.48, 1.00, 76.0, 10.0, 10.0, 216.0, 44.0, -10.0}, // Current
        {0.120, 0.150, 0.184, 0.122, 0.017, 0.024, 0.024, 0.030, 1.84, 1.72, 0.34, 0.25, 0.80, 0.12, 0.50, 1.00, 80.0, 12.0, 8.0, 208.0, 36.0, -10.0},   // Focused
        {0.166, 0.214, 0.228, 0.176, 0.008, 0.012, 0.040, 0.052, 2.18, 2.20, 0.46, 0.30, 0.56, 0.08, 0.43, 1.02, 62.0, 8.0, 16.0, 238.0, 60.0, -6.0},      // Aggressive
        {0.112, 0.148, 0.214, 0.150, 0.010, 0.015, 0.046, 0.056, 2.10, 2.02, 0.28, 0.22, 0.64, 0.09, 0.44, 1.04, 78.0, 6.0, 4.0, 224.0, 52.0, -5.0},        // Punchy
        {0.040, 0.050, 0.080, 0.045, 0.028, 0.034, 0.006, 0.008, 1.18, 0.92, 0.38, 0.08, 1.10, 0.22, 0.66, 0.88, 94.0, 20.0, 12.0, 184.0, 18.0, -18.0},      // Delicate
        {0.126, 0.158, 0.176, 0.110, 0.014, 0.020, 0.026, 0.032, 1.78, 1.62, 0.46, 0.23, 0.74, 0.10, 0.50, 0.99, 72.0, 10.0, 8.0, 220.0, 36.0, -10.0}        // Warm
    }};
    return kPresets[static_cast<int>(preset) < 0 || static_cast<int>(preset) >= static_cast<int>(kPresets.size()) ? 0 : static_cast<int>(preset)];
}
double soft_clip3(double x) {
    const double xc = std::max(-1.5, std::min(1.5, x));
    return xc - (xc * xc * xc) / 3.0;
}



double deep_bass_headroom_tail_db(DeepBassPreset preset) {
    switch (preset) {
        case DeepBassPreset::Punchy: return 0.80;
        case DeepBassPreset::Delicate: return 1.10;
        case DeepBassPreset::Aggressive: return 0.70;
        case DeepBassPreset::Warm: return 0.60;
        case DeepBassPreset::Focused: return 0.55;
        default: return 0.55;
    }
}

double fast_tanh_like(double x) {
    const double xc = std::max(-3.0, std::min(3.0, x));
    const double x2 = xc * xc;
    return (xc * (27.0 + x2)) / (27.0 + 9.0 * x2);
}

double smooth_parameter(double current, double target) {
    return current + 0.18 * (target - current);
}

double deep_bass_cleanup_mix(const DeepBassPresetParams& p, double contour_strength, double low_mid_ratio) {
    return p.cleanup_bias + p.cleanup_gain * contour_strength + (0.012 + p.low_mid_scale * 0.04) * low_mid_ratio;
}

double deep_bass_foundation_mix(const DeepBassPresetParams& p, double contour_strength) {
    return p.foundation_bias + p.foundation_gain * contour_strength;
}

double deep_bass_harmonic_mix(const DeepBassPresetParams& p, double contour_strength, double low_mid_ratio) {
    return p.harmonic_bias + p.harmonic_gain * contour_strength + (0.026 + p.low_mid_scale * 0.02) * (1.0 - low_mid_ratio);
}

double deep_bass_shape(const DeepBassPresetParams& p, double contour_strength, double low_mid_ratio) {
    return clamp01(p.shape_bias + p.shape_gain * contour_strength + 0.10 * (1.0 - low_mid_ratio));
}

double simulate_deep_bass_peak(std::uint32_t sample_rate,
                               DeepBassPreset preset,
                               double linear_gain_a,
                               double freq_a_hz,
                               double linear_gain_b = 0.0,
                               double freq_b_hz = 0.0,
                               double burst_hz = 0.0,
                               double amount_gain = 1.0) {
    if (sample_rate == 0 || freq_a_hz <= 0.0) {
        return std::max(0.0, linear_gain_a);
    }
    DeepBassState state{};
    const int total_samples = std::max(64, static_cast<int>(std::ceil(kDeepBassTestDurationSeconds * static_cast<double>(sample_rate))));
    const int settle_samples = std::max(0, static_cast<int>(std::ceil(kDeepBassTestSettleSeconds * static_cast<double>(sample_rate))));
    double peak = 0.0;
    double phase_a = 0.0;
    double phase_b = 0.0;
    const double phase_step_a = kTwoPi * freq_a_hz / static_cast<double>(sample_rate);
    const double phase_step_b = kTwoPi * freq_b_hz / static_cast<double>(sample_rate);
    for (int i = 0; i < total_samples; ++i) {
        double burst = 1.0;
        if (burst_hz > 0.0) {
            const double t = static_cast<double>(i) / static_cast<double>(sample_rate);
            burst = 0.28 + 0.72 * (0.5 + 0.5 * std::sin(kTwoPi * burst_hz * t));
        }
        double input = burst * linear_gain_a * std::sin(phase_a);
        phase_a += phase_step_a;
        if (phase_a >= kTwoPi) phase_a -= kTwoPi;
        if (linear_gain_b > 0.0 && freq_b_hz > 0.0) {
            input += burst * linear_gain_b * std::sin(phase_b);
            phase_b += phase_step_b;
            if (phase_b >= kTwoPi) phase_b -= kTwoPi;
        }
        const double output = process_deep_bass_normalized(input, sample_rate, preset, state, amount_gain);
        if (i >= settle_samples) {
            peak = std::max(peak, std::fabs(output));
        }
    }
    return peak;
}

} // namespace

int clamp_bass_hz(int hz) {
    return std::max(kBassMinHz, std::min(kBassMaxHz, hz));
}

int clamp_treble_hz(int hz) {
    return std::max(kTrebleMinHz, std::min(kTrebleMaxHz, hz));
}

double deep_bass_amount_gain_from_steps(int amount_steps) {
    const int clamped = std::max(-2, std::min(2, amount_steps));
    return std::pow(10.0, (static_cast<double>(clamped) * 1.5) / 20.0);
}

ShelfCoefficients make_low_shelf(std::uint32_t sample_rate, double gain_db, double cutoff_hz) {
    return make_shelf(false, sample_rate, gain_db, cutoff_hz, kBassMinHz, kBassMaxHz, kBaxandallBassShelfSlope);
}

ShelfCoefficients make_high_shelf(std::uint32_t sample_rate, double gain_db, double cutoff_hz) {
    return make_shelf(true, sample_rate, gain_db, cutoff_hz, kTrebleMinHz, kTrebleMaxHz, kBaxandallTrebleShelfSlope);
}

double cascaded_shelf_response_db(std::uint32_t sample_rate,
                                  int bass_db,
                                  int bass_hz,
                                  int treble_db,
                                  int treble_hz,
                                  double hz) {
    if (sample_rate == 0 || hz <= 0.0) {
        return 0.0;
    }
    const ShelfCoefficients low = make_low_shelf(sample_rate, static_cast<double>(bass_db), static_cast<double>(bass_hz));
    const ShelfCoefficients high = make_high_shelf(sample_rate, static_cast<double>(treble_db), static_cast<double>(treble_hz));
    const double w = kTwoPi * hz / static_cast<double>(sample_rate);
    const double mag = std::abs(frequency_response(low, w) * frequency_response(high, w));
    return 20.0 * std::log10(std::max(mag, 1.0e-12));
}

double process_deep_bass_normalized(double input,
                                    std::uint32_t sample_rate,
                                    DeepBassPreset preset,
                                    DeepBassState& state,
                                    double amount_gain) {
    const DeepBassPresetParams& params = preset_params(preset);
    const double xn = std::max(-8.0, std::min(8.0, input));
    if (state.cached_sample_rate != sample_rate) {
        state = DeepBassState{};
        state.cached_sample_rate = sample_rate;
        state.foundation_alpha = one_pole_alpha(sample_rate, 74.0);
        state.contour_alpha = one_pole_alpha(sample_rate, 132.0);
        state.low_mid_alpha = one_pole_alpha(sample_rate, 300.0);
        state.envelope_attack_alpha = one_pole_alpha(sample_rate, 18.0);
        state.envelope_release_alpha = one_pole_alpha(sample_rate, 3.6);
        state.low_mid_env_attack_alpha = one_pole_alpha(sample_rate, 13.0);
        state.low_mid_env_release_alpha = one_pole_alpha(sample_rate, 2.6);
        state.target_foundation_mix = state.foundation_mix = deep_bass_foundation_mix(params, 0.66) + 0.018;
        state.target_cleanup_mix = state.cleanup_mix = std::max(0.0, deep_bass_cleanup_mix(params, 0.66, 0.24) - 0.005);
        state.target_harmonic_mix = state.harmonic_mix = deep_bass_harmonic_mix(params, 0.66, 0.24) + 0.008;
        state.target_drive = state.drive = params.drive_bias + params.drive_gain * 0.66 + 0.06;
        state.target_shape = state.shape = clamp01(deep_bass_shape(params, 0.66, 0.24) + 0.03);
        state.target_harmonic_hp_alpha = state.harmonic_hp_alpha = one_pole_alpha(sample_rate, params.hp_bias_hz + params.hp_lowmid_hz * 0.24 + params.hp_contour_hz * (1.0 - 0.68));
        state.target_harmonic_lp_alpha = state.harmonic_lp_alpha = one_pole_alpha(sample_rate, params.lp_bias_hz + params.lp_contour_hz * 0.68 + params.lp_lowmid_hz * 0.24);
    }

    const std::uint32_t tick = state.control_counter++;
    const double foundation = process_lowpass(xn, state.foundation_alpha, state.foundation_lp);
    const double contour = process_lowpass(xn, state.contour_alpha, state.contour_lp);
    const double low_mid_full = process_lowpass(xn, state.low_mid_alpha, state.low_mid_lp);
    const double low_mid = low_mid_full - contour;

    double envelope = state.envelope;
    double low_mid_env = state.low_mid_envelope;
    if ((tick % kDeepBassEnvelopePeriodSamples) == 0) {
        envelope = process_envelope(std::fabs(contour), state.envelope_attack_alpha, state.envelope_release_alpha, state.envelope);
        low_mid_env = process_envelope(std::fabs(low_mid), state.low_mid_env_attack_alpha, state.low_mid_env_release_alpha, state.low_mid_envelope);
    }
    const double low_mid_ratio = clamp01(low_mid_env / (envelope + 1.0e-6));

    if ((tick % kDeepBassControlPeriodSamples) == 0) {
        const double env_term = std::min(1.0, envelope * params.env_scale);
        const double contour_strength = std::max(params.contour_floor, std::min(params.contour_ceiling, 1.16 - 0.08 * env_term - params.low_mid_scale * 0.04 * low_mid_ratio));
        const double drive_lift = (0.06 + 0.10 * (1.0 - env_term)) * (1.0 - low_mid_ratio);
        state.target_foundation_mix = deep_bass_foundation_mix(params, contour_strength) + 0.026;
        state.target_cleanup_mix = std::max(0.0, deep_bass_cleanup_mix(params, contour_strength, low_mid_ratio) - 0.004);
        state.target_harmonic_mix = deep_bass_harmonic_mix(params, contour_strength, low_mid_ratio) + 0.016 * (1.0 - low_mid_ratio);
        state.target_drive = params.drive_bias + params.drive_gain * contour_strength + drive_lift + 0.05;
        state.target_shape = clamp01(deep_bass_shape(params, contour_strength, low_mid_ratio) + 0.07 * (1.0 - env_term));
        const double harmonic_hp_hz = params.hp_bias_hz + params.hp_lowmid_hz * low_mid_ratio + params.hp_contour_hz * (1.0 - contour_strength);
        const double harmonic_lp_hz = params.lp_bias_hz + params.lp_contour_hz * contour_strength + params.lp_lowmid_hz * low_mid_ratio;
        state.target_harmonic_hp_alpha = one_pole_alpha(sample_rate, harmonic_hp_hz);
        state.target_harmonic_lp_alpha = one_pole_alpha(sample_rate, harmonic_lp_hz);
    }

    state.foundation_mix = smooth_parameter(state.foundation_mix, state.target_foundation_mix);
    state.cleanup_mix = smooth_parameter(state.cleanup_mix, state.target_cleanup_mix);
    state.harmonic_mix = smooth_parameter(state.harmonic_mix, state.target_harmonic_mix);
    state.drive = smooth_parameter(state.drive, state.target_drive);
    state.shape = smooth_parameter(state.shape, state.target_shape);
    state.harmonic_hp_alpha = smooth_parameter(state.harmonic_hp_alpha, state.target_harmonic_hp_alpha);
    state.harmonic_lp_alpha = smooth_parameter(state.harmonic_lp_alpha, state.target_harmonic_lp_alpha);

    const double waveshaper_input = (0.96 + 0.18 * state.shape) * state.drive * contour;
    const double nonlinear_primary = fast_tanh_like(waveshaper_input);
    const double nonlinear_secondary = soft_clip3((0.90 + 0.10 * state.shape) * waveshaper_input);
    const double nonlinear = ((1.0 - state.shape) * nonlinear_primary) + (state.shape * nonlinear_secondary);
    const double enhancer = (fast_tanh_like((0.52 + 0.12 * state.shape) * waveshaper_input) - (0.70 * waveshaper_input)) * (0.15 + 0.09 * state.shape);
    const double generated = nonlinear - ((0.30 + 0.07 * state.shape) * contour) + enhancer;
    const double harmonic_hp = process_highpass(generated, state.harmonic_hp_alpha, state.harmonic_hp_lp);
    const double harmonic_band = process_lowpass(harmonic_hp, state.harmonic_lp_alpha, state.harmonic_band_lp);

    const double contour_fill = contour - foundation;
    const double contour_fill_mix = params.contour_fill_bias + params.contour_fill_gain * (0.92 + 0.16 * state.shape);

    const double processed = xn + (state.foundation_mix * foundation) + (contour_fill_mix * contour_fill)
                             + (state.harmonic_mix * harmonic_band) - (state.cleanup_mix * low_mid);
    const double safe_amount = std::max(0.0, amount_gain);
    return xn + safe_amount * (processed - xn);
}

void process_deep_bass_normalized_stereo(double& left,
                                         double& right,
                                         std::uint32_t sample_rate,
                                         DeepBassPreset preset,
                                         DeepBassState& left_state,
                                         DeepBassState& right_state,
                                         double amount_gain) {
    left = process_deep_bass_normalized(left, sample_rate, preset, left_state, amount_gain);
    right = process_deep_bass_normalized(right, sample_rate, preset, right_state, amount_gain);
}


double estimate_cascaded_shelf_max_gain_db(std::uint32_t sample_rate,
                                           int bass_db,
                                           int bass_hz,
                                           int treble_db,
                                           int treble_hz) {
    if (sample_rate == 0 || (bass_db == 0 && treble_db == 0)) {
        return 0.0;
    }

    const double nyquist_hz = static_cast<double>(sample_rate) * 0.5;
    if (nyquist_hz <= kMinAuditedHz) {
        return 0.0;
    }

    double max_db = 0.0;
    const double log_min = std::log(kMinAuditedHz);
    const double log_max = std::log(nyquist_hz);
    for (int i = 0; i < kResponseAuditPoints; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kResponseAuditPoints - 1);
        const double hz = std::exp(log_min + (log_max - log_min) * t);
        const double gain_db = cascaded_shelf_response_db(sample_rate, bass_db, bass_hz, treble_db, treble_hz, hz);
        if (gain_db > max_db) {
            max_db = gain_db;
        }
    }

    return std::max(kHeadroomEstimateFloorDb, max_db);
}

double estimate_total_processing_max_gain_db(std::uint32_t sample_rate,
                                             int bass_db,
                                             int bass_hz,
                                             int treble_db,
                                             int treble_hz,
                                             bool deep_bass_enabled,
                                             DeepBassPreset deep_bass_preset,
                                             int deep_bass_amount_steps) {
    const double linear_max_db = estimate_cascaded_shelf_max_gain_db(sample_rate, bass_db, bass_hz, treble_db, treble_hz);
    double max_linear = std::pow(10.0, linear_max_db / 20.0);
    if (!deep_bass_enabled || sample_rate == 0) {
        return 20.0 * std::log10(std::max(1.0, max_linear));
    }

    const double amount_gain = deep_bass_amount_gain_from_steps(deep_bass_amount_steps);
    const double nyquist_hz = static_cast<double>(sample_rate) * 0.5;
    const double log_min = std::log(20.0);
    const double log_max = std::log(std::min(420.0, nyquist_hz * 0.98));
    for (int i = 0; i < kDeepBassLowFreqTests; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kDeepBassLowFreqTests - 1);
        const double hz = std::exp(log_min + (log_max - log_min) * t);
        const double shelf_gain = std::pow(10.0, cascaded_shelf_response_db(sample_rate, bass_db, bass_hz, treble_db, treble_hz, hz) / 20.0);
        max_linear = std::max(max_linear, simulate_deep_bass_peak(sample_rate, deep_bass_preset, shelf_gain, hz, 0.0, 0.0, 0.0, amount_gain));
        max_linear = std::max(max_linear, simulate_deep_bass_peak(sample_rate, deep_bass_preset, shelf_gain * 0.88, hz, shelf_gain * 0.16, std::min(nyquist_hz * 0.95, hz * 1.60), 0.0, amount_gain));
    }

    const double multi_tone_pairs[kDeepBassMultiToneTests][3] = {
        {36.0, 72.0, 1.6},
        {40.0, 80.0, 1.8},
        {52.0, 104.0, 2.0},
        {64.0, 128.0, 2.2},
        {72.0, 144.0, 2.4},
        {84.0, 168.0, 2.6}
    };
    for (int i = 0; i < kDeepBassMultiToneTests; ++i) {
        const double f1 = std::min(nyquist_hz * 0.90, multi_tone_pairs[i][0]);
        const double f2 = std::min(nyquist_hz * 0.92, multi_tone_pairs[i][1]);
        const double burst_hz = multi_tone_pairs[i][2];
        const double g1 = std::pow(10.0, cascaded_shelf_response_db(sample_rate, bass_db, bass_hz, treble_db, treble_hz, f1) / 20.0);
        const double g2 = std::pow(10.0, cascaded_shelf_response_db(sample_rate, bass_db, bass_hz, treble_db, treble_hz, f2) / 20.0);
        max_linear = std::max(max_linear, simulate_deep_bass_peak(sample_rate, deep_bass_preset, 0.64 * g1, f1, 0.14 * g2, f2, burst_hz, amount_gain));
        max_linear = std::max(max_linear, simulate_deep_bass_peak(sample_rate, deep_bass_preset, 0.52 * g1, f1, 0.18 * g2, f2, burst_hz * 0.75, amount_gain));
    }

    max_linear *= std::pow(10.0, deep_bass_headroom_tail_db(deep_bass_preset) / 20.0);
    return 20.0 * std::log10(std::max(1.0, max_linear));
}

} // namespace tone
} // namespace pcmtp
