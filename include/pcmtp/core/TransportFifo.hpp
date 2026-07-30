// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>

#include "pcmtp/core/PcmTypes.hpp"

namespace pcmtp {

class TransportFifo {
public:
    explicit TransportFifo(std::size_t capacity_samples);

    std::size_t write(const PcmSample* data, std::size_t samples, bool block);
    std::size_t read(PcmSample* data, std::size_t samples, bool block);

    void close();
    bool is_closed() const;

    std::size_t size() const;
    std::size_t capacity() const;

private:
    std::size_t free_space_unsafe() const;

    mutable std::mutex mutex_;
    std::condition_variable not_empty_cv_;
    std::condition_variable not_full_cv_;
    std::vector<PcmSample> buffer_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t size_ = 0;
    bool closed_ = false;
};

} // namespace pcmtp
