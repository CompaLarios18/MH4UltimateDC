// The `services` boot step (N1-10): the service manager of a runtime, with every registered service
// installed and the SVC layer's ConnectToPort and SendSyncRequest handlers pointing at it.
#include <map>
#include <memory>
#include <mutex>

#include "internal.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"

namespace mh4n {

namespace {

std::mutex g_managers_mutex;
std::map<Runtime*, ServiceManager*>& Managers() {
    static std::map<Runtime*, ServiceManager*> managers;
    return managers;
}

void StartServices(Runtime& rt) {
    ServiceEnvironment env;
    env.game_folder = rt.GameFolder();
    env.user_folder = rt.UserFolder();
    env.options = &rt.GetOptions();
    env.kernel = &rt.GetKernel();
    env.scheduler = &rt.Scheduler();
    env.threads = &rt.Threads();
    env.interfaces = &rt.Interfaces();
    env.request_exit = [&rt](int status, const std::string& reason) {
        rt.RequestExit(status, reason);
    };
    auto manager = std::make_shared<ServiceManager>(std::move(env));
    manager->InstallRegistered();
    manager->Install(rt.Svc());
    detail::RememberServiceManager(&rt, manager.get());
    rt.Own(manager);
}

MH4N_REGISTER(BootStep, "services.ipc", BootStep{Phase::Services, StartServices});

}  // namespace

namespace detail {

void RememberServiceManager(Runtime* runtime, ServiceManager* manager) {
    std::lock_guard lock(g_managers_mutex);
    Managers()[runtime] = manager;
}

void ForgetServiceManager(ServiceManager* manager) {
    std::lock_guard lock(g_managers_mutex);
    for (auto it = Managers().begin(); it != Managers().end();) {
        it = it->second == manager ? Managers().erase(it) : std::next(it);
    }
}

}  // namespace detail

ServiceManager* FindServiceManager(Runtime& runtime) {
    std::lock_guard lock(g_managers_mutex);
    const auto it = Managers().find(&runtime);
    return it == Managers().end() ? nullptr : it->second;
}

}  // namespace mh4n
