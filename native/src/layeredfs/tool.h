// The command line of mh4n_layeredfs (N3-10):
//
//     mh4n_layeredfs BASE OFFSET SIZE MODS_DIR OUT
//
// Lays MODS_DIR/romfs and MODS_DIR/romfs_ext over the RomFS image that is SIZE bytes of the file BASE from
// OFFSET (a level-3 image, starting at its 0x28-byte header; SIZE 0 means to the end of the file), and writes
// the whole layered image to OUT. Numbers are decimal or 0x-hexadecimal. Exit status 0 on success, 1 on a
// bad command line, 2 when the image cannot be built or written. It is what the oracle of
// tools/native/layeredfs_oracle compares with Azahar's own code.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace mh4n::layeredfs {

int RunTool(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace mh4n::layeredfs
