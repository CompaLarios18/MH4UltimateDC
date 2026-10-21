// Derived from Azahar src/core/file_sys/archive_ncch.cpp, src/core/file_sys/archive_ncch.h,
// src/core/file_sys/ncch_container.cpp, src/core/hle/service/am/am.cpp and
// externals/open_source_archives/Readme.md at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-3.0-or-later: ncch_container.cpp and am.cpp say "GPLv3 or any later version"; archive_ncch.cpp and
// archive_ncch.h say "GPLv2 or any later version"; Readme.md names no licence): the NCCH archive
// (archive id 0x2345678A): the archive path (NCCHArchivePath) and file path (NCCHFilePath) layouts, how a
// title is found (a cartridge answers only for its own program id; an installed title by its content
// index), which file path types there are (RomFS 0, Code 1, ExeFS 2), what a failed open falls back to (the
// open-source replacements of the Mii data, the country list, the shared fonts and the bad-word list, by
// the high and low halves of the title id) and the result when there is none.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (archive_ncch.cpp)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (archive_ncch.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (ncch_container.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (am.cpp)
//
// NCCH (N1-12): other titles' content. Azahar looks a title up in the user's NAND and SD card
// (`Nintendo 3DS/.../title/...`); this module has the game folder instead, so:
//  - the update title (the base program id with the high word 0x0004000E) on the NAND or the SD card
//    is the game folder's update_romfs.bin (RomFS) and code.bin (the `.code` section), content index 0;
//  - the base title on the game card is romfs.bin;
//  - any other title is not installed, so it falls to the replacements below or is "not found".
// Departures from Azahar: the other ExeFS sections (icon, banner, logo) and the other content indices
// (manuals, which the game folder does not hold) are not found; a read past the end of an ExeFS file
// gives 0 bytes (Azahar's NCCHFile subtracts the offset from the size without a check).
#include <cstring>

#include "file_backends.h"
#include "fs_state.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "system_archives.h"

namespace mh4n::fs {

namespace {

constexpr const char* kLog = "Service.FS";

// NCCHFilePathType
constexpr u32 kFileRomFs = 0;
constexpr u32 kFileCode = 1;
constexpr u32 kFileExeFs = 2;

constexpr std::size_t kArchivePathSize = 0x10;  // u64 tid, u32 media type, u32 unknown
constexpr std::size_t kFilePathSize = 0x14;     // u32 open type, u32 content index, u32 type, char[8]

u32 Le32(const u8* p) {
    u32 v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
u64 Le64(const u8* p) {
    u64 v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// The base program id with the high word of an update title (0x0004000E).
constexpr u64 UpdateTitleId(u64 program_id) {
    return (program_id & 0xFFFFFFFFull) | 0x0004000E00000000ull;
}

class NcchArchive final : public ArchiveBackend {
public:
    NcchArchive(const GameFiles& game, u64 title_id, u32 media_type)
        : game_(game), title_id_(title_id), media_type_(media_type) {}

    std::string GetName() const override { return "NCCHArchive"; }

    Result OpenFile(const Path& path, u32 mode, u32 attributes,
                    std::shared_ptr<FileBackend>* file) override {
        (void)mode, (void)attributes;
        if (!path.IsBinary()) {
            MH4N_LOG(Error, kLog, "NCCH: the path needs to be binary");
            return kResultInvalidPath;
        }
        if (path.data.size() != kFilePathSize) {
            MH4N_LOG(Error, kLog, "NCCH: wrong path size {}", path.data.size());
            return kResultInvalidPath;
        }
        const u32 content_index = Le32(path.data.data() + 4);
        const u32 file_type = Le32(path.data.data() + 8);
        const char* exefs_name = reinterpret_cast<const char*>(path.data.data() + 12);

        enum class Source { None, Base, Update };
        Source source = Source::None;
        if (media_type_ == kMediaGameCard) {
            // The cartridge answers only for its own program id, and nothing else is looked at.
            const bool cartridge = game_.romfs != nullptr || game_.program_id != 0;
            if (!cartridge || game_.program_id == 0 || title_id_ != game_.program_id) {
                return kResultNotFound;
            }
            source = content_index == 0 ? Source::Base : Source::None;
        } else if ((media_type_ == kMediaNand || media_type_ == kMediaSdmc) && game_.program_id != 0 &&
                   title_id_ == UpdateTitleId(game_.program_id)) {
            source = content_index == 0 ? Source::Update : Source::None;
        }

        std::shared_ptr<FileBackend> found;
        if (file_type == kFileRomFs) {
            const std::shared_ptr<PositionedFile>& image =
                source == Source::Base ? game_.romfs : (source == Source::Update ? game_.update_romfs : nullptr);
            if (image != nullptr && image->Size() >= kRomFsHeaderSize) {
                found = std::make_shared<SliceFile>(image, kRomFsHeaderSize, image->Size() - kRomFsHeaderSize);
            }
        } else if (file_type == kFileCode || file_type == kFileExeFs) {
            const std::string name(exefs_name, strnlen(exefs_name, 8));
            if (source == Source::Update && name == ".code" && game_.code != nullptr) {
                found = std::make_shared<SliceFile>(game_.code, 0, game_.code->Size());
            }
        } else {
            MH4N_LOG(Error, kLog, "NCCH: unknown archive type {}", file_type);
        }
        if (found != nullptr) {
            *file = std::move(found);
            return kResultSuccess;
        }

        // The title is not there: the open-source replacement of a system archive, if it is one.
        constexpr u32 shared_data_archive = 0x0004009B;
        constexpr u32 system_data_archive = 0x000400DB;
        constexpr u32 mii_data = 0x00010202;
        constexpr u32 region_manifest = 0x00010402;
        constexpr u32 ng_word_list = 0x00010302;
        constexpr u32 shared_font = 0x00014002;
        constexpr u32 shared_font_CHN = 0x00014102;
        constexpr u32 shared_font_KOR = 0x00014202;
        constexpr u32 shared_font_TWN = 0x00014302;
        const u32 high = static_cast<u32>(title_id_ >> 32);
        const u32 low = static_cast<u32>(title_id_ & 0xFFFFFFFFu);
        const unsigned char* data = nullptr;
        u64 size = 0;
        if (high == shared_data_archive) {
            if (low == mii_data) {
                MH4N_LOG(Warning, kLog, "Mii data file missing. Loading open source replacement from memory");
                data = embedded::kMii, size = embedded::kMii_size;
            } else if (low == region_manifest) {
                MH4N_LOG(Warning, kLog, "Country list file missing. Loading open source replacement from memory");
                data = embedded::kCountryList, size = embedded::kCountryList_size;
            } else if (low == shared_font) {
                MH4N_LOG(Warning, kLog, "Shared Font file missing. Loading open source replacement from memory");
                data = embedded::kSharedFont, size = embedded::kSharedFont_size;
            } else if (low == shared_font_CHN || low == shared_font_KOR || low == shared_font_TWN) {
                MH4N_LOG(Error, kLog,
                         "CHN/KOR/TWN shared font file missing. Loading open source replacement, but text will "
                         "not display properly");
                data = embedded::kSharedFont, size = embedded::kSharedFont_size;
            }
        } else if (high == system_data_archive) {
            if (low == ng_word_list) {
                MH4N_LOG(Warning, kLog, "Bad Word List file missing. Loading open source replacement from memory");
                data = embedded::kBadWordList, size = embedded::kBadWordList_size;
            }
        }
        if (data != nullptr && size != 0) {
            *file = std::make_shared<MemoryFile>(data, size);
            return kResultSuccess;
        }
        return kResultNotFound;
    }

    // The other operations are the defaults (IVFCArchive's answers); only OpenDirectory differs: NCCHArchive
    // answers ResultUnknown (no directory is made).
    Result DirectoryOpen(const Path&, std::unique_ptr<DirectoryBackend>*) override { return kResultUnknown; }

private:
    const GameFiles& game_;
    u64 title_id_;
    u32 media_type_;
};

class NcchFactory final : public ArchiveFactory {
public:
    explicit NcchFactory(FsState& state) : state_(state) {}
    std::string GetName() const override { return "NCCH"; }

    Result Open(const Path& path, u64 program_id, std::unique_ptr<ArchiveBackend>* archive) override {
        (void)program_id;
        if (!path.IsBinary()) {
            MH4N_LOG(Error, kLog, "NCCH: the path needs to be binary");
            return kResultInvalidPath;
        }
        if (path.data.size() != kArchivePathSize) {
            MH4N_LOG(Error, kLog, "NCCH: wrong path size {}", path.data.size());
            return kResultInvalidPath;
        }
        const u64 title_id = Le64(path.data.data());
        const u32 media_type = Le32(path.data.data() + 8) & 0xFF;
        *archive = std::make_unique<NcchArchive>(state_.game, title_id, media_type);
        return kResultSuccess;
    }

private:
    FsState& state_;
};

MH4N_REGISTER(ArchiveProvider, "2345678a.ncch", ArchiveProvider{archive_id::kNCCH, [](FsState& state) {
                                                                    return std::make_unique<NcchFactory>(state);
                                                                }});

}  // namespace

}  // namespace mh4n::fs
