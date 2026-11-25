/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <atomic>

#include "dynarmic/backend/arm64/address_space.h"
#include "dynarmic/backend/block_range_information.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace Dynarmic::Backend::Arm64 {

struct EmittedBlockInfo;

class A32AddressSpace final : public AddressSpace {
public:
    explicit A32AddressSpace(const A32::UserConfig& conf);

    IR::Block GenerateIR(IR::LocationDescriptor) const override;

    void InvalidateCacheRanges(const boost::icl::interval_set<u32>& ranges);

protected:
    friend class A32Core;

    void EmitPrelude();
    EmitConfig GetEmitConfig() override;
    void RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo& block_info) override;

    /// OptimizationFlag::InlineExclusiveMemory is on and usable with this configuration.
    bool InlineExclusiveMemory() const;

    const A32::UserConfig conf;
    BlockRangeInformation<u32> block_ranges;

public:
    /// What the inline exclusive access slow paths need (a32_address_space.cpp).
    struct InlineExclusiveFallbackContext {
        A32::UserCallbacks* callbacks;
        ExclusiveMonitor::InlineReservationWord* reservations;
    };

private:
    InlineExclusiveFallbackContext inline_exclusive_context{};
};

}  // namespace Dynarmic::Backend::Arm64
