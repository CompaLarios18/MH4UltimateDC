// Derived from Azahar src/core/hle/service/cecd/cecd.cpp, src/core/hle/service/cecd/cecd.h and
// src/core/hle/service/cecd/cecd_u.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later:
// each of them says "GPLv2 or any later version"): the command table of cecd:u, the layouts and
// enumerations (CecDataPathType, CecOpenMode, the box, mbox list and message headers), the data path
// names and the message id encoding, the replies and result codes of every command, the validation
// and update of the box files (CheckAndUpdateFile), and the initial contents of the archive.
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (cecd.cpp)
// Copyright 2015-2024 Citra Emulator Project / Azahar Emulator Project (cecd.h)
// Copyright 2014 Citra Emulator Project (cecd_u.cpp)
//
// cecd:u (N1-19): the StreetPass service. There is no StreetPass data: the service works on its own
// archive (the CEC system save data, kept in memory, see cecd_store.h), which starts with what Azahar
// creates for a new one (the CEC folder, eventlog.dat of 0x30D54 bytes and CEC/MBoxList____ of 0x18C
// bytes). The state is shared by every session of the service, as Azahar's Module is; each session has
// its own SessionData (the path and the file Open selected, which Read and Write then use).
//  - Start and Stop succeed and do nothing; GetCecdState is "NDM idle"; the events of
//    GetCecInfoEventHandle and GetChangeStateEventHandle are created and never signalled (Azahar
//    signals them nowhere).
//  - ReadData answers the EULA version 0x7F7F (the value Azahar's cfg module forces), "EULA agreed"
//    and "no parental control".
//  - GetEventLog (0x1E) and GetEventLogStart (0x1F) are nullptr entries in Azahar: unknown commands.
//
// Departures from Azahar (all in places where Azahar crashes or reads past a buffer, or in the host
// file system it uses):
//  - Nothing is stored on disk and a directory lists its entries in name order (Azahar lists them in
//    host order). The archive starts as Azahar's does for a console that never ran CEC.
//  - Read, Write or a message operation on a session whose Open failed (no file) answers NoData with
//    0 bytes (Azahar dereferences a null file).
//  - The header structures CheckAndUpdateFile reads are zero-extended when the buffer is shorter than
//    the structure and written back truncated (Azahar reads and writes past the buffer); its fixed
//    arrays of 8 message headers and 8 message ids are never indexed past their end; a message file
//    shorter than a header contributes a zero-extended one.
//  - The program id MBoxData.050 stores is read from exheader.bin in the game folder (the loader's
//    ParseExHeader); a run without one (an ELF) stores 0.
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../../common/utf8.h"
#include "cecd_store.h"
#include "mh4n/ipc.h"
#include "mh4n/kernel.h"
#include "mh4n/loader.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"
#include "sha256.h"

namespace mh4n::cecd {

namespace {

constexpr const char* kLog = "Service.CECD";

constexpr u32 kModuleCec = 66;
constexpr u32 kSummaryNotFound = 4;
constexpr u32 kLevelStatus = 25;
constexpr Result kResultNoData = MakeResult(1007, kModuleCec, kSummaryNotFound, kLevelStatus);
constexpr Result kResultNotAuthorized = MakeResult(1002, kModuleCec, kSummaryNotFound, kLevelStatus);

enum class PathType : u32 {
    Invalid = 0,
    MboxList = 1,
    MboxInfo = 2,
    InboxInfo = 3,
    OutboxInfo = 4,
    OutboxIndex = 5,
    InboxMsg = 6,
    OutboxMsg = 7,
    RootDir = 10,
    MboxDir = 11,
    InboxDir = 12,
    OutboxDir = 13,
    MboxData = 100,
    MboxIcon = 101,
    MboxTitle = 110,
    MboxProgramId = 150,
};

enum class SystemInfoType : u32 { EulaVersion = 1, Eula = 2, ParentControl = 3 };

constexpr u32 kCecdStateNdmIdle = 1;  // CecdState::NdmStatusIdle

// CecOpenMode: bit 0 unknown, 1 read, 2 write, 3 create, 4 check, 30 unknown flag.
struct OpenModeBits {
    u32 raw = 0;
    bool create() const { return (raw >> 3) & 1; }
    bool check() const { return (raw >> 4) & 1; }
};

// ---- headers (little-endian, as in the files) ---------------------------------------------------

struct BoxInfoHeader {  // CecBoxInfoHeader
    u16 magic;
    u8 padding[2];
    u32 box_info_size;
    u32 max_box_size;
    u32 box_size;
    u32 max_message_num;
    u32 message_num;
    u32 max_batch_size;
    u32 max_message_size;
};
static_assert(sizeof(BoxInfoHeader) == 0x20);

struct TimeStamp {
    u32 year;
    u8 month, day, hour, minute, second, millisecond, microsecond, padding;
};
static_assert(sizeof(TimeStamp) == 12);

struct MboxInfoHeader {  // CecMBoxInfoHeader
    u16 magic;
    u8 padding0[2];
    u32 program_id;
    u32 private_id;
    u8 flag;
    u8 flag2;
    u8 padding1[2];
    std::array<u8, 32> hmac_key;
    u8 padding2[4];
    TimeStamp last_accessed;
    u8 padding3[4];
    TimeStamp last_received;
    u8 padding4[4];
    TimeStamp unknown_time;
};
static_assert(sizeof(MboxInfoHeader) == 0x60);

struct MboxListHeader {  // CecMBoxListHeader
    u16 magic;
    u8 padding0[2];
    u16 version;
    u8 padding1[2];
    u16 num_boxes;
    u8 padding2[2];
    std::array<std::array<u8, 16>, 24> box_names;
};
static_assert(sizeof(MboxListHeader) == 0x18C);

struct MessageHeader {  // CecMessageHeader
    u16 magic;
    u8 padding[2];
    u32 message_size;
    u32 header_size;
    u32 body_size;
    u32 title_id;
    u32 title_id2;
    u32 batch_id;
    u32 unknown_id;
    std::array<u8, 8> message_id;
    u32 version;
    std::array<u8, 8> message_id2;
    u8 flag;
    u8 send_method;
    u8 is_unopen;
    u8 is_new;
    u64 sender_id;
    u64 sender_id2;
    TimeStamp send_time, recv_time, create_time;
    u8 send_count;
    u8 forward_count;
    u16 user_data;
};
static_assert(sizeof(MessageHeader) == 0x70);

struct ObIndexHeader {  // CecOBIndexHeader
    u16 magic;
    u8 padding[2];
    u32 message_num;
};
static_assert(sizeof(ObIndexHeader) == 0x08);

// Copies the start of `bytes` into a zero-initialised T.
template <class T> T LoadHeader(const std::vector<u8>& bytes) {
    T value{};
    std::memcpy(&value, bytes.data(), std::min(bytes.size(), sizeof(T)));
    return value;
}
template <class T> void StoreHeader(std::vector<u8>& bytes, const T& value) {
    std::memcpy(bytes.data(), &value, std::min(bytes.size(), sizeof(T)));
}

// ---- the module ------------------------------------------------------------------------------------

std::string EncodeBase64(std::span<const u8> in) {
    static constexpr char kDict[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string out;
    std::size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        const u32 v = in[i] << 16 | in[i + 1] << 8 | in[i + 2];
        out += kDict[v >> 18];
        out += kDict[(v >> 12) & 63];
        out += kDict[(v >> 6) & 63];
        out += kDict[v & 63];
    }
    if (in.size() - i == 1) {  // no padding
        const u32 v = in[i] << 16;
        out += kDict[v >> 18];
        out += kDict[(v >> 12) & 63];
    } else if (in.size() - i == 2) {
        const u32 v = in[i] << 16 | in[i + 1] << 8;
        out += kDict[v >> 18];
        out += kDict[(v >> 12) & 63];
        out += kDict[(v >> 6) & 63];
    }
    return out;
}

std::string Hex8(u32 value) {
    return fmt::format("{:08x}", value);
}

std::string DataPath(PathType type, u32 program_id, std::span<const u8> msg_id = {}) {
    switch (type) {
    case PathType::MboxList:
        return "/CEC/MBoxList____";
    case PathType::MboxInfo:
        return "/CEC/" + Hex8(program_id) + "/MBoxInfo____";
    case PathType::InboxInfo:
        return "/CEC/" + Hex8(program_id) + "/InBox___/BoxInfo_____";
    case PathType::OutboxInfo:
        return "/CEC/" + Hex8(program_id) + "/OutBox__/BoxInfo_____";
    case PathType::OutboxIndex:
        return "/CEC/" + Hex8(program_id) + "/OutBox__/OBIndex_____";
    case PathType::InboxMsg:
        return "/CEC/" + Hex8(program_id) + "/InBox___/_" + EncodeBase64(msg_id);
    case PathType::OutboxMsg:
        return "/CEC/" + Hex8(program_id) + "/OutBox__/_" + EncodeBase64(msg_id);
    case PathType::RootDir:
        return "/CEC";
    case PathType::MboxDir:
        return "/CEC/" + Hex8(program_id);
    case PathType::InboxDir:
        return "/CEC/" + Hex8(program_id) + "/InBox___";
    case PathType::OutboxDir:
        return "/CEC/" + Hex8(program_id) + "/OutBox__";
    case PathType::MboxData:
    case PathType::MboxIcon:
    case PathType::MboxTitle:
    default:
        return "/CEC/" + Hex8(program_id) + "/MBoxData." +
               fmt::format("{:03}", static_cast<u32>(type) - 100);
    }
}

bool IsDirectory(PathType type) {
    return type == PathType::RootDir || type == PathType::MboxDir || type == PathType::InboxDir ||
           type == PathType::OutboxDir;
}

constexpr const char* CecCommandName(u32 command) {
    constexpr const char* kNames[] = {
        "None",       "Start",         "ResetStart",  "ReadyScan",           "ReadyScanWait",
        "StartScan",  "Rescan",        "NdmResume",   "NdmSuspend",          "NdmSuspendImmediate",
        "StopWait",   "Stop",          "StopForce",   "StopForceWait",       "ResetFilter",
        "DaemonStop", "DaemonStart",   "Exit",        "OverBoss",            "OverBossForce",
        "OverBossForceWait", "End"};
    return command < std::size(kNames) ? kNames[command] : "Unknown";
}

// What the sessions of cecd:u share: Azahar's Module (the archive and the three events).
struct CecdModule {
    CecStore store;
    std::shared_ptr<Event> cecinfo_event;
    std::shared_ptr<Event> change_state_event;
    bool created = false;
    bool program_id_read = false;
    u64 program_id = 0;
};

void Create(CecdModule& module, ServiceContext& context) {
    module.created = true;
    Kernel& kernel = context.GetKernel();
    module.cecinfo_event = kernel.NewEvent(ResetType::OneShot, "CECD::cecinfo_event");
    module.change_state_event = kernel.NewEvent(ResetType::OneShot, "CECD::change_state_event");

    // The archive did not exist: create the CEC folder, eventlog.dat and CEC/MBoxList____.
    module.store.CreateDirectory(DataPath(PathType::RootDir, 0));
    OpenMode mode;
    mode.write = true;
    mode.create = true;

    // eventlog.dat: 0x01 0x41 0x12 and zeroes to the end (0x30D54 bytes, see NewEventLogContents).
    std::shared_ptr<OpenFile> file;
    if (module.store.OpenFile("/eventlog.dat", mode, &file) == kFsOk) {
        const std::vector<u8> buffer = NewEventLogContents();
        file->Write(0, buffer.size(), buffer.data());
        file->Close();
    }
    // MBoxList____: 'hh', version 1, zeroes up to 0x18C.
    if (module.store.OpenFile(DataPath(PathType::MboxList, 0), mode, &file) == kFsOk) {
        constexpr u32 kMboxListSize = 0x18c;
        std::vector<u8> buffer(kMboxListSize, 0);
        buffer[0] = 0x68;
        buffer[1] = 0x68;
        buffer[4] = 0x01;
        file->Write(0, buffer.size(), buffer.data());
        file->Close();
    }
}

// Reads the bytes of an entry of a directory and the messages' headers (for CheckAndUpdateFile).
std::vector<u8> ReadWholeFile(CecStore& store, const std::string& path) {
    OpenMode mode;
    mode.read = true;
    std::shared_ptr<OpenFile> file;
    if (store.OpenFile(path, mode, &file) != kFsOk) {
        return {};
    }
    std::vector<u8> bytes(static_cast<std::size_t>(file->GetSize()));
    const std::size_t count = file->Read(0, bytes.size(), bytes.data());
    bytes.resize(count);
    file->Close();
    return bytes;
}

// CheckAndUpdateFile: validates and repairs the contents of a box file before it is stored.
void CheckAndUpdateFile(CecStore& store, PathType path_type, u32 ncch_program_id,
                        std::vector<u8>& file_buffer) {
    constexpr u32 kMaxNumBoxes = 24;
    constexpr u32 kNameSize = 16;
    constexpr u32 kValidNameSize = 8;
    const u32 file_size = static_cast<u32>(file_buffer.size());

    switch (path_type) {
    case PathType::MboxList: {
        MboxListHeader header = LoadHeader<MboxListHeader>(file_buffer);
        MH4N_LOG(Debug, kLog, "CecMBoxList: magic={:#06x}, version={:#06x}, num_boxes={:#06x}",
                 header.magic, header.version, header.num_boxes);
        if (header.magic != 0x6868) {
            std::memset(&header, 0, sizeof(header));
            header.magic = 0x6868;
        }
        if (header.version != 0x01) {
            header.version = 0x01;
        }
        if (header.num_boxes > 24) {
            MH4N_LOG(Debug, kLog, "CecMBoxListHeader number of boxes is too large: {}",
                     header.num_boxes);
        } else {
            std::array<u8, kNameSize> name_buffer{};
            if (ncch_program_id != 0) {
                const std::string name = Hex8(ncch_program_id);
                std::memcpy(name_buffer.data(), name.data(), name.size());
                bool already_activated = false;
                for (u32 i = 0; i < header.num_boxes; ++i) {
                    if (std::memcmp(name_buffer.data(), &header.box_names[i], kValidNameSize) == 0) {
                        already_activated = true;
                    }
                }
                if (!already_activated && header.num_boxes < kMaxNumBoxes) {
                    MH4N_LOG(Debug, kLog, "Adding title to mboxlist____: {}", name);
                    std::memcpy(&header.box_names[header.num_boxes], name_buffer.data(), kNameSize);
                    ++header.num_boxes;
                }
            } else {
                // Remove/update the activated boxes: every entry of /CEC except MBoxList____.
                std::vector<std::string> names;
                store.OpenDirectory(DataPath(PathType::RootDir, 0), &names);
                if (names.size() > kMaxNumBoxes + 1) {
                    names.resize(kMaxNumBoxes + 1);  // Azahar reads 25 entries
                }
                for (const std::string& file_name : names) {
                    if (file_name != "MBoxList____" && header.num_boxes < kMaxNumBoxes) {
                        MH4N_LOG(Debug, kLog, "Adding title to mboxlist____: {}", file_name);
                        std::memcpy(&header.box_names[header.num_boxes++], file_name.data(),
                                    std::min<std::size_t>(file_name.size(), kValidNameSize));
                    }
                }
            }
        }
        StoreHeader(file_buffer, header);
        break;
    }
    case PathType::MboxInfo: {
        MboxInfoHeader header = LoadHeader<MboxInfoHeader>(file_buffer);
        if (header.magic != 0x6363) {
            header.magic = 0x6363;
        }
        if (header.program_id != ncch_program_id) {
            MH4N_LOG(Debug, kLog, "CecMBoxInfoHeader program id doesn't match current id: {}",
                     header.program_id);
        }
        StoreHeader(file_buffer, header);
        break;
    }
    case PathType::InboxInfo: {
        BoxInfoHeader header = LoadHeader<BoxInfoHeader>(file_buffer);
        if (header.magic != 0x6262) {
            header.magic = 0x6262;
        }
        if (header.box_info_size != file_size) {
            header.box_info_size = sizeof(BoxInfoHeader);
        }
        if (header.max_batch_size == 0) {
            header.max_batch_size = header.max_message_num;
        }
        StoreHeader(file_buffer, header);
        break;
    }
    case PathType::OutboxInfo: {
        BoxInfoHeader header = LoadHeader<BoxInfoHeader>(file_buffer);
        if (header.magic != 0x6262) {
            header.magic = 0x6262;
        }
        if (header.box_info_size != file_buffer.size()) {
            header.box_info_size = sizeof(BoxInfoHeader);
            header.message_num = 0;
        }
        if (header.max_batch_size == 0) {
            header.max_batch_size = header.max_message_num;
        }

        // The messages in /CEC/<id>/OutBox: everything but BoxInfo_____ and OBIndex_____.
        std::vector<std::string> names;
        store.OpenDirectory(DataPath(PathType::OutboxDir, ncch_program_id), &names);
        if (names.size() > static_cast<std::size_t>(header.max_message_num) + 2) {
            names.resize(static_cast<std::size_t>(header.max_message_num) + 2);
        }
        std::array<MessageHeader, 8> message_headers{};
        for (const std::string& file_name : names) {
            if (file_name == "BoxInfo_____" || file_name == "OBIndex_____") {
                continue;
            }
            MH4N_LOG(Debug, kLog, "Adding message to BoxInfo_____: {}", file_name);
            const std::vector<u8> buffer = ReadWholeFile(
                store, DataPath(PathType::OutboxDir, ncch_program_id) + "/" + file_name);
            if (header.message_num < message_headers.size()) {
                message_headers[header.message_num] = LoadHeader<MessageHeader>(buffer);
            }
            ++header.message_num;
        }
        if (header.message_num > 0) {
            const u32 count = std::min<u32>(header.message_num, message_headers.size());
            const u32 message_headers_size = header.message_num * sizeof(MessageHeader);
            file_buffer.resize(sizeof(BoxInfoHeader) + message_headers_size, 0);
            header.box_info_size += message_headers_size;
            std::memcpy(file_buffer.data() + sizeof(BoxInfoHeader), message_headers.data(),
                        count * sizeof(MessageHeader));
        }
        StoreHeader(file_buffer, header);
        break;
    }
    case PathType::OutboxIndex: {
        ObIndexHeader header = LoadHeader<ObIndexHeader>(file_buffer);
        if (header.magic != 0x6767) {
            header.magic = 0x6767;
        }
        if (header.message_num == 0) {
            if (file_size > sizeof(ObIndexHeader)) {
                header.message_num = (file_size % 8) - 1;  // 8 byte message id - 1 header
            }
        } else if (header.message_num != (file_size % 8) - 1) {
            header.message_num = 0;
        }

        std::vector<std::string> names;
        store.OpenDirectory(DataPath(PathType::OutboxDir, ncch_program_id), &names);
        if (names.size() > 8) {
            names.resize(8);  // Azahar reads 8 entries
        }
        std::array<std::array<u8, 8>, 8> message_ids{};
        for (const std::string& file_name : names) {
            if (file_name == "BoxInfo_____" || file_name == "OBIndex_____") {
                continue;
            }
            const std::vector<u8> buffer = ReadWholeFile(
                store, DataPath(PathType::OutboxDir, ncch_program_id) + "/" + file_name);
            // The message id is at offset 0x20, 8 bytes.
            if (header.message_num < message_ids.size()) {
                std::array<u8, 8> id{};
                if (buffer.size() > 0x20) {
                    std::memcpy(id.data(), buffer.data() + 0x20,
                                std::min<std::size_t>(8, buffer.size() - 0x20));
                }
                message_ids[header.message_num] = id;
            }
            ++header.message_num;
        }
        if (header.message_num > 0) {
            const u32 count = std::min<u32>(header.message_num, message_ids.size());
            file_buffer.resize(sizeof(ObIndexHeader) + static_cast<std::size_t>(header.message_num) * 8);
            std::memcpy(file_buffer.data() + sizeof(ObIndexHeader), message_ids.data(), count * 8);
        }
        StoreHeader(file_buffer, header);
        break;
    }
    default:
        break;
    }
}

void LogMessageHeader(const std::vector<u8>& buffer) {
    const MessageHeader header = LoadHeader<MessageHeader>(buffer);
    MH4N_LOG(Debug, kLog,
             "magic={:#06x}, message_size={:#010x}, header_size={:#010x}, body_size={:#010x}, "
             "title_id={:#010x}, title_id_2={:#010x}, batch_id={:#010x}",
             header.magic, header.message_size, header.header_size, header.body_size,
             header.title_id, header.title_id2, header.batch_id);
}

// ---- the service -----------------------------------------------------------------------------------

class CecdU final : public Service {
public:
    explicit CecdU(ServiceContext& context)
        : Service(context, "cecd:u"), module_(context.State<CecdModule>("cecd")) {
        if (!module_->created) {
            Create(*module_, context);
        }
        Register(0x0001, "Open", &CecdU::Open);
        Register(0x0002, "Read", &CecdU::Read);
        Register(0x0003, "ReadMessage", &CecdU::ReadMessage);
        Register(0x0004, "ReadMessageWithHMAC", &CecdU::ReadMessageWithHmac);
        Register(0x0005, "Write", &CecdU::Write);
        Register(0x0006, "WriteMessage", &CecdU::WriteMessage);
        Register(0x0007, "WriteMessageWithHMAC", &CecdU::WriteMessageWithHmac);
        Register(0x0008, "Delete", &CecdU::Delete);
        Register(0x0009, "SetData", &CecdU::SetData);
        Register(0x000A, "ReadData", &CecdU::ReadData);
        Register(0x000B, "Start", &CecdU::Start);
        Register(0x000C, "Stop", &CecdU::Stop);
        Register(0x000D, "GetCecInfoBuffer", &CecdU::GetCecInfoBuffer);
        Register(0x000E, "GetCecdState", &CecdU::GetCecdState);
        Register(0x000F, "GetCecInfoEventHandle", &CecdU::GetCecInfoEventHandle);
        Register(0x0010, "GetChangeStateEventHandle", &CecdU::GetChangeStateEventHandle);
        Register(0x0011, "OpenAndWrite", &CecdU::OpenAndWrite);
        Register(0x0012, "OpenAndRead", &CecdU::OpenAndRead);
        Register(0x001E, "GetEventLog");
        Register(0x001F, "GetEventLogStart");
    }

private:
    CecStore& Store() { return module_->store; }

    // The bytes of a mapped buffer (up to `size`, zero-filled if the read fails).
    static std::vector<u8> ReadBuffer(const MappedBuffer* buffer, u32 size) {
        constexpr u32 kLimit = 0x1000000;
        std::vector<u8> bytes(std::min(size, kLimit), 0);
        if (buffer != nullptr && !bytes.empty()) {
            buffer->Read(bytes.data(), 0, static_cast<u32>(bytes.size()));
        }
        return bytes;
    }
    static void WriteBuffer(const MappedBuffer* buffer, const std::vector<u8>& bytes, u32 size) {
        if (buffer != nullptr && size != 0) {
            std::vector<u8> padded = bytes;
            padded.resize(std::min<u32>(size, 0x1000000), 0);
            buffer->Write(padded.data(), 0, static_cast<u32>(padded.size()));
        }
    }
    static void PushBuffer(Reply& reply, const MappedBuffer* buffer) {
        if (buffer != nullptr) {
            reply.PushMappedBuffer(*buffer);
        } else {
            reply.Push<u32>(0);
            reply.Push<u32>(0);
        }
    }

    // 0x000100C2: program id, path type, open mode, the calling PID. Reply: result, size (a file) or
    // entry count (a directory).
    void Open(Request& request) {
        const u32 ncch_program_id = request.Pop<u32>();
        const PathType path_type = request.Pop<PathType>();
        OpenModeBits open_mode;
        open_mode.raw = request.Pop<u32>();
        request.PopPid();

        const std::string path = DataPath(path_type, ncch_program_id);
        ncch_program_id_ = ncch_program_id;
        open_mode_ = open_mode;
        data_path_type_ = path_type;
        path_ = path;

        Reply& reply = request.MakeReply(2, 0);
        if (IsDirectory(path_type)) {
            std::vector<std::string> names;
            if (Store().OpenDirectory(path, &names) != kFsOk) {
                if (open_mode.create()) {
                    Store().CreateDirectory(path);
                    reply.Push(kResultSuccess);
                } else {
                    MH4N_LOG(Debug, kLog, "Failed to open directory: {}", path);
                    reply.Push(kResultNoData);
                }
                reply.Push<u32>(0);
            } else {
                constexpr std::size_t kMaxEntries = 32;  // just over the maximum number of boxes (24)
                reply.Push(kResultSuccess);
                reply.Push<u32>(static_cast<u32>(std::min(names.size(), kMaxEntries)));
            }
        } else {
            OpenMode mode;
            mode.read = mode.write = mode.create = true;
            file_.reset();
            const Result result = Store().OpenFile(path, mode, &file_);
            if (result != kFsOk) {
                MH4N_LOG(Debug, kLog, "Failed to open file: {}", path);
                reply.Push(kResultNoData);
                reply.Push<u32>(0);
            } else {
                reply.Push(kResultSuccess);
                reply.Push(static_cast<u32>(file_->GetSize()));
            }
            if (path_type == PathType::MboxProgramId && file_ != nullptr) {
                // The program id of the running process, little-endian, at offset 0 of the file.
                const u64 program_id = ProgramId();
                std::array<u8, 8> id{};
                std::memcpy(id.data(), &program_id, sizeof(id));
                file_->Write(0, id.size(), id.data());
                file_->Close();
            }
        }
        MH4N_LOG(Debug, kLog, "called, ncch_program_id={:#010x}, path_type={:#04x}, path={}, open_mode: raw={:#x}",
                 ncch_program_id, static_cast<u32>(path_type), path, open_mode.raw);
    }

    // The program id of the application's process (Azahar: the code set's, which it takes from the
    // exheader): read from exheader.bin in the game folder; 0 when the run has none (an ELF).
    u64 ProgramId() {
        if (!module_->program_id_read) {
            module_->program_id_read = true;
            std::ifstream in(utf8::PathFromUtf8(Context().GameFolder() + "/exheader.bin"), std::ios::binary);
            std::vector<u8> bytes(kExHeaderMinSize);
            in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            ExHeader header;
            std::string error;
            if (in.gcount() == static_cast<std::streamsize>(bytes.size()) &&
                ParseExHeader(bytes.data(), bytes.size(), &header, &error)) {
                module_->program_id = header.program_id;
            }
        }
        return module_->program_id;
    }

    // 0x00020042: size, the output buffer (mapped, write). Reply: result, bytes read, the buffer.
    void Read(Request& request) {
        const u32 write_buffer_size = request.Pop<u32>();
        const MappedBuffer* write_buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(2, 2);
        if (IsDirectory(data_path_type_)) {
            reply.Push(kResultNotAuthorized);
            reply.Push<u32>(0);
        } else if (file_ == nullptr) {
            MH4N_LOG(Error, kLog, "Read without an open file, path={}", path_);
            reply.Push(kResultNoData);
            reply.Push<u32>(0);
        } else {
            std::vector<u8> buffer(std::min<u32>(write_buffer_size, 0x1000000), 0);
            const u32 bytes_read =
                static_cast<u32>(file_->Read(0, buffer.size(), buffer.data()));
            WriteBuffer(write_buffer, buffer, write_buffer_size);
            file_->Close();
            reply.Push(kResultSuccess);
            reply.Push(bytes_read);
        }
        PushBuffer(reply, write_buffer);
        MH4N_LOG(Debug, kLog, "called, write_buffer_size={:#x}, path={}", write_buffer_size, path_);
    }

    // The message operations share their first parameters: program id, outbox flag, id size, buffer
    // size.
    struct MessageParams {
        u32 ncch_program_id;
        bool is_outbox;
        u32 message_id_size;
        u32 buffer_size;
    };
    static MessageParams PopMessageParams(Request& request) {
        MessageParams p;
        p.ncch_program_id = request.Pop<u32>();
        p.is_outbox = request.Pop<bool>();
        p.message_id_size = request.Pop<u32>();
        p.buffer_size = request.Pop<u32>();
        return p;
    }
    static std::string MessagePath(const MessageParams& p, const std::vector<u8>& id) {
        return DataPath(p.is_outbox ? PathType::OutboxMsg : PathType::InboxMsg, p.ncch_program_id,
                        id);
    }

    // 0x00030104: params; the message id (mapped, read), the output (mapped, write). Reply: result,
    // bytes read, both buffers.
    void ReadMessage(Request& request) {
        const MessageParams p = PopMessageParams(request);
        const MappedBuffer* message_id_buffer = request.PopMappedBuffer();
        const MappedBuffer* write_buffer = request.PopMappedBuffer();
        const std::vector<u8> id = ReadBuffer(message_id_buffer, p.message_id_size);

        OpenMode mode;
        mode.read = true;
        std::shared_ptr<OpenFile> message;
        Reply& reply = request.MakeReply(2, 4);
        if (Store().OpenFile(MessagePath(p, id), mode, &message) == kFsOk) {
            std::vector<u8> buffer(std::min<u32>(p.buffer_size, 0x1000000), 0);
            const u32 bytes_read = static_cast<u32>(message->Read(0, buffer.size(), buffer.data()));
            WriteBuffer(write_buffer, buffer, p.buffer_size);
            message->Close();
            LogMessageHeader(buffer);
            reply.Push(kResultSuccess);
            reply.Push(bytes_read);
        } else {
            reply.Push(kResultNoData);
            reply.Push<u32>(0);
        }
        PushBuffer(reply, message_id_buffer);
        PushBuffer(reply, write_buffer);
        MH4N_LOG(Debug, kLog, "called, ncch_program_id={:#010x}, is_outbox={}, message_id_size={:#x}, buffer_size={:#x}",
                 p.ncch_program_id, p.is_outbox, p.message_id_size, p.buffer_size);
    }

    // 0x00040106: params; the message id, the HMAC key (0x20 bytes), the output. Reply: result, bytes
    // read, the three buffers.
    void ReadMessageWithHmac(Request& request) {
        const MessageParams p = PopMessageParams(request);
        const MappedBuffer* message_id_buffer = request.PopMappedBuffer();
        const MappedBuffer* hmac_key_buffer = request.PopMappedBuffer();
        const MappedBuffer* write_buffer = request.PopMappedBuffer();
        const std::vector<u8> id = ReadBuffer(message_id_buffer, p.message_id_size);

        OpenMode mode;
        mode.read = true;
        std::shared_ptr<OpenFile> message;
        Reply& reply = request.MakeReply(2, 6);
        if (Store().OpenFile(MessagePath(p, id), mode, &message) == kFsOk) {
            std::vector<u8> buffer(std::min<u32>(p.buffer_size, 0x1000000), 0);
            const u32 bytes_read = static_cast<u32>(message->Read(0, buffer.size(), buffer.data()));
            WriteBuffer(write_buffer, buffer, p.buffer_size);
            message->Close();
            LogMessageHeader(buffer);

            // Verifies the digest stored after the body (only logged, as in Azahar).
            const MessageHeader header = LoadHeader<MessageHeader>(buffer);
            const u64 digest_at = static_cast<u64>(header.header_size) + header.body_size;
            bool verified = false;
            if (digest_at + 0x20 <= buffer.size()) {
                const std::vector<u8> key = ReadBuffer(hmac_key_buffer, 0x20);
                const auto expected = HmacSha256(
                    key, std::span<const u8>(buffer.data() + header.header_size, header.body_size));
                verified = std::memcmp(expected.data(), buffer.data() + digest_at, 0x20) == 0;
            }
            MH4N_LOG(Debug, kLog, "Verification {}", verified ? "succeeded" : "failed");
            reply.Push(kResultSuccess);
            reply.Push(bytes_read);
        } else {
            reply.Push(kResultNoData);
            reply.Push<u32>(0);
        }
        PushBuffer(reply, message_id_buffer);
        PushBuffer(reply, hmac_key_buffer);
        PushBuffer(reply, write_buffer);
    }

    // 0x00050042: size, the data (mapped, read). Reply: result, the buffer.
    void Write(Request& request) {
        const u32 read_buffer_size = request.Pop<u32>();
        const MappedBuffer* read_buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(1, 2);
        if (IsDirectory(data_path_type_)) {
            reply.Push(kResultNotAuthorized);
        } else if (file_ == nullptr) {
            MH4N_LOG(Error, kLog, "Write without an open file, path={}", path_);
            reply.Push(kResultNoData);
        } else {
            std::vector<u8> buffer = ReadBuffer(read_buffer, read_buffer_size);
            if (file_->GetSize() != read_buffer_size) {
                file_->SetSize(read_buffer_size);
            }
            if (open_mode_.check()) {
                CheckAndUpdateFile(Store(), data_path_type_, ncch_program_id_, buffer);
            }
            file_->Write(0, buffer.size(), buffer.data());
            file_->Close();
            reply.Push(kResultSuccess);
        }
        PushBuffer(reply, read_buffer);
        MH4N_LOG(Debug, kLog, "called, read_buffer_size={:#x}", read_buffer_size);
    }

    // 0x00060104: params; the message (mapped, read), the message id. Reply: result, both buffers.
    void WriteMessage(Request& request) {
        const MessageParams p = PopMessageParams(request);
        const MappedBuffer* read_buffer = request.PopMappedBuffer();
        const MappedBuffer* message_id_buffer = request.PopMappedBuffer();
        const std::vector<u8> id = ReadBuffer(message_id_buffer, p.message_id_size);

        OpenMode mode;
        mode.write = mode.create = true;
        std::shared_ptr<OpenFile> message;
        Reply& reply = request.MakeReply(1, 4);
        if (Store().OpenFile(MessagePath(p, id), mode, &message) == kFsOk) {
            const std::vector<u8> buffer = ReadBuffer(read_buffer, p.buffer_size);
            LogMessageHeader(buffer);
            message->Write(0, buffer.size(), buffer.data());
            message->Close();
            reply.Push(kResultSuccess);
        } else {
            reply.Push(kResultNoData);
        }
        PushBuffer(reply, read_buffer);
        PushBuffer(reply, message_id_buffer);
    }

    // 0x00070106: params; the message, the HMAC key, the message id. Writes the message with the HMAC of
    // its body stored after it. Reply: result, the three buffers.
    void WriteMessageWithHmac(Request& request) {
        const MessageParams p = PopMessageParams(request);
        const MappedBuffer* read_buffer = request.PopMappedBuffer();
        const MappedBuffer* hmac_key_buffer = request.PopMappedBuffer();
        const MappedBuffer* message_id_buffer = request.PopMappedBuffer();
        const std::vector<u8> id = ReadBuffer(message_id_buffer, p.message_id_size);

        OpenMode mode;
        mode.write = mode.create = true;
        std::shared_ptr<OpenFile> message;
        Reply& reply = request.MakeReply(1, 6);
        if (Store().OpenFile(MessagePath(p, id), mode, &message) == kFsOk) {
            std::vector<u8> buffer = ReadBuffer(read_buffer, p.buffer_size);
            LogMessageHeader(buffer);
            const MessageHeader header = LoadHeader<MessageHeader>(buffer);
            const u64 hmac_offset = static_cast<u64>(header.header_size) + header.body_size;
            if (hmac_offset + 0x20 <= buffer.size()) {
                const std::vector<u8> key = ReadBuffer(hmac_key_buffer, 0x20);
                const auto digest = HmacSha256(
                    key, std::span<const u8>(buffer.data() + header.header_size, header.body_size));
                std::memcpy(buffer.data() + hmac_offset, digest.data(), digest.size());
            } else {
                MH4N_LOG(Error, kLog, "message of {} bytes has no room for its HMAC", buffer.size());
            }
            message->Write(0, buffer.size(), buffer.data());
            message->Close();
            reply.Push(kResultSuccess);
        } else {
            reply.Push(kResultNoData);
        }
        PushBuffer(reply, read_buffer);
        PushBuffer(reply, hmac_key_buffer);
        PushBuffer(reply, message_id_buffer);
    }

    // 0x000800C2...: program id, path type, outbox flag, id size, the message id (mapped). Reply:
    // result (the archive's), the buffer.
    void Delete(Request& request) {
        const u32 ncch_program_id = request.Pop<u32>();
        const PathType path_type = request.Pop<PathType>();
        const bool is_outbox = request.Pop<bool>();
        const u32 message_id_size = request.Pop<u32>();
        const MappedBuffer* message_id_buffer = request.PopMappedBuffer();

        const std::string path = DataPath(path_type, ncch_program_id);
        Reply& reply = request.MakeReply(1, 2);
        if (IsDirectory(path_type)) {
            reply.Push(Store().DeleteDirectoryRecursively(path));
        } else if (message_id_size == 0) {
            reply.Push(Store().DeleteFile(path));
        } else {
            const std::vector<u8> id = ReadBuffer(message_id_buffer, message_id_size);
            reply.Push(Store().DeleteFile(DataPath(
                is_outbox ? PathType::OutboxMsg : PathType::InboxMsg, ncch_program_id, id)));
        }
        PushBuffer(reply, message_id_buffer);
        MH4N_LOG(Debug, kLog, "called, ncch_program_id={:#010x}, path_type={:#04x}, path={}, is_outbox={}, message_id_size={:#x}",
                 ncch_program_id, static_cast<u32>(path_type), path, is_outbox, message_id_size);
    }

    // 0x000900C2: program id, size, option, the data (mapped, read). Option 2 rewrites the outbox
    // index. Reply: result, the buffer.
    void SetData(Request& request) {
        const u32 ncch_program_id = request.Pop<u32>();
        const u32 buffer_size = request.Pop<u32>();
        const u32 option = request.Pop<u32>();
        const MappedBuffer* read_buffer = request.PopMappedBuffer();

        if (option == 2 && buffer_size > 0) {
            OpenMode mode;
            mode.write = mode.create = true;
            std::shared_ptr<OpenFile> file;
            if (Store().OpenFile(DataPath(PathType::OutboxIndex, ncch_program_id), mode, &file) ==
                kFsOk) {
                std::vector<u8> buffer = ReadBuffer(read_buffer, buffer_size);
                CheckAndUpdateFile(Store(), PathType::OutboxIndex, ncch_program_id, buffer);
                file->Write(0, buffer.size(), buffer.data());
                file->Close();
            }
        }
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, read_buffer);
        MH4N_LOG(Debug, kLog, "called, ncch_program_id={:#010x}, buffer_size={:#x}, option={:#x}",
                 ncch_program_id, buffer_size, option);
    }

    // 0x000A00C4: destination size, info type, parameter size, the parameter (mapped), the destination
    // (mapped). Reply: result, both buffers.
    void ReadData(Request& request) {
        const u32 dest_buffer_size = request.Pop<u32>();
        const SystemInfoType info_type = request.Pop<SystemInfoType>();
        const u32 param_buffer_size = request.Pop<u32>();
        const MappedBuffer* param_buffer = request.PopMappedBuffer();
        const MappedBuffer* dest_buffer = request.PopMappedBuffer();

        std::vector<u8> buffer;
        switch (info_type) {
        case SystemInfoType::EulaVersion:
            buffer = {0x7F, 0x7F};  // {minor, major} of the EULA version cfg keeps
            break;
        case SystemInfoType::Eula:
            buffer = {1};  // agreed
            break;
        case SystemInfoType::ParentControl:
            buffer = {0};  // none
            break;
        default:
            MH4N_LOG(Error, kLog, "Unknown system info type={:#x}", static_cast<u32>(info_type));
        }
        if (dest_buffer != nullptr && !buffer.empty()) {
            const u32 count = std::min<u32>(dest_buffer_size, static_cast<u32>(buffer.size()));
            if (count != 0) {
                dest_buffer->Write(buffer.data(), 0, count);
            }
        }
        Reply& reply = request.MakeReply(1, 4);
        reply.Push(kResultSuccess);
        PushBuffer(reply, param_buffer);
        PushBuffer(reply, dest_buffer);
        MH4N_LOG(Debug, kLog, "called, dest_buffer_size={:#x}, info_type={:#x}, param_buffer_size={:#x}",
                 dest_buffer_size, static_cast<u32>(info_type), param_buffer_size);
    }

    void Start(Request& request) {
        const u32 command = request.Pop<u32>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) called, command={}", CecCommandName(command));
    }
    void Stop(Request& request) {
        const u32 command = request.Pop<u32>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) called, command={}", CecCommandName(command));
    }

    // 0x000D0082: info type, size, the buffer (mapped). Reply: result, the buffer (untouched).
    void GetCecInfoBuffer(Request& request) {
        const u32 possible_info_type = request.Pop<u32>();
        const u32 buffer_size = request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer);
        MH4N_LOG(Debug, kLog, "called, possible_info_type={}, buffer_size={}", possible_info_type,
                 buffer_size);
    }

    void GetCecdState(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(kCecdStateNdmIdle);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    void GetCecInfoEventHandle(Request& request) {
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(module_->cecinfo_event);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    void GetChangeStateEventHandle(Request& request) {
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(module_->change_state_event);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    // 0x00110104: size, program id, path type, open mode, the calling PID, the data (mapped, read).
    // Reply: result, the buffer.
    void OpenAndWrite(Request& request) {
        const u32 buffer_size = request.Pop<u32>();
        const u32 ncch_program_id = request.Pop<u32>();
        const PathType path_type = request.Pop<PathType>();
        OpenModeBits open_mode;
        open_mode.raw = request.Pop<u32>();
        request.PopPid();
        const MappedBuffer* read_buffer = request.PopMappedBuffer();

        const std::string path = DataPath(path_type, ncch_program_id);
        Reply& reply = request.MakeReply(1, 2);
        if (IsDirectory(path_type)) {
            reply.Push(kResultNotAuthorized);
        } else {
            OpenMode mode;
            mode.write = mode.create = true;
            std::shared_ptr<OpenFile> file;
            if (Store().OpenFile(path, mode, &file) == kFsOk) {
                std::vector<u8> buffer = ReadBuffer(read_buffer, buffer_size);
                if (file->GetSize() != buffer_size) {
                    file->SetSize(buffer_size);
                }
                if (open_mode.check()) {
                    CheckAndUpdateFile(Store(), path_type, ncch_program_id, buffer);
                }
                file->Write(0, buffer.size(), buffer.data());
                file->Close();
                reply.Push(kResultSuccess);
            } else {
                reply.Push(kResultNoData);
            }
        }
        PushBuffer(reply, read_buffer);
        MH4N_LOG(Debug, kLog, "called, ncch_program_id={:#010x}, path_type={:#04x}, path={}, buffer_size={:#x} open_mode: raw={:#x}",
                 ncch_program_id, static_cast<u32>(path_type), path, buffer_size, open_mode.raw);
    }

    // 0x00120104: size, program id, path type, open mode, the calling PID, the output (mapped, write).
    // Reply: result, bytes read, the buffer.
    void OpenAndRead(Request& request) {
        const u32 buffer_size = request.Pop<u32>();
        const u32 ncch_program_id = request.Pop<u32>();
        const PathType path_type = request.Pop<PathType>();
        OpenModeBits open_mode;
        open_mode.raw = request.Pop<u32>();
        request.PopPid();
        const MappedBuffer* write_buffer = request.PopMappedBuffer();

        const std::string path = DataPath(path_type, ncch_program_id);
        Reply& reply = request.MakeReply(2, 2);
        if (IsDirectory(path_type)) {
            reply.Push(kResultNotAuthorized);
            reply.Push<u32>(0);
        } else {
            OpenMode mode;
            mode.read = true;
            std::shared_ptr<OpenFile> file;
            if (Store().OpenFile(path, mode, &file) == kFsOk) {
                std::vector<u8> buffer(std::min<u32>(buffer_size, 0x1000000), 0);
                const u32 bytes_read = static_cast<u32>(file->Read(0, buffer.size(), buffer.data()));
                WriteBuffer(write_buffer, buffer, buffer_size);
                file->Close();
                reply.Push(kResultSuccess);
                reply.Push(bytes_read);
            } else {
                reply.Push(kResultNoData);
                reply.Push<u32>(0);
            }
        }
        PushBuffer(reply, write_buffer);
        MH4N_LOG(Debug, kLog, "called, ncch_program_id={:#010x}, path_type={:#04x}, path={}, buffer_size={:#x} open_mode: raw={:#x}",
                 ncch_program_id, static_cast<u32>(path_type), path, buffer_size, open_mode.raw);
    }

    std::shared_ptr<CecdModule> module_;
    // SessionData: what Open selected for the Read and Write that follow.
    u32 ncch_program_id_ = 0;
    PathType data_path_type_ = PathType::Invalid;
    OpenModeBits open_mode_;
    std::string path_;
    std::shared_ptr<OpenFile> file_;
};

MH4N_REGISTER(ServiceFactory, "cecd:u",
              ServiceFactory([](ServiceContext& context) { return std::make_unique<CecdU>(context); }));

}  // namespace

}  // namespace mh4n::cecd
