// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/make_unique_for_overwrite.h"

namespace Common {

/**
 * ScratchBuffer class
 * This class creates a default initialized heap allocated buffer for cases such as intermediate
 * buffers being copied into entirely, where value initializing members during allocation or resize
 * is redundant.
 */
template <typename T>
class ScratchBuffer {
public:
    ScratchBuffer() = default;

    explicit ScratchBuffer(size_t initial_capacity)
        : last_requested_size{initial_capacity}, buffer_capacity{initial_capacity},
          buffer{Common::make_unique_for_overwrite<T[]>(initial_capacity)} {}

    ~ScratchBuffer() = default;
    ScratchBuffer(const ScratchBuffer&) = delete;
    ScratchBuffer& operator=(const ScratchBuffer&) = delete;
    ScratchBuffer(ScratchBuffer&&) = default;
    ScratchBuffer& operator=(ScratchBuffer&&) = default;

    /// This will only grow the buffer's capacity if size is greater than the current capacity.
    /// The previously held data will remain intact.
    void resize(size_t size) {
        if (size > buffer_capacity) {
            auto new_buffer = Common::make_unique_for_overwrite<T[]>(size);
            std::move(buffer.get(), buffer.get() + buffer_capacity, new_buffer.get());
            buffer = std::move(new_buffer);
            buffer_capacity = size;
        }
        last_requested_size = size;
    }

    /// This will only grow the buffer's capacity if size is greater than the current capacity.
    /// The previously held data will be destroyed if a reallocation occurs.
    void resize_destructive(size_t size) {
        if (size > buffer_capacity) {
            buffer_capacity = size;
            buffer = Common::make_unique_for_overwrite<T[]>(buffer_capacity);
        }
        last_requested_size = size;
    }

    [[nodiscard]] T* data() noexcept {
        return buffer.get();
    }

    [[nodiscard]] const T* data() const noexcept {
        return buffer.get();
    }

    [[nodiscard]] T* begin() noexcept {
        return data();
    }

    [[nodiscard]] const T* begin() const noexcept {
        return data();
    }

    [[nodiscard]] T* end() noexcept {
        return data() + last_requested_size;
    }

    [[nodiscard]] const T* end() const noexcept {
        return data() + last_requested_size;
    }

    [[nodiscard]] T& operator[](size_t i) {
        return buffer[i];
    }

    [[nodiscard]] const T& operator[](size_t i) const {
        return buffer[i];
    }

    [[nodiscard]] size_t size() const noexcept {
        return last_requested_size;
    }

    [[nodiscard]] size_t capacity() const noexcept {
        return buffer_capacity;
    }

    void swap(ScratchBuffer& other) {
        std::swap(last_requested_size, other.last_requested_size);
        std::swap(buffer_capacity, other.buffer_capacity);
        std::swap(buffer, other.buffer);
    }

    void swap(ScratchBuffer&& other) {
        std::swap(last_requested_size, other.last_requested_size);
        std::swap(buffer_capacity, other.buffer_capacity);
        std::swap(buffer, other.buffer);
    }

private:
    size_t last_requested_size{};
    size_t buffer_capacity{};
    std::unique_ptr<T[]> buffer{};
};

} // namespace Common
