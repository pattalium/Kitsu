#include "../src/kitsu_tx_turnaround.h"

#include <assert.h>
#include <stdint.h>

namespace {

using kitsu868::mesh::TxTurnaroundCompletion;
using kitsu868::mesh::TxTurnaroundResult;
using kitsu868::mesh::runSynchronousTxTurnaround;
using kitsu868::mesh::txTurnaroundTimeoutMs;

struct FakeRadio {
  bool startOk = true;
  uint32_t now = 0U;
  uint32_t polls = 0U;
  uint32_t doneAtPoll = UINT32_MAX;
  uint32_t starts = 0U;
  uint32_t txDoneConsumes = 0U;
  uint32_t finishes = 0U;
  uint32_t rearms = 0U;
  uint32_t yields = 0U;
  uint32_t nSent = 0U;
  uint32_t boardBefore = 0U;
  uint32_t boardAfter = 0U;
  bool txIrq = false;
  bool rxDone = false;

  bool start() {
    ++starts;
    ++boardBefore;
    if (!startOk) {
      ++boardAfter;  // RadioLib failed-start cleanup.
      return false;
    }
    return true;
  }
  void poll() {
    ++polls;
    if (polls >= doneAtPoll) txIrq = true;
  }
  bool consumeTxDone() {
    if (!txIrq) return false;
    txIrq = false;
    ++txDoneConsumes;
    ++nSent;
    return true;
  }
  void finish() {
    ++finishes;
    ++boardAfter;
    txIrq = false;
  }
  void rearm() { ++rearms; }
  void yieldNow() {
    ++yields;
    ++now;
  }
};

TxTurnaroundResult run(FakeRadio& radio, TxTurnaroundCompletion& completion,
                       uint32_t timeout) {
  return runSynchronousTxTurnaround(
      completion, timeout,
      [&radio]() { return radio.start(); },
      [&radio]() { return radio.now; },
      [&radio]() { radio.poll(); },
      [&radio]() { return radio.consumeTxDone(); },
      [&radio]() { radio.finish(); },
      [&radio]() { radio.rearm(); },
      [&radio]() { radio.yieldNow(); });
}

}  // namespace

int main() {
  static_assert(txTurnaroundTimeoutMs(0U) == 32U,
                "zero/truncated airtime still gets the fixed margin");
  static_assert(txTurnaroundTimeoutMs(1U) == 33U,
                "integer 1.5x scaling preserves the fixed margin");
  static_assert(txTurnaroundTimeoutMs(2U) == 35U,
                "integer 1.5x scaling plus margin changed");
  static_assert(txTurnaroundTimeoutMs(229087U) == 343662U,
                "long valid low-bandwidth profiles must not be truncated");
  static_assert(txTurnaroundTimeoutMs(UINT32_MAX) == INT32_MAX,
                "overflow-safe timeout must remain wrap-safe");
  {
    FakeRadio radio{};
    radio.doneAtPoll = 3U;
    TxTurnaroundCompletion completion;
    assert(run(radio, completion, 10U) == TxTurnaroundResult::Completed);
    assert(radio.starts == 1U && radio.txDoneConsumes == 1U);
    assert(radio.nSent == 1U && radio.finishes == 1U);
    assert(radio.rearms == 1U);
    assert(radio.boardBefore == 1U && radio.boardAfter == 1U);
    assert(completion.takeForDispatcher());
    assert(!completion.takeForDispatcher());
    assert(completion.consumeReportedFinish());
    assert(!completion.consumeReportedFinish());
    // Dispatcher consumed only the independent latch: no second n_sent or
    // physical finish occurred.
    assert(radio.nSent == 1U && radio.finishes == 1U);
  }

  {
    FakeRadio radio{};
    radio.doneAtPoll = 1U;
    TxTurnaroundCompletion completion;
    assert(run(radio, completion, 10U) == TxTurnaroundResult::Completed);
    radio.rxDone = true;  // RX_DONE arrives before Dispatcher sees TX latch.
    assert(completion.takeForDispatcher());
    assert(radio.rxDone);  // isSendComplete must not consume the base RX flag.
    assert(completion.consumeReportedFinish());
    assert(radio.rxDone);  // duplicate finish is suppressed, preserving RX.
    assert(radio.finishes == 1U && radio.nSent == 1U);
  }

  {
    FakeRadio radio{};
    TxTurnaroundCompletion completion;
    assert(run(radio, completion, 3U) == TxTurnaroundResult::TimedOut);
    assert(radio.finishes == 1U && radio.rearms == 1U);
    assert(radio.nSent == 0U && radio.boardAfter == 1U);
    assert(!completion.takeForDispatcher());
    assert(!completion.consumeReportedFinish());
  }

  {
    FakeRadio radio{};
    radio.startOk = false;
    TxTurnaroundCompletion completion;
    assert(run(radio, completion, 3U) == TxTurnaroundResult::StartFailed);
    assert(radio.starts == 1U && radio.finishes == 0U);
    assert(radio.rearms == 1U && radio.nSent == 0U);
    assert(radio.boardBefore == 1U && radio.boardAfter == 1U);
  }

  {
    // TX_DONE appears only on the explicit final poll at the timeout edge.
    FakeRadio radio{};
    radio.doneAtPoll = 4U;
    TxTurnaroundCompletion completion;
    assert(run(radio, completion, 2U) == TxTurnaroundResult::Completed);
    assert(radio.polls == 4U && radio.nSent == 1U);
    assert(radio.finishes == 1U && radio.rearms == 1U);
  }

  {
    // A new failed attempt clears an unconsumed completion from the prior TX.
    FakeRadio radio{};
    radio.doneAtPoll = 1U;
    TxTurnaroundCompletion completion;
    assert(run(radio, completion, 2U) == TxTurnaroundResult::Completed);
    radio.startOk = false;
    assert(run(radio, completion, 2U) == TxTurnaroundResult::StartFailed);
    assert(!completion.takeForDispatcher());
    assert(!completion.consumeReportedFinish());
    assert(radio.nSent == 1U && radio.finishes == 1U);
    assert(radio.boardBefore == 2U && radio.boardAfter == 2U);
  }
  return 0;
}
