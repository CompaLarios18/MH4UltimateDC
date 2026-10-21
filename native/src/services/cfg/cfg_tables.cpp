// Derived from Azahar src/core/hle/service/cfg/cfg.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: it says "GPLv2 or any later version"): the table of country codes and the table of
// default subregions (states), copied as they are.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
#include "cfg_store.h"

namespace mh4n::cfg {

namespace {
constexpr u16 C(const char code[2]) {
    return static_cast<u16>(static_cast<u8>(code[0]) | (static_cast<u8>(code[1]) << 8));
}
}  // namespace

const std::array<u16, 187> kCountryCodes = {{
    // 0-7 Japan
    0,
    C("JP"),
    0,
    0,
    0,
    0,
    0,
    0,

    // 8-15 America
    C("AI"),
    C("AG"),
    C("AR"),
    C("AW"),
    C("BS"),
    C("BB"),
    C("BZ"),
    C("BO"),
    // 16-23 America
    C("BR"),
    C("VG"),
    C("CA"),
    C("KY"),
    C("CL"),
    C("CO"),
    C("CR"),
    C("DM"),
    // 24-31 America
    C("DO"),
    C("EC"),
    C("SV"),
    C("GF"),
    C("GD"),
    C("GP"),
    C("GT"),
    C("GY"),
    // 32-39 America
    C("HT"),
    C("HN"),
    C("JM"),
    C("MQ"),
    C("MX"),
    C("MS"),
    C("AN"),
    C("NI"),
    // 40-47 America
    C("PA"),
    C("PY"),
    C("PE"),
    C("KN"),
    C("LC"),
    C("VC"),
    C("SR"),
    C("TT"),
    // 48-55 America
    C("TC"),
    C("US"),
    C("UY"),
    C("VI"),
    C("VE"),
    0,
    0,
    0,

    // 56-63 Invalid
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,

    // 64-71 Europe
    C("AL"),
    C("AU"),
    C("AT"),
    C("BE"),
    C("BA"),
    C("BW"),
    C("BG"),
    C("HR"),
    // 72-79 Europe
    C("CY"),
    C("CZ"),
    C("DK"),
    C("EE"),
    C("FI"),
    C("FR"),
    C("DE"),
    C("GR"),
    // 80-87 Europe
    C("HU"),
    C("IS"),
    C("IE"),
    C("IT"),
    C("LV"),
    C("LS"),
    C("LI"),
    C("LT"),
    // 88-95 Europe
    C("LU"),
    C("MK"),
    C("MT"),
    C("ME"),
    C("MZ"),
    C("NA"),
    C("NL"),
    C("NZ"),
    // 96-103 Europe
    C("NO"),
    C("PL"),
    C("PT"),
    C("RO"),
    C("RU"),
    C("RS"),
    C("SK"),
    C("SI"),
    // 104-111 Europe
    C("ZA"),
    C("ES"),
    C("SZ"),
    C("SE"),
    C("CH"),
    C("TR"),
    C("GB"),
    C("ZM"),
    // 112-119 Europe
    C("ZW"),
    C("AZ"),
    C("MR"),
    C("ML"),
    C("NE"),
    C("TD"),
    C("SD"),
    C("ER"),
    // 120-127 Europe
    C("DJ"),
    C("SO"),
    C("AD"),
    C("GI"),
    C("GG"),
    C("IM"),
    C("JE"),
    C("MC"),

    // 128-135 Taiwan
    C("TW"),
    0,
    0,
    0,
    0,
    0,
    0,
    0,

    // 136-143 Korea
    C("KR"),
    0,
    0,
    0,
    0,
    0,
    0,
    0,

    // 144-151 China? (Hong Kong & Macao)
    C("HK"),
    C("MO"),
    0,
    0,
    0,
    0,
    0,
    0,

    // 152-159 Southeast Asia
    C("ID"),
    C("SG"), // USA
    C("TH"),
    C("PH"),
    C("MY"), // USA
    0,
    0,
    0,

    // 160-167 China
    C("CN"),
    0,
    0,
    0,
    0,
    0,
    0,
    0,

    // 168-175 Middle East
    C("AE"), // USA
    C("IN"), // EUR
    C("EG"),
    C("OM"),
    C("QA"),
    C("KW"),
    C("SA"), // USA
    C("SY"),
    // 176-183 Middle East
    C("BH"),
    C("JO"),
    0,
    0,
    0,
    0,
    0,
    0,

    // 184-186 European Microstates
    C("SM"),
    C("VA"),
    C("BM"),
}};

// Based on PKHeX's lists of subregions at
// https://github.com/kwsch/PKHeX/tree/master/PKHeX.Core/Resources/text/locale3DS/subregions
const std::array<u8, 187> kDefaultSubregion = {{
    0, 2, 0,  0, 0, 0, 0, 0, // 0-7
    1, 2, 2,  1, 1, 1, 2, 2, // 8-15
    2, 1, 2,  1, 2, 2, 2, 1, // 16-23
    2, 2, 2,  1, 1, 1, 2, 2, // 24-31
    2, 2, 2,  1, 2, 1, 1, 2, // 32-39
    2, 2, 2,  2, 1, 1, 2, 2, // 40-47
    1, 2, 2,  1, 2, 0, 0, 0, // 48-55
    0, 0, 0,  0, 0, 0, 0, 0, // 56-63
    2, 2, 2,  2, 2, 1, 2, 6, // 64-71
    1, 2, 18, 1, 8, 2, 2, 2, // 72-79
    2, 1, 2,  2, 1, 2, 1, 2, // 80-87
    1, 1, 1,  1, 1, 1, 2, 2, // 88-95
    7, 2, 2,  2, 9, 1, 2, 1, // 96-103
    2, 2, 2,  2, 2, 2, 2, 1, // 104-111
    1, 1, 1,  1, 1, 1, 1, 1, // 112-119
    1, 1, 1,  1, 1, 1, 1, 1, // 120-127
    2, 0, 0,  0, 0, 0, 0, 0, // 128-135
    2, 0, 0,  0, 0, 0, 0, 0, // 136-143
    1, 0, 0,  0, 0, 0, 0, 0, // 144-151
    0, 1, 0,  0, 2, 0, 0, 0, // 152-159
    2, 0, 0,  0, 0, 0, 0, 0, // 160-167
    2, 2, 0,  0, 0, 0, 2, 0, // 168-175
    0, 0, 0,  0, 0, 0, 0, 0, // 176-183
    1, 1, 1,                 // 184-186
}};

// Azahar's Module::IsValidRegionCountry.
bool IsValidRegionCountry(u32 region, u8 country_code) {
    switch (region) {
    case 0:  // JPN
        return country_code == 1;
    case 1:  // USA
        return (country_code >= 8 && country_code <= 52) || country_code == 153 ||
               country_code == 156 || country_code == 168 || country_code == 174;
    case 2:  // EUR
    case 3:  // AUS
        return (country_code >= 64 && country_code <= 127) ||
               (country_code >= 184 && country_code <= 186) || country_code == 169;
    case 4:  // CHN
        return country_code == 144 || country_code == 145 || country_code == 160;
    case 5:  // KOR
        return country_code == 136;
    case 6:  // TWN
        return country_code == 128;
    default:
        break;
    }
    return false;
}

}  // namespace mh4n::cfg
