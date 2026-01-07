#ifndef _OPENSPRINKLER_RF_H
#define _OPENSPRINKLER_RF_H

#include <stdint.h>

#define RF_MODE_NONE   0
#define RF_MODE_ZIGBEE 1
#define RF_MODE_MATTER 2

extern uint8_t current_rf_mode;

bool switch_rf_mode(uint8_t new_mode);

#endif
