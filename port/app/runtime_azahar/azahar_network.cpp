// AzaharRuntime: the network room. Azahar's room client (Network::RoomMember) and room server
// (Network::Room) as the app's multiplayer.cpp sees them, with the console's identity from the
// core's settings.
#include <memory>
#include <mutex>
#include <utility>

#include "common/file_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/shared_page.h"
#include "core/hle/service/cfg/cfg.h"
#include "netsync.h"
#include "network/network.h"
#include "network/room.h"
#include "network/room_member.h"
#include "network/verify_user.h"
#include "runtime_azahar/azahar_runtime.h"

#ifdef ENABLE_WEB_SERVICE
#include "network/announce_multiplayer_session.h"
#include "web_service/verify_user_jwt.h"
#endif

namespace App::Azahar {

namespace {

using Network::RoomMember;

/// The port's net sync clock reports (port/src/netsync.cpp): a Wi-Fi packet type no 3DS frame
/// uses.
constexpr auto NET_SYNC_PACKET = static_cast<Network::WifiPacket::PacketType>(0x4d);

/// What the room callbacks and the lobby listing keep between calls.
struct NetworkState {
    std::shared_ptr<const RoomCallbacks> callbacks;
    RoomMember::CallbackHandle<RoomMember::Error> on_error;
    RoomMember::CallbackHandle<RoomMember::State> on_state;
    RoomMember::CallbackHandle<Network::RoomInformation> on_info;
    RoomMember::CallbackHandle<Network::WifiPacket> on_wifi;
    RoomMember::CallbackHandle<Network::StatusMessageEntry> on_status;

    std::mutex announce_mutex;
#ifdef ENABLE_WEB_SERVICE
    std::shared_ptr<Network::AnnounceMultiplayerSession> announce;
#endif
};

NetworkState& N() {
    static NetworkState state;
    return state;
}

RoomError ConvertError(RoomMember::Error error) {
    switch (error) {
    case RoomMember::Error::LostConnection:
        return RoomError::LostConnection;
    case RoomMember::Error::HostKicked:
        return RoomError::HostKicked;
    case RoomMember::Error::UnknownError:
        return RoomError::Unknown;
    case RoomMember::Error::NameCollision:
        return RoomError::NameCollision;
    case RoomMember::Error::MacCollision:
        return RoomError::MacCollision;
    case RoomMember::Error::ConsoleIdCollision:
        return RoomError::ConsoleIdCollision;
    case RoomMember::Error::WrongVersion:
        return RoomError::WrongVersion;
    case RoomMember::Error::WrongPassword:
        return RoomError::WrongPassword;
    case RoomMember::Error::CouldNotConnect:
        return RoomError::CouldNotConnect;
    case RoomMember::Error::RoomIsFull:
        return RoomError::RoomIsFull;
    case RoomMember::Error::HostBanned:
        return RoomError::HostBanned;
    case RoomMember::Error::PermissionDenied:
        return RoomError::PermissionDenied;
    case RoomMember::Error::NoSuchUser:
        return RoomError::NoSuchUser;
    }
    return RoomError::Unknown;
}

RoomState ConvertState(RoomMember::State state) {
    switch (state) {
    case RoomMember::State::Joining:
        return RoomState::Joining;
    case RoomMember::State::Joined:
        return RoomState::Joined;
    case RoomMember::State::Moderator:
        return RoomState::Moderator;
    case RoomMember::State::Uninitialized:
    case RoomMember::State::Idle:
        return RoomState::Idle;
    }
    return RoomState::Idle;
}

std::vector<RoomMemberInfo> ConvertMembers(const RoomMember::MemberList& list) {
    std::vector<RoomMemberInfo> members;
    members.reserve(list.size());
    for (const auto& m : list) {
        members.push_back({m.nickname, m.username, m.game_info.name, m.mac_address});
    }
    return members;
}

RoomDetails ConvertDetails(const Network::RoomInformation& info) {
    return {info.name, info.member_slots};
}

void StopListing() {
#ifdef ENABLE_WEB_SERVICE
    std::shared_ptr<Network::AnnounceMultiplayerSession> announce;
    {
        auto& n = N();
        std::scoped_lock lock{n.announce_mutex};
        announce = std::move(n.announce);
    }
    if (announce) {
        announce->Stop();
    }
#endif
}

} // namespace

void AzaharRuntime::InitNetwork() {
    Network::Init();
}

void AzaharRuntime::ShutdownNetwork() {
    Network::Shutdown();
}

bool AzaharRuntime::RoomsAvailable() const {
    return Network::GetRoomMember().lock() && Network::GetRoom().lock();
}

void AzaharRuntime::PrepareConsoleData() {
    FileUtil::CreateFullPath(FileUtil::GetUserPath(FileUtil::UserPath::SysDataDir));
}

std::string AzaharRuntime::ConsoleUsername() const {
    return Service::CFG::GetUsername(Core::System::GetInstance());
}

std::string AzaharRuntime::ConsoleIdHash() const {
    return Service::CFG::GetConsoleIdHash(Core::System::GetInstance());
}

MacAddress AzaharRuntime::ConsoleMac() const {
    return Service::CFG::GetConsoleMacAddress(Core::System::GetInstance());
}

bool AzaharRuntime::IsGameRunning() const {
    return Core::System::GetInstance().IsPoweredOn();
}

bool AzaharRuntime::SetConsoleMac(const MacAddress& mac) {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        return false;
    }
    auto& page = system.Kernel().GetSharedPageHandler();
    if (page.GetMacAddress() == mac) {
        return false;
    }
    page.SetMacAddress(mac);
    return true;
}

void AzaharRuntime::BindRoomCallbacks(RoomCallbacks callbacks) {
    const auto member = Network::GetRoomMember().lock();
    if (!member) {
        return;
    }
    auto& n = N();
    n.callbacks = std::make_shared<const RoomCallbacks>(std::move(callbacks));
    const auto cb = n.callbacks;
    n.on_error = member->BindOnError([cb](const RoomMember::Error& error) {
        if (cb->on_error) {
            cb->on_error(ConvertError(error));
        }
    });
    n.on_status = member->BindOnStatusMessageReceived([cb](const Network::StatusMessageEntry& msg) {
        if (cb->on_member && (msg.type == Network::IdMemberJoin || msg.type == Network::IdMemberLeave)) {
            cb->on_member(msg.type == Network::IdMemberJoin, msg.nickname);
        }
    });
    // the member list is read on the network's thread, right after it changed
    n.on_info = member->BindOnRoomInformationChanged([cb](const Network::RoomInformation& info) {
        if (!cb->on_info) {
            return;
        }
        const auto member = Network::GetRoomMember().lock();
        cb->on_info(ConvertDetails(info),
                    member ? ConvertMembers(member->GetMemberInformation()) : std::vector<RoomMemberInfo>{});
    });
    n.on_state = member->BindOnStateChanged([cb](const RoomMember::State& state) {
        if (cb->on_state) {
            cb->on_state(ConvertState(state));
        }
    });
    n.on_wifi = member->BindOnWifiPacketReceived([cb](const Network::WifiPacket& packet) {
        if (!cb->on_packet) {
            return;
        }
        RoomCallbacks::Packet kind = RoomCallbacks::Packet::Other;
        if (packet.type == NET_SYNC_PACKET) {
            kind = RoomCallbacks::Packet::NetSync;
        } else if (packet.type == Network::WifiPacket::PacketType::Beacon) {
            kind = RoomCallbacks::Packet::Beacon;
        } else if (packet.type == Network::WifiPacket::PacketType::Data) {
            kind = RoomCallbacks::Packet::Data;
        }
        cb->on_packet(packet.transmitter_address, kind);
    });
}

void AzaharRuntime::UnbindRoomCallbacks() {
    auto& n = N();
    if (const auto member = Network::GetRoomMember().lock()) {
        member->Unbind(n.on_error);
        member->Unbind(n.on_status);
        member->Unbind(n.on_info);
        member->Unbind(n.on_state);
        member->Unbind(n.on_wifi);
    }
}

void AzaharRuntime::JoinRoom(const RoomJoinRequest& request) {
    const auto member = Network::GetRoomMember().lock();
    if (!member) {
        return;
    }
    member->Join(request.nickname, request.console_id, request.host.c_str(),
                 static_cast<u16>(request.port), 0,
                 request.preferred_mac ? *request.preferred_mac : Network::NoPreferredMac,
                 request.password);
}

void AzaharRuntime::LeaveRoom() {
    if (const auto member = Network::GetRoomMember().lock()) {
        member->Leave();
    }
}

RoomState AzaharRuntime::RoomMemberState() const {
    const auto member = Network::GetRoomMember().lock();
    return member ? ConvertState(member->GetState()) : RoomState::Idle;
}

bool AzaharRuntime::RoomConnected() const {
    const auto member = Network::GetRoomMember().lock();
    return member && member->IsConnected();
}

MacAddress AzaharRuntime::RoomMacAddress() const {
    const auto member = Network::GetRoomMember().lock();
    return member ? member->GetMacAddress() : MacAddress{};
}

std::string AzaharRuntime::RoomNickname() const {
    const auto member = Network::GetRoomMember().lock();
    return member ? member->GetNickname() : std::string{};
}

RoomDetails AzaharRuntime::RoomInformation() const {
    const auto member = Network::GetRoomMember().lock();
    return member ? ConvertDetails(member->GetRoomInformation()) : RoomDetails{};
}

std::vector<RoomMemberInfo> AzaharRuntime::RoomMembers() const {
    const auto member = Network::GetRoomMember().lock();
    return member ? ConvertMembers(member->GetMemberInformation()) : std::vector<RoomMemberInfo>{};
}

bool AzaharRuntime::HostingRoom() const {
    const auto room = Network::GetRoom().lock();
    return room && room->GetState() == Network::Room::State::Open;
}

RoomOpenResult AzaharRuntime::OpenRoom(const RoomOpenRequest& request) {
    RoomOpenResult result;
    const auto room = Network::GetRoom().lock();
    if (!room) {
        return result;
    }
    std::unique_ptr<Network::VerifyUser::Backend> verify =
        std::make_unique<Network::VerifyUser::NullBackend>();
#ifdef ENABLE_WEB_SERVICE
    if (!request.lobby_url.empty()) {
        Settings::values.web_api_url = request.lobby_url;
        Settings::values.network_token = request.token;
        verify = std::make_unique<WebService::VerifyUserJWT>(Settings::values.web_api_url.GetValue());
    }
#endif
    if (!room->Create(request.name, "", "", static_cast<u16>(request.port), request.password,
                      static_cast<u32>(request.slots), request.host_nickname, request.game_name,
                      request.game_id, std::move(verify))) {
        return result;
    }
    result.ok = true;
#ifdef ENABLE_WEB_SERVICE
    if (!request.lobby_url.empty()) {
        auto announce = std::make_shared<Network::AnnounceMultiplayerSession>(request.host_nickname);
        const auto registered = announce->Register();
        if (registered.result_code != Common::WebResult::Code::Success) {
            result.listing = RoomOpenResult::Listing::Failed;
            result.listing_error = registered.result_string;
        } else {
            announce->Start();
            result.listing = RoomOpenResult::Listing::Listed;
            auto& n = N();
            std::scoped_lock lock{n.announce_mutex};
            n.announce = std::move(announce);
        }
    }
#endif
    return result;
}

std::vector<std::string> AzaharRuntime::EndSkipPending() {
    return Port::NetSync::EndSkipPending();
}

void AzaharRuntime::CloseRoom() {
    StopListing();
    if (const auto room = Network::GetRoom().lock()) {
        room->Destroy();
    }
}

} // namespace App::Azahar
