#include "../src/kitsu_endpoint_rx_policy.h"

#include <Dispatcher.h>
#include <Packet.h>

#include <assert.h>
#include <stdint.h>
#include <string.h>

namespace {

constexpr size_t kPoolCapacity = 10U;
constexpr size_t kBurstFrames = 32U;

class FakeClock final : public mesh::MillisecondClock {
 public:
  unsigned long getMillis() override { return now_; }
  void advance() { ++now_; }

 private:
  unsigned long now_ = 1UL;
};

class BurstRadio final : public mesh::Radio {
 public:
  int recvRaw(uint8_t* bytes, int capacity) override {
    if (delivered_ >= kBurstFrames) return 0;
    assert(bytes != nullptr);
    assert(capacity >= 4);
    bytes[0] = static_cast<uint8_t>(
        (PAYLOAD_TYPE_GRP_TXT << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD);
    bytes[1] = 1U;  // one one-byte path token
    bytes[2] = static_cast<uint8_t>(delivered_ + 1U);
    bytes[3] = 0x42U;  // non-empty payload
    ++delivered_;
    return 4;
  }

  uint32_t getEstAirtimeFor(int) override { return 100U; }
  float packetScore(float, int) override { return 0.0F; }
  bool startSendRaw(const uint8_t*, int) override { return false; }
  bool isSendComplete() override { return false; }
  void onSendFinished() override {}
  bool isInRecvMode() const override { return true; }
  float getLastRSSI() const override { return -90.0F; }
  float getLastSNR() const override { return 7.0F; }

  size_t delivered() const { return delivered_; }

 private:
  size_t delivered_ = 0U;
};

class FixedPoolManager final : public mesh::PacketManager {
 public:
  mesh::Packet* allocNew() override {
    ++allocations_;
    for (size_t i = 0U; i < kPoolCapacity; ++i) {
      if (used_[i]) continue;
      used_[i] = true;
      ++retained_;
      if (retained_ > peakRetained_) peakRetained_ = retained_;
      return &packets_[i];
    }
    ++allocationFailures_;
    return nullptr;
  }

  void free(mesh::Packet* packet) override {
    assert(packet != nullptr);
    for (size_t i = 0U; i < kPoolCapacity; ++i) {
      if (packet != &packets_[i]) continue;
      assert(used_[i]);
      used_[i] = false;
      assert(retained_ > 0U);
      --retained_;
      ++frees_;
      return;
    }
    assert(false && "freed packet did not belong to fixed pool");
  }

  void queueOutbound(mesh::Packet*, uint8_t, uint32_t) override {
    assert(false && "endpoint RX test must not queue outbound packets");
  }
  mesh::Packet* getNextOutbound(uint32_t) override { return nullptr; }
  int getOutboundCount(uint32_t) const override { return 0; }
  int getOutboundTotal() const override { return 0; }
  int getFreeCount() const override {
    return static_cast<int>(kPoolCapacity - retained_);
  }
  mesh::Packet* getOutboundByIdx(int) override { return nullptr; }
  mesh::Packet* removeOutboundByIdx(int) override { return nullptr; }

  void queueInbound(mesh::Packet*, uint32_t) override { ++inboundQueued_; }
  mesh::Packet* getNextInbound(uint32_t) override { return nullptr; }

  size_t allocations() const { return allocations_; }
  size_t allocationFailures() const { return allocationFailures_; }
  size_t frees() const { return frees_; }
  size_t retained() const { return retained_; }
  size_t peakRetained() const { return peakRetained_; }
  size_t inboundQueued() const { return inboundQueued_; }

 private:
  mesh::Packet packets_[kPoolCapacity]{};
  bool used_[kPoolCapacity]{};
  size_t allocations_ = 0U;
  size_t allocationFailures_ = 0U;
  size_t frees_ = 0U;
  size_t retained_ = 0U;
  size_t peakRetained_ = 0U;
  size_t inboundQueued_ = 0U;
};

class EndpointDispatcher final : public mesh::Dispatcher {
 public:
  EndpointDispatcher(mesh::Radio& radio, mesh::MillisecondClock& clock,
                     mesh::PacketManager& packets)
      : mesh::Dispatcher(radio, clock, packets) {}

  size_t processed() const { return processed_; }
  size_t rawLogged() const { return rawLogged_; }

 protected:
  mesh::DispatcherAction onRecvPacket(mesh::Packet* packet) override {
    assert(packet != nullptr);
    assert(packet->isRouteFlood());
    ++processed_;
    return ACTION_RELEASE;
  }

  void logRxRaw(float, float, const uint8_t*, int) override { ++rawLogged_; }

  int calcRxDelay(float score, uint32_t airTime) const override {
    return kitsu868::mesh::endpointFloodReceiveDelayMs(score, airTime);
  }

 private:
  size_t processed_ = 0U;
  size_t rawLogged_ = 0U;
};

}  // namespace

int main() {
  FakeClock clock;
  BurstRadio radio;
  FixedPoolManager packets;
  EndpointDispatcher dispatcher(radio, clock, packets);
  dispatcher.begin();

  for (size_t i = 0U; i < kBurstFrames; ++i) {
    dispatcher.loop();
    clock.advance();
  }

  assert(radio.delivered() == kBurstFrames);
  assert(dispatcher.rawLogged() == kBurstFrames);
  assert(dispatcher.processed() == kBurstFrames);
  assert(dispatcher.getNumRecvFlood() == kBurstFrames);
  assert(packets.allocations() == kBurstFrames);
  assert(packets.allocationFailures() == 0U);
  assert(packets.inboundQueued() == 0U);
  assert(packets.frees() == kBurstFrames);
  assert(packets.retained() == 0U);
  assert(packets.peakRetained() == 1U);
  assert(packets.getFreeCount() == static_cast<int>(kPoolCapacity));
  return 0;
}
