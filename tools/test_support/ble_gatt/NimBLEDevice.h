#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

constexpr uint16_t BLE_HS_CONN_HANDLE_NONE = 0xffffU;
constexpr uint8_t BLE_HS_IO_DISPLAY_YESNO = 1U;
constexpr uint8_t BLE_ATT_F_READ = 0x01U;
constexpr uint8_t BLE_ATT_F_WRITE = 0x02U;
constexpr uint8_t BLE_ATT_F_READ_ENC = 0x04U;
constexpr uint8_t BLE_ATT_F_READ_AUTHEN = 0x08U;
constexpr uint8_t BLE_ATT_F_WRITE_ENC = 0x10U;
constexpr uint8_t BLE_ATT_F_WRITE_AUTHEN = 0x20U;

struct BleHsConfig {
  uint8_t sm_sc_only = 0U;
};

inline BleHsConfig ble_hs_cfg{};

namespace NIMBLE_PROPERTY {
constexpr uint32_t WRITE = 1U << 0U;
constexpr uint32_t WRITE_NR = 1U << 1U;
constexpr uint32_t WRITE_ENC = 1U << 2U;
constexpr uint32_t WRITE_AUTHEN = 1U << 3U;
constexpr uint32_t NOTIFY = 1U << 4U;
}  // namespace NIMBLE_PROPERTY

class NimBLEServer;
class NimBLECharacteristic;

class NimBLEConnInfo {
 public:
  explicit NimBLEConnInfo(uint16_t handle = BLE_HS_CONN_HANDLE_NONE,
                          uint16_t mtu = 23U, bool encrypted = false,
                          bool authenticated = false, bool bonded = false,
                          uint8_t securityKeyBytes = 0U)
      : handle_(handle),
        mtu_(mtu),
        encrypted_(encrypted),
        authenticated_(authenticated),
        bonded_(bonded),
        securityKeyBytes_(securityKeyBytes) {}

  uint16_t getConnHandle() const { return handle_; }
  uint16_t getMTU() const { return mtu_; }
  bool isEncrypted() const { return encrypted_; }
  bool isAuthenticated() const { return authenticated_; }
  bool isBonded() const { return bonded_; }
  uint8_t getSecKeySize() const { return securityKeyBytes_; }

 private:
  uint16_t handle_;
  uint16_t mtu_;
  bool encrypted_;
  bool authenticated_;
  bool bonded_;
  uint8_t securityKeyBytes_;
};

class NimBLEAttValue {
 public:
  const uint8_t* data() const { return bytes_.data(); }
  size_t size() const { return bytes_.size(); }
  void assign(const uint8_t* input, size_t inputBytes) {
    bytes_.assign(input, input + inputBytes);
  }

 private:
  std::vector<uint8_t> bytes_{};
};

class NimBLEServerCallbacks {
 public:
  virtual ~NimBLEServerCallbacks() = default;
  virtual void onConnect(NimBLEServer*, NimBLEConnInfo&) {}
  virtual void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {}
  virtual void onMTUChange(uint16_t, NimBLEConnInfo&) {}
  virtual void onConfirmPassKey(NimBLEConnInfo&, uint32_t) {}
  virtual void onAuthenticationComplete(NimBLEConnInfo&) {}
};

class NimBLECharacteristicCallbacks {
 public:
  virtual ~NimBLECharacteristicCallbacks() = default;
  virtual void onWrite(NimBLECharacteristic*, NimBLEConnInfo&) {}
  virtual void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&,
                           uint16_t) {}
  virtual void onStatus(NimBLECharacteristic*, NimBLEConnInfo&, int) {}
};

class NimBLECharacteristic {
 public:
  void setCallbacks(NimBLECharacteristicCallbacks* callbacks) {
    callbacks_ = callbacks;
  }
  const NimBLEAttValue& getValue() const { return value_; }
  bool notify(const uint8_t*, size_t, uint16_t handle) {
    notifiedHandles.push_back(handle);
    return notifyResult;
  }

  NimBLECharacteristicCallbacks* callbacks() const { return callbacks_; }

  bool notifyResult = true;
  std::vector<uint16_t> notifiedHandles{};

 private:
  NimBLECharacteristicCallbacks* callbacks_ = nullptr;
  NimBLEAttValue value_{};
};

class NimBLEService {
 public:
  NimBLECharacteristic* createCharacteristic(const char*, uint32_t,
                                               uint16_t) {
    characteristics_.push_back(std::make_unique<NimBLECharacteristic>());
    return characteristics_.back().get();
  }

 private:
  std::vector<std::unique_ptr<NimBLECharacteristic>> characteristics_{};
};

class NimBLEAdvertising {
 public:
  bool setName(const char* name) {
    name_ = name ? name : "";
    return setNameResult;
  }
  bool addServiceUUID(const char*) { return addServiceResult; }
  void enableScanResponse(bool enabled) { scanResponse_ = enabled; }
  void setMinInterval(uint16_t interval) { minimumInterval_ = interval; }
  void setMaxInterval(uint16_t interval) { maximumInterval_ = interval; }
  bool start() {
    if (startResult) advertising_ = true;
    return startResult;
  }
  bool isAdvertising() const { return advertising_; }
  void stop() { advertising_ = false; }

  bool setNameResult = true;
  bool addServiceResult = true;
  bool startResult = true;

 private:
  std::string name_{};
  bool scanResponse_ = false;
  bool advertising_ = false;
  uint16_t minimumInterval_ = 0U;
  uint16_t maximumInterval_ = 0U;
};

class NimBLEServer {
 public:
  void setCallbacks(NimBLEServerCallbacks* callbacks, bool) {
    callbacks_ = callbacks;
  }
  void advertiseOnDisconnect(bool enabled) {
    advertiseOnDisconnect_ = enabled;
  }
  NimBLEService* createService(const char*) {
    service_ = std::make_unique<NimBLEService>();
    return service_.get();
  }
  bool disconnect(uint16_t handle) {
    disconnectCalls.push_back(handle);
    return true;
  }
  NimBLEConnInfo getPeerInfoByHandle(uint16_t handle) const {
    for (const NimBLEConnInfo& peer : peers_) {
      if (peer.getConnHandle() == handle) return peer;
    }
    return NimBLEConnInfo{};
  }

  void simulateConnect(NimBLEConnInfo connection) {
    replacePeer(connection);
    callbacks_->onConnect(this, connection);
  }
  void simulateDisconnect(uint16_t handle, int reason = 0) {
    NimBLEConnInfo connection = connectionForCallback(handle);
    callbacks_->onDisconnect(this, connection, reason);
    erasePeer(handle);
  }
  void simulateConfirmPasskey(uint16_t handle, uint32_t passkey) {
    NimBLEConnInfo connection = connectionForCallback(handle);
    callbacks_->onConfirmPassKey(connection, passkey);
  }
  void simulateAuthenticationComplete(NimBLEConnInfo connection) {
    replacePeer(connection);
    callbacks_->onAuthenticationComplete(connection);
  }

  std::vector<uint16_t> disconnectCalls{};

 private:
  NimBLEConnInfo connectionForCallback(uint16_t handle) const {
    const NimBLEConnInfo connection = getPeerInfoByHandle(handle);
    return connection.getConnHandle() == handle
        ? connection
        : NimBLEConnInfo(handle);
  }

  void replacePeer(const NimBLEConnInfo& connection) {
    erasePeer(connection.getConnHandle());
    peers_.push_back(connection);
  }
  void erasePeer(uint16_t handle) {
    for (auto peer = peers_.begin(); peer != peers_.end(); ++peer) {
      if (peer->getConnHandle() == handle) {
        peers_.erase(peer);
        return;
      }
    }
  }

  NimBLEServerCallbacks* callbacks_ = nullptr;
  std::unique_ptr<NimBLEService> service_{};
  std::vector<NimBLEConnInfo> peers_{};
  bool advertiseOnDisconnect_ = true;
};

class NimBLEDevice {
 public:
  struct NumericConfirmation {
    uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
    bool accepted = false;
  };

  static void init(const char*) { initialized_ = true; }
  static void setMTU(uint16_t mtu) { requestedMtu_ = mtu; }
  static void setSecurityIOCap(uint8_t ioCapability) {
    ioCapability_ = ioCapability;
  }
  static void setSecurityAuth(bool bonding, bool mitm,
                              bool secureConnections) {
    bonding_ = bonding;
    mitm_ = mitm;
    secureConnections_ = secureConnections;
  }
  static NimBLEServer* createServer() {
    server_ = std::make_unique<NimBLEServer>();
    return server_.get();
  }
  static NimBLEAdvertising* getAdvertising() { return &advertising_; }
  static void stopAdvertising() { advertising_.stop(); }
  static void deinit(bool) {
    server_.reset();
    initialized_ = false;
  }
  static bool startSecurity(uint16_t handle) {
    securityRequests.push_back(handle);
    return startSecurityResult;
  }
  static bool injectConfirmPasskey(const NimBLEConnInfo& connection,
                                   bool accept) {
    numericConfirmations.push_back(
        NumericConfirmation{connection.getConnHandle(), accept});
    return injectConfirmationResult;
  }
  static int getNumBonds() { return bondCount; }
  static bool deleteAllBonds() {
    if (deleteAllBondsResult) bondCount = 0;
    return deleteAllBondsResult;
  }

  static NimBLEServer* testServer() { return server_.get(); }
  static void testReset() {
    server_.reset();
    advertising_ = NimBLEAdvertising{};
    securityRequests.clear();
    numericConfirmations.clear();
    startSecurityResult = true;
    injectConfirmationResult = true;
    deleteAllBondsResult = true;
    bondCount = 0;
    requestedMtu_ = 0U;
    ioCapability_ = 0U;
    bonding_ = false;
    mitm_ = false;
    secureConnections_ = false;
    initialized_ = false;
    ble_hs_cfg = BleHsConfig{};
  }

  static inline bool startSecurityResult = true;
  static inline bool injectConfirmationResult = true;
  static inline bool deleteAllBondsResult = true;
  static inline int bondCount = 0;
  static inline std::vector<uint16_t> securityRequests{};
  static inline std::vector<NumericConfirmation> numericConfirmations{};

 private:
  static inline std::unique_ptr<NimBLEServer> server_{};
  static inline NimBLEAdvertising advertising_{};
  static inline bool initialized_ = false;
  static inline uint16_t requestedMtu_ = 0U;
  static inline uint8_t ioCapability_ = 0U;
  static inline bool bonding_ = false;
  static inline bool mitm_ = false;
  static inline bool secureConnections_ = false;
};
