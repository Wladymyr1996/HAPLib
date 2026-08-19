#pragma once

#include <HAPFrame/HAPFrame.hpp>
#include <HAPITransport/HAPITransport.hpp>
#include <HAPNode/HAPNode.hpp>
#include <HAPRouter/HAPRouter.hpp>
#include <HTimer/HTimer.hpp>

#include <etl/delegate.h>

/** How often an unbound node repeats its announcement. */
#ifndef HAP_ANNOUNCE_PERIOD_MS
#define HAP_ANNOUNCE_PERIOD_MS 500
#endif

/**
 * How long an announcing node stays on one channel before trying the next.
 *
 * It has to be long enough for a master sitting on that channel to hear at
 * least one announcement and answer it, so it is several announce periods.
 */
#ifndef HAP_SWEEP_DWELL_MS
#define HAP_SWEEP_DWELL_MS 1500
#endif

/** How long a master waits for the confirmation that completes a bind. */
#ifndef HAP_CONFIRM_TIMEOUT_MS
#define HAP_CONFIRM_TIMEOUT_MS 2000
#endif

/**
 * How long a master will wait for its BindAccept to be reported as sent before
 * switching the link to encrypted anyway.
 *
 * A **fallback**, not the mechanism. The accept must go out in the CLEAR - it
 * carries the key the other end does not have yet - and everything after it is
 * encrypted, so the link changes key part-way through a conversation. Switching
 * too early can encrypt the accept itself; switching too late means the
 * confirmation arrives encrypted at a peer that is still expecting plain text,
 * and is dropped by the radio before any of this code sees it.
 *
 * The right moment is the instant the accept has actually left, which the
 * transport reports through HAPITransport::SendResult - see notifySent(). This
 * timeout only covers a transport that never reports one.
 */
#ifndef HAP_KEY_SWITCH_TIMEOUT_MS
#define HAP_KEY_SWITCH_TIMEOUT_MS 200
#endif

/**
 * @brief Both halves of the bind handshake. Docs/Protocol.md section 6.
 *
 * A press on each device, and nothing happens without both. This class is the
 * conversation that follows, on either side of it.
 *
 * ## It does not route, and does not need to
 * Every frame of a bind travels exactly one hop: the announcement is broadcast,
 * and the accept and the confirmation are unicast between two devices that are
 * about to become parent and child. So the binder talks to the transport
 * directly and never asks the router to forward anything.
 *
 * The one frame that IS routed - `ChildAttached`, telling the root about a new
 * node - is deliberately not sent from here. It has to climb, which needs a
 * parent, a path and a sequence number, none of which are this class's
 * business: it fires onChildBound() and whoever owns the stack sends it.
 *
 * ## The window is the exposure
 * The announcement is broadcast in the clear and the accept carries the link
 * key. That is the protocol's one honest weak point, mitigated by needing a
 * physical press at each end, by a short window, and by the accept going only to
 * the address that announced - and not eliminated. See Docs/Protocol.md
 * section 7.
 */
class HAPBinder {
 public:
  enum class State : uint8_t {
    Idle,        ///< Nothing in progress.
    Listening,   ///< Master: the window is open.
    Accepting,   ///< Master: accepted somebody, waiting for their confirmation.
    Announcing,  ///< Slave: shouting, and sweeping channels.
    Bound        ///< Slave: finished. One parent, for life.
  };

  /**
   * @param node Read for what to announce; never modified.
   */
  HAPBinder(HAPITransport& transport, HAPRouter& router,
            const HAPNode& node) noexcept;

  /**
   * @brief Opens the master's window. The user pressed Bind on this device.
   * @return false when the node already has HAP_MAX_CHILDREN children, which is
   *         the radio's ceiling on encrypted peers rather than a choice.
   */
  bool listen() noexcept;

  /** @brief Closes it early. */
  void stopListening() noexcept;

  /**
   * @brief Starts a slave announcing. The user pressed Bind on this device.
   *
   * Refused when the node already has a parent: one parent for life is what
   * keeps the network a tree, and a factory reset is the only way out of it.
   */
  bool announce() noexcept;

  void stopAnnouncing() noexcept;

  /** @brief Drives the timers. Call every HCORELIB_TICK_MS. */
  void update() noexcept;

  /**
   * @brief Offers a received frame to the handshake.
   *
   * Call this BEFORE the router: bind traffic comes from an address that is
   * neither parent nor child, which is exactly what the router refuses.
   *
   * @return true when the frame belonged to a handshake and was consumed.
   */
  bool handle(const HAPMac& from, const HAPFrame& frame) noexcept;

  /**
   * @brief Tells the handshake that a frame has left the radio.
   *
   * Wire the transport's SendResult here. It matters for exactly one instant:
   * a master's BindAccept goes out unencrypted, and the link must become
   * encrypted the moment it has - too early and the accept itself is encrypted,
   * too late and the confirmation is dropped by the radio for being encrypted
   * when the peer is not.
   *
   * Dispatched by the owner rather than subscribed to directly, so the binder
   * and everything else that wants send results are not fighting over one
   * callback slot.
   */
  void notifySent(const HAPMac& to, bool delivered) noexcept;

  State state() const noexcept;
  bool isListening() const noexcept;
  bool isAnnouncing() const noexcept;

  /** @brief Which channel the sweep is currently on; 0 when not sweeping. */
  uint8_t sweepChannel() const noexcept;

  /**
   * @brief Master: a child completed its bind.
   *
   * The moment to persist the new child and to originate a ChildAttached
   * upstream.
   */
  using ChildBoundHook = etl::delegate<void(const HAPChild&)>;
  void onChildBound(ChildBoundHook hook) noexcept;

  /**
   * @brief Slave: this node now has a parent.
   *
   * The moment to persist the parent, its index and the link key, because
   * everything after this depends on surviving a reboot.
   */
  using BoundHook = etl::delegate<void(const HAPMac& parent, uint8_t index,
                                       const uint8_t* linkKey,
                                       uint8_t channel)>;
  void onBound(BoundHook hook) noexcept;

  /** @brief The key of the link just bound, for whoever must store it. */
  const uint8_t* linkKey() const noexcept;

 private:
  /** Channels tried while announcing, in the order most networks use them. */
  //
  // 1, 6 and 11 first because they do not overlap and almost every network is
  // on one of them - including anything left at HAP_DEFAULT_CHANNEL.
  static constexpr uint8_t kSweepChannels[] = {1, 6, 11, 2,  3,  4,  5,
                                               7, 8, 9,  10, 12, 13};
  static constexpr size_t kSweepCount = sizeof(kSweepChannels);

  bool sendAnnouncement() noexcept;
  bool sendAccept(const HAPMac& to, uint8_t childIndex) noexcept;
  bool sendConfirm() noexcept;

  bool handleAnnounce(const HAPMac& from, const HAPFrame& frame) noexcept;
  bool handleAccept(const HAPMac& from, const HAPFrame& frame) noexcept;
  bool handleConfirm(const HAPMac& from, const HAPFrame& frame) noexcept;

  bool transmit(const HAPMac& to, HAPMessage type, uint8_t flags,
                const uint8_t* payload, size_t size) noexcept;

  void abandonAccept() noexcept;
  void switchToEncrypted() noexcept;

  HAPITransport& transport_;
  HAPRouter& router_;
  const HAPNode& node_;

  State state_ = State::Idle;

  HTimer window_;    ///< The master's 60 s, or the slave's.
  HTimer announce_;  ///< Between announcements.
  HTimer dwell_;     ///< On one channel of the sweep.
  HTimer confirm_;   ///< Waiting for a confirmation, or for the key switch.

  size_t sweepIndex_ = 0;
  uint8_t homeChannel_ = 0;  ///< Where a sweep started, to go back to.

  /** Master: who was accepted, and as which child, until they confirm. */
  HAPMac pending_;
  uint8_t pendingIndex_ = 0;
  uint8_t pendingCapabilities_ = HAPCaps::None;
  HAPDeviceType pendingType_ = HAPDeviceType::Sensor;
  uint16_t pendingRev_ = 0;
  uint16_t pendingInterval_ = 0;
  bool keySwitched_ = false;

  uint8_t linkKey_[HAP_KEY_LEN] = {};

  uint16_t seq_ = 0;

  ChildBoundHook onChildBound_;
  BoundHook onBound_;
};
