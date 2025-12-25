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

#include "sensors.h"
#include "Sensor.hpp"
#include "sensor_modbus_rtu.h"

extern OpenSprinkler os;

// modbus transaction id
static uint16_t modbusTcpId = 0;
#if defined(OSPI)
static modbus_t * modbusDevs[MAX_RS485_DEVICES];
#endif

void sensor_modbus_rtu_free() {
    modbusTcpId = 0;
}

// Implementations for ModbusRtuSensor declared in header
int ModbusRtuSensor::read(unsigned long time) {
    flags.data_ok = false;
    if (!ip || !port) {
      flags.enable = false;
      return HTTP_RQT_CONNECT_ERR;
    }

#if defined(ARDUINO)
    Client *client;
  #if defined(ESP8266) || defined(ESP32)
    WiFiClient wifiClient;
    client = &wifiClient;
  #else
    EthernetClient etherClient;
    client = &etherClient;
  #endif

#else
    EthernetClient etherClient;
    EthernetClient *client = &etherClient;
#endif

#if defined(ARDUINO)
    IPAddress _ip(ip);
    unsigned char ipbytes[4] = {_ip[0], _ip[1], _ip[2], _ip[3]};
#else
    unsigned char ipbytes[4];
    ipbytes[3] = (unsigned char)((ip >> 24) & 0xFF);
    ipbytes[2] = (unsigned char)((ip >> 16) & 0xFF);
    ipbytes[1] = (unsigned char)((ip >> 8) & 0xFF);
    ipbytes[0] = (unsigned char)((ip & 0xFF));
#endif
    char server[20];
    sprintf(server, "%d.%d.%d.%d", ipbytes[0], ipbytes[1], ipbytes[2], ipbytes[3]);
    client->setTimeout(200);
    if (!client->connect(server, port)) {
      DEBUG_PRINT(server);
      DEBUG_PRINT(":");
      DEBUG_PRINT(port);
      DEBUG_PRINT(" ");
      DEBUG_PRINTLN(F("failed."));
      client->stop();
      return HTTP_RQT_TIMEOUT;
    }

    uint8_t buffer[20];

    if (modbusTcpId >= 0xFFFE)
      modbusTcpId = 1;
    else
      modbusTcpId++;

    bool isTemp = type == SENSOR_SMT100_TEMP || type == SENSOR_TH100_TEMP;
    bool isMois = type == SENSOR_SMT100_MOIS || type == SENSOR_TH100_MOIS;
    uint8_t senstype = isTemp ? 0x00 : isMois ? 0x01 : 0x02;

    buffer[0] = (0xFF00 & modbusTcpId) >> 8;
    buffer[1] = (0x00FF & modbusTcpId);
    buffer[2] = 0;
    buffer[3] = 0;
    buffer[4] = 0;
    buffer[5] = 6;  // len
    buffer[6] = id;  // Modbus ID
    buffer[7] = 0x03;        // Read Holding Registers
    buffer[8] = 0x00;
    buffer[9] = senstype;
    buffer[10] = 0x00;
    buffer[11] = 0x01;

    client->write(buffer, 12);
#if defined(ESP8266) || defined(ESP32)
    client->flush();
#endif

    switch (type) {
      case SENSOR_SMT100_MOIS:
      case SENSOR_SMT100_TEMP:
      case SENSOR_SMT100_PMTY:
      case SENSOR_TH100_MOIS:
      case SENSOR_TH100_TEMP: {
        uint32_t stoptime = millis() + SENSOR_READ_TIMEOUT;
#if defined(ESP8266) || defined(ESP32)
        while (true) {
          if (client->available()) break;
          if (millis() >= stoptime) {
            client->stop();
            DEBUG_PRINT(F("Sensor "));
            DEBUG_PRINT(nr);
            DEBUG_PRINTLN(F(" timeout read!"));
            return HTTP_RQT_TIMEOUT;
          }
          delay(5);
        }

        int n = client->read(buffer, 11);
        if (n < 11) n += client->read(buffer + n, 11 - n);
#else
        int n = 0;
        while (true) {
          n = client->read(buffer, 11);
          if (n < 11) n += client->read(buffer + n, 11 - n);
          if (n > 0) break;
          if (millis() >= stoptime) {
            client->stop();
            DEBUG_PRINT(F("Sensor "));
            DEBUG_PRINT(nr);
            DEBUG_PRINT(F(" timeout read!"));
            return HTTP_RQT_TIMEOUT;
          }
          delay(5);
        }
#endif
        client->stop();
        DEBUG_PRINT(F("Sensor "));
        DEBUG_PRINT(nr);
        if (n != 11) {
          DEBUG_PRINT(F(" returned "));
          DEBUG_PRINT(n);
          DEBUG_PRINTLN(F(" bytes??"));
          return n == 0 ? HTTP_RQT_EMPTY_RETURN : HTTP_RQT_TIMEOUT;
        }
        if (buffer[0] != (0xFF00 & modbusTcpId) >> 8 ||
            buffer[1] != (0x00FF & modbusTcpId)) {
          DEBUG_PRINT(F(" returned transaction id "));
          DEBUG_PRINTLN((uint16_t)((buffer[0] << 8) + buffer[1]));
          return HTTP_RQT_NOT_RECEIVED;
        }
        if ((buffer[6] != id && id != 253)) {  // 253 is broadcast
          DEBUG_PRINT(F(" returned sensor id "));
          DEBUG_PRINTLN((int)buffer[0]);
          return HTTP_RQT_NOT_RECEIVED;
        }

        // Valid result:
        last_native_data = (buffer[9] << 8) | buffer[10];
        DEBUG_PRINT(F(" native: "));
        DEBUG_PRINT(last_native_data);

        // Convert to readable value:
        switch (type) {
          case SENSOR_TH100_MOIS:
          case SENSOR_SMT100_MOIS:
            last_data = ((double)last_native_data / 100.0);
            flags.data_ok = last_native_data < 10000;
            DEBUG_PRINT(F(" soil moisture %: "));
            break;
          case SENSOR_TH100_TEMP:
          case SENSOR_SMT100_TEMP:
            last_data = ((double)last_native_data / 100.0) - 100.0;
            flags.data_ok = last_native_data > 7000;
            DEBUG_PRINT(F(" temperature °C: "));
            break;
          case SENSOR_SMT100_PMTY:
            last_data = ((double)last_native_data / 100.0);
            flags.data_ok = true;
            DEBUG_PRINT(F(" permittivity DK: "));
            break;
        }
        DEBUG_PRINTLN(last_data);
        return flags.data_ok ? HTTP_RQT_SUCCESS : HTTP_RQT_NOT_RECEIVED;
      }
    }

    return HTTP_RQT_NOT_RECEIVED;
}

int ModbusRtuSensor::setAddress(uint8_t newAddress) {
    return set_sensor_address_ip(this, newAddress);
}

// class-level implementation
bool ModbusRtuSensor::sendCommand(uint32_t ip, uint16_t port, uint8_t address, uint16_t reg,uint16_t data, bool isbit) {
#if defined(ARDUINO)
  Client *client;
  #if defined(ESP8266) || defined(ESP32)
    WiFiClient wifiClient;
    client = &wifiClient;
  #else
    EthernetClient etherClient;
    client = &etherClient;
  #endif
#else
  EthernetClient etherClient;
  EthernetClient *client = &etherClient;
#endif

#if defined(ARDUINO)
  IPAddress _ip(ip);
  unsigned char ips[4] = {_ip[0], _ip[1], _ip[2], _ip[3]};
#else
  unsigned char ips[4];
  ips[3] = (unsigned char)((ip >> 24) & 0xFF);
  ips[2] = (unsigned char)((ip >> 16) & 0xFF);
  ips[1] = (unsigned char)((ip >> 8) & 0xFF);
  ips[0] = (unsigned char)((ip & 0xFF));
#endif
  char server[20];
  sprintf(server, "%d.%d.%d.%d", ips[0], ips[1], ips[2], ips[3]);
  client->setTimeout(200);
  if (!client->connect(server, port)) {
    DEBUG_PRINT(server);
    DEBUG_PRINT(":");
    DEBUG_PRINT(port);
    DEBUG_PRINT(" ");
    DEBUG_PRINTLN(F("failed."));
    client->stop();
    return false;
  }

  uint8_t buffer[20];

  if (modbusTcpId >= 0xFFFE)
    modbusTcpId = 1;
  else
    modbusTcpId++;

  buffer[0] = (0xFF00 & modbusTcpId) >> 8;
  buffer[1] = (0x00FF & modbusTcpId);
  buffer[2] = 0;
  buffer[3] = 0;
  buffer[4] = 0;
  buffer[5] = 6;  // len
  buffer[6] = address;  // Modbus ID
  buffer[7] = isbit?0x05:0x06;        // Write Registers
  buffer[8] = (reg >> 8) & 0xFF;  // high byte of register address
  buffer[9] = reg & 0xFF;  // low byte
  if (isbit) {
    buffer[10] = data?0xFF:0x00;
    buffer[11] = 0x00;
  } else {
    buffer[10] = (data >> 8) & 0xFF;  // high byte
    buffer[11] = data & 0xFF;  // low byte
  }

  client->write(buffer, 12);
  #if defined(ESP8266) || defined(ESP32)
    client->flush();
  #endif
  client->stop();
  return true;
}

int ModbusRtuSensor::setAddressIp(SensorBase *sensor, uint8_t new_address) {
#if defined(ARDUINO)
  Client *client;
  #if defined(ESP8266) || defined(ESP32)
    WiFiClient wifiClient;
    client = &wifiClient;
  #else
    EthernetClient etherClient;
    client = &etherClient;
  #endif
#else
  EthernetClient etherClient;
  EthernetClient *client = &etherClient;
#endif
#if defined(ESP8266) || defined(ESP32)
  IPAddress _ip(sensor->ip);
  unsigned char ip[4] = {_ip[0], _ip[1], _ip[2], _ip[3]};
#else
  unsigned char ip[4];
  ip[3] = (unsigned char)((sensor->ip >> 24) & 0xFF);
  ip[2] = (unsigned char)((sensor->ip >> 16) & 0xFF);
  ip[1] = (unsigned char)((sensor->ip >> 8) & 0xFF);
  ip[0] = (unsigned char)((sensor->ip & 0xFF));
#endif
  char server[20];
  sprintf(server, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  client->setTimeout(200);
  if (!client->connect(server, sensor->port)) {
    DEBUG_PRINT(F("Cannot connect to "));
    DEBUG_PRINT(server);
    DEBUG_PRINT(":");
    DEBUG_PRINTLN(sensor->port);
    client->stop();
    return HTTP_RQT_CONNECT_ERR;
  }

  uint8_t buffer[20];

  if (modbusTcpId >= 0xFFFE)
    modbusTcpId = 1;
  else
    modbusTcpId++;

  buffer[0] = (0xFF00 & modbusTcpId) >> 8;
  buffer[1] = (0x00FF & modbusTcpId);
  buffer[2] = 0;
  buffer[3] = 0;
  buffer[4] = 0;
  buffer[5] = 6;  // len
  buffer[6] = sensor->id;
  buffer[7] = 0x06;
  buffer[8] = 0x00;
  buffer[9] = 0x04;
  buffer[10] = 0x00;
  buffer[11] = new_address;

  client->write(buffer, 12);
  #if defined(ESP8266) || defined(ESP32)
    client->flush();
  #endif

  int n = client->read(buffer, 12);
  client->stop();
  DEBUG_PRINT(F("Sensor "));
  DEBUG_PRINT(sensor->nr);
  if (n != 12) {
    DEBUG_PRINT(F(" returned "));
    DEBUG_PRINT(n);
    DEBUG_PRINT(F(" bytes??"));
    return n == 0 ? HTTP_RQT_EMPTY_RETURN : HTTP_RQT_TIMEOUT;
  }
  if (buffer[0] != (0xFF00 & modbusTcpId) >> 8 ||
      buffer[1] != (0x00FF & modbusTcpId)) {
    DEBUG_PRINT(F(" returned transaction id "));
    DEBUG_PRINTLN((uint16_t)((buffer[0] << 8) + buffer[1]));
    return HTTP_RQT_NOT_RECEIVED;
  }
  if ((buffer[6] != sensor->id && sensor->id != 253)) {  // 253 is broadcast
    DEBUG_PRINT(F(" returned sensor id "));
    DEBUG_PRINT((int)buffer[0]);
    return HTTP_RQT_NOT_RECEIVED;
  }
  sensor->id = new_address;
  sensor_save();
  return HTTP_RQT_SUCCESS;
}

bool send_modbus_rtu_command(uint32_t ip, uint16_t port, uint8_t address, uint16_t reg,uint16_t data, bool isbit) {
  return ModbusRtuSensor::sendCommand(ip, port, address, reg, data, isbit);
}

int set_sensor_address_ip(SensorBase *sensor, uint8_t new_address) {
  return ModbusRtuSensor::setAddressIp(sensor, new_address);
}

void ModbusRtuSensor::toJson(ArduinoJson::JsonObject obj) const {
  SensorBase::toJson(obj);
  
  // RS485/Modbus-specific fields
  uint16_t rs = 0;
  rs |= (rs485_flags.parity & 0x3) << 0;
  rs |= (rs485_flags.stopbits & 0x1) << 2;
  rs |= (rs485_flags.speed & 0x7) << 3;
  rs |= (rs485_flags.swapped & 0x1) << 6;
  rs |= (rs485_flags.datatype & 0x7) << 7;
  obj["rs485flags"] = rs;
  obj["rs485code"] = rs485_code;
  obj["rs485reg"] = rs485_reg;
}

void ModbusRtuSensor::fromJson(ArduinoJson::JsonVariantConst obj) {
  SensorBase::fromJson(obj);
  
  // RS485/Modbus-specific fields
  if (obj.containsKey("rs485flags")) {
    uint16_t rs = obj["rs485flags"];
    rs485_flags.parity = (rs >> 0) & 0x3;
    rs485_flags.stopbits = (rs >> 2) & 0x1;
    rs485_flags.speed = (rs >> 3) & 0x7;
    rs485_flags.swapped = (rs >> 6) & 0x1;
    rs485_flags.datatype = (rs >> 7) & 0x7;
  }
  if (obj.containsKey("rs485code")) rs485_code = obj["rs485code"];
  if (obj.containsKey("rs485reg")) rs485_reg = obj["rs485reg"];
}
