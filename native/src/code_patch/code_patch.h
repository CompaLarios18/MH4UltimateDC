// Code patches and exheader overrides from the mods folder (N3-09, D72): what the native loader (N3-29)
// does to a game's code and exheader, in Azahar's order, with the files of
// <user>/load/mods/<program id>/.
//
//  1. Replacement: `exefs/code.bin`, else `code.bin`, takes the place of the game's code (before the bss
//     pages are appended). A file that cannot be read in full is skipped, as Azahar skips it.
//  2. The bss pages (the exheader's bss size rounded up to 0x1000, in zeros) are appended.
//  3. Patch: the first of `exefs/code.ips`, `exefs/code.bps`, `code.ips`, `code.bps` that exists is applied
//     to the code with its bss pages; no other is tried. A patch that cannot be read, is malformed, does not
//     fit the buffer (an IPS record past its end) or is for another code (a BPS CRC-32) is an error.
//     The IPS and BPS rules are mh4n::layeredfs::patch's (D78 lists where they depart from Azahar).
//     Luma's sdmc/luma/titles/<id>/code.ips and the `<game file>.exefsdir` paths are not looked at (D72).
//  4. `exheader.bin` (at least 0x800 bytes: its first 0x800 are used) replaces the game's exheader.
// The loader ends the load with exit status 2 and the error's text, `mods: <file>: patch error`, on an error.
//
// Deviation from the work package's text: ApplyToCode takes the code *without* the bss pages and appends
// them itself, because a replacement code.bin comes before the pages (Azahar's order) and the size of the
// buffer the loader holds is then not the one the pages belong to.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::code_patch {

inline constexpr std::size_t kExheaderSize = 0x800;  // sizeof(ExHeader_Header), Azahar's static_assert
inline constexpr std::size_t kPageSize = 0x1000;

struct Outcome {
    bool ok = true;
    // The error (when !ok): `mods: <file>: patch error`, <file> relative to the mods folder (no host path).
    std::string error;
    // Why the patch was refused, for the log: "malformed", "source CRC-32 differs", "unreadable".
    std::string detail;
    // What was applied, for the log, relative to the mods folder: e.g. "exefs/code.bin", "code.ips".
    std::vector<std::string> applied;
};

// The mods folder of a title under <load dir> (Azahar's `mods/<GetModId>/`): an update's program id
// (high half 0x0004000E) counts as the base game's (0x00040000).
u64 ModId(u64 program_id);
std::string ModsPath(const std::string& load_dir, u64 program_id);

// The bss size of the exheader as pages (Azahar: (bss_size + 0xFFF) & ~0xFFF).
u32 BssPages(u32 bss_size);

class ModFolder {
public:
    explicit ModFolder(std::string path);
    // `code`: the game's code, ro and data without the bss pages. On success it is the code the program runs
    // with `bss_pages` pages of zeros appended and the patch applied; on an error its contents are not
    // defined.
    Outcome ApplyToCode(std::vector<u8>& code, u32 bss_pages) const;
    // exheader.bin, when it holds a whole exheader.
    std::optional<std::vector<u8>> ExheaderOverride() const;

private:
    std::string path_;
};

}  // namespace mh4n::code_patch
