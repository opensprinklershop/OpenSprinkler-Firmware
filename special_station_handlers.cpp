#include "special_station_handlers.h"

#include "ArduinoJson.hpp"
#include "sensors.h"
#include "sensor_zigbee.h"
#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
#include "sensor_zigbee_gw.h"
#endif

#if defined(ESP32)
#include "sensor_gardena.h"
#endif

#include <vector>
#include <new>

namespace {

static constexpr const char* kZigbeeLogicalDevicesFile = ZIGBEE_LOGICAL_FILENAME;
static constexpr const char* kZigbeeLogicalDevicesTmpFile = "/zigbee_logical_devices.tmp";
static constexpr const char* kZigbeeLogicalDevicesBadFile = "/zigbee_logical_devices.bad.json";

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

 static void fill_logical_device_from_json(ZigBeeLogicalDevice& dev, ArduinoJson::JsonObjectConst obj) {
 	dev = ZigBeeLogicalDevice{};

 	const char* ieee = "";
 	if (obj.containsKey("ieee") && obj["ieee"].is<const char*>()) {
 		ieee = obj["ieee"].as<const char*>();
 	} else if (obj.containsKey("device_ieee") && obj["device_ieee"].is<const char*>()) {
 		ieee = obj["device_ieee"].as<const char*>();
 	}
 	const char* name = "";
 	if (obj.containsKey("name") && obj["name"].is<const char*>()) {
 		name = obj["name"].as<const char*>();
 	} else if (obj.containsKey("logical_name") && obj["logical_name"].is<const char*>()) {
 		name = obj["logical_name"].as<const char*>();
 	}
 	strncpy(dev.ieee, ieee, sizeof(dev.ieee) - 1);
 	dev.ieee[sizeof(dev.ieee) - 1] = '\0';
 	strncpy(dev.name, name, sizeof(dev.name) - 1);
 	dev.name[sizeof(dev.name) - 1] = '\0';
 	dev.endpoint = obj.containsKey("endpoint") ? obj["endpoint"].as<uint8_t>() : 1U;
 	dev.cluster_id = obj.containsKey("cluster_id") ? obj["cluster_id"].as<uint16_t>() : 0U;
 	if (obj.containsKey("attr_id")) {
 		dev.attr_id = obj["attr_id"].as<uint16_t>();
 	} else if (obj.containsKey("attribute_id")) {
 		dev.attr_id = obj["attribute_id"].as<uint16_t>();
 	}
 	dev.is_tuya = obj.containsKey("is_tuya") ? obj["is_tuya"].as<bool>() : false;
 	if (obj.containsKey("tuya_dp_value")) {
 		dev.tuya_dp_value = obj["tuya_dp_value"].as<int16_t>();
 	} else if (obj.containsKey("tuya_dp")) {
 		dev.tuya_dp_value = obj["tuya_dp"].as<int16_t>();
 	}
 	if (obj.containsKey("tuya_dp_battery")) {
 		dev.tuya_dp_battery = obj["tuya_dp_battery"].as<int16_t>();
 	} else if (obj.containsKey("tuya_dp_batt")) {
 		dev.tuya_dp_battery = obj["tuya_dp_batt"].as<int16_t>();
 	}
 	if (obj.containsKey("tuya_dp_unit")) {
 		dev.tuya_dp_unit = obj["tuya_dp_unit"].as<int16_t>();
 	}
 	if (obj.containsKey("tuya_dp_status")) {
 		dev.tuya_dp_status = obj["tuya_dp_status"].as<int16_t>();
 	}
 	if (obj.containsKey("tuya_dp_consumption")) {
 		dev.tuya_dp_consumption = obj["tuya_dp_consumption"].as<int16_t>();
 	}
 	dev.factor = obj.containsKey("factor") ? obj["factor"].as<int16_t>() : 0;
 	dev.divider = obj.containsKey("divider") ? obj["divider"].as<int16_t>() : 0;
 	dev.offset = obj.containsKey("offset") ? obj["offset"].as<int16_t>() : 0;
 	const char* unit = "";
 	if (obj.containsKey("unit") && obj["unit"].is<const char*>()) {
 		unit = obj["unit"].as<const char*>();
 	}
 	strncpy(dev.unit, unit, sizeof(dev.unit) - 1);
 	dev.unit[sizeof(dev.unit) - 1] = '\0';
 	dev.unitid = obj.containsKey("unitid") ? obj["unitid"].as<uint8_t>() : 0U;
 }

 static void add_logical_device_to_json(ArduinoJson::JsonArray arr, const ZigBeeLogicalDevice& dev) {
 	ArduinoJson::JsonObject obj = arr.createNestedObject();
 	obj["ieee"] = dev.ieee;
 	obj["name"] = dev.name;
 	obj["endpoint"] = dev.endpoint;
 	obj["cluster_id"] = dev.cluster_id;
 	obj["attr_id"] = dev.attr_id;
 	obj["is_tuya"] = dev.is_tuya;
 	if (dev.tuya_dp_value >= 0) obj["tuya_dp_value"] = dev.tuya_dp_value;
 	if (dev.tuya_dp_battery >= 0) obj["tuya_dp_battery"] = dev.tuya_dp_battery;
 	if (dev.tuya_dp_unit >= 0) obj["tuya_dp_unit"] = dev.tuya_dp_unit;
 	if (dev.tuya_dp_status >= 0) obj["tuya_dp_status"] = dev.tuya_dp_status;
 	if (dev.tuya_dp_consumption >= 0) obj["tuya_dp_consumption"] = dev.tuya_dp_consumption;
 	obj["factor"] = dev.factor;
 	obj["divider"] = dev.divider;
 	obj["offset"] = dev.offset;
 	if (dev.unit[0] != '\0') obj["unit"] = dev.unit;
 	obj["unitid"] = dev.unitid;
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
	bool has_cfg = sensor_zigbee_get_station_control_config(ieee, &cfg, ep, dp_id) && cfg.found;
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

// Initialize the dynamic hash map for logical devices (allocated on first use)
OpenSprinkler::LogicalDeviceMap* OpenSprinkler::zigbee_logical_devices_map = nullptr;

// Helper: Get or create the logical devices map
static OpenSprinkler::LogicalDeviceMap& _get_logical_devices_map() {
	if (!OpenSprinkler::zigbee_logical_devices_map) {
		OpenSprinkler::zigbee_logical_devices_map = new (std::nothrow) OpenSprinkler::LogicalDeviceMap();
		if (OpenSprinkler::zigbee_logical_devices_map) {
			DEBUG_PRINTF(F("[ZIGBEE] Logical device map allocated in PSRAM/Heap\n"));
		} else {
			DEBUG_PRINTF(F("[ZIGBEE] ERROR: Failed to allocate logical device map\n"));
			static OpenSprinkler::LogicalDeviceMap fallback_map;
			return fallback_map;
		}
	}
	return *OpenSprinkler::zigbee_logical_devices_map;
}

static void _clear_logical_devices_map(OpenSprinkler::LogicalDeviceMap& map) {
	map.clear();
}

static bool _save_logical_devices_map(OpenSprinkler::LogicalDeviceMap& map) {
	if (map.empty()) {
		remove_file(kZigbeeLogicalDevicesTmpFile);
		remove_file(kZigbeeLogicalDevicesFile);
		return true;
	}

	ArduinoJson::JsonDocument doc;
	doc["version"] = 1;
	ArduinoJson::JsonArray arr = doc["devices"].to<ArduinoJson::JsonArray>();
	for (const auto& entry : map) {
		add_logical_device_to_json(arr, entry.second.device);
	}

	remove_file(kZigbeeLogicalDevicesTmpFile);
	size_t json_size = ArduinoJson::measureJson(doc);
	std::vector<char> json(json_size + 1, '\0');
	size_t written = ArduinoJson::serializeJson(doc, json.data(), json.size());
	if (written == 0) {
		DEBUG_PRINTLN(F("[ZIGBEE] Failed to serialize logical device registry"));
		remove_file(kZigbeeLogicalDevicesTmpFile);
		return false;
	}

	file_write_block(kZigbeeLogicalDevicesTmpFile, json.data(), 0, written);
	remove_file(kZigbeeLogicalDevicesFile);
	if (!rename_file(kZigbeeLogicalDevicesTmpFile, kZigbeeLogicalDevicesFile)) {
		DEBUG_PRINTLN(F("[ZIGBEE] Failed to replace logical device registry"));
		remove_file(kZigbeeLogicalDevicesTmpFile);
		return false;
	}

	return true;
}

/** Register a logical device (insert or update) */
bool OpenSprinkler::zigbee_logical_register(const ZigBeeLogicalDevice& logdev) {
	auto& map = _get_logical_devices_map();
	
	// Build key: IEEE#LogicalDeviceName
	char key_buf[128];
	snprintf(key_buf, sizeof(key_buf), "%s#%s", logdev.ieee, logdev.name);
	std::string key(key_buf);
	
	LogicalDeviceEntry entry;
	entry.device = logdev;
	entry.key = key;
	
	map[key] = entry;
	if (!zigbee_logical_save()) {
		DEBUG_PRINTF(F("[ZIGBEE] WARN: Persisting logical device failed: %s\n"), key_buf);
		return false;
	}
	DEBUG_PRINTF(F("[ZIGBEE] Registered logical device: %s (map size=%u)\n"),
	             key_buf, (unsigned int)map.size());
	return true;
}

/** Lookup a logical device by IEEE and name */
ZigBeeLogicalDevice* OpenSprinkler::zigbee_logical_lookup(const char *ieee, const char *name) {
	if (!ieee || !name) return nullptr;
	
	auto& map = _get_logical_devices_map();
	
	// Build key: IEEE#LogicalDeviceName
	char key_buf[128];
	snprintf(key_buf, sizeof(key_buf), "%s#%s", ieee, name);
	std::string key(key_buf);
	
	auto it = map.find(key);
	if (it != map.end()) {
		return &(it->second.device);
	}
	return nullptr;
}

/** Unregister a specific logical device */
void OpenSprinkler::zigbee_logical_unregister(const char *ieee, const char *name) {
	if (!ieee || !name) return;
	
	auto& map = _get_logical_devices_map();
	
	// Build key: IEEE#LogicalDeviceName
	char key_buf[128];
	snprintf(key_buf, sizeof(key_buf), "%s#%s", ieee, name);
	std::string key(key_buf);
	
	if (map.erase(key) > 0) {
		DEBUG_PRINTF(F("[ZIGBEE] Unregistered logical device: %s (map size=%u)\n"),
		             key_buf, (unsigned int)map.size());
		zigbee_logical_save();
	}
}

/** Clear all logical devices for a given IEEE */
void OpenSprinkler::zigbee_logical_clear_ieee(const char *ieee) {
	if (!ieee) return;
	
	auto& map = _get_logical_devices_map();
	
	std::vector<std::string> to_remove;
	for (auto& entry : map) {
		if (strncmp(entry.second.device.ieee, ieee, 16) == 0) {
			to_remove.push_back(entry.first);
		}
	}
	
	uint16_t removed = to_remove.size();
	for (const auto& key : to_remove) {
		map.erase(key);
	}
	
	if (removed > 0) {
		DEBUG_PRINTF(F("[ZIGBEE] Cleared %d logical devices for IEEE %s (remaining=%u)\n"),
		             removed, ieee, (unsigned int)map.size());
		zigbee_logical_save();
	}
}

/** Clear all logical devices (used during scan/rejoin) */
void OpenSprinkler::zigbee_logical_clear_all() {
	auto& map = _get_logical_devices_map();
	uint16_t old_count = map.size();
	(void)old_count;
	map.clear();
	DEBUG_PRINTF(F("[ZIGBEE] Cleared all %d logical devices\n"), old_count);
	zigbee_logical_save();
}

/** Get count of logical devices for an IEEE */
uint16_t OpenSprinkler::zigbee_logical_count_ieee(const char *ieee) {
	if (!ieee) return 0;
	
	auto& map = _get_logical_devices_map();
	
	uint16_t count = 0;
	for (const auto& entry : map) {
		if (strncmp(entry.second.device.ieee, ieee, 16) == 0) {
			count++;
		}
	}
	return count;
}

bool OpenSprinkler::zigbee_logical_load() {
	auto& map = _get_logical_devices_map();
	_clear_logical_devices_map(map);

	if (!file_exists(kZigbeeLogicalDevicesFile)) {
		return true;
	}

	ulong size = file_size(kZigbeeLogicalDevicesFile);
	if (size == 0) {
		return true;
	}

	std::vector<char> json(size + 1, '\0');
	ulong read = file_read_block(kZigbeeLogicalDevicesFile, json.data(), 0, size);
	if (read == 0) {
		DEBUG_PRINTLN(F("[ZIGBEE] Failed to read logical device registry"));
		return false;
	}
	json[read] = '\0';

	ArduinoJson::JsonDocument doc;
	ArduinoJson::DeserializationError err = ArduinoJson::deserializeJson(doc, json.data(), read);
	if (err) {
		DEBUG_PRINTF(F("[ZIGBEE] Logical device registry parse error (%s)\n"), err.c_str());
		remove_file(kZigbeeLogicalDevicesBadFile);
		rename_file(kZigbeeLogicalDevicesFile, kZigbeeLogicalDevicesBadFile);
		return false;
	}

	ArduinoJson::JsonArrayConst devices = doc.as<ArduinoJson::JsonArrayConst>();
	if (devices.isNull() && doc["devices"].is<ArduinoJson::JsonArrayConst>()) {
		devices = doc["devices"].as<ArduinoJson::JsonArrayConst>();
	}
	if (devices.isNull()) {
		DEBUG_PRINTLN(F("[ZIGBEE] Logical device registry missing device array"));
		return true;
	}

	for (ArduinoJson::JsonVariantConst variant : devices) {
		if (!variant.is<ArduinoJson::JsonObjectConst>()) continue;
		ArduinoJson::JsonObjectConst obj = variant.as<ArduinoJson::JsonObjectConst>();

		ZigBeeLogicalDevice dev = {};
		fill_logical_device_from_json(dev, obj);
		if (dev.ieee[0] == '\0' || dev.name[0] == '\0') continue;

		LogicalDeviceEntry entry;
		entry.device = dev;
		char key_buf[128];
		snprintf(key_buf, sizeof(key_buf), "%s#%s", dev.ieee, dev.name);
		entry.key = key_buf;
		map[entry.key] = entry;
	}

	DEBUG_PRINTF(F("[ZIGBEE] Loaded %u persisted logical device(s)\n"), (unsigned int)map.size());
	return true;
}

bool OpenSprinkler::zigbee_logical_save() {
	auto& map = _get_logical_devices_map();
	return _save_logical_devices_map(map);
}
