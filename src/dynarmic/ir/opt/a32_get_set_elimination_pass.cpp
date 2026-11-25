/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <algorithm>
#include <array>
#include <functional>

#include <mcl/assert.hpp>
#include <mcl/stdint.hpp>

#include "dynarmic/frontend/A32/a32_ir_emitter.h"
#include "dynarmic/frontend/A32/a32_types.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/opt/passes.h"
#include "dynarmic/ir/value.h"

namespace Dynarmic::Optimization {

namespace {

void FlagsPass(IR::Block& block) {
    using Iterator = std::reverse_iterator<IR::Block::iterator>;

    struct FlagInfo {
        bool set_not_required = false;
        bool has_value_request = false;
        Iterator value_request = {};
    };
    struct ValuelessFlagInfo {
        bool set_not_required = false;
    };
    ValuelessFlagInfo nzcvq;
    ValuelessFlagInfo nzcv;
    ValuelessFlagInfo nz;
    FlagInfo c_flag;
    FlagInfo ge;

    auto do_set = [&](FlagInfo& info, IR::Value value, Iterator inst) {
        if (info.has_value_request) {
            info.value_request->ReplaceUsesWith(value);
        }
        info.has_value_request = false;

        if (info.set_not_required) {
            inst->Invalidate();
        }
        info.set_not_required = true;
    };

    auto do_set_valueless = [&](ValuelessFlagInfo& info, Iterator inst) {
        if (info.set_not_required) {
            inst->Invalidate();
        }
        info.set_not_required = true;
    };

    auto do_get = [](FlagInfo& info, Iterator inst) {
        if (info.has_value_request) {
            info.value_request->ReplaceUsesWith(IR::Value{&*inst});
        }
        info.has_value_request = true;
        info.value_request = inst;
    };

    A32::IREmitter ir{block, A32::LocationDescriptor{block.Location()}, {}};

    for (auto inst = block.rbegin(); inst != block.rend(); ++inst) {
        switch (inst->GetOpcode()) {
        case IR::Opcode::A32GetCFlag: {
            do_get(c_flag, inst);
            break;
        }
        case IR::Opcode::A32SetCpsrNZCV: {
            if (c_flag.has_value_request) {
                ir.SetInsertionPointBefore(inst.base());  // base is one ahead
                IR::U1 c = ir.GetCFlagFromNZCV(IR::NZCV{inst->GetArg(0)});
                c_flag.value_request->ReplaceUsesWith(c);
                c_flag.has_value_request = false;
                break;  // This case will be executed again because of the above
            }

            do_set_valueless(nzcv, inst);

            nz = {.set_not_required = true};
            c_flag = {.set_not_required = true};
            break;
        }
        case IR::Opcode::A32SetCpsrNZCVRaw: {
            if (c_flag.has_value_request) {
                nzcv.set_not_required = false;
            }

            do_set_valueless(nzcv, inst);

            nzcvq = {};
            nz = {.set_not_required = true};
            c_flag = {.set_not_required = true};
            break;
        }
        case IR::Opcode::A32SetCpsrNZCVQ: {
            if (c_flag.has_value_request) {
                nzcvq.set_not_required = false;
            }

            do_set_valueless(nzcvq, inst);

            nzcv = {.set_not_required = true};
            nz = {.set_not_required = true};
            c_flag = {.set_not_required = true};
            break;
        }
        case IR::Opcode::A32SetCpsrNZ: {
            do_set_valueless(nz, inst);

            nzcvq = {};
            nzcv = {};
            break;
        }
        case IR::Opcode::A32SetCpsrNZC: {
            if (c_flag.has_value_request) {
                c_flag.value_request->ReplaceUsesWith(inst->GetArg(1));
                c_flag.has_value_request = false;
            }

            if (!inst->GetArg(1).IsImmediate() && inst->GetArg(1).GetInstRecursive()->GetOpcode() == IR::Opcode::A32GetCFlag) {
                const auto nz_value = inst->GetArg(0);

                inst->Invalidate();

                ir.SetInsertionPointBefore(inst.base());
                ir.SetCpsrNZ(IR::NZCV{nz_value});

                nzcvq = {};
                nzcv = {};
                nz = {.set_not_required = true};
                break;
            }

            if (nz.set_not_required && c_flag.set_not_required) {
                inst->Invalidate();
            } else if (nz.set_not_required) {
                inst->SetArg(0, IR::Value::EmptyNZCVImmediateMarker());
            }
            nz.set_not_required = true;
            c_flag.set_not_required = true;

            nzcv = {};
            nzcvq = {};
            break;
        }
        case IR::Opcode::A32SetGEFlags: {
            do_set(ge, inst->GetArg(0), inst);
            break;
        }
        case IR::Opcode::A32GetGEFlags: {
            do_get(ge, inst);
            break;
        }
        case IR::Opcode::A32SetGEFlagsCompressed: {
            ge = {.set_not_required = true};
            break;
        }
        case IR::Opcode::A32OrQFlag: {
            break;
        }
        default: {
            if (inst->ReadsFromCPSR() || inst->WritesToCPSR()) {
                nzcvq = {};
                nzcv = {};
                nz = {};
                c_flag = {};
                ge = {};
            }
            break;
        }
        }
    }
}

void RegisterPass(IR::Block& block, bool forward_ext_reg_views) {
    using Iterator = IR::Block::iterator;

    struct RegInfo {
        IR::Value register_value;
        std::optional<Iterator> last_set_instruction;
    };
    std::array<RegInfo, 15> reg_info;

    const auto do_get = [](RegInfo& info, Iterator get_inst) {
        if (info.register_value.IsEmpty()) {
            info.register_value = IR::Value(&*get_inst);
            return;
        }
        get_inst->ReplaceUsesWith(info.register_value);
    };

    const auto do_set = [](RegInfo& info, IR::Value value, Iterator set_inst) {
        if (info.last_set_instruction) {
            (*info.last_set_instruction)->Invalidate();
        }
        info = {
            .register_value = value,
            .last_set_instruction = set_inst,
        };
    };

    enum class ExtValueType {
        Empty,
        Single,
        Double,
        VectorDouble,
        VectorQuad,
    };
    struct ExtRegInfo {
        ExtValueType value_type = {};
        IR::Value register_value;
        std::optional<Iterator> last_set_instruction;
    };
    std::array<ExtRegInfo, 64> ext_reg_info;

    // Location and version don't matter here.
    A32::IREmitter ir{block, A32::LocationDescriptor{block.Location()}, {}};

    // forward_ext_reg_views: a read of a register whose words were last written by several
    // smaller register ops (two D registers, then the Q register that contains them; two S
    // registers, then their D register) would load from the JIT state right after those
    // stores, which the host cannot store-forward (a long stall). Build such values from the
    // pieces instead. A read contained in one earlier write is left alone: its load is
    // store-forwarded, which is as cheap as moving the value between register files.
    // ext_reg_info[k] describes 32-bit word k by the register op that last covered it (S
    // register k, D register k / 2 or Q register k / 4, as the type says), so two words of
    // one D register with the same non-Single type come from the same op.
    // These insert IR before the current insertion point.
    const auto info_type = [&](size_t k) { return ext_reg_info[k].value_type; };
    const auto pack_singles = [&](size_t first_word) {
        return ir.Pack2x32To1x64(IR::U32{ext_reg_info[first_word].register_value}, IR::U32{ext_reg_info[first_word + 1].register_value});
    };
    // Value of register `type` whose first word is `first`, when it is worth building.
    const auto view_of = [&](ExtValueType type, size_t first) -> std::optional<IR::Value> {
        switch (type) {
        case ExtValueType::Single:
            // The low half of a D value is free (same register in either file).
            if (info_type(first) == ExtValueType::Double && first % 2 == 0) {
                return IR::Value{ir.LeastSignificantWord(IR::U64{ext_reg_info[first].register_value})};
            }
            return std::nullopt;
        case ExtValueType::Double:
            if (info_type(first) == ExtValueType::Single && info_type(first + 1) == ExtValueType::Single) {
                return IR::Value{pack_singles(first)};
            }
            // A D register out of a vector value (e.g. VST1 of a Q register a NEON op just
            // wrote). The lane goes to a general register, which suits stores and VMOVs.
            if (info_type(first) == ExtValueType::VectorQuad && info_type(first + 1) == ExtValueType::VectorQuad) {
                return IR::Value{ir.VectorGetElement(64, IR::U128{ext_reg_info[first].register_value}, (first / 2) % 2)};
            }
            if (info_type(first) == ExtValueType::VectorDouble && info_type(first + 1) == ExtValueType::VectorDouble) {
                return IR::Value{ir.VectorGetElement(64, IR::U128{ext_reg_info[first].register_value}, 0)};
            }
            return std::nullopt;
        case ExtValueType::VectorDouble:
            if (info_type(first) == ExtValueType::VectorQuad && info_type(first + 1) == ExtValueType::VectorQuad && first % 4 == 0) {
                return IR::Value{ir.VectorZeroUpper(IR::U128{ext_reg_info[first].register_value})};
            }
            if (info_type(first) == ExtValueType::Double && info_type(first + 1) == ExtValueType::Double) {
                return IR::Value{ir.ZeroExtendToQuad(IR::U64{ext_reg_info[first].register_value})};
            }
            if (info_type(first) == ExtValueType::Single && info_type(first + 1) == ExtValueType::Single) {
                return IR::Value{ir.ZeroExtendToQuad(pack_singles(first))};
            }
            return std::nullopt;
        case ExtValueType::VectorQuad: {
            const ExtValueType lo = info_type(first), hi = info_type(first + 2);
            if (lo != info_type(first + 1) || hi != info_type(first + 3)) {
                return std::nullopt;
            }
            const IR::Value lo_value = ext_reg_info[first].register_value;
            const IR::Value hi_value = ext_reg_info[first + 2].register_value;
            if (lo == ExtValueType::Double && hi == ExtValueType::Double) {
                return IR::Value{ir.Pack2x64To1x128(IR::U64{lo_value}, IR::U64{hi_value})};
            }
            if (lo == ExtValueType::VectorDouble && hi == ExtValueType::VectorDouble) {
                return IR::Value{ir.VectorInterleaveLower(64, IR::U128{lo_value}, IR::U128{hi_value})};
            }
            // One half rewritten by a D op since the Q register was last written or read.
            if (lo == ExtValueType::VectorQuad && hi == ExtValueType::Double) {
                return IR::Value{ir.VectorSetElement(64, IR::U128{lo_value}, 1, IR::U64{hi_value})};
            }
            if (lo == ExtValueType::Double && hi == ExtValueType::VectorQuad) {
                return IR::Value{ir.VectorSetElement(64, IR::U128{hi_value}, 0, IR::U64{lo_value})};
            }
            return std::nullopt;
        }
        case ExtValueType::Empty:
            break;
        }
        return std::nullopt;
    };

    const auto do_ext_get = [&](ExtValueType type, std::initializer_list<std::reference_wrapper<ExtRegInfo>> infos, Iterator get_inst) {
        if (!std::all_of(infos.begin(), infos.end(), [type](const auto& info) { return info.get().value_type == type; })) {
            IR::Value value = IR::Value(&*get_inst);
            if (forward_ext_reg_views && std::none_of(infos.begin(), infos.end(), [](const auto& info) { return info.get().value_type == ExtValueType::Empty; })) {
                const size_t first = static_cast<size_t>(&std::data(infos)[0].get() - ext_reg_info.data());
                ir.SetInsertionPointBefore(get_inst);
                if (const auto view = view_of(type, first)) {
                    get_inst->ReplaceUsesWith(*view);
                    value = *view;
                }
            }
            for (auto& info : infos) {
                info.get() = {
                    .value_type = type,
                    .register_value = value,
                    .last_set_instruction = std::nullopt,
                };
            }
            return;
        }
        get_inst->ReplaceUsesWith(std::data(infos)[0].get().register_value);
    };

    const auto do_ext_set = [](ExtValueType type, std::initializer_list<std::reference_wrapper<ExtRegInfo>> infos, IR::Value value, Iterator set_inst) {
        if (std::all_of(infos.begin(), infos.end(), [type](const auto& info) { return info.get().value_type == type; })) {
            if (std::data(infos)[0].get().last_set_instruction) {
                (*std::data(infos)[0].get().last_set_instruction)->Invalidate();
            }
        }
        for (auto& info : infos) {
            info.get() = {
                .value_type = type,
                .register_value = value,
                .last_set_instruction = set_inst,
            };
        }
    };


    for (auto inst = block.begin(); inst != block.end(); ++inst) {
        switch (inst->GetOpcode()) {
        case IR::Opcode::A32GetRegister: {
            const A32::Reg reg = inst->GetArg(0).GetA32RegRef();
            ASSERT(reg != A32::Reg::PC);
            const size_t reg_index = static_cast<size_t>(reg);
            do_get(reg_info[reg_index], inst);
            break;
        }
        case IR::Opcode::A32SetRegister: {
            const A32::Reg reg = inst->GetArg(0).GetA32RegRef();
            if (reg == A32::Reg::PC) {
                break;
            }
            const auto reg_index = static_cast<size_t>(reg);
            do_set(reg_info[reg_index], inst->GetArg(1), inst);
            break;
        }
        case IR::Opcode::A32GetExtendedRegister32: {
            const A32::ExtReg reg = inst->GetArg(0).GetA32ExtRegRef();
            const size_t reg_index = A32::RegNumber(reg);
            do_ext_get(ExtValueType::Single, {ext_reg_info[reg_index]}, inst);
            break;
        }
        case IR::Opcode::A32SetExtendedRegister32: {
            const A32::ExtReg reg = inst->GetArg(0).GetA32ExtRegRef();
            const size_t reg_index = A32::RegNumber(reg);
            do_ext_set(ExtValueType::Single, {ext_reg_info[reg_index]}, inst->GetArg(1), inst);
            break;
        }
        case IR::Opcode::A32GetExtendedRegister64: {
            const A32::ExtReg reg = inst->GetArg(0).GetA32ExtRegRef();
            const size_t reg_index = A32::RegNumber(reg);
            do_ext_get(ExtValueType::Double,
                       {
                           ext_reg_info[reg_index * 2 + 0],
                           ext_reg_info[reg_index * 2 + 1],
                       },
                       inst);
            break;
        }
        case IR::Opcode::A32SetExtendedRegister64: {
            const A32::ExtReg reg = inst->GetArg(0).GetA32ExtRegRef();
            const size_t reg_index = A32::RegNumber(reg);
            do_ext_set(ExtValueType::Double,
                       {
                           ext_reg_info[reg_index * 2 + 0],
                           ext_reg_info[reg_index * 2 + 1],
                       },
                       inst->GetArg(1),
                       inst);
            break;
        }
        case IR::Opcode::A32GetVector: {
            const A32::ExtReg reg = inst->GetArg(0).GetA32ExtRegRef();
            const size_t reg_index = A32::RegNumber(reg);
            if (A32::IsDoubleExtReg(reg)) {
                do_ext_get(ExtValueType::VectorDouble,
                           {
                               ext_reg_info[reg_index * 2 + 0],
                               ext_reg_info[reg_index * 2 + 1],
                           },
                           inst);
            } else {
                DEBUG_ASSERT(A32::IsQuadExtReg(reg));
                do_ext_get(ExtValueType::VectorQuad,
                           {
                               ext_reg_info[reg_index * 4 + 0],
                               ext_reg_info[reg_index * 4 + 1],
                               ext_reg_info[reg_index * 4 + 2],
                               ext_reg_info[reg_index * 4 + 3],
                           },
                           inst);
            }
            break;
        }
        case IR::Opcode::A32SetVector: {
            const A32::ExtReg reg = inst->GetArg(0).GetA32ExtRegRef();
            const size_t reg_index = A32::RegNumber(reg);
            if (A32::IsDoubleExtReg(reg)) {
                ir.SetInsertionPointAfter(inst);
                const IR::U128 stored_value = ir.VectorZeroUpper(IR::U128{inst->GetArg(1)});
                do_ext_set(ExtValueType::VectorDouble,
                           {
                               ext_reg_info[reg_index * 2 + 0],
                               ext_reg_info[reg_index * 2 + 1],
                           },
                           stored_value,
                           inst);
            } else {
                DEBUG_ASSERT(A32::IsQuadExtReg(reg));
                do_ext_set(ExtValueType::VectorQuad,
                           {
                               ext_reg_info[reg_index * 4 + 0],
                               ext_reg_info[reg_index * 4 + 1],
                               ext_reg_info[reg_index * 4 + 2],
                               ext_reg_info[reg_index * 4 + 3],
                           },
                           inst->GetArg(1),
                           inst);
            }
            break;
        }
        default: {
            if (inst->ReadsFromCoreRegister() || inst->WritesToCoreRegister()) {
                reg_info = {};
                ext_reg_info = {};
            }
            break;
        }
        }
    }
}

}  // namespace

void A32GetSetElimination(IR::Block& block, A32GetSetEliminationOptions opt) {
    FlagsPass(block);
    RegisterPass(block, opt.forward_ext_reg_views);
}

}  // namespace Dynarmic::Optimization
