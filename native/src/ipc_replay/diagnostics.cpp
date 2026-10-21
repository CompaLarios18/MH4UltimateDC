// Derived from Azahar src/core/hle/result.h, src/core/file_sys/errors.h and src/core/file_sys/directory_backend.h
// at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each says "GPLv2 or any later version"): the
// fields of a result code (description 10 bits, module 8 from bit 10, summary 6 from bit 21, level 5 from
// bit 27) and the names Azahar gives the modules, summaries, levels, the well-known descriptions (1000 and
// above) and the descriptions of the FS module; and the layout of a directory entry (0x228 bytes: the
// UTF-16 name at 0, 0x20C bytes of it, the directory flag at 0x21C).
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (result.h, directory_backend.h)
// Copyright 2016 Citra Emulator Project (errors.h)
#include "ipc_replay/diagnostics.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <numeric>

#include <fmt/format.h>

#include "mh4n/crc32.h"

namespace mh4n::ipc_replay {

namespace {

struct Named {
    u32 value;
    const char* name;
};

template <std::size_t N>
const char* NameOf(const Named (&table)[N], u32 value) {
    for (const Named& entry : table) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return nullptr;
}

// result.h: ErrorModule.
constexpr Named kModules[] = {
    {0, "Common"}, {1, "Kernel"}, {2, "Util"}, {3, "FileServer"}, {4, "LoaderServer"}, {5, "TCB"}, {6, "OS"},
    {7, "DBG"}, {8, "DMNT"}, {9, "PDN"}, {10, "GX"}, {11, "I2C"}, {12, "GPIO"}, {13, "DD"}, {14, "CODEC"},
    {15, "SPI"}, {16, "PXI"}, {17, "FS"}, {18, "DI"}, {19, "HID"}, {20, "CAM"}, {21, "PI"}, {22, "PM"},
    {23, "PM_LOW"}, {24, "FSI"}, {25, "SRV"}, {26, "NDM"}, {27, "NWM"}, {28, "SOC"}, {29, "LDR"}, {30, "ACC"},
    {31, "RomFS"}, {32, "AM"}, {33, "HIO"}, {34, "Updater"}, {35, "MIC"}, {36, "FND"}, {37, "MP"},
    {38, "MPWL"}, {39, "AC"}, {40, "HTTP"}, {41, "DSP"}, {42, "SND"}, {43, "DLP"}, {44, "HIO_LOW"},
    {45, "CSND"}, {46, "SSL"}, {47, "AM_LOW"}, {48, "NEX"}, {49, "Friends"}, {50, "RDT"}, {51, "Applet"},
    {52, "NIM"}, {53, "PTM"}, {54, "MIDI"}, {55, "MC"}, {56, "SWC"}, {57, "FatFS"}, {58, "NGC"}, {59, "CARD"},
    {60, "CARDNOR"}, {61, "SDMC"}, {62, "BOSS"}, {63, "DBM"}, {64, "Config"}, {65, "PS"}, {66, "CEC"},
    {67, "IR"}, {68, "UDS"}, {69, "PL"}, {70, "CUP"}, {71, "Gyroscope"}, {72, "MCU"}, {73, "NS"},
    {74, "News"}, {75, "RO"}, {76, "GD"}, {77, "CardSPI"}, {78, "EC"}, {79, "WebBrowser"}, {80, "Test"},
    {81, "ENC"}, {82, "PIA"}, {83, "ACT"}, {84, "VCTL"}, {85, "OLV"}, {86, "NEIA"}, {87, "NPNS"}, {90, "AVD"},
    {91, "L2B"}, {92, "MVD"}, {93, "NFC"}, {94, "UART"}, {95, "SPM"}, {96, "QTM"}, {97, "NFP"},
    {254, "Application"}, {255, "InvalidResult"},
};

// result.h: ErrorSummary.
constexpr Named kSummaries[] = {
    {0, "Success"},       {1, "NothingHappened"}, {2, "WouldBlock"},    {3, "OutOfResource"},
    {4, "NotFound"},      {5, "InvalidState"},    {6, "NotSupported"},  {7, "InvalidArgument"},
    {8, "WrongArgument"}, {9, "Canceled"},        {10, "StatusChanged"}, {11, "Internal"},
    {63, "InvalidResult"},
};

// result.h: ErrorLevel.
constexpr Named kLevels[] = {
    {0, "Success"}, {1, "Info"},         {25, "Status"}, {26, "Temporary"},
    {27, "Permanent"}, {28, "Usage"},    {29, "Reinitialize"}, {30, "Reset"}, {31, "Fatal"},
};

// result.h: ErrorDescription, the well-known codes shared by every module.
constexpr Named kCommonDescriptions[] = {
    {0, "Success"},          {1000, "InvalidSection"},   {1001, "TooLarge"},          {1002, "NotAuthorized"},
    {1003, "AlreadyDone"},   {1004, "InvalidSize"},      {1005, "InvalidEnumValue"},  {1006, "InvalidCombination"},
    {1007, "NoData"},        {1008, "Busy"},             {1009, "MisalignedAddress"}, {1010, "MisalignedSize"},
    {1011, "OutOfMemory"},   {1012, "NotImplemented"},   {1013, "InvalidAddress"},    {1014, "InvalidPointer"},
    {1015, "InvalidHandle"}, {1016, "NotInitialized"},   {1017, "AlreadyInitialized"}, {1018, "NotFound"},
    {1019, "CancelRequested"}, {1020, "AlreadyExists"},  {1021, "OutOfRange"},        {1022, "Timeout"},
    {1023, "InvalidResultValue"},
};

// errors.h: ErrCodes, the descriptions of the FS module (module 17).
constexpr u32 kModuleFs = 17;
constexpr Named kFsDescriptions[] = {
    {100, "RomFSNotFound"},      {101, "ArchiveNotMounted"},   {112, "FileNotFound"},
    {113, "PathNotFound"},       {120, "NotFound"},            {141, "GameCardNotInserted"},
    {180, "FileAlreadyExists"},  {185, "DirectoryAlreadyExists"}, {190, "AlreadyExists"},
    {210, "InsufficientSpace"},  {230, "InvalidOpenFlags"},    {240, "DirectoryNotEmpty"},
    {250, "NotAFile"},           {340, "NotFormatted"},        {567, "ExeFSSectionNotFound"},
    {630, "CommandNotAllowed"},  {700, "InvalidReadFlag"},     {702, "InvalidPath"},
    {705, "WriteBeyondEnd"},     {760, "UnsupportedOpenFlags"}, {761, "IncorrectExeFSReadSize"},
    {770, "UnexpectedFileOrDirectory"},
};

std::string Field(const char* label, u32 value, const char* name) {
    return name != nullptr ? fmt::format("{} {} ({})", label, value, name) : fmt::format("{} {}", label, value);
}

// ---- directory listings ---------------------------------------------------------------------------------------

constexpr std::size_t kNameUnits = 0x20C / 2;
constexpr std::size_t kDirectoryFlag = 0x21C;

// A simple upper-casing of one UTF-16 code unit: ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic.
// What a volume's own table does with other scripts is not tried.
char16_t SimpleUpper(char16_t c) {
    if (c >= u'a' && c <= u'z') {
        return static_cast<char16_t>(c - 0x20);
    }
    if (c < 0x80) {
        return c;
    }
    if (c == 0xB5) {
        return 0x39C;
    }
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) {
        return static_cast<char16_t>(c - 0x20);
    }
    if (c == 0xFF) {
        return 0x178;
    }
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) {
        return c == 0x131 ? u'I' : ((c & 1) != 0 ? static_cast<char16_t>(c - 1) : c);
    }
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) {
        return (c & 1) == 0 ? static_cast<char16_t>(c - 1) : c;
    }
    if (c >= 0x3B1 && c <= 0x3C9) {
        return c == 0x3C2 ? char16_t{0x3A3} : static_cast<char16_t>(c - 0x20);
    }
    if (c >= 0x430 && c <= 0x44F) {
        return static_cast<char16_t>(c - 0x20);
    }
    if (c >= 0x450 && c <= 0x45F) {
        return static_cast<char16_t>(c - 0x50);
    }
    return c;
}

struct ListedEntry {
    std::u16string name;  // as UTF-16 code units, up to the first null
    bool is_directory = false;
};

ListedEntry ParseEntry(const u8* raw) {
    ListedEntry entry;
    for (std::size_t i = 0; i < kNameUnits; ++i) {
        const char16_t unit = static_cast<char16_t>(raw[i * 2] | (raw[i * 2 + 1] << 8));
        if (unit == 0) {
            break;
        }
        entry.name.push_back(unit);
    }
    entry.is_directory = raw[kDirectoryFlag] != 0;
    return entry;
}

std::u16string Folded(const std::u16string& name) {
    std::u16string out = name;
    for (char16_t& unit : out) {
        unit = SimpleUpper(unit);
    }
    return out;
}

struct Order {
    const char* name;
    // The permutation (entry indices in the order they are written) for the native entries.
    std::function<std::vector<std::size_t>(const std::vector<ListedEntry>&)> arrange;
};

std::vector<std::size_t> Sorted(const std::vector<ListedEntry>& entries,
                                const std::function<bool(const ListedEntry&, const ListedEntry&)>& less) {
    std::vector<std::size_t> order(entries.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return less(entries[a], entries[b]); });
    return order;
}

bool ByName(const ListedEntry& a, const ListedEntry& b) {
    return a.name < b.name;
}
bool ByFoldedName(const ListedEntry& a, const ListedEntry& b) {
    return Folded(a.name) < Folded(b.name);
}

std::vector<std::size_t> Reversed(std::vector<std::size_t> order) {
    std::reverse(order.begin(), order.end());
    return order;
}

const std::vector<Order>& Orders() {
    static const std::vector<Order> orders = {
        {"by name, bytewise (UTF-16 code units)",
         [](const std::vector<ListedEntry>& e) { return Sorted(e, ByName); }},
        {"by name, case-insensitive (simple upper-casing)",
         [](const std::vector<ListedEntry>& e) { return Sorted(e, ByFoldedName); }},
        {"reversed (the native's order backwards)",
         [](const std::vector<ListedEntry>& e) {
             std::vector<std::size_t> order(e.size());
             std::iota(order.begin(), order.end(), std::size_t{0});
             return Reversed(std::move(order));
         }},
        {"by name, bytewise, reversed",
         [](const std::vector<ListedEntry>& e) { return Reversed(Sorted(e, ByName)); }},
        {"by name, case-insensitive, reversed",
         [](const std::vector<ListedEntry>& e) { return Reversed(Sorted(e, ByFoldedName)); }},
        {"directories first, then by name case-insensitively",
         [](const std::vector<ListedEntry>& e) {
             return Sorted(e, [](const ListedEntry& a, const ListedEntry& b) {
                 return a.is_directory != b.is_directory ? a.is_directory : ByFoldedName(a, b);
             });
         }},
        {"files first, then by name case-insensitively",
         [](const std::vector<ListedEntry>& e) {
             return Sorted(e, [](const ListedEntry& a, const ListedEntry& b) {
                 return a.is_directory != b.is_directory ? !a.is_directory : ByFoldedName(a, b);
             });
         }},
    };
    return orders;
}

}  // namespace

std::string DescribeResult(u32 code) {
    const u32 description = ResultDescription(code);
    const u32 module = ResultModule(code);
    const char* description_name = module == kModuleFs ? NameOf(kFsDescriptions, description) : nullptr;
    if (description_name == nullptr) {
        description_name = NameOf(kCommonDescriptions, description);
    }
    return fmt::format("{}, {}, {}, {}", Field("description", description, description_name),
                       Field("module", module, NameOf(kModules, module)),
                       Field("summary", ResultSummary(code), NameOf(kSummaries, ResultSummary(code))),
                       Field("level", ResultLevel(code), NameOf(kLevels, ResultLevel(code))));
}

bool IsListingRead(const std::string& session, u32 request_header, const std::string& buffer_kind, u32 size) {
    const std::size_t slash = session.rfind('/');
    const std::size_t hash = session.rfind('#');
    if (slash == std::string::npos || hash == std::string::npos || hash < slash ||
        session.compare(slash + 1, hash - slash - 1, "dir") != 0) {
        return false;
    }
    return (request_header >> 16) == 0x0801 && buffer_kind == "mapped-w" && size != 0 &&
           size % kListingEntrySize == 0;
}

std::vector<std::string> ListingOrderNames() {
    std::vector<std::string> names;
    for (const Order& order : Orders()) {
        names.push_back(order.name);
    }
    return names;
}

std::vector<std::string> DiagnoseListing(const std::vector<u8>& buffer, std::optional<u32> native_entries,
                                         std::optional<u32> recorded_entries, u32 recorded_crc, bool kept_tail) {
    std::vector<std::string> lines;
    const u64 size = buffer.size();
    if (!native_entries) {
        lines.push_back("listing: the native reply has no entry count");
        return lines;
    }
    const u64 written = static_cast<u64>(*native_entries) * kListingEntrySize;
    if (written > size) {
        lines.push_back(fmt::format("listing: the native reply says {} entries, more than the {}-byte buffer holds",
                                    *native_entries, size));
        return lines;
    }

    std::vector<ListedEntry> entries;
    u32 directories = 0;
    for (u32 i = 0; i < *native_entries; ++i) {
        entries.push_back(ParseEntry(buffer.data() + static_cast<std::size_t>(i) * kListingEntrySize));
        directories += entries.back().is_directory ? 1 : 0;
    }
    std::string recorded = "the recorded reply has no entry count";
    if (recorded_entries) {
        recorded = fmt::format("recorded {} entries ({} bytes)", *recorded_entries,
                               static_cast<u64>(*recorded_entries) * kListingEntrySize);
    }
    lines.push_back(fmt::format("listing: native {} entries ({} directories, {} files), {} of {} bytes written; {}",
                                *native_entries, directories, *native_entries - directories, written, size,
                                recorded));

    const u64 tail = size - written;
    if (recorded_entries && *recorded_entries != *native_entries) {
        lines.push_back("listing: the entry counts differ: the content differs");
    } else {
        // The same entries in another order, then what the buffer held after them (the replay's zeros).
        bool found = false;
        for (const Order& order : Orders()) {
            std::vector<u8> arranged(buffer);
            const std::vector<std::size_t> permutation = order.arrange(entries);
            for (std::size_t slot = 0; slot < permutation.size(); ++slot) {
                std::memcpy(arranged.data() + slot * kListingEntrySize,
                            buffer.data() + permutation[slot] * kListingEntrySize, kListingEntrySize);
            }
            if (Crc32(arranged.data(), arranged.size()) == recorded_crc) {
                lines.push_back(fmt::format("listing: the recorded CRC equals the native's entries in order {}: only "
                                            "the order differs",
                                            order.name));
                found = true;
                break;
            }
        }
        if (!found) {
            lines.push_back(tail == 0 ? "listing: no tried order reproduces the recorded CRC: the content differs"
                            : kept_tail ? "listing: no tried order reproduces the recorded CRC: the content differs, or "
                                          "the bytes after the entries were not what the earlier requests left there"
                                        : "listing: no tried order reproduces the recorded CRC: the content differs, or "
                                          "the bytes after the entries were not zero in the recording");
        }
    }
    if (tail != 0) {
        lines.push_back(fmt::format("listing: the last {} bytes of the buffer are not written by the service; the "
                                    "trace has no bytes for a write-only buffer, so the replay {}",
                                    tail,
                                    kept_tail ? "keeps what the requests before this one left there (--keep-buffers)"
                                              : "gives zeros there"));
    }
    return lines;
}

}  // namespace mh4n::ipc_replay
