// Derived from Azahar src/core/hle/service/act/act.cpp, src/core/hle/service/act/act_u.cpp,
// src/core/hle/service/act/act_errors.cpp and src/core/hle/service/act/act_errors.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the command table of act:u with its session limit, the stubbed Initialize and
// GetAccountInfo, and GetErrorCode with its table from ACT result descriptions to error codes.
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (act.cpp)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (act_u.cpp)
// Copyright 2024-2025 Citra Emulator Project / Azahar Emulator Project (act_errors.cpp, act_errors.h)
//
// act:u (N1-19): the Nintendo Network ID account service, as Azahar stubs it. Initialize and
// GetAccountInfo succeed and do nothing (GetAccountInfo writes nothing into its output buffer);
// GetErrorCode maps an ACT result to its 022-xxxx error code (the module must be ACT, 83; anything
// else is 229999). The other commands of Azahar's table are nullptr entries, answered as unknown
// commands.
#include <algorithm>
#include <array>
#include <utility>

#include "mh4n/ipc.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Service.ACT";

constexpr u32 kModuleAct = 83;
constexpr u32 kErrUnknown = 229999;  // 022-9999

// ErrDescriptions -> ErrCodes (act_errors.cpp's switch), in the order of that switch.
constexpr std::array<std::pair<u32, u32>, 246> kErrorCodes = {{
    {0, 220000}, {1, 220001}, {100, 220500}, {101, 220501}, {102, 220502}, {103, 225103},
    {104, 225104}, {111, 220511}, {112, 225112}, {191, 220591}, {192, 220592}, {193, 220593},
    {200, 220600}, {201, 220601}, {202, 220602}, {203, 220603}, {204, 220604}, {205, 220605},
    {206, 220606}, {300, 220700}, {301, 220701}, {310, 220710}, {311, 220711}, {312, 220712},
    {313, 220713}, {314, 220714}, {315, 225315}, {330, 220730}, {331, 220731}, {340, 220740},
    {341, 220741}, {350, 225350}, {351, 225351}, {352, 225352}, {360, 225360}, {361, 225361},
    {362, 225362}, {363, 225363}, {400, 221000}, {401, 221001}, {402, 221002}, {411, 221011},
    {412, 221012}, {413, 221013}, {421, 221021}, {422, 221022}, {423, 221023}, {431, 225431},
    {432, 225432}, {433, 225433}, {451, 221101}, {500, 222000}, {501, 222100}, {502, 225502},
    {503, 225503}, {504, 225504}, {505, 225505}, {506, 225506}, {507, 225507}, {508, 225508},
    {509, 225509}, {510, 225510}, {511, 225511}, {512, 225512}, {513, 225513}, {514, 225514},
    {515, 225515}, {516, 225516}, {517, 225517}, {518, 225518}, {519, 225519}, {520, 225520},
    {521, 225521}, {522, 225522}, {523, 225523}, {524, 225524}, {525, 225525}, {526, 225526},
    {527, 225527}, {528, 225528}, {529, 225529}, {530, 225530}, {531, 225531}, {532, 225532},
    {533, 225533}, {534, 225534}, {535, 225535}, {536, 225536}, {537, 225537}, {538, 225538},
    {539, 225539}, {540, 225540}, {541, 225541}, {542, 225542}, {543, 225543}, {544, 225544},
    {545, 225545}, {546, 225546}, {547, 225547}, {548, 225548}, {549, 225549}, {550, 225550},
    {551, 225551}, {552, 225552}, {553, 225553}, {600, 222400}, {601, 222401}, {602, 222402},
    {603, 222403}, {604, 222404}, {620, 222420}, {621, 222421}, {622, 222422}, {623, 222423},
    {650, 222450}, {651, 222451}, {652, 222452}, {653, 222453}, {654, 222454}, {655, 222455},
    {656, 222456}, {657, 222457}, {658, 222458}, {659, 222459}, {660, 222460}, {661, 222461},
    {671, 222471}, {672, 222472}, {673, 222473}, {681, 222481}, {682, 222482}, {683, 222483},
    {684, 222484}, {685, 222485}, {701, 222501}, {702, 222502}, {703, 222503}, {710, 222510},
    {711, 222511}, {712, 222512}, {720, 222520}, {722, 222522}, {730, 222530}, {731, 222531},
    {732, 222532}, {733, 222533}, {734, 222534}, {735, 222535}, {736, 222536}, {737, 222537},
    {738, 222538}, {740, 222540}, {741, 222541}, {742, 222542}, {743, 222543}, {770, 222570},
    {771, 222571}, {772, 222572}, {773, 222573}, {774, 222574}, {775, 222575}, {776, 222576},
    {777, 222577}, {778, 222578}, {779, 222579}, {780, 222580}, {781, 222581}, {782, 222582},
    {783, 222583}, {784, 222584}, {785, 222585}, {786, 222586}, {787, 222587}, {791, 222591},
    {792, 222592}, {793, 222593}, {794, 222594}, {795, 222595}, {796, 222596}, {797, 222597},
    {810, 222610}, {811, 222611}, {812, 222612}, {813, 222613}, {814, 222614}, {815, 222615},
    {816, 222616}, {831, 222631}, {832, 222632}, {833, 222633}, {834, 222634}, {835, 222635},
    {836, 222636}, {841, 222641}, {842, 222642}, {843, 222643}, {844, 225844}, {851, 222651},
    {861, 222661}, {862, 222662}, {870, 222670}, {871, 222671}, {872, 222672}, {873, 222673},
    {874, 222674}, {875, 222675}, {876, 222676}, {877, 222677}, {878, 222678}, {879, 222679},
    {880, 222680}, {881, 222681}, {900, 222800}, {901, 222801}, {902, 222802}, {903, 222803},
    {904, 222804}, {905, 222805}, {911, 222811}, {912, 222812}, {913, 222813}, {914, 222814},
    {915, 222815}, {921, 222821}, {922, 222822}, {923, 222823}, {924, 222824}, {925, 222825},
    {931, 222831}, {932, 222832}, {933, 222833}, {934, 222834}, {935, 222835}, {950, 222880},
    {951, 222881}, {952, 222882}, {953, 222883}, {954, 222884}, {970, 222900}, {971, 222901},
    {980, 222910}, {981, 222911}, {991, 222931}, {992, 222932}, {998, 222998}, {999, 222999}}};

u32 GetActErrorCode(Result result) {
    if (ResultModule(result) != kModuleAct) {
        return kErrUnknown;
    }
    const u32 description = ResultDescription(result);
    for (const auto& [desc, code] : kErrorCodes) {
        if (desc == description) {
            return code;
        }
    }
    return kErrUnknown;
}

class ActU final : public Service {
public:
    explicit ActU(ServiceContext& context) : Service(context, "act:u") {
        Register(0x0001, "Initialize", &ActU::Initialize);
        Register(0x0002, "GetErrorCode", &ActU::GetErrorCode);
        Register(0x0003, "GetLastResponseCode");
        Register(0x0005, "GetCommonInfo");
        Register(0x0006, "GetAccountInfo", &ActU::GetAccountInfo);
        Register(0x0007, "GetResultAsync");
        Register(0x0008, "GetMiiImageData");
        Register(0x0009, "SetNfsPassword");
        Register(0x000B, "AcquireEulaList");
        Register(0x000C, "AcquireTimeZoneList");
        Register(0x000D, "GenerateUuid");
        Register(0x000F, "FindSlotNoByUuid");
        Register(0x0010, "SaveData");
        Register(0x0011, "GetTransferableId");
        Register(0x0012, "AcquireNexServiceToken");
        Register(0x0013, "GetNexServiceToken");
        Register(0x0014, "AcquireIndependentServiceToken");
        Register(0x0015, "GetIndependentServiceToken");
        Register(0x0016, "AcquireAccountInfo");
        Register(0x0017, "AcquireAccountIdByPrincipalId");
        Register(0x0018, "AcquirePrincipalIdByAccountId");
        Register(0x0019, "AcquireMii");
        Register(0x001A, "AcquireAccountInfoEx");
        Register(0x001D, "InquireMailAddress");
        Register(0x001E, "AcquireEula");
        Register(0x001F, "AcquireEulaLanguageList");
    }

private:
    // 0x00010084: SDK version, shared memory size, the calling PID, the shared memory. Reply: result.
    void Initialize(Request& request) {
        const u32 sdk_version = request.Pop<u32>();
        const u32 shared_memory_size = request.Pop<u32>();
        const u32 caller_pid = request.PopPid();
        request.PopObject<SharedMemory>();
        MH4N_LOG(Debug, kLog, "(STUBBED) called sdk_version={:08X}, shared_memory_size={:08X}, caller_pid={}",
                 sdk_version, shared_memory_size, caller_pid);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // 0x00020040: a result. Reply: result, error code.
    void GetErrorCode(Request& request) {
        const Result result = request.Pop<Result>();
        MH4N_LOG(Debug, kLog, "called result={:08X}", result);
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(GetActErrorCode(result));
    }

    // 0x000600C2: account slot (u8), size, block id, the output buffer (mapped). Reply: result.
    void GetAccountInfo(Request& request) {
        const u8 account_slot = request.Pop<u8>();
        const u32 size = request.Pop<u32>();
        const u32 block_id = request.Pop<u32>();
        request.PopMappedBuffer();
        MH4N_LOG(Debug, kLog, "(STUBBED) called account_slot={:02X}, size={:08X}, block_id={:08X}",
                 account_slot, size, block_id);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }
};

MH4N_REGISTER(ServiceFactory, "act:u",
              ServiceFactory([](ServiceContext& context) { return std::make_unique<ActU>(context); },
                             3));

}  // namespace

}  // namespace mh4n
