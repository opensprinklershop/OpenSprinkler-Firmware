/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Matter (CHIP) protocol implementation
 *
 * Jan 2026 @ OpenSprinkler.com
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

#include "opensprinkler_matter.h"

#ifdef ENABLE_MATTER

#include "OpenSprinkler.h"
#include "program.h"
#include "main.h"
#include "sensors.h"
#include "SensorBase.hpp"

// Arduino ESP32 Matter SDK - Matter.h includes all endpoint types
#include <Matter.h>

// Maximum number of sensor endpoints we can create
#define MAX_NUM_SENSORS 16

extern OpenSprinkler os;
extern ProgramData pd;

// Matter endpoint instances - direkt als statische Objekte, kein Pointer-Array nötig
// Für Stationen (als Ventile = On/Off Plugins)
MatterOnOffPlugin matter_stations[MAX_NUM_STATIONS];

// Für Sensoren - gruppiert nach Typ
MatterTemperatureSensor matter_temp_sensors[MAX_NUM_SENSORS];
MatterHumiditySensor matter_hum_sensors[MAX_NUM_SENSORS];

static bool matter_initialized = false;
static bool matter_commissioned = false;

// Zähler für tatsächlich verwendete Endpoints pro Typ
static uint8_t num_temp_sensors = 0;
static uint8_t num_hum_sensors = 0;

/**
 * Matter event callback
 * Handles commissioning events, fabric changes, etc.
 */
static void matter_event_callback(matterEvent_t event, const chip::DeviceLayer::ChipDeviceEvent *eventData) {
	DEBUG_PRINTF("Matter Event: 0x%04X\n", event);
	
	switch(event) {
		case MATTER_EVENT_COMMISSIONED:
			matter_commissioned = true;
			DEBUG_PRINTLN("Matter: Device commissioned");
			break;
			
		case MATTER_EVENT_DECOMMISSIONED:
			matter_commissioned = false;
			DEBUG_PRINTLN("Matter: Device decommissioned");
			break;
			
		case MATTER_EVENT_FABRIC_ADDED:
			DEBUG_PRINTLN("Matter: Fabric added");
			break;
			
		case MATTER_EVENT_FABRIC_REMOVED:
			DEBUG_PRINTLN("Matter: Fabric removed");
			break;
			
		case MATTER_EVENT_WIFI_CONNECTIVITY_CHANGE:
			DEBUG_PRINTLN("Matter: WiFi connectivity changed");
			break;
			
		default:
			break;
	}
}

/**
 * Initialize Matter stack
 */
void matter_init() {
	DEBUG_PRINTLN("Matter: Initializing...");
	
	if (matter_initialized) {
		DEBUG_PRINTLN("Matter: Already initialized");
		return;
	}
	
	// Reset Zähler
	num_temp_sensors = 0;
	num_hum_sensors = 0;
	
	// 1. Register event callback
	Matter.onEvent(matter_event_callback);
	
	// 2. Create valve endpoints for each station (als On/Off Plugins)
	DEBUG_PRINTF("Matter: Creating %d valve endpoints...\n", os.nstations);
	for (unsigned char sid = 0; sid < os.nstations; sid++) {
		// Set initial state from current station bits
		bool is_on = (os.station_bits[(sid>>3)] >> (sid&0x07)) & 1;
		
		// Begin endpoint with initial state
		if (matter_stations[sid].begin(is_on)) {
			// Register callback using lambda to capture station ID
			matter_stations[sid].onChange([sid](bool value) {
				DEBUG_PRINTF("Matter: Station %d OnOff -> %s\n", sid, value ? "ON" : "OFF");
				if (value) {
					matter_station_on(sid);
				} else {
					matter_station_off(sid);
				}
				return true; // Return true to acknowledge the change
			});
			DEBUG_PRINTF("Matter: Created valve endpoint %d\n", sid);
		} else {
			DEBUG_PRINTF("Matter: Failed to create valve endpoint %d\n", sid);
		}
	}
	
	// 3. Create sensor endpoints based on configured sensors
	DEBUG_PRINTLN("Matter: Creating sensor endpoints...");
	
	SensorIterator it;
	SensorBase* sensor;
	while ((sensor = sensors_iterate_next(it)) != nullptr) {
		if (!sensor || sensor->type == SENSOR_NONE) continue;
		
		// Map sensor types to Matter endpoints
		switch(sensor->type) {
			// Temperature sensors
			case SENSOR_SMT100_TEMP:
			case SENSOR_SMT50_TEMP:
			case SENSOR_SMT100_ANALOG_TEMP:
			case SENSOR_OSPI_ANALOG_SMT50_TEMP:
			case SENSOR_OSPI_INTERNAL_TEMP:
			case SENSOR_TH100_TEMP:
			case SENSOR_THERM200:
			case SENSOR_FYTA_TEMPERATURE:
			case SENSOR_WEATHER_TEMP_C:
			case SENSOR_WEATHER_TEMP_F:
				if (num_temp_sensors < MAX_NUM_SENSORS) {
					// Begin mit default Temperatur (20°C = 2000 in centi-degrees)
					if (matter_temp_sensors[num_temp_sensors].begin()) {
						DEBUG_PRINTF("Matter: Temp sensor %d -> endpoint %d\n", sensor->nr, num_temp_sensors);
						num_temp_sensors++;
					}
				}
				break;
				
			// Humidity sensors
			case SENSOR_TH100_MOIS:
			case SENSOR_WEATHER_HUM:
				if (num_hum_sensors < MAX_NUM_SENSORS) {
					// Begin mit default Humidity (50% = 5000 in basis points)
					if (matter_hum_sensors[num_hum_sensors].begin()) {
						DEBUG_PRINTF("Matter: Humidity sensor %d -> endpoint %d\n", sensor->nr, num_hum_sensors);
						num_hum_sensors++;
					}
				}
				break;
				
			default:
				break;
		}
	}
	
	DEBUG_PRINTF("Matter: Created %d temp, %d humidity sensors\n", 
		num_temp_sensors, num_hum_sensors);
	
	// 4. Start Matter stack - MUST be called after all endpoints are created
	Matter.begin();
	
	// Print commissioning info
	if (!Matter.isDeviceCommissioned()) {
		DEBUG_PRINTLN("Matter: Device not commissioned");
		DEBUG_PRINTF("Matter: QR Code URL: %s\n", Matter.getOnboardingQRCodeUrl().c_str());
		DEBUG_PRINTF("Matter: Manual Code: %s\n", Matter.getManualPairingCode().c_str());
	} else {
		matter_commissioned = true;
		DEBUG_PRINTLN("Matter: Device already commissioned");
		// Update all accessories to reflect current state
		for (unsigned char sid = 0; sid < os.nstations; sid++) {
			matter_stations[sid].updateAccessory();
		}
	}
	
	matter_initialized = true;
	DEBUG_PRINTLN("Matter: Init complete");
}

/**
 * Matter main loop handler
 */
void matter_loop() {
	// Arduino Matter SDK handles processing in background tasks
	// No explicit loop processing needed
	
	// Optionally: periodically check commissioning status
	static unsigned long last_status_check = 0;
	if (millis() - last_status_check > 60000) { // Every 60 seconds
		if (!matter_commissioned && Matter.isDeviceCommissioned()) {
			matter_commissioned = true;
			DEBUG_PRINTLN("Matter: Device commissioned (detected in loop)");
		}
		last_status_check = millis();
	}
}

/**
 * Shutdown Matter stack
 */
void matter_shutdown() {
	if (!matter_initialized) return;
	
	DEBUG_PRINTLN("Matter: Shutting down...");
	
	// Note: Arduino Matter SDK endpoints are static objects, no delete needed
	// Just reset state
	matter_initialized = false;
	matter_commissioned = false;
	num_temp_sensors = 0;
	num_hum_sensors = 0;
	
	DEBUG_PRINTLN("Matter: Shutdown complete");
}

/**
 * Turn on station via Matter command
 */
void matter_station_on(unsigned char sid) {
	DEBUG_PRINTF("Matter: Station %d ON requested\n", sid);
	
	if (sid >= os.nstations) {
		DEBUG_PRINTLN("Matter: Invalid station ID");
		return;
	}
	
	// Check if station is a master station (cannot be scheduled independently)
	if ((os.status.mas == sid+1) || (os.status.mas2 == sid+1)) {
		DEBUG_PRINTLN("Matter: Cannot schedule master station");
		return;
	}
	
	// Check if station already has a schedule
	unsigned char sqi = pd.station_qid[sid];
	if (sqi != 0xFF) {
		DEBUG_PRINTF("Matter: Station %d already scheduled\n", sid);
		return;
	}
	
	// Create new queue element
	RuntimeQueueStruct *q = pd.enqueue();
	if (!q) {
		DEBUG_PRINTLN("Matter: Queue full, cannot schedule");
		return;
	}
	
	// Default timer: 10 minutes (600 seconds)
	uint16_t timer = 600;
	
	// Schedule the station
	unsigned long curr_time = os.now_tz();
	q->st = 0;
	q->dur = timer;
	q->sid = sid;
	q->pid = 99; // Matter-controlled stations use program index 99
	
	// Schedule all stations (qo=0: append to queue)
	extern void schedule_all_stations(time_os_t, unsigned char);
	schedule_all_stations(curr_time, 0);
	
	DEBUG_PRINTF("Matter: Station %d scheduled for %d seconds\n", sid, timer);
}

/**
 * Turn off station via Matter command
 */
void matter_station_off(unsigned char sid) {
	DEBUG_PRINTF("Matter: Station %d OFF requested\n", sid);
	
	if (sid >= os.nstations) {
		DEBUG_PRINTLN("Matter: Invalid station ID");
		return;
	}
	
	// Check if station is in queue
	if (pd.station_qid[sid] == 255) {
		DEBUG_PRINTF("Matter: Station %d not running\n", sid);
		return;
	}
	
	// Mark station for removal and turn off
	unsigned long curr_time = os.now_tz();
	RuntimeQueueStruct *q = pd.queue + pd.station_qid[sid];
	q->deque_time = curr_time;
	
	// Turn off station (ssta=0: no shift remaining stations)
	extern void turn_off_station(unsigned char, time_os_t, unsigned char);
	turn_off_station(sid, curr_time, 0);
	
	DEBUG_PRINTF("Matter: Station %d stopped\n", sid);
}

/**
 * Update Matter attribute when station status changes in OpenSprinkler
 */
void matter_update_station_status(unsigned char sid, bool on) {
	if (!matter_initialized || !matter_commissioned) return;
	if (sid >= os.nstations) return;
	
	DEBUG_PRINTF("Matter: Update station %d status to %s\n", sid, on ? "ON" : "OFF");
	
	// Update OnOff attribute and notify Matter controllers
	matter_stations[sid].setOnOff(on);
	matter_stations[sid].updateAccessory();
}

/**
 * Update flow rate sensor in Matter
 */
void matter_update_flow_rate(float gpm) {
	if (!matter_initialized || !matter_commissioned) return;
	
	// Convert GPM (Gallons Per Minute) to L/h (Liters Per Hour)
	// 1 GPM = 3.78541 liters/min = 227.125 liters/hour
	float lph = gpm * 227.125f;
	
	DEBUG_PRINTF("Matter: Update flow rate %.2f GPM (%.2f L/h)\n", gpm, lph);
	
	// Note: Arduino Matter SDK doesn't have MatterFlowSensor yet
	// Flow rate could be exposed as a custom attribute or numeric sensor
	// For now, just log the value
	// Future: Implement custom cluster or wait for SDK support
}

/**
 * Update generic sensor value in Matter
 */
void matter_update_sensor_value(unsigned char sensor_id, float value) {
	if (!matter_initialized || !matter_commissioned) return;
	if (sensor_id >= MAX_NUM_SENSORS) return;
	
	DEBUG_PRINTF("Matter: Update sensor %d = %.2f\n", sensor_id, value);
	
	// Get sensor object
	SensorBase* sensor = sensor_by_nr(sensor_id);
	if (!sensor || sensor->type == SENSOR_NONE) return;
	
	// Find sensor in our endpoint arrays by iterating and counting
	SensorIterator it;
	SensorBase* s;
	uint8_t temp_idx = 0, hum_idx = 0, rain_idx = 0;
	bool found = false;
	
	while ((s = sensors_iterate_next(it)) != nullptr && !found) {
		if (!s || s->type == SENSOR_NONE) continue;
		
		// Check if this is the sensor we're looking for
		if (s->nr == sensor_id) {
			// Update appropriate Matter endpoint based on sensor type
			switch(sensor->type) {
				// Temperature sensors (value is in Celsius)
				case SENSOR_SMT100_TEMP:
				case SENSOR_SMT50_TEMP:
				case SENSOR_SMT100_ANALOG_TEMP:
				case SENSOR_OSPI_ANALOG_SMT50_TEMP:
				case SENSOR_OSPI_INTERNAL_TEMP:
				case SENSOR_TH100_TEMP:
				case SENSOR_THERM200:
				case SENSOR_FYTA_TEMPERATURE:
				case SENSOR_WEATHER_TEMP_C:
					if (temp_idx < num_temp_sensors) {
						// Matter expects temperature in Celsius * 100 (for 0.01°C precision)
						matter_temp_sensors[temp_idx].setTemperature((int16_t)(value * 100));
						DEBUG_PRINTF("Matter: Updated temp sensor %d: %.2f°C\n", sensor_id, value);
					}
					found = true;
					break;
					
				case SENSOR_WEATHER_TEMP_F:
					if (temp_idx < num_temp_sensors) {
						// Convert Fahrenheit to Celsius
						float celsius = (value - 32.0f) * 5.0f / 9.0f;
						matter_temp_sensors[temp_idx].setTemperature((int16_t)(celsius * 100));
						DEBUG_PRINTF("Matter: Updated temp sensor %d: %.2f°F (%.2f°C)\n", sensor_id, value, celsius);
					}
					found = true;
					break;
					
				// Humidity sensors (value is percentage 0-100)
				case SENSOR_TH100_MOIS:
				case SENSOR_WEATHER_HUM:
					if (hum_idx < num_hum_sensors) {
						// Matter expects humidity in percent * 100 (for 0.01% precision)
						matter_hum_sensors[hum_idx].setHumidity((uint16_t)(value * 100));
						DEBUG_PRINTF("Matter: Updated humidity sensor %d: %.2f%%\n", sensor_id, value);
					}
					found = true;
					break;
					
				default:
					DEBUG_PRINTF("Matter: Sensor type %d not supported\n", sensor->type);
					found = true; // Stop searching
					break;
			}
		}
		
		// Count indices as we iterate (only if not found yet)
		if (!found) {
			switch(s->type) {
				case SENSOR_SMT100_TEMP:
				case SENSOR_SMT50_TEMP:
				case SENSOR_SMT100_ANALOG_TEMP:
				case SENSOR_OSPI_ANALOG_SMT50_TEMP:
				case SENSOR_OSPI_INTERNAL_TEMP:
				case SENSOR_TH100_TEMP:
				case SENSOR_THERM200:
				case SENSOR_FYTA_TEMPERATURE:
				case SENSOR_WEATHER_TEMP_C:
				case SENSOR_WEATHER_TEMP_F:
					temp_idx++;
					break;
				case SENSOR_TH100_MOIS:
				case SENSOR_WEATHER_HUM:
					hum_idx++;
					break;
				default:
					break;
			}
		}
	}
}
/**
 * Check if device is commissioned
 */
bool matter_is_commissioned() {
	return matter_commissioned;
}

/**
 * Get number of commissioned fabrics
 */
uint8_t matter_get_fabric_count() {
	if (!matter_initialized) return 0;
	
	// Arduino Matter SDK doesn't expose fabric count directly
	// Return 1 if commissioned, 0 otherwise
	return matter_commissioned ? 1 : 0;
}

/**
 * Factory reset Matter credentials
 */
void matter_factory_reset() {
	DEBUG_PRINTLN("Matter: Factory reset requested");
	
	if (!matter_initialized) {
		DEBUG_PRINTLN("Matter: Not initialized, cannot factory reset");
		return;
	}
	
	// Decommission device (clears all Matter credentials and fabrics)
	Matter.decommission();
	
	matter_commissioned = false;
	DEBUG_PRINTLN("Matter: Factory reset complete - device decommissioned");
	DEBUG_PRINTLN("Matter: Device needs to be recommissioned");
	DEBUG_PRINTF("Matter: QR Code URL: %s\n", Matter.getOnboardingQRCodeUrl().c_str());
	DEBUG_PRINTF("Matter: Manual Code: %s\n", Matter.getManualPairingCode().c_str());
}

#endif // ENABLE_MATTER
