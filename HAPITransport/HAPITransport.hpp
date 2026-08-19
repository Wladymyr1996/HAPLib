#pragma once

#include <HAP.h>
#include <HAPMac/HAPMac.hpp>

#include <cstddef>

#include <etl/delegate.h>

/**
 * @brief The radio, behind one interface.
 *
 * Everything above this line - routing, binding, reporting, links - is logic
 * over `send these bytes to that address`. Putting the radio behind an interface
 * is what lets a whole tree of nodes be built inside one host process, each with
 * its own transport on a shared simulated bus, and it is why the two-chip
 * gateway can later speak HAP over a serial link with nothing above it changing.
 *
 * ## Why this one is virtual when HLib's platform seams are not
 * HLib compiles exactly one backend of HFs or HGpio, because a device has one
 * filesystem and one set of pins. A test here has SEVERAL transports alive at
 * once - one per simulated node - so the choice is per object rather than per
 * build, and that is what a virtual is for. On target there is still only one
 * implementation, and the vtable costs one indirection per frame.
 *
 * ## What an implementation must promise
 * - **send() is not delivery.** It reports that the frame was handed to the
 *   radio. Whether anything received it is answered by the protocol above, and
 *   on a sleeping peer the honest answer is usually "no".
 * - **The receiver runs where the radio does.** On ESP-NOW that is the Wi-Fi
 *   task, where blocking is not allowed and calling back into HAP is not safe -
 *   so an implementation copies into a queue and nothing more.
 * - **A stranger cannot be unicast to.** ESP-NOW refuses to send to an address
 *   that is not in its peer table, and every implementation must behave the
 *   same way, because code written against a permissive simulator would fail on
 *   the first real board.
 */
class HAPITransport {
 public:
  /**
   * @brief Called with every frame that arrives.
   * @param from The sender's address - which is the only trustworthy statement
   *        about where a frame came from, since nothing inside it is signed.
   */
  using Receiver = etl::delegate<void(const HAPMac& from, const uint8_t* data,
                                      size_t size)>;

  /**
   * @brief Called when a frame has actually left the radio.
   *
   * NOT delivery - it says the frame was transmitted and, for a unicast,
   * whether the link layer saw an acknowledgement. Two things need it: retrying
   * a hop, and the one moment in the bind handshake where a link's encryption
   * changes part-way through a conversation. Guessing at that moment with a
   * timer is a race; this is the event it should hang off.
   */
  using SendResult = etl::delegate<void(const HAPMac& to, bool delivered)>;

  virtual ~HAPITransport();

  /**
   * @brief Brings the radio up on one channel.
   *
   * Every node in a network must share it: ESP-NOW has no channel negotiation,
   * and two boards one channel apart are simply deaf to each other.
   */
  virtual bool begin(uint8_t channel) = 0;

  /** @brief The channel begin() was given, or 0 before it. */
  virtual uint8_t channel() const noexcept = 0;

  /**
   * @brief Moves to another channel.
   *
   * Needed by exactly one thing: an unbound node has no idea which channel its
   * future parent is listening on, so it sweeps while it announces. Everything
   * else in the protocol assumes the network shares one channel and never
   * touches this.
   */
  virtual bool setChannel(uint8_t channel) = 0;

  /** @brief This node's own address. */
  virtual const HAPMac& localMac() const noexcept = 0;

  /**
   * @brief Adds an address this node may unicast to.
   * @param key HAP_KEY_LEN bytes to encrypt the link with, or nullptr for a
   *        plain peer. Only HAP_MAX_CHILDREN links may be encrypted - the radio
   *        ceiling that decides how many children a node may have.
   */
  virtual bool addPeer(const HAPMac& mac, const uint8_t* key) = 0;

  virtual bool removePeer(const HAPMac& mac) = 0;
  virtual bool hasPeer(const HAPMac& mac) const = 0;

  /** @brief Hands one frame to the radio. See the note above about delivery. */
  virtual bool send(const HAPMac& mac, const uint8_t* data, size_t size) = 0;

  /**
   * @brief Sends to everyone in range, unencrypted.
   *
   * Only binding uses this, and only because a node that has never met anybody
   * has no other way to be heard.
   */
  virtual bool broadcast(const uint8_t* data, size_t size) = 0;

  /** @brief Installs the receiver. One at a time; the last wins. */
  virtual void onReceive(Receiver receiver) = 0;

  /** @brief Installs the send-result callback. One at a time; the last wins. */
  virtual void onSendResult(SendResult result) = 0;
};
