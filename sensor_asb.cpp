/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Analog Sensor Board (ASB) sensor implementation
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

#if defined(ESP8266) || defined(ESP32)

#include "sensor_asb.h"
#include "OpenSprinkler.h"
#include "sensors.h"

extern OpenSprinkler os;
extern uint16_t get_asb_detected_boards();

/**
 * Read ESP8266/ESP32 ADS1115 sensors (Analog Sensor Board)
 */
int AsbSensor::read(unsigned long time) {
  //DEBUG_PRINTLN(F("AsbSensor::read"));
  if (!this->flags.enable) return HTTP_RQT_NOT_RECEIVED;
  if (this->id >= 16) return HTTP_RQT_NOT_RECEIVED;
  // Init + Detect:

  if (this->id < 8 && ((get_asb_detected_boards() & ASB_BOARD1) == 0))
    return HTTP_RQT_NOT_RECEIVED;
  if (this->id >= 8 && this->id < 16 &&
      ((get_asb_detected_boards() & ASB_BOARD2) == 0))
    return HTTP_RQT_NOT_RECEIVED;

  int port = ASB_BOARD_ADDR1a + this->id / 4;
  int id = this->id % 4;

  // unsigned long startTime = millis();
  ADS1115 adc(port);
  if (!adc.begin()) {
    DEBUG_PRINTLN(F("no asb board?!?"));
    return HTTP_RQT_NOT_RECEIVED;
  }
  // unsigned long endTime = millis();
  // DEBUG_PRINTF("t=%lu ms\n", endTime-startTime);

  // startTime = millis();
  this->repeat_native += adc.readADC(id);
  // endTime = millis();
  // DEBUG_PRINTF("t=%lu ms\n", endTime-startTime);

  if (++this->repeat_read < MAX_SENSOR_REPEAT_READ &&
      time < this->last_read + this->read_interval)
    return HTTP_RQT_NOT_RECEIVED;

  uint64_t avgValue = this->repeat_native / this->repeat_read;

  this->repeat_native = avgValue;
  this->repeat_data = 0;
  this->repeat_read = 1;  // read continously

  this->last_native_data = avgValue;
  this->last_data = adc.toVoltage(this->last_native_data);
  double v = this->last_data;

  switch (this->type) {
    case SENSOR_SMT50_MOIS:  // SMT50 VWC [%] = (U * 50) : 3
      this->last_data = (v * 50.0) / 3.0;
      break;
    case SENSOR_SMT50_TEMP:  // SMT50 T [°C] = (U – 0,5) * 100
      this->last_data = (v - 0.5) * 100.0;
      break;
    case SENSOR_ANALOG_EXTENSION_BOARD_P:  // 0..3,3V -> 0..100%
      this->last_data = v * 100.0 / 3.3;
      if (this->last_data < 0)
        this->last_data = 0;
      else if (this->last_data > 100)
        this->last_data = 100;
      break;
    case SENSOR_SMT100_ANALOG_MOIS:  // 0..3V -> 0..100%
      this->last_data = v * 100.0 / 3;
      break;
    case SENSOR_SMT100_ANALOG_TEMP:  // 0..3V -> -40°C..60°C
      this->last_data = v * 100.0 / 3 - 40;
      break;

    case SENSOR_VH400:  // http://vegetronix.com/Products/VH400/VH400-Piecewise-Curve
      if (v <= 1.1)  // 0 to 1.1V         VWC= 10*V-1
        this->last_data = 10 * v - 1;
      else if (v < 1.3)  // 1.1V to 1.3V      VWC= 25*V- 17.5
        this->last_data = 25 * v - 17.5;
      else if (v < 1.82)  // 1.3V to 1.82V     VWC= 48.08*V- 47.5
        this->last_data = 48.08 * v - 47.5;
      else if (v < 2.2)  // 1.82V to 2.2V     VWC= 26.32*V- 7.89
        this->last_data = 26.32 * v - 7.89;
      else  // 2.2V - 3.0V       VWC= 62.5*V - 87.5
        this->last_data = 62.5 * v - 87.5;
      break;
    case SENSOR_THERM200:  // http://vegetronix.com/Products/THERM200/
      this->last_data = v * 41.67 - 40;
      break;
    case SENSOR_AQUAPLUMB:  // http://vegetronix.com/Products/AquaPlumb/
      this->last_data = v * 100.0 / 3.0;  // 0..3V -> 0..100%
      if (this->last_data < 0)
        this->last_data = 0;
      else if (this->last_data > 100)
        this->last_data = 100;
      break;
    case SENSOR_USERDEF:  // User defined sensor
      v -= (double)this->offset_mv /
           1000;  // adjust zero-point offset in millivolt
      if (this->factor && this->divider)
        v *= (double)this->factor / (double)this->divider;
      else if (this->divider)
        v /= this->divider;
      else if (this->factor)
        v *= this->factor;
      this->last_data = v + this->offset2 / 100;
      break;
  }

  this->flags.data_ok = true;
  this->last_read = time;

  DEBUG_PRINT(F("adc sensor values: "));
  DEBUG_PRINT(this->last_native_data);
  DEBUG_PRINT(",");
  DEBUG_PRINTLN(this->last_data);

  return HTTP_RQT_SUCCESS;
}

#endif // defined(ESP8266) || defined(ESP32)
