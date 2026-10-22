// The command line of mh4n_vkprobe (N2-24); native/tools/vkprobe holds only main.
#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace mh4n::vkprobe {

// The largest file --from-json reads (a `vulkaninfo --json` file is a few MB).
inline constexpr size_t kMaxInputBytes = 16u * 1024 * 1024;

// Exit status: 0 done; 2 a bad command line, or a file that is missing, truncated or not JSON;
// 3 the live probe found no Vulkan loader or no device.
int RunVkProbe(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
               size_t max_input_bytes = kMaxInputBytes);

}  // namespace mh4n::vkprobe
