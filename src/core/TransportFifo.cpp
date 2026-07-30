// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/core/TransportFifo.hpp"

#include <algorithm>
#include <stdexcept>

namespace pcmtp {

TransportFifo::TransportFifo(std::size_t capacity_samples)
    : buffer_(capacity_samples) {
    if (capacity_samples == 0) {
        throw std::invalid_argument("TransportFifo capacity must be > 0");
    }
}

std::size_t TransportFifo::write(const PcmSample* data, std::size_t samples, bool block) {
    std::unique_lock<std::mutex> lock(mutex_);
    std::size_t written = 0;

    while (written < samples) {
        if (closed_) {
            break;
        }

        if (size_ == buffer_.size()) {
            if (!block) {
                break;
            }
            not_full_cv_.wait(lock, [this]() { return closed_ || size_ < buffer_.size(); });
            continue;
        }

        const std::size_t contiguous =
            std::min({samples - written, free_space_unsafe(), buffer_.size() - tail_});
        for (std::size_t i = 0; i < contiguous; ++i) {
            buffer_[tail_ + i] = data[written + i];
        }
        tail_ = (tail_ + contiguous) % buffer_.size();
        size_ += contiguous;
        written += contiguous;
        not_empty_cv_.notify_one();
    }

    return written;
}

std::size_t TransportFifo::read(PcmSample* data, std::size_t samples, bool block) {
    std::unique_lock<std::mutex> lock(mutex_);
    std::size_t read = 0;

    while (read < samples) {
        if (size_ == 0) {
            if (closed_) {
                break;
            }
            if (!block) {
                break;
            }
            not_empty_cv_.wait(lock, [this]() { return closed_ || size_ > 0; });
            continue;
        }

        const std::size_t contiguous = std::min({samples - read, size_, buffer_.size() - head_});
        for (std::size_t i = 0; i < contiguous; ++i) {
            data[read + i] = buffer_[head_ + i];
        }
        head_ = (head_ + contiguous) % buffer_.size();
        size_ -= contiguous;
        read += contiguous;
        not_full_cv_.notify_one();
    }

    return read;
}

void TransportFifo::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    not_empty_cv_.notify_all();
    not_full_cv_.notify_all();
}

bool TransportFifo::is_closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

std::size_t TransportFifo::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
}

std::size_t TransportFifo::capacity() const {
    return buffer_.size();
}

std::size_t TransportFifo::free_space_unsafe() const {
    return buffer_.size() - size_;
}

} // namespace pcmtp
