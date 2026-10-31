// Audio output on 3DS: hal/sdat/out_win.cpp's interface on top of NDSP.
//
// One stereo 16-bit NDSP channel at SD_MIX_RATE (32768 Hz, the DS's own rate),
// fed by a ring of NBUF buffers of OUT_FRAMES frames (4 x 1024, 125 ms, same as
// the PC's waveOut ring). Each sd_out_push fills every buffer the DSP has given
// back and queues it, so the mixer advances at the rate the DSP plays.
//
// NDSP needs the DSP firmware (sdmc:/3ds/dspfirm.cdc, dumped with DSP1);
// Azahar works without it. Without NDSP the game runs silently and the mixer is
// clocked by frames instead. SM64DS_NO_AUDIO=1 skips the device;
// SM64DS_VOLUME=0..100 sets the master volume (default 100, the DS's level).
//
// Mixing runs on its own thread, on core 2 when available (inline it cost the
// game thread 1-17 ms a tick). NDSP's callback wakes it and it renders every
// returned buffer in OUT_CHUNK pieces under g_lock. The game thread holds
// g_lock for all of sdat_host_tick (the ROM's sound frame and feeding the
// mixer), and nothing else touches the mixer. Old 3DS mixes on the system core
// (start_mix_thread); without a thread sd_out_push mixes inline.
// SM64DS_AUDIO_INLINE=1 forces inline.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal/sdat/sdat.h"

namespace {

enum { OUT_FRAMES = 1024, NBUF = 4, CHANNEL = 0, OUT_CHUNK = 64 };

int g_opened;              // 0 untried, 1 live, -1 no device
int g_volPct = -1;         // -1 unread, else 0..100
ndspWaveBuf g_wb[NBUF];
sd_s16 *g_pcm;             // NBUF x OUT_FRAMES stereo frames, linear memory
int g_pushes, g_refills, g_starved;
u64 g_lock_wait, g_lock_wait_max;   // the game thread's waits for g_lock, since the last report
u64 g_hold_max;            // the mix thread's longest chunk under g_lock, since the last report
int g_peak;                // the loudest sample since the last report

LightLock g_lock;          // the mixer and sequencer state (see the header)
LightEvent g_wake;         // NDSP's frame callback -> the mix thread
// Let the tick go first. A LightLock isn't fair, so the mix thread could
// release and immediately retake it, making the tick wait for a whole refill.
// The game thread raises g_tick_wants before locking, and the mix thread won't
// take the lock for its next chunk until the tick has had it.
// Chunks are 64 frames (2 ms of audio): on Old 3DS the mix runs on core 1,
// which we only get 30% of, so a thread paused mid-chunk holds the lock until
// core 1 comes back. Shorter chunks make that rarer.
volatile bool g_tick_wants;
LightEvent g_tick_out;
Thread g_thread;           // null: sd_out_push mixes inline
int g_core;                // its core: 2 (New 3DS) or 1 (Old 3DS, the system core)
volatile bool g_quit;

// Render every buffer the DSP has given back. Called from the mix thread (each
// chunk under g_lock), or inline from sd_out_push, which already runs inside
// sdat_host_tick.
void fill_free_buffers(bool locked)
{
    int free_on_entry = 0;
    for (int i = 0; i < NBUF; ++i)
        if (g_wb[i].status == NDSP_WBUF_DONE || g_wb[i].status == NDSP_WBUF_FREE)
            ++free_on_entry;
    // every buffer back means the DSP played the ring dry (not on the first fill)
    if (free_on_entry == NBUF && g_refills > 0) ++g_starved;
    for (int i = 0; i < NBUF; ++i) {
        ndspWaveBuf &wb = g_wb[i];
        if (wb.status != NDSP_WBUF_DONE && wb.status != NDSP_WBUF_FREE) continue;
        sd_s16 *pcm = g_pcm + (size_t)i * OUT_FRAMES * 2;
        for (int f = 0; f < OUT_FRAMES; f += OUT_CHUNK) {
            if (locked) {
                while (g_tick_wants) LightEvent_Wait(&g_tick_out);
                LightLock_Lock(&g_lock);
            }
            const u64 h0 = locked ? svcGetSystemTick() : 0;
            sd_mix_render(pcm + (size_t)f * 2, OUT_CHUNK);
            if (locked) {
                const u64 held = svcGetSystemTick() - h0;
                if (held > g_hold_max) g_hold_max = held;
                LightLock_Unlock(&g_lock);
            }
        }
        for (int k = 0; k < OUT_FRAMES * 2; ++k) {
            const int a = pcm[k] < 0 ? -pcm[k] : pcm[k];
            if (a > g_peak) g_peak = a;
        }
        sd_wav_write(pcm, OUT_FRAMES);
        DSP_FlushDataCache(pcm, OUT_FRAMES * 2 * sizeof(sd_s16));
        ndspChnWaveBufAdd(CHANNEL, &wb);
        ++g_refills;
    }
}

void ndsp_frame(void *)
{
    LightEvent_Signal(&g_wake);
}

void mix_thread(void *)
{
    while (!g_quit) {
        LightEvent_Wait(&g_wake);
        if (g_quit) break;
        fill_free_buffers(true);
    }
}

// Core 2 is the New 3DS's second app core. Old 3DS doesn't have one, so the mix
// goes to the system core (core 1), which APT lends the app kCore1Pct percent
// of. If that's refused, or SM64DS_AUDIO_NO_CORE1=1, the mix stays on the game
// thread.
enum { kCore1Pct = 30 };
void start_mix_thread(void)
{
    if (getenv("SM64DS_AUDIO_INLINE")) return;
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    int core = 2;
    if (!n3ds) {
        if (getenv("SM64DS_AUDIO_NO_CORE1")) return;
        u32 pct = 0;
        APT_GetAppCpuTimeLimit(&pct);
        if (pct < kCore1Pct) {
            const Result r = APT_SetAppCpuTimeLimit(kCore1Pct);
            if (R_FAILED(r)) {
                fprintf(stderr, "[audio] core 1 refused (%08lx): the mix stays inline\n",
                        (unsigned long)r);
                return;
            }
        }
        core = 1;
    }
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    LightEvent_Init(&g_wake, RESET_ONESHOT);
    LightEvent_Init(&g_tick_out, RESET_ONESHOT);
    g_quit = false;
    g_thread = threadCreate(mix_thread, nullptr, 32 * 1024, prio > 0x18 ? prio - 1 : prio, core,
                            false);
    if (g_thread)
        ndspSetCallback(ndsp_frame, nullptr);
    g_core = core;
}

void out_report(void)
{
    fprintf(stderr, "[audio] NDSP ring: %d pushes, %d refills, %d starved (%d x %d frames "
                    "at %d Hz)\n", g_pushes, g_refills, g_starved, NBUF, OUT_FRAMES,
            (int)SD_MIX_RATE);
}

void out_exit(void)
{
    if (g_opened != 1) return;
    if (g_thread) {
        ndspSetCallback(nullptr, nullptr);
        g_quit = true;
        LightEvent_Signal(&g_wake);
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = nullptr;
    }
    out_report();
    ndspChnReset(CHANNEL);
    ndspExit();
    linearFree(g_pcm);
    g_pcm = nullptr;
    g_opened = -1;
}

}  // namespace

// The game thread's side of g_lock, from phase_timers.cpp's sdat_host_tick
// wrapper. Only while a mix thread runs; the return says whether to unlock
// (the device can open and the thread start inside the tick).
extern "C" int out3ds_sound_lock(void)
{
    if (!g_thread) return 0;
    const u64 t0 = svcGetSystemTick();
    g_tick_wants = true;
    __dmb();
    LightLock_Lock(&g_lock);
    const u64 waited = svcGetSystemTick() - t0;
    g_lock_wait += waited;
    if (waited > g_lock_wait_max) g_lock_wait_max = waited;
    return 1;
}

extern "C" void out3ds_sound_unlock(int locked)
{
    if (!locked) return;
    LightLock_Unlock(&g_lock);
    g_tick_wants = false;
    __dmb();
    LightEvent_Signal(&g_tick_out);
}

int out_volume_pct(void)
{
    if (g_volPct < 0) {
        const char *v = getenv("SM64DS_VOLUME");
        if (v && *v) {
            const long n = strtol(v, 0, 10);
            g_volPct = (int)(n < 0 ? 0 : (n > 100 ? 100 : n));
        } else {
            // 100 is the DS's own level (the mix's full scale matches the DS DAC). The PC's
            // 50 was 6 dB quieter than the cartridge; at 100 the loudest test cases peak
            // at -1.7 dBFS.
            g_volPct = 100;
        }
    }
    return g_volPct;
}

extern "C" void out_set_volume_pct(int pct)
{
    g_volPct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

// SM64DS_WAV_DUMP=<path> (e.g. sdmc:/sm64ds3ds/mix.wav): record the mix as the
// device gets it for SM64DS_WAV_SECONDS (default 30) from the first sound.
// Written on the mix thread a buffer at a time. For measuring only (SD writes).
namespace {
FILE *g_wav;
sd_u32 g_wav_frames, g_wav_limit;

void wav_header(FILE *f, sd_u32 frames)
{
    const sd_u32 data = frames * 4, riff = 36 + data, fmt_len = 16, rate = SD_MIX_RATE,
                 byte_rate = SD_MIX_RATE * 4;
    const sd_u16 fmt = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_len, 4, 1, f);
    fwrite(&fmt, 2, 1, f);
    fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
}
}  // namespace

void sd_wav_close(void)
{
    if (!g_wav) return;
    fseek(g_wav, 0, SEEK_SET);
    wav_header(g_wav, g_wav_frames);
    fclose(g_wav);
    g_wav = nullptr;
    fprintf(stderr, "[audio] wav dump closed: %u frames (%.1f s)\n", (unsigned)g_wav_frames,
            g_wav_frames / (double)SD_MIX_RATE);
}

void sd_wav_open(const char *path)
{
    if (g_wav || !path || !*path) return;
    g_wav = fopen(path, "wb");
    if (!g_wav) return;
    const char *s = getenv("SM64DS_WAV_SECONDS");
    const long sec = s ? strtol(s, nullptr, 10) : 30;
    g_wav_limit = (sd_u32)((sec > 0 ? sec : 30) * SD_MIX_RATE);
    g_wav_frames = 0;
    wav_header(g_wav, 0);
    fprintf(stderr, "[audio] wav dump -> %s (%ld s)\n", path, sec > 0 ? sec : 30);
}

void sd_wav_write(const sd_s16 *pcm, int frames)
{
    if (!g_wav || frames <= 0) return;
    if (!g_wav_frames) {   // from the first sound
        bool any = false;
        for (int i = 0; i < frames * 2 && !any; ++i) any = pcm[i] != 0;
        if (!any) return;
    }
    sd_u32 n = (sd_u32)frames;
    if (n > g_wav_limit - g_wav_frames) n = g_wav_limit - g_wav_frames;
    fwrite(pcm, 4, n, g_wav);
    g_wav_frames += n;
    if (g_wav_frames >= g_wav_limit) sd_wav_close();
}

int sd_out_open(void)
{
    if (g_opened) return g_opened == 1;
    g_opened = -1;
    if (getenv("SM64DS_NO_AUDIO")) {
        fprintf(stderr, "[audio] SM64DS_NO_AUDIO=1: no output, the mixer runs silently\n");
        return 0;
    }
    const Result r = ndspInit();
    if (R_FAILED(r)) {
        fprintf(stderr, "[audio] ndspInit failed (%08lx; on hardware it needs "
                        "sdmc:/3ds/dspfirm.cdc) -- silent\n", (unsigned long)r);
        return 0;
    }
    g_pcm = (sd_s16 *)linearAlloc(NBUF * OUT_FRAMES * 2 * sizeof(sd_s16));
    if (!g_pcm) {
        ndspExit();
        fprintf(stderr, "[audio] no linear memory for the ring -- silent\n");
        return 0;
    }
    memset(g_pcm, 0, NBUF * OUT_FRAMES * 2 * sizeof(sd_s16));
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(CHANNEL);
    // The DSP plays at ~32728 Hz, so it resamples our 32768 Hz mix. Its polyphase
    // filter does that cleanly; linear interpolation at a ratio this close to 1
    // dulls the highs. SM64DS_NDSP_INTERP=linear switches back for comparison.
    {
        const char *e = getenv("SM64DS_NDSP_INTERP");
        ndspChnSetInterp(CHANNEL, (e && strcmp(e, "linear") == 0) ? NDSP_INTERP_LINEAR
                                                                   : NDSP_INTERP_POLYPHASE);
    }
    ndspChnSetRate(CHANNEL, (float)SD_MIX_RATE);
    ndspChnSetFormat(CHANNEL, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {1.0f, 1.0f};   // front left, front right
    ndspChnSetMix(CHANNEL, mix);
    memset(g_wb, 0, sizeof g_wb);
    for (int i = 0; i < NBUF; ++i) {
        g_wb[i].data_pcm16 = g_pcm + (size_t)i * OUT_FRAMES * 2;
        g_wb[i].nsamples = OUT_FRAMES;
        g_wb[i].status = NDSP_WBUF_DONE;   // free to fill
    }
    g_opened = 1;
    LightLock_Init(&g_lock);
    atexit(out_exit);
    start_mix_thread();
    fprintf(stderr, "[audio] NDSP open: %d Hz stereo, %d x %d frames (%.0f ms), volume %d%%, "
                    "mix %s\n",
            (int)SD_MIX_RATE, NBUF, OUT_FRAMES, 1000.0 * NBUF * OUT_FRAMES / SD_MIX_RATE,
            out_volume_pct(), !g_thread ? "inline" : g_core == 1 ? "on core 1" : "on core 2");
    return 1;
}

void sd_out_close(void)
{
    out_exit();
}

void sd_out_push(void)
{
    if (!g_opened) sd_out_open();
    if (g_opened != 1) {
        // no device: clock the mixer off the frame so state still advances
        static sd_s16 scratch[(SD_MIX_RATE / 60) * 2];
        sd_mix_render(scratch, SD_MIX_RATE / 60);
        return;
    }
    ++g_pushes;
    if (!g_thread)
        fill_free_buffers(false);
    // every 300 pushes (10 s of game at 30 ticks a second): the ring's health
    // and how loud the mix was, so a silent mix is a number, not an ear
    if (g_pushes % 300 == 0) {
        unsigned long decodes, samples;   // (the tick holds the lock here)
        sd_waves_stats(&decodes, &samples, 1);
        fprintf(stderr, "[audio] %d pushes: %d refills, %d starved, peak %d; the tick's lock wait "
                        "%.2f ms a push (max %.2f), the mix's longest hold %.2f; %lu waves decoded "
                        "(%lu K samples)\n",
                g_pushes, g_refills, g_starved, g_peak, g_lock_wait * 1000.0 / SYSCLOCK_ARM11 / 300,
                g_lock_wait_max * 1000.0 / SYSCLOCK_ARM11, g_hold_max * 1000.0 / SYSCLOCK_ARM11,
                decodes, samples / 1000);
        g_lock_wait = g_lock_wait_max = g_hold_max = 0;
        g_peak = 0;
    }
}
