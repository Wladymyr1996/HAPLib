#include <HAP.h>

const char* HAPResultToString(HAPResult result) noexcept {
  switch (result) {
    case HAPResult::Ok:
      return "ok";
    case HAPResult::NoSuchChild:
      return "no such child";
    case HAPResult::ChildUnreachable:
      return "child unreachable";
    case HAPResult::NoSuchClass:
      return "no such class";
    case HAPResult::NotWritable:
      return "not writable";
    case HAPResult::BadValue:
      return "bad value";
    case HAPResult::BadRequest:
      return "bad request";
    case HAPResult::NotBound:
      return "not bound";
    case HAPResult::Busy:
      return "busy";
    case HAPResult::Unsupported:
      return "unsupported";
    case HAPResult::NoRoom:
      return "no room";
    case HAPResult::NoSuchPort:
      return "no such port";
    case HAPResult::TypeMismatch:
      return "type mismatch";
    case HAPResult::InputBusy:
      return "input busy";
    case HAPResult::NoLinkSlot:
      return "no link slot";
  }

  // Reached only by a code from a peer this build is older than. Naming it
  // "unknown" in a log is more use than asserting on a byte that came off a
  // radio.
  return "unknown";
}
