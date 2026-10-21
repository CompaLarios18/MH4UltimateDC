// Derived from Azahar src/core/hle/service/cfg/cfg.cpp, src/core/hle/service/cfg/cfg_u.cpp,
// src/core/hle/service/cfg/cfg_s.cpp, src/core/hle/service/cfg/cfg_i.cpp and
// src/core/hle/service/cfg/cfg_nor.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later:
// each of them says "GPLv2 or any later version"): the commands of the four services with their ids,
// replies and result codes, the region and language logic (GetRegionValue, UpdatePreferredRegionCode,
// AdjustLanguageInfoBlock), the model answers (GetSystemModel, GetModelNintendo2DS), the country
// translation tables and the transferable ID.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (cfg.cpp, cfg_u.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (cfg_s.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (cfg_i.cpp)
// Copyright 2016 Citra Emulator Project (cfg_nor.cpp)
//
// cfg:u, cfg:s, cfg:i and cfg:nor (N1-15). All sessions share one `Module` (the ServiceContext state
// "cfg"), made on first use: the config savegame of the user folder, or Azahar's default blocks. See
// cfg_store.h for what is read and written and when.
//
// Start-up (D37): when a runner starts with --user DIR, DIR exists and holds no config savegame, a
// random console unique ID is made and the file is written once (EnsureConfigFile), so the folder works
// with Azahar too. A read command never writes. Without --user, or when DIR does not exist (this step never
// creates it, and neither do mac.txt or mcu.dat; a saving cfg:s/cfg:i command and `--new-console-id`
// still do), nothing is written by it and the ID is 0 for that run; when --user names a folder that does
// not exist, one Info line says so. The replay tool (ipc-replay, Runtime::Prepare) does not
// run this: it is repeatable on the folder of a recording, which holds a config. A short or damaged
// config is left alone with an Error log.
//
// Options: `--new-console-id` gives the console a new random unique ID before the game starts, as the
// runner of the reference does with Azahar's GenerateConsoleUniqueId + SetConsoleUniqueId +
// UpdateConfigNANDSavegame (the file is written); `--region auto|0..6` is Azahar's region setting
// (auto: the region lockout of the game's icon, as its loader reads it).
//
// Departures from Azahar:
//  - Azahar's commands that read the secure info and the local friend code seed need a dump of the
//    console's secure data that is not available. They are cfg:s 0x0404, 0x0405, 0x0407, 0x0408 and
//    cfg:i 0x0404, 0x0405, 0x0407, 0x0408, 0x0817 (cfg:i inherits the cfg:s ids and has 0x0817 too);
//    all are registered without a handler here, so they are answered as unknown commands. Azahar
//    without a dump answers 0x0404/0x0405 with a success reply that carries a zero seed (its error
//    reply is built but overwritten by the later success one). The game does not send them.
//  - SetUUIDClockSequence and GetUUIDClockSequence keep the sequence in mcu.dat as Azahar does, but the
//    file is only written by SetUUIDClockSequence, never when it is read or missing.
//  - A GetConfig for a size larger than the config file is answered with the invalid size result without
//    allocating (Azahar allocates `size` bytes first).
//  - GetConfig (0x0001) with a size above 0x8000 answers the invalid size result before it looks for the
//    block, so before NotFound/NotAuthorized (Azahar allocates the buffer and then looks).
//  - SetSystemConfig (0x0402) answers the invalid size result for a size beyond the buffer it was given
//    or above 0x8000 (Azahar copies without checking).
//  - GetCountryCodeString writes 0 in word 2 of its error reply, where Azahar leaves the word unwritten.
//  - `--region auto` takes the region lockout from `<game folder>/icon.bin` (or
//    `<game folder>/exefs/icon.bin`): the SMDH of the ExeFS 'icon' file, which Azahar's loader reads
//    from the game itself. No tool of this repository writes that file yet (prepare_game.py writes
//    exheader.bin, code.bin, romfs.bin, update_romfs.bin and info.txt), so with an unmodified game folder
//    `auto` answers JPN (0) and leaves the language unadjusted. For a dump of another region GetRegion,
//    IsCoppacsSupported and the language block then differ from Azahar's until the icon is provided
//    (or `--region` is given). The source of the icon is a decision for the orchestrator.
//  - Azahar's GetSystemModel/GetModelNintendo2DS/IsCoppacsSupported read a block that fails with the
//    error result and then push the zero-filled value; so do these.
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "../../common/utf8.h"
#include "mh4n/cfg.h"
#include "cfg_store.h"
#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"
#include "sha256.h"

namespace mh4n::cfg {

namespace {

constexpr const char* kLog = "Service.CFG";
constexpr u32 kMaxSessions = 23;

// ---- the module the sessions share ---------------------------------------------------------------------------

struct Module {
    std::unique_ptr<ConfigStore> store;
    int region_setting = -1;  // Azahar's Settings::values.region_value; -1 = auto select
    u32 preferred_region_code = 0;
    bool preferred_region_chosen = false;
    bool mcu_loaded = false;
    McuData mcu;
    bool mac_loaded = false;
    std::array<u8, 6> mac{};

    ConfigStore& Store(ServiceContext& context) {
        if (!store) {
            store = std::make_unique<ConfigStore>(context.UserFolder());
            region_setting = ParseRegion(context.GetOptions());
        }
        return *store;
    }

    // `--region auto|0..6`; anything else is reported at start (the boot step) and means auto.
    static int ParseRegion(const Options& options) {
        const std::string text = options.GetString("--region", "auto");
        if (text.size() == 1 && text[0] >= '0' && text[0] <= '6') {
            return text[0] - '0';
        }
        return -1;
    }

    // Azahar's UpdatePreferredRegionCode (the system is always powered on here).
    void UpdatePreferredRegionCode(ServiceContext& context) {
        if (preferred_region_chosen) {
            return;
        }
        preferred_region_chosen = true;
        const std::vector<u32> regions = RegionsFromIcon(context.GameFolder());
        if (regions.empty()) {
            MH4N_LOG(Warning, kLog,
                     "--region auto: no icon.bin in the game folder, so GetRegion answers JPN; give "
                     "--region or provide icon.bin");
            return;
        }
        ConfigStore& config = Store(context);
        const u8 current = config.GetLanguageRaw();
        const auto [region, adjusted] = AdjustLanguage(regions, current);
        preferred_region_code = region;
        MH4N_LOG(Info, kLog, "Preferred region code set to {}", preferred_region_code);
        if (current != adjusted) {
            MH4N_LOG(Warning, kLog, "System language {} does not fit the region. Adjusted to {}",
                     current, adjusted);
            config.SetLanguage(adjusted);
        }
    }

    // Azahar's GetRegionValue(from_secure_info = false).
    u32 GetRegionValue(ServiceContext& context) {
        Store(context);
        if (region_setting < 0) {
            UpdatePreferredRegionCode(context);
            return preferred_region_code;
        }
        return static_cast<u32>(region_setting);
    }

    u8 GetSystemLanguage(ServiceContext& context) {
        Store(context);
        if (region_setting < 0) {
            UpdatePreferredRegionCode(context);
        }
        return Store(context).GetLanguageRaw();
    }

    McuData& Mcu(ServiceContext& context) {
        if (!mcu_loaded) {
            mcu = ReadMcuData(context.UserFolder());
            mcu_loaded = true;
        }
        return mcu;
    }
};

Module& ModuleOf(ServiceContext& context) {
    return *context.State<Module>("cfg");
}

// ---- the services ------------------------------------------------------------------------------------------------

struct CountryInfo {
    u8 unknown[2];
    u8 state_code;
    u8 country_code;
    bool operator==(const CountryInfo& other) const {
        return std::memcmp(this, &other, sizeof(*this)) == 0;
    }
};
static_assert(sizeof(CountryInfo) == 4);

enum class Kind { User, System, Internal };

class CfgService final : public Service {
public:
    CfgService(ServiceContext& context, const char* name, Kind kind)
        : Service(context, name), module_(ModuleOf(context)) {
        // cfg common
        Register(0x0001, "GetConfig", &CfgService::GetConfig);
        Register(0x0002, "GetRegion", &CfgService::GetRegion);
        Register(0x0003, "GetTransferableId", &CfgService::GetTransferableId);
        Register(0x0004, "IsCoppacsSupported", &CfgService::IsCoppacsSupported);
        Register(0x0005, "GetSystemModel", &CfgService::GetSystemModel);
        Register(0x0006, "GetModelNintendo2DS", &CfgService::GetModelNintendo2DS);
        Register(0x0007, "WriteToFirstByteCfgSavegame");
        Register(0x0008, "TranslateCountryInfo", &CfgService::TranslateCountryInfo);
        Register(0x0009, "GetCountryCodeString", &CfgService::GetCountryCodeString);
        Register(0x000A, "GetCountryCodeID", &CfgService::GetCountryCodeID);
        Register(0x000B, "IsFangateSupported");
        if (kind == Kind::User) {
            return;
        }
        // cfg:s
        Register(0x0401, "GetSystemConfig", &CfgService::GetSystemConfig);
        Register(0x0402, "SetSystemConfig", &CfgService::SetSystemConfig);
        Register(0x0403, "UpdateConfigNANDSavegame", &CfgService::UpdateConfigNANDSavegame);
        Register(0x0404, "GetLocalFriendCodeSeedData");
        Register(0x0405, "GetLocalFriendCodeSeed");
        Register(0x0406, "GetRegion", &CfgService::GetRegion);
        Register(0x0407, "SecureInfoGetByte101");
        Register(0x0408, "SecureInfoGetSerialNo");
        Register(0x0409, "UpdateConfigBlk00040003");
        if (kind == Kind::System) {
            Register(0x040D, "SetUUIDClockSequence", &CfgService::SetUUIDClockSequence);
            Register(0x040E, "GetUUIDClockSequence", &CfgService::GetUUIDClockSequence);
            return;
        }
        // cfg:i
        Register(0x0801, "GetSystemConfig", &CfgService::GetSystemConfig);
        Register(0x0802, "SetSystemConfig", &CfgService::SetSystemConfig);
        Register(0x0803, "UpdateConfigNANDSavegame", &CfgService::UpdateConfigNANDSavegame);
        Register(0x0804, "CreateConfigInfoBlk");
        Register(0x0805, "DeleteConfigNANDSavefile");
        Register(0x0806, "FormatConfig", &CfgService::FormatConfig);
        Register(0x0808, "UpdateConfigBlk1");
        Register(0x0809, "UpdateConfigBlk2");
        Register(0x080A, "UpdateConfigBlk3");
        Register(0x080B, "SetGetLocalFriendCodeSeedData");
        Register(0x080C, "SetLocalFriendCodeSeedSignature");
        Register(0x080D, "DeleteCreateNANDLocalFriendCodeSeed");
        Register(0x080E, "VerifySigLocalFriendCodeSeed");
        Register(0x080F, "GetLocalFriendCodeSeedData");
        Register(0x0810, "GetLocalFriendCodeSeed");
        Register(0x0811, "SetSecureInfo");
        Register(0x0812, "DeleteCreateNANDSecureInfo");
        Register(0x0813, "VerifySigSecureInfo");
        Register(0x0814, "SecureInfoGetData");
        Register(0x0815, "SecureInfoGetSignature");
        Register(0x0816, "GetRegion", &CfgService::GetRegion);
        Register(0x0817, "SecureInfoGetByte101");
        Register(0x0818, "SecureInfoGetSerialNo");
    }

private:
    ConfigStore& Store() { return module_.Store(Context()); }

    // Inputs: 1: country code id. Outputs: 1: result, 2: the two characters.
    void GetCountryCodeString(Request& request) {
        const u16 id = request.Pop<u16>();
        Reply& reply = request.MakeReply(2, 0);
        if (id >= kCountryCodes.size() || kCountryCodes[id] == 0) {
            MH4N_LOG(Error, kLog, "requested country code id={} is invalid", id);
            reply.Push(kResultNotFound);
            reply.Push<u32>(0);
            return;
        }
        reply.Push(kResultSuccess);
        reply.Push<u32>(kCountryCodes[id]);
    }

    // Inputs: 1: the two characters. Outputs: 1: result, 2: the id.
    void GetCountryCodeID(Request& request) {
        const u16 code = request.Pop<u16>();
        u16 id = 0;
        for (u16 i = 0; i < kCountryCodes.size(); ++i) {
            if (kCountryCodes[i] == code) {
                id = i;
                break;
            }
        }
        Reply& reply = request.MakeReply(2, 0);
        if (id == 0) {
            MH4N_LOG(Error, kLog, "requested country code name={}{} is invalid",
                     static_cast<char>(code & 0xFF), static_cast<char>(code >> 8));
            reply.Push(kResultNotFound);
            reply.Push<u16>(0x00FF);
            return;
        }
        reply.Push(kResultSuccess);
        reply.Push<u16>(id);
    }

    // Outputs: 1: result, 2: the region (a byte).
    void GetRegion(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u8>(static_cast<u8>(module_.GetRegionValue(Context())));
    }

    // Inputs: 1: the 20 bit salt. Outputs: 1: result, 2-3: the low and high words of the ID.
    void GetTransferableId(Request& request) {
        const u32 salt = request.Pop<u32>() & 0x000FFFFF;
        std::array<u8, 12> buffer{};
        const Result result = Store().Get(kBlockConsoleUniqueId2, 8, kGlobal, buffer.data());
        Reply& reply = request.MakeReply(3, 0);
        reply.Push(result);
        if (!IsError(result)) {
            std::memcpy(&buffer[8], &salt, sizeof(salt));
            const std::array<u8, 32> hash = Sha256(buffer.data(), buffer.size());
            u32 low, high;
            std::memcpy(&low, &hash[hash.size() - 8], sizeof(low));
            std::memcpy(&high, &hash[hash.size() - 4], sizeof(high));
            reply.Push(low);
            reply.Push(high);
        } else {
            reply.Push<u32>(0);
            reply.Push<u32>(0);
        }
        MH4N_LOG(Debug, kLog, "called app_id_salt=0x{:X}", salt);
    }

    // Outputs: 1: result, 2: 1 for a Canada or USA console.
    void IsCoppacsSupported(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(module_.GetRegionValue(Context()) == 1);
    }

    // Azahar's GetSystemModel with New 3DS mode off (D10: Old 3DS): a New 3DS model is reported as the
    // Old 3DS XL.
    void GetSystemModel(Request& request) {
        u32 data = 0;
        const Result result = Store().Get(kBlockConsoleModel, 4, kSystemRead, &data);
        const u8 model = data & 0xFF;
        u8 reported = model;
        if (model == kNew3ds || model == kNew3dsXl || model == kNew2dsXl) {
            reported = kNintendo3dsXl;
        }
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(result);
        reply.Push<u8>(reported);
    }

    // Outputs: 1: result, 2: 0 for a Nintendo 2DS, 1 otherwise.
    void GetModelNintendo2DS(Request& request) {
        u32 data = 0;
        const Result result = Store().Get(kBlockConsoleModel, 4, kSystemRead, &data);
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(result);
        reply.Push((data & 0xFF) != kNintendo2ds);
    }

    // Inputs: 1: the country info, 2: the direction (0: B to A, 1: A to B).
    void TranslateCountryInfo(Request& request) {
        const CountryInfo info = request.PopRaw<CountryInfo>();
        const u8 direction = request.Pop<u8>();
        // Left is version A and right is version B.
        static constexpr CountryInfo kTranslations[5][2] = {
            {{{0x00, 0x00}, 0x03, 0x6E}, {{0x00, 0x00}, 0x04, 0x6E}},
            {{{0x00, 0x00}, 0x04, 0x6E}, {{0x00, 0x00}, 0x05, 0x6E}},
            {{{0x00, 0x00}, 0x05, 0x6E}, {{0x00, 0x00}, 0x06, 0x6E}},
            {{{0x00, 0x00}, 0x06, 0x6E}, {{0x00, 0x00}, 0x07, 0x6E}},
            {{{0x00, 0x00}, 0x07, 0x6E}, {{0x00, 0x00}, 0x03, 0x6E}},
        };
        CountryInfo final_info = info;
        if (direction == 0) {
            for (const auto& pair : kTranslations) {
                if (info == pair[1]) {
                    final_info = pair[0];
                    break;
                }
            }
        } else if (direction == 1) {
            for (const auto& pair : kTranslations) {
                if (info == pair[0]) {
                    final_info = pair[1];
                    break;
                }
            }
        }
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.PushRaw(final_info);
    }

    // GetConfig / GetSystemConfig. Inputs: 1: size, 2: block id, 3-4: the output buffer.
    void ReadBlock(Request& request, u16 access) {
        const u32 size = request.Pop<u32>();
        const u32 block_id = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        if (buffer == nullptr) {
            request.MakeReply(1, 0).Push(kResultInvalidBufferDescriptor);
            return;
        }
        std::vector<u8> data(size <= kSaveFileSize ? size : 0);
        Result result;
        if (size > kSaveFileSize) {
            result = kResultInvalidSize;  // no block is that large
        } else {
            result = Store().Get(block_id, size, access, data.data());
        }
        if (!data.empty()) {
            buffer->Write(data.data(), 0, static_cast<u32>(data.size()));
        }
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(result);
        reply.PushMappedBuffer(*buffer);
    }
    void GetConfig(Request& request) { ReadBlock(request, kUserRead); }
    void GetSystemConfig(Request& request) { ReadBlock(request, kSystemRead); }

    // Inputs: 1: block id, 2: size, 3-4: the input buffer.
    void SetSystemConfig(Request& request) {
        const u32 block_id = request.Pop<u32>();
        const u32 size = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        if (buffer == nullptr) {
            request.MakeReply(1, 0).Push(kResultInvalidBufferDescriptor);
            return;
        }
        Result result = kResultInvalidSize;
        if (size <= buffer->Size() && size <= kSaveFileSize) {
            std::vector<u8> data(size);
            if (size == 0 || buffer->Read(data.data(), 0, size)) {
                result = Store().Set(block_id, size, kSystemWrite, data.data());
            }
        }
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(result);
        reply.PushMappedBuffer(*buffer);
    }

    void UpdateConfigNANDSavegame(Request& request) {
        request.MakeReply(1, 0).Push(Store().Save() ? kResultSuccess : kResultUnknown);
    }

    // cfg:i FormatConfig: all blocks back to their defaults with a new console unique ID, saved.
    void FormatConfig(Request& request) {
        ConfigStore& store = Store();
        // The ID is drawn first: a random source that fails (an Error is logged) changes and writes nothing.
        std::pair<u32, u64> drawn;
        if (!TryGenerateConsoleUniqueId(HostRandom64, &drawn)) {
            request.MakeReply(1, 0).Push(kResultUnknown);
            return;
        }
        Result result = store.Format();
        if (!IsError(result)) {
            result = store.SetConsoleUniqueId(drawn.first, drawn.second);
        }
        if (!IsError(result) && !store.Save()) {
            result = kResultUnknown;
        }
        request.MakeReply(1, 0).Push(result);
    }

    // Inputs: 1: the sequence (16 bits).
    void SetUUIDClockSequence(Request& request) {
        McuData& mcu = module_.Mcu(Context());
        mcu.clock_sequence = request.Pop<u16>();
        WriteMcuData(Context().UserFolder(), mcu);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }
    void GetUUIDClockSequence(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u16>(static_cast<u16>(module_.Mcu(Context()).clock_sequence));
    }

    Module& module_;
};

template <Kind kind> ServiceFactory Factory(const char* name) {
    return ServiceFactory(
        [name](ServiceContext& c) { return std::make_unique<CfgService>(c, name, kind); }, kMaxSessions);
}

MH4N_REGISTER(ServiceFactory, "cfg:u", Factory<Kind::User>("cfg:u"));
MH4N_REGISTER(ServiceFactory, "cfg:s", Factory<Kind::System>("cfg:s"));
MH4N_REGISTER(ServiceFactory, "cfg:i", Factory<Kind::Internal>("cfg:i"));

// cfg:nor: every command is known and none is implemented, as in Azahar.
class CfgNor final : public Service {
public:
    explicit CfgNor(ServiceContext& context) : Service(context, "cfg:nor") {
        Register(0x0001, "Initialize");
        Register(0x0002, "Shutdown");
        Register(0x0005, "ReadData");
        Register(0x0006, "WriteData");
    }
};
MH4N_REGISTER(ServiceFactory, "cfg:nor",
              ServiceFactory([](ServiceContext& c) { return std::make_unique<CfgNor>(c); }, kMaxSessions));

// ---- options and the start-up step --------------------------------------------------------------------------------

MH4N_OPTION("runner", "--new-console-id", "",
            "give the console a new random unique ID before the game starts (saved in the user folder's config; "
            "a local wireless room lets in one player per console ID)");
MH4N_OPTION("runner", "--region", "REGION",
            "the console's region: auto (the game's region lockout, default) or 0 JPN, 1 USA, 2 EUR, 3 AUS, 4 CHN, 5 KOR, 6 TWN");
MH4N_OPTION("ipc-replay", "--region", "REGION",
            "the console's region: auto (the game's region lockout, default) or 0 JPN, 1 USA, 2 EUR, 3 AUS, 4 CHN, 5 KOR, 6 TWN");

void StartIdentity(Runtime& rt) {
    const Options& options = rt.GetOptions();
    if (options.Has("--region")) {
        const std::string text = options.GetString("--region");
        if (text != "auto" && Module::ParseRegion(options) < 0) {
            MH4N_LOG(Error, kLog, "bad --region '{}': expected auto or 0 to 6", text);
            rt.RequestExit(2, "bad --region");
            return;
        }
    }
    if (!options.Has("--new-console-id")) {
        // D37: the first start of a user folder that exists and holds no config makes it, once. Only a
        // runner boot does (it has a loader); Runtime::Prepare, the replay tool's way in, never writes.
        if (rt.GetLoader() != nullptr) {
            u64 id = 0;
            const EnsureResult result = EnsureConfigFile(rt.UserFolder(), HostRandom64, &id);
            if (result == EnsureResult::Created) {
                MH4N_LOG(Info, kLog, "new user folder: config savegame made, console id {:016x}", id);
            } else if (result == EnsureResult::NoUserFolder && !rt.UserFolder().empty()) {
                // --user DIR was given but DIR is not a folder: say so once, where the player can see why
                // the console id is 0 (the runtime never creates DIR).
                std::error_code ec;
                const bool exists = std::filesystem::exists(utf8::PathFromUtf8(rt.UserFolder()), ec);
                MH4N_LOG(Info, kLog, "user folder {} {}: no config is made, console id 0 for this run",
                         rt.UserFolder(), exists ? "is not a folder" : "does not exist");
            }
        }
        return;
    }
    // A damaged config is never replaced (not even when asked to): the player's data comes first.
    const std::string config_path = ConfigPath(rt.UserFolder());
    const ConfigFileState state = rt.UserFolder().empty() ? ConfigFileState::Absent : GetConfigFileState(config_path);
    if (state == ConfigFileState::Invalid || state == ConfigFileState::LookupFailed) {
        MH4N_LOG(Error, kLog, "--new-console-id: the config savegame {} is short, damaged or unreadable and is not replaced",
                 config_path);
        rt.RequestExit(3, "--new-console-id failed");
        return;
    }
    // Before the game: the file is read (or the default blocks made), the ID replaced, the file saved.
    ConfigStore store(rt.UserFolder());
    std::pair<u32, u64> drawn;
    if (!TryGenerateConsoleUniqueId(HostRandom64, &drawn)) {
        // The random source failed (an Error is logged): the file is left as it is.
        MH4N_LOG(Error, kLog, "--new-console-id: no new console unique ID could be made");
        rt.RequestExit(3, "--new-console-id failed");
        return;
    }
    const auto [random_number, console_id] = drawn;
    if (IsError(store.SetConsoleUniqueId(random_number, console_id)) || !store.Save()) {
        MH4N_LOG(Error, kLog, "--new-console-id: the console unique ID could not be saved");
        rt.RequestExit(3, "--new-console-id failed");
        return;
    }
    MH4N_LOG(Info, kLog, "console id {:016x}", console_id);
}

MH4N_REGISTER(BootStep, "services.cfg", BootStep{Phase::Services, StartIdentity});

}  // namespace

// ---- mh4n/cfg.h ---------------------------------------------------------------------------------------------------

std::u16string GetUserName(ServiceContext& context) {
    return ModuleOf(context).Store(context).GetUsername();
}

u64 GetConsoleUniqueId(ServiceContext& context) {
    return ModuleOf(context).Store(context).GetConsoleUniqueId();
}

u32 GetRegion(ServiceContext& context) {
    return ModuleOf(context).GetRegionValue(context);
}

u8 GetSystemLanguage(ServiceContext& context) {
    return ModuleOf(context).GetSystemLanguage(context);
}

std::array<u8, 6> GetMacAddress(ServiceContext& context) {
    Module& module = ModuleOf(context);
    if (!module.mac_loaded) {
        const std::string text = ReadMacAddress(context.UserFolder(), HostRandom64);
        ParseMac(text, &module.mac);
        module.mac_loaded = true;
    }
    return module.mac;
}

}  // namespace mh4n::cfg
