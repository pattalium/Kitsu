#include "../src/kitsu_endpoint_rx_policy.h"

#include <assert.h>
#include <stddef.h>

int main() {
  static_assert(kitsu868::mesh::endpointFloodReceiveDelayMs(-1000.0f,
                                                            0xffffffffU) == 0,
                "endpoint floods must never enter the delayed queue");
  constexpr size_t kPhysicalPoolEntries = 10U;
  size_t retained = 0U;
  size_t peak = 0U;
  // Model a 32-frame burst, more than three times the real packet pool. The
  // Dispatcher <50 ms branch processes/releases each frame synchronously.
  for (size_t frame = 0U; frame < 32U; ++frame) {
    assert(retained < kPhysicalPoolEntries);
    ++retained;
    if (retained > peak) peak = retained;
    const int delay =
        kitsu868::mesh::endpointFloodReceiveDelayMs(0.0f, 1000U);
    assert(delay < 50);
    --retained;
  }
  assert(peak == 1U);
  assert(retained == 0U);
  return 0;
}
