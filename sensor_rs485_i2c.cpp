/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Utility functions
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
#include "sensor_rs485_i2c.h"
#include "sensors.h"
#include "OpenSprinkler.h"
#include "opensprinkler_server.h"

#if defined(ESP8266) || defined(ESP32)

#if defined(ESP32C5)
#include "soc/lp_aon_reg.h"
#endif

extern OpenSprinkler os;

int i2c_rs485_addr = 0;
int active_i2c_RS485 = 0;
int active_i2c_RS485_mode = 0;
int i2c_pending = 0;
static bool i2c_rs485_wire_started = false;

// EFCR Auto-RS485 Richtungssteuerung (RTS steuert DE/RE des Transceivers):
//   Bit4 = Transmitter steuert RTS, Bit5 = RTS-Polaritaet invertiert.
//   0x30 = RTS HIGH beim Senden (DE active-high + /RE active-low, zusammengelegt).
//   0x10 = RTS LOW beim Senden (falls DE/RE-Logik invertiert verdrahtet ist).
uint8_t i2c_rs485_efcr = 0x30;

// SC16IS752 Register Adressen (Teilauswahl)
#define REG_RHR     0x00
#define REG_THR     0x00
#define REG_DLL     0x00
#define REG_DLH     0x01
#define REG_FCR     0x02
#define REG_LCR     0x03
#define REG_MCR     0x04
#define REG_LSR     0x05
#define REG_SPR     0x07  // Scratch Pad Register (kein HW-Effekt, für Chip-Erkennung)
#define REG_IOD     0x0A
#define REG_IOS     0x0B
#define REG_IOC     0x0E
#define REG_EFCR    0x0F

// Schreib/Rücklese-Test auf dem SC16IS752 Scratch Pad Register (REG_SPR).
// Stellt sicher, dass wirklich ein SC16IS752 antwortet und nicht ein anderer I2C-Teilnehmer.
// Ein einzelner Read-back direkt nach dem Write scheitert bei manchen Boards/Bussen
// (0x48 kollidiert mit dem ADS1115) durch transientes I2C-Timing -> mit Settle-Delay
// und mehreren Versuchen absichern. Zwei komplementäre Muster verhindern, dass ein
// schwebender Bus oder ein anderes Device zufällig als SC16IS752 durchgeht.
static bool sc16is752_scratch_test_pattern(int addr, uint8_t test_val) {
  Wire.beginTransmission(addr);
  Wire.write((REG_SPR << 3) | 0x00);  // Schreibzugriff auf SPR
  Wire.write(test_val);
  if (Wire.endTransmission() != 0) return false;

  delay(1);  // SPR vor dem Rücklesen einschwingen lassen (wie readSC16Register)
  Wire.beginTransmission(addr);
  Wire.write((REG_SPR << 3) | 0x80);  // Lesezugriff auf SPR
  Wire.endTransmission(false);
  Wire.requestFrom(addr, 1);
  if (!Wire.available()) return false;
  return (Wire.read() == test_val);
}

static bool sc16is752_scratch_test(int addr) {
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    if (sc16is752_scratch_test_pattern(addr, 0xA5) &&
        sc16is752_scratch_test_pattern(addr, 0x5A))
      return true;
    delay(2);
  }
  return false;
}

static bool ensure_i2c_rs485_bus() {
  if (!i2c_rs485_wire_started) {
#if defined(ESP32)
    #if defined(ESP32C5)
    // Force GPIO 0/1 to stay in digital mode (LP domain can steal them)
    REG_CLR_BIT(LP_AON_GPIO_MUX_REG, BIT(SDA) | BIT(SCL));
    #endif
    if (!Wire.begin()) {
      DEBUG_PRINTLN(F("i2c_rs485: Wire.begin failed"));
      return false;
    }
#else
    Wire.begin();
#endif
    Wire.setClock(100000);
    i2c_rs485_wire_started = true;
  }
  return true;
}

void sensor_rs485_i2c_init() {
  if (!ensure_i2c_rs485_bus()) {
    return;
  }
  if (detect_i2c(ASB_I2C_RS485_ADDR)) {    // 0x4C
    if (sc16is752_scratch_test(ASB_I2C_RS485_ADDR)) {
      i2c_rs485_addr = ASB_I2C_RS485_ADDR;
      DEBUG_PRINTF(F("Found I2C RS485 at address %02x\n"), ASB_I2C_RS485_ADDR);
      add_asb_detected_boards(ASB_I2C_RS485);
    }
  }
  else if (detect_i2c(ASB_I2C_RS485_ADDR_ALT)) {    // 0x48 wrong address, but for backward compatibility we still support it
    if (sc16is752_scratch_test(ASB_I2C_RS485_ADDR_ALT)) {
      i2c_rs485_addr = ASB_I2C_RS485_ADDR_ALT;
      DEBUG_PRINTF(F("Found I2C RS485 at address %02x\n"), ASB_I2C_RS485_ADDR_ALT);
      add_asb_detected_boards(ASB_I2C_RS485);
    }
    else {
      // 0x48 is shared with the ADS1115 analog board; a failed scratch test here
      // is expected for analog boards but is a hard failure for an actual RS485
      // board (no reads, no set-address). Log it so it is diagnosable.
      DEBUG_PRINTF(F("I2C dev at %02x is not an SC16IS752 (scratch test failed)\n"), ASB_I2C_RS485_ADDR_ALT);
    }
  }
}

void writeSC16Register(uint8_t reg, uint8_t value) {
  if (!ensure_i2c_rs485_bus() || i2c_rs485_addr == 0) return;
  Wire.beginTransmission(i2c_rs485_addr); // Wire Lib erwartet 7-bit Adresse
  Wire.write( (reg << 3) | 0x00 ); // Befehlsbyte für Schreibzugriff
  Wire.write(value);
  Wire.endTransmission();
}

uint8_t readSC16Register(uint8_t reg) {
  uint8_t result =0;
  if (!ensure_i2c_rs485_bus() || i2c_rs485_addr == 0) return result;
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
  uint32_t start = millis();
  while (!(readSC16Register(REG_LSR) & 0x20)) { // LSR Bit 5 (THRE)
    if (millis() - start > 50) {
      DEBUG_PRINTLN(F("i2c_rs485: THR timeout"));
      return;
    }
    delay(1);
  }
  writeSC16Register(REG_THR, data);
}

void UART_sendBytes(uint8_t data[], uint8_t len) {
  // Blast all bytes directly to the TX FIFO in a SINGLE I2C transaction!
  // If we use separate transactions, FreeRTOS might preempt the task between bytes,
  // causing the TX FIFO to run empty, which causes Auto-RTS to drop the line mid-frame!
  if (!ensure_i2c_rs485_bus() || i2c_rs485_addr == 0) return;
  Wire.beginTransmission(i2c_rs485_addr);
  Wire.write(REG_THR << 3); // Select THR register
  for (uint8_t i = 0; i < len; i++) {
    Wire.write(data[i]);
  }
  Wire.endTransmission();
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

// GPIO7 des SC16IS752 schaltet ueber einen TPS22917 Load-Switch die Versorgung
// (VCC/Pin 1) des CA-IS3092W Transceivers. GPIO7 HIGH = Transceiver an.
// Die DE/RE-Richtungsumschaltung macht die Hardware selbst ueber RTS (EFCR Auto-RS485).
void set_rs485_power(bool on) {
    writeSC16Register(REG_IOD, 0x80); // GPIO7 Output
    uint8_t ioState = readSC16Register(REG_IOS);

    if (on) {
      ioState |= 0x80; // GPIO7 HIGH -> Load-Switch an
    } else {
      ioState &= ~0x80; // GPIO7 LOW -> Load-Switch aus
    }
    writeSC16Register(REG_IOS, ioState);
    if (on) {
      // Isolierten Transceiver nach dem Einschalten einschwingen lassen,
      // bevor das erste Byte gesendet wird (sonst geht der erste Frame verloren).
      delay(2);
    }
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
  // DEBUG_PRINTLN(F("i2c_rs485: init"));

  uint8_t baudf = (uint32_t)(8000000 / (16 * baudrate)); // Assuming 8 MHz clock and 9600 baud rate
  uint8_t lcr = LCR_DATALEN_8 | (use2stopbits ? LCR_STOP_2 : LCR_STOP_1) | 
                (parity == 0 ? LCR_PAR_NONE : (parity == 1 ? LCR_PAR_EVEN : LCR_PAR_ODD));
  // DEBUG_PRINTF(F("i2c_rs485: baudf=%02x lcr=%02x\n"), baudf, lcr);
  writeSC16Register(REG_LCR, LCR_DLAB);// Enable access to the baud rate registers
  writeSC16Register(REG_DLL, baudf); // Set baud rate to 9600 (assuming 8 MHz clock) (0x34=52=9600)
  writeSC16Register(REG_DLH, 0x00); // Set baud rate to 9600
  writeSC16Register(REG_LCR, lcr); // parity+stopbits (0x1B=1 stop bit, parity even, 8 data bits)
  set_rs485_power(false);
  writeSC16Register(REG_EFCR, i2c_rs485_efcr); // Auto-RS485 RTS Richtungssteuerung (siehe i2c_rs485_efcr)
}
/**
 * @brief I2C to RS485 Interface
 *        Alternative I2C Board for any RS485 Sensors
 *        (SC16IS752 and MAX485)
 * @param sensor
 * @return int
 */
int RS485I2CSensor::read(unsigned long time) {
  if (i2c_rs485_addr == 0) {
    sensor_rs485_i2c_init();
  }
  if (!(get_asb_detected_boards() & ASB_I2C_RS485)) {
    DEBUG_PRINTLN(F("RS485 board not detected, skipping read"));
    flags.data_ok = false;
    return HTTP_RQT_NOT_RECEIVED;
  }

  if (active_i2c_RS485 > 0 && active_i2c_RS485 != (int)nr) {
    repeat_read = 1;
    SensorBase *t = sensor_by_nr(active_i2c_RS485);
    if (!t || !t->flags.enable)
      active_i2c_RS485 = 0; //breakout
    if (i2c_pending == 0)
      i2c_pending = nr;
    return HTTP_RQT_NOT_RECEIVED;
  }
  
  if (active_i2c_RS485 == 0) {
    active_i2c_RS485 = nr;
    if (i2c_pending == (int)nr)
      i2c_pending = 0;
  }
  
  bool isGeneric = type == SENSOR_MODBUS_RTU;
  bool isTemp = type == SENSOR_SMT100_TEMP || type == SENSOR_TH100_TEMP;
  bool isMois = type == SENSOR_SMT100_MOIS || type == SENSOR_TH100_MOIS;
  uint8_t code = isGeneric ? rs485_code : 0x03; // Read Holding Registers

  static uint32_t current_baud = 0;
  static uint8_t current_stop = 0xFF;
  static uint8_t current_parity = 0xFF;
  static uint32_t rs485_power_on_time = 0;
  static uint32_t rs485_request_time = 0;
  static bool rs485_power_is_on = false;

  // Mode 0: Init & Power ON
  if (active_i2c_RS485_mode == 0) {
    uint32_t baudrate = isGeneric ? generic_baud(rs485_flags.speed) : 9600;
    uint8_t stopbits = isGeneric ? rs485_flags.stopbits : 0; 
    uint8_t parity = isGeneric ? rs485_flags.parity : 1;
    
    if (baudrate != current_baud || stopbits != current_stop || parity != current_parity) {
      set_rs485_power(false); // Ensure OFF before init
      rs485_power_is_on = false;
      init_SC16IS752(baudrate, stopbits, parity);
      current_baud = baudrate;
      current_stop = stopbits;
      current_parity = parity;
    }
    
    if (!rs485_power_is_on) {
      // 1. 1s vor der Messung aktivieren
      set_rs485_power(true);
      rs485_power_is_on = true;
      rs485_power_on_time = millis();
    } else {
      // Power is already on (chained sensor).
      // Ensure a strict 20ms Modbus inter-frame silence gap before the next request!
      rs485_power_on_time = millis() - 1000 + 20;
    }
    
    writeSC16Register(REG_MCR, 0x03); // Enable RTS and Auto RTS/CTS
    writeSC16Register(REG_FCR, 0x07); // FIFO Enable, Reset
    
    active_i2c_RS485_mode = 1;
    if (repeat_read == 0) repeat_read = 1;
    return HTTP_RQT_NOT_RECEIVED;
  } 

  // Mode 1: Wait 1s, then Send Request
  if (active_i2c_RS485_mode == 1) {
    if (millis() - rs485_power_on_time < 1000) {
      return HTTP_RQT_NOT_RECEIVED; // Keep repeat_read as is
    }
    
    uint16_t reg_count = isGeneric ? datatype2length(rs485_flags.datatype) : 0x01;
    DEBUG_PRINT(F("i2c_rs485: Send Request:"));
    uint8_t request[8];
    request[0] = id;
    request[1] = code; 
    request[2] = isGeneric ? (rs485_reg >> 8) : 0x00;
    request[3] = isGeneric ? (rs485_reg & 0xFF) : (isTemp ? 0x00 : (isMois ? 0x01 : 0x02));
    request[4] = reg_count >> 8;
    request[5] = reg_count & 0xFF; 
    uint16_t crc = CRC16(request, 6);
    request[6] = lowByte(crc); 
    request[7] = highByte(crc); 
    for (int i = 0; i < 8; i++) {
       DEBUG_PRINTF(F(" %02x"), request[i]);
    }
    DEBUG_PRINTLN();
    
    writeSC16Register(REG_FCR, 0x07); // Reset TX/RX FIFO before sending
    UART_sendBytes(request, 8);
    
    rs485_request_time = millis();
    active_i2c_RS485_mode = 2;
    return HTTP_RQT_NOT_RECEIVED;
  }

  // Mode 2: Wait for response
  if (active_i2c_RS485_mode == 2) {
    // 2. Messung dauert ca 500ms, Antwort nach ca 600ms
    if (millis() - rs485_request_time < 2000 && !UART_available()) {
      return HTTP_RQT_NOT_RECEIVED;
    }
    
    DEBUG_PRINT(F("i2c_rs485: Read Response:"));
    uint16_t reg_count = isGeneric ? datatype2length(rs485_flags.datatype) : 0x01;
    uint8_t response[20];
    uint8_t expected_length = 5 + (reg_count * 2);
    
    // Read the incoming bytes. 250ms timeout to catch the rest of the frame once it started.
    uint8_t len = UART_readBytes(response, expected_length, 250); 
    for (int i = 0; i < len; i++) {
      DEBUG_PRINTF(F(" %02x"), response[i]);
    }
    DEBUG_PRINTLN("");
    
    uint16_t crc = len > 2 ? CRC16(response, len - 2) : 0;
    if (len != expected_length || response[0] != id || response[1] != code || response[2] != reg_count*2 ||
        response[expected_length-2] != lowByte(crc) || response[expected_length-1] != highByte(crc)) {
          
      DEBUG_PRINTLN(F("read_sensor_i2c_rs485: invalid response"));
      DEBUG_PRINT(F("len="));
      DEBUG_PRINTLN(len);
      
      // Retry block
      repeat_read++;
      if (repeat_read > 4) {
        repeat_read = 0;
        active_i2c_RS485 = 0;
        active_i2c_RS485_mode = 0;
        last_read = time;
        if (!i2c_pending) {
          set_rs485_power(false); // 3. deaktivieren nur wenn fertig
          rs485_power_is_on = false;
        }
        DEBUG_PRINTLN(F("i2c_rs485: timeout"));
        return HTTP_RQT_NOT_RECEIVED;
      } else {
        // Prepare for retry
        active_i2c_RS485_mode = 1; // Jump back to Mode 1 to re-send
        rs485_power_on_time = millis() - 1000; // Skip 1s wait on retry since power is already on
        return HTTP_RQT_NOT_RECEIVED;
      }
    }

    //Extract Data
    if (!isGeneric) { 
      uint16_t data = (response[3] << 8) | response[4];
      DEBUG_PRINTF(F("read_sensor_i2c_rs485: result: %d - %d (%d %d)\n"), id, data, response[3], response[4]);
      double value = isTemp ? (data / 100.0) - 100.0 : (isMois ? data / 100.0 : data);
      last_native_data = data;
      last_data = value;
      flags.data_ok = true;
    } else {       
      uint64_t data = 0;
      for (uint8_t i = 0; i < reg_count*2; i++) {
        data <<= 8;
        if (rs485_flags.swapped) {
          data |= response[3 + ((reg_count*2 -1) - i)];
        } else {
          data |= response[3 + i];
        }
      }
      DEBUG_PRINTF(F("read_sensor_i2c_rs485: result: %d - %llx\n"), id, data);
      last_native_data = data; 
      double value = 0.0;
      switch (rs485_flags.datatype) {
        case RS485FLAGS_DATATYPE_UINT16: value = (uint16_t)data; break;
        case RS485FLAGS_DATATYPE_INT16: value = (int16_t)data; break;
        case RS485FLAGS_DATATYPE_UINT32: value = (uint32_t)data; break;
        case RS485FLAGS_DATATYPE_INT32: value = (int32_t)data; break;
        case RS485FLAGS_DATATYPE_FLOAT: {
          float f; uint32_t temp = static_cast<uint32_t>(data);
          memcpy(&f, &temp, sizeof(float)); value = static_cast<double>(f); break;
        }
        case RS485FLAGS_DATATYPE_DOUBLE: {
          double d; uint64_t temp = data;
          memcpy(&d, &temp, sizeof(double)); value = d; break;
        }
        default: value = static_cast<double>(static_cast<uint16_t>(data)); break;
      }
      if (factor && divider) value *= (double)factor / (double)divider;
      else if (divider) value /= divider;
      else if (factor) value *= factor;
      last_native_data = data;
      last_data = value;
    }
    
    DEBUG_PRINTF(F("Result = %f %s\n"), last_data, getSensorUnit(this));
    flags.data_ok = true;
    repeat_read = 0;
    active_i2c_RS485 = 0;
    last_read = time;
    
    // 3. deaktivieren erst nach dem vollständigen einlesen (wenn kein weiterer wartet)
    if (!i2c_pending) {
      set_rs485_power(false);
      rs485_power_is_on = false;
    }
    
    active_i2c_RS485_mode = 0;
    return HTTP_RQT_SUCCESS;
  }
  
  return HTTP_RQT_NOT_RECEIVED;
}

int RS485I2CSensor::setAddress(uint8_t new_address) {
  if (i2c_rs485_addr == 0) {
    sensor_rs485_i2c_init();
  }
  if (!(get_asb_detected_boards() & ASB_I2C_RS485)) 
    return HTTP_RQT_NOT_RECEIVED;

  if (new_address == 0 || new_address > 247)
    return HTTP_RQT_CONNECT_ERR;

  // DEBUG_PRINTF(F("set_sensor_address_i2c_rs485: %d %s\n"), nr, name)
  
  if (active_i2c_RS485 > 0 && active_i2c_RS485 != (int)nr) {
    // setAddress is a one-shot HTTP action (no repeat_read loop): wait bounded
    // for the shared RS485 bus to free up instead of silently dropping the request.
    uint32_t bus_wait = millis();
    while (active_i2c_RS485 > 0 && active_i2c_RS485 != (int)nr) {
      SensorBase *t = sensor_by_nr(active_i2c_RS485);
      if (!t || !t->flags.enable) {
        active_i2c_RS485 = 0; //stale holder, breakout
        break;
      }
      if (millis() - bus_wait > 2000) {
        DEBUG_PRINTLN(F("i2c_rs485: setAddress bus busy, aborting"));
        return HTTP_RQT_NOT_RECEIVED;
      }
      delay(10);
    }
  }
  active_i2c_RS485 = nr; // claim the bus for the duration of this operation

  // Init chip
  init_SC16IS752(9600, 0, 1); //Truebner default: 9600, 1 stopbit, even parity
  active_i2c_RS485_mode = 0;

  // Switch power on
  set_rs485_power(true); delay(10);
  writeSC16Register(REG_FCR, 0x07); // FIFO Enable (FCR): Enable FIFOs, Reset TX/RX FIFO (0x07)
  writeSC16Register(REG_MCR, 0x03); // Enable RTS and Auto RTS/CTS

  // DEBUG_PRINT(F("i2c_rs485: Send Request:"));
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
    // DEBUG_PRINTF(F(" %02x"), request[i]);
  }
  // DEBUG_PRINTLN();

  UART_sendBytes(request, 8);
  delay(10);
  uint8_t response[8];
  int len = UART_readBytes(response, 8, 100); // timeout 100ms
  // for (int i = 0; i < len; i++) {
  //   DEBUG_PRINTF(F(" %02x"), response[i]);
  // }
  // DEBUG_PRINTLN();

  // Do not turn off RS485 power
  active_i2c_RS485 = 0;      // release the shared bus
  active_i2c_RS485_mode = 0;

  // Validate response (echo of address, func code, reg high/low)
  if (len < 6 || response[0] != 253 || response[1] != 0x06 || response[2] != 0x00 || response[3] != 0x04) {
    DEBUG_PRINTLN(F("i2c_rs485: setAddress response invalid"));
    return HTTP_RQT_NOT_RECEIVED;
  }

  // Update internal ID and save configuration
  this->id = new_address;
  sensor_save();
  return HTTP_RQT_SUCCESS;
}

// class-level helper
int RS485I2CSensor::sendCommand(uint8_t address, uint16_t reg, uint16_t data, bool isbit) {
  if (i2c_rs485_addr == 0) {
    sensor_rs485_i2c_init();
  }
  if (!(get_asb_detected_boards() & ASB_I2C_RS485)) 
    return HTTP_RQT_NOT_RECEIVED;

  DEBUG_PRINTF(F("send_i2c_rs485_command: %d %d %d %d\n"), address, reg, data, isbit);
  
  if (active_i2c_RS485 > 0) {
    // one-shot helper: wait bounded for the shared RS485 bus to free up
    // instead of silently dropping the command.
    uint32_t bus_wait = millis();
    while (active_i2c_RS485 > 0) {
      SensorBase *t = sensor_by_nr(active_i2c_RS485);
      if (!t || !t->flags.enable) {
        active_i2c_RS485 = 0; //stale holder, breakout
        break;
      }
      if (millis() - bus_wait > 2000) {
        DEBUG_PRINT(F("cant' send, allocated by sensor "));
        DEBUG_PRINTLN(active_i2c_RS485);
        return HTTP_RQT_NOT_RECEIVED;
      }
      delay(10);
    }
  }

  init_SC16IS752(9600, 0, 0); // 9600, 1 stopbit, no parity
  active_i2c_RS485_mode = 0;

  // Switch power on
  set_rs485_power(true); delay(1000);
  writeSC16Register(REG_FCR, 0x07); // FIFO Enable (FCR): Enable FIFOs, Reset TX/RX FIFO (0x07)
  writeSC16Register(REG_MCR, 0x03); // Enable RTS and Auto RTS/CTS

  // DEBUG_PRINT(F("i2c_rs485: Send Request:"));
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
    // DEBUG_PRINTF(F(" %02x"), request[i]);
  }
  // DEBUG_PRINTLN();

  UART_sendBytes(request, 8);
  delay(10);
  uint8_t response[7];
  int len = UART_readBytes(response, 7, 100); // timeout 100ms
  for (int i = 0; i < len; i++) {
    // DEBUG_PRINTF(F(" %02x"), response[i]);
  }
  // DEBUG_PRINTLN();
  
  set_rs485_power(false);
  active_i2c_RS485_mode = 0;
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
  obj[F("rs485flags")] = rs;
  obj[F("rs485code")] = rs485_code;
  obj[F("rs485reg")] = rs485_reg;
}

void RS485I2CSensor::fromJson(ArduinoJson::JsonVariantConst obj) {
  SensorBase::fromJson(obj);
  
  // RS485-specific fields
  if (obj.containsKey(F("rs485flags"))) {
    uint16_t rs = obj[F("rs485flags")];
    rs485_flags.parity = (rs >> 0) & 0x3;
    rs485_flags.stopbits = (rs >> 2) & 0x1;
    rs485_flags.speed = (rs >> 3) & 0x7;
    rs485_flags.swapped = (rs >> 6) & 0x1;
    rs485_flags.datatype = (rs >> 7) & 0x7;
  }
  if (obj.containsKey(F("rs485code"))) rs485_code = obj[F("rs485code")];
  if (obj.containsKey(F("rs485reg"))) rs485_reg = obj[F("rs485reg")];
}

void RS485I2CSensor::emitJson(BufferFiller& bfill) const {
	SensorBase::emitJson(bfill);
}

// Backwards-compatible wrapper
int send_i2c_rs485_command(uint8_t address, uint16_t reg, uint16_t data, bool isbit) {
  if (!is_api_init()) return false;
  return RS485I2CSensor::sendCommand(address, reg, data, isbit);
}

#endif
