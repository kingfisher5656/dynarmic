/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <memory>

#include <dynarmic/common/spin_lock.h>

namespace Dynarmic {

using VAddr = std::uint64_t;
using Vector = std::array<std::uint64_t, 2>;

class ExclusiveMonitor {
public:
    /// @param processor_count Maximum number of processors using this global
    ///                        exclusive monitor. Each processor must have a
    ///                        unique id.
    explicit ExclusiveMonitor(size_t processor_count);

    size_t GetProcessorCount() const;

    /// Marks a region containing [address, address+size) to be exclusive to
    /// processor processor_id.
    template<typename T, typename Function>
    T ReadAndMark(size_t processor_id, VAddr address, Function op) {
        static_assert(std::is_trivially_copyable_v<T>);
        const VAddr masked_address = address & RESERVATION_GRANULE_MASK;

        Lock();
        exclusive_addresses[processor_id] = masked_address;
        const T value = op();
        std::memcpy(exclusive_values[processor_id].data(), &value, sizeof(T));
        Unlock();
        return value;
    }

    /// Checks to see if processor processor_id has exclusive access to the
    /// specified region. If it does, executes the operation then clears
    /// the exclusive state for processors if their exclusive region(s)
    /// contain [address, address+size).
    template<typename T, typename Function>
    bool DoExclusiveOperation(size_t processor_id, VAddr address, Function op) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (!CheckAndClear(processor_id, address)) {
            return false;
        }

        T saved_value;
        std::memcpy(&saved_value, exclusive_values[processor_id].data(), sizeof(T));
        const bool result = op(saved_value);

        Unlock();
        return result;
    }

    /// Unmark everything.
    void Clear();
    /// Unmark processor id
    void ClearProcessor(size_t processor_id);

    /// Reservation versions for exclusive accesses that the arm64 JIT runs inline
    /// (OptimizationFlag::InlineExclusiveMemory) instead of through this monitor.
    /// Word InlineReservationIndex(a) covers every address hashing there: bit 0 is held
    /// while an exclusive store to such an address is in progress, and the remaining bits
    /// count the successful exclusive stores. A store-exclusive succeeds only if the word
    /// still holds the value its load-exclusive saw, so (as with this monitor) another
    /// processor's exclusive store in between makes it fail, even if it restored the value.
    ///
    /// The two schemes do not see each other: every JIT sharing a monitor must use the same
    /// one. A JIT uses the inline scheme when InlineExclusiveMemory is on and it has a page
    /// table and no fastmem, so a JIT with a different memory setup does not interoperate
    /// even with the same optimization flags. Clear() and ClearProcessor() only reset this
    /// monitor's own reservations; clear an inline reservation with Jit::ClearExclusiveState().
    using InlineReservationWord = std::atomic<std::uint64_t>;
    static constexpr size_t INLINE_RESERVATION_TABLE_BITS = 12;
    static constexpr size_t InlineReservationIndex(std::uint32_t vaddr) {
        return (vaddr >> 3) & ((size_t{1} << INLINE_RESERVATION_TABLE_BITS) - 1);
    }
    InlineReservationWord* InlineReservationTable() { return inline_reservations.get(); }

private:
    bool CheckAndClear(size_t processor_id, VAddr address);

    void Lock();
    void Unlock();

    friend volatile int* GetExclusiveMonitorLockPointer(ExclusiveMonitor*);
    friend size_t GetExclusiveMonitorProcessorCount(ExclusiveMonitor*);
    friend VAddr* GetExclusiveMonitorAddressPointer(ExclusiveMonitor*, size_t index);
    friend Vector* GetExclusiveMonitorValuePointer(ExclusiveMonitor*, size_t index);

    static constexpr VAddr RESERVATION_GRANULE_MASK = 0xFFFF'FFFF'FFFF'FFFFull;
    static constexpr VAddr INVALID_EXCLUSIVE_ADDRESS = 0xDEAD'DEAD'DEAD'DEADull;
    SpinLock lock;
    std::vector<VAddr> exclusive_addresses;
    std::vector<Vector> exclusive_values;
    std::unique_ptr<InlineReservationWord[]> inline_reservations;
};

}  // namespace Dynarmic
