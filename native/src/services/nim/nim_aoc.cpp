// Derived from Azahar src/core/hle/service/nim/nim_aoc.cpp and src/core/hle/service/nim/nim.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the name of the service, its session limit and its command table.
// Copyright 2014 Citra Emulator Project (nim_aoc.cpp)
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (nim.cpp)
//
// nim:aoc (N1-19): the add-on content service of the eShop. Azahar registers it with two sessions
// and no implemented command: every entry of its table is a nullptr, so the game's requests are
// answered as an unknown command (logged once as `unimplemented nim:aoc <header>`; the command id,
// one normal parameter, the result 0). The game sends 16 commands to it, 8 of them in Azahar's
// table by name; the rest have no name in Azahar and are answered the same way.
#include "mh4n/ipc.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {

namespace {

class NimAoc final : public Service {
public:
    explicit NimAoc(ServiceContext& context) : Service(context, "nim:aoc") {
        Register(0x0003, "SetApplicationId");
        Register(0x0004, "SetTin");
        Register(0x0009, "ListContentSetsEx");
        Register(0x0018, "GetBalance");
        Register(0x001D, "GetCustomerSupportCode");
        Register(0x0021, "Initialize");
        Register(0x0024, "CalculateContentsRequiredSize");
        Register(0x0025, "RefreshServerTime");
    }
};

MH4N_REGISTER(ServiceFactory, "nim:aoc",
              ServiceFactory([](ServiceContext& context) { return std::make_unique<NimAoc>(context); },
                             2));

}  // namespace

}  // namespace mh4n
