#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @file HAPRandom.hpp
 * @brief Random bytes, for the one thing in HAP that needs them: link keys.
 *
 * On an ESP32 this is `esp_random()`, which is a hardware generator fed by
 * radio noise once the RF subsystem is running - and a bind only ever happens
 * with the radio up, which is exactly when it is trustworthy.
 *
 * On a host it is a small deterministic generator, seeded to a fixed value so a
 * test that binds two nodes produces the same key every run. **That is not
 * suitable for a real key and is not meant to be**: a host build makes no
 * networks, and the property its tests check is that both ends ended up with
 * the SAME key, never what the key was.
 */
namespace HAPRandom {

/** @brief Fills `size` bytes. Does nothing if `buffer` is null. */
void fill(uint8_t* buffer, size_t size) noexcept;

/** @brief One random word. */
uint32_t next() noexcept;

/**
 * @brief Reseeds the host generator, so a test can vary or repeat a run.
 *
 * Ignored on target, where the hardware generator cannot be steered and should
 * not be.
 */
void seed(uint32_t value) noexcept;

}  // namespace HAPRandom
