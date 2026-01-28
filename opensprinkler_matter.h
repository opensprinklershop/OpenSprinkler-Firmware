/* OpenSprinkler Firmware
 * Copyright (C) 2026 by OpenSprinkler Shop Ltd.
 *
 * Matter Protocol Integration - Simplified API
 */

#ifndef _OPENSPRINKLER_MATTER_H
#define _OPENSPRINKLER_MATTER_H

#ifdef ENABLE_MATTER

class OSMatter {
public:
  static OSMatter& instance();

  void init();
  void loop();
  
  void update_station(uint8_t sid, bool is_on);
  bool is_commissioned();
  
  // Internal callbacks
  void station_on(unsigned char sid);
  void station_off(unsigned char sid);

private:
  OSMatter() = default;
};

#endif // ENABLE_MATTER
#endif // _OPENSPRINKLER_MATTER_H
