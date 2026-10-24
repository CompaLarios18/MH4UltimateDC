// The pure rules of the core (N2-26), kept apart from Vulkan so that tests can run them on synthetic
// values: the order in which devices are preferred, the override by text, the present-wait rule, the
// Apple test.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gpu/backend/rhi.h"

namespace mh4n::gpu::rhi::core {

// The device types in the order the choice prefers them (9.4 item 12): discrete before integrated before
// virtual before CPU.
enum class DeviceType : u8 { Other, Cpu, Virtual, Integrated, Discrete };

const char* DeviceTypeName(DeviceType type);

// What the choice needs to know about one physical device.
struct DeviceCandidate {
    u32 index = 0;  // in the order of vkEnumeratePhysicalDevices on the instance
    std::string name;
    std::string driver_name;
    DeviceType type = DeviceType::Other;
    bool has_graphics_queue = false;
    bool fragment_stores_and_atomics = false;  // the shadow write has no fallback without it (5.1)
    bool can_present = true;                   // some graphics queue family can present to the surface (true when there is none)
    u32 capability_bits = 0;                   // how many capability bits the device has after the mask

    // "<device name> (<driver name>)": the text MH4N_VULKAN_DEVICE and BackendOptions::device match.
    std::string Label() const;
    // Empty when the device may be chosen, else why not.
    std::string Ineligible(bool surface) const;
};

struct DeviceChoice {
    bool ok = false;
    u32 index = 0;                 // the chosen candidate's `index`
    std::string error;             // when !ok
    std::vector<std::string> log;  // one line per candidate: chosen or not, and why
};

// Among the devices with a graphics queue and fragmentStoresAndAtomics (and, with a surface, a queue that
// can present): discrete before integrated before virtual before CPU, then the most capability bits, then
// the lowest index. A non-empty `override_text` picks the first device (in index order) whose Label()
// contains it, ignoring case; it fails, listing the devices, when none does or when the one that does
// cannot be used.
DeviceChoice ChooseDevice(std::span<const DeviceCandidate> candidates, std::string_view override_text, bool surface);

// MoltenVK's driver id (14) or KosmicKrisp's (28; unverified: the id is not in the v1.3.296 headers, so
// the driver's name is tested too) or Apple's vendor id (0x106B).
bool IsAppleDriver(u32 driver_id, std::string_view driver_name, u32 vendor_id);

// PresentWaitAvailable() and a driver whose wait is released at display time: false for MoltenVK
// (7.3) and for KosmicKrisp (unverified); the other drivers are taken to measure display (inference).
bool PresentWaitMeasuresDisplay(bool present_wait_available, u32 driver_id, std::string_view driver_name);

// The mask from MH4N_RHI_MASK's text (null or empty: none) and the option's mask, as one.
Result<CapabilityMask> CombineMasks(const char* environment_text, CapabilityMask option_mask);

// "a, b, c"
std::string JoinNames(const std::vector<std::string>& names);

inline constexpr u32 kDriverIdMoltenVk = 14;
inline constexpr u32 kDriverIdKosmicKrispUnverified = 28;
inline constexpr u32 kVendorIdApple = 0x106B;

}  // namespace mh4n::gpu::rhi::core
