// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include "format.hpp"
namespace mxcore_fp4 {
struct Profile { unsigned m, n, k, cycles, first, count; };
// Each transfer: delta cycles [31:16], region [15:13], byte offset / 32 [12:0].
// Generated only after RTL results pass the independent arithmetic oracle.
#include "profiles.inc"
struct OutputProfile { unsigned format; Profile timing; };
#include "output_profiles.inc"
inline const Profile *find_profile(unsigned m, unsigned n, unsigned k) {
    for (const auto &p : profiles) if (p.m==m && p.n==n && p.k==k) return &p;
    return nullptr;
}
inline const Profile *find_output_profile(unsigned m, unsigned n, unsigned k,
                                         OutputFormat format, const uint32_t *&stream) {
    if (format==OutputFormat::MXFP4) {
        if (const Profile *p=find_profile(m,n,k)) { stream=transfers; return p; }
    }
    for (const auto &p : output_profiles)
        if (p.format==unsigned(format) && p.timing.m==m && p.timing.n==n && p.timing.k==k) {
            stream=output_transfers; return &p.timing;
        }
    return nullptr;
}
}
