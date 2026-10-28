// The update's own code, read from a .cia or from the program Azahar installed from one, and
// which update it is by that code: the SHA-1 of the decompressed code, the way
// tools/build_game.py tells a version (config/versions.json). Some tools that decrypt or rebuild
// a .cia reset the version label of its title metadata (to v0.0.1 or v1.0.0) and leave the
// content as it was, so the label alone would turn away a good v1.3.0 update (issue #47).
//
// No Azahar here (the caller gives a function that reads the file, launcher/checks.cpp Azahar's
// own files), so the rules are tested on their own (tests/test_update_code.py).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Launcher::UpdateCode {

/// v1.3.0, the update the port is made for (config/versions.json: title_version, code_sha1).
constexpr std::uint16_t TARGET_VERSION = 1072;
constexpr const char* TARGET_CODE_SHA1 = "c9225f87ca3f65e8537484d029b08b8b04c3717d";
/// Europe's v1.1.0, built from the same tables (config/versions.json, config/shifts/eur_v110.txt).
constexpr std::uint16_t EUR_VERSION = 1040;
constexpr const char* EUR_CODE_SHA1 = "0a927cb1a59d90c8c806fed091b3755cd518b3f8";

/// Reads `size` bytes at `offset` of a file into `out`; false when they aren't all there.
using Read = std::function<bool(std::uint64_t offset, void* out, std::size_t size)>;

/// "1.3.0" for a title version (major in bits 10-15, minor in 4-9, micro in 0-3).
std::string VersionText(std::uint16_t version);

/// The SHA-1 of `size` bytes, in lower-case hex.
std::string Sha1(const std::uint8_t* data, std::size_t size);

/// Undoes the compression of an ExeFS .code (BLZ, a backward LZ77) as tools/blz.py does; false
/// when `in` isn't that.
bool Decompress(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out);

/// A CIA's title metadata, and where its content 0 (the program) is in the file.
struct Cia {
    bool ok = false;
    std::string error;          ///< why it couldn't be read
    std::uint64_t title_id = 0;
    std::uint16_t version = 0;  ///< the version label
    std::uint64_t content_offset = 0;
    std::uint64_t content_size = 0;
    bool encrypted = false;     ///< content 0 is encrypted with the title key
};

/// Reads a CIA's header and title metadata as tools/extract_ncch.py does.
Cia ReadCia(const Read& read);

/// A program's code: its ExeFS .code, decompressed when its exheader says so.
struct Code {
    bool ok = false;
    std::string error;  ///< why it couldn't be read
    std::string sha1;
};

/// The code of the NCCH (a program) that starts at `base` in the file.
Code ReadCode(const Read& read, std::uint64_t base);

/// The code of a CIA's content 0.
Code ReadCiaCode(const Read& read);

/// An update by its version label and, when that isn't the target's (v1.3.0, or Europe's v1.1.0
/// for its game), by its code.
enum class Verdict {
    Target,        ///< labelled as the target (its code isn't read, as before)
    Relabeled,     ///< labelled otherwise, with the target's code: the label was reset
    OtherVersion,  ///< labelled otherwise, with other code
    Unchecked,     ///< labelled otherwise, and the code couldn't be read
};

/// What the setup does with a .cia of MH4U's update: installed when `problem` is empty (`note`,
/// if any, said once it is in), else not, for that reason under `title`.
struct CiaCheck {
    Verdict verdict = Verdict::Target;
    std::string title;
    std::string problem;
    std::string note;
};

/// A .cia whose title metadata says `label`; `read_code` reads its code, and is called only
/// when the label isn't the target's (a v1.3.0 label is taken as it is, as before this check).
/// The target is v1.3.0 unless given (Europe's: EUR_CODE_SHA1, EUR_VERSION).
CiaCheck CheckCia(std::uint16_t label, const std::function<Code()>& read_code,
                  const std::string& target_sha1 = TARGET_CODE_SHA1,
                  std::uint16_t target_version = TARGET_VERSION);

/// What the setup says of the update installed in Azahar's virtual SD card: `ok` when it is
/// used as v1.3.0, `note` the setup's line about it ("" for a v1.3.0 label).
struct InstalledCheck {
    Verdict verdict = Verdict::Target;
    bool ok = true;
    std::string note;
};

/// The installed update, whose title metadata (Azahar's copy of the .cia's) says `label`;
/// `read_code` reads the installed program's code, only when the label isn't the target's.
InstalledCheck CheckInstalled(std::uint16_t label, const std::function<Code()>& read_code,
                              const std::string& target_sha1 = TARGET_CODE_SHA1,
                              std::uint16_t target_version = TARGET_VERSION);

} // namespace Launcher::UpdateCode
