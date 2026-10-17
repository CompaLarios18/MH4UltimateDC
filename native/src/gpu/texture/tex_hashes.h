// `--check tex-hashes` (N2-14): the custom-texture hashes of the textures a capture's draws use.
//
// The check runs the capture's command lists through its own PICA command processor
// (gpu/pica), on the capture host's memory, and at every draw hashes the textures the enabled
// units name, in both modes of hash.h. It writes one list when the capture ends (option
// `--tex-hashes-out FILE`; the default is `<capture>.tex-hashes.txt`):
//
//     # mh4n tex-hashes 1
//     # use_new_hash=1
//     <hash> <w>x<h> <format>        one line per distinct texture, in order of first use
//     # use_new_hash=0
//     <hash> <w>x<h> <format>
//
// <hash> is 16 upper-case hex digits (the form of the file names in a pack), <format> the number
// of the texture format (0-13). tools/native/hd_hash_check.py reads this file.
//
// The memory a draw is hashed from is the Gpu's memory after the command list was executed and
// judged (the check sees a command when the player has finished it), so a texture a list draws
// into is hashed as it is after the list.
#pragma once

#include <string>
#include <vector>

#include "gpu/texture/formats.h"

namespace mh4n::gpu::texture {

struct TextureHashLine {
    u64 hash = 0;
    u32 width = 0;
    u32 height = 0;
    Format format = Format::RGBA8;

    friend bool operator==(const TextureHashLine&, const TextureHashLine&) = default;
};

// The text of the list above, for the textures in `new_mode` and `old_mode` (already distinct).
std::string FormatTexHashes(const std::vector<TextureHashLine>& new_mode,
                            const std::vector<TextureHashLine>& old_mode);

} // namespace mh4n::gpu::texture
