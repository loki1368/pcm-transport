#pragma once

#include <string>

namespace pcmtp {

bool is_remote_media_uri(const std::string& path);
bool is_http_media_uri(const std::string& path);

} // namespace pcmtp
