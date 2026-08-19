#include <HAPRandom/HAPRandom.hpp>

#include <HCoreLib.h>

#if IS_MCU
#include "esp_random.h"
#else
namespace {

/**
 * xorshift32, and deliberately nothing better.
 *
 * A host build makes no networks, so this exists to be REPEATABLE rather than
 * unpredictable: two nodes bound in a test agree on a key, and the same test
 * run twice produces the same one.
 */
uint32_t gState = 0x2545F491u;

}  // namespace
#endif

namespace HAPRandom {

uint32_t next() noexcept {
#if IS_MCU
  return esp_random();
#else
  gState ^= gState << 13;
  gState ^= gState >> 17;
  gState ^= gState << 5;
  return gState;
#endif
}

void fill(uint8_t* buffer, size_t size) noexcept {
  if (buffer == nullptr) {
    return;
  }

  size_t written = 0;

  while (written < size) {
    const uint32_t word = next();
    const size_t chunk = size - written < 4 ? size - written : 4;

    for (size_t i = 0; i < chunk; ++i) {
      buffer[written + i] = static_cast<uint8_t>((word >> (i * 8)) & 0xFF);
    }

    written += chunk;
  }
}

void seed(uint32_t value) noexcept {
#if IS_MCU
  // The hardware generator cannot be steered, and a caller who thinks it can
  // would be building a key nobody else could reproduce - which is the point.
  (void)value;
#else
  gState = value == 0 ? 1u : value;
#endif
}

}  // namespace HAPRandom
