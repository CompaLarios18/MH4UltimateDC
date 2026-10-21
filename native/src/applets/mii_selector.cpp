// Derived from Azahar src/core/hle/applets/mii_selector.cpp, src/core/frontend/applets/mii_selector.cpp
// and src/core/hle/mii.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of
// them says "GPLv2 or any later version"): MiiSelector::ReceiveParameterImpl, Start, Update,
// Finalize and GetStandardMiiResult, DefaultMiiSelector::Setup, the layout of Mii::MiiData and its
// checksum.
// Copyright 2016 Citra Emulator Project (hle/applets/mii_selector.cpp)
// Copyright 2018 Citra Emulator Project (frontend/applets/mii_selector.cpp)
// Copyright 2023 Citra Emulator Project (hle/mii.h)
#include "applets/mii_selector.h"

#include <cstring>

#include "mh4n/log.h"

namespace mh4n::apt {

namespace {
constexpr const char* kLog = "Service.APT";

void PutBe16(u8* p, u32 v) {
    p[0] = static_cast<u8>(v >> 8), p[1] = static_cast<u8>(v);
}
void PutBe32(u8* p, u32 v) {
    PutBe16(p, v >> 16), PutBe16(p + 2, v & 0xFFFF);
}
void PutLe16(u8* p, u32 v) {
    p[0] = static_cast<u8>(v), p[1] = static_cast<u8>(v >> 8);
}
}  // namespace

u16 MiiCrc16(const u8* data, std::size_t size) {
    u32 crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<u32>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) != 0 ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return static_cast<u16>(crc);
}

std::array<u8, kMiiResultSize> StandardMiiResult() {
    // This data was obtained (by Azahar) by writing the returned buffer in AppletManager::
    // GlanceParameter of the LLEd Mii picker of system version 11.8.0 to a file and then matching
    // the values to the members of the MiiResult struct. Mii::MiiData's fields are packed
    // (#pragma pack(1)), the multi-byte details big-endian and the names UTF-16 little-endian.
    std::array<u8, kMiiResultSize> r{};
    PutBe32(&r[miiresult::kReturnCode], 0);
    PutBe32(&r[miiresult::kIsGuestMiiSelected], 0);
    PutBe32(&r[miiresult::kSelectedGuestMiiIndex], 0xFFFFFFFF);

    u8* mii = &r[miiresult::kSelectedMiiData];
    mii[0x00] = 0x03;  // version
    mii[0x01] = 0x00;  // mii_options
    mii[0x02] = 0x10;  // mii_pos
    mii[0x03] = 0x30;  // console_identity
    PutBe32(mii + 0x04, 0xD285B6B3), PutBe32(mii + 0x08, 0x00C8850A);  // system_id (u64, big-endian)
    PutBe32(mii + 0x0C, 0x98391EE4);                                    // mii_id
    const u8 mac[6] = {0x40, 0xF4, 0x07, 0xB7, 0x37, 0x10};
    std::memcpy(mii + 0x10, mac, sizeof(mac));  // mac
    PutLe16(mii + 0x16, 0x0000);                // pad
    PutBe16(mii + 0x18, 0xA600);                // mii_details
    const char16_t name[] = {u'C', u'i', u't', u'r', u'a'};
    for (std::size_t i = 0; i < 5; ++i) {
        PutLe16(mii + 0x1A + 2 * i, name[i]);  // mii_name (10 units, the rest zero)
    }
    mii[0x2E] = 0x40;  // height
    mii[0x2F] = 0x40;  // width
    mii[0x30] = 0x00;  // face_style
    mii[0x31] = 0x00;  // face_details
    mii[0x32] = 0x21;  // hair_style
    mii[0x33] = 0x01;  // hair_details
    PutBe32(mii + 0x34, 0x02684418);  // eye_details
    PutBe32(mii + 0x38, 0x26344614);  // eyebrow_details
    PutBe16(mii + 0x3C, 0x8112);      // nose_details
    PutBe16(mii + 0x3E, 0x1768);      // mouth_details
    PutBe16(mii + 0x40, 0x0D00);      // mustache_details
    PutBe16(mii + 0x42, 0x0029);      // beard_details
    PutBe16(mii + 0x44, 0x0052);      // glasses_details
    PutBe16(mii + 0x46, 0x4850);      // mole_details
    const char16_t author[] = {u'f', u'l', u'T', u'o', u'b', u'i'};
    for (std::size_t i = 0; i < 6; ++i) {
        PutLe16(mii + 0x48 + 2 * i, author[i]);  // author_name (10 units, the rest zero)
    }

    // ChecksummedMiiData: the data, padding = 0, then the CRC16 of everything before it (big-endian).
    PutLe16(mii + kMiiDataSize, 0);
    PutBe16(mii + kMiiDataSize + 2, MiiCrc16(mii, kMiiDataSize + 2));

    // guest_mii_name: all zero.
    return r;
}

Result MiiSelector::ReceiveParameterImpl(const MessageParameter& parameter) {
    return AnswerFramebufferRequest(context_, parameter, framebuffer_memory_, "MiiSelector");
}

Result MiiSelector::Start(const MessageParameter& parameter) {
    if (parameter.buffer.size() != kMiiConfigSize) {
        // Azahar asserts.
        MH4N_LOG(Error, kLog, "The size of the parameter (MiiConfig) is wrong: {} bytes",
                 parameter.buffer.size());
        return kResultUnknown;
    }
    // The default selector (DefaultMiiSelector::Setup) is ready at once with the standard Mii; the
    // configuration (the cancel button, the title, the selected index) changes nothing.
    return kResultSuccess;
}

void MiiSelector::Update() {
    // The result is only filled in here, as Azahar does: a cancel before the first update closes the
    // applet with a result of zeros.
    result_ = StandardMiiResult();
    // TODO(Subv): We're finalizing the applet immediately after it's started, but we should defer
    // this call until after all the input has been collected (Azahar's note).
    Finalize();
}

Result MiiSelector::Finalize() {
    CloseApplet(nullptr, std::vector<u8>(result_.begin(), result_.end()));
    return kResultSuccess;
}

}  // namespace mh4n::apt
