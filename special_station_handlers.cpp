#include "special_station_handlers.h"

#include "sensors.h"
#include "sensor_zigbee.h"
#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
#include "sensor_zigbee_gw.h"
#endif

#if defined(ESP32)
#include "sensor_gardena.h"
#endif

namespace {

static ulong hex_to_ulong(const unsigned char *code, unsigned char len) {
	ulong v = 0;
	while (len--) {
		char c = *code++;
		v <<= 4;
		if (c >= '0' && c <= '9') v += (c - '0');
		else if (c >= 'A' && c <= 'F') v += (c - 'A' + 10);
		else if (c >= 'a' && c <= 'f') v += (c - 'a' + 10);
	}
	return v;
}

static uint64_t parse_ieee_hex(const char *hex16) {
	uint64_t ieee = 0;
	for (int i = 0; i < 16; i++) {
		char c = hex16[i];
		ieee <<= 4;
		if (c >= '0' && c <= '9') {
			ieee += (c - '0');
		} else if (c >= 'A' && c <= 'F') {
			ieee += 10 + (c - 'A');
		} else if (c >= 'a' && c <= 'f') {
			ieee += 10 + (c - 'a');
		}
	}
	return ieee;
}

 #if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
static bool infer_tuya_from_sensor_rows(uint64_t ieee, uint8_t* inferred_dp) {
	if (inferred_dp) *inferred_dp = 0;
	if (ieee == 0) return false;

	bool found_tuya_signature = false;
	uint8_t first_dp = 0;
	uint8_t first_dp_val = 0;

	SensorIterator it = sensors_iterate_begin();
	SensorBase* sensor;
	while ((sensor = sensors_iterate_next(it)) != NULL) {
		if (!sensor || sensor->type != SENSOR_ZIGBEE) continue;
		ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(sensor);
		if (zb->device_ieee != ieee) continue;

		if (zb->tuya_dp_status > 0 && first_dp == 0) first_dp = (uint8_t)zb->tuya_dp_status;
		if (zb->tuya_dp_value > 0 && first_dp_val == 0) first_dp_val = (uint8_t)zb->tuya_dp_value;

		if (zb->cluster_id == 0xEF00 ||
			zb->tuya_dp_value >= 0 ||
			zb->tuya_dp_status >= 0 ||
			zb->tuya_dp_battery >= 0 ||
			zb->tuya_dp_consumption >= 0 ||
			zb->tuya_dp_unit >= 0 ||
			(zb->zb_manufacturer[0] == '_' && zb->zb_manufacturer[1] == 'T')) {
			found_tuya_signature = true;
		}
	}

	// Prefer status DP for on/off control; fall back to value DP if no status DP configured.
	if (first_dp == 0 && first_dp_val != 0) first_dp = first_dp_val;

	if (found_tuya_signature && inferred_dp && first_dp != 0) {
		*inferred_dp = first_dp;
	}

	return found_tuya_signature;
}
#endif

} // namespace

#if defined(ESP32)
void switch_gardena_station_handler(StationData *pdata, unsigned char value, uint16_t dur) {
	GardenaApi gardenaapi(os.sopt_load(SOPT_GARDENA_OPTS));
	JsonDocument doc;
	JsonDocument settings;
	DeserializationError error = deserializeJson(settings, os.sopt_load(SOPT_GARDENA_OPTS));
	String locationId = "";
	if (!error && settings.is<JsonObject>()) {
		if (settings.containsKey("location_id")) {
			locationId = settings["location_id"].as<String>();
		} else if (settings.containsKey("locationId")) {
			locationId = settings["locationId"].as<String>();
		}
	}
	if (locationId.isEmpty()) {
		return;
	}
	if (!gardenaapi.getLocationData(locationId, doc)) {
		return;
	}
	JsonArrayConst valves = doc["valves"].as<JsonArrayConst>();
	if (valves.isNull()) {
		return;
	}
	unsigned long index = strtoul((const char *)pdata->sped, NULL, 0);
	for (JsonVariantConst variant : valves) {
		JsonObjectConst valve = variant.as<JsonObjectConst>();
		if (!valve.isNull() && valve["id"].as<unsigned long>() == index) {
			String serviceId = valve["serviceId"].as<String>();
			gardenaapi.sendValveCommand(serviceId, value, dur > 0 ? dur : 60);
			break;
		}
	}
}
#else
void switch_gardena_station_handler(StationData *, unsigned char, uint16_t) {}
#endif

#if defined(OSPI)
extern boolean send_rs485_command(uint8_t device, uint8_t address, uint16_t reg, uint16_t data, bool isbit);
#elif defined(ESP8266) || defined(ESP32)
extern boolean send_rs485_command(uint32_t ip, uint16_t port, uint8_t address, uint16_t reg, uint16_t data, bool isbit);
#endif

void OpenSprinkler::switch_modbusStation(ModbusStationData *data, bool turnon) {
#if defined(OSPI)
	uint8_t device = (uint8_t)hex_to_ulong(data->device, sizeof(data->device));
	uint8_t address = (uint8_t)hex_to_ulong(data->address, sizeof(data->address));
	uint16_t reg = (uint16_t)hex_to_ulong(turnon ? data->register_on : data->register_off, sizeof(data->register_on));
	uint16_t onoff = (uint16_t)hex_to_ulong(turnon ? data->data_on : data->data_off, sizeof(data->data_on));

	send_rs485_command(device, address, reg, onoff, true);
#elif defined(ESP8266) || defined(ESP32)
	uint32_t ip4 = hex_to_ulong(data->ip, sizeof(data->ip));
	uint16_t port = (uint16_t)hex_to_ulong(data->port, sizeof(data->port));
	uint8_t address = (uint8_t)hex_to_ulong(data->address, sizeof(data->address));
	uint16_t reg = (uint16_t)hex_to_ulong(turnon ? data->register_on : data->register_off, sizeof(data->register_on));
	uint16_t onoff = (uint16_t)hex_to_ulong(turnon ? data->data_on : data->data_off, sizeof(data->data_on));

	send_rs485_command(ip4, port, address, reg, onoff, true);
#endif
}

void OpenSprinkler::switch_zigbeestation(ZigbeeStationData *data, bool turnon, uint8_t sid, uint16_t dur) {
	uint64_t ieee = parse_ieee_hex(data->device_ieee);

	uint8_t ep = (uint8_t)hex_to_ulong((unsigned char*)data->endpoint, sizeof(data->endpoint));
	if (ep == 0) ep = 1;

	bool use_tuya = (data->use_tuya[0] == '1');
	uint8_t dp_id = (uint8_t)hex_to_ulong((unsigned char*)data->tuya_dp, sizeof(data->tuya_dp));
	if (dp_id == 0) dp_id = 1;
	ZigbeeStationControlConfig cfg = {};
	bool has_cfg = sensor_zigbee_get_station_control_config(ieee, &cfg) && cfg.found;
	if (has_cfg) {
		ep = cfg.endpoint ? cfg.endpoint : ep;
		if (cfg.control_mode == ZB_STATION_CTRL_TUYA) {
			use_tuya = true;
		} else if (cfg.control_mode == ZB_STATION_CTRL_STANDARD) {
			use_tuya = false;
		} else if (!use_tuya && cfg.protocol_type == 1) {
			// DB template marks this device as Tuya-specific.
			use_tuya = true;
		} else if (!use_tuya && (cfg.dp_value != 0 || cfg.dp_status != 0)) {
			// AUTO mode with explicit DP mapping is effectively Tuya control.
			use_tuya = true;
		}
		if (use_tuya) {
			// Prefer dp_status (on/off DP) for valve control; fall back to dp_value.
			if (cfg.dp_status != 0) dp_id = cfg.dp_status;
			else if (cfg.dp_value != 0) dp_id = cfg.dp_value;
		}
	}

	#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
	{
		if (!use_tuya) {
			uint8_t inferred_dp = 0;
			if (infer_tuya_from_sensor_rows(ieee, &inferred_dp)) {
				use_tuya = true;
				if (inferred_dp != 0) dp_id = inferred_dp;
				DEBUG_PRINTF(F("[ZIGBEE] Runtime inference: forcing Tuya DP for ieee=%016llX dp=%d\n"),
				             (unsigned long long)ieee, dp_id);
			}
		}

		if (!has_cfg) {
			// Legacy fallback for stations that still rely on the station payload.
			bool matched = false;
			bool dev_is_tuya = false;
			static ZigbeeDeviceInfo devices[64];
			int count = sensor_zigbee_get_discovered_devices(devices, 64);
			for (int i = 0; i < count; i++) {
				if (devices[i].ieee_addr == 0 || devices[i].ieee_addr == ieee) continue;
				if ((devices[i].ieee_addr >> 8) == ieee || (ieee >> 8) == devices[i].ieee_addr) {
					DEBUG_PRINTF(F("[ZIGBEE] Corrected station IEEE before metadata lookup: %016llX -> %016llX\n"),
					             (unsigned long long)ieee, (unsigned long long)devices[i].ieee_addr);
					ieee = devices[i].ieee_addr;
					break;
				}
			}
			for (int i = 0; i < count; i++) {
				if (devices[i].ieee_addr != ieee) continue;
				dev_is_tuya = devices[i].is_tuya;
				matched = true;
				break;
			}
			if (matched && !use_tuya && dev_is_tuya) {
				use_tuya = true;
				DEBUG_PRINTF(F("[ZIGBEE] Runtime fallback: forcing Tuya DP for ieee=%016llX dp=%d\n"),
				             (unsigned long long)ieee, dp_id);
			}
		}
	}
	#endif

	if (use_tuya) {
		sensor_zigbee_send_tuya_dp_write(ieee, ep, dp_id, turnon);
	} else {
		sensor_zigbee_send_on_off(ieee, ep, turnon);
	}
}

// ============================================================================
// ZigBee Logical Device Management
// ============================================================================

// Initialize static storage in OpenSprinkler class
uint16_t OpenSprinkler::zigbee_logical_device_count = 0;
ZigBeeLogicalDevice OpenSprinkler::zigbee_logical_devices[OpenSprinkler::MAX_ZIGBEE_LOGICAL_DEVICES];

/** Register a logical device (insert or update) */
bool OpenSprinkler::zigbee_logical_register(const ZigBeeLogicalDevice& logdev) {
	if (zigbee_logical_device_count >= MAX_ZIGBEE_LOGICAL_DEVICES) {
		DEBUG_PRINTF(F("[ZIGBEE] WARN: Logical device array full, cannot add %s#%s\n"),
		             logdev.ieee, logdev.name);
		return false;
	}

	// Check if already exists (update case)
	for (uint16_t i = 0; i < zigbee_logical_device_count; i++) {
		if (strcmp(zigbee_logical_devices[i].ieee, logdev.ieee) == 0 &&
		    strcmp(zigbee_logical_devices[i].name, logdev.name) == 0) {
			zigbee_logical_devices[i] = logdev;
			DEBUG_PRINTF(F("[ZIGBEE] Updated logical device: %s#%s\n"),
			             logdev.ieee, logdev.name);
			return true;
		}
	}

	// New entry
	zigbee_logical_devices[zigbee_logical_device_count] = logdev;
	zigbee_logical_device_count++;
	DEBUG_PRINTF(F("[ZIGBEE] Registered logical device: %s#%s (count=%d)\n"),
	             logdev.ieee, logdev.name, zigbee_logical_device_count);
	return true;
}

/** Lookup a logical device by IEEE and name */
ZigBeeLogicalDevice* OpenSprinkler::zigbee_logical_lookup(const char *ieee, const char *name) {
	if (!ieee || !name) return nullptr;

	for (uint16_t i = 0; i < zigbee_logical_device_count; i++) {
		if (strcmp(zigbee_logical_devices[i].ieee, ieee) == 0 &&
		    strcmp(zigbee_logical_devices[i].name, name) == 0) {
			return &zigbee_logical_devices[i];
		}
	}
	return nullptr;
}

/** Unregister a specific logical device */
void OpenSprinkler::zigbee_logical_unregister(const char *ieee, const char *name) {
	if (!ieee || !name) return;

	for (uint16_t i = 0; i < zigbee_logical_device_count; i++) {
		if (strcmp(zigbee_logical_devices[i].ieee, ieee) == 0 &&
		    strcmp(zigbee_logical_devices[i].name, name) == 0) {
			// Swap with last and shrink
			if (i < zigbee_logical_device_count - 1) {
				zigbee_logical_devices[i] = zigbee_logical_devices[zigbee_logical_device_count - 1];
			}
			zigbee_logical_device_count--;
			DEBUG_PRINTF(F("[ZIGBEE] Unregistered logical device: %s#%s (count=%d)\n"),
			             ieee, name, zigbee_logical_device_count);
			return;
		}
	}
}

/** Clear all logical devices for a given IEEE */
void OpenSprinkler::zigbee_logical_clear_ieee(const char *ieee) {
	if (!ieee) return;

	uint16_t removed = 0;
	for (uint16_t i = 0; i < zigbee_logical_device_count; ) {
		if (strcmp(zigbee_logical_devices[i].ieee, ieee) == 0) {
			// Swap with last and shrink
			if (i < zigbee_logical_device_count - 1) {
				zigbee_logical_devices[i] = zigbee_logical_devices[zigbee_logical_device_count - 1];
			}
			zigbee_logical_device_count--;
			removed++;
		} else {
			i++;
		}
	}
	if (removed > 0) {
		DEBUG_PRINTF(F("[ZIGBEE] Cleared %d logical devices for IEEE %s (remaining=%d)\n"),
		             removed, ieee, zigbee_logical_device_count);
	}
}

/** Clear all logical devices (used during scan/rejoin) */
void OpenSprinkler::zigbee_logical_clear_all() {
	uint16_t old_count = zigbee_logical_device_count;
	zigbee_logical_device_count = 0;
	memset(zigbee_logical_devices, 0, sizeof(zigbee_logical_devices));
	DEBUG_PRINTF(F("[ZIGBEE] Cleared all %d logical devices\n"), old_count);
}

/** Get count of logical devices for an IEEE */
uint16_t OpenSprinkler::zigbee_logical_count_ieee(const char *ieee) {
	if (!ieee) return 0;

	uint16_t count = 0;
	for (uint16_t i = 0; i < zigbee_logical_device_count; i++) {
		if (strcmp(zigbee_logical_devices[i].ieee, ieee) == 0) {
			count++;
		}
	}
	return count;
}
