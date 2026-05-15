#include "defines.h"

#if defined(ESP32C5) && defined(ENABLE_MATTER)
#include <esp_netif.h>

namespace chip {
namespace DeviceLayer {
namespace Internal {

class ESP32Utils {
public:
    static esp_netif_t * GetStationNetif();
};

esp_netif_t * ESP32Utils::GetStationNetif()
{
    return nullptr;
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
#endif