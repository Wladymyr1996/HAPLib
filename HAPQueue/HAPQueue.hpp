#pragma once

#include <HAP.h>

#include <etl/vector.h>

/**
 * @brief One frame held for each child that is asleep.
 *
 * A battery node is awake for perhaps 250 ms in every 60 s, and ESP-NOW has no
 * store-and-forward - so for the rest of the time a parent cannot send to it at
 * all. It holds the frame here instead and delivers it as the reply to the
 * child's next report.
 *
 * A command to a sleeping node is therefore not lost, it is **late**: it lands
 * within one report interval. A user interface should say *pending* rather than
 * showing the lamp already off.
 *
 * ## One frame per child, and a refusal rather than a queue
 * A second frame for a child that already has one is refused, and the sender
 * gets Nack(Busy). A deeper queue would reorder commands nobody is watching and
 * deliver them minutes apart - an honest refusal is worth more than a promise
 * that arrives in the wrong order.
 *
 * ## A parent never answers on behalf of a child
 * Nothing here acknowledges anything. The frame sits until the child itself
 * takes it and answers, which is what lets an originator tell *pending* from
 * *done*.
 */
class HAPQueue {
 public:
  /**
   * @brief Holds a frame for a child.
   * @param childIndex 1..HAP_MAX_CHILDREN.
   * @return false when the index is not one, the frame is too big, or that
   *         child already has one waiting - which is Nack(Busy) at the caller.
   */
  bool push(uint8_t childIndex, const uint8_t* frame, size_t size) noexcept;

  bool has(uint8_t childIndex) const noexcept;

  /**
   * @brief The frame waiting for a child, without removing it.
   * @return nullptr when there is none. The pointer is valid until the next
   *         push() or pop() for that child.
   */
  const uint8_t* peek(uint8_t childIndex, size_t& size) const noexcept;

  /** @brief Drops what was waiting - after it has actually been sent. */
  void pop(uint8_t childIndex) noexcept;

  /** @brief How long a frame has been waiting, in ms. 0 when there is none. */
  uint32_t ageMs(uint8_t childIndex) const noexcept;

  /**
   * @brief Drops anything that has waited longer than `olderThanMs`.
   *
   * How long to hold a command for a node that has stopped waking is a decision
   * about the application, not about the queue - a lamp's "off" is worth
   * delivering an hour late and a thermostat's setpoint is not. So the policy
   * lives with whoever calls this.
   *
   * @return How many were dropped.
   */
  size_t expire(uint32_t olderThanMs) noexcept;

  /** @brief Frames waiting, across all children. */
  size_t size() const noexcept;

  void clear() noexcept;

 private:
  struct Slot {
    uint8_t childIndex = 0;
    uint32_t queuedAtMs = 0;
    uint16_t size = 0;
    uint8_t frame[HAP_MAX_FRAME_SIZE] = {};
  };

  Slot* find(uint8_t childIndex) noexcept;
  const Slot* find(uint8_t childIndex) const noexcept;

  etl::vector<Slot, HAP_MAX_CHILDREN> slots_;
};
