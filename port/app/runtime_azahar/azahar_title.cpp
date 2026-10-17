// AzaharRuntime: the game file's header and the titles on Azahar's virtual SD card (Service::AM),
// for the launcher's setup.
#include "common/common_types.h"
#include "core/file_sys/ncch_container.h"
#include "core/file_sys/title_metadata.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/fs/archive.h"
#include "core/loader/loader.h"
#include "runtime_azahar/azahar_runtime.h"

namespace App::Azahar {

namespace {

CiaResult ResultFor(Service::AM::InstallStatus status) {
    switch (status) {
    case Service::AM::InstallStatus::Success:
        return CiaResult::Ok;
    case Service::AM::InstallStatus::ErrorEncrypted:
        return CiaResult::Encrypted;
    case Service::AM::InstallStatus::ErrorFileNotFound:
    case Service::AM::InstallStatus::ErrorFailedToOpenFile:
        return CiaResult::Unreadable;
    case Service::AM::InstallStatus::ErrorAborted:
        return CiaResult::Aborted;
    default:
        return CiaResult::Invalid;
    }
}

} // namespace

GameHeader AzaharRuntime::ReadGameHeader(const std::string& path) const {
    GameHeader header;
    FileSys::NCCHContainer ncch(path);
    switch (ncch.LoadHeader()) {
    case Loader::ResultStatus::Success:
        header.state = GameHeader::State::Ok;
        header.program_id = ncch.ncch_header.program_id;
        break;
    case Loader::ResultStatus::ErrorEncrypted:
        // the header itself is readable: it says which game it is
        header.state = GameHeader::State::Encrypted;
        header.program_id = ncch.ncch_header.program_id;
        break;
    default:
        break;
    }
    return header;
}

InstalledTitle AzaharRuntime::FindInstalledTitle(std::uint64_t title_id) const {
    InstalledTitle title;
    const auto media = Service::FS::MediaType::SDMC;
    FileSys::TitleMetadata tmd;
    if (tmd.Load(Service::AM::GetTitleMetadataPath(media, title_id)) !=
        Loader::ResultStatus::Success) {
        return title;
    }
    title.found = true;
    title.version = tmd.GetTitleVersion();
    title.content_path = Service::AM::GetTitleContentPath(media, title_id, 0);
    return title;
}

CiaResult AzaharRuntime::CheckCia(const std::string& path, std::optional<CiaInfo>& info) const {
    info.reset();
    bool compressed = false;
    const CiaResult result =
        ResultFor(Service::AM::CheckCIAToInstall(path, compressed, true));
    if (result != CiaResult::Ok) {
        return result;
    }
    if (const auto infos = Service::AM::GetCIAInfos(path)) {
        info = CiaInfo{static_cast<std::uint64_t>(infos->first.tid),
                       static_cast<std::uint16_t>(infos->first.version)};
    }
    return result;
}

CiaResult AzaharRuntime::InstallCia(const std::string& path,
                                    const std::function<void(std::size_t, std::size_t)>& progress) {
    std::function<Service::AM::ProgressCallback> callback;
    if (progress) {
        callback = progress;
    }
    return ResultFor(Service::AM::InstallCIA(path, std::move(callback)));
}

} // namespace App::Azahar
