#include <cstdint>

void setup();
void loop();

extern "C" void kitsu_hal_advance_millis(uint32_t milliseconds);
extern "C" uint32_t kitsu_emulator_entropy_ready();

namespace {
bool booted = false;
}

extern "C" uint32_t kitsu_emulator_boot() {
  if (booted || kitsu_emulator_entropy_ready() == 0U) return 0U;
  setup();
  booted = true;
  return 1U;
}

extern "C" uint32_t kitsu_emulator_step(uint32_t elapsedMilliseconds) {
  if (!booted) return 0U;
  kitsu_hal_advance_millis(elapsedMilliseconds);
  loop();
  return 1U;
}

extern "C" uint32_t kitsu_emulator_booted() { return booted ? 1U : 0U; }
