# Azahar dependency inventory (N0-16)

Every place where the app, the launcher, the hooks, the runner and `server/` depend on Azahar, grouped by concern, each with its planned replacement. Written for N0-18 (hooks on a platform interface), N0-20 and N0-24 (`IRuntime`) and the N1 workers.

Pinned Azahar commit: `5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6`. Counts below come from the grep in the next section, run on `def93e9`; the section "Since the recount" lists what N0-04 and N0-07 added to the runner and the patch afterwards. No game data is involved: this is a map of our own sources.

## Replacement vocabulary

| Tag | Meaning |
|---|---|
| **IRuntime** | A method of the `IRuntime` interface (`port/app/runtime.h`): N0-20 (lifecycle, options, perf stats, paths, title install, logging and file helpers) or N0-24 (input, multiplayer, software keyboard). Azahar implements it in `port/app/runtime_azahar/`. |
| **Platform** | The `Port::Platform` interface of the hooks (`port/src/platform.h`, N0-18). Azahar implements it in `port/src/platform_azahar.cpp`; the native runtime in N1-20. |
| **util** | `port/app/util/` (N0-20): the app's own logging and file helpers; the Azahar implementation forwards logging to Azahar's logger. |
| **native module** | A module of the native runtime (`native/src/...`, N1+), named in the cell. |
| **Azahar build only** | Stays in the Azahar executable (the oracle factory, D12). The native executable has no counterpart because N2b/N1 replace the whole piece. |

## Since the recount

N0-18 moved every use of Azahar in `port/src/hooks.cpp` and `hooks.h` (the logger, `Layout::ScreenOverlay` and `SetTopScreenWidth`, `Core::System`, `Settings::values`, `FileUtil::GetUserPath`, `NetSync`) behind `Port::Platform` (`port/src/platform.h`); `port/src/platform_azahar.cpp` holds the calls and `Port::AzaharPlatform()` is its instance. The two files no longer count in the grep below. `runner.cpp` reaches Azahar's layout through `Port::AzaharPlatform()` in `--overlay-layout`. N1-20 implements `Platform` for the native runtime; `NetSync::ParseMode` is now unused by the hooks (`platform.h` has `Port::ParseNetSyncMode` with the same rules).

N0-04, N0-07 and N0-21 changed `port/src/runner.cpp` and `port/azahar/azahar.patch` after `def93e9`: `runner.cpp` had 32 Azahar includes at the time (38 after N0-05 and N0-06; the recount below is current) (N0-04 and N0-07 added `core/loader/loader.h` and `core/memory.h`; N0-21 added seven for the IPC trace: `client_session`, `ipc_debugger/recorder`, `object`, `server_session`, `session`, `fs/directory` and `fs/file`) and the patch is 1,525 lines (N0-04 and N0-07 only; N0-21 added no hook). The rows below for `runner.cpp` and the patch are otherwise as counted; these APIs are new, with their replacement:

| API | Used for | Replacement |
|---|---|---|
| `Kernel::SetSVCPostObserver` (the patch adds it in `svc.cpp`/`svc.h`) | the `out=` registers of each SVC call in the SVC trace (N0-04) | the native runtime writes the same trace format from its own SVC table (N1-07, N1-09); Azahar build only here |
| `Kernel::SetThreadResumeObserver` (the patch adds it in `thread.cpp`/`thread.h`) | writes the `out=` of a call that blocked, when its thread next runs (N0-04) | the native scheduler (N1-06) |
| `Settings::values.is_new_3ds` | `--new-3ds on\|off` (N0-04); reference runs are Old 3DS (D10) | the `new_3ds` setting goes away in the native port (D10) |
| `Core::System::GetAppLoader()` and `Loader::AppLoader::ReadProgramId` | the title id in trace headers (N0-04, N0-07) | native loader (N1-03) |
| `Memory::SetUnmappedObserver` (the patch adds it in `memory.cpp`/`memory.h`) | the unmapped-access report of `--count-unmapped` (N0-07) | guest memory reports bad accesses itself (N1-02) |
| `Memory::MemorySystem::ReadBlock`, `IsValidVirtualAddress` | reading the observable values of `--state-trace` (N0-07) | guest memory helpers (N1-02) |
| `Kernel().GetIPCRecorder()` (`SetEnabled`, `BindCallback`, `UnbindCallback`, `RequestRecord`), `Process::handle_table.GetGeneric`, `KernelSystem::GetProcessById`, `Thread::GetCommandBufferAddress`, `Service::FS::File` and `Directory` (a `dynamic_pointer_cast` on a session's `hle_handler`); `ClientPort::name` is `<service>_Client` and the trace strips the suffix | `--trace-ipc` (N0-21) | the native IPC core writes the same trace from its own sessions (N1-10, N1-11) |

## How to recount

```
grep -rlE '#include "(core|video_core|audio_core|network|input_common|common|citra[a-z_]*|web_service)/' port server | sort
```

This finds 23 files, all under `port/` (the hooks reach Azahar only through `src/platform_azahar.cpp`; in the app, only `app/runtime_azahar/`, the window files (`window.h`, `window*.cpp`) and `ui/render_*.cpp` include Azahar since N0-20 and N0-24; `server/` includes no Azahar header; its dependency is on the `azahar-room` binary, see section 9). The bootstrap table of N0-16 listed counts per file; the exact recount below differs in a few small files, so the table is the starting point, not a contract. `common/` is Azahar's common library (logging, file utilities, settings), not ours.

Since N2b-01 `IRuntime::HostSystem()` is gone: `port/app/main.cpp` gets the system the windows are made for from `AzaharRuntime::CoreSystem()` (a method of the Azahar runtime, not of `IRuntime`), `IRuntime::Load` takes `App::IWindow&` (`port/app/app_window.h`; the Azahar runtime casts to the `App::Window` it made), and `Multiplayer::Init(config)` and `Ui::Start(window, config)` take no `Core::System&`. The in-game menu (`ui/ui.cpp`, `ui/menu.cpp`, `ui/layer.h`) draws over an `App::IWindow`, so no file under `port/app/ui/` (but `render_*.cpp`) or `port/app/launcher/` includes `window.h`, `frontend.h`, `netsync.h` or an Azahar header; `tests/test_native_app_util.py` checks it. The window and renderer files are what N2b replaces.

Count of Azahar includes per file (recount after N0-24; the grep also counts `applets/` includes):

| Includes | File |
|---|---|
| 50 | `port/azahar/azahar.patch` (the patch's own context lines, not code of ours) |
| 38 | `port/src/runner.cpp` |
| 15 | `port/app/runtime_azahar/azahar_runtime.cpp` |
| 12 | `port/app/runtime_azahar/azahar_network.cpp`, `port/app/runtime_azahar/azahar_input.cpp` |
| 11 | `port/src/platform_azahar.cpp` |
| 8 | `port/src/arm_recomp.cpp` |
| 7 | `port/app/runtime_azahar/azahar_options.cpp` |
| 6 | `port/src/frontend.cpp`, `port/app/runtime_azahar/azahar_title.cpp` |
| 5 | `port/src/netsync.cpp`, `port/app/window_gl.cpp`, `port/app/window_sw.cpp` |
| 1-2 | the rest (`window.cpp`, `window.h`, `window_vk.cpp`, `ui/render_*.cpp`, `src/arm_recomp.h`, `src/netsync.h`, `runtime_azahar/azahar_keyboard.h`, `runtime_azahar/log_sink.cpp`) |

## Libraries linked

`port/app/CMakeLists.txt` links the executable `mh4u` against `audio_core citra_common citra_core input_common network video_core inih`, plus `web_service` when `ENABLE_WEB_SERVICE` is on. The runner library (`mh4u_backend`, `port/CMakeLists.txt`) uses the same Azahar targets.

| Library | Used for | Replacement |
|---|---|---|
| `citra_common` | logging, file utilities, `Settings::values`, `ParamPackage`, string utilities, scm revision | util (logging, files, strings); IRuntime options (settings); `native/src/common` options (`mh4n/options.h`, N1-01; the runner's options in N1-09) |
| `citra_core` | `Core::System`, kernel, services, loader, file_sys, memory, timing, CPU interface, frontend interfaces | native modules: kernel (N1-05..N1-07), services (N1-10..N1-19), loader (N1-03), memory (N1-02), scheduler and clock (N1-06) |
| `video_core` | GPU, renderers (software, OpenGL, Vulkan), `Pica` registers | native GPU (N2); Azahar build only for the existing renderers |
| `audio_core` | DSP HLE sink selection | native DSP (N1-18) and audio mixer; IRuntime option for the sink |
| `input_common` | SDL/keyboard/touch input providers | IRuntime input sink (N0-24); N1-16 (scripts); SDL3 input in N2b (N2b-01/02) |
| `network` | room client and server, `Network::Init` | IRuntime multiplayer (N0-24); native room client compatible with Azahar's (N4-01, provisional id; N1-29 only answers nwm::UDS without a room) |
| `web_service` | JWT verification for lobby login | IRuntime multiplayer (N0-24); native `net/` lobby client |

## 1. Lifecycle

| File | APIs used | Replacement |
|---|---|---|
| `port/app/main.cpp` | `Core::System::GetInstance`, `System::Load`, `System::RunLoop`, `System::Shutdown`, `System::GetStatusDetails`, `System::ApplySettings`, `System::GetAppLoader().ReadProgramId`, `System::ResultStatus::{Success,ShutdownRequested}`, `System::ServiceManager`, `Core::System::GPU().ApplyPerProgramSettings`, `Frontend::RegisterDefaultApplets`, `VideoCore::LoadCallbackStage`, `Service::APT/CFG/HID/IR::GetModule`, `Service::IR::IR_USER`, `Service::IR::IR_RST`, `Common::DetachedTasks` | IRuntime lifecycle (N0-20: load game, update, code library; run; pause; stop; status); the service-module poking becomes native services (N1-14, N1-15, N1-16) |
| `port/src/runner.cpp` | `Core::System::GetInstance`, `System::Load`, `System::RunLoop`, `System::Shutdown`, `System::SetCPUFactory`, `System::GetRunningCore`, `System::ResultStatus`, `System::RegisterImageInterface`, `Frontend::ImageInterface`, `Frontend::RegisterDefaultApplets`, `Frontend::EmuWindow`, `Core::Timing::Timer`, `Core::ARM_Interface`, `Service::service_module_map`, `Settings::InitClock::FixedTime` | native runner (N1-09); the headless-runner options stay in `port/src/runner.cpp` (Azahar build only) |
| `port/app/launcher/launcher.cpp` | `Core::System::GetInstance`, `Common::g_build_fullname` | IRuntime lifecycle (N0-20: a build-name accessor) |
| `port/app/ui/layer.h` | none since N2b-01 (it took `Core::System` before N0-20 and `Window&`, Azahar's, before N2b-01; now `App::IWindow&`) | done |
| `port/app/ui/ui.h` | none since N2b-01 (`Ui::Start(IWindow&, Config&)`); it still declares the OpenGL and software drawing functions the Azahar windows call (N2b-02 moves them) | done |
| `port/app/window.h` | `Core::System`, `Frontend::EmuWindow`, `InputCommon::Init`; implements `App::IWindow` (`port/app/app_window.h`, N2b-01) | Azahar build only (window and renderer files, N2b replaces them) |
| `port/src/arm_recomp.h` | `Core::ARM_Interface`, `Core::System`, `Core::Timing::Timer`, `Memory::MemorySystem`, `Memory::PageTable`, `Kernel::SVCContext` | native module `native/src/cpu` (N1-04, code host); Azahar build only for `ARM_Recomp` |
| `port/src/arm_recomp.cpp` | `Core::ARM_DynCom` (fallback interpreter), `Core::System`, `Core::Timing::Timer`, `Memory::PageTable`, `Memory::CITRA_PAGE_SIZE`, `Memory::CITRA_PAGE_MASK`, `Memory::MemorySystem`, `Common::DynamicLibrary`, `ASSERT`, `UNREACHABLE` | N0-17 (`RC_NATIVE`) for the build mode; native code host (N1-04) for library loading; fallback interpreter (N1-28, D13) for `ARM_DynCom`; Azahar build only for the rest |
| `port/src/frontend.cpp` | `Core::System`, `System::SetCPUFactory`, `Core::Timing::Timer`, `Core::ARM_Interface`, `Memory::MemorySystem`, `Loader::GetLoader`, `Loader::AppLoader_NCCH`, `Loader::ResultStatus`, `FileSys::NCCHContainer`, `Service::AM::GetTitleContentPath`, `Service::FS::MediaType::SDMC` | native loader (N1-03) and extracted RomFS (N1-12); Azahar build only for `SetCPUFactory` |
| `port/src/netsync.h` | `Core::System::RunLoop` (comment/type reference), `Core::System` | Platform (net sync, N0-18) |

## 2. Settings and options

| File | APIs used | Replacement |
|---|---|---|
| `port/app/config.h` | `Settings::NativeButton`, `Settings::NativeAnalog` (array sizes) | IRuntime options (N0-20); button and analog counts become constants of the app |
| `port/app/config.cpp` | `Settings::values`, `Settings::LayoutOption::*`, `Settings::GraphicsAPI::*`, `AudioCore::SinkType::*`, `Settings::NativeButton`, `Settings::NativeAnalog`, `Input::BuildProfile`, `Service::service_module_map` (every module set to high-level emulation) | IRuntime options (N0-20); layout and graphics-API enums become app enums converted in `runtime_azahar`; the service map is Azahar build only |
| `port/app/main.cpp` | `Settings::values`, `Settings::LogSettings`, `Settings::LayoutOption::*`, `Settings::GraphicsAPI::*` | IRuntime options (N0-20) |
| `port/app/ui/menu.cpp` | `Settings::values` (13 reads and writes), `Settings::LayoutOption::*`, `Settings::GraphicsAPI::*`, `Settings::NativeButton`, `Settings::NativeAnalog`, `Core::System::CoreTiming().UpdateClockSpeed` | IRuntime options and perf (N0-20); `UpdateClockSpeed` becomes the option `cpu_clock` forwarded to the Platform in the Azahar build only (native clock is the host clock) |
| `port/app/ui/hint.cpp` | `Settings::values`, `Settings::NativeButton` (button mapping shown in the hint) | IRuntime input (N0-24) |
| `port/app/window_gl.cpp` | `Settings::values` (resolution, VSync, filter) | Azahar build only (window files) |
| `port/app/window_sw.cpp` | `Settings::values` | Azahar build only |
| `port/src/hooks.cpp` | `Settings::values`, `Settings::AspectRatio` | Platform `GetOption`/`SetOption` (N0-18) |
| `port/src/runner.cpp` | `Settings::values` (13), `Settings::NativeButton::*`, `Settings::NativeAnalog::CirclePad`, `Settings::LayoutOption::CustomLayout`, `Settings::GraphicsAPI::Software`, `AudioCore::SinkType::Null`, `AudioCore::InputType::Null` | native runner options (N1-09); Azahar build only for the headless setup |
| `port/app/multiplayer.cpp` | `Settings::values` (3: username, room address) | IRuntime multiplayer (N0-24) |
| `port/app/input.cpp` | `Settings::values` (input profile) | IRuntime input (N0-24) |

## 3. Input

| File | APIs used | Replacement |
|---|---|---|
| `port/app/input.cpp` | `Input::ButtonDevice`, `Input::AnalogDevice`, `Input::Factory`, `Input::RegisterFactory`, `Input::CreateDevice`, `Common::ParamPackage`, `InputCommon::GetControllerButtonBinds`, `InputCommon::GetControllerAnalogBinds`, `InputCommon::GenerateKeyboardParam`, `InputCommon::GenerateAnalogParamFromKeys`, `Common::StripSpaces`, `Common::SplitString` | IRuntime input sink (N0-24: buttons, sticks, touch, keyboard text); native hid, N1-16 (scripts); SDL3 input in N2b (N2b-01/02) |
| `port/app/main.cpp` | `InputCommon::Init`, `InputCommon::Shutdown`, `Input::Register`, `Input::BuildProfile`, `Input::ControllerDescription` | IRuntime input (N0-24) |
| `port/app/ui/ui.cpp` | `InputCommon::GetKeyboard`, `Input::SetMuted`, `Input::KeyCode` | IRuntime input (N0-24) |
| `port/app/ui/menu.cpp` | `Input::KeyCode`, `Input::ControllerDescription`, `Common::ParamPackage` | IRuntime input (N0-24) |
| `port/app/window.cpp` | `InputCommon::GetKeyboard`, `Input::KeyCode` (key down and up forwarding) | Azahar build only (the native window forwards to the input sink, N2b) |
| `port/src/runner.cpp` | `Input::TouchDevice`, `Input::ButtonDevice`, `Input::AnalogDevice`, `Input::RegisterFactory`, `Input::Factory`, `Input::CreateDevice`, `Common::ParamPackage` (scripted input) | native hid from scripts (N1-16) |
| `port/app/ui/hint.cpp` | `Settings::NativeButton` | IRuntime input (N0-24) |

## 4. Rendering and window

| File | APIs used | Replacement |
|---|---|---|
| `port/app/window.h` | `Frontend::EmuWindow` (base class), `Core::System` | Azahar build only; native window and compositor (N2b) |
| `port/app/window.cpp` | `Core::System::GetAndResetPerfStats`, `Core::System`, `LOG_*` | IRuntime perf stats (N0-20); window is Azahar build only |
| `port/app/window_gl.cpp` | `Frontend::GraphicsContext`, `Frontend::TextureMailbox`, `VideoCore::RendererBase` (`System::GPU().Renderer()`), `VideoCore::GPU` | Azahar build only |
| `port/app/window_sw.cpp` | `Frontend::GraphicsContext`, `VideoCore::ScreenId::{TopLeft,Bottom}`, `SwRenderer::RendererSoftware`, `Common::Rectangle` | Azahar build only |
| `port/app/window_vk.cpp` | `Frontend::GraphicsContext`, `Frontend::WindowSystemType::{X,Windows,Wayland,MacOS}` | Azahar build only |
| `port/app/ui/render_vk.cpp` | `video_core/renderer_vulkan/vk_overlay.h` (from our patch: draws the overlay in Azahar's Vulkan renderer) | Azahar build only; native compositor (N2b) |
| `port/app/ui/render_gl.cpp` | `LOG_ERROR` only | util logging (N0-20) |
| `port/app/ui/render_sw.cpp` | `LOG_ERROR` only | util logging (N0-20) |
| `port/app/ui/menu.cpp` | `Layout::ScreenOverlay`, `Layout::ScreenOverlay::Corner`, `VideoCore::RendererBase` (`System::GPU().Renderer()`) | Platform overlay struct (N0-18) replaces `Layout::ScreenOverlay`; renderer access is Azahar build only |
| `port/src/hooks.cpp` | `Layout::ScreenOverlay`, `Layout::SetScreenOverlay`, `Layout::SetTopScreenWidth`, `Core::kScreenBottomWidth`, `Core::kScreenBottomHeight`, `Core::System::GetInstance().PicaCore().regs.framebuffer_config`, `Core::System::UpdateCurrentFramebufferLayout`, `Core::System::SetRefreshRate`, `Core::System::IsPoweredOn`, `Core::System::CoreTiming().GetGlobalTicks` | Platform `SetTopScreenRows`, `SetRefreshRate`, overlay struct, `RefreshScreenLayout`, top-screen width (N0-18); constants become Platform or app constants |
| `port/src/hooks.h` | `Layout::ScreenOverlay` (in the interface) | Platform plain struct (N0-18) |
| `port/src/runner.cpp` | `Layout::SetTopScreenWidth`, `Layout::SetScreenOverlay`, `Layout::CustomFrameLayout`, `Core::kScreenTopWidth`, `Core::kScreenBottomWidth`, `Core::kScreenBottomHeight`, `VideoCore::ScreenId::{TopLeft,Bottom}`, `SwRenderer::RendererSoftware` (screenshots), `Frontend::EmuWindow` (headless window) | native runner and software compositor (N1-09, N2b); Azahar build only here |
| `port/app/main.cpp` | `VideoCore::LoadCallbackStage`, `VideoCore::RendererBase`, `VideoCore::RasterizerInterface`, `Core::System::GPU().Renderer()` | Azahar build only (shader-cache progress callbacks); native pipeline cache (N2-16) |

## 5. Rooms and network

| File | APIs used | Replacement |
|---|---|---|
| `port/app/multiplayer.cpp` | `Network::GetRoomMember`, `Network::GetRoom`, `Network::RoomMember` (join, leave, callbacks, `MemberList`, `State`, `Error`), `Network::Room` (`State::Open`), `Network::RoomInformation`, `Network::WifiPacket` (`PacketType::{Data,Beacon}`), `Network::StatusMessageEntry`, `Network::MacAddress`, `Network::NoPreferredMac`, `Network::IdMemberJoin`, `Network::IdMemberLeave`, `Network::GetErrorStr`, `Network::AnnounceMultiplayerSession`, `Network::VerifyUser::{Backend,NullBackend}`, `WebService::VerifyUserJWT`, `Common::WebResult::Code`, `Service::CFG::GetConsoleMacAddress`, `Service::CFG::GetUsername`, `Service::CFG::GetConsoleIdHash`, `Kernel::GetSharedPageHandler` (via `System::Kernel()`) | IRuntime multiplayer (N0-24: join, host, leave, members, chat, callbacks); native room client (N4-01, provisional id) and own room server (N4-04, provisional id), compatible with Azahar's; cfg identity from native cfg (N1-15) |
| `port/app/ui/multiplayer_tab.cpp` | `LOG_INFO` only (uses `multiplayer.h`, our own) | util logging (N0-20); the tab's data comes from IRuntime multiplayer (N0-24) |
| `port/app/main.cpp` | `Network::Init`, `Network::Shutdown`, `Network::GetRoomMember` | IRuntime multiplayer (N0-24) |
| `port/src/netsync.cpp` | `Network::RoomMember`, `Network::GetRoomMember`, `Network::RoomMember::{MemberList,CallbackHandle}`, `Network::WifiPacket`, `Network::MacAddress`, `Network::BroadcastMac`, `Network::Init`, `Core::System::SetRunLoopHook`, `Core::System::CoreTiming().GetGlobalTicks` | Platform net sync (install with a mode, end-skip vote, N0-18); native net sync on the game clock (N4-03, provisional id; N1-20 only gives the local-only Platform net sync until N4); `SetRunLoopHook` is Azahar build only |
| `port/src/runner.cpp` | `Network::Init`, `Network::Shutdown`, `Network::GetRoomMember`, `Network::GetRoom`, `Network::RoomMember::{State,Error}`, `Network::GetStateStr`, `Network::GetErrorStr`, `Network::VerifyUser::NullBackend`, `Service::CFG::MacToString`, `Service::CFG::GetConsoleIdHash` | native runner room options (N4-01, provisional id); Azahar build only here |

## 6. Title install (AM, NCCH, TMD) and files

| File | APIs used | Replacement |
|---|---|---|
| `port/app/launcher/checks.h` | `Service::AM` types in a comment and `common_types.h` (`u16`, `u64`) | IRuntime title install (N0-20); `common_types.h` becomes `<cstdint>` aliases in util |
| `port/app/launcher/checks.cpp` | `Service::AM::InstallCIA`, `Service::AM::CheckCIAToInstall`, `Service::AM::GetCIAInfos`, `Service::AM::GetTitleMetadataPath`, `Service::AM::GetTitleContentPath`, `Service::AM::InstallStatus::*`, `Service::FS::MediaType::SDMC`, `FileSys::TitleMetadata`, `FileSys::NCCHContainer`, `Loader::ResultStatus::*`, `FileUtil::Exists`, `FileUtil::IsDirectory`, `FileUtil::GetUserPath` | IRuntime title install (N0-20); native: extracted RomFS and folders (N1-03, N1-12; `tools/extract_ncch.py` exists) |
| `port/src/frontend.cpp` | `FileSys::NCCHContainer`, `Loader::GetLoader`, `Loader::AppLoader_NCCH`, `Loader::ResultStatus`, `Service::AM::GetTitleContentPath`, `FileUtil::GetUserPath`, `FileUtil::Exists` | native loader (N1-03) |
| `port/src/runner.cpp` | `Service::AM::InstallCIA`, `Service::AM::InstallStatus::Success` (update install in the headless runner), `Kernel::Process`, `Kernel::ProcessStatus::Exited`, `Kernel::VMAType::BackingMemory`, `Kernel::VMAPermission::Write`, `System::Kernel().GetCurrentThreadManager`, `System::Kernel().GetCurrentProcess` (reads guest memory maps), `Kernel::SetSVCObserver` (from our patch), `Memory::MemorySystem` | native runner (N1-09); `SetSVCObserver` gets a native equivalent in the SVC table (N1-07); Azahar build only here |
| `port/app/multiplayer.cpp` | `FileUtil::UserPath::SysDataDir`, `FileUtil::GetUserPath`, `FileUtil::CreateFullPath` | util files (N0-20) |

## 7. Logging and common utilities

The same few helpers appear in many files; the replacement is always `port/app/util/` (N0-20, app files), the Platform `Log(level, message)` (N0-18, hooks), or the native logger of N1-01 (`native/src/common`, `mh4n/log.h`, `MH4N_LOG`, native runtime).

| File | APIs used | Replacement |
|---|---|---|
| `port/app/dialog.cpp` | `LOG_INFO`, `LOG_ERROR`, `Common::UTF8ToUTF16`, `Common::UTF16ToUTF8` (string_util) | util (logging, strings) |
| `port/app/launcher/lobby.cpp` | `LOG_INFO`, `LOG_WARNING`, `FileUtil::Exists` | util |
| `port/app/launcher/lobby.h` | `common_types.h` | util (fixed-width aliases) |
| `port/app/launcher/pick.cpp` | `Common::UTF8ToUTF16W` (string_util) | util |
| `port/app/launcher/process.cpp` | `Common::UTF8ToUTF16W` (string_util) | util |
| `port/app/launcher/screen_mods.cpp` | `LOG_INFO`, `LOG_WARNING`, `FileUtil::CreateFullPath` | util |
| `port/app/launcher/screen_settings.cpp` | none since N2b-01 (it includes `runtime.h`, `launcher/*.h`, ImGui and fmt only) | done |
| `port/app/launcher/tools.cpp` | `FileUtil::Exists`, `FileUtil::IsDirectory`, `FileUtil::GetUserPath`, `FileUtil::ForeachDirectoryEntry` | util files / IRuntime paths (N0-20) |
| `port/app/launcher/ui.cpp` | `LOG_INFO`, `FileUtil::Exists` | util |
| `port/app/launcher/launcher.cpp` | `LOG_INFO`, `LOG_ERROR`, `LOG_WARNING`, `FileUtil::Exists`, `Common::g_build_fullname` | util; IRuntime build name (a string accessor to add in N0-20) |
| `port/app/config.cpp` | `LOG_INFO`, `LOG_WARNING`, `LOG_ERROR`, `FileUtil::WriteStringToFile`, `FileUtil::CreateFullPath`, `FileUtil::GetUserPath`, `FileUtil::UserPath::UserDir`, `FileUtil::Exists` | util; IRuntime paths (N0-20) |
| `port/app/main.cpp` | `Common::Log::{Initialize,Start,Stop,SetGlobalFilter,SetColorConsoleBackendEnabled,Filter}`, `LOG_*`, `FileUtil::{Exists,WriteStringToFile,SetUserPath}`, `Common::g_scm_desc`, `Common::g_scm_branch`, `Common::g_build_fullname`, `Common::UTF8ToUTF16W` | util (logging backend wrapper: Azahar implementation forwards to Azahar's logger); IRuntime build info and paths |
| `port/app/input.cpp` | `LOG_WARNING`, `Common::StripSpaces`, `Common::SplitString`, `Common::UTF8ToUTF16W` | util |
| `port/app/ui/menu.cpp` | `LOG_INFO`, `LOG_WARNING`, `LOG_ERROR`, `FileUtil::ReadFileToString`, `FileUtil::WriteStringToFile`, `Common::StripSpaces`, `Common::SplitString` | util |
| `port/app/ui/keyboard.cpp` | `LOG_INFO`, `LOG_WARNING`, `Common::UTF8ToUTF16`, `Common::UTF16ToUTF8` | util |
| `port/app/ui/ui.h` | `common_types.h` | util |
| `port/app/ui/layer.h` | `common_types.h` | util |
| `port/app/ui/ui.cpp` | `LOG_INFO` | util |
| `port/app/window.cpp` | `LOG_ERROR`, `LOG_CRITICAL` | Azahar build only (window); util for the shared part |
| `port/app/window_gl.cpp`, `port/app/window_sw.cpp`, `port/app/window_vk.cpp` | `LOG_CRITICAL`, `LOG_*` | Azahar build only |
| `port/app/multiplayer.cpp` | `LOG_INFO` (13), `LOG_WARNING`, `LOG_ERROR` | util |
| `port/src/hooks.cpp` | `LOG_INFO` (27), `FileUtil::GetUserPath(UserDir)`, `common_types.h` (`u8`..`u64`) | Platform `Log` and `UserDir()` (N0-18); fixed-width types from `<cstdint>` |
| `port/src/netsync.h` | `common_types.h` | Platform net sync (N0-18) |
| `port/src/netsync.cpp` | `LOG_INFO` | Platform `Log` (N0-18) |
| `port/src/arm_recomp.cpp` | `LOG_CRITICAL`, `LOG_WARNING`, `LOG_TRACE`, `LOG_INFO`, `ASSERT`, `UNREACHABLE`, `Common::DynamicLibrary` | native code host (N1-04); Azahar build only here |
| `port/src/frontend.cpp` | `LOG_*`, `FileUtil::*` | native loader (N1-03) |
| `port/src/runner.cpp` | `Common::Log::*`, `FileUtil::{SetUserPath,CreateFullPath}` | native runner (N1-09); Azahar build only here |

## 8. Web service

| File | APIs used | Replacement |
|---|---|---|
| `port/app/multiplayer.cpp` | `WebService::VerifyUserJWT` and `Network::AnnounceMultiplayerSession` (lobby announce), both under `#ifdef ENABLE_WEB_SERVICE` | IRuntime multiplayer (N0-24); native lobby client (N4-01, provisional id); `server/lobby/mh4u_lobby.py` stays and speaks the same web API |
| `port/src/runner.cpp` | `Network::VerifyUser::NullBackend` | native runner room options (N4-01, provisional id) |

## 9. `server/` and the patch

`server/` includes no Azahar header, but depends on Azahar at the process and protocol level:

| Path | Dependency | Replacement |
|---|---|---|
| `server/docker/Dockerfile.room`, `server/docker/room-entrypoint.sh`, `server/docker/compose.yaml`, `server/docker/example.env` | builds `azahar-room` from Azahar's source at `port/azahar/COMMIT` with `port/azahar/azahar.patch`; the entrypoint starts it | native room server in `native/src/net/` (ROADMAP: network with the room server); Azahar build only until then |
| `server/unix/host-room.sh`, `server/windows/host-room.ps1`, `server/windows/Host room.bat` | start `azahar-room` | the native room server once it exists; Azahar build only until then |
| `server/lobby/mh4u_lobby.py` | speaks the web API Azahar's `web_service` and `network/announce_multiplayer_session` call; follows Azahar's nickname and room-list limits | stays as it is; the native room client (N4-01, provisional id) implements the same API |
| `server/tests/e2e_room.py` | drives `azahar-room` end to end | extended to the native room server (N4-04, provisional id) |
| `port/azahar/azahar.patch` | 1,387-line patch on Azahar (the 46 includes the grep found at `def93e9`, 50 now, are its own context lines): `Core::System` hooks (`SetCPUFactory`, `SetRunLoopHook`), `Kernel::SetSVCObserver`, `Layout::ScreenOverlay`/`SetTopScreenWidth`, `framebuffer_layout`, GPU/PICA changes, rooms (`network/room.cpp`), nwm/UDS, FS, savestate, LayeredFS, input_common | none: ROADMAP principle "Integration: nothing"; each piece is covered by the native module that replaces what it patches (kernel N1-05..07, GPU N2, rooms N4-01/N4-04 (provisional ids)); Azahar build only |

## File index

Every file found by the recount, with its concerns (section numbers above) and the milestone that removes the dependency. `D` = depends on Azahar only in the Azahar build.

| File | Sections | Removed by |
|---|---|---|
| `port/app/config.cpp` | 2, 7 | N0-20 |
| `port/app/config.h` | 2 | N0-20 |
| `port/app/dialog.cpp` | 7 | N0-24 |
| `port/app/input.cpp` | 2, 3, 7 | N0-24 |
| `port/app/launcher/checks.cpp` | 6 | N0-20 |
| `port/app/launcher/checks.h` | 6 | N0-20 |
| `port/app/launcher/launcher.cpp` | 1, 7 | N0-20 |
| `port/app/launcher/lobby.cpp` | 7 | N0-20 |
| `port/app/launcher/lobby.h` | 7 | N0-20 |
| `port/app/launcher/pick.cpp` | 7 | N0-20 |
| `port/app/launcher/process.cpp` | 7 | N0-20 |
| `port/app/launcher/screen_mods.cpp` | 7 | N0-20 |
| `port/app/launcher/screen_settings.cpp` | 7 | N0-20 |
| `port/app/launcher/tools.cpp` | 7 | N0-20 |
| `port/app/launcher/ui.cpp` | 7 | N0-20 |
| `port/app/main.cpp` | 1, 2, 3, 4, 5, 7, 8 | N0-20, N0-24 |
| `port/app/multiplayer.cpp` | 2, 5, 6, 7, 8 | N0-24 |
| `port/app/ui/hint.cpp` | 2, 3 | N0-24 |
| `port/app/ui/keyboard.cpp` | 7 | N0-24 |
| `port/app/ui/keyboard.h` | 7 (`Frontend::SoftwareKeyboard`, `Frontend::KeyboardConfig`, `swkbd.h`) | N0-24 |
| `port/app/ui/layer.h` | 1, 7 | N0-20, N0-24 |
| `port/app/ui/menu.cpp` | 2, 3, 4, 7 | N0-20, N0-18 |
| `port/app/ui/multiplayer_tab.cpp` | 5, 7 | N0-24 |
| `port/app/ui/render_gl.cpp` | 4, 7 | D |
| `port/app/ui/render_sw.cpp` | 4, 7 | D |
| `port/app/ui/render_vk.cpp` | 4 | D (N2b) |
| `port/app/ui/ui.cpp` | 3, 7 | N0-24 |
| `port/app/ui/ui.h` | 1, 7 | N0-20 |
| `port/app/window.cpp` | 3, 4, 7 | D (N2b) |
| `port/app/window.h` | 1, 4 | D (N2b) |
| `port/app/window_gl.cpp` | 2, 4, 7 | D (N2b) |
| `port/app/window_sw.cpp` | 2, 4, 7 | D (N2b) |
| `port/app/window_vk.cpp` | 4, 7 | D (N2b) |
| `port/azahar/azahar.patch` | 9 | D |
| `port/src/arm_recomp.cpp` | 1, 7 | N0-17, N1-04 |
| `port/src/arm_recomp.h` | 1 | N1-04 |
| `port/src/frontend.cpp` | 1, 6, 7 | N1-03 |
| `port/src/hooks.cpp` | 2, 4, 7 | N0-18 |
| `port/src/hooks.h` | 4 | N0-18 |
| `port/src/netsync.cpp` | 5, 7 | N0-18 |
| `port/src/netsync.h` | 1, 7 | N0-18 |
| `port/src/runner.cpp` | 1, 2, 3, 4, 5, 6, 7 | N1-09 (D until then) |

## Notes for the consumers

- **N0-18 (hooks):** the Azahar uses of `port/src/hooks.cpp`, `port/src/hooks.h` and `port/src/netsync.cpp` are exactly the rows tagged Platform above; `SetRefreshRate`, `IsPoweredOn`, `UpdateCurrentFramebufferLayout` and `GetGlobalTicks` map to `Platform::SetRefreshRate`, `IsRunning`, `RefreshScreenLayout` and `GameSeconds`.
- **N0-20 and N0-24:** the file lists of their allowlists match the files tagged IRuntime or util in the index; after N0-24 the only app files that still include Azahar are `port/app/runtime_azahar/**` (by design), `port/app/window.h`, `port/app/window*.cpp` and `port/app/ui/render_*.cpp`.
- **Every `LOG_*` call and `u8`..`u64` alias** is a trivial dependency; do it in one commit per file group to keep reviews short.
