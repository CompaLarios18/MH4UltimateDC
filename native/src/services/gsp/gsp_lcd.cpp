// Derived from Azahar src/core/hle/service/gsp/gsp_lcd.cpp, src/core/hle/service/gsp/gsp_lcd.h and
// src/core/hle/service/gsp/gsp.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later:
// each of them says "GPLv2 or any later version"): the gsp::Lcd service and its table of command
// names, none of which Azahar implements.
// Copyright 2015 Citra Emulator Project (gsp_lcd.cpp, gsp_lcd.h)
// Copyright 2017 Citra Emulator Project (gsp.cpp)
//
// The gsp::Lcd service (N1-17): every command is known by name and implemented by none, as in
// Azahar, so each is logged once as unimplemented and answered the way Azahar answers it. The game
// does not open this service; it exists so that one that does gets a session.
#include <memory>

#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n::gsp {

namespace {

class GspLcd final : public Service {
public:
    explicit GspLcd(ServiceContext& context) : Service(context, "gsp::Lcd") {
        Register(0x000A, "SetBrightnessRaw");
        Register(0x000B, "SetBrightness");
        Register(0x000F, "PowerOnAllBacklights");
        Register(0x0010, "PowerOffAllBacklights");
        Register(0x0011, "PowerOnBacklight");
        Register(0x0012, "PowerOffBacklight");
        Register(0x0013, "SetLedForceOff");
        Register(0x0014, "GetVendor");
        Register(0x0015, "GetBrightness");
    }
};

// ServiceFramework("gsp::Lcd") takes the default number of sessions.
MH4N_REGISTER(ServiceFactory, "gsp::Lcd",
              ServiceFactory([](ServiceContext& context) { return std::make_unique<GspLcd>(context); }));

}  // namespace

}  // namespace mh4n::gsp
