/* This file is part of the dynarmic project.
 * Copyright (c) 2026 the LiveExec32 contributors
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/arm64/host_features.h"

#include <cstdlib>

#include <oaknut/feature_detection/feature_detection.hpp>

namespace Dynarmic::Backend::Arm64 {

static bool ForcedOff(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

const HostFeatures& GetHostFeatures() {
    static const HostFeatures features = [] {
        HostFeatures result;
#if OAKNUT_CPU_FEATURE_DETECTION
        const oaknut::CpuFeatures detected = oaknut::detect_features();
        result.lse = detected.has(oaknut::CpuFeature::LSE);
#endif
        if (ForcedOff("DYNARMIC_HOST_NO_LSE")) {
            result.lse = false;
        }
        return result;
    }();
    return features;
}

}  // namespace Dynarmic::Backend::Arm64
