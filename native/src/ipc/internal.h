// Private to mh4n_ipc.
#pragma once

namespace mh4n {

class Runtime;
class ServiceManager;

namespace detail {

// FindServiceManager's table: the boot step records the manager of each runtime and the manager's
// destructor forgets it.
void RememberServiceManager(Runtime* runtime, ServiceManager* manager);
void ForgetServiceManager(ServiceManager* manager);

}  // namespace detail

}  // namespace mh4n
