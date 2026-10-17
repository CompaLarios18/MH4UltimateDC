// The app's network room (multiplayer.cpp) against a scripted fake runtime: joining with the
// console's MAC address first and the room's after a collision, a refusal with its reason,
// hosting (the room's request, the lobby listing, the join to itself), leaving, the room's events
// arriving on another thread, and the jobs that run one at a time. Synthetic data only.
// Built with -DMH4U_APP_TESTS=ON (port/app/CMakeLists.txt) or by hand against doctest, {fmt},
// inih, httplib and nlohmann's json.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "config.h"
#include "invite.h"
#include "launcher/lobby.h"
#include "multiplayer.h"
#include "util/tests/fake_runtime.h"

// multiplayer.h only declares the system, which Init does not use
class Core::System {};

namespace MP = App::Multiplayer;
using namespace App;
using fake::Env;

namespace {

template <typename Pred>
bool WaitFor(Pred pred, int ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

/// No job runs and the status settled.
bool JobDone() {
    const MP::Status st = MP::GetStatus();
    return st.phase != MP::Phase::Busy && st.action.empty();
}

/// The room module is process-wide: every test starts it, and leaves it out of the room and shut
/// down for the next one.
struct Room {
    Env env;
    Core::System system;
    Config config = Defaults();
    Room() {
        config.nickname.clear();
        REQUIRE(MP::GetStatus().phase == MP::Phase::Off);
    }
    void Start() {
        MP::Init(system, config);
    }
    MP::JoinRequest Request() {
        MP::JoinRequest request;
        request.host = "10.0.0.5";
        request.port = 24872;
        request.password = "pw";
        return request;
    }
    void WaitDone() {
        REQUIRE(WaitFor(JobDone));
    }
    ~Room() {
        WaitFor(JobDone);
        MP::Leave(true);
        WaitFor(JobDone);
        MP::Tick();
        MP::Shutdown();
        // (the module keeps its status between tests: it is out of the room now)
    }
};

} // namespace

TEST_CASE("Init makes the console's data folder and binds the room's callbacks once") {
    Room r;
    r.Start();
    CHECK(r.env.runtime.console_data_prepared == 1);
    CHECK(r.env.runtime.binds == 1);
    MP::Shutdown();
    CHECK(r.env.runtime.unbinds == 1);
    CHECK_FALSE(MP::Join(r.Request()));  // not started
    CHECK_FALSE(MP::Host({}));
    CHECK_FALSE(MP::Leave(false));
}

TEST_CASE("without a network layer nothing is bound and a join says so") {
    Room r;
    r.env.runtime.rooms_available = false;
    r.Start();
    CHECK(r.env.runtime.binds == 0);
    REQUIRE(MP::Join(r.Request()));
    r.WaitDone();
    const MP::Status st = MP::GetStatus();
    CHECK(st.phase == MP::Phase::Off);
    CHECK(st.error == "The network library didn't start.");
    CHECK(r.env.runtime.joins.empty());
}

TEST_CASE("a join asks for the console's own MAC address and its identity, and is remembered") {
    Room r;
    r.Start();
    REQUIRE(MP::Join(r.Request()));
    r.WaitDone();
    const MP::Status st = MP::GetStatus();
    REQUIRE(st.phase == MP::Phase::InRoom);
    CHECK(st.host == "10.0.0.5");
    CHECK(st.port == 24872);
    CHECK(st.password == "pw");
    CHECK(st.joined_in_game);
    CHECK(st.nickname == "Hunter");
    CHECK(st.room_name == "Room");
    CHECK(st.slots == 4);
    REQUIRE(r.env.runtime.joins.size() == 1);
    const RoomJoinRequest& j = r.env.runtime.joins[0];
    CHECK(j.nickname == "Hunter");  // the console's user name: the config has none
    CHECK(j.console_id == r.env.runtime.console_id);
    CHECK(j.host == "10.0.0.5");
    CHECK(j.port == 24872);
    CHECK(j.password == "pw");
    REQUIRE(j.preferred_mac.has_value());
    CHECK(*j.preferred_mac == r.env.runtime.console_mac);

    // what the emulation thread does afterwards waits for Tick: the room gave the same MAC, so
    // the console keeps it; the settings are saved
    CHECK(r.env.runtime.mac_sets.empty());
    MP::Tick();
    CHECK(r.env.runtime.mac_sets.empty());
    const Config saved = Load();
    CHECK(saved.join == "10.0.0.5:24872");
    CHECK(saved.password == "pw");
    CHECK(saved.last_room == "mh4u://10.0.0.5:24872?password=pw");
    CHECK(saved.host == 0);
    CHECK(MP::LastRoom() == "join 10.0.0.5:24872");
}

TEST_CASE("the nickname is the config's, else the console's") {
    Room r;
    r.Start();
    CHECK(MP::Nickname() == "Hunter");
    r.config.nickname = "Rathalos_99";
    CHECK(MP::Nickname() == "Rathalos_99");
    REQUIRE(MP::Join(r.Request()));
    r.WaitDone();
    CHECK(r.env.runtime.joins.at(0).nickname == "Rathalos_99");
}

TEST_CASE("a MAC collision joins again with the MAC the room gives, and the console takes it") {
    Room r;
    r.env.runtime.join_script = {{false, RoomError::MacCollision}};
    r.Start();
    REQUIRE(MP::Join(r.Request()));
    r.WaitDone();
    REQUIRE(MP::GetStatus().phase == MP::Phase::InRoom);
    REQUIRE(r.env.runtime.joins.size() == 2);
    CHECK(r.env.runtime.joins[0].preferred_mac == r.env.runtime.console_mac);
    CHECK_FALSE(r.env.runtime.joins[1].preferred_mac.has_value());
    CHECK(r.env.Logged("joining with one it gives"));
    MP::Tick();
    REQUIRE(r.env.runtime.mac_sets.size() == 1);
    CHECK(r.env.runtime.mac_sets[0] == r.env.runtime.assigned_mac);
    CHECK(r.env.Logged("the console's MAC address is now 40:F4:07:AA:BB:CC"));
}

TEST_CASE("a refusal is explained, not retried, and the console gets its own MAC back") {
    Room r;
    r.env.runtime.default_outcome = {false, RoomError::WrongPassword};
    r.env.runtime.console_mac_now = r.env.runtime.assigned_mac;  // a room had changed it
    r.Start();
    REQUIRE(MP::Join(r.Request()));
    r.WaitDone();
    const MP::Status st = MP::GetStatus();
    CHECK(st.phase == MP::Phase::Off);
    CHECK(st.error == "The room's password is wrong.");
    CHECK_FALSE(st.hint.empty());
    CHECK(r.env.runtime.joins.size() == 1);
    MP::Tick();
    REQUIRE(r.env.runtime.mac_sets.size() == 1);
    CHECK(r.env.runtime.mac_sets[0] == r.env.runtime.console_mac);
}

TEST_CASE("each refusal has its own message") {
    struct Case {
        RoomError error;
        const char* message;
    };
    const Case cases[] = {
        {RoomError::NameCollision, "The room turned the name \"Hunter\" away: someone in it has that name."},
        {RoomError::ConsoleIdCollision, "Someone in the room has the same console ID as this one"},
        {RoomError::WrongVersion, "The room speaks another version of Azahar's room protocol."},
        {RoomError::RoomIsFull, "The room is full."},
        {RoomError::HostBanned, "The room's host has banned you."},
        {RoomError::HostKicked, "The room's host removed you from the room."},
        {RoomError::LostConnection, "The connection to the room was lost."},
        {RoomError::CouldNotConnect, "No room answered at 10.0.0.5:24872."},
        {RoomError::Unknown, "Couldn't join the room at 10.0.0.5:24872 (UnknownError)."},
        {RoomError::NoSuchUser, "Couldn't join the room at 10.0.0.5:24872 (NoSuchUser)."},
    };
    for (const Case& c : cases) {
        CAPTURE(c.message);
        Room r;
        r.env.runtime.default_outcome = {false, c.error};
        r.Start();
        REQUIRE(MP::Join(r.Request()));
        r.WaitDone();
        const MP::Status st = MP::GetStatus();
        CHECK(st.phase == MP::Phase::Off);
        CHECK(st.error.find(c.message) == 0);
    }
}

TEST_CASE("a join nobody answers with a reason for is a room that did not answer") {
    Room r;
    r.env.runtime.default_outcome = {false, std::nullopt};
    r.Start();
    REQUIRE(MP::Join(r.Request()));
    r.WaitDone();
    const MP::Status st = MP::GetStatus();
    CHECK(st.phase == MP::Phase::Off);
    CHECK(st.error == "No room answered at 10.0.0.5:24872.");
    CHECK(st.hint.find("forward UDP port 24872") != std::string::npos);
}

TEST_CASE("a job runs at a time") {
    Room r;
    r.env.runtime.join_delay_ms = 150;
    r.Start();
    REQUIRE(MP::Join(r.Request()));
    CHECK(MP::GetStatus().phase == MP::Phase::Busy);
    CHECK(MP::GetStatus().action == "Joining 10.0.0.5:24872...");
    CHECK_FALSE(MP::Join(r.Request()));
    CHECK_FALSE(MP::Host({}));
    CHECK_FALSE(MP::Leave(false));
    r.WaitDone();
    CHECK(MP::GetStatus().phase == MP::Phase::InRoom);
    CHECK(r.env.runtime.joins.size() == 1);
}

TEST_CASE("a label names the join while it runs") {
    Room r;
    r.env.runtime.join_delay_ms = 100;
    r.Start();
    MP::JoinRequest request = r.Request();
    request.label = "Friends' room";
    REQUIRE(MP::Join(request));
    CHECK(MP::GetStatus().action == "Joining Friends' room...");
    r.WaitDone();
}

TEST_CASE("the room's events arrive on another thread and show in the status") {
    Room r;
    r.Start();
    REQUIRE(MP::Join(r.Request()));
    r.WaitDone();
    REQUIRE(MP::GetStatus().phase == MP::Phase::InRoom);
    MP::Tick();

    const MacAddress other{2, 0, 0, 0, 0, 1}, quiet{2, 0, 0, 0, 0, 2};
    std::thread network([&] {
        const RoomCallbacks c = r.env.runtime.Callbacks();
        c.on_info({"Big room", 8}, {{"Hunter", "", "Monster Hunter 4 Ultimate", r.env.runtime.console_mac},
                                    {"Friend", "friend_web", "Monster Hunter 4 Ultimate", other},
                                    {"Quiet", "", "", quiet}});
        c.on_member(true, "Friend");
        c.on_packet(other, RoomCallbacks::Packet::Data);
        c.on_packet(quiet, RoomCallbacks::Packet::NetSync);  // not local play: not heard
    });
    network.join();
    MP::Status st = MP::GetStatus();
    CHECK(st.room_name == "Big room");
    CHECK(st.slots == 8);
    REQUIRE(st.members.size() == 3);
    CHECK(st.members[0].nickname == "Hunter");
    CHECK(st.members[0].you);
    CHECK(st.members[0].heard == -1.0);
    CHECK(st.members[1].nickname == "Friend");
    CHECK(st.members[1].username == "friend_web");
    CHECK(st.members[1].game == "Monster Hunter 4 Ultimate");
    CHECK_FALSE(st.members[1].you);
    CHECK(st.members[1].heard >= 0.0);
    CHECK(st.members[1].heard < 5.0);
    CHECK(st.members[2].heard == -1.0);
    CHECK(r.env.Logged("Friend joined the room"));

    // the room drops the connection: the status says so and the console gets its own MAC back
    r.env.runtime.console_mac_now = r.env.runtime.assigned_mac;
    std::thread dropped([&] {
        const RoomCallbacks c = r.env.runtime.Callbacks();
        c.on_state(RoomState::Idle);
        c.on_error(RoomError::LostConnection);
    });
    dropped.join();
    st = MP::GetStatus();
    CHECK(st.phase == MP::Phase::Off);
    CHECK(st.error == "The connection to the room was lost.");
    CHECK_FALSE(st.joined_in_game);
    MP::Tick();
    REQUIRE(r.env.runtime.mac_sets.size() == 1);
    CHECK(r.env.runtime.mac_sets[0] == r.env.runtime.console_mac);
}

TEST_CASE("the status is read while two threads feed the room's events") {
    Room r;
    r.Start();
    REQUIRE(MP::Join(r.Request()));
    r.WaitDone();
    std::atomic<bool> stop{false};
    const auto feed = [&](MacAddress mac) {
        const RoomCallbacks c = r.env.runtime.Callbacks();
        for (int i = 0; i < 2000 && !stop; ++i) {
            c.on_packet(mac, i % 3 == 0 ? RoomCallbacks::Packet::Beacon : RoomCallbacks::Packet::Data);
            if (i % 100 == 0) {
                c.on_info({"Room", 4}, {{"Hunter", "", "", mac}});
            }
        }
    };
    std::thread a(feed, MacAddress{2, 0, 0, 0, 0, 1}), b(feed, MacAddress{2, 0, 0, 0, 0, 2});
    for (int i = 0; i < 200; ++i) {
        const MP::Status st = MP::GetStatus();
        CHECK(st.phase == MP::Phase::InRoom);
        MP::Tick();
    }
    stop = true;
    a.join();
    b.join();
}

TEST_CASE("hosting opens the room with the app's request, then joins it as a member") {
    Room r;
    r.config.room_name = "";
    r.Start();
    MP::HostRequest request;
    request.port = 5000;
    request.name = "";
    request.slots = 1;
    request.password = "secret";
    REQUIRE(MP::Host(request));
    r.WaitDone();
    const MP::Status st = MP::GetStatus();
    REQUIRE(st.phase == MP::Phase::InRoom);
    CHECK(st.hosting);
    CHECK(st.host == "127.0.0.1");
    CHECK(st.port == 5000);
    CHECK(st.lobby_note == "Unlisted: friends join by address or invite.");
    REQUIRE(r.env.runtime.opens.size() == 1);
    const RoomOpenRequest& o = r.env.runtime.opens[0];
    CHECK(o.name == "MH4U");  // an empty name is MH4U
    CHECK(o.port == 5000);
    CHECK(o.password == "secret");
    CHECK(o.slots == 2);  // at least two
    CHECK(o.host_nickname == "Hunter");
    CHECK(o.game_name == "Monster Hunter 4 Ultimate");
    CHECK(o.game_id == 0x0004000000126300ull);
    CHECK(o.lobby_url.empty());
    REQUIRE(r.env.runtime.joins.size() == 1);
    CHECK(r.env.runtime.joins[0].host == "127.0.0.1");
    CHECK(r.env.runtime.joins[0].port == 5000);
    CHECK(r.env.runtime.joins[0].password == "secret");
    MP::Tick();
    const Config saved = Load();
    CHECK(saved.host == 5000);
    CHECK(saved.room_port == 5000);
    CHECK(saved.room_name == "");
    CHECK(saved.last_room == "host");
    CHECK(saved.join.empty());
}

TEST_CASE("a room has at most 254 places") {
    Room r;
    r.Start();
    MP::HostRequest request;
    request.slots = 999;
    REQUIRE(MP::Host(request));
    r.WaitDone();
    REQUIRE(r.env.runtime.opens.size() == 1);
    CHECK(r.env.runtime.opens[0].slots == 254);
}

TEST_CASE("listing needs a lobby and a token; the lobby's answer becomes the note") {
    struct Case {
        std::string lobby, token;
        RoomOpenResult::Listing listing;
        const char* note;
        bool asked;
    };
    const std::string url = Launcher::Lobby::NormalizeUrl("http://127.0.0.1:1");
    REQUIRE_FALSE(url.empty());
    const Case cases[] = {
        {"", "tok", RoomOpenResult::Listing::None, "Not listed: listing needs a lobby and your token there.", false},
        {"http://127.0.0.1:1", "", RoomOpenResult::Listing::None, "Not listed: listing needs a lobby and your token there.", false},
        {"http://127.0.0.1:1", "tok", RoomOpenResult::Listing::Listed, "", true},
        {"http://127.0.0.1:1", "tok", RoomOpenResult::Listing::Failed, "", true},
        // (a build without the lobby's web service answers None for a listed room: no note)
        {"http://127.0.0.1:1", "tok", RoomOpenResult::Listing::None, "", true},
    };
    for (const Case& c : cases) {
        Room r;
        r.config.lobby = c.lobby;
        r.config.token = c.token;
        r.env.runtime.open_result = {true, c.listing, c.listing == RoomOpenResult::Listing::Failed ? "nope" : ""};
        r.Start();
        MP::HostRequest request;
        request.listed = true;
        REQUIRE(MP::Host(request));
        r.WaitDone();
        const MP::Status st = MP::GetStatus();
        REQUIRE(r.env.runtime.opens.size() == 1);
        const RoomOpenRequest& o = r.env.runtime.opens[0];
        CHECK(o.lobby_url == (c.asked ? url : ""));
        CHECK(o.token == (c.asked ? c.token : ""));
        if (c.listing == RoomOpenResult::Listing::Listed) {
            CHECK(st.lobby_note == "Listed in the lobby at " + url + ".");
        } else if (c.listing == RoomOpenResult::Listing::Failed) {
            CHECK(st.lobby_note == "The lobby didn't list the room: nope. Friends can still join by address or invite.");
            CHECK(r.env.Logged("didn't list the room: nope"));
        } else {
            CHECK(st.lobby_note == c.note);
        }
    }
}

TEST_CASE("a room that can't open says so and joins nothing") {
    Room r;
    r.env.runtime.open_result = {false, RoomOpenResult::Listing::None, ""};
    r.Start();
    MP::HostRequest request;
    request.port = 5001;
    REQUIRE(MP::Host(request));
    r.WaitDone();
    const MP::Status st = MP::GetStatus();
    CHECK(st.phase == MP::Phase::Off);
    CHECK(st.error == "Couldn't open a room on UDP port 5001.");
    CHECK_FALSE(st.hint.empty());
    CHECK(r.env.runtime.joins.empty());
    CHECK(r.env.runtime.closes == 0);
}

TEST_CASE("a hosted room that its host can't join is closed again") {
    Room r;
    r.env.runtime.default_outcome = {false, RoomError::CouldNotConnect};
    r.Start();
    MP::HostRequest request;
    request.port = 5002;
    REQUIRE(MP::Host(request));
    r.WaitDone();
    const MP::Status st = MP::GetStatus();
    CHECK(st.phase == MP::Phase::Off);
    CHECK(r.env.runtime.closes == 1);
    CHECK_FALSE(r.env.runtime.hosting);
    CHECK(r.env.runtime.opens.size() == 1);
}

TEST_CASE("leaving keeps a hosted room open unless it is asked to close it") {
    Room r;
    r.Start();
    MP::HostRequest request;
    request.port = 5003;
    REQUIRE(MP::Host(request));
    r.WaitDone();
    REQUIRE(MP::GetStatus().phase == MP::Phase::InRoom);
    MP::Tick();
    CHECK(Load().host == 5003);

    REQUIRE(MP::Leave(false));
    r.WaitDone();
    CHECK(r.env.runtime.leaves == 1);
    CHECK(r.env.runtime.closes == 0);
    CHECK(r.env.runtime.hosting);
    CHECK(MP::GetStatus().phase == MP::Phase::Off);
    CHECK(MP::GetStatus().error.empty());  // asked for: not "Out of the room."
    MP::Tick();
    const Config left = Load();
    CHECK(left.host == 0);
    CHECK(left.join.empty());
    CHECK(left.last_room == "host");  // Rejoin brings it back

    // out again, and closing it too
    REQUIRE(MP::Join(r.Request()));
    r.WaitDone();
    CHECK_FALSE(r.env.runtime.hosting);  // joining another room closes the hosted one
    CHECK(r.env.runtime.closes == 1);
    REQUIRE(MP::Leave(true));
    r.WaitDone();
    CHECK(r.env.runtime.leaves == 2);
}

TEST_CASE("Rejoin brings the last room back") {
    Room r;
    r.Start();
    CHECK(MP::LastRoom().empty());
    CHECK_FALSE(MP::Rejoin());
    REQUIRE(MP::Join(r.Request()));
    r.WaitDone();
    MP::Tick();
    REQUIRE(MP::Leave(false));
    r.WaitDone();
    MP::Tick();
    REQUIRE(MP::LastRoom() == "join 10.0.0.5:24872");  // kept by Join (Leave only clears join and host)
    REQUIRE(MP::Rejoin());
    r.WaitDone();
    REQUIRE(r.env.runtime.joins.size() == 2);
    CHECK(r.env.runtime.joins[1].host == "10.0.0.5");
    CHECK(r.env.runtime.joins[1].port == 24872);
    CHECK(r.env.runtime.joins[1].password == "pw");
    CHECK(MP::GetStatus().phase == MP::Phase::InRoom);

    // a hosted last room is hosted again with the settings
    r.config.last_room = "host";
    r.config.room_port = 5004;
    r.config.room_name = "Mine";
    r.config.room_members = 6;
    CHECK(MP::LastRoom() == "host \"Mine\" on port 5004");
    REQUIRE(MP::Rejoin());
    r.WaitDone();
    REQUIRE(r.env.runtime.opens.size() == 1);
    CHECK(r.env.runtime.opens[0].name == "Mine");
    CHECK(r.env.runtime.opens[0].port == 5004);
    CHECK(r.env.runtime.opens[0].slots == 6);
}

TEST_CASE("--join-after joins once the loop has run for that long") {
    Room r;
    r.Start();
    MP::JoinLater(r.Request(), 0.05);
    MP::Tick();  // the first tick starts the clock
    CHECK(MP::GetStatus().phase == MP::Phase::Off);
    CHECK(r.env.runtime.JoinCount() == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    MP::Tick();
    r.WaitDone();
    CHECK(r.env.runtime.joins.size() == 1);
    CHECK(r.env.Logged("(--join-after)"));
    MP::Tick();  // once only
    CHECK(r.env.runtime.JoinCount() == 1);
}

TEST_CASE("StartAtBoot joins or hosts what the settings say, and waits for it") {
    SUBCASE("nothing to do") {
        Room r;
        r.Start();
        std::string error;
        CHECK(MP::StartAtBoot(error));
        CHECK(r.env.runtime.joins.empty());
        CHECK(r.env.runtime.opens.empty());
    }
    SUBCASE("a room to join") {
        Room r;
        r.config.join = "10.1.1.1:3000";
        r.config.password = "pw2";
        r.Start();
        std::string error;
        CHECK(MP::StartAtBoot(error));
        REQUIRE(r.env.runtime.joins.size() == 1);
        CHECK(r.env.runtime.joins[0].host == "10.1.1.1");
        CHECK(r.env.runtime.joins[0].port == 3000);
        CHECK(r.env.runtime.joins[0].password == "pw2");
        CHECK(MP::GetStatus().phase == MP::Phase::InRoom);
        CHECK(r.config.last_room == "mh4u://10.1.1.1:3000?password=pw2");
        CHECK(Load().last_room == r.config.last_room);
    }
    SUBCASE("a code in the settings carries its own password") {
        Room r;
        r.config.join = "mh4u://10.1.1.2:3001?password=code";
        r.config.password = "other";
        r.Start();
        std::string error;
        CHECK(MP::StartAtBoot(error));
        REQUIRE(r.env.runtime.joins.size() == 1);
        CHECK(r.env.runtime.joins[0].password == "code");
    }
    SUBCASE("a join that is not an address") {
        Room r;
        r.config.join = "no way";
        r.Start();
        std::string error;
        CHECK_FALSE(MP::StartAtBoot(error));
        CHECK(error.find("join = no way in mh4u-app.ini: ") == 0);
        CHECK(r.env.runtime.joins.empty());
    }
    SUBCASE("a refused join returns the reason and the hint") {
        Room r;
        r.config.join = "10.1.1.1:3000";
        r.env.runtime.default_outcome = {false, RoomError::RoomIsFull};
        r.Start();
        std::string error;
        CHECK_FALSE(MP::StartAtBoot(error));
        CHECK(error.find("The room is full. Wait for someone to leave") == 0);
        CHECK(MP::GetStatus().phase == MP::Phase::Off);
    }
    SUBCASE("a room to host") {
        Room r;
        r.config.host = 5005;
        r.config.room_name = "Boot room";
        r.config.room_members = 3;
        r.config.password = "bootpw";
        r.config.room_public = false;
        r.Start();
        std::string error;
        CHECK(MP::StartAtBoot(error));
        REQUIRE(r.env.runtime.opens.size() == 1);
        CHECK(r.env.runtime.opens[0].name == "Boot room");
        CHECK(r.env.runtime.opens[0].slots == 3);
        CHECK(r.env.runtime.opens[0].port == 5005);
        CHECK(r.env.runtime.opens[0].password == "bootpw");
        CHECK(MP::GetStatus().phase == MP::Phase::InRoom);
        CHECK(MP::GetStatus().lobby_note == "Unlisted: friends join by address or invite.");
        CHECK(r.config.last_room == "host");
        CHECK(Load().room_port == 5005);
    }
}

TEST_CASE("Shutdown waits for a job, leaves the room and closes a hosted one") {
    Room r;
    r.env.runtime.join_delay_ms = 100;
    r.Start();
    MP::HostRequest request;
    request.port = 5006;
    REQUIRE(MP::Host(request));
    MP::Shutdown();  // while the job runs
    CHECK(r.env.runtime.unbinds == 1);
    CHECK(r.env.runtime.leaves >= 1);
    CHECK(r.env.runtime.closes == 1);
    CHECK_FALSE(r.env.runtime.hosting);
    MP::Shutdown();  // twice is fine
    CHECK(r.env.runtime.unbinds == 1);
}

TEST_CASE("nicknames follow Azahar's rule") {
    CHECK_FALSE(MP::ValidNickname(""));
    CHECK_FALSE(MP::ValidNickname("abc"));
    CHECK(MP::ValidNickname("abcd"));
    CHECK(MP::ValidNickname("Rathalos_99"));
    CHECK(MP::ValidNickname("a b-c_d.e"));
    CHECK(MP::ValidNickname(std::string(20, 'x')));
    CHECK_FALSE(MP::ValidNickname(std::string(21, 'x')));
    CHECK_FALSE(MP::ValidNickname("bad!"));
    CHECK_FALSE(MP::ValidNickname("tab\there"));
    CHECK_FALSE(MP::ValidNickname("caf\xc3\xa9 x"));
}
