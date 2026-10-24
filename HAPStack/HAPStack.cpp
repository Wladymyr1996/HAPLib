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

bool HAPStack::begin() noexcept {
  HAPStore::init();

  // What this node already knew. A node bound before the reboot is bound after
  // it, with no handshake and nobody pressing anything.
  if (HAPStore::loadBind(bind_)) {
    router_.setParent(bind_.parent, bind_.indexAtParent);
  }

  if (!transport_.begin()) {
    return false;
  }

  transport_.onReceive(
      HAPITransport::Receiver::create<HAPStack, &HAPStack::onFrame>(*this));

  binder_.onChildBound(
      HAPBinder::ChildBoundHook::create<HAPStack, &HAPStack::childBound>(*this));
  binder_.onBound(
      HAPBinder::BoundHook::create<HAPStack, &HAPStack::bound>(*this));
  links_.onDeliver(
      HAPLinks::DeliveryHook::create<HAPStack, &HAPStack::deliverLink>(*this));

  // What this node was TOLD about the tree below it, before it restarted.
  // Restored before the children below, so that seeding them refreshes entries
  // rather than creating rival ones - and so that a device which moved while
  // this node was off is recognised by its id rather than appearing twice.
  if (model_ != nullptr) {
    HAPStore::loadModel(*model_);
  }

  HAPStore::loadChildren(router_);
  HAPStore::loadNames(node_);

  for (size_t i = 0; i < router_.childCount(); ++i) {
    const HAPChild* child = router_.childAt(i);
    if (child != nullptr) {
      // The model is told what this node already knows about that child.
      //
      // A REPORT CARRIES NO ID, NO CAPABILITIES AND NO INTERVAL - only
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

        child->id.toMac(restored.mac);

        model_->noteChildAttached(restored);
      }
    }
  }

  reporter_.begin(node_.reportIntervalSec(), HValue());

  if (node_.isBatteryPowered() && node_.reportIntervalSec() != 0) {
    // A wake period is a floor on reporting whatever a master asks for.
    reporter_.setMinimumIntervalSec(node_.reportIntervalSec());
  }

  started_ = true;

  HInfo("%s, %u children, %s", node_.name().c_str(),
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
  // an unbound node has nobody to say it to - and only when the application has
  // not taken the schedule over; see setAutomaticReports().
  if (automaticReports_ && router_.hasParent() && reporter_.isDue()) {
    sendReport();
  }

  if (model_ != nullptr) {
    model_->sweepOffline();
  }
}

void HAPStack::adoptPolicy(const HAPFrame& frame) noexcept {
  // What the node actually TOOK, which is not always what was asked - and
  // which the model has to adopt as the interval it measures a silence
  // against. Without this, tightening a node's reporting is a way to have it
  // declared offline for obeying: the master goes on expecting the interval
  // announced at bind time and calls the node dead three of those later.
  //
  // The same message also arrives UNASKED, from a battery node whose owner
  // changed its wake period - see announceReportInterval(). The two are
  // handled alike, because they say the same thing: this is the interval
  // now in force.
  HAPReader reader(frame.payload, frame.payloadSize);
  const HAPSetPolicyResponse response = HAPSetPolicyResponse::decode(reader);

  if (!reader.ok()) {
    return;
  }

  // A refusal promises nothing - see HAPModel::notePolicy() - so only an
  // accepted interval is worth keeping anywhere.
  const bool accepted = (response.result == HAPResult::Ok);

  // Our own child's entry too, and it has to be kept: begin() re-seeds the
  // model from the child table after every reboot, so a stale entry there
  // would quietly put the old interval back.
  const HAPChild* ownChild = router_.childById(frame.src);
  if (accepted && ownChild != nullptr &&
      router_.setChildReportInterval(ownChild->index, response.intervalSec)) {
    HAPStore::saveChildren(router_);
  }

  if (model_ != nullptr) {
    const HAPRemoteNode* known = model_->find(frame.src);
    const bool changed =
        accepted && (known == nullptr || known->reportIntervalSec != response.intervalSec);

    model_->notePolicy(frame.src, response);

    // Written only when it moved. Most of these answer a master's own
    // request with the value it already expected, and a flash write per
    // answer would be a write for nothing.
    if (changed) {
      rememberModel();
    }
  }

  if (accepted && onReportInterval_.is_valid()) {
    onReportInterval_(frame.src, response.intervalSec);
  }
}

void HAPStack::adoptDescribe(const HAPNodeId& from,
                             const HAPDescribeResponse& response) noexcept {
  const HAPRemoteNode* known = model_ != nullptr ? model_->find(from) : nullptr;
  const bool modelMoved = response.hasInterval && known != nullptr &&
                          known->reportIntervalSec != response.reportIntervalSec;

  // Our own child's entry is kept for the same reason as in adoptPolicy():
  // begin() re-seeds the model from it after every reboot.
  const HAPChild* ownChild = router_.childById(from);
  const bool childMoved =
      response.hasInterval && ownChild != nullptr &&
      router_.setChildReportInterval(ownChild->index, response.reportIntervalSec);

  if (childMoved) {
    HAPStore::saveChildren(router_);
  }

  if (model_ != nullptr) {
    model_->noteDescribe(from, response);
  }

  if (modelMoved) {
    rememberModel();
  }

  // Only a change is news: every node is re-described after every restart,
  // and nearly always nothing moved.
  if ((modelMoved || childMoved) && onReportInterval_.is_valid()) {
    onReportInterval_(from, response.reportIntervalSec);
  }
}

void HAPStack::setAutomaticReports(bool enabled) noexcept {
  automaticReports_ = enabled;
}

bool HAPStack::maySleep() const noexcept {
  return window_.maySleep() && binder_.state() != HAPBinder::State::Announcing &&
         binder_.state() != HAPBinder::State::Accepting;
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

void HAPStack::onFrame(const HAPAddress& from, const uint8_t* data,
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

  // The address says who sent it THIS HOP; frame.src says who it began with. A
  // frame climbing the tree is relayed, so the two differ - the router decides
  // on the first and everything else (model, links, hooks) on the second.
  const HAPNodeId origin = from.nodeId();
  const HAPChild* sender = router_.childById(origin);
  const HAPRoute route = router_.route(frame, origin, transport_.localId());

  switch (route.action) {
    case HAPRouteAction::Deliver:
      deliverLocally(frame);
      break;

    case HAPRouteAction::ToParent: {
      // A report passing through feeds anything wired to it - and carries on.
      // Its source is the originator's id, which is what a link stores.
      if (frame.message() == HAPMessage::Report) {
        HAPReader reader(frame.payload, frame.payloadSize);
        HAPReport report;

        if (report.decode(reader)) {
          // This node's own model hears it too: without it a middle node's
          // model never sees its children speak, and sweepOffline() calls
          // every one of them offline three intervals after boot.
          if (model_ != nullptr) {
            model_->noteReport(frame.src, report);
          }

          links_.deliver(frame.src, report);

          if (onValues_.is_valid()) {
            onValues_(frame.src, report);
          }
        }
      }

      // An interval passing through - a child's own announcement, or its
      // answer to the root - is adopted here as well, and carries on. A middle
      // node measures silence against it too, and re-seeds its model from its
      // child table after a reboot.
      if (frame.message() == HAPMessage::SetPolicyResponse) {
        adoptPolicy(frame);
      }

      // A description passing through, asked for by the root, is as true here.
      if (frame.message() == HAPMessage::DescribeResponse) {
        HAPReader reader(frame.payload, frame.payloadSize);
        HAPDescribeResponse response;

        if (response.decode(reader)) {
          adoptDescribe(frame.src, response);
        }
      }

      transmit(router_.parentId(), frame);
      break;
    }

    case HAPRouteAction::ToChild:
    case HAPRouteAction::ToNode:
      // Only a frame this node ORIGINATES goes straight to its target; route()
      // never decides it for one that arrived.
      break;

    case HAPRouteAction::Drop:
      HWarning("dropped %s from %s: %s", frame.src.toString().c_str(),
               from.toString().c_str(), HAPResultToString(route.reason));
      break;
  }

  // Whatever it was, the child is awake right now - which for a sleeping one is
  // the only moment anything held for it can be delivered.
  if (sender != nullptr && sender->isBatteryPowered()) {
    flushQueue(sender->index);
  }
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
    case HAPMessage::OtaResponse:
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
      if (response.decode(reader)) {
        adoptDescribe(frame.src, response);
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

    case HAPMessage::SetPolicyResponse:
      adoptPolicy(frame);
      break;

    case HAPMessage::ChildAttached: {
      HAPChildAttached attached;
      if (attached.decode(reader) && model_ != nullptr) {
        model_->noteChildAttached(attached);

        // Identity changed - a node appeared, or one moved. This is the only
        // message that can say so, and it is never repeated.
        rememberModel();
      }
      break;
    }

    case HAPMessage::OtaResponse: {
      // Listed as upstream news rather than left to handleRequest(), whose
      // default branch is a Nack: a master answering its own child's "I will
      // update" with "unsupported" would be news about nothing climbing to the
      // root. Logged and no more - what a master does about it is its own
      // business, and the interesting half arrives later as the node's next
      // report from a firmware it did not have before.
      HAPOtaResponse response;
      if (response.decode(reader)) {
        HInfo("%s runs firmware %s: %s", frame.src.toString().c_str(),
              response.version.c_str(), HAPResultToString(response.result));
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

    case HAPMessage::OtaRequest: {
      HAPOtaRequest request;
      const bool parsed = request.decode(reader);

      HAPOtaResponse response;
      response.version = node_.firmwareVersion();

      if (!parsed) {
        response.result = HAPResult::BadRequest;
      } else if (!onOta_.is_valid()) {
        // A node that cannot update itself says so rather than staying silent,
        // so a master learns the answer instead of timing out.
        response.result = HAPResult::Unsupported;
      } else if (request.isQueryOnly()) {
        // The version is the whole answer, and nothing is acted on. This is
        // what lets a master confirm an update afterwards - the firmware that
        // would have reported the outcome has been replaced by the one it
        // installed.
        response.result = HAPResult::Ok;
      } else {
        response.result = onOta_(request);
      }

      // ALWAYS answered before anything happens. The hook records what was
      // asked and returns; the reboot is the application's, a tick later, once
      // this frame has left the radio.
      response.encode(writer);
      respond(frame, HAPMessage::OtaResponse, payload, writer.size());
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

bool HAPStack::transmit(const HAPNodeId& to, const HAPFrame& frame) noexcept {
  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = frame.encode(buffer, sizeof(buffer));

  if (size == 0 || !transport_.send(HAPAddress::fromNodeId(to), buffer, size)) {
    return false;
  }

  ++sent_;

  // Only traffic for the master opens a window. A master sending to a child is
  // not waiting on the radio; a slave that just spoke is.
  if (frame.isForMaster()) {
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
    return transmit(child->id, frame);
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

    sendUpstream(HAPMessage::Nack, HAPFlags::None, payload, writer.size());
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

  if (transport_.send(HAPAddress::fromNodeId(child->id), waiting, size)) {
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
  frame.flags = flags;
  frame.seq = ++seq_;
  frame.src = transport_.localId();
  frame.payload = payload;
  frame.payloadSize = static_cast<uint8_t>(size);

  return transmit(router_.parentId(), frame);
}

bool HAPStack::sendDownstream(const HAPNodeId& node, HAPMessage type,
                              uint8_t flags, const uint8_t* payload, size_t size,
                              uint16_t seq) noexcept {
  HAPFrame frame;
  frame.type = static_cast<uint8_t>(type);
  frame.flags = flags;
  frame.seq = seq != 0 ? seq : ++seq_;
  frame.dst = node;
  frame.src = transport_.localId();
  frame.payload = payload;
  frame.payloadSize = static_cast<uint8_t>(size);

  const HAPRoute route = router_.resolveDownstream(node, frame.src);

  switch (route.action) {
    case HAPRouteAction::Deliver:
      deliverLocally(frame);
      return true;

    case HAPRouteAction::ToChild:
      return forwardToChild(frame, route.childIndex);

    case HAPRouteAction::ToNode:
      return transmit(node, frame);

    default:
      HWarning("cannot reach %s: %s", node.toString().c_str(),
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
  frame.flags = HAPFlags::None;
  frame.seq = to.seq;
  frame.src = transport_.localId();
  frame.payload = payload;
  frame.payloadSize = static_cast<uint8_t>(size);

  if (!router_.hasParent()) {
    return false;
  }

  return transmit(router_.parentId(), frame);
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
  links_.deliver(HAPNodeId::zero(), report);

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

bool HAPStack::announceReportInterval() noexcept {
  // Any out port will do - the model keeps one interval per NODE and ignores
  // which port a policy names - so the first instance's primary one is named.
  const HAPInstance* first = node_.instanceAt(0);
  if (first == nullptr) {
    return false;
  }

  const HAPSetPolicyResponse notice(HAPResult::Ok, first->classId(),
                                    first->instanceId(), 0,
                                    node_.reportIntervalSec(), HValue());

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  notice.encode(writer);

  if (!writer.ok()) {
    return false;
  }

  return sendUpstream(HAPMessage::SetPolicyResponse, HAPFlags::None, payload,
                      writer.size());
}

bool HAPStack::ping(const HAPNodeId& node) noexcept {
  return sendDownstream(node, HAPMessage::Ping, HAPFlags::None, nullptr, 0);
}

bool HAPStack::requestDescribe(const HAPNodeId& node, uint8_t fromPage) noexcept {
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));

  HAPDescribeRequest request;
  request.fromPage = fromPage;
  request.encode(writer);

  return sendDownstream(node, HAPMessage::DescribeRequest, HAPFlags::None,
                        payload, writer.size());
}

bool HAPStack::requestRead(const HAPNodeId& node,
                           const HAPReadRequest& request) noexcept {
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  request.encode(writer);

  return sendDownstream(node, HAPMessage::ReadRequest, HAPFlags::None, payload,
                        writer.size());
}

bool HAPStack::requestWrite(const HAPNodeId& node,
                            const HAPWriteRequest& request) noexcept {
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  request.encode(writer);

  return sendDownstream(node, HAPMessage::WriteRequest, HAPFlags::AckRequested,
                        payload, writer.size());
}

bool HAPStack::requestSetPolicy(const HAPNodeId& node,
                                const HAPSetPolicyRequest& request) noexcept {
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  request.encode(writer);

  return sendDownstream(node, HAPMessage::SetPolicyRequest,
                        HAPFlags::AckRequested, payload, writer.size());
}

bool HAPStack::requestSetName(const HAPNodeId& node,
                              const HAPSetNameRequest& request) noexcept {
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  request.encode(writer);

  return sendDownstream(node, HAPMessage::SetNameRequest, HAPFlags::AckRequested,
                        payload, writer.size());
}

bool HAPStack::requestSetLink(const HAPNodeId& node,
                              const HAPLinkSpec& spec) noexcept {
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  spec.encode(writer);

  return sendDownstream(node, HAPMessage::SetLinkRequest, HAPFlags::AckRequested,
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

  child.id.toMac(attached.mac);

  attached.encode(writer);

  if (router_.hasParent()) {
    return sendUpstream(HAPMessage::ChildAttached, HAPFlags::None, payload,
                        writer.size());
  }

  if (model_ != nullptr) {
    // This node IS the root, so the notice has nowhere to climb to - it is
    // already where it was going.
    model_->noteChildAttached(attached);
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

void HAPStack::bound(const HAPAddress& parent, uint8_t index,
                     const uint8_t* linkKey, uint8_t channel) noexcept {
  bind_.bound = true;
  bind_.parent = parent.nodeId();
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
  // would build model entries for them out of reports alone: no id, so a device
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
  if (destination.node.isZero()) {
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
  sendDownstream(destination.node, HAPMessage::WriteRequest, HAPFlags::None,
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
void HAPStack::onReportInterval(IntervalHook hook) noexcept { onReportInterval_ = hook; }
void HAPStack::onOta(OtaHook hook) noexcept { onOta_ = hook; }

void HAPStack::onChildBound(HAPBinder::ChildBoundHook hook) noexcept {
  onChildBound_ = hook;
}

void HAPStack::onBound(HAPBinder::BoundHook hook) noexcept {
  onBound_ = hook;
}

uint32_t HAPStack::sentCount() const noexcept { return sent_; }
uint32_t HAPStack::receivedCount() const noexcept { return received_; }
