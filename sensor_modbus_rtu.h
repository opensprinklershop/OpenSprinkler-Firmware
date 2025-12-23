/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * sensors header file
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

#ifndef _SENSOR_MODBUS_RTU_H
#define _SENSOR_MODBUS_RTU_H

#include "sensors.h"

int read_sensor_ip(Sensor_t *sensor);

void sensor_modbus_rtu_free();
boolean send_modbus_rtu_command(uint32_t ip, uint16_t port, uint8_t address, uint16_t reg,uint16_t data, bool isbit);
int set_sensor_address_ip(Sensor_t *sensor, uint8_t new_address);

#endif