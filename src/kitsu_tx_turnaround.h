#pragma once

#include <stdint.h>

namespace kitsu868 {
namespace mesh {

enum class TxTurnaroundResult : uint8_t {
  Completed = 0,
  StartFailed,
  TimedOut,
};

constexpr uint32_t kTxTurnaroundMarginMs = 32U;
constexpr uint32_t kMaximumTxTurnaroundTimeoutMs = INT32_MAX;

// RadioLib truncates time-on-air from microseconds to milliseconds. Keep the
// upstream 1.5x safety factor, add a small scheduler/rounding margin, and
// clamp only at the largest wrap-safe millis() interval. RadioLib's uint32
// microsecond ToA can produce at most ~6.45 million ms after this scaling,
// far below INT32_MAX; this clamp therefore never truncates a valid estimate.
// Kitsu accepts low-bandwidth SF12 profiles whose valid packet airtime can
// exceed many seconds, so an arbitrary short cap would abort valid traffic.
constexpr uint32_t txTurnaroundTimeoutMs(uint32_t estimatedAirtimeMs) {
  return static_cast<uint64_t>(estimatedAirtimeMs) +
          static_cast<uint64_t>(estimatedAirtimeMs) / 2U +
          kTxTurnaroundMarginMs > kMaximumTxTurnaroundTimeoutMs
      ? kMaximumTxTurnaroundTimeoutMs
      : static_cast<uint32_t>(
            static_cast<uint64_t>(estimatedAirtimeMs) +
            static_cast<uint64_t>(estimatedAirtimeMs) / 2U +
            kTxTurnaroundMarginMs);
}

// Completion is consumed by Dispatcher separately from physical TX cleanup.
// Keeping this latch independent from RadioLib's shared RX/TX IRQ flag is what
// lets RX_DONE arrive after immediate rearm without being mistaken for (or
// erased by) the already-consumed TX_DONE.
class TxTurnaroundCompletion {
 public:
  void reset() {
    completionLatched_ = false;
    completionReported_ = false;
  }

  void latchCompleted() { completionLatched_ = true; }

  bool takeForDispatcher() {
    if (!completionLatched_) return false;
    completionLatched_ = false;
    completionReported_ = true;
    return true;
  }

  // True means physical finish/rearm already happened synchronously and the
  // matching Dispatcher cleanup callback must be a one-shot no-op.
  bool consumeReportedFinish() {
    if (!completionReported_) return false;
    completionReported_ = false;
    return true;
  }

 private:
  bool completionLatched_ = false;
  bool completionReported_ = false;
};

// Start the asynchronous radio operation, synchronously consume its latched
// TX_DONE within the caller-provided bound, finish the transmitter exactly
// once, and rearm RX before returning. Callback arguments keep the state
// machine host-testable and prevent any SPI work from running in an ISR.
template <typename Start, typename Clock, typename PollIrq,
          typename ConsumeTxDone, typename FinishTx, typename RearmRx,
          typename CooperativeYield>
TxTurnaroundResult runSynchronousTxTurnaround(
    TxTurnaroundCompletion& completion, uint32_t timeoutMs,
    Start start, Clock clock, PollIrq pollIrq,
    ConsumeTxDone consumeTxDone, FinishTx finishTx, RearmRx rearmRx,
    CooperativeYield cooperativeYield) {
  completion.reset();
  if (!start()) {
    // RadioLib's failed start path already balances onBefore/onAfterTransmit.
    // It leaves the wrapper idle, so only RX rearm belongs here.
    rearmRx();
    return TxTurnaroundResult::StartFailed;
  }

  const uint32_t startedAt = clock();
  while (true) {
    pollIrq();
    if (consumeTxDone()) {
      finishTx();
      rearmRx();
      completion.latchCompleted();
      return TxTurnaroundResult::Completed;
    }
    if (static_cast<uint32_t>(clock() - startedAt) >= timeoutMs) break;
    cooperativeYield();
  }

  // One final level-latched DIO1 sample closes the boundary between the last
  // timed check and abort cleanup.
  pollIrq();
  if (consumeTxDone()) {
    finishTx();
    rearmRx();
    completion.latchCompleted();
    return TxTurnaroundResult::Completed;
  }

  finishTx();
  rearmRx();
  completion.reset();
  return TxTurnaroundResult::TimedOut;
}

}  // namespace mesh
}  // namespace kitsu868
