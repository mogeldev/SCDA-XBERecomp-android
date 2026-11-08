// Host test: Turnip eligibility follows Mesa's dev_id_compare for KGSL chip ids (turnip_detect.h).
#include <cassert>
#include <cstdio>

#include "android/turnip_detect.h"

int main() {
    using turnip::Matches;
    assert(Matches(0x07010000));   // Adreno 710, exact KGSL entry (tested phone)
    assert(!Matches(0x07010001));  // unknown 710 patch: Mesa has no wildcard for it
    assert(Matches(0x43020000));   // 720
    assert(Matches(0x43020100));   // 722
    assert(Matches(0x06030001));   // 630 via gpu_id entry, any patch
    assert(Matches(0x06040001));   // 640
    assert(Matches(0x43050a01));   // 740
    assert(Matches(0x43051401));   // 750
    assert(Matches(0x44050001));   // 830 (A8xx)
    assert(Matches(0x44010000));   // 810 via fuse-wildcard entry
    assert(!Matches(0x05030002));  // 530: A5xx is not Turnip
    assert(!Matches(0x03000620));  // 306
    assert(!Matches(0x07090000));  // not in the table
    assert(!Matches(0));
    std::puts("PASS: Turnip device matching");
}
