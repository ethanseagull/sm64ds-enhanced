// CPU exception handler: on a data/prefetch abort or undefined instruction,
// log the exception and registers so the SD log shows the faulting PC and the
// caller (LR). Look them up with
//   arm-none-eabi-addr2line -f -C -e build3ds/<name>.elf <pc> <lr>
#include "crash_handler.h"

#include "console_tee.h"

#include <3ds.h>
#include <malloc.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <new>

// These weak references don't exist in the small smoke programs. The actor
// walker already records its current node, so this costs nothing per frame.
extern "C" {
extern void *data_020a4b68 __attribute__((weak));
extern unsigned data_02099f24 __attribute__((weak));
extern void *data_0209f394[] __attribute__((weak));
extern signed char data_0209f2f8 __attribute__((weak));
}
extern bool card3ds_screenshot(const char *, bool) __attribute__((weak));
extern "C" void game3ds_save_flush(void) __attribute__((weak));
extern "C" u32 __ctru_heap_size, __ctru_linear_heap_size;
extern "C" char *fake_heap_start, *fake_heap_end;

namespace {

alignas(8) u8 g_exc_stack[0x2000];

// Also write the report to its own file (sdmc:/sm64ds3ds/game3ds_crash.log),
// since game3ds.log is overwritten on the next run.
FILE *g_crash_file;

void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void report(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fputs(line, stdout);
    if (g_crash_file) fputs(line, g_crash_file);
}

const char *kind_name(ERRF_ExceptionType t)
{
    switch (t) {
    case ERRF_EXCEPTION_PREFETCH_ABORT: return "prefetch abort";
    case ERRF_EXCEPTION_DATA_ABORT: return "data abort";
    case ERRF_EXCEPTION_UNDEFINED: return "undefined instruction";
    case ERRF_EXCEPTION_VFP: return "VFP exception";
    default: return "exception";
    }
}

bool heap_pointer(const void *p, unsigned bytes)
{
    const unsigned at = (unsigned)p;
    return !(at & 3) && at >= 0x08000000 && at <= 0x0E000000 - bytes;
}

void actor_context()
{
    if (!&data_020a4b68) return;
    void *node = data_020a4b68;
    const unsigned phase = &data_02099f24 ? data_02099f24 : 0;
    report("  actor-context: phase %lu node %p level %d player %p\n",
           (unsigned long)phase, node,
           &data_0209f2f8 ? (int)data_0209f2f8 : -1,
           data_0209f394 ? data_0209f394[0] : nullptr);
    if (!heap_pointer(node, 12)) return;
    void *actor = ((void **)node)[2];
    if (!heap_pointer(actor, 0x68)) return;
    const unsigned *words = (const unsigned *)actor;
    report("  current-actor: %p vptr %08lx unique %lu id %lu param %08lx "
           "pos (%ld,%ld,%ld)\n", actor, (unsigned long)words[0],
           (unsigned long)words[1], (unsigned long)*(const unsigned short *)((char *)actor + 0x0c),
           (unsigned long)words[2], (long)(int)words[0x5c/4],
           (long)(int)words[0x60/4], (long)(int)words[0x64/4]);
    if (*(const unsigned short *)((char *)actor + 0x0c) == 191 &&
        heap_pointer(actor, 0x734)) {
        const unsigned *state = *(const unsigned **)((char *)actor + 0x370);
        report("  player-state: %p character %u step %u no-control %u\n", state,
               ((const unsigned char *)actor)[0x6d9],
               ((const unsigned char *)actor)[0x6e3],
               ((const unsigned char *)actor)[0x709]);
        const unsigned at = (unsigned)state;
        if (!(at & 3) && ((at >= 0x00100000 && at <= 0x01000000 - 24) ||
                         heap_pointer(state, 24))) {
            report("  state-pmfs:");
            for (int i = 0; i < 6; ++i) report(" %08lx", (unsigned long)state[i]);
            report("\n");
        }
    }
    // The actor being live proves the hosted DS register pages are mapped.
    const volatile unsigned short *main_regs = (const volatile unsigned short *)0x04000000;
    report("  display-a: blend %04x alpha %04x y %04x master %04x 3dcnt %04x\n",
           main_regs[0x50/2], main_regs[0x52/2], main_regs[0x54/2],
           main_regs[0x6c/2], main_regs[0x60/2]);
}

void heap_state()
{
    const struct mallinfo mi = mallinfo();
    report("  heap arena %u KB of %lu KB (in use %u KB, free in it %u KB), linear free %lu KB of %lu KB\n",
           (unsigned)(mi.arena / 1024), (unsigned long)(__ctru_heap_size / 1024),
           (unsigned)(mi.uordblks / 1024), (unsigned)(mi.fordblks / 1024),
           (unsigned long)(linearSpaceFree() / 1024), (unsigned long)(__ctru_linear_heap_size / 1024));
    // where sbrk stands against its end (the arena alone once left ~2 MB
    // unexplained)
    const char *top = static_cast<const char *>(sbrk(0));
    report("  sbrk top %p, heap %p..%p: %ld KB above the top, the top %ld KB past the start\n",
           (const void *)top, (const void *)fake_heap_start, (const void *)fake_heap_end,
           (long)((fake_heap_end - top) / 1024), (long)((top - fake_heap_start) / 1024));
}

// The report's file closed, the log and the save written, the screens kept;
// then this thread stops for good, the crash on screen.
[[noreturn]] void halt()
{
    if (g_crash_file) {
        fclose(g_crash_file);
        g_crash_file = nullptr;
    }
    fflush(stdout);
    console_tee_flush();   // the log is written by its own thread
    if (game3ds_save_flush) game3ds_save_flush();   // a save still in memory
    if (card3ds_screenshot) {
        const bool top = card3ds_screenshot("sdmc:/sm64ds3ds/game3ds_crash_top.bmp", false);
        const bool bottom = card3ds_screenshot("sdmc:/sm64ds3ds/game3ds_crash_bottom.bmp", true);
        printf("  crash-screens: top %d bottom %d\n", top, bottom);
    }
    printf("RESULT CRASH\n");
    fflush(stdout);
    console_tee_flush();   // the log is written by its own thread
    for (;;) svcSleepThread(1000000000LL);
}

void handler(ERRF_ExceptionInfo *info, CpuRegisters *regs)
{
    g_crash_file = fopen("sdmc:/sm64ds3ds/game3ds_crash.log", "w");
    report("\n*** CRASH: %s (%s)\n", kind_name(info->type),
           envIsHomebrew() ? "homebrew launch" : "installed title");
    report("  pc  %08lx   lr  %08lx   sp  %08lx\n", (unsigned long)regs->pc,
           (unsigned long)regs->lr, (unsigned long)regs->sp);
    report("  far %08lx   fsr %08lx\n", (unsigned long)info->far, (unsigned long)info->fsr);
    for (int i = 0; i < 13; ++i)
        report("  r%-2d %08lx%s", i, (unsigned long)regs->r[i], (i % 4 == 3) ? "\n" : "");
    report("  cpsr %08lx\n", (unsigned long)regs->cpsr);
    actor_context();
    // A few words above sp: return addresses of the frames above, often.
    const u32 *sp = (const u32 *)regs->sp;
    if (regs->sp >= 0x08000000 && regs->sp < 0x10000000) {
        report("  stack:");
        for (int i = 0; i < 128 && regs->sp + i * 4 < 0x08200000; ++i) {
            if (i && !(i & 15)) report("\n        ");
            report(" %08lx", (unsigned long)sp[i]);
        }
        report("\n");
    }
    heap_state();
    halt();
}

// Silent quits. abort() (an uncaught std::bad_alloc, libstdc++'s checks)
// raises SIGABRT, and newlib's default just calls _exit, so the app returned
// to the HOME Menu with no report and the log's last 250 ms lost. Now both
// report like an exception and halt. lr shows who called.
volatile bool g_stopping;

[[noreturn]] void stop(const char *what, void *lr)
{
    if (g_stopping) halt();   // the report itself failed
    g_stopping = true;
    g_crash_file = fopen("sdmc:/sm64ds3ds/game3ds_crash.log", "w");   // may fail, the heap gone
    report("\n*** CRASH: %s (%s)\n", what, envIsHomebrew() ? "homebrew launch" : "installed title");
    report("  lr %08lx, thread %08lx\n", (unsigned long)lr, (unsigned long)threadGetCurrent());
    heap_state();
    actor_context();
    // lr is operator new's or _raise_r's: the asker is further up, among
    // these words (return addresses, often), as in the exception's report
    const u32 *sp = static_cast<const u32 *>(__builtin_frame_address(0));
    report("  stack:");
    for (int i = 0; i < 128; ++i) {
        if (i && !(i & 15)) report("\n        ");
        report(" %08lx", (unsigned long)sp[i]);
    }
    report("\n");
    halt();
}

void on_new_failed()
{
    stop("operator new: out of memory", __builtin_return_address(0));
}

void on_abort(int)
{
    stop("abort() (SIGABRT)", __builtin_return_address(0));
}

}  // namespace

void crash_handler_install()
{
    threadOnException(handler, g_exc_stack + sizeof g_exc_stack, WRITE_DATA_TO_HANDLER_STACK);
    std::set_new_handler(on_new_failed);
    signal(SIGABRT, on_abort);
}
