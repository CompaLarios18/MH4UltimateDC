// Derived from Azahar src/core/hle/service/frd/frd.cpp, src/core/hle/service/frd/frd.h and
// src/core/hle/service/frd/frd_u.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later:
// each of them says "GPLv2 or any later version"): the command table of frd:u with its session
// limit, the layouts (FriendKey, MyPresence, Profile, Game, Comment) and the stubbed answers of the
// commands, and the 500 ms login delay.
// Copyright 2015-2024 Citra Emulator Project / Azahar Emulator Project (frd.cpp)
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (frd.h)
// Copyright 2014 Citra Emulator Project (frd_u.cpp)
//
// frd:u (N1-19): the friends list, as Azahar stubs it: nobody is a friend, nothing is online.
//  - HasLoggedIn and IsOnline answer whether Login has completed. Login keeps the event the game
//    passes and, 500 ms of game time later (timer queue, exact in ticks), sets "logged in" and signals
//    the event; it replies at once. The state is shared by all the sessions, as Azahar's Module.
//  - GetMyFriendKey, GetMyPresence, GetFriendKeyList, GetFriendProfile and GetFriendAttributeFlags
//    answer zeros (0 friends); UnscrambleLocalFriendCode answers zeros; SetClientSdkVersion and
//    GetLastResponseResult succeed.
//  - Every other entry of Azahar's table is a nullptr there (Logout, GetMyPassword, GetFriendPresence,
//    GetFriendScreenName, UpdateGameMode, AttachToEventNotification, GetEventNotification,
//    ResultToErrorCode, RequestGameAuthentication, ...): answered as unknown commands.
//
// Not implemented here (answered as unknown commands, which differs from Azahar; the game does not
// send them): GetMyScreenName (Azahar reads the user name from the cfg username block, owned by the
// cfg service) and GetMyMii (Azahar builds its standard Mii and a CRC-16 around it).
//
// Departures from Azahar: a structure whose size the game declares differently from the number of
// keys is not asserted (Azahar's ASSERT aborts); the reply is built from the declared count.
#include <algorithm>
#include <memory>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/ipc.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/scheduler.h"
#include "mh4n/service.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Service.FRD";

constexpr u32 kFriendKeySize = 16;       // FriendKey: friend_id, unknown, friend_code
constexpr u32 kMyPresenceSize = 0x12C;
constexpr u32 kScrambledFriendCodeSize = 12;
constexpr u32 kFriendCodeSize = 8;
constexpr u32 kProfileSize = 8;
constexpr int kLoginDelayMs = 500;

// The Module of Azahar: shared by every session.
struct FrdState {
    bool logged_in = false;
    std::shared_ptr<Event> login_event;
};

class FrdU final : public Service {
public:
    explicit FrdU(ServiceContext& context)
        : Service(context, "frd:u"), state_(context.State<FrdState>("frd:u")) {
        Register(0x0001, "HasLoggedIn", &FrdU::HasLoggedIn);
        Register(0x0002, "IsOnline", &FrdU::IsOnline);
        Register(0x0003, "Login", &FrdU::Login);
        Register(0x0004, "Logout");
        Register(0x0005, "GetMyFriendKey", &FrdU::GetMyFriendKey);
        Register(0x0006, "GetMyPreference", &FrdU::GetMyPreference);
        Register(0x0007, "GetMyProfile", &FrdU::GetMyProfile);
        Register(0x0008, "GetMyPresence", &FrdU::GetMyPresence);
        Register(0x0009, "GetMyScreenName");
        Register(0x000A, "GetMyMii");
        Register(0x000B, "GetMyLocalAccountId");
        Register(0x000C, "GetMyPlayingGame", &FrdU::GetMyPlayingGame);
        Register(0x000D, "GetMyFavoriteGame", &FrdU::GetMyFavoriteGame);
        Register(0x000E, "GetMyNcPrincipalId");
        Register(0x000F, "GetMyComment", &FrdU::GetMyComment);
        Register(0x0010, "GetMyPassword");
        Register(0x0011, "GetFriendKeyList", &FrdU::GetFriendKeyList);
        Register(0x0012, "GetFriendPresence");
        Register(0x0013, "GetFriendScreenName");
        Register(0x0014, "GetFriendMii");
        Register(0x0015, "GetFriendProfile", &FrdU::GetFriendProfile);
        Register(0x0016, "GetFriendRelationship");
        Register(0x0017, "GetFriendAttributeFlags", &FrdU::GetFriendAttributeFlags);
        Register(0x0018, "GetFriendPlayingGame");
        Register(0x0019, "GetFriendFavoriteGame");
        Register(0x001A, "GetFriendInfo");
        Register(0x001B, "IsIncludedInFriendList");
        Register(0x001C, "UnscrambleLocalFriendCode", &FrdU::UnscrambleLocalFriendCode);
        Register(0x001D, "UpdateGameModeDescription");
        Register(0x001E, "UpdateGameMode");
        Register(0x001F, "SendInvitation");
        Register(0x0020, "AttachToEventNotification");
        Register(0x0021, "SetNotificationMask");
        Register(0x0022, "GetEventNotification");
        Register(0x0023, "GetLastResponseResult", &FrdU::GetLastResponseResult);
        Register(0x0024, "PrincipalIdToFriendCode");
        Register(0x0025, "FriendCodeToPrincipalId");
        Register(0x0026, "IsValidFriendCode");
        Register(0x0027, "ResultToErrorCode");
        Register(0x0028, "RequestGameAuthentication");
        Register(0x0029, "GetGameAuthenticationData");
        Register(0x002A, "RequestServiceLocator");
        Register(0x002B, "GetServiceLocatorData");
        Register(0x002C, "DetectNatProperties");
        Register(0x002D, "GetNatProperties");
        Register(0x002E, "GetServerTimeInterval");
        Register(0x002F, "AllowHalfAwake");
        Register(0x0030, "GetServerTypes");
        Register(0x0031, "GetFriendComment");
        Register(0x0032, "SetClientSdkVersion", &FrdU::SetClientSdkVersion);
        Register(0x0033, "GetMyApproachContext");
        Register(0x0034, "AddFriendWithApproach");
        Register(0x0035, "DecryptApproachContext");
    }

private:
    static std::vector<u8> Zeros(u64 size) {
        return std::vector<u8>(static_cast<std::size_t>(std::min<u64>(size, 0x3FFFF)), 0);
    }

    // Outputs: result, logged in (0/1).
    void HasLoggedIn(Request& request) {
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(state_->logged_in);
    }
    void IsOnline(Request& request) {
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(state_->logged_in);
    }

    // Inputs: the event to signal when the login is done (a handle). Outputs: result. The login takes
    // 500 ms of game time.
    void Login(Request& request) {
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
        state_->login_event = request.PopObject<Event>();
        const std::shared_ptr<FrdState> state = state_;
        Kernel& kernel = Context().GetKernel();
        kernel.ScheduleAt(kernel.NowTicks() + NsToTicks(kLoginDelayMs * 1000000ull), [state] {
            state->logged_in = true;
            if (state->login_event) {
                state->login_event->Signal();
            }
        });
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // Outputs: result, FriendKey (friend_id, unknown, friend_code): all zero.
    void GetMyFriendKey(Request& request) {
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
        Reply& reply = request.MakeReply(5, 0);
        reply.Push(kResultSuccess);
        for (u32 i = 0; i < kFriendKeySize / 4; ++i) {
            reply.Push<u32>(0);
        }
    }

    // Outputs: result, public mode 1, show game 1, show history 0.
    void GetMyPreference(Request& request) {
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
        Reply& reply = request.MakeReply(4, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(1);
        reply.Push<u32>(1);
        reply.Push<u32>(0);
    }

    // Outputs: result, Profile{region 1, country 1, area 1, language 1, platform 1} (8 bytes).
    void GetMyProfile(Request& request) {
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
        Reply& reply = request.MakeReply(3, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0x01010101);
        reply.Push<u32>(0x00000001);
    }

    // Outputs: result; the 0x12C bytes of MyPresence (zeros) as static buffer 0.
    void GetMyPresence(Request& request) {
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushStaticBuffer(Zeros(kMyPresenceSize), 0);
    }

    // Both: result, Game{title id 0x0004000E00030700, version 1} (16 bytes).
    void PushGame(Request& request) {
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
        Reply& reply = request.MakeReply(5, 0);
        reply.Push(kResultSuccess);
        reply.Push<u64>(0x0004000E00030700ull);
        reply.Push<u32>(1);
        reply.Push<u32>(0);
    }
    void GetMyPlayingGame(Request& request) { PushGame(request); }
    void GetMyFavoriteGame(Request& request) { PushGame(request); }

    // Outputs: result, Comment{"Hey!"} (0x20 bytes of UTF-16), 0.
    void GetMyComment(Request& request) {
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
        Reply& reply = request.MakeReply(10, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(u'H' | (u'e' << 16));
        reply.Push<u32>(u'y' | (u'!' << 16));
        for (int i = 0; i < 6; ++i) {
            reply.Push<u32>(0);
        }
        reply.Push<u32>(0);
    }

    // Inputs: unknown, the capacity of the list. Outputs: result, 0 friends; the list (zeros,
    // capacity * 16 bytes) as static buffer 0.
    void GetFriendKeyList(Request& request) {
        const u32 unknown = request.Pop<u32>();
        const u32 frd_count = request.Pop<u32>();
        MH4N_LOG(Warning, kLog, "(STUBBED) called, unknown={}, frd_count={}", unknown, frd_count);
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);
        reply.PushStaticBuffer(Zeros(static_cast<u64>(kFriendKeySize) * frd_count), 0);
    }

    // Inputs: count, the friend keys (static buffer). Outputs: result; count * 8 bytes (zeros).
    void GetFriendProfile(Request& request) {
        const u32 count = request.Pop<u32>();
        CheckKeys(request, count);
        MH4N_LOG(Warning, kLog, "(STUBBED) called, count={}", count);
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushStaticBuffer(Zeros(static_cast<u64>(kProfileSize) * count), 0);
    }

    // Inputs: count, the friend keys (static buffer). Outputs: result; count bytes (zeros; Azahar
    // assumes one byte per friend).
    void GetFriendAttributeFlags(Request& request) {
        const u32 count = request.Pop<u32>();
        CheckKeys(request, count);
        MH4N_LOG(Warning, kLog, "(STUBBED) called, count={}", count);
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushStaticBuffer(Zeros(count), 0);
    }

    // Inputs: count, the scrambled codes (static buffer, 12 bytes each). Outputs: result; count * 8
    // bytes (zeros: Azahar does not unscramble anything).
    void UnscrambleLocalFriendCode(Request& request) {
        const u32 count = request.Pop<u32>();
        const StaticBuffer* codes = request.PopStaticBuffer();
        if (codes == nullptr ||
            codes->size != static_cast<u64>(count) * kScrambledFriendCodeSize) {
            MH4N_LOG(Error, kLog, "Wrong input buffer size");
        }
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushStaticBuffer(Zeros(static_cast<u64>(kFriendCodeSize) * count), 0);
    }

    // Inputs: the SDK version, the calling PID. Outputs: result.
    void SetClientSdkVersion(Request& request) {
        const u32 version = request.Pop<u32>();
        request.PopPid();
        MH4N_LOG(Warning, kLog, "(STUBBED) called, version: 0x{:08X}", version);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void GetLastResponseResult(Request& request) {
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    static void CheckKeys(Request& request, u32 count) {
        const StaticBuffer* keys = request.PopStaticBuffer();
        if (keys == nullptr || keys->size != static_cast<u64>(count) * kFriendKeySize) {
            MH4N_LOG(Error, kLog, "friend key list size does not match the count {}", count);
        }
    }

    std::shared_ptr<FrdState> state_;
};

MH4N_REGISTER(ServiceFactory, "frd:u",
              ServiceFactory([](ServiceContext& context) { return std::make_unique<FrdU>(context); },
                             8));

}  // namespace

}  // namespace mh4n
