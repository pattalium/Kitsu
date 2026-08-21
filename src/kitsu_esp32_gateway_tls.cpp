#include "kitsu_esp32_gateway_tls.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <WiFi.h>

#include <errno.h>
#include <fcntl.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <esp_heap_caps.h>
#include <esp_random.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "kitsu_esp32_security.h"
#include "kitsu_connectivity_runtime.h"

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr uint32_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr char kTlsPersonalization[] = "kitsu-esp32-gateway-tls-v1";
constexpr int64_t kMinimumTrustedSystemEpoch = 1704067200LL;  // 2024-01-01
constexpr int64_t kMaximumTrustedSystemEpoch = 4102444800LL;  // 2100-01-01

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}

bool allZero(const uint8_t* input, size_t bytes) {
  if (!input) return true;
  uint8_t combined = 0U;
  for (size_t i = 0U; i < bytes; ++i) combined |= input[i];
  return combined == 0U;
}

bool constantTimeEqual(const uint8_t* left, const uint8_t* right,
                       size_t bytes) {
  if (!left || !right) return false;
  uint8_t difference = 0U;
  for (size_t i = 0U; i < bytes; ++i) difference |= left[i] ^ right[i];
  return difference == 0U;
}

bool validFqdn(const char* value) {
  if (!value) return false;
  const size_t bytes = strlen(value);
  if (bytes == 0U || bytes > 253U || value[0] == '.' ||
      value[bytes - 1U] == '.') {
    return false;
  }
  size_t labelBytes = 0U;
  bool hasDot = false;
  bool hasNonNumericLabelByte = false;
  for (size_t i = 0U; i < bytes; ++i) {
    const char c = value[i];
    if (c == '.') {
      if (labelBytes == 0U || labelBytes > 63U || value[i - 1U] == '-') {
        return false;
      }
      labelBytes = 0U;
      hasDot = true;
      continue;
    }
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-')) {
      return false;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-') {
      hasNonNumericLabelByte = true;
    }
    if (labelBytes == 0U && c == '-') return false;
    ++labelBytes;
  }
  return hasDot && hasNonNumericLabelByte && labelBytes != 0U &&
         labelBytes <= 63U &&
         value[bytes - 1U] != '-';
}

bool validIpv4(const char* value) {
  if (!value) return false;
  const size_t bytes = strlen(value);
  if (bytes < 7U || bytes > 15U) return false;
  size_t start = 0U;
  uint8_t groups = 0U;
  for (size_t i = 0U; i <= bytes; ++i) {
    if (i != bytes && value[i] != '.') continue;
    const size_t count = i - start;
    if (count == 0U || count > 3U ||
        (count > 1U && value[start] == '0')) return false;
    uint16_t part = 0U;
    for (size_t j = start; j < i; ++j) {
      if (value[j] < '0' || value[j] > '9') return false;
      part = static_cast<uint16_t>(part * 10U + value[j] - '0');
    }
    if (part > 255U) return false;
    ++groups;
    start = i + 1U;
  }
  return groups == 4U;
}

bool validIpv6(const char* value) {
  if (!value) return false;
  const size_t bytes = strlen(value);
  if (bytes < 2U || bytes > 45U) return false;
  bool compressed = false;
  for (size_t i = 0U; i + 1U < bytes; ++i) {
    if (i + 2U < bytes && value[i] == ':' && value[i + 1U] == ':' &&
        value[i + 2U] == ':') return false;
    if (value[i] == ':' && value[i + 1U] == ':') {
      if (compressed) return false;
      compressed = true;
      ++i;
    }
  }
  if ((value[0] == ':' && value[1] != ':') ||
      (value[bytes - 1U] == ':' && value[bytes - 2U] != ':')) return false;
  uint8_t groups = 0U;
  size_t start = 0U;
  while (start < bytes) {
    if (value[start] == ':') {
      ++start;
      continue;
    }
    size_t end = start;
    while (end < bytes && value[end] != ':') ++end;
    bool dotted = false;
    for (size_t i = start; i < end; ++i) dotted = dotted || value[i] == '.';
    if (dotted) {
      char embedded[16]{};
      if (end != bytes || end - start >= sizeof(embedded)) return false;
      memcpy(embedded, value + start, end - start);
      if (!validIpv4(embedded)) return false;
      groups = static_cast<uint8_t>(groups + 2U);
    } else {
      if (end - start == 0U || end - start > 4U) return false;
      for (size_t i = start; i < end; ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
      }
      ++groups;
    }
    start = end + 1U;
  }
  return compressed ? groups < 8U : groups == 8U;
}

bool validEndpointHost(const char* value) {
  return validIpv4(value) || validIpv6(value) || validFqdn(value);
}

bool trustedSystemTime() {
  int64_t now = 0;
  return trustedWallClock(now) && now >= kMinimumTrustedSystemEpoch &&
         now <= kMaximumTrustedSystemEpoch;
}

void captureMemoryBefore(Esp32GatewayTlsMemoryEvidence& output) {
  output = Esp32GatewayTlsMemoryEvidence{};
  output.freeInternalBefore = heap_caps_get_free_size(kInternalCaps);
  output.largestInternalBefore =
      heap_caps_get_largest_free_block(kInternalCaps);
  output.headroomGuardPassed =
      output.freeInternalBefore >= kEsp32GatewayTlsMinimumFreeHeapBytes &&
      output.largestInternalBefore >=
          kEsp32GatewayTlsMinimumLargestBlockBytes;
}

bool captureMemoryAfter(Esp32GatewayTlsMemoryEvidence& output) {
  output.freeInternalAfter = heap_caps_get_free_size(kInternalCaps);
  output.largestInternalAfter =
      heap_caps_get_largest_free_block(kInternalCaps);
  output.globalMinimumInternalAfter =
      heap_caps_get_minimum_free_size(kInternalCaps);
  return output.freeInternalAfter >=
             kEsp32GatewayTlsMinimumPostHandshakeFreeBytes &&
         output.largestInternalAfter >=
             kEsp32GatewayTlsMinimumPostHandshakeBlockBytes;
}

int espRandomCallback(void*, unsigned char* output, size_t outputBytes) {
  if (!output && outputBytes != 0U) return -1;
  if (outputBytes != 0U) esp_fill_random(output, outputBytes);
  return 0;
}

bool loadP256Private(const uint8_t* privateKey, size_t privateKeyBytes,
                     mbedtls_pk_context& output) {
  if (!privateKey || privateKeyBytes != kEnrollmentPrivateKeyBytes ||
      mbedtls_pk_setup(
          &output, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) {
    return false;
  }
  mbedtls_ecp_keypair* keypair = mbedtls_pk_ec(output);
  return keypair &&
         mbedtls_ecp_group_load(&keypair->grp,
                                MBEDTLS_ECP_DP_SECP256R1) == 0 &&
         mbedtls_mpi_read_binary(&keypair->d, privateKey,
                                 privateKeyBytes) == 0 &&
         mbedtls_ecp_check_privkey(&keypair->grp, &keypair->d) == 0 &&
         mbedtls_ecp_mul(&keypair->grp, &keypair->Q, &keypair->d,
                         &keypair->grp.G, espRandomCallback, nullptr) == 0;
}

bool clientCertificateBinding(const GatewayLanCredentialView& credentials,
                              mbedtls_pk_context& privateKey) {
  mbedtls_ecp_keypair* keypair = mbedtls_pk_ec(privateKey);
  if (!keypair || keypair->grp.id != MBEDTLS_ECP_DP_SECP256R1) return false;
  uint8_t publicKey[kEnrollmentPublicKeyBytes]{};
  size_t publicKeyBytes = 0U;
  const bool exported =
      mbedtls_ecp_point_write_binary(
          &keypair->grp, &keypair->Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
          &publicKeyBytes, publicKey, sizeof(publicKey)) == 0 &&
      publicKeyBytes == sizeof(publicKey);
  Esp32EnrollmentPlatformCrypto verifier;
  const bool bound = exported && verifier.certificateBindsKeyAndCompanion(
      credentials.leafCertificateDer, credentials.leafCertificateBytes,
      publicKey, credentials.companionUuid);
  secureZero(publicKey, sizeof(publicKey));
  return bound;
}

class DirectMbedTlsClient {
 public:
  enum class ConnectProgress : uint8_t {
    InProgress = 0,
    Ready,
    Failed,
  };

  DirectMbedTlsClient() {
    mbedtls_net_init(&network_);
    mbedtls_ssl_init(&ssl_);
    mbedtls_ssl_config_init(&config_);
    mbedtls_x509_crt_init(&ca_);
    mbedtls_x509_crt_init(&clientCertificate_);
    mbedtls_pk_init(&clientKey_);
    mbedtls_entropy_init(&entropy_);
    mbedtls_ctr_drbg_init(&drbg_);
  }

  ~DirectMbedTlsClient() { close(); }

  DirectMbedTlsClient(const DirectMbedTlsClient&) = delete;
  DirectMbedTlsClient& operator=(const DirectMbedTlsClient&) = delete;

  ConnectProgress connectServerAuthenticated(
      const char* endpointHost, const char* serverName, uint16_t port,
      const uint8_t* caDer, size_t caBytes, const char* alpn,
      uint32_t timeoutMs) {
    if (connectState_ == ConnectState::Idle &&
        !startConnect(endpointHost, serverName, port, caDer, caBytes, alpn,
                      timeoutMs, nullptr, nullptr)) {
      connectState_ = ConnectState::Failed;
    }
    return pollConnect();
  }

  ConnectProgress connectMutualTls(
      const GatewayLanCredentialView& credentials, const char* alpn,
      uint32_t timeoutMs, bool& certificateBound) {
    if (connectState_ == ConnectState::Idle) {
      certificateBound = false;
      if (!startConnect(credentials.host, credentials.serverName,
                        credentials.port, credentials.caCertificateDer,
                        credentials.caCertificateBytes, alpn, timeoutMs,
                        &credentials, &certificateBound)) {
        connectState_ = ConnectState::Failed;
      }
      clientCertificateBound_ = certificateBound;
    } else {
      certificateBound = clientCertificateBound_;
    }
    return pollConnect();
  }

  ConnectProgress pollExistingConnect() { return pollConnect(); }

  bool chainAndNameVerified() const {
    return connected_ && mbedtls_ssl_get_verify_result(&ssl_) == 0U &&
           mbedtls_ssl_get_peer_cert(&ssl_) != nullptr;
  }

  bool negotiatedAlpn(const char* expected) const {
    const char* selected = connected_ ? mbedtls_ssl_get_alpn_protocol(&ssl_)
                                      : nullptr;
    return selected && expected && strcmp(selected, expected) == 0;
  }

  bool tlsVersionAtLeast12() const {
    const char* version = connected_ ? mbedtls_ssl_get_version(&ssl_) : nullptr;
    return version &&
           (strcmp(version, "TLSv1.2") == 0 ||
            strcmp(version, "TLSv1.3") == 0);
  }

  bool clientAuthenticationRequested() const {
    // mbedTLS sets this client-side flag only after the server's TLS 1.2
    // CertificateRequest is processed. With a configured, key-bound leaf and
    // a successful handshake this is evidence that client authentication was
    // actually negotiated, not merely configured locally.
    return connected_ && ssl_.client_auth != 0;
  }

  bool peerSpkiSha256(uint8_t output[32]) const {
    if (!output || !connected_) return false;
    const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(&ssl_);
    if (!peer) return false;
    uint8_t der[256]{};
    const int written = mbedtls_pk_write_pubkey_der(
        const_cast<mbedtls_pk_context*>(&peer->pk), der, sizeof(der));
    bool ok = written > 0 && static_cast<size_t>(written) <= sizeof(der);
    if (ok) {
      ok = mbedtls_sha256_ret(der + sizeof(der) - written,
                              static_cast<size_t>(written), output, 0) == 0;
    }
    secureZero(der, sizeof(der));
    if (!ok) secureZero(output, 32U);
    return ok;
  }

  // One nonblocking TLS write step. Positive is progress, zero is WANT_IO,
  // and -1 is a terminal TLS/transport error.
  int writeSome(const uint8_t* input, size_t inputBytes) {
    if (!input || inputBytes == 0U || !connected_) return -1;
    const int result = mbedtls_ssl_write(&ssl_, input, inputBytes);
    if (result > 0) return result;
    if (result == MBEDTLS_ERR_SSL_WANT_READ ||
        result == MBEDTLS_ERR_SSL_WANT_WRITE) {
      return 0;
    }
    connected_ = false;
    return -1;
  }

  // Returns positive bytes, zero for a nonblocking WANT_READ, -2 for orderly
  // close, and -1 for a transport/TLS failure.
  int readSome(uint8_t* output, size_t outputCapacity) {
    if (!output || outputCapacity == 0U || !connected_) return -1;
    const int result = mbedtls_ssl_read(&ssl_, output, outputCapacity);
    if (result > 0) return result;
    if (result == MBEDTLS_ERR_SSL_WANT_READ ||
        result == MBEDTLS_ERR_SSL_WANT_WRITE) {
      return 0;
    }
    if (result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
      connected_ = false;
      return -2;
    }
    connected_ = false;
    return -1;
  }

  size_t pendingBytes() const {
    return connected_ ? mbedtls_ssl_get_bytes_avail(&ssl_) : 0U;
  }

  bool connected() const { return connected_; }

  void close() {
    if (connected_) (void)mbedtls_ssl_close_notify(&ssl_);
    connected_ = false;
    mbedtls_net_free(&network_);
    mbedtls_ssl_free(&ssl_);
    mbedtls_ssl_config_free(&config_);
    mbedtls_x509_crt_free(&ca_);
    mbedtls_x509_crt_free(&clientCertificate_);
    mbedtls_pk_free(&clientKey_);
    mbedtls_ctr_drbg_free(&drbg_);
    mbedtls_entropy_free(&entropy_);
    connectState_ = ConnectState::Failed;
  }

 private:
  enum class ConnectState : uint8_t {
    Idle = 0,
    TcpConnecting,
    Handshaking,
    Ready,
    Failed,
  };

  bool startNonblockingSocket(int family, int socketType, int protocol,
                              const sockaddr* address,
                              socklen_t addressBytes) {
    if (!address || addressBytes == 0U) return false;
    const int socketFd = lwip_socket(family, socketType, protocol);
    if (socketFd < 0) return false;
    const int oldFlags = fcntl(socketFd, F_GETFL, 0);
    if (oldFlags < 0 ||
        fcntl(socketFd, F_SETFL, oldFlags | O_NONBLOCK) < 0) {
      lwip_close(socketFd);
      return false;
    }
    const int result = lwip_connect(socketFd, address, addressBytes);
    if (result != 0 && (result >= 0 || errno != EINPROGRESS)) {
      lwip_close(socketFd);
      return false;
    }
    network_.fd = socketFd;
    connectState_ = result == 0 ? ConnectState::Handshaking
                                : ConnectState::TcpConnecting;
    return true;
  }

  bool startTcp(const char* endpointHost, uint16_t port) {
    if (WiFi.status() != WL_CONNECTED || !endpointHost || port == 0U) {
      return false;
    }
    // The production catalog deliberately routes by numeric home-LAN IP and
    // keeps DNS-only serverName separate for SNI/certificate verification.
    // Parse numeric IPv4/IPv6 locally so this reliable path never enters a
    // potentially blocking DNS resolver on the Arduino loop.
    sockaddr_in ipv4{};
    ipv4.sin_family = AF_INET;
    ipv4.sin_port = lwip_htons(port);
    if (lwip_inet_pton(AF_INET, endpointHost, &ipv4.sin_addr) == 1) {
      return startNonblockingSocket(
          AF_INET, SOCK_STREAM, IPPROTO_TCP,
          reinterpret_cast<const sockaddr*>(&ipv4), sizeof(ipv4));
    }
    sockaddr_in6 ipv6{};
    ipv6.sin6_family = AF_INET6;
    ipv6.sin6_port = lwip_htons(port);
    if (lwip_inet_pton(AF_INET6, endpointHost, &ipv6.sin6_addr) == 1) {
      return startNonblockingSocket(
          AF_INET6, SOCK_STREAM, IPPROTO_TCP,
          reinterpret_cast<const sockaddr*>(&ipv6), sizeof(ipv6));
    }

    // Provisioned DNS names remain supported for other installations. The
    // release catalog uses the numeric branch above; .local resolution is not
    // a hidden prerequisite for kitsu-host.
    char service[6]{};
    const int serviceBytes = snprintf(service, sizeof(service), "%u", port);
    if (serviceBytes <= 0 ||
        static_cast<size_t>(serviceBytes) >= sizeof(service)) return false;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    if (lwip_getaddrinfo(endpointHost, service, &hints, &addresses) != 0 ||
        !addresses) return false;

    bool accepted = false;
    for (addrinfo* address = addresses; address && !accepted;
         address = address->ai_next) {
      if (!address->ai_addr || address->ai_addrlen == 0U) continue;
      accepted = startNonblockingSocket(
          address->ai_family, address->ai_socktype, address->ai_protocol,
          address->ai_addr, static_cast<socklen_t>(address->ai_addrlen));
    }
    lwip_freeaddrinfo(addresses);
    return accepted;
  }

  bool startConnect(const char* endpointHost, const char* serverName,
                    uint16_t port, const uint8_t* caDer, size_t caBytes,
                    const char* alpn, uint32_t timeoutMs,
                    const GatewayLanCredentialView* clientCredentials,
                    bool* certificateBound = nullptr) {
    if (!validEndpointHost(endpointHost) || !validFqdn(serverName) ||
        port == 0U ||
        !caDer || caBytes == 0U || caBytes > kGatewayLanMaximumCaBytes ||
        !alpn || alpn[0] == '\0' || timeoutMs == 0U) {
      return false;
    }
    if (mbedtls_ctr_drbg_seed(
            &drbg_, mbedtls_entropy_func, &entropy_,
            reinterpret_cast<const uint8_t*>(kTlsPersonalization),
            sizeof(kTlsPersonalization) - 1U) != 0 ||
        mbedtls_x509_crt_parse_der(&ca_, caDer, caBytes) != 0 ||
        mbedtls_ssl_config_defaults(&config_, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
      return false;
    }
    // Arduino-ESP32 2.x enables TLS 1.0/1.1 in its SDK configuration, so the
    // minimum is explicitly raised before any handshake or application byte.
    mbedtls_ssl_conf_min_version(&config_, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_authmode(&config_, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&config_, &ca_, nullptr);
    mbedtls_ssl_conf_rng(&config_, mbedtls_ctr_drbg_random, &drbg_);
    alpnProtocols_[0] = alpn;
    alpnProtocols_[1] = nullptr;
    if (mbedtls_ssl_conf_alpn_protocols(&config_, alpnProtocols_) != 0) {
      return false;
    }

    if (clientCredentials) {
      if (mbedtls_x509_crt_parse_der(
              &clientCertificate_,
              clientCredentials->leafCertificateDer,
              clientCredentials->leafCertificateBytes) != 0) {
        return false;
      }
      for (size_t i = 0U;
           i < clientCredentials->certificateChainCount; ++i) {
        if (mbedtls_x509_crt_parse_der(
                &clientCertificate_,
                clientCredentials->certificateChainDer[i],
                clientCredentials->certificateChainBytes[i]) != 0) {
          return false;
        }
      }
      if (!loadP256Private(clientCredentials->privateKey,
                           clientCredentials->privateKeyBytes, clientKey_)) {
        return false;
      }
      uint32_t clientVerifyFlags = 0U;
      if (mbedtls_x509_crt_verify(&clientCertificate_, &ca_, nullptr,
                                  nullptr, &clientVerifyFlags, nullptr,
                                  nullptr) != 0 ||
          clientVerifyFlags != 0U) {
        return false;
      }
      const bool bound =
          clientCertificateBinding(*clientCredentials, clientKey_);
      if (certificateBound) *certificateBound = bound;
      if (!bound ||
          mbedtls_ssl_conf_own_cert(&config_, &clientCertificate_,
                                    &clientKey_) != 0) {
        return false;
      }
    }
    if (mbedtls_ssl_setup(&ssl_, &config_) != 0 ||
        // This exact value is both the SNI extension and the certificate-name
        // verification input. endpointHost is used only for DNS/TCP routing.
        mbedtls_ssl_set_hostname(&ssl_, serverName) != 0 ||
        !startTcp(endpointHost, port)) {
      return false;
    }
    mbedtls_ssl_set_bio(&ssl_, &network_, mbedtls_net_send,
                        mbedtls_net_recv, nullptr);
    connectStartedAt_ = millis();
    connectTimeoutMs_ = timeoutMs;
    return true;
  }

  ConnectProgress pollConnect() {
    if (connectState_ == ConnectState::Ready) return ConnectProgress::Ready;
    if (connectState_ == ConnectState::Failed ||
        connectState_ == ConnectState::Idle) {
      return ConnectProgress::Failed;
    }
    if (static_cast<uint32_t>(millis() - connectStartedAt_) >=
        connectTimeoutMs_) {
      connectState_ = ConnectState::Failed;
      return ConnectProgress::Failed;
    }
    if (connectState_ == ConnectState::TcpConnecting) {
      fd_set writable;
      FD_ZERO(&writable);
      FD_SET(network_.fd, &writable);
      timeval wait{};
      const int selected =
          lwip_select(network_.fd + 1, nullptr, &writable, nullptr, &wait);
      if (selected == 0) return ConnectProgress::InProgress;
      int socketError = 0;
      socklen_t errorBytes = sizeof(socketError);
      if (selected < 0 ||
          lwip_getsockopt(network_.fd, SOL_SOCKET, SO_ERROR, &socketError,
                          &errorBytes) < 0 ||
          socketError != 0) {
        connectState_ = ConnectState::Failed;
        return ConnectProgress::Failed;
      }
      connectState_ = ConnectState::Handshaking;
    }
    const int handshake = mbedtls_ssl_handshake(&ssl_);
    if (handshake == MBEDTLS_ERR_SSL_WANT_READ ||
        handshake == MBEDTLS_ERR_SSL_WANT_WRITE) {
      return ConnectProgress::InProgress;
    }
    if (handshake != 0) {
      connectState_ = ConnectState::Failed;
      return ConnectProgress::Failed;
    }
    if (mbedtls_ssl_get_verify_result(&ssl_) != 0U ||
        mbedtls_ssl_get_peer_cert(&ssl_) == nullptr) {
      connectState_ = ConnectState::Failed;
      return ConnectProgress::Failed;
    }
    // The socket remains nonblocking from TCP connect through the TLS
    // handshake, so the outer elapsed-time check is a real overall deadline
    // rather than a series of independently blocking socket waits. Steady
    // framing then owns its own independent assembly timeout.
    const int flags = fcntl(network_.fd, F_GETFL, 0);
    if (flags < 0 || fcntl(network_.fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      connectState_ = ConnectState::Failed;
      return ConnectProgress::Failed;
    }
    connected_ = true;
    connectState_ = ConnectState::Ready;
    return ConnectProgress::Ready;
  }

  mbedtls_net_context network_{};
  mbedtls_ssl_context ssl_{};
  mbedtls_ssl_config config_{};
  mbedtls_x509_crt ca_{};
  mbedtls_x509_crt clientCertificate_{};
  mbedtls_pk_context clientKey_{};
  mbedtls_entropy_context entropy_{};
  mbedtls_ctr_drbg_context drbg_{};
  const char* alpnProtocols_[2]{};
  bool connected_ = false;
  bool clientCertificateBound_ = false;
  ConnectState connectState_ = ConnectState::Idle;
  uint32_t connectStartedAt_ = 0U;
  uint32_t connectTimeoutMs_ = 0U;
};

bool trustEvidence(DirectMbedTlsClient& client, const uint8_t expected[32],
                   const char* alpn, bool& chainAndName, bool& spki,
                   bool& selectedAlpn, bool& tls12) {
  uint8_t actual[32]{};
  chainAndName = client.chainAndNameVerified();
  selectedAlpn = client.negotiatedAlpn(alpn);
  tls12 = client.tlsVersionAtLeast12();
  spki = client.peerSpkiSha256(actual) &&
         constantTimeEqual(actual, expected, sizeof(actual));
  secureZero(actual, sizeof(actual));
  return chainAndName && spki && selectedAlpn && tls12;
}

GatewayLanIoResult evaluateSteadyConnect(
    DirectMbedTlsClient::ConnectProgress progress,
    DirectMbedTlsClient& client, const uint8_t expectedSpki[32],
    bool certificateBound, Esp32GatewayTlsMemoryEvidence& memory,
    GatewayLanTlsEvidence& evidence) {
  evidence = GatewayLanTlsEvidence{};
  evidence.systemTimeValid = trustedSystemTime();
  evidence.plaintextFallbackUsed = false;
  evidence.redirectFollowed = false;
  if (!evidence.systemTimeValid) return GatewayLanIoResult::TimeUnavailable;
  if (progress == DirectMbedTlsClient::ConnectProgress::InProgress) {
    return GatewayLanIoResult::WouldBlock;
  }
  const bool handshakeSucceeded =
      progress == DirectMbedTlsClient::ConnectProgress::Ready;
  bool ok = handshakeSucceeded;
  if (ok) {
    bool chainAndName = false;
    ok = trustEvidence(client, expectedSpki, kGatewayLanAlpn, chainAndName,
                       evidence.spkiMatched, evidence.alpnMatched,
                       evidence.tlsVersionAtLeast12);
    evidence.serverChainVerified = chainAndName;
    evidence.serverNameVerified = chainAndName;
  }
  evidence.clientCredentialPresented =
      handshakeSucceeded && client.clientAuthenticationRequested();
  evidence.clientCertificateBindsCompanion = certificateBound;
  bool postHandshakeMemoryOk = true;
  if (ok) {
    postHandshakeMemoryOk = captureMemoryAfter(memory);
    ok = postHandshakeMemoryOk;
  }
  if (!ok || !evidence.serverChainVerified ||
      !evidence.serverNameVerified || !evidence.spkiMatched ||
      !evidence.alpnMatched || !evidence.tlsVersionAtLeast12 ||
      !evidence.clientCredentialPresented ||
      !evidence.clientCertificateBindsCompanion) {
    if (!postHandshakeMemoryOk) return GatewayLanIoResult::OutOfMemory;
    return handshakeSucceeded ? GatewayLanIoResult::SecurityRejected
                              : GatewayLanIoResult::IoFailed;
  }
  return GatewayLanIoResult::Ok;
}

void putU32Be(uint8_t output[4], uint32_t value) {
  output[0] = static_cast<uint8_t>(value >> 24U);
  output[1] = static_cast<uint8_t>(value >> 16U);
  output[2] = static_cast<uint8_t>(value >> 8U);
  output[3] = static_cast<uint8_t>(value);
}

uint32_t getU32Be(const uint8_t input[4]) {
  return (static_cast<uint32_t>(input[0]) << 24U) |
         (static_cast<uint32_t>(input[1]) << 16U) |
         (static_cast<uint32_t>(input[2]) << 8U) |
         static_cast<uint32_t>(input[3]);
}

}  // namespace

struct Esp32GatewayBootstrapTransport::Implementation {
  DirectMbedTlsClient client{};
  const uint8_t* request = nullptr;
  size_t requestBytes = 0U;
  uint8_t* response = nullptr;
  size_t responseCapacity = 0U;
  size_t responseBytes = 0U;
  uint8_t requestHeader[4]{};
  size_t requestHeaderSent = 0U;
  size_t requestSent = 0U;
  uint8_t responseHeader[4]{};
  size_t responseHeaderBytes = 0U;
  size_t expectedResponseBytes = 0U;
  uint32_t ioStartedAtMillis = 0U;
  GatewayBootstrapTlsEvidence evidence{};
  bool trustAccepted = false;
  bool terminal = false;
  GatewayBootstrapIoResult terminalResult = GatewayBootstrapIoResult::Failed;
};

Esp32GatewayBootstrapTransport::Esp32GatewayBootstrapTransport() = default;

Esp32GatewayBootstrapTransport::~Esp32GatewayBootstrapTransport() { close(); }

GatewayBootstrapIoResult
Esp32GatewayBootstrapTransport::exchangeOneFramedRequest(
    const GatewayBootstrapTrust& trust, const char* alpn,
    const uint8_t* requestJson, size_t requestBytes, uint8_t* responseJson,
    size_t responseCapacity, size_t& responseBytes,
    GatewayBootstrapTlsEvidence& evidence) {
  responseBytes = 0U;
  evidence = implementation_ ? implementation_->evidence
                             : GatewayBootstrapTlsEvidence{};
  evidence.systemTimeChecked = true;
  evidence.systemTimeValid = trustedSystemTime();
  if (!validEndpointHost(trust.host) || !validFqdn(trust.serverName) ||
      trust.port == 0U || !trust.caCertificateDer ||
      trust.caCertificateBytes == 0U ||
      trust.caCertificateBytes > kGatewayBootstrapMaximumCaBytes ||
      allZero(trust.spkiSha256, sizeof(trust.spkiSha256)) ||
      !alpn || strcmp(alpn, kGatewayBootstrapAlpn) != 0 || !requestJson ||
      requestBytes == 0U ||
      requestBytes > kGatewayBootstrapMaximumProxyRequestBytes ||
      !responseJson || responseCapacity == 0U ||
      responseCapacity > kGatewayBootstrapMaximumProxyResponseBytes) {
    return GatewayBootstrapIoResult::Failed;
  }
  // mbedTLS certificate validity is never bypassed. Cold boot can recover
  // time asynchronously through completed SNTP, but an unproven libc clock
  // never enables this socket.
  if (!evidence.systemTimeValid) return GatewayBootstrapIoResult::Failed;
  if (!implementation_) {
    captureMemoryBefore(memory_);
    if (!memory_.headroomGuardPassed) return GatewayBootstrapIoResult::Failed;
    implementation_ = new (std::nothrow) Implementation();
    if (!implementation_) return GatewayBootstrapIoResult::Failed;
    implementation_->request = requestJson;
    implementation_->requestBytes = requestBytes;
    implementation_->response = responseJson;
    implementation_->responseCapacity = responseCapacity;
    implementation_->evidence = evidence;
    putU32Be(implementation_->requestHeader,
             static_cast<uint32_t>(requestBytes));
  }
  Implementation& state = *implementation_;
  if (state.request != requestJson || state.requestBytes != requestBytes ||
      state.response != responseJson ||
      state.responseCapacity != responseCapacity) {
    return GatewayBootstrapIoResult::Failed;
  }
  if (state.terminal) {
    evidence = state.evidence;
    responseBytes = state.terminalResult == GatewayBootstrapIoResult::Ok
        ? state.responseBytes
        : 0U;
    return state.terminalResult;
  }

  const DirectMbedTlsClient::ConnectProgress progress =
      state.client.connectServerAuthenticated(
          trust.host, trust.serverName, trust.port, trust.caCertificateDer,
          trust.caCertificateBytes, alpn, kGatewayLanConnectTimeoutMs);
  if (progress == DirectMbedTlsClient::ConnectProgress::InProgress) {
    return GatewayBootstrapIoResult::WouldBlock;
  }
  bool ok = progress == DirectMbedTlsClient::ConnectProgress::Ready;
  if (ok && !state.trustAccepted) {
    bool chainAndName = false;
    ok = trustEvidence(state.client, trust.spkiSha256, alpn, chainAndName,
                       state.evidence.spkiMatched,
                       state.evidence.alpnMatched,
                       state.evidence.tlsVersionAtLeast12);
    state.evidence.serverChainVerified = chainAndName;
    state.evidence.serverNameVerified = chainAndName;
    state.evidence.clientCredentialPresented = false;
    state.evidence.plaintextFallbackUsed = false;
    state.evidence.redirectFollowed = false;
    state.evidence.systemTimeChecked = true;
    state.evidence.systemTimeValid = true;
    if (ok) {
      ok = captureMemoryAfter(memory_);
      state.trustAccepted = ok;
      state.ioStartedAtMillis = millis();
    }
  }
  if (!ok || !state.trustAccepted ||
      static_cast<uint32_t>(millis() - state.ioStartedAtMillis) >=
          kGatewayLanIoTimeoutMs) {
    state.terminal = true;
    state.terminalResult = GatewayBootstrapIoResult::Failed;
    secureZero(responseJson, responseCapacity);
    evidence = state.evidence;
    return state.terminalResult;
  }
  if (state.requestHeaderSent < sizeof(state.requestHeader)) {
    const int written = state.client.writeSome(
        state.requestHeader + state.requestHeaderSent,
        sizeof(state.requestHeader) - state.requestHeaderSent);
    if (written < 0) ok = false;
    if (written > 0) state.requestHeaderSent += static_cast<size_t>(written);
    if (ok && state.requestHeaderSent < sizeof(state.requestHeader)) {
      return GatewayBootstrapIoResult::WouldBlock;
    }
  } else if (state.requestSent < state.requestBytes) {
    const int written = state.client.writeSome(
        state.request + state.requestSent,
        state.requestBytes - state.requestSent);
    if (written < 0) ok = false;
    if (written > 0) state.requestSent += static_cast<size_t>(written);
    if (ok && state.requestSent < state.requestBytes) {
      return GatewayBootstrapIoResult::WouldBlock;
    }
  } else if (state.responseHeaderBytes < sizeof(state.responseHeader)) {
    const int received = state.client.readSome(
        state.responseHeader + state.responseHeaderBytes,
        sizeof(state.responseHeader) - state.responseHeaderBytes);
    if (received < 0) ok = false;
    if (received > 0) {
      state.responseHeaderBytes += static_cast<size_t>(received);
    }
    if (ok && state.responseHeaderBytes < sizeof(state.responseHeader)) {
      return GatewayBootstrapIoResult::WouldBlock;
    }
    if (ok) {
      state.expectedResponseBytes = getU32Be(state.responseHeader);
      ok = state.expectedResponseBytes != 0U &&
           state.expectedResponseBytes <=
               kGatewayBootstrapMaximumProxyResponseBytes &&
           state.expectedResponseBytes <= state.responseCapacity;
    }
  } else if (state.responseBytes < state.expectedResponseBytes) {
    const int received = state.client.readSome(
        state.response + state.responseBytes,
        state.expectedResponseBytes - state.responseBytes);
    if (received < 0) ok = false;
    if (received > 0) state.responseBytes += static_cast<size_t>(received);
    if (ok && state.responseBytes < state.expectedResponseBytes) {
      return GatewayBootstrapIoResult::WouldBlock;
    }
  }
  if (ok && state.requestSent == state.requestBytes &&
      state.responseHeaderBytes == sizeof(state.responseHeader) &&
      state.responseBytes == state.expectedResponseBytes) {
    ok = state.client.pendingBytes() == 0U;
    if (ok) {
      state.terminal = true;
      state.terminalResult = GatewayBootstrapIoResult::Ok;
      responseBytes = state.responseBytes;
      evidence = state.evidence;
      return state.terminalResult;
    }
  }
  if (!ok) {
    secureZero(responseJson, responseCapacity);
    state.responseBytes = 0U;
    state.terminal = true;
    state.terminalResult = GatewayBootstrapIoResult::Failed;
  }
  evidence = state.evidence;
  return ok ? GatewayBootstrapIoResult::WouldBlock : state.terminalResult;
}

void Esp32GatewayBootstrapTransport::close() {
  if (implementation_) {
    delete implementation_;
    implementation_ = nullptr;
  }
}

Esp32GatewayTlsMemoryEvidence
Esp32GatewayBootstrapTransport::memoryEvidence() const {
  return memory_;
}

struct Esp32GatewayLanTlsTransport::Implementation {
  DirectMbedTlsClient client{};
  uint8_t expectedSpki[kGatewayLanSpkiBytes]{};
  bool certificateBound = false;
  uint8_t* sendFrame = nullptr;
  size_t sendFrameBytes = 0U;
  size_t sentBytes = 0U;
  uint32_t sendStartedAtMillis = 0U;
  uint8_t header[4]{};
  uint8_t headerBytes = 0U;
  uint8_t* frame = nullptr;
  size_t expectedBytes = 0U;
  size_t receivedBytes = 0U;
  uint32_t startedAtMillis = 0U;
  bool started = false;
  bool ready = false;

  ~Implementation() {
    clearSend();
    clearFrame();
  }

  void clearSend() {
    if (sendFrame) {
      secureZero(sendFrame, sendFrameBytes);
      heap_caps_free(sendFrame);
    }
    sendFrame = nullptr;
    sendFrameBytes = 0U;
    sentBytes = 0U;
    sendStartedAtMillis = 0U;
  }

  void clearFrame() {
    if (frame) {
      secureZero(frame, expectedBytes);
      heap_caps_free(frame);
    }
    frame = nullptr;
    expectedBytes = 0U;
    receivedBytes = 0U;
    headerBytes = 0U;
    secureZero(header, sizeof(header));
    startedAtMillis = 0U;
    started = false;
    ready = false;
  }
};

Esp32GatewayLanTlsTransport::Esp32GatewayLanTlsTransport() = default;

Esp32GatewayLanTlsTransport::~Esp32GatewayLanTlsTransport() { close(); }

GatewayLanIoResult Esp32GatewayLanTlsTransport::beginConnect(
    const GatewayLanCredentialView& credentials, const char* alpn,
    uint32_t timeoutMs, GatewayLanTlsEvidence& evidence) {
  evidence = GatewayLanTlsEvidence{};
  evidence.systemTimeValid = trustedSystemTime();
  if (!validEndpointHost(credentials.host) ||
      !validFqdn(credentials.serverName) ||
      credentials.port == 0U || !credentials.caCertificateDer ||
      credentials.caCertificateBytes == 0U ||
      credentials.caCertificateBytes > kGatewayLanMaximumCaBytes ||
      allZero(credentials.spkiSha256, sizeof(credentials.spkiSha256)) ||
      !credentials.privateKey ||
      credentials.privateKeyBytes != kEnrollmentPrivateKeyBytes ||
      !credentials.leafCertificateDer ||
      credentials.leafCertificateBytes == 0U ||
      credentials.certificateChainCount == 0U ||
      credentials.certificateChainCount >
          kEnrollmentMaximumChainCertificates ||
      !alpn || strcmp(alpn, kGatewayLanAlpn) != 0 || timeoutMs == 0U) {
    return GatewayLanIoResult::InvalidArgument;
  }
  if (!evidence.systemTimeValid) return GatewayLanIoResult::TimeUnavailable;
  if (implementation_) return GatewayLanIoResult::InvalidArgument;
  captureMemoryBefore(memory_);
  if (!memory_.headroomGuardPassed) return GatewayLanIoResult::OutOfMemory;
  implementation_ = new (std::nothrow) Implementation();
  if (!implementation_) return GatewayLanIoResult::OutOfMemory;
  memcpy(implementation_->expectedSpki, credentials.spkiSha256,
         sizeof(implementation_->expectedSpki));

  bool certificateBound = false;
  const DirectMbedTlsClient::ConnectProgress progress =
      implementation_->client.connectMutualTls(
          credentials, alpn, timeoutMs, certificateBound);
  implementation_->certificateBound = certificateBound;
  const GatewayLanIoResult result = evaluateSteadyConnect(
      progress, implementation_->client, implementation_->expectedSpki,
      implementation_->certificateBound, memory_, evidence);
  if (result != GatewayLanIoResult::Ok &&
      result != GatewayLanIoResult::WouldBlock) {
    close();
  }
  return result;
}

GatewayLanIoResult Esp32GatewayLanTlsTransport::pollConnect(
    GatewayLanTlsEvidence& evidence) {
  if (!implementation_) {
    evidence = GatewayLanTlsEvidence{};
    return GatewayLanIoResult::InvalidArgument;
  }
  const DirectMbedTlsClient::ConnectProgress progress =
      implementation_->client.pollExistingConnect();
  const GatewayLanIoResult result = evaluateSteadyConnect(
      progress, implementation_->client, implementation_->expectedSpki,
      implementation_->certificateBound, memory_, evidence);
  if (result != GatewayLanIoResult::Ok &&
      result != GatewayLanIoResult::WouldBlock) {
    close();
  }
  return result;
}

GatewayLanIoResult Esp32GatewayLanTlsTransport::writeOneFrame(
    const uint8_t* frame, size_t frameBytes, uint32_t timeoutMs) {
  if (!frame || frameBytes == 0U || frameBytes > kLanMaximumFrameBytes ||
      !implementation_ || !implementation_->client.connected()) {
    return GatewayLanIoResult::InvalidArgument;
  }
  Implementation& state = *implementation_;
  if (!state.sendFrame) {
    const size_t framedBytes = sizeof(uint32_t) + frameBytes;
    state.sendFrame = static_cast<uint8_t*>(
        heap_caps_malloc(framedBytes, kInternalCaps));
    if (!state.sendFrame) return GatewayLanIoResult::OutOfMemory;
    state.sendFrameBytes = framedBytes;
    state.sentBytes = 0U;
    state.sendStartedAtMillis = millis();
    putU32Be(state.sendFrame, static_cast<uint32_t>(frameBytes));
    memcpy(state.sendFrame + sizeof(uint32_t), frame, frameBytes);
  } else if (state.sendFrameBytes != sizeof(uint32_t) + frameBytes ||
             memcmp(state.sendFrame + sizeof(uint32_t), frame,
                    frameBytes) != 0) {
    return GatewayLanIoResult::InvalidArgument;
  }
  if (static_cast<uint32_t>(millis() - state.sendStartedAtMillis) >=
      timeoutMs) {
    state.clearSend();
    return GatewayLanIoResult::TimedOut;
  }
  const int written = state.client.writeSome(
      state.sendFrame + state.sentBytes,
      state.sendFrameBytes - state.sentBytes);
  if (written < 0) {
    state.clearSend();
    return GatewayLanIoResult::IoFailed;
  }
  if (written == 0) return GatewayLanIoResult::WouldBlock;
  state.sentBytes += static_cast<size_t>(written);
  if (state.sentBytes < state.sendFrameBytes) {
    return GatewayLanIoResult::WouldBlock;
  }
  state.clearSend();
  return GatewayLanIoResult::Ok;
}

GatewayLanIoResult Esp32GatewayLanTlsTransport::receiveOneFrame(
    const uint8_t*& frame, size_t& frameBytes, uint32_t nowMillis,
    uint32_t timeoutMs) {
  frame = nullptr;
  frameBytes = 0U;
  if (!implementation_ || !implementation_->client.connected() ||
      timeoutMs == 0U) {
    return GatewayLanIoResult::Closed;
  }
  Implementation& state = *implementation_;
  if (state.ready) {
    frame = state.frame;
    frameBytes = state.expectedBytes;
    return GatewayLanIoResult::Ok;
  }
  if (state.started &&
      static_cast<uint32_t>(nowMillis - state.startedAtMillis) >= timeoutMs) {
    return GatewayLanIoResult::TimedOut;
  }
  uint8_t* destination = nullptr;
  size_t capacity = 0U;
  if (state.headerBytes < sizeof(state.header)) {
    destination = state.header + state.headerBytes;
    capacity = sizeof(state.header) - state.headerBytes;
  } else {
    destination = state.frame + state.receivedBytes;
    capacity = state.expectedBytes - state.receivedBytes;
  }
  // Exactly one nonblocking TLS read is handed off per Arduino loop. Even if
  // a complete frame is already buffered, parsing cannot turn into an
  // unbounded drain loop that starves BLE, MeshCore, or the portrait UI.
  const int received = state.client.readSome(destination, capacity);
  if (received == 0) return GatewayLanIoResult::WouldBlock;
  if (received == -2) return GatewayLanIoResult::Closed;
  if (received < 0) return GatewayLanIoResult::IoFailed;
  if (!state.started) {
    state.started = true;
    state.startedAtMillis = nowMillis;
  }
  if (state.headerBytes < sizeof(state.header)) {
    state.headerBytes = static_cast<uint8_t>(
        state.headerBytes + static_cast<size_t>(received));
    if (state.headerBytes < sizeof(state.header)) {
      return GatewayLanIoResult::WouldBlock;
    }
    const uint32_t declared = getU32Be(state.header);
    if (declared == 0U || declared > kLanMaximumFrameBytes) {
      return GatewayLanIoResult::SecurityRejected;
    }
    state.expectedBytes = declared;
    state.frame = static_cast<uint8_t*>(
        heap_caps_malloc(state.expectedBytes, kInternalCaps));
    if (!state.frame) return GatewayLanIoResult::OutOfMemory;
    memset(state.frame, 0, state.expectedBytes);
    return GatewayLanIoResult::WouldBlock;
  }
  state.receivedBytes += static_cast<size_t>(received);
  if (state.receivedBytes == state.expectedBytes) {
    state.ready = true;
    frame = state.frame;
    frameBytes = state.expectedBytes;
    return GatewayLanIoResult::Ok;
  }
  return GatewayLanIoResult::WouldBlock;
}

void Esp32GatewayLanTlsTransport::consumeReceivedFrame() {
  if (implementation_) implementation_->clearFrame();
}

bool Esp32GatewayLanTlsTransport::connected() const {
  return implementation_ && implementation_->client.connected();
}

void Esp32GatewayLanTlsTransport::close() {
  if (implementation_) {
    delete implementation_;
    implementation_ = nullptr;
  }
}

Esp32GatewayTlsMemoryEvidence
Esp32GatewayLanTlsTransport::memoryEvidence() const {
  return memory_;
}

}  // namespace connectivity
}  // namespace kitsu868

#endif  // ARDUINO_ARCH_ESP32
