#include "gpu/backend/core/policy.h"

#include <algorithm>
#include <array>
#include <cctype>

#include <fmt/format.h>

namespace mh4n::gpu::rhi {

namespace {

constexpr std::array<std::string_view, kCapCount> kCapNames = {
    "d24s8",    "interlock",         "index_uint8",      "custom_border_color", "triangle_fans",
    "logic_op", "dynamic_rendering", "pipeline_library", "eds3_blend",          "bc7",
    "timeline", "present_wait",      "direct_upload",    "geometry_shader",
};

std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

std::string ValidNames() {
    std::string out;
    for (std::string_view name : kCapNames) {
        out += out.empty() ? "" : ", ";
        out += name;
    }
    return out + ", all";
}

} // namespace

std::string_view CapName(Cap cap) {
    return kCapNames[static_cast<size_t>(cap)];
}

Result<CapabilityMask> CapabilityMask::Parse(std::string_view list) {
    CapabilityMask mask;
    size_t begin = 0;
    while (begin <= list.size()) {
        size_t end = list.find(',', begin);
        if (end == std::string_view::npos) {
            end = list.size();
        }
        const std::string name = Lower(Trim(list.substr(begin, end - begin)));
        begin = end + 1;
        if (name.empty()) {
            continue;
        }
        if (name == "all") {
            mask = mask.Union(All());
            continue;
        }
        const auto it = std::find(kCapNames.begin(), kCapNames.end(), name);
        if (it == kCapNames.end()) {
            return Error{fmt::format("unknown capability \"{}\" in the mask; valid names: {}", name, ValidNames())};
        }
        mask = mask.With(static_cast<Cap>(it - kCapNames.begin()));
    }
    return mask;
}

std::string CapabilityMask::ToString() const {
    if (*this == All()) {
        return "all";
    }
    std::string out;
    for (u32 i = 0; i < kCapCount; ++i) {
        if (Has(static_cast<Cap>(i))) {
            out += out.empty() ? "" : ",";
            out += kCapNames[i];
        }
    }
    return out;
}

} // namespace mh4n::gpu::rhi

namespace mh4n::gpu::rhi::core {

const char* DeviceTypeName(DeviceType type) {
    switch (type) {
    case DeviceType::Discrete: return "discrete";
    case DeviceType::Integrated: return "integrated";
    case DeviceType::Virtual: return "virtual";
    case DeviceType::Cpu: return "cpu";
    case DeviceType::Other: break;
    }
    return "other";
}

std::string DeviceCandidate::Label() const {
    return fmt::format("{} ({})", name, driver_name);
}

std::string DeviceCandidate::Ineligible(bool surface) const {
    if (!has_graphics_queue) {
        return "no graphics queue";
    }
    if (!fragment_stores_and_atomics) {
        return "no fragmentStoresAndAtomics (the shadow write has no fallback)";
    }
    if (surface && !can_present) {
        return "no queue that can present to the surface";
    }
    return {};
}

namespace {

std::string ListDevices(std::span<const DeviceCandidate> candidates) {
    std::string out;
    for (const DeviceCandidate& c : candidates) {
        out += fmt::format("\n  {}: {} [{}]", c.index, c.Label(), DeviceTypeName(c.type));
    }
    return out;
}

} // namespace

DeviceChoice ChooseDevice(std::span<const DeviceCandidate> candidates, std::string_view override_text, bool surface) {
    DeviceChoice choice;
    if (candidates.empty()) {
        choice.error = "the Vulkan loader lists no device";
        return choice;
    }

    const DeviceCandidate* chosen = nullptr;
    std::string why;
    if (!override_text.empty()) {
        const std::string wanted = Lower(override_text);
        for (const DeviceCandidate& c : candidates) {
            if (Lower(c.Label()).find(wanted) != std::string::npos) {
                chosen = &c;
                break;
            }
        }
        if (chosen == nullptr) {
            choice.error = fmt::format("no device matches \"{}\"; devices:{}", override_text, ListDevices(candidates));
            return choice;
        }
        if (const std::string reason = chosen->Ineligible(surface); !reason.empty()) {
            choice.error = fmt::format("device {} ({}) matches \"{}\" but cannot be used: {}; devices:{}",
                                       chosen->index, chosen->Label(), override_text, reason, ListDevices(candidates));
            return choice;
        }
        why = fmt::format("chosen: its label contains \"{}\"", override_text);
    } else {
        for (const DeviceCandidate& c : candidates) {
            if (!c.Ineligible(surface).empty()) {
                continue;
            }
            const bool better = chosen == nullptr || c.type > chosen->type ||
                                (c.type == chosen->type && c.capability_bits > chosen->capability_bits);
            if (better) {
                chosen = &c; // candidates are in index order, so the lowest index wins a full tie
            }
        }
        if (chosen == nullptr) {
            std::string reasons;
            for (const DeviceCandidate& c : candidates) {
                reasons += fmt::format("\n  {}: {}: {}", c.index, c.Label(), c.Ineligible(surface));
            }
            choice.error = "no device can be used:" + reasons;
            return choice;
        }
        why = fmt::format("chosen: {} device with {} capability bits", DeviceTypeName(chosen->type),
                          chosen->capability_bits);
    }

    for (const DeviceCandidate& c : candidates) {
        if (&c == chosen) {
            choice.log.push_back(
                fmt::format("device {}: {} [{}]: {}", c.index, c.Label(), DeviceTypeName(c.type), why));
        } else if (const std::string reason = c.Ineligible(surface); !reason.empty()) {
            choice.log.push_back(
                fmt::format("device {}: {} [{}]: not chosen: {}", c.index, c.Label(), DeviceTypeName(c.type), reason));
        } else if (!override_text.empty()) {
            choice.log.push_back(fmt::format("device {}: {} [{}]: not chosen: the override picked another", c.index,
                                             c.Label(), DeviceTypeName(c.type)));
        } else if (c.type != chosen->type) {
            choice.log.push_back(fmt::format("device {}: {} [{}]: not chosen: a {} device is preferred to a {} one",
                                             c.index, c.Label(), DeviceTypeName(c.type), DeviceTypeName(chosen->type),
                                             DeviceTypeName(c.type)));
        } else if (c.capability_bits != chosen->capability_bits) {
            choice.log.push_back(fmt::format("device {}: {} [{}]: not chosen: {} capability bits against {}", c.index,
                                             c.Label(), DeviceTypeName(c.type), c.capability_bits,
                                             chosen->capability_bits));
        } else {
            choice.log.push_back(fmt::format("device {}: {} [{}]: not chosen: the lower index wins a tie", c.index,
                                             c.Label(), DeviceTypeName(c.type)));
        }
    }
    choice.ok = true;
    choice.index = chosen->index;
    return choice;
}

std::string JoinNames(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        out += out.empty() ? "" : ", ";
        out += name;
    }
    return out;
}

bool IsAppleDriver(u32 driver_id, std::string_view driver_name, u32 vendor_id) {
    std::string lower(driver_name);
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return driver_id == kDriverIdMoltenVk || driver_id == kDriverIdKosmicKrispUnverified ||
           lower.find("kosmickrisp") != std::string::npos || lower.find("moltenvk") != std::string::npos ||
           vendor_id == kVendorIdApple;
}

bool PresentWaitAvailable(bool has_surface, bool present_wait_bit, bool features_enabled) {
    return has_surface && present_wait_bit && features_enabled;
}

bool PresentWaitMeasuresDisplay(bool present_wait_available, u32 driver_id, std::string_view driver_name) {
    if (!present_wait_available) {
        return false;
    }
    // MoltenVK releases the wait at command-buffer completion, not at display (7.3); KosmicKrisp's
    // semantics are unverified (4 R3): neither is taken to measure display.
    return !IsAppleDriver(driver_id, driver_name, 0);
}

Result<CapabilityMask> CombineMasks(const char* environment_text, CapabilityMask option_mask) {
    CapabilityMask mask = option_mask;
    if (environment_text != nullptr && *environment_text != '\0') {
        Result<CapabilityMask> parsed = CapabilityMask::Parse(environment_text);
        if (!parsed.ok()) {
            return Error{"MH4N_RHI_MASK: " + parsed.error()};
        }
        mask = mask.Union(*parsed);
    }
    return mask;
}

} // namespace mh4n::gpu::rhi::core
