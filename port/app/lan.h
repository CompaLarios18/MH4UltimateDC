// Rooms on this network, found with no address typed: a PC that hosts a room answers on UDP port
// 24873 (DISCOVERY_PORT) and a player looking for one asks there by broadcast, to the whole
// network (255.255.255.255) and to each of this PC's networks (192.168.1.255, ...). The answer
// is the room's name, its owner, its port, the players in it and whether it has a password; the
// address is where the answer came from. Joining is then the room's usual join (Azahar's rooms,
// multiplayer.h): this only finds them, for laptops and PC handhelds on one Wi-Fi network.
//
//     query   "MH4U-LAN? 1\n"
//     answer  "MH4U-LAN 1\nname=Night hunts\nowner=Hunter\nport=24872\nplayers=2\nslots=4\npassword=0\n"
//
// Standard library and the system's sockets only, without Azahar (tests/test_app_lan.py).
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace App::Lan {

inline constexpr int DISCOVERY_PORT = 24873;

struct RoomInfo {
    std::string name;     ///< the room's name
    std::string owner;    ///< the host's nickname
    int port = 24872;     ///< the room's own port (Azahar's room, UDP)
    int players = 0;
    int slots = 0;
    bool password = false;
};

struct FoundRoom : RoomInfo {
    std::string address;  ///< the IPv4 address the answer came from
};

/// The question a player's search sends, and whether a datagram is one.
std::string Query();
bool IsQuery(const std::string& datagram);

/// A host's answer about its room (names cut to 64 characters, line breaks taken out), and the
/// room in one (false for anything else: a query, another program's datagram, a broken answer).
std::string Answer(const RoomInfo& room);
bool ParseAnswer(const std::string& datagram, RoomInfo& room);

/// Answers searches while this PC hosts a room. `info` is asked on the responder's thread at each
/// query; false: no room to tell about (no answer).
class Responder {
public:
    using Info = std::function<bool(RoomInfo&)>;

    Responder() = default;
    Responder(const Responder&) = delete;
    Responder& operator=(const Responder&) = delete;
    ~Responder();

    /// Listens on `port` (UDP, every network of this PC); false with the reason if it can't.
    bool Start(Info info, std::string& error, int port = DISCOVERY_PORT);
    void Stop();
    bool Running() const {
        return thread.joinable();
    }

private:
    void Run(Info info);

    std::thread thread;
    std::atomic<bool> stop{false};
    long long socket_handle = -1;
};

struct Search {
    bool ok = false;
    std::string error;            ///< why nothing could be asked (no network, ...)
    std::vector<FoundRoom> rooms; ///< by name, one per address and port
};

/// Asks this PC's networks for rooms and collects the answers for `wait_ms` (the question goes
/// out three times, in case one is lost). Blocks: run it on a worker thread. `also` are
/// addresses asked directly besides the broadcasts (127.0.0.1 for this PC's own room, in tests).
Search Find(int wait_ms = 1500, int port = DISCOVERY_PORT, const std::vector<std::string>& also = {});

/// The broadcast addresses Find asks: the whole network's and each of this PC's IPv4 networks'.
std::vector<std::string> BroadcastAddresses();

} // namespace App::Lan
