// Install a per-thread CPU exception handler that logs the faulting PC, LR and
// registers (see crash_handler.cpp). Call once, early, on the main thread.
#pragma once
void crash_handler_install();
