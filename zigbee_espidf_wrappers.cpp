/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * ZigBee ESP-IDF Wrapper Functions - Implementation
 * 
 * See zigbee_espidf_wrappers.h for documentation and rationale.
 * 
 * 2026 @ OpenSprinklerShop
 */

#include "zigbee_espidf_wrappers.h"

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)

#include "OpenSprinkler.h"

// Static storage for attribute ID - must persist until ZBOSS processes the request
static uint16_t s_wrapper_attr_id = 0;

bool zigbee_read_remote_attribute(
    uint64_t device_ieee,
    uint16_t short_addr,
    uint8_t endpoint,
    uint16_t cluster_id,
    uint16_t attribute_id,
    uint8_t* tsn
) {
    // Defensive checks
    if (device_ieee == 0 || short_addr == 0xFFFF || short_addr == 0xFFFE) {
        DEBUG_PRINTLN(F("[ZIGBEE-WRAPPER] Invalid device address"));
        return false;
    }
    
    // Store attribute ID in static memory (ZBOSS requirement: pointer must remain valid)
    s_wrapper_attr_id = attribute_id;
    
    // Build ZCL Read Attributes request
    esp_zb_zcl_read_attr_cmd_t read_req;
    memset(&read_req, 0, sizeof(read_req));
    
    read_req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;  // Unicast by short addr
    read_req.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    read_req.zcl_basic_cmd.dst_endpoint = endpoint;
    read_req.zcl_basic_cmd.src_endpoint = 10;  // OpenSprinkler ZigBee endpoint
    read_req.clusterID = cluster_id;
    read_req.attr_number = 1;
    read_req.attr_field = &s_wrapper_attr_id;  // Static pointer
    
    // Send command (caller must hold lock!)
    uint8_t returned_tsn = esp_zb_zcl_read_attr_cmd_req(&read_req);
    
    if (tsn) {
        *tsn = returned_tsn;
    }
    
    DEBUG_PRINTF(F("[ZIGBEE-WRAPPER] Read attr: ieee=0x%016llX short=0x%04X ep=%d cluster=0x%04X attr=0x%04X TSN=%d\n"),
                 (unsigned long long)device_ieee, short_addr, endpoint, cluster_id, attribute_id, returned_tsn);
    
    return true;
}

#endif // ESP32C5 && OS_ENABLE_ZIGBEE
