// Derived from Azahar src/core/hle/kernel/object.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later): which handle types a thread can wait on.
// Copyright 2018-2024 Citra Emulator Project / Azahar Emulator Project
#include "mh4n/assert.h"
#include "mh4n/kernel.h"

namespace mh4n {

Object::Object(Kernel& kernel) : kernel_(kernel), object_id_(kernel.GenerateObjectId()) {}

bool Object::IsWaitable() const {
    switch (GetHandleType()) {
    case HandleType::Event:
    case HandleType::Mutex:
    case HandleType::Thread:
    case HandleType::Semaphore:
    case HandleType::Timer:
    case HandleType::ServerPort:
    case HandleType::ServerSession:
        return true;
    case HandleType::Unknown:
    case HandleType::SharedMemory:
    case HandleType::Process:
    case HandleType::AddressArbiter:
    case HandleType::ResourceLimit:
    case HandleType::CodeSet:
    case HandleType::ClientPort:
    case HandleType::ClientSession:
        return false;
    }
    MH4N_UNREACHABLE();
}

} // namespace mh4n
