#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
#include "esp_rom_sys.h"

void __real_esp_restart(void);

void __wrap_esp_restart(void) {
  void *caller = __builtin_return_address(0);
  esp_rom_printf("[RESTART_TRACE] esp_restart caller=%p\n", caller);
  __real_esp_restart();
}
#endif