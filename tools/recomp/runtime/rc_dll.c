/* Glue for building recompiled code as a shared library (.so/.dll/.dylib).
 *
 * Compile together with the generated sources, with RC_PAGED and hidden visibility. The
 * host loads the library, looks up rc_code_init and passes its callbacks; the generated
 * code's calls to rc_call(), rc_svc(), ... land here and go through that table. */
#include "recomp.h"

#if defined(_WIN32)
#define RC_EXPORT __declspec(dllexport)
#else
#define RC_EXPORT __attribute__((visibility("default")))
#endif

extern const RFuncEntry rc_func_table[];
extern const u32 rc_func_count, rc_text_base, rc_text_size, rc_text_crc32;

static RHost host;

void rc_call(RCtx *ctx, u32 target) { host.call(ctx, target); }
void rc_svc(RCtx *ctx, u32 imm, u32 next_pc) { host.svc(ctx, imm, next_pc); }
void rc_trap(RCtx *ctx, u32 pc, u32 kind) { host.trap(ctx, pc, kind); }
void rc_bad_return(RCtx *ctx, u32 got, u32 expected) { host.bad_return(ctx, got, expected); }
void rc_budget(RCtx *ctx) { host.budget(ctx); }
u32 rc_slow_read(RCtx *ctx, u32 a, u32 size) { return host.slow_read(ctx, a, size); }
void rc_slow_write(RCtx *ctx, u32 a, u32 v, u32 size) { host.slow_write(ctx, a, v, size); }

RC_EXPORT int rc_code_init(const RHost *h, RCodeInfo *info) {
    if (!h || h->version != RC_HOST_VERSION || h->ctx_size != sizeof(RCtx)) {
        return 0;
    }
    host = *h;
    info->table = rc_func_table;
    info->count = rc_func_count;
    info->text_base = rc_text_base;
    info->text_size = rc_text_size;
    info->text_crc32 = rc_text_crc32;
    return 1;
}
