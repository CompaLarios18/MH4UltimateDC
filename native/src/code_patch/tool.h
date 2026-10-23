// The command line of mh4n_code_patch (N3-09):
//
//     mh4n_code_patch --mods DIR --bss-pages N IN OUT
//
// Applies the mods folder DIR (the folder of one program id: exefs/code.bin, code.ips, ...) to the code file
// IN as the loader does: a replacement, N pages of 0x1000 zero bytes (the bss) appended, the first patch;
// OUT is the code with its bss pages. N is decimal or 0x-hexadecimal.
// Exit status: 0 applied, or nothing to apply (OUT is IN plus the pages); 1 a bad command line; 2 a patch
// error (`mods: <file>: patch error` and its detail on stderr, OUT not written); 3 a file that cannot be
// read or written.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace mh4n::code_patch {

int RunTool(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace mh4n::code_patch
