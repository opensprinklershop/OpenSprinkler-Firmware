/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Group sensor implementation (MIN/MAX/AVG/SUM)
 * 2024 @ OpenSprinklerShop
 * Stefan Schmaltz (info@opensprinklershop.de)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>. 
 */

#include "sensor_group.h"

// Group sensor read is handled by sensor_update_groups() in sensors.cpp
// This method is called during the normal sensor reading cycle
int GroupSensor::read(unsigned long time) {
  // Group sensors are updated via sensor_update_groups()
  // which is called after all regular sensors have been read
  (void)time;
  return HTTP_RQT_SUCCESS;
}

// Group sensors inherit unit from their first non-group member
// This uses the same logic as the original getSensorUnitId() for groups
unsigned char GroupSensor::getUnitId() const {
  // Iterate through all sensors to find members of this group
  const SensorBase* current = this;
  
  for (int iteration = 0; iteration < 100; iteration++) {
    bool found = false;
    
    for (auto it = sensors_iterate_begin(); ; ) {
      SensorBase *sen = sensors_iterate_next(it);
      if (!sen) break;
      
      // Check if this sensor is a member of the current group
      if (sen != current && sen->group > 0 && sen->group == current->nr) {
        // If it's not a group sensor, return its unit ID
        if (!sensor_isgroup(sen)) {
          return sen->getUnitId();
        }
        // If it's a nested group, continue searching from that group
        current = sen;
        found = true;
        break;
      }
    }
    
    if (!found) break;
  }
  
  // No valid member found
  return UNIT_NONE;
}
