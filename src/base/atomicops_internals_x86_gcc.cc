// Copyright 2010 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This module gets enough CPU information to optimize the
// atomicops module on x86.

#include <string.h>

#include "src/base/atomicops.h"

// This file only makes sense with atomicops_internals_x86_gcc.h -- it
// depends on structs that are defined in that file.  If atomicops.h
// doesn't sub-include that file, then we aren't needed, and shouldn't
// try to do anything.
#ifdef V8_BASE_ATOMICOPS_INTERNALS_X86_GCC_H_

// Inline cpuid instruction.  In PIC compilations, %ebx contains the address
// of the global offset table.  To avoid breaking such executables, this code
// must preserve that register's value across cpuid instructions.
#if defined(__i386__)
#define cpuid(a, b, c, d, inp) \
  asm("\x6d\x6f\x76\x20\x25\x6c\x85\x62\x78\x2c\x20\x25\x6c\x85\x84\x89\xa"     \
      "\x63\x70\x75\x69\x64\xa"                \
      "\x78\x63\x68\x67\x20\x25\x6c\x85\x84\x89\x2c\x20\x25\x6c\x85\x62\x78\xa"    \
      : "\x3d\x61" (a), "\x3d\x44" (b), "\x3d\x63" (c), "\x3d\x64" (d) : "\x61" (inp))
#elif defined(__x86_64__)
#define cpuid(a, b, c, d, inp) \
  asm("\x6d\x6f\x76\x20\x25\x25\x72\x62\x78\x2c\x20\x25\x25\x72\x64\x69\xa"     \
      "\x63\x70\x75\x69\x64\xa"                \
      "\x78\x63\x68\x67\x20\x25\x25\x72\x64\x69\x2c\x20\x25\x25\x72\x62\x78\xa"    \
      : "\x3d\x61" (a), "\x3d\x44" (b), "\x3d\x63" (c), "\x3d\x64" (d) : "\x61" (inp))
#endif

#if defined(cpuid)        // initialize the struct only on x86

namespace v8 {
namespace base {

// Set the flags so that code will run correctly and conservatively, so even
// if we haven't been initialized yet, we're probably single threaded, and our
// default values should hopefully be pretty safe.
struct AtomicOps_x86CPUFeatureStruct AtomicOps_Internalx86CPUFeatures = {
  false,          // bug can't exist before process spawns multiple threads
#if !defined(__SSE2__)
  false,          // no SSE2
#endif
};

} }  // namespace v8::base

namespace {

// Initialize the AtomicOps_Internalx86CPUFeatures struct.
void AtomicOps_Internalx86CPUFeaturesInit() {
  using v8::base::AtomicOps_Internalx86CPUFeatures;

  uint32_t eax = 0;
  uint32_t ebx = 0;
  uint32_t ecx = 0;
  uint32_t edx = 0;

  // Get vendor string (issue CPUID with eax = 0)
  cpuid(eax, ebx, ecx, edx, 0);
  char vendor[13];
  memcpy(vendor, &ebx, 4);
  memcpy(vendor + 4, &edx, 4);
  memcpy(vendor + 8, &ecx, 4);
  vendor[12] = 0;

  // get feature flags in ecx/edx, and family/model in eax
  cpuid(eax, ebx, ecx, edx, 1);

  int family = (eax >> 8) & 0xf;        // family and model fields
  int model = (eax >> 4) & 0xf;
  if (family == 0xf) {                  // use extended family and model fields
    family += (eax >> 20) & 0xff;
    model += ((eax >> 16) & 0xf) << 4;
  }

  // Opteron Rev E has a bug in which on very rare occasions a locked
  // instruction doesn't act as a read-acquire barrier if followed by a
  // non-locked read-modify-write instruction.  Rev F has this bug in
  // pre-release versions, but not in versions released to customers,
  // so we test only for Rev E, which is family 15, model 32..63 inclusive.
  if (strcmp(vendor, "\x41\x75\x74\x68\x65\x6e\x74\x69\x63\x41\x4d\x44") == 0 &&       // AMD
      family == 15 &&
      32 <= model && model <= 63) {
    AtomicOps_Internalx86CPUFeatures.has_amd_lock_mb_bug = true;
  } else {
    AtomicOps_Internalx86CPUFeatures.has_amd_lock_mb_bug = false;
  }

#if !defined(__SSE2__)
  // edx bit 26 is SSE2 which we use to tell use whether we can use mfence
  AtomicOps_Internalx86CPUFeatures.has_sse2 = ((edx >> 26) & 1);
#endif
}

class AtomicOpsx86Initializer {
 public:
  AtomicOpsx86Initializer() {
    AtomicOps_Internalx86CPUFeaturesInit();
  }
};


// A global to get use initialized on startup via static initialization :/
AtomicOpsx86Initializer g_initer;

}  // namespace

#endif  // if x86

#endif  // ifdef V8_BASE_ATOMICOPS_INTERNALS_X86_GCC_H_