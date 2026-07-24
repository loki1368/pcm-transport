#include "pcmtp/patches/StreamPlaylistUtils.hpp"

#include <algorithm>
#include <cctype>

namespace pcmtp {

std::string stream_display_label(const std::string& url) {
    if (url.empty()) {
        return "Stream";
    }
    const std::size_t scheme = url.find("://");
    std::size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    while (start < url.size() && url[start] == '/') {
        ++start;
    }
    std::size_t end = url.find('/', start);
    if (end == std::string::npos) {
        end = url.size();
    }
    const std::size_t query = url.find('?', start);
    if (query != std::string::npos && query < end) {
        end = query;
    }
    if (end <= start) {
        return url;
    }
    return url.substr(start, end - start);
}

std::uint32_t stream_sample_rate_hint(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    struct RateHint {
        const char* token;
        std::uint32_t rate;
    };
    static const RateHint hints[] = {
        {"192 khz", 192000}, {"192khz", 192000},
        {"176.4 khz", 176400}, {"176.4khz", 176400},
        {"96 khz", 96000}, {"96khz", 96000},
        {"88.2 khz", 88200}, {"88.2khz", 88200},
        {"48 khz", 48000}, {"48khz", 48000},
        {"44.1 khz", 44100}, {"44.1khz", 44100},
    };
    for (const RateHint& hint : hints) {
        if (lower.find(hint.token) != std::string::npos) {
            return hint.rate;
        }
    }
    return 0;
}

std::uint16_t stream_bits_per_sample_hint(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower.find("24-bit") != std::string::npos || lower.find("24 bit") != std::string::npos) {
        return 24;
    }
    if (lower.find("32-bit") != std::string::npos || lower.find("32 bit") != std::string::npos) {
        return 32;
    }
    if (lower.find("flac") != std::string::npos) {
        return 16;
    }
    return 0;
}

} // namespace pcmtp
