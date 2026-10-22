// Derived from Azahar src/core/file_sys/patch.cpp and src/core/file_sys/patch.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: both say "GPLv2 or any later version"): the
// IPS and BPS patch formats as Azahar applies them to a file of the RomFS (Patch::ApplyIpsPatch and
// Patch::ApplyBpsPatch).
// Copyright 2019-2026 Citra Emulator Project / Azahar Emulator Project (patch.cpp, patch.h)
//
// IPS and BPS patches (N3-10). The rules are Azahar's, byte for byte on every patch Azahar's code handles
// without undefined behaviour:
//  - IPS: the header `PATCH`; records of a 3-byte offset, a 2-byte length and the bytes, or, with a length
//    of 0, an RLE record (a 2-byte count and one byte); the marker `EOF` ends it, and so does the end of the
//    patch (the last 3 bytes are not read as a record). A record that ends past the end of the buffer is an
//    error: the buffer never grows. Azahar's code does not read the "truncate" size some IPS files put
//    after `EOF`, and neither does this.
//  - BPS: the header `BPS1`, variable-length numbers, no metadata, the commands SourceRead, TargetRead,
//    SourceCopy and TargetCopy, and the three CRC-32 of the footer (source, target, patch). Azahar checks the
//    source and target CRC but not the patch's own; neither does this. A target larger than the buffer
//    makes the buffer larger; a smaller one leaves the buffer's size as it is, with zeros after the target.
// Departure from Azahar: a patch that Azahar's code reads past its end (a truncated record, a BPS shorter
// than its header and footer, a TargetCopy that starts before the target) is undefined behaviour there and
// an error here.
#pragma once

#include <vector>

#include "mh4n/types.h"

namespace mh4n::layeredfs::patch {

// Azahar's Loader::ResultStatus values that the two functions return.
enum class Status {
    Success,
    ErrorPatches,              // a malformed patch, or one that does not fit the buffer
    ErrorPatchesInvalidTitle,  // BPS: the source's CRC-32 is not the one the patch was made for
};

Status ApplyIps(const std::vector<u8>& ips, std::vector<u8>& buffer);
Status ApplyBps(const std::vector<u8>& bps, std::vector<u8>& buffer);

}  // namespace mh4n::layeredfs::patch
