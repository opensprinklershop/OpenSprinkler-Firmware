#if defined(ESP32C5)
#ifdef __cplusplus
extern "C" {
#endif

void ble_svc_sps_reset(void) __attribute__((weak));
void ble_svc_sps_reset(void) {}

#ifdef __cplusplus
}
#endif
#endif