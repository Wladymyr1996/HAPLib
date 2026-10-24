#pragma once

#include <HAPITransport/HAPITransport.hpp>

#include <etl/vector.h>

/**
 * @file HAPLoopback.hpp
 * @brief A radio made of memory, so a whole network fits inside one test.
 *
 * Host builds only. Several nodes attach to one HAPLoopbackBus, each with its
 * own address, and frames move between them when the
 * test says so.
 *
 * ## It is deliberately unhelpful
 * A simulator that delivers everything to everybody would let code pass that
 * cannot work on a real board, so this one is strict about what a radio is:
 *
 * - nothing is delivered until the test pumps the bus, so no send ever
 *   completes inside the caller's own stack frame.
 *
 * Each of those is a real failure that has cost somebody a day with two boards
 * and a serial log, and each is now a test that runs in a millisecond.
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

  struct Packet {
    HAPAddress from;
    HAPAddress to;
    uint8_t data[HAP_MAX_FRAME_SIZE] = {};
    size_t size = 0;
  };

  struct Block {
    HAPNodeId a;
    HAPNodeId b;
  };

  /** Called by a node's send()/broadcast(). */
  bool enqueue(const Packet& packet) noexcept;

  HAPLoopback* find(const HAPAddress& address) const noexcept;
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

  /** @brief The address other nodes on the bus reach this one at. */
  const HAPAddress& localAddress() const noexcept;

  bool send(const HAPAddress& to, const uint8_t* data, size_t size) override;
  bool broadcast(const uint8_t* data, size_t size) override;
  void onReceive(Receiver receiver) override;

  /** @brief Frames this node has handed to the bus, and taken off it. */
  size_t sentCount() const noexcept;
  size_t receivedCount() const noexcept;

 private:
  friend class HAPLoopbackBus;

  /** Called by the bus when a frame reaches this node. */
  void receive(const HAPLoopbackBus::Packet& packet) noexcept;

  HAPNodeId id_;
  HAPAddress address_;
  HAPLoopbackBus& bus_;
  Receiver receiver_;
  bool started_ = false;

  size_t sent_ = 0;
  size_t received_ = 0;
};
