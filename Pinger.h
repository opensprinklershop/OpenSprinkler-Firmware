/* Unified Pinger Implementation for ESP32 and Linux/OSPi
 * Compatible with ESP8266 Pinger API
 * Supports ping to gateway, IP addresses, and hostnames
 */

#ifndef PINGER_H
#define PINGER_H

#if defined(ESP32)
  #include <Arduino.h>
  #include <WiFi.h>
  #include "ping/ping_sock.h"
  #include <lwip/icmp.h>
  #include <lwip/netdb.h>
  #include "freertos/semphr.h"
  #include <functional>
  using std::function;
#elif defined(ESP8266)
  #include <Arduino.h>
  #include <ESP8266WiFi.h>
  extern "C" {
    #include <ping.h>
  }
  #include <functional>
  using std::function;
#elif defined(OSPI) || defined(OSBO)
  #include <string>
  #include <functional>
  #include <cstdint>
  #include <ctime>
  #include <arpa/inet.h>
  #include <netinet/ip_icmp.h>
  #include <sys/socket.h>
  #include <netdb.h>
  #include <unistd.h>
  using std::function;
  
  // Minimal IPAddress wrapper for Linux - just for compatibility
  class IPAddress {
  public:
    uint32_t v;
    IPAddress() : v(0) {}
    explicit IPAddress(uint32_t addr) : v(addr) {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
      v = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
    }
    
    operator uint32_t() const { return v; }
    operator bool() const { return v != 0; }
    
    std::string toString() const {
      char buf[16];
      snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
        (v >> 24) & 0xFF, (v >> 16) & 0xFF,
        (v >> 8) & 0xFF, v & 0xFF);
      return std::string(buf);
    }
  };
#endif

// PingerResponse struct - compatible with ESP8266 Pinger
struct PingerResponse {
  IPAddress DestIPAddress;
  
#if defined(OSPI) || defined(OSBO)
  std::string DestHostname;
#else
  String DestHostname;
#endif
  
  bool ReceivedResponse;
  uint32_t ResponseTime;      // in milliseconds
  uint8_t TimeToLive;
  uint32_t EchoMessageSize;
  
  uint32_t TotalSentRequests;
  uint32_t TotalReceivedResponses;
  uint32_t MinResponseTime;
  uint32_t MaxResponseTime;
  float AvgResponseTime;
  
  const uint8_t* DestMacAddress = nullptr;
  
#if defined(OSPI) || defined(OSBO)
  PingerResponse() : DestHostname(""), ReceivedResponse(false), ResponseTime(0), TimeToLive(0),
    EchoMessageSize(0), TotalSentRequests(0), TotalReceivedResponses(0),
    MinResponseTime(0), MaxResponseTime(0), AvgResponseTime(0.0f) {}
#else
  PingerResponse() : ReceivedResponse(false), ResponseTime(0), TimeToLive(0),
    EchoMessageSize(0), TotalSentRequests(0), TotalReceivedResponses(0),
    MinResponseTime(0), MaxResponseTime(0), AvgResponseTime(0.0f) {}
#endif
};

class Pinger {
private:
  function<bool(const PingerResponse&)> onReceive;
  function<bool(const PingerResponse&)> onEnd;
  
  PingerResponse response;
  uint32_t ping_count;
  uint32_t ping_timeout_ms;
  
  bool resolve_hostname(const char* hostname, uint32_t& ip_addr);
  
#if defined(ESP32)
  bool ping_ip(uint32_t ip_addr);
#elif defined(ESP8266)
  bool ping_ip_esp8266(uint32_t ip_addr);
#elif defined(OSPI) || defined(OSBO)
  bool ping_ip_linux(uint32_t ip_addr);
#endif
  
public:
  Pinger() : ping_count(4), ping_timeout_ms(1000) {}
  
  void OnReceive(function<bool(const PingerResponse&)> callback) {
    onReceive = callback;
  }
  
  void OnEnd(function<bool(const PingerResponse&)> callback) {
    onEnd = callback;
  }
  
  bool Ping(IPAddress ip, uint32_t count = 4, uint32_t timeout_ms = 1000);
  bool Ping(const char* hostname, uint32_t count = 4, uint32_t timeout_ms = 1000);
};

// ============ Implementation ============

#if defined(ESP8266)
// Async ping result state for ESP8266 SDK callback
static volatile bool _esp8266_ping_done;
static volatile bool _esp8266_ping_success;
static volatile uint32_t _esp8266_ping_time;

static void ICACHE_FLASH_ATTR _ping_recv_cb(void* opt, void* resp) {
  struct ping_resp* pr = reinterpret_cast<struct ping_resp*>(resp);
  if (pr->ping_err == 0 && pr->resp_time > 0) {
    _esp8266_ping_success = true;
    _esp8266_ping_time = pr->resp_time;
  }
  _esp8266_ping_done = true;
}

bool Pinger::ping_ip_esp8266(uint32_t ip_addr) {
  struct ping_option opt;
  memset(&opt, 0, sizeof(opt));
  opt.count = 1;
  opt.ip = ip_addr;
  opt.coarse_time = (ping_timeout_ms + 999) / 1000;  // round up to seconds
  if (opt.coarse_time == 0) opt.coarse_time = 1;

  _esp8266_ping_done = false;
  _esp8266_ping_success = false;
  _esp8266_ping_time = 0;

  ping_regist_recv(&opt, _ping_recv_cb);

  if (!ping_start(&opt)) {
    response.ReceivedResponse = false;
    if (onReceive) onReceive(response);
    return false;
  }

  // Wait for SDK callback with yield to prevent WDT
  uint32_t start = millis();
  while (!_esp8266_ping_done && (millis() - start) < (ping_timeout_ms + 1000)) {
    delay(10);
  }

  if (_esp8266_ping_success) {
    response.ReceivedResponse = true;
    response.ResponseTime = _esp8266_ping_time;
    response.TotalReceivedResponses++;

    if (response.MinResponseTime == 0 || response.ResponseTime < response.MinResponseTime)
      response.MinResponseTime = response.ResponseTime;
    if (response.ResponseTime > response.MaxResponseTime)
      response.MaxResponseTime = response.ResponseTime;

    response.AvgResponseTime =
      (response.AvgResponseTime * (response.TotalReceivedResponses - 1) + response.ResponseTime)
      / response.TotalReceivedResponses;

    if (onReceive) onReceive(response);
    return true;
  }

  response.ReceivedResponse = false;
  if (onReceive) onReceive(response);
  return false;
}
#endif

bool Pinger::resolve_hostname(const char* hostname, uint32_t& ip_addr) {
#if defined(ESP8266)
  IPAddress addr;
  if (WiFi.hostByName(hostname, addr)) {
    ip_addr = (uint32_t)addr;
    return true;
  }
  return false;
#else
  struct hostent* host = gethostbyname(hostname);
  if (!host || host->h_length != 4) {
    return false;
  }
  memcpy(&ip_addr, host->h_addr, 4);
  return true;
#endif
}

#if defined(ESP32)

// Context passed to esp_ping callbacks
struct _PingerCtx {
  SemaphoreHandle_t done;
  Pinger* self;
  uint32_t elapsed_ms;
  bool     received;
};

static void _pinger_on_success(esp_ping_handle_t hdl, void* args) {
  auto* ctx = static_cast<_PingerCtx*>(args);
  uint32_t elapsed = 0;
  esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
  ctx->elapsed_ms = elapsed;
  ctx->received   = true;
}
static void _pinger_on_timeout(esp_ping_handle_t /*hdl*/, void* /*args*/) {}
static void _pinger_on_end(esp_ping_handle_t /*hdl*/, void* args) {
  auto* ctx = static_cast<_PingerCtx*>(args);
  if (ctx->done) xSemaphoreGive(ctx->done);
}

bool Pinger::ping_ip(uint32_t ip_addr) {
  esp_ping_config_t cfg = {};
  cfg.count          = 1;
  cfg.interval_ms    = 0;
  cfg.timeout_ms     = ping_timeout_ms;
  cfg.data_size      = 32;
  cfg.ttl            = 64;
  cfg.task_stack_size = 2048;
  cfg.task_prio      = 2;
  cfg.target_addr.type              = IPADDR_TYPE_V4;
  cfg.target_addr.u_addr.ip4.addr   = ip_addr;  // same byte-order as Arduino IPAddress

  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (!done) return false;

  _PingerCtx ctx = { done, this, 0, false };

  esp_ping_callbacks_t cbs = {};
  cbs.cb_args        = &ctx;
  cbs.on_ping_success = _pinger_on_success;
  cbs.on_ping_timeout = _pinger_on_timeout;
  cbs.on_ping_end     = _pinger_on_end;

  esp_ping_handle_t handle;
  if (esp_ping_new_session(&cfg, &cbs, &handle) != ESP_OK) {
    vSemaphoreDelete(done);
    return false;
  }

  esp_ping_start(handle);
  xSemaphoreTake(done, pdMS_TO_TICKS(ping_timeout_ms + 500));
  esp_ping_stop(handle);
  esp_ping_delete_session(handle);
  vSemaphoreDelete(done);

  if (ctx.received) {
    uint32_t rtt = ctx.elapsed_ms;
    response.TotalReceivedResponses++;
    response.ReceivedResponse = true;
    response.ResponseTime     = rtt;
    if (!response.MinResponseTime || rtt < response.MinResponseTime)
      response.MinResponseTime = rtt;
    if (rtt > response.MaxResponseTime)
      response.MaxResponseTime = rtt;
    response.AvgResponseTime =
      (response.AvgResponseTime * (response.TotalReceivedResponses - 1) + rtt)
      / response.TotalReceivedResponses;
    if (onReceive) onReceive(response);
    return true;
  }

  response.ReceivedResponse = false;
  if (onReceive) onReceive(response);
  return false;
}
#elif defined(OSPI) || defined(OSBO)
bool Pinger::ping_ip_linux(uint32_t ip_addr) {
  int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (sock < 0) {
    return false;
  }
  
  // Set socket timeout
  struct timeval tv;
  tv.tv_sec = ping_timeout_ms / 1000;
  tv.tv_usec = (ping_timeout_ms % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  
  // Prepare ICMP echo request
  struct icmp_echo_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t chksum;
    uint16_t id;
    uint16_t seqno;
  };
  
  char packet[64];
  struct icmp_echo_hdr* echo = (struct icmp_echo_hdr*)packet;
  echo->type = ICMP_ECHO;
  echo->code = 0;
  echo->id = 0xABCD;
  echo->seqno = 1;
  memset(packet + sizeof(struct icmp_echo_hdr), 0xA5, 
         sizeof(packet) - sizeof(struct icmp_echo_hdr));
  
  // Calculate checksum manually for Linux
  echo->chksum = 0;
  uint32_t sum = 0;
  uint16_t* ptr = (uint16_t*)packet;
  for (size_t i = 0; i < sizeof(packet) / 2; i++) {
    sum += ptr[i];
  }
  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }
  echo->chksum = ~sum;
  
  // Send ping
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = ip_addr;
  
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint32_t start_time = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
  
  if (sendto(sock, packet, sizeof(packet), 0, 
             (struct sockaddr*)&addr, sizeof(addr)) <= 0) {
    close(sock);
    return false;
  }
  
  // Wait for reply
  char recv_buf[256];
  socklen_t addr_len = sizeof(addr);
  int bytes = recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                       (struct sockaddr*)&addr, &addr_len);
  
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint32_t end_time = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
  
  close(sock);
  
  if (bytes > 0) {
    response.ReceivedResponse = true;
    response.ResponseTime = end_time - start_time;
    response.TotalReceivedResponses++;
    
    if (response.MinResponseTime == 0 || response.ResponseTime < response.MinResponseTime) {
      response.MinResponseTime = response.ResponseTime;
    }
    if (response.ResponseTime > response.MaxResponseTime) {
      response.MaxResponseTime = response.ResponseTime;
    }
    
    // Update average
    response.AvgResponseTime = 
      (response.AvgResponseTime * (response.TotalReceivedResponses - 1) + response.ResponseTime) 
      / response.TotalReceivedResponses;
    
    if (onReceive) {
      onReceive(response);
    }
    return true;
  }
  
  response.ReceivedResponse = false;
  if (onReceive) {
    onReceive(response);
  }
  return false;
}
#endif

bool Pinger::Ping(IPAddress ip, uint32_t count, uint32_t timeout_ms) {
  ping_count = count;
  ping_timeout_ms = timeout_ms;
  
  // Reset response
  response = PingerResponse();
  response.DestIPAddress = ip;
  response.TotalSentRequests = 0;
  response.TotalReceivedResponses = 0;
  response.EchoMessageSize = 64;
  
  // Convert IPAddress to uint32_t using operator cast
  uint32_t ip_addr = (uint32_t)ip;
  
  // Perform pings
  for (uint32_t i = 0; i < count; i++) {
    response.TotalSentRequests++;
#if defined(ESP32)
    ping_ip(ip_addr);
#elif defined(ESP8266)
    ping_ip_esp8266(ip_addr);
#elif defined(OSPI) || defined(OSBO)
    ping_ip_linux(ip_addr);
#endif
    
#if defined(OSPI) || defined(OSBO)
    usleep(100000);
#else
    delay(100);
#endif
  }
  
  // Call OnEnd callback
  if (onEnd) {
    return onEnd(response);
  }
  return response.TotalReceivedResponses > 0;
}

bool Pinger::Ping(const char* hostname, uint32_t count, uint32_t timeout_ms) {
  uint32_t ip_addr = 0;
  if (!resolve_hostname(hostname, ip_addr)) {
    return false;
  }
  
  response.DestHostname = hostname;
  // h_addr bytes are already in network order; IPAddress uses same layout - no ntohl needed
  return Ping(IPAddress(ip_addr), count, timeout_ms);
}

#endif // PINGER_H
