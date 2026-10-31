// stdout/stderr -> the 3DS console AND a log file on the SD card, so a test
// run can be read back on a PC (the emulator's virtual SD is a host folder).
#pragma once

// Call after consoleInit(). Creates the directory part of `path` if needed.
// Returns false if the log could not be opened; the console still works.
bool console_tee_open(const char *path);
void console_tee_close();

// Write out what the log thread has not yet (the crash handler's last lines).
void console_tee_flush();

// Stop (or resume) writing to the console: a program that draws on the
// console's screen itself keeps the log file only.
void console_tee_quiet(bool on);
