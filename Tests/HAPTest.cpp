#include "HAPTest.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

const char* gSuite = "";
int gChecks = 0;
int gFailures = 0;

int hexDigit(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

void dump(const char* label, const uint8_t* data, size_t size) noexcept {
  std::printf("      %s (%zu bytes):", label, size);
  for (size_t i = 0; i < size; ++i) {
    if (i % 16 == 0) {
      std::printf("\n        ");
    }
    std::printf("%02X ", data[i]);
  }
  std::printf("\n");
}

}  // namespace

namespace HAPTest {

void begin(const char* suite) noexcept {
  gSuite = suite;
  std::printf("\n%s\n", suite);
}

bool check(bool passed, const char* expression, int line) noexcept {
  ++gChecks;

  if (passed) {
    return true;
  }

  ++gFailures;
  std::printf("  FAIL line %d: %s\n", line, expression);
  return false;
}

size_t parseHex(const char* hex, uint8_t* out, size_t capacity) noexcept {
  size_t written = 0;
  int high = -1;

  for (const char* c = hex; *c != '\0'; ++c) {
    if (std::isspace(static_cast<unsigned char>(*c))) {
      continue;
    }

    const int digit = hexDigit(*c);
    if (digit < 0) {
      return 0;
    }

    if (high < 0) {
      high = digit;
      continue;
    }

    if (written >= capacity) {
      return 0;
    }

    out[written++] = static_cast<uint8_t>((high << 4) | digit);
    high = -1;
  }

  // A trailing half-byte means the expected text is wrong, and comparing
  // against it would be comparing against a typo.
  return high < 0 ? written : 0;
}

void checkBytes(const uint8_t* actual, size_t actualSize, const char* expectedHex,
                const char* what, int line) noexcept {
  ++gChecks;

  uint8_t expected[512];
  const size_t expectedSize = parseHex(expectedHex, expected, sizeof(expected));

  if (expectedSize == 0) {
    ++gFailures;
    std::printf("  FAIL line %d: %s - malformed expected hex\n", line, what);
    return;
  }

  if (actualSize == expectedSize &&
      std::memcmp(actual, expected, expectedSize) == 0) {
    return;
  }

  ++gFailures;
  std::printf("  FAIL line %d: %s\n", line, what);

  if (actualSize != expectedSize) {
    std::printf("      size: expected %zu, got %zu\n", expectedSize, actualSize);
  }

  const size_t common = actualSize < expectedSize ? actualSize : expectedSize;
  for (size_t i = 0; i < common; ++i) {
    if (actual[i] != expected[i]) {
      std::printf("      first difference at offset %zu: expected %02X, got %02X\n",
                  i, expected[i], actual[i]);
      break;
    }
  }

  dump("expected", expected, expectedSize);
  dump("actual  ", actual, actualSize);
}

int report() noexcept {
  std::printf("\n%d checks, %d failed\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}

}  // namespace HAPTest

int main() {
  runPathTests();
  runCodecTests();
  runFrameTests();
  runMessageTests();
  runRouterTests();
  runTreeTests();
  runNodeTests();
  runBinderTests();
  runStoreTests();
  runReportTests();
  runModelTests();
  runLinksTests();
  runStackTests();
  runSpecTests();

  return HAPTest::report();
}
