// Derived from Azahar src/core/hle/service/soc/soc_u.cpp and src/core/hle/service/soc/soc_u.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the command table of soc:U with its session limit, the 3DS socket error numbers
// (error_map), the translations of domains, types, protocols, options and flags, the 3DS poll event
// bits (CTRPollFD) with the libctru POLLOUT quirk, the layouts CTRSockAddr, CTRAddrInfo,
// HostByNameData and CTRLinger, the process and descriptor checks (GetSocketHolder), the replies of
// every command and the network option answers (GetNetworkOpt).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (soc_u.cpp)
// Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (soc_u.h)
//
// soc:U (N1-19): sockets. Azahar passes every call to the host's sockets, so what it answers depends on
// the host. This service answers as Azahar does on a host whose network is down: there is no
// interface, not even a loopback one (so Azahar's default-interface lookup, which connects to
// 8.8.8.8, fails and GetHostId and the IP information answer zeros), no address can be reached and
// no name can be resolved. The model is that of a Linux host, whose numbers Azahar's translation
// tables are written against:
//  - sockets of the AF_INET family, stream or datagram, are created and closed, and are bound only to
//    0.0.0.0 (or a broadcast or multicast address: the kernel accepts those without an interface);
//    any other address is EADDRNOTAVAIL; two sockets of one type cannot share a port unless both set
//    SO_REUSEADDR; port 0 takes the next port from 49152 up;
//  - connect and sending to an address are ENETUNREACH, sending on an unconnected stream socket is
//    EPIPE, without an address on a datagram socket EDESTADDRREQ; receiving on a stream socket is
//    ENOTCONN; receiving on a datagram socket is EAGAIN on a non-blocking socket (or with the "don't
//    wait" flag) and otherwise waits, without end, until the socket is closed (EBADF) or shut down for
//    reading (0 bytes);
//  - shutdown on an unconnected socket is ENOTCONN, but the kernel keeps the direction closed, which
//    wakes a waiting receiver (as Azahar's busy-wait on it relies on); poll answers what Linux's
//    tcp_poll and datagram_poll answer for such a socket;
//  - names: dotted quads and "localhost" resolve, anything else fails (gethostbyname with the errno
//    of the failed lookup: ENETUNREACH; getaddrinfo with EAI_AGAIN); getnameinfo gives numeric names.
// Blocking calls wait in game time: a poll with a timeout answers when the timeout has passed on the
// timer queue, in exact ticks; one without a timeout answers when a socket it watches is closed.
//
// Departures from Azahar: the answers are those of the model above, not of the host; bytes of an
// address reply past the fields Azahar fills (it leaves them uninitialised) are zero; a stale static
// buffer descriptor in a reply that Azahar does not complete (an error answer of gethostbyname and
// the like) is replaced by an empty static buffer; an address of a length or family Azahar asserts
// on is EINVAL or EAFNOSUPPORT; the sockets are shared by the service's sessions, as they are in
// Azahar (one service object).
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/ipc.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n::soc {

namespace {

constexpr const char* kLog = "Service.SOC";

// ---- results ------------------------------------------------------------------------------------------

constexpr u32 kModuleSoc = 28;
constexpr Result kResultWrongProcess = MakeResult(4, kModuleSoc, 5, 25);
constexpr Result kResultNotInitialized = MakeResult(6, kModuleSoc, 7, 27);
constexpr Result kResultInvalidSocketDescriptor = MakeResult(7, kModuleSoc, 7, 27);
constexpr Result kResultAlreadyInitialized = MakeResult(11, kModuleSoc, 5, 25);
constexpr Result kResultUnimplemented = MakeResult(1012, kModuleSoc, 6, 27);  // UnimplementedFunction
constexpr u32 kErrInvalidEnumValue = 0xFFFF8025;                                // SOC_ERR_INAVLID_ENUM_VALUE

// The 3DS socket error numbers (the values of Azahar's error_map), returned negated.
enum Errno : s32 {
    kEaccess = 2,
    kEaddrInUse = 3,
    kEaddrNotAvail = 4,
    kEafNoSupport = 5,
    kEagain = 6,
    kEbadf = 8,
    kEdestAddrReq = 17,
    kEinval = 28,
    kEmfile = 33,
    kEmsgSize = 35,
    kEnetUnreach = 40,
    kEnoDev = 44,
    kEnoProtoOpt = 51,
    kEnotConn = 56,
    kEopNotSupp = 63,
    kEpipe = 66,
    kEaiAgain = 302,
    kEaiNoName = 305,
};
constexpr s32 Err(Errno e) {
    return -static_cast<s32>(e);
}

// ---- constants on both sides --------------------------------------------------------------------------

// 3DS side
constexpr u32 kDomainInet = 2;
constexpr u32 kDomainInet6 = 23;
constexpr u32 kTypeStream = 1;
constexpr u32 kTypeDgram = 2;
constexpr u32 kSolIp = 0x0000;
constexpr u32 kSolTcp = 0x0006;
constexpr u32 kSolConfig = 0xFFFE;
constexpr u32 kSolSocket = 0xFFFF;
constexpr s32 kTtlDefault = 64;
constexpr u32 kMsgOob = 1, kMsgPeek = 2, kMsgDontWait = 4;
constexpr u32 kNetoptMac = 0x1004;
constexpr u32 kNetoptIpInfo = 0x4003;

// Linux side (the platform numbers Azahar's tables translate to)
constexpr int kLinSolSocket = 1;
constexpr int kLinSoReuseAddr = 2, kLinSoType = 3, kLinSoError = 4, kLinSoSndBuf = 7,
              kLinSoRcvBuf = 8, kLinSoOobInline = 10, kLinSoLinger = 13, kLinSoRcvLowat = 18,
              kLinSoSndLowat = 19;
constexpr int kLinIpprotoIp = 0, kLinIpTos = 1, kLinIpTtl = 2, kLinIpMulticastTtl = 33,
              kLinIpMulticastLoop = 34, kLinIpAddMembership = 35, kLinIpDropMembership = 36;
constexpr int kLinIpprotoTcp = 6, kLinTcpNodelay = 1;
constexpr u32 kPollPri = 0x2, kPollErr = 0x8, kPollHup = 0x10, kPollNval = 0x20, kPollRdNorm = 0x40,
              kPollWrNorm = 0x100, kPollWrBand = 0x200, kPollIn = 0x1, kPollOut = 0x4,
              kPollRdHup = 0x2000, kPollRdBand = 0x80;

// ---- addresses -------------------------------------------------------------------------------------------

constexpr std::size_t kSockAddrSize = 28;  // sizeof(CTRSockAddr)

struct Address {
    bool valid = false;
    u32 family = 0;      // 3DS value: 2 or 23
    u16 raw_port = 0;    // as the guest wrote it (network byte order)
    u32 raw_addr = 0;    // as the guest wrote it
};

std::array<u8, kSockAddrSize> MakeInetAddress(u16 raw_port, u32 raw_addr) {
    std::array<u8, kSockAddrSize> out{};
    out[0] = 8;
    out[1] = kDomainInet;
    std::memcpy(&out[2], &raw_port, 2);
    std::memcpy(&out[4], &raw_addr, 4);
    return out;
}

Address ParseAddress(const StaticBuffer* buffer, u32 len) {
    Address address;
    std::array<u8, kSockAddrSize> bytes{};
    if (buffer != nullptr) {
        std::memcpy(bytes.data(), buffer->data.data(),
                    std::min<std::size_t>({static_cast<std::size_t>(len), kSockAddrSize, buffer->data.size()}));
    }
    // Azahar asserts that the length byte is the size of a sockaddr_in (8) or sockaddr_in6 (28).
    if (bytes[0] != 8 && bytes[0] != 28) {
        return address;
    }
    address.valid = true;
    address.family = bytes[1];
    std::memcpy(&address.raw_port, &bytes[2], 2);
    std::memcpy(&address.raw_addr, &bytes[4], 4);
    return address;
}

u16 Swap16(u16 value) {
    return static_cast<u16>(value >> 8 | value << 8);
}

// ---- the state --------------------------------------------------------------------------------------------

struct OptionValue {
    std::vector<u8> bytes;
};

struct Socket {
    u32 type = kTypeStream;  // 3DS value (equal to Linux's SOCK_STREAM and SOCK_DGRAM)
    bool blocking = true;
    bool is_global = false;
    bool listening = false;
    bool bound = false;
    u16 bound_raw_port = 0;
    u32 bound_raw_addr = 0;
    bool shutdown_rd = false;  // the kernel's RCV_SHUTDOWN
    bool shutdown_wr = false;  // SEND_SHUTDOWN
    u32 owner_process = 0;
    std::map<std::pair<int, int>, s32> int_options;
    bool linger_set = false;
    std::array<s32, 2> linger{};
};

// A request that waits: a receive or an accept on one socket, or a poll.
struct Pending {
    std::shared_ptr<Request> request;
    u32 socket = 0;                         // a receive or accept: the socket it waits on
    std::function<Reply(s32 ret)> finish;   // a receive or accept: the reply for this result
    // A poll
    bool is_poll = false;
    std::vector<std::array<u32, 3>> fds;    // fd, events (3DS bits), revents
    std::vector<u8> libctru_bug;
    u64 timer = 0;
    bool done = false;
};

struct SocState {
    std::map<u32, Socket> sockets;
    std::set<u32> initialized;
    u32 next_socket_id = 3;
    u32 next_ephemeral_port = 49152;
    std::vector<std::shared_ptr<Pending>> pending;
};

// ---- the offline host's option model ----------------------------------------------------------------------------

struct PlatformOption {
    int level;
    int name;
};

// Azahar's sockopt_map: (3DS level, option) -> (platform level, platform option); -1 means no effect.
PlatformOption TranslateSockOpt(u32 level, s32 opt) {
    struct Entry {
        u32 level;
        s32 opt;
        int plevel;
        int popt;
    };
    static constexpr Entry kMap[] = {
        {kSolIp, 0x0007, kLinIpprotoIp, kLinIpTos},
        {kSolIp, 0x0008, kLinIpprotoIp, kLinIpTtl},
        {kSolIp, 0x0009, kLinIpprotoIp, kLinIpMulticastLoop},
        {kSolIp, 0x000A, kLinIpprotoIp, kLinIpMulticastTtl},
        {kSolIp, 0x000B, kLinIpprotoIp, kLinIpAddMembership},
        {kSolIp, 0x000C, kLinIpprotoIp, kLinIpDropMembership},
        {kSolSocket, 0x0004, kLinSolSocket, kLinSoReuseAddr},
        {kSolSocket, 0x0080, kLinSolSocket, kLinSoLinger},
        {kSolSocket, 0x0100, kLinSolSocket, kLinSoOobInline},
        {kSolSocket, 0x1001, kLinSolSocket, kLinSoSndBuf},
        {kSolSocket, 0x1002, kLinSolSocket, kLinSoRcvBuf},
        {kSolSocket, 0x1003, kLinSolSocket, kLinSoSndLowat},
        {kSolSocket, 0x1004, kLinSolSocket, kLinSoRcvLowat},
        {kSolSocket, 0x1008, kLinSolSocket, kLinSoType},
        {kSolSocket, 0x1009, kLinSolSocket, kLinSoError},
        {kSolTcp, 0x2001, kLinIpprotoTcp, kLinTcpNodelay},
        {kSolTcp, 0x2002, kLinIpprotoTcp, -1},
        {kSolTcp, 0x2003, kLinIpprotoTcp, -1},
    };
    for (const Entry& entry : kMap) {
        if (entry.level == level && entry.opt == opt) {
            return {entry.plevel, entry.popt};
        }
    }
    return {kLinSolSocket, opt};
}

bool IsKnownOption(int level, int name) {
    switch (level) {
    case kLinSolSocket:
        return name == kLinSoReuseAddr || name == kLinSoType || name == kLinSoError ||
               name == kLinSoSndBuf || name == kLinSoRcvBuf || name == kLinSoOobInline ||
               name == kLinSoLinger || name == kLinSoRcvLowat || name == kLinSoSndLowat;
    case kLinIpprotoIp:
        return name == kLinIpTos || name == kLinIpTtl || name == kLinIpMulticastTtl ||
               name == kLinIpMulticastLoop || name == kLinIpAddMembership ||
               name == kLinIpDropMembership;
    case kLinIpprotoTcp:
        return name == kLinTcpNodelay;
    default:
        return false;
    }
}

// What getsockopt answers for an option that was never set (Linux's defaults for a fresh socket).
s32 DefaultOption(const Socket& socket, int level, int name) {
    const bool stream = socket.type == kTypeStream;
    switch (level) {
    case kLinSolSocket:
        switch (name) {
        case kLinSoType:
            return static_cast<s32>(socket.type);
        case kLinSoSndBuf:
            return stream ? 87040 : 212992;
        case kLinSoRcvBuf:
            return stream ? 131072 : 212992;
        case kLinSoRcvLowat:
        case kLinSoSndLowat:
            return 1;
        default:
            return 0;
        }
    case kLinIpprotoIp:
        switch (name) {
        case kLinIpTtl:
            return kTtlDefault;
        case kLinIpMulticastTtl:
        case kLinIpMulticastLoop:
            return 1;
        default:
            return 0;
        }
    default:
        return 0;
    }
}

std::vector<u8> IntBytes(s32 value) {
    std::vector<u8> bytes(4);
    std::memcpy(bytes.data(), &value, 4);
    return bytes;
}

// ---- the 3DS poll structure -----------------------------------------------------------------------------------

// CTRPollFD::Events bits
constexpr u32 kEvRdNorm = 1u << 0, kEvPri = 1u << 2, kEvWrNorm = 1u << 3, kEvWrBand = 1u << 4,
              kEvErr = 1u << 5, kEvHup = 1u << 6, kEvNval = 1u << 7;

// Events::TranslateTo3DS: platform bits -> 3DS bits.
u32 PollTo3ds(u32 platform, bool libctru_bug) {
    u32 out = 0;
    if (platform & kPollRdNorm) out |= kEvRdNorm;
    if (platform & kPollRdBand) out |= kEvPri;  // "the 3DS OS mistakenly uses POLLRDBAND as POLLPRI"
    if (platform & kPollHup) out |= kEvHup;
    if (platform & kPollErr) out |= kEvErr;
    if (platform & kPollWrNorm) out |= libctru_bug ? kEvWrBand : kEvWrNorm;
    if (platform & kPollWrBand) out |= kEvWrBand;
    if (platform & kPollNval) out |= kEvNval;
    return out;
}

// Events::TranslateToPlatform (Linux: not Windows).
u32 PollToPlatform(u32 ev, bool is_output, u8* has_libctru_bug) {
    *has_libctru_bug = 0;
    if (!(ev & kEvWrNorm) && (ev & kEvWrBand)) {
        // a bug in libctru and some homebrew using libcurl: the wrong value for POLLOUT
        ev = (ev | kEvWrNorm) & ~kEvWrBand;
        *has_libctru_bug = 1;
    }
    u32 out = 0;
    if (ev & kEvRdNorm) out |= kPollRdNorm;
    if (ev & kEvPri) out |= kPollPri;
    if ((ev & kEvHup) && is_output) out |= kPollHup;
    if ((ev & kEvErr) && is_output) out |= kPollErr;
    if (ev & kEvWrNorm) out |= kPollWrNorm;
    if (ev & kEvWrBand) out |= kPollWrBand;
    if ((ev & kEvNval) && is_output) out |= kPollNval;
    return out;
}

// What Linux's tcp_poll and datagram_poll report for a socket of the offline host.
u32 SocketPollMask(const Socket& socket) {
    u32 mask = 0;
    if (socket.type == kTypeDgram) {
        if (socket.shutdown_rd) mask |= kPollRdHup | kPollIn | kPollRdNorm;
        if (socket.shutdown_rd && socket.shutdown_wr) mask |= kPollHup;
        mask |= kPollOut | kPollWrNorm | kPollWrBand;
        return mask;
    }
    if (socket.listening) {
        return 0;
    }
    // a stream socket that was never connected is in the CLOSE state
    mask |= kPollHup;
    if (socket.shutdown_rd) mask |= kPollRdHup | kPollIn | kPollRdNorm;
    if (!socket.shutdown_wr) mask |= kPollOut | kPollWrNorm | kPollWrBand;
    return mask;
}

// ---- names ----------------------------------------------------------------------------------------------------------

bool ParseDottedQuad(const std::string& text, u32* raw_addr) {
    u32 parts[4] = {};
    std::size_t at = 0;
    for (int i = 0; i < 4; ++i) {
        if (at >= text.size() || text[at] < '0' || text[at] > '9') return false;
        u32 value = 0;
        std::size_t digits = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            value = value * 10 + static_cast<u32>(text[at] - '0');
            if (value > 255 || ++digits > 3) return false;
            ++at;
        }
        parts[i] = value;
        if (i < 3) {
            if (at >= text.size() || text[at] != '.') return false;
            ++at;
        }
    }
    if (at != text.size()) return false;
    const u8 bytes[4] = {static_cast<u8>(parts[0]), static_cast<u8>(parts[1]),
                         static_cast<u8>(parts[2]), static_cast<u8>(parts[3])};
    std::memcpy(raw_addr, bytes, 4);
    return true;
}

// A name the offline host resolves without a network: a dotted quad, or localhost (127.0.0.1).
bool ResolveOffline(const std::string& name, u32* raw_addr) {
    if (name == "localhost") {
        const u8 loopback[4] = {127, 0, 0, 1};
        std::memcpy(raw_addr, loopback, 4);
        return true;
    }
    return ParseDottedQuad(name, raw_addr);
}

std::string DottedQuad(u32 raw_addr) {
    u8 bytes[4];
    std::memcpy(bytes, &raw_addr, 4);
    return fmt::format("{}.{}.{}.{}", bytes[0], bytes[1], bytes[2], bytes[3]);
}

std::string CString(const StaticBuffer* buffer, std::size_t limit) {
    if (buffer == nullptr) return {};
    const std::size_t size = std::min(limit, buffer->data.size());
    const char* text = reinterpret_cast<const char*>(buffer->data.data());
    return std::string(text, ::strnlen(text, size));
}

// ---- waiting requests -----------------------------------------------------------------------------------------------

// Fills in the revents of a poll and returns the number of ready descriptors.
u32 EvaluatePoll(const SocState& state, Pending& pending) {
    u32 ready = 0;
    for (std::size_t i = 0; i < pending.fds.size(); ++i) {
        u8 unused = 0;
        const u32 requested = PollToPlatform(pending.fds[i][1], false, &unused);
        u32 revents = 0;
        const auto it = state.sockets.find(pending.fds[i][0]);
        if (it == state.sockets.end()) {
            revents = kPollNval;
        } else {
            revents = SocketPollMask(it->second) & (requested | kPollErr | kPollHup | kPollNval);
        }
        pending.fds[i][2] = PollTo3ds(revents, pending.libctru_bug[i] != 0);
        if (revents != 0) ++ready;
    }
    return ready;
}

Reply PollReply(const Pending& pending, s32 ret) {
    std::vector<u8> out(pending.fds.size() * 12);
    for (std::size_t i = 0; i < pending.fds.size(); ++i) {
        std::memcpy(out.data() + i * 12, pending.fds[i].data(), 12);
    }
    Reply reply(0x0014, 2, 2);
    reply.Push(kResultSuccess);
    reply.Push(ret);
    reply.PushStaticBuffer(std::move(out), 0);
    return reply;
}

// Completes a waiting request with its reply.
void Finish(SocState& state, Kernel& kernel, const std::shared_ptr<Pending>& pending, Reply reply) {
    if (pending->done) return;
    pending->done = true;
    if (pending->timer != 0) {
        kernel.Cancel(pending->timer);
        pending->timer = 0;
    }
    std::erase(state.pending, pending);
    mh4n::Complete(pending->request, std::move(reply));
}

void FinishPoll(SocState& state, Kernel& kernel, const std::shared_ptr<Pending>& pending) {
    const u32 ready = EvaluatePoll(state, *pending);
    Finish(state, kernel, pending, PollReply(*pending, static_cast<s32>(ready)));
}

// Wakes what waits on a socket that has been closed or shut down.
void WakeSocket(SocState& state, Kernel& kernel, u32 socket, s32 ret, bool closed) {
    const std::vector<std::shared_ptr<Pending>> all = state.pending;
    for (const std::shared_ptr<Pending>& pending : all) {
        if (!pending->is_poll && pending->socket == socket) {
            Finish(state, kernel, pending, pending->finish(ret));
        } else if (pending->is_poll && closed) {
            for (const auto& fd : pending->fds) {
                if (fd[0] == socket) {
                    FinishPoll(state, kernel, pending);
                    break;
                }
            }
        }
    }
}

void CloseSocketNow(SocState& state, Kernel& kernel, u32 handle) {
    state.sockets.erase(handle);
    WakeSocket(state, kernel, handle, Err(kEbadf), true);
}

void CloseAllSockets(SocState& state, Kernel& kernel, u32 pid) {
    std::vector<u32> handles;
    for (const auto& [handle, socket] : state.sockets) {
        if (socket.owner_process == pid) handles.push_back(handle);
    }
    for (const u32 handle : handles) CloseSocketNow(state, kernel, handle);
}

// ---- the service ------------------------------------------------------------------------------------------------------

class SocU final : public Service {
public:
    explicit SocU(ServiceContext& context)
        : Service(context, "soc:U"), st_(context.State<SocState>("soc:U")) {
        Register(0x0001, "InitializeSockets", &SocU::InitializeSockets);
        Register(0x0002, "Socket", &SocU::SocketCmd);
        Register(0x0003, "Listen", &SocU::Listen);
        Register(0x0004, "Accept", &SocU::Accept);
        Register(0x0005, "Bind", &SocU::Bind);
        Register(0x0006, "Connect", &SocU::Connect);
        Register(0x0007, "recvfrom_other", &SocU::RecvFromOther);
        Register(0x0008, "RecvFrom", &SocU::RecvFrom);
        Register(0x0009, "SendToOther", &SocU::SendToOther);
        Register(0x000A, "SendToSingle", &SocU::SendToSingle);
        Register(0x000B, "Close", &SocU::Close);
        Register(0x000C, "Shutdown", &SocU::Shutdown);
        Register(0x000D, "GetHostByName", &SocU::GetHostByName);
        Register(0x000E, "GetHostByAddr", &SocU::GetHostByAddr);
        Register(0x000F, "GetAddrInfo", &SocU::GetAddrInfo);
        Register(0x0010, "GetNameInfo", &SocU::GetNameInfo);
        Register(0x0011, "GetSockOpt", &SocU::GetSockOpt);
        Register(0x0012, "SetSockOpt", &SocU::SetSockOpt);
        Register(0x0013, "Fcntl", &SocU::Fcntl);
        Register(0x0014, "Poll", &SocU::Poll);
        Register(0x0015, "SockAtMark", &SocU::SockAtMark);
        Register(0x0016, "GetHostId", &SocU::GetHostId);
        Register(0x0017, "GetSockName", &SocU::GetSockName);
        Register(0x0018, "GetPeerName", &SocU::GetPeerName);
        Register(0x0019, "ShutdownSockets", &SocU::ShutdownSockets);
        Register(0x001A, "GetNetworkOpt", &SocU::GetNetworkOpt);
        Register(0x001B, "ICMPSocket");
        Register(0x001C, "ICMPPing");
        Register(0x001D, "ICMPCancel");
        Register(0x001E, "ICMPClose");
        Register(0x001F, "GetResolverInfo");
        Register(0x0020, "SendToMultiple", &SocU::SendToMultiple);
        Register(0x0021, "CloseSockets", &SocU::CloseSockets);
        Register(0x0023, "AddGlobalSocket", &SocU::AddGlobalSocket);
    }

private:
    // ---- helpers ----

    // GetSocketHolder: the socket, or null after replying with the error.
    Socket* Holder(Request& request, u32 handle, u32 pid) {
        if (!st_->initialized.contains(pid)) {
            MH4N_LOG(Debug, kLog, "Process not initialized: pid={}", pid);
            request.MakeReply(1, 0).Push(kResultNotInitialized);
            return nullptr;
        }
        const auto found = st_->sockets.find(handle);
        if (found == st_->sockets.end()) {
            MH4N_LOG(Debug, kLog, "Invalid socket: pid={}, fd={}", pid, handle);
            request.MakeReply(1, 0).Push(kResultInvalidSocketDescriptor);
            return nullptr;
        }
        if (found->second.owner_process != pid && !found->second.is_global) {
            MH4N_LOG(Debug, kLog, "Invalid process owner: pid={}, fd={}", pid, handle);
            request.MakeReply(1, 0).Push(kResultWrongProcess);
            return nullptr;
        }
        return &found->second;
    }

    static void ReplyRet(Request& request, s32 ret) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(ret);
    }

    static std::vector<u8> Truncated(std::array<u8, kSockAddrSize> address, u32 max_len) {
        std::vector<u8> bytes(address.begin(), address.end());
        if (bytes.size() > max_len) {
            MH4N_LOG(Debug, kLog, "CTRSockAddr is too long, truncating data.");
            bytes.resize(max_len);
        }
        return bytes;
    }

    Kernel& GetKernel() { return Context().GetKernel(); }

    // ---- commands ----

    // 0x00010044: memory block size, the calling PID, the shared memory. Reply: result.
    void InitializeSockets(Request& request) {
        request.Pop<u32>();
        const u32 pid = request.PopPid();
        request.PopObject<SharedMemory>();
        Result result = kResultSuccess;
        if (!st_->initialized.contains(pid)) {
            st_->initialized.insert(pid);
        } else {
            result = kResultAlreadyInitialized;
        }
        MH4N_LOG(Debug, kLog, "called, pid={}, res={:#08X}", pid, result);
        request.MakeReply(1, 0).Push(result);
    }

    // 0x000200C2: domain, type, protocol, the calling PID. Reply: result, the socket or an error.
    void SocketCmd(Request& request) {
        const u32 domain = request.Pop<u32>();
        const u32 type = request.Pop<u32>();
        const u32 protocol = request.Pop<u32>();
        const u32 pid = request.PopPid();
        Reply& reply = request.MakeReply(2, 0);
        // Only protocol 0 is allowed (the OS decides), only AF_INET, only stream and datagram.
        if (protocol != 0 || domain != kDomainInet || (type != kTypeStream && type != kTypeDgram)) {
            reply.Push(kResultUnimplemented);
            reply.Push(request.Words()[2]);  // Azahar skips this word: it keeps the request's
            return;
        }
        s32 ret;
        if (st_->sockets.size() >= 1024) {
            ret = Err(kEmfile);
        } else {
            ret = static_cast<s32>(st_->next_socket_id++);
            Socket socket;
            socket.type = type;
            socket.owner_process = pid;
            st_->sockets[static_cast<u32>(ret)] = socket;
        }
        MH4N_LOG(Debug, kLog, "called, pid={}, ret={}", pid, ret);
        reply.Push(kResultSuccess);
        reply.Push(ret);
    }

    // The port numbers of sockets of one type that are in use.
    bool PortInUse(const Socket& self, u16 raw_port) const {
        for (const auto& [handle, other] : st_->sockets) {
            if (&other == &self || !other.bound || other.type != self.type ||
                other.bound_raw_port != raw_port) {
                continue;
            }
            const bool reuse_self = self.int_options.contains({kLinSolSocket, kLinSoReuseAddr}) &&
                                    self.int_options.at({kLinSolSocket, kLinSoReuseAddr}) != 0;
            const bool reuse_other = other.int_options.contains({kLinSolSocket, kLinSoReuseAddr}) &&
                                     other.int_options.at({kLinSolSocket, kLinSoReuseAddr}) != 0;
            if (other.listening || !(reuse_self && reuse_other)) return true;
        }
        return false;
    }

    s32 BindImpl(Socket& socket, const Address& address) {
        if (!address.valid || address.family != kDomainInet) return Err(kEafNoSupport);
        if (socket.bound) return Err(kEinval);
        const u8 first = static_cast<u8>(address.raw_addr & 0xFF);
        const bool any = address.raw_addr == 0;
        const bool broadcast = address.raw_addr == 0xFFFFFFFF;
        const bool multicast = first >= 224 && first <= 239;
        if (!any && !broadcast && !multicast) return Err(kEaddrNotAvail);
        u16 raw_port = address.raw_port;
        if (raw_port == 0) {
            raw_port = Swap16(static_cast<u16>(st_->next_ephemeral_port));
            for (u32 tries = 0; PortInUse(socket, raw_port) && tries < 20000; ++tries) {
                st_->next_ephemeral_port = st_->next_ephemeral_port >= 60999 ? 49152 : st_->next_ephemeral_port + 1;
                raw_port = Swap16(static_cast<u16>(st_->next_ephemeral_port));
            }
            st_->next_ephemeral_port = st_->next_ephemeral_port >= 60999 ? 49152 : st_->next_ephemeral_port + 1;
        } else if (PortInUse(socket, raw_port)) {
            return Err(kEaddrInUse);
        }
        socket.bound = true;
        socket.bound_raw_port = raw_port;
        socket.bound_raw_addr = address.raw_addr;
        return 0;
    }

    // 0x00050084: handle, address length, the calling PID, the address (static buffer).
    void Bind(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 len = request.Pop<u32>();
        const u32 pid = request.PopPid();
        const StaticBuffer* buffer = request.PopStaticBuffer();
        Socket* socket = Holder(request, handle, pid);
        if (socket == nullptr) return;
        const s32 ret = BindImpl(*socket, ParseAddress(buffer, len));
        MH4N_LOG(Debug, kLog, "called, pid={}, fd={}, ret={}", pid, handle, ret);
        ReplyRet(request, ret);
    }

    // 0x001300C2: handle, command, argument, the calling PID. F_GETFL (3) and F_SETFL (4).
    void Fcntl(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 cmd = request.Pop<u32>();
        const u32 arg = request.Pop<u32>();
        const u32 pid = request.PopPid();
        Socket* socket = Holder(request, handle, pid);
        if (socket == nullptr) return;
        u32 ret = 0;
        if (cmd == 3) {
            ret = socket->blocking ? 0 : 4;  // O_NONBLOCK
        } else if (cmd == 4) {
            socket->blocking = !(arg & 4);
        } else {
            MH4N_LOG(Error, kLog, "Unsupported command ({}) in fcntl call", cmd);
            ret = static_cast<u32>(Err(kEinval));
        }
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(ret);
    }

    void Listen(Request& request) {
        const u32 handle = request.Pop<u32>();
        request.Pop<u32>();  // backlog
        const u32 pid = request.PopPid();
        Socket* socket = Holder(request, handle, pid);
        if (socket == nullptr) return;
        s32 ret = 0;
        if (socket->type != kTypeStream) {
            ret = Err(kEopNotSupp);
        } else {
            if (!socket->bound) {
                Address any;
                any.valid = true;
                any.family = kDomainInet;
                BindImpl(*socket, any);
            }
            socket->listening = true;
        }
        ReplyRet(request, ret);
    }

    // 0x000400C2: handle, the longest address, the calling PID. Reply: result, the new socket or an
    // error, the peer address (static buffer 0).
    void Accept(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 max_addr_len = request.Pop<u32>();
        const u32 pid = request.PopPid();
        Socket* socket = Holder(request, handle, pid);
        if (socket == nullptr) return;
        const auto build = [max_addr_len](s32 ret) {
            Reply reply(0x0004, 2, 2);
            reply.Push(kResultSuccess);
            reply.Push(ret);
            std::array<u8, kSockAddrSize> none{};
            reply.PushStaticBuffer(Truncated(none, max_addr_len), 0);
            return reply;
        };
        if (socket->type != kTypeStream) return Answer(request, build(Err(kEopNotSupp)));
        if (!socket->listening) return Answer(request, build(Err(kEinval)));
        if (!socket->blocking) return Answer(request, build(Err(kEagain)));
        Wait(request, handle, build);  // no connection ever arrives
    }

    static void Answer(Request& request, Reply reply) {
        // An immediate answer: the reply of the request, built away from it.
        const ipc::Header header{reply.HeaderWord()};
        request.MakeReply(header.NormalParamsSize(), header.TranslateParamsSize()) = std::move(reply);
    }

    // Parks the request until the socket is closed or shut down.
    void Wait(Request& request, u32 socket, std::function<Reply(s32)> finish) {
        auto pending = std::make_shared<Pending>();
        pending->request = request.Share();
        pending->socket = socket;
        pending->finish = std::move(finish);
        st_->pending.push_back(pending);
        deferred_ = true;
    }

    Handled Handle(Request& request) override {
        deferred_ = false;
        const Handled handled = Service::Handle(request);
        if (handled == Handled::Done && deferred_) {
            deferred_ = false;
            return Handled::Deferred;
        }
        return handled;
    }

    // 0x00060082...: handle, address length, the calling PID, the address.
    void Connect(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 len = request.Pop<u32>();
        const u32 pid = request.PopPid();
        const StaticBuffer* buffer = request.PopStaticBuffer();
        Socket* socket = Holder(request, handle, pid);
        if (socket == nullptr) return;
        const Address address = ParseAddress(buffer, len);
        s32 ret;
        if (!address.valid) {
            ret = Err(kEinval);
        } else if (address.family != kDomainInet) {
            ret = Err(kEafNoSupport);
        } else if (socket->listening) {
            ret = Err(kEinval);  // Linux: EISCONN is for a connected stream; a listener cannot connect
        } else {
            ret = Err(kEnetUnreach);  // no route to anywhere
        }
        MH4N_LOG(Debug, kLog, "called, pid={}, fd={}, ret={}", pid, handle, ret);
        ReplyRet(request, ret);
    }

    // The result of receiving on a socket that has nothing to receive, or nullopt when the call
    // has to wait.
    std::optional<s32> RecvResult(const Socket& socket, u32 flags) const {
        if (flags & kMsgOob) return socket.type == kTypeStream ? Err(kEinval) : Err(kEopNotSupp);
        if (socket.type == kTypeStream) return Err(kEnotConn);
        if (socket.shutdown_rd) return 0;  // end of file
        if (!socket.blocking || (flags & kMsgDontWait)) return Err(kEagain);
        return std::nullopt;
    }

    // 0x00080102: handle, length, flags, address length, the calling PID. Reply: result, the result,
    // the bytes received, the data (static buffer 0), the address (static buffer 1).
    void RecvFrom(Request& request) {
        const u32 handle = request.Pop<u32>();
        request.Pop<u32>();  // length
        const u32 flags = request.Pop<u32>();
        const u32 addr_len = request.Pop<u32>();
        const u32 pid = request.PopPid();
        Socket* socket = Holder(request, handle, pid);
        if (socket == nullptr) return;
        const auto build = [addr_len](s32 ret) {
            Reply reply(0x0008, 3, 4);
            reply.Push(kResultSuccess);
            reply.Push(ret);
            reply.Push<s32>(ret > 0 ? ret : 0);
            reply.PushStaticBuffer({}, 0);
            reply.PushStaticBuffer(std::vector<u8>(std::min<u32>(addr_len, 0x1000), 0), 1);
            return reply;
        };
        const std::optional<s32> result = RecvResult(*socket, flags);
        if (result) return Answer(request, build(*result));
        Wait(request, handle, build);
    }

    // 0x00070104: handle, length, flags, address length, the calling PID, the data (mapped). Reply:
    // result, the result, the address (static buffer 0), the data buffer.
    void RecvFromOther(Request& request) {
        const u32 handle = request.Pop<u32>();
        request.Pop<u32>();
        const u32 flags = request.Pop<u32>();
        const u32 addr_len = request.Pop<u32>();
        const u32 pid = request.PopPid();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Socket* socket = Holder(request, handle, pid);
        if (socket == nullptr) return;
        const auto build = [addr_len, buffer](s32 ret) {
            Reply reply(0x0007, 2, 4);
            reply.Push(kResultSuccess);
            reply.Push(ret);
            reply.PushStaticBuffer(std::vector<u8>(std::min<u32>(addr_len, 0x1000), 0), 0);
            if (buffer != nullptr) {
                reply.PushMappedBuffer(*buffer);
            } else {
                reply.Push<u32>(0);
                reply.Push<u32>(0);
            }
            return reply;
        };
        const std::optional<s32> result = RecvResult(*socket, flags);
        if (result) return Answer(request, build(*result));
        Wait(request, handle, build);
    }

    // SendToImpl on the offline host.
    s32 SendResult(const Socket& socket, u32 len, u32 flags, u32 addr_len, const StaticBuffer* dest,
                   std::size_t dest_offset) const {
        (void)flags;
        (void)kMsgPeek;
        if (socket.type == kTypeStream) return Err(kEpipe);
        if (len > 65535) return Err(kEmsgSize);
        if (addr_len == 0) return Err(kEdestAddrReq);
        StaticBuffer view;
        if (dest != nullptr && dest_offset <= dest->data.size()) {
            view.data.assign(dest->data.begin() + static_cast<std::ptrdiff_t>(dest_offset), dest->data.end());
        }
        const Address address = ParseAddress(&view, addr_len);
        if (!address.valid) return Err(kEinval);
        if (address.family != kDomainInet) return Err(kEafNoSupport);
        return Err(kEnetUnreach);
    }

    // 0x00090106: handle, length, flags, address length, the calling PID, the destination (static
    // buffer), the data (mapped). Reply: result, the result, the data buffer.
    void SendToOther(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 len = request.Pop<u32>();
        const u32 flags = request.Pop<u32>();
        const u32 addr_len = request.Pop<u32>();
        const u32 pid = request.PopPid();
        const StaticBuffer* dest = request.PopStaticBuffer();
        const MappedBuffer* data = request.PopMappedBuffer();
        Socket* socket = Holder(request, handle, pid);
        if (socket == nullptr) return;
        const s32 ret = SendResult(*socket, len, flags, addr_len, dest, 0);
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push(ret);
        if (data != nullptr) {
            reply.PushMappedBuffer(*data);
        } else {
            reply.Push<u32>(0);
            reply.Push<u32>(0);
        }
    }

    // 0x000A0106: handle, length, flags, address length, the calling PID, the data and the
    // destination (static buffers).
    void SendToSingle(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 len = request.Pop<u32>();
        const u32 flags = request.Pop<u32>();
        const u32 addr_len = request.Pop<u32>();
        const u32 pid = request.PopPid();
        request.PopStaticBuffer();
        const StaticBuffer* dest = request.PopStaticBuffer();
        Socket* socket = Holder(request, handle, pid);
        if (socket == nullptr) return;
        ReplyRet(request, SendResult(*socket, len, flags, addr_len, dest, 0));
    }

    // 0x00200146: handle, length, flags, address length, total length of the addresses, the PID, the
    // data and the addresses (static buffers). Stops at the first failure.
    void SendToMultiple(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 len = request.Pop<u32>();
        const u32 flags = request.Pop<u32>();
        const u32 addr_len = request.Pop<u32>();
        const u32 total_addr_len = request.Pop<u32>();
        const u32 pid = request.PopPid();
        request.PopStaticBuffer();
        const StaticBuffer* dest = request.PopStaticBuffer();
        Socket* socket = Holder(request, handle, pid);
        if (socket == nullptr) return;
        const u32 count = addr_len == 0 ? 1 : total_addr_len / addr_len;
        s32 ret = 0;
        u32 i = 0;
        do {
            ret = SendResult(*socket, len, flags, addr_len, dest, static_cast<std::size_t>(i) * addr_len);
            ++i;
        } while (i < count && ret >= 0);
        ReplyRet(request, ret);
    }

    // 0x000B0042: handle, the calling PID.
    void Close(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 pid = request.PopPid();
        if (Holder(request, handle, pid) == nullptr) return;
        MH4N_LOG(Debug, kLog, "pid={}, fd={}, ret=0", pid, handle);
        CloseSocketNow(*st_, GetKernel(), handle);
        ReplyRet(request, 0);
    }

    // 0x000C0082: handle, how, the calling PID.
    void Shutdown(Request& request) {
        const u32 handle = request.Pop<u32>();
        const s32 how = request.Pop<s32>();
        const u32 pid = request.PopPid();
        Socket* socket = Holder(request, handle, pid);
        if (socket == nullptr) return;
        s32 ret;
        if (how < 0 || how > 2) {
            ret = Err(kEinval);
        } else {
            // The kernel records the shutdown even for a socket that was never connected, and
            // answers ENOTCONN; a listening socket that is shut down for reading is disconnected.
            const bool read = how == 0 || how == 2;
            const bool write = how == 1 || how == 2;
            if (socket->listening) {
                ret = 0;
                if (read) socket->listening = false;
            } else {
                ret = Err(kEnotConn);
                socket->shutdown_rd = socket->shutdown_rd || read;
                socket->shutdown_wr = socket->shutdown_wr || write;
            }
            if (read) WakeSocket(*st_, GetKernel(), handle, 0, false);
        }
        MH4N_LOG(Debug, kLog, "called, pid={}, fd={}, ret={}", pid, handle, ret);
        ReplyRet(request, ret);
    }

    static std::vector<u8> HostByNameData(const std::string& name, u32 raw_addr) {
        std::vector<u8> data(0x1A88, 0);
        // addr_type (AF_INET), addr_len, addr_count, alias_count, h_name[256], aliases, addresses
        data[0] = 2;
        data[2] = 4;
        data[4] = 1;
        std::memcpy(&data[8], name.data(), std::min<std::size_t>(name.size(), 255));
        std::memcpy(&data[8 + 256 + 24 * 256], &raw_addr, 4);
        return data;
    }

    // 0x000D0082: name length, output length, the name (static buffer). Reply: result, the result, the
    // host data (static buffer 0).
    void GetHostByName(Request& request) {
        request.Pop<u32>();
        request.Pop<u32>();
        const StaticBuffer* name_buffer = request.PopStaticBuffer();
        const std::string name = CString(name_buffer, 0x3FFFF);
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        u32 raw_addr = 0;
        if (ResolveOffline(name, &raw_addr)) {
            reply.Push<s32>(0);
            reply.PushStaticBuffer(HostByNameData(name, raw_addr), 0);
        } else {
            reply.Push(Err(kEnetUnreach));  // the errno the resolver leaves
            reply.PushStaticBuffer({}, 0);
        }
        MH4N_LOG(Debug, kLog, "called, name={}", name);
    }

    // 0x000E00C2...: address length, type, output length, the address (static buffer).
    void GetHostByAddr(Request& request) {
        request.Pop<u32>();
        request.Pop<u32>();
        request.Pop<u32>();
        request.PopStaticBuffer();
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push(Err(kEnetUnreach));
        reply.PushStaticBuffer({}, 0);
    }

    static u32 Be32(const u8* p) {
        return static_cast<u32>(p[0]) << 24 | static_cast<u32>(p[1]) << 16 | static_cast<u32>(p[2]) << 8 | p[3];
    }

    // 0x000F0306: node length, service length, hints size, output size, the node, the service and the
    // hints (static buffers). Reply: result, the result, the count, the entries (static buffer 0).
    void GetAddrInfo(Request& request) {
        const u32 node_length = request.Pop<u32>();
        const u32 service_length = request.Pop<u32>();
        const u32 hints_size = request.Pop<u32>();
        const u32 out_size = request.Pop<u32>();
        const StaticBuffer* node = request.PopStaticBuffer();
        const StaticBuffer* service = request.PopStaticBuffer();
        const StaticBuffer* hints = request.PopStaticBuffer();

        const bool has_node = node_length > 0;
        const bool has_service = service_length > 0;
        const std::string node_text = has_node ? CString(node, node_length) : std::string();
        const std::string service_text = has_service ? CString(service, service_length) : std::string();
        s32 flags = 0, family = 0, socktype = 0, protocol = 0;
        if (hints_size > 0 && hints != nullptr && hints->data.size() >= 16) {
            std::memcpy(&flags, hints->data.data(), 4);
            std::memcpy(&family, hints->data.data() + 4, 4);
            std::memcpy(&socktype, hints->data.data() + 8, 4);
            std::memcpy(&protocol, hints->data.data() + 12, 4);
            // Mimic what soc does in real hardware
            if (socktype != 0 && protocol == 0) protocol = socktype == 1 ? 6 : 17;
            if (protocol != 0 && socktype == 0) socktype = protocol == 6 ? 1 : 2;
        }

        s32 ret = 0;
        u32 raw_addr = 0;
        u16 raw_port = 0;
        if (!has_node && !has_service) {
            ret = -kEaiNoName;
        } else if (family == static_cast<s32>(kDomainInet6)) {
            ret = -9;  // EAI_ADDRFAMILY: a name or address with no IPv6 form
        } else if (has_node && (flags & 4) && !ParseDottedQuad(node_text, &raw_addr)) {
            ret = -kEaiNoName;  // AI_NUMERICHOST
        } else if (has_node && !ResolveOffline(node_text, &raw_addr)) {
            ret = -kEaiAgain;  // temporary failure in name resolution
        } else if (!has_node) {
            const u8 loopback[4] = {127, 0, 0, 1};
            if (!(flags & 1)) std::memcpy(&raw_addr, loopback, 4);  // AI_PASSIVE: any address
        }
        if (ret == 0 && has_service) {
            u32 port = 0;
            bool numeric = !service_text.empty() && service_text.size() <= 5;
            for (const char c : service_text) {
                numeric = numeric && c >= '0' && c <= '9';
                if (numeric) port = port * 10 + static_cast<u32>(c - '0');
            }
            if (!numeric || port > 65535) {
                ret = -8;  // EAI_SERVICE: only numeric services are known to the offline host
            } else {
                raw_port = Swap16(static_cast<u16>(port));
            }
        }

        const u32 out_limit = std::min<u32>(out_size, 0x3FFFF);
        std::vector<u8> out(ret == 0 ? out_limit : 0, 0);
        u32 count = 0;
        if (ret == 0) {
            struct Variant {
                s32 type, proto;
            };
            std::vector<Variant> variants;
            if (socktype == 0) {
                variants = {{1, 6}, {2, 17}, {-1, 0}};  // stream, datagram and raw (3DS has no raw)
            } else {
                variants = {{socktype, protocol}};
            }
            std::size_t pos = 0;
            for (const Variant& variant : variants) {
                if (sizeof(CTRAddrInfo) <= out_limit - pos) {
                    // ai_flags (the hints', 3DS bits), ai_family, ai_socktype, ai_protocol, ai_addrlen,
                    // ai_canonname[256], ai_addr
                    std::array<u8, sizeof(CTRAddrInfo)> entry{};
                    const s32 words[5] = {flags & 0xF, 2, variant.type, variant.proto, 8};
                    std::memcpy(entry.data(), words, sizeof(words));
                    if ((flags & 2) && count == 0 && has_node) {
                        std::memcpy(entry.data() + 20, node_text.data(), std::min<std::size_t>(node_text.size(), 255));
                    }
                    const auto addr = MakeInetAddress(raw_port, raw_addr);
                    std::memcpy(entry.data() + 20 + 256, addr.data(), addr.size());
                    std::memcpy(out.data() + pos, entry.data(), entry.size());
                    pos += entry.size();
                }
                ++count;
            }
        }
        Reply& reply = request.MakeReply(3, 2);
        reply.Push(kResultSuccess);
        reply.Push(ret);
        reply.Push(count);
        reply.PushStaticBuffer(std::move(out), 0);
    }

    struct CTRAddrInfo {
        u8 bytes[0x130];
    };

    // 0x00100102...: address length, host length, service length, flags, the address (static buffer).
    // Reply: result, the result, the host and the service (static buffers 0 and 1).
    void GetNameInfo(Request& request) {
        const u32 socklen = request.Pop<u32>();
        const u32 hostlen = request.Pop<u32>();
        const u32 servlen = request.Pop<u32>();
        const s32 flags = request.Pop<s32>();
        const StaticBuffer* sa = request.PopStaticBuffer();
        const Address address = ParseAddress(sa, socklen);
        std::vector<u8> host(hostlen, 0), serv(servlen, 0);
        s32 ret = 0;
        if (!address.valid || address.family != kDomainInet) {
            ret = -kEaiNoName;
        } else {
            std::string host_text = DottedQuad(address.raw_addr);
            const u8 loopback[4] = {127, 0, 0, 1};
            u32 raw_loopback;
            std::memcpy(&raw_loopback, loopback, 4);
            if (!(flags & 2) && address.raw_addr == raw_loopback) host_text = "localhost";
            if ((flags & 4) && host_text != "localhost" && !(flags & 2)) ret = -kEaiNoName;  // NI_NAMEREQD
            const std::string serv_text = std::to_string(Swap16(address.raw_port));
            if (ret == 0 && hostlen > 0 && host_text.size() + 1 > hostlen) ret = -12;  // EAI_OVERFLOW
            if (ret == 0 && servlen > 0 && serv_text.size() + 1 > servlen) ret = -12;
            if (ret == 0) {
                if (hostlen > 0) std::memcpy(host.data(), host_text.c_str(), host_text.size() + 1);
                if (servlen > 0) std::memcpy(serv.data(), serv_text.c_str(), serv_text.size() + 1);
            }
        }
        Reply& reply = request.MakeReply(2, 4);
        reply.Push(kResultSuccess);
        reply.Push(ret);
        reply.PushStaticBuffer(std::move(host), 0);
        reply.PushStaticBuffer(std::move(serv), 1);
    }

    // 0x00110144: handle, level, option, length, the calling PID. Reply: result, the error, the length,
    // the value (static buffer 0).
    void GetSockOpt(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 level = request.Pop<u32>();
        const s32 optname = request.Pop<s32>();
        const u32 optlen = request.Pop<u32>();
        const u32 pid = request.PopPid();
        Socket* socket = Holder(request, handle, pid);
        if (socket == nullptr) return;
        s32 err = 0;
        std::vector<u8> optval(std::min<u32>(optlen, 0x1000), 0);
        if (optname < 0) {
            err = 22;  // EINVAL, not translated: Azahar returns the host's number
        } else {
            const PlatformOption option = TranslateSockOpt(level, optname);
            if (!IsKnownOption(option.level, option.name)) {
                err = Err(kEnoProtoOpt);
            } else if (option.level == kLinIpprotoTcp && socket->type != kTypeStream) {
                err = Err(kEnoProtoOpt);
            } else if (option.level == kLinIpprotoIp &&
                       (option.name == kLinIpAddMembership || option.name == kLinIpDropMembership)) {
                err = Err(kEnoProtoOpt);  // write only
            } else if (option.level == kLinSolSocket && option.name == kLinSoLinger) {
                // CTRLinger: l_onoff, l_linger
                std::array<u8, 8> linger{};
                std::memcpy(linger.data(), socket->linger.data(), 8);
                std::memcpy(optval.data(), linger.data(), std::min<std::size_t>(optval.size(), 8));
            } else {
                s32 value = DefaultOption(*socket, option.level, option.name);
                const auto it = socket->int_options.find({option.level, option.name});
                if (it != socket->int_options.end()) value = it->second;
                // TranslateSockOptDataFromPlatform: an int into 1, 2 or 4 bytes; other sizes give the int
                if (optval.size() == 1 || optval.size() == 2 || optval.size() == 4) {
                    std::memcpy(optval.data(), &value, optval.size());
                } else {
                    optval = IntBytes(value);
                }
            }
        }
        Reply& reply = request.MakeReply(3, 2);
        reply.Push(kResultSuccess);
        reply.Push(err);
        reply.Push(static_cast<u32>(optval.size()));
        reply.PushStaticBuffer(std::move(optval), 0);
        MH4N_LOG(Debug, kLog, "called, pid={}, fd={}, ret={}", pid, handle, err);
    }

    // 0x00120104: handle, level, option, length, the calling PID, the value (static buffer).
    void SetSockOpt(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 level = request.Pop<u32>();
        const s32 optname = request.Pop<s32>();
        const u32 optlen = request.Pop<u32>();
        const u32 pid = request.PopPid();
        const StaticBuffer* value_buffer = request.PopStaticBuffer();
        Socket* socket = Holder(request, handle, pid);
        if (socket == nullptr) return;
        std::vector<u8> value(optlen, 0);
        if (value_buffer != nullptr) {
            std::memcpy(value.data(), value_buffer->data.data(),
                        std::min<std::size_t>(value.size(), value_buffer->data.size()));
        }
        s32 err = 0;
        if (optname < 0) {
            err = 22;  // EINVAL, not translated
        } else {
            const PlatformOption option = TranslateSockOpt(level, optname);
            if (!IsKnownOption(option.level, option.name)) {
                err = Err(kEnoProtoOpt);
            } else if (option.level == kLinSolSocket &&
                       (option.name == kLinSoType || option.name == kLinSoError ||
                        option.name == kLinSoSndLowat)) {
                err = Err(kEnoProtoOpt);  // read only
            } else if (option.level == kLinIpprotoTcp && socket->type != kTypeStream) {
                err = Err(kEnoProtoOpt);
            } else if (option.level == kLinSolSocket && option.name == kLinSoLinger) {
                if (value.size() != 8) {
                    err = Err(kEinval);
                } else {
                    std::memcpy(socket->linger.data(), value.data(), 8);
                    socket->linger_set = true;
                }
            } else if (option.level == kLinIpprotoIp &&
                       (option.name == kLinIpAddMembership || option.name == kLinIpDropMembership)) {
                err = option.name == kLinIpAddMembership ? Err(kEnoDev) : Err(kEaddrNotAvail);
            } else {
                // TranslateSockOptDataToPlatform: 1, 2 and 4 byte values become an int
                s32 number = 0;
                if (value.size() == 1) {
                    number = value[0];
                } else if (value.size() == 2) {
                    u16 v;
                    std::memcpy(&v, value.data(), 2);
                    number = v;
                } else if (value.size() == 4) {
                    std::memcpy(&number, value.data(), 4);
                } else {
                    err = Err(kEinval);
                }
                if (err == 0) {
                    if (option.level == kLinIpprotoIp && option.name == kLinIpTtl && number == 0) {
                        number = kTtlDefault;
                    }
                    if (option.level == kLinIpprotoIp && option.name == kLinIpTtl && (number < 1 || number > 255)) {
                        err = Err(kEinval);
                    } else if (option.level == kLinSolSocket && option.name == kLinSoSndBuf) {
                        number = std::max(std::min(number, 212992), 1024) * 2;
                        number = std::max(number, 4608);
                    } else if (option.level == kLinSolSocket && option.name == kLinSoRcvBuf) {
                        number = std::max(std::min(number, 212992), 256) * 2;
                        number = std::max(number, 2304);
                    } else if (option.level == kLinSolSocket && (option.name == kLinSoReuseAddr ||
                                                                 option.name == kLinSoOobInline)) {
                        number = number != 0 ? 1 : 0;
                    } else if (option.level == kLinIpprotoTcp && option.name == kLinTcpNodelay) {
                        number = number != 0 ? 1 : 0;
                    }
                    if (err == 0) socket->int_options[{option.level, option.name}] = number;
                }
            }
        }
        MH4N_LOG(Debug, kLog, "called, pid={}, fd={}, ret={}", pid, handle, err);
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(err);
    }

    // 0x00140084: count, timeout (ms), the calling PID, the poll structures (static buffer).
    void Poll(Request& request) {
        const u32 nfds = request.Pop<u32>();
        const s32 timeout = request.Pop<s32>();
        const u32 pid = request.PopPid();
        const StaticBuffer* input = request.PopStaticBuffer();

        auto pending = std::make_shared<Pending>();
        pending->is_poll = true;
        pending->fds.resize(nfds);
        pending->libctru_bug.assign(nfds, 0);
        for (u32 i = 0; i < nfds; ++i) {
            std::array<u32, 3> fd{};
            if (input != nullptr && input->data.size() >= (static_cast<std::size_t>(i) + 1) * 12) {
                std::memcpy(fd.data(), input->data.data() + static_cast<std::size_t>(i) * 12, 12);
            }
            if (Holder(request, fd[0], pid) == nullptr) {
                return;
            }
            // events and revents through the platform and back (what the guest then sees)
            u8 bug = 0;
            const u32 platform_events = PollToPlatform(fd[1], false, &bug);
            pending->libctru_bug[i] = bug;
            fd[1] = PollTo3ds(platform_events, bug != 0);
            pending->fds[i] = fd;
        }
        pending->request = request.Share();

        const u32 ready = EvaluatePoll(*st_, *pending);
        if (ready > 0 || timeout == 0) {
            Answer(request, PollReply(*pending, static_cast<s32>(ready)));
            return;
        }
        st_->pending.push_back(pending);
        if (timeout > 0) {
            Kernel& kernel = GetKernel();
            const u64 ticks = NsToTicks(static_cast<u64>(timeout) * 1000000ull);
            const std::weak_ptr<Pending> weak = pending;
            const std::shared_ptr<SocState> state = st_;
            Kernel* kernel_ptr = &kernel;
            pending->timer = kernel.ScheduleAt(kernel.NowTicks() + ticks, [weak, state, kernel_ptr] {
                if (const auto p = weak.lock()) {
                    p->timer = 0;
                    FinishPoll(*state, *kernel_ptr, p);
                }
            });
        }
        deferred_ = true;
    }

    // 0x00150042: handle, the calling PID. Reply: result, 0 (never at the mark).
    void SockAtMark(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 pid = request.PopPid();
        if (Holder(request, handle, pid) == nullptr) return;
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);
    }

    // The default interface: none can reach the internet, so the host id is 0.
    void GetHostId(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);
    }

    // 0x00170082: handle, the longest address, the calling PID. Reply: result, the result, the address.
    void GetSockName(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 max_addr_len = request.Pop<u32>();
        const u32 pid = request.PopPid();
        Socket* socket = Holder(request, handle, pid);
        if (socket == nullptr) return;
        const auto address = MakeInetAddress(socket->bound_raw_port, socket->bound_raw_addr);
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push<s32>(0);
        reply.PushStaticBuffer(Truncated(address, max_addr_len), 0);
        MH4N_LOG(Debug, kLog, "called, pid={}, fd={}, ret=0", pid, handle);
    }

    void GetPeerName(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 max_addr_len = request.Pop<u32>();
        const u32 pid = request.PopPid();
        if (Holder(request, handle, pid) == nullptr) return;
        std::array<u8, kSockAddrSize> none{};
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push(Err(kEnotConn));
        reply.PushStaticBuffer(Truncated(none, max_addr_len), 0);
    }

    // 0x00190000: closes the sockets of the calling process and forgets the process.
    void ShutdownSockets(Request& request) {
        const u32 pid = request.CallingPid();
        if (!st_->initialized.contains(pid)) {
            MH4N_LOG(Debug, kLog, "Process not initialized: pid={}", pid);
            request.MakeReply(1, 0).Push(kResultNotInitialized);
            return;
        }
        CloseAllSockets(*st_, GetKernel(), pid);
        st_->initialized.erase(pid);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // 0x001A00C0: level, option, length. Reply: result, the error, the length, the value.
    void GetNetworkOpt(Request& request) {
        const u32 level = request.Pop<u32>();
        const u32 opt_name = request.Pop<u32>();
        u32 opt_len = request.Pop<u32>();
        std::vector<u8> opt_data(std::min<u32>(opt_len, 0x1000), 0);
        u32 err = kErrInvalidEnumValue;
        if (level == kSolConfig) {
            if (opt_name == kNetoptMac) {
                // a fake MAC address of zeros
                err = 0;
            } else if (opt_name == kNetoptIpInfo) {
                // the default interface info: there is no interface, so zeros
                err = 0;
            } else {
                MH4N_LOG(Error, kLog, "(STUBBED) called, level={} opt_name={}", level, opt_name);
            }
        } else {
            MH4N_LOG(Error, kLog, "Unknown level={}", level);
        }
        if (err != 0) {
            opt_data.clear();
            opt_len = 0;
        }
        Reply& reply = request.MakeReply(3, 2);
        reply.Push(kResultSuccess);
        reply.Push(err);
        reply.Push(opt_len);
        reply.PushStaticBuffer(std::move(opt_data), 0);
    }

    void CloseSockets(Request& request) {
        const u32 pid = request.PopPid();
        if (!st_->initialized.contains(pid)) {
            request.MakeReply(1, 0).Push(kResultNotInitialized);
            return;
        }
        CloseAllSockets(*st_, GetKernel(), pid);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void AddGlobalSocket(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 pid = request.CallingPid();
        Socket* socket = Holder(request, handle, pid);
        if (socket == nullptr) return;
        socket->is_global = true;
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    std::shared_ptr<SocState> st_;
    bool deferred_ = false;  // the handler parked the request
};

MH4N_REGISTER(ServiceFactory, "soc:U",
              ServiceFactory([](ServiceContext& context) { return std::make_unique<SocU>(context); },
                             18));

}  // namespace

}  // namespace mh4n::soc
