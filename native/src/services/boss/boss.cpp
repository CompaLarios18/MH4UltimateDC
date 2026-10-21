// Derived from Azahar src/core/hle/service/boss/boss.cpp, src/core/hle/service/boss/boss.h,
// src/core/hle/service/boss/boss_u.cpp, src/core/hle/service/boss/boss_p.cpp,
// src/core/hle/service/boss/online_service.cpp and src/core/hle/service/boss/online_service.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the command tables of boss:U and boss:P, the replies of every command, the per-session
// task and property state (BossTaskProperties and its table of properties, RegisterTask,
// UnregisterTask, GetTaskIdList, SendProperty, ReceiveProperty) and the result codes.
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (boss.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (boss.h)
// Copyright 2014 Citra Emulator Project (boss_u.cpp)
// Copyright 2015 Citra Emulator Project (boss_p.cpp)
// Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (online_service.cpp)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (online_service.h)
//
// boss:U and boss:P (N1-19): SpotPass. There is no SpotPass data: no download task runs, no NsData
// exists (the BOSS extdata of the application is missing, so every NsData lookup fails as in Azahar
// when that archive cannot be opened), and the BOSS databases are empty, so InitializeSession finds no
// stored task. What is kept is what Azahar keeps in memory: the module's flags and its task-finish event
// (created, never signalled: Azahar signals it nowhere), and per session the registered task ids and
// the property table (SendProperty/ReceiveProperty, RegisterTask, UnregisterTask, GetTaskIdList).
// Commands Azahar stubs answer the same fixed replies. boss:P is the service Azahar registers beside
// boss:U with the same commands and the 0x04xx privileged ones.
//
// Departures from Azahar: a property sent or received with a size larger than the property is
// clamped to the property (Azahar reads or writes past the variable it holds); a task id longer than
// the guest buffer is read as far as the buffer goes. A task id shorter than 8 bytes is zero padded in
// the task id list (Azahar copies 8 bytes out of the string object).
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mh4n/ipc.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Service.BOSS";

constexpr u32 kModuleBoss = 62;
constexpr u32 kSummaryInvalidState = 5;
constexpr u32 kSummaryInternal = 11;
constexpr u32 kLevelStatus = 25;
constexpr Result kResultUnknown = 0xFFFFFFFF;  // Azahar's ResultUnknown
constexpr Result kResultTaskNotFound = MakeResult(51, kModuleBoss, kSummaryInvalidState, kLevelStatus);
constexpr Result kResultNsDataNotFound = MakeResult(64, kModuleBoss, kSummaryInvalidState, kLevelStatus);
constexpr Result kResultUnknownPropertyId = MakeResult(77, kModuleBoss, kSummaryInternal, kLevelStatus);

constexpr std::size_t kTaskIdSize = 8;
constexpr std::size_t kUrlSize = 0x200;
constexpr std::size_t kHeadersSize = 0x360;
constexpr std::size_t kCertIdListSize = 3;
constexpr std::size_t kTaskIdListSize = 0x400;
constexpr u16 kPropTotalTasks = 0x35;
constexpr u16 kPropTaskIdList = 0x36;
constexpr u32 kBufferLimit = 0x1000000;

// The kinds of value a task property holds: a scalar of 1, 2, 4 or 8 bytes, a vector of bytes, or a
// vector of 32-bit words (whose count a send sets to the number of bytes sent, as Azahar does).
enum class Kind { Scalar, Bytes, Words };

struct PropertyInit {
    u16 id;
    Kind kind;
    std::size_t size;  // scalar: bytes; vectors: elements
};

constexpr PropertyInit kPropertyTable[] = {
    {0x0, Kind::Scalar, 1},
    {0x1, Kind::Scalar, 1},
    {0x2, Kind::Scalar, 4},
    {0x3, Kind::Scalar, 4},
    {0x4, Kind::Scalar, 4},
    {0x5, Kind::Scalar, 1},
    {0x6, Kind::Scalar, 1},
    {0x7, Kind::Bytes, kUrlSize},
    {0x8, Kind::Scalar, 4},
    {0x9, Kind::Scalar, 1},
    {0xa, Kind::Bytes, 0x100},
    {0xb, Kind::Bytes, 0x200},
    {0xc, Kind::Scalar, 4},
    {0xd, Kind::Bytes, kHeadersSize},
    {0xe, Kind::Scalar, 4},
    {0xf, Kind::Words, kCertIdListSize},
    {0x10, Kind::Scalar, 1},
    {0x11, Kind::Scalar, 1},
    {0x12, Kind::Scalar, 1},
    {0x13, Kind::Scalar, 4},
    {0x14, Kind::Scalar, 4},
    {0x15, Kind::Bytes, 0x40},
    {0x16, Kind::Scalar, 4},
    {0x18, Kind::Scalar, 1},
    {0x19, Kind::Scalar, 1},
    {0x1a, Kind::Scalar, 1},
    {0x1b, Kind::Scalar, 4},
    {0x1c, Kind::Scalar, 4},
    {0x1d, Kind::Scalar, 1},
    {0x1e, Kind::Scalar, 1},
    {0x1f, Kind::Scalar, 1},
    {0x20, Kind::Scalar, 1},
    {0x21, Kind::Scalar, 1},
    {0x22, Kind::Scalar, 1},
    {0x23, Kind::Scalar, 4},
    {0x24, Kind::Scalar, 1},
    {0x25, Kind::Scalar, 4},
    {0x26, Kind::Scalar, 4},
    {0x27, Kind::Scalar, 4},
    {0x28, Kind::Scalar, 8},
    {0x29, Kind::Scalar, 8},
    {0x2a, Kind::Scalar, 4},
    {0x2b, Kind::Scalar, 4},
    {0x2c, Kind::Scalar, 1},
    {0x2d, Kind::Scalar, 2},
    {0x2e, Kind::Scalar, 2},
    {0x2f, Kind::Bytes, 0x40},
    {0x35, Kind::Scalar, 2},
    {0x36, Kind::Bytes, kTaskIdListSize},
    {0x3b, Kind::Scalar, 4},
    {0x3e, Kind::Bytes, 0x200},
    {0x3f, Kind::Scalar, 1},
};

struct Property {
    Kind kind = Kind::Scalar;
    std::vector<u8> bytes;
    // The size Azahar compares the sent or received size with: a scalar's width, a vector's elements.
    std::size_t Expected() const { return kind == Kind::Words ? bytes.size() / 4 : bytes.size(); }
};

struct TaskProperties {
    std::map<u16, Property> properties;
    TaskProperties() {
        for (const PropertyInit& init : kPropertyTable) {
            Property property;
            property.kind = init.kind;
            property.bytes.assign(init.kind == Kind::Words ? init.size * 4 : init.size, 0);
            properties.emplace(init.id, std::move(property));
        }
    }
};

// What Azahar calls OnlineService: the state of one session.
struct OnlineSession {
    TaskProperties current_props;
    std::map<std::string, TaskProperties> task_id_list;
};

// The module the sessions of boss:U and boss:P share.
struct BossModule {
    std::shared_ptr<Event> task_finish_event;
    u8 new_arrival_flag = 0;
    u8 ns_data_new_flag = 0;
    u8 ns_data_new_flag_privileged = 0;
    u8 output_flag = 0;
    bool created = false;
};

void PushBuffer(Reply& reply, const MappedBuffer* buffer) {
    if (buffer != nullptr) {
        reply.PushMappedBuffer(*buffer);
    } else {
        reply.Push<u32>(0);
        reply.Push<u32>(0);
    }
}

std::string ReadTaskId(const MappedBuffer* buffer, u32 size) {
    std::string task_id(std::min(size, kBufferLimit), 0);
    if (buffer != nullptr && !task_id.empty()) {
        task_id.resize(std::min<std::size_t>(task_id.size(), buffer->Size()));
        if (!task_id.empty()) {
            buffer->Read(task_id.data(), 0, static_cast<u32>(task_id.size()));
        }
    }
    return task_id;
}

class Boss final : public Service {
public:
    Boss(ServiceContext& context, const char* name, bool privileged)
        : Service(context, name), module_(context.State<BossModule>("boss")) {
        if (!module_->created) {
            module_->created = true;
            module_->task_finish_event =
                context.GetKernel().NewEvent(ResetType::OneShot, "BOSS::task_finish_event");
        }
        Register(0x0001, "InitializeSession", &Boss::InitializeSession);
        Register(0x0002, privileged ? "RegisterStorage" : "SetStorageInfo", &Boss::SetStorageInfo);
        Register(0x0003, "UnregisterStorage", &Boss::UnregisterStorage);
        Register(0x0004, "GetStorageInfo", &Boss::GetStorageInfo);
        Register(0x0005, "RegisterPrivateRootCa", &Boss::RegisterPrivateRootCa);
        Register(0x0006, "RegisterPrivateClientCert", &Boss::RegisterPrivateClientCert);
        Register(0x0007, "GetNewArrivalFlag", &Boss::GetNewArrivalFlag);
        Register(0x0008, "RegisterNewArrivalEvent", &Boss::RegisterNewArrivalEvent);
        Register(0x0009, "SetOptoutFlag", &Boss::SetOptoutFlag);
        Register(0x000A, "GetOptoutFlag", &Boss::GetOptoutFlag);
        Register(0x000B, "RegisterTask", &Boss::RegisterTask);
        Register(0x000C, "UnregisterTask", &Boss::UnregisterTask);
        Register(0x000D, "ReconfigureTask", &Boss::ReconfigureTask);
        Register(0x000E, "GetTaskIdList", &Boss::GetTaskIdList);
        Register(0x000F, "GetStepIdList", &Boss::GetStepIdList);
        Register(0x0010, "GetNsDataIdList", &Boss::GetNsDataIdList);
        Register(0x0011, "GetNsDataIdList1", &Boss::GetNsDataIdList);
        Register(0x0012, "GetNsDataIdList2", &Boss::GetNsDataIdList);
        Register(0x0013, "GetNsDataIdList3", &Boss::GetNsDataIdList);
        Register(0x0014, "SendProperty", &Boss::SendProperty);
        Register(0x0015, "SendPropertyHandle", &Boss::SendPropertyHandle);
        Register(0x0016, "ReceiveProperty", &Boss::ReceiveProperty);
        Register(0x0017, "UpdateTaskInterval", &Boss::UpdateTaskInterval);
        Register(0x0018, "UpdateTaskCount", &Boss::UpdateTaskCount);
        Register(0x0019, "GetTaskInterval", &Boss::GetTaskInterval);
        Register(0x001A, "GetTaskCount", &Boss::GetTaskCount);
        Register(0x001B, "GetTaskServiceStatus", &Boss::GetTaskServiceStatus);
        Register(0x001C, "StartTask", &Boss::BufferOnly);
        Register(0x001D, "StartTaskImmediate", &Boss::BufferOnly);
        Register(0x001E, "CancelTask", &Boss::BufferOnly);
        Register(0x001F, "GetTaskFinishHandle", &Boss::GetTaskFinishHandle);
        Register(0x0020, "GetTaskState", &Boss::GetTaskState);
        Register(0x0021, "GetTaskResult", &Boss::GetTaskResult);
        Register(0x0022, "GetTaskCommErrorCode", &Boss::GetTaskCommErrorCode);
        Register(0x0023, "GetTaskStatus", &Boss::GetTaskStatus);
        Register(0x0024, "GetTaskError", &Boss::GetTaskError);
        Register(0x0025, "GetTaskInfo", &Boss::GetTaskInfo);
        Register(0x0026, "DeleteNsData", &Boss::DeleteNsData);
        Register(0x0027, "GetNsDataHeaderInfo", &Boss::GetNsDataHeaderInfo);
        Register(0x0028, "ReadNsData", &Boss::ReadNsData);
        Register(0x0029, "SetNsDataAdditionalInfo", &Boss::SetNsDataAdditionalInfo);
        Register(0x002A, "GetNsDataAdditionalInfo", &Boss::GetNsDataAdditionalInfo);
        Register(0x002B, "SetNsDataNewFlag", &Boss::SetNsDataNewFlag);
        Register(0x002C, "GetNsDataNewFlag", &Boss::GetNsDataNewFlag);
        Register(0x002D, "GetNsDataLastUpdate", &Boss::GetNsDataLastUpdate);
        Register(0x002E, "GetErrorCode", &Boss::GetErrorCode);
        Register(0x002F, "RegisterStorageEntry", &Boss::RegisterStorageEntry);
        Register(0x0030, "GetStorageEntryInfo", &Boss::GetStorageEntryInfo);
        Register(0x0031, "SetStorageOption", &Boss::SetStorageOption);
        Register(0x0032, "GetStorageOption", &Boss::GetStorageOption);
        Register(0x0033, "StartBgImmediate", &Boss::BufferOnly);
        Register(0x0034, "GetTaskProperty0", &Boss::GetTaskProperty0);
        Register(0x0035, "RegisterImmediateTask", &Boss::RegisterImmediateTask);
        Register(0x0036, "SetTaskQuery", &Boss::TwoBuffers);
        Register(0x0037, "GetTaskQuery", &Boss::TwoBuffers);
        if (privileged) {
            Register(0x0401, "InitializeSessionPrivileged", &Boss::InitializeSessionPrivileged);
            Register(0x0404, "GetAppNewFlag", &Boss::GetAppNewFlag);
            Register(0x040D, "GetNsDataIdListPrivileged", &Boss::GetNsDataIdListPrivileged);
            Register(0x040E, "GetNsDataIdListPrivileged1", &Boss::GetNsDataIdListPrivileged);
            Register(0x0413, "SendPropertyPrivileged", &Boss::SendPropertyPrivileged);
            Register(0x0415, "DeleteNsDataPrivileged", &Boss::DeleteNsDataPrivileged);
            Register(0x0416, "GetNsDataHeaderInfoPrivileged", &Boss::GetNsDataHeaderInfoPrivileged);
            Register(0x0417, "ReadNsDataPrivileged", &Boss::ReadNsDataPrivileged);
            Register(0x041A, "SetNsDataNewFlagPrivileged", &Boss::SetNsDataNewFlagPrivileged);
            Register(0x041B, "GetNsDataNewFlagPrivileged", &Boss::GetNsDataNewFlagPrivileged);
        }
    }

private:
    // GetSessionService: the session's OnlineService, or the reply of an uninitialised session
    // (rb(1,0), ResultUnknown) and null.
    OnlineSession* Session(Request& request) {
        if (online_ == nullptr) {
            MH4N_LOG(Warning, kLog, "Client attempted to use uninitialized BOSS session.");
            request.MakeReply(1, 0).Push(kResultUnknown);
            return nullptr;
        }
        return online_.get();
    }

    // 0x00010082: program id (u64), the calling PID. Reply: result.
    void InitializeSession(Request& request) {
        const u64 program_id = request.Pop<u64>();
        request.PopPid();
        if (online_ == nullptr) {
            online_ = std::make_unique<OnlineSession>();
        }
        // OnlineService::InitializeSession: the properties start over; the databases hold no task.
        online_->current_props = TaskProperties();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Debug, kLog, "called, program_id={:#018x}", program_id);
    }

    void SetStorageInfo(Request& request) {
        const u64 extdata_id = request.Pop<u64>();
        const u32 boss_size = request.Pop<u32>();
        const u8 extdata_type = request.Pop<u8>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) extdata_id={:#018x}, boss_size={:#010x}, extdata_type={:#04x}",
                 extdata_id, boss_size, extdata_type);
    }
    void UnregisterStorage(Request& request) {
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }
    void GetStorageInfo(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }
    void RegisterPrivateRootCa(Request& request) {
        request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED)");
    }
    void RegisterPrivateClientCert(Request& request) {
        request.Pop<u32>();
        request.Pop<u32>();
        const MappedBuffer* buffer1 = request.PopMappedBuffer();
        const MappedBuffer* buffer2 = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(1, 4);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer1);
        PushBuffer(reply, buffer2);
        MH4N_LOG(Warning, kLog, "(STUBBED)");
    }
    void GetNewArrivalFlag(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u8>(module_->new_arrival_flag);
        MH4N_LOG(Warning, kLog, "(STUBBED) new_arrival_flag={}", module_->new_arrival_flag);
    }
    void RegisterNewArrivalEvent(Request& request) {
        request.PopObject<Event>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED)");
    }
    void SetOptoutFlag(Request& request) {
        module_->output_flag = request.Pop<u8>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "output_flag={}", module_->output_flag);
    }
    void GetOptoutFlag(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u8>(module_->output_flag);
        MH4N_LOG(Warning, kLog, "output_flag={}", module_->output_flag);
    }

    // 0x000B0082: size, two unknown bytes, the task id (mapped). Reply: result, the buffer.
    void RegisterTask(Request& request) {
        const u32 size = request.Pop<u32>();
        const u8 unk2 = request.Pop<u8>();
        const u8 unk3 = request.Pop<u8>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        OnlineSession* session = Session(request);
        if (session == nullptr) {
            return;
        }
        const std::string task_id = ReadTaskId(buffer, size);
        if (session->task_id_list.erase(task_id) != 0) {
            MH4N_LOG(Warning, kLog, "TaskId already in list, will be replaced");
        }
        session->task_id_list.emplace(task_id, std::move(session->current_props));
        session->current_props = TaskProperties();
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer);
        MH4N_LOG(Debug, kLog, "called, size={:#010x}, unk_param2={:#04x}, unk_param3={:#04x}", size, unk2, unk3);
    }

    // 0x000C0082: size, one unknown byte, the task id (mapped). Reply: result, the buffer.
    void UnregisterTask(Request& request) {
        const u32 size = request.Pop<u32>();
        const u8 unk2 = request.Pop<u8>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        OnlineSession* session = Session(request);
        if (session == nullptr) {
            return;
        }
        Result result = kResultSuccess;
        if (size > kTaskIdSize) {
            MH4N_LOG(Warning, kLog, "TaskId cannot be longer than 8");
            result = kResultUnknown;
        } else if (session->task_id_list.erase(ReadTaskId(buffer, size)) == 0) {
            MH4N_LOG(Warning, kLog, "TaskId not in list");
            result = kResultTaskNotFound;
        }
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(result);
        PushBuffer(reply, buffer);
        MH4N_LOG(Debug, kLog, "called, size={:#010x}, unk_param2={:#04x}", size, unk2);
    }

    void ReconfigureTask(Request& request) {
        const u32 size = request.Pop<u32>();
        const u8 unk2 = request.Pop<u8>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) size={:#010x}, unk_param2={:#04x}", size, unk2);
    }

    // 0x000E0000: fills the TotalTasks and TaskIdList properties. Reply: result.
    void GetTaskIdList(Request& request) {
        OnlineSession* session = Session(request);
        if (session == nullptr) {
            return;
        }
        TaskProperties& props = session->current_props;
        const u16 total = static_cast<u16>(session->task_id_list.size());
        props.properties[kPropTotalTasks].bytes = {static_cast<u8>(total), static_cast<u8>(total >> 8)};
        constexpr std::size_t kNumTaskIds = kTaskIdListSize / kTaskIdSize;
        std::vector<u8> task_ids(kTaskIdListSize, 0);
        std::size_t returned = 0;
        for (const auto& entry : session->task_id_list) {
            if (entry.first.size() > kTaskIdSize || returned >= kNumTaskIds) {
                MH4N_LOG(Warning, kLog, "TaskId {} too long or too many TaskIds", entry.first);
                continue;
            }
            std::memcpy(task_ids.data() + returned * kTaskIdSize, entry.first.data(), entry.first.size());
            ++returned;
        }
        // The property is a vector of 0x400 bytes; the list is copied when it still has that size.
        Property& list = props.properties[kPropTaskIdList];
        if (list.kind == Kind::Bytes && list.bytes.size() == kTaskIdListSize) {
            list.bytes = task_ids;
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Debug, kLog, "called");
    }

    void GetStepIdList(Request& request) {
        const u32 size = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) size={:#010x}", size);
    }

    // 0x00100102 (and the three variants): filter, max entries, word index (u16), start id, the output
    // (mapped). No NsData exists: nothing is written. Reply: result, count, last word index, the buffer.
    void GetNsDataIdList(Request& request) {
        const u32 filter = request.Pop<u32>();
        const u32 max_entries = request.Pop<u32>();
        const u16 word_index_start = request.Pop<u16>();
        const u32 start_ns_data_id = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        if (Session(request) == nullptr) {
            return;
        }
        Reply& reply = request.MakeReply(3, 2);
        reply.Push(kResultSuccess);
        reply.Push<u16>(0);
        reply.Push<u16>(0);
        PushBuffer(reply, buffer);
        MH4N_LOG(Debug, kLog, "filter={:#010x}, max_entries={:#010x}, word_index_start={:#06x}, start_ns_data_id={:#010x}",
                 filter, max_entries, word_index_start, start_ns_data_id);
    }

    // 0x00140082: property id (u16), size, the value (mapped). Reply: result, the buffer.
    void SendProperty(Request& request) {
        const u16 property_id = request.Pop<u16>();
        const u32 size = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        OnlineSession* session = Session(request);
        if (session == nullptr) {
            return;
        }
        Result result = kResultSuccess;
        auto found = session->current_props.properties.find(property_id);
        if (found == session->current_props.properties.end()) {
            MH4N_LOG(Error, kLog, "Unknown property with ID {:#06x} and size {}", property_id, size);
            result = kResultUnknownPropertyId;
        } else {
            Property& property = found->second;
            if (size != property.Expected()) {
                MH4N_LOG(Error, kLog, "Unexpected size of property {:#06x}, was expecting {}, got {}",
                         property_id, property.Expected(), size);
            }
            const u32 clamped = std::min(size, kBufferLimit);
            std::vector<u8> value;
            if (property.kind == Kind::Scalar) {
                value.assign(property.bytes.size(), 0);
            } else if (property.kind == Kind::Bytes) {
                value.assign(clamped, 0);
            } else {
                value.assign(static_cast<std::size_t>(clamped) * 4, 0);  // `size` words
            }
            const u32 count = std::min<u32>(clamped, static_cast<u32>(value.size()));
            if (buffer != nullptr && count != 0) {
                buffer->Read(value.data(), 0, count);
            }
            property.bytes = std::move(value);
        }
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(result);
        PushBuffer(reply, buffer);
        MH4N_LOG(Debug, kLog, "property_id={:#06x}, size={:#010x}", property_id, size);
    }

    void SendPropertyHandle(Request& request) {
        const u16 property_id = request.Pop<u16>();
        request.PopGenericObject();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) property_id={:#06x}", property_id);
    }

    // 0x00160082: property id (u16), size, the output (mapped). Reply: result, size, the buffer.
    void ReceiveProperty(Request& request) {
        const u16 property_id = request.Pop<u16>();
        const u32 size = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        OnlineSession* session = Session(request);
        if (session == nullptr) {
            return;
        }
        Result result = kResultSuccess;
        const auto found = session->current_props.properties.find(property_id);
        if (found == session->current_props.properties.end()) {
            MH4N_LOG(Error, kLog, "Unknown property with ID {:#06x} and size {}", property_id, size);
            result = kResultUnknownPropertyId;
        } else {
            const Property& property = found->second;
            if (size != property.Expected()) {
                MH4N_LOG(Error, kLog, "Unexpected size of property {:#06x}, was expecting {}, got {}",
                         property_id, property.Expected(), size);
            }
            const u32 count = std::min<u32>(size, static_cast<u32>(property.bytes.size()));
            if (buffer != nullptr && count != 0) {
                buffer->Write(property.bytes.data(), 0, count);
            }
        }
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(result);
        reply.Push<u32>(size);
        PushBuffer(reply, buffer);
        MH4N_LOG(Debug, kLog, "property_id={:#06x}, size={:#010x}", property_id, size);
    }

    void UpdateTaskInterval(Request& request) {
        const u32 size = request.Pop<u32>();
        const u16 unk2 = request.Pop<u16>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) size={:#010x}, unk_param2={:#06x}", size, unk2);
    }
    void UpdateTaskCount(Request& request) {
        const u32 size = request.Pop<u32>();
        const u32 unk2 = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        const std::string task_id = ReadTaskId(buffer, size);
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) size={:#010x}, unk_param2={:#010x}, task_id={}", size, unk2, task_id);
    }
    void GetTaskInterval(Request& request) {
        const u32 size = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) size={:#010x}", size);
    }
    void GetTaskCount(Request& request) {
        const u32 size = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        const std::string task_id = ReadTaskId(buffer, size);
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) size={:#010x}, task_id={}", size, task_id);
    }
    void GetTaskServiceStatus(Request& request) {
        const u32 size = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push<u8>(1);  // Azahar: "not the task status", 1
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) size={:#010x}", size);
    }

    // StartTask, StartTaskImmediate, CancelTask and StartBgImmediate: size, the task id; reply: result,
    // the buffer.
    void BufferOnly(Request& request) {
        const u32 size = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) size={:#010x}", size);
    }

    void GetTaskFinishHandle(Request& request) {
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(module_->task_finish_event);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    void GetTaskState(Request& request) {
        const u32 size = request.Pop<u32>();
        const u8 state = request.Pop<u8>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(4, 2);
        reply.Push(kResultSuccess);
        reply.Push<u8>(0);   // TaskStatus
        reply.Push<u32>(0);  // current state value for task property 0x4
        reply.Push<u8>(0);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) size={:#010x}, state={:#06x}", size, state);
    }
    void GetTaskResult(Request& request) {
        const u32 size = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(4, 2);
        reply.Push(kResultSuccess);
        reply.Push<u8>(0);
        reply.Push<u32>(0);
        reply.Push<u8>(0);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) size={:#010x}", size);
    }
    void GetTaskCommErrorCode(Request& request) {
        const u32 size = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(4, 2);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);
        reply.Push<u32>(0);
        reply.Push<u8>(0);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) size={:#010x}", size);
    }
    void GetTaskStatus(Request& request) {
        const u32 size = request.Pop<u32>();
        const u8 unk2 = request.Pop<u8>();
        const u8 unk3 = request.Pop<u8>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push<u8>(0);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) size={:#010x}, unk_param2={:#04x}, unk_param3={:#04x}", size, unk2, unk3);
    }
    void GetTaskError(Request& request) {
        const u32 size = request.Pop<u32>();
        const u8 unk2 = request.Pop<u8>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push<u8>(0);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) size={:#010x}, unk_param2={:#04x}", size, unk2);
    }
    void GetTaskInfo(Request& request) {
        const u32 size = request.Pop<u32>();
        const u8 unk2 = request.Pop<u8>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) size={:#010x}, unk_param2={:#04x}", size, unk2);
    }
    void DeleteNsData(Request& request) {
        const u32 ns_data_id = request.Pop<u32>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) ns_data_id={:#010x}", ns_data_id);
    }

    // 0x002700C2: NsData id, header info type (u8), size, the output (mapped). No NsData exists:
    // reply: "NsData not found" (no buffer).
    void GetNsDataHeaderInfo(Request& request) {
        const u32 ns_data_id = request.Pop<u32>();
        const u8 type = request.Pop<u8>();
        const u32 size = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        if (Session(request) == nullptr) {
            return;
        }
        MH4N_LOG(Warning, kLog, "Failed to find NsData entry for ID {:#010X}", ns_data_id);
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultNsDataNotFound);
        PushBuffer(reply, buffer);
        MH4N_LOG(Debug, kLog, "called, ns_data_id={:#010x}, type={:#04x}, size={:#010x}", ns_data_id, type, size);
    }

    // 0x00280102: NsData id, offset (u64), size, the output (mapped). Reply: the failure only.
    void ReadNsData(Request& request) {
        const u32 ns_data_id = request.Pop<u32>();
        const u64 offset = request.Pop<u64>();
        const u32 size = request.Pop<u32>();
        request.PopMappedBuffer();
        if (Session(request) == nullptr) {
            return;
        }
        MH4N_LOG(Warning, kLog, "Failed to find NsData entry for ID {:#010X}", ns_data_id);
        request.MakeReply(1, 0).Push(kResultNsDataNotFound);
        MH4N_LOG(Debug, kLog, "called, ns_data_id={:#010x}, offset={:#018x}, size={:#010x}", ns_data_id, offset, size);
    }

    void SetNsDataAdditionalInfo(Request& request) {
        const u32 unk1 = request.Pop<u32>();
        const u32 unk2 = request.Pop<u32>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) unk_param1={:#010x}, unk_param2={:#010x}", unk1, unk2);
    }
    void GetNsDataAdditionalInfo(Request& request) {
        const u32 unk1 = request.Pop<u32>();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);
        MH4N_LOG(Warning, kLog, "(STUBBED) unk_param1={:#010x}", unk1);
    }
    void SetNsDataNewFlag(Request& request) {
        const u32 unk1 = request.Pop<u32>();
        module_->ns_data_new_flag = request.Pop<u8>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) unk_param1={:#010x}, ns_data_new_flag={:#04x}", unk1, module_->ns_data_new_flag);
    }
    void GetNsDataNewFlag(Request& request) {
        const u32 unk1 = request.Pop<u32>();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u8>(module_->ns_data_new_flag);
        MH4N_LOG(Warning, kLog, "(STUBBED) unk_param1={:#010x}, ns_data_new_flag={:#04x}", unk1, module_->ns_data_new_flag);
    }

    // 0x002D0040: NsData id. No NsData exists: ResultUnknown.
    void GetNsDataLastUpdate(Request& request) {
        const u32 ns_data_id = request.Pop<u32>();
        if (Session(request) == nullptr) {
            return;
        }
        MH4N_LOG(Warning, kLog, "Could not find NsData with ID {:#010X}", ns_data_id);
        request.MakeReply(1, 0).Push(kResultUnknown);
    }

    void GetErrorCode(Request& request) {
        const u8 input = request.Pop<u8>();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);
        MH4N_LOG(Warning, kLog, "(STUBBED) input={:#010x}", input);
    }
    void RegisterStorageEntry(Request& request) {
        for (int i = 0; i < 5; ++i) {
            request.Pop<u32>();  // four u32 and a u8
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) RegisterStorageEntry");
    }
    void GetStorageEntryInfo(Request& request) {
        Reply& reply = request.MakeReply(3, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);
        reply.Push<u16>(0);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }
    void SetStorageOption(Request& request) {
        request.Pop<u8>();
        request.Pop<u32>();
        request.Pop<u16>();
        request.Pop<u16>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) SetStorageOption");
    }
    void GetStorageOption(Request& request) {
        Reply& reply = request.MakeReply(5, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);
        reply.Push<u8>(0);
        reply.Push<u16>(0);
        reply.Push<u16>(0);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }
    void GetTaskProperty0(Request& request) {
        const u32 size = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push<u8>(0);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) size={:#010x}", size);
    }
    void RegisterImmediateTask(Request& request) {
        const u32 size = request.Pop<u32>();
        const u8 unk2 = request.Pop<u8>();
        const u8 unk3 = request.Pop<u8>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) size={:#010x}, unk_param2={:#04x}, unk_param3={:#04x}", size, unk2, unk3);
    }
    // SetTaskQuery and GetTaskQuery: two sizes, two buffers; reply: result, both buffers.
    void TwoBuffers(Request& request) {
        request.Pop<u32>();
        request.Pop<u32>();
        const MappedBuffer* buffer1 = request.PopMappedBuffer();
        const MappedBuffer* buffer2 = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(1, 4);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer1);
        PushBuffer(reply, buffer2);
        MH4N_LOG(Warning, kLog, "(STUBBED)");
    }

    // ---- boss:P ----
    void InitializeSessionPrivileged(Request& request) {
        const u64 program_id = request.Pop<u64>();
        request.PopPid();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) programID={:#018x}", program_id);
    }
    void GetAppNewFlag(Request& request) {
        const u64 program_id = request.Pop<u64>();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u8>(0);  // 0 = nothing new
        MH4N_LOG(Warning, kLog, "(STUBBED) programID={:#018x}", program_id);
    }
    void GetNsDataIdListPrivileged(Request& request) {
        const u64 program_id = request.Pop<u64>();
        request.Pop<u32>();
        request.Pop<u32>();
        request.Pop<u16>();
        request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(3, 2);
        reply.Push(kResultSuccess);
        reply.Push<u16>(0);
        reply.Push<u16>(0);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) programID={:#018x}", program_id);
    }
    void SendPropertyPrivileged(Request& request) {
        const u16 property_id = request.Pop<u16>();
        const u32 size = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) property_id={:#06x}, size={:#010x}", property_id, size);
    }
    void DeleteNsDataPrivileged(Request& request) {
        const u64 program_id = request.Pop<u64>();
        const u32 ns_data_id = request.Pop<u32>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) programID={:#018x}, ns_data_id={:#010x}", program_id, ns_data_id);
    }
    void GetNsDataHeaderInfoPrivileged(Request& request) {
        const u64 program_id = request.Pop<u64>();
        const u32 ns_data_id = request.Pop<u32>();
        const u8 type = request.Pop<u8>();
        const u32 size = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) programID={:#018x} ns_data_id={:#010x}, type={:#04x}, size={:#010x}",
                 program_id, ns_data_id, type, size);
    }
    void ReadNsDataPrivileged(Request& request) {
        const u64 program_id = request.Pop<u64>();
        const u32 ns_data_id = request.Pop<u32>();
        const u64 offset = request.Pop<u64>();
        const u32 size = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(3, 2);
        reply.Push(kResultSuccess);
        reply.Push<u32>(size);  // "should be the actual read size"
        reply.Push<u32>(0);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) programID={:#018x}, ns_data_id={:#010x}, offset={:#018x}, size={:#010x}",
                 program_id, ns_data_id, offset, size);
    }
    void SetNsDataNewFlagPrivileged(Request& request) {
        const u64 program_id = request.Pop<u64>();
        const u32 unk1 = request.Pop<u32>();
        module_->ns_data_new_flag_privileged = request.Pop<u8>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) programID={:#018x}, unk_param1={:#010x}, ns_data_new_flag_privileged={:#04x}",
                 program_id, unk1, module_->ns_data_new_flag_privileged);
    }
    void GetNsDataNewFlagPrivileged(Request& request) {
        const u64 program_id = request.Pop<u64>();
        const u32 unk1 = request.Pop<u32>();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u8>(module_->ns_data_new_flag_privileged);
        MH4N_LOG(Warning, kLog, "(STUBBED) programID={:#018x}, unk_param1={:#010x}, ns_data_new_flag_privileged={:#04x}",
                 program_id, unk1, module_->ns_data_new_flag_privileged);
    }

    std::shared_ptr<BossModule> module_;
    std::unique_ptr<OnlineSession> online_;  // SessionData::online_service
};

MH4N_REGISTER(ServiceFactory, "boss:U",
              ServiceFactory([](ServiceContext& context) {
                  return std::make_unique<Boss>(context, "boss:U", false);
              }));
MH4N_REGISTER(ServiceFactory, "boss:P",
              ServiceFactory([](ServiceContext& context) {
                  return std::make_unique<Boss>(context, "boss:P", true);
              }));

}  // namespace

}  // namespace mh4n
