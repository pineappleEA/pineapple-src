// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>

#include "common/alignment.h"
#include "common/common_funcs.h"
#include "common/common_types.h"
#include "common/div_ceil.h"
#include "core/memory.h"

namespace VideoCommon {

enum class BufferFlagBits {
    Picked = 1 << 0,
    CachedWrites = 1 << 1,
};
DECLARE_ENUM_FLAG_OPERATORS(BufferFlagBits)

/// Tag for creating null buffers with no storage or size
struct NullBufferParams {};

/**
 * Range tracking buffer container.
 *
 * It keeps track of the modified CPU and GPU ranges on a CPU page granularity, notifying the given
 * rasterizer about state changes in the tracking behavior of the buffer.
 *
 * The buffer size and address is forcefully aligned to CPU page boundaries.
 */
template <class RasterizerInterface>
class BufferBase {
    static constexpr u64 PAGES_PER_WORD = 64;
    static constexpr u64 BYTES_PER_PAGE = Core::Memory::PAGE_SIZE;
    static constexpr u64 BYTES_PER_WORD = PAGES_PER_WORD * BYTES_PER_PAGE;

    /// Vector tracking modified pages tightly packed with small vector optimization
    union WordsArray {
        /// Returns the pointer to the words state
        [[nodiscard]] const u64* Pointer(bool is_short) const noexcept {
            return is_short ? &stack : heap;
        }

        /// Returns the pointer to the words state
        [[nodiscard]] u64* Pointer(bool is_short) noexcept {
            return is_short ? &stack : heap;
        }

        u64 stack = 0; ///< Small buffers storage
        u64* heap;     ///< Not-small buffers pointer to the storage
    };

    struct Words {
        explicit Words() = default;
        explicit Words(u64 size_bytes_) : size_bytes{size_bytes_} {
            if (IsShort()) {
                cpu.stack = ~u64{0};
                gpu.stack = 0;
                cached_cpu.stack = 0;
                untracked.stack = ~u64{0};
            } else {
                // Share allocation between CPU and GPU pages and set their default values
                const size_t num_words = NumWords();
                u64* const alloc = new u64[num_words * 4];
                cpu.heap = alloc;
                gpu.heap = alloc + num_words;
                cached_cpu.heap = alloc + num_words * 2;
                untracked.heap = alloc + num_words * 3;
                std::fill_n(cpu.heap, num_words, ~u64{0});
                std::fill_n(gpu.heap, num_words, 0);
                std::fill_n(cached_cpu.heap, num_words, 0);
                std::fill_n(untracked.heap, num_words, ~u64{0});
            }
            // Clean up tailing bits
            const u64 last_word_size = size_bytes % BYTES_PER_WORD;
            const u64 last_local_page = Common::DivCeil(last_word_size, BYTES_PER_PAGE);
            const u64 shift = (PAGES_PER_WORD - last_local_page) % PAGES_PER_WORD;
            const u64 last_word = (~u64{0} << shift) >> shift;
            cpu.Pointer(IsShort())[NumWords() - 1] = last_word;
            untracked.Pointer(IsShort())[NumWords() - 1] = last_word;
        }

        ~Words() {
            Release();
        }

        Words& operator=(Words&& rhs) noexcept {
            Release();
            size_bytes = rhs.size_bytes;
            cpu = rhs.cpu;
            gpu = rhs.gpu;
            cached_cpu = rhs.cached_cpu;
            untracked = rhs.untracked;
            rhs.cpu.heap = nullptr;
            return *this;
        }

        Words(Words&& rhs) noexcept
            : size_bytes{rhs.size_bytes}, cpu{rhs.cpu}, gpu{rhs.gpu},
              cached_cpu{rhs.cached_cpu}, untracked{rhs.untracked} {
            rhs.cpu.heap = nullptr;
        }

        Words& operator=(const Words&) = delete;
        Words(const Words&) = delete;

        /// Returns true when the buffer fits in the small vector optimization
        [[nodiscard]] bool IsShort() const noexcept {
            return size_bytes <= BYTES_PER_WORD;
        }

        /// Returns the number of words of the buffer
        [[nodiscard]] size_t NumWords() const noexcept {
            return Common::DivCeil(size_bytes, BYTES_PER_WORD);
        }

        /// Release buffer resources
        void Release() {
            if (!IsShort()) {
                // CPU written words is the base for the heap allocation
                delete[] cpu.heap;
            }
        }

        u64 size_bytes = 0;
        WordsArray cpu;
        WordsArray gpu;
        WordsArray cached_cpu;
        WordsArray untracked;
    };

    enum class Type {
        CPU,
        GPU,
        CachedCPU,
        Untracked,
    };

public:
    explicit BufferBase(RasterizerInterface& rasterizer_, VAddr cpu_addr_, u64 size_bytes)
        : rasterizer{&rasterizer_}, cpu_addr{Common::AlignDown(cpu_addr_, BYTES_PER_PAGE)},
          words(Common::AlignUp(size_bytes + (cpu_addr_ - cpu_addr), BYTES_PER_PAGE)) {}

    explicit BufferBase(NullBufferParams) {}

    BufferBase& operator=(const BufferBase&) = delete;
    BufferBase(const BufferBase&) = delete;

    BufferBase& operator=(BufferBase&&) = default;
    BufferBase(BufferBase&&) = default;

    /// Returns the inclusive CPU modified range in a begin end pair
    [[nodiscard]] std::pair<u64, u64> ModifiedCpuRegion(VAddr query_cpu_addr,
                                                        u64 query_size) const noexcept {
        const u64 offset = query_cpu_addr - cpu_addr;
        return ModifiedRegion<Type::CPU>(offset, query_size);
    }

    /// Returns the inclusive GPU modified range in a begin end pair
    [[nodiscard]] std::pair<u64, u64> ModifiedGpuRegion(VAddr query_cpu_addr,
                                                        u64 query_size) const noexcept {
        const u64 offset = query_cpu_addr - cpu_addr;
        return ModifiedRegion<Type::GPU>(offset, query_size);
    }

    /// Returns true if a region has been modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr query_cpu_addr, u64 query_size) const noexcept {
        const u64 offset = query_cpu_addr - cpu_addr;
        return IsRegionModified<Type::CPU>(offset, query_size);
    }

    /// Returns true if a region has been modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr query_cpu_addr, u64 query_size) const noexcept {
        const u64 offset = query_cpu_addr - cpu_addr;
        return IsRegionModified<Type::GPU>(offset, query_size);
    }

    /// Mark region as CPU modified, notifying the rasterizer about this change
    void MarkRegionAsCpuModified(VAddr dirty_cpu_addr, u64 size) {
        ChangeRegionState<Type::CPU, true>(dirty_cpu_addr, size);
    }

    /// Unmark region as CPU modified, notifying the rasterizer about this change
    void UnmarkRegionAsCpuModified(VAddr dirty_cpu_addr, u64 size) {
        ChangeRegionState<Type::CPU, false>(dirty_cpu_addr, size);
    }

    /// Mark region as modified from the host GPU
    void MarkRegionAsGpuModified(VAddr dirty_cpu_addr, u64 size) noexcept {
        ChangeRegionState<Type::GPU, true>(dirty_cpu_addr, size);
    }

    /// Unmark region as modified from the host GPU
    void UnmarkRegionAsGpuModified(VAddr dirty_cpu_addr, u64 size) noexcept {
        ChangeRegionState<Type::GPU, false>(dirty_cpu_addr, size);
    }

    /// Mark region as modified from the CPU
    /// but don't mark it as modified until FlusHCachedWrites is called.
    void CachedCpuWrite(VAddr dirty_cpu_addr, u64 size) {
        flags |= BufferFlagBits::CachedWrites;
        ChangeRegionState<Type::CachedCPU, true>(dirty_cpu_addr, size);
    }

    /// Flushes cached CPU writes, and notify the rasterizer about the deltas
    void FlushCachedWrites() noexcept {
        flags &= ~BufferFlagBits::CachedWrites;
        const u64 num_words = NumWords();
        const u64* const cached_words = Array<Type::CachedCPU>();
        u64* const untracked_words = Array<Type::Untracked>();
        u64* const cpu_words = Array<Type::CPU>();
        for (u64 word_index = 0; word_index < num_words; ++word_index) {
            const u64 cached_bits = cached_words[word_index];
            NotifyRasterizer<false>(word_index, untracked_words[word_index], cached_bits);
            untracked_words[word_index] |= cached_bits;
            cpu_words[word_index] |= cached_bits;
            //cached_words[word_index] = 0;
        }
    }

    /// Call 'func' for each CPU modified range and unmark those pages as CPU modified
    template <typename Func>
    void ForEachUploadRange(VAddr query_cpu_range, u64 size, Func&& func) {
        ForEachModifiedRange<Type::CPU>(query_cpu_range, size, true, func);
    }

    /// Call 'func' for each GPU modified range and unmark those pages as GPU modified
    template <typename Func>
    void ForEachDownloadRange(VAddr query_cpu_range, u64 size, bool clear, Func&& func) {
        ForEachModifiedRange<Type::GPU>(query_cpu_range, size, clear, func);
    }

    template <typename Func>
    void ForEachDownloadRangeAndClear(VAddr query_cpu_range, u64 size, Func&& func) {
        ForEachModifiedRange<Type::GPU>(query_cpu_range, size, true, func);
    }

    /// Call 'func' for each GPU modified range and unmark those pages as GPU modified
    template <typename Func>
    void ForEachDownloadRange(Func&& func) {
        ForEachModifiedRange<Type::GPU>(cpu_addr, SizeBytes(), true, func);
    }

    /// Mark buffer as picked
    void Pick() noexcept {
        flags |= BufferFlagBits::Picked;
    }

    /// Unmark buffer as picked
    void Unpick() noexcept {
        flags &= ~BufferFlagBits::Picked;
    }

    /// Increases the likeliness of this being a stream buffer
    void IncreaseStreamScore(int score) noexcept {
        stream_score += score;
    }

    /// Returns the likeliness of this being a stream buffer
    [[nodiscard]] int StreamScore() const noexcept {
        return stream_score;
    }

    /// Returns true when vaddr -> vaddr+size is fully contained in the buffer
    [[nodiscard]] bool IsInBounds(VAddr addr, u64 size) const noexcept {
        return addr >= cpu_addr && addr + size <= cpu_addr + SizeBytes();
    }

    /// Returns true if the buffer has been marked as picked
    [[nodiscard]] bool IsPicked() const noexcept {
        return True(flags & BufferFlagBits::Picked);
    }

    /// Returns true when the buffer has pending cached writes
    [[nodiscard]] bool HasCachedWrites() const noexcept {
        return True(flags & BufferFlagBits::CachedWrites);
    }

    /// Returns the base CPU address of the buffer
    [[nodiscard]] VAddr CpuAddr() const noexcept {
        return cpu_addr;
    }

    /// Returns the offset relative to the given CPU address
    /// @pre IsInBounds returns true
    [[nodiscard]] u32 Offset(VAddr other_cpu_addr) const noexcept {
        return static_cast<u32>(other_cpu_addr - cpu_addr);
    }

    /// Returns the size in bytes of the buffer
    [[nodiscard]] u64 SizeBytes() const noexcept {
        return words.size_bytes;
    }

    size_t getLRUID() const noexcept {
        return lru_id;
    }

    void setLRUID(size_t lru_id_) {
        lru_id = lru_id_;
    }

private:
    template <Type type>
    u64* Array() noexcept {
        if constexpr (type == Type::CPU) {
            return words.cpu.Pointer(IsShort());
        } else if constexpr (type == Type::GPU) {
            return words.gpu.Pointer(IsShort());
        } else if constexpr (type == Type::CachedCPU) {
            return words.cached_cpu.Pointer(IsShort());
        } else if constexpr (type == Type::Untracked) {
            return words.untracked.Pointer(IsShort());
        }
    }

    template <Type type>
    const u64* Array() const noexcept {
        if constexpr (type == Type::CPU) {
            return words.cpu.Pointer(IsShort());
        } else if constexpr (type == Type::GPU) {
            return words.gpu.Pointer(IsShort());
        } else if constexpr (type == Type::CachedCPU) {
            return words.cached_cpu.Pointer(IsShort());
        } else if constexpr (type == Type::Untracked) {
            return words.untracked.Pointer(IsShort());
        }
    }

    /**
     * Change the state of a range of pages
     *
     * @param dirty_addr    Base address to mark or unmark as modified
     * @param size          Size in bytes to mark or unmark as modified
     */
    template <Type type, bool enable>
    void ChangeRegionState(u64 dirty_addr, s64 size) noexcept(type == Type::GPU) {
        const s64 difference = dirty_addr - cpu_addr;
        const u64 offset = std::max<s64>(difference, 0);
        size += std::min<s64>(difference, 0);
        if (offset >= SizeBytes() || size < 0) {
            return;
        }
        u64* const untracked_words = Array<Type::Untracked>();
        u64* const state_words = Array<type>();
        const u64 offset_end = std::min(offset + size, SizeBytes());
        const u64 begin_page_index = offset / BYTES_PER_PAGE;
        const u64 begin_word_index = begin_page_index / PAGES_PER_WORD;
        const u64 end_page_index = Common::DivCeil(offset_end, BYTES_PER_PAGE);
        const u64 end_word_index = Common::DivCeil(end_page_index, PAGES_PER_WORD);
        u64 page_index = begin_page_index % PAGES_PER_WORD;
        u64 word_index = begin_word_index;
        while (word_index < end_word_index) {
            const u64 next_word_first_page = (word_index + 1) * PAGES_PER_WORD;
            const u64 left_offset =
                std::min(next_word_first_page - end_page_index, PAGES_PER_WORD) % PAGES_PER_WORD;
            const u64 right_offset = page_index;
            u64 bits = ~u64{0};
            bits = (bits >> right_offset) << right_offset;
            bits = (bits << left_offset) >> left_offset;
            if constexpr (type == Type::CPU || type == Type::CachedCPU) {
                NotifyRasterizer<!enable>(word_index, untracked_words[word_index], bits);
            }
            if constexpr (enable) {
                state_words[word_index] |= bits;
                if constexpr (type == Type::CPU || type == Type::CachedCPU) {
                    untracked_words[word_index] |= bits;
                }
            } else {
                state_words[word_index] &= ~bits;
                if constexpr (type == Type::CPU || type == Type::CachedCPU) {
                    untracked_words[word_index] &= ~bits;
                }
            }
            page_index = 0;
            ++word_index;
        }
    }

    /**
     * Notify rasterizer about changes in the CPU tracking state of a word in the buffer
     *
     * @param word_index   Index to the word to notify to the rasterizer
     * @param current_bits Current state of the word
     * @param new_bits     New state of the word
     *
     * @tparam add_to_rasterizer True when the rasterizer should start tracking the new pages
     */
    template <bool add_to_rasterizer>
    void NotifyRasterizer(u64 word_index, u64 current_bits, u64 new_bits) const {
        u64 changed_bits = (add_to_rasterizer ? current_bits : ~current_bits) & new_bits;
        VAddr addr = cpu_addr + word_index * BYTES_PER_WORD;
        while (changed_bits != 0) {
            const int empty_bits = std::countr_zero(changed_bits);
            addr += empty_bits * BYTES_PER_PAGE;
            changed_bits >>= empty_bits;

            const u32 continuous_bits = std::countr_one(changed_bits);
            const u64 size = continuous_bits * BYTES_PER_PAGE;
            const VAddr begin_addr = addr;
            addr += size;
            changed_bits = continuous_bits < PAGES_PER_WORD ? (changed_bits >> continuous_bits) : 0;
            rasterizer->UpdatePagesCachedCount(begin_addr, size, add_to_rasterizer ? 1 : -1);
        }
    }

    /**
     * Loop over each page in the given range, turn off those bits and notify the rasterizer if
     * needed. Call the given function on each turned off range.
     *
     * @param query_cpu_range Base CPU address to loop over
     * @param size            Size in bytes of the CPU range to loop over
     * @param func            Function to call for each turned off region
     */
    template <Type type, typename Func>
    void ForEachModifiedRange(VAddr query_cpu_range, s64 size, bool clear, Func&& func) {
        static_assert(type != Type::Untracked);

        const s64 difference = query_cpu_range - cpu_addr;
        const u64 query_begin = std::max<s64>(difference, 0);
        size += std::min<s64>(difference, 0);
        if (query_begin >= SizeBytes() || size < 0) {
            return;
        }
        u64* const untracked_words = Array<Type::Untracked>();
        u64* const state_words = Array<type>();
        const u64 query_end = query_begin + std::min(static_cast<u64>(size), SizeBytes());
        u64* const words_begin = state_words + query_begin / BYTES_PER_WORD;
        u64* const words_end = state_words + Common::DivCeil(query_end, BYTES_PER_WORD);

        const auto modified = [](u64 word) { return word != 0; };
        const auto first_modified_word = std::find_if(words_begin, words_end, modified);
        if (first_modified_word == words_end) {
            // Exit early when the buffer is not modified
            return;
        }
        const auto last_modified_word = std::find_if_not(first_modified_word, words_end, modified);

        const u64 word_index_begin = std::distance(state_words, first_modified_word);
        const u64 word_index_end = std::distance(state_words, last_modified_word);

        const unsigned local_page_begin = std::countr_zero(*first_modified_word);
        const unsigned local_page_end =
            static_cast<unsigned>(PAGES_PER_WORD) - std::countl_zero(last_modified_word[-1]);
        const u64 word_page_begin = word_index_begin * PAGES_PER_WORD;
        const u64 word_page_end = (word_index_end - 1) * PAGES_PER_WORD;
        const u64 query_page_begin = query_begin / BYTES_PER_PAGE;
        const u64 query_page_end = Common::DivCeil(query_end, BYTES_PER_PAGE);
        const u64 page_index_begin = std::max(word_page_begin + local_page_begin, query_page_begin);
        const u64 page_index_end = std::min(word_page_end + local_page_end, query_page_end);
        const u64 first_word_page_begin = page_index_begin % PAGES_PER_WORD;
        const u64 last_word_page_end = (page_index_end - 1) % PAGES_PER_WORD + 1;

        u64 page_begin = first_word_page_begin;
        u64 current_base = 0;
        u64 current_size = 0;
        bool on_going = false;
        for (u64 word_index = word_index_begin; word_index < word_index_end; ++word_index) {
            const bool is_last_word = word_index + 1 == word_index_end;
            const u64 page_end = is_last_word ? last_word_page_end : PAGES_PER_WORD;
            const u64 right_offset = page_begin;
            const u64 left_offset = PAGES_PER_WORD - page_end;
            u64 bits = ~u64{0};
            bits = (bits >> right_offset) << right_offset;
            bits = (bits << left_offset) >> left_offset;

            const u64 current_word = state_words[word_index] & bits;
            if (clear) {
                state_words[word_index] &= ~bits;
            }

            if constexpr (type == Type::CPU) {
                const u64 current_bits = untracked_words[word_index] & bits;
                untracked_words[word_index] &= ~bits;
                NotifyRasterizer<true>(word_index, current_bits, ~u64{0});
            }
            // Exclude CPU modified pages when visiting GPU pages
            const u64 word = current_word & ~(type == Type::GPU ? untracked_words[word_index] : 0);
            u64 page = page_begin;
            page_begin = 0;

            while (page < page_end) {
                const int empty_bits = std::countr_zero(word >> page);
                if (on_going && empty_bits != 0) {
                    InvokeModifiedRange(func, current_size, current_base);
                    current_size = 0;
                    on_going = false;
                }
                if (empty_bits == PAGES_PER_WORD) {
                    break;
                }
                page += empty_bits;

                const int continuous_bits = std::countr_one(word >> page);
                if (!on_going && continuous_bits != 0) {
                    current_base = word_index * PAGES_PER_WORD + page;
                    on_going = true;
                }
                current_size += continuous_bits;
                page += continuous_bits;
            }
        }
        if (on_going && current_size > 0) {
            InvokeModifiedRange(func, current_size, current_base);
        }
    }

    template <typename Func>
    void InvokeModifiedRange(Func&& func, u64 current_size, u64 current_base) {
        const u64 current_size_bytes = current_size * BYTES_PER_PAGE;
        const u64 offset_begin = current_base * BYTES_PER_PAGE;
        const u64 offset_end = std::min(offset_begin + current_size_bytes, SizeBytes());
        func(offset_begin, offset_end - offset_begin);
    }

    /**
     * Returns true when a region has been modified
     *
     * @param offset Offset in bytes from the start of the buffer
     * @param size   Size in bytes of the region to query for modifications
     */
    template <Type type>
    [[nodiscard]] bool IsRegionModified(u64 offset, u64 size) const noexcept {
        static_assert(type != Type::Untracked);

        const u64* const untracked_words = Array<Type::Untracked>();
        const u64* const state_words = Array<type>();
        const u64 num_query_words = size / BYTES_PER_WORD + 1;
        const u64 word_begin = offset / BYTES_PER_WORD;
        const u64 word_end = std::min(word_begin + num_query_words, NumWords());
        const u64 page_limit = Common::DivCeil(offset + size, BYTES_PER_PAGE);
        u64 page_index = (offset / BYTES_PER_PAGE) % PAGES_PER_WORD;
        for (u64 word_index = word_begin; word_index < word_end; ++word_index, page_index = 0) {
            const u64 off_word = type == Type::GPU ? untracked_words[word_index] : 0;
            const u64 word = state_words[word_index] & ~off_word;
            if (word == 0) {
                continue;
            }
            const u64 page_end = std::min((word_index + 1) * PAGES_PER_WORD, page_limit);
            const u64 local_page_end = page_end % PAGES_PER_WORD;
            const u64 page_end_shift = (PAGES_PER_WORD - local_page_end) % PAGES_PER_WORD;
            if (((word >> page_index) << page_index) << page_end_shift != 0) {
                return true;
            }
        }
        return false;
    }

    /**
     * Returns a begin end pair with the inclusive modified region
     *
     * @param offset Offset in bytes from the start of the buffer
     * @param size   Size in bytes of the region to query for modifications
     */
    template <Type type>
    [[nodiscard]] std::pair<u64, u64> ModifiedRegion(u64 offset, u64 size) const noexcept {
        static_assert(type != Type::Untracked);

        const u64* const untracked_words = Array<Type::Untracked>();
        const u64* const state_words = Array<type>();
        const u64 num_query_words = size / BYTES_PER_WORD + 1;
        const u64 word_begin = offset / BYTES_PER_WORD;
        const u64 word_end = std::min(word_begin + num_query_words, NumWords());
        const u64 page_base = offset / BYTES_PER_PAGE;
        const u64 page_limit = Common::DivCeil(offset + size, BYTES_PER_PAGE);
        u64 begin = std::numeric_limits<u64>::max();
        u64 end = 0;
        for (u64 word_index = word_begin; word_index < word_end; ++word_index) {
            const u64 off_word = type == Type::GPU ? untracked_words[word_index] : 0;
            const u64 word = state_words[word_index] & ~off_word;
            if (word == 0) {
                continue;
            }
            const u64 local_page_begin = std::countr_zero(word);
            const u64 local_page_end = PAGES_PER_WORD - std::countl_zero(word);
            const u64 page_index = word_index * PAGES_PER_WORD;
            const u64 page_begin = std::max(page_index + local_page_begin, page_base);
            const u64 page_end = std::min(page_index + local_page_end, page_limit);
            begin = std::min(begin, page_begin);
            end = std::max(end, page_end);
        }
        static constexpr std::pair<u64, u64> EMPTY{0, 0};
        return begin < end ? std::make_pair(begin * BYTES_PER_PAGE, end * BYTES_PER_PAGE) : EMPTY;
    }

    /// Returns the number of words of the buffer
    [[nodiscard]] size_t NumWords() const noexcept {
        return words.NumWords();
    }

    /// Returns true when the buffer fits in the small vector optimization
    [[nodiscard]] bool IsShort() const noexcept {
        return words.IsShort();
    }

    RasterizerInterface* rasterizer = nullptr;
    VAddr cpu_addr = 0;
    Words words;
    BufferFlagBits flags{};
    int stream_score = 0;
    size_t lru_id = SIZE_MAX;
};

} // namespace VideoCommon
