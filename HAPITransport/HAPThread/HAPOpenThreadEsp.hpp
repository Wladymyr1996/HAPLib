#pragma once

#ifdef ESP_PLATFORM
// CONFIG_OPENTHREAD_ENABLED only exists once this is included; without it the
// guard below would be silently false and the class would vanish.
#include "sdkconfig.h"

#if CONFIG_OPENTHREAD_ENABLED

#include <HAPITransport/HAPThread/HAPIOpenThread.hpp>

/**
 * @brief HAPIOpenThread over ESP-IDF's OpenThread port.
 *
 * Target only, and only in a build that has OpenThread enabled. The
 * application starts OpenThread (esp_openthread_start) and sets the dataset;
 * this only looks the instance up on every call, so open() fails - and
 * HAPThread::begin() with it - until that has happened. No OpenThread header is
 * included here, so any component may include this one.
 *
 * Every call runs under the OpenThread lock with a bounded wait; a call that
 * cannot get the lock fails like any other local failure. Datagrams are
 * delivered to the handler on the OpenThread task, which already holds that
 * lock.
 */
class HAPOpenThreadEsp : public HAPIOpenThread {
 public:
  HAPOpenThreadEsp() noexcept;
  ~HAPOpenThreadEsp() override;

  HAPOpenThreadEsp(const HAPOpenThreadEsp&) = delete;
  HAPOpenThreadEsp& operator=(const HAPOpenThreadEsp&) = delete;

  bool open(uint16_t port, Handler handler) override;
  void close() override;
  bool isAttached() override;
  bool meshLocalEid(HAPAddress& out) override;
  HAPNodeId factoryId() override;
  bool serviceAloc(uint32_t enterprise, const uint8_t* data, uint8_t size,
                   HAPAddress& out) override;
  bool isLocalAddress(const HAPAddress& address) override;
  bool sendTo(const HAPAddress& to, const HAPAddress& from, const uint8_t* data,
              size_t size) override;

 private:
  /** The socket and the receive trampoline live in the .cpp, with the OpenThread types. */
  struct Socket;

  /** close(), waiting for the lock without a limit when `unbounded`. */
  void closeSocket(bool unbounded);

  Handler handler_;
  Socket* socket_ = nullptr;
  uint16_t port_ = 0;

  // One datagram, read here and handed on. One byte more than a frame, so an
  // oversize datagram is seen as one. The OpenThread task is the only user.
  uint8_t rx_[HAP_MAX_FRAME_SIZE + 1];
};

#endif  // CONFIG_OPENTHREAD_ENABLED
#endif  // ESP_PLATFORM
