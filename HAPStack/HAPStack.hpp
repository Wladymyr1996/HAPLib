#pragma once

#include <HAPBinder/HAPBinder.hpp>
#include <HAPLinks/HAPLinks.hpp>
#include <HAPListenWindow/HAPListenWindow.hpp>
#include <HAPModel/HAPModel.hpp>
#include <HAPQueue/HAPQueue.hpp>
#include <HAPReporter/HAPReporter.hpp>
#include <HAPStore/HAPStore.hpp>

/**
 * @brief The whole protocol as one object: what an application actually holds.
 *
 * Everything else in HAPLib is a mechanism with no opinion - a router that says
 * where a frame goes, a binder that runs a handshake, a model that caches what
 * it hears. This is the twenty lines that connect them, and they are the same
 * twenty lines on every device in the ecosystem, which is what makes them
 * library work rather than each application's.
 *
 * @code
 *   HAPEspNow radio;
 *   HAPNode node;
 *   HAPStack stack(radio, node);
 *
 *   node.begin(HAPDeviceType::Sensor, HAPCaps::BatteryPowered, "Bedroom");
 *   node.addInstance(HAPClassId::Thermometer, "Temp");
 *   stack.begin(1);
 *
 *   // every HLIB_TICK_MS
 *   stack.update();
 * @endcode
 *
 * ## What it decides, so an application does not have to
 * - a frame from a stranger goes to the binder, and everything else to the
 *   router - in that order, because bind traffic comes from an address that is
 *   by definition neither parent nor child;
 * - a frame for a **sleeping** child is queued rather than sent, and delivered
 *   the moment that child speaks;
 * - a frame that cannot be forwarded produces a `RouteError` naming the hop,
 *   rather than a silence the far end has to time out;
 * - a report that passes through feeds any link watching it **and** carries on
 *   upward - a tap, not a diversion;
 * - a node with something to say says it, on its own schedule.
 *
 * ## What it refuses to decide
 * When to sleep, what to draw, which REST route to publish, what a button
 * means. It offers maySleep() and a handful of hooks; the device does the rest.
 *
 * ## The model is optional
 * A root wants HAPModel and a leaf must not pay for it - close to a kilobyte per
 * remote node. So the stack holds a pointer rather than an instance, and a
 * device that needs one supplies it with setModel().
 */
class HAPStack {
 public:
  /**
   * @param transport Not owned; must outlive this.
   * @param node This device's own model, which the application fills in before
   *        begin() and which the stack then answers questions about.
   */
  HAPStack(HAPITransport& transport, HAPNode& node) noexcept;

  /**
   * @brief Brings up the radio and restores whatever this node already knew.
   *
   * Loads the bind, the children and the names from HAPStore, and installs the
   * peers that go with them - so a node that was bound before the reboot is
   * bound after it, with no handshake and no user involved.
   */
  bool begin(uint8_t channel) noexcept;

  /** @brief Drives everything. Call every HLIB_TICK_MS. */
  void update() noexcept;

  // -- the pieces, for an application that needs to look inside ------------

  HAPNode& node() noexcept;
  HAPRouter& router() noexcept;
  HAPBinder& binder() noexcept;
  HAPReporter& reporter() noexcept;
  HAPLinks& links() noexcept;
  HAPQueue& queue() noexcept;
  const HAPListenWindow& listenWindow() const noexcept;

  /** @brief Gives this node somewhere to keep a picture of the network. */
  void setModel(HAPModel& model) noexcept;
  HAPModel* model() noexcept;

  bool isRoot() const noexcept;
  bool isBound() const noexcept;

  /**
   * @brief True when nothing is waiting for the radio.
   *
   * False while a listen window is open or a bind is in progress. A device asks
   * this before sleeping; what it does about the answer is its own business.
   */
  bool maySleep() const noexcept;

  // -- what a user does ----------------------------------------------------

  /** @brief Opens a window to adopt somebody. The master half of a bind. */
  bool bindAsMaster() noexcept;

  /** @brief Announces, to be adopted. The slave half. */
  bool bindAsSlave() noexcept;

  /**
   * @brief One Bind button, for a device that only has one role to play.
   *
   * A gateway can only adopt and a sensor can only be adopted, so for those
   * this is unambiguous. A **controller is both**, and the guess it makes -
   * announce while unbound, adopt once it has a parent - is only a guess: a
   * controller being given its second child wants the other one.
   *
   * So a device with two roles should offer the user two actions and call
   * bindAsMaster() or bindAsSlave() directly. This exists for the devices that
   * do not have that problem.
   */
  bool bind() noexcept;

  /** @brief Forgets the parent, the children, the names and the links. */
  bool factoryReset() noexcept;

  // -- originating ---------------------------------------------------------

  /** @brief Sends a report now, whether or not one was due. */
  bool sendReport() noexcept;

  bool ping(const HAPPath& path) noexcept;
  bool requestDescribe(const HAPPath& path, uint8_t fromPage = 0) noexcept;
  bool requestRead(const HAPPath& path, const HAPReadRequest& request) noexcept;
  bool requestWrite(const HAPPath& path, const HAPWriteRequest& request) noexcept;

  /**
   * @brief Changes how often a node speaks, and how far a value must move first.
   *
   * The other half of HAPReporter::setPolicy(): that one answers the message,
   * and this one is what sends it. Without both, the deadband a node carries
   * could only ever be the one its own firmware chose - which makes the setting
   * a compile-time constant wearing a protocol message's clothes.
   *
   * The answer may not be what was asked: a battery node clamps an interval to
   * its wake period and says so in HAPSetPolicyResponse.
   */
  bool requestSetPolicy(const HAPPath& path,
                        const HAPSetPolicyRequest& request) noexcept;

  bool requestSetName(const HAPPath& path,
                      const HAPSetNameRequest& request) noexcept;
  bool requestSetLink(const HAPPath& path, const HAPLinkSpec& spec) noexcept;

  // -- hooks ---------------------------------------------------------------

  /** @brief A command arrived for one of this node's own in ports. */
  using WriteHook = etl::delegate<void(uint8_t classId, uint8_t instanceId,
                                       uint8_t portId, const HValue& value)>;
  void onWrite(WriteHook hook) noexcept;

  /** @brief A report arrived from below - delivered here, or passing through. */
  using ValuesHook =
      etl::delegate<void(const HAPPath& from, const HAPReport& report)>;
  void onValues(ValuesHook hook) noexcept;

  /** @brief This node gained a child, or gained a parent. */
  void onChildBound(HAPBinder::ChildBoundHook hook) noexcept;
  void onBound(HAPBinder::BoundHook hook) noexcept;

  /** Frames this node has sent and received, for a log line. */
  uint32_t sentCount() const noexcept;
  uint32_t receivedCount() const noexcept;

 private:
  void onFrame(const HAPMac& from, const uint8_t* data, size_t size) noexcept;
  void onSendResult(const HAPMac& to, bool delivered) noexcept;

  void deliverLocally(const HAPFrame& frame) noexcept;
  void handleRequest(const HAPFrame& frame) noexcept;
  void handleUpstreamNews(const HAPFrame& frame) noexcept;

  /** Sends a frame to a child, or holds it if that child is asleep. */
  bool forwardToChild(HAPFrame& frame, uint8_t childIndex) noexcept;

  /** Anything waiting for this child goes out now, in its listen window. */
  void flushQueue(uint8_t childIndex) noexcept;

  bool sendUpstream(HAPMessage type, uint8_t flags, const uint8_t* payload,
                    size_t size) noexcept;
  bool sendDownstream(const HAPPath& path, HAPMessage type, uint8_t flags,
                      const uint8_t* payload, size_t size,
                      uint16_t seq = 0) noexcept;
  bool respond(const HAPFrame& to, HAPMessage type, const uint8_t* payload,
               size_t size) noexcept;
  bool transmit(const HAPMac& mac, const HAPFrame& frame) noexcept;

  void sendRouteError(const HAPFrame& frame, uint8_t failedHop,
                      HAPResult reason) noexcept;

  void childBound(const HAPChild& child) noexcept;

  /**
   * @brief Tells whoever is above about one child of this node.
   *
   * Climbs as ChildAttached when there is a parent, and goes straight into the
   * model when this node IS the root. When there is neither - a controller that
   * has adopted somebody before being adopted itself - it does nothing and says
   * so, and bound() repeats it the moment a parent arrives.
   */
  bool announceChild(const HAPChild& child) noexcept;

  void bound(const HAPMac& parent, uint8_t index, const uint8_t* linkKey,
             uint8_t channel) noexcept;
  void deliverLink(const HAPPortRef& destination, const HValue& value) noexcept;

  /** @brief Writes the model down, when something about WHO is out there moved. */
  void rememberModel() noexcept;

  HAPITransport& transport_;
  HAPNode& node_;

  HAPRouter router_;
  HAPBinder binder_;
  HAPReporter reporter_;
  HAPLinks links_;
  HAPQueue queue_;
  HAPListenWindow window_;
  HAPModel* model_ = nullptr;

  HAPBindState bind_;
  uint16_t seq_ = 0;
  uint32_t sent_ = 0;
  uint32_t received_ = 0;
  bool started_ = false;

  WriteHook onWrite_;
  ValuesHook onValues_;
  HAPBinder::ChildBoundHook onChildBound_;
  HAPBinder::BoundHook onBound_;
};
