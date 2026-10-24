#pragma once

#include <HAPITransport/HAPITransport.hpp>
#include <HAPITransport/HAPThread/HAPIOpenThread.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

/** The UDP port every HAP node binds. Never 61631: that one is Thread's own. */
#ifndef HAP_THREAD_UDP_PORT
#define HAP_THREAD_UDP_PORT 61700
#endif

/** Received frames held between the OpenThread task and update(); a slot is ~270 bytes. */
#ifndef HAP_THREAD_RX_QUEUE
#define HAP_THREAD_RX_QUEUE 8
#endif

/** Enterprise number of the `HAP-Master` Network Data service. A placeholder until the service is registered. */
#ifndef HAP_THREAD_ENTERPRISE_NUMBER
#define HAP_THREAD_ENTERPRISE_NUMBER 0x00ABCDEF
#endif

static_assert(HAP_THREAD_RX_QUEUE > 0, "the receive queue needs at least one slot");

/**
 * @file HAPThread.hpp
 * @brief HAPITransport over Thread: one UDP socket, the ML-EID as the address.
 *
 * It behaves the way HAPLoopback does, which is the reference; the differences
 * are the ones below. All OpenThread calls go through a HAPIOpenThread, so
 * everything here runs against a fake in a host test.
 *
 * ## Addresses
 * A node's address is its mesh-local EID: it survives reboots and role changes,
 * unlike an RLOC. Every send pins the ML-EID as the source address, so `from`
 * at the receiver is always the sender's own unicast address - even when the
 * destination is an anycast one, where the stack would otherwise pick the RLOC.
 * master() resolves to the `HAP-Master` service ALOC found in Network Data on
 * every send, and the frame goes to the nearest server. With no server, or when
 * this node is the server, it is lost silently and send() still returns true.
 *
 * ## Differences from the loopback
 * - multicast() goes to `ff03::2`, so it reaches the rx-on nodes (routers,
 *   REEDs, FEDs). A sleepy end device does not receive it.
 * - A destination that is not a unicast address - zero(), broadcast(), any
 *   other multicast - is refused silently: true, nothing sent.
 * - A frame already queued when the node detaches is still delivered by the
 *   next update(); the loopback discards it.
 * - localId() is zero until begin() has succeeded, because the OpenThread
 *   instance the id is read from only exists once the application started it.
 *   Read it after begin().
 * - begin() returns false until OpenThread is running. It never forms or joins
 *   a network and never waits for one; the application does that, and polls
 *   isAttached().
 *
 * ## Tasks
 * The stack delivers datagrams on its own task, which copies them into a
 * bounded queue and returns. update() hands them to the receiver on the
 * caller's task, so call it regularly from the one task that runs the HAP
 * stack. onReceive() and update() belong to that task, and the receiver should
 * be installed before begin(). send() and multicast() may be called from any
 * task. A full queue drops the new frame and counts it.
 */
class HAPThread : public HAPITransport {
 public:
  /** The `HAP-Master` service data, without a terminator. */
  static constexpr uint8_t kMasterServiceData[10] = {'H', 'A', 'P', '-', 'M',
                                                     'a', 's', 't', 'e', 'r'};

  /** @param ot Not owned, and must outlive this. */
  explicit HAPThread(HAPIOpenThread& ot) noexcept;

  /** Closes the socket if begin() opened it. */
  ~HAPThread() override;

  HAPThread(const HAPThread&) = delete;
  HAPThread& operator=(const HAPThread&) = delete;

  /**
   * @brief Opens the socket. Idempotent.
   * @return False while OpenThread is not running, or if the bind fails.
   */
  bool begin() override;

  /** @brief The factory EUI-64: zero until begin() has succeeded. */
  const HAPNodeId& localId() const noexcept override;

  /** @brief Started, in a child, router or leader role, and holding an ML-EID. */
  bool isAttached() const noexcept override;

  bool send(const HAPAddress& to, const uint8_t* data, size_t size) override;
  bool multicast(const uint8_t* data, size_t size) override;
  void onReceive(Receiver receiver) override;

  /**
   * @brief Delivers the frames that were queued when this was called.
   *
   * Frames that arrive meanwhile wait for the next call. With no receiver
   * installed the frames are discarded.
   */
  void update() noexcept;

  /** @brief The own ML-EID, or zero() when there is none yet. */
  HAPAddress localAddress() const noexcept;

  /** @brief Frames dropped on receive: a full queue, or a size of 0 or above the maximum. */
  size_t droppedCount() const noexcept;

 private:
  struct Slot {
    HAPAddress from;
    uint16_t size = 0;
    uint8_t data[HAP_MAX_FRAME_SIZE] = {};
  };

  /** Runs on the OpenThread task. */
  void onFrame(const HAPAddress& from, const uint8_t* data, size_t size) noexcept;

  /** Where master() goes right now; false when nothing should be sent. */
  bool resolveMaster(HAPAddress& out) const noexcept;

  bool post(const HAPAddress& to, const uint8_t* data, size_t size) noexcept;

  HAPIOpenThread& ot_;
  Receiver receiver_;
  HAPNodeId id_;
  bool started_ = false;

  // A single-producer, single-consumer ring: the OpenThread task writes
  // straight into a slot, so no frame-sized temporary lands on its small
  // stack, and neither side ever blocks. The counters only grow; a slot is
  // the counter modulo the capacity.
  Slot slots_[HAP_THREAD_RX_QUEUE];
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
  std::atomic<size_t> dropped_{0};
};
