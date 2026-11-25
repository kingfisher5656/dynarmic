/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/arm64/emit_arm64_memory.h"

#include <optional>
#include <utility>

#include <mcl/bit_cast.hpp>
#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/emit_context.h"
#include "dynarmic/backend/arm64/fastmem.h"
#include "dynarmic/backend/arm64/fpsr_manager.h"
#include "dynarmic/backend/arm64/reg_alloc.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/ir/acc_type.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Arm64 {

using namespace oaknut::util;

namespace {

bool IsOrdered(IR::AccType acctype) {
    return acctype == IR::AccType::ORDERED || acctype == IR::AccType::ORDEREDRW || acctype == IR::AccType::LIMITEDORDERED;
}

LinkTarget ReadMemoryLinkTarget(size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::ReadMemory8;
    case 16:
        return LinkTarget::ReadMemory16;
    case 32:
        return LinkTarget::ReadMemory32;
    case 64:
        return LinkTarget::ReadMemory64;
    case 128:
        return LinkTarget::ReadMemory128;
    }
    UNREACHABLE();
}

LinkTarget WriteMemoryLinkTarget(size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::WriteMemory8;
    case 16:
        return LinkTarget::WriteMemory16;
    case 32:
        return LinkTarget::WriteMemory32;
    case 64:
        return LinkTarget::WriteMemory64;
    case 128:
        return LinkTarget::WriteMemory128;
    }
    UNREACHABLE();
}

LinkTarget WrappedReadMemoryLinkTarget(size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::WrappedReadMemory8;
    case 16:
        return LinkTarget::WrappedReadMemory16;
    case 32:
        return LinkTarget::WrappedReadMemory32;
    case 64:
        return LinkTarget::WrappedReadMemory64;
    case 128:
        return LinkTarget::WrappedReadMemory128;
    }
    UNREACHABLE();
}

LinkTarget WrappedWriteMemoryLinkTarget(size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::WrappedWriteMemory8;
    case 16:
        return LinkTarget::WrappedWriteMemory16;
    case 32:
        return LinkTarget::WrappedWriteMemory32;
    case 64:
        return LinkTarget::WrappedWriteMemory64;
    case 128:
        return LinkTarget::WrappedWriteMemory128;
    }
    UNREACHABLE();
}

LinkTarget ExclusiveReadMemoryLinkTarget(size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::ExclusiveReadMemory8;
    case 16:
        return LinkTarget::ExclusiveReadMemory16;
    case 32:
        return LinkTarget::ExclusiveReadMemory32;
    case 64:
        return LinkTarget::ExclusiveReadMemory64;
    case 128:
        return LinkTarget::ExclusiveReadMemory128;
    }
    UNREACHABLE();
}

LinkTarget ExclusiveWriteMemoryLinkTarget(size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::ExclusiveWriteMemory8;
    case 16:
        return LinkTarget::ExclusiveWriteMemory16;
    case 32:
        return LinkTarget::ExclusiveWriteMemory32;
    case 64:
        return LinkTarget::ExclusiveWriteMemory64;
    case 128:
        return LinkTarget::ExclusiveWriteMemory128;
    }
    UNREACHABLE();
}

template<size_t bitsize>
void CallbackOnlyEmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1]);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());

    EmitRelocation(code, ctx, ReadMemoryLinkTarget(bitsize));
    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }

    if constexpr (bitsize == 128) {
        code.MOV(Q8.B16(), Q0.B16());
        ctx.reg_alloc.DefineAsRegister(inst, Q8);
    } else {
        ctx.reg_alloc.DefineAsRegister(inst, X0);
    }
}

template<size_t bitsize>
void CallbackOnlyEmitExclusiveReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1]);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());

    code.MOV(Wscratch0, 1);
    code.STRB(Wscratch0, Xstate, ctx.conf.state_exclusive_state_offset);
    EmitRelocation(code, ctx, ExclusiveReadMemoryLinkTarget(bitsize));
    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }

    if constexpr (bitsize == 128) {
        code.MOV(Q8.B16(), Q0.B16());
        ctx.reg_alloc.DefineAsRegister(inst, Q8);
    } else {
        ctx.reg_alloc.DefineAsRegister(inst, X0);
    }
}

template<size_t bitsize>
void CallbackOnlyEmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1], args[2]);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());

    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }
    EmitRelocation(code, ctx, WriteMemoryLinkTarget(bitsize));
    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }
}

template<size_t bitsize>
void CallbackOnlyEmitExclusiveWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1], args[2]);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());

    oaknut::Label end;

    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }
    code.MOV(W0, 1);
    code.LDRB(Wscratch0, Xstate, ctx.conf.state_exclusive_state_offset);
    code.CBZ(Wscratch0, end);
    code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);
    EmitRelocation(code, ctx, ExclusiveWriteMemoryLinkTarget(bitsize));
    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }
    code.l(end);
    ctx.reg_alloc.DefineAsRegister(inst, X0);
}

constexpr size_t page_bits = 12;
constexpr size_t page_size = 1 << page_bits;
constexpr size_t page_mask = (1 << page_bits) - 1;

// This function may use Xscratch1 as a scratch register (leaving addr & page_mask in Xscratch1 on page-boundary check)
// Trashes NZCV when a check is emitted
template<size_t bitsize>
bool EmitDetectMisalignedVAddr(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, const SharedLabel& fallback) {
    static_assert(bitsize == 8 || bitsize == 16 || bitsize == 32 || bitsize == 64 || bitsize == 128);

    if (bitsize == 8 || (ctx.conf.detect_misaligned_access_via_page_table & bitsize) == 0) {
        return false;
    }

    if (!ctx.conf.only_detect_misalignment_via_page_table_on_page_boundary) {
        const u64 align_mask = []() -> u64 {
            switch (bitsize) {
            case 16:
                return 0b1;
            case 32:
                return 0b11;
            case 64:
                return 0b111;
            case 128:
                return 0b1111;
            default:
                UNREACHABLE();
            }
        }();

        code.TST(Xaddr, align_mask);
        code.B(NE, *fallback);
        return false;
    } else {
        // If (addr & page_mask) > page_size - byte_size, use fallback.
        code.AND(Xscratch1, Xaddr, page_mask);
        code.CMP(Xscratch1, page_size - bitsize / 8);
        code.B(HI, *fallback);
        return true;
    }
}

template<size_t bitsize>
bool InlinePageTableLookupTrashesFlags(const EmitContext& ctx) {
    const size_t unused_top_bits = 64 - ctx.conf.page_table_address_space_bits;
    const bool misaligned_check = bitsize != 8 && (ctx.conf.detect_misaligned_access_via_page_table & bitsize) != 0;
    const bool top_bits_check = !ctx.conf.silently_mirror_page_table && unused_top_bits != 0;
    return misaligned_check || top_bits_check;
}

// Outputs Xscratch0 = table_reg[addr >> page_bits]
// May use Xscratch1 as scratch register
// Address to read/write = [ret0 + ret1], ret0 is always Xscratch0 and ret1 is either Xaddr or Xscratch1
// Trashes NZCV iff InlinePageTableLookupTrashesFlags<bitsize>(ctx) is true
template<size_t bitsize>
std::pair<oaknut::XReg, oaknut::XReg> InlinePageTableEmitVAddrLookup(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, const SharedLabel& fallback, oaknut::XReg table_reg) {
    const size_t valid_page_index_bits = ctx.conf.page_table_address_space_bits - page_bits;
    const size_t unused_top_bits = 64 - ctx.conf.page_table_address_space_bits;

    const bool has_page_offset_in_scratch1 = EmitDetectMisalignedVAddr<bitsize>(code, ctx, Xaddr, fallback);

    if (ctx.conf.silently_mirror_page_table || unused_top_bits == 0) {
        code.UBFX(Xscratch0, Xaddr, page_bits, valid_page_index_bits);
    } else {
        code.LSR(Xscratch0, Xaddr, page_bits);
        code.TST(Xscratch0, u64(~u64(0)) << valid_page_index_bits);
        code.B(NE, *fallback);
    }

    code.LDR(Xscratch0, table_reg, Xscratch0, LSL, 3);

    if (ctx.conf.page_table_pointer_mask_bits != 0) {
        const u64 mask = u64(~u64(0)) << ctx.conf.page_table_pointer_mask_bits;
        code.AND(Xscratch0, Xscratch0, mask);
    }

    code.CBZ(Xscratch0, *fallback);

    if (ctx.conf.absolute_offset_page_table) {
        return std::make_pair(Xscratch0, Xaddr);
    }
    if (!has_page_offset_in_scratch1) {
        code.AND(Xscratch1, Xaddr, page_mask);
    }
    return std::make_pair(Xscratch0, Xscratch1);
}

template<std::size_t bitsize>
CodePtr EmitMemoryLdr(oaknut::CodeGenerator& code, int value_idx, oaknut::XReg Xbase, oaknut::XReg Xoffset, bool ordered, bool extend32 = false) {
    const auto index_ext = extend32 ? oaknut::IndexExt::UXTW : oaknut::IndexExt::LSL;
    const auto add_ext = extend32 ? oaknut::AddSubExt::UXTW : oaknut::AddSubExt::LSL;
    const auto Roffset = extend32 ? oaknut::RReg{Xoffset.toW()} : oaknut::RReg{Xoffset};

    CodePtr fastmem_location = code.xptr<CodePtr>();

    if (ordered) {
        code.ADD(Xscratch0, Xbase, Roffset, add_ext);

        fastmem_location = code.xptr<CodePtr>();

        switch (bitsize) {
        case 8:
            code.LDARB(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 16:
            code.LDARH(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 32:
            code.LDAR(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 64:
            code.LDAR(oaknut::XReg{value_idx}, Xscratch0);
            break;
        case 128:
            code.LDR(oaknut::QReg{value_idx}, Xscratch0);
            code.DMB(oaknut::BarrierOp::ISH);
            break;
        default:
            ASSERT_FALSE("Invalid bitsize");
        }
    } else {
        fastmem_location = code.xptr<CodePtr>();

        switch (bitsize) {
        case 8:
            code.LDRB(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 16:
            code.LDRH(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 32:
            code.LDR(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 64:
            code.LDR(oaknut::XReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 128:
            code.LDR(oaknut::QReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        default:
            ASSERT_FALSE("Invalid bitsize");
        }
    }

    return fastmem_location;
}

template<std::size_t bitsize>
CodePtr EmitMemoryStr(oaknut::CodeGenerator& code, int value_idx, oaknut::XReg Xbase, oaknut::XReg Xoffset, bool ordered, bool extend32 = false) {
    const auto index_ext = extend32 ? oaknut::IndexExt::UXTW : oaknut::IndexExt::LSL;
    const auto add_ext = extend32 ? oaknut::AddSubExt::UXTW : oaknut::AddSubExt::LSL;
    const auto Roffset = extend32 ? oaknut::RReg{Xoffset.toW()} : oaknut::RReg{Xoffset};

    CodePtr fastmem_location;

    if (ordered) {
        code.ADD(Xscratch0, Xbase, Roffset, add_ext);

        fastmem_location = code.xptr<CodePtr>();

        switch (bitsize) {
        case 8:
            code.STLRB(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 16:
            code.STLRH(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 32:
            code.STLR(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 64:
            code.STLR(oaknut::XReg{value_idx}, Xscratch0);
            break;
        case 128:
            code.DMB(oaknut::BarrierOp::ISH);
            code.STR(oaknut::QReg{value_idx}, Xscratch0);
            code.DMB(oaknut::BarrierOp::ISH);
            break;
        default:
            ASSERT_FALSE("Invalid bitsize");
        }
    } else {
        fastmem_location = code.xptr<CodePtr>();

        switch (bitsize) {
        case 8:
            code.STRB(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 16:
            code.STRH(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 32:
            code.STR(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 64:
            code.STR(oaknut::XReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 128:
            code.STR(oaknut::QReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        default:
            ASSERT_FALSE("Invalid bitsize");
        }
    }

    return fastmem_location;
}

// For a 64-bit access the inline path rejected (it crosses a page, or its page is not in
// `table`): when the address is 4-aligned and both of its words' pages are in the table,
// leaves the host addresses of the low word in Xscratch0 and of the high word minus 4 in
// Xscratch1, so the access can be done as two 32-bit ones (which is what the guest's
// VLDR/VSTR/LDRD/STRD architecturally are). Otherwise branches to `slow` (memory callbacks).
// Returns false, having emitted nothing, unless OptimizationFlag::WideVectorMemory is on (so
// turning it off sends page-crossing 64-bit accesses to the callbacks again), for page-table
// layouts it does not handle, and when misaligned accesses must reach the callbacks anywhere
// (not only at page boundaries).
// Trashes NZCV; only used where the 64-bit lookup already does (page-crossing checks).
bool EmitSplitAccess64(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, oaknut::Label& slow, oaknut::XReg table) {
    if (!ctx.conf.HasOptimization(OptimizationFlag::WideVectorMemory)) {
        return false;
    }
    if (!ctx.conf.absolute_offset_page_table || !ctx.conf.zero_extend_32bit_page_table_address || !ctx.conf.silently_mirror_page_table || (ctx.conf.detect_misaligned_access_via_page_table & 64) == 0 || !ctx.conf.only_detect_misalignment_via_page_table_on_page_boundary) {
        return false;
    }
    const size_t valid_page_index_bits = ctx.conf.page_table_address_space_bits - page_bits;
    const auto mask_entry = [&](oaknut::XReg entry) {
        if (ctx.conf.page_table_pointer_mask_bits != 0) {
            code.AND(entry, entry, u64(~u64(0)) << ctx.conf.page_table_pointer_mask_bits);
        }
    };
    code.TST(Xaddr.toW(), 3);
    code.B(NE, slow);
    code.UBFX(Xscratch0, Xaddr, page_bits, valid_page_index_bits);
    code.LDR(Xscratch0, table, Xscratch0, LSL, 3);
    mask_entry(Xscratch0);
    code.CBZ(Xscratch0, slow);
    code.ADD(Wscratch1, Xaddr.toW(), 4);
    code.CBZ(Wscratch1, slow);  // the high word wraps around to address 0
    code.UBFX(Xscratch1, Xscratch1, page_bits, valid_page_index_bits);
    code.LDR(Xscratch1, table, Xscratch1, LSL, 3);
    mask_entry(Xscratch1);
    code.CBZ(Xscratch1, slow);
    code.ADD(Xscratch0, Xscratch0, oaknut::RReg{Xaddr.toW()}, oaknut::AddSubExt::UXTW);
    code.ADD(Xscratch1, Xscratch1, oaknut::RReg{Xaddr.toW()}, oaknut::AddSubExt::UXTW);
    return true;
}

template<size_t bitsize>
void InlinePageTableEmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = [&] {
        if constexpr (bitsize == 128) {
            return ctx.reg_alloc.WriteQ(inst);
        } else {
            return ctx.reg_alloc.WriteReg<std::max<std::size_t>(bitsize, 32)>(inst);
        }
    }();
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());
    ctx.fpsr.Spill();
    if (InlinePageTableLookupTrashesFlags<bitsize>(ctx) || !ctx.conf.wrapped_memory_preserves_flags || ctx.conf.check_halt_on_memory_access) {
        ctx.reg_alloc.SpillFlags();
    }
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    const auto [Xbase, Xoffset] = InlinePageTableEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback, Xpagetable);
    const bool extend32 = ctx.conf.absolute_offset_page_table && ctx.conf.zero_extend_32bit_page_table_address;
    EmitMemoryLdr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered, extend32);

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fallback, end] {
        code.l(*fallback);
        if constexpr (bitsize == 64) {
            oaknut::Label slow;
            if (!ordered && EmitSplitAccess64(code, ctx, Xaddr, slow, Xpagetable)) {
                code.LDR(Wscratch0, Xscratch0);
                code.LDR(Wscratch1, Xscratch1, 4);
                code.ORR(Rvalue.toX(), Xscratch0, Xscratch1, LSL, 32);
                code.B(*end);
            }
            code.l(slow);
        }
        code.MOV(Xscratch0, Xaddr);
        EmitRelocation(code, ctx, WrappedReadMemoryLinkTarget(bitsize));
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        if constexpr (bitsize == 128) {
            code.MOV(Rvalue.B16(), Q0.B16());
        } else {
            code.MOV(Rvalue.toX(), Xscratch0);
        }
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });

    code.l(*end);
}

template<size_t bitsize>
void InlinePageTableEmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = [&] {
        if constexpr (bitsize == 128) {
            return ctx.reg_alloc.ReadQ(args[2]);
        } else {
            return ctx.reg_alloc.ReadReg<std::max<std::size_t>(bitsize, 32)>(args[2]);
        }
    }();
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());
    ctx.fpsr.Spill();
    if (InlinePageTableLookupTrashesFlags<bitsize>(ctx) || !ctx.conf.wrapped_memory_preserves_flags || ctx.conf.check_halt_on_memory_access) {
        ctx.reg_alloc.SpillFlags();
    }
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    const oaknut::XReg write_table_reg = ctx.conf.write_page_table_pointer != 0 ? Xfastmem : Xpagetable;
    const auto [Xbase, Xoffset] = InlinePageTableEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback, write_table_reg);
    const bool extend32 = ctx.conf.absolute_offset_page_table && ctx.conf.zero_extend_32bit_page_table_address;
    EmitMemoryStr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered, extend32);

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fallback, end] {
        code.l(*fallback);
        if constexpr (bitsize == 64) {
            oaknut::Label slow;
            const oaknut::XReg write_table = ctx.conf.write_page_table_pointer != 0 ? Xfastmem : Xpagetable;
            if (!ordered && EmitSplitAccess64(code, ctx, Xaddr, slow, write_table)) {
                code.STR(Rvalue.toW(), Xscratch0);
                code.LSR(Xscratch0, Rvalue.toX(), 32);
                code.STR(Wscratch0, Xscratch1, 4);
                code.B(*end);
            }
            code.l(slow);
        }
        if constexpr (bitsize == 128) {
            code.MOV(Xscratch0, Xaddr);
            code.MOV(Q0.B16(), Rvalue.B16());
        } else {
            code.MOV(Xscratch0, Xaddr);
            code.MOV(Xscratch1, Rvalue.toX());
        }
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        EmitRelocation(code, ctx, WrappedWriteMemoryLinkTarget(bitsize));
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });

    code.l(*end);
}

std::optional<DoNotFastmemMarker> ShouldFastmem(EmitContext& ctx, IR::Inst* inst) {
    if (!ctx.conf.fastmem_pointer || !ctx.fastmem.SupportsFastmem()) {
        return std::nullopt;
    }

    const auto marker = std::make_tuple(ctx.block.Location(), inst->GetName());
    if (ctx.fastmem.ShouldFastmem(marker)) {
        return marker;
    }
    return std::nullopt;
}

inline bool ShouldExt32(EmitContext& ctx) {
    return ctx.conf.fastmem_address_space_bits == 32 && ctx.conf.silently_mirror_fastmem;
}

// May use Xscratch0 as scratch register
// Address to read/write = [ret0 + ret1], ret0 is always Xfastmem and ret1 is either Xaddr or Xscratch0
// Trashes NZCV
template<size_t bitsize>
std::pair<oaknut::XReg, oaknut::XReg> FastmemEmitVAddrLookup(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, const SharedLabel& fallback) {
    if (ctx.conf.fastmem_address_space_bits == 64 || ShouldExt32(ctx)) {
        return std::make_pair(Xfastmem, Xaddr);
    }

    if (ctx.conf.silently_mirror_fastmem) {
        code.UBFX(Xscratch0, Xaddr, 0, ctx.conf.fastmem_address_space_bits);
        return std::make_pair(Xfastmem, Xscratch0);
    }

    code.LSR(Xscratch0, Xaddr, ctx.conf.fastmem_address_space_bits);
    code.CBNZ(Xscratch0, *fallback);
    return std::make_pair(Xfastmem, Xaddr);
}

template<size_t bitsize>
void FastmemEmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, DoNotFastmemMarker marker) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = [&] {
        if constexpr (bitsize == 128) {
            return ctx.reg_alloc.WriteQ(inst);
        } else {
            return ctx.reg_alloc.WriteReg<std::max<std::size_t>(bitsize, 32)>(inst);
        }
    }();
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());
    ctx.fpsr.Spill();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    const auto [Xbase, Xoffset] = FastmemEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
    const auto fastmem_location = EmitMemoryLdr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered, ShouldExt32(ctx));

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, marker, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fallback, end, fastmem_location] {
        ctx.ebi.fastmem_patch_info.emplace(
            fastmem_location - ctx.ebi.entry_point,
            FastmemPatchInfo{
                .marker = marker,
                .fc = FakeCall{
                    .call_pc = mcl::bit_cast<u64>(code.xptr<void*>()),
                },
                .recompile = ctx.conf.recompile_on_fastmem_failure,
            });

        code.l(*fallback);
        code.MOV(Xscratch0, Xaddr);
        EmitRelocation(code, ctx, WrappedReadMemoryLinkTarget(bitsize));
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        if constexpr (bitsize == 128) {
            code.MOV(Rvalue.B16(), Q0.B16());
        } else {
            code.MOV(Rvalue.toX(), Xscratch0);
        }
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });

    code.l(*end);
}

template<size_t bitsize>
void FastmemEmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, DoNotFastmemMarker marker) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = [&] {
        if constexpr (bitsize == 128) {
            return ctx.reg_alloc.ReadQ(args[2]);
        } else {
            return ctx.reg_alloc.ReadReg<std::max<std::size_t>(bitsize, 32)>(args[2]);
        }
    }();
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());
    ctx.fpsr.Spill();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    const auto [Xbase, Xoffset] = FastmemEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
    const auto fastmem_location = EmitMemoryStr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered, ShouldExt32(ctx));

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, marker, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fallback, end, fastmem_location] {
        ctx.ebi.fastmem_patch_info.emplace(
            fastmem_location - ctx.ebi.entry_point,
            FastmemPatchInfo{
                .marker = marker,
                .fc = FakeCall{
                    .call_pc = mcl::bit_cast<u64>(code.xptr<void*>()),
                },
                .recompile = ctx.conf.recompile_on_fastmem_failure,
            });

        code.l(*fallback);
        if constexpr (bitsize == 128) {
            code.MOV(Xscratch0, Xaddr);
            code.MOV(Q0.B16(), Rvalue.B16());
        } else {
            code.MOV(Xscratch0, Xaddr);
            code.MOV(Xscratch1, Rvalue.toX());
        }
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        EmitRelocation(code, ctx, WrappedWriteMemoryLinkTarget(bitsize));
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });

    code.l(*end);
}

LinkTarget InlineExclusiveReadFallbackLinkTarget(size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::InlineExclusiveReadFallback8;
    case 16:
        return LinkTarget::InlineExclusiveReadFallback16;
    case 32:
        return LinkTarget::InlineExclusiveReadFallback32;
    case 64:
        return LinkTarget::InlineExclusiveReadFallback64;
    }
    UNREACHABLE();
}

LinkTarget InlineExclusiveWriteFallbackLinkTarget(size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::InlineExclusiveWriteFallback8;
    case 16:
        return LinkTarget::InlineExclusiveWriteFallback16;
    case 32:
        return LinkTarget::InlineExclusiveWriteFallback32;
    case 64:
        return LinkTarget::InlineExclusiveWriteFallback64;
    }
    UNREACHABLE();
}

// Inline exclusive accesses (OptimizationFlag::InlineExclusiveMemory, A32 with page tables).
// See ExclusiveMonitor::InlineReservationTable() for the protocol. The load-exclusive
// records (address, value, version) in the JIT state with exclusive_state = 2; the
// store-exclusive locks the address's version word if it still holds that version,
// compare-and-swaps the value, and releases the word with version + 2 on success.
// Pages missing from the page table and misaligned host addresses take the same protocol
// through the inline_exclusive_*_fallback trampolines (memory callbacks).

// Xscratch0 = host address of Xaddr in `table` for an access of `bytes` bytes whose guest and
// host addresses are both naturally aligned; otherwise branches to fallback. Uses Xscratch1.
void EmitExclusiveHostAddress(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, size_t bytes, const SharedLabel& fallback, oaknut::XReg table) {
    const size_t valid_page_index_bits = ctx.conf.page_table_address_space_bits - page_bits;
    code.UBFX(Xscratch0, Xaddr, page_bits, valid_page_index_bits);
    code.LDR(Xscratch0, table, Xscratch0, LSL, 3);
    if (ctx.conf.page_table_pointer_mask_bits != 0) {
        const u64 mask = u64(~u64(0)) << ctx.conf.page_table_pointer_mask_bits;
        code.AND(Xscratch0, Xscratch0, mask);
    }
    code.CBZ(Xscratch0, *fallback);
    if (!ctx.conf.absolute_offset_page_table) {
        code.AND(Xscratch1, Xaddr, page_mask);
        code.ADD(Xscratch0, Xscratch0, Xscratch1);
    } else if (ctx.conf.zero_extend_32bit_page_table_address) {
        code.ADD(Xscratch0, Xscratch0, oaknut::RReg{Xaddr.toW()}, oaknut::AddSubExt::UXTW);
    } else {
        code.ADD(Xscratch0, Xscratch0, Xaddr);
    }
    if (bytes > 1) {
        // Host atomics need natural alignment, and a naturally aligned guest address cannot
        // cross a guest page. A misaligned guest address, or a page whose host backing is
        // not aligned like the guest page, goes through the callbacks.
        code.ORR(Wscratch1, Xaddr.toW(), Wscratch0);
        code.TST(Wscratch1, bytes - 1);
        code.B(NE, *fallback);
    }
}

// Xbucket = &InlineReservationTable()[InlineReservationIndex(Xaddr)]. Uses Xscratch2.
void EmitInlineReservationWord(oaknut::CodeGenerator& code, oaknut::XReg Xbucket, oaknut::XReg Xaddr) {
    static_assert(ExclusiveMonitor::InlineReservationIndex(0x12345678) == ((0x12345678 >> 3) & ((1 << ExclusiveMonitor::INLINE_RESERVATION_TABLE_BITS) - 1)));
    static_assert(sizeof(ExclusiveMonitor::InlineReservationWord) == 8);
    code.LDR(Xbucket, SP, offsetof(StackLayout, inline_reservation_table));
    code.UBFX(Wscratch2, Xaddr.toW(), 3, ExclusiveMonitor::INLINE_RESERVATION_TABLE_BITS);
    code.ADD(Xbucket, Xbucket, Xscratch2, LSL, 3);
}

template<size_t bitsize>
void InlineEmitExclusiveReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    static_assert(bitsize == 8 || bitsize == 16 || bitsize == 32 || bitsize == 64);
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = ctx.reg_alloc.WriteReg<std::max<std::size_t>(bitsize, 32)>(inst);
    ctx.fpsr.Spill();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    EmitExclusiveHostAddress(code, ctx, *Xaddr, bitsize / 8, fallback, Xpagetable);
    EmitInlineReservationWord(code, Xscratch1, *Xaddr);
    code.LDAR(Xscratch1, Xscratch1);  // version first: the value read below is ordered after it
    if constexpr (bitsize == 8) {
        ordered ? code.LDARB(Rvalue->toW(), Xscratch0) : code.LDRB(Rvalue->toW(), Xscratch0);
    } else if constexpr (bitsize == 16) {
        ordered ? code.LDARH(Rvalue->toW(), Xscratch0) : code.LDRH(Rvalue->toW(), Xscratch0);
    } else if constexpr (bitsize == 32) {
        ordered ? code.LDAR(Rvalue->toW(), Xscratch0) : code.LDR(Rvalue->toW(), Xscratch0, 0);
    } else {
        ordered ? code.LDAR(Rvalue->toX(), Xscratch0) : code.LDR(Rvalue->toX(), Xscratch0, 0);
    }
    // A store-exclusive may be holding the word (bit 0): if it succeeds the version moves on
    // and our store-exclusive fails; if not, the word returns to this version.
    code.AND(Xscratch1, Xscratch1, ~u64(1));
    code.STR(Xscratch1, Xstate, ctx.conf.state_exclusive_version_offset);
    code.STR(Rvalue->toX(), Xstate, ctx.conf.state_exclusive_value_offset);
    code.STR(Xaddr->toW(), Xstate, ctx.conf.state_exclusive_address_offset);
    code.MOV(Wscratch1, 2);
    code.STR(Wscratch1, Xstate, ctx.conf.state_exclusive_state_offset);

    ctx.deferred_emits.emplace_back([&code, &ctx, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fallback, end] {
        code.l(*fallback);
        code.MOV(Xscratch0, Xaddr);
        EmitRelocation(code, ctx, InlineExclusiveReadFallbackLinkTarget(bitsize));
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        code.MOV(Rvalue.toX(), Xscratch0);
        code.B(*end);
    });

    code.l(*end);
}

template<size_t bitsize>
void InlineEmitExclusiveWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    static_assert(bitsize == 8 || bitsize == 16 || bitsize == 32 || bitsize == 64);
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    // The inline path is acquire-release (CASAL / LDAXR+STLXR) either way.
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = ctx.reg_alloc.ReadReg<std::max<std::size_t>(bitsize, 32)>(args[2]);
    auto Wstatus = ctx.reg_alloc.WriteW(inst);
    ctx.fpsr.Spill();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue, Wstatus);
    RegAlloc::ScratchGpr version{ctx.reg_alloc}, temp{ctx.reg_alloc};
    const bool lse = ctx.conf.host_has_lse;
    // Without LSE: exclusive load/store pairs, retried a few times. A store-exclusive may fail
    // even without contention; when other cores keep taking the line, give up and report a
    // failed store-exclusive (allowed, the guest retries) rather than spin here. The value
    // store gets fewer tries because the reservation word is locked meanwhile.
    constexpr u32 lock_attempts = 4;
    constexpr u32 store_attempts = 2;
    std::optional<RegAlloc::ScratchGpr> attempts;
    if (!lse) {
        attempts.emplace(ctx.reg_alloc);
    }

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel(), lock_failed = GenSharedLabel();
    oaknut::Label unlock;

    // Status 1 unless the store happens. The reservation is used up either way.
    code.MOV(*Wstatus, 1);
    code.LDR(Wscratch0, Xstate, ctx.conf.state_exclusive_state_offset);
    code.STR(WZR, Xstate, ctx.conf.state_exclusive_state_offset);
    code.CMP(Wscratch0, 2);
    code.B(NE, *end);
    code.LDR(Wscratch0, Xstate, ctx.conf.state_exclusive_address_offset);
    code.CMP(Wscratch0, Xaddr->toW());
    code.B(NE, *end);

    const oaknut::XReg write_table = ctx.conf.write_page_table_pointer != 0 ? Xfastmem : Xpagetable;
    EmitExclusiveHostAddress(code, ctx, *Xaddr, bitsize / 8, fallback, write_table);
    EmitInlineReservationWord(code, Xscratch1, *Xaddr);

    // Lock the reservation word: version -> version | 1, only if it still holds our version.
    // A plain load first lets a stale reservation fail without taking the cache line
    // exclusively (contended words otherwise bounce between cores on every attempt).
    code.LDR(version.X(), Xstate, ctx.conf.state_exclusive_version_offset);
    code.LDR(temp.X(), Xscratch1);
    code.CMP(temp.X(), version.X());
    code.B(NE, *end);
    code.ADD(Xscratch2, version.X(), 1);
    if (lse) {
        code.MOV(temp.X(), version.X());
        code.CASA(temp.X(), Xscratch2, Xscratch1);
        code.CMP(temp.X(), version.X());
        code.B(NE, *end);
    } else {
        oaknut::Label retry, locked;
        code.MOV(attempts->W(), lock_attempts);
        code.l(retry);
        code.LDAXR(temp.X(), Xscratch1);
        code.CMP(temp.X(), version.X());
        code.B(NE, *lock_failed);
        code.STXR(temp.W(), Xscratch2, Xscratch1);
        code.CBZ(temp.W(), locked);
        code.SUBS(attempts->W(), attempts->W(), 1);
        code.B(NE, retry);
        code.B(*end);
        code.l(locked);
    }

    // Compare-and-swap the value against the one the load-exclusive saw. Only the low
    // bitsize bits count (the load-exclusive may have been wider).
    code.LDR(temp.X(), Xstate, ctx.conf.state_exclusive_value_offset);
    if (lse) {
        code.MOV(Xscratch2, temp.X());
        if constexpr (bitsize == 8) {
            code.CASALB(temp.W(), Rvalue->toW(), Xscratch0);
            code.CMP(temp.W(), Wscratch2, oaknut::AddSubExt::UXTB);
        } else if constexpr (bitsize == 16) {
            code.CASALH(temp.W(), Rvalue->toW(), Xscratch0);
            code.CMP(temp.W(), Wscratch2, oaknut::AddSubExt::UXTH);
        } else if constexpr (bitsize == 32) {
            code.CASAL(temp.W(), Rvalue->toW(), Xscratch0);
            code.CMP(temp.W(), Wscratch2);
        } else {
            code.CASAL(temp.X(), Rvalue->toX(), Xscratch0);
            code.CMP(temp.X(), Xscratch2);
        }
        code.CSET(*Wstatus, NE);
    } else {
        oaknut::Label retry, mismatch;
        code.MOV(attempts->W(), store_attempts);
        code.l(retry);
        if constexpr (bitsize == 8) {
            code.LDAXRB(Wscratch2, Xscratch0);
            code.CMP(Wscratch2, temp.W(), oaknut::AddSubExt::UXTB);
        } else if constexpr (bitsize == 16) {
            code.LDAXRH(Wscratch2, Xscratch0);
            code.CMP(Wscratch2, temp.W(), oaknut::AddSubExt::UXTH);
        } else if constexpr (bitsize == 32) {
            code.LDAXR(Wscratch2, Xscratch0);
            code.CMP(Wscratch2, temp.W());
        } else {
            code.LDAXR(Xscratch2, Xscratch0);
            code.CMP(Xscratch2, temp.X());
        }
        code.B(NE, mismatch);
        if constexpr (bitsize == 8) {
            code.STLXRB(*Wstatus, Rvalue->toW(), Xscratch0);
        } else if constexpr (bitsize == 16) {
            code.STLXRH(*Wstatus, Rvalue->toW(), Xscratch0);
        } else if constexpr (bitsize == 32) {
            code.STLXR(*Wstatus, Rvalue->toW(), Xscratch0);
        } else {
            code.STLXR(*Wstatus, Rvalue->toX(), Xscratch0);
        }
        code.CBZ(*Wstatus, unlock);
        code.SUBS(attempts->W(), attempts->W(), 1);
        code.B(NE, retry);
        code.B(unlock);  // Wstatus is 1: kept losing the line
        code.l(mismatch);
        code.CLREX();
        code.MOV(*Wstatus, 1);
    }

    // Release the word: version + 2 if the store happened, back to version otherwise.
    code.l(unlock);
    code.ADD(Xscratch2, version.X(), 2);
    code.CMP(*Wstatus, 0);
    code.CSEL(Xscratch2, Xscratch2, version.X(), EQ);
    code.STLR(Xscratch2, Xscratch1);

    ctx.deferred_emits.emplace_back([&code, &ctx, Xaddr = *Xaddr, Rvalue = *Rvalue, Wstatus = *Wstatus, lse, ordered, fallback, end, lock_failed] {
        code.l(*fallback);
        code.MOV(Xscratch0, Xaddr);
        code.MOV(Xscratch1, Rvalue.toX());
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        EmitRelocation(code, ctx, InlineExclusiveWriteFallbackLinkTarget(bitsize));
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        code.MOV(Wstatus, Wscratch0);
        code.B(*end);

        if (!lse) {
            code.l(*lock_failed);
            code.CLREX();
            code.B(*end);
        }
    });

    code.l(*end);
}

}  // namespace

template<size_t bitsize>
void EmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if (const auto marker = ShouldFastmem(ctx, inst)) {
        FastmemEmitReadMemory<bitsize>(code, ctx, inst, *marker);
    } else if (ctx.conf.page_table_pointer != 0) {
        InlinePageTableEmitReadMemory<bitsize>(code, ctx, inst);
    } else {
        CallbackOnlyEmitReadMemory<bitsize>(code, ctx, inst);
    }
}

template<size_t bitsize>
void EmitExclusiveReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if constexpr (bitsize <= 64) {
        if (ctx.conf.inline_exclusive_memory) {
            InlineEmitExclusiveReadMemory<bitsize>(code, ctx, inst);
            return;
        }
    }
    CallbackOnlyEmitExclusiveReadMemory<bitsize>(code, ctx, inst);
}

template<size_t bitsize>
void EmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if (const auto marker = ShouldFastmem(ctx, inst)) {
        FastmemEmitWriteMemory<bitsize>(code, ctx, inst, *marker);
    } else if (ctx.conf.page_table_pointer != 0) {
        InlinePageTableEmitWriteMemory<bitsize>(code, ctx, inst);
    } else {
        CallbackOnlyEmitWriteMemory<bitsize>(code, ctx, inst);
    }
}

template<size_t bitsize>
void EmitExclusiveWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if constexpr (bitsize <= 64) {
        if (ctx.conf.inline_exclusive_memory) {
            InlineEmitExclusiveWriteMemory<bitsize>(code, ctx, inst);
            return;
        }
    }
    CallbackOnlyEmitExclusiveWriteMemory<bitsize>(code, ctx, inst);
}

// Memory runs: PageRangeLookup* checks a whole run against the page table once and
// returns the host address of its first byte (0 when the run leaves its page or the page
// is not mapped there). Each *MemoryHosted* access is then one load or store at a fixed
// offset from that address; when it is 0, the access takes the ordinary inline page-table
// path for its own guest address (falling back to the memory callbacks), so a run that
// crosses a page or touches an unmapped page behaves exactly like separate accesses.
template<bool is_write>
void EmitPageRangeLookup(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    ASSERT(ctx.conf.page_table_pointer != 0 && !ctx.conf.fastmem_pointer && ctx.conf.silently_mirror_page_table);

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u64 size = args[1].GetImmediateU32();
    ASSERT(size >= 1 && size <= page_size);

    auto Xaddr = ctx.reg_alloc.ReadX(args[0]);
    auto Xhost = ctx.reg_alloc.WriteX(inst);
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Xhost);

    const oaknut::XReg table = is_write && ctx.conf.write_page_table_pointer != 0 ? Xfastmem : Xpagetable;
    const size_t valid_page_index_bits = ctx.conf.page_table_address_space_bits - page_bits;

    code.UBFX(Xscratch0, Xaddr, page_bits, valid_page_index_bits);
    code.LDR(Xscratch0, table, Xscratch0, LSL, 3);
    if (ctx.conf.page_table_pointer_mask_bits != 0) {
        const u64 mask = u64(~u64(0)) << ctx.conf.page_table_pointer_mask_bits;
        code.AND(Xscratch0, Xscratch0, mask);
    }
    code.AND(Xscratch1, Xaddr, page_mask);
    if (!ctx.conf.absolute_offset_page_table) {
        code.ADD(*Xhost, Xscratch0, Xscratch1);
    } else if (ctx.conf.zero_extend_32bit_page_table_address) {
        code.ADD(*Xhost, Xscratch0, oaknut::RReg{Xaddr->toW()}, oaknut::AddSubExt::UXTW);
    } else {
        code.ADD(*Xhost, Xscratch0, Xaddr);
    }
    // host = (page offset <= page_size - size && entry != 0) ? host : 0
    code.CMP(Xscratch1, page_size - size);
    code.CCMP(Xscratch0, 0, 0b0100, LS);
    code.CSEL(Xhost, Xhost, XZR, NE);
}

namespace {

// [Xhost + offset] for a hosted access of `bytes` bytes. May use Xscratch0.
std::pair<oaknut::XReg, u32> HostedAddress(oaknut::CodeGenerator& code, oaknut::XReg Xhost, u32 offset, size_t bytes) {
    if (offset % bytes == 0 && offset / bytes < 4096) {
        return {Xhost, offset};
    }
    code.ADD(Xscratch0, Xhost, offset);
    return {Xscratch0, 0};
}

}  // namespace

template<size_t bitsize>
void EmitReadMemoryHosted(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    static_assert(bitsize == 32 || bitsize == 64);
    constexpr size_t bytes = bitsize / 8;

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 offset = args[2].GetImmediateU32();
    const bool to_vector = args[3].GetImmediateU1();
    ASSERT(offset < 4096);

    auto Xhost = ctx.reg_alloc.ReadX(args[0]);
    auto Xstart = ctx.reg_alloc.ReadX(args[1]);
    ctx.fpsr.Spill();
    // The fallback's page-boundary check trashes NZCV.
    ctx.reg_alloc.SpillFlags();

    // value_index: register number; is_vector: S/D register instead of W/X.
    const auto emit = [&](int value_index, bool is_vector) {
        SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

        code.CBZ(*Xhost, *fallback);
        {
            const auto [Xbase, imm] = HostedAddress(code, *Xhost, offset, bytes);
            if (is_vector) {
                if constexpr (bitsize == 64) {
                    code.LDR(oaknut::DReg{value_index}, Xbase, imm);
                } else {
                    code.LDR(oaknut::SReg{value_index}, Xbase, imm);
                }
            } else {
                if constexpr (bitsize == 64) {
                    code.LDR(oaknut::XReg{value_index}, Xbase, imm);
                } else {
                    code.LDR(oaknut::WReg{value_index}, Xbase, imm);
                }
            }
        }

        ctx.deferred_emits.emplace_back([&code, &ctx, Xstart = *Xstart, value_index, is_vector, offset, fallback, end] {
            SharedLabel callback = GenSharedLabel();

            code.l(*fallback);
            // Xscratch2 = guest address of this access; the ordinary inline path follows.
            code.ADD(Wscratch2, Xstart.toW(), offset);
            const auto [Xbase, Xoffset] = InlinePageTableEmitVAddrLookup<bitsize>(code, ctx, Xscratch2, callback, Xpagetable);
            const bool extend32 = ctx.conf.absolute_offset_page_table && ctx.conf.zero_extend_32bit_page_table_address;
            if (is_vector) {
                const auto index_ext = extend32 ? oaknut::IndexExt::UXTW : oaknut::IndexExt::LSL;
                const auto Roffset = extend32 ? oaknut::RReg{Xoffset.toW()} : oaknut::RReg{Xoffset};
                if constexpr (bitsize == 64) {
                    code.LDR(oaknut::DReg{value_index}, Xbase, Roffset, index_ext);
                } else {
                    code.LDR(oaknut::SReg{value_index}, Xbase, Roffset, index_ext);
                }
            } else {
                EmitMemoryLdr<bitsize>(code, value_index, Xbase, Xoffset, false, extend32);
            }
            code.B(*end);

            code.l(*callback);
            if constexpr (bitsize == 64) {
                oaknut::Label slow;
                if (EmitSplitAccess64(code, ctx, Xscratch2, slow, Xpagetable)) {
                    code.LDR(Wscratch0, Xscratch0);
                    code.LDR(Wscratch1, Xscratch1, 4);
                    if (is_vector) {
                        code.ORR(Xscratch0, Xscratch0, Xscratch1, LSL, 32);
                        code.FMOV(oaknut::DReg{value_index}, Xscratch0);
                    } else {
                        code.ORR(oaknut::XReg{value_index}, Xscratch0, Xscratch1, LSL, 32);
                    }
                    code.B(*end);
                }
                code.l(slow);
            }
            code.MOV(Xscratch0, Xscratch2);
            EmitRelocation(code, ctx, WrappedReadMemoryLinkTarget(bitsize));
            if (is_vector) {
                if constexpr (bitsize == 64) {
                    code.FMOV(oaknut::DReg{value_index}, Xscratch0);
                } else {
                    code.FMOV(oaknut::SReg{value_index}, Wscratch0);
                }
            } else {
                code.MOV(oaknut::XReg{value_index}, Xscratch0);
            }
            code.B(*end);
        });

        code.l(*end);
    };

    if (to_vector) {
        if constexpr (bitsize == 64) {
            auto Dvalue = ctx.reg_alloc.WriteD(inst);
            RegAlloc::Realize(Xhost, Xstart, Dvalue);
            emit(Dvalue->index(), true);
        } else {
            auto Svalue = ctx.reg_alloc.WriteS(inst);
            RegAlloc::Realize(Xhost, Xstart, Svalue);
            emit(Svalue->index(), true);
        }
    } else {
        auto Rvalue = ctx.reg_alloc.WriteReg<bitsize>(inst);
        RegAlloc::Realize(Xhost, Xstart, Rvalue);
        emit(Rvalue->index(), false);
    }
}

template<size_t bitsize>
void EmitWriteMemoryHosted(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    static_assert(bitsize == 32 || bitsize == 64);
    constexpr size_t bytes = bitsize / 8;

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 offset = args[2].GetImmediateU32();
    ASSERT(offset < 4096);
    const bool from_vector = args[3].IsInFpr();

    auto Xhost = ctx.reg_alloc.ReadX(args[0]);
    auto Xstart = ctx.reg_alloc.ReadX(args[1]);
    ctx.fpsr.Spill();
    // The fallback's page-boundary check trashes NZCV.
    ctx.reg_alloc.SpillFlags();

    const auto emit = [&](int value_index, bool is_vector) {
        SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

        code.CBZ(*Xhost, *fallback);
        {
            const auto [Xbase, imm] = HostedAddress(code, *Xhost, offset, bytes);
            if (is_vector) {
                if constexpr (bitsize == 64) {
                    code.STR(oaknut::DReg{value_index}, Xbase, imm);
                } else {
                    code.STR(oaknut::SReg{value_index}, Xbase, imm);
                }
            } else {
                if constexpr (bitsize == 64) {
                    code.STR(oaknut::XReg{value_index}, Xbase, imm);
                } else {
                    code.STR(oaknut::WReg{value_index}, Xbase, imm);
                }
            }
        }

        ctx.deferred_emits.emplace_back([&code, &ctx, Xstart = *Xstart, value_index, is_vector, offset, fallback, end] {
            SharedLabel callback = GenSharedLabel();

            code.l(*fallback);
            // Xscratch2 = guest address of this access; the ordinary inline path follows.
            code.ADD(Wscratch2, Xstart.toW(), offset);
            const oaknut::XReg write_table_reg = ctx.conf.write_page_table_pointer != 0 ? Xfastmem : Xpagetable;
            const auto [Xbase, Xoffset] = InlinePageTableEmitVAddrLookup<bitsize>(code, ctx, Xscratch2, callback, write_table_reg);
            const bool extend32 = ctx.conf.absolute_offset_page_table && ctx.conf.zero_extend_32bit_page_table_address;
            if (is_vector) {
                const auto index_ext = extend32 ? oaknut::IndexExt::UXTW : oaknut::IndexExt::LSL;
                const auto Roffset = extend32 ? oaknut::RReg{Xoffset.toW()} : oaknut::RReg{Xoffset};
                if constexpr (bitsize == 64) {
                    code.STR(oaknut::DReg{value_index}, Xbase, Roffset, index_ext);
                } else {
                    code.STR(oaknut::SReg{value_index}, Xbase, Roffset, index_ext);
                }
            } else {
                EmitMemoryStr<bitsize>(code, value_index, Xbase, Xoffset, false, extend32);
            }
            code.B(*end);

            code.l(*callback);
            if constexpr (bitsize == 64) {
                oaknut::Label slow;
                const oaknut::XReg split_table = ctx.conf.write_page_table_pointer != 0 ? Xfastmem : Xpagetable;
                if (EmitSplitAccess64(code, ctx, Xscratch2, slow, split_table)) {
                    if (is_vector) {
                        code.STR(oaknut::SReg{value_index}, Xscratch0);
                        code.UMOV(Wscratch0, oaknut::QReg{value_index}.Selem()[1]);
                    } else {
                        code.STR(oaknut::WReg{value_index}, Xscratch0);
                        code.LSR(Xscratch0, oaknut::XReg{value_index}, 32);
                    }
                    code.STR(Wscratch0, Xscratch1, 4);
                    code.B(*end);
                }
                code.l(slow);
            }
            code.MOV(Xscratch0, Xscratch2);
            if (is_vector) {
                if constexpr (bitsize == 64) {
                    code.FMOV(Xscratch1, oaknut::DReg{value_index});
                } else {
                    code.FMOV(Wscratch1, oaknut::SReg{value_index});
                }
            } else {
                code.MOV(Xscratch1, oaknut::XReg{value_index});
            }
            EmitRelocation(code, ctx, WrappedWriteMemoryLinkTarget(bitsize));
            code.B(*end);
        });

        code.l(*end);
    };

    if (from_vector) {
        if constexpr (bitsize == 64) {
            auto Dvalue = ctx.reg_alloc.ReadD(args[3]);
            RegAlloc::Realize(Xhost, Xstart, Dvalue);
            emit(Dvalue->index(), true);
        } else {
            auto Svalue = ctx.reg_alloc.ReadS(args[3]);
            RegAlloc::Realize(Xhost, Xstart, Svalue);
            emit(Svalue->index(), true);
        }
    } else {
        auto Rvalue = ctx.reg_alloc.ReadReg<bitsize>(args[3]);
        RegAlloc::Realize(Xhost, Xstart, Rvalue);
        emit(Rvalue->index(), false);
    }
}

template void EmitPageRangeLookup<false>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitPageRangeLookup<true>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemoryHosted<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemoryHosted<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemoryHosted<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemoryHosted<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);

template void EmitReadMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);

}  // namespace Dynarmic::Backend::Arm64
