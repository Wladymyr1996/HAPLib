#include <HAPPath/HAPPath.hpp>

#include <cstdio>
#include <cstring>

HAPPath::HAPPath() noexcept : hops_{}, length_(0) {}

bool HAPPath::isLegalHop(uint8_t hop) noexcept {
  return hop >= kMinHop && hop <= kMaxHop;
}

HAPPath HAPPath::fromBytes(const uint8_t* hops, uint8_t length) noexcept {
  HAPPath path;

  if (length > HAP_MAX_DEPTH) {
    return path;
  }

  if (length > 0 && hops == nullptr) {
    return path;
  }

  // Validated in full before anything is stored: a path is an address, and a
  // half-copied one would still be a routable address.
  for (uint8_t i = 0; i < length; ++i) {
    if (!isLegalHop(hops[i])) {
      return path;
    }
  }

  for (uint8_t i = 0; i < length; ++i) {
    path.hops_[i] = hops[i];
  }
  path.length_ = length;
  return path;
}

uint8_t HAPPath::length() const noexcept {
  return length_;
}

bool HAPPath::isEmpty() const noexcept {
  return length_ == 0;
}

bool HAPPath::isFull() const noexcept {
  return length_ >= HAP_MAX_DEPTH;
}

uint8_t HAPPath::hop(uint8_t index) const noexcept {
  return index < length_ ? hops_[index] : 0;
}

uint8_t HAPPath::first() const noexcept {
  return hop(0);
}

bool HAPPath::shift() noexcept {
  if (length_ == 0) {
    return false;
  }

  for (uint8_t i = 1; i < length_; ++i) {
    hops_[i - 1] = hops_[i];
  }

  --length_;
  hops_[length_] = 0;  // Keep the tail zeroed - see bytes().
  return true;
}

bool HAPPath::prepend(uint8_t hop) noexcept {
  if (isFull() || !isLegalHop(hop)) {
    return false;
  }

  for (uint8_t i = length_; i > 0; --i) {
    hops_[i] = hops_[i - 1];
  }

  hops_[0] = hop;
  ++length_;
  return true;
}

bool HAPPath::append(uint8_t hop) noexcept {
  if (isFull() || !isLegalHop(hop)) {
    return false;
  }

  hops_[length_] = hop;
  ++length_;
  return true;
}

void HAPPath::clear() noexcept {
  std::memset(hops_, 0, sizeof(hops_));
  length_ = 0;
}

const uint8_t* HAPPath::bytes() const noexcept {
  return hops_;
}

bool HAPPath::operator==(const HAPPath& other) const noexcept {
  if (length_ != other.length_) {
    return false;
  }

  return std::memcmp(hops_, other.hops_, length_) == 0;
}

bool HAPPath::operator!=(const HAPPath& other) const noexcept {
  return !(*this == other);
}

etl::string<HAP_PATH_TEXT_LEN> HAPPath::toString() const noexcept {
  etl::string<HAP_PATH_TEXT_LEN> text;

  // A lone dot rather than an empty string: "sent to " reads as truncated
  // output, and this address turns up in log lines often enough to matter.
  if (length_ == 0) {
    text = ".";
    return text;
  }

  char digits[4];
  for (uint8_t i = 0; i < length_; ++i) {
    if (i > 0) {
      text.append(1, '.');
    }

    std::snprintf(digits, sizeof(digits), "%u", static_cast<unsigned>(hops_[i]));
    text.append(digits);
  }

  return text;
}
