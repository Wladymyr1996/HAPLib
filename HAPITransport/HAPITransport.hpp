#pragma once

#include <HAP.h>
#include <HAPAddress/HAPAddress.hpp>
#include <HAPNodeId/HAPNodeId.hpp>

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
 * ## Why this one is virtual when HCoreLib's platform seams are not
 * HCoreLib compiles exactly one backend of HFs or HGpio, because a device has
 * one filesystem and one set of pins. A test here has SEVERAL transports alive at
 * once - one per simulated node - so the choice is per object rather than per
 * build, and that is what a virtual is for. On target there is still only one
 * implementation, and the vtable costs one indirection per frame.
 *
 * ## What an implementation must promise
 * It is an IP-style datagram interface: no channel, no peer table, no key and
 * no delivery report. Whoever is behind it - a Thread stack, a simulated bus -
 * must behave the same way, because code written against a permissive simulator
 * would fail on the first real board.
 *
 * - **send() is not delivery.** `true` means the frame was handed to the
 *   stack; `false` is only ever a local failure (not started or not attached,
 *   a null pointer, a size of 0 or above HAP_MAX_FRAME_SIZE, a full queue).
 *   Whether anything received it is answered by the protocol above - with
 *   ACK_REQ and timeouts - and on a sleeping peer the honest answer is usually
 *   "no".
 * - **An address nobody holds is lost silently.** The send still returns true.
 * - **HAPAddress::master() is anycast.** It is a valid destination for send(),
 *   resolved when the frame is delivered rather than when it is sent, and a
 *   frame for a master nobody serves is lost silently. Which node answers is
 *   the stack's choice, not a promise of this interface.
 * - **`from` is always the sender's own unicast address**, never master().
 * - **begin() may return before the node is attached** to a network. Poll
 *   isAttached(); send() and multicast() fail until it is true.
 * - **The receiver runs where the stack does.** On a real radio that can be a
 *   task where blocking is not allowed and calling back into HAP is not safe -
 *   so an implementation copies into a queue and nothing more.
 */
class HAPITransport {
 public:
  /**
   * @brief Called with every frame that arrives.
   * @param from The sender's address - which is the only trustworthy statement
   *        about where a frame came from, since nothing inside it is signed.
   */
  using Receiver = etl::delegate<void(const HAPAddress& from,
                                      const uint8_t* data, size_t size)>;

  virtual ~HAPITransport();

  /**
   * @brief Brings the stack up.
   *
   * May return before the node has joined a network; see isAttached().
   */
  virtual bool begin() = 0;

  /** @brief This node's own identity. */
  virtual const HAPNodeId& localId() const noexcept = 0;

  /** @brief True once the node has joined a network and can send and receive. */
  virtual bool isAttached() const noexcept = 0;

  /**
   * @brief Hands one frame to the stack, for one address or for master().
   *
   * See the notes above: true is not delivery, an unknown address is lost
   * silently, and master() is resolved at delivery.
   */
  virtual bool send(const HAPAddress& to, const uint8_t* data,
                    size_t size) = 0;

  /**
   * @brief Sends to every attached node in range except this one.
   *
   * Only binding uses this, and only because a node that has never met anybody
   * has no other way to be heard. There is no per-peer key: whatever protects
   * the network protects this too.
   */
  virtual bool multicast(const uint8_t* data, size_t size) = 0;

  /** @brief Installs the receiver. One at a time; the last wins. */
  virtual void onReceive(Receiver receiver) = 0;
};
