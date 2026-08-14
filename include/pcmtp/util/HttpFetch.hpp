// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <string>

namespace pcmtp {

// Small libcurl helper for short text downloads (remote M3U, etc.).
std::string http_fetch_text(const std::string& url,
                            long timeout_seconds = 30,
                            std::size_t max_bytes = 4 * 1024 * 1024);

} // namespace pcmtp
