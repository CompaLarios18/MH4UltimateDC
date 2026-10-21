// What the replay can say about a difference without printing a name or a byte of the game (N1-12c):
//  - a result code taken apart into the fields of Azahar's Result (description, module, summary, level),
//    with the names Azahar gives them where it has one;
//  - for the reply to a directory Read, whether the buffer's CRC differs because the entries differ or
//    only because they came in another order.
// Every string these functions return holds numbers, counts, sizes and fixed phrases and nothing else:
// no name of an entry, no byte of the buffer. docs/native/ipc-replay.md ("Diagnostics") is the manual.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::ipc_replay {

// "description 180 (FileAlreadyExists), module 17 (FS), summary 1 (NothingHappened), level 25 (Status)".
// A name is left out (and the number stays) for a value Azahar has no name for. Descriptions are named for
// the FS module (Azahar's errors.h) and for the common ones of 1000 and above (result.h).
std::string DescribeResult(u32 code);

// The directory entry the 3DS returns from a directory Read (Azahar FileSys::Entry): 0x228 bytes, the
// UTF-16 name first (0x20C bytes), the directory flag at 0x21C.
inline constexpr u32 kListingEntrySize = 0x228;

// Whether a request is the Read of a directory session whose output buffer holds a listing: the session is
// named `<service>/dir#<k>` (the kind the services give a directory session), the command id is 0x0801 and
// the buffer (kind `mapped-w`, `size` bytes) is a whole number of entries.
bool IsListingRead(const std::string& session, u32 request_header, const std::string& buffer_kind, u32 size);

// The orders `DiagnoseListing` tries the native's entries in, by the name it reports them under.
std::vector<std::string> ListingOrderNames();

// What the native wrote into the output buffer of a directory Read, against the recorded reply.
//   buffer           the whole buffer as it is after the reply (`size` bytes, the entries first, then what
//                    the service did not write)
//   native_entries   the entry count of the native's reply; none if the reply has no count word
//   recorded_entries the entry count of the recorded reply; none if it has no count word
//   recorded_crc     the CRC-32 of the recorded buffer
//   kept_tail        the replay kept what earlier requests left after the entries (ReplayConfig::
//                    keep_output_buffers); false: it wrote zeros there. Only the wording depends on it.
// The lines say: the native's entry count, the size it wrote and the size of the buffer, the recorded
// count and size, how many bytes at the end of the buffer the service did not write, and the verdict:
// "the recorded CRC equals the native's entries in order <name>" (only the order differs), or that no
// tried order reproduces it (the content differs). Each line starts with "listing: ".
std::vector<std::string> DiagnoseListing(const std::vector<u8>& buffer, std::optional<u32> native_entries,
                                         std::optional<u32> recorded_entries, u32 recorded_crc,
                                         bool kept_tail = false);

}  // namespace mh4n::ipc_replay
