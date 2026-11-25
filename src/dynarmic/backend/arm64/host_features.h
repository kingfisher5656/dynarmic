/* This file is part of the dynarmic project.
 * Copyright (c) 2026 the LiveExec32 contributors
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

namespace Dynarmic::Backend::Arm64 {

/// Optional instructions the JIT may emit on this host, detected once.
/// DYNARMIC_HOST_NO_LSE=1 in the environment forces the ARMv8.0 code paths
/// (Apple A9/A10 have no LSE atomics) so they can be tested on newer hosts.
struct HostFeatures {
    bool lse = false;
};

const HostFeatures& GetHostFeatures();

}  // namespace Dynarmic::Backend::Arm64
