#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>
#include <vector>

#include "../src/kitsu_connectivity_config.h"
#include "../src/kitsu_connectivity_runtime.h"

using namespace kitsu868::connectivity;

namespace {

constexpr uint8_t kTestCa[] = {0x30U, 0x01U, 0x00U};

class FakeTrust final : public GatewayTrustValidator {
 public:
  bool validCertificateAuthority(const uint8_t* der,
                                 size_t derBytes) override {
    return der && derBytes == sizeof(kTestCa) &&
           memcmp(der, kTestCa, sizeof(kTestCa)) == 0;
  }

  bool validateEnrollmentChain(
      const uint8_t* caDer, size_t caDerBytes, const uint8_t* leafDer,
      size_t leafDerBytes, const uint8_t* const* chainDer,
      const size_t* chainBytes, size_t chainCount) override {
    if (!validCertificateAuthority(caDer, caDerBytes) || !leafDer ||
        leafDerBytes == 0U || chainCount > kEnrollmentMaximumChainCertificates) {
      return false;
    }
    for (size_t i = 0U; i < chainCount; ++i) {
      if (!chainDer || !chainBytes || !chainDer[i] || chainBytes[i] == 0U) {
        return false;
      }
    }
    return true;
  }
};

uint32_t hashBytes(uint32_t hash, const uint8_t* input, size_t bytes) {
  for (size_t i = 0U; i < bytes; ++i) {
    hash ^= input[i];
    hash *= 16777619UL;
  }
  return hash;
}

class FakeCrypto final : public ConnectionStoreCrypto {
 public:
  bool ready() const override { return ready_; }
  bool randomBytes(uint8_t* output, size_t outputBytes) override {
    if (!ready_ || (!output && outputBytes != 0U)) return false;
    for (size_t i = 0U; i < outputBytes; ++i) {
      output[i] = static_cast<uint8_t>(0x40U + i + nonceCounter_);
    }
    ++nonceCounter_;
    return true;
  }
  bool seal(const uint8_t nonce[12], const uint8_t* aad, size_t aadBytes,
            const uint8_t* plaintext, size_t plaintextBytes,
            uint8_t* ciphertext, uint8_t tag[16]) override {
    if (!ready_ || !nonce || !aad || !plaintext || !ciphertext || !tag) {
      return false;
    }
    for (size_t i = 0U; i < plaintextBytes; ++i) {
      ciphertext[i] = plaintext[i] ^ 0xa5U ^ nonce[i % 12U];
    }
    makeTag(nonce, aad, aadBytes, ciphertext, plaintextBytes, tag);
    return true;
  }
  bool open(const uint8_t nonce[12], const uint8_t* aad, size_t aadBytes,
            const uint8_t* ciphertext, size_t ciphertextBytes,
            const uint8_t tag[16], uint8_t* plaintext) override {
    uint8_t expected[16]{};
    makeTag(nonce, aad, aadBytes, ciphertext, ciphertextBytes, expected);
    if (memcmp(expected, tag, sizeof(expected)) != 0) return false;
    for (size_t i = 0U; i < ciphertextBytes; ++i) {
      plaintext[i] = ciphertext[i] ^ 0xa5U ^ nonce[i % 12U];
    }
    return true;
  }

  bool ready_ = true;

 private:
  void makeTag(const uint8_t nonce[12], const uint8_t* aad,
               size_t aadBytes, const uint8_t* ciphertext,
               size_t ciphertextBytes, uint8_t tag[16]) {
    uint32_t hash = hashBytes(2166136261UL, nonce, 12U);
    hash = hashBytes(hash, aad, aadBytes);
    hash = hashBytes(hash, ciphertext, ciphertextBytes);
    for (uint8_t i = 0U; i < 16U; ++i) {
      hash = hash * 1664525UL + 1013904223UL;
      tag[i] = static_cast<uint8_t>(hash >> 24U);
    }
  }

  uint8_t nonceCounter_ = 1U;
};

class FakeStorage final : public ConnectionSlotStorage {
 public:
  FakeStorage() {
    for (auto& slot : slots_) slot.assign(kConnectionSnapshotBytes, 0xffU);
  }

  bool available() const override { return available_; }
  bool readSlot(uint8_t slot, uint8_t* output, size_t capacity,
                size_t& outputBytes) override {
    outputBytes = 0U;
    if (!available_ || failRead_ || slot >= slots_.size() || !output ||
        capacity < kConnectionSnapshotBytes) {
      return false;
    }
    bool erased = true;
    for (uint8_t byte : slots_[slot]) erased = erased && byte == 0xffU;
    if (erased) return true;
    memcpy(output, slots_[slot].data(), kConnectionSnapshotBytes);
    outputBytes = kConnectionSnapshotBytes;
    return true;
  }
  bool writeSlot(uint8_t slot, const uint8_t* input,
                 size_t inputBytes) override {
    if (!available_ || failWrite_ || slot >= slots_.size() || !input ||
        inputBytes != kConnectionSnapshotBytes) {
      return false;
    }
    memcpy(slots_[slot].data(), input, inputBytes);
    if (corruptAfterWrite_) slots_[slot][100U] ^= 0x01U;
    return true;
  }

  bool contains(const char* needle) const {
    const size_t bytes = strlen(needle);
    for (const auto& slot : slots_) {
      for (size_t i = 0U; i + bytes <= slot.size(); ++i) {
        if (memcmp(slot.data() + i, needle, bytes) == 0) return true;
      }
    }
    return false;
  }

  void corruptEveryNonemptySlot() {
    for (auto& slot : slots_) {
      if (slot[0] != 0xffU) slot[100U] ^= 0x55U;
    }
  }

  bool available_ = true;
  bool failRead_ = false;
  bool failWrite_ = false;
  bool corruptAfterWrite_ = false;
  std::array<std::vector<uint8_t>, kConnectionSlotCount> slots_{};
};

void putU16Le(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
}

void putU32Le(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
  output[2] = static_cast<uint8_t>(value >> 16U);
  output[3] = static_cast<uint8_t>(value >> 24U);
}

uint32_t testCrc32(const uint8_t* input, size_t bytes) {
  uint32_t crc = 0xffffffffUL;
  for (size_t i = 0U; i < bytes; ++i) {
    crc ^= input[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask =
          static_cast<uint32_t>(-static_cast<int32_t>(crc & 1U));
      crc = (crc >> 1U) ^ (0xedb88320UL & mask);
    }
  }
  return ~crc;
}

void convertSnapshotToLegacyV1(FakeStorage& storage, FakeCrypto& crypto,
                               uint8_t slot, uint16_t steadyPort) {
  constexpr size_t kOuterHeaderBytes = 48U;
  constexpr size_t kGatewayPortOffset = 134U;
  constexpr size_t kLegacyReservedTailOffset = 29464U;
  constexpr size_t kPlainCrcOffset = kConnectionPlainBytes - 4U;
  assert(slot < storage.slots_.size());
  std::vector<uint8_t>& outer = storage.slots_[slot];
  std::vector<uint8_t> plain(kConnectionPlainBytes, 0U);
  assert(crypto.open(outer.data() + 16U, outer.data(), 28U,
                     outer.data() + kOuterHeaderBytes,
                     kConnectionPlainBytes, outer.data() + 28U,
                     plain.data()));
  putU16Le(plain.data() + 4U, 1U);
  putU16Le(plain.data() + kGatewayPortOffset, steadyPort);
  memset(plain.data() + kLegacyReservedTailOffset, 0,
         kPlainCrcOffset - kLegacyReservedTailOffset);
  putU32Le(plain.data() + kPlainCrcOffset,
           testCrc32(plain.data(), kPlainCrcOffset));
  putU16Le(outer.data() + 4U, 1U);
  assert(crypto.seal(outer.data() + 16U, outer.data(), 28U,
                     plain.data(), plain.size(),
                     outer.data() + kOuterHeaderBytes,
                     outer.data() + 28U));
  putU32Le(outer.data() + 44U, testCrc32(outer.data(), 44U));
  memset(plain.data(), 0, plain.size());
}

WifiConfig parsedWifi() {
  static const char json[] =
      "{\"ssid_b64\":\"SG9tZQ\",\"security\":\"wpa2_wpa3\","
      "\"passphrase\":\"correct horse battery\"}";
  WifiConfig config{};
  assert(decodeWifiConfig(reinterpret_cast<const uint8_t*>(json),
                          strlen(json), config) == ConfigResult::Ok);
  return config;
}

GatewayConfig parsedGateway(FakeTrust& trust) {
  static const char json[] =
      "{\"gateway_id\":\"12345678-1234-4abc-8def-1234567890ab\","
      "\"host\":\"192.0.2.10\",\"bootstrap_port\":7442,"
      "\"port\":7443,\"server_name\":\"gateway.example\","
      "\"ca_cert_der_b64\":\"MAEA\","
      "\"spki_sha256_b64\":"
      "\"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE\"}";
  GatewayConfig config{};
  assert(decodeGatewayConfig(reinterpret_cast<const uint8_t*>(json),
                             strlen(json), trust, config) ==
         ConfigResult::Ok);
  return config;
}

void parserTests() {
  WifiConfig wifi = parsedWifi();
  assert(wifi.ssidBytes == 4U && memcmp(wifi.ssid, "Home", 4U) == 0);
  assert(wifi.security == WifiSecurity::Wpa2Wpa3);
  assert(strcmp(wifi.passphrase, "correct horse battery") == 0);

  const char padded[] =
      "{\"ssid_b64\":\"SG9tZQ==\",\"security\":\"wpa2\","
      "\"passphrase\":\"12345678\"}";
  assert(decodeWifiConfig(reinterpret_cast<const uint8_t*>(padded),
                          strlen(padded), wifi) == ConfigResult::InvalidSsid);
  const char nulSsid[] =
      "{\"ssid_b64\":\"AA\",\"security\":\"wpa2\","
      "\"passphrase\":\"12345678\"}";
  assert(decodeWifiConfig(reinterpret_cast<const uint8_t*>(nulSsid),
                          strlen(nulSsid), wifi) ==
         ConfigResult::InvalidSsid);
  const char duplicate[] =
      "{\"ssid_b64\":\"QQ\",\"ssid_b64\":\"Qg\","
      "\"security\":\"wpa2\",\"passphrase\":\"12345678\"}";
  assert(decodeWifiConfig(reinterpret_cast<const uint8_t*>(duplicate),
                          strlen(duplicate), wifi) ==
         ConfigResult::InvalidWifiPayload);

  FakeTrust trust;
  GatewayConfig gateway = parsedGateway(trust);
  assert(strcmp(gateway.host, "192.0.2.10") == 0);
  assert(strcmp(gateway.serverName, "gateway.example") == 0);
  assert(gateway.bootstrapPort == 7442U && gateway.port == 7443U &&
         gateway.caCertificateBytes == 3U);

  const char missingBootstrap[] =
      "{\"gateway_id\":\"12345678-1234-4abc-8def-1234567890ab\","
      "\"host\":\"192.0.2.10\",\"port\":7443,"
      "\"server_name\":\"gateway.example\","
      "\"ca_cert_der_b64\":\"MAEA\","
      "\"spki_sha256_b64\":"
      "\"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE\"}";
  assert(decodeGatewayConfig(
             reinterpret_cast<const uint8_t*>(missingBootstrap),
             strlen(missingBootstrap), trust, gateway) ==
         ConfigResult::InvalidGatewayPayload);
  const char confusedPorts[] =
      "{\"gateway_id\":\"12345678-1234-4abc-8def-1234567890ab\","
      "\"host\":\"192.0.2.10\",\"bootstrap_port\":7443,"
      "\"port\":7443,\"server_name\":\"gateway.example\","
      "\"ca_cert_der_b64\":\"MAEA\","
      "\"spki_sha256_b64\":"
      "\"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE\"}";
  assert(decodeGatewayConfig(reinterpret_cast<const uint8_t*>(confusedPorts),
                             strlen(confusedPorts), trust, gateway) ==
         ConfigResult::InvalidPort);

  const char scheme[] =
      "{\"gateway_id\":\"12345678-1234-4abc-8def-1234567890ab\","
      "\"host\":\"https://gateway.example\",\"bootstrap_port\":7442,"
      "\"port\":7443,"
      "\"server_name\":\"gateway.k32.run\","
      "\"ca_cert_der_b64\":\"MAEA\","
      "\"spki_sha256_b64\":"
      "\"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE\"}";
  assert(decodeGatewayConfig(reinterpret_cast<const uint8_t*>(scheme),
                             strlen(scheme), trust, gateway) ==
         ConfigResult::InvalidHost);
  const char uppercaseUuid[] =
      "{\"gateway_id\":\"12345678-1234-4ABC-8def-1234567890ab\","
      "\"host\":\"192.0.2.10\",\"bootstrap_port\":7442,"
      "\"port\":7443,"
      "\"server_name\":\"gateway.k32.run\","
      "\"ca_cert_der_b64\":\"MAEA\","
      "\"spki_sha256_b64\":"
      "\"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE\"}";
  assert(decodeGatewayConfig(
             reinterpret_cast<const uint8_t*>(uppercaseUuid),
             strlen(uppercaseUuid), trust, gateway) ==
         ConfigResult::InvalidGatewayId);

  const char numericSni[] =
      "{\"gateway_id\":\"12345678-1234-4abc-8def-1234567890ab\","
      "\"host\":\"2001:db8::1\",\"bootstrap_port\":7442,"
      "\"port\":7443,"
      "\"server_name\":\"192.0.2.10\","
      "\"ca_cert_der_b64\":\"MAEA\","
      "\"spki_sha256_b64\":"
      "\"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE\"}";
  assert(decodeGatewayConfig(reinterpret_cast<const uint8_t*>(numericSni),
                             strlen(numericSni), trust, gateway) ==
         ConfigResult::InvalidServerName);

  const char malformedIpv6[] =
      "{\"gateway_id\":\"12345678-1234-4abc-8def-1234567890ab\","
      "\"host\":\"2001:::1\",\"bootstrap_port\":7442,"
      "\"port\":7443,"
      "\"server_name\":\"gateway.k32.run\","
      "\"ca_cert_der_b64\":\"MAEA\","
      "\"spki_sha256_b64\":"
      "\"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE\"}";
  assert(decodeGatewayConfig(
             reinterpret_cast<const uint8_t*>(malformedIpv6),
             strlen(malformedIpv6), trust, gateway) ==
         ConfigResult::InvalidHost);
}

void storeTests() {
  static_assert(sizeof(ConnectionConfigStore) < 15U * 1024U,
                "resident store exceeded budget");
  // A Wi-Fi-only configuration is a complete, durable configuration in its
  // own right. It must survive a reboot before any gateway is selected.
  FakeStorage wifiOnlyStorage;
  FakeCrypto wifiOnlyCrypto;
  FakeTrust wifiOnlyTrust;
  ConnectionConfigStore wifiOnly;
  assert(wifiOnly.begin(wifiOnlyStorage, wifiOnlyCrypto, wifiOnlyTrust) ==
         ConfigResult::Ok);
  const WifiConfig wifiOnlyConfig = parsedWifi();
  assert(wifiOnly.commitWifi(wifiOnlyConfig) == ConfigResult::Ok);
  assert(wifiOnly.status().wifiConfigured &&
         !wifiOnly.status().gatewayConfigured);
  assert(!wifiOnlyStorage.contains("correct horse battery"));
  ConnectionConfigStore wifiOnlyRebooted;
  assert(wifiOnlyRebooted.begin(wifiOnlyStorage, wifiOnlyCrypto,
                                wifiOnlyTrust) == ConfigResult::Ok);
  WifiConfig wifiAfterReboot{};
  assert(wifiOnlyRebooted.status().wifiConfigured &&
         !wifiOnlyRebooted.status().gatewayConfigured &&
         wifiOnlyRebooted.copyWifi(wifiAfterReboot));
  assert(wifiAfterReboot.ssidBytes == wifiOnlyConfig.ssidBytes &&
         memcmp(wifiAfterReboot.ssid, wifiOnlyConfig.ssid,
                wifiOnlyConfig.ssidBytes) == 0 &&
         strcmp(wifiAfterReboot.passphrase, "correct horse battery") == 0);

  FakeStorage storage;
  FakeCrypto crypto;
  FakeTrust trust;
  ConnectionConfigStore store;
  assert(store.begin(storage, crypto, trust) == ConfigResult::Ok);
  assert(store.status().begun && store.status().generation == 0U);

  const WifiConfig wifi = parsedWifi();
  const GatewayConfig gateway = parsedGateway(trust);
  assert(store.commitWifi(wifi) == ConfigResult::Ok);
  assert(store.status().generation == 1U && store.status().wifiConfigured);
  assert(store.commitGateway(gateway) == ConfigResult::Ok);
  ConnectionConfigStatus status = store.status();
  assert(status.generation == 2U && status.gatewayConfigured &&
         !status.gatewayEnrolled && status.activeSlot == 1);
  assert(!storage.contains("correct horse battery"));
  assert(!storage.contains("192.0.2.10"));

  ConnectionConfigStore rebooted;
  assert(rebooted.begin(storage, crypto, trust) == ConfigResult::Ok);
  WifiConfig loadedWifi{};
  GatewayConfig loadedGateway{};
  assert(rebooted.copyWifi(loadedWifi) && rebooted.copyGateway(loadedGateway));
  assert(strcmp(loadedWifi.passphrase, "correct horse battery") == 0);
  assert(strcmp(loadedGateway.host, "192.0.2.10") == 0);
  assert(loadedGateway.bootstrapPort == 7442U &&
         loadedGateway.port == 7443U);

  // A failed target write does not erase or supersede generation 2.
  storage.failWrite_ = true;
  assert(rebooted.commitWifi(wifi) == ConfigResult::StorageWriteFailed);
  storage.failWrite_ = false;
  ConnectionConfigStore afterPowerLoss;
  assert(afterPowerLoss.begin(storage, crypto, trust) == ConfigResult::Ok);
  assert(afterPowerLoss.status().generation == 2U);

  // A write that fails authenticated readback also leaves generation 2 as
  // the newest valid generation on reboot.
  storage.corruptAfterWrite_ = true;
  assert(afterPowerLoss.commitWifi(wifi) ==
         ConfigResult::StorageReadbackFailed);
  storage.corruptAfterWrite_ = false;
  ConnectionConfigStore afterBadReadback;
  assert(afterBadReadback.begin(storage, crypto, trust) == ConfigResult::Ok);
  assert(afterBadReadback.status().generation == 2U);

  // Exercise the full EnrollmentCredentialSink upper bounds.  Four 4096-byte
  // chain certificates plus the 4096-byte leaf fit the same 30,048-byte slot.
  std::vector<uint8_t> leaf(kEnrollmentMaximumCertificateBytes, 0x31U);
  std::array<std::vector<uint8_t>, kEnrollmentMaximumChainCertificates> chain;
  const uint8_t* chainPointers[kEnrollmentMaximumChainCertificates]{};
  size_t chainLengths[kEnrollmentMaximumChainCertificates]{};
  for (size_t i = 0U; i < chain.size(); ++i) {
    chain[i].assign(kEnrollmentMaximumCertificateBytes,
                    static_cast<uint8_t>(0x40U + i));
    chainPointers[i] = chain[i].data();
    chainLengths[i] = chain[i].size();
  }
  uint8_t companionId[16]{};
  uint8_t privateKey[32]{};
  uint8_t backendSecret[32]{};
  companionId[0] = privateKey[0] = backendSecret[0] = 1U;
  assert(afterBadReadback.commitEnrollmentCredential(
      companionId, gateway.gatewayId, 1U, privateKey, leaf.data(), leaf.size(),
      chainPointers, chainLengths, chain.size(), backendSecret));
  assert(afterBadReadback.status().gatewayEnrolled);
  assert(!afterBadReadback.remoteConnectivityAllowed());
  afterBadReadback.setRemoteConnectivityAllowed(true);
  GatewayLanCredentialView credentials{};
  assert(afterBadReadback.acquire(credentials));
  assert(credentials.privateKeyBytes == kEnrollmentPrivateKeyBytes);
  assert(credentials.leafCertificateBytes ==
         kEnrollmentMaximumCertificateBytes);
  assert(credentials.certificateChainCount ==
         kEnrollmentMaximumChainCertificates);
  assert(credentials.backendHmacSecretBytes == kEnrollmentSecretBytes);
  assert(credentials.privateKey[0] == 1U &&
         credentials.backendHmacSecret[0] == 1U);
  // A lease is exclusive so two TLS consumers cannot alias the transient
  // plaintext. release() wipes/frees both transient buffers.
  GatewayLanCredentialView second{};
  assert(!afterBadReadback.acquire(second));
  afterBadReadback.release(credentials);
  assert(credentials.privateKey == nullptr &&
         credentials.backendHmacSecret == nullptr);

  // Repeating the exact owner-authenticated relay UUID + CA is read-only: it
  // must preserve both the durable generation and completed enrollment.
  FakeStorage relayStorage;
  FakeCrypto relayCrypto;
  ConnectionConfigStore relayStore;
  assert(relayStore.begin(relayStorage, relayCrypto, trust) ==
         ConfigResult::Ok);
  uint8_t relayGatewayId[16]{};
  relayGatewayId[0] = 0x55U;
  assert(relayStore.commitMobileRelayGateway(
             relayGatewayId, kTestCa, sizeof(kTestCa)) ==
         MobileRelayGatewayConfigResult::Changed);
  uint8_t relayCompanionId[16]{};
  uint8_t relayPrivateKey[32]{};
  uint8_t relaySecret[32]{};
  const uint8_t relayLeaf[] = {0x31U};
  const uint8_t relayIssuer[] = {0x41U};
  const uint8_t* relayChain[] = {relayIssuer};
  const size_t relayChainBytes[] = {sizeof(relayIssuer)};
  relayCompanionId[0] = relayPrivateKey[0] = relaySecret[0] = 1U;
  assert(relayStore.commitEnrollmentCredential(
      relayCompanionId, relayGatewayId, 1U, relayPrivateKey, relayLeaf,
      sizeof(relayLeaf), relayChain, relayChainBytes, 1U, relaySecret));
  const ConnectionConfigStatus beforeRelayReplay = relayStore.status();
  assert(beforeRelayReplay.mobileRelayConfigured &&
         !beforeRelayReplay.gatewayLanConfigured &&
         beforeRelayReplay.gatewayEnrolled);
  assert(relayStore.commitMobileRelayGateway(
             relayGatewayId, kTestCa, sizeof(kTestCa)) ==
         MobileRelayGatewayConfigResult::Unchanged);
  const ConnectionConfigStatus afterRelayReplay = relayStore.status();
  assert(afterRelayReplay.generation == beforeRelayReplay.generation &&
         afterRelayReplay.gatewayEnrolled);

  storage.corruptEveryNonemptySlot();
  ConnectionConfigStore corrupt;
  assert(corrupt.begin(storage, crypto, trust) == ConfigResult::StorageCorrupt);
  assert(!corrupt.ready());
}

void legacyMigrationTests() {
  // Build a fully enrolled snapshot with the frozen v1 credential offsets,
  // convert its authenticated version marker, and leave it as the sole valid
  // generation. begin() must rotate it to v2 without losing any secret bytes.
  FakeStorage storage;
  FakeCrypto crypto;
  FakeTrust trust;
  ConnectionConfigStore original;
  assert(original.begin(storage, crypto, trust) == ConfigResult::Ok);
  const WifiConfig wifi = parsedWifi();
  GatewayConfig gateway = parsedGateway(trust);
  assert(original.commitWifi(wifi) == ConfigResult::Ok);
  assert(original.commitGateway(gateway) == ConfigResult::Ok);
  uint8_t companionId[16]{};
  uint8_t privateKey[32]{};
  uint8_t backendSecret[32]{};
  uint8_t leaf[] = {0x31U};
  uint8_t issuer[] = {0x41U};
  const uint8_t* chain[] = {issuer};
  const size_t chainBytes[] = {sizeof(issuer)};
  companionId[0] = 1U;
  privateKey[0] = 2U;
  backendSecret[0] = 3U;
  assert(original.commitEnrollmentCredential(
      companionId, gateway.gatewayId, 7U, privateKey, leaf, sizeof(leaf),
      chain, chainBytes, 1U, backendSecret));
  const ConnectionConfigStatus before = original.status();
  assert(before.gatewayEnrolled && before.activeSlot >= 0);
  const uint8_t legacySlot = static_cast<uint8_t>(before.activeSlot);
  convertSnapshotToLegacyV1(storage, crypto, legacySlot, 7443U);
  for (uint8_t slot = 0U; slot < kConnectionSlotCount; ++slot) {
    if (slot != legacySlot) {
      storage.slots_[slot].assign(kConnectionSnapshotBytes, 0xffU);
    }
  }

  ConnectionConfigStore migrated;
  assert(migrated.begin(storage, crypto, trust) == ConfigResult::Ok);
  const ConnectionConfigStatus after = migrated.status();
  assert(after.generation == before.generation + 1U &&
         after.gatewayEnrolled && after.activeSlot >= 0 &&
         after.activeSlot != before.activeSlot);
  GatewayConfig loadedGateway{};
  WifiConfig loadedWifi{};
  assert(migrated.copyGateway(loadedGateway) &&
         migrated.copyWifi(loadedWifi));
  assert(loadedGateway.bootstrapPort == 7442U &&
         loadedGateway.port == 7443U &&
         strcmp(loadedGateway.host, "192.0.2.10") == 0);
  assert(strcmp(loadedWifi.passphrase, "correct horse battery") == 0);
  migrated.setRemoteConnectivityAllowed(true);
  GatewayLanCredentialView lease{};
  assert(migrated.acquire(lease));
  assert(lease.keyVersion == 7U && lease.privateKey[0] == 2U &&
         lease.backendHmacSecret[0] == 3U);
  migrated.release(lease);

  // A v1 single-port record from an unknown deployment is ambiguous. It is
  // never guessed into a bootstrap listener and requires BLE reprovisioning.
  FakeStorage ambiguousStorage;
  FakeCrypto ambiguousCrypto;
  ConnectionConfigStore ambiguousOriginal;
  assert(ambiguousOriginal.begin(ambiguousStorage, ambiguousCrypto, trust) ==
         ConfigResult::Ok);
  gateway.port = 7555U;
  assert(ambiguousOriginal.commitGateway(gateway) == ConfigResult::Ok);
  const uint8_t ambiguousSlot =
      static_cast<uint8_t>(ambiguousOriginal.status().activeSlot);
  convertSnapshotToLegacyV1(ambiguousStorage, ambiguousCrypto,
                            ambiguousSlot, 7555U);
  ConnectionConfigStore rejected;
  assert(rejected.begin(ambiguousStorage, ambiguousCrypto, trust) ==
         ConfigResult::StorageCorrupt);
  assert(!rejected.ready());
}

ConnectivityPrerequisites readyPrerequisites(bool remoteAllowed = true) {
  ConnectivityPrerequisites prerequisites{};
  prerequisites.configStoreReady = true;
  prerequisites.remoteConnectivityAllowed = remoteAllowed;
  prerequisites.wifiConfigured = true;
  prerequisites.gatewayConfigured = true;
  return prerequisites;
}

void policyTests() {
  ConnectivityPolicy unavailablePolicy;
  ConnectivityPrerequisites unavailableRemote = readyPrerequisites(false);
  assert(unavailablePolicy.tick(0U, unavailableRemote,
                                WifiLinkObservation::Down) ==
         WifiPolicyAction::None);
  assert(unavailablePolicy.status(0U).wifiState ==
         WifiRuntimeState::ConnectivityUnavailable);
  assert(unavailablePolicy.tick(60000U, unavailableRemote,
                                WifiLinkObservation::Down) !=
         WifiPolicyAction::Start);

  ConnectivityPolicy missing;
  ConnectivityPrerequisites unavailable{};
  unavailable.remoteConnectivityAllowed = true;
  assert(missing.tick(0U, unavailable, WifiLinkObservation::Down) ==
         WifiPolicyAction::None);
  assert(missing.status(0U).wifiState ==
         WifiRuntimeState::StorageUnavailable);

  // Wi-Fi association is independent from gateway provisioning and starts on
  // the first service tick. This makes a Wi-Fi-only save observable.
  ConnectivityPolicy wifiOnly;
  ConnectivityPrerequisites wifiOnlyPrerequisites{};
  wifiOnlyPrerequisites.configStoreReady = true;
  wifiOnlyPrerequisites.remoteConnectivityAllowed = true;
  wifiOnlyPrerequisites.wifiConfigured = true;
  wifiOnlyPrerequisites.authenticatedBleSession = true;
  assert(wifiOnly.tick(0U, wifiOnlyPrerequisites,
                       WifiLinkObservation::Down) ==
         WifiPolicyAction::Start);
  assert(wifiOnly.status(0U).wifiState == WifiRuntimeState::Connecting);
  assert(wifiOnly.tick(1U, wifiOnlyPrerequisites,
                       WifiLinkObservation::ConnectedAccepted) ==
         WifiPolicyAction::None);
  assert(wifiOnly.status(1U).wifiState == WifiRuntimeState::Connected);
  assert(wifiOnly.status(1U).lanState == LanRuntimeState::Unconfigured);

  // Wi-Fi/BLE coexistence keeps the association warm, while LAN state makes
  // BLE ownership explicit. Main separately prevents bootstrap/steady LAN
  // work until the authenticated BLE session ends.
  wifiOnlyPrerequisites.gatewayConfigured = true;
  assert(wifiOnly.tick(2U, wifiOnlyPrerequisites,
                       WifiLinkObservation::ConnectedAccepted) ==
         WifiPolicyAction::None);
  assert(wifiOnly.status(2U).wifiState == WifiRuntimeState::Connected);
  assert(wifiOnly.status(2U).lanState == LanRuntimeState::BleActive);
  wifiOnlyPrerequisites.authenticatedBleSession = false;
  assert(wifiOnly.tick(3U, wifiOnlyPrerequisites,
                       WifiLinkObservation::ConnectedAccepted) ==
         WifiPolicyAction::None);
  assert(wifiOnly.status(3U).lanState ==
         LanRuntimeState::EnrollmentPending);
  wifiOnlyPrerequisites.gatewayEnrolled = true;
  wifiOnlyPrerequisites.timeValid = true;
  assert(wifiOnly.tick(4U, wifiOnlyPrerequisites,
                       WifiLinkObservation::ConnectedAccepted) ==
         WifiPolicyAction::None);
  assert(wifiOnly.status(4U).lanState == LanRuntimeState::TlsPending);

  ConnectivityPolicy reflashable;
  ConnectivityPrerequisites prerequisites = readyPrerequisites();
  assert(reflashable.tick(0U, prerequisites, WifiLinkObservation::Down) ==
         WifiPolicyAction::Start);
  assert(reflashable.tick(1U, prerequisites,
                          WifiLinkObservation::Connecting) ==
         WifiPolicyAction::None);

  // Authenticated BLE no longer tears down the STA link.
  prerequisites.authenticatedBleSession = true;
  assert(reflashable.tick(2U, prerequisites,
                          WifiLinkObservation::Connecting) ==
         WifiPolicyAction::None);
  assert(reflashable.status(2U).wifiState ==
         WifiRuntimeState::Connecting);
  assert(reflashable.status(2U).lanState == LanRuntimeState::BleActive);
  prerequisites.authenticatedBleSession = false;
  assert(reflashable.tick(3U, prerequisites,
                          WifiLinkObservation::ConnectedDowngraded) ==
         WifiPolicyAction::Stop);
  ConnectivityRuntimeStatus backoff =
      reflashable.status(3U);
  assert(backoff.wifiState == WifiRuntimeState::Backoff);
  assert(backoff.retryRemainingMs == kWifiReconnectInitialMs);
  assert(reflashable.tick(3U + kWifiReconnectInitialMs - 1U,
                          prerequisites, WifiLinkObservation::Down) ==
         WifiPolicyAction::None);
  assert(reflashable.tick(3U + kWifiReconnectInitialMs, prerequisites,
                          WifiLinkObservation::Down) ==
         WifiPolicyAction::Start);

  // A verified credential replacement clears stale backoff and can retry
  // immediately. Esp32WifiRuntime invokes this reset after dropping the old
  // driver association.
  reflashable.reset();
  assert(reflashable.tick(9000U, prerequisites,
                          WifiLinkObservation::Down) ==
         WifiPolicyAction::Start);
}

}  // namespace

int main() {
  parserTests();
  storeTests();
  legacyMigrationTests();
  policyTests();
  printf("Kitsu connectivity tests passed; resident_store_bytes=%zu "
         "snapshot_bytes=%zu enrollment_used_bytes=29464 slots=%zu\n",
         sizeof(ConnectionConfigStore), kConnectionSnapshotBytes,
         kConnectionSlotCount);
  return 0;
}
