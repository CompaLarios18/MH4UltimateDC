// Derived from Azahar src/core/hle/service/am/am_app.cpp and src/core/hle/service/am/am.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: am.cpp says "GPLv3 or any later
// version"; am_app.cpp says "GPLv2 or any later version"): the command table of am:app with its
// session limit, the title id checks (TID_HIGH_DLC, TID_HIGH_UPDATE) and result codes of the DLC and
// patch queries, and the answers of a console with no DLC, update or ticket installed.
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (am.cpp)
// Copyright 2014 Citra Emulator Project (am_app.cpp)
//
// am:app (N1-19): the application's view of the title manager. The runtime has no installed titles,
// tickets or DLC (Azahar answers from the NAND and SD title folders; with none of them present):
//  - the title queries check the title ids as Azahar does (DLC ids for the DLC queries, update ids for
//    GetPatchTitleInfos): an id with the wrong prefix is answered InvalidTIDInList, and an id with the
//    right prefix finds nothing ("not found"); an empty list succeeds with nothing written;
//  - GetDLCContentInfoCount answers 1 for a DLC id ("the number of content infos plus one" Azahar
//    gives for a missing TMD), ListDLCContentInfos and FindDLCContentInfos succeed and write nothing;
//  - GetNumDataTitleTickets, GetItemRights, IsDataTitleInUse, IsExternalTitleDatabaseInitialized,
//    GetNumExistingContentInfos and ListExistingContentInfos are nullptr entries in Azahar: answered
//    as an unknown command (logged once; the command id, one normal parameter, the result 0).
//
// Departures from Azahar: Azahar runs the DLC queries on a worker thread (ctx.RunAsync) and the
// client wakes when it finishes, in host time; here the reply is immediate (plus the standard IPC
// delay), which no game-visible state depends on. A reply word Azahar's RequestBuilder declares and
// does not write keeps the request's word of the same position in Azahar; the same is done here for
// positions inside the request's normal parameters, and 0 beyond them (Azahar leaves those
// uninitialised). TitleInfos is sized by the buffer the ids came in, not by the guest's count (a count
// that does not fit the buffer is InvalidTIDInList, as the ids Azahar would read past it are not ids).
#include <algorithm>
#include <vector>

#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Service.AM";

constexpr u32 kTidHighUpdate = 0x0004000E;
constexpr u32 kTidHighDlc = 0x0004008C;
constexpr u32 kTidHighDemoData = 0x0004000D;  // ListDataTitleTicketInfos' other accepted id

constexpr u32 kErrInvalidTid = 31;
constexpr u32 kErrInvalidTidInList = 60;
constexpr u32 kErrNotFound = 1018;  // ErrorDescription::NotFound
constexpr u32 kModuleAm = 32;
constexpr u32 kSummaryInvalidState = 5;
constexpr u32 kSummaryInvalidArgument = 7;
constexpr u32 kLevelPermanent = 27;
constexpr u32 kLevelUsage = 28;

constexpr Result kResultInvalidTid =
    MakeResult(kErrInvalidTid, kModuleAm, kSummaryInvalidArgument, kLevelUsage);
constexpr Result kResultInvalidTidInList =
    MakeResult(kErrInvalidTidInList, kModuleAm, kSummaryInvalidArgument, kLevelUsage);
constexpr Result kResultTitleNotFound =
    MakeResult(kErrNotFound, kModuleAm, kSummaryInvalidState, kLevelPermanent);

// The word at `index` of the request when it is one of the request's normal parameters (what an
// undeclared reply word holds in Azahar), else 0.
u32 LeftOver(const Request& request, std::size_t index) {
    const std::span<const u32> words = request.Words();
    return index <= request.NormalParamsSize() && index < words.size() ? words[index] : 0;
}

class AmApp final : public Service {
public:
    explicit AmApp(ServiceContext& context) : Service(context, "am:app") {
        Register(0x1001, "GetDLCContentInfoCount", &AmApp::GetDLCContentInfoCount);
        Register(0x1002, "FindDLCContentInfos", &AmApp::FindDLCContentInfos);
        Register(0x1003, "ListDLCContentInfos", &AmApp::ListDLCContentInfos);
        Register(0x1004, "DeleteContents", &AmApp::DeleteContents);
        Register(0x1005, "GetDLCTitleInfos", &AmApp::GetDLCTitleInfos);
        Register(0x1006, "GetNumDataTitleTickets");
        Register(0x1007, "ListDataTitleTicketInfos", &AmApp::ListDataTitleTicketInfos);
        Register(0x1008, "GetItemRights");
        Register(0x1009, "IsDataTitleInUse");
        Register(0x100A, "IsExternalTitleDatabaseInitialized");
        Register(0x100B, "GetNumExistingContentInfos");
        Register(0x100C, "ListExistingContentInfos");
        Register(0x100D, "GetPatchTitleInfos", &AmApp::GetPatchTitleInfos);
    }

private:
    // 0x100100C0: media type (u8), title id (u64). Reply: result, count.
    void GetDLCContentInfoCount(Request& request) {
        request.Pop<u8>();
        const u64 title_id = request.Pop<u64>();
        Reply& reply = request.MakeReply(2, 0);
        if (static_cast<u32>(title_id >> 32) != kTidHighDlc) {
            reply.Push(kResultInvalidTid);
            reply.Push<u32>(0);
            return;
        }
        // Azahar: no TMD on the media -> "number of content infos plus one", with a warning.
        MH4N_LOG(Warning, kLog, "missing TMD, title_id=0x{:016x}", title_id);
        reply.Push(kResultSuccess);
        reply.Push<u32>(1);
    }

    // 0x10020104: media type (u8), title id (u64), count (u32), the requested indices (mapped, in),
    // the content infos (mapped, out). Reply: result (the second declared word is not written).
    void FindDLCContentInfos(Request& request) {
        request.Pop<u8>();
        const u64 title_id = request.Pop<u64>();
        request.Pop<u32>();
        request.PopMappedBuffer();
        request.PopMappedBuffer();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(static_cast<u32>(title_id >> 32) != kTidHighDlc ? kResultInvalidTidInList
                                                                    : kResultSuccess);
        reply.Push(LeftOver(request, 2));
    }

    // 0x10030140: count (u32), media type (u8), title id (u64), start index (u32), the content infos
    // (mapped, out). Reply: result, number written.
    void ListDLCContentInfos(Request& request) {
        request.Pop<u32>();
        request.Pop<u8>();
        const u64 title_id = request.Pop<u64>();
        request.Pop<u32>();
        request.PopMappedBuffer();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(static_cast<u32>(title_id >> 32) != kTidHighDlc ? kResultInvalidTidInList
                                                                    : kResultSuccess);
        reply.Push<u32>(0);
    }

    // 0x10040102: media type (u8), title id (u64), count (u32), the content ids (mapped, in).
    // Stubbed in Azahar: succeeds and gives the buffer back.
    void DeleteContents(Request& request) {
        const u8 media_type = request.Pop<u8>();
        const u64 title_id = request.Pop<u64>();
        const u32 content_count = request.Pop<u32>();
        const MappedBuffer* ids = request.PopMappedBuffer();
        MH4N_LOG(Warning, kLog, "(STUBBED) media_type={}, title_id=0x{:016x}, content_count={}",
                 media_type, title_id, content_count);
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        if (ids != nullptr) {
            reply.PushMappedBuffer(*ids);
        } else {
            reply.Push<u32>(0);
            reply.Push<u32>(0);
        }
    }

    // Shared by GetDLCTitleInfos and GetPatchTitleInfos: media type (u8), count (u32), the title ids
    // (mapped, in), the title infos (mapped, out). Reply: result and both buffers back.
    void TitleInfos(Request& request, u32 required_tid_high) {
        request.Pop<u8>();
        const u32 count = request.Pop<u32>();
        const MappedBuffer* ids = request.PopMappedBuffer();
        const MappedBuffer* infos = request.PopMappedBuffer();

        Result result = kResultSuccess;
        // A list that does not fit the buffer reads as zero ids (which fail the TID check); the
        // vector is sized by the buffer, not by the guest's count.
        const bool fits = ids != nullptr && count <= ids->Size() / sizeof(u64);
        std::vector<u64> title_ids(fits ? count : std::min<u32>(count, 1), 0);
        if (fits && count != 0) {
            ids->Read(title_ids.data(), 0, count * static_cast<u32>(sizeof(u64)));
        }
        for (const u64 title_id : title_ids) {
            if (static_cast<u32>(title_id >> 32) != required_tid_high) {
                result = kResultInvalidTidInList;
                break;
            }
        }
        // GetTitleInfoFromList: no title has a TMD and there is no cartridge to match, so the first
        // title id of a list is "not found" (an empty list succeeds).
        if (result == kResultSuccess && !title_ids.empty()) {
            MH4N_LOG(Debug, kLog, "not found title_id={:016X}", title_ids.front());
            result = kResultTitleNotFound;
        }
        Reply& reply = request.MakeReply(1, 4);
        reply.Push(result);
        PushBuffer(reply, ids);
        PushBuffer(reply, infos);
    }

    static void PushBuffer(Reply& reply, const MappedBuffer* buffer) {
        if (buffer != nullptr) {
            reply.PushMappedBuffer(*buffer);
        } else {
            reply.Push<u32>(0);
            reply.Push<u32>(0);
        }
    }

    void GetDLCTitleInfos(Request& request) { TitleInfos(request, kTidHighDlc); }
    void GetPatchTitleInfos(Request& request) { TitleInfos(request, kTidHighUpdate); }

    // 0x10070102: ticket count (u32), title id (u64), start index (u32), the
    // ticket infos (mapped, out). Azahar answers an id that is not a data title with an error and then
    // overwrites it with the success reply it builds next, so the result is always success. No
    // tickets are installed: nothing is written.
    void ListDataTitleTicketInfos(Request& request) {
        const u32 ticket_count = request.Pop<u32>();
        const u64 title_id = request.Pop<u64>();
        request.Pop<u32>();
        MH4N_LOG(Debug, kLog, "(STUBBED) called, ticket_count={}", ticket_count);
        const u32 tid_high = static_cast<u32>(title_id >> 32);
        if (tid_high != kTidHighDlc && tid_high != kTidHighDemoData) {
            MH4N_LOG(Error, kLog, "Tried to get infos for non-data title title_id={:016X}",
                     title_id);
        }
        request.PopMappedBuffer();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);
    }
};

MH4N_REGISTER(ServiceFactory, "am:app",
              ServiceFactory([](ServiceContext& context) { return std::make_unique<AmApp>(context); },
                             5));

}  // namespace

}  // namespace mh4n
