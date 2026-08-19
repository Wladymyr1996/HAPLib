#define HLOG_MODULE_NAME "HapStore"

#include <HAPStore/HAPStore.hpp>

#include <HConfig/HConfig.hpp>
#include <HLog/HLog.hpp>
#include <HRtcStore/HRtcStore.hpp>

#include <cstring>

#if IS_MCU
// For RTC_DATA_ATTR, which is what puts the mirror below in memory the chip
// keeps powered through a deep sleep.
#include "esp_attr.h"
#endif

// A 16-byte link key is 32 hexadecimal characters, and an HValue that cannot
// hold them would truncate one silently - producing a device that binds, saves,
// reboots and can never talk to its parent again. Failing the build is the only
// acceptable version of that.
static_assert(HVALUE_MAX_STRING_LEN >= 32,
              "HAPStore stores a 16-byte link key as 32 hex characters; raise "
              "HVALUE_MAX_STRING_LEN to at least 32 in the application's "
              "HLibConfig.h");

namespace {

constexpr const char* kBindModule = "hapbind";
constexpr const char* kChildModule = "hapkids";
constexpr const char* kNameModule = "hapnames";
constexpr const char* kModelModule = "hapmodel";

// Sorted, because HConfig::write() requires it and getting it wrong is a file
// that reads back as defaults.
constexpr const char* kKeyLinkKey = "key";
constexpr const char* kKeyLink = "link";
constexpr const char* kKeyChildCount = "n";
constexpr const char* kKeyNodeName = "node";
constexpr const char* kKeyNodeCount = "n";

/** Retained across deep sleep; plain data, so nothing constructs over it. */
struct MirroredBind {
  uint32_t magic;
  uint8_t parent[HAP_MAC_LEN];
  uint8_t linkKey[HAP_KEY_LEN];
  uint8_t indexAtParent;
  uint8_t channel;
  uint8_t bound;
};

constexpr uint32_t kMagic = 0x48415042u;  // 'HAPB'

#if IS_MCU
RTC_DATA_ATTR MirroredBind mirror;
#else
MirroredBind mirror;
#endif

bool gRestoredFromRtc = false;

char hexDigit(uint8_t value) noexcept {
  return static_cast<char>(value < 10 ? '0' + value : 'a' + (value - 10));
}

int hexValue(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/** Appends `size` bytes as lower-case hex. */
void appendHex(etl::string<HVALUE_MAX_STRING_LEN>& text, const uint8_t* data,
               size_t size) noexcept {
  for (size_t i = 0; i < size; ++i) {
    text.append(1, hexDigit(static_cast<uint8_t>(data[i] >> 4)));
    text.append(1, hexDigit(static_cast<uint8_t>(data[i] & 0x0F)));
  }
}

/** @return false on a bad digit or a string that is not exactly `size` bytes. */
bool parseHex(const char* text, size_t length, uint8_t* out,
              size_t size) noexcept {
  if (text == nullptr || length != size * 2) {
    return false;
  }

  for (size_t i = 0; i < size; ++i) {
    const int high = hexValue(text[i * 2]);
    const int low = hexValue(text[i * 2 + 1]);

    if (high < 0 || low < 0) {
      return false;
    }

    out[i] = static_cast<uint8_t>((high << 4) | low);
  }

  return true;
}

void writeMirror(const HAPBindState& state) noexcept {
  std::memcpy(mirror.parent, state.parent.bytes, HAP_MAC_LEN);
  std::memcpy(mirror.linkKey, state.linkKey, HAP_KEY_LEN);
  mirror.indexAtParent = state.indexAtParent;
  mirror.channel = state.channel;
  mirror.bound = state.bound ? 1 : 0;
  mirror.magic = kMagic;
}

void readMirror(HAPBindState& state) noexcept {
  state.parent = HAPMac::fromBytes(mirror.parent);
  std::memcpy(state.linkKey, mirror.linkKey, HAP_KEY_LEN);
  state.indexAtParent = mirror.indexAtParent;
  state.channel = mirror.channel;
  state.bound = mirror.bound != 0;
}

}  // namespace

void HAPStore::init() noexcept {
  HRtcStore::track(mirror.magic);
}

// ---------------------------------------------------------------------------
// The link upward
// ---------------------------------------------------------------------------

bool HAPStore::loadBind(HAPBindState& state) noexcept {
  gRestoredFromRtc = false;

  // Retained memory first. On a timer wake this is the whole of the work, and
  // the filesystem is never touched.
  if (HRtcStore::isValid(mirror.magic, kMagic)) {
    readMirror(state);
    gRestoredFromRtc = true;
    return state.bound;
  }

  const etl::string<HVALUE_MAX_STRING_LEN> link =
      HConfig::read(kBindModule, kKeyLink, HValue("")).asString();
  const etl::string<HVALUE_MAX_STRING_LEN> key =
      HConfig::read(kBindModule, kKeyLinkKey, HValue("")).asString();

  // parent(6) + index(1) + channel(1), packed.
  uint8_t link_[HAP_MAC_LEN + 2] = {};
  if (!parseHex(link.c_str(), link.size(), link_, sizeof(link_))) {
    return false;
  }

  uint8_t linkKey[HAP_KEY_LEN] = {};
  if (!parseHex(key.c_str(), key.size(), linkKey, sizeof(linkKey))) {
    HWarning("config/%s.cfg has a link but no usable key", kBindModule);
    return false;
  }

  state.parent = HAPMac::fromBytes(link_);
  state.indexAtParent = link_[HAP_MAC_LEN];
  state.channel = link_[HAP_MAC_LEN + 1];
  std::memcpy(state.linkKey, linkKey, sizeof(linkKey));
  state.bound = true;

  // Seed the mirror from the file, so the NEXT wake is the cheap kind.
  writeMirror(state);

  HInfo("restored parent %s as child %u on channel %u",
        state.parent.toString().c_str(), state.indexAtParent, state.channel);
  return true;
}

bool HAPStore::saveBind(const HAPBindState& state) noexcept {
  writeMirror(state);

  if (!state.bound) {
    clearBind();
    return true;
  }

  uint8_t packed[HAP_MAC_LEN + 2] = {};
  std::memcpy(packed, state.parent.bytes, HAP_MAC_LEN);
  packed[HAP_MAC_LEN] = state.indexAtParent;
  packed[HAP_MAC_LEN + 1] = state.channel;

  etl::string<HVALUE_MAX_STRING_LEN> link;
  appendHex(link, packed, sizeof(packed));

  etl::string<HVALUE_MAX_STRING_LEN> key;
  appendHex(key, state.linkKey, HAP_KEY_LEN);

  const HConfigEntry entries[] = {
      {kKeyLinkKey, HValue(etl::string_view(key.data(), key.size()))},
      {kKeyLink, HValue(etl::string_view(link.data(), link.size()))},
  };

  if (!HConfig::write(kBindModule, etl::span<const HConfigEntry>(entries, 2))) {
    HCritical("could not write config/%s.cfg - this bind will not survive a "
              "power cut",
              kBindModule);
    return false;
  }

  return true;
}

void HAPStore::clearBind() noexcept {
  // The retained copy goes first. Clearing only the file would leave the next
  // wake restoring the parent this call just deleted.
  mirror.magic = 0;
  mirror.bound = 0;

  HConfig::remove(kBindModule);
}

bool HAPStore::restoredFromRtc() noexcept {
  return gRestoredFromRtc;
}

// ---------------------------------------------------------------------------
// Children
// ---------------------------------------------------------------------------

size_t HAPStore::loadChildren(HAPRouter& router) noexcept {
  const int stored = HConfig::read(kChildModule, kKeyChildCount, HValue(0)).asInt();
  if (stored <= 0) {
    return 0;
  }

  size_t restored = 0;

  for (int i = 0; i < stored && i < static_cast<int>(HAP_MAX_CHILDREN); ++i) {
    char path[4] = {'c', static_cast<char>('0' + i), '\0', '\0'};

    const etl::string<HVALUE_MAX_STRING_LEN> packed =
        HConfig::read(kChildModule, path, HValue("")).asString();

    // index(1) + mac(6) + caps(1) + type(1) + interval(2) + rev(2)
    uint8_t bytes[HAP_MAC_LEN + 7] = {};
    if (!parseHex(packed.c_str(), packed.size(), bytes, sizeof(bytes))) {
      HWarning("config/%s.cfg entry %s is unreadable", kChildModule, path);
      continue;
    }

    HAPChild child;
    child.index = bytes[0];
    child.mac = HAPMac::fromBytes(bytes + 1);
    child.capabilities = bytes[HAP_MAC_LEN + 1];
    child.deviceType = static_cast<HAPDeviceType>(bytes[HAP_MAC_LEN + 2]);
    child.reportIntervalSec = static_cast<uint16_t>(
        (static_cast<uint16_t>(bytes[HAP_MAC_LEN + 3]) << 8) |
        bytes[HAP_MAC_LEN + 4]);
    child.descriptorRev = static_cast<uint16_t>(
        (static_cast<uint16_t>(bytes[HAP_MAC_LEN + 5]) << 8) |
        bytes[HAP_MAC_LEN + 6]);

    // The key, in an entry of its own rather than packed with the rest: 16 bytes
    // is 32 hex characters, which is exactly what HVALUE_MAX_STRING_LEN is
    // already required to hold. Appending it to the record above would have
    // needed 58 and broken every application that sized its HValue to the
    // documented minimum.
    //
    // A file written before keys were kept simply has no `k` entry, and the
    // child comes back without one - unencrypted, which is what that link
    // actually is.
    char keyPath[4] = {'k', static_cast<char>('0' + i), '\0', '\0'};

    const etl::string<HVALUE_MAX_STRING_LEN> storedKey =
        HConfig::read(kChildModule, keyPath, HValue("")).asString();

    if (!storedKey.empty() &&
        !parseHex(storedKey.c_str(), storedKey.size(), child.linkKey,
                  HAP_KEY_LEN)) {
      HWarning("config/%s.cfg entry %s is unreadable", kChildModule, keyPath);
    }

    if (router.addChild(child)) {
      ++restored;
    }
  }

  return restored;
}

bool HAPStore::saveChildren(const HAPRouter& router) noexcept {
  // One record and one key per child, plus the count.
  HConfigEntry entries[2 * HAP_MAX_CHILDREN + 1];
  size_t count = 0;

  for (size_t i = 0; i < router.childCount(); ++i) {
    const HAPChild* child = router.childAt(i);
    if (child == nullptr) {
      continue;
    }

    uint8_t bytes[HAP_MAC_LEN + 7] = {};
    bytes[0] = child->index;
    std::memcpy(bytes + 1, child->mac.bytes, HAP_MAC_LEN);
    bytes[HAP_MAC_LEN + 1] = child->capabilities;
    bytes[HAP_MAC_LEN + 2] = static_cast<uint8_t>(child->deviceType);
    bytes[HAP_MAC_LEN + 3] = static_cast<uint8_t>(child->reportIntervalSec >> 8);
    bytes[HAP_MAC_LEN + 4] =
        static_cast<uint8_t>(child->reportIntervalSec & 0xFF);
    bytes[HAP_MAC_LEN + 5] = static_cast<uint8_t>(child->descriptorRev >> 8);
    bytes[HAP_MAC_LEN + 6] = static_cast<uint8_t>(child->descriptorRev & 0xFF);

    etl::string<HVALUE_MAX_STRING_LEN> packed;
    appendHex(packed, bytes, sizeof(bytes));

    char path[4] = {'c', static_cast<char>('0' + count), '\0', '\0'};
    entries[count] = HConfigEntry(
        path, HValue(etl::string_view(packed.data(), packed.size())));
    ++count;
  }

  // The keys, after every record, because HConfig::write() wants its entries
  // sorted and "c0".."c5" < "k0".."k5" < "n".
  const size_t children = count;

  for (size_t i = 0; i < children; ++i) {
    const HAPChild* child = router.childAt(i);
    if (child == nullptr || !child->hasLinkKey()) {
      continue;
    }

    etl::string<HVALUE_MAX_STRING_LEN> key;
    appendHex(key, child->linkKey, HAP_KEY_LEN);

    char keyPath[4] = {'k', static_cast<char>('0' + i), '\0', '\0'};
    entries[count] =
        HConfigEntry(keyPath, HValue(etl::string_view(key.data(), key.size())));
    ++count;
  }

  entries[count] = HConfigEntry(kKeyChildCount, HValue(static_cast<int>(children)));
  ++count;

  if (!HConfig::write(kChildModule,
                      etl::span<const HConfigEntry>(entries, count))) {
    HCritical("could not write config/%s.cfg", kChildModule);
    return false;
  }

  return true;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

void HAPStore::loadNames(HAPNode& node) noexcept {
  const etl::string<HVALUE_MAX_STRING_LEN> nodeName =
      HConfig::read(kNameModule, kKeyNodeName, HValue("")).asString();

  if (!nodeName.empty()) {
    node.setName(HAPName(nodeName.c_str()));
  }

  for (size_t i = 0; i < node.instanceCount(); ++i) {
    HAPInstance* instance = node.instanceAt(i);
    if (instance == nullptr) {
      continue;
    }

    char path[4] = {'i', static_cast<char>('0' + instance->instanceId()), '\0',
                    '\0'};

    const etl::string<HVALUE_MAX_STRING_LEN> stored =
        HConfig::read(kNameModule, path, HValue("")).asString();

    // An instance with no stored name keeps the one the firmware gave it, which
    // is what a device that has never been renamed should show.
    if (!stored.empty()) {
      instance->setName(HAPName(stored.c_str()));
    }
  }
}

bool HAPStore::saveNames(const HAPNode& node) noexcept {
  HConfigEntry entries[HAP_MAX_INSTANCES + 1];
  size_t count = 0;

  for (size_t i = 0; i < node.instanceCount(); ++i) {
    const HAPInstance* instance = node.instanceAt(i);
    if (instance == nullptr) {
      continue;
    }

    char path[4] = {'i', static_cast<char>('0' + instance->instanceId()), '\0',
                    '\0'};

    entries[count] = HConfigEntry(
        path, HValue(etl::string_view(instance->name().data(),
                                      instance->name().size())));
    ++count;
  }

  // "node" sorts after "i0".."i7".
  entries[count] = HConfigEntry(
      kKeyNodeName,
      HValue(etl::string_view(node.name().data(), node.name().size())));
  ++count;

  if (!HConfig::write(kNameModule,
                      etl::span<const HConfigEntry>(entries, count))) {
    HCritical("could not write config/%s.cfg - the rename is not persistent",
              kNameModule);
    return false;
  }

  return true;
}


// ---------------------------------------------------------------------------
// What a master was told about the tree below it
// ---------------------------------------------------------------------------

namespace {

/** path length + hops + mac + caps + type + interval. 32 hex characters. */
constexpr size_t kModelRecordSize = 1 + HAP_MAX_DEPTH + HAP_MAC_LEN + 4;

static_assert(kModelRecordSize * 2 <= HVALUE_MAX_STRING_LEN,
              "A model record is stored as hexadecimal text and must fit in an "
              "HValue; raise HVALUE_MAX_STRING_LEN or shorten the record");

}  // namespace

bool HAPStore::saveModel(const HAPModel& model) noexcept {
  HConfigEntry entries[HAP_MODEL_MAX_NODES + 1];
  size_t count = 0;

  for (size_t i = 0; i < model.size(); ++i) {
    const HAPRemoteNode* node = model.at(i);
    if (node == nullptr) {
      continue;
    }

    // A node nobody has named is not worth keeping: it was built from reports
    // alone, so writing it would preserve exactly the blank entry this file
    // exists to prevent.
    if (node->mac.isZero()) {
      continue;
    }

    uint8_t bytes[kModelRecordSize] = {};
    bytes[0] = node->path.length();
    std::memcpy(bytes + 1, node->path.bytes(), HAP_MAX_DEPTH);
    std::memcpy(bytes + 1 + HAP_MAX_DEPTH, node->mac.bytes, HAP_MAC_LEN);

    uint8_t* tail = bytes + 1 + HAP_MAX_DEPTH + HAP_MAC_LEN;
    tail[0] = node->capabilities;
    tail[1] = static_cast<uint8_t>(node->deviceType);
    tail[2] = static_cast<uint8_t>(node->reportIntervalSec >> 8);
    tail[3] = static_cast<uint8_t>(node->reportIntervalSec & 0xFF);

    etl::string<HVALUE_MAX_STRING_LEN> packed;
    appendHex(packed, bytes, sizeof(bytes));

    char path[4] = {'m', static_cast<char>('0' + count), '\0', '\0'};
    entries[count] = HConfigEntry(
        path, HValue(etl::string_view(packed.data(), packed.size())));
    ++count;
  }

  // "n" sorts after "m0".."m7", which is what HConfig::write() requires.
  entries[count] = HConfigEntry(kKeyNodeCount, HValue(static_cast<int>(count)));
  ++count;

  if (!HConfig::write(kModelModule,
                      etl::span<const HConfigEntry>(entries, count))) {
    HCritical("could not write config/%s.cfg", kModelModule);
    return false;
  }

  return true;
}

size_t HAPStore::loadModel(HAPModel& model) noexcept {
  const int stored = HConfig::read(kModelModule, kKeyNodeCount, HValue(0)).asInt();
  if (stored <= 0) {
    return 0;
  }

  size_t restored = 0;

  for (int i = 0; i < stored && i < static_cast<int>(HAP_MODEL_MAX_NODES); ++i) {
    char path[4] = {'m', static_cast<char>('0' + i), '\0', '\0'};

    const etl::string<HVALUE_MAX_STRING_LEN> packed =
        HConfig::read(kModelModule, path, HValue("")).asString();

    uint8_t bytes[kModelRecordSize] = {};
    if (!parseHex(packed.c_str(), packed.size(), bytes, sizeof(bytes))) {
      HWarning("config/%s.cfg entry %s is unreadable", kModelModule, path);
      continue;
    }

    const HAPPath nodePath = HAPPath::fromBytes(bytes + 1, bytes[0]);
    if (nodePath.isEmpty()) {
      // An empty path addresses the node holding the model, which is not
      // something the model may contain.
      continue;
    }

    const uint8_t* tail = bytes + 1 + HAP_MAX_DEPTH + HAP_MAC_LEN;
    const uint16_t interval =
        static_cast<uint16_t>((static_cast<uint16_t>(tail[2]) << 8) | tail[3]);

    if (model.restore(nodePath, HAPMac::fromBytes(bytes + 1 + HAP_MAX_DEPTH),
                      static_cast<HAPDeviceType>(tail[1]), tail[0],
                      interval) != nullptr) {
      ++restored;
    }
  }

  return restored;
}
bool HAPStore::clearAll() noexcept {
  clearBind();

  const bool children = HConfig::remove(kChildModule);
  const bool names = HConfig::remove(kNameModule);
  const bool tree = HConfig::remove(kModelModule);

  return children && names && tree;
}
