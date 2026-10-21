// Derived from Azahar src/core/hle/service/ac/ac.cpp, src/core/hle/service/ac/ac.h,
// src/core/hle/service/ac/ac_u.cpp and src/core/hle/service/ac/ac_i.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the commands of ac:u and ac:i with their ids, replies and result codes, and the state
// they keep (connected or not, the four events), the status values.
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (ac.cpp, ac.h, ac_i.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (ac_u.cpp)
//
// ac:u and ac:i (N1-15): the console is on the internet, wifi slot 1: GetStatus answers 3 (internet),
// GetWifiStatus 1 (connected, slot 1). ConnectAsync succeeds at once and signals the event it was
// given, CloseAsync signals the close event (and the disconnect event if connected). All sessions
// share the state, as Azahar's one module does.
//
// Departures from Azahar:
//  - GetNZoneBeaconNotFoundEvent with a handle that is not an event dereferences a null pointer in
//    Azahar; here the event is not signalled and the command succeeds.
//  - SetRequestEulaVersion echoes the static buffer it received (Azahar's `ac_config`), and with no
//    buffer an empty one.
#include <memory>
#include <vector>

#include "mh4n/ipc.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Service.AC";
constexpr u32 kMaxSessions = 10;
constexpr u32 kModuleAc = 39;
constexpr u32 kStatusInternet = 3;          // Status::STATUS_INTERNET
constexpr u32 kWifiConnectedSlot1 = 1 << 0;  // WifiStatus::STATUS_CONNECTED_SLOT1
constexpr std::size_t kAcConfigSize = 0x200;

struct State {
    bool ac_connected = false;
    std::shared_ptr<Event> close_event;
    std::shared_ptr<Event> connect_event;
    std::shared_ptr<Event> disconnect_event;
    std::shared_ptr<Event> nintendo_zone_beacon_not_found_event;
};

class Ac final : public Service {
public:
    Ac(ServiceContext& context, const char* name)
        : Service(context, name), s_(*context.State<State>("ac")) {
        Register(0x0001, "CreateDefaultConfig", &Ac::CreateDefaultConfig);
        Register(0x0004, "ConnectAsync", &Ac::ConnectAsync);
        Register(0x0005, "GetConnectResult", &Ac::GetConnectResult);
        Register(0x0007, "CancelConnectAsync", &Ac::CancelConnectAsync);
        Register(0x0008, "CloseAsync", &Ac::CloseAsync);
        Register(0x0009, "GetCloseResult", &Ac::GetCloseResult);
        Register(0x000A, "GetLastErrorCode");
        Register(0x000C, "GetStatus", &Ac::GetStatus);
        Register(0x000D, "GetWifiStatus", &Ac::GetWifiStatus);
        Register(0x000E, "GetCurrentAPInfo");
        Register(0x0010, "GetCurrentNZoneInfo");
        Register(0x0011, "GetNZoneApNumService");
        Register(0x001D, "ScanAPs");
        Register(0x0024, "AddDenyApType");
        Register(0x0027, "GetInfraPriority", &Ac::GetInfraPriority);
        Register(0x002D, "SetRequestEulaVersion", &Ac::SetRequestEulaVersion);
        Register(0x002F, "GetNZoneBeaconNotFoundEvent", &Ac::GetNZoneBeaconNotFoundEvent);
        Register(0x0030, "RegisterDisconnectEvent", &Ac::RegisterDisconnectEvent);
        Register(0x0036, "GetConnectingProxyEnable", &Ac::GetConnectingProxyEnable);
        Register(0x003C, "GetAPSSIDList");
        Register(0x003E, "IsConnected", &Ac::IsConnected);
        Register(0x0040, "SetClientVersion", &Ac::SetClientVersion);
    }

private:
    void Success(Request& request) { request.MakeReply(1, 0).Push(kResultSuccess); }
    // The PID descriptor and PID that start the translate parameters of most commands (Azahar:
    // `rp.Skip(2, false)`).
    static void SkipPid(Request& request) {
        request.Pop<u32>();
        request.Pop<u32>();
    }

    // Outputs: 1: result, 2-3: a static buffer of 0x200 bytes (id 0).
    void CreateDefaultConfig(Request& request) {
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushStaticBuffer(std::vector<u8>(kAcConfigSize, 0), 0);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    // Inputs: 1-2: the PID, 3-4: the connection event, 5-6: the config buffer.
    void ConnectAsync(Request& request) {
        SkipPid(request);
        s_.connect_event = request.PopObject<Event>();
        if (s_.connect_event) {
            s_.connect_event->Signal();
            s_.ac_connected = true;
        }
        Success(request);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    void GetConnectResult(Request& request) {
        SkipPid(request);
        Success(request);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    void CancelConnectAsync(Request& request) {
        SkipPid(request);
        // ErrorDescription 301 when connected, 302 when not; InvalidState, Usage.
        request.MakeReply(1, 0).Push(MakeResult(s_.ac_connected ? 301 : 302, kModuleAc, 5, 28));
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    void CloseAsync(Request& request) {
        SkipPid(request);
        s_.close_event = request.PopObject<Event>();
        if (s_.ac_connected && s_.disconnect_event) {
            s_.disconnect_event->Signal();
        }
        if (s_.close_event) {
            s_.close_event->Signal();
        }
        s_.ac_connected = false;
        Success(request);
    }

    void GetCloseResult(Request& request) {
        SkipPid(request);
        Success(request);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    void GetStatus(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(kStatusInternet);
    }

    void GetWifiStatus(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(kWifiConnectedSlot1);
    }

    // Inputs: 64-65: the config buffer. Outputs: 1: result, 2: the priority (0).
    void GetInfraPriority(Request& request) {
        request.PopStaticBuffer();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    // Inputs: 1: major (8 bits), 2: minor (8 bits), 3-4: the config buffer, which is returned.
    void SetRequestEulaVersion(Request& request) {
        const u32 major = request.Pop<u8>();
        const u32 minor = request.Pop<u8>();
        const StaticBuffer* config = request.PopStaticBuffer();
        std::vector<u8> data = config != nullptr ? config->data : std::vector<u8>();
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushStaticBuffer(std::move(data), 0);
        MH4N_LOG(Warning, kLog, "(STUBBED) called, major={}, minor={}", major, minor);
    }

    void GetNZoneBeaconNotFoundEvent(Request& request) {
        SkipPid(request);
        s_.nintendo_zone_beacon_not_found_event = request.PopObject<Event>();
        if (s_.nintendo_zone_beacon_not_found_event) {
            s_.nintendo_zone_beacon_not_found_event->Signal();
        }
        Success(request);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    void RegisterDisconnectEvent(Request& request) {
        SkipPid(request);
        s_.disconnect_event = request.PopObject<Event>();
        Success(request);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    void GetConnectingProxyEnable(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(false);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    // Inputs: 1: ?, 2: a descriptor, 3: ?. Outputs: 1: result, 2: connected.
    void IsConnected(Request& request) {
        const u32 unk = request.Pop<u32>();
        const u32 unk_descriptor = request.Pop<u32>();
        const u32 unk_param = request.Pop<u32>();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(s_.ac_connected);
        MH4N_LOG(Debug, kLog, "(STUBBED) called unk=0x{:08X} descriptor=0x{:08X} param=0x{:08X}", unk,
                 unk_descriptor, unk_param);
    }

    void SetClientVersion(Request& request) {
        const u32 version = request.Pop<u32>();
        Success(request);
        MH4N_LOG(Warning, kLog, "(STUBBED) called, version: 0x{:08X}", version);
    }

    State& s_;
};

MH4N_REGISTER(ServiceFactory, "ac:u",
              ServiceFactory([](ServiceContext& c) { return std::make_unique<Ac>(c, "ac:u"); }, kMaxSessions));
MH4N_REGISTER(ServiceFactory, "ac:i",
              ServiceFactory([](ServiceContext& c) { return std::make_unique<Ac>(c, "ac:i"); }, kMaxSessions));

}  // namespace

}  // namespace mh4n
