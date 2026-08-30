#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kitsu_controller_permissions.h"
#include "kitsu_reflashable_profile.h"

namespace kitsu868 {
namespace connectivity {

constexpr size_t kKitsuSecretBytes = 32U;
constexpr size_t kKitsuDeviceIdBytes = 16U;
constexpr size_t kKitsuControllerIdBytes = 16U;
constexpr size_t kSecurityNonceBytes = 12U;
constexpr size_t kSecurityTagBytes = 16U;
constexpr size_t kKitsuControllerCapacity = 4U;
constexpr size_t kSecurityBlobCapacity = 384U;
constexpr size_t kSecuritySlots = 2U;

enum class SecurityResult : uint8_t {
  Ok = 0,
  OkReflashable,
  NotBegun,
  InvalidArgument,
  SecurityModeUnavailable,
  WrappingRootUnavailable,
  StorageReadFailed,
  StorageWriteFailed,
  ReadbackFailed,
  CorruptStorage,
  CryptoFailed,
  AuthorizationRequired,
  ControllerNotProvisioned,
  ControllerTableFull,
};

const char* securityResultName(SecurityResult result);

class DeviceSecurityStorage {
 public:
  virtual ~DeviceSecurityStorage() = default;
  virtual bool readSlot(uint8_t slot, uint8_t* output, size_t capacity,
                        size_t& outputBytes) = 0;
  virtual bool writeSlot(uint8_t slot, const uint8_t* input,
                         size_t inputBytes) = 0;
  virtual bool clearSlot(uint8_t slot) = 0;
};

// Platform cryptography is backed by the ESP32-S3 RNG and mbedTLS. In the
// explicit reflashable profile, deriveWrappingKey() is an application-level
// wrapping root: it protects records from accidental disclosure/corruption,
// but is intentionally recoverable by an owner with a complete flash image.
// No eFuse programming API is part of this interface. The firmware may read
// the public factory MAC as a non-secret device identifier; it never writes,
// protects, revokes, locks, or otherwise changes an eFuse bit.
class DeviceSecurityPlatform {
 public:
  virtual ~DeviceSecurityPlatform() = default;
  virtual SecurityMode securityMode() const = 0;
  virtual bool deriveWrappingKey(const uint8_t hardwareId[8],
                                 uint8_t output[kKitsuSecretBytes]) = 0;
  virtual bool randomBytes(uint8_t* output, size_t outputBytes) = 0;
  virtual bool seal(const uint8_t key[kKitsuSecretBytes],
                    uint32_t generation,
                    const uint8_t nonce[kSecurityNonceBytes],
                    const uint8_t* plaintext, size_t plaintextBytes,
                    uint8_t* ciphertext,
                    uint8_t tag[kSecurityTagBytes]) = 0;
  virtual bool open(const uint8_t key[kKitsuSecretBytes],
                    uint32_t generation,
                    const uint8_t nonce[kSecurityNonceBytes],
                    const uint8_t* ciphertext, size_t ciphertextBytes,
                    const uint8_t tag[kSecurityTagBytes],
                    uint8_t* plaintext) = 0;
  virtual bool hkdfSha256(const uint8_t* inputKey, size_t inputKeyBytes,
                          const uint8_t* salt, size_t saltBytes,
                          const uint8_t* info, size_t infoBytes,
                          uint8_t* output, size_t outputBytes) = 0;
};

struct DeviceSecurityStatus {
  SecurityResult lastResult = SecurityResult::NotBegun;
  SecurityMode securityMode = SecurityMode::Reflashable;
  bool begun = false;
  bool applicationEncrypted = true;
  bool hardwareRootProtected = false;
  uint8_t controllerCount = 0;
  int8_t activeSlot = -1;
  uint32_t generation = 0;
};

// Owns only the local device root and authenticated BLE controller roots.
// MeshCore identity keys are neither accepted nor exposed here, making
// accidental key reuse impossible at the API boundary.
class KitsuDeviceSecurity {
 public:
  KitsuDeviceSecurity();
  ~KitsuDeviceSecurity();

  KitsuDeviceSecurity(const KitsuDeviceSecurity&) = delete;
  KitsuDeviceSecurity& operator=(const KitsuDeviceSecurity&) = delete;

  SecurityResult begin(DeviceSecurityStorage& storage,
                       DeviceSecurityPlatform& platform,
                       const uint8_t hardwareId[8]);

  bool ready() const;
  DeviceSecurityStatus status() const;

  bool copyDeviceId(uint8_t output[kKitsuDeviceIdBytes]) const;
  bool controllerAt(size_t ordinal,
                    uint8_t controllerId[kKitsuControllerIdBytes]) const;
  // Physical recovery UI uses stable storage slots, never controller roots.
  // False means that the bounded slot is empty (or the slot/output is invalid).
  bool controllerAtSlot(
      size_t slot, uint8_t controllerId[kKitsuControllerIdBytes]) const;
  bool findControllerRoot(
      const uint8_t controllerId[kKitsuControllerIdBytes],
      uint8_t outputRoot[kKitsuSecretBytes]) const;
  // Returns the role authenticated by the same durable record as the root.
  // The role output is invalidated on failure so callers cannot accidentally
  // retain Owner authority from an earlier lookup.
  bool findControllerRoot(
      const uint8_t controllerId[kKitsuControllerIdBytes],
      uint8_t outputRoot[kKitsuSecretBytes], ControllerRole& outputRole) const;

  SecurityResult deriveJournalKey(uint8_t output[kKitsuSecretBytes]);

  // Pairing is deliberately two-phase.  generatePendingControllerRoot() only
  // returns an in-RAM one-time grant; nothing durable changes until the BLE
  // state machine verifies pair_commit and calls commitControllerAfterPairing.
  SecurityResult generatePendingControllerRoot(
      bool secureConnections, bool linkEncrypted, bool bonded,
      bool physicalConfirmed, uint8_t outputRoot[kKitsuSecretBytes]);
  SecurityResult commitControllerAfterPairing(
      const uint8_t controllerId[kKitsuControllerIdBytes],
      const uint8_t pendingRoot[kKitsuSecretBytes],
      bool secureConnections, bool linkEncrypted, bool bonded,
      bool physicalConfirmed, bool pairCommitVerified,
      ControllerRole role = ControllerRole::Owner);
  SecurityResult revokeControllerAfterPhysicalConfirmation(
      const uint8_t controllerId[kKitsuControllerIdBytes],
      bool physicalConfirmed);
  // Lost-phone recovery for a full table. This preserves the device material
  // and every non-controller namespace; only an explicit local physical flow
  // may pass physicalConfirmed=true.
  SecurityResult revokeAllControllersAfterPhysicalConfirmation(
      bool physicalConfirmed);
  // An authenticated companion session is itself the authorization to revoke
  // that same controller. The caller must supply the controller id captured
  // by the verified handshake, never an id from an untrusted request body.
  SecurityResult revokeAuthenticatedController(
      const uint8_t controllerId[kKitsuControllerIdBytes]);

 private:
  struct Material {
    uint8_t deviceId[kKitsuDeviceIdBytes]{};
    uint8_t deviceSecret[kKitsuSecretBytes]{};
    struct Controller {
      bool valid = false;
      ControllerRole role = ControllerRole::Owner;
      uint8_t id[kKitsuControllerIdBytes]{};
      uint8_t root[kKitsuSecretBytes]{};
    } controllers[kKitsuControllerCapacity]{};
    bool controllerRetirementPending = false;
  };

  SecurityResult setResult(SecurityResult result);
  void clear();
  bool validateSlot(uint8_t slot, uint32_t& generation, size_t& bytes,
                    bool& nonempty);
  bool decodeLoaded(size_t bytes, bool& retiredMaterialPresent);
  bool encode(uint32_t generation, size_t& bytes);
  SecurityResult persist();
  SecurityResult revokeController(
      const uint8_t controllerId[kKitsuControllerIdBytes]);
  SecurityResult retirePreviousSlot();
  static bool generationAfter(uint32_t candidate, uint32_t reference);

  DeviceSecurityStorage* storage_ = nullptr;
  DeviceSecurityPlatform* platform_ = nullptr;
  Material material_{};
  DeviceSecurityStatus status_{};
  uint8_t hardwareId_[8]{};
  uint8_t wrappingKey_[kKitsuSecretBytes]{};
  uint8_t scratch_[kSecurityBlobCapacity]{};
  uint8_t cryptScratch_[kSecurityBlobCapacity]{};
};

}  // namespace connectivity
}  // namespace kitsu868
