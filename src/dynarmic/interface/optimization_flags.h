/* This file is part of the dynarmic project.
 * Copyright (c) 2020 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstdint>

namespace Dynarmic {

enum class OptimizationFlag : std::uint32_t {
    /// This optimization avoids dispatcher lookups by allowing emitted basic blocks to jump
    /// directly to other basic blocks if the destination PC is predictable at JIT-time.
    /// This is a safe optimization.
    BlockLinking = 0x00000001,
    /// This optimization avoids dispatcher lookups by emulating a return stack buffer. This
    /// allows for function returns and syscall returns to be predicted at runtime.
    /// This is a safe optimization.
    ReturnStackBuffer = 0x00000002,
    /// This optimization enables a two-tiered dispatch system.
    /// A fast dispatcher (written in assembly) first does a look-up in a small MRU cache.
    /// If this fails, it falls back to the usual slower dispatcher.
    /// This is a safe optimization.
    FastDispatch = 0x00000004,
    /// This is an IR optimization. This optimization eliminates unnecessary emulated CPU state
    /// context lookups.
    /// This is a safe optimization.
    GetSetElimination = 0x00000008,
    /// This is an IR optimization. This optimization does constant propagation.
    /// This is a safe optimization.
    ConstProp = 0x00000010,
    /// This is enables miscellaneous safe IR optimizations.
    MiscIROpt = 0x00000020,
    /// A32 with page tables (arm64 backend): instructions that access a known run of
    /// memory (LDM/STM, PUSH/POP, VLDM/VSTM, VPUSH/VPOP, VLD1/VST1) do one page-table
    /// lookup for the whole run. Falls back to the per-access path at page boundaries.
    /// This is a safe optimization.
    MemoryRangeFusion = 0x00000040,
    /// A32: 64-bit VFP/NEON loads and stores (VLDR/VSTR/VLDM/VSTM/VPUSH/VPOP Dn, VLD1/VST1)
    /// use one 64-bit memory access per D register instead of 32-bit or per-element ones
    /// (little-endian only), and get/set elimination forwards values between the S, D and
    /// Q views of the extension registers. As with LDRD/STRD, a D register that crosses
    /// into a faulting page reaches the callbacks as one 64-bit access (so its first word is
    /// not stored separately). With page tables the wide accesses are only used when
    /// detect_misaligned_access_via_page_table includes 64, and (with
    /// only_detect_misalignment_via_page_table_on_page_boundary) any 4-aligned 64-bit access
    /// that crosses a page, LDRD/STRD included, is done as two 32-bit page-table accesses
    /// when both pages are in the table, instead of calling back. This is a safe optimization.
    WideVectorMemory = 0x00000080,
    /// A32 with page tables (arm64 backend): LDREX/STREX run inline against the page tables
    /// instead of calling out to the global exclusive monitor (see
    /// ExclusiveMonitor::InlineReservationTable). All JITs sharing a global_monitor must
    /// end up using the same scheme: the same setting, and either all with page tables and
    /// no fastmem (inline) or all without (monitor). This is a safe optimization.
    InlineExclusiveMemory = 0x00000100,
    /// A32 (arm64 backend): conditional terminals, conditional block entries reached through
    /// a block link, VMRS APSR_nzcv and Thumb CBZ/CBNZ use values still in host registers
    /// (the host NZCV, a register) instead of storing and reloading them. Like get/set
    /// elimination, this assumes memory callbacks do not change the guest CPSR. This is a
    /// safe optimization.
    HostFlagForwarding = 0x00000200,

    /// This is an UNSAFE optimization that reduces accuracy of fused multiply-add operations.
    /// This unfuses fused instructions to improve performance on host CPUs without FMA support.
    Unsafe_UnfuseFMA = 0x00010000,
    /// This is an UNSAFE optimization that reduces accuracy of certain floating-point instructions.
    /// This allows results of FRECPE and FRSQRTE to have **less** error than spec allows.
    Unsafe_ReducedErrorFP = 0x00020000,
    /// This is an UNSAFE optimization that causes floating-point instructions to not produce correct NaNs.
    /// This may also result in inaccurate results when instructions are given certain special values.
    Unsafe_InaccurateNaN = 0x00040000,
    /// This is an UNSAFE optimization that causes ASIMD floating-point instructions to be run with incorrect
    /// rounding modes. This may result in inaccurate results with all floating-point ASIMD instructions.
    Unsafe_IgnoreStandardFPCRValue = 0x00080000,
    /// This is an UNSAFE optimization that causes the global monitor to be ignored. This may
    /// result in unexpected behaviour in multithreaded scenarios, including but not limited
    /// to data races and deadlocks.
    Unsafe_IgnoreGlobalMonitor = 0x00100000,
    /// This is an UNSAFE optimization that stops tracking cumulative floating-point exception
    /// flags (and the saturation flag QC). Guest reads of those FPSCR bits always see zero.
    /// arm64 backend only: saves an FPSR write and read in every block that uses floating point.
    Unsafe_IgnoreFPExceptionFlags = 0x00200000,
};

constexpr OptimizationFlag no_optimizations = static_cast<OptimizationFlag>(0);
constexpr OptimizationFlag all_safe_optimizations = static_cast<OptimizationFlag>(0x0000FFFF);

constexpr OptimizationFlag operator~(OptimizationFlag f) {
    return static_cast<OptimizationFlag>(~static_cast<std::uint32_t>(f));
}

constexpr OptimizationFlag operator|(OptimizationFlag f1, OptimizationFlag f2) {
    return static_cast<OptimizationFlag>(static_cast<std::uint32_t>(f1) | static_cast<std::uint32_t>(f2));
}

constexpr OptimizationFlag operator&(OptimizationFlag f1, OptimizationFlag f2) {
    return static_cast<OptimizationFlag>(static_cast<std::uint32_t>(f1) & static_cast<std::uint32_t>(f2));
}

constexpr OptimizationFlag operator|=(OptimizationFlag& result, OptimizationFlag f) {
    return result = (result | f);
}

constexpr OptimizationFlag operator&=(OptimizationFlag& result, OptimizationFlag f) {
    return result = (result & f);
}

constexpr bool operator!(OptimizationFlag f) {
    return f == no_optimizations;
}

}  // namespace Dynarmic
