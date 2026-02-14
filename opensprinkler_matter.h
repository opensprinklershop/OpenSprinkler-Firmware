/* OpenSprinkler Firmware
 * Copyright (C) 2026 by OpenSprinkler Shop Ltd.
 *
 * Matter Protocol Integration - Simplified API
 */

#ifndef _OPENSPRINKLER_MATTER_H
#define _OPENSPRINKLER_MATTER_H

#ifdef ENABLE_MATTER

uint32_t matter_get_init_time_ms();

class OSMatter {
public:
  static OSMatter& instance();

  void init();
  void loop();
  
  void update_station(uint8_t sid, bool is_on);
  bool is_commissioned();
  
  // Get Matter pairing information
  String get_qr_code_url();
  String get_manual_pairing_code();
  
  // Internal callbacks
  void station_on(unsigned char sid);
  void station_off(unsigned char sid);

private:
  OSMatter() = default;
};

#endif // ENABLE_MATTER
#endif // _OPENSPRINKLER_MATTER_H
