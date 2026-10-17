// The command line of mh4n_hdpack (N2-17): the logic of native/tools/hdpack.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace mh4n::hdpack {

// `mh4n_hdpack PACK_DIR -o OUT [--threads N] [--quality fast|normal|high] [--restart] [--verify]`
// Exit status: 0 the archive is written; 2 bad arguments or an unusable pack; 3 a write failed;
// 4 interrupted (run it again to continue).
int RunHdPack(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

} // namespace mh4n::hdpack
