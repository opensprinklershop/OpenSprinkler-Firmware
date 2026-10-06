/* OpenSprinkler Unified Firmware
 *
 * Main-loop stall guard (ESP32 only). See loop_guard.h.
 */

#include "loop_guard.h"

#if defined(ESP32)

#include <Arduino.h>
#include "OpenSprinkler.h"
#include "defines.h"
#include "online_update.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "riscv/rvruntime-frames.h"

extern OpenSprinkler os;
const char* os_http_mutex_holder_name(); // OpenSprinkler.cpp

namespace {

constexpr uint32_t GUARD_PERIOD_MS = 5000;
constexpr uint32_t STALL_MAGIC = 0x4C535441; // "LSTA"
constexpr size_t STACK_DUMP_WORDS = 192;

// Survives esp_restart() (.noinit is not cleared on a software reset):
// tells the next boot that the guard rebooted us.
__NOINIT_ATTR uint32_t s_stall_marker;

volatile uint32_t s_beat_ms = 0;
volatile uint32_t s_beat_count = 0;
TaskHandle_t s_loop_task = nullptr;
esp_timer_handle_t s_timer = nullptr;
bool s_dumped = false;
uint32_t s_last_dump_ms = 0;

const char* task_state_name(eTaskState st) {
	switch (st) {
		case eRunning:   return "Running";
		case eReady:     return "Ready";
		case eBlocked:   return "Blocked";
		case eSuspended: return "Suspended";
		case eDeleted:   return "Deleted";
		default:         return "?";
	}
}

// Text addresses on ESP32-C5: IROM (flash) 0x42000000.., IRAM 0x40800000..
bool looks_like_code(uint32_t w) {
	return (w >= 0x42000000UL && w < 0x42800000UL) || (w >= 0x40800000UL && w < 0x40880000UL);
}

// Dump the saved context of the (blocked or preempted) loop task. The first
// TCB member is pxTopOfStack, which the RISC-V port points at the RvExcFrame
// written on context switch. mepc/ra tell where the task last yielded; the
// code-looking words above sp are return addresses for addr2line.
void dump_loop_task_frame() {
	if (!s_loop_task) return;
	StackType_t *top = *reinterpret_cast<StackType_t * volatile *>(s_loop_task);
	const RvExcFrame *f = reinterpret_cast<const RvExcFrame *>(top);
	if (!f) return;
	esp_rom_printf("[LOOP_GUARD] frame mepc=0x%08x ra=0x%08x sp=0x%08x a0=0x%08x a1=0x%08x\n",
		(unsigned)f->mepc, (unsigned)f->ra, (unsigned)f->sp, (unsigned)f->a0, (unsigned)f->a1);
	esp_rom_printf("[LOOP_GUARD] stack words (addr2line candidates):");
	const uint32_t *sp = reinterpret_cast<const uint32_t *>(f->sp);
	if (!sp) { esp_rom_printf(" (null sp)\n"); return; }
	int printed = 0;
	for (size_t i = 0; i < STACK_DUMP_WORDS; i++) {
		uint32_t w = sp[i];
		if (looks_like_code(w)) {
			esp_rom_printf(" 0x%08x", (unsigned)w);
			if (++printed % 8 == 0) esp_rom_printf("\n[LOOP_GUARD]  ");
		}
	}
	esp_rom_printf("\n");
}

void guard_tick(void *) {
	uint32_t now = millis();
	uint32_t silent = now - s_beat_ms;
	if (silent < LOOP_GUARD_DUMP_MS) {
		s_dumped = false;
		return;
	}
	// Legitimate long blocking: an OTA download drives its own progress.
	if (online_update_in_progress()) return;

	if (!s_dumped || (now - s_last_dump_ms) >= 60000UL) {
		s_dumped = true;
		s_last_dump_ms = now;
		char why[48];
		snprintf(why, sizeof why, "no heartbeat for %u ms", (unsigned)silent);
		loop_guard_dump(why);
	}

	if (silent >= LOOP_GUARD_REBOOT_MS) {
		esp_rom_printf("[LOOP_GUARD] main loop stalled for %u ms -> reboot\n", (unsigned)silent);
		s_stall_marker = STALL_MAGIC;
		// Do not touch LittleFS here: the stuck loop may hold its lock. The
		// reboot cause is recorded on the next boot from s_stall_marker.
		esp_restart();
	}
}

} // namespace

void loop_guard_dump(const char *why) {
	esp_rom_printf("\n[LOOP_GUARD] ===== %s (beats=%u, uptime=%u ms) =====\n",
		why ? why : "dump", (unsigned)s_beat_count, (unsigned)millis());
	if (s_loop_task) {
		esp_rom_printf("[LOOP_GUARD] loop task '%s' state=%s prio=%u stack_hwm=%u\n",
			pcTaskGetName(s_loop_task), task_state_name(eTaskGetState(s_loop_task)),
			(unsigned)uxTaskPriorityGet(s_loop_task), (unsigned)uxTaskGetStackHighWaterMark(s_loop_task));
		dump_loop_task_frame();
	}
	esp_rom_printf("[LOOP_GUARD] http mutex holder: %s\n", os_http_mutex_holder_name());
	esp_rom_printf("[LOOP_GUARD] heap int=%u (min %u, largest %u) psram=%u\n",
		(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
		(unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
		(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
		(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
	// Task list: name / state / prio / stack hwm / num. ~60 bytes per task.
	char *list = (char *)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!list) list = (char *)malloc(2048);
	if (list) {
		list[0] = 0;
		vTaskList(list);
		esp_rom_printf("[LOOP_GUARD] tasks:\n%s", list);
		free(list);
	}
#endif
	esp_rom_printf("[LOOP_GUARD] ===== end =====\n");
}

void loop_guard_init() {
	if (s_stall_marker == STALL_MAGIC) {
		s_stall_marker = 0;
		os.last_reboot_cause = REBOOT_CAUSE_LOOP_STALL;
		DEBUG_PRINTLN(F("[LOOP_GUARD] previous boot ended in a main-loop stall reboot"));
	}
	s_loop_task = xTaskGetCurrentTaskHandle();
	s_beat_ms = millis();
	if (s_timer) return;
	const esp_timer_create_args_t args = {
		.callback = &guard_tick,
		.arg = nullptr,
		.dispatch_method = ESP_TIMER_TASK,
		.name = "loop_guard",
		.skip_unhandled_events = true,
	};
	if (esp_timer_create(&args, &s_timer) == ESP_OK) {
		esp_timer_start_periodic(s_timer, (uint64_t)GUARD_PERIOD_MS * 1000ULL);
	}
}

void loop_guard_beat() {
	s_beat_ms = millis();
	s_beat_count++;
}

#else // !ESP32

void loop_guard_init() {}
void loop_guard_beat() {}
void loop_guard_dump(const char *) {}

#endif
