// Derived from Azahar src/core/hle/service/apt/apt.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: it says "GPLv2 or any later version"): the block's name, size, region and
// permissions are the ones of the Module constructor.
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project
//
// The shared font memory block of the APT module (N1-14b).
#pragma once

#include <memory>

#include "mh4n/kernel.h"
#include "mh4n/service.h"

namespace mh4n::apt {

// "APT:SharedFont": 0x332000 bytes (3272 KB).
inline constexpr u32 kSharedFontSize = 0x332000;

// Makes the block as Azahar's APT::Module does: from the SYSTEM region (linear, first fit), ReadWrite
// for the owner (none) and Read for the others. Azahar unwraps the result and so asserts when the
// region is full; here the error is returned (kResultOutOfMemory) and *out is left empty.
Result AllocateSharedFontBlock(Kernel& kernel, std::shared_ptr<SharedMemory>* out);

// The block the module of this service context holds (nullptr if it could not be made).
std::shared_ptr<SharedMemory> SharedFontBlock(ServiceContext& context);

}  // namespace mh4n::apt
