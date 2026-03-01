/* OpenSprinkler ZigBee Client Mode — Expose local data to ZigBee hub
 *
 * Creates ZCL endpoints that report OS sensor values, zone on/off control,
 * program start/stop, and rain sensor state to the ZigBee coordinator/hub.
 *
 * Endpoint allocation:
 *   ep 11-26:  OS sensor values  (ZigbeeTempSensor / ZigbeeAnalog)
 *   ep 31-62:  Zone on/off ctrl  (ZigbeePowerOutlet subclass)
 *   ep 63-78:  Program start/stop(ZigbeePowerOutlet subclass)
 *   ep 80-81:  Rain sensor 1/2   (ZigbeeBinary input)
 *
 * 2026 @ OpenSprinklerShop
 */

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)

#include "sensor_zigbee_client_expose.h"
#include "defines.h"
#include "OpenSprinkler.h"
#include "program.h"
#include "sensors.h"
#include "SensorBase.hpp"
#include "main.h"
#include <Arduino.h>
#include <vector>

#include "Zigbee.h"
#include "ZigbeeEP.h"
#include "ep/ZigbeeTempSensor.h"
#include "ep/ZigbeeAnalog.h"
#include "ep/ZigbeePowerOutlet.h"
#include "ep/ZigbeeBinary.h"
#include "ep/ZigbeeFlowSensor.h"

extern OpenSprinkler os;
extern ProgramData pd;

// =========================================================================
// Constants
// =========================================================================
static constexpr uint8_t  EP_SENSOR_BASE  = 11;
static constexpr uint8_t  EP_SENSOR_MAX   = 26;   // ep 11-26 (up to 16 sensors)
static constexpr uint8_t  EP_ZONE_BASE    = 31;
static constexpr uint8_t  EP_ZONE_MAX     = 62;   // ep 31-62 (up to 32 zones)
static constexpr uint8_t  EP_PROG_BASE    = 63;
static constexpr uint8_t  EP_PROG_MAX     = 78;   // ep 63-78 (up to 16 programs)
static constexpr uint8_t  EP_RAIN_BASE    = 80;   // ep 80-81 (rain sensor 1/2)

static constexpr unsigned long UPDATE_INTERVAL_MS = 30000;  // sensor value update every 30s
static constexpr unsigned long ZONE_SYNC_INTERVAL_MS = 5000;  // zone state sync every 5s
static constexpr uint16_t     ZONE_DEFAULT_DURATION = 3600; // 1h default for ZigBee-triggered zone

// =========================================================================
// Endpoint types for sensors (determines ZCL cluster)
// =========================================================================
enum SensorEPType {
    SENSOR_EP_TEMPERATURE, // ZigbeeTempSensor (cluster 0x0402)
    SENSOR_EP_HUMIDITY,    // ZigbeeTempSensor + humidity (cluster 0x0405)
    SENSOR_EP_FLOW,        // ZigbeeFlowSensor (cluster 0x0404)
    SENSOR_EP_ANALOG,      // ZigbeeAnalog + AnalogInput (cluster 0x000C)
};

// =========================================================================
// Storage for created endpoints
// =========================================================================
struct SensorEndpoint {
    ZigbeeEP*    ep;
    uint          sensor_nr;  // OS sensor number (1-based)
    SensorEPType  ep_type;
};
static std::vector<SensorEndpoint> s_sensor_eps;

// Zone endpoint: receives On/Off from hub → controls OS station
class OSZoneEndpoint : public ZigbeePowerOutlet {
public:
    OSZoneEndpoint(uint8_t ep_num, uint8_t sid)
        : ZigbeePowerOutlet(ep_num), _sid(sid) {}
    uint8_t stationId() const { return _sid; }
private:
    void zbAttributeSet(const esp_zb_zcl_set_attr_value_message_t *message) override;
    uint8_t _sid;
};

struct ZoneEndpoint {
    OSZoneEndpoint* ep;
    uint8_t         station_id;
};
static std::vector<ZoneEndpoint> s_zone_eps;

// Program endpoint: On → start program, Off → no-op (programs run to completion)
class OSProgramEndpoint : public ZigbeePowerOutlet {
public:
    OSProgramEndpoint(uint8_t ep_num, uint8_t pid)
        : ZigbeePowerOutlet(ep_num), _pid(pid) {}
    uint8_t programId() const { return _pid; }
private:
    void zbAttributeSet(const esp_zb_zcl_set_attr_value_message_t *message) override;
    uint8_t _pid;
};

struct ProgramEndpoint {
    OSProgramEndpoint* ep;
    uint8_t            program_id; // 0-indexed
};
static std::vector<ProgramEndpoint> s_prog_eps;

struct RainEndpoint {
    ZigbeeBinary* ep;
    uint8_t       sensor_idx; // 0 = sensor1, 1 = sensor2
};
static std::vector<RainEndpoint> s_rain_eps;

static unsigned long s_last_sensor_update = 0;
static unsigned long s_last_zone_sync = 0;

// =========================================================================
// Zone On/Off handler — called from ZigBee task via zbAttributeSet override
// =========================================================================
void OSZoneEndpoint::zbAttributeSet(const esp_zb_zcl_set_attr_value_message_t *message) {
    if (message->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) return;
    if (message->attribute.id != ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) return;
    if (message->attribute.data.type != ESP_ZB_ZCL_ATTR_TYPE_BOOL) return;

    bool state = *(bool *)message->attribute.data.value;
    uint8_t sid = _sid;

    DEBUG_PRINTF(F("[ZB-EXPOSE] Zone %d (%s) → %s (from hub)\n"),
                 sid, "", state ? "ON" : "OFF");

    if (state) {
        // Turn on station with default duration
        // Skip master stations
        if ((os.status.mas == sid + 1) || (os.status.mas2 == sid + 1)) {
            DEBUG_PRINTF(F("[ZB-EXPOSE] Zone %d is master — skipped\n"), sid);
            return;
        }
        unsigned long curr_time = os.now_tz();
        RuntimeQueueStruct *q = nullptr;
        unsigned char sqi = pd.station_qid[sid];
        if (sqi == 0xFF) {
            q = pd.enqueue();
        }
        if (q) {
            q->st = 0;
            q->dur = ZONE_DEFAULT_DURATION;
            q->sid = sid;
            q->pid = 99; // manual
            schedule_all_stations(curr_time);
        }
    } else {
        // Turn off station
        if (pd.station_qid[sid] != 255) {
            unsigned long curr_time = os.now_tz();
            RuntimeQueueStruct *q = pd.queue + pd.station_qid[sid];
            q->deque_time = curr_time;
            turn_off_station(sid, curr_time);
        }
    }
}

// =========================================================================
// Program On/Off handler — On = start program, Off = no-op
// =========================================================================
void OSProgramEndpoint::zbAttributeSet(const esp_zb_zcl_set_attr_value_message_t *message) {
    if (message->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) return;
    if (message->attribute.id != ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) return;
    if (message->attribute.data.type != ESP_ZB_ZCL_ATTR_TYPE_BOOL) return;

    bool state = *(bool *)message->attribute.data.value;

    if (state) {
        // pid is 0-indexed here; manual_start_program expects 1-indexed pid
        // (pid=0 → test program, pid=1 → first real program index 0)
        uint8_t pid_1based = _pid + 1;
        DEBUG_PRINTF(F("[ZB-EXPOSE] Program %d → START (from hub)\n"), _pid);
        manual_start_program(pid_1based, 255, 0); // 255 = use program's weather setting
    }
    // Off: programs run to completion, no stop action
}

// =========================================================================
// Determine ZCL endpoint type from OS sensor unit
// =========================================================================
static SensorEPType classify_sensor(SensorBase* sensor) {
    unsigned char unit = getSensorUnitId(sensor);
    switch (unit) {
        case UNIT_DEGREE:
        case UNIT_FAHRENHEIT:
            return SENSOR_EP_TEMPERATURE;
        case UNIT_HUM_PERCENT:
            return SENSOR_EP_HUMIDITY;
        case UNIT_LITER:
            return SENSOR_EP_FLOW;
        default:
            return SENSOR_EP_ANALOG;
    }
}

// =========================================================================
// Create all endpoints — call BEFORE Zigbee.begin()
// =========================================================================
void client_expose_create_endpoints() {
    // ---- Sensor endpoints ----
    {
        uint8_t ep_num = EP_SENSOR_BASE;
        SensorIterator it = sensors_iterate_begin();
        SensorBase* sensor;
        while ((sensor = sensors_iterate_next(it)) != nullptr) {
            if (ep_num > EP_SENSOR_MAX) break;
            if (!sensor || !sensor->flags.enable) continue;

            SensorEPType etype = classify_sensor(sensor);
            ZigbeeEP* ep = nullptr;

            switch (etype) {
                case SENSOR_EP_TEMPERATURE: {
                    auto* ts = new ZigbeeTempSensor(ep_num);
                    ts->setMinMaxValue(-40.0f, 125.0f);
                    ts->setManufacturerAndModel("OpenSprinkler", sensor->name);
                    ep = ts;
                    break;
                }
                case SENSOR_EP_HUMIDITY: {
                    // Use TempSensor with humidity cluster added
                    auto* ts = new ZigbeeTempSensor(ep_num);
                    ts->addHumiditySensor();
                    ts->setManufacturerAndModel("OpenSprinkler", sensor->name);
                    ep = ts;
                    break;
                }
                case SENSOR_EP_FLOW: {
                    auto* fs = new ZigbeeFlowSensor(ep_num);
                    fs->setManufacturerAndModel("OpenSprinkler", sensor->name);
                    ep = fs;
                    break;
                }
                case SENSOR_EP_ANALOG:
                default: {
                    auto* as = new ZigbeeAnalog(ep_num);
                    as->addAnalogInput();
                    as->setAnalogInputDescription(sensor->name);
                    as->setManufacturerAndModel("OpenSprinkler", sensor->name);
                    ep = as;
                    break;
                }
            }

            Zigbee.addEndpoint(ep);
            s_sensor_eps.push_back({ep, sensor->nr, etype});
            DEBUG_PRINTF(F("[ZB-EXPOSE] Sensor ep=%d nr=%d name='%s' type=%d\n"),
                         ep_num, sensor->nr, sensor->name, etype);
            ep_num++;
        }
    }

    // ---- Zone endpoints ----
    {
        uint8_t max_zones = os.nstations;
        if (max_zones > (EP_ZONE_MAX - EP_ZONE_BASE + 1))
            max_zones = (EP_ZONE_MAX - EP_ZONE_BASE + 1);

        char sname[STATION_NAME_SIZE + 1];
        for (uint8_t sid = 0; sid < max_zones; sid++) {
            uint8_t ep_num = EP_ZONE_BASE + sid;
            os.get_station_name(sid, sname);

            auto* zep = new OSZoneEndpoint(ep_num, sid);
            zep->setManufacturerAndModel("OpenSprinkler", sname);
            Zigbee.addEndpoint(zep);
            s_zone_eps.push_back({zep, sid});
            DEBUG_PRINTF(F("[ZB-EXPOSE] Zone ep=%d sid=%d name='%s'\n"),
                         ep_num, sid, sname);
        }
    }

    // ---- Program endpoints ----
    {
        uint8_t max_progs = pd.nprograms;
        if (max_progs > (EP_PROG_MAX - EP_PROG_BASE + 1))
            max_progs = (EP_PROG_MAX - EP_PROG_BASE + 1);

        ProgramStruct prog;
        for (uint8_t pid = 0; pid < max_progs; pid++) {
            uint8_t ep_num = EP_PROG_BASE + pid;
            pd.read(pid, &prog);

            auto* pep = new OSProgramEndpoint(ep_num, pid);
            pep->setManufacturerAndModel("OpenSprinkler", prog.name);
            Zigbee.addEndpoint(pep);
            s_prog_eps.push_back({pep, pid});
            DEBUG_PRINTF(F("[ZB-EXPOSE] Program ep=%d pid=%d name='%s'\n"),
                         ep_num, pid, prog.name);
        }
    }

    // ---- Rain sensor endpoints ----
    {
        uint8_t ep_num = EP_RAIN_BASE;
        // Sensor 1
        if (os.iopts[IOPT_SENSOR1_TYPE] == SENSOR_TYPE_RAIN ||
            os.iopts[IOPT_SENSOR1_TYPE] == SENSOR_TYPE_SOIL) {
            auto* rb = new ZigbeeBinary(ep_num);
            rb->addBinaryInput();
            rb->setBinaryInputDescription("Rain Sensor 1");
            rb->setManufacturerAndModel("OpenSprinkler", "RainSensor1");
            Zigbee.addEndpoint(rb);
            s_rain_eps.push_back({rb, 0});
            DEBUG_PRINTF(F("[ZB-EXPOSE] Rain sensor 1 ep=%d\n"), ep_num);
            ep_num++;
        }
        // Sensor 2
        if (os.iopts[IOPT_SENSOR2_TYPE] == SENSOR_TYPE_RAIN ||
            os.iopts[IOPT_SENSOR2_TYPE] == SENSOR_TYPE_SOIL) {
            auto* rb = new ZigbeeBinary(ep_num);
            rb->addBinaryInput();
            rb->setBinaryInputDescription("Rain Sensor 2");
            rb->setManufacturerAndModel("OpenSprinkler", "RainSensor2");
            Zigbee.addEndpoint(rb);
            s_rain_eps.push_back({rb, 1});
            DEBUG_PRINTF(F("[ZB-EXPOSE] Rain sensor 2 ep=%d\n"), ep_num);
        }
    }

    DEBUG_PRINTF(F("[ZB-EXPOSE] Created endpoints: %d sensors, %d zones, %d programs, %d rain\n"),
                 s_sensor_eps.size(), s_zone_eps.size(), s_prog_eps.size(), s_rain_eps.size());
}

// =========================================================================
// Update loop — sync OS values to ZCL attributes and report
// =========================================================================
void client_expose_update_loop() {
    unsigned long now = millis();

    // ---- Sensor values (every 30s) ----
    if ((now - s_last_sensor_update) >= UPDATE_INTERVAL_MS) {
        s_last_sensor_update = now;

        for (auto& se : s_sensor_eps) {
            SensorBase* sensor = sensor_by_nr(se.sensor_nr);
            if (!sensor || !sensor->flags.data_ok) continue;

            float val = (float)sensor->last_data;

            switch (se.ep_type) {
                case SENSOR_EP_TEMPERATURE: {
                    auto* ts = static_cast<ZigbeeTempSensor*>(se.ep);
                    // ZigbeeTempSensor expects °C; convert if Fahrenheit
                    unsigned char unit = getSensorUnitId(sensor);
                    float celsius = val;
                    if (unit == UNIT_FAHRENHEIT) celsius = (val - 32.0f) * 5.0f / 9.0f;
                    ts->setTemperature(celsius);
                    ts->reportTemperature();
                    break;
                }
                case SENSOR_EP_HUMIDITY: {
                    auto* ts = static_cast<ZigbeeTempSensor*>(se.ep);
                    ts->setHumidity(val);
                    ts->reportHumidity();
                    break;
                }
                case SENSOR_EP_FLOW: {
                    auto* fs = static_cast<ZigbeeFlowSensor*>(se.ep);
                    fs->setFlow(val);
                    fs->report();
                    break;
                }
                case SENSOR_EP_ANALOG:
                default: {
                    auto* as = static_cast<ZigbeeAnalog*>(se.ep);
                    as->setAnalogInput(val);
                    as->reportAnalogInput();
                    break;
                }
            }
        }
    }

    // ---- Zone state sync (every 5s) ----
    if ((now - s_last_zone_sync) >= ZONE_SYNC_INTERVAL_MS) {
        s_last_zone_sync = now;

        for (auto& ze : s_zone_eps) {
            bool running = OpenSprinkler::is_running(ze.station_id);
            bool ep_state = ze.ep->getPowerOutletState();
            if (running != ep_state) {
                ze.ep->setState(running);
            }
        }

        // Rain sensor state
        for (auto& re : s_rain_eps) {
            bool active;
            if (re.sensor_idx == 0) {
                active = os.status.sensor1_active;
            } else {
                active = os.status.sensor2_active;
            }
            re.ep->setBinaryInput(active);
            re.ep->reportBinaryInput();
        }
    }
}

#endif // ESP32C5 && OS_ENABLE_ZIGBEE
