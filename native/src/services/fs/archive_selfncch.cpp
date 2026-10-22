// Derived from Azahar src/core/file_sys/archive_selfncch.cpp, src/core/file_sys/archive_selfncch.h,
// src/core/file_sys/ncch_container.cpp and src/core/file_sys/romfs_reader.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: ncch_container.cpp says "GPLv3 or any later
// version"; the others say "GPLv2 or any later version"): the SelfNCCH archive (archive id 3): the file path
// types and their layout (SelfNCCHFilePath: RomFS 0, Code 1, ExeFS 2, UpdateRomFS 5), what each answers,
// that the open mode is not checked, that an archive of an unregistered program has no RomFS, where
// the readable RomFS starts (NCCHContainer::ReadRomFS), the ExeFS section names (OpenExeFS) and the file
// that serves one (ExeFSSectionFile, with NCCHContainer::LoadSectionExeFS giving it the section's bytes).
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (archive_selfncch.cpp,
//   archive_selfncch.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (ncch_container.cpp)
// Copyright 2018-2026 Citra Emulator Project / Azahar Emulator Project (romfs_reader.cpp)
//
// SelfNCCH (N1-12): the application's own RomFS, served from the game folder's romfs.bin, and the update's
// from update_romfs.bin. Departures from Azahar:
//  - ExeFS sections: the game folder holds the icon only, as icon.bin (prepare_game.py, D38), so `icon` is
//    answered like Azahar when that file is there, and `logo` and `banner` always answer "section not found"
//    (Azahar answers with the cartridge's bytes); `icon` answers "section not found" too without icon.bin.
//    The icon is read from the file when the game reads it, not kept in memory from the start.
//  - The data is registered once for the game's program id (from exheader.bin); a client that did not
//    initialise its session has program id 0 and finds no RomFS, as in Azahar (`ncch_data[program_id]`).
#include <cstring>

#include "file_backends.h"
#include "fs_state.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"

namespace mh4n::fs {

namespace {

constexpr const char* kLog = "Service.FS";

// SelfNCCHFilePathType
constexpr u32 kPathRomFs = 0;
constexpr u32 kPathCode = 1;
constexpr u32 kPathExeFs = 2;
constexpr u32 kPathUpdateRomFs = 5;

// struct SelfNCCHFilePath: the type, then the ExeFS file name (12 bytes)
constexpr std::size_t kFilePathSize = 12;

// The readable part of a RomFS section: from 0x1000 to the end, or null if there is none.
std::shared_ptr<FileBackend> OpenRomFsSection(const std::shared_ptr<PositionedFile>& image) {
    if (image == nullptr || image->Size() < kRomFsHeaderSize) {
        return nullptr;
    }
    return std::make_shared<SliceFile>(image, kRomFsHeaderSize, image->Size() - kRomFsHeaderSize);
}

// A read-only ExeFS section (Azahar's ExeFSSectionFile): it can be read once, whole, from its start. It is
// served from the file on the host, which is as big as the section.
class ExeFsSectionFile final : public FileBackend {
public:
    explicit ExeFsSectionFile(std::shared_ptr<PositionedFile> data) : data_(std::move(data)) {}

    bool ReadsRequestedLength() const override { return true; }

    Result Read(u64 offset, std::size_t length, u8* buffer, std::size_t* read) override {
        *read = 0;
        if (offset != 0) {
            MH4N_LOG(Error, kLog, "offset must be zero!");
            return kResultUnsupportedOpenFlags;
        }
        if (length != data_->Size()) {
            MH4N_LOG(Error, kLog, "size must match the file size!");
            return kResultIncorrectExefsReadSize;
        }
        bool failed = false;
        *read = data_->ReadAt(0, buffer, length, &failed);
        if (failed || *read != length) {
            // The file changed or the host failed since it was opened: Azahar's copy in memory cannot.
            MH4N_LOG(Error, kLog, "the icon could not be read from the game folder");
            *read = 0;
            return kResultHostFailure;
        }
        return kResultSuccess;
    }
    Result Write(u64, std::size_t, bool, bool, const u8*, std::size_t* written) override {
        MH4N_LOG(Error, kLog, "The file is read-only!");
        *written = 0;
        return kResultUnsupportedOpenFlags;
    }
    u64 GetSize() override { return data_->Size(); }

private:
    std::shared_ptr<PositionedFile> data_;
};

class SelfNcchArchive final : public ArchiveBackend {
public:
    SelfNcchArchive(std::shared_ptr<PositionedFile> romfs, std::shared_ptr<PositionedFile> update_romfs,
                    std::shared_ptr<PositionedFile> icon)
        : romfs_(std::move(romfs)), update_romfs_(std::move(update_romfs)), icon_(std::move(icon)) {}

    std::string GetName() const override { return "SelfNCCHArchive"; }

    Result OpenFile(const Path& path, u32 mode, u32 attributes,
                    std::shared_ptr<FileBackend>* file) override {
        (void)mode, (void)attributes;  // SelfNCCHArchive doesn't check the open mode
        if (!path.IsBinary()) {
            MH4N_LOG(Error, kLog, "SelfNCCH: the path needs to be binary");
            return kResultInvalidPath;
        }
        if (path.data.size() != kFilePathSize) {
            MH4N_LOG(Error, kLog, "SelfNCCH: wrong path size {}", path.data.size());
            return kResultInvalidPath;
        }
        u32 type = 0;
        std::memcpy(&type, path.data.data(), sizeof(type));
        switch (type) {
        case kPathUpdateRomFs:
            return OpenImage(update_romfs_, "update RomFS", file);
        case kPathRomFs:
            return OpenImage(romfs_, "RomFS", file);
        case kPathCode:
            MH4N_LOG(Error, kLog, "SelfNCCH: reading the code section is not supported");
            return kResultCommandNotAllowed;
        case kPathExeFs: {
            const char* name = reinterpret_cast<const char*>(path.data.data()) + sizeof(u32);
            const std::string filename(name, strnlen(name, 8));
            if (filename == "icon") {
                if (icon_ != nullptr) {
                    *file = std::make_shared<ExeFsSectionFile>(icon_);
                    return kResultSuccess;
                }
                MH4N_LOG(Warning, kLog, "SelfNCCH: unable to read {}", filename);
                return kResultExefsSectionNotFound;
            }
            if (filename == "logo" || filename == "banner") {
                MH4N_LOG(Warning, kLog, "SelfNCCH: unable to read {}", filename);
                return kResultExefsSectionNotFound;
            }
            MH4N_LOG(Error, kLog, "SelfNCCH: unknown ExeFS section {}", filename);
            return kResultInvalidPath;
        }
        default:
            MH4N_LOG(Error, kLog, "SelfNCCH: unknown file type {}", type);
            return kResultInvalidPath;
        }
    }

    // Every other operation is refused with UnsupportedOpenFlags (Azahar's SelfNCCHArchive).
    Result FileDelete(const Path&) override { return kResultUnsupportedOpenFlags; }
    Result DirectoryDelete(const Path&) override { return kResultUnsupportedOpenFlags; }
    Result DirectoryDeleteRecursively(const Path&) override { return kResultUnsupportedOpenFlags; }
    Result FileCreate(const Path&, u64, u32) override { return kResultUnsupportedOpenFlags; }
    Result DirectoryCreate(const Path&, u32) override { return kResultUnsupportedOpenFlags; }
    Result DirectoryOpen(const Path&, std::unique_ptr<DirectoryBackend>*) override {
        return kResultUnsupportedOpenFlags;
    }

private:
    static Result OpenImage(const std::shared_ptr<PositionedFile>& image, const char* what,
                            std::shared_ptr<FileBackend>* file) {
        *file = OpenRomFsSection(image);
        if (*file == nullptr) {
            MH4N_LOG(Info, kLog, "SelfNCCH: unable to read the {}", what);
            return kResultRomfsNotFound;
        }
        return kResultSuccess;
    }

    std::shared_ptr<PositionedFile> romfs_, update_romfs_, icon_;
};

class SelfNcchFactory final : public ArchiveFactory {
public:
    explicit SelfNcchFactory(FsState& state) : state_(state) {}
    std::string GetName() const override { return "SelfNCCH"; }

    // The archive path is not looked at (Azahar's ArchiveFactory_SelfNCCH::Open).
    Result Open(const Path& path, u64 program_id, std::unique_ptr<ArchiveBackend>* archive) override {
        (void)path;
        if (program_id == state_.ClientProgramId()) {
            *archive = std::make_unique<SelfNcchArchive>(state_.game.romfs, state_.game.update_romfs,
                                                              state_.game.icon);
        } else {
            *archive = std::make_unique<SelfNcchArchive>(nullptr, nullptr, nullptr);
        }
        return kResultSuccess;
    }

private:
    FsState& state_;
};

MH4N_REGISTER(ArchiveProvider, "03.selfncch", ArchiveProvider{archive_id::kSelfNCCH, [](FsState& state) {
                                                                  return std::make_unique<SelfNcchFactory>(
                                                                      state);
                                                              }});

}  // namespace

}  // namespace mh4n::fs
