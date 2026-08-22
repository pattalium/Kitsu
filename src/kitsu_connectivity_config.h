#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kitsu_enrollment.h"
#include "kitsu_gateway_lan_runtime.h"
#include "kitsu_mobile_relay.h"

namespace kitsu868 {
namespace connectivity {

constexpr size_t kWifiSsidMaximumBytes = 32U;
constexpr size_t kWifiPassphraseMaximumBytes = 63U;
constexpr size_t kGatewayHostMaximumBytes = 253U;
constexpr size_t kGatewayServerNameMaximumBytes = 253U;
constexpr size_t kGatewayCaMaximumBytes = 8192U;
constexpr size_t kGatewaySpkiSha256Bytes = 32U;
constexpr size_t kConnectionPartitionBytes = 0x40000U;
constexpr size_t kConnectionSlotBytes = 0x8000U;
constexpr size_t kConnectionSlotCount =
    kConnectionPartitionBytes / kConnectionSlotBytes;
constexpr size_t kConnectionPlainBytes = 30000U;
constexpr size_t kConnectionOuterHeaderBytes = 48U;
constexpr size_t kConnectionSnapshotBytes =
    kConnectionOuterHeaderBytes + kConnectionPlainBytes;
constexpr size_t kEnrollmentMaximumChainBytes =
    kEnrollmentMaximumChainCertificates * kEnrollmentMaximumCertificateBytes;

static_assert(kConnectionSlotCount >= 2U,
              "connectivity store requires power-loss fallback slots");
static_assert(kConnectionSnapshotBytes <= kConnectionSlotBytes,
              "connectivity snapshot must fit one raw-flash slot");

enum class WifiSecurity : uint8_t {
  Wpa2 = 0,
  Wpa2Wpa3,
  Wpa3,
};

const char* wifiSecurityName(WifiSecurity security);

struct WifiConfig {
  uint8_t ssid[kWifiSsidMaximumBytes + 1U]{};
  uint8_t ssidBytes = 0U;
  char passphrase[kWifiPassphraseMaximumBytes + 1U]{};
  uint8_t passphraseBytes = 0U;
  WifiSecurity security = WifiSecurity::Wpa2;
};

struct GatewayConfig {
  uint8_t gatewayId[kEnrollmentUuidBytes]{};
  // A mobile relay stores only the logical gateway identity and enrollment
  // issuer CA. LAN endpoint fields remain empty and Wi-Fi stays optional.
  bool mobileRelayOnly = false;
  char host[kGatewayHostMaximumBytes + 1U]{};
  uint16_t hostBytes = 0U;
  // Initial enrollment is server-authenticated TLS on a listener that does
  // not require a client certificate. Steady traffic uses the distinct mTLS
  // listener in port. Keeping the two explicit prevents the former v1
  // certificate/bootstrap deadlock from returning.
  uint16_t bootstrapPort = 0U;
  uint16_t port = 0U;
  char serverName[kGatewayServerNameMaximumBytes + 1U]{};
  uint16_t serverNameBytes = 0U;
  uint8_t caCertificateDer[kGatewayCaMaximumBytes]{};
  uint16_t caCertificateBytes = 0U;
  uint8_t spkiSha256[kGatewaySpkiSha256Bytes]{};
};

enum class ConfigResult : uint8_t {
  Ok = 0,
  NotBegun,
  InvalidArgument,
  InvalidWifiPayload,
  InvalidSsid,
  InvalidSecurity,
  InvalidPassphrase,
  InvalidGatewayPayload,
  InvalidGatewayId,
  InvalidHost,
  InvalidBootstrapPort,
  InvalidPort,
  InvalidServerName,
  InvalidCaCertificate,
  InvalidSpkiSha256,
  SecurityUnavailable,
  StorageUnavailable,
  StorageReadFailed,
  StorageWriteFailed,
  StorageReadbackFailed,
  StorageAllocationFailed,
  StorageCorrupt,
  CryptoFailed,
  EnrollmentInvalid,
  EnrollmentTrustFailed,
};

const char* configResultName(ConfigResult result);

class GatewayTrustValidator {
 public:
  virtual ~GatewayTrustValidator() = default;
  virtual bool validCertificateAuthority(const uint8_t* der,
                                         size_t derBytes) = 0;
  virtual bool validateEnrollmentChain(
      const uint8_t* caDer, size_t caDerBytes, const uint8_t* leafDer,
      size_t leafDerBytes, const uint8_t* const* chainDer,
      const size_t* chainBytes, size_t chainCount) = 0;
};

// Raw partition adapter.  Implementations must treat an erased slot as a
// successful zero-byte read.  writeSlot atomically replaces only the selected
// 32 KiB slot (erase, write, then let the caller perform authenticated
// readback); it must never erase another slot.
class ConnectionSlotStorage {
 public:
  virtual ~ConnectionSlotStorage() = default;
  virtual bool available() const = 0;
  virtual bool readSlot(uint8_t slot, uint8_t* output, size_t capacity,
                        size_t& outputBytes) = 0;
  virtual bool writeSlot(uint8_t slot, const uint8_t* input,
                         size_t inputBytes) = 0;
};

class ConnectionStoreCrypto {
 public:
  virtual ~ConnectionStoreCrypto() = default;
  virtual bool ready() const = 0;
  virtual bool randomBytes(uint8_t* output, size_t outputBytes) = 0;
  virtual bool seal(const uint8_t nonce[12], const uint8_t* aad,
                    size_t aadBytes, const uint8_t* plaintext,
                    size_t plaintextBytes, uint8_t* ciphertext,
                    uint8_t tag[16]) = 0;
  virtual bool open(const uint8_t nonce[12], const uint8_t* aad,
                    size_t aadBytes, const uint8_t* ciphertext,
                    size_t ciphertextBytes, const uint8_t tag[16],
                    uint8_t* plaintext) = 0;
};

struct ConnectionConfigStatus {
  ConfigResult lastResult = ConfigResult::NotBegun;
  bool begun = false;
  bool wifiConfigured = false;
  bool gatewayConfigured = false;
  bool gatewayLanConfigured = false;
  bool mobileRelayConfigured = false;
  bool gatewayEnrolled = false;
  int8_t activeSlot = -1;
  uint32_t generation = 0U;
};

ConfigResult decodeWifiConfig(const uint8_t* json, size_t jsonBytes,
                              WifiConfig& output);
ConfigResult decodeGatewayConfig(const uint8_t* json, size_t jsonBytes,
                                 GatewayTrustValidator& trust,
                                 GatewayConfig& output);

// One authenticated generation contains Wi-Fi, gateway trust, and eventual
// enrollment credentials.  Each logical update rotates to a different raw
// slot and is decrypted/read back before becoming active.  No credential is
// ever sent to NVS or returned by this API.
class ConnectionConfigStore final : public EnrollmentCredentialSink,
                                    public GatewayLanCredentialProvider,
                                    public MobileRelayGatewayConfigSink {
 public:
  ConnectionConfigStore();
  ~ConnectionConfigStore() override;

  ConnectionConfigStore(const ConnectionConfigStore&) = delete;
  ConnectionConfigStore& operator=(const ConnectionConfigStore&) = delete;

  ConfigResult begin(ConnectionSlotStorage& storage,
                     ConnectionStoreCrypto& crypto,
                     GatewayTrustValidator& trust);
  ConfigResult commitWifi(const WifiConfig& config);
  ConfigResult commitGateway(const GatewayConfig& config);
  // Authenticated owner reset for a mobile-relay gateway. The expected UUID
  // prevents a stale controller view from clearing a newer gateway. An
  // already-unconfigured store resumes the fail-closed fallback-slot scrub.
  ConfigResult forgetMobileRelayGateway(
      const uint8_t expectedGatewayUuid[kEnrollmentUuidBytes]);
  MobileRelayGatewayConfigResult commitMobileRelayGateway(
      const uint8_t gatewayUuid[kEnrollmentUuidBytes],
      const uint8_t* caCertificateDer,
      size_t caCertificateBytes) override;
  void setRemoteConnectivityAllowed(bool allowed);

  bool copyWifi(WifiConfig& output) const;
  bool copyGateway(GatewayConfig& output) const;
  bool copyGatewayId(uint8_t output[kEnrollmentUuidBytes]) const;
  ConnectionConfigStatus status() const;
  bool ready() const;

  bool remoteConnectivityAllowed() const override;
  bool acquire(GatewayLanCredentialView& output) override;
  void release(GatewayLanCredentialView& view) override;

  bool commitEnrollmentCredential(
      const uint8_t companionUuid[kEnrollmentUuidBytes],
      const uint8_t gatewayUuid[kEnrollmentUuidBytes], uint32_t keyVersion,
      const uint8_t mtlsPrivateKey[kEnrollmentPrivateKeyBytes],
      const uint8_t* certificateDer, size_t certificateBytes,
      const uint8_t* const* certificateChainDer,
      const size_t* certificateChainBytes, size_t certificateChainCount,
      const uint8_t backendHmacSecret[kEnrollmentSecretBytes]) override;

 private:
  ConfigResult setResult(ConfigResult result);
  ConfigResult loadSlot(uint8_t slot, uint8_t* outer, uint8_t* plain,
                        uint32_t& generation, uint16_t& version,
                        bool& nonempty) const;
  ConfigResult loadActivePlain(uint8_t* outer, uint8_t* plain) const;
  bool decodeResident(const uint8_t* plain, uint32_t generation);
  ConfigResult persistPlain(uint8_t* outer, uint8_t* plain,
                            uint32_t generation);
  void clear();
  static bool generationAfter(uint32_t candidate, uint32_t reference);

  ConnectionSlotStorage* storage_ = nullptr;
  ConnectionStoreCrypto* crypto_ = nullptr;
  GatewayTrustValidator* trust_ = nullptr;
  GatewayConfig gateway_{};
  ConnectionConfigStatus status_{};
  uint8_t* credentialLeasePlain_ = nullptr;
  bool remoteConnectivityAllowed_ = false;
};

static_assert(sizeof(ConnectionConfigStore) < 15U * 1024U,
              "connectivity store permanent RAM budget exceeded");

}  // namespace connectivity
}  // namespace kitsu868
