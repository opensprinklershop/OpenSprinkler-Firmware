/* OpenSprinkler Unified Firmware
 *
 * Main-loop stall guard (ESP32 only).
 *
 * A periodic esp_timer watches a heartbeat that do_loop() refreshes once per
 * iteration. If the main loop stops advancing (blocked on a lock/socket or
 * spinning), the guard first prints a diagnostic dump of the loop task
 * (state, saved PC/RA, raw stack words for addr2line, HTTP mutex holder) via
 * esp_rom_printf — this works in release builds without ENABLE_DEBUG — and,
 * if the stall persists, reboots with REBOOT_CAUSE_LOOP_STALL so the
 * controller recovers instead of staying dead with possibly open valves.
 *
 * On other platforms all calls are no-ops.
 */

#ifndef _LOOP_GUARD_H
#define _LOOP_GUARD_H

#include <stdint.h>

#ifndef LOOP_GUARD_DUMP_MS
#define LOOP_GUARD_DUMP_MS    60000UL    // print diagnostics after 60 s without heartbeat
#endif
#ifndef LOOP_GUARD_REBOOT_MS
#define LOOP_GUARD_REBOOT_MS  300000UL   // reboot after 5 min without heartbeat
#endif

// Call once from do_setup() on the task that runs do_loop().
void loop_guard_init();

// Call once per do_loop() iteration.
void loop_guard_beat();

// Print the diagnostic dump now (used by the guard itself; handy for tests).
void loop_guard_dump(const char *why);

#endif // _LOOP_GUARD_H
