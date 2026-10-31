#include "launcher/update_code.h"

#include <algorithm>
#include <cstring>

namespace Launcher::UpdateCode {

namespace {

constexpr std::uint64_t MEDIA_UNIT = 0x200;
constexpr std::size_t EXHEADER_SIZE = 0x800;
constexpr std::size_t EXEFS_HEADER_SIZE = 0x200;
constexpr std::size_t MAX_TMD_SIZE = 4u << 20;     // far more than a title's contents need
constexpr std::size_t MAX_CODE_SIZE = 256u << 20;  // far more than a 3DS program's code

std::uint16_t Be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] << 8 | p[1]);
}

std::uint32_t Be32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3];
}

std::uint64_t Be64(const std::uint8_t* p) {
    return std::uint64_t{Be32(p)} << 32 | Be32(p + 4);
}

std::uint32_t Le32(const std::uint8_t* p) {
    return std::uint32_t{p[3]} << 24 | std::uint32_t{p[2]} << 16 | std::uint32_t{p[1]} << 8 | p[0];
}

std::uint64_t Align64(std::uint64_t x) {
    return (x + 63) & ~std::uint64_t{63};
}

std::uint32_t Rotl(std::uint32_t x, int n) {
    return x << n | x >> (32 - n);
}

/// One 64-byte block into the SHA-1 state.
void Sha1Block(std::uint32_t h[5], const std::uint8_t* p) {
    std::uint32_t w[80];
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = Be32(p + 4 * i);
    }
    for (std::size_t i = 16; i < 80; ++i) {
        w[i] = Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (std::size_t i = 0; i < 80; ++i) {
        std::uint32_t f = 0, k = 0;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const std::uint32_t t = Rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = Rotl(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

} // namespace

std::string VersionText(std::uint16_t version) {
    return std::to_string(version >> 10) + "." + std::to_string((version >> 4) & 0x3F) + "." +
           std::to_string(version & 0xF);
}

std::string Sha1(const std::uint8_t* data, std::size_t size) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::size_t done = 0;
    for (; size - done >= 64; done += 64) {
        Sha1Block(h, data + done);
    }
    // the rest, a 1 bit, zeros and the length in bits: one block more, or two
    std::uint8_t tail[128] = {};
    const std::size_t rest = size - done;
    if (rest > 0) {
        std::memcpy(tail, data + done, rest);
    }
    tail[rest] = 0x80;
    const std::size_t blocks = rest < 56 ? 1 : 2;
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[blocks * 64 - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
    }
    for (std::size_t i = 0; i < blocks; ++i) {
        Sha1Block(h, tail + 64 * i);
    }
    static constexpr char DIGITS[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(40);
    for (const std::uint32_t word : h) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            hex += DIGITS[(word >> shift) & 0xF];
        }
    }
    return hex;
}

bool Decompress(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out) {
    // the footer, in the last 8 bytes: the compressed part's length from the end (24 bits), the
    // footer's own (8, and up to 3 bytes of padding), and what decompressing adds
    const std::size_t size = in.size();
    if (size < 8) {
        return false;
    }
    const std::uint32_t top_bottom = Le32(in.data() + size - 8);
    const std::size_t top = top_bottom & 0xFFFFFF;
    const std::size_t bottom = top_bottom >> 24;
    const std::size_t extra = Le32(in.data() + size - 4);
    if (bottom < 8 || bottom > 11 || top < bottom || top > size || size + extra > MAX_CODE_SIZE) {
        return false;
    }
    out.assign(size + extra, 0);
    std::copy(in.begin(), in.end(), out.begin());
    // backwards from the footer: a flag byte for the next 8 items, each a byte as it is (0) or a
    // copy of 3 to 18 bytes from 3 to 4098 bytes further on, written already (1)
    std::size_t src = size - bottom;
    std::size_t dst = out.size();
    const std::size_t end = size - top;
    while (src > end) {
        std::uint8_t flags = in[--src];
        for (int item = 0; item < 8; ++item) {
            if ((flags & 0x80) == 0) {
                if (src == end || dst == end) {
                    return false;
                }
                out[--dst] = in[--src];
            } else {
                if (src < end + 2) {
                    return false;
                }
                src -= 2;
                const std::size_t length = static_cast<std::size_t>(in[src + 1] >> 4) + 3;
                const std::size_t distance = static_cast<std::size_t>((in[src + 1] & 0x0F) << 8 | in[src]) + 3;
                if (dst < end + length || dst + distance > out.size()) {
                    return false;
                }
                for (std::size_t i = 0; i < length; ++i) {
                    --dst;
                    out[dst] = out[dst + distance];
                }
            }
            flags = static_cast<std::uint8_t>(flags << 1);
            if (src <= end) {
                break;
            }
        }
    }
    return true;
}

Cia ReadCia(const Read& read) {
    Cia cia;
    const auto fail = [&cia](const char* why) {
        cia.error = why;
        return cia;
    };
    // the header: its own size (0x2020), then the sizes of the certificates, the ticket and the
    // title metadata, each of them (and the contents after them) at a multiple of 64
    std::uint8_t header[0x20];
    if (!read(0, header, sizeof(header)) || Le32(header) != 0x2020) {
        return fail("it isn't a CIA");
    }
    const std::uint64_t certificates = Align64(Le32(header));
    const std::uint64_t ticket = Align64(certificates + Le32(header + 0x08));
    const std::uint64_t tmd_offset = Align64(ticket + Le32(header + 0x0C));
    const std::size_t tmd_size = Le32(header + 0x10);
    const std::uint64_t contents = Align64(tmd_offset + tmd_size);
    std::vector<std::uint8_t> tmd(std::min(tmd_size, MAX_TMD_SIZE));
    if (tmd_size < 4 || tmd_size > MAX_TMD_SIZE || !read(tmd_offset, tmd.data(), tmd.size())) {
        return fail("its title metadata can't be read");
    }
    // the title metadata's header follows its signature, whose size its type gives
    std::size_t signature = 0;
    switch (Be32(tmd.data())) {
    case 0x10000:
    case 0x10003:
        signature = 0x200 + 0x3C;
        break;
    case 0x10001:
    case 0x10004:
        signature = 0x100 + 0x3C;
        break;
    case 0x10002:
    case 0x10005:
        signature = 0x3C + 0x40;
        break;
    default:
        return fail("its title metadata is of an unknown kind");
    }
    const std::size_t th = 4 + signature;
    if (tmd.size() < th + 0xA0) {
        return fail("its title metadata is cut short");
    }
    cia.title_id = Be64(&tmd[th + 0x4C]);
    cia.version = Be16(&tmd[th + 0x9C]);
    const std::size_t count = Be16(&tmd[th + 0x9E]);
    // the content records (id, index, type, size), after 64 content info records; the contents
    // follow one another in the records' order
    const std::size_t records = th + 0xC4 + 0x24 * 64;
    std::uint64_t offset = contents;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t record = records + 0x30 * i;
        if (tmd.size() < record + 0x10) {
            return fail("its title metadata is cut short");
        }
        const std::uint64_t size = Be64(&tmd[record + 8]);
        if (Be16(&tmd[record + 4]) == 0) {
            cia.ok = true;
            cia.content_offset = offset;
            cia.content_size = size;
            cia.encrypted = (Be16(&tmd[record + 6]) & 1) != 0;
            return cia;
        }
        offset += size;
    }
    return fail("it has no content 0");
}

Code ReadCode(const Read& read, std::uint64_t base) {
    Code code;
    const auto fail = [&code](const char* why) {
        code.error = why;
        return code;
    };
    std::uint8_t header[0x200];
    if (!read(base, header, sizeof(header)) || std::memcmp(header + 0x100, "NCCH", 4) != 0) {
        return fail("its program isn't one the app can read");
    }
    const std::uint8_t* flags = header + 0x188;
    if ((flags[7] & 0x04) == 0) {
        return fail("its program is encrypted");
    }
    if (Le32(header + 0x180) == 0 || flags[6] > 16) {
        return fail("its program isn't one the app can read");
    }
    const std::uint64_t unit = MEDIA_UNIT << flags[6];
    // the exheader's system control info: the code's compression (0x0D) and its segments
    std::uint8_t exheader[EXHEADER_SIZE];
    if (!read(base + 0x200, exheader, sizeof(exheader))) {
        return fail("its program's extended header can't be read");
    }
    const std::uint64_t exefs = base + Le32(header + 0x1A0) * unit;
    const std::uint64_t exefs_size = std::uint64_t{Le32(header + 0x1A4)} * unit;
    std::uint8_t files[EXEFS_HEADER_SIZE];
    std::uint8_t last = 0;
    if (exefs_size < sizeof(files) || !read(exefs, files, sizeof(files)) ||
        !read(exefs + exefs_size - 1, &last, 1)) {
        return fail("its program's ExeFS can't be read");
    }
    // ten files: a name, the offset after this header, the size (the last ".code" counts, as
    // tools/extract_ncch.py reads them)
    std::uint64_t code_offset = 0, code_size = 0;
    for (std::size_t i = 0; i < 10; ++i) {
        const std::uint8_t* file = files + 16 * i;
        std::string name(reinterpret_cast<const char*>(file), 8);
        name.erase(name.find_last_not_of('\0') + 1);
        const std::uint64_t offset = sizeof(files) + std::uint64_t{Le32(file + 8)};
        const std::uint64_t size = Le32(file + 12);
        if (name.empty() || size == 0) {
            continue;
        }
        if (offset + size > exefs_size) {
            return fail("its program's ExeFS is damaged");
        }
        if (name == ".code") {
            code_offset = offset;
            code_size = size;
        }
    }
    if (code_size == 0) {
        return fail("its program has no code");
    }
    if (code_size > MAX_CODE_SIZE) {
        return fail("its program's code is too big");
    }
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(code_size));
    if (!read(exefs + code_offset, raw.data(), raw.size())) {
        return fail("its program's code can't be read");
    }
    std::vector<std::uint8_t> plain;
    if ((exheader[0x0D] & 1) == 0) {
        plain = std::move(raw);
    } else if (!Decompress(raw, plain)) {
        // decompressed already by the tool that made the file, if it is as long as the segments
        // (tools/codebin2elf.py takes it so)
        const std::uint64_t segments = std::uint64_t{Le32(exheader + 0x14)} * 0x1000 +
                                       std::uint64_t{Le32(exheader + 0x24)} * 0x1000 +
                                       Le32(exheader + 0x38);
        if (raw.size() < segments) {
            return fail("its program's code can't be decompressed");
        }
        plain = std::move(raw);
    }
    code.ok = true;
    code.sha1 = Sha1(plain.data(), plain.size());
    return code;
}

Code ReadCiaCode(const Read& read) {
    const Cia cia = ReadCia(read);
    if (!cia.ok || cia.encrypted) {
        Code code;
        code.error = cia.ok ? "its program is encrypted" : cia.error;
        return code;
    }
    return ReadCode(read, cia.content_offset);
}

namespace {

/// The verdict on a label other than v1.3.0's, by the code read for it.
Verdict ByCode(const Code& code, const std::string& target_sha1) {
    if (!code.ok) {
        return Verdict::Unchecked;
    }
    return code.sha1 == target_sha1 ? Verdict::Relabeled : Verdict::OtherVersion;
}

} // namespace

CiaCheck CheckCia(std::uint16_t label, const std::function<Code()>& read_code,
                  const std::string& target_sha1) {
    CiaCheck check;
    if (label == TARGET_VERSION) {
        return check;
    }
    const Code code = read_code();
    check.verdict = ByCode(code, target_sha1);
    // the version as the label says it (some tools reset it: it isn't the update's for certain)
    const std::string found = "v" + VersionText(label);
    const std::string target = "v" + VersionText(TARGET_VERSION);
    switch (check.verdict) {
    case Verdict::Target:
        break;
    case Verdict::Relabeled:
        check.note = "The .cia's version label says " + found + ", but its code is " + target +
                     "'s (some decryption tools reset the label), so it's used as " + target + ".";
        break;
    case Verdict::OtherVersion:
        check.title = "This isn't the update";
        check.problem = "The .cia's version label says " + found + ", and its code isn't " + target +
                        "'s either: it holds another update, or a changed one. The port is made for " +
                        target + ": its code library is recompiled from that update's code.";
        break;
    case Verdict::Unchecked:
        check.title = "This .cia couldn't be checked";
        check.problem = "The .cia's version label says " + found + "; the port is made for " + target +
                        ". Some decryption tools reset that label on a " + target +
                        " update, but this file's code couldn't be read to tell (" + code.error +
                        "). Build the .cia again with GodMode9 from the update installed on your 3DS.";
        break;
    }
    return check;
}

InstalledCheck CheckInstalled(std::uint16_t label, const std::function<Code()>& read_code,
                              const std::string& target_sha1) {
    InstalledCheck check;
    if (label == TARGET_VERSION) {
        return check;
    }
    const Code code = read_code();
    check.verdict = ByCode(code, target_sha1);
    check.ok = check.verdict == Verdict::Relabeled;
    const std::string found = "v" + VersionText(label);
    const std::string target = "v" + VersionText(TARGET_VERSION);
    switch (check.verdict) {
    case Verdict::Target:
        break;
    case Verdict::Relabeled:
        check.note = "Installed in Azahar's virtual SD card; its version label says " + found +
                     ", but its code is " + target + "'s (some decryption tools reset the label)";
        break;
    case Verdict::OtherVersion:
        check.note = "The installed update's version label says " + found + ", and its code isn't " +
                     target + "'s either; the port is made for " + target;
        break;
    case Verdict::Unchecked:
        check.note = "The installed update's version label says " + found + "; the port is made for " +
                     target + " (some decryption tools reset that label on a " + target +
                     " update, but the installed code couldn't be read to tell: " + code.error + ")";
        break;
    }
    return check;
}

} // namespace Launcher::UpdateCode
