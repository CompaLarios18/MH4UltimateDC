#include "lan.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace App::Lan {

namespace {

constexpr const char QUERY[] = "MH4U-LAN? 1\n";
constexpr const char ANSWER_FIRST_LINE[] = "MH4U-LAN 1";
constexpr std::size_t MAX_TEXT = 64;
constexpr std::size_t MAX_DATAGRAM = 1024;

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket NO_SOCKET = INVALID_SOCKET;
void CloseSocket(Socket s) {
    closesocket(s);
}
/// Winsock for this module's sockets (counted: Azahar's network library starts it too).
bool StartSockets() {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}
void StopSockets() {
    WSACleanup();
}
#else
using Socket = int;
constexpr Socket NO_SOCKET = -1;
void CloseSocket(Socket s) {
    close(s);
}
bool StartSockets() {
    return true;
}
void StopSockets() {}
#endif

/// A name as an answer carries it: one line, without control characters, at most MAX_TEXT long.
std::string Clean(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (out.size() >= MAX_TEXT) {
            break;
        }
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) {
            out += c;
        }
    }
    return out;
}

bool ToInt(const std::string& text, int lo, int hi, int& value) {
    if (text.empty() || text.size() > 6) {
        return false;
    }
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (*end != '\0' || v < lo || v > hi) {
        return false;
    }
    value = static_cast<int>(v);
    return true;
}

/// Waits up to `ms` for a datagram on `s`: true when one is there.
bool Readable(Socket s, int ms) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(s, &read_set);
    timeval timeout{};
    timeout.tv_sec = ms / 1000;
    timeout.tv_usec = (ms % 1000) * 1000;
    return select(static_cast<int>(s) + 1, &read_set, nullptr, nullptr, &timeout) > 0;
}

sockaddr_in AddressOf(const std::string& ip, int port) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<unsigned short>(port));
    inet_pton(AF_INET, ip.c_str(), &to.sin_addr);
    return to;
}

std::string Text(const in_addr& address) {
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &address, text, sizeof(text));
    return text;
}

} // namespace

std::string Query() {
    return QUERY;
}

bool IsQuery(const std::string& datagram) {
    return datagram.rfind("MH4U-LAN?", 0) == 0;
}

std::string Answer(const RoomInfo& room) {
    std::ostringstream out;
    out << ANSWER_FIRST_LINE << '\n'
        << "name=" << Clean(room.name) << '\n'
        << "owner=" << Clean(room.owner) << '\n'
        << "port=" << room.port << '\n'
        << "players=" << room.players << '\n'
        << "slots=" << room.slots << '\n'
        << "password=" << (room.password ? 1 : 0) << '\n';
    return out.str();
}

bool ParseAnswer(const std::string& datagram, RoomInfo& room) {
    if (datagram.size() > MAX_DATAGRAM) {
        return false;
    }
    std::istringstream in(datagram);
    std::string line;
    if (!std::getline(in, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line != ANSWER_FIRST_LINE) {
        return false;
    }
    RoomInfo found;
    bool has_port = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "name") {
            found.name = Clean(value);
        } else if (key == "owner") {
            found.owner = Clean(value);
        } else if (key == "port") {
            if (!ToInt(value, 1, 65535, found.port)) {
                return false;
            }
            has_port = true;
        } else if (key == "players") {
            ToInt(value, 0, 254, found.players);
        } else if (key == "slots") {
            ToInt(value, 0, 254, found.slots);
        } else if (key == "password") {
            found.password = value == "1";
        }
        // (keys a later version adds are left for it)
    }
    if (!has_port) {
        return false;
    }
    room = found;
    return true;
}

// ── The host's side ──

Responder::~Responder() {
    Stop();
}

bool Responder::Start(Info info, std::string& error, int port) {
    Stop();
    if (!StartSockets()) {
        error = "The system's network sockets didn't start.";
        return false;
    }
    const Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == NO_SOCKET) {
        StopSockets();
        error = "No socket for answering searches on this network.";
        return false;
    }
    // (another copy of the app on this PC may listen too: each answers for its own room)
    const int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&on), sizeof(on));
#endif
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_port = htons(static_cast<unsigned short>(port));
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<const sockaddr*>(&any), sizeof(any)) != 0) {
        CloseSocket(s);
        StopSockets();
        error = "UDP port " + std::to_string(port) + " is in use: players on this network won't see the room "
                "in their search (they can still join by address).";
        return false;
    }
    socket_handle = static_cast<long long>(s);
    stop = false;
    thread = std::thread([this, info = std::move(info)] { Run(info); });
    return true;
}

void Responder::Stop() {
    if (!thread.joinable()) {
        return;
    }
    stop = true;
    thread.join();
    CloseSocket(static_cast<Socket>(socket_handle));
    socket_handle = -1;
    StopSockets();
}

void Responder::Run(Info info) {
    const auto s = static_cast<Socket>(socket_handle);
    char buffer[MAX_DATAGRAM];
    while (!stop) {
        if (!Readable(s, 200)) {
            continue;
        }
        sockaddr_in from{};
        socklen_t from_size = sizeof(from);
        const auto size = recvfrom(s, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &from_size);
        if (size <= 0 || !IsQuery(std::string(buffer, static_cast<std::size_t>(size)))) {
            continue;
        }
        RoomInfo room;
        if (!info(room)) {
            continue;
        }
        const std::string answer = Answer(room);
        sendto(s, answer.data(), static_cast<int>(answer.size()), 0, reinterpret_cast<const sockaddr*>(&from),
               sizeof(from));
    }
}

// ── The player's side ──

std::vector<std::string> BroadcastAddresses() {
    std::vector<std::string> list{"255.255.255.255"};
    const auto add = [&list](const std::string& ip) {
        if (!ip.empty() && std::find(list.begin(), list.end(), ip) == list.end()) {
            list.push_back(ip);
        }
    };
#ifdef _WIN32
    // (Windows sends 255.255.255.255 out of one network only: each one's own broadcast too)
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        INTERFACE_INFO interfaces[32];
        DWORD size = 0;
        if (WSAIoctl(s, SIO_GET_INTERFACE_LIST, nullptr, 0, interfaces, sizeof(interfaces), &size, nullptr,
                     nullptr) == 0) {
            const auto count = size / sizeof(INTERFACE_INFO);
            for (DWORD i = 0; i < count; ++i) {
                const INTERFACE_INFO& it = interfaces[i];
                if (!(it.iiFlags & IFF_UP) || (it.iiFlags & IFF_LOOPBACK) || !(it.iiFlags & IFF_BROADCAST)) {
                    continue;
                }
                in_addr broadcast;
                broadcast.s_addr = it.iiAddress.AddressIn.sin_addr.s_addr | ~it.iiNetmask.AddressIn.sin_addr.s_addr;
                add(Text(broadcast));
            }
        }
        closesocket(s);
    }
#else
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (ifaddrs* i = interfaces; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) ||
                (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_BROADCAST) || !i->ifa_broadaddr) {
                continue;
            }
            add(Text(reinterpret_cast<const sockaddr_in*>(i->ifa_broadaddr)->sin_addr));
        }
        freeifaddrs(interfaces);
    }
#endif
    return list;
}

Search Find(int wait_ms, int port, const std::vector<std::string>& also) {
    Search result;
    if (!StartSockets()) {
        result.error = "The system's network sockets didn't start.";
        return result;
    }
    const Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == NO_SOCKET) {
        StopSockets();
        result.error = "No socket for searching this network.";
        return result;
    }
    const int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(s, reinterpret_cast<const sockaddr*>(&any), sizeof(any));

    std::vector<std::string> targets = BroadcastAddresses();
    for (const auto& ip : also) {
        if (std::find(targets.begin(), targets.end(), ip) == targets.end()) {
            targets.push_back(ip);
        }
    }
    const std::string query = Query();
    int sent = 0;
    const auto ask = [&] {
        for (const auto& ip : targets) {
            const sockaddr_in to = AddressOf(ip, port);
            if (sendto(s, query.data(), static_cast<int>(query.size()), 0, reinterpret_cast<const sockaddr*>(&to),
                       sizeof(to)) > 0) {
                ++sent;
            }
        }
    };

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::milliseconds(std::max(wait_ms, 50));
    int asked = 0;
    char buffer[MAX_DATAGRAM];
    while (true) {
        const auto now = Clock::now();
        // the question at the start, a third and two thirds of the way
        if (asked < 3 && now >= start + std::chrono::milliseconds(wait_ms * asked / 3)) {
            ask();
            ++asked;
        }
        if (now >= deadline) {
            break;
        }
        const auto next_ask = asked < 3 ? start + std::chrono::milliseconds(wait_ms * asked / 3) : deadline;
        const auto until = std::min(deadline, next_ask);
        const int ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count());
        if (!Readable(s, std::max(ms, 1))) {
            continue;
        }
        sockaddr_in from{};
        socklen_t from_size = sizeof(from);
        const auto size = recvfrom(s, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &from_size);
        RoomInfo room;
        if (size <= 0 || !ParseAnswer(std::string(buffer, static_cast<std::size_t>(size)), room)) {
            continue;
        }
        FoundRoom found;
        static_cast<RoomInfo&>(found) = room;
        found.address = Text(from.sin_addr);
        const bool seen = std::any_of(result.rooms.begin(), result.rooms.end(), [&](const FoundRoom& r) {
            return r.address == found.address && r.port == found.port;
        });
        if (!seen) {
            result.rooms.push_back(found);
        }
    }
    CloseSocket(s);
    StopSockets();
    if (sent == 0) {
        result.error = "This PC isn't on a network (no search could go out).";
        return result;
    }
    std::stable_sort(result.rooms.begin(), result.rooms.end(),
                     [](const FoundRoom& a, const FoundRoom& b) { return a.name < b.name; });
    result.ok = true;
    return result;
}

} // namespace App::Lan
