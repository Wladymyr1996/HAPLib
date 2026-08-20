#pragma once

/**
 * @file HAP.h
 * @brief HatynkaAirProtocol: the root header every HAPLib module includes.
 *
 * The protocol itself is specified in Docs/Protocol.md, and this header is that
 * document's vocabulary in code: the wire constants, the message codes, the
 * result codes and the class identifiers. When the two disagree the document is
 * right and this file is a bug - Tests/ exists to keep them honest by encoding
 * the document's own worked examples byte for byte.
 *
 * Built on HCoreLib, and to the same rules: no heap after start-up, no
 * exceptions, ETL containers, and one platform backend compiled in rather than
 * switched at runtime.
 */

#include <HCoreLib.h>
#include <HValue/HValue.hpp>

#include <cstdint>
#include <new>

#include <etl/string.h>

// Optional application overrides for the limits below. Unlike HCoreLib.h -
// which demands its HCoreLibConfig.h and fails the build without one - HAPLib
// runs on its defaults perfectly well: nothing here is a board fact that only the
// application can know, so an application supplies this header only when it
// disagrees with a number.
#if defined(__has_include)
#if __has_include(<HAPConfig.h>)
#include <HAPConfig.h>
#endif
#endif

// ---------------------------------------------------------------------------
// The library. NOT the protocol.
// ---------------------------------------------------------------------------
//
// HAPLIB_VERSION_* is the version of THIS SOURCE TREE, and it is not HAP_VERSION
// below. The two answer different questions and move on different schedules:
// HAP_VERSION is a byte on the wire that every node must agree on, so it changes
// only when the frame format does; this is the revision of the implementation,
// which changes whenever anything here does. A build can gain three of these
// without the byte on the wire moving once.
//
// The point of spelling them out as macros is that HValue and HTimer sit in this
// library's PUBLIC interface, so an application that pairs the wrong revisions
// fails to compile rather than misbehaves. HAPLIB_VERSION_AT_LEAST lets a
// consumer say which revision it needs and get one clear line about it instead
// of a page of template errors.

#define HAPLIB_VERSION_MAJOR 1
#define HAPLIB_VERSION_MINOR 0
#define HAPLIB_VERSION_PATCH 0

/** Two levels, because the inner one must see a NUMBER and not the macro's name. */
#define HAPLIB_STRINGIFY_(x) #x
#define HAPLIB_STRINGIFY(x) HAPLIB_STRINGIFY_(x)

/**
 * This build's version as text: `"1.0-0"`.
 *
 * Assembled from the three numbers above rather than written out, so there is
 * exactly one place to edit and the text cannot drift from what a `#if` sees.
 */
#define HAPLIB_VERSION_STRING                    \
  HAPLIB_STRINGIFY(HAPLIB_VERSION_MAJOR) "."     \
  HAPLIB_STRINGIFY(HAPLIB_VERSION_MINOR) "-"     \
  HAPLIB_STRINGIFY(HAPLIB_VERSION_PATCH)

/**
 * @brief True when this build is @p major.@p minor-@p patch or newer.
 *
 * For `#if`, so a consumer can guard against a HAPLib older than the one it
 * needs:
 * @code
 *   #if !HAPLIB_VERSION_AT_LEAST(1, 0, 0)
 *   #error "This application needs HAPLib 1.0-0 or newer."
 *   #endif
 * @endcode
 */
#define HAPLIB_VERSION_AT_LEAST(major, minor, patch)                          \
  ((HAPLIB_VERSION_MAJOR) > (major) ||                                        \
   ((HAPLIB_VERSION_MAJOR) == (major) &&                                      \
    ((HAPLIB_VERSION_MINOR) > (minor) ||                                      \
     ((HAPLIB_VERSION_MINOR) == (minor) &&                                    \
      (HAPLIB_VERSION_PATCH) >= (patch)))))

/**
 * @brief This build's version as text - `"1.0-0"`. Never nullptr.
 *
 * The same characters HAPLIB_VERSION_STRING holds, reachable from code that has
 * a pointer rather than a preprocessor: a banner line at start-up, a
 * DescribeResponse, a log header. constexpr, so asking costs nothing at runtime
 * and it may be used wherever a compile-time constant is required.
 */
constexpr const char* HAPLibVersion() noexcept {
  return HAPLIB_VERSION_STRING;
}

// ---------------------------------------------------------------------------
// The protocol. NOT tunable.
// ---------------------------------------------------------------------------
//
// These are the wire format. A device that changes one of them is not running a
// differently-configured HAP, it is running a different protocol that will
// silently misparse its neighbours - so they are plain defines with no #ifndef
// escape hatch, and a static_assert below nails down the arithmetic between
// them.

/** Protocol version carried in every frame's header. */
#define HAP_VERSION 1

/** `'H'`, `'A'` - the first two bytes of every frame. */
#define HAP_MAGIC_0 0x48
#define HAP_MAGIC_1 0x41

/** Fixed header size: magic, version, type, flags, seq, path lengths, two paths. */
#define HAP_HEADER_SIZE 18

/**
 * The largest frame that may be transmitted.
 *
 * ESP-NOW v2 carries 1470 bytes, but a v1 device can only receive a v2 packet if
 * it is <= 250 - so designing to 250 keeps every node interoperable whatever IDF
 * it was built against. Nothing in the protocol needs more; longer responses
 * paginate instead (see HAPFlags::More).
 */
#define HAP_MAX_FRAME_SIZE 250

/** What is left of a frame once the header has taken its share: 232 bytes. */
#define HAP_MAX_PAYLOAD_SIZE (HAP_MAX_FRAME_SIZE - HAP_HEADER_SIZE)

/** Hops in the deepest possible address. A path fits in this many bytes. */
#define HAP_MAX_DEPTH 5

/** Longest name, in BYTES - "Кімната" is 14 of them and 7 characters. */
#define HAP_MAX_NAME_LEN 31

/** Length of an ESP-NOW local master key. */
#define HAP_KEY_LEN 16

/** Length of a MAC address - a node's permanent identity. */
#define HAP_MAC_LEN 6

/** Widest text form of a path: "254.254.254.254.254". */
#define HAP_PATH_TEXT_LEN 19

static_assert(HAP_HEADER_SIZE + HAP_MAX_PAYLOAD_SIZE == HAP_MAX_FRAME_SIZE,
              "The payload budget must be exactly what the header leaves over.");

// ---------------------------------------------------------------------------
// This build's limits. Tunable, within reason.
// ---------------------------------------------------------------------------

/**
 * Children one node may have.
 *
 * Six is not a protocol decision - it is ESP-NOW's ceiling on ENCRYPTED peers.
 * Raising it means links beyond the sixth cannot be encrypted, which is a
 * security decision rather than a memory one, so raise it only with that in
 * mind.
 */
#ifndef HAP_MAX_CHILDREN
#define HAP_MAX_CHILDREN 6
#endif

/** Class instances one node may expose. Costs RAM per node, not per network. */
#ifndef HAP_MAX_INSTANCES
#define HAP_MAX_INSTANCES 8
#endif

/** Links one node may hold. Only the lowest common ancestor of a pair holds one. */
#ifndef HAP_MAX_LINKS
#define HAP_MAX_LINKS 16
#endif

/**
 * Values one Report or ReadResponse may carry.
 *
 * Not the same as the instance count: an instance with several out ports
 * contributes one entry per port. Eight covers a node reporting every port it
 * has; a node with more than that pages its reports.
 */
#ifndef HAP_MAX_REPORT_ENTRIES
#define HAP_MAX_REPORT_ENTRIES 8
#endif

/** How long a master listens for a BindAnnounce after the user asks it to. */
#ifndef HAP_BIND_WINDOW_MS
#define HAP_BIND_WINDOW_MS 60000
#endif

/**
 * How long a node keeps its radio on after sending an upstream frame.
 *
 * This is what makes a sleeping node reachable at all: its parent answers into
 * this window, and anything queued for it arrives here or waits another report
 * cycle. Every millisecond is paid for out of a battery at radio-receive
 * current, so it is long enough for one round trip and not a millisecond more.
 */
#ifndef HAP_LISTEN_WINDOW_MS
#define HAP_LISTEN_WINDOW_MS 120
#endif

/** Attempts a node makes at one hop before calling the link down. */
#ifndef HAP_HOP_RETRIES
#define HAP_HOP_RETRIES 3
#endif

/** Missed report intervals before a master calls a child offline. */
#ifndef HAP_OFFLINE_INTERVALS
#define HAP_OFFLINE_INTERVALS 3
#endif

/**
 * The channel a network runs on until something says otherwise.
 *
 * ESP-NOW has no channel negotiation: every node has to be on the same one or
 * they are simply deaf to each other. A master brings its radio up here, an
 * unbound slave starts here and sweeps if nobody answers, and the channel a
 * slave is finally told to use arrives in BindAccept and is stored with the
 * bind.
 *
 * Change it only for a whole network at once. One node with a different default
 * is a node that binds slowly - it has to sweep to find everybody else - and a
 * whole network with a different one costs nothing.
 */
#ifndef HAP_DEFAULT_CHANNEL
#define HAP_DEFAULT_CHANNEL 1
#endif

// ---------------------------------------------------------------------------
// Vocabulary
// ---------------------------------------------------------------------------

/** A user-visible name: a node's, or one instance's. Never a port's. */
using HAPName = etl::string<HAP_MAX_NAME_LEN>;

/** Message type, byte 3 of every frame. See Docs/Protocol.md section 4. */
enum class HAPMessage : uint8_t {
  BindAnnounce = 0x01,
  BindAccept = 0x02,
  BindConfirm = 0x03,
  ChildAttached = 0x04,

  DescribeRequest = 0x10,
  DescribeResponse = 0x11,
  Report = 0x12,
  ReadRequest = 0x13,
  ReadResponse = 0x14,
  WriteRequest = 0x15,
  WriteResponse = 0x16,
  SetPolicyRequest = 0x17,
  SetPolicyResponse = 0x18,
  SetNameRequest = 0x19,
  SetNameResponse = 0x1A,

  Ping = 0x20,
  Pong = 0x21,

  Ack = 0x30,
  Nack = 0x31,
  RouteError = 0x40,

  SetLinkRequest = 0x50,
  SetLinkResponse = 0x51,
  ClearLinkRequest = 0x52,
  ClearLinkResponse = 0x53,
  ListLinksRequest = 0x54,
  ListLinksResponse = 0x55
};

/**
 * Header flag bits. A plain namespace rather than an enum class because these
 * are combined, tested and masked - all of which an enum class makes tedious for
 * no safety worth having in a byte that came off a radio.
 */
namespace HAPFlags {
constexpr uint8_t None = 0x00;
/** The originator wants an end-to-end acknowledgement. */
constexpr uint8_t AckRequested = 0x01;
/** Route toward the root; destPath is unused. */
constexpr uint8_t Upstream = 0x02;
/** Another frame of this response follows. */
constexpr uint8_t More = 0x04;
/** Delivered out of a parent's queue rather than sent live. */
constexpr uint8_t Queued = 0x08;
/** Written as zero, ignored on receipt - this is how version 1 grows. */
constexpr uint8_t Reserved = 0xF0;
}  // namespace HAPFlags

/** What a node says about itself when it announces. */
enum class HAPDeviceType : uint8_t {
  Sensor = 0x00,      ///< Slave only, and always a leaf.
  Controller = 0x01,  ///< Slave to its parent, master to its children.
  Gateway = 0x02      ///< Master only; never binds upward.
};

/** Capability bits, alongside the device type in BindAnnounce. */
namespace HAPCaps {
constexpr uint8_t None = 0x00;
/** Asleep almost always: unreachable except in its listen window. */
constexpr uint8_t BatteryPowered = 0x01;
/** May take children of its own. */
constexpr uint8_t CanBeMaster = 0x02;
}  // namespace HAPCaps

/** Per-instance bits in a descriptor. */
namespace HAPInstanceFlags {
constexpr uint8_t None = 0x00;
constexpr uint8_t Writable = 0x01;
constexpr uint8_t EventDriven = 0x02;
}  // namespace HAPInstanceFlags

/**
 * The five types a VALUE carries - deliberately HValue's five, so nothing in
 * the ecosystem needs a second representation of a reading.
 */
enum class HAPValueType : uint8_t {
  Null = 0x00,
  Bool = 0x01,
  Int = 0x02,
  Float = 0x03,
  String = 0x04
};

/**
 * What a port MEANS, as opposed to how it is encoded.
 *
 * A hygrometer's 44.0 and a thermometer's 21.5 are both Float; wiring one into
 * the other is nonsense that a primitive type check cannot catch. Links match on
 * this instead - which is only sufficient because units are canonical on the
 * wire, so there is no conversion left to get wrong.
 */
enum class HAPKind : uint8_t {
  Temperature = 0x01,  ///< Float, degrees Celsius.
  Humidity = 0x02,     ///< Float, % relative humidity.
  Pressure = 0x03,     ///< Float, pascals.
  OnOff = 0x04,        ///< Bool.
  Ratio = 0x05,        ///< Float, 0.0 to 1.0.
  Count = 0x06,        ///< Int.
  Voltage = 0x07,      ///< Float, volts.
  Text = 0x08          ///< String.
};

/** Standard classes. See Docs/Classes/ for what each one's ports mean. */
enum class HAPClassId : uint8_t {
  Thermometer = 0x01,
  Hygrometer = 0x02,
  Barometer = 0x03,

  Switch = 0x10,
  Lamp = 0x11,
  Door = 0x12,

  BatteryState = 0x20,

  Regulator = 0x30
};

/** Result of a request, and reason of a refusal. Docs/Protocol.md section 7. */
enum class HAPResult : uint8_t {
  Ok = 0x00,
  NoSuchChild = 0x01,
  ChildUnreachable = 0x02,
  NoSuchClass = 0x03,
  NotWritable = 0x04,
  BadValue = 0x05,
  BadRequest = 0x06,
  NotBound = 0x07,
  Busy = 0x08,
  Unsupported = 0x09,
  NoRoom = 0x0A,
  NoSuchPort = 0x0B,
  TypeMismatch = 0x0C,
  InputBusy = 0x0D,
  NoLinkSlot = 0x0E
};

/** Human-readable form of a result, for logs. Never nullptr. */
const char* HAPResultToString(HAPResult result) noexcept;

/**
 * @brief Replaces an HValue's contents AND its type.
 *
 * The one correct way to put a decoded value somewhere, and the only reason this
 * function exists: HValue fixes its type at construction and its `operator=`
 * COERCES rather than replaces, so `stored = HValue(21.5f)` on a
 * default-constructed HValue leaves it Null. That is deliberate in HCoreLib -
 * it is what makes a configuration entry keep the type its file declared - and
 * it is exactly wrong for a value that arrived off a radio already carrying
 * its own.
 *
 * Destroying and re-constructing in place is legal, cheap, and the whole of the
 * fix. Every structure in HAPLib that holds an HValue goes through here.
 */
inline void HAPAssign(HValue& target, const HValue& source) noexcept {
  if (&target == &source) {
    return;
  }

  target.~HValue();
  new (&target) HValue(source);
}
