// What other modules read of the console's configuration (N1-15): the software keyboard's default
// text (the user name, N1-14), the console unique ID and the MAC address that the local wireless
// service sends (UDS), the language and region the services answer with. They read the same state the
// cfg: services answer from, on the scheduler thread. Include as "../cfg/cfg_api.h"; the header should
// move to native/include/mh4n/cfg.h (proposed in the N1-15 report).
#pragma once

#include <array>
#include <string>

#include "mh4n/service.h"

namespace mh4n::cfg {

// The user name (config block 0x000A0000), at most 10 UTF-16 units; Azahar's default is "AZAHAR".
std::u16string GetUserName(ServiceContext& context);
// The console unique ID (config block 0x00090001): the friend code seed in the low 34 bits and the
// random number in bits 48-63. Azahar's `Module::GetConsoleUniqueId`.
u64 GetConsoleUniqueId(ServiceContext& context);
// The region the cfg: services report (0 JPN, 1 USA, 2 EUR, 3 AUS, 4 CHN, 5 KOR, 6 TWN) and the
// system language (0 JP, 1 EN, ...) after the region adjusted it, as Azahar's GetRegionValue and
// GetSystemLanguage do.
u32 GetRegion(ServiceContext& context);
u8 GetSystemLanguage(ServiceContext& context);
// The console's MAC address (sysdata/mac.txt of the user folder; one is made and kept when there is
// none: the one place the cfg module writes outside a command that saves).
std::array<u8, 6> GetMacAddress(ServiceContext& context);

}  // namespace mh4n::cfg
