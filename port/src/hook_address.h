// Where a hook point is in a code library, by its name in config/hooks.txt. Kept apart from
// hooks.cpp (which needs Azahar) so the rule is tested on its own (tests/test_hook_address.py).
#pragma once

#include <string_view>

extern "C" {
#include "recomp.h"
}

namespace Port::Hooks {

/// Where the hook point `name` is in this library, or 0 if it isn't one: the libraries name their
/// hook points, so another version of the game (EUR, whose code sits a little further along) finds
/// them by name, and a name the library lacks is missing, not at USA's address. Only a library
/// without names (built before they were listed) is USA v1.3.0's, where it is at `usa_addr`.
inline u32 FindHookPoint(const RCodeInfo& info, const char* name, u32 usa_addr) {
    if (!info.hook_names) {
        return usa_addr;
    }
    for (u32 i = 0; i < info.hook_count; ++i) {
        if (info.hook_names[i] && std::string_view(info.hook_names[i]) == name) {
            return info.hook_addrs[i];
        }
    }
    return 0;
}

} // namespace Port::Hooks
