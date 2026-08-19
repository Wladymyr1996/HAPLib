#define HLOG_MODULE_NAME "HapStack"

#include <HAPStack/HAPStack.hpp>

#include <HLog/HLog.hpp>

HAPStack::HAPStack(HAPITransport& transport, HAPNode& node) noexcept
    : transport_(transport),
      node_(node),
      binder_(transport, router_, node),
      reporter_(node),
      links_(node) {}

// ---------------------------------------------------------------------------
// Start-up
// ---------------------------------------------------------------------------

bool HAPStack::begin(uint8_t channel) noexcept {
  HAPStore::init();

  // What this node already knew. A node bound before the reboot is bound after
  // it, with no handshake and nobody pressing anything.
  uint8_t startChannel = channel;

  if (HAPStore::loadBind(bind_)) {
    router_.setParent(bind_.parent, bind_.indexAtParent);

    if (bind_.channel != 0) {
      startChannel = bind_.channel;
    }
  }

  if (!transport_.begin(startChannel)) {
    return false;
  }

  transport_.onReceive(
      HAPITransport::Receiver::create<HAPStack, &HAPStack::onFrame>(*this));
  transport_.onSendResult(
      HAPITransport::SendResult::create<HAPStack, &HAPStack::onSendResult>(
          *this));

  binder_.onChildBound(
      HAPBinder::ChildBoundHook::create<HAPStack, &HAPStack::childBound>(*this));
  binder_.onBound(
      HAPBinder::BoundHook::create<HAPStack, &HAPStack::bound>(*this));
  links_.onDeliver(
      HAPLinks::DeliveryHook::create<HAPStack, &HAPStack::deliverLink>(*this));

  // The peer has to exist before anything can be sent to it, and a restored
  // bind has no handshake to create one.
  if (bind_.bound) {
    transport_.addPeer(bind_.parent, bind_.linkKey);
  }

  // What this node was TOLD about the tree below it, before it restarted.
  // Restored before the children below, so that seeding them refreshes entries
  // rather than creating rival ones - and so that a device which moved while
  // this node was off is recognised by its MAC rather than appearing twice.
  if (model_ != nullptr) {
    HAPStore::loadModel(*model_);
  }

  HAPStore::loadChildren(router_);
  HAPStore::loadNames(node_);

  for (size_t i = 0; i < router_.childCount(); ++i) {
    const HAPChild* child = router_.childAt(i);
    if (child != nullptr) {
      // Restored children are peers again, WITH THE KEY THEY WERE BOUND WITH.
      //
      // This used to add them unencrypted, on the theory that a link key belongs
      // to the child's own copy of the bind and would be re-established when the
      // child next bound. It never binds again: one parent, for life. So a
      // master that rebooted came back holding every child as a plain peer while
      // each child went on transmitting encrypted, and the radio dropped the lot
      // in both directions - silently, below the protocol. The network was dead
      // and every table in it looked perfect.
      //
      // nullptr for a child restored from a file written before keys were kept,
      // which is honest: unencrypted is what that link actually is.
      transport_.addPeer(child->mac,
                         child->hasLinkKey() ? child->linkKey : nullptr);

      // And the model is told what this node already knows about that child.
      //
      // A REPORT CARRIES NO MAC, NO CAPABILITIES AND NO INTERVAL - only
      // ChildAttached does, and that happened before the reboot. So a master
      // that came back and waited to be told would build a model entry out of a
      // report alone: a zero address, no capabilities, and an interval of zero.
      // An interval of zero means "promises nothing", and a node that promised
      // nothing is never called offline - so a rebooted master could watch a
      // child die and never say so.
      //
      // Everything the notice carried is in the child table, which survives. It
      // is the same information by the same route; only the trigger differs.
      if (model_ != nullptr) {
        HAPChildAttached restored;
        restored.childIndex = child->index;
        restored.deviceType = child->deviceType;
        restored.capabilities = child->capabilities;
        restored.reportIntervalSec = child->reportIntervalSec;
        restored.descriptorRev = child->descriptorRev;

        for (size_t byte = 0; byte < HAP_MAC_LEN; ++byte) {
          restored.mac[byte] = child->mac.bytes[byte];
        }

        model_->noteChildAttached(HAPPath(), restored);
      }
    }
  }

  reporter_.begin(node_.reportIntervalSec(), HValue());

  if (node_.isBatteryPowered() && node_.reportIntervalSec() != 0) {
    // A wake period is a floor on reporting whatever a master asks for.
    reporter_.setMinimumIntervalSec(node_.reportIntervalSec());
  }

  started_ = true;

  HInfo("%s on channel %u, %u children, %s", node_.name().c_str(), startChannel,
        static_cast<unsigned>(router_.childCount()),
        router_.isRoot() ? "root" : "bound");

  return true;
}

// ---------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------

void HAPStack::update() noexcept {
  if (!started_) {
    return;
  }

  binder_.update();

  // A node with something to say says it, on its own schedule. Only when bound:
  // an unbound node has nobody to say it to.
  if (router_.hasParent() && reporter_.isDue()) {
    sendReport();
  }

  if (model_ != nullptr) {
    model_->sweepOffline();
  }
}

bool HAPStack::maySleep() const noexcept {
  return window_.maySleep() && binder_.state() != HAPBinder::State::Announcing &&
         binder_.state() != HAPBinder::State::Accepting;
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

void HAPStack::onFrame(const HAPMac& from, const uint8_t* data,
                       size_t size) noexcept {
  ++received_;

  HAPFrame frame;
  const HAPFrameError error = HAPFrame::decode(data, size, frame);

  if (error != HAPFrameError::None) {
    HWarning("dropped a frame from %s: %s", from.toString().c_str(),
             HAPFrameErrorToString(error));
    return;
  }

  // Anything arriving keeps a listen window alive: a frame inside it means the
  // conversation is not over, and closing now would cost a whole report cycle.
  window_.extend();

  // The binder first. Bind traffic comes from an address that is neither parent
  // nor child, which is exactly what the router refuses.
  if (binder_.handle(from, frame)) {
    return;
  }

  const HAPChild* sender = router_.childByMac(from);
  const HAPRoute route = router_.route(frame, from);

  switch (route.action) {
    case HAPRouteAction::Deliver:
      deliverLocally(frame);
      break;

    case HAPRouteAction::ToParent: {
      // A report passing through feeds anything wired to it - and carries on.
      // The source path has already been prepended, so it is the downward path
      // from here, which is what a link stores.
      if (frame.message() == HAPMessage::Report) {
        HAPReader reader(frame.payload, frame.payloadSize);
        HAPReport report;

        if (report.decode(reader)) {
          links_.deliver(frame.src, report);

          if (onValues_.is_valid()) {
            onValues_(frame.src, report);
          }
        }
      }

      transmit(router_.parentMac(), frame);
      break;
    }

    case HAPRouteAction::ToChild:
      forwardToChild(frame, route.childIndex);
      break;

    case HAPRouteAction::Drop:
      if (route.reason == HAPResult::NoSuchChild ||
          route.reason == HAPResult::ChildUnreachable) {
        // A diagnosis rather than a timeout: the far end learns WHICH hop
        // failed, which is the difference between a fault and a mystery.
        sendRouteError(frame, route.childIndex, route.reason);
      } else {
        HWarning("dropped %s from %s: %s",
                 frame.src.toString().c_str(), from.toString().c_str(),
                 HAPResultToString(route.reason));
      }
      break;
  }

  // Whatever it was, the child is awake right now - which for a sleeping one is
  // the only moment anything held for it can be delivered.
  if (sender != nullptr && sender->isBatteryPowered()) {
    flushQueue(sender->index);
  }
}

void HAPStack::onSendResult(const HAPMac& to, bool delivered) noexcept {
  binder_.notifySent(to, delivered);
}

// ---------------------------------------------------------------------------
// Frames for this node
// ---------------------------------------------------------------------------

void HAPStack::deliverLocally(const HAPFrame& frame) noexcept {
  switch (frame.message()) {
    // Things a master hears from below.
    case HAPMessage::Report:
    case HAPMessage::ReadResponse:
    case HAPMessage::DescribeResponse:
    case HAPMessage::Pong:
    case HAPMessage::ChildAttached:
    case HAPMessage::RouteError:
    case HAPMessage::Nack:
    case HAPMessage::WriteResponse:
    case HAPMessage::SetNameResponse:
    // Answers to things this node asked for. They belong here rather than in
    // handleRequest() for a reason worth spelling out: the default branch there
    // is a Nack, so a response landing in it would be REFUSED - a master would
    // answer its own child's "link installed" with "unsupported", and on a
    // middle node that refusal climbs to the root as news about nothing.
    case HAPMessage::SetPolicyResponse:
    case HAPMessage::SetLinkResponse:
    case HAPMessage::ClearLinkResponse:
    case HAPMessage::ListLinksResponse:
    case HAPMessage::Ack:
      handleUpstreamNews(frame);
      break;

    default:
      handleRequest(frame);
      break;
  }
}

void HAPStack::handleUpstreamNews(const HAPFrame& frame) noexcept {
  HAPReader reader(frame.payload, frame.payloadSize);

  switch (frame.message()) {
    case HAPMessage::Report:
    case HAPMessage::ReadResponse: {
      HAPReport report;
      if (!report.decode(reader)) {
        return;
      }

      if (model_ != nullptr) {
        model_->noteReport(frame.src, report);
      }

      links_.deliver(frame.src, report);

      if (onValues_.is_valid()) {
        onValues_(frame.src, report);
      }
      break;
    }

    case HAPMessage::DescribeResponse: {
      HAPDescribeResponse response;
      if (response.decode(reader) && model_ != nullptr) {
        model_->noteDescribe(frame.src, response);
      }
      break;
    }

    case HAPMessage::Pong: {
      HAPPong pong;
      if (pong.decode(reader) && model_ != nullptr) {
        model_->notePong(frame.src, pong);
      }
      break;
    }

    case HAPMessage::SetPolicyResponse: {
      // What the node actually TOOK, which is not always what was asked - and
      // which the model has to adopt as the interval it measures a silence
      // against. Without this, tightening a node's reporting is a way to have it
      // declared offline for obeying: the master goes on expecting the interval
      // announced at bind time and calls the node dead three of those later.
      const HAPSetPolicyResponse response = HAPSetPolicyResponse::decode(reader);

      if (reader.ok() && model_ != nullptr) {
        model_->notePolicy(frame.src, response);
      }
      break;
    }

    case HAPMessage::ChildAttached: {
      HAPChildAttached attached;
      if (attached.decode(reader) && model_ != nullptr) {
        model_->noteChildAttached(frame.src, attached);

        // Identity changed - a node appeared, or one moved. This is the only
        // message that can say so, and it is never repeated.
        rememberModel();
      }
      break;
    }

    case HAPMessage::RouteError: {
      HAPRouteError routeError;
      if (routeError.decode(reader)) {
        HWarning("%s could not reach hop %u: %s", frame.src.toString().c_str(),
                 routeError.failedHop, HAPResultToString(routeError.reason));
      }
      break;
    }

    default:
      // A response this build does not act on. Reaching a master at all is
      // proof of life, and observe() has already recorded that.
      if (model_ != nullptr) {
        model_->observe(frame.src);
      }
      break;
  }
}

void HAPStack::handleRequest(const HAPFrame& frame) noexcept {
  HAPReader reader(frame.payload, frame.payloadSize);
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));

  switch (frame.message()) {
    case HAPMessage::Ping: {
      HAPPong pong;
      pong.descriptorRev = node_.descriptorRev();
      pong.encode(writer);
      respond(frame, HAPMessage::Pong, payload, writer.size());
      break;
    }

    case HAPMessage::DescribeRequest: {
      HAPDescribeRequest request;
      request.decode(reader);

      HAPDescribeResponse response;
      if (node_.fillDescribe(response, request.fromPage)) {
        response.encode(writer);
        respond(frame, HAPMessage::DescribeResponse, payload, writer.size());
      }
      break;
    }

    case HAPMessage::ReadRequest: {
      HAPReadRequest request;
      request.decode(reader);

      HAPReport report;
      node_.read(request, report);
      report.encode(writer);
      respond(frame, HAPMessage::ReadResponse, payload, writer.size());
      break;
    }

    case HAPMessage::WriteRequest: {
      const HAPWriteRequest request = HAPWriteRequest::decode(reader);
      const HAPWriteResponse response = node_.write(request);

      // The device only learns of a command that was actually taken - a refusal
      // is the far end's news, not a driver's.
      if (response.result == HAPResult::Ok && onWrite_.is_valid()) {
        onWrite_(request.classId, request.instanceId, request.portId,
                 response.value);
      }

      response.encode(writer);
      respond(frame, HAPMessage::WriteResponse, payload, writer.size());
      break;
    }

    case HAPMessage::SetNameRequest: {
      HAPSetNameRequest request;
      request.decode(reader);

      const HAPSetNameResponse response = node_.rename(request);

      if (response.result == HAPResult::Ok) {
        HAPStore::saveNames(node_);
      }

      response.encode(writer);
      respond(frame, HAPMessage::SetNameResponse, payload, writer.size());
      break;
    }

    case HAPMessage::SetPolicyRequest: {
      const HAPSetPolicyRequest request = HAPSetPolicyRequest::decode(reader);
      const HAPSetPolicyResponse response = reporter_.setPolicy(request);
      response.encode(writer);
      respond(frame, HAPMessage::SetPolicyResponse, payload, writer.size());
      break;
    }

    case HAPMessage::SetLinkRequest: {
      HAPLinkSpec spec;
      HAPSetLinkResponse response;
      response.result =
          spec.decode(reader) ? links_.set(spec) : HAPResult::BadRequest;
      response.linkId = spec.linkId;
      response.encode(writer);
      respond(frame, HAPMessage::SetLinkResponse, payload, writer.size());
      break;
    }

    case HAPMessage::ClearLinkRequest: {
      HAPClearLinkRequest request;
      HAPClearLinkResponse response;
      response.result =
          request.decode(reader) ? links_.clear(request.linkId)
                                 : HAPResult::BadRequest;
      response.encode(writer);
      respond(frame, HAPMessage::ClearLinkResponse, payload, writer.size());
      break;
    }

    case HAPMessage::ListLinksRequest: {
      HAPListLinksRequest request;
      request.decode(reader);

      HAPListLinksResponse response;
      if (links_.fillList(response, request.fromPage)) {
        response.encode(writer);
        respond(frame, HAPMessage::ListLinksResponse, payload, writer.size());
      }
      break;
    }

    default: {
      HAPNack nack;
      nack.seq = frame.seq;
      nack.reason = HAPResult::Unsupported;
      nack.encode(writer);
      respond(frame, HAPMessage::Nack, payload, writer.size());
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

bool HAPStack::transmit(const HAPMac& mac, const HAPFrame& frame) noexcept {
  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = frame.encode(buffer, sizeof(buffer));

  if (size == 0 || !transport_.send(mac, buffer, size)) {
    return false;
  }

  ++sent_;

  // Only upward traffic opens a window. A master sending to a child is not
  // waiting on the radio; a slave that just spoke is.
  if (frame.isUpstream()) {
    window_.open();
  }

  return true;
}

bool HAPStack::forwardToChild(HAPFrame& frame, uint8_t childIndex) noexcept {
  const HAPChild* child = router_.child(childIndex);
  if (child == nullptr) {
    return false;
  }

  if (!child->isBatteryPowered()) {
    return transmit(child->mac, frame);
  }

  // It is not there. Sending would be shouting into an empty room, so the frame
  // waits - marked, so the far end can see it was late rather than live.
  frame.set(HAPFlags::Queued);

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = frame.encode(buffer, sizeof(buffer));

  if (size == 0 || !queue_.push(childIndex, buffer, size)) {
    // A second command for a child that already has one waiting. An honest
    // refusal beats a queue that reorders things nobody is watching.
    HWarning("nothing queued for child %u: it already has a frame waiting",
             childIndex);

    uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
    HAPWriter writer(payload, sizeof(payload));
    HAPNack nack;
    nack.seq = frame.seq;
    nack.reason = HAPResult::Busy;
    nack.encode(writer);

    sendUpstream(HAPMessage::Nack, HAPFlags::Upstream, payload, writer.size());
    return false;
  }

  HInfo("queued %u bytes for sleeping child %u", static_cast<unsigned>(size),
        childIndex);
  return true;
}

void HAPStack::flushQueue(uint8_t childIndex) noexcept {
  size_t size = 0;
  const uint8_t* waiting = queue_.peek(childIndex, size);

  if (waiting == nullptr) {
    return;
  }

  const HAPChild* child = router_.child(childIndex);
  if (child == nullptr) {
    queue_.pop(childIndex);
    return;
  }

  if (transport_.send(child->mac, waiting, size)) {
    ++sent_;
    queue_.pop(childIndex);
    HInfo("delivered a queued frame to child %u", childIndex);
  }
}

bool HAPStack::sendUpstream(HAPMessage type, uint8_t flags,
                            const uint8_t* payload, size_t size) noexcept {
  if (!router_.hasParent()) {
    return false;
  }

  HAPFrame frame;
  frame.type = static_cast<uint8_t>(type);
  frame.flags = static_cast<uint8_t>(flags | HAPFlags::Upstream);
  frame.seq = ++seq_;
  frame.payload = payload;
  frame.payloadSize = static_cast<uint8_t>(size);

  return transmit(router_.parentMac(), frame);
}

bool HAPStack::sendDownstream(const HAPPath& path, HAPMessage type,
                              uint8_t flags, const uint8_t* payload, size_t size,
                              uint16_t seq) noexcept {
  HAPFrame frame;
  frame.type = static_cast<uint8_t>(type);
  frame.flags = flags;
  frame.seq = seq != 0 ? seq : ++seq_;
  frame.dest = path;
  frame.payload = payload;
  frame.payloadSize = static_cast<uint8_t>(size);

  // The first hop is resolved here, so this node's own frames take the same
  // path through the same code as anything it forwards.
  const HAPRoute route = router_.resolveDownstream(frame);

  switch (route.action) {
    case HAPRouteAction::Deliver:
      deliverLocally(frame);
      return true;

    case HAPRouteAction::ToChild:
      return forwardToChild(frame, route.childIndex);

    default:
      HWarning("cannot reach %s: %s", path.toString().c_str(),
               HAPResultToString(route.reason));
      return false;
  }
}

bool HAPStack::respond(const HAPFrame& to, HAPMessage type,
                       const uint8_t* payload, size_t size) noexcept {
  // The response echoes the request's sequence number, which is what lets an
  // originator match the two - and why a WriteResponse IS the acknowledgement a
  // WriteRequest asked for.
  HAPFrame frame;
  frame.type = static_cast<uint8_t>(type);
  frame.flags = HAPFlags::Upstream;
  frame.seq = to.seq;
  frame.payload = payload;
  frame.payloadSize = static_cast<uint8_t>(size);

  if (!router_.hasParent()) {
    return false;
  }

  return transmit(router_.parentMac(), frame);
}

void HAPStack::sendRouteError(const HAPFrame& frame, uint8_t failedHop,
                              HAPResult reason) noexcept {
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));

  HAPRouteError routeError;
  routeError.seq = frame.seq;
  routeError.failedHop = failedHop;
  routeError.reason = reason;
  routeError.encode(writer);

  HWarning("cannot forward seq %u to hop %u: %s", frame.seq, failedHop,
           HAPResultToString(reason));

  sendUpstream(HAPMessage::RouteError, HAPFlags::None, payload, writer.size());
}

// ---------------------------------------------------------------------------
// Originating
// ---------------------------------------------------------------------------

bool HAPStack::sendReport() noexcept {
  HAPReport report;
  reporter_.fillReport(report);

  // Its own values feed its own wires first - the case that never touches the
  // radio at all.
  links_.deliver(HAPPath(), report);

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  report.encode(writer);

  if (!writer.ok()) {
    return false;
  }

  if (!sendUpstream(HAPMessage::Report, HAPFlags::None, payload,
                    writer.size())) {
    return false;
  }

  // Marked only once it has actually gone. Marking a report that failed would
  // start the next interval from a reading nobody received.
  reporter_.markReported();
  return true;
}

bool HAPStack::ping(const HAPPath& path) noexcept {
  return sendDownstream(path, HAPMessage::Ping, HAPFlags::None, nullptr, 0);
}

bool HAPStack::requestDescribe(const HAPPath& path, uint8_t fromPage) noexcept {
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));

  HAPDescribeRequest request;
  request.fromPage = fromPage;
  request.encode(writer);

  return sendDownstream(path, HAPMessage::DescribeRequest, HAPFlags::None,
                        payload, writer.size());
}

bool HAPStack::requestRead(const HAPPath& path,
                           const HAPReadRequest& request) noexcept {
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  request.encode(writer);

  return sendDownstream(path, HAPMessage::ReadRequest, HAPFlags::None, payload,
                        writer.size());
}

bool HAPStack::requestWrite(const HAPPath& path,
                            const HAPWriteRequest& request) noexcept {
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  request.encode(writer);

  return sendDownstream(path, HAPMessage::WriteRequest, HAPFlags::AckRequested,
                        payload, writer.size());
}

bool HAPStack::requestSetPolicy(const HAPPath& path,
                                const HAPSetPolicyRequest& request) noexcept {
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  request.encode(writer);

  return sendDownstream(path, HAPMessage::SetPolicyRequest,
                        HAPFlags::AckRequested, payload, writer.size());
}

bool HAPStack::requestSetName(const HAPPath& path,
                              const HAPSetNameRequest& request) noexcept {
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  request.encode(writer);

  return sendDownstream(path, HAPMessage::SetNameRequest, HAPFlags::AckRequested,
                        payload, writer.size());
}

bool HAPStack::requestSetLink(const HAPPath& path,
                              const HAPLinkSpec& spec) noexcept {
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  spec.encode(writer);

  return sendDownstream(path, HAPMessage::SetLinkRequest, HAPFlags::AckRequested,
                        payload, writer.size());
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

bool HAPStack::bindAsMaster() noexcept {
  return binder_.listen();
}

bool HAPStack::bindAsSlave() noexcept {
  return binder_.announce();
}

bool HAPStack::bind() noexcept {
  switch (node_.deviceType()) {
    case HAPDeviceType::Gateway:
      // It can only ever adopt.
      return bindAsMaster();

    case HAPDeviceType::Sensor:
      // It can only ever be adopted.
      return bindAsSlave();

    case HAPDeviceType::Controller:
      // Both, so this is a guess: get placed first, then take children. A
      // controller with a user interface should ask rather than guess - see the
      // header.
      return router_.hasParent() ? bindAsMaster() : bindAsSlave();
  }

  return false;
}


bool HAPStack::announceChild(const HAPChild& child) noexcept {
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));

  HAPChildAttached attached;
  attached.childIndex = child.index;
  attached.deviceType = child.deviceType;
  attached.capabilities = child.capabilities;
  attached.reportIntervalSec = child.reportIntervalSec;
  attached.descriptorRev = child.descriptorRev;

  for (size_t i = 0; i < HAP_MAC_LEN; ++i) {
    attached.mac[i] = child.mac.bytes[i];
  }

  attached.encode(writer);

  if (router_.hasParent()) {
    return sendUpstream(HAPMessage::ChildAttached, HAPFlags::None, payload,
                        writer.size());
  }

  if (model_ != nullptr) {
    // This node IS the root, so the notice has nowhere to climb to - it is
    // already where it was going.
    model_->noteChildAttached(HAPPath(), attached);
    rememberModel();
    return true;
  }

  // Neither a parent to tell nor a model to remember in. The child is recorded
  // in the router either way; see HAPStack::bound(), which is where this gets
  // said once there IS somebody to say it to.
  return false;
}

void HAPStack::rememberModel() noexcept {
  // Only once the node is running: begin() seeds the model from its own child
  // table, and saving inside that loop would rewrite the file once per child at
  // every boot for a value that had not changed.
  if (!started_ || model_ == nullptr) {
    return;
  }

  // Called from the three places identity can change - a ChildAttached arriving,
  // this node being the root of one, and a factory reset - and from nowhere
  // else. A report changes values, never identity, and writing on one would be
  // a flash write every ten seconds for no reason.
  HAPStore::saveModel(*model_);
}

void HAPStack::childBound(const HAPChild& child) noexcept {
  HAPStore::saveChildren(router_);

  // The root hears about it now rather than at this child's first report, which
  // for a battery sensor could be a minute away - and if this node has no parent
  // yet, at the moment it gains one instead. See bound().
  announceChild(child);

  if (onChildBound_.is_valid()) {
    onChildBound_(child);
  }
}

void HAPStack::bound(const HAPMac& parent, uint8_t index,
                     const uint8_t* linkKey, uint8_t channel) noexcept {
  bind_.bound = true;
  bind_.parent = parent;
  bind_.indexAtParent = index;
  bind_.channel = channel;

  for (size_t i = 0; i < HAP_KEY_LEN; ++i) {
    bind_.linkKey[i] = linkKey[i];
  }

  // Persisted at once. Everything after this depends on surviving a reboot, and
  // a battery node's next event may well be a power cut.
  HAPStore::saveBind(bind_);

  // AND EVERYTHING THIS NODE ALREADY HAD, now that there is somebody to tell.
  //
  // ChildAttached is fired once, when a child completes its handshake - and if
  // this node had no parent at that instant it went nowhere at all. A controller
  // adopted downward before it was adopted upward is not a strange order to do
  // things in; it is what happens whenever somebody sets up the bottom of a tree
  // first. The root would then never learn those grandchildren existed, and
  // would build model entries for them out of reports alone: no MAC, so a device
  // that moved could not be recognised, and no interval, so it could never be
  // called offline either. An entry that cannot be identified and cannot expire.
  //
  // Found on three boards, where the middle node was factory-reset, adopted the
  // leaf five seconds later, and bound upward five seconds after that.
  for (size_t i = 0; i < router_.childCount(); ++i) {
    const HAPChild* child = router_.childAt(i);

    if (child != nullptr) {
      announceChild(*child);
    }
  }

  if (onBound_.is_valid()) {
    onBound_(parent, index, linkKey, channel);
  }
}

bool HAPStack::factoryReset() noexcept {
  // THE RADIO'S PEER TABLE GOES FIRST, and forgetting it is not a tidiness
  // problem - it is what stops the node ever binding again.
  //
  // Every link in HAP is encrypted, so a bound node holds its parent and its
  // children as ENCRYPTED peers. Clearing the router without clearing those
  // leaves the addresses behind with keys nobody will use again, and ESP-NOW
  // then expects everything from them to be encrypted. The next BindAccept must
  // arrive in the CLEAR - it carries the new key - so the radio discards it
  // before any of this code sees it, while still acknowledging it at the MAC
  // layer. The far end therefore sees a successful send and no reply.
  //
  // The symptom is a node that announces for ever after a factory reset while
  // its would-be parent logs "never confirmed", and it survives every retry
  // because nothing in RAM has changed. A power cycle "fixes" it, which is what
  // makes it so easy to miss: on the bench the reset that failed and the reboot
  // that fixed it are two different actions minutes apart.
  //
  // A master does not have this problem in the same way - sendAccept() re-adds
  // the peer unencrypted before every acceptance - which is exactly why the
  // fault only ever appears on the node being adopted.
  if (bind_.bound) {
    transport_.removePeer(bind_.parent);
  }

  for (size_t i = 0; i < router_.childCount(); ++i) {
    const HAPChild* child = router_.childAt(i);
    if (child != nullptr) {
      transport_.removePeer(child->mac);
    }
  }

  router_.clearParent();
  router_.clearChildren();
  links_.clear(HAPClearLinkRequest::kAllLinks);
  queue_.clear();
  bind_ = HAPBindState();

  if (model_ != nullptr) {
    model_->clear();
  }

  HInfo("factory reset: unbound and forgotten");
  return HAPStore::clearAll();
}

void HAPStack::deliverLink(const HAPPortRef& destination,
                           const HValue& value) noexcept {
  if (destination.path.isEmpty()) {
    // The case worth having: no radio, no latency, and it keeps working with
    // everything above this node unplugged.
    HAPInstance* instance =
        node_.instance(destination.classId, destination.instanceId);

    if (instance != nullptr) {
      instance->write(destination.portId, value);
    }

    return;
  }

  const HAPWriteRequest request(destination.classId, destination.instanceId,
                                destination.portId, value);

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  request.encode(writer);

  // No acknowledgement asked for: confirming every reading would double the
  // traffic to prove something the destination's staleness timer measures
  // better.
  sendDownstream(destination.path, HAPMessage::WriteRequest, HAPFlags::None,
                 payload, writer.size());
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

HAPNode& HAPStack::node() noexcept { return node_; }
HAPRouter& HAPStack::router() noexcept { return router_; }
HAPBinder& HAPStack::binder() noexcept { return binder_; }
HAPReporter& HAPStack::reporter() noexcept { return reporter_; }
HAPLinks& HAPStack::links() noexcept { return links_; }
HAPQueue& HAPStack::queue() noexcept { return queue_; }

const HAPListenWindow& HAPStack::listenWindow() const noexcept {
  return window_;
}

void HAPStack::setModel(HAPModel& model) noexcept { model_ = &model; }
HAPModel* HAPStack::model() noexcept { return model_; }

bool HAPStack::isRoot() const noexcept { return router_.isRoot(); }
bool HAPStack::isBound() const noexcept { return router_.hasParent(); }

void HAPStack::onWrite(WriteHook hook) noexcept { onWrite_ = hook; }
void HAPStack::onValues(ValuesHook hook) noexcept { onValues_ = hook; }

void HAPStack::onChildBound(HAPBinder::ChildBoundHook hook) noexcept {
  onChildBound_ = hook;
}

void HAPStack::onBound(HAPBinder::BoundHook hook) noexcept {
  onBound_ = hook;
}

uint32_t HAPStack::sentCount() const noexcept { return sent_; }
uint32_t HAPStack::receivedCount() const noexcept { return received_; }
