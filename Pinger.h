/* Unified Pinger Implementation for ESP32 and Linux/OSPi
 * Compatible with ESP8266 Pinger API
 * Supports ping to gateway, IP addresses, and hostnames
 */

#ifndef PINGER_H
#define PINGER_H

#if defined(ESP32)
  #include <Arduino.h>
  #include <WiFi.h>
  #include <lwip/icmp.h>
  #include <lwip/inet_chksum.h>
  #include <lwip/ip.h>
  #include <lwip/sockets.h>
  #include <lwip/netdb.h>
  #include <functional>
  using std::function;
  using String = ::String;
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
  #include <cstring>
  using std::function;
  using String = std::string;
  #include <cstring>
  // Mock IPAddress for Linux
  class IPAddress {
  public:
    uint32_t _address;
    IPAddress() : _address(0) {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
      _address = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
    }
    IPAddress(uint32_t addr) : _address(addr) {}
    
    String toString() const {
      char buf[16];
      snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
        (_address >> 24) & 0xFF, (_address >> 16) & 0xFF,
        (_address >> 8) & 0xFF, _address & 0xFF);
      return String(buf);
    }
    
    const char* c_str() const {
      static char buf[16];
      snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
        (_address >> 24) & 0xFF, (_address >> 16) & 0xFF,
        (_address >> 8) & 0xFF, _address & 0xFF);
      return buf;
    }
    
    operator bool() const { return _address != 0; }
    uint32_t operator*() const { return _address; }
  };
#endif

// PingerResponse struct - compatible with ESP8266 Pinger
struct PingerResponse {
  IPAddress DestIPAddress;
  String DestHostname;
  
  bool ReceivedResponse;
  uint32_t ResponseTime;      // in milliseconds
  uint8_t TimeToLive;
  uint32_t EchoMessageSize;
  
  uint32_t TotalSentRequests;
  uint32_t TotalReceivedResponses;
  uint32_t MinResponseTime;
  uint32_t MaxResponseTime;
  float AvgResponseTime;
  
#if defined(ESP8266)
  const uint8_t* DestMacAddress;
#else
  const uint8_t* DestMacAddress = nullptr;
#endif
  
  PingerResponse() : DestHostname(""), ReceivedResponse(false), ResponseTime(0), TimeToLive(0),
    EchoMessageSize(0), TotalSentRequests(0), TotalReceivedResponses(0),
    MinResponseTime(0), MaxResponseTime(0), AvgResponseTime(0.0f) {}
};

class Pinger {
private:
  function<bool(const PingerResponse&)> onReceive;
  function<bool(const PingerResponse&)> onEnd;
  
  PingerResponse response;
  uint32_t ping_count;
  uint32_t ping_timeout_ms;
  
  bool ping_ip(uint32_t ip_addr);
  bool resolve_hostname(const char* hostname, uint32_t& ip_addr);
  
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

bool Pinger::resolve_hostname(const char* hostname, uint32_t& ip_addr) {
  struct hostent* host = gethostbyname(hostname);
  if (!host || host->h_length != 4) {
    return false;
  }
  memcpy(&ip_addr, host->h_addr, 4);
  return true;
}

bool Pinger::ping_ip(uint32_t ip_addr) {
#if defined(ESP32) || defined(OSPI) || defined(OSBO)
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
  
  // Calculate checksum
  echo->chksum = 0;
#if defined(ESP32)
  echo->chksum = inet_chksum(echo, sizeof(packet));
#else
  // Manual checksum for Linux
  uint32_t sum = 0;
  uint16_t* ptr = (uint16_t*)packet;
  for (size_t i = 0; i < sizeof(packet) / 2; i++) {
    sum += ptr[i];
  }
  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }
  echo->chksum = ~sum;
#endif
  
  // Send ping
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = ip_addr;
  
  uint32_t start_time = 
#if defined(ESP32)
    millis();
#else
    []() {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    }();
#endif
  
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
  
  uint32_t end_time = 
#if defined(ESP32)
    millis();
#else
    []() {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    }();
#endif
  
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
#else
  return false;
#endif
}

bool Pinger::Ping(IPAddress ip, uint32_t count, uint32_t timeout_ms) {
  ping_count = count;
  ping_timeout_ms = timeout_ms;
  
  // Reset response
  response = PingerResponse();
  response.DestIPAddress = ip;
  response.TotalSentRequests = 0;
  response.TotalReceivedResponses = 0;
  response.EchoMessageSize = 64;
  
  uint32_t ip_addr = *ip;
  
  // Perform pings
  for (uint32_t i = 0; i < count; i++) {
    response.TotalSentRequests++;
    ping_ip(ip_addr);
    
#if defined(ESP32)
    delay(100);
#else
    usleep(100000);
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
  if (!resolve_hostname(hostname, &ip_addr)) {
    return false;
  }
  
  response.DestHostname = hostname;
  return Ping(IPAddress(ntohl(ip_addr)), count, timeout_ms);
}

#endif // PINGER_H
