"""server/lobby/mh4u_lobby.py, the lobby for Azahar's network rooms, over HTTP as Azahar's web
client calls it (src/web_service/web_backend.cpp): every endpoint's status, Content-Type and
JSON shape, sign-in and owner checks, rooms expiring on an injected clock, open and invite
mode, state across restarts, the user commands, and the RS256 tokens rooms check, verified
by an independent implementation here and by OpenSSL when it is installed. Synthetic data,
and a fixed small RSA key, so that it stays fast."""
import base64
import hashlib
import json
import math
import os
import shutil
import stat
import subprocess
import sys
import threading
import urllib.error
import urllib.request

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "server", "lobby"))

import mh4u_lobby  # noqa: E402

# a fixed 1024-bit test key (two 512-bit primes), so that the tests don't have to make one
P = int("f56ce78a10a6182b6031df15fa9c87f06554928e51b0f5ca9d57069cd3125dd5"
        "13e272105affbe4c7f3984aa66aa61b68e0364441330dce4ab7249aa31c050b9", 16)
Q = int("d9611fdac58851a9600dc68d6c38b6b12fc1ee74a349535242bbb781ffb676bc"
        "bf3c0c39b08f7bae341df159e0cd82bc1cc6e83c119c822c44b868ada1d37123", 16)
MH4U = 0x0004000000126300
NO_PROXY = urllib.request.build_opener(urllib.request.ProxyHandler({}))


@pytest.fixture(autouse=True)
def fast_hashes(monkeypatch):
    monkeypatch.setattr(mh4u_lobby, "PBKDF2_ITERATIONS", 1000)


class Clock:
    def __init__(self):
        self.now = 1_760_000_000.0

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


def write_test_key(folder):
    os.makedirs(folder, exist_ok=True)
    key = mh4u_lobby.RSAKey(P * Q, 65537, pow(65537, -1, math.lcm(P - 1, Q - 1)), P, Q)
    with open(os.path.join(folder, "lobby_key.pem"), "w") as f:
        f.write(key.private_pem())


class Server:
    """A lobby on a free port in a thread, and requests with the headers Azahar sends."""

    def __init__(self, folder, clock=None, **options):
        if not os.path.exists(os.path.join(folder, "lobby_key.pem")):
            write_test_key(folder)
        self.clock = clock or Clock()
        self.log = []
        self.lobby = mh4u_lobby.Lobby(str(folder), clock=self.clock, log=self.log.append, **options)
        self.httpd = mh4u_lobby.LobbyServer(("127.0.0.1", 0), self.lobby)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, kwargs={"poll_interval": 0.02},
                         daemon=True).start()

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()

    def request(self, method, path, body=None, headers=None):
        """(status, Content-Type, body bytes)"""
        sent = {"api-version": "1"}
        if method != "GET":
            sent["Content-Type"] = "application/json"
        sent.update(headers or {})
        data = body if body is None or isinstance(body, bytes) else json.dumps(body).encode()
        if method == "POST" and data is None:
            data = b""
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}", data=data,
                                     headers=sent, method=method)
        try:
            with NO_PROXY.open(req, timeout=10) as reply:
                return reply.status, reply.headers.get("Content-Type", ""), reply.read()
        except urllib.error.HTTPError as error:
            return error.code, error.headers.get("Content-Type", ""), error.read()

    def sign_in(self, name, token):
        status, kind, body = self.request("POST", "/jwt/internal",
                                          headers={"x-username": name, "x-token": token})
        assert (status, "text/html" in kind) == (200, True), body
        return body.decode()

    def register(self, jwt, room):
        status, kind, body = self.request("POST", "/lobby", room, auth(jwt))
        assert (status, "application/json" in kind) == (200, True), body
        return json.loads(body)

    def rooms(self):
        status, kind, body = self.request("GET", "/lobby")
        assert (status, "application/json" in kind) == (200, True)
        return json.loads(body)["rooms"]


@pytest.fixture
def server(tmp_path):
    s = Server(tmp_path / "data")
    yield s
    s.stop()


def auth(jwt):
    return {"Authorization": f"Bearer {jwt}"}


def room_json(name="MH4U Gathering Hall", port=24872, **extra):
    """A room as Azahar's to_json(Room) sends it (no players key while it has none)."""
    room = {"port": port, "name": name, "preferredGameName": "Monster Hunter 4 Ultimate",
            "preferredGameId": MH4U, "maxPlayers": 16, "netVersion": 4, "hasPassword": False}
    room.update(extra)
    return room


def check_azahar_room(room):
    """What Azahar's from_json(Room) reads, with the types nlohmann's get<> insists on."""
    kinds = {"id": str, "externalGuid": str, "address": str, "name": str, "owner": str,
             "port": int, "preferredGameName": str, "preferredGameId": int, "maxPlayers": int,
             "netVersion": int, "hasPassword": bool}
    for key, kind in kinds.items():
        assert type(room[key]) is kind, key
    assert 0 < room["port"] < 2**16 and 0 <= room["preferredGameId"] < 2**64
    assert type(room.get("description", "")) is str
    for player in room.get("players", []):
        assert type(player["nickname"]) is str and type(player["gameName"]) is str
        assert type(player["gameId"]) is int
        # Azahar reads username and avatarUrl together: one without the other loses the name
        assert ("username" in player) == ("avatarUrl" in player)


def unb64url(text):
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def jwt_parts(token):
    head, body, signature = token.split(".")
    return json.loads(unb64url(head)), json.loads(unb64url(body)), unb64url(signature)


def der(data, pos):
    """A DER element, read independently of the server's code: (tag, content, next)."""
    tag, n = data[pos], data[pos + 1]
    pos += 2
    if n & 0x80:
        n, pos = int.from_bytes(data[pos:pos + (n & 0x7F)], "big"), pos + (n & 0x7F)
    return tag, data[pos:pos + n], pos + n


def public_numbers(pem):
    """n and e from a "PUBLIC KEY" PEM (SubjectPublicKeyInfo with rsaEncryption)."""
    lines = pem.strip().splitlines()
    assert lines[0] == "-----BEGIN PUBLIC KEY-----" and lines[-1] == "-----END PUBLIC KEY-----"
    tag, info, _ = der(base64.b64decode("".join(lines[1:-1])), 0)
    assert tag == 0x30
    tag, algorithm, pos = der(info, 0)
    assert (tag, algorithm) == (0x30, bytes.fromhex("06092a864886f70d0101010500"))
    tag, bits, _ = der(info, pos)
    assert tag == 0x03 and bits[0] == 0
    _, rsa, _ = der(bits, 1)
    _, n, pos = der(rsa, 0)
    _, e, _ = der(rsa, pos)
    return int.from_bytes(n, "big"), int.from_bytes(e, "big")


def rs256_valid(token, pem):
    """RSASSA-PKCS1-v1_5 with SHA-256, checked with the public key alone."""
    n, e = public_numbers(pem)
    head, body, signature = token.split(".")
    size = (n.bit_length() + 7) // 8
    digest_info = bytes.fromhex("3031300d060960864801650304020105000420") + \
        hashlib.sha256(f"{head}.{body}".encode()).digest()
    expected = b"\x00\x01" + b"\xff" * (size - len(digest_info) - 3) + b"\x00" + digest_info
    return pow(int.from_bytes(unb64url(signature), "big"), e, n).to_bytes(size, "big") == expected


def openssl_verifies(token, pem, folder):
    head, body, signature = token.split(".")
    files = {"key.pem": pem.encode(), "signed": f"{head}.{body}".encode(),
             "signature": unb64url(signature)}
    for name, content in files.items():
        (folder / name).write_bytes(content)
    r = subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(folder / "key.pem"),
                        "-signature", str(folder / "signature"), str(folder / "signed")],
                       capture_output=True, text=True)
    return r.returncode == 0 and "Verified OK" in r.stdout


def cli(capsys, folder, *args):
    """A user command: (exit code, stdout lines)."""
    code = mh4u_lobby.main(["--data", str(folder), *args])
    return code, capsys.readouterr().out.splitlines()


# ── the endpoints ───────────────────────────────────────────────────────────────────────────

def test_empty_lobby_and_front_page(server):
    assert server.rooms() == []
    status, kind, body = server.request("GET", "/")
    assert status == 200 and "text/plain" in kind and b"0 room(s)" in body


def test_unknown_paths_and_methods(server):
    for method, path, expected in [("GET", "/nothing", 404), ("PUT", "/lobby", 501),
                                   ("DELETE", "/lobby", 405), ("GET", "/jwt/internal", 405)]:
        status, kind, _ = server.request(method, path)
        assert status == expected, (method, path)
        if expected != 501:  # (501 comes from Python's own server, before the lobby sees it)
            assert "application/json" in kind


def test_room_lifecycle(server):
    jwt = server.sign_in("Hunter One", "token-one")
    sent = room_json(description="Low rank, any weapon", hasPassword=True)
    room = server.register(jwt, sent)
    check_azahar_room(room)
    assert (room["address"], room["owner"]) == ("127.0.0.1", "Hunter One")
    assert {key: room[key] for key in sent} == sent
    assert room["externalGuid"] and room["id"] != room["externalGuid"]
    assert server.rooms() == [room]

    # every 15 s Azahar sends the players: a verified one has a username and maybe no avatar
    players = [{"nickname": "Hunter One", "username": "Hunter One", "gameName": "MH4U",
                "gameId": MH4U},
               {"nickname": "Guest", "gameName": "MH4U", "gameId": MH4U}]
    status, kind, body = server.request("POST", f"/lobby/{room['id']}", {"players": players},
                                        auth(jwt))
    assert status == 200 and "application/json" in kind
    listed, = server.rooms()
    check_azahar_room(listed)
    assert listed["players"] == [dict(players[0], avatarUrl=""), players[1]]

    status, kind, body = server.request("DELETE", f"/lobby/{room['id']}", b"", auth(jwt))
    assert (status, json.loads(body)) == (200, {}) and "application/json" in kind
    assert server.rooms() == []
    # an update for a room the lobby doesn't have: 404, and Azahar announces the room again
    status, _, _ = server.request("POST", f"/lobby/{room['id']}", {"players": []}, auth(jwt))
    assert status == 404


def test_room_without_description_or_players(server):
    room = server.register(server.sign_in("Hunter One", "t1"), room_json())
    assert "description" not in room and room["players"] == []


def test_profile_and_public_key(server):
    jwt = server.sign_in("Hunter One", "t1")
    status, kind, body = server.request("GET", "/profile", headers=auth(jwt))
    assert status == 200 and "application/json" in kind
    assert json.loads(body)["username"] == "Hunter One"  # what Azahar's VerifyLogin compares
    status, kind, body = server.request("GET", "/jwt/external/key.pem")
    assert status == 200 and "text/plain" in kind
    assert body.decode().startswith("-----BEGIN PUBLIC KEY-----\n")


def test_sign_in_token(server):
    header, claims, _ = jwt_parts(server.sign_in("Hunter One", "t1"))
    assert header == {"alg": "HS256", "typ": "JWT"}
    assert claims["sub"] == "Hunter One" and claims["exp"] > claims["iat"]


# ── names and sign-in ───────────────────────────────────────────────────────────────────────

def test_open_mode_first_sign_in_claims_the_name(server):
    server.sign_in("Hunter One", "secret-1")
    server.sign_in("Hunter One", "secret-1")
    headers = lambda name, token: {"x-username": name, "x-token": token}  # noqa: E731
    for name, token in [("Hunter One", "secret-2"), ("HUNTER ONE", "secret-1"),
                        ("HUNTER ONE", "secret-2"), ("abc", "x"), ("Hunter One", "")]:
        status, _, _ = server.request("POST", "/jwt/internal", headers=headers(name, token))
        assert status == 401, (name, token)
    status, _, _ = server.request("POST", "/jwt/internal")
    assert status == 401
    # only a salted hash is kept
    with open(server.lobby.users.path) as f:
        users = f.read()
    assert "secret-1" not in users and "Hunter One" in users


def test_auth_failures(server):
    jwt = server.sign_in("Hunter One", "t1")
    head, body, signature = jwt.split(".")
    claims = json.loads(unb64url(body))
    forged = mh4u_lobby.b64url(json.dumps(dict(claims, sub="Hunter Two")).encode())
    none_alg = mh4u_lobby.b64url(b'{"alg":"none","typ":"JWT"}')
    bad = [None, "Bearer", "Bearer nonsense", f"Bearer {head}.{forged}.{signature}",
           f"Bearer {none_alg}.{body}.", f"Basic {jwt}"]
    for header in bad:
        headers = {"Authorization": header} if header else {}
        for method, path, data in [("POST", "/lobby", room_json()), ("GET", "/profile", None),
                                   ("POST", "/jwt/external/abc", None),
                                   ("POST", "/lobby/abc", {"players": []}),
                                   ("DELETE", "/lobby/abc", None)]:
            status, kind, _ = server.request(method, path, data, headers)
            assert status == 401 and "application/json" in kind, (header, path)
    # expiry: Azahar then asks for a new sign-in token (a 401 makes it do that once)
    server.clock.advance(mh4u_lobby.SIGN_IN_SECONDS + 1)
    status, _, _ = server.request("GET", "/profile", headers=auth(jwt))
    assert status == 401
    jwt = server.sign_in("Hunter One", "t1")
    assert server.request("GET", "/profile", headers=auth(jwt))[0] == 200


def test_failed_sign_ins_are_limited(server):
    server.sign_in("Hunter One", "right")
    wrong = {"x-username": "Hunter One", "x-token": "wrong"}
    for _ in range(20):
        assert server.request("POST", "/jwt/internal", headers=wrong)[0] == 401
    assert server.request("POST", "/jwt/internal", headers=wrong)[0] == 429
    assert server.request("POST", "/jwt/internal",
                          headers={"x-username": "Hunter One", "x-token": "right"})[0] == 429
    server.clock.advance(601)
    server.sign_in("Hunter One", "right")


def test_new_names_are_limited(server):
    for i in range(10):
        server.sign_in(f"Hunter {i}", "t")
    status, _, _ = server.request("POST", "/jwt/internal",
                                  headers={"x-username": "Hunter 10", "x-token": "t"})
    assert status == 429
    server.clock.advance(3601)
    server.sign_in("Hunter 10", "t")


def test_invite_mode(tmp_path, capsys):
    folder = tmp_path / "data"
    s = Server(folder, mode="invite")
    try:
        status, _, _ = s.request("POST", "/jwt/internal",
                                 headers={"x-username": "Hunter One", "x-token": "anything"})
        assert status == 401
        # a name the admin adds, while the lobby runs
        code, out = cli(capsys, folder, "add-user", "Hunter One")
        assert code == 0 and len(out) == 1
        s.sign_in("Hunter One", out[0])
        # an invite: claims a name of the player's choice, once
        code, out = cli(capsys, folder, "invite")
        invite = out[0]
        s.sign_in("New Hunter", invite)
        status, _, _ = s.request("POST", "/jwt/internal",
                                 headers={"x-username": "Other Hunter", "x-token": invite})
        assert status == 401
        s.sign_in("New Hunter", invite)  # now that name's token
        code, out = cli(capsys, folder, "list-users")
        assert [line.split("  ")[0].strip() for line in out[:-1]] == ["Hunter One", "New Hunter"]
        assert out[-1] == "2 name(s), 0 unused invite(s)"
    finally:
        s.stop()


def test_user_commands_reach_a_running_lobby(server, capsys):
    folder = server.lobby.folder
    code, out = cli(capsys, folder, "add-user", "Room Host", "--token", "room-token", "--moderator")
    assert (code, out) == (0, ["room-token"])

    def roles(jwt):
        reply = server.request("POST", "/jwt/external/abc", None, auth(jwt))[2].decode()
        return jwt_parts(reply)[1]["roles"]
    jwt = server.sign_in("Room Host", "room-token")
    assert roles(jwt) == ["user", "moderator"]
    # a new token ends the sign-ins made with the old one
    code, out = cli(capsys, folder, "add-user", "Room Host")
    assert server.request("GET", "/profile", headers=auth(jwt))[0] == 401
    jwt = server.sign_in("Room Host", out[0])
    assert roles(jwt) == ["user"]
    # so does removing the name
    assert cli(capsys, folder, "remove-user", "Room Host")[0] == 0
    assert server.request("GET", "/profile", headers=auth(jwt))[0] == 401
    assert cli(capsys, folder, "remove-user", "Room Host")[0] == 1
    with pytest.raises(SystemExit):
        cli(capsys, folder, "add-user", "x")  # not a name Azahar allows


# ── rooms ───────────────────────────────────────────────────────────────────────────────────

def test_only_the_owner_updates_or_closes(server):
    room = server.register(server.sign_in("Hunter One", "t1"), room_json())
    other = server.sign_in("Hunter Two", "t2")
    assert server.request("POST", f"/lobby/{room['id']}", {"players": []}, auth(other))[0] == 403
    assert server.request("DELETE", f"/lobby/{room['id']}", None, auth(other))[0] == 403
    assert [r["id"] for r in server.rooms()] == [room["id"]]


def test_rooms_expire_without_news(server):
    jwt = server.sign_in("Hunter One", "t1")
    room = server.register(jwt, room_json())
    server.clock.advance(45)
    assert server.request("POST", f"/lobby/{room['id']}", {"players": []}, auth(jwt))[0] == 200
    server.clock.advance(59)
    assert len(server.rooms()) == 1
    server.clock.advance(2)  # 61 s since the last update
    assert server.rooms() == []
    assert any("went quiet" in line for line in server.log)
    assert server.request("POST", f"/lobby/{room['id']}", {"players": []}, auth(jwt))[0] == 404


def test_room_ttl_option(tmp_path):
    s = Server(tmp_path / "data", room_ttl=10)
    try:
        s.register(s.sign_in("Hunter One", "t1"), room_json())
        s.clock.advance(11)
        assert s.rooms() == []
    finally:
        s.stop()


def test_announcing_again_replaces_the_old_entry(server):
    one, two = server.sign_in("Hunter One", "t1"), server.sign_in("Hunter Two", "t2")
    first = server.register(one, room_json())
    second = server.register(one, room_json(name="Renamed"))
    other_port = server.register(one, room_json(port=24873))
    other_owner = server.register(two, room_json())
    ids = {r["id"] for r in server.rooms()}
    assert ids == {second["id"], other_port["id"], other_owner["id"]}
    assert first["id"] not in ids


def test_room_validation(server):
    jwt = server.sign_in("Hunter One", "t1")
    bad = [b"{not json", b"[]", json.dumps(room_json(port="24872")).encode(),
           json.dumps(room_json(hasPassword=1)).encode(),
           json.dumps(room_json(preferredGameId=-1)).encode(),
           json.dumps(room_json(preferredGameId=2**64)).encode(),
           json.dumps(room_json(maxPlayers=True)).encode(),
           json.dumps(room_json(name=" ")).encode(),
           json.dumps(room_json(players=[{"nickname": "x", "gameId": 1}])).encode(),
           json.dumps(room_json(players="none")).encode(),
           json.dumps({k: v for k, v in room_json().items() if k != "netVersion"}).encode(),
           b"[" * 5000 + b"]" * 5000]
    for body in bad:
        status, kind, reply = server.request("POST", "/lobby", body, auth(jwt))
        assert status == 400 and "error" in json.loads(reply), body[:60]
    status, _, _ = server.request("POST", "/lobby", b" " * (mh4u_lobby.MAX_BODY + 1), auth(jwt))
    assert status == 413
    # control characters go (a description keeps its line breaks), long text is cut
    room = server.register(jwt, room_json(name="Hall\n\x00One", description="d" * 5000))
    assert room["name"] == "Hall One" and len(room["description"]) == 1000
    lines = server.register(jwt, room_json(port=2, description="High rank\r\nG rank\x07"))
    assert lines["description"] == "High rank\nG rank"
    assert server.rooms() == [room, lines]


def test_games_allowlist(tmp_path):
    s = Server(tmp_path / "data", games={MH4U})
    try:
        jwt = s.sign_in("Hunter One", "t1")
        status, _, _ = s.request("POST", "/lobby", room_json(preferredGameId=0x0004000000155400),
                                 auth(jwt))
        assert status == 403
        s.register(jwt, room_json())
    finally:
        s.stop()


def test_room_limits_per_address(server, monkeypatch):
    monkeypatch.setattr(mh4u_lobby, "MAX_ROOMS_PER_ADDRESS", 2)
    jwt = server.sign_in("Hunter One", "t1")
    server.register(jwt, room_json(port=1))
    server.register(jwt, room_json(port=2))
    assert server.request("POST", "/lobby", room_json(port=3), auth(jwt))[0] == 429


def test_addresses_behind_a_proxy_and_on_the_lobbys_network(tmp_path):
    s = Server(tmp_path / "data", trust_proxy=True)
    try:
        jwt = s.sign_in("Hunter One", "t1")
        assert s.register(jwt, room_json(port=1))["address"] == "127.0.0.1"
        forwarded = dict(auth(jwt), **{"X-Forwarded-For": "198.51.100.1, 203.0.113.7"})
        status, _, body = s.request("POST", "/lobby", room_json(port=3), forwarded)
        assert json.loads(body)["address"] == "203.0.113.7"  # the proxy's entry, the last one
        garbage = dict(auth(jwt), **{"X-Forwarded-For": "not an address"})
        assert json.loads(s.request("POST", "/lobby", room_json(port=4), garbage)[2])["address"] \
            == "127.0.0.1"
    finally:
        s.stop()
    # without --trust-proxy the header means nothing
    s = Server(tmp_path / "data")
    try:
        jwt = s.sign_in("Hunter One", "t1")
        forwarded = dict(auth(jwt), **{"X-Forwarded-For": "203.0.113.7"})
        assert json.loads(s.request("POST", "/lobby", room_json(), forwarded)[2])["address"] \
            == "127.0.0.1"
    finally:
        s.stop()
    # a room next to the lobby (here: on this machine) is listed at the public address
    s = Server(tmp_path / "data", public_address="lobby.example.org")
    try:
        assert s.register(s.sign_in("Hunter One", "t1"), room_json())["address"] == \
            "lobby.example.org"
        assert "announced from 127.0.0.1" in s.log[-1]
    finally:
        s.stop()


def test_client_address_rules(tmp_path):
    lobby = Server(tmp_path / "data", trust_proxy=True)
    try:
        headers = {"X-Forwarded-For": "203.0.113.7"}
        assert lobby.lobby.client_address("::ffff:127.0.0.1", {}) == "127.0.0.1"
        assert lobby.lobby.client_address("10.0.0.2", headers) == "203.0.113.7"
        # a client on the internet can't pick its own address with the header (Python counts
        # the documentation ranges as private, so this is an ordinary public address)
        assert lobby.lobby.client_address("1.2.3.4", headers) == "1.2.3.4"
    finally:
        lobby.stop()


# ── the tokens rooms check (RS256) ──────────────────────────────────────────────────────────

def room_token(server, token="t1", uid="0b1c2d3e-aaaa-4bbb-8ccc-123456789abc"):
    jwt = server.sign_in("Hunter One", token)
    status, kind, body = server.request("POST", f"/jwt/external/{uid}", None, auth(jwt))
    assert status == 200 and "text/html" in kind
    return body.decode()


def test_room_token_claims(server):
    uid = "0b1c2d3e-aaaa-4bbb-8ccc-123456789abc"
    header, claims, _ = jwt_parts(room_token(server, uid=uid))
    # what Azahar's VerifyUserJWT asks of cpp-jwt, and the claims it reads
    assert header == {"alg": "RS256", "typ": "JWT"}
    assert claims["iss"] == "citra-core" and claims["aud"] == f"external-{uid}"
    assert type(claims["iat"]) is int and claims["exp"] > claims["iat"] and claims["jti"]
    assert "nbf" not in claims
    assert (claims["username"], claims["displayName"], claims["avatarUrl"]) == \
        ("Hunter One", "Hunter One", "")
    assert claims["roles"] == ["user"]


def test_room_token_signature(server):
    token = room_token(server)
    pem = server.request("GET", "/jwt/external/key.pem")[2].decode()
    assert public_numbers(pem) == (P * Q, 65537)
    assert rs256_valid(token, pem)
    head, body, signature = token.split(".")
    other = mh4u_lobby.b64url(json.dumps(dict(jwt_parts(token)[1], username="Someone")).encode())
    assert not rs256_valid(f"{head}.{other}.{signature}", pem)


@pytest.mark.skipif(shutil.which("openssl") is None, reason="openssl not installed")
def test_room_token_verifies_with_openssl(server, tmp_path):
    token = room_token(server)
    pem = server.request("GET", "/jwt/external/key.pem")[2].decode()
    assert openssl_verifies(token, pem, tmp_path)
    head, body, signature = token.split(".")
    assert not openssl_verifies(f"{head}.{body}x.{signature}", pem, tmp_path)


# ── state on disk ───────────────────────────────────────────────────────────────────────────

def test_state_survives_a_restart(tmp_path):
    folder, clock = tmp_path / "data", Clock()
    first = Server(folder, clock)
    jwt = first.sign_in("Hunter One", "t1")
    pem = first.request("GET", "/jwt/external/key.pem")[2]
    first.register(jwt, room_json())
    first.stop()
    second = Server(folder, clock)
    try:
        assert second.request("GET", "/jwt/external/key.pem")[2] == pem
        assert second.request("GET", "/profile", headers=auth(jwt))[0] == 200  # same secret
        second.sign_in("Hunter One", "t1")
        status, _, _ = second.request("POST", "/jwt/internal",
                                      headers={"x-username": "Hunter One", "x-token": "t2"})
        assert status == 401
        assert second.rooms() == []  # rooms announce themselves again (their update gets 404)
    finally:
        second.stop()


def test_a_new_data_folder_gets_its_own_key(tmp_path):
    folder = tmp_path / "fresh"
    lobby = mh4u_lobby.Lobby(str(folder), log=lambda line: None, key_bits=1024)
    path = folder / "lobby_key.pem"
    assert lobby.key.n.bit_length() == 1024 and lobby.key.e == 65537
    if os.name == "posix":
        assert stat.S_IMODE(os.stat(path).st_mode) == 0o600
        assert stat.S_IMODE(os.stat(folder / "jwt_secret").st_mode) == 0o600
    again = mh4u_lobby.Lobby(str(folder), log=lambda line: None)
    assert (again.key.n, again.key.d, again.secret) == (lobby.key.n, lobby.key.d, lobby.secret)
    if shutil.which("openssl"):
        r = subprocess.run(["openssl", "rsa", "-check", "-noout", "-in", str(path)],
                           capture_output=True, text=True)
        assert r.returncode == 0 and "RSA key ok" in r.stdout


def test_rsa_key_checks():
    key = mh4u_lobby.RSAKey.generate(512)
    assert key.n.bit_length() == 512 and key.p != key.q
    assert (key.e * key.d) % math.lcm(key.p - 1, key.q - 1) == 1
    assert mh4u_lobby.RSAKey.from_pem(key.private_pem()).d == key.d
    with pytest.raises(ValueError):
        mh4u_lobby.RSAKey(key.n, key.e, key.d + 2, key.p, key.q)
    assert all(mh4u_lobby.is_probable_prime(p) for p in (2, 3, 65537, P, Q))
    assert not any(mh4u_lobby.is_probable_prime(n) for n in (0, 1, 4, 561, P * Q))


# ── the log ─────────────────────────────────────────────────────────────────────────────────

def test_one_log_line_per_request_and_no_tokens(server):
    jwt = server.sign_in("Hunter One", "very-secret-token")
    room = server.register(jwt, room_json())
    token = room_token(server, "very-secret-token")  # one sign-in, one request
    server.request("POST", f"/lobby/{room['id']}", {"players": []}, auth(jwt))
    server.request("GET", "/lobby")
    server.request("DELETE", f"/lobby/{room['id']}", None, auth(jwt))
    assert len(server.log) == 7
    text = "\n".join(server.log)
    for secret in ("very-secret-token", jwt, jwt.split(".")[2], token, token.split(".")[2]):
        assert secret not in text
    assert "127.0.0.1 POST /lobby 200 [Hunter One]" in text
