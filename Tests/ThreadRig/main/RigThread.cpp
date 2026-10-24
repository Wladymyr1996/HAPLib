#include "RigThread.hpp"

#include <HAP.h>
#include <HAPITransport/HAPThread/HAPOpenThreadEsp.hpp>
#include <HAPITransport/HAPThread/HAPThread.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "openthread/ip6.h"

namespace rig {

namespace {

// OpenThread's own task runs at 5 and the console at 2. Both of ours sit
// between: above the console so a typed command cannot starve delivery, below
// OpenThread so a burst can never starve the stack that carries it (send takes
// the OpenThread lock with a bounded wait).
constexpr UBaseType_t kTaskPriority = 3;

// Each update() copies a ~270 byte slot and then formats a line with printf.
constexpr uint32_t kTickStack = 5120;
constexpr uint32_t kBurstStack = 4096;

constexpr TickType_t kTickPeriod = pdMS_TO_TICKS(10);

/** A burst frame is 'H', 'B', a big-endian sequence, then filler. */
constexpr size_t kBurstHeader = 6;

/** How many payload bytes one HAPRX line shows. */
constexpr size_t kShownBytes = 48;
constexpr size_t kShownHexBytes = 16;

HAPOpenThreadEsp gOpenThread;
HAPThread gThread(gOpenThread);

std::atomic<int64_t> gStallUntilUs{0};
std::atomic<bool> gBursting{false};

struct BurstJob {
  bool toMulticast = false;
  HAPAddress to;
  uint32_t count = 0;
  uint16_t size = 0;
  uint32_t intervalMs = 0;
};
BurstJob gJob;

void formatAddress(const HAPAddress& address, char* out, size_t size) {
  otIp6Address ip;
  std::memcpy(ip.mFields.m8, address.bytes, sizeof(ip.mFields.m8));
  otIp6AddressToString(&ip, out, static_cast<uint16_t>(size));
}

void formatId(const HAPNodeId& id, char* out) {
  for (size_t i = 0; i < HAP_NODE_ID_LEN; ++i) {
    std::snprintf(out + 2 * i, 3, "%02x", id.bytes[i]);
  }
}

uint8_t fillByte(uint32_t seq, size_t index) {
  return static_cast<uint8_t>((seq + index) & 0xFF);
}

bool isPrintable(const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    if (data[i] < 0x20 || data[i] > 0x7E) return false;
  }
  return true;
}

/** The HAPRX line. Runs on the tick task, inside update(). */
void onFrame(const HAPAddress& from, const uint8_t* data, size_t size) {
  char address[48];
  formatAddress(from, address, sizeof(address));

  char seq[12] = "-";
  const char* fill = "-";
  if (size >= kBurstHeader && data[0] == 'H' && data[1] == 'B') {
    const uint32_t number = (static_cast<uint32_t>(data[2]) << 24) |
                            (static_cast<uint32_t>(data[3]) << 16) |
                            (static_cast<uint32_t>(data[4]) << 8) | data[5];
    std::snprintf(seq, sizeof(seq), "%lu", static_cast<unsigned long>(number));
    fill = "ok";
    for (size_t i = kBurstHeader; i < size; ++i) {
      if (data[i] != fillByte(number, i)) {
        fill = "bad";
        break;
      }
    }
  }

  // The line is a preview; the full length is in len=.
  char shown[kShownBytes + 1];
  if (isPrintable(data, size)) {
    const size_t count = size < kShownBytes ? size : kShownBytes;
    std::memcpy(shown, data, count);
    shown[count] = '\0';
  } else {
    const size_t count = size < kShownHexBytes ? size : kShownHexBytes;
    size_t used = std::snprintf(shown, sizeof(shown), "hex:");
    for (size_t i = 0; i < count; ++i) {
      used += std::snprintf(shown + used, sizeof(shown) - used, "%02x", data[i]);
    }
  }

  std::printf("HAPRX from=%s len=%u seq=%s fill=%s data=%s\n", address,
              static_cast<unsigned>(size), seq, fill, shown);
}

void tickTask(void*) {
  int lastAttached = -1;
  for (;;) {
    const int attached = gThread.isAttached() ? 1 : 0;
    if (attached != lastAttached) {
      lastAttached = attached;
      std::printf("HAPSTATE attached=%d\n", attached);
    }
    if (esp_timer_get_time() >= gStallUntilUs.load()) {
      gThread.update();
    }
    vTaskDelay(kTickPeriod);
  }
}

void burstTask(void*) {
  uint8_t frame[HAP_MAX_FRAME_SIZE];
  uint32_t sent = 0;
  uint32_t failed = 0;
  const TickType_t interval = pdMS_TO_TICKS(gJob.intervalMs);

  for (uint32_t seq = 0; seq < gJob.count; ++seq) {
    frame[0] = 'H';
    frame[1] = 'B';
    frame[2] = static_cast<uint8_t>(seq >> 24);
    frame[3] = static_cast<uint8_t>(seq >> 16);
    frame[4] = static_cast<uint8_t>(seq >> 8);
    frame[5] = static_cast<uint8_t>(seq);
    for (size_t i = kBurstHeader; i < gJob.size; ++i) {
      frame[i] = fillByte(seq, i);
    }

    const bool ok = gJob.toMulticast ? gThread.multicast(frame, gJob.size)
                                     : gThread.send(gJob.to, frame, gJob.size);
    ok ? ++sent : ++failed;

    // At least one tick, so a zero interval still yields to the other tasks.
    vTaskDelay(interval == 0 ? 1 : interval);
  }

  std::printf("HAPBURST sent=%lu failed=%lu\n", static_cast<unsigned long>(sent),
              static_cast<unsigned long>(failed));
  gBursting = false;
  vTaskDelete(nullptr);
}

}  // namespace

bool startThread() {
  // The receiver goes in before begin(), as the transport asks.
  gThread.onReceive(HAPITransport::Receiver::create<onFrame>());

  if (!gThread.begin()) {
    std::printf("HAPERR begin\n");
    return false;
  }

  char id[2 * HAP_NODE_ID_LEN + 1];
  formatId(gThread.localId(), id);
  std::printf("HAPBOOT id=%s\n", id);

  xTaskCreate(tickTask, "rig_tick", kTickStack, nullptr, kTaskPriority, nullptr);
  return true;
}

void printInfo() {
  char id[2 * HAP_NODE_ID_LEN + 1];
  formatId(gThread.localId(), id);

  char address[48] = "-";
  const HAPAddress own = gThread.localAddress();
  if (!own.isZero()) formatAddress(own, address, sizeof(address));

  std::printf("HAPINFO id=%s addr=%s attached=%d dropped=%u heap=%u\n", id,
              address, gThread.isAttached() ? 1 : 0,
              static_cast<unsigned>(gThread.droppedCount()),
              static_cast<unsigned>(esp_get_free_heap_size()));
}

void stall(uint32_t ms) {
  gStallUntilUs = esp_timer_get_time() + static_cast<int64_t>(ms) * 1000;
}

void send(const HAPAddress& to, const uint8_t* data, size_t size) {
  std::printf("HAPTX ok=%d\n", gThread.send(to, data, size) ? 1 : 0);
}

void multicast(const uint8_t* data, size_t size) {
  std::printf("HAPTX ok=%d\n", gThread.multicast(data, size) ? 1 : 0);
}

bool startBurst(bool toMulticast, const HAPAddress& to, uint32_t count,
                uint16_t size, uint32_t intervalMs) {
  bool expected = false;
  if (!gBursting.compare_exchange_strong(expected, true)) return false;

  gJob.toMulticast = toMulticast;
  gJob.to = to;
  gJob.count = count;
  gJob.size = size;
  gJob.intervalMs = intervalMs;

  if (xTaskCreate(burstTask, "rig_burst", kBurstStack, nullptr, kTaskPriority,
                  nullptr) != pdPASS) {
    gBursting = false;
    return false;
  }
  return true;
}

}  // namespace rig
