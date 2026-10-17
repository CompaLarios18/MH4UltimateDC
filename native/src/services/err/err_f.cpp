// Derived from Azahar src/core/hle/service/err/err_f.cpp and src/core/hle/service/err/err_f.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the err:f port, its ThrowFatalError command, the layout of the error information it
// receives (ErrInfo, ErrInfoCommon, ExceptionInfo, ExceptionContext, ExceptionData, FatalErrType,
// ExceptionType) and what it logs.
// Copyright 2014 Citra Emulator Project
//
// The err:f port (N1-10): the application reports a fatal error. The information is logged as Azahar
// logs it (type, process, title ids, address, result code, and by type the registers and exception
// registers or the failure message) and the run ends with status 3 (Runtime::RequestExit); the reply
// is a success, as Azahar's. SetUserString (0x02) is not implemented, as in Azahar. One departure: the
// FPEXC line of a vector floating point exception shows the fpexc field (Azahar prints fpinst under
// that name).
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <string>

#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Service.ERR";

enum class FatalErrType : u32 {
    Generic = 0,
    Corrupted = 1,
    CardRemoved = 2,
    Exception = 3,
    ResultFailure = 4,
    Logged = 5,
};

enum class ExceptionType : u32 {
    PrefetchAbort = 0,
    DataAbort = 1,
    Undefined = 2,
    VectorFP = 3,
};

struct ExceptionInfo {
    u8 exception_type;
    u8 padding[3];
    u32 sr;
    u32 ar;
    u32 fpexc;
    u32 fpinst;
    u32 fpinst2;
};
static_assert(sizeof(ExceptionInfo) == 0x18, "ExceptionInfo struct has incorrect size");

struct ExceptionContext {
    std::array<u32, 16> arm_regs;
    u32 cpsr;
};
static_assert(sizeof(ExceptionContext) == 0x44, "ExceptionContext struct has incorrect size");

struct ExceptionData {
    ExceptionInfo exception_info;
    ExceptionContext exception_context;
    u32 padding;
};
static_assert(sizeof(ExceptionData) == 0x60, "ExceptionData struct has incorrect size");

struct ErrInfoCommon {
    u8 specifier;           // 0x0
    u8 rev_high;            // 0x1
    u16 rev_low;            // 0x2
    u32 result_code;        // 0x4
    u32 pc_address;         // 0x8
    u32 pid;                // 0xC
    u32 title_id_low;       // 0x10
    u32 title_id_high;      // 0x14
    u32 app_title_id_low;   // 0x18
    u32 app_title_id_high;  // 0x1C
};
static_assert(sizeof(ErrInfoCommon) == 0x20, "ErrInfoCommon struct has incorrect size");

// What ThrowFatalError sends: 0x20 bytes for every type, then 0x60 bytes whose meaning depends on the
// type (the exception data, the failure message, or free data).
struct ErrInfo {
    ErrInfoCommon common;
    u8 body[0x60];
};
static_assert(sizeof(ErrInfo) == 0x80, "ErrInfo struct has incorrect size");

std::string ErrTypeName(u8 type_code) {
    switch (static_cast<FatalErrType>(type_code)) {
    case FatalErrType::Generic:
        return "Generic";
    case FatalErrType::Corrupted:
        return "Corrupted";
    case FatalErrType::CardRemoved:
        return "CardRemoved";
    case FatalErrType::Exception:
        return "Exception";
    case FatalErrType::ResultFailure:
        return "ResultFailure";
    case FatalErrType::Logged:
        return "Logged";
    default:
        return "Unknown Error Type";
    }
}

std::string ExceptionTypeName(u8 type_code) {
    switch (static_cast<ExceptionType>(type_code)) {
    case ExceptionType::PrefetchAbort:
        return "Prefetch Abort";
    case ExceptionType::DataAbort:
        return "Data Abort";
    case ExceptionType::Undefined:
        return "Undefined Exception";
    case ExceptionType::VectorFP:
        return "Vector Floating Point Exception";
    default:
        return "Unknown Exception Type";
    }
}

std::string CurrentSystemTime() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char text[32] = "";
    if (const std::tm* local = std::localtime(&now)) {
        std::strftime(text, sizeof(text), "%Y/%m/%d %H:%M:%S", local);
    }
    return text;
}

void LogGenericInfo(const ErrInfoCommon& common) {
    MH4N_LOG(Critical, kLog, "PID: 0x{:08X}", common.pid);
    MH4N_LOG(Critical, kLog, "REV: 0x{:08X}_0x{:08X}", common.rev_high, common.rev_low);
    MH4N_LOG(Critical, kLog, "TID: 0x{:08X}_0x{:08X}", common.title_id_high, common.title_id_low);
    MH4N_LOG(Critical, kLog, "AID: 0x{:08X}_0x{:08X}", common.app_title_id_high,
             common.app_title_id_low);
    MH4N_LOG(Critical, kLog, "ADR: 0x{:08X}", common.pc_address);
    MH4N_LOG(Critical, kLog, "RSL: 0x{:08X}", common.result_code);
    MH4N_LOG(Critical, kLog, "  Level: {}", ResultLevel(common.result_code));
    MH4N_LOG(Critical, kLog, "  Summary: {}", ResultSummary(common.result_code));
    MH4N_LOG(Critical, kLog, "  Module: {}", ResultModule(common.result_code));
    MH4N_LOG(Critical, kLog, "  Desc: {}", ResultDescription(common.result_code));
}

class ErrF final : public Service {
public:
    explicit ErrF(ServiceContext& context) : Service(context, "err:f") {
        Register(0x0001, "ThrowFatalError", &ErrF::ThrowFatalError);
        Register(0x0002, "SetUserString");
    }

private:
    // Inputs:  0: 0x00010800, 1-32: the error information (0x80 bytes)
    // Outputs: 0: 0x00010040, 1: result
    void ThrowFatalError(Request& request) {
        const ErrInfo info = request.PopRaw<ErrInfo>();
        MH4N_LOG(Critical, kLog, "Fatal error");
        MH4N_LOG(Critical, kLog, "Fatal error type: {}", ErrTypeName(info.common.specifier));
        LogGenericInfo(info.common);

        std::string detail;
        switch (static_cast<FatalErrType>(info.common.specifier)) {
        case FatalErrType::Generic:
        case FatalErrType::Corrupted:
        case FatalErrType::CardRemoved:
        case FatalErrType::Logged:
            MH4N_LOG(Critical, kLog, "Datetime: {}", CurrentSystemTime());
            break;
        case FatalErrType::Exception: {
            ExceptionData data;
            std::memcpy(&data, info.body, sizeof(data));
            MH4N_LOG(Critical, kLog, "ARM Registers:");
            for (u32 index = 0; index < data.exception_context.arm_regs.size(); ++index) {
                const u32 value = data.exception_context.arm_regs[index];
                if (index < 13) {
                    MH4N_LOG(Debug, kLog, "r{}=0x{:08X}", index, value);
                } else if (index == 13) {
                    MH4N_LOG(Critical, kLog, "SP=0x{:08X}", value);
                } else if (index == 14) {
                    MH4N_LOG(Critical, kLog, "LR=0x{:08X}", value);
                } else {
                    MH4N_LOG(Critical, kLog, "PC=0x{:08X}", value);
                }
            }
            MH4N_LOG(Critical, kLog, "CPSR=0x{:08X}", data.exception_context.cpsr);
            MH4N_LOG(Critical, kLog, "EXCEPTION TYPE: {}",
                     ExceptionTypeName(data.exception_info.exception_type));
            switch (static_cast<ExceptionType>(data.exception_info.exception_type)) {
            case ExceptionType::PrefetchAbort:
                MH4N_LOG(Critical, kLog, "IFSR: 0x{:08X}", data.exception_info.sr);
                MH4N_LOG(Critical, kLog, "r15: 0x{:08X}", data.exception_info.ar);
                break;
            case ExceptionType::DataAbort:
                MH4N_LOG(Critical, kLog, "DFSR: 0x{:08X}", data.exception_info.sr);
                MH4N_LOG(Critical, kLog, "DFAR: 0x{:08X}", data.exception_info.ar);
                break;
            case ExceptionType::VectorFP:
                MH4N_LOG(Critical, kLog, "FPEXC: 0x{:08X}", data.exception_info.fpexc);
                MH4N_LOG(Critical, kLog, "FINST: 0x{:08X}", data.exception_info.fpinst);
                MH4N_LOG(Critical, kLog, "FINST2: 0x{:08X}", data.exception_info.fpinst2);
                break;
            case ExceptionType::Undefined:
                break;  // Azahar does not log the exception information for this one
            }
            MH4N_LOG(Critical, kLog, "Datetime: {}", CurrentSystemTime());
            detail = ", " + ExceptionTypeName(data.exception_info.exception_type) + " at PC=0x" +
                     fmt::format("{:08X}", data.exception_context.arm_regs[15]) + " LR=0x" +
                     fmt::format("{:08X}", data.exception_context.arm_regs[14]);
            break;
        }
        case FatalErrType::ResultFailure: {
            // The message is 0x60 bytes and need not end with a NUL.
            const std::string message(
                reinterpret_cast<const char*>(info.body),
                ::strnlen(reinterpret_cast<const char*>(info.body), sizeof(info.body)));
            MH4N_LOG(Critical, kLog, "Failure Message: {}", message);
            MH4N_LOG(Critical, kLog, "Datetime: {}", CurrentSystemTime());
            break;
        }
        }

        request.MakeReply(1, 0).Push(kResultSuccess);
        Context().RequestExit(
            3,
            fmt::format("fatal error through err:f: type {}, result 0x{:08X}, address 0x{:08X}{}",
                        ErrTypeName(info.common.specifier), info.common.result_code,
                        info.common.pc_address, detail));
    }
};

MH4N_REGISTER(ServiceFactory, "err:f",
              ServiceFactory::NamedPort(
                  [](ServiceContext& context) { return std::make_unique<ErrF>(context); }, 1));

}  // namespace

}  // namespace mh4n
