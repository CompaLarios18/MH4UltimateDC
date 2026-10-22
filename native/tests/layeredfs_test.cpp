// Derived from Azahar src/core/file_sys/layered_fs.cpp and src/core/file_sys/patch.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: both say "GPLv2 or any later version"): the
// layout rules of a RomFS (3dbrew's hash of a name, the alignment of file data) that the tests check the
// rebuilt images against, and the rules of the IPS and BPS formats.
// Copyright 2020-2026 Citra Emulator Project / Azahar Emulator Project (layered_fs.cpp)
// Copyright 2019-2026 Citra Emulator Project / Azahar Emulator Project (patch.cpp)
//
// LayeredFS (N3-10). The base images are made here, by a RomFS writer of the test's own (independent of the
// module); every image the module builds is read back by a parser of the test's own that checks the whole
// structure, the hash tables included. The byte-for-byte comparison with Azahar's code is
// tests/test_native_layeredfs.py.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../src/layeredfs/layeredfs.h"
#include "../src/layeredfs/patch.h"
#include "../src/layeredfs/tool.h"
#include "mh4n/crc32.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace mh4n;
namespace stdfs = std::filesystem;

namespace {

using Bytes = std::vector<u8>;

Bytes B(const std::string& text) { return Bytes(text.begin(), text.end()); }

Bytes Pattern(std::size_t size, u8 seed) {
    Bytes data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<u8>(seed + i * 7 + (i >> 8));
    }
    return data;
}

int Pid() {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

struct TempDir {
    stdfs::path path;
    TempDir() {
        static std::atomic<int> counter{0};
        path = stdfs::temp_directory_path() / ("mh4n_layeredfs_test_" + std::to_string(Pid()) + "_" +
                                               std::to_string(counter++));
        stdfs::remove_all(path);
        stdfs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        stdfs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};

void WriteFile(const stdfs::path& path, const Bytes& data) {
    stdfs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

// ---- a RomFS writer and parser of the test's own (3dbrew's layout) --------------------------------------

struct Tree {
    std::map<std::string, Bytes> files;  // "dir/sub/name" -> contents (names in UTF-8)
    std::set<std::string> dirs;          // folders that exist besides the ones the files imply ("dir/sub")
};

u32 Hash(const std::u16string& name, u32 parent) {
    u32 h = parent ^ 123456789;
    for (const char16_t c : name) {
        h = (h >> 5) | (h << 27);
        h ^= static_cast<u16>(c);
    }
    return h;
}

std::u16string To16(const std::string& s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i]);
        u32 cp = 0;
        int n = 0;
        if (c < 0x80) {
            cp = c, n = 1;
        } else if (c < 0xE0) {
            cp = c & 0x1F, n = 2;
        } else if (c < 0xF0) {
            cp = c & 0x0F, n = 3;
        } else {
            cp = c & 0x07, n = 4;
        }
        for (int k = 1; k < n; ++k) {
            cp = cp << 6 | (static_cast<u8>(s[i + k]) & 0x3F);
        }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string To8(const std::u16string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        u32 cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size()) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | cp >> 6));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | cp >> 12));
            out.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | cp >> 18));
            out.push_back(static_cast<char>(0x80 | (cp >> 12 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

void Put32(Bytes& b, std::size_t at, u32 v) {
    for (int i = 0; i < 4; ++i) {
        b[at + i] = static_cast<u8>(v >> (8 * i));
    }
}
void Put64(Bytes& b, std::size_t at, u64 v) {
    Put32(b, at, static_cast<u32>(v));
    Put32(b, at + 4, static_cast<u32>(v >> 32));
}
u32 Get32(const Bytes& b, std::size_t at) {
    return u32(b[at]) | u32(b[at + 1]) << 8 | u32(b[at + 2]) << 16 | u32(b[at + 3]) << 24;
}
u64 Get64(const Bytes& b, std::size_t at) { return Get32(b, at) | u64(Get32(b, at + 4)) << 32; }

// The base images are laid out depth first, folders before their files' data; the order is not the module's.
struct Layout {
    Bytes image;
    std::size_t data_start = 0;  // the file data offset of the header
};

// With `force_size` every file has that size and the image holds no file data (a base too big to exist).
Layout WriteRomFs(const Tree& tree, u64 force_size = 0) {
    // folders: every prefix of a file's path and the explicit ones
    std::set<std::string> dirs{""};
    const auto add_prefixes = [&](const std::string& path) {
        for (std::size_t i = 0; i < path.size(); ++i) {
            if (path[i] == '/') {
                dirs.insert(path.substr(0, i));
            }
        }
    };
    for (const auto& [path, data] : tree.files) {
        add_prefixes(path);
    }
    for (const std::string& d : tree.dirs) {
        dirs.insert(d);
        add_prefixes(d + "/x");
    }
    const auto parent_of = [](const std::string& p) {
        const auto slash = p.rfind('/');
        return slash == std::string::npos ? std::string() : p.substr(0, slash);
    };
    const auto leaf = [](const std::string& p) {
        const auto slash = p.rfind('/');
        return slash == std::string::npos ? p : p.substr(slash + 1);
    };

    std::vector<std::string> dir_order;  // depth first, names sorted
    std::map<std::string, std::vector<std::string>> child_dirs, child_files;
    for (const std::string& d : dirs) {
        if (!d.empty()) {
            child_dirs[parent_of(d)].push_back(d);
        }
    }
    for (const auto& [path, data] : tree.files) {
        child_files[parent_of(path)].push_back(path);
    }
    std::vector<std::string> file_order;
    const auto walk = [&](auto&& self, const std::string& d) -> void {
        dir_order.push_back(d);
        for (const std::string& f : child_files[d]) {
            file_order.push_back(f);
        }
        for (const std::string& c : child_dirs[d]) {
            self(self, c);
        }
    };
    walk(walk, "");

    std::map<std::string, u32> dir_off, file_off;
    u32 dpos = 0, fpos = 0;
    for (const std::string& d : dir_order) {
        dir_off[d] = dpos;
        dpos += 0x18 + ((To16(leaf(d)).size() * 2 + 3) & ~3u);
    }
    for (const std::string& f : file_order) {
        file_off[f] = fpos;
        fpos += 0x20 + ((To16(leaf(f)).size() * 2 + 3) & ~3u);
    }
    const auto table_size = [](std::size_t n) -> std::size_t {
        if (n < 3) return 3;
        if (n < 19) return n | 1;
        while (n % 2 == 0 || n % 3 == 0 || n % 5 == 0 || n % 7 == 0 || n % 11 == 0 || n % 13 == 0 || n % 17 == 0) ++n;
        return n;
    };
    std::vector<u32> dhash(table_size(dir_order.size()), 0xFFFFFFFF), fhash(table_size(file_order.size()), 0xFFFFFFFF);
    Bytes dmeta(dpos, 0xFF), fmeta(fpos, 0xFF);

    u64 data_pos = 0;
    std::map<std::string, u64> data_off;
    const auto size_of = [&](const std::string& f) -> u64 {
        return force_size != 0 ? force_size : tree.files.at(f).size();
    };
    for (const std::string& f : file_order) {
        data_off[f] = data_pos;
        data_pos += (size_of(f) + 15) & ~u64{15};
    }

    for (const std::string& d : dir_order) {
        const std::size_t at = dir_off[d];
        const u32 parent = d.empty() ? 0 : dir_off[parent_of(d)];
        u32 sibling = 0xFFFFFFFF;
        if (!d.empty()) {
            const auto& list = child_dirs[parent_of(d)];
            const auto it = std::find(list.begin(), list.end(), d);
            if (it + 1 != list.end()) sibling = dir_off[*(it + 1)];
        }
        const auto& cd = child_dirs[d];
        const auto& cf = child_files[d];
        const std::u16string name = To16(leaf(d));
        const std::size_t bucket = Hash(name, parent) % dhash.size();
        Put32(dmeta, at + 0, parent);
        Put32(dmeta, at + 4, sibling);
        Put32(dmeta, at + 8, cd.empty() ? 0xFFFFFFFF : dir_off[cd.front()]);
        Put32(dmeta, at + 12, cf.empty() ? 0xFFFFFFFF : file_off[cf.front()]);
        Put32(dmeta, at + 16, dhash[bucket]);
        Put32(dmeta, at + 20, static_cast<u32>(name.size() * 2));
        dhash[bucket] = dir_off[d];
        for (std::size_t i = 0; i < name.size(); ++i) {
            dmeta[at + 0x18 + 2 * i] = static_cast<u8>(name[i]);
            dmeta[at + 0x18 + 2 * i + 1] = static_cast<u8>(name[i] >> 8);
        }
    }
    for (const std::string& f : file_order) {
        const std::size_t at = file_off[f];
        const std::string parent_dir = parent_of(f);
        const u32 parent = dir_off[parent_dir];
        u32 sibling = 0xFFFFFFFF;
        const auto& list = child_files[parent_dir];
        const auto it = std::find(list.begin(), list.end(), f);
        if (it + 1 != list.end()) sibling = file_off[*(it + 1)];
        const std::u16string name = To16(leaf(f));
        const std::size_t bucket = Hash(name, parent) % fhash.size();
        Put32(fmeta, at + 0, parent);
        Put32(fmeta, at + 4, sibling);
        Put64(fmeta, at + 8, data_off[f]);
        Put64(fmeta, at + 16, size_of(f));
        Put32(fmeta, at + 24, fhash[bucket]);
        Put32(fmeta, at + 28, static_cast<u32>(name.size() * 2));
        fhash[bucket] = file_off[f];
        for (std::size_t i = 0; i < name.size(); ++i) {
            fmeta[at + 0x20 + 2 * i] = static_cast<u8>(name[i]);
            fmeta[at + 0x20 + 2 * i + 1] = static_cast<u8>(name[i] >> 8);
        }
    }

    Layout out;
    const u32 dh_off = 0x28, dh_len = static_cast<u32>(dhash.size() * 4);
    const u32 dm_off = dh_off + dh_len, dm_len = static_cast<u32>(dmeta.size());
    const u32 fh_off = dm_off + dm_len, fh_len = static_cast<u32>(fhash.size() * 4);
    const u32 fm_off = fh_off + fh_len, fm_len = static_cast<u32>(fmeta.size());
    const u32 data_off0 = (fm_off + fm_len + 15) & ~15u;
    out.data_start = data_off0;
    out.image.assign(data_off0, 0);
    Put32(out.image, 0, 0x28);
    Put32(out.image, 4, dh_off);
    Put32(out.image, 8, dh_len);
    Put32(out.image, 12, dm_off);
    Put32(out.image, 16, dm_len);
    Put32(out.image, 20, fh_off);
    Put32(out.image, 24, fh_len);
    Put32(out.image, 28, fm_off);
    Put32(out.image, 32, fm_len);
    Put32(out.image, 36, data_off0);
    for (std::size_t i = 0; i < dhash.size(); ++i) Put32(out.image, dh_off + 4 * i, dhash[i]);
    std::copy(dmeta.begin(), dmeta.end(), out.image.begin() + dm_off);
    for (std::size_t i = 0; i < fhash.size(); ++i) Put32(out.image, fh_off + 4 * i, fhash[i]);
    std::copy(fmeta.begin(), fmeta.end(), out.image.begin() + fm_off);
    if (force_size == 0) {
        for (const std::string& f : file_order) {
            Bytes data = tree.files.at(f);
            data.resize((data.size() + 15) & ~std::size_t{15}, 0);
            out.image.insert(out.image.end(), data.begin(), data.end());
        }
    }
    return out;
}

struct Parsed {
    std::string error;  // empty if the structure is sound
    Tree tree;
};

// Reads an image back and checks it all: the header, the tables, every link, the hash chains.
Parsed ParseRomFs(const Bytes& image) {
    Parsed result;
    const auto fail = [&](const std::string& why) {
        if (result.error.empty()) result.error = why;
    };
    if (image.size() < 0x28 || Get32(image, 0) != 0x28) {
        fail("header");
        return result;
    }
    const u32 dh_off = Get32(image, 4), dh_len = Get32(image, 8), dm_off = Get32(image, 12), dm_len = Get32(image, 16);
    const u32 fh_off = Get32(image, 20), fh_len = Get32(image, 24), fm_off = Get32(image, 28), fm_len = Get32(image, 32);
    const u32 data0 = Get32(image, 36);
    if (dh_off != 0x28 || dm_off != dh_off + dh_len || fh_off != dm_off + dm_len || fm_off != fh_off + fh_len ||
        data0 != ((fm_off + fm_len + 15) & ~15u) || data0 > image.size()) {
        fail("table layout");
        return result;
    }
    const auto chain_has = [&](u32 hash_off, u32 meta_off, u32 meta_len, u32 hash_next_at, u32 wanted_offset,
                               u32 bucket) {
        u32 at = Get32(image, hash_off + 4 * bucket);
        for (int guard = 0; at != 0xFFFFFFFF && guard < 100000; ++guard) {
            if (at == wanted_offset) return true;
            if (at + 0x18 > meta_len) return false;
            at = Get32(image, meta_off + at + hash_next_at);
        }
        return false;
    };
    std::set<u32> seen_dirs, seen_files;
    u64 expected_data = 0;
    std::vector<std::pair<u32, std::string>> stack{{0, ""}};  // directory offset, path
    const auto name_at = [&](u32 base, u32 entry) {
        const u32 len = Get32(image, base + entry + (base == dm_off ? 20 : 28));
        std::u16string n;
        const u32 head = base == dm_off ? 0x18 : 0x20;
        for (u32 i = 0; i < len / 2; ++i)
            n.push_back(static_cast<char16_t>(image[base + entry + head + 2 * i] | image[base + entry + head + 2 * i + 1] << 8));
        return n;
    };
    while (!stack.empty() && result.error.empty()) {
        const auto [doff, dpath] = stack.back();
        stack.pop_back();
        if (!seen_dirs.insert(doff).second) {
            fail("directory visited twice");
            break;
        }
        if (doff + 0x18 > dm_len) {
            fail("directory outside the table");
            break;
        }
        const u32 e = dm_off + doff;
        const u32 parent = Get32(image, e), first_child = Get32(image, e + 8), first_file = Get32(image, e + 12);
        const std::u16string dname = name_at(dm_off, doff);
        if (dpath.empty() && (doff != 0 || parent != 0 || !dname.empty())) fail("root");
        const std::size_t db = Hash(dname, parent) % (dh_len / 4);
        if (!chain_has(dh_off, dm_off, dm_len, 16, doff, static_cast<u32>(db))) fail("directory not in its hash chain");
        result.tree.dirs.insert(dpath);
        // files
        for (u32 f = first_file; f != 0xFFFFFFFF && result.error.empty(); f = Get32(image, fm_off + f + 4)) {
            if (f + 0x20 > fm_len || !seen_files.insert(f).second) {
                fail("file entry");
                break;
            }
            const u32 fe = fm_off + f;
            if (Get32(image, fe) != doff) fail("file parent");
            const std::u16string fname = name_at(fm_off, f);
            const std::size_t fb = Hash(fname, doff) % (fh_len / 4);
            if (!chain_has(fh_off, fm_off, fm_len, 24, f, static_cast<u32>(fb))) fail("file not in its hash chain");
            const u64 off = Get64(image, fe + 8), len = Get64(image, fe + 16);
            if (off != expected_data) fail("file data not contiguous and 16-aligned in table order");
            expected_data += (len + 15) & ~u64{15};
            if (data0 + off + len > image.size()) {
                fail("file data outside the image");
                break;
            }
            const std::string path = dpath.empty() ? To8(fname) : dpath + "/" + To8(fname);
            result.tree.files[path] = Bytes(image.begin() + data0 + off, image.begin() + data0 + off + len);
            // the padding to 16 is zero
            for (u64 p = len; p < ((len + 15) & ~u64{15}); ++p)
                if (image[data0 + off + p] != 0) fail("padding not zero");
        }
        // children, pushed so they pop in order
        std::vector<u32> kids;
        for (u32 c = first_child; c != 0xFFFFFFFF; c = Get32(image, dm_off + c + 4)) {
            if (c + 0x18 > dm_len || kids.size() > dm_len) {
                fail("child directory entry");
                break;
            }
            kids.push_back(c);
        }
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
            if (Get32(image, dm_off + *it) != doff) fail("directory parent");
            const std::string n = To8(name_at(dm_off, *it));
            stack.emplace_back(*it, dpath.empty() ? n : dpath + "/" + n);
        }
    }
    if (result.error.empty() && image.size() != data0 + expected_data) fail("image size");
    // every entry of both tables was reached
    if (result.error.empty()) {
        std::size_t dentries = 0, fentries = 0;
        for (u32 at = 0; at < dm_len; at += 0x18 + ((Get32(image, dm_off + at + 20) + 3) & ~3u)) ++dentries;
        for (u32 at = 0; at < fm_len; at += 0x20 + ((Get32(image, fm_off + at + 28) + 3) & ~3u)) ++fentries;
        if (dentries != seen_dirs.size()) fail("directory table has entries nothing links to");
        if (fentries != seen_files.size()) fail("file table has entries nothing links to");
    }
    return result;
}

// ---- helpers over the module ---------------------------------------------------------------------------

Bytes ReadAll(fs::FileBackend& file) {
    Bytes out(static_cast<std::size_t>(file.GetSize()));
    std::size_t got = 0;
    file.Read(0, out.size(), out.data(), &got);
    CHECK(got == out.size());
    return out;
}

struct Fixture {
    TempDir dir;
    Tree base;
    Layout layout;
    stdfs::path base_path;
    stdfs::path romfs() const { return dir.path / "mods" / "romfs"; }
    stdfs::path ext() const { return dir.path / "mods" / "romfs_ext"; }

    explicit Fixture(Tree tree) : base(std::move(tree)) {
        layout = WriteRomFs(base);
        base_path = dir.path / "base.bin";
        WriteFile(base_path, layout.image);
    }
    std::shared_ptr<fs::FileBackend> Build(std::string* error = nullptr) {
        std::string message;
        auto file = fs::PositionedFile::Open(base_path.string(), &message);
        REQUIRE(file != nullptr);
        return layeredfs::Build(file, 0, file->Size(), romfs().string(), ext().string(), error);
    }
    Parsed Run() {
        auto image = Build();
        REQUIRE(image != nullptr);
        return ParseRomFs(ReadAll(*image));
    }
};

Tree SampleTree() {
    Tree t;
    t.files["a.txt"] = B("alpha");
    t.files["b.bin"] = Pattern(100, 1);
    t.files["c.bin"] = Pattern(32, 2);
    t.files["data/x.arc"] = Pattern(1000, 3);
    t.files["data/y.arc"] = Pattern(17, 4);
    t.files["data/z.arc"] = Pattern(4096, 5);
    t.files["data/sub/deep.bin"] = Pattern(50, 6);
    t.files["eng/data/core.arc"] = Pattern(300, 7);
    t.files["empty.bin"] = {};
    t.dirs.insert("hollow");
    return t;
}

// ---- patches --------------------------------------------------------------------------------------------

Bytes Ips(const std::vector<std::pair<u32, Bytes>>& records, bool eof = true) {
    Bytes p = B("PATCH");
    for (const auto& [offset, data] : records) {
        p.push_back(static_cast<u8>(offset >> 16));
        p.push_back(static_cast<u8>(offset >> 8));
        p.push_back(static_cast<u8>(offset));
        p.push_back(static_cast<u8>(data.size() >> 8));
        p.push_back(static_cast<u8>(data.size()));
        p.insert(p.end(), data.begin(), data.end());
    }
    if (eof) {
        const Bytes tail = B("EOF");
        p.insert(p.end(), tail.begin(), tail.end());
    }
    return p;
}

Bytes IpsRle(u32 offset, u16 count, u8 value) {
    Bytes p = B("PATCH");
    p.push_back(static_cast<u8>(offset >> 16));
    p.push_back(static_cast<u8>(offset >> 8));
    p.push_back(static_cast<u8>(offset));
    p.push_back(0);
    p.push_back(0);
    p.push_back(static_cast<u8>(count >> 8));
    p.push_back(static_cast<u8>(count));
    p.push_back(value);
    const Bytes tail = B("EOF");
    p.insert(p.end(), tail.begin(), tail.end());
    return p;
}

void PutNumber(Bytes& out, u64 n) {
    for (;;) {
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

void Le32Append(Bytes& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

// A BPS patch from `source` to `target`: SourceRead where they agree at the same position, TargetRead elsewhere.
Bytes Bps(const Bytes& source, const Bytes& target, u32 source_crc_delta = 0, u32 target_crc_delta = 0) {
    Bytes p = B("BPS1");
    PutNumber(p, source.size());
    PutNumber(p, target.size());
    PutNumber(p, 0);
    std::size_t i = 0;
    while (i < target.size()) {
        const bool same = i < source.size() && source[i] == target[i];
        std::size_t j = i;
        while (j < target.size() && (j < source.size() && source[j] == target[j]) == same) ++j;
        const std::size_t length = j - i;
        PutNumber(p, ((length - 1) << 2) | (same ? 0 : 1));
        if (!same) p.insert(p.end(), target.begin() + i, target.begin() + j);
        i = j;
    }
    Le32Append(p, Crc32(source.data(), source.size()) + source_crc_delta);
    Le32Append(p, Crc32(target.data(), target.size()) + target_crc_delta);
    Le32Append(p, Crc32(p.data(), p.size()));
    return p;
}

}  // namespace


namespace {

std::set<std::string> DirsOf(const Tree& tree) {
    std::set<std::string> dirs{""};
    const auto add = [&](const std::string& path) {
        for (std::size_t i = 0; i < path.size(); ++i) {
            if (path[i] == '/') dirs.insert(path.substr(0, i));
        }
    };
    for (const auto& [path, data] : tree.files) add(path);
    for (const std::string& d : tree.dirs) {
        dirs.insert(d);
        add(d + "/x");
    }
    return dirs;
}

// The parsed image is sound and holds exactly the tree.
void ExpectTree(const Parsed& parsed, const Tree& expected) {
    INFO("structure: " << parsed.error);
    CHECK(parsed.error.empty());
    CHECK(parsed.tree.dirs == DirsOf(expected));
    for (const auto& [path, data] : expected.files) {
        INFO("file " << path);
        const auto found = parsed.tree.files.find(path);
        REQUIRE(found != parsed.tree.files.end());
        CHECK(found->second.size() == data.size());
        CHECK(found->second == data);
    }
    for (const auto& [path, data] : parsed.tree.files) {
        INFO("unexpected file " << path);
        CHECK(expected.files.count(path) == 1);
    }
}

}  // namespace

TEST_CASE("the test's RomFS writer and parser agree on the base images") {
    Fixture f(SampleTree());
    ExpectTree(ParseRomFs(f.layout.image), f.base);
    Bytes broken = f.layout.image;
    Put32(broken, 0x28 + 4, 0x12345678);  // a hash chain that leads nowhere
    CHECK(!ParseRomFs(broken).error.empty());
}

TEST_CASE("the writer and the parser agree on names outside ASCII") {
    Tree t;
    t.files["caf\xC3\xA9.txt"] = B("coffee");
    t.files["\xE6\x97\xA5\xE6\x9C\xAC/\xE8\xAA\x9E.bin"] = Pattern(40, 1);
    t.files["\xF0\x9D\x84\x9E/clef.bin"] = Pattern(20, 2);
    const Layout layout = WriteRomFs(t);
    ExpectTree(ParseRomFs(layout.image), t);
}

TEST_CASE("an empty or missing mods folder gives the base's files, and the same bytes when built again") {
    Fixture f(SampleTree());
    SUBCASE("no folders") { ExpectTree(f.Run(), f.base); }
    SUBCASE("empty folders") {
        stdfs::create_directories(f.romfs());
        stdfs::create_directories(f.ext());
        ExpectTree(f.Run(), f.base);
    }
    SUBCASE("a layer made again from its own image") {
        auto once = f.Build();
        REQUIRE(once != nullptr);
        const Bytes first = ReadAll(*once);
        WriteFile(f.dir.path / "again.bin", first);
        std::string message;
        auto file = fs::PositionedFile::Open((f.dir.path / "again.bin").string(), &message);
        REQUIRE(file != nullptr);
        auto twice = layeredfs::Build(file, 0, file->Size(), "", "");
        REQUIRE(twice != nullptr);
        CHECK(ReadAll(*twice) == first);
    }
}

TEST_CASE("replacement and new files, in existing and new folders") {
    Fixture f(SampleTree());
    WriteFile(f.romfs() / "a.txt", Pattern(200, 9));                 // bigger
    WriteFile(f.romfs() / "data" / "y.arc", B("tiny"));              // smaller
    WriteFile(f.romfs() / "data" / "z.arc", Bytes());                // empty
    WriteFile(f.romfs() / "new.bin", Pattern(33, 10));               // new, at the top
    WriteFile(f.romfs() / "data" / "sub" / "more.bin", Pattern(70, 11));
    WriteFile(f.romfs() / "newdir" / "inner" / "f.bin", Pattern(16, 12));  // new folders
    WriteFile(f.romfs() / "newdir" / "g.bin", Pattern(15, 13));
    stdfs::create_directories(f.romfs() / "emptydir");
    WriteFile(f.romfs() / "A.TXT", B("names are case sensitive"));

    Tree expected = f.base;
    expected.files["a.txt"] = Pattern(200, 9);
    expected.files["data/y.arc"] = B("tiny");
    expected.files["data/z.arc"] = {};
    expected.files["new.bin"] = Pattern(33, 10);
    expected.files["data/sub/more.bin"] = Pattern(70, 11);
    expected.files["newdir/inner/f.bin"] = Pattern(16, 12);
    expected.files["newdir/g.bin"] = Pattern(15, 13);
    expected.files["A.TXT"] = B("names are case sensitive");
    expected.dirs.insert("emptydir");
    ExpectTree(f.Run(), expected);
}

TEST_CASE("an IPS and a BPS patch, in the top of romfs_ext and in sub-folders") {
    Fixture f(SampleTree());
    Tree expected = f.base;

    // IPS: two records and an RLE record
    Bytes x = f.base.files["data/x.arc"];
    WriteFile(f.ext() / "data" / "x.arc.ips",
              [&] {
                  Bytes p = Ips({{0, B("PATCHED")}, {990, B("0123456789")}}, false);
                  const Bytes rle = IpsRle(100, 50, 0xEE);
                  p.insert(p.end(), rle.begin() + 5, rle.end());  // the RLE record and the EOF, no second header
                  return p;
              }());
    std::copy_n("PATCHED", 7, x.begin());
    std::copy_n("0123456789", 10, x.begin() + 990);
    std::fill_n(x.begin() + 100, 50, 0xEE);
    expected.files["data/x.arc"] = x;

    // IPS at the top of the folder, on a file of the root
    WriteFile(f.ext() / "c.bin.ips", Ips({{3, B("xyz")}}));
    Bytes c = f.base.files["c.bin"];
    std::copy_n("xyz", 3, c.begin() + 3);
    expected.files["c.bin"] = c;

    // BPS in a sub-folder two levels down: same size, changed bytes
    Bytes deep = f.base.files["data/sub/deep.bin"];
    Bytes deep_new = deep;
    for (std::size_t i = 10; i < 20; ++i) deep_new[i] ^= 0x55;
    WriteFile(f.ext() / "data" / "sub" / "deep.bin.bps", Bps(deep, deep_new));
    expected.files["data/sub/deep.bin"] = deep_new;

    // BPS that makes a file bigger (the file grows to the target size)
    Bytes core = f.base.files["eng/data/core.arc"];
    Bytes core_new = core;
    core_new.resize(core.size() + 123, 0xAB);
    core_new[0] = 0;
    WriteFile(f.ext() / "eng" / "data" / "core.arc.bps", Bps(core, core_new));
    expected.files["eng/data/core.arc"] = core_new;

    // BPS on a file that stays the size of the original when the target is smaller (zeros after the target)
    Bytes b = f.base.files["b.bin"];
    Bytes b_new(b.begin(), b.begin() + 60);
    WriteFile(f.ext() / "b.bin.bps", Bps(b, b_new));
    Bytes b_kept = b_new;
    b_kept.resize(b.size(), 0);
    expected.files["b.bin"] = b_kept;

    ExpectTree(f.Run(), expected);
}

TEST_CASE("a stub removes a file: in the middle, first, last, all of a folder, and one that is not there") {
    Fixture f(SampleTree());
    Tree expected = f.base;
    const auto stub = [&](const std::string& path) {
        WriteFile(f.ext() / (path + ".stub"), {});
        expected.files.erase(path);
        const auto slash = path.rfind('/');
        if (slash != std::string::npos) expected.dirs.insert(path.substr(0, slash));  // the folder stays
    };
    SUBCASE("in the middle") { stub("data/y.arc"); }
    SUBCASE("the first file of a folder (Azahar's code throws here)") { stub("a.txt"); }
    SUBCASE("the first file of a sub-folder") { stub("data/x.arc"); }
    SUBCASE("the last file of a folder") { stub("data/z.arc"); }
    SUBCASE("every file of a folder") {
        stub("data/x.arc");
        stub("data/y.arc");
        stub("data/z.arc");
    }
    SUBCASE("the only file of a folder") { stub("data/sub/deep.bin"); }
    SUBCASE("a file that does not exist, and one in a folder that does not exist") {
        WriteFile(f.ext() / "nothing.bin.stub", {});
        WriteFile(f.ext() / "nowhere" / "nothing.bin.stub", {});
    }
    SUBCASE("a stub after a patch of the same file removes it") {
        WriteFile(f.ext() / "data" / "x.arc.ips", Ips({{0, B("zz")}}));
        stub("data/x.arc");
    }
    ExpectTree(f.Run(), expected);
}

TEST_CASE("a patch that fails leaves the file as it was, and odd files in romfs_ext are handled as Azahar does") {
    Fixture f(SampleTree());
    Tree expected = f.base;

    WriteFile(f.ext() / "a.txt.ips", Ips({{3, B("0123456789")}}));  // a record past the end of the file
    const Bytes y = f.base.files["data/y.arc"];
    Bytes y_new = y;
    y_new[0] ^= 1;
    WriteFile(f.ext() / "data" / "y.arc.bps", Bps(y, y_new, /*source crc off by*/ 1));
    const Bytes z = f.base.files["data/z.arc"];
    Bytes z_new = z;
    z_new[1] ^= 1;
    WriteFile(f.ext() / "data" / "z.arc.bps", Bps(z, z_new, 0, /*target crc off by*/ 1));
    WriteFile(f.ext() / "c.bin.ips", B("not a patch"));
    WriteFile(f.ext() / "nofile.bin.ips", Ips({{0, B("x")}}));       // nothing to patch
    WriteFile(f.ext() / "ab", B("short name"));                       // too short to have an extension
    WriteFile(f.ext() / "empty.bin.ips", Ips({{0, B("x")}}));         // the file is empty: the record is past its end

    // Any other extension: the original is looked up without its last 4 characters, and the file is
    // applied as a BPS patch (Azahar warns and goes on).
    const Bytes x = f.base.files["data/x.arc"];
    Bytes x_new = x;
    x_new[5] ^= 0xFF;
    WriteFile(f.ext() / "data" / "x.arc.xyz", Bps(x, x_new));
    expected.files["data/x.arc"] = x_new;
    WriteFile(f.ext() / "data" / "sub" / "deep.bin.txt", B("a note, not a patch"));

    ExpectTree(f.Run(), expected);
}

TEST_CASE("names outside ASCII, a pair of surrogates among them") {
    Tree t;
    t.files["caf\xC3\xA9.txt"] = B("coffee");
    t.files["\xE6\x97\xA5\xE6\x9C\xAC/\xE8\xAA\x9E.bin"] = Pattern(40, 1);       // 日本/語.bin
    t.files["\xF0\x9D\x84\x9E/clef.bin"] = Pattern(20, 2);                       // U+1D11E, outside the BMP
    t.files["plain.bin"] = Pattern(8, 3);
    Fixture f(t);
    WriteFile(f.romfs() / "caf\xC3\xA9.txt", B("tea"));
    WriteFile(f.romfs() / "\xF0\x9D\x84\x9E" / "new\xE2\x82\xAC.bin", Pattern(9, 4));   // new€.bin
    WriteFile(f.ext() / "\xE6\x97\xA5\xE6\x9C\xAC" / "\xE8\xAA\x9E.bin.ips", Ips({{0, B("Z")}}));
    Tree expected = t;
    expected.files["caf\xC3\xA9.txt"] = B("tea");
    expected.files["\xF0\x9D\x84\x9E/new\xE2\x82\xAC.bin"] = Pattern(9, 4);
    expected.files["\xE6\x97\xA5\xE6\x9C\xAC/\xE8\xAA\x9E.bin"][0] = 'Z';
    ExpectTree(f.Run(), expected);
}

namespace {

// Everything at once: a replacement, a new file, a patch, a stub.
void LayEverything(Fixture& f) {
    WriteFile(f.romfs() / "a.txt", Pattern(77, 20));
    WriteFile(f.romfs() / "data" / "fresh.bin", Pattern(500, 21));
    WriteFile(f.ext() / "data" / "x.arc.ips", Ips({{500, Pattern(40, 22)}}));
    WriteFile(f.ext() / "data" / "y.arc.stub", {});
}

}  // namespace

TEST_CASE("reads of any offset and length give the bytes of the whole image") {
    Fixture f(SampleTree());
    LayEverything(f);
    auto image = f.Build();
    REQUIRE(image != nullptr);
    const Bytes whole = ReadAll(*image);
    const u64 size = image->GetSize();
    REQUIRE(size == whole.size());

    u64 state = 12345;
    const auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return state >> 33;
    };
    Bytes window;
    const auto check = [&](u64 offset, std::size_t length) {
        window.assign(length + 1, 0xCD);
        std::size_t got = 99;
        CHECK(image->Read(offset, length, window.data(), &got) == kResultSuccess);
        const std::size_t expect = offset >= size ? 0 : static_cast<std::size_t>(std::min<u64>(length, size - offset));
        REQUIRE(got == expect);
        if (expect != 0) {
            CHECK(std::memcmp(window.data(), whole.data() + offset, expect) == 0);
        }
        CHECK(window[expect] == 0xCD);  // nothing written past what was read
    };
    for (int i = 0; i < 3000; ++i) {
        check(next() % (size + 40), next() % 700);
    }
    for (u64 offset = 0; offset < size; offset += 1) {  // one byte at every position
        check(offset, 1);
    }
    check(size, 10);
    check(size - 1, 10);
    check(size + 5, 10);
    check(0, 0);
    check(0, static_cast<std::size_t>(size) + 100);
    CHECK(whole.size() == size);
    ExpectTree(ParseRomFs(whole), [&] {
        Tree t = f.base;
        t.files["a.txt"] = Pattern(77, 20);
        t.files["data/fresh.bin"] = Pattern(500, 21);
        Bytes x = t.files["data/x.arc"];
        const Bytes p = Pattern(40, 22);
        std::copy(p.begin(), p.end(), x.begin() + 500);
        t.files["data/x.arc"] = x;
        t.files.erase("data/y.arc");
        return t;
    }());
}

TEST_CASE("a replacement file is opened when it is first read") {
    Fixture f(SampleTree());
    WriteFile(f.romfs() / "a.txt", B("before"));
    auto image = f.Build();
    REQUIRE(image != nullptr);
    WriteFile(f.romfs() / "a.txt", B("after!"));  // same size, changed after the build
    Tree expected = f.base;
    expected.files["a.txt"] = B("after!");
    ExpectTree(ParseRomFs(ReadAll(*image)), expected);
}

TEST_CASE("a base that is a slice of a bigger file") {
    Fixture f(SampleTree());
    Bytes wrapped(0x1000, 0x5A);
    wrapped.insert(wrapped.end(), f.layout.image.begin(), f.layout.image.end());
    wrapped.insert(wrapped.end(), 5000, 0xA5);
    WriteFile(f.dir.path / "wrapped.bin", wrapped);
    std::string message;
    auto file = fs::PositionedFile::Open((f.dir.path / "wrapped.bin").string(), &message);
    REQUIRE(file != nullptr);
    auto image = layeredfs::Build(file, 0x1000, f.layout.image.size(), "", "");
    REQUIRE(image != nullptr);
    ExpectTree(ParseRomFs(ReadAll(*image)), f.base);
    // a size past the end of the file is cut to it
    auto longer = layeredfs::Build(file, 0x1000, u64{1} << 40, "", "");
    REQUIRE(longer != nullptr);
    ExpectTree(ParseRomFs(ReadAll(*longer)), f.base);
    std::string error;
    CHECK(layeredfs::Build(file, file->Size() + 1, 10, "", "", &error) == nullptr);
    CHECK(!error.empty());
}

namespace {

// A base with no file behind it: the metadata is real, the file data is `(u8)position` and any amount of it.
class VirtualBase final : public layeredfs::BaseReader {
public:
    VirtualBase(Bytes metadata, u64 total) : meta_(std::move(metadata)), total_(total) {}
    std::size_t ReadAt(u64 offset, void* buffer, std::size_t length) override {
        if (offset >= total_ || length == 0) return 0;
        const std::size_t n = static_cast<std::size_t>(std::min<u64>(length, total_ - offset));
        bytes_ += n;
        u64 end = max_end_.load();
        while (offset + n > end && !max_end_.compare_exchange_weak(end, offset + n)) {
        }
        u8* out = static_cast<u8*>(buffer);
        for (std::size_t i = 0; i < n; ++i) {
            const u64 pos = offset + i;
            out[i] = pos < meta_.size() ? meta_[static_cast<std::size_t>(pos)] : static_cast<u8>(pos);
        }
        return n;
    }
    u64 Size() const override { return total_; }
    std::atomic<u64> bytes_{0};
    std::atomic<u64> max_end_{0};

private:
    Bytes meta_;
    u64 total_;
};

}  // namespace

TEST_CASE("building over a base of terabytes reads only its metadata") {
    Tree t;
    for (int i = 0; i < 3000; ++i) {
        char name[64];
        std::snprintf(name, sizeof name, "d%02d/f%04d.bin", i % 40, i);
        t.files[name] = {};
    }
    t.files["top.bin"] = {};
    constexpr u64 kEach = u64{3} << 29;  // 1.5 GiB each
    const Layout layout = WriteRomFs(t, kEach);
    const u64 total = layout.data_start + t.files.size() * kEach;
    REQUIRE(total > (u64{4} << 40));
    auto base = std::make_shared<VirtualBase>(layout.image, total);

    SUBCASE("with replacements and a patch-free mods folder") {
        TempDir dir;
        WriteFile(dir.path / "romfs" / "d00" / "f0000.bin", Pattern(100, 1));
        WriteFile(dir.path / "romfs" / "new.bin", Pattern(10, 2));
        WriteFile(dir.path / "romfs_ext" / "d01" / "f0001.bin.stub", {});
        auto image = layeredfs::Build(base, (dir.path / "romfs").string(), (dir.path / "romfs_ext").string());
        REQUIRE(image != nullptr);
        CHECK(base->bytes_.load() <= layout.data_start);
        CHECK(base->max_end_.load() <= layout.data_start);
        CHECK(image->GetSize() > (u64{4} << 40));
    }
    SUBCASE("a window deep inside, read from the base at the position it came from") {
        auto image = layeredfs::Build(base, "", "");
        REQUIRE(image != nullptr);
        CHECK(base->bytes_.load() <= layout.data_start);
        CHECK(base->max_end_.load() <= layout.data_start);
        const u64 before = base->bytes_.load();
        Bytes header(0x28);
        std::size_t got = 0;
        image->Read(0, header.size(), header.data(), &got);
        const u64 data_start = Get32(header, 0x24);
        // the first file of the root is the first in both images, so its data starts at the data offset of each
        Bytes window(64);
        image->Read(data_start + 100, window.size(), window.data(), &got);
        REQUIRE(got == window.size());
        for (std::size_t i = 0; i < window.size(); ++i) {
            CHECK(window[i] == static_cast<u8>(layout.data_start + 100 + i));
        }
        CHECK(base->bytes_.load() - before == 64);
        // the end of the image reads without touching anything else
        image->Read(image->GetSize() - 16, 16, window.data(), &got);
        CHECK(got == 16);
        CHECK(base->bytes_.load() - before <= 64 + 16);
    }
}

TEST_CASE("many threads read one image at once") {
    Fixture f(SampleTree());
    LayEverything(f);
    auto image = f.Build();
    REQUIRE(image != nullptr);
    const Bytes whole = ReadAll(*image);
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t) {
        threads.emplace_back([&, t] {
            Bytes buffer(1 + t * 37);
            for (u64 offset = static_cast<u64>(t); offset < whole.size(); offset += buffer.size() / 2 + 1) {
                std::size_t got = 0;
                image->Read(offset, buffer.size(), buffer.data(), &got);
                if (std::memcmp(buffer.data(), whole.data() + offset, got) != 0) ++bad;
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(bad.load() == 0);
}

TEST_CASE("a damaged base image is refused with a message") {
    Fixture f(SampleTree());
    const Bytes good = f.layout.image;
    const auto refused = [&](const Bytes& image, const char* what) {
        INFO(what);
        WriteFile(f.dir.path / "bad.bin", image);
        std::string message;
        auto file = fs::PositionedFile::Open((f.dir.path / "bad.bin").string(), &message);
        REQUIRE(file != nullptr);
        std::string error;
        CHECK(layeredfs::Build(file, 0, file->Size(), "", "", &error) == nullptr);
        CHECK(!error.empty());
    };
    refused(Bytes(10, 0), "shorter than the header");
    refused(Bytes(), "empty");
    Bytes b = good;
    Put32(b, 0, 0x30);
    refused(b, "header size");
    b = good;
    Put32(b, 0x10, 0x7FFFFFF0);
    refused(b, "directory table longer than the image");
    b = good;
    Put32(b, 0x20, static_cast<u32>(good.size()));
    refused(b, "file table past the end");
    b = good;
    Bytes shorter(good.begin(), good.begin() + Get32(good, 0x1C) + 4);
    refused(shorter, "image cut inside its tables");
    // a loop in the file chain: the first file of the root is its own next sibling
    b = good;
    const u32 dm = Get32(b, 0x0C), fm = Get32(b, 0x1C);
    const u32 first_file = Get32(b, dm + 12);
    REQUIRE(first_file != 0xFFFFFFFF);
    Put32(b, fm + first_file + 4, first_file);
    refused(b, "file chain loop");
    // a child directory that is the root again
    b = good;
    Put32(b, dm + 8, 0);
    refused(b, "directory loop");
    // an odd name length
    b = good;
    Put32(b, fm + first_file + 28, 3);
    refused(b, "odd name length");
    // a name that runs out of the table
    b = good;
    Put32(b, fm + first_file + 28, 0x10000);
    refused(b, "name past the table");
}

TEST_CASE("the command line writes the layered image") {
    Fixture f(SampleTree());
    LayEverything(f);
    auto image = f.Build();
    REQUIRE(image != nullptr);
    const Bytes whole = ReadAll(*image);
    const std::string out_path = (f.dir.path / "out.bin").string();
    const std::string mods = (f.dir.path / "mods").string();

    std::ostringstream out, err;
    CHECK(layeredfs::RunTool({f.base_path.string(), "0", "0", mods, out_path}, out, err) == 0);
    std::ifstream in(out_path, std::ios::binary);
    const Bytes written((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(written == whole);

    // an offset and a size, in hexadecimal
    Bytes wrapped(0x20, 1);
    wrapped.insert(wrapped.end(), f.layout.image.begin(), f.layout.image.end());
    WriteFile(f.dir.path / "w.bin", wrapped);
    char size_text[32];
    std::snprintf(size_text, sizeof size_text, "0x%zX", f.layout.image.size());
    CHECK(layeredfs::RunTool({(f.dir.path / "w.bin").string(), "0x20", size_text, mods, out_path}, out, err) == 0);
    std::ifstream in2(out_path, std::ios::binary);
    CHECK(Bytes((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>()) == whole);

    CHECK(layeredfs::RunTool({}, out, err) == 1);
    CHECK(layeredfs::RunTool({"a", "b", "c", "d", "e"}, out, err) == 1);
    CHECK(layeredfs::RunTool({"a", "-1", "0", "d", "e"}, out, err) == 1);
    CHECK(layeredfs::RunTool({(f.dir.path / "missing").string(), "0", "0", mods, out_path}, out, err) == 2);
    CHECK(layeredfs::RunTool({f.base_path.string(), "0", "0", mods, (f.dir.path / "no" / "such" / "o.bin").string()},
                             out, err) == 2);
    CHECK(layeredfs::RunTool({f.base_path.string(), "99999999", "0", mods, out_path}, out, err) == 2);
}

// ---- the patch formats -------------------------------------------------------------------------------------

using layeredfs::patch::ApplyBps;
using layeredfs::patch::ApplyIps;
using layeredfs::patch::Status;

TEST_CASE("IPS: records, RLE records, the EOF marker and the end of the patch") {
    Bytes buffer(64, 0);
    CHECK(ApplyIps(Ips({{2, B("abc")}, {60, B("wxyz")}}), buffer) == Status::Success);
    CHECK(std::string(buffer.begin() + 2, buffer.begin() + 5) == "abc");
    CHECK(std::string(buffer.begin() + 60, buffer.end()) == "wxyz");

    buffer.assign(64, 0);
    CHECK(ApplyIps(IpsRle(10, 20, 0x77), buffer) == Status::Success);
    for (std::size_t i = 0; i < 64; ++i) CHECK(buffer[i] == (i >= 10 && i < 30 ? 0x77 : 0));

    // a patch without EOF ends where the data ends (the last 3 bytes are not read as a record)
    buffer.assign(64, 0);
    CHECK(ApplyIps(Ips({{1, B("q")}}, false), buffer) == Status::Success);
    CHECK(buffer[1] == 'q');

    // nothing after EOF is read
    Bytes after = Ips({{1, B("q")}});
    const Bytes junk = B("garbage that is not a record");
    after.insert(after.end(), junk.begin(), junk.end());
    buffer.assign(64, 0);
    CHECK(ApplyIps(after, buffer) == Status::Success);

    // errors: a record that ends past the buffer, in both forms; the buffer never grows
    buffer.assign(64, 0);
    CHECK(ApplyIps(Ips({{62, B("abc")}}), buffer) == Status::ErrorPatches);
    CHECK(ApplyIps(IpsRle(60, 5, 1), buffer) == Status::ErrorPatches);
    CHECK(buffer.size() == 64);
    // exactly to the end is fine
    CHECK(ApplyIps(Ips({{61, B("abc")}}), buffer) == Status::Success);
    // a wrong or missing header
    CHECK(ApplyIps(B("PATCX"), buffer) == Status::ErrorPatches);
    CHECK(ApplyIps(B("PAT"), buffer) == Status::ErrorPatches);
    CHECK(ApplyIps({}, buffer) == Status::ErrorPatches);
    // truncated records (Azahar's code reads past the patch there)
    Bytes truncated = Ips({{1, B("abcdef")}}, false);
    truncated.resize(truncated.size() - 3);
    CHECK(ApplyIps(truncated, buffer) == Status::ErrorPatches);
    Bytes cut_rle = IpsRle(1, 4, 9);
    cut_rle.resize(cut_rle.size() - 5);  // the RLE record loses its value byte and EOF
    CHECK(ApplyIps(cut_rle, buffer) == Status::ErrorPatches);
    // PATCH + EOF: nothing to do
    CHECK(ApplyIps(Ips({}), buffer) == Status::Success);
    // an offset of "EOF" is a marker, as in Azahar
    CHECK(ApplyIps(Ips({{0x454F46, B("a")}}), buffer) == Status::Success);
}

TEST_CASE("BPS: the commands, the sizes and the checksums") {
    const Bytes source = Pattern(300, 5);
    Bytes target = source;
    for (std::size_t i = 40; i < 90; ++i) target[i] ^= 0x33;

    Bytes buffer = source;
    CHECK(ApplyBps(Bps(source, target), buffer) == Status::Success);
    CHECK(buffer == target);

    // a bigger target: the buffer grows
    Bytes bigger = target;
    bigger.resize(500, 0x42);
    buffer = source;
    CHECK(ApplyBps(Bps(source, bigger), buffer) == Status::Success);
    CHECK(buffer == bigger);

    // a smaller target: the buffer keeps its size, zeros after the target
    Bytes smaller(target.begin(), target.begin() + 100);
    buffer = source;
    CHECK(ApplyBps(Bps(source, smaller), buffer) == Status::Success);
    CHECK(buffer.size() == source.size());
    CHECK(Bytes(buffer.begin(), buffer.begin() + 100) == smaller);
    CHECK(std::all_of(buffer.begin() + 100, buffer.end(), [](u8 v) { return v == 0; }));

    // the checksums
    buffer = source;
    CHECK(ApplyBps(Bps(source, target, 1, 0), buffer) == Status::ErrorPatchesInvalidTitle);
    buffer = source;
    CHECK(ApplyBps(Bps(source, target, 0, 1), buffer) == Status::ErrorPatches);
    // the patch's own checksum is not checked
    Bytes unchecked = Bps(source, target);
    unchecked.back() ^= 0xFF;
    buffer = source;
    CHECK(ApplyBps(unchecked, buffer) == Status::Success);
    // a source that is not the one the patch was made for
    buffer = Pattern(300, 6);
    CHECK(ApplyBps(Bps(source, target), buffer) == Status::ErrorPatchesInvalidTitle);
    // a bad magic, metadata, and patches too short to be one
    Bytes bad = Bps(source, target);
    bad[3] = '2';
    buffer = source;
    CHECK(ApplyBps(bad, buffer) == Status::ErrorPatches);
    CHECK(ApplyBps({}, buffer) == Status::ErrorPatches);
    CHECK(ApplyBps(B("BPS1"), buffer) == Status::ErrorPatches);
    CHECK(ApplyBps(Bytes(15, 'B'), buffer) == Status::ErrorPatches);
    // a source size larger than the buffer
    buffer.resize(100);
    CHECK(ApplyBps(Bps(source, target), buffer) == Status::ErrorPatches);
}

TEST_CASE("BPS: SourceCopy and TargetCopy") {
    // source "ABCDEFGH" (8 bytes); the target is "EFGH" (SourceCopy from 4) + "EFGH" (TargetCopy from 0)
    const Bytes source = B("ABCDEFGH");
    const Bytes target = B("EFGHEFGH");
    Bytes p = B("BPS1");
    PutNumber(p, 8);
    PutNumber(p, 8);
    PutNumber(p, 0);
    PutNumber(p, ((4 - 1) << 2) | 2);  // SourceCopy 4
    PutNumber(p, 4 << 1);              // +4 from the start
    PutNumber(p, ((4 - 1) << 2) | 3);  // TargetCopy 4
    PutNumber(p, 0 << 1);              // +0: from the start of the target
    Le32Append(p, Crc32(source.data(), 8));
    Le32Append(p, Crc32(target.data(), 8));
    Le32Append(p, Crc32(p.data(), p.size()));
    Bytes buffer = source;
    CHECK(ApplyBps(p, buffer) == Status::Success);
    CHECK(buffer == target);

    // TargetCopy that overlaps what it writes repeats a byte (run-length)
    Bytes q = B("BPS1");
    PutNumber(q, 8);
    PutNumber(q, 8);
    PutNumber(q, 0);
    PutNumber(q, ((1 - 1) << 2) | 1);  // TargetRead 1
    q.push_back('x');
    PutNumber(q, ((7 - 1) << 2) | 3);  // TargetCopy 7 from offset 0
    PutNumber(q, 0 << 1);
    const Bytes run = B("xxxxxxxx");
    Le32Append(q, Crc32(source.data(), 8));
    Le32Append(q, Crc32(run.data(), 8));
    Le32Append(q, Crc32(q.data(), q.size()));
    buffer = source;
    CHECK(ApplyBps(q, buffer) == Status::Success);
    CHECK(buffer == run);

    // a TargetCopy from before the start of the target, and one that runs past its end
    for (const bool negative : {true, false}) {
        Bytes r = B("BPS1");
        PutNumber(r, 8);
        PutNumber(r, 8);
        PutNumber(r, 0);
        PutNumber(r, ((4 - 1) << 2) | 3);
        PutNumber(r, negative ? (5 << 1 | 1) : (6 << 1));  // -5, or +6 (4 bytes from 6 pass the end)
        Le32Append(r, Crc32(source.data(), 8));
        Le32Append(r, 0);
        Le32Append(r, Crc32(r.data(), r.size()));
        buffer = source;
        CHECK(ApplyBps(r, buffer) == Status::ErrorPatches);
    }
}
