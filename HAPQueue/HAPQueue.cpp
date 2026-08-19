#include <HAPQueue/HAPQueue.hpp>

#include <HSystemUtils/HSystemUtils.hpp>

#include <cstring>

HAPQueue::Slot* HAPQueue::find(uint8_t childIndex) noexcept {
  for (Slot& slot : slots_) {
    if (slot.childIndex == childIndex) {
      return &slot;
    }
  }

  return nullptr;
}

const HAPQueue::Slot* HAPQueue::find(uint8_t childIndex) const noexcept {
  for (const Slot& slot : slots_) {
    if (slot.childIndex == childIndex) {
      return &slot;
    }
  }

  return nullptr;
}

bool HAPQueue::push(uint8_t childIndex, const uint8_t* frame,
                    size_t size) noexcept {
  if (childIndex < 1 || childIndex > HAP_MAX_CHILDREN) {
    return false;
  }

  if (frame == nullptr || size == 0 || size > HAP_MAX_FRAME_SIZE) {
    return false;
  }

  // Occupied. Refused rather than replaced or queued behind: the sender is owed
  // Nack(Busy), and silently dropping either frame would be worse than both.
  if (find(childIndex) != nullptr) {
    return false;
  }

  if (slots_.full()) {
    return false;
  }

  Slot slot;
  slot.childIndex = childIndex;
  slot.queuedAtMs = HSystemUtils::millis();
  slot.size = static_cast<uint16_t>(size);
  std::memcpy(slot.frame, frame, size);

  slots_.push_back(slot);
  return true;
}

bool HAPQueue::has(uint8_t childIndex) const noexcept {
  return find(childIndex) != nullptr;
}

const uint8_t* HAPQueue::peek(uint8_t childIndex, size_t& size) const noexcept {
  const Slot* slot = find(childIndex);

  if (slot == nullptr) {
    size = 0;
    return nullptr;
  }

  size = slot->size;
  return slot->frame;
}

void HAPQueue::pop(uint8_t childIndex) noexcept {
  for (auto it = slots_.begin(); it != slots_.end(); ++it) {
    if (it->childIndex == childIndex) {
      slots_.erase(it);
      return;
    }
  }
}

uint32_t HAPQueue::ageMs(uint8_t childIndex) const noexcept {
  const Slot* slot = find(childIndex);
  return slot == nullptr ? 0 : HSystemUtils::millis() - slot->queuedAtMs;
}

size_t HAPQueue::expire(uint32_t olderThanMs) noexcept {
  const uint32_t now = HSystemUtils::millis();
  size_t dropped = 0;

  for (auto it = slots_.begin(); it != slots_.end();) {
    if (now - it->queuedAtMs >= olderThanMs) {
      it = slots_.erase(it);
      ++dropped;
    } else {
      ++it;
    }
  }

  return dropped;
}

size_t HAPQueue::size() const noexcept {
  return slots_.size();
}

void HAPQueue::clear() noexcept {
  slots_.clear();
}
