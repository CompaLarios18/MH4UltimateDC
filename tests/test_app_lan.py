"""port/app/lan.cpp: rooms found on this network with no address typed. The answer's format both
ways, broken and foreign datagrams turned away, and a real search over this PC's loopback: a
responder for a made-up room on a free port, and Find asking 127.0.0.1."""
from __future__ import annotations

import os
import shutil
import socket
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "port", "app")

CXX = next((c for c in ("c++", "clang++", "g++") if shutil.which(c)), None)
pytestmark = pytest.mark.skipif(CXX is None or os.name == "nt", reason="needs a C++ compiler (POSIX sockets)")

DRIVER = r"""
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include "lan.h"

static void Print(const App::Lan::RoomInfo& r) {
    std::printf("%s\t%s\t%d\t%d\t%d\t%d\n", r.name.c_str(), r.owner.c_str(), r.port, r.players, r.slots,
                r.password ? 1 : 0);
}

// answer NAME OWNER PORT PLAYERS SLOTS PASSWORD: the answer's bytes
// parse FILE: the room in the datagram FILE holds, or "no"
// query: the query's bytes; isquery FILE: 1 or 0
// serve PORT NAME: answers on PORT for a room NAME until stdin closes
// find PORT MS: the rooms Find gets asking 127.0.0.1 on PORT, with their addresses
int main(int argc, char** argv) {
    const std::string mode = argv[1];
    if (mode == "answer") {
        App::Lan::RoomInfo r;
        r.name = argv[2];
        r.owner = argv[3];
        r.port = std::atoi(argv[4]);
        r.players = std::atoi(argv[5]);
        r.slots = std::atoi(argv[6]);
        r.password = std::atoi(argv[7]) != 0;
        std::fputs(App::Lan::Answer(r).c_str(), stdout);
    } else if (mode == "parse" || mode == "isquery") {
        std::ifstream file(argv[2], std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (mode == "isquery") {
            std::printf("%d\n", App::Lan::IsQuery(data) ? 1 : 0);
            return 0;
        }
        App::Lan::RoomInfo r;
        if (App::Lan::ParseAnswer(data, r)) {
            Print(r);
        } else {
            std::printf("no\n");
        }
    } else if (mode == "query") {
        std::fputs(App::Lan::Query().c_str(), stdout);
    } else if (mode == "serve") {
        const std::string name = argv[3];
        App::Lan::Responder responder;
        std::string error;
        int asked = 0;
        if (!responder.Start([&](App::Lan::RoomInfo& r) {
                r.name = name;
                r.owner = "Host";
                r.port = 24900;
                r.players = ++asked;
                r.slots = 4;
                r.password = true;
                return true;
            }, error, std::atoi(argv[2]))) {
            std::printf("error\t%s\n", error.c_str());
            return 1;
        }
        std::printf("ready\n");
        std::fflush(stdout);
        while (std::fgetc(stdin) != EOF) {
        }
        responder.Stop();
    } else if (mode == "find") {
        const auto found = App::Lan::Find(std::atoi(argv[3]), std::atoi(argv[2]), {"127.0.0.1"});
        std::printf("%s\t%s\n", found.ok ? "ok" : "error", found.error.c_str());
        for (const auto& r : found.rooms) {
            std::printf("%s\t", r.address.c_str());
            Print(r);
        }
    } else if (mode == "broadcasts") {
        for (const auto& ip : App::Lan::BroadcastAddresses()) {
            std::printf("%s\n", ip.c_str());
        }
    }
    return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    work = tmp_path_factory.mktemp("lan")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    exe = work / "driver"
    subprocess.run([CXX, "-std=c++17", "-pthread", "-I", APP, str(src), os.path.join(APP, "lan.cpp"),
                    "-o", str(exe)], check=True)
    return exe


def out(driver, *args) -> str:
    return subprocess.run([str(driver), *args], capture_output=True, text=True, check=True).stdout


def parse(driver, tmp_path, data: bytes):
    path = tmp_path / "datagram"
    path.write_bytes(data)
    line = out(driver, "parse", str(path)).rstrip("\n")
    if line == "no":
        return None
    name, owner, port, players, slots, password = line.split("\t")
    return name, owner, int(port), int(players), int(slots), password == "1"


def test_answer_round_trip(driver, tmp_path):
    data = out(driver, "answer", "Night hunts", "Hunter", "24872", "2", "4", "1").encode()
    assert data.startswith(b"MH4U-LAN 1\n")
    assert parse(driver, tmp_path, data) == ("Night hunts", "Hunter", 24872, 2, 4, True)


def test_names_stay_on_one_line_and_short(driver, tmp_path):
    data = out(driver, "answer", "evil\nport=1", "x" * 200, "24872", "1", "4", "0").encode()
    name, owner, port, *_ = parse(driver, tmp_path, data)
    assert name == "evilport=1"
    assert port == 24872
    assert len(owner) == 64


def test_carriage_returns_and_unknown_keys(driver, tmp_path):
    data = b"MH4U-LAN 1\r\nname=A\r\nport=2000\r\nlater=whatever\r\nplayers=3\r\n"
    assert parse(driver, tmp_path, data) == ("A", "", 2000, 3, 0, False)


@pytest.mark.parametrize("data", [
    b"",
    b"MH4U-LAN? 1\n",                       # a query isn't an answer
    b"MH4U-LAN 2\nname=A\nport=24872\n",    # another version
    b"HELLO\nport=24872\n",                 # another program
    b"MH4U-LAN 1\nname=A\n",                # no port
    b"MH4U-LAN 1\nname=A\nport=0\n",
    b"MH4U-LAN 1\nname=A\nport=70000\n",
    b"MH4U-LAN 1\nname=A\nport=12ab\n",
    b"MH4U-LAN 1\nport=24872\n" + b"x" * 2000,  # too long for one
])
def test_not_answers(driver, tmp_path, data):
    assert parse(driver, tmp_path, data) is None


def test_bad_counts_are_left_at_zero(driver, tmp_path):
    data = b"MH4U-LAN 1\nname=A\nport=24872\nplayers=-4\nslots=999\n"
    assert parse(driver, tmp_path, data) == ("A", "", 24872, 0, 0, False)


def test_query(driver, tmp_path):
    query = out(driver, "query").encode()
    assert query == b"MH4U-LAN? 1\n"
    path = tmp_path / "q"
    path.write_bytes(query)
    assert out(driver, "isquery", str(path)).strip() == "1"
    path.write_bytes(b"MH4U-LAN 1\nport=24872\n")
    assert out(driver, "isquery", str(path)).strip() == "0"


def test_broadcast_addresses_start_with_the_whole_network(driver):
    lines = out(driver, "broadcasts").split()
    assert lines[0] == "255.255.255.255"
    assert "127.255.255.255" not in lines  # loopback isn't searched by broadcast
    assert len(lines) == len(set(lines))


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def test_find_a_room_on_this_pc(driver):
    port = free_udp_port()
    server = subprocess.Popen([str(driver), "serve", str(port), "Loopback room"], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, text=True)
    try:
        assert server.stdout.readline().strip() == "ready"
        lines = out(driver, "find", str(port), "600").splitlines()
        assert lines[0].split("\t")[0] == "ok"
        rooms = [line.split("\t") for line in lines[1:]]
        assert rooms, "no answer from the responder"
        # one per address: 127.0.0.1, and this PC's own network if a broadcast came back too
        assert len({r[0] for r in rooms}) == len(rooms)
        local = next(r for r in rooms if r[0] == "127.0.0.1")
        assert local[1:4] == ["Loopback room", "Host", "24900"]
        assert local[6] == "1"
    finally:
        server.stdin.close()
        server.wait(timeout=10)
    assert server.returncode == 0


def test_find_with_no_room(driver):
    port = free_udp_port()
    lines = out(driver, "find", str(port), "200").splitlines()
    assert lines[0].split("\t")[0] == "ok"
    assert lines[1:] == []


def test_port_in_use(driver):
    port = free_udp_port()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as taken:
        taken.bind(("0.0.0.0", port))
        result = subprocess.run([str(driver), "serve", str(port), "X"], input="", capture_output=True, text=True)
    assert result.returncode == 1
    assert "in use" in result.stdout
