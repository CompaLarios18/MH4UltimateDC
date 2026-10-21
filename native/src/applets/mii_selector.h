// Derived from Azahar src/core/hle/applets/mii_selector.h, src/core/hle/applets/mii_selector.cpp,
// src/core/frontend/applets/mii_selector.cpp and src/core/hle/mii.h, src/core/hle/mii.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the layouts of the configuration (0x104 bytes), the result (0x84 bytes) and the Mii data
// (0x5C bytes, with its CRC16), the applet's behaviour, the default selector (the frontend: the
// standard Mii it answers with) and the standard Mii's values.
// Copyright 2016 Citra Emulator Project (hle/applets/mii_selector.h, .cpp)
// Copyright 2018 Citra Emulator Project (frontend/applets/mii_selector.cpp)
// Copyright 2023 Citra Emulator Project (hle/mii.h, hle/mii.cpp)
//
// The Mii selector (N1-14c, decision D43), HLE as in Azahar for the ids Ed1 (0x202) and Ed2 (0x402),
// answering as Azahar's default selector does: nobody chooses. PrepareToStartLibraryApplet makes the
// applet, the Request parameter gets the Response with a shared memory block of the framebuffer's
// size, and StartLibraryApplet's parameter is the 0x104-byte configuration (not looked at, but for
// its size). At the next update tick (4,468,352 ticks after the applet was made) the applet closes
// with the 0x84-byte result: return code 0, no guest Mii, and the standard Mii that Azahar answers
// with (the data of an LLE Mii picker, system version 11.8.0), its CRC16 included.
//
// Departures from Azahar: as the error applet's (a Request parameter of the wrong size, and here a
// configuration of the wrong size, are logged and answered with kResultUnknown instead of an
// assertion, at warning level with the size seen and expected: the applet then does not start, so the
// applet never closes).
//
// Threads (principle 6): scheduler thread only.
#pragma once

#include <array>
#include <cstddef>
#include <memory>

#include "applets/applet.h"

namespace mh4n::apt {

inline constexpr std::size_t kMiiConfigSize = 0x104;
inline constexpr std::size_t kMiiDataSize = 0x5C;
inline constexpr std::size_t kChecksummedMiiSize = 0x60;  // the data, 2 bytes of padding, the CRC16
inline constexpr std::size_t kMiiResultSize = 0x84;

namespace miiresult {
// MiiResult: three big-endian words, the checksummed Mii, the guest Mii's name.
inline constexpr std::size_t kReturnCode = 0x00;
inline constexpr std::size_t kIsGuestMiiSelected = 0x04;
inline constexpr std::size_t kSelectedGuestMiiIndex = 0x08;
inline constexpr std::size_t kSelectedMiiData = 0x0C;
inline constexpr std::size_t kGuestMiiName = 0x6C;
}  // namespace miiresult

// CRC-16 with the polynomial 0x1021, initial value 0, no reflection and no final xor (what Azahar's
// boost::crc<16, 0x1021, 0, 0, false, false> computes, 3dbrew's Mii checksum).
u16 MiiCrc16(const u8* data, std::size_t size);

// The result Azahar's default selector answers with, byte by byte (MiiSelector::GetStandardMiiResult).
std::array<u8, kMiiResultSize> StandardMiiResult();

class MiiSelector final : public Applet {
public:
    MiiSelector(AppletId id, AppletId parent, std::weak_ptr<AppletHost> host, ServiceContext& context)
        : Applet(id, parent, std::move(host)), context_(context) {}

    Result ReceiveParameterImpl(const MessageParameter& parameter) override;
    Result Start(const MessageParameter& parameter) override;
    Result Finalize() override;
    void Update() override;

private:
    ServiceContext& context_;
    // Made when the Request message arrives (the framebuffer info of ImportDisplayCaptureInfo).
    std::shared_ptr<SharedMemory> framebuffer_memory_;
    std::array<u8, kMiiResultSize> result_{};
};

}  // namespace mh4n::apt
