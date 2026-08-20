// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string>

#include "pcmtp/decoder/ExternalAudioDecoder.hpp"
#include "pcmtp/util/ProbeCancellation.hpp"

namespace pcmtp {

struct MediaProbeResult {
    bool success = false;
    std::string error;
    AudioFormat format{};
    std::uint64_t total_samples_per_channel = 0;
    bool source_supports_trusted_decoder_eof = false;
    ExactPresentationDrainPolicy sample_extent_drain_policy =
        ExactPresentationDrainPolicy::DecoderEofMatchesPresentation;
    bool source_presentation_start_known = false;
    std::uint64_t source_presentation_start_sample = 0;
    SampleExtentKind sample_extent_kind = SampleExtentKind::Unknown;
    SampleExtentSource sample_extent_source = SampleExtentSource::None;
    GenericTags tags{};
    std::string codec_name;
    std::uint32_t bit_rate = 0;
    bool native_decode = false;
    bool lossless = false;
    bool dsd_source = false;
    std::uint32_t dsd_sample_rate = 0;
    std::string probe_backend;
    std::uint64_t probe_elapsed_microseconds = 0;
};

MediaProbeResult probe_media_file(const std::string& path,
                                  ProbeCancellation* probe_cancellation = nullptr);

} // namespace pcmtp
