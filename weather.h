/* OpenSprinkler Unified Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Weather functions header file
 * Feb 2015 @ OpenSprinkler.com
 *
 * This file is part of the OpenSprinkler library
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
 * <http://www.gnu.org/licenses/>
 */


#ifndef _WEATHER_H
#define _WEATHER_H

#define WEATHER_UPDATE_SUNRISE  0x01
#define WEATHER_UPDATE_SUNSET   0x02
#define WEATHER_UPDATE_EIP      0x04
#define WEATHER_UPDATE_WL       0x08
#define WEATHER_UPDATE_TZ       0x10
#define WEATHER_UPDATE_RD       0x20

#define MAX_N_MD_SCALES 14 // maximum number of days that can be stored in md_scales array

// Weather status reason codes (diagnostic detail shown in System Diagnostics).
// Independent of wt_errCode: explains *why* the last weather update failed.
#define WT_REASON_OK             0  // last update successful
#define WT_REASON_PENDING        1  // no update attempted yet / in progress
#define WT_REASON_NETWORK_DOWN   2  // no network connection
#define WT_REASON_LOW_MEMORY     3  // free heap too low to start request
#define WT_REASON_NO_URL         4  // weather server URL not configured
#define WT_REASON_CONNECT_FAILED 5  // could not connect to weather server
#define WT_REASON_TIMEOUT        6  // request timed out
#define WT_REASON_EMPTY_RESPONSE 7  // server returned an empty response
#define WT_REASON_NO_RESPONSE    8  // no/invalid data received from server
#define WT_REASON_SERVER_ERROR   9  // weather server returned an error code (errCode>0)
#define WT_REASON_STALE          10 // cached weather data is stale
#define WT_REASON_DNS_FAILED     11 // weather server hostname could not be resolved

void GetWeather();

extern char wt_rawData[];
extern int wt_errCode;
extern int wt_errReason;
extern unsigned char md_scales[]; // multiday watering scales
extern unsigned char md_N; // number of elements in the md_scales array
extern unsigned char mda;
extern unsigned char wt_monthly[];
extern unsigned char wt_restricted;
bool parse_wto(char* wto);
void apply_monthly_adjustment(time_os_t curr_time);
#endif  // _WEATHER_H
