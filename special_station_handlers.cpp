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
	if (has_cfg && cfg.control_mode != ZB_STATION_CTRL_AUTO) {
		ep = cfg.endpoint ? cfg.endpoint : ep;
		use_tuya = (cfg.control_mode == ZB_STATION_CTRL_TUYA);
		if (use_tuya && cfg.dp_value != 0) {
			dp_id = cfg.dp_value;
		}
	}

	#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
	{
		if (!has_cfg || cfg.control_mode == ZB_STATION_CTRL_AUTO) {
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

	bool command_sent = false;
	if (use_tuya) {
		command_sent = sensor_zigbee_send_tuya_dp_write(ieee, ep, dp_id, turnon);
	} else {
		command_sent = sensor_zigbee_send_on_off(ieee, ep, turnon);
	}
#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
	// Register for switch-failure verification: if the device doesn't echo
	// the expected DP state within ZB_VERIFY_TIMEOUT_MS, an MQTT alert is
	// published on station/{sid}/alert/switch.
	// Only standard ZCL devices are registered here (Tuya is single-fire, zero-retry).
	if (!use_tuya && command_sent && ieee != 0) {
		sensor_zigbee_station_verify_register(sid, ieee, ep, dp_id, turnon);
	}
#endif
}
