#pragma once

#include <HAPITransport/HAPITransport.hpp>

#include <etl/vector.h>

/**
 * @file HAPLoopback.hpp
 * @brief A network made of memory, so a whole tree fits inside one test.
 *
 * Host builds only. Several nodes attach to one HAPLoopbackBus, each with its
 * own address, and frames move between them when the test says so.
 *
 * ## It is deliberately unhelpful
 * A simulator that delivers everything to everybody would let code pass that
 * cannot work on a real board, so this one behaves the way Thread does:
 *
 * - a send that returns `true` only means the frame was queued, and a unicast
 *   to an address no node holds is **lost silently**;
 * - `HAPAddress::master()` is **anycast**: it reaches exactly one node that
 *   serves it, chosen when the frame is delivered, and nobody at all when no
 *   node does;
 * - a node that is not attached **neither sends nor receives**, not even a
 *   frame that was already queued for it;
 * - nothing is delivered until the test pumps the bus, so no send ever
 *   completes inside the caller's own stack frame.
 *
 * Each of those is a real failure that has cost somebody a day with two boards
 * and a serial log, and each is now a test that runs in a millisecond.
 *
 * ## Edge cases, decided
 * - A node receives only while it is started AND attached. One that never
 *   called begin() receives nothing, whatever setAttached() says.
 * - begin() attaches the node, even after an earlier setAttached(false).
 * - setServesMaster(true) makes a node an anycast server. Of several
 *   candidates the lowest node id that is attached and reachable from the
 *   sender gets the frame. That tie-break is a convenience for tests, not a
 *   promise: Thread picks the nearest server.
 * - A server that sends to master() does not hear itself, so a stack must
 *   handle its own local delivery.
 * - Unicast to an unknown address and anycast with no server are not counted
 *   by HAPLoopbackBus::droppedCount(), which counts only dropNext() losses.
 *
 * ## What it does not model
 * - Sleepy reception ("a sleepy end device receives only while it polls"):
 *   deferred until the poll and listen-window semantics are designed.
 * - Attachment takes time on Thread, where begin() returns first. Here it is
 *   immediate, so tests that need the gap use setAttached(false).
 */

class HAPLoopback;

/** @brief The shared medium. Create one per test; nodes attach to it. */
class HAPLoopbackBus {
 public:
  /** Frames in flight at once. Generous: a test that exceeds it has a loop. */
  static constexpr size_t kMaxQueued = 64;

  /** Nodes on one bus - more than a tree of depth 5 could hold. */
  static constexpr size_t kMaxNodes = 16;

  /** Blocked pairs, for simulating a node that has lost power. */
  static constexpr size_t kMaxBlocks = 16;

  void attach(HAPLoopback& node) noexcept;
  void detach(const HAPLoopback& node) noexcept;

  /**
   * @brief Delivers one round: every frame queued when this was called.
   *
   * Frames enqueued BY those deliveries wait for the next round, which is what
   * makes a multi-hop journey observable one hop at a time - and what stops a
   * forwarding loop from recursing until the stack runs out.
   *
   * @return How many frames were delivered.
   */
  size_t pump() noexcept;

  /**
   * @brief Pumps until nothing is left, or the round limit is hit.
   * @return Rounds run. Reaching `maxRounds` means something is circulating.
   */
  size_t pumpAll(size_t maxRounds = 16) noexcept;

  size_t pending() const noexcept;

  /** @brief Cuts the air between two nodes, both ways - a node losing power. */
  void setReachable(const HAPNodeId& a, const HAPNodeId& b,
                    bool reachable) noexcept;
  bool isReachable(const HAPNodeId& a, const HAPNodeId& b) const noexcept;

  /** @brief Silently loses the next `count` frames, wherever they are going. */
  void dropNext(size_t count) noexcept;

  size_t deliveredCount() const noexcept;
  size_t droppedCount() const noexcept;

  /** @brief Empties the queue and the counters; leaves nodes attached. */
  void reset() noexcept;

 private:
  friend class HAPLoopback;

  /** What the sender asked for, which decides who may receive the frame. */
  enum class Kind : uint8_t {
    Unicast,   ///< To one address.
    Anycast,   ///< To master(): one serving node, chosen at delivery.
    Multicast  ///< To every attached node but the sender.
  };

  struct Packet {
    Kind kind = Kind::Unicast;
    HAPAddress from;
    HAPAddress to;  ///< Unused by a multicast.
    uint8_t data[HAP_MAX_FRAME_SIZE] = {};
    size_t size = 0;
  };

  struct Block {
    HAPNodeId a;
    HAPNodeId b;
  };

  /** Called by a node's send()/multicast(). */
  bool enqueue(const Packet& packet) noexcept;

  HAPLoopback* find(const HAPAddress& address) const noexcept;

  /** Attached, not the sender, and not cut off from it. */
  bool hears(const HAPLoopback& node, const Packet& packet) const noexcept;

  void deliver(const Packet& packet) noexcept;

  etl::vector<HAPLoopback*, kMaxNodes> nodes_;
  etl::vector<Packet, kMaxQueued> queue_;
  etl::vector<Block, kMaxBlocks> blocks_;

  size_t dropCountdown_ = 0;
  size_t delivered_ = 0;
  size_t dropped_ = 0;
};

/** @brief One node's radio on a HAPLoopbackBus. */
class HAPLoopback : public HAPITransport {
 public:
  /**
   * @param id This node's identity; must be unique on the bus, and must come
   *        from HAPNodeId::fromMac(). The bus is MAC-addressed, so the address
   *        it is reached at is derived from the id, and only an id built that
   *        way survives the round trip (localAddress().nodeId() == id).
   * @param bus Not owned, and must outlive this.
   */
  HAPLoopback(const HAPNodeId& id, HAPLoopbackBus& bus) noexcept;

  ~HAPLoopback() override;

  bool begin() override;
  const HAPNodeId& localId() const noexcept override;

  /** @brief Started and attached: the only state in which a node hears anything. */
  bool isAttached() const noexcept override;

  /** @brief The address other nodes on the bus reach this one at. */
  const HAPAddress& localAddress() const noexcept;

  bool send(const HAPAddress& to, const uint8_t* data, size_t size) override;
  bool multicast(const uint8_t* data, size_t size) override;
  void onReceive(Receiver receiver) override;

  /**
   * @brief Test hook: leaves or rejoins the network.
   *
   * Detached, the node's send() and multicast() fail and it receives nothing,
   * including frames already queued for it. begin() attaches it again.
   */
  void setAttached(bool attached) noexcept;

  /**
   * @brief Test hook: makes this node an anycast server for master().
   *
   * Not part of the interface: how a real node becomes the server is the
   * Thread backend's business.
   */
  void setServesMaster(bool serves) noexcept;

  /** @brief Frames this node has handed to the bus, and taken off it. */
  size_t sentCount() const noexcept;
  size_t receivedCount() const noexcept;

 private:
  friend class HAPLoopbackBus;

  /** Called by the bus when a frame reaches this node. */
  void receive(const HAPLoopbackBus::Packet& packet) noexcept;

  bool servesMaster() const noexcept;

  bool post(HAPLoopbackBus::Kind kind, const HAPAddress& to,
            const uint8_t* data, size_t size) noexcept;

  HAPNodeId id_;
  HAPAddress address_;
  HAPLoopbackBus& bus_;
  Receiver receiver_;
  bool started_ = false;
  bool attached_ = false;
  bool servesMaster_ = false;

  size_t sent_ = 0;
  size_t received_ = 0;
};
