// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/util/HttpFetch.hpp"

#include <curl/curl.h>

#include <stdexcept>

namespace pcmtp {
namespace {

struct FetchBuffer {
    std::string* body = nullptr;
    std::size_t max_bytes = 0;
    bool overflow = false;
};

std::size_t write_callback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* buffer = static_cast<FetchBuffer*>(userdata);
    if (buffer == nullptr || buffer->body == nullptr || ptr == nullptr) {
        return 0;
    }

    const std::size_t chunk = size * nmemb;
    if (buffer->max_bytes > 0 &&
        buffer->body->size() + chunk > buffer->max_bytes) {
        buffer->overflow = true;
        return 0;
    }
    buffer->body->append(ptr, chunk);
    return chunk;
}

struct CurlGlobal {
    CurlGlobal() {
        const CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code != CURLE_OK) {
            throw std::runtime_error(std::string("curl_global_init failed: ") +
                                     curl_easy_strerror(code));
        }
    }
    ~CurlGlobal() {
        curl_global_cleanup();
    }
};

void ensure_curl_global() {
    static CurlGlobal global;
}

} // namespace

std::string http_fetch_text(const std::string& url,
                            long timeout_seconds,
                            std::size_t max_bytes) {
    if (url.empty()) {
        throw std::runtime_error("Cannot fetch empty URL");
    }

    ensure_curl_global();

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        throw std::runtime_error("Cannot initialize libcurl");
    }

    std::string body;
    body.reserve(4096);
    FetchBuffer buffer{&body, max_bytes, false};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pcm-transport/0.9");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds > 0 ? timeout_seconds : 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode code = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);

    if (buffer.overflow) {
        throw std::runtime_error("Remote playlist exceeds size limit: " + url);
    }
    if (code != CURLE_OK) {
        throw std::runtime_error(std::string("Cannot fetch remote playlist: ") +
                                 url + " (" + curl_easy_strerror(code) + ")");
    }
    if (http_status >= 400) {
        throw std::runtime_error("Cannot fetch remote playlist: " + url +
                                 " (HTTP " + std::to_string(http_status) + ")");
    }
    if (body.empty()) {
        throw std::runtime_error("Cannot fetch remote playlist: " + url + " (empty body)");
    }
    return body;
}

} // namespace pcmtp
