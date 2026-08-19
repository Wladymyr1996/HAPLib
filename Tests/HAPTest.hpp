#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @file HAPTest.hpp
 * @brief A test harness small enough to not need explaining.
 *
 * No framework: a host suite that needs one has stopped being a thing you can
 * run in a second on any machine, and the whole point of testing this library on
 * a desktop is that doing so costs nothing.
 *
 * CHECK_BYTES is the one that earns its place. HAP is a wire protocol whose
 * specification prints hexadecimal, so a test that compares against that same
 * hexadecimal - and prints both dumps when they differ - is the only thing that
 * keeps Docs/Protocol.md and this code from drifting apart.
 */
namespace HAPTest {

/** @brief Starts a named group; every check after this belongs to it. */
void begin(const char* suite) noexcept;

/** @brief Records one assertion. Called through CHECK. */
bool check(bool passed, const char* expression, int line) noexcept;

/**
 * @brief Compares bytes against hexadecimal text, ignoring whitespace in it.
 *
 * On a mismatch it prints both, byte for byte, with the first differing offset
 * called out - because "expected 60 bytes, got 59" is not a debuggable message
 * for something that came off a radio.
 */
void checkBytes(const uint8_t* actual, size_t actualSize, const char* expectedHex,
                const char* what, int line) noexcept;

/**
 * @brief Parses hexadecimal text into bytes; whitespace is ignored.
 * @return Bytes written, or 0 on a malformed digit or an odd digit count -
 *         which fails the suite rather than quietly testing half a frame.
 */
size_t parseHex(const char* hex, uint8_t* out, size_t capacity) noexcept;

/** @brief Prints the totals. @return A process exit code: 0 when all passed. */
int report() noexcept;

}  // namespace HAPTest

#define CHECK(expression) HAPTest::check((expression), #expression, __LINE__)

/**
 * @brief A check the rest of the test cannot survive - a pointer, usually.
 *
 * CHECK records a failure and carries on, which is what you want for an
 * assertion about a value. It is exactly what you do NOT want after `p !=
 * nullptr`: the next line dereferences it and the whole run dies with a
 * segfault instead of a failure report naming the line.
 */
#define REQUIRE(expression)                                  \
  do {                                                       \
    if (!HAPTest::check((expression), #expression, __LINE__)) { \
      return;                                                \
    }                                                        \
  } while (false)

#define CHECK_BYTES(buffer, size, expectedHex) \
  HAPTest::checkBytes((buffer), (size), (expectedHex), #buffer, __LINE__)

void runPathTests() noexcept;
void runCodecTests() noexcept;
void runFrameTests() noexcept;
void runMessageTests() noexcept;
void runRouterTests() noexcept;
void runTreeTests() noexcept;
void runNodeTests() noexcept;
void runBinderTests() noexcept;
void runStoreTests() noexcept;
void runReportTests() noexcept;
void runModelTests() noexcept;
void runLinksTests() noexcept;
void runStackTests() noexcept;
void runSpecTests() noexcept;
