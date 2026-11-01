// mh4u-native's stubs of the network rooms (App::Multiplayer, Launcher::Lobby): the shared UI
// links against them and finds no rooms. Rooms are N4's; until then Available() is false, so the
// in-game menu's Multiplayer tab says "Not available in this build", and every other call does
// nothing or answers "not connected".
#include <algorithm>
#include <cctype>

#include "config.h"
#include "launcher/lobby.h"
#include "multiplayer.h"

namespace App::Multiplayer {

namespace {
Config* g_config = nullptr;
}

bool Available() {
    return false;
}

void Init(Config& config) {
    g_config = &config;
}

void Shutdown() {
    g_config = nullptr;
}

bool StartAtBoot(std::string& error) {
    error.clear();  // no room was asked for that could fail: there are none
    return true;
}

bool Join(const JoinRequest&) {
    return false;
}

bool Host(const HostRequest&) {
    return false;
}

bool Leave(bool) {
    return false;
}

bool Rejoin() {
    return false;
}

std::string LastRoom() {
    return {};
}

void Tick() {}

void JoinLater(const JoinRequest&, double) {}

Status GetStatus() {
    return {};  // Phase::Off: not in a room
}

std::string Nickname() {
    return g_config ? g_config->nickname : std::string();
}

bool ValidNickname(const std::string& name) {
    return name.size() >= 4 && name.size() <= 20 && std::all_of(name.begin(), name.end(), [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '.' ||
                      c == '_' || c == '-';
           });
}

} // namespace App::Multiplayer

namespace Launcher::Lobby {

namespace {
constexpr const char* kNotAvailable = "Rooms are not available in this build.";
}

List Fetch(const std::string&, u64) {
    List list;
    list.error = kNotAvailable;
    return list;
}

List Parse(const std::string&, u64) {
    List list;
    list.error = kNotAvailable;
    return list;
}

std::string NormalizeUrl(const std::string& typed) {
    std::string url = typed;
    const auto first = url.find_first_not_of(" \t");
    const auto last = url.find_last_not_of(" \t/");
    url = first == std::string::npos ? std::string() : url.substr(first, last - first + 1);
    if (!url.empty() && url.find("://") == std::string::npos) {
        url = "http://" + url;
    }
    return url;
}

} // namespace Launcher::Lobby
