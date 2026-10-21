// Derived from Azahar src/core/hle/service/nwm/nwm_uds.cpp, src/core/hle/service/nwm/nwm_uds.h,
// src/core/hle/service/nwm/uds_data.h and src/core/hle/service/nwm/uds_beacon.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the commands of nwm::UDS and their replies, the checks and result codes of each, the
// connection status, node and network structures (NodeInfo, ConnectionStatus, NetworkInfo,
// BeaconDataReplyHeader), and which events are signalled where. Also read, for the timing of the two
// commands that put the client to sleep and for the reply that is written when it wakes:
// src/core/hle/kernel/hle_ipc.cpp (SleepClientThread, ThreadCallback), src/core/hle/kernel/server_session.cpp
// (HandleSyncRequest) and src/core/hle/kernel/thread.cpp (WakeAfterDelay, ThreadWakeupCallback), all
// GPL-2.0-or-later as well.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (nwm_uds.cpp, nwm_uds.h, thread.cpp)
// Copyright 2017 Citra Emulator Project (uds_beacon.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (uds_data.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (hle_ipc.cpp)
// Copyright 2016-2024 Citra Emulator Project / Azahar Emulator Project (server_session.cpp)
//
// nwm::UDS without a room (N1-29): the local-wireless service as Azahar answers it when the emulator is
// not in a multiplayer room (Network::GetRoomMember has no joined member, so SendPacket sends nothing and
// no packet ever arrives). The village's Hunter Search scans, hosts a network for a while, destroys it and
// starts over; every answer here is the one Azahar gives, so that loop stays idle. Rooms come with N4.
//
// What is the same as Azahar's:
//  - One service state for all the sessions (Azahar has one NWM_UDS object): the connection status, the
//    node list, the bound data channels and the connection status event.
//  - RecvBeaconBroadcastData finds no beacons: the reply has the header of the beacon data with no
//    entries, and the client gets it 300 ms of game time after it asked (Azahar sleeps the client for
//    that long on an event nobody signals, then writes the reply).
//  - ConnectNetwork moves the status to Connecting, sends nothing, and answers with the timeout error 5 s
//    of game time later (the connection never completes, so the sleep runs out); the status stays
//    Connecting.
//  - BeginHostingNetwork makes this console the host (status 6, node 1) and signals the connection status
//    event; DestroyNetwork and DisconnectNetwork go back to NotConnected, signal that event and every
//    bound channel's event, and drop the bindings, as Azahar does.
//  - Bind gives a new OneShot event per data channel; nothing signals it but Unbind, Shutdown, Destroy and
//    Disconnect. PullPacket finds an empty queue.
//
// Departures from Azahar:
//  - The beacon broadcast timer (every 102.4 ms of game time while hosting) is not run: with no room its
//    callback only builds a frame and hands it to a SendPacket that sends nothing, so nothing the game or
//    a trace can see depends on it. N4 adds it with the room.
//  - DecryptBeaconData (0x0E and 0x1F) is answered as an unknown command: it decrypts the beacon data of a
//    scanned network (AES-CTR with the NWM beacon key), and without a room there is no scan result to
//    give it. N4 implements it.
//  - Where Azahar asserts and aborts, the call logs an error and goes on, except for the two checks the
//    real UDS module makes fatal (hosting a network of fewer than 2 nodes, application data of more than
//    0xC8 bytes): those log a critical message and end the run with status 3, as err:f does. Azahar's
//    assertions are: the shared memory block of Initialize has the size given (and exists), Bind finds the
//    channel free (the new binding replaces the old one here), the buffer sizes of BeginHostingNetwork,
//    ConnectNetwork, SetApplicationData and SendTo agree with their parameters, and RecvBeaconBroadcastData
//    gets an output buffer of the size given.
//  - PullPacket on an empty queue answers the source node 0; Azahar pushes a SecureDataHeader that it
//    never filled in, so its value is whatever was on the stack.
//  - GetApplicationData with a buffer smaller than the data answers two words (the result and a size of
//    0); Azahar declares two more translate words that it leaves as they were in the command buffer.
//  - The SPOOF_FRIEND_CODE_SEED hack of Azahar (it replaces the friend code seed with the MAC address for
//    chosen titles) is not implemented; it is off for every title by default. The console MAC address
//    (cfg) is only used by Azahar for the host address of a hosted network and for the shared page, and
//    nothing here shows either, so this module needs nothing from cfg.
//  - The deprecated commands read the first 0x3C bytes of the network information as Azahar does; the
//    rest of the structure is zero (Azahar leaves it uninitialised).
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/ipc.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Service.NWM";

// Azahar's ErrorModule::UDS and the ErrorDescription / ErrorSummary / ErrorLevel values the commands use
// (src/core/hle/result.h).
constexpr u32 kModuleUds = 68;
constexpr u32 kDescTooLarge = 1001;
constexpr u32 kDescNotAuthorized = 1002;
constexpr u32 kDescOutOfMemory = 1011;
constexpr u32 kDescNotInitialized = 1016;
constexpr u32 kDescNotFound = 1018;
constexpr u32 kDescTimeout = 1022;
constexpr u32 kDescWrongStatus = 490;  // ErrCodes::WrongStatus of nwm_uds.cpp
constexpr u32 kSumOutOfResource = 3;
constexpr u32 kSumInvalidState = 5;
constexpr u32 kSumWrongArgument = 8;
constexpr u32 kSumCanceled = 9;
constexpr u32 kSumStatusChanged = 10;
constexpr u32 kLevelStatus = 25;
constexpr u32 kLevelUsage = 28;

constexpr Result kErrNotInitialized =
    MakeResult(kDescNotInitialized, kModuleUds, kSumStatusChanged, kLevelStatus);
constexpr Result kErrNodeNotFound =
    MakeResult(kDescNotFound, kModuleUds, kSumWrongArgument, kLevelStatus);
constexpr Result kErrBindArgsZero =
    MakeResult(kDescNotAuthorized, kModuleUds, kSumWrongArgument, kLevelUsage);
constexpr Result kErrBindMaxBinds =
    MakeResult(kDescOutOfMemory, kModuleUds, kSumOutOfResource, kLevelStatus);
constexpr Result kErrTooLargeUsage =
    MakeResult(kDescTooLarge, kModuleUds, kSumWrongArgument, kLevelUsage);
constexpr Result kErrNotConnected =
    MakeResult(kDescNotAuthorized, kModuleUds, kSumInvalidState, kLevelStatus);
constexpr Result kErrEjectNotHost =
    MakeResult(kDescNotAuthorized, kModuleUds, kSumInvalidState, kLevelUsage);
constexpr Result kErrWrongStatus =
    MakeResult(kDescWrongStatus, kModuleUds, kSumInvalidState, kLevelStatus);
constexpr Result kErrConnectTimeout =
    MakeResult(kDescTimeout, kModuleUds, kSumCanceled, kLevelStatus);
static_assert(kErrWrongStatus == 0xC8A111EA, "WrongStatus is description 490, UDS, InvalidState, Status");

// ---- structures (nwm_uds.h, uds_beacon.h, uds_data.h) --------------------------------------------------

constexpr std::size_t kApplicationDataSize = 0xC8;
constexpr u8 kDefaultNetworkChannel = 11;
constexpr u32 kMaxBindNodes = 16;
constexpr u32 kMinRecvBufferSize = 0x5F4;
constexpr u32 kMaxSendSize = 0x5C6;
constexpr u32 kUdsMaxNodes = 16;
constexpr u16 kBroadcastNetworkNodeId = 0xFFFF;
constexpr u16 kHostDestNodeId = 1;
constexpr u32 kPullPacketBufferWords = 0x172;
constexpr u8 kBroadcastFlag = 0x2;

struct NodeInfo {
    u64 friend_code_seed;
    u16 username[10];
    u8 padding0[4];
    u16 network_node_id;
    u8 padding1[6];
};
static_assert(sizeof(NodeInfo) == 40, "NodeInfo has incorrect size.");

enum class NetworkStatus : u32 {
    NotConnected = 3,
    ConnectedAsHost = 6,
    Connecting = 7,
    ConnectedAsClient = 9,
    ConnectedAsSpectator = 10,
};

enum class NetworkStatusChangeReason : u32 {
    None = 0,
    ConnectionEstablished = 1,
    ConnectionLost = 4,
};

struct ConnectionStatus {
    u32 status;
    u32 status_change_reason;
    u16 network_node_id;
    u16 changed_nodes;
    u16 nodes[kUdsMaxNodes];
    u8 total_nodes;
    u8 max_nodes;
    u16 node_bitmask;
};
static_assert(sizeof(ConnectionStatus) == 0x30, "ConnectionStatus has incorrect size.");

// Azahar's NetworkInfo with the big-endian and padding members as bytes; only the channel, the node
// counts and the application data are read.
struct NetworkInfo {
    u8 host_mac_address[6];
    u8 channel;
    u8 unk1;
    u8 initialized;
    u8 padding0[3];
    u8 oui_value[3];
    u8 oui_type;
    u8 wlan_comm_id[4];
    u8 id;
    u8 padding1;
    u8 attributes[2];
    u8 network_id[4];
    u8 total_nodes;
    u8 max_nodes;
    u8 padding2[2];
    u8 padding3[0x1F];
    u8 application_data_size;
    u8 application_data[kApplicationDataSize];
};
static_assert(offsetof(NetworkInfo, oui_value) == 0xC, "oui_value is at the wrong offset.");
static_assert(offsetof(NetworkInfo, wlan_comm_id) == 0x10, "wlancommid is at the wrong offset.");
static_assert(offsetof(NetworkInfo, application_data_size) == 0x3F, "application_data_size offset");
static_assert(sizeof(NetworkInfo) == 0x108, "NetworkInfo has incorrect size.");

struct BeaconDataReplyHeader {
    u32 max_output_size;
    u32 total_size;
    u32 total_entries;
};
static_assert(sizeof(BeaconDataReplyHeader) == 12, "BeaconDataReplyHeader has incorrect size.");

// ---- what all the sessions share --------------------------------------------------------------------------

struct BindNodeData {
    u32 bind_node_id = 0;
    u8 channel = 0;
    u16 network_node_id = 0;
    std::shared_ptr<Event> event;
    // Packets received on the channel: with no room, none ever arrive.
    std::size_t queued_packets = 0;
};

struct UdsState {
    bool initialized = false;
    std::shared_ptr<Event> connection_status_event;
    std::shared_ptr<SharedMemory> recv_buffer_memory;
    ConnectionStatus connection_status{};
    std::vector<NodeInfo> node_info;
    NodeInfo current_node{};
    std::map<u32, BindNodeData> channel_data;  // by data channel
    u8 network_channel = kDefaultNetworkChannel;
    NetworkInfo network_info{};
};

// How long the two commands that sleep the client take, in game time.
constexpr s64 kBeaconScanNs = 300000000;      // UDSBeaconScanInterval
constexpr s64 kConnectionTimeoutNs = 5000000000;  // UDSConnectionTimeout

class NwmUds final : public Service {
public:
    explicit NwmUds(ServiceContext& context)
        : Service(context, "nwm::UDS"), state_(context.State<UdsState>("nwm::UDS")) {
        if (!state_->connection_status_event) {
            state_->connection_status_event = context.GetKernel().NewEvent(
                ResetType::OneShot, "NWM::connection_status_event");
        }
        Register(0x0001, "Initialize (deprecated)", &NwmUds::InitializeDeprecated);
        Register(0x0002, "Scrap");
        Register(0x0003, "Shutdown", &NwmUds::Shutdown);
        Register(0x0004, "BeginHostingNetwork (deprecated)", &NwmUds::BeginHostingNetworkDeprecated);
        Register(0x0005, "EjectClient", &NwmUds::EjectClient);
        Register(0x0006, "EjectSpectators", &NwmUds::EjectSpectators);
        Register(0x0007, "UpdateNetworkAttribute", &NwmUds::UpdateNetworkAttribute);
        Register(0x0008, "DestroyNetwork", &NwmUds::DestroyNetwork);
        Register(0x0009, "ConnectToNetwork (deprecated)", &NwmUds::ConnectToNetworkDeprecated);
        Register(0x000A, "DisconnectNetwork", &NwmUds::DisconnectNetwork);
        Register(0x000B, "GetConnectionStatus", &NwmUds::GetConnectionStatus);
        Register(0x000D, "GetNodeInformation", &NwmUds::GetNodeInformation);
        Register(0x000E, "DecryptBeaconData (deprecated)");
        Register(0x000F, "RecvBeaconBroadcastData", &NwmUds::RecvBeaconBroadcastData);
        Register(0x0010, "SetApplicationData", &NwmUds::SetApplicationData);
        Register(0x0011, "GetApplicationData", &NwmUds::GetApplicationData);
        Register(0x0012, "Bind", &NwmUds::Bind);
        Register(0x0013, "Unbind", &NwmUds::Unbind);
        Register(0x0014, "PullPacket", &NwmUds::PullPacket);
        Register(0x0015, "SetMaxSendDelay");
        Register(0x0017, "SendTo", &NwmUds::SendTo);
        Register(0x001A, "GetChannel", &NwmUds::GetChannel);
        Register(0x001B, "InitializeWithVersion", &NwmUds::InitializeWithVersion);
        Register(0x001D, "BeginHostingNetwork", &NwmUds::BeginHostingNetwork);
        Register(0x001E, "ConnectToNetwork", &NwmUds::ConnectToNetwork);
        Register(0x001F, "DecryptBeaconData");
        Register(0x0020, "Flush");
        Register(0x0021, "SetProbeResponseParam");
        Register(0x0022, "ScanOnConnection");
    }

private:
    UdsState& S() { return *state_; }

    // Azahar's SignalEventAsync on the emulation thread: signal at once. Everything here runs on the
    // scheduler thread.
    static void Signal(const std::shared_ptr<Event>& event) {
        if (event) {
            event->Signal();
        }
    }

    // The client sleeps `ns` of game time, then gets `reply`. Azahar puts the client thread in
    // WaitHleEvent with a timeout (WakeAfterDelay: nsToCycles), writes the reply when the timeout
    // wakes it, and adds no IPC delay; the event it waits on is one nobody holds, so only the timeout
    // wakes it.
    Handled ReplyAfter(Request& request, s64 ns, Reply reply) {
        Kernel& kernel = Context().GetKernel();
        auto shared = std::make_shared<Reply>(std::move(reply));
        kernel.ScheduleAt(kernel.NowTicks() + NsToTicks(static_cast<u64>(ns)),
                          [pending = request.Share(), shared] {
                              pending->Deliver(std::move(*shared));
                          });
        return Handled::Deferred;
    }

    void FatalRequest(const char* what) {
        MH4N_LOG(Critical, kLog, "fatal error of the UDS module: {}", what);
        Context().RequestExit(3, std::string("nwm::UDS: ") + what);
    }

    // ---- Initialize / Shutdown ----

    void DoInitialize(Request& request, u32 sharedmem_size, const NodeInfo& node,
                      std::shared_ptr<SharedMemory> sharedmem) {
        UdsState& s = S();
        s.current_node = node;
        s.initialized = true;
        s.recv_buffer_memory = std::move(sharedmem);
        if (!s.recv_buffer_memory) {
            MH4N_LOG(Error, kLog, "Initialize without a shared memory block");
        } else if (s.recv_buffer_memory->Size() != sharedmem_size) {
            MH4N_LOG(Error, kLog, "Invalid shared memory size ({:#x}, the call says {:#x})",
                     s.recv_buffer_memory->Size(), sharedmem_size);
        }
        // Reset the connection status, it contains all zeros after initialization, except for the
        // actual status value.
        s.connection_status = {};
        s.connection_status.status = static_cast<u32>(NetworkStatus::NotConnected);
        s.node_info.clear();
        s.node_info.push_back(s.current_node);
        s.channel_data.clear();

        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(s.connection_status_event);
    }

    // Inputs:  0: 0x001B0302, 1: shared memory size, 2-11: NodeInfo, 12: version, 13: descriptor,
    //          14: shared memory handle
    // Outputs: 0: 0x001B0042, 1: result, 2: descriptor, 3: connection status event handle
    void InitializeWithVersion(Request& request) {
        const u32 sharedmem_size = request.Pop<u32>();
        const auto node = request.PopRaw<NodeInfo>();
        const u16 version = request.Pop<u16>();
        auto sharedmem = request.PopObject<SharedMemory>();
        MH4N_LOG(Debug, kLog, "InitializeWithVersion sharedmem_size={:#x} version={:#x}",
                 sharedmem_size, version);
        DoInitialize(request, sharedmem_size, node, std::move(sharedmem));
    }

    // Inputs:  1: shared memory size, 2-11: NodeInfo, 12: descriptor, 13: shared memory handle
    void InitializeDeprecated(Request& request) {
        const u32 sharedmem_size = request.Pop<u32>();
        const auto node = request.PopRaw<NodeInfo>();
        auto sharedmem = request.PopObject<SharedMemory>();
        MH4N_LOG(Debug, kLog, "Initialize (deprecated) sharedmem_size={:#x}", sharedmem_size);
        DoInitialize(request, sharedmem_size, node, std::move(sharedmem));
    }

    // Outputs: 0: 0x00030040, 1: result
    void Shutdown(Request& request) {
        UdsState& s = S();
        s.initialized = false;
        for (auto& bind_node : s.channel_data) {
            Signal(bind_node.second.event);
        }
        s.channel_data.clear();
        s.recv_buffer_memory.reset();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Debug, kLog, "Shutdown");
    }

    // ---- the connection status ----

    // Outputs: 0: 0x000B00C0, 1: result, 2-13: the connection status
    void GetConnectionStatus(Request& request) {
        UdsState& s = S();
        const ConnectionStatus out = s.connection_status;
        // Reset the bitmask of changed nodes after each call to this function to prevent falsely
        // informing games of outstanding changes in subsequent calls.
        s.connection_status.changed_nodes = 0;
        Reply& reply = request.MakeReply(13, 0);
        reply.Push(kResultSuccess);
        reply.PushRaw(out);
    }

    // Inputs:  1: node id
    // Outputs: 1: result, 2-11: NodeInfo
    void GetNodeInformation(Request& request) {
        const u16 network_node_id = request.Pop<u16>();
        UdsState& s = S();
        if (!s.initialized) {
            request.MakeReply(1, 0).Push(kErrNotInitialized);
            return;
        }
        const auto node =
            std::find_if(s.node_info.begin(), s.node_info.end(), [network_node_id](const NodeInfo& n) {
                return n.network_node_id == network_node_id;
            });
        if (node == s.node_info.end()) {
            request.MakeReply(1, 0).Push(kErrNodeNotFound);
            return;
        }
        Reply& reply = request.MakeReply(11, 0);
        reply.Push(kResultSuccess);
        reply.PushRaw(*node);
    }

    // Outputs: 1: result, 2: the channel of the network (0 when not connected)
    void GetChannel(Request& request) {
        UdsState& s = S();
        const bool is_connected =
            s.connection_status.status != static_cast<u32>(NetworkStatus::NotConnected);
        const u8 channel = is_connected ? s.network_channel : 0;
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(channel);
    }

    // ---- scanning and connecting ----

    // Inputs:  1: output buffer size, 2-3: unknown, 4-5: MAC address, 6-14: unused, 15: wlan comm id,
    //          16: id, 17-18: input event handle, 19-20: output mapped buffer
    // Outputs: 1: result, 2-3: the output buffer
    Handled RecvBeaconBroadcastData(Request& request) {
        const u32 out_buffer_size = request.Pop<u32>();
        request.Pop<u32>();  // unk1
        request.Pop<u32>();  // unk2
        request.PopRaw<std::array<u8, 6>>();  // the MAC address filter
        for (int i = 0; i < 9; ++i) {
            request.Pop<u32>();  // the rest of the scan input structure
        }
        const u32 wlan_comm_id = request.Pop<u32>();
        const u32 id = request.Pop<u32>();
        // 3dbrew: official processes make an event for this and never keep it. Azahar does not use it.
        request.PopObject<Event>();
        const MappedBuffer* out_buffer = request.PopMappedBuffer();
        if (out_buffer == nullptr) {
            MH4N_LOG(Error, kLog, "RecvBeaconBroadcastData without an output buffer");
            request.MakeReply(1, 0).Push(kResultSuccess);
            return Handled::Done;
        }
        if (out_buffer->Size() != out_buffer_size) {
            MH4N_LOG(Error, kLog, "RecvBeaconBroadcastData output buffer is {:#x} bytes, not {:#x}",
                     out_buffer->Size(), out_buffer_size);
        }

        // With no room there are no received beacons: the header with no entries.
        BeaconDataReplyHeader header{};
        header.max_output_size = out_buffer_size;
        header.total_size = static_cast<u32>(sizeof(BeaconDataReplyHeader));
        header.total_entries = 0;
        if (!out_buffer->Write(&header, 0, sizeof(header))) {
            MH4N_LOG(Error, kLog, "RecvBeaconBroadcastData could not write the beacon data header");
        }
        MH4N_LOG(Debug, kLog, "RecvBeaconBroadcastData out_buffer_size={:#x} wlan_comm_id={:#x} id={:#x}",
                 out_buffer_size, wlan_comm_id, id);

        Reply reply(request.CommandId(), 1, 2);
        reply.Push(kResultSuccess);
        reply.PushMappedBuffer(*out_buffer);
        // On a real 3DS this is about 0.38 seconds.
        return ReplyAfter(request, kBeaconScanNs, std::move(reply));
    }

    void StartConnection(const NetworkInfo& info) {
        UdsState& s = S();
        s.network_info = info;
        // StartConnectionSequence: Connecting, then an authentication frame that goes nowhere.
        s.connection_status.status = static_cast<u32>(NetworkStatus::Connecting);
    }

    Handled ConnectCommon(Request& request, const std::vector<u8>& info_bytes) {
        NetworkInfo info{};
        std::memcpy(&info, info_bytes.data(), std::min(info_bytes.size(), sizeof(info)));
        StartConnection(info);
        MH4N_LOG(Debug, kLog, "ConnectToNetwork");
        // Originally 300 ms, Azahar made it 5 s to accommodate high ping. Nothing completes the
        // connection without a room, so the sleep times out and the callback answers with the error.
        Reply reply(request.CommandId(), 1, 0);
        reply.Push(kErrConnectTimeout);
        return ReplyAfter(request, kConnectionTimeoutNs, std::move(reply));
    }

    // Inputs:  1: connection type, 2: passphrase size, 3-4: the network structure (static buffer 0),
    //          5-6: the passphrase (static buffer 2)
    Handled ConnectToNetwork(Request& request) {
        request.Pop<u8>();   // connection type
        request.Pop<u32>();  // passphrase size
        const StaticBuffer* network = request.PopStaticBuffer();
        request.PopStaticBuffer();  // the passphrase
        std::vector<u8> bytes = network != nullptr ? network->data : std::vector<u8>{};
        if (bytes.size() != sizeof(NetworkInfo)) {
            MH4N_LOG(Error, kLog, "ConnectToNetwork network structure of {} bytes, not {}",
                     bytes.size(), sizeof(NetworkInfo));
        }
        return ConnectCommon(request, bytes);
    }

    // Inputs:  1-15: the first 0x3C bytes of the network structure, 16: connection type,
    //          17: passphrase size, 18-19: the passphrase
    Handled ConnectToNetworkDeprecated(Request& request) {
        const auto network = request.PopRaw<std::array<u8, 0x3C>>();
        request.Pop<u8>();   // connection type
        request.Pop<u32>();  // passphrase size
        request.PopStaticBuffer();
        return ConnectCommon(request, std::vector<u8>(network.begin(), network.end()));
    }

    // ---- hosting ----

    Result DoBeginHosting(const std::vector<u8>& info_bytes) {
        UdsState& s = S();
        NetworkInfo info{};
        std::memcpy(&info, info_bytes.data(), std::min(info_bytes.size(), sizeof(info)));
        // The real UDS module throws a fatal error if this fails.
        if (info.max_nodes <= 1) {
            FatalRequest("trying to host a network of only one member");
            return kResultSuccess;
        }
        // Ensure the application data size is less than the maximum value.
        if (info.application_data_size > kApplicationDataSize) {
            FatalRequest("application data is too big");
            return kResultSuccess;
        }
        s.network_info = info;

        s.connection_status.status = static_cast<u32>(NetworkStatus::ConnectedAsHost);
        s.connection_status.status_change_reason =
            static_cast<u32>(NetworkStatusChangeReason::ConnectionEstablished);
        s.connection_status.max_nodes = s.network_info.max_nodes;

        // Resize the nodes list to hold max_nodes.
        s.node_info.clear();
        s.node_info.resize(s.network_info.max_nodes);

        // There's currently only one node in the network (the host).
        s.connection_status.total_nodes = 1;
        s.network_info.total_nodes = 1;

        // The host is always the first node.
        s.connection_status.network_node_id = 1;
        s.current_node.network_node_id = 1;
        s.connection_status.nodes[0] = s.connection_status.network_node_id;
        // Set the bit 0 in the nodes bitmask to indicate that node 1 is already taken.
        s.connection_status.node_bitmask |= 1;
        // Notify the application that the first node was set.
        s.connection_status.changed_nodes |= 1;

        s.node_info[0] = s.current_node;

        // If the game has a preferred channel, use that instead.
        if (s.network_info.channel != 0) {
            s.network_channel = s.network_info.channel;
        } else {
            s.network_info.channel = kDefaultNetworkChannel;
        }

        Signal(s.connection_status_event);
        return kResultSuccess;
    }

    // Inputs:  1: passphrase size, 2-3: the network structure (static buffer 0), 4-5: the passphrase
    void BeginHostingNetwork(Request& request) {
        request.Pop<u32>();  // passphrase size
        const StaticBuffer* network = request.PopStaticBuffer();
        request.PopStaticBuffer();  // the passphrase
        std::vector<u8> bytes = network != nullptr ? network->data : std::vector<u8>{};
        if (bytes.size() != sizeof(NetworkInfo)) {
            MH4N_LOG(Error, kLog, "BeginHostingNetwork network structure of {} bytes, not {}",
                     bytes.size(), sizeof(NetworkInfo));
        }
        MH4N_LOG(Debug, kLog, "BeginHostingNetwork");
        const Result result = DoBeginHosting(bytes);
        request.MakeReply(1, 0).Push(result);
    }

    // Inputs:  1-15: the first 0x3C bytes of the network structure, 16: passphrase size,
    //          17-18: the passphrase
    void BeginHostingNetworkDeprecated(Request& request) {
        const auto network = request.PopRaw<std::array<u8, 0x3C>>();
        request.Pop<u32>();  // passphrase size
        request.PopStaticBuffer();
        MH4N_LOG(Debug, kLog, "BeginHostingNetwork (deprecated)");
        const Result result = DoBeginHosting(std::vector<u8>(network.begin(), network.end()));
        request.MakeReply(1, 0).Push(result);
    }

    // Outputs: 1: result
    void DestroyNetwork(Request& request) {
        UdsState& s = S();
        // Only a host can destroy.
        if (s.connection_status.status != static_cast<u32>(NetworkStatus::ConnectedAsHost)) {
            MH4N_LOG(Warning, kLog, "DestroyNetwork called with status {}", s.connection_status.status);
            request.MakeReply(1, 0).Push(kErrWrongStatus);
            return;
        }
        const u16 node_id = s.connection_status.network_node_id;
        s.connection_status = {};
        s.connection_status.status = static_cast<u32>(NetworkStatus::NotConnected);
        s.connection_status.network_node_id = node_id;
        Signal(s.connection_status_event);
        for (auto& bind_node : s.channel_data) {
            Signal(bind_node.second.event);
        }
        s.channel_data.clear();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Debug, kLog, "DestroyNetwork");
    }

    // Outputs: 1: result
    void DisconnectNetwork(Request& request) {
        UdsState& s = S();
        MH4N_LOG(Debug, kLog, "disconnecting from network");
        if (s.connection_status.status == static_cast<u32>(NetworkStatus::ConnectedAsHost)) {
            // A real 3DS makes strange things here. We do the same.
            const u16 node_id = s.connection_status.network_node_id;
            s.connection_status = {};
            s.connection_status.status = static_cast<u32>(NetworkStatus::ConnectedAsHost);
            s.connection_status.network_node_id = node_id;
            MH4N_LOG(Debug, kLog, "called as a host");
            request.MakeReply(1, 0).Push(kErrWrongStatus);
            return;
        }
        const u16 node_id = s.connection_status.network_node_id;
        s.connection_status = {};
        s.connection_status.status = static_cast<u32>(NetworkStatus::NotConnected);
        s.connection_status.network_node_id = node_id;
        Signal(s.connection_status_event);
        // The deauthentication frame goes to the room, which is not there.
        for (auto& bind_node : s.channel_data) {
            Signal(bind_node.second.event);
        }
        s.channel_data.clear();
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // Inputs:  1: node id
    void EjectClient(Request& request) {
        const u16 network_node_id = request.Pop<u16>();
        MH4N_LOG(Warning, kLog, "(stubbed) EjectClient called");
        UdsState& s = S();
        Result result = kResultSuccess;
        if (network_node_id == 1) {
            // The host can not be kicked.
            result = kErrBindArgsZero;
        } else if (s.connection_status.status != static_cast<u32>(NetworkStatus::ConnectedAsHost)) {
            // Only the host can kick people.
            MH4N_LOG(Warning, kLog, "called with status {}", s.connection_status.status);
            result = kErrEjectNotHost;
        }
        // Otherwise: the deauthentication goes to the room (or to a node that is not there: no error).
        request.MakeReply(1, 0).Push(result);
    }

    void EjectSpectators(Request& request) {
        MH4N_LOG(Warning, kLog, "(STUBBED) EjectSpectators called");
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // Inputs:  1: node bitmask (u16), 2: flag (u8)
    void UpdateNetworkAttribute(Request& request) {
        // Stubbed in Azahar: success whatever the arguments.
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // ---- application data ----

    // Inputs:  1: size, 2-3: the data (static buffer 0)
    void SetApplicationData(Request& request) {
        const u32 size = request.Pop<u32>();
        const StaticBuffer* buffer = request.PopStaticBuffer();
        const std::size_t have = buffer != nullptr ? buffer->data.size() : 0;
        if (have != size) {
            MH4N_LOG(Error, kLog, "SetApplicationData: the buffer has {} bytes, the call says {}", have,
                     size);
        }
        MH4N_LOG(Debug, kLog, "SetApplicationData");
        if (size > kApplicationDataSize) {
            request.MakeReply(1, 0).Push(kErrTooLargeUsage);
            return;
        }
        NetworkInfo& info = S().network_info;
        info.application_data_size = static_cast<u8>(size);
        if (buffer != nullptr) {
            std::memcpy(info.application_data, buffer->data.data(), std::min<std::size_t>(size, have));
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // Inputs:  1: the size of the caller's buffer
    // Outputs: 1: result, 2: the size of the data, 3-4: the data (static buffer 0)
    void GetApplicationData(Request& request) {
        const u32 input_size = request.Pop<u32>();
        const NetworkInfo& info = S().network_info;
        const u8 appdata_size = info.application_data_size;
        if (input_size < appdata_size) {
            Reply& reply = request.MakeReply(2, 0);
            reply.Push(kResultSuccess);
            reply.Push(u32{0});
            return;
        }
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push(appdata_size);
        reply.PushStaticBuffer(std::vector<u8>(info.application_data, info.application_data + appdata_size), 0);
    }

    // ---- data channels ----

    // Inputs:  1: bind node id, 2: receive buffer size, 3: data channel, 4: network node id
    // Outputs: 1: result, 2: descriptor, 3: the data available event
    void Bind(Request& request) {
        const u32 bind_node_id = request.Pop<u32>();
        const u32 recv_buffer_size = request.Pop<u32>();
        const u8 data_channel = request.Pop<u8>();
        const u16 network_node_id = request.Pop<u16>();
        UdsState& s = S();

        if (data_channel == 0 || bind_node_id == 0) {
            MH4N_LOG(Warning, kLog, "data_channel = {}, bind_node_id = {}", data_channel, bind_node_id);
            request.MakeReply(1, 0).Push(kErrBindArgsZero);
            return;
        }
        if (s.channel_data.size() >= kMaxBindNodes) {
            MH4N_LOG(Warning, kLog, "max bind nodes");
            request.MakeReply(1, 0).Push(kErrBindMaxBinds);
            return;
        }
        if (recv_buffer_size < kMinRecvBufferSize) {
            MH4N_LOG(Warning, kLog, "MinRecvBufferSize");
            request.MakeReply(1, 0).Push(kErrTooLargeUsage);
            return;
        }
        if (s.channel_data.count(data_channel) != 0) {
            // Azahar asserts that the channel is free.
            MH4N_LOG(Error, kLog, "data channel {} is already bound; the new binding replaces it",
                     data_channel);
        }
        auto event = Context().GetKernel().NewEvent(
            ResetType::OneShot, "NWM::BindNodeEvent" + std::to_string(bind_node_id));
        // TODO(B3N30) in Azahar: support more than one bind node per channel.
        BindNodeData& data = s.channel_data[data_channel];
        data = {};
        data.bind_node_id = bind_node_id;
        data.channel = data_channel;
        data.network_node_id = network_node_id;
        data.event = event;
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(std::move(event));
    }

    // Inputs:  1: bind node id
    // Outputs: 1: result, 2: the bind node id, 3-5: zero
    void Unbind(Request& request) {
        const u32 bind_node_id = request.Pop<u32>();
        if (bind_node_id == 0) {
            request.MakeReply(1, 0).Push(kErrBindArgsZero);
            return;
        }
        UdsState& s = S();
        const auto bound = std::find_if(s.channel_data.begin(), s.channel_data.end(), [&](const auto& d) {
            return d.second.bind_node_id == bind_node_id;
        });
        if (bound != s.channel_data.end()) {
            Signal(bound->second.event);
            s.channel_data.erase(bound);
        }
        Reply& reply = request.MakeReply(5, 0);
        reply.Push(kResultSuccess);
        reply.Push(bind_node_id);
        reply.Push(u32{0});
        reply.Push(u32{0});
        reply.Push(u32{0});
    }

    // Inputs:  1: bind node id, 2: output buffer size >> 2, 3: output buffer size
    // Outputs: 1: result, 2: the size received, 3: the source node, 4-5: the data (static buffer 0)
    void PullPacket(Request& request) {
        const u32 bind_node_id = request.Pop<u32>();
        const u32 max_out_buff_size_aligned = request.Pop<u32>();
        request.Pop<u32>();  // max_out_buff_size: only a queued packet is compared with it
        UdsState& s = S();

        // This size is hard coded into the uds module. We don't know the meaning yet.
        const u32 buff_size = std::min<u32>(max_out_buff_size_aligned, kPullPacketBufferWords) << 2;

        const u32 status = s.connection_status.status;
        if (status != static_cast<u32>(NetworkStatus::ConnectedAsHost) &&
            status != static_cast<u32>(NetworkStatus::ConnectedAsClient) &&
            status != static_cast<u32>(NetworkStatus::ConnectedAsSpectator)) {
            MH4N_LOG(Error, kLog, "Not connected yet.");
            request.MakeReply(1, 0).Push(kErrNotConnected);
            return;
        }
        const auto channel = std::find_if(s.channel_data.begin(), s.channel_data.end(), [&](const auto& d) {
            return d.second.bind_node_id == bind_node_id;
        });
        if (channel == s.channel_data.end()) {
            MH4N_LOG(Error, kLog, "Could not find channel bn {:#x}.", bind_node_id);
            request.MakeReply(1, 0).Push(kErrNotConnected);
            return;
        }
        // No room: the queue is always empty. Azahar answers a zeroed buffer of buff_size bytes.
        Reply& reply = request.MakeReply(3, 2);
        reply.Push(kResultSuccess);
        reply.Push(u32{0});
        reply.Push(u16{0});
        reply.PushStaticBuffer(std::vector<u8>(buff_size, 0), 0);
    }

    // Inputs:  2: destination node id, 3: data channel, 5: data size, 6: flags,
    //          7-8: the data (static buffer 0)
    void SendTo(Request& request) {
        request.Pop<u32>();  // unknown
        const u16 dest_node_id = request.Pop<u16>();
        request.Pop<u8>();   // data channel
        request.Pop<u32>();  // buffer size >> 2
        const u32 data_size = request.Pop<u32>();
        const u8 flags = request.Pop<u8>();
        const StaticBuffer* buffer = request.PopStaticBuffer();
        const std::size_t have = buffer != nullptr ? buffer->data.size() : 0;
        if (have < data_size) {
            MH4N_LOG(Error, kLog, "SendTo: the buffer has {} bytes, the call says {}", have, data_size);
        }
        UdsState& s = S();

        const u32 status = s.connection_status.status;
        if (status != static_cast<u32>(NetworkStatus::ConnectedAsClient) &&
            status != static_cast<u32>(NetworkStatus::ConnectedAsHost)) {
            MH4N_LOG(Error, kLog,
                     "You are not connected as a client or a host. (you are connected as type {})", status);
            request.MakeReply(1, 0).Push(kErrNotConnected);
            return;
        }
        // There should never be a dest_node_id of 0, and a node does not send to itself.
        if (dest_node_id == 0) {
            MH4N_LOG(Error, kLog, "dest_node_id is 0");
            request.MakeReply(1, 0).Push(kErrNodeNotFound);
            return;
        }
        if (dest_node_id == s.connection_status.network_node_id) {
            MH4N_LOG(Error, kLog, "tried to send packet to itself");
            request.MakeReply(1, 0).Push(kErrNodeNotFound);
            return;
        }
        if (flags >> 2) {
            MH4N_LOG(Error, kLog, "Unexpected flags {:#04x}", flags);
        }
        // GetNodeMacAddress: a broadcast and the host have an address; a client needs a connected
        // entry in the node map, and with no room the map is empty.
        const bool has_address = (flags & kBroadcastFlag) != 0 ||
                                 dest_node_id == kBroadcastNetworkNodeId ||
                                 dest_node_id == kHostDestNodeId;
        if (!has_address) {
            MH4N_LOG(Error, kLog, "Destination address was 0");
            request.MakeReply(1, 0).Push(kErrNodeNotFound);
            return;
        }
        if (data_size > kMaxSendSize) {
            MH4N_LOG(Error, kLog, "Data size was greater than the max packet size {} > {}", data_size,
                     kMaxSendSize);
            request.MakeReply(1, 0).Push(kErrTooLargeUsage);
            return;
        }
        // The data frame goes to the room, which is not there.
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    std::shared_ptr<UdsState> state_;
};

MH4N_REGISTER(ServiceFactory, "nwm::UDS",
              ServiceFactory([](ServiceContext& context) { return std::make_unique<NwmUds>(context); }));

}  // namespace

}  // namespace mh4n
