#include "pcmtp/util/TextEncoding.hpp"

#include <glib.h>

#include <cstddef>
#include <string>

#include "pcmtp/util/Logger.hpp"

namespace pcmtp {
namespace text {
namespace {

bool is_valid_utf8(const std::string& value) {
    return g_utf8_validate(value.data(), static_cast<gssize>(value.size()), nullptr) != FALSE;
}

std::string make_valid_utf8(const std::string& value) {
    gchar* valid = g_utf8_make_valid(value.data(), static_cast<gssize>(value.size()));
    if (valid == nullptr) {
        return std::string();
    }
    std::string result(valid);
    g_free(valid);
    return result;
}

std::string convert_to_utf8(const std::string& value, const char* source_encoding) {
    GError* error = nullptr;
    gsize bytes_read = 0;
    gsize bytes_written = 0;
    gchar* converted = g_convert(value.data(),
                                 static_cast<gssize>(value.size()),
                                 "UTF-8",
                                 source_encoding,
                                 &bytes_read,
                                 &bytes_written,
                                 &error);
    if (error != nullptr) {
        g_error_free(error);
    }
    if (converted == nullptr) {
        return std::string();
    }
    std::string result(converted, bytes_written);
    g_free(converted);
    return is_valid_utf8(result) ? result : std::string();
}

bool contains_cyrillic(const std::string& value) {
    const gchar* current = value.c_str();
    const gchar* end = current + value.size();
    while (current < end) {
        const gunichar ch = g_utf8_get_char(current);
        if ((ch >= 0x0400u && ch <= 0x052Fu) ||
            (ch >= 0x2DE0u && ch <= 0x2DFFu) ||
            (ch >= 0xA640u && ch <= 0xA69Fu)) {
            return true;
        }
        current = g_utf8_next_char(current);
    }
    return false;
}

bool reconstruct_latin1_bytes(const std::string& value,
                              std::string* bytes,
                              std::size_t* non_ascii_count) {
    if (bytes == nullptr || non_ascii_count == nullptr) {
        return false;
    }
    bytes->clear();
    bytes->reserve(value.size());
    *non_ascii_count = 0;

    const gchar* current = value.c_str();
    const gchar* end = current + value.size();
    while (current < end) {
        const gunichar ch = g_utf8_get_char(current);
        if (ch > 0xFFu) {
            return false;
        }
        bytes->push_back(static_cast<char>(ch));
        if (ch >= 0x80u) {
            ++(*non_ascii_count);
        }
        current = g_utf8_next_char(current);
    }
    return true;
}

bool looks_like_cyrillic_repair(const std::string& source,
                                const std::string& candidate,
                                std::size_t source_non_ascii_count) {
    if (source_non_ascii_count < 2 || contains_cyrillic(source) || !contains_cyrillic(candidate)) {
        return false;
    }

    std::size_t letters = 0;
    std::size_t cyrillic_letters = 0;
    const gchar* current = candidate.c_str();
    const gchar* end = current + candidate.size();
    while (current < end) {
        const gunichar ch = g_utf8_get_char(current);
        if (g_unichar_isalpha(ch)) {
            ++letters;
            if ((ch >= 0x0400u && ch <= 0x052Fu) ||
                (ch >= 0x2DE0u && ch <= 0x2DFFu) ||
                (ch >= 0xA640u && ch <= 0xA69Fu)) {
                ++cyrillic_letters;
            }
        }
        current = g_utf8_next_char(current);
    }

    return cyrillic_letters >= 3 && letters > 0 && cyrillic_letters * 2 >= letters;
}

std::string try_repair_latin1_declared_cp1251(const std::string& value) {
    std::string bytes;
    std::size_t non_ascii_count = 0;
    if (!reconstruct_latin1_bytes(value, &bytes, &non_ascii_count) ||
        non_ascii_count < 2 || contains_cyrillic(value)) {
        return std::string();
    }

    const std::string candidate = convert_to_utf8(bytes, "WINDOWS-1251");
    if (candidate.empty() || !looks_like_cyrillic_repair(value, candidate, non_ascii_count)) {
        return std::string();
    }
    return candidate;
}

} // namespace

std::string normalize_text_file_bytes(const std::string& bytes) {
    std::string value = bytes;
    if (value.size() >= 3 &&
        static_cast<unsigned char>(value[0]) == 0xEFu &&
        static_cast<unsigned char>(value[1]) == 0xBBu &&
        static_cast<unsigned char>(value[2]) == 0xBFu) {
        value.erase(0, 3);
    }

    if (is_valid_utf8(value)) {
        return value;
    }

    const std::string converted = convert_to_utf8(value, "WINDOWS-1251");
    if (!converted.empty()) {
        return converted;
    }

    Logger::instance().debug("Text encoding normalization failed; invalid bytes were replaced");
    return make_valid_utf8(value);
}

std::string normalize_metadata_value(const std::string& value) {
    if (value.empty()) {
        return value;
    }

    if (!is_valid_utf8(value)) {
        const std::string converted = convert_to_utf8(value, "WINDOWS-1251");
        if (!converted.empty()) {
            return converted;
        }
        Logger::instance().debug("Metadata encoding normalization failed; invalid bytes were replaced");
        return make_valid_utf8(value);
    }

    const std::string repaired = try_repair_latin1_declared_cp1251(value);
    return repaired.empty() ? value : repaired;
}

} // namespace text
} // namespace pcmtp
