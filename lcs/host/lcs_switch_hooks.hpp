#pragma once

// Calls from shared host code into the Nintendo Switch platform layer
// (lcs/switch/switch_platform.cpp). Empty on every other platform.

#if defined(__SWITCH__)
extern "C" {
// Turns the sys-clk overrides on or off (Settings > Overclock).
void lcs_switch_set_overclock(bool enabled);
// True when the sys-clk service answered at start.
bool lcs_switch_sysclk_available();
// Names the calling thread in the profiler's log lines.
void lcs_switch_name_current_thread(const char *name);
}
#endif
