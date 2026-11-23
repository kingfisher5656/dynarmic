// SPDX-FileCopyrightText: Copyright (c) 2024 merryhime <https://mary.rs>
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>

#if defined(_WIN32)
#    define NOMINMAX
#    include <windows.h>
#elif defined(__APPLE__)
#    include <mach/mach.h>
#    include <mach/vm_map.h>
#    include <mach-o/loader.h>

#    include <CoreFoundation/CoreFoundation.h>
#    include <IOKit/IOKitLib.h>
#    include <TargetConditionals.h>
#    include <libkern/OSCacheControl.h>
#    include <pthread.h>
#    include <sys/mman.h>
#    include <unistd.h>

#    include <os/lock.h>

#    include <cstdio>

// Missing from some IOKit header sets (e.g. Theos's vendored ones).
#    ifndef kIORegistryEntryPropertyKeysKey
#        define kIORegistryEntryPropertyKeysKey "IORegistryEntryPropertyKeys"
#    endif
#else
#    if !defined(_GNU_SOURCE)
#        define _GNU_SOURCE
#    endif
#    include <sys/mman.h>
#    include <sys/types.h>
#    include <unistd.h>
#endif

#if defined(__APPLE__)
extern "C" std::uint32_t dyld_get_active_platform(void);
#endif

namespace oaknut {

#if defined(__APPLE__)
namespace detail {

inline bool IsProbablyJailbroken()
{
    return ::access("/usr/lib/systemhook.dylib", F_OK) == 0;
}

inline bool RunningOnPhysicalIOS()
{
    return dyld_get_active_platform() == PLATFORM_IOS;
}

inline bool RunningOnIOS26OrLater()
{
    if (!RunningOnPhysicalIOS())
        return false;

    if (__builtin_available(iOS 19.0, *))
        return true;
    return false;
}

inline bool RequiresDualMapping()
{
    if (!RunningOnPhysicalIOS())
        return false;

    // Skip dual mapping on jailbroken devices(?)
    if (IsProbablyJailbroken())
        return false;

    return RunningOnIOS26OrLater();
}

inline bool DeviceHasTXM()
{
    static const bool has_txm = [] {
        // https://github.com/opa334/Dopamine/commit/e8438b4a64ead3997d2c70a575431cb1b4070fb9
        io_registry_entry_t memory_map = IORegistryEntryFromPath(0, "IODeviceTree:/chosen/memory-map");
        if (memory_map == IO_OBJECT_NULL)
            return false;

        CFArrayRef keys = (CFArrayRef)IORegistryEntryCreateCFProperty(memory_map, CFSTR(kIORegistryEntryPropertyKeysKey), 0, 0);
        IOObjectRelease(memory_map);
        if (!keys)
            return false;

        CFRange range = CFRangeMake(0, CFArrayGetCount(keys));
        bool result = CFArrayContainsValue(keys, range, CFSTR("SPTM")) && CFArrayContainsValue(keys, range, CFSTR("TXM"));
        CFRelease(keys);
        return result;
    }();
    return has_txm;
}

/// True when every new executable region must be prepared by an attached
/// iOS 26 JIT debugger (JIT26PrepareRegion).
inline bool UsesJIT26PrepareRegion()
{
    return DeviceHasTXM() && RunningOnIOS26OrLater() && !IsProbablyJailbroken();
}

// The JIT debuggers that serve JIT26PrepareRegion reach only the low 64 GiB:
// StikDebug's prepare_memory_region writes each page address as nine hex
// digits, so a region above that is "prepared" at a truncated address and its
// first instruction then dies with a code-signing fault (KERN_CODESIGN_ERROR).
// Ordinary anonymous mappings move up there (0x70_xxxx_xxxx on iOS 27) once
// the low address space is used up, which a big game can do while loading.
inline constexpr std::uintptr_t kJIT26AddressLimit = std::uintptr_t{1} << 36;

inline bool EndsBelowJIT26Limit(const void* address, std::size_t size)
{
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    return start <= kJIT26AddressLimit && size <= kJIT26AddressLimit - start;
}

/// Reserves `size` bytes of inaccessible address space in the lowest free hole
/// below kJIT26AddressLimit that the kernel accepts, or returns nullptr.
inline void* ReserveHoleBelowJIT26Limit(std::size_t size)
{
    const vm_address_t page_mask = static_cast<vm_address_t>(vm_page_size) - 1;
    vm_address_t cursor = 0x100000000;  // above __PAGEZERO
    vm_address_t failed_at = 0;
    while (cursor < kJIT26AddressLimit && size <= kJIT26AddressLimit - cursor) {
        vm_address_t region = cursor;
        vm_size_t region_size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        const kern_return_t found = vm_region_64(mach_task_self(), &region, &region_size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (object != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), object);

        const vm_address_t hole_end = found == KERN_SUCCESS ? std::min<vm_address_t>(region, kJIT26AddressLimit) : kJIT26AddressLimit;
        if (hole_end > cursor && hole_end - cursor >= size) {
            // VM_FLAGS_FIXED without VM_FLAGS_OVERWRITE fails rather than
            // replacing whatever another thread has mapped there meanwhile.
            vm_address_t candidate = cursor;
            if (vm_map(mach_task_self(), &candidate, size, 0, VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_DEFAULT) == KERN_SUCCESS)
                return reinterpret_cast<void*>(candidate);
            // Another thread may have just mapped into this hole: look at the
            // map again from here once; a second failure here is the kernel
            // refusing these addresses, so move past the hole.
            if (failed_at != cursor) {
                failed_at = cursor;
                continue;
            }
            cursor = hole_end;
            continue;
        }
        if (found != KERN_SUCCESS || region_size == 0)
            break;
        cursor = (region + region_size + page_mask) & ~page_mask;
    }
    return nullptr;
}

/// Reserves `size` bytes of inaccessible address space that ends at or below
/// kJIT26AddressLimit, or returns nullptr when none is left.
inline void* ReserveBelowJIT26Limit(std::size_t size)
{
    void* address = mmap(nullptr, size, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (address != MAP_FAILED) {
        if (EndsBelowJIT26Limit(address, size))
            return address;
        munmap(address, size);
    }
    return ReserveHoleBelowJIT26Limit(size);
}

// Address space set aside early (see ReserveLowDualCodeBlockSlots) for code
// blocks created later, when the low address space may be gone.
//
// Plain zero-initialized data only: this header is compiled both as C++20
// (Dynarmic) and as C++17 (its embedder), and code blocks are built from
// static initializers. A std::vector/std::mutex static gets constant
// initialization under C++20 but a guarded constructor under C++17; the
// linker keeps one definition, and the other side then locks a zeroed mutex.
inline constexpr std::size_t kMaxLowCodeSlots = 128;

struct LowCodeSlotPool {
    os_unfair_lock lock;
    std::size_t slot_size;
    std::size_t owned_count;  // reserved slots, including those in use
    std::size_t free_count;
    void* free_slots[kMaxLowCodeSlots];
};

inline LowCodeSlotPool& GetLowCodeSlotPool()
{
    static LowCodeSlotPool pool;  // zero-initialized; OS_UNFAIR_LOCK_INIT is 0
    return pool;
}

inline void* TakeLowCodeSlot(std::size_t size, std::size_t& slot_size)
{
    LowCodeSlotPool& pool = GetLowCodeSlotPool();
    void* slot = nullptr;
    os_unfair_lock_lock(&pool.lock);
    if (pool.free_count != 0 && size <= pool.slot_size) {
        slot = pool.free_slots[--pool.free_count];
        slot_size = pool.slot_size;
    }
    os_unfair_lock_unlock(&pool.lock);
    return slot;
}

/// `slot` must be an inaccessible reservation of the pool's slot size again.
inline void ReturnLowCodeSlot(void* slot)
{
    LowCodeSlotPool& pool = GetLowCodeSlotPool();
    os_unfair_lock_lock(&pool.lock);
    // At most owned_count <= kMaxLowCodeSlots slots exist, so one always fits.
    pool.free_slots[pool.free_count++] = slot;
    os_unfair_lock_unlock(&pool.lock);
}

/// A slot whose range may no longer be wholly ours (a MAP_FIXED that failed
/// part-way): never hand it out or unmap it again; its address space is lost.
inline void RetireLowCodeSlot()
{
    LowCodeSlotPool& pool = GetLowCodeSlotPool();
    os_unfair_lock_lock(&pool.lock);
    pool.owned_count--;
    os_unfair_lock_unlock(&pool.lock);
}

}  // namespace detail

/// Sets aside address space for up to `count` later DualCodeBlocks of at most
/// `slot_size` bytes, below the 64 GiB that iOS 26 JIT debuggers can prepare.
/// Call it early, while the low address space is still free; the slots cost
/// address space only. `count` is the total number of slots to own, including
/// those in use. Returns how many slots are free now. Only one slot size is
/// supported; a call with another size changes nothing.
inline std::size_t ReserveLowDualCodeBlockSlots(std::size_t slot_size, std::size_t count)
{
    slot_size = round_page(slot_size);
    count = std::min(count, detail::kMaxLowCodeSlots);
    detail::LowCodeSlotPool& pool = detail::GetLowCodeSlotPool();
    os_unfair_lock_lock(&pool.lock);
    if (pool.slot_size == 0)
        pool.slot_size = slot_size;
    // Reserve with the lock held: code blocks taking slots meanwhile just wait.
    while (pool.slot_size == slot_size && pool.owned_count < count) {
        void* slot = detail::ReserveBelowJIT26Limit(slot_size);
        if (slot == nullptr)
            break;
        pool.free_slots[pool.free_count++] = slot;
        pool.owned_count++;
    }
    const std::size_t free_count = pool.free_count;
    os_unfair_lock_unlock(&pool.lock);
    return free_count;
}
#endif

class DualCodeBlock {
public:
    explicit DualCodeBlock(std::size_t size)
        : m_size(size)
    {
#if defined(_WIN32)
        m_wmem = m_xmem = (std::uint32_t*)VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (m_wmem == nullptr)
            throw std::bad_alloc{};
#elif defined(__APPLE__)
        const bool jit26 = detail::UsesJIT26PrepareRegion();
        m_xmem = MapExecutable(size, jit26);
        if (m_xmem == nullptr)
            throw std::bad_alloc{};

        if (DeviceHasTXM()) {
            if (jit26) {
                if (!detail::EndsBelowJIT26Limit(m_xmem, m_size)) {
                    std::fprintf(stderr,
                                 "oaknut: JIT region %p-%p lies above 64 GiB, where JIT debuggers may not prepare it; "
                                 "its code can then fault with a code-signing error\n",
                                 (void*)m_xmem, (void*)((char*)m_xmem + m_size));
                }
                JIT26PrepareRegion(m_xmem, m_size);
            } else if (detail::RunningOnPhysicalIOS() &&
                       !PreparePreIOS26TXMRegion()) {
                ReleaseExecutable();
                throw std::bad_alloc{};
            }
        }

        vm_address_t wmem = 0;
        vm_prot_t cur_prot, max_prot;
        kern_return_t ret = vm_remap(mach_task_self(), &wmem, size, 0, VM_FLAGS_ANYWHERE | VM_FLAGS_RANDOM_ADDR, mach_task_self(), reinterpret_cast<vm_address_t>(m_xmem), false, &cur_prot, &max_prot, VM_INHERIT_NONE);
        if (ret != KERN_SUCCESS) {
            ReleaseExecutable();
            throw std::bad_alloc{};
        }
        m_wmem = reinterpret_cast<std::uint32_t*>(wmem);

        if (mprotect(m_wmem, size, PROT_READ | PROT_WRITE) != 0) {
            munmap(m_wmem, m_size);
            ReleaseExecutable();
            throw std::bad_alloc{};
        }
#else
#    if defined(__OpenBSD__)
        char tmpl[] = "oaknut_dual_code_block.XXXXXXXXXX";
        fd = shm_mkstemp(tmpl);
        if (fd < 0)
            throw std::bad_alloc{};
        shm_unlink(tmpl);
#    else
        fd = memfd_create("oaknut_dual_code_block", 0);
        if (fd < 0)
            throw std::bad_alloc{};
#    endif

        int ret = ftruncate(fd, size);
        if (ret != 0)
            throw std::bad_alloc{};

        m_wmem = (std::uint32_t*)mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        m_xmem = (std::uint32_t*)mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);

        if (m_wmem == MAP_FAILED || m_xmem == MAP_FAILED)
            throw std::bad_alloc{};
#endif
    }

    ~DualCodeBlock()
    {
#if defined(_WIN32)
        VirtualFree((void*)m_xmem, 0, MEM_RELEASE);
#elif defined(__APPLE__)
        munmap(m_wmem, m_size);
        ReleaseExecutable();
#else
        munmap(m_wmem, m_size);
        munmap(m_xmem, m_size);
        close(fd);
#endif
    }

    DualCodeBlock(const DualCodeBlock&) = delete;
    DualCodeBlock& operator=(const DualCodeBlock&) = delete;
    DualCodeBlock(DualCodeBlock&&) = delete;
    DualCodeBlock& operator=(DualCodeBlock&&) = delete;

    /// Pointer to executable mirror of memory (permissions: R-X)
    std::uint32_t* xptr() const
    {
        return m_xmem;
    }

    /// Pointer to writeable mirror of memory (permissions: RW-)
    std::uint32_t* wptr() const
    {
        return m_wmem;
    }

    /// Invalidate should be used with executable memory pointers.
    void invalidate(std::uint32_t* mem, std::size_t size)
    {
#if defined(__APPLE__)
        sys_icache_invalidate(mem, size);
#elif defined(_WIN32)
        FlushInstructionCache(GetCurrentProcess(), mem, size);
#else
        static std::size_t icache_line_size = 0x10000, dcache_line_size = 0x10000;

        std::uint64_t ctr;
        __asm__ volatile("mrs %0, ctr_el0"
                         : "=r"(ctr));

        const std::size_t isize = icache_line_size = std::min<std::size_t>(icache_line_size, 4 << ((ctr >> 0) & 0xf));
        const std::size_t dsize = dcache_line_size = std::min<std::size_t>(dcache_line_size, 4 << ((ctr >> 16) & 0xf));

        const std::uintptr_t end = (std::uintptr_t)mem + size;

        for (std::uintptr_t addr = ((std::uintptr_t)mem) & ~(dsize - 1); addr < end; addr += dsize) {
            __asm__ volatile("dc cvau, %0"
                             :
                             : "r"(addr)
                             : "memory");
        }
        __asm__ volatile("dsb ish\n"
                         :
                         :
                         : "memory");

        for (std::uintptr_t addr = ((std::uintptr_t)mem) & ~(isize - 1); addr < end; addr += isize) {
            __asm__ volatile("ic ivau, %0"
                             :
                             : "r"(addr)
                             : "memory");
        }
        __asm__ volatile("dsb ish\nisb\n"
                         :
                         :
                         : "memory");
#endif
    }

    void invalidate_all()
    {
        invalidate(m_xmem, m_size);
    }

protected:
#if !defined(_WIN32) && !defined(__APPLE__)
    int fd = -1;
#endif
    std::uint32_t* m_xmem = nullptr;
    std::uint32_t* m_wmem = nullptr;
    std::size_t m_size = 0;

#if defined(__APPLE__)
    // Nonzero when m_xmem sits in a slot from detail::GetLowCodeSlotPool().
    std::size_t m_slot_size = 0;

    // Maps a fresh R-X region. With a JIT26 debugger it must end below
    // detail::kJIT26AddressLimit: take a reserved slot, else map anywhere and,
    // if that lands too high, retry in a free hole below the limit.
    std::uint32_t* MapExecutable(std::size_t size, bool jit26)
    {
        std::size_t slot_size = 0;
        void* reserved = detail::TakeLowCodeSlot(size, slot_size);
        if (reserved == nullptr && jit26) {
            void* xmem = mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
            if (xmem != MAP_FAILED && detail::EndsBelowJIT26Limit(xmem, size))
                return static_cast<std::uint32_t*>(xmem);
            if (xmem != MAP_FAILED)
                munmap(xmem, size);
            reserved = detail::ReserveHoleBelowJIT26Limit(size);
        }
        if (reserved != nullptr) {
            // Replaces only our own inaccessible reservation.
            void* xmem = mmap(reserved, size, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
            if (xmem != MAP_FAILED) {
                m_slot_size = slot_size;
                return static_cast<std::uint32_t*>(xmem);
            }
            // XNU rolls a failed overwrite back, but not always completely,
            // so the range may no longer be ours: leave it alone for good.
            if (slot_size != 0)
                detail::RetireLowCodeSlot();
        }
        void* xmem = mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
        return xmem == MAP_FAILED ? nullptr : static_cast<std::uint32_t*>(xmem);
    }

    void ReleaseExecutable()
    {
        if (m_slot_size == 0) {
            munmap(m_xmem, m_size);
            return;
        }
        // Turn the slot back into an inaccessible reservation for the next
        // code block; that one is prepared afresh.
        if (mmap(m_xmem, m_slot_size, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) != MAP_FAILED) {
            detail::ReturnLowCodeSlot(m_xmem);
        } else {
            // As in MapExecutable: the range may no longer be ours.
            detail::RetireLowCodeSlot();
        }
        m_slot_size = 0;
    }

    bool PreparePreIOS26TXMRegion() const {
        // iOS 17 and 18 require every page in the executable backing to be
        // materialized before vm_remap creates its writable alias.  Retain
        // W^X while doing so: fault the pages through a temporary RW mapping,
        // then restore the original RX protection before continuing.
        if (mprotect(m_xmem, m_size, PROT_READ | PROT_WRITE) != 0)
            return false;

        volatile std::uint8_t* const bytes =
            reinterpret_cast<volatile std::uint8_t*>(m_xmem);
        const std::size_t page_size =
            static_cast<std::size_t>(getpagesize());
        for (std::size_t offset = 0; offset < m_size;
             offset += page_size) {
            const std::uint8_t value = bytes[offset];
            bytes[offset] = value;
        }

        return mprotect(m_xmem, m_size, PROT_READ | PROT_EXEC) == 0;
    }

    bool DeviceHasTXM() const {
        return detail::DeviceHasTXM();
    }

    __attribute__((noinline,optnone,naked))
    void JIT26PrepareRegion(void *addr, std::size_t len) const {
        asm("mov x0, x1 \n"
            "mov x1, x2 \n"
            "mov x16, #1 \n"
            "brk #0xf00d \n"
            "ret");
    }
#endif
};

}  // namespace oaknut
