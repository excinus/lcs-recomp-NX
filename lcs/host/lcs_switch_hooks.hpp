#pragma once

// Calls from shared host code into the Nintendo Switch platform layer
// (lcs/switch/switch_platform.cpp). Empty on every other platform.

#if defined(__SWITCH__)
extern "C" {
// Names the calling thread in the profiler's log lines; "ge" also moves it
// to the core kept for the GE worker.
void lcs_switch_name_current_thread(const char *name);
}
#endif
