/* This file is part of the dynarmic project.
 * Copyright (c) 2026 the LiveExec32 contributors
 * SPDX-License-Identifier: 0BSD
 */

#include <optional>

#include <boost/variant/get.hpp>

#include "dynarmic/frontend/A32/a32_ir_emitter.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/opt/passes.h"

// OptimizationFlag::HostFlagForwarding: tell the backend which IR values the block's
// terminal tests, so it can keep them in host registers instead of storing them to the JIT
// state (or stack) and reloading them at the end of the block.
//  - VCMP followed by VMRS APSR_nzcv: the comparison result goes straight to the CPSR.
//  - A32TerminalNZCV(x): x is the guest NZCV at the end of the block. Conditional terminals
//    test it in the host flags, and links can enter a conditional block past its flag load.
//    In a conditional block without NZCV writes, x is the entry flags (A32GetCpsrNZCV).
//  - A32TerminalCheckBit(b): a CheckBit terminal tests b in its register (Thumb CBZ/CBNZ).

namespace Dynarmic::Optimization {

namespace {

// Instructions after which the JIT state may have been changed behind the IR's back.
// Memory accesses are not among them: as everywhere in the IR optimizations, memory
// callbacks must not change guest registers or the CPSR.
bool MayChangeStateExternally(const IR::Inst& inst) {
    return inst.CausesCPUException()
        || inst.IsCoprocessorInstruction()
        || inst.GetOpcode() == IR::Opcode::CallHostFunction
        || inst.GetOpcode() == IR::Opcode::A32InstructionSynchronizationBarrier;
}

bool WritesToNZCV(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::A32SetCpsr:
    case IR::Opcode::A32SetCpsrNZCV:
    case IR::Opcode::A32SetCpsrNZCVRaw:
    case IR::Opcode::A32SetCpsrNZCVQ:
    case IR::Opcode::A32SetCpsrNZ:
    case IR::Opcode::A32SetCpsrNZC:
        return true;
    default:
        return false;
    }
}

// VCMP + VMRS APSR_nzcv: SetFpscrNZCV(x) ... SetCpsrNZCVRaw(GetFpscrNZCV()) becomes
// SetCpsrNZCV(x), dropping the FPSCR store/load round trip.
void ForwardFpscrNZCV(IR::Block& block, A32::IREmitter& ir) {
    std::optional<IR::Value> fpscr_nzcv;   // argument of the last SetFpscrNZCV
    const IR::Inst* reader = nullptr;      // a GetFpscrNZCV after it
    for (auto inst = block.begin(); inst != block.end(); ++inst) {
        switch (inst->GetOpcode()) {
        case IR::Opcode::A32SetFpscrNZCV:
            fpscr_nzcv = inst->GetArg(0);
            reader = nullptr;
            if (fpscr_nzcv->IsImmediate()) {
                fpscr_nzcv.reset();
            }
            break;
        case IR::Opcode::A32GetFpscrNZCV:
            reader = fpscr_nzcv ? &*inst : nullptr;
            break;
        case IR::Opcode::A32SetCpsrNZCVRaw: {
            const IR::Value arg = inst->GetArg(0);
            if (reader && !arg.IsImmediate() && arg.GetInst() == reader) {
                ir.SetInsertionPointBefore(inst);
                ir.SetCpsrNZCV(IR::NZCV{*fpscr_nzcv});
                inst->Invalidate();
            }
            break;
        }
        default:
            if (inst->GetOpcode() == IR::Opcode::A32SetFpscr || MayChangeStateExternally(*inst)) {
                fpscr_nzcv.reset();
                reader = nullptr;
            }
            break;
        }
    }
}

struct TerminalUse {
    bool flags = false;  // tests NZCV or links to blocks (which may be conditional)
    bool outer_check_bit = false;
    int check_bits = 0;
};

void Inspect(const IR::Terminal& terminal, TerminalUse& use, bool outermost) {
    if (boost::get<IR::Term::LinkBlock>(&terminal) || boost::get<IR::Term::LinkBlockFast>(&terminal)) {
        use.flags = true;
    } else if (const auto* t = boost::get<IR::Term::If>(&terminal)) {
        use.flags = true;
        Inspect(t->then_, use, false);
        Inspect(t->else_, use, false);
    } else if (const auto* t = boost::get<IR::Term::CheckBit>(&terminal)) {
        use.outer_check_bit |= outermost;
        use.check_bits++;
        Inspect(t->then_, use, false);
        Inspect(t->else_, use, false);
    } else if (const auto* t = boost::get<IR::Term::CheckHalt>(&terminal)) {
        Inspect(t->else_, use, false);
    }
}

}  // namespace

void A32TerminalFlagsPass(IR::Block& block) {
    A32::IREmitter ir{block, A32::LocationDescriptor{block.Location()}, {}};

    ForwardFpscrNZCV(block, ir);

    TerminalUse use;
    Inspect(block.GetTerminal(), use, true);

    std::optional<IR::Value> check_bit;
    if (use.outer_check_bit && use.check_bits == 1) {
        for (auto inst = block.end(); inst != block.begin();) {
            --inst;
            if (inst->GetOpcode() == IR::Opcode::A32SetCheckBit) {
                if (!inst->GetArg(0).IsImmediate()) {
                    check_bit = inst->GetArg(0);
                    inst->Invalidate();
                }
                break;
            }
            if (MayChangeStateExternally(*inst)) {
                break;
            }
        }
    }

    std::optional<IR::Value> nzcv;
    if (use.flags) {
        bool usable = true;
        for (auto inst = block.end(); inst != block.begin();) {
            --inst;
            if (inst->GetOpcode() == IR::Opcode::A32SetCpsrNZCV) {
                if (!inst->GetArg(0).IsImmediate()) {
                    nzcv = inst->GetArg(0);
                }
                usable = false;
                break;
            }
            if (WritesToNZCV(inst->GetOpcode()) || MayChangeStateExternally(*inst)) {
                usable = false;
                break;
            }
        }
        if (usable && block.GetCondition() != IR::Cond::AL) {
            // No NZCV writes: the flags the block's condition check loaded are still current.
            ir.SetInsertionPointBefore(block.begin());
            nzcv = ir.GetCpsrNZCV();
        }
    }

    ir.SetInsertionPointBefore(block.end());
    // TerminalCheckBit last: nothing may reuse its register before the terminal.
    if (nzcv) {
        ir.TerminalNZCV(IR::NZCV{*nzcv});
    }
    if (check_bit) {
        ir.TerminalCheckBit(IR::U1{*check_bit});
    }
}

}  // namespace Dynarmic::Optimization
