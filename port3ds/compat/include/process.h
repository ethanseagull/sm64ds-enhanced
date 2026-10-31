/* MSVC <process.h> on the New 3DS: the one call the HAL makes, to name a
 * per-run log file. One process; the id is constant. */
#ifndef PORT3DS_COMPAT_PROCESS_H
#define PORT3DS_COMPAT_PROCESS_H
static inline int _getpid(void) { return 1; }
#endif
