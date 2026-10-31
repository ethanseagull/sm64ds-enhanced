// The ROM's card page reader, reading each block in one call.
//
// The ROM's card driver reads files 0x200 bytes at a time through
// src/func_02060a64.c: it writes ROMCTRL, polls it, and reads the data port
// 128 times per block, and each of those is an emulated register access on
// 3DS. That was most of a level change's CPU time. This is func_02060a64 line
// for line, but with the store, polling and 128 reads replaced by
// port_card_block_direct, which leaves the card in the same state the last
// read would. Hooked with -Wl,--wrap=func_02060a64; GAME3DS_CARD_POLL=1 keeps
// the ROM's own loop.

#include <stdint.h>
#include <stdlib.h>

extern "C" {
struct CardXferState {
    unsigned char pad[0x18];
    unsigned int src;
    unsigned int dst;
    unsigned int size;
};
extern CardXferState data_020a8180;
void func_02060d98(unsigned int a, unsigned int b);
int func_02060ebc(char *self);
int __real_func_02060a64(char *self);
unsigned port_card_block_direct(uint32_t romctrl, uint32_t *out, unsigned max_words);

int __wrap_func_02060a64(char *self)
{
    // read each call (once a transfer): the first come before env.txt is read
    if (getenv("GAME3DS_CARD_POLL")) return __real_func_02060a64(self);
    CardXferState *g = &data_020a8180;
    unsigned int *alt = (unsigned int *)(self + 0x20);
    for (;;) {
        const unsigned int src = g->src;
        const unsigned int aligned = src & ~0x1FFu;
        unsigned int *buf;
        if (aligned == src && (g->dst & 3) == 0 && g->size >= 0x200) {
            buf = (unsigned int *)(uintptr_t)g->dst;
        } else {
            buf = alt;
            *(unsigned int *)(self + 8) = aligned;
        }
        func_02060d98((aligned >> 8) | 0xb7000000u, aligned << 0x18);
        port_card_block_direct(*(unsigned int *)(self + 4), (uint32_t *)buf, 0x200);
        if (buf == (unsigned int *)(uintptr_t)g->dst) {
            g->src += 0x200;
            g->dst += 0x200;
            g->size -= 0x200;
            if (g->size == 0) return (int)g->size;
        } else {
            const int rc = func_02060ebc(self);
            if (rc == 0) return rc;
        }
    }
}
}
