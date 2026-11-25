/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/reg_alloc.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/ir/basic_block.h"

namespace Dynarmic::IR {
class Block;
}  // namespace Dynarmic::IR

namespace Dynarmic::Backend::Arm64 {

struct EmitConfig;
class FastmemManager;
class FpsrManager;

using SharedLabel = std::shared_ptr<oaknut::Label>;

inline SharedLabel GenSharedLabel() {
    return std::make_shared<oaknut::Label>();
}

struct EmitContext {
    IR::Block& block;
    RegAlloc& reg_alloc;
    const EmitConfig& conf;
    EmittedBlockInfo& ebi;
    FpsrManager& fpsr;
    FastmemManager& fastmem;

    std::vector<std::function<void()>> deferred_emits;

    // HostFlagForwarding state (A32):
    // the block's condition check is being emitted (record flags_entry_offset)
    bool recording_flags_entry = false;
    // the host NZCV holds the guest NZCV: at block entry, before the first IR instruction
    bool entry_nzcv_in_host = false;
    // ... and while emitting the terminal (set by A32TerminalNZCV)
    bool terminal_nzcv_in_host = false;
    // register holding the check bit for a CheckBit terminal (A32TerminalCheckBit)
    std::optional<int> terminal_check_bit_reg;

    FP::FPCR FPCR(bool fpcr_controlled = true) const {
        const FP::FPCR fpcr = conf.descriptor_to_fpcr(block.Location());
        if (fpcr_controlled)
            return fpcr;
        // Unsafe_IgnoreStandardFPCRValue: run ASIMD with the guest's own FZ/DN
        // instead of switching FPCR around every instruction. Keep the switch
        // when the rounding mode differs, since emitters assume round-to-nearest.
        const FP::FPCR standard = fpcr.ASIMDStandardValue();
        if (conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreStandardFPCRValue) && fpcr.RMode() == standard.RMode())
            return fpcr;
        return standard;
    }
};

}  // namespace Dynarmic::Backend::Arm64
