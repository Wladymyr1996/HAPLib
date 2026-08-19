#pragma once

#include <HAP.h>
#include <HTimer/HTimer.hpp>

/**
 * @brief The short moment after speaking when a sleeping node can be reached.
 *
 * This is the whole of what makes a battery node addressable. It wakes, sends
 * its report, and keeps the radio on for HAP_LISTEN_WINDOW_MS - long enough for
 * its parent's reply and one queued frame. Then it sleeps, and for the next
 * minute it is not there at all.
 *
 * Every millisecond of it is paid for out of a cell at radio-receive current,
 * so the window is long enough for one round trip and not a millisecond more.
 *
 * ## Why this is not just an HTimer
 * Because of extend(). Anything arriving inside the window is evidence that the
 * parent is mid-conversation - a queued command, say, which is about to want a
 * response - so the window starts again rather than closing underneath it. A
 * bare timer would cut off the exchange it exists to allow.
 *
 * The application asks maySleep() before sleeping, and that is the only
 * coupling between this and the rest of the node.
 */
class HAPListenWindow {
 public:
  HAPListenWindow() noexcept;

  /** @brief Opens it, after an upstream frame has gone out. */
  void open() noexcept;

  /**
   * @brief Starts it again, after something arrived inside it.
   *
   * A frame in the window means the conversation is not over. Extending costs a
   * few milliseconds; closing early costs a whole report interval, because the
   * answer has to wait for the next wake.
   */
  void extend() noexcept;

  /** @brief Closes it now - the exchange finished early. */
  void close() noexcept;

  bool isOpen() const noexcept;

  /** @brief The inverse, for the sleep path to read. */
  bool maySleep() const noexcept;

  /** @brief Milliseconds left, or 0 when closed. */
  uint32_t remainingMs() const noexcept;

  /** @brief How many times this window has been extended since it opened. */
  uint8_t extensions() const noexcept;

 private:
  HTimer timer_;
  uint8_t extensions_ = 0;
};
