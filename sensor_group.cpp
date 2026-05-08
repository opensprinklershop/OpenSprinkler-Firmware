/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Group sensor implementation (MIN/MAX/AVG/SUM)
 * 2026 @ OpenSprinklerShop
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

int GroupSensor::read(unsigned long time) {
  (void)time;
  double value = 0;
  int n = 0;

  for (auto it = sensors_iterate_begin(); ; ) {
    SensorBase *member = sensors_iterate_next(it);
    if (!member) break;
    if (member->nr == nr || member->group != nr || !member->flags.enable)
      continue;
    switch (type) {
      case SENSOR_GROUP_MIN:
        if (n++ == 0) value = member->last_data;
        else if (member->last_data < value) value = member->last_data;
        break;
      case SENSOR_GROUP_MAX:
        if (n++ == 0) value = member->last_data;
        else if (member->last_data > value) value = member->last_data;
        break;
      case SENSOR_GROUP_AVG:
      case SENSOR_GROUP_SUM:
        n++;
        value += member->last_data;
        break;
    }
  }
  if (type == SENSOR_GROUP_AVG && n > 0)
    value = value / (double)n;

  last_data = value;
  last_native_data = 0;
  flags.data_ok = n > 0;
  return (n > 0) ? HTTP_RQT_SUCCESS : HTTP_RQT_NOT_RECEIVED;
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
