// Code patches and exheader overrides from the mods folder (N3-09). Patches are written here, from the
// formats' rules, on invented bytes. The byte-for-byte comparison with Azahar's code and with
// tools/code_mods.py is tests/test_native_code_patch.py.
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "code_patch/code_patch.h"
#include "code_patch/tool.h"
#include "mh4n/crc32.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace mh4n;
using namespace mh4n::code_patch;
namespace stdfs = std::filesystem;

namespace {

using Bytes = std::vector<u8>;

struct TempDir {
    stdfs::path path;
    TempDir() {
        static std::atomic<int> counter{0};
#if defined(_WIN32)
        const int pid = _getpid();
#else
        const int pid = static_cast<int>(getpid());
#endif
        path = stdfs::temp_directory_path() / ("mh4n_code_patch_test_" + std::to_string(pid) + "_" +
                                               std::to_string(counter++));
        stdfs::remove_all(path);
        stdfs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        stdfs::remove_all(path, ec);
    }
    void Write(const std::string& rel, const Bytes& data) const {
        const stdfs::path p = path / rel;
        stdfs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
};

Bytes Code(std::size_t size, u8 seed) {
    Bytes b(size);
    for (std::size_t i = 0; i < size; ++i) {
        b[i] = static_cast<u8>(seed + i * 7);
    }
    return b;
}

void Append(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }

Bytes Ips(const std::vector<std::pair<u32, Bytes>>& records, const std::vector<std::array<u32, 3>>& rle = {},
          bool eof = true) {
    Bytes out;
    Append(out, "PATCH");
    for (const auto& [offset, data] : records) {
        out.insert(out.end(), {u8(offset >> 16), u8(offset >> 8), u8(offset), u8(data.size() >> 8), u8(data.size())});
        out.insert(out.end(), data.begin(), data.end());
    }
    for (const auto& r : rle) {  // offset, count, value
        out.insert(out.end(), {u8(r[0] >> 16), u8(r[0] >> 8), u8(r[0]), 0, 0, u8(r[1] >> 8), u8(r[1]), u8(r[2])});
    }
    if (eof) {
        Append(out, "EOF");
    }
    return out;
}

void Number(Bytes& out, u64 n) {
    while (true) {
        const u8 x = n & 0x7F;
        n >>= 7;
        if (n == 0) {
            out.push_back(0x80 | x);
            return;
        }
        out.push_back(x);
        --n;
    }
}

void Le32(Bytes& out, u32 v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(u8(v >> (8 * i)));
    }
}

// A BPS made of one TargetRead of the whole target (valid for any source).
Bytes Bps(const Bytes& source, const Bytes& target, u32 source_crc_delta = 0) {
    Bytes out;
    Append(out, "BPS1");
    Number(out, source.size());
    Number(out, target.size());
    Number(out, 0);
    Number(out, ((target.size() - 1) << 2) | 1);
    out.insert(out.end(), target.begin(), target.end());
    Le32(out, Crc32(source.data(), source.size()) + source_crc_delta);
    Le32(out, Crc32(target.data(), target.size()));
    Le32(out, Crc32(out.data(), out.size()));
    return out;
}

Bytes WithBss(Bytes code, u32 pages) {
    code.resize(code.size() + pages * 0x1000, 0);
    return code;
}

}  // namespace

TEST_CASE("an empty mods folder only appends the bss pages") {
    TempDir dir;
    Bytes code = Code(0x40, 1);
    const Outcome o = ModFolder(dir.path.string()).ApplyToCode(code, 2);
    CHECK(o.ok);
    CHECK(o.applied.empty());
    CHECK(code == WithBss(Code(0x40, 1), 2));
    CHECK_FALSE(ModFolder(dir.path.string()).ExheaderOverride().has_value());
}

TEST_CASE("an IPS patch with a record, an RLE run and the EOF marker") {
    TempDir dir;
    dir.Write("exefs/code.ips", Ips({{4, {9, 9, 9}}}, {{0x10, 5, 0xAB}}));
    Bytes code = Code(0x40, 1);
    const Outcome o = ModFolder(dir.path.string()).ApplyToCode(code, 0);
    REQUIRE(o.ok);
    CHECK(o.applied == std::vector<std::string>{"exefs/code.ips"});
    Bytes want = Code(0x40, 1);
    want[4] = want[5] = want[6] = 9;
    for (int i = 0; i < 5; ++i) {
        want[0x10 + i] = 0xAB;
    }
    CHECK(code == want);
}

TEST_CASE("an IPS patch writes into the bss pages") {
    TempDir dir;
    dir.Write("code.ips", Ips({{0x40, {1, 2, 3, 4}}, {0x1042, {5}}}));
    Bytes code = Code(0x40, 3);
    REQUIRE(ModFolder(dir.path.string()).ApplyToCode(code, 2).ok);
    Bytes want = WithBss(Code(0x40, 3), 2);
    want[0x40] = 1, want[0x41] = 2, want[0x42] = 3, want[0x43] = 4;
    want[0x1042] = 5;
    CHECK(code == want);
}

TEST_CASE("an IPS record past the end, with and without bss, is a patch error naming the file") {
    TempDir dir;
    dir.Write("exefs/code.ips", Ips({{0x1000, {1, 2}}}));
    Bytes code = Code(0x40, 1);
    Outcome o = ModFolder(dir.path.string()).ApplyToCode(code, 0);  // the buffer is 0x40 long
    CHECK_FALSE(o.ok);
    CHECK(o.error == "mods: exefs/code.ips: patch error");
    code = Code(0x40, 1);
    CHECK(ModFolder(dir.path.string()).ApplyToCode(code, 1).ok);  // 0x1040 long: fits
    dir.Write("exefs/code.ips", Ips({}, {{0x1038, 0x10, 7}}));    // an RLE run ending past 0x1040
    code = Code(0x40, 1);
    CHECK_FALSE(ModFolder(dir.path.string()).ApplyToCode(code, 1).ok);
}

TEST_CASE("a header-less or truncated IPS patch is an error") {
    TempDir dir;
    dir.Write("code.ips", {'P', 'A'});
    Bytes code = Code(0x40, 1);
    CHECK_FALSE(ModFolder(dir.path.string()).ApplyToCode(code, 0).ok);
    Bytes truncated = Ips({{4, {1, 2, 3, 4, 5, 6, 7, 8}}}, {}, false);
    truncated.resize(truncated.size() - 4);
    dir.Write("code.ips", truncated);
    code = Code(0x40, 1);
    CHECK_FALSE(ModFolder(dir.path.string()).ApplyToCode(code, 0).ok);
}

TEST_CASE("a BPS patch, and one with a wrong source CRC") {
    TempDir dir;
    const Bytes source = WithBss(Code(0x30, 5), 1);
    Bytes target = source;
    target[0x20] ^= 0xFF;
    dir.Write("exefs/code.bps", Bps(source, target));
    Bytes code = Code(0x30, 5);
    Outcome o = ModFolder(dir.path.string()).ApplyToCode(code, 1);
    REQUIRE(o.ok);
    CHECK(code == target);
    dir.Write("exefs/code.bps", Bps(source, target, 1));
    code = Code(0x30, 5);
    o = ModFolder(dir.path.string()).ApplyToCode(code, 1);
    CHECK_FALSE(o.ok);
    CHECK(o.error == "mods: exefs/code.bps: patch error");
    CHECK(o.detail == "source CRC-32 differs");
}

TEST_CASE("a code.bin replacement comes before the bss pages and the patch") {
    TempDir dir;
    dir.Write("code.bin", Code(0x20, 9));
    dir.Write("exefs/code.ips", Ips({{0x1, {0xEE}}}));
    Bytes code = Code(0x40, 1);
    const Outcome o = ModFolder(dir.path.string()).ApplyToCode(code, 1);
    REQUIRE(o.ok);
    CHECK(o.applied == std::vector<std::string>{"code.bin", "exefs/code.ips"});
    Bytes want = WithBss(Code(0x20, 9), 1);
    want[1] = 0xEE;
    CHECK(code == want);
}

TEST_CASE("exefs/code.bin wins over code.bin") {
    TempDir dir;
    dir.Write("code.bin", Code(0x20, 9));
    dir.Write("exefs/code.bin", Code(0x18, 2));
    Bytes code = Code(0x40, 1);
    REQUIRE(ModFolder(dir.path.string()).ApplyToCode(code, 0).ok);
    CHECK(code == Code(0x18, 2));
}

TEST_CASE("only the first patch of the precedence order is applied") {
    const char* order[] = {"exefs/code.ips", "exefs/code.bps", "code.ips", "code.bps"};
    for (int first = 0; first < 4; ++first) {
        TempDir dir;
        const Bytes source = Code(0x30, 4);
        for (int i = first; i < 4; ++i) {
            const std::string name = order[i];
            const u8 value = static_cast<u8>(0x10 + i);
            if (name.ends_with(".ips")) {
                dir.Write(name, Ips({{0, {value}}}));
            } else {
                Bytes target = source;
                target[0] = value;
                dir.Write(name, Bps(source, target));
            }
        }
        Bytes code = source;
        const Outcome o = ModFolder(dir.path.string()).ApplyToCode(code, 0);
        REQUIRE(o.ok);
        CHECK(o.applied == std::vector<std::string>{order[first]});
        CHECK(code[0] == 0x10 + first);
    }
}

TEST_CASE("a bad first patch is an error even when a good one follows") {
    TempDir dir;
    dir.Write("exefs/code.ips", Ips({{0x9999, {1}}}));
    dir.Write("code.ips", Ips({{0, {1}}}));
    Bytes code = Code(0x40, 1);
    CHECK_FALSE(ModFolder(dir.path.string()).ApplyToCode(code, 0).ok);
}

TEST_CASE("the exheader override is the first 0x800 bytes of exheader.bin") {
    TempDir dir;
    dir.Write("exheader.bin", Code(0x7FF, 1));
    CHECK_FALSE(ModFolder(dir.path.string()).ExheaderOverride().has_value());
    dir.Write("exheader.bin", Code(0x900, 1));
    const auto exh = ModFolder(dir.path.string()).ExheaderOverride();
    REQUIRE(exh.has_value());
    CHECK(*exh == Code(0x800, 1));
}

TEST_CASE("mods paths, update ids and bss pages") {
    CHECK(ModId(0x0004000E00126300ull) == 0x0004000000126300ull);
    CHECK(ModId(0x0004000000126300ull) == 0x0004000000126300ull);
    CHECK(ModsPath("/u/load", 0x0004000E00126300ull) == "/u/load/mods/0004000000126300");
    CHECK(BssPages(0) == 0);
    CHECK(BssPages(1) == 1);
    CHECK(BssPages(0x1000) == 1);
    CHECK(BssPages(0x1001) == 2);
}

TEST_CASE("the tool: exit 0, exit 2 on a patch error, exit 1 on a bad line") {
    TempDir dir;
    dir.Write("mods/code.ips", Ips({{0x41, {7}}}));
    dir.Write("in.bin", Code(0x40, 1));
    const std::string mods = (dir.path / "mods").string(), in = (dir.path / "in.bin").string(),
                      out_path = (dir.path / "out.bin").string();
    std::ostringstream out, err;
    CHECK(RunTool({"--mods", mods, "--bss-pages", "1", in, out_path}, out, err) == 0);
    std::ifstream f(out_path, std::ios::binary);
    Bytes got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Bytes want = WithBss(Code(0x40, 1), 1);
    want[0x41] = 7;
    CHECK(got == want);
    std::ostringstream out2, err2;
    CHECK(RunTool({"--mods", mods, "--bss-pages", "0", in, out_path}, out2, err2) == 2);
    CHECK(err2.str().find("mods: code.ips: patch error") != std::string::npos);
    CHECK(RunTool({"--mods", mods, in, out_path}, out2, err2) == 1);
}
