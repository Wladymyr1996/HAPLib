#pragma once

#include <HAPAddress/HAPAddress.hpp>
#include <HAPNodeId/HAPNodeId.hpp>

#include <cstddef>
#include <cstdint>

#include <etl/delegate.h>

/**
 * @brief The OpenThread calls HAPThread needs, in HAP types.
 *
 * HAPThread holds the logic - what to send where, what to drop, what to queue -
 * and this holds only the stack. The split is what lets the logic run in a host
 * test against a fake, while the target gets the real thing from
 * HAPOpenThreadEsp. No OpenThread header appears here or in HAPThread.
 *
 * Every method may be called from any task; an implementation does its own
 * locking against the stack.
 */
class HAPIOpenThread {
 public:
  /**
   * @brief Called for every datagram that arrives on the socket.
   *
   * Runs on the OpenThread task: copy and return, never call HAP code. `data`
   * is only valid during the call, and `size` is the real datagram size even
   * when `data` holds fewer bytes than that.
   */
  using Handler = etl::delegate<void(const HAPAddress& from, const uint8_t* data,
                                     size_t size)>;

  virtual ~HAPIOpenThread();

  /** @brief Opens the UDP socket and binds it to `port`. False if the stack is not running. */
  virtual bool open(uint16_t port, Handler handler) = 0;

  /** @brief Closes the socket. Harmless when it is not open. */
  virtual void close() = 0;

  /** @brief True while the role is child, router or leader. */
  virtual bool isAttached() = 0;

  /** @brief The mesh-local EID. False if the stack has none to give. */
  virtual bool meshLocalEid(HAPAddress& out) = 0;

  /** @brief The factory-assigned 802.15.4 EUI-64. */
  virtual HAPNodeId factoryId() = 0;

  /**
   * @brief The ALOC of the first Network Data service that matches.
   * @param enterprise The service's enterprise number.
   * @param data The service data it must carry, byte for byte.
   * @return False when no entry matches or its ALOC cannot be resolved.
   */
  virtual bool serviceAloc(uint32_t enterprise, const uint8_t* data,
                           uint8_t size, HAPAddress& out) = 0;

  /** @brief True if this node itself holds `address` as a unicast address. */
  virtual bool isLocalAddress(const HAPAddress& address) = 0;

  /**
   * @brief Sends one datagram from the bound port to the same port at `to`.
   * @param from The source address to pin; the stack must not choose its own.
   * @return False on a local failure: no buffer, stack busy, socket not open.
   */
  virtual bool sendTo(const HAPAddress& to, const HAPAddress& from,
                      const uint8_t* data, size_t size) = 0;
};
