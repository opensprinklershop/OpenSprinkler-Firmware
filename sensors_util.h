/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Sensor utility classes
 * Dec 2025 @ OpenSprinklerShop
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

#ifndef _SENSORS_UTIL_H
#define _SENSORS_UTIL_H

#include "utils.h"
#include <map>

#ifndef SENSORS_FILE_IO_BUFFER_SIZE
#define SENSORS_FILE_IO_BUFFER_SIZE 512
#endif

// Forward declarations
class ProgSensorAdjust;
class Monitor;

/**
 * @brief Custom Writer class for direct file writing
 * @note Used with ArduinoJson for efficient serialization to files with buffering
 */
class FileWriter {
private:
  const char* filename;
  size_t filePos;
  uint8_t buffer[SENSORS_FILE_IO_BUFFER_SIZE];  // Write buffer
  size_t bufferPos;
  
  /**
   * @brief Flush internal buffer to file
   * @note Writes buffered data to disk and resets buffer position
   */
  void flush() {
    if (bufferPos > 0) {
      file_write_block(filename, buffer, filePos, bufferPos);
      filePos += bufferPos;
      bufferPos = 0;
    }
  }
  
public:
  /**
   * @brief Constructor
   * @param fname Filename to write to
   */
  FileWriter(const char* fname) : filename(fname), filePos(0), bufferPos(0) {}
  
  /**
   * @brief Destructor - flushes remaining buffer data
   */
  ~FileWriter() {
    flush();  // Flush on destruction
  }
  
  /**
   * @brief Write single byte to file (buffered)
   * @param c Byte to write
   * @return Number of bytes written (always 1)
   * @note Automatically flushes buffer when full
   */
  size_t write(uint8_t c) {
    buffer[bufferPos++] = c;
    if (bufferPos >= sizeof(buffer)) {
      flush();
    }
    return 1;
  }
  
  /**
   * @brief Write byte array to file (buffered)
   * @param data Pointer to data array
   * @param length Number of bytes to write
   * @return Number of bytes written
   * @note Large writes bypass buffer for efficiency
   */
  size_t write(const uint8_t* data, size_t length) {
    // If data is larger than buffer, flush and write directly
    if (length >= sizeof(buffer)) {
      flush();
      file_write_block(filename, data, filePos, length);
      filePos += length;
      return length;
    }
    
    // Otherwise, copy to buffer
    size_t written = 0;
    while (written < length) {
      size_t toWrite = min(length - written, sizeof(buffer) - bufferPos);
      memcpy(buffer + bufferPos, data + written, toWrite);
      bufferPos += toWrite;
      written += toWrite;
      
      if (bufferPos >= sizeof(buffer)) {
        flush();
      }
    }
    return written;
  }
};

/**
 * @brief Custom Reader class for direct file reading
 * @note Used with ArduinoJson for efficient deserialization from files with buffering
 */
class FileReader {
private:
  const char* filename;
  size_t filePos;      // Position in file
  size_t fileSize;
  uint8_t buffer[SENSORS_FILE_IO_BUFFER_SIZE]; // Read buffer
  size_t bufferPos;    // Current position in buffer
  size_t bufferLen;    // Valid data in buffer
  
  /**
   * @brief Fill internal buffer from file
   * @note Reads next chunk from file into buffer
   */
  void fillBuffer() {
    if (filePos >= fileSize) {
      bufferLen = 0;
      return;
    }
    
    size_t toRead = min(sizeof(buffer), fileSize - filePos);
    file_read_block(filename, buffer, filePos, toRead);
    filePos += toRead;
    bufferLen = toRead;
    bufferPos = 0;
  }
  
public:
  /**
   * @brief Constructor - opens file and prefills buffer
   * @param fname Filename to read from
   */
  FileReader(const char* fname) : filename(fname), filePos(0), bufferPos(0), bufferLen(0) {
    fileSize = file_size(fname);
    fillBuffer();  // Prefill buffer
  }
  
  /**
   * @brief Read single byte from file (buffered)
   * @return Byte value (0-255) or -1 on EOF
   * @note Automatically refills buffer when needed
   */
  int read() {
    if (bufferPos >= bufferLen) {
      fillBuffer();
      if (bufferLen == 0) return -1;
    }
    return buffer[bufferPos++];
  }
  
  /**
   * @brief Read multiple bytes from file (buffered)
   * @param outBuffer Output buffer to fill
   * @param length Maximum number of bytes to read
   * @return Actual number of bytes read
   * @note Automatically refills internal buffer as needed
   */
  size_t readBytes(char* outBuffer, size_t length) {
    size_t totalRead = 0;
    
    while (totalRead < length) {
      // Refill buffer if empty
      if (bufferPos >= bufferLen) {
        fillBuffer();
        if (bufferLen == 0) break;  // EOF
      }
      
      // Copy from buffer
      size_t available = bufferLen - bufferPos;
      size_t toCopy = min(available, length - totalRead);
      memcpy(outBuffer + totalRead, buffer + bufferPos, toCopy);
      bufferPos += toCopy;
      totalRead += toCopy;
    }
    
    return totalRead;
  }
};

// Forward declaration for sensor map type
#include <map>
class SensorBase;
class ProgSensorAdjust;

/**
 * @brief Import legacy binary sensor.dat format and convert to sensor map
 * @param sensorsMap Reference to sensor map to populate
 * @note Initializes sensors, saves to JSON, and deletes legacy file on success
 */
bool sensor_load_legacy(std::map<uint, SensorBase*>& sensorsMap);

/**
 * @brief Import legacy binary progsensor.dat format and convert to JSON
 * @param progSensorAdjustsMap Reference to map to populate
 * @return true if legacy file was found and imported, false otherwise
 * @note Saves to JSON and deletes legacy file on success
 */
bool prog_adjust_load_legacy(std::map<uint, ProgSensorAdjust*>& progSensorAdjustsMap);

/**
 * @brief Import legacy binary monitors.dat format and convert to JSON
 * @param monitorsMap Reference to map to populate
 * @return true if legacy file was found and imported, false otherwise
 * @note Saves to JSON and deletes legacy file on success
 */
bool monitor_load_legacy(std::map<uint, Monitor*>& monitorsMap);

#endif // _SENSORS_UTIL_H
