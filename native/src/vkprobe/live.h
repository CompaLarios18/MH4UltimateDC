// The live probe (N2-24): loads the Vulkan loader at run time, makes an instance and reads what every
// device reports. No Vulkan SDK and no linking against a loader: the declarations come from the
// vendored vulkan_core.h (native/third_party/vulkan), the entry points from the loader library.
#pragma once

#include <string>
#include <vector>

#include "vkprobe/facts.h"

namespace mh4n::vkprobe {

// The loader libraries to try, in order, on this platform: vulkan-1.dll on Windows; libvulkan.so.1
// and libvulkan.so on Linux; on macOS libvulkan.1.dylib, libvulkan.dylib and the MoltenVK dylib, bare
// and under the Homebrew and /usr/local prefixes.
std::vector<std::string> DefaultLoaderNames();

// Loads the first library of `loader_names` (UTF-8 names or paths) that opens and has
// vkGetInstanceProcAddr, makes an instance and fills `devices` with one Facts per physical device
// (software ones included). False, with a message, when no loader loads, the instance cannot be made
// or there is no device. The library stays loaded until the process ends (drivers do not always
// survive being unloaded).
bool ProbeLive(const std::vector<std::string>& loader_names, std::vector<Facts>* devices, std::string* error);

}  // namespace mh4n::vkprobe
