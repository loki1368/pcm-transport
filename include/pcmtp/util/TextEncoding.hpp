// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>

namespace pcmtp {
namespace text {

// Normalize complete CUE/M3U/M3U8 file contents before line parsing.
std::string normalize_text_file_bytes(const std::string& bytes);

// Normalize a single metadata value to valid UTF-8.
std::string normalize_metadata_value(const std::string& value);

} // namespace text
} // namespace pcmtp
