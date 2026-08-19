#include <HAPFrame/HAPFrame.hpp>

#include <HAPCodec/HAPCodec.hpp>

namespace {

/** Offsets from Docs/Protocol.md section 2, named so the code reads like it. */
constexpr size_t kOffsetMagic = 0;
constexpr size_t kOffsetVersion = 2;
constexpr size_t kOffsetType = 3;
constexpr size_t kOffsetFlags = 4;
constexpr size_t kOffsetSeq = 5;
constexpr size_t kOffsetPathLens = 7;
constexpr size_t kOffsetDestPath = 8;
constexpr size_t kOffsetSrcPath = kOffsetDestPath + HAP_MAX_DEPTH;

static_assert(kOffsetSrcPath + HAP_MAX_DEPTH == HAP_HEADER_SIZE,
              "The header layout must add up to HAP_HEADER_SIZE.");

}  // namespace

const char* HAPFrameErrorToString(HAPFrameError error) noexcept {
  switch (error) {
    case HAPFrameError::None:
      return "none";
    case HAPFrameError::TooShort:
      return "too short";
    case HAPFrameError::BadMagic:
      return "bad magic";
    case HAPFrameError::BadVersion:
      return "bad version";
    case HAPFrameError::BadPathLength:
      return "bad path length";
    case HAPFrameError::BadHop:
      return "bad hop";
    case HAPFrameError::TooLong:
      return "too long";
  }

  return "unknown";
}

HAPFrame::HAPFrame() noexcept
    : type(static_cast<uint8_t>(HAPMessage::Ping)),
      flags(HAPFlags::None),
      seq(0),
      dest(),
      src(),
      payload(nullptr),
      payloadSize(0) {}

HAPFrameError HAPFrame::decode(const uint8_t* data, size_t size,
                               HAPFrame& out) noexcept {
  if (data == nullptr || size < HAP_HEADER_SIZE) {
    return HAPFrameError::TooShort;
  }

  if (size > HAP_MAX_FRAME_SIZE) {
    return HAPFrameError::TooLong;
  }

  if (data[kOffsetMagic] != HAP_MAGIC_0 || data[kOffsetMagic + 1] != HAP_MAGIC_1) {
    return HAPFrameError::BadMagic;
  }

  if (data[kOffsetVersion] != HAP_VERSION) {
    return HAPFrameError::BadVersion;
  }

  const uint8_t pathLens = data[kOffsetPathLens];
  const uint8_t destLen = static_cast<uint8_t>((pathLens >> 4) & 0x0F);
  const uint8_t srcLen = static_cast<uint8_t>(pathLens & 0x0F);

  if (destLen > HAP_MAX_DEPTH || srcLen > HAP_MAX_DEPTH) {
    return HAPFrameError::BadPathLength;
  }

  // fromBytes() returns an empty path for an illegal hop, which for a non-zero
  // length is exactly the case that must be refused: a zero or reserved index
  // inside an address would route the frame somewhere nobody addressed.
  const HAPPath dest = HAPPath::fromBytes(data + kOffsetDestPath, destLen);
  const HAPPath src = HAPPath::fromBytes(data + kOffsetSrcPath, srcLen);

  if (dest.length() != destLen || src.length() != srcLen) {
    return HAPFrameError::BadHop;
  }

  out.type = data[kOffsetType];
  out.flags = data[kOffsetFlags];
  out.seq = static_cast<uint16_t>(data[kOffsetSeq] |
                                  (static_cast<uint16_t>(data[kOffsetSeq + 1]) << 8));
  out.dest = dest;
  out.src = src;
  out.payloadSize = static_cast<uint8_t>(size - HAP_HEADER_SIZE);
  out.payload = out.payloadSize > 0 ? data + HAP_HEADER_SIZE : nullptr;

  return HAPFrameError::None;
}

size_t HAPFrame::encode(uint8_t* out, size_t capacity) const noexcept {
  if (payloadSize > HAP_MAX_PAYLOAD_SIZE) {
    return 0;
  }

  if (payloadSize > 0 && payload == nullptr) {
    return 0;
  }

  HAPWriter writer(out, capacity);

  writer.u8(HAP_MAGIC_0);
  writer.u8(HAP_MAGIC_1);
  writer.u8(HAP_VERSION);
  writer.u8(type);
  writer.u8(flags);
  writer.u16(seq);
  writer.u8(static_cast<uint8_t>((dest.length() << 4) | src.length()));

  // HAPPath keeps its unused tail zeroed, so this writes the padding the
  // specification calls for without anyone having to remember to.
  writer.bytes(dest.bytes(), HAP_MAX_DEPTH);
  writer.bytes(src.bytes(), HAP_MAX_DEPTH);
  writer.bytes(payload, payloadSize);

  return writer.ok() ? writer.size() : 0;
}

size_t HAPFrame::frameSize() const noexcept {
  return HAP_HEADER_SIZE + payloadSize;
}

bool HAPFrame::has(uint8_t mask) const noexcept {
  return (flags & mask) == mask;
}

void HAPFrame::set(uint8_t mask) noexcept {
  flags = static_cast<uint8_t>(flags | mask);
}

void HAPFrame::unset(uint8_t mask) noexcept {
  flags = static_cast<uint8_t>(flags & ~mask);
}

bool HAPFrame::isUpstream() const noexcept {
  return has(HAPFlags::Upstream);
}

HAPMessage HAPFrame::message() const noexcept {
  return static_cast<HAPMessage>(type);
}
