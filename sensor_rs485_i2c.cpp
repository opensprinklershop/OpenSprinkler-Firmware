/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Utility functions
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
#include "sensor_rs485_i2c.h"
#include "sensors.h"
#include "OpenSprinkler.h"
#include "opensprinkler_server.h"

#if defined(ESP8266) || defined(ESP32)

extern OpenSprinkler os;

int i2c_rs485_addr = 0;
int active_i2c_RS485 = 0;
int active_i2c_RS485_mode = 0;
int i2c_pending = 0;

// SC16IS752 Register Adressen (Teilauswahl)
#define REG_RHR     0x00
#define REG_THR     0x00
#define REG_DLL     0x00
#define REG_DLH     0x01
#define REG_FCR     0x02
#define REG_LCR     0x03
#define REG_MCR     0x04
#define REG_LSR     0x05
#define REG_IOD     0x0A
#define REG_IOS     0x0B
#define REG_IOC     0x0E
#define REG_EFCR    0x0F

void sensor_rs485_i2c_init() {
   if (detect_i2c(ASB_I2C_RS485_ADDR)) {    // 0x48
        i2c_rs485_addr = ASB_I2C_RS485_ADDR;
        DEBUG_PRINTF(F("Found I2C RS485 at address %02x\n"), ASB_I2C_RS485_ADDR);
        add_asb_detected_boards(ASB_I2C_RS485);
    } else if (os.hw_rev != 3 && detect_i2c(ASB_I2C_RS485_ADDR1)) {    //dev adapters, 0x50 unuseable hw_rev=3 
        i2c_rs485_addr = ASB_I2C_RS485_ADDR1;
        DEBUG_PRINTF(F("Found I2C RS485 at address %02x\n"), ASB_I2C_RS485_ADDR1);
        add_asb_detected_boards(ASB_I2C_RS485);
    }
}

void writeSC16Register(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(i2c_rs485_addr); // Wire Lib erwartet 7-bit Adresse
  Wire.write( (reg << 3) | 0x00 ); // Befehlsbyte für Schreibzugriff
  Wire.write(value);
  Wire.endTransmission();
}

uint8_t readSC16Register(uint8_t reg) {
  uint8_t result =0;
  Wire.beginTransmission(i2c_rs485_addr);
  Wire.write( (reg << 3) | 0x80 ); // Befehlsbyte für Lesezugriff
  Wire.endTransmission(false); // Kein Stop, um Re-Start zu senden

  Wire.requestFrom(i2c_rs485_addr, 1);
  if (Wire.available()) {
    result = Wire.read();
  }
  delay(1);
  return result;
}

void UART_sendByte(uint8_t data) {
  // Warten bis der THR (Transmit Holding Register) leer ist
  while (!(readSC16Register(REG_LSR) & 0x20)) // LSR Bit 5 (THRE)
    delay(1); 
  writeSC16Register(REG_THR, data);
}

void UART_sendBytes(uint8_t data[], uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    UART_sendByte(data[i]);
  }
}

uint8_t UART_receiveByte() {
  // Warten bis Daten im RHR (Receive Holding Register) verfügbar sind
  while (!(readSC16Register(REG_LSR) & 0x01)); // LSR Bit 0 (DR)
  return readSC16Register(REG_RHR);
}

bool UART_available() {
    return (readSC16Register(REG_LSR) & 0x01);
}

uint8_t UART_readBytes(uint8_t* buffer, uint8_t len, uint16_t timeout) {
  uint8_t count = 0;
  uint32_t startTime = millis();
  while (count < len) {
    if (UART_available()) {
      buffer[count++] = UART_receiveByte();
    } else {
      if (millis() - startTime >= timeout) {
        break; // Timeout erreicht
      }
    }
  }
  return count;
}

void set_RS485_Mode(bool transmitMode) {
    writeSC16Register(REG_IOD, 0x80); // GPIO7 Output
    uint8_t ioState = readSC16Register(REG_IOS);

    if (transmitMode) {
      // Bei RS485: DE=LOW, RE=HIGH -> Pin muss LOW sein
      ioState &= ~0x80; // Löscht Bit 7 auf LOW
    } else {
      // Bei RS485: DE=HIGH, RE=LOW -> Pin muss HIGH sein
      ioState |= 0x80; // Setzt Bit 7 auf HIGH
    }
    writeSC16Register(REG_IOS, ioState);
}

uint16_t datatype2length(uint8_t datatype) {
  switch (datatype) {
    case RS485FLAGS_DATATYPE_UINT16:
    case RS485FLAGS_DATATYPE_INT16:
      return 1;
    case RS485FLAGS_DATATYPE_UINT32:
    case RS485FLAGS_DATATYPE_INT32:
    case RS485FLAGS_DATATYPE_FLOAT:
      return 2;
    case RS485FLAGS_DATATYPE_DOUBLE:
      return 4;
    default:
      return 1; // Default to 2 bytes
  }
}

uint32_t generic_baud(uint8_t speed) {
  switch (speed) {
    case 0: return 9600;
    case 1: return 19200;
    case 2: return 38400;
    case 3: return 57600;
    case 4: return 115200;
    default: return 9600;
  }
}

// LCR Bit-Masken
#define LCR_DATALEN_8  0x03  // 8 Datenbits
#define LCR_STOP_1     0x00  // 1 Stoppbit
#define LCR_STOP_2     0x04  // 2 Stoppbits
#define LCR_PAR_NONE   0x00  // Keine Parität
#define LCR_PAR_ODD    0x08  // Ungerade Parität (PEN=1, EPS=0)
#define LCR_PAR_EVEN   0x18  // Gerade Parität (PEN=1, EPS=1)
#define LCR_DLAB       0x80  // Divisor Latch Access Bit (für Baudrate)

void init_SC16IS752(uint32_t baudrate, uint8_t use2stopbits, uint parity) {
  DEBUG_PRINTLN(F("i2c_rs485: init"));

  uint8_t baudf = (uint32_t)(8000000 / (16 * baudrate)); // Assuming 8 MHz clock and 9600 baud rate
  uint8_t lcr = LCR_DATALEN_8 | (use2stopbits ? LCR_STOP_2 : LCR_STOP_1) | 
                (parity == 0 ? LCR_PAR_NONE : (parity == 1 ? LCR_PAR_EVEN : LCR_PAR_ODD));
  DEBUG_PRINTF(F("i2c_rs485: baudf=%02x lcr=%02x\n"), baudf, lcr);
  writeSC16Register(REG_LCR, LCR_DLAB);// Enable access to the baud rate registers
  writeSC16Register(REG_DLL, baudf); // Set baud rate to 9600 (assuming 8 MHz clock) (0x34=52=9600)
  writeSC16Register(REG_DLH, 0x00); // Set baud rate to 9600
  writeSC16Register(REG_LCR, lcr); // parity+stopbits (0x1B=1 stop bit, parity even, 8 data bits)
  set_RS485_Mode(true);
  writeSC16Register(REG_EFCR, 0x30); // 0x30 = 00110000 RS485 Mode Enable + RTS Inversion
}
/**
 * @brief I2C to RS485 Interface
 *        Alternative I2C Board for any RS485 Sensors
 *        (SC16IS752 and MAX485)
 * @param sensor
 * @return int
 */
int RS485I2CSensor::read(unsigned long /*time*/) {
  if (!(get_asb_detected_boards() & ASB_I2C_RS485)) 
    return HTTP_RQT_NOT_RECEIVED;

  if (active_i2c_RS485 > 0 && active_i2c_RS485 != (int)nr) {
    repeat_read = 1;
    SensorBase *t = sensor_by_nr(active_i2c_RS485);
    if (!t || !t->flags.enable)
      active_i2c_RS485 = 0; //breakout
    if (i2c_pending == 0)
      i2c_pending = nr;
    return HTTP_RQT_NOT_RECEIVED;
  }

  DEBUG_PRINTF(F("read_sensor_i2c_rs485: %d %s m=%d\n"), nr, name, active_i2c_RS485_mode);

  if (active_i2c_RS485 != (int)nr) {  
    active_i2c_RS485 = nr;
    if (i2c_pending != (int)nr)
      active_i2c_RS485_mode = 0;
    i2c_pending = 0;
  } 
  bool isGeneric = type == SENSOR_RS485;

  //Init chip
  if (active_i2c_RS485_mode == 0) { // Init SC16IS752 for RS485:
    DEBUG_PRINTLN(F("i2c_rs485: INIT"));
    uint32_t baudrate = isGeneric ? generic_baud(rs485_flags.speed) : 9600;
    uint8_t stopbits = isGeneric ? rs485_flags.stopbits : 0; // 0=1 stopbit
    uint8_t parity = isGeneric ? rs485_flags.parity : 1; // 1=even parity default for truebner
    init_SC16IS752(baudrate, stopbits, parity);
    active_i2c_RS485_mode = 1;
  } 

  if (active_i2c_RS485_mode == 1) {
    DEBUG_PRINTLN(F("i2c_rs485: POWER ON"));
    set_RS485_Mode(true);
    writeSC16Register(REG_MCR, 0x03); // Enable RTS and Auto RTS/CTS
    writeSC16Register(REG_FCR, 0x07); // FIFO Enable (FCR): Enable FIFOs, Reset TX/RX FIFO (0x07)

    active_i2c_RS485_mode = 2;
    repeat_read = 1;
    return HTTP_RQT_NOT_RECEIVED;
  }

  bool isTemp = type == SENSOR_SMT100_TEMP || type == SENSOR_TH100_TEMP;
  bool isMois = type == SENSOR_SMT100_MOIS || type == SENSOR_TH100_MOIS;
  uint8_t code = isGeneric ? rs485_code : 0x03; // Read Holding Registers
  uint16_t reg = isGeneric ? rs485_reg : isTemp ? 0x00 : isMois ? 0x01 : 0x02;
  uint16_t reg_count = isGeneric ? datatype2length(rs485_flags.datatype) : 0x01;

  // Send Request
  if (active_i2c_RS485_mode == 2) {
    DEBUG_PRINT(F("i2c_rs485: Send Request:"));
    uint8_t request[8];
    request[0] = id;
    request[1] = code; // Function Code
    request[2] = highByte(reg); // Register Address
    request[3] = lowByte(reg); // Register Address
    request[4] = highByte(reg_count); // Number of Registers to read (1 or more)
    request[5] = lowByte(reg_count); // Number of Registers to read (1 or more)
    uint16_t crc = CRC16(request, 6); // little-endian!
    request[6] = lowByte(crc); // CRC Low Byte
    request[7] = highByte(crc); // CRC High Byte
    for (int i = 0; i < 8; i++) {
      DEBUG_PRINTF(F(" %02x"), request[i]);
    }
    DEBUG_PRINTLN();
    writeSC16Register(REG_FCR, 0x07); // FIFO Enable (FCR): Enable FIFOs, Reset TX/RX FIFO (0x07)
    UART_sendBytes(request, 8);
    active_i2c_RS485_mode = 3;
    repeat_read = 1;
    return HTTP_RQT_NOT_RECEIVED;
  }

  // Read Response
  if (active_i2c_RS485_mode == 3) {
    DEBUG_PRINT(F("i2c_rs485: Read Response:"));
    uint8_t response[20];
    uint8_t expected_length = 5 + (reg_count * 2); // 5 bytes overhead + 2 bytes per register
    uint8_t len = UART_readBytes(response, expected_length, 500); // timeout 500ms
    for (int i = 0; i < len; i++) {
      DEBUG_PRINTF(F(" %02x"), response[i]);
    }
    DEBUG_PRINTLN();
    // Expected Response (16bit):
    // Byte 0: Slave Address
    // Byte 1: Function Code
    // Byte 2: Byte Count
    // Byte 3: Data Low Byte
    // Byte 4: Data High Byte
    // Byte 5: CRC Low Byte
    // Byte 6: CRC High Byte
    uint16_t crc = len == expected_length?CRC16(response, expected_length-2):0xFFFF;
    if (len != expected_length || response[0] != id || response[1] != code || response[2] != reg_count*2 ||
        response[expected_length-2] != lowByte(crc) || response[expected_length-1] != highByte(crc)) {
          
      DEBUG_PRINTLN(F("read_sensor_i2c_rs485: invalid response"));
      DEBUG_PRINT(F("len="));
      DEBUG_PRINTLN(len);
      repeat_read = 0;
      flags.data_ok = false;
      active_i2c_RS485 = 0;
      active_i2c_RS485_mode = 0;
      set_RS485_Mode(false);
      return HTTP_RQT_NOT_RECEIVED;
    }

    //Extract Data
    if (!isGeneric) { // Truebner Sensor Data Extraction
      uint16_t data = (response[3] << 8) | response[4];
      DEBUG_PRINTF("read_sensor_i2c_rs485: result: %d - %d (%d %d)\n", id,
                   data, response[3], response[4]);
      double value = isTemp ? (data / 100.0) - 100.0 : (isMois ? data / 100.0 : data);
      last_native_data = data;
      last_data = value;
      flags.data_ok = true;
    } else {       // Generic Sensor Data Extraction
      uint64_t data = 0;
      for (uint8_t i = 0; i < reg_count*2; i++) {
        data <<= 8;
        if (rs485_flags.swapped) {
          data |= response[3 + ((reg_count*2 -1) - i)];
        } else {
          data |= response[3 + i];
        }
      }
      DEBUG_PRINTF("read_sensor_i2c_rs485: result: %d - %llx\n", id, data);
      last_native_data = data; // raw data - only 32bit
      double value = 0.0;
      switch (rs485_flags.datatype) {
        case RS485FLAGS_DATATYPE_UINT16:
          value = (uint16_t)data;
          break;
        case RS485FLAGS_DATATYPE_INT16:
          value = (int16_t)data;
          break;
        case RS485FLAGS_DATATYPE_UINT32:
          value = (uint32_t)data;
          break;
        case RS485FLAGS_DATATYPE_INT32:
          value = (int32_t)data;
          break;
        case RS485FLAGS_DATATYPE_FLOAT: {
          float f;
          uint32_t temp = static_cast<uint32_t>(data);
          memcpy(&f, &temp, sizeof(float));
          value = static_cast<double>(f);
          break;
        }
        case RS485FLAGS_DATATYPE_DOUBLE: {
          double d;
          uint64_t temp = data;
          memcpy(&d, &temp, sizeof(double));
          value = d;
          break;
        }
        default:
          value = static_cast<double>(static_cast<uint16_t>(data));
          break;
      }
      if (factor && divider)
        value *= (double)factor / (double)divider;
      else if (divider)
        value /= divider;
      else if (factor)
        value *= factor;
      last_native_data = data;
      last_data = value;
    }
    DEBUG_PRINTF(F("Result = %f %s\n"), last_data, getSensorUnit(this));
    flags.data_ok = true;
    repeat_read = 0;
    active_i2c_RS485 = 0;
    if (i2c_pending) {
      active_i2c_RS485_mode = 2;
    } else {
      active_i2c_RS485_mode = 0;
      set_RS485_Mode(false);
    }
    return HTTP_RQT_SUCCESS;
  }

  // Timeout
  repeat_read++;
  if (repeat_read > 4) {  // timeout
    repeat_read = 0;
    flags.data_ok = false;
    active_i2c_RS485 = 0;
    active_i2c_RS485_mode = 0;
    set_RS485_Mode(false);
    DEBUG_PRINTLN(F("i2c_rs485: timeout"));
  }
  DEBUG_PRINTLN(F("i2c_rs485: Exit"));
  return HTTP_RQT_NOT_RECEIVED;
}

int RS485I2CSensor::setAddress(uint8_t new_address) {
  if (!(get_asb_detected_boards() & ASB_I2C_RS485)) 
    return HTTP_RQT_NOT_RECEIVED;

  if (new_address == 0 || new_address > 247)
    return HTTP_RQT_CONNECT_ERR;

  DEBUG_PRINTF(F("set_sensor_address_i2c_rs485: %d %s\n"), nr, name)
  
  if (active_i2c_RS485 > 0 && active_i2c_RS485 != (int)nr) {
    repeat_read = 1;
    SensorBase *t = sensor_by_nr(active_i2c_RS485);
    if (!t || !t->flags.enable)
      active_i2c_RS485 = 0; //breakout
    return HTTP_RQT_NOT_RECEIVED;
  }

  // Init chip
  init_SC16IS752(9600, 0, 1); //Truebner default: 9600, 1 stopbit, even parity
  active_i2c_RS485_mode = 0;

  // Switch power on
  set_RS485_Mode(true);
  writeSC16Register(REG_FCR, 0x07); // FIFO Enable (FCR): Enable FIFOs, Reset TX/RX FIFO (0x07)
  writeSC16Register(REG_MCR, 0x03); // Enable RTS and Auto RTS/CTS

  DEBUG_PRINT(F("i2c_rs485: Send Request:"));
  uint8_t request[8];
  request[0] = 253;
  request[1] = 0x06; // change adress
  request[2] = 0x00;    
  request[3] = 0x04; // Register Address
  request[4] = 0x00;
  request[5] = new_address; // Number of Registers to read (1
  uint16_t crc = CRC16(request, 6);
  request[6] = lowByte(crc); // CRC Low Byte
  request[7] = highByte(crc); // CRC High Byte
  for (int i = 0; i < 8; i++) {
    DEBUG_PRINTF(F(" %02x"), request[i]);
  }
  DEBUG_PRINTLN();

  UART_sendBytes(request, 8);
  delay(10);
  uint8_t response[7];
  int len = UART_readBytes(response, 7, 100); // timeout 100ms
  for (int i = 0; i < len; i++) {
    DEBUG_PRINTF(F(" %02x"), response[i]);
  }
  DEBUG_PRINTLN();

  return HTTP_RQT_SUCCESS;
}

// class-level helper
int RS485I2CSensor::sendCommand(uint8_t address, uint16_t reg, uint16_t data, bool isbit) {
  if (!(get_asb_detected_boards() & ASB_I2C_RS485)) 
    return HTTP_RQT_NOT_RECEIVED;

  DEBUG_PRINTF(F("send_i2c_rs485_command: %d %d %d %d\n"), address, reg, data, isbit);
  
  if (active_i2c_RS485 > 0) {
    DEBUG_PRINT(F("cant' send, allocated by sensor "));
    DEBUG_PRINTLN(active_i2c_RS485);
    SensorBase *t = sensor_by_nr(active_i2c_RS485);
    if (!t || !t->flags.enable)
      active_i2c_RS485 = 0; //breakout
    return HTTP_RQT_NOT_RECEIVED;
  }

  init_SC16IS752(9600, 0, 0); // 9600, 1 stopbit, no parity
  active_i2c_RS485_mode = 0;

  // Switch power on
  set_RS485_Mode(true);
  writeSC16Register(REG_FCR, 0x07); // FIFO Enable (FCR): Enable FIFOs, Reset TX/RX FIFO (0x07)
  writeSC16Register(REG_MCR, 0x03); // Enable RTS and Auto RTS/CTS

  DEBUG_PRINT(F("i2c_rs485: Send Request:"));
  uint8_t request[8];
  request[0] = address;  // Modbus ID
  request[1] = isbit?0x05:0x06;        // Write Registers
  request[2] = reg >> 8;  // high byte of register address
  request[3] = reg & 0xFF;  // low byte
  if (isbit) {
    request[4] = data?0xFF:0x00;
    request[5] = 0x00;
  } else {
    request[4] = data >> 8;  // high byte
    request[5] = data & 0xFF;  // low byte
  }
  uint16_t crc = CRC16(request, 6);
  request[6] = lowByte(crc); // CRC Low Byte
  request[7] = highByte(crc); // CRC High Byte
  for (int i = 0; i < 8; i++) {
    DEBUG_PRINTF(F(" %02x"), request[i]);
  }
  DEBUG_PRINTLN();

  UART_sendBytes(request, 8);
  delay(10);
  uint8_t response[7];
  int len = UART_readBytes(response, 7, 100); // timeout 100ms
  for (int i = 0; i < len; i++) {
    DEBUG_PRINTF(F(" %02x"), response[i]);
  }
  DEBUG_PRINTLN();
  
  return HTTP_RQT_SUCCESS;
}

void RS485I2CSensor::toJson(ArduinoJson::JsonObject obj) const {
  SensorBase::toJson(obj);
  
  // RS485-specific fields
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

void RS485I2CSensor::fromJson(ArduinoJson::JsonVariantConst obj) {
  SensorBase::fromJson(obj);
  
  // RS485-specific fields
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

void RS485I2CSensor::emitJson(BufferFiller& bfill) const {
  ArduinoJson::JsonDocument doc;
  ArduinoJson::JsonObject obj = doc.to<ArduinoJson::JsonObject>();
  toJson(obj);
  
  // Serialize to string and output
  String jsonStr;
  ArduinoJson::serializeJson(doc, jsonStr);
  bfill.emit_p(PSTR("$S"), jsonStr.c_str());
}

// Backwards-compatible wrapper
int send_i2c_rs485_command(uint8_t address, uint16_t reg, uint16_t data, bool isbit) {
  return RS485I2CSensor::sendCommand(address, reg, data, isbit);
}

#endif
