#include "console_tee.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>

// The log is written by its own thread. Flushing to SD on every write (stdout
// and stderr are unbuffered) caused a ~1 s stall per [perf] report on hardware.
// Now writers append to a RAM buffer under a LightLock and a low-priority thread
// writes it out every kDrainNs, so only that thread waits on the SD card. A
// crash (console_tee_flush) and console_tee_close still write everything.
// SM64DS_LOG_SYNC=1 goes back to flushing every write, for a hang that would
// otherwise lose the last 250 ms.

namespace {

constexpr size_t kBufSize = 64 * 1024;
constexpr s64 kDrainNs = 250 * 1000 * 1000LL;

const devoptab_t *g_console;   // what consoleInit() installed for stdout
devoptab_t g_tee;
FILE *g_log;
bool g_quiet;                  // the log only: the console's screen is taken

// Two buffers: writers fill g_buf[g_fill]; the thread writes the other.
char g_buf[2][kBufSize];
size_t g_len[2];
int g_fill;
LightLock g_lock;              // g_fill, g_len
LightLock g_io;                // the FILE, so writes reach it in order
LightEvent g_wake;
Thread g_thread;
volatile bool g_quit;
volatile bool g_sync;

// Writes out what the writers have filled. Holding g_io across the swap
// keeps the buffers in order: whoever swaps first also writes first.
void drain()
{
    LightLock_Lock(&g_io);
    LightLock_Lock(&g_lock);
    const int full = g_fill;
    const size_t len = g_len[full];
    g_fill ^= 1;
    g_len[g_fill] = 0;
    LightLock_Unlock(&g_lock);
    if (len && g_log) {
        fwrite(g_buf[full], 1, len, g_log);   // sdmc's devoptab, not ours
        fflush(g_log);
    }
    LightLock_Unlock(&g_io);
}

void writer(void *)
{
    while (!g_quit) {
        LightEvent_WaitTimeout(&g_wake, kDrainNs);
        g_sync = getenv("SM64DS_LOG_SYNC") != nullptr;
        drain();
    }
}

void log_append(const char *ptr, size_t len)
{
    if (!g_thread || g_sync) {
        LightLock_Lock(&g_io);
        fwrite(ptr, 1, len, g_log);
        fflush(g_log);                        // a crash must not lose the last lines
        LightLock_Unlock(&g_io);
        return;
    }
    while (len) {
        LightLock_Lock(&g_lock);
        size_t &used = g_len[g_fill];
        const size_t n = len < kBufSize - used ? len : kBufSize - used;
        memcpy(g_buf[g_fill] + used, ptr, n);
        used += n;
        LightLock_Unlock(&g_lock);
        ptr += n;
        len -= n;
        if (len) {
            // Full (a boot-time dump): wake the thread and wait for room
            // rather than drop lines.
            LightEvent_Signal(&g_wake);
            svcSleepThread(1000 * 1000);
        }
    }
}

ssize_t tee_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    if (g_log) log_append(ptr, len);
    if (g_quiet) return (ssize_t)len;
    return g_console->write_r(r, fd, ptr, len);
}

void make_parent_dirs(const char *path)
{
    char buf[256];
    strncpy(buf, path, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *p = strchr(buf, '/'); p; p = strchr(p + 1, '/')) {
        if (p == buf || p[-1] == ':') continue;   // "sdmc:/" itself
        *p = 0;
        mkdir(buf, 0777);
        *p = '/';
    }
}

}  // namespace

bool console_tee_open(const char *path)
{
    g_console = devoptab_list[STD_OUT];
    make_parent_dirs(path);
    g_log = fopen(path, "w");

    LightLock_Init(&g_lock);
    LightLock_Init(&g_io);
    LightEvent_Init(&g_wake, RESET_ONESHOT);
    if (g_log) {
        // Below the game thread, on its core: it only runs while the game
        // waits (for core 2, for the vblank), and then mostly waits on FS.
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        g_thread = threadCreate(writer, nullptr, 32 * 1024, prio < 0x3f ? prio + 1 : prio,
                                -2, false);
    }

    g_tee = *g_console;
    g_tee.write_r = tee_write;
    devoptab_list[STD_OUT] = &g_tee;
    devoptab_list[STD_ERR] = &g_tee;
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    return g_log != nullptr;
}

void console_tee_quiet(bool on) { g_quiet = on; }

void console_tee_flush()
{
    if (g_log) drain();
}

void console_tee_close()
{
    if (g_thread) {
        g_quit = true;
        LightEvent_Signal(&g_wake);
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = nullptr;
    }
    if (g_log) {
        drain();
        fclose(g_log);
        g_log = nullptr;
    }
}
