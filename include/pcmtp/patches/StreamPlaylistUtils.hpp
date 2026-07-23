#pragma once

#include <cstdint>
#include <string>

namespace pcmtp {

std::string stream_display_label(const std::string& url);
std::uint32_t stream_sample_rate_hint(const std::string& text);
std::uint16_t stream_bits_per_sample_hint(const std::string& text);

} // namespace pcmtp
