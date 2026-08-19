#include <HAPListenWindow/HAPListenWindow.hpp>

HAPListenWindow::HAPListenWindow() noexcept : timer_(HAP_LISTEN_WINDOW_MS) {}

void HAPListenWindow::open() noexcept {
  timer_.start(HAP_LISTEN_WINDOW_MS);
  extensions_ = 0;
}

void HAPListenWindow::extend() noexcept {
  // isOpen(), not isRunning(): an HTimer stays "running" after it expires - it
  // is stop() that ends a run, not the timeout. Extending on isRunning() would
  // silently REOPEN a window that had already closed, and a battery node would
  // keep its radio on for a frame that arrived too late.
  if (!isOpen()) {
    // Nothing to extend. A frame arriving outside the window is a mains node's
    // ordinary traffic, not the tail of an exchange.
    return;
  }

  timer_.reset();

  if (extensions_ < UINT8_MAX) {
    ++extensions_;
  }
}

void HAPListenWindow::close() noexcept {
  timer_.stop();
}

bool HAPListenWindow::isOpen() const noexcept {
  return timer_.isRunning() && !timer_.isExpired();
}

bool HAPListenWindow::maySleep() const noexcept {
  return !isOpen();
}

uint32_t HAPListenWindow::remainingMs() const noexcept {
  return isOpen() ? timer_.remainingMs() : 0;
}

uint8_t HAPListenWindow::extensions() const noexcept {
  return extensions_;
}
