// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
namespace mxcore_fp4 {
struct Profile { unsigned m, n, k, cycles, first, count; };
// Each transfer: delta cycles [31:16], region [15:13], byte offset / 32 [12:0].
// Generated only after RTL results pass the independent arithmetic oracle.
#include "profiles.inc"
inline const Profile *find_profile(unsigned m, unsigned n, unsigned k) {
    for (const auto &p : profiles) if (p.m==m && p.n==n && p.k==k) return &p;
    return nullptr;
}
}
