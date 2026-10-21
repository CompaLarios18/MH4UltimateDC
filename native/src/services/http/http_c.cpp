// Derived from Azahar src/core/hle/service/http/http_c.cpp and src/core/hle/service/http/http_c.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the command table of http:C with its session limit, the request states and methods, the
// result codes (ErrCodes and the Error* constants), the session and context rules of every command
// (initialisation, the bound context, the checks before a request starts, post data, certificates) and
// their replies.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (http_c.cpp, http_c.h)
//
// http:C (N1-19): HTTP requests. Azahar makes the request on a worker thread with a library and the
// host's network. With nothing reachable the request fails when it connects; this service does the
// same at once: a started request is "completed" without a status line, headers or body, so
//  - the status code is 0xFFFFFFFF (httplib's default -1) and no header is found (0xD8A0A028);
//  - the response header block is " -1 \r\n\r\n" (the version and reason are empty);
//  - ReceiveData succeeds with nothing and moves the state to Completed; the download size is 0;
//  - the state of a started request reads Completed (Azahar's reads SendingRequest for as long as the
//    worker takes to fail).
// A call that waits for a response (the headers, the body) on a request that has not been started
// waits in game time: for the timeout of the *Timeout commands (answering "timeout"), otherwise until
// the request starts (NotifyFinishSendPostData) or the context is cancelled or closed.
// Post data is kept only as far as the checks of Azahar's commands need.
//
// Departures from Azahar:
//  - The default client certificate (ClCertA) is not available: Azahar decrypts it with a key of the
//    console, which the runtime does not have, so a context that selects it fails BeginRequest with
//    0xD8A0A066, the error Azahar returns in that case ("client cert not found").
//  - RegisterURLReplacement and UnregisterURLReplacement (0x0C00, 0x0C01) are Azahar's own commands
//    for a file of replacement rules; they are answered as unknown commands, and no request is made
//    that could use them.
//  - A context handle that no context has is answered with 0xD8A0A064 where Azahar asserts.
//  - The error answers that Azahar leaves with words it did not write (timeouts of the commands that
//    return buffers) give the empty static buffer or the mapped buffer of the request.
#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/ipc.h"
#include "mh4n/kernel.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

namespace mh4n::http {

namespace {

constexpr const char* kLog = "Service.HTTP";

constexpr u32 kModuleHttp = 40;
constexpr u32 kModuleSsl = 46;
constexpr u32 kSummaryInvalidState = 5;
constexpr u32 kSummaryInternal = 11;
constexpr u32 kSummaryWouldBlock = 2;
constexpr u32 kSummaryNothingHappened = 1;
constexpr u32 kSummaryInvalidArgument = 7;
constexpr u32 kLevelPermanent = 27;

constexpr Result kErrorStateError = MakeResult(102, kModuleHttp, kSummaryInvalidState, kLevelPermanent);
constexpr Result kErrorNotImplemented = MakeResult(1012, kModuleHttp, kSummaryInternal, kLevelPermanent);
constexpr Result kErrorTooManyClientCerts = MakeResult(203, kModuleHttp, kSummaryInvalidState, kLevelPermanent);
constexpr Result kErrorHeaderNotFound = MakeResult(40, kModuleHttp, kSummaryInvalidState, kLevelPermanent);
constexpr Result kErrorBufferSmall = MakeResult(43, kModuleHttp, kSummaryWouldBlock, kLevelPermanent);
constexpr Result kErrorWrongCertId = MakeResult(57, kModuleSsl, kSummaryInvalidArgument, kLevelPermanent);
constexpr Result kErrorWrongCertHandle = MakeResult(201, kModuleHttp, kSummaryInvalidState, kLevelPermanent);
constexpr Result kErrorCertAlreadySet = MakeResult(61, kModuleHttp, kSummaryInvalidState, kLevelPermanent);
constexpr Result kErrorIncompatibleAddPostData = MakeResult(50, kModuleHttp, kSummaryInvalidState, kLevelPermanent);
constexpr Result kErrorContextNotFound = MakeResult(100, kModuleHttp, kSummaryInvalidState, kLevelPermanent);
constexpr Result kErrorTimeout = MakeResult(105, kModuleHttp, kSummaryNothingHappened, kLevelPermanent);
constexpr Result kErrorTooManyContexts = MakeResult(26, kModuleHttp, kSummaryInvalidState, kLevelPermanent);
constexpr Result kErrorInvalidRequestMethod = MakeResult(32, kModuleHttp, kSummaryInvalidState, kLevelPermanent);
constexpr Result kErrorInvalidRequestState = MakeResult(22, kModuleHttp, kSummaryInvalidState, kLevelPermanent);
constexpr Result kErrorInvalidPostDataEncoding = MakeResult(53, kModuleHttp, kSummaryInvalidState, kLevelPermanent);
constexpr Result kErrorIncompatibleSendPostData = MakeResult(54, kModuleHttp, kSummaryInvalidState, kLevelPermanent);

constexpr std::size_t kMaxContexts = 8;
constexpr u32 kTotalRequestMethods = 8;
constexpr u32 kClientCertDefault = 0x40;

enum class RequestMethod : u8 { None = 0, Get, Post, Head, Put, Delete, PostEmpty, PutEmpty };
enum class RequestState : u8 {
    NotStarted = 0x1,
    ConnectingToServer = 0x5,
    SendingRequest = 0x6,
    ReceivingResponse = 0x7,
    ReceivingBody = 0x8,
    Received = 0x9,
    Completed = 0xA,
};
enum class PostDataEncoding : u8 { Auto = 0, AsciiForm = 1, MultipartForm = 2 };
enum class PostDataType : u8 { AsciiForm = 0, MultipartForm = 1, Raw = 2 };

struct ClientCert {
    u32 handle = 0;
    u32 session_id = 0;
    u8 cert_id = 0;
    std::size_t certificate_size = 0;
    std::size_t key_size = 0;
};

// A request that waits for the response of a context (see the overview).
struct Waiter {
    std::shared_ptr<Request> request;
    std::function<Reply(bool timed_out)> build;
    u64 timer = 0;
    bool done = false;
};

struct HttpContext {
    u32 handle = 0;
    u32 session_id = 0;
    std::string url;
    RequestMethod method = RequestMethod::None;
    RequestState state = RequestState::NotStarted;
    std::vector<std::pair<std::string, std::string>> headers;
    bool uses_default_client_cert = false;
    std::weak_ptr<ClientCert> client_cert;
    bool post_data_added = false;
    bool post_pending_request = false;
    std::multimap<std::string, bool> post_data;  // name -> is binary
    std::size_t post_data_raw_size = 0;
    PostDataEncoding post_data_encoding = PostDataEncoding::Auto;
    PostDataType post_data_type = PostDataType::AsciiForm;
    bool force_multipart = false;
    bool chunked_request = false;
    u32 chunked_content_length = 0;
    u32 ssl_options = 0;
    // the transfer
    bool cancelled = false;
    bool transfer_finished = false;
    std::size_t current_copied_data = 0;
    s32 response_status = -1;  // httplib::Response::status when nothing was received
    std::vector<std::shared_ptr<Waiter>> waiters;
};

struct HttpState {
    std::map<u32, std::shared_ptr<HttpContext>> contexts;
    u32 context_counter = 0;
    u32 session_counter = 0;
    std::map<u32, std::shared_ptr<ClientCert>> client_certs;
    u32 client_certs_counter = 0;
};

void FinishWaiter(Kernel& kernel, HttpContext& context, const std::shared_ptr<Waiter>& waiter, bool timed_out) {
    if (waiter->done) return;
    waiter->done = true;
    if (waiter->timer != 0) {
        kernel.Cancel(waiter->timer);
        waiter->timer = 0;
    }
    std::erase(context.waiters, waiter);
    mh4n::Complete(waiter->request, waiter->build(timed_out));
}

// The transfer is over (it failed) or the context was cancelled: whoever waits can go on.
void WakeWaiters(Kernel& kernel, HttpContext& context) {
    const std::vector<std::shared_ptr<Waiter>> all = context.waiters;
    for (const std::shared_ptr<Waiter>& waiter : all) {
        FinishWaiter(kernel, context, waiter, false);
    }
}

// MakeRequest with nothing reachable: the connection fails and the request completes.
void MakeRequest(Kernel& kernel, HttpContext& context) {
    context.state = RequestState::SendingRequest;
    MH4N_LOG(Error, kLog, "Request failed: Connection (nothing is reachable)");
    context.state = RequestState::Completed;
    context.transfer_finished = true;
    WakeWaiters(kernel, context);
}

void CancelContext(Kernel& kernel, HttpContext& context) {
    context.cancelled = true;
    WakeWaiters(kernel, context);
}

void PushBuffer(Reply& reply, const MappedBuffer* buffer) {
    if (buffer != nullptr) {
        reply.PushMappedBuffer(*buffer);
    } else {
        reply.Push<u32>(0);
        reply.Push<u32>(0);
    }
}

std::string BufferString(const StaticBuffer* buffer) {
    if (buffer == nullptr || buffer->data.empty()) return {};
    return std::string(buffer->data.begin(), buffer->data.end() - 1);  // without the \0 at the end
}

std::string MappedString(const MappedBuffer* buffer, u32 size) {
    std::string text(std::min<u32>(size, 0x100000), '\0');
    if (buffer != nullptr && !text.empty()) {
        buffer->Read(text.data(), 0, std::min<u32>(static_cast<u32>(text.size()), buffer->Size()));
    }
    return text;
}

class HttpC final : public Service {
public:
    explicit HttpC(ServiceContext& context)
        : Service(context, "http:C"), st_(context.State<HttpState>("http:C")) {
        Register(0x0001, "Initialize", &HttpC::Initialize);
        Register(0x0002, "CreateContext", &HttpC::CreateContext);
        Register(0x0003, "CloseContext", &HttpC::CloseContext);
        Register(0x0004, "CancelConnection", &HttpC::CancelConnection);
        Register(0x0005, "GetRequestState", &HttpC::GetRequestState);
        Register(0x0006, "GetDownloadSizeState", &HttpC::GetDownloadSizeState);
        Register(0x0007, "GetRequestError");
        Register(0x0008, "InitializeConnectionSession", &HttpC::InitializeConnectionSession);
        Register(0x0009, "BeginRequest", &HttpC::BeginRequest);
        Register(0x000A, "BeginRequestAsync", &HttpC::BeginRequest);
        Register(0x000B, "ReceiveData", &HttpC::ReceiveData);
        Register(0x000C, "ReceiveDataTimeout", &HttpC::ReceiveDataTimeout);
        Register(0x000D, "SetProxy");
        Register(0x000E, "SetProxyDefault", &HttpC::SetProxyDefault);
        Register(0x000F, "SetBasicAuthorization");
        Register(0x0010, "SetSocketBufferSize");
        Register(0x0011, "AddRequestHeader", &HttpC::AddRequestHeader);
        Register(0x0012, "AddPostDataAscii", &HttpC::AddPostDataAscii);
        Register(0x0013, "AddPostDataBinary", &HttpC::AddPostDataBinary);
        Register(0x0014, "AddPostDataRaw", &HttpC::AddPostDataRaw);
        Register(0x0015, "SetPostDataType", &HttpC::SetPostDataType);
        Register(0x0016, "SendPostDataAscii", &HttpC::SendPostDataAscii);
        Register(0x0017, "SendPostDataAsciiTimeout", &HttpC::SendPostDataAsciiTimeout);
        Register(0x0018, "SendPostDataBinary", &HttpC::SendPostDataBinary);
        Register(0x0019, "SendPostDataBinaryTimeout", &HttpC::SendPostDataBinaryTimeout);
        Register(0x001A, "SendPostDataRaw", &HttpC::SendPostDataRaw);
        Register(0x001B, "SendPostDataRawTimeout", &HttpC::SendPostDataRawTimeout);
        Register(0x001C, "SetPostDataEncoding", &HttpC::SetPostDataEncoding);
        Register(0x001D, "NotifyFinishSendPostData", &HttpC::NotifyFinishSendPostData);
        Register(0x001E, "GetResponseHeader", &HttpC::GetResponseHeader);
        Register(0x001F, "GetResponseHeaderTimeout", &HttpC::GetResponseHeaderTimeout);
        Register(0x0020, "GetResponseData", &HttpC::GetResponseData);
        Register(0x0021, "GetResponseDataTimeout", &HttpC::GetResponseDataTimeout);
        Register(0x0022, "GetResponseStatusCode", &HttpC::GetResponseStatusCode);
        Register(0x0023, "GetResponseStatusCodeTimeout", &HttpC::GetResponseStatusCodeTimeout);
        Register(0x0024, "AddTrustedRootCA", &HttpC::AddTrustedRootCA);
        Register(0x0025, "AddDefaultCert", &HttpC::AddDefaultCert);
        Register(0x0026, "SelectRootCertChain");
        Register(0x0027, "SetClientCert");
        Register(0x0028, "SetDefaultClientCert", &HttpC::SetDefaultClientCert);
        Register(0x0029, "SetClientCertContext", &HttpC::SetClientCertContext);
        Register(0x002A, "GetSSLError", &HttpC::GetSSLError);
        Register(0x002B, "SetSSLOpt", &HttpC::SetSSLOpt);
        Register(0x002C, "SetSSLClearOpt");
        Register(0x002D, "CreateRootCertChain");
        Register(0x002E, "DestroyRootCertChain");
        Register(0x002F, "RootCertChainAddCert");
        Register(0x0030, "RootCertChainAddDefaultCert");
        Register(0x0031, "RootCertChainRemoveCert");
        Register(0x0032, "OpenClientCertContext", &HttpC::OpenClientCertContext);
        Register(0x0033, "OpenDefaultClientCertContext", &HttpC::OpenDefaultClientCertContext);
        Register(0x0034, "CloseClientCertContext", &HttpC::CloseClientCertContext);
        Register(0x0035, "SetDefaultProxy");
        Register(0x0036, "ClearDNSCache");
        Register(0x0037, "SetKeepAlive", &HttpC::SetKeepAlive);
        Register(0x0038, "SetPostDataTypeSize", &HttpC::SetPostDataTypeSize);
        Register(0x0039, "Finalize", &HttpC::Finalize);
        Register(0x0C00, "RegisterURLReplacement");
        Register(0x0C01, "UnregisterURLReplacement");
    }

    Handled Handle(Request& request) override {
        deferred_ = false;
        const Handled handled = Service::Handle(request);
        if (handled == Handled::Done && deferred_) {
            deferred_ = false;
            return Handled::Deferred;
        }
        return handled;
    }

private:
    Kernel& GetKernel() { return Context().GetKernel(); }

    static void Error(Request& request, Result result, bool with_buffer = false,
                      const MappedBuffer* buffer = nullptr) {
        if (with_buffer) {
            Reply& reply = request.MakeReply(1, 2);
            reply.Push(result);
            PushBuffer(reply, buffer);
        } else {
            request.MakeReply(1, 0).Push(result);
        }
    }

    // EnsureSessionInitialized: false after replying with the error.
    bool EnsureInitialized(Request& request) {
        if (!initialized_) {
            MH4N_LOG(Error, kLog, "Tried to make a request on an uninitialized session");
            Error(request, kErrorStateError);
            return false;
        }
        return true;
    }

    // PerformStateChecks: the context of the request, or null after replying with the error.
    std::shared_ptr<HttpContext> CheckedContext(Request& request, u32 handle) {
        if (!EnsureInitialized(request)) return nullptr;
        // This command can only be called with a bound context
        if (!current_http_context_) {
            MH4N_LOG(Error, kLog, "Tried to make a request without a bound context");
            Error(request, kErrorNotImplemented);
            return nullptr;
        }
        if (*current_http_context_ != handle) {
            MH4N_LOG(Error, kLog, "Tried to make a request on a mismatched session input context={} session context={}",
                     handle, *current_http_context_);
            Error(request, kErrorStateError);
            return nullptr;
        }
        return Find(request, handle);
    }

    std::shared_ptr<HttpContext> Find(Request& request, u32 handle) {
        const auto it = st_->contexts.find(handle);
        if (it == st_->contexts.end()) {
            Error(request, kErrorContextNotFound);  // Azahar asserts
            return nullptr;
        }
        return it->second;
    }

    // ---- session and contexts ----

    // 0x00010044: shared memory size, the calling PID, the shared memory. Reply: result.
    void Initialize(Request& request) {
        const u32 shmem_size = request.Pop<u32>();
        const u32 pid = request.PopPid();
        request.PopObject<SharedMemory>();
        MH4N_LOG(Debug, kLog, "called, shared memory size: {} pid: {}", shmem_size, pid);
        if (initialized_) {
            MH4N_LOG(Error, kLog, "Tried to initialize an already initialized session");
            Error(request, kErrorStateError);
            return;
        }
        initialized_ = true;
        session_id_ = ++st_->session_counter;
        // Azahar returns 0xD8A0A046 when no network connection is available but assumes one.
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // 0x00080042: the context, the calling PID. Binds the session to the context.
    void InitializeConnectionSession(Request& request) {
        const u32 handle = request.Pop<u32>();
        request.PopPid();
        if (initialized_) {
            Error(request, kErrorStateError);
            return;
        }
        if (!st_->contexts.contains(handle)) {
            Error(request, kErrorContextNotFound);
            return;
        }
        initialized_ = true;
        session_id_ = ++st_->session_counter;
        current_http_context_ = handle;
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // 0x00020082: URL size, method, the URL (mapped). Reply: result, the context, the buffer.
    void CreateContext(Request& request) {
        const u32 url_size = request.Pop<u32>();
        const u8 method = request.Pop<u8>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        const std::string url = MappedString(buffer, url_size == 0 ? 0 : url_size - 1);
        MH4N_LOG(Debug, kLog, "called, url_size={}, url={}, method={}", url_size, url, method);
        if (!EnsureInitialized(request)) return;
        if (current_http_context_) {
            MH4N_LOG(Error, kLog, "Command called with a bound context");
            Error(request, kErrorNotImplemented, true, buffer);
            return;
        }
        if (num_http_contexts_ >= kMaxContexts) {
            MH4N_LOG(Error, kLog, "Tried to open too many HTTP contexts");
            Error(request, kErrorTooManyContexts, true, buffer);
            return;
        }
        if (method == static_cast<u8>(RequestMethod::None) || method >= kTotalRequestMethods) {
            MH4N_LOG(Error, kLog, "invalid request method={}", method);
            Error(request, kErrorInvalidRequestMethod, true, buffer);
            return;
        }
        auto context = std::make_shared<HttpContext>();
        context->handle = ++st_->context_counter;
        context->session_id = session_id_;
        context->url = url;
        context->method = static_cast<RequestMethod>(method);
        st_->contexts[context->handle] = context;
        ++num_http_contexts_;
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push(context->handle);
        PushBuffer(reply, buffer);
    }

    // 0x00030040: the context. Closing a context that does not exist succeeds.
    void CloseContext(Request& request) {
        const u32 handle = request.Pop<u32>();
        MH4N_LOG(Debug, kLog, "called, handle={}", handle);
        if (!EnsureInitialized(request)) return;
        if (current_http_context_) {
            MH4N_LOG(Error, kLog, "Unimplemented CloseContext on context-bound session");
            Error(request, kErrorNotImplemented);
            return;
        }
        const auto it = st_->contexts.find(handle);
        if (it == st_->contexts.end()) {
            MH4N_LOG(Error, kLog, "called, context {} not found", handle);
            request.MakeReply(1, 0).Push(kResultSuccess);
            return;
        }
        const std::shared_ptr<HttpContext> context = it->second;
        st_->contexts.erase(it);
        --num_http_contexts_;
        CancelContext(GetKernel(), *context);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void CancelConnection(Request& request) {
        const u32 handle = request.Pop<u32>();
        if (!EnsureInitialized(request)) return;
        const std::shared_ptr<HttpContext> context = Find(request, handle);
        if (context == nullptr) return;
        CancelContext(GetKernel(), *context);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void GetRequestState(Request& request) {
        const u32 handle = request.Pop<u32>();
        if (!EnsureInitialized(request)) return;
        const std::shared_ptr<HttpContext> context = Find(request, handle);
        if (context == nullptr) return;
        RequestState state = context->state;
        // While POST data is pending the request reads as being sent
        if (state == RequestState::NotStarted && context->post_pending_request) {
            state = RequestState::SendingRequest;
        }
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u8>(static_cast<u8>(state));
    }

    // 0x00060040: the context. Reply: result, the bytes copied, the content length.
    void GetDownloadSizeState(Request& request) {
        const u32 handle = request.Pop<u32>();
        if (!EnsureInitialized(request)) return;
        const std::shared_ptr<HttpContext> context = Find(request, handle);
        if (context == nullptr) return;
        Reply& reply = request.MakeReply(3, 0);
        reply.Push(kResultSuccess);
        reply.Push(static_cast<u32>(context->current_copied_data));
        reply.Push<u32>(0);  // no response, no Content-Length
    }

    // ---- starting a request ----

    // BeginRequest and BeginRequestAsync: always success, the request runs when it has not started.
    void BeginRequest(Request& request) {
        const u32 handle = request.Pop<u32>();
        const std::shared_ptr<HttpContext> context = CheckedContext(request, handle);
        if (context == nullptr) return;
        // The default client certificate is decrypted with a key the runtime does not have
        if (context->uses_default_client_cert) {
            MH4N_LOG(Error, kLog, "Failed to begin HTTP request: client cert not found.");
            Error(request, kErrorStateError);
            return;
        }
        if (context->state == RequestState::NotStarted) {
            if (context->method == RequestMethod::Post && !context->post_data_added) {
                context->post_pending_request = true;
            } else {
                context->current_copied_data = 0;
                MakeRequest(GetKernel(), *context);
            }
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void NotifyFinishSendPostData(Request& request) {
        const u32 handle = request.Pop<u32>();
        const std::shared_ptr<HttpContext> context = CheckedContext(request, handle);
        if (context == nullptr) return;
        if (context->state != RequestState::NotStarted || !context->post_pending_request ||
            !context->post_data_added) {
            MH4N_LOG(Error, kLog, "Tried to notify finish Post on a context in the wrong state");
            Error(request, kErrorInvalidRequestState);
            return;
        }
        context->current_copied_data = 0;
        MakeRequest(GetKernel(), *context);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // ---- waiting for the response ----

    // Answers now if the response is there, otherwise parks the request until the transfer ends, the
    // context is cancelled, or the timeout (game time) passes.
    void WaitOrAnswer(Request& request, const std::shared_ptr<HttpContext>& context,
                      std::optional<u64> timeout_ns, std::function<Reply(bool timed_out)> build) {
        const bool ready = context->cancelled || context->transfer_finished;
        if (ready) {
            Reply reply = build(false);
            const ipc::Header header{reply.HeaderWord()};
            request.MakeReply(header.NormalParamsSize(), header.TranslateParamsSize()) = std::move(reply);
            return;
        }
        auto waiter = std::make_shared<Waiter>();
        waiter->request = request.Share();
        waiter->build = std::move(build);
        context->waiters.push_back(waiter);
        if (timeout_ns) {
            Kernel& kernel = GetKernel();
            const u64 ticks = NsToTicks(*timeout_ns);
            const u64 now = kernel.NowTicks();
            if (ticks < ~u64{0} - now) {
                Kernel* kernel_ptr = &kernel;
                const std::weak_ptr<Waiter> weak = waiter;
                const std::shared_ptr<HttpContext> held = context;
                waiter->timer = kernel.ScheduleAt(now + ticks, [kernel_ptr, weak, held] {
                    if (const auto w = weak.lock()) {
                        w->timer = 0;
                        FinishWaiter(*kernel_ptr, *held, w, true);
                    }
                });
            }
        }
        deferred_ = true;
    }

    static Reply TimeoutReply(const Request& request, u32 normal, u32 translate) {
        Reply reply(request.CommandId(), normal, translate);
        reply.Push(kErrorTimeout);
        return reply;
    }

    // ReceiveData and ReceiveDataTimeout: the context, the buffer size, [the timeout], the buffer.
    void ReceiveDataImpl(Request& request, bool timeout) {
        const u32 handle = request.Pop<u32>();
        const u32 buffer_size = request.Pop<u32>();
        const std::optional<u64> timeout_ns = timeout ? std::optional(request.Pop<u64>()) : std::nullopt;
        request.PopMappedBuffer();
        const std::shared_ptr<HttpContext> context = CheckedContext(request, handle);
        if (context == nullptr) return;
        const std::shared_ptr<Request> shared = request.Share();
        WaitOrAnswer(request, context, timeout_ns, [context, shared, buffer_size](bool timed_out) {
            if (timed_out) return TimeoutReply(*shared, 1, 0);
            Reply reply(shared->CommandId(), 1, 0);
            // nothing is received: the transfer is over (or cancelled), so the body is complete
            (void)buffer_size;
            const bool completed = context->cancelled || context->transfer_finished;
            if (completed) context->state = RequestState::Completed;
            reply.Push(completed ? kResultSuccess : kErrorBufferSmall);
            return reply;
        });
    }
    void ReceiveData(Request& request) { ReceiveDataImpl(request, false); }
    void ReceiveDataTimeout(Request& request) { ReceiveDataImpl(request, true); }

    // GetResponseStatusCode(Timeout): the context, [the timeout]. Reply: result, the status code.
    void GetResponseStatusCodeImpl(Request& request, bool timeout) {
        const u32 handle = request.Pop<u32>();
        const std::optional<u64> timeout_ns = timeout ? std::optional(request.Pop<u64>()) : std::nullopt;
        const std::shared_ptr<HttpContext> context = CheckedContext(request, handle);
        if (context == nullptr) return;
        const std::shared_ptr<Request> shared = request.Share();
        WaitOrAnswer(request, context, timeout_ns, [context, shared](bool timed_out) {
            if (timed_out) return TimeoutReply(*shared, 1, 0);
            Reply reply(shared->CommandId(), 2, 0);
            reply.Push(kResultSuccess);
            reply.Push(static_cast<u32>(context->response_status));
            return reply;
        });
    }
    void GetResponseStatusCode(Request& request) { GetResponseStatusCodeImpl(request, false); }
    void GetResponseStatusCodeTimeout(Request& request) { GetResponseStatusCodeImpl(request, true); }

    // GetResponseData(Timeout): the context, the buffer size, [the timeout], the buffer. Reply: result,
    // the bytes written (the reconstructed header block: " -1 \r\n\r\n" when nothing was received).
    void GetResponseDataImpl(Request& request, bool timeout) {
        const u32 handle = request.Pop<u32>();
        request.Pop<u32>();
        const std::optional<u64> timeout_ns = timeout ? std::optional(request.Pop<u64>()) : std::nullopt;
        const MappedBuffer* buffer = request.PopMappedBuffer();
        const std::shared_ptr<HttpContext> context = CheckedContext(request, handle);
        if (context == nullptr) return;
        const std::shared_ptr<Request> shared = request.Share();
        WaitOrAnswer(request, context, timeout_ns, [context, shared, buffer](bool timed_out) {
            if (timed_out) {
                Reply reply(shared->CommandId(), 2, 0);
                reply.Push(kErrorTimeout);
                reply.Push(shared->Words().size() > 2 ? shared->Words()[2] : 0u);  // not written by Azahar
                return reply;
            }
            const std::string block = fmt::format("{} {} {}\r\n", "", context->response_status, "") + "\r\n";
            const std::size_t write_size = std::min<std::size_t>(block.size(), buffer != nullptr ? buffer->Size() : 0);
            if (buffer != nullptr && write_size != 0) {
                buffer->Write(block.data(), 0, static_cast<u32>(write_size));
            }
            Reply reply(shared->CommandId(), 2, 0);
            reply.Push(kResultSuccess);
            reply.Push(static_cast<u32>(write_size));
            return reply;
        });
    }
    void GetResponseData(Request& request) { GetResponseDataImpl(request, false); }
    void GetResponseDataTimeout(Request& request) { GetResponseDataImpl(request, true); }

    // GetResponseHeader(Timeout): the context, the name length, the value's size, [the timeout], the
    // name (static buffer), the value (mapped). Reply: result, the length, the value.
    void GetResponseHeaderImpl(Request& request, bool timeout) {
        const u32 handle = request.Pop<u32>();
        request.Pop<u32>();
        request.Pop<u32>();
        const std::optional<u64> timeout_ns = timeout ? std::optional(request.Pop<u64>()) : std::nullopt;
        const StaticBuffer* name = request.PopStaticBuffer();
        const MappedBuffer* value = request.PopMappedBuffer();
        const std::shared_ptr<HttpContext> context = CheckedContext(request, handle);
        if (context == nullptr) return;
        const std::shared_ptr<Request> shared = request.Share();
        const u32 name_id = name != nullptr ? name->id : 0;
        WaitOrAnswer(request, context, timeout_ns, [shared, value, name_id](bool timed_out) {
            Reply reply(shared->CommandId(), 2, 2);
            if (timed_out) {
                reply.Push(kErrorTimeout);
                reply.Push(shared->Words().size() > 2 ? shared->Words()[2] : 0u);  // not written by Azahar
                reply.PushStaticBuffer({}, static_cast<u8>(name_id));
                return reply;
            }
            // the response has no headers
            reply.Push(kErrorHeaderNotFound);
            reply.Push<s32>(0);
            PushBuffer(reply, value);
            return reply;
        });
    }
    void GetResponseHeader(Request& request) { GetResponseHeaderImpl(request, false); }
    void GetResponseHeaderTimeout(Request& request) { GetResponseHeaderImpl(request, true); }

    // ---- request setup ----

    void SetProxyDefault(Request& request) {
        request.Pop<u32>();
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    // 0x001100C4: the context, the name length, the value's size, the name (static), the value (mapped).
    void AddRequestHeader(Request& request) {
        const u32 handle = request.Pop<u32>();
        request.Pop<u32>();
        const u32 value_size = request.Pop<u32>();
        const StaticBuffer* name_buffer = request.PopStaticBuffer();
        const MappedBuffer* value_buffer = request.PopMappedBuffer();
        const std::string name = BufferString(name_buffer);
        const std::string value = MappedString(value_buffer, value_size == 0 ? 0 : value_size - 1);
        const std::shared_ptr<HttpContext> context = CheckedContext(request, handle);
        if (context == nullptr) return;
        if (context->state != RequestState::NotStarted) {
            Error(request, kErrorInvalidRequestState, true, value_buffer);
            return;
        }
        context->headers.emplace_back(name, value);
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, value_buffer);
    }

    // AddPostDataAscii and AddPostDataBinary: the same checks, the binary one forces multipart.
    void AddPostDataNamed(Request& request, bool binary) {
        const u32 handle = request.Pop<u32>();
        request.Pop<u32>();
        const u32 value_size = request.Pop<u32>();
        const StaticBuffer* name_buffer = request.PopStaticBuffer();
        const MappedBuffer* value_buffer = request.PopMappedBuffer();
        const std::string name = BufferString(name_buffer);
        (void)value_size;
        const std::shared_ptr<HttpContext> context = CheckedContext(request, handle);
        if (context == nullptr) return;
        if (context->state != RequestState::NotStarted) {
            Error(request, kErrorInvalidRequestState, true, value_buffer);
            return;
        }
        if (context->post_data_raw_size != 0 || context->chunked_request) {
            Error(request, kErrorIncompatibleAddPostData, true, value_buffer);
            return;
        }
        context->post_data.emplace(name, binary);
        if (binary) context->force_multipart = true;
        context->post_data_added = true;
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, value_buffer);
    }
    void AddPostDataAscii(Request& request) { AddPostDataNamed(request, false); }
    void AddPostDataBinary(Request& request) { AddPostDataNamed(request, true); }

    // 0x00140082: the context, the length, the data (mapped).
    void AddPostDataRaw(Request& request) {
        const u32 handle = request.Pop<u32>();
        request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        const std::shared_ptr<HttpContext> context = CheckedContext(request, handle);
        if (context == nullptr) return;
        if (context->state != RequestState::NotStarted) {
            Error(request, kErrorInvalidRequestState, true, buffer);
            return;
        }
        if (!context->post_data.empty() || context->chunked_request) {
            Error(request, kErrorIncompatibleAddPostData, true, buffer);
            return;
        }
        context->post_data_raw_size = buffer != nullptr ? buffer->Size() : 0;
        context->post_data_added = true;
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer);
    }

    // SetPostDataType and SetPostDataTypeSize: the context, the type [, the size].
    void SetPostDataTypeImpl(Request& request, bool with_size) {
        const u32 handle = request.Pop<u32>();
        const u8 type = request.Pop<u8>();
        const u32 size = with_size ? request.Pop<u32>() : 0;
        const std::shared_ptr<HttpContext> context = CheckedContext(request, handle);
        if (context == nullptr) return;
        if (context->state != RequestState::NotStarted) {
            Error(request, kErrorInvalidRequestState);
            return;
        }
        if (!context->post_data.empty() || context->post_data_raw_size != 0) {
            Error(request, kErrorIncompatibleSendPostData);
            return;
        }
        context->post_data_type = type <= 2 ? static_cast<PostDataType>(type) : PostDataType::AsciiForm;
        context->chunked_request = true;
        if (with_size) context->chunked_content_length = size;
        request.MakeReply(1, 0).Push(kResultSuccess);
    }
    void SetPostDataType(Request& request) { SetPostDataTypeImpl(request, false); }
    void SetPostDataTypeSize(Request& request) { SetPostDataTypeImpl(request, true); }

    // SendPostDataAscii[Timeout] / Binary[Timeout]: the context, name length, value size, [timeout],
    // name (static), value (mapped). The ASCII one needs a context not started, the binary one a context
    // that has started; both need chunked mode.
    void SendPostDataNamed(Request& request, bool binary, bool timeout) {
        const u32 handle = request.Pop<u32>();
        request.Pop<u32>();
        request.Pop<u32>();
        if (timeout) request.Pop<u64>();
        const StaticBuffer* name_buffer = request.PopStaticBuffer();
        const MappedBuffer* value_buffer = request.PopMappedBuffer();
        const std::string name = BufferString(name_buffer);
        const std::shared_ptr<HttpContext> context = CheckedContext(request, handle);
        if (context == nullptr) return;
        const bool wrong_state = binary ? context->state == RequestState::NotStarted
                                        : context->state != RequestState::NotStarted;
        if (wrong_state || !context->chunked_request) {
            Error(request, kErrorInvalidRequestState, true, value_buffer);
            return;
        }
        context->post_data.emplace(name, binary);
        context->post_data_added = true;
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, value_buffer);
    }
    void SendPostDataAscii(Request& r) { SendPostDataNamed(r, false, false); }
    void SendPostDataAsciiTimeout(Request& r) { SendPostDataNamed(r, false, true); }
    void SendPostDataBinary(Request& r) { SendPostDataNamed(r, true, false); }
    void SendPostDataBinaryTimeout(Request& r) { SendPostDataNamed(r, true, true); }

    // SendPostDataRaw[Timeout]: the context, the length, [timeout], the data (mapped).
    void SendPostDataRawImpl(Request& request, bool timeout) {
        const u32 handle = request.Pop<u32>();
        request.Pop<u32>();
        if (timeout) request.Pop<u64>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        const std::shared_ptr<HttpContext> context = CheckedContext(request, handle);
        if (context == nullptr) return;
        if (context->state != RequestState::NotStarted || !context->chunked_request) {
            Error(request, kErrorInvalidRequestState, true, buffer);
            return;
        }
        if (context->post_data_type != PostDataType::Raw) {
            Error(request, kErrorIncompatibleSendPostData, true, buffer);
            return;
        }
        context->post_data.emplace(std::to_string(context->post_data.size()), true);
        context->post_data_added = true;
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer);
    }
    void SendPostDataRaw(Request& r) { SendPostDataRawImpl(r, false); }
    void SendPostDataRawTimeout(Request& r) { SendPostDataRawImpl(r, true); }

    void SetPostDataEncoding(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u8 encoding = request.Pop<u8>();
        const std::shared_ptr<HttpContext> context = CheckedContext(request, handle);
        if (context == nullptr) return;
        if (context->state != RequestState::NotStarted) {
            Error(request, kErrorInvalidRequestState);
            return;
        }
        switch (encoding) {
        case static_cast<u8>(PostDataEncoding::Auto):
            context->post_data_encoding = PostDataEncoding::Auto;
            break;
        case static_cast<u8>(PostDataEncoding::AsciiForm):
        case static_cast<u8>(PostDataEncoding::MultipartForm):
            if (context->post_data_raw_size != 0) {
                Error(request, kErrorIncompatibleAddPostData);
                return;
            }
            context->post_data_encoding = static_cast<PostDataEncoding>(encoding);
            break;
        default:
            Error(request, kErrorInvalidPostDataEncoding);
            return;
        }
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // ---- certificates ----

    void AddTrustedRootCA(Request& request) {
        request.Pop<u32>();
        request.Pop<u32>();
        const MappedBuffer* buffer = request.PopMappedBuffer();
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        PushBuffer(reply, buffer);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }
    void AddDefaultCert(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 certificate_id = request.Pop<u32>();
        MH4N_LOG(Warning, kLog, "(STUBBED) called, handle={}, certificate_id={}", handle, certificate_id);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void SetDefaultClientCert(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 client_cert_id = request.Pop<u32>();
        const std::shared_ptr<HttpContext> context = CheckedContext(request, handle);
        if (context == nullptr) return;
        if (client_cert_id != kClientCertDefault) {
            Error(request, kErrorWrongCertId);
            return;
        }
        context->uses_default_client_cert = true;
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void SetClientCertContext(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 cert_handle = request.Pop<u32>();
        const std::shared_ptr<HttpContext> context = CheckedContext(request, handle);
        if (context == nullptr) return;
        const auto cert = st_->client_certs.find(cert_handle);
        if (cert == st_->client_certs.end()) {
            Error(request, kErrorWrongCertHandle);
            return;
        }
        if (context->client_cert.lock()) {
            Error(request, kErrorCertAlreadySet);
            return;
        }
        if (context->state != RequestState::NotStarted) {
            Error(request, kErrorInvalidRequestState);
            return;
        }
        context->client_cert = cert->second;
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // 0x002A0040: the context. No error: the SSL context is only made when the request is.
    void GetSSLError(Request& request) {
        const u32 handle = request.Pop<u32>();
        const std::shared_ptr<HttpContext> context = Find(request, handle);
        if (context == nullptr) return;
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push<u32>(0);
        MH4N_LOG(Warning, kLog, "(STUBBED) called, context_handle={}", handle);
    }

    void SetSSLOpt(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 options = request.Pop<u32>();
        MH4N_LOG(Warning, kLog, "(STUBBED) called, context_handle={}, opts={}", handle, options);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    // 0x003200C4...: certificate size, key size, the certificate and the key (mapped).
    void OpenClientCertContext(Request& request) {
        const u32 cert_size = request.Pop<u32>();
        const u32 key_size = request.Pop<u32>();
        const MappedBuffer* cert = request.PopMappedBuffer();
        const MappedBuffer* key = request.PopMappedBuffer();
        Result result = kResultSuccess;
        if (!initialized_) {
            result = kErrorStateError;
        } else if (current_http_context_) {
            result = kErrorNotImplemented;
        } else if (num_client_certs_ >= 2) {
            result = kErrorTooManyClientCerts;
        } else {
            auto made = std::make_shared<ClientCert>();
            made->handle = ++st_->client_certs_counter;
            made->certificate_size = cert_size;
            made->key_size = key_size;
            made->session_id = session_id_;
            st_->client_certs[made->handle] = made;
            ++num_client_certs_;
        }
        Reply& reply = request.MakeReply(1, 4);
        reply.Push(result);
        PushBuffer(reply, cert);
        PushBuffer(reply, key);
    }

    // 0x00330040: the certificate id (a byte). Reply: result, the certificate handle.
    void OpenDefaultClientCertContext(Request& request) {
        const u8 cert_id = request.Pop<u8>();
        if (!EnsureInitialized(request)) return;
        if (current_http_context_) {
            Error(request, kErrorNotImplemented);
            return;
        }
        if (num_client_certs_ >= 2) {
            Error(request, kErrorTooManyClientCerts);
            return;
        }
        if (cert_id != kClientCertDefault) {
            Error(request, kErrorWrongCertId);
            return;
        }
        for (const auto& [handle, cert] : st_->client_certs) {
            if (cert->cert_id == kClientCertDefault && cert->session_id == session_id_) {
                Reply& reply = request.MakeReply(2, 0);
                reply.Push(kResultSuccess);
                reply.Push(handle);
                return;
            }
        }
        auto made = std::make_shared<ClientCert>();
        made->handle = ++st_->client_certs_counter;
        made->session_id = session_id_;
        // Azahar keeps cert_id 0 on the context it makes here, so the same call makes another one
        st_->client_certs[made->handle] = made;
        ++num_client_certs_;
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(made->handle);
    }

    void CloseClientCertContext(Request& request) {
        const u32 handle = request.Pop<u32>();
        const auto it = st_->client_certs.find(handle);
        if (it == st_->client_certs.end() || it->second->session_id != session_id_) {
            request.MakeReply(1, 0).Push(kResultSuccess);  // silently succeeds
            return;
        }
        st_->client_certs.erase(it);
        --num_client_certs_;
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void SetKeepAlive(Request& request) {
        const u32 handle = request.Pop<u32>();
        const u32 option = request.Pop<u32>();
        MH4N_LOG(Warning, kLog, "(STUBBED) called, handle={}, option={}", handle, option);
        request.MakeReply(1, 0).Push(kResultSuccess);
    }

    void Finalize(Request& request) {
        request.MakeReply(1, 0).Push(kResultSuccess);
        MH4N_LOG(Warning, kLog, "(STUBBED) called");
    }

    std::shared_ptr<HttpState> st_;
    // SessionData
    bool initialized_ = false;
    u32 session_id_ = 0;
    std::optional<u32> current_http_context_;
    u32 num_http_contexts_ = 0;
    u32 num_client_certs_ = 0;
    bool deferred_ = false;
};

MH4N_REGISTER(ServiceFactory, "http:C",
              ServiceFactory([](ServiceContext& context) { return std::make_unique<HttpC>(context); },
                             32));

}  // namespace

}  // namespace mh4n::http
