#pragma once

#if defined(ARDUINO_ARCH_ESP32)

#include "kitsu_gateway_bootstrap.h"
#include "kitsu_gateway_lan_runtime.h"

namespace kitsu868 {
namespace connectivity {

constexpr size_t kEsp32GatewayTlsMinimumFreeHeapBytes = 96U * 1024U;
constexpr size_t kEsp32GatewayTlsMinimumLargestBlockBytes = 48U * 1024U;
constexpr size_t kEsp32GatewayTlsMinimumPostHandshakeFreeBytes = 32U * 1024U;
constexpr size_t kEsp32GatewayTlsMinimumPostHandshakeBlockBytes =
    kLanMaximumFrameBytes;

struct Esp32GatewayTlsMemoryEvidence {
  size_t freeInternalBefore = 0U;
  size_t freeInternalAfter = 0U;
  size_t largestInternalBefore = 0U;
  size_t largestInternalAfter = 0U;
  size_t globalMinimumInternalAfter = 0U;
  bool headroomGuardPassed = false;
};

// Direct mbedTLS adapter. It does not use HTTP, redirects, plaintext sockets,
// or WiFiClientSecure's insecure mode. Endpoint DNS uses trust.host while SNI
// and hostname verification use the separately provisioned serverName.
class Esp32GatewayBootstrapTransport final
    : public GatewayBootstrapTransport {
 public:
  Esp32GatewayBootstrapTransport();
  ~Esp32GatewayBootstrapTransport() override;

  GatewayBootstrapIoResult exchangeOneFramedRequest(
      const GatewayBootstrapTrust& trust, const char* alpn,
      const uint8_t* requestJson, size_t requestBytes,
      uint8_t* responseJson, size_t responseCapacity, size_t& responseBytes,
      GatewayBootstrapTlsEvidence& evidence) override;
  void close() override;
  Esp32GatewayTlsMemoryEvidence memoryEvidence() const;

 private:
  struct Implementation;
  Implementation* implementation_ = nullptr;
  Esp32GatewayTlsMemoryEvidence memory_{};
};

class Esp32GatewayLanTlsTransport final : public GatewayLanTlsTransport {
 public:
  Esp32GatewayLanTlsTransport();
  ~Esp32GatewayLanTlsTransport() override;

  GatewayLanIoResult beginConnect(
      const GatewayLanCredentialView& credentials, const char* alpn,
      uint32_t timeoutMs, GatewayLanTlsEvidence& evidence) override;
  GatewayLanIoResult pollConnect(
      GatewayLanTlsEvidence& evidence) override;
  GatewayLanIoResult writeOneFrame(const uint8_t* frame, size_t frameBytes,
                                   uint32_t timeoutMs) override;
  GatewayLanIoResult receiveOneFrame(const uint8_t*& frame,
                                     size_t& frameBytes,
                                     uint32_t nowMillis,
                                     uint32_t timeoutMs) override;
  void consumeReceivedFrame() override;
  bool connected() const override;
  void close() override;
  Esp32GatewayTlsMemoryEvidence memoryEvidence() const;

 private:
  struct Implementation;
  Implementation* implementation_ = nullptr;
  Esp32GatewayTlsMemoryEvidence memory_{};
};

static_assert(sizeof(Esp32GatewayBootstrapTransport) <= 96U,
              "bootstrap TLS handle unexpectedly became resident-heavy");
static_assert(sizeof(Esp32GatewayLanTlsTransport) <= 96U,
              "steady TLS handle unexpectedly became resident-heavy");

}  // namespace connectivity
}  // namespace kitsu868

#endif  // ARDUINO_ARCH_ESP32
