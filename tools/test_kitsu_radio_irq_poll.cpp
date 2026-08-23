#include "../src/kitsu_radio_irq_poll.h"

#include <assert.h>

namespace {

unsigned callbackCount = 0U;

void onRadioIrq() {
  ++callbackCount;
}

}  // namespace

int main() {
  using kitsu868::mesh::LatchedRadioIrqPoll;
  using kitsu868::mesh::kKitsuRadioDio1Pin;
  using kitsu868::mesh::kKitsuRadioDio1RisingMode;

  LatchedRadioIrqPoll poller;
  assert(!poller.claimed());
  assert(!poller.poll(true));
  auto diagnostics = poller.diagnostics();
  assert(diagnostics.polls == 1U);
  assert(diagnostics.highPolls == 1U);
  assert(diagnostics.highEdges == 1U);
  assert(diagnostics.callbacks == 0U);

  // Only the fixed Heltec SX1262 DIO1 rising registration is intercepted.
  assert(!poller.claim(kKitsuRadioDio1Pin + 1U, onRadioIrq,
                       kKitsuRadioDio1RisingMode));
  assert(!poller.claim(kKitsuRadioDio1Pin, onRadioIrq,
                       kKitsuRadioDio1RisingMode + 1));
  assert(!poller.claim(kKitsuRadioDio1Pin, nullptr,
                       kKitsuRadioDio1RisingMode));
  assert(poller.claim(kKitsuRadioDio1Pin, onRadioIrq,
                      kKitsuRadioDio1RisingMode));
  assert(poller.claimed());

  // DIO1 low is inert. A latched high is deliberately idempotent at the
  // MeshCore callback (its production callback ORs STATE_INT_READY); polling
  // therefore covers both RX-done and TX-done without an interrupt race.
  assert(!poller.poll(false));
  assert(callbackCount == 0U);
  assert(poller.poll(true));
  assert(callbackCount == 1U);
  assert(poller.poll(true));
  assert(callbackCount == 2U);

  diagnostics = poller.diagnostics();
  assert(diagnostics.polls == 4U);
  assert(diagnostics.highPolls == 3U);
  assert(diagnostics.highEdges == 2U);
  assert(diagnostics.callbacks == 2U);

  assert(poller.release(kKitsuRadioDio1Pin));
  assert(!poller.claimed());
  assert(!poller.poll(true));
  assert(callbackCount == 2U);
  assert(!poller.release(kKitsuRadioDio1Pin + 1U));

  diagnostics = poller.diagnostics();
  assert(diagnostics.polls == 5U);
  assert(diagnostics.highPolls == 4U);
  assert(diagnostics.highEdges == 2U);
  assert(diagnostics.callbacks == 2U);
  poller.clearDiagnostics();
  diagnostics = poller.diagnostics();
  assert(diagnostics.polls == 0U);
  assert(diagnostics.highPolls == 0U);
  assert(diagnostics.highEdges == 0U);
  assert(diagnostics.callbacks == 0U);
  return 0;
}
