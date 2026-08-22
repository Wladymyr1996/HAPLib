# HAP — how it works

The reasoning behind the Hatynka Air Protocol. For byte layouts and exact
message contents, see [Protocol.md](Protocol.md).

---

## The shape of a network

One gateway at the root, controllers in the middle, sensors at the leaves.

```
Gateway ──1── Heating controller ──2── Thermometer (bedroom)
        │                         └─4── Lamp (hall)
        └─3── Thermometer (hall)
```

| Role | Master | Slave | Sleeps |
| --- | --- | --- | --- |
| Gateway | always | never | never |
| Controller | yes | yes | never |
| Sensor on a battery | never | yes | almost always |

A master may have **six children if it is the root, and five if it is not** —
not a protocol choice but a radio one. ESP-NOW permits six *encrypted* peers per
device, and every HAP link is encrypted, so a node that has a parent has already
spent one of the six going upward.

A slave has **exactly one** parent, for its whole bound life, which is what keeps
the graph a tree.

A battery device is always a leaf. It is asleep when a frame would need
forwarding, so it cannot be anyone's parent.

### The root is whoever has no parent

**Gateway is a role, not a device class.** Read the upstream rule below: *have a
parent → forward to it; am the root → this frame is mine.* Nothing there names a
gateway, and nothing needs one.

So the smallest useful network is a master and a slave — a heat controller with
a thermometer under it, configured from the controller's own portal. It is depth
1, it uses the same routing, the same bind, the same queueing, and it has no
special case anywhere in the code.

Bind a gateway on top of it a year later and **nothing below renumbers**, because
addresses are relative: the thermometer stays `1` from the controller and merely
becomes `1.1` from the gateway as well. Reports find the new root by themselves —
they already climb — and any links already installed on the controller keep
running, because they are local. Unplug that gateway again and the heating
carries on.

What changes hands is only the *model*: whoever is root holds the full picture
and the UI worth trusting.

## Nobody knows the whole network

Each node knows two things: the MAC of its parent, and the indices it handed to
its own children. There is no routing table, no node list, no map.

An address is therefore a **path** — the hops from where you are standing to
where you want to reach. The bedroom thermometer above is `1.2` from the
gateway, `2` from the heating controller, and nothing at all from itself.

**Downstream**, each node eats the first hop: the gateway sees `1.2`, recognises
hop 1 as its own child, and passes on `2`. The controller sees `2`, recognises
its child, and passes on an empty path. Empty means "this is for me".

**Upstream** is the mirror image, and it is the part worth understanding. A
report leaves the sensor with **both paths empty**. Every node that forwards it
**prepends the index of the child it arrived from** to the frame's source path:

```
sensor sends       src = []
controller sends   src = [2]        (it arrived from child 2)
gateway receives   src = [1, 2]     (it arrived from child 1)
```

The path builds itself on the way up. The gateway ends up holding a route it can
reply straight back down, and no node in the middle had to know anything beyond
its own children.

Two consequences:

- **A dead node hides its subtree.** Unplug the heating controller and the
  bedroom thermometer is unreachable — there is no second way in. The gateway
  sees a `RouteError` from the last node that could still answer, or silence.
- **A node with a dead parent keeps serving its own subtree.** The controller
  cannot forward to a gateway that is gone, but it can still read its
  thermometer and drive its heating. Local control does not depend on the root.

## Does everything go to the gateway?

**Yes — every report climbs the whole chain**, and every node it passes through
reads it on the way.

That is a deliberate simplification. There is no subscription registry, nobody
registers interest in anything, and there is no state anywhere that can fall out
of sync with what a node is actually sending. In a house-sized tree the traffic
is a few dozen bytes per node per minute; a subscription table would cost more
in bugs than it saves in air time.

What *is* adjustable is how often a node speaks. Every class instance has a
**report policy** — an interval, and for analog values a deadband:

> report at most every 60 s, and only if the value moved by more than 0.2 °C

The policy lives on the node, defaults come from its firmware, and a master can
change it. That gives the same throttling a subscription would, without anyone
having to remember who asked for what.

**A forwarding node is a consumer too.** The heating controller acts on the
thermometer reading it is relaying, and the gateway sees the same reading. That
is the point of the tree rather than a hub: control loops stay local and short
while the gateway still gets to watch.

## What a node is made of

A node has a **user name** — free text, not unique, two rooms may both hold a
"Thermometer" — and up to **8 class instances**.

A class is a thing the node can do: `Thermometer`, `Hygrometer`, `Barometer`,
`Switch`, `Lamp`, `Door`, `BatteryState`. A node may have two of the same
class, told apart by instance id, each with its own name. See
[Classes/](Classes/) for what each one reports.

Values travel in the same five-type encoding `HValue` uses — null, bool, int,
float, string — and always in **canonical units**: °C and pascals on the wire,
converted only where they are displayed. A gateway showing Fahrenheit converts
in the gateway.

## Classes are stable; names are not

The split that keeps this simple:

| | Changes when | Owned by |
| --- | --- | --- |
| **The class set** — which classes, how many, their ids, types, access | firmware update only | the firmware |
| **Names** — the node's, and each instance's | whenever the user likes | the user |

A thermometer has a thermometer: that is hardware, and nothing at runtime may
add or remove a class. What people actually edit is the label — "Bedroom"
becomes "Nursery" — and that must be free.

**Names live on the node**, in its own configuration, not on the gateway. The
device is self-describing: replace a dead gateway and the house re-labels itself
from the nodes. The cost is that renaming a sleeping sensor takes one report
cycle, which is the same latency as any other command to it.

### One number keeps everyone honest

Every node computes a **`descriptorRev`** — a CRC-16 over its serialised
descriptor, names included — and carries it in every report and every pong. Two
bytes, in frames it was sending anyway.

A master caches `path → (descriptor, rev)`. When a rev arrives that it does not
recognise, it asks for a fresh `Describe`. That one mechanism covers every case:

- an instance renamed while the gateway was switched off;
- a firmware update that added a class;
- a device physically swapped for a different one in the same slot.

Nothing needs an invalidation message, and it heals itself after any outage. A
gateway that was down for a week finds everything stale on the first report from
each node and re-reads only what changed.

A CRC rather than a counter, deliberately: a counter needs persistent state, gets
confused by a factory reset, and lets two devices disagree about what "revision
4" meant. A CRC of the content cannot.

## How a gateway learns about grandchildren

**State is proportional to responsibility, not to subtree size:**

| Node | Keeps |
| --- | --- |
| every node | its **direct children** — index, MAC, capabilities, rev. Needed to route; mandatory. |
| every node | the **links** it was given, and nothing about the endpoints beyond those. |
| the root | the **whole model** — every node it has heard from, by path, with descriptors. It is mains-powered, it has the RAM, and it is the only one with a UI. |

A controller with twenty descendants and no links stores nothing about any of
them. It forwards by child index and never needs to know what is further down.

The rest does not need to be told. Discovery falls out of reporting:

1. **A report announces existence.** The first report carrying `src = [1,2]`
   tells the gateway there is a node at `1.2`, and its `descriptorRev` says
   whether the gateway already knows what it is.
2. **Describe is pulled lazily**, addressed by path, and cached under
   `(path, rev)`.
3. **Intermediate nodes stay dumb.** A controller caches only its **direct
   children** — it needs those to interpret their reports and act on them — and
   knows nothing about its grandchildren beyond "child 2 exists, forward to it".
   No node holds a model of a subtree, so no node can hold a stale one.

The one push is `ChildAttached`: when a child binds or reattaches, its parent
sends a short notice upward with the child's relative path and rev. Without it a
new battery sensor would go unnoticed until its first report, up to a minute
later; with it, the bind feels finished the moment the button is pressed.

## Binding

Deliberately symmetrical and physical: **a press on each device**, and nothing
happens without both.

```
        MASTER                                     SLAVE
  user presses "Bind"                        user presses "Bind"
  opens a 60 s window                        announces, repeatedly,
                                             sweeping channels
                          ◄── BindAnnounce ──  (broadcast: name, classes,
                                                battery flag, report interval)
   ── BindAccept ──►                           (unicast: child index, link key,
                                                master name, channel)
                          ◄── BindConfirm ──   (encrypted with the new key)
  stores MAC + index                          stores parent MAC + index + key
  window closes                               bound for life, or until reset
   ── ChildAttached ──►  upward, so the root hears about it now
```

- The master's window closes on the first successful bind, or after 60 s.
- A slave that is already bound **ignores its own Bind button**. One parent for
  life is what keeps the graph a tree, and an accidental rebind would silently
  detach a subtree. A factory reset is the only way out.
- The master refuses when it is out of encrypted peer slots — six of them, one
  already spent upward if it has a parent. The refusal happens when the window
  is opened, so it can be seen, rather than three frames later where it cannot.
- Nothing is a child until it confirms. A master that accepted somebody who then
  went quiet takes the slot *and the peer entry* back, or a device that never
  finished binding would occupy one of six encrypted slots forever.

### The slave sweeps; the master does not

ESP-NOW has no channel negotiation, and a master listens only on its own
channel — so an unbound node has no way of knowing where to shout. It announces
on 1, 6 and 11 first, then the rest, dwelling on each long enough for a master
sitting there to hear one and answer, and adopts whatever channel the acceptance
names. That is what the channel field in `BindAccept` is for.

### The one moment the link changes key

`BindAccept` must go out **in the clear** — it carries the key the other end
does not have yet — and everything after it is encrypted. So the master switches
that peer to encrypted the instant the acceptance has actually left the radio.

Not before, or the acceptance itself is encrypted and unreadable. Not later, or
the confirmation arrives encrypted at a peer still expecting plain text and is
dropped by the radio *below* the protocol, where nothing can log it. Hanging
that on a timer is a race; it hangs on the transport's send-completion report
instead, with a timeout only as a fallback for a transport that never sends one.

## Sleeping nodes

A battery sensor is awake for roughly 250 ms in every 60 s. ESP-NOW has no
store-and-forward, so for the other 99.6 % of the time it simply is not there.

The protocol inverts the initiative:

1. At bind, the node declares itself battery-powered and states its report
   interval.
2. It sends `Report` when it wakes, on its own schedule.
3. Its parent **queues** anything meant for it and delivers it as the reply to
   that report, marked as queued.
4. The node stays awake for a short **listen window** after each upstream frame
   — long enough for the reply and one queued frame — and then sleeps.

So a command to a sleeping node is not lost, it is **late**: it lands within one
report interval. A user interface should say *pending* rather than showing the
lamp already off. Mains nodes have their radio on and are reachable at any time.

## Reliability

**Hop by hop, not end to end.** Each hop retries on the ESP-NOW send callback;
reports are otherwise fire-and-forget. End-to-end acknowledgement is requested
per frame, and only commands and configuration writes ask for it.

Through a chain containing a sleeping node, an end-to-end ack costs an extra
wake window per report to confirm something nobody acts on. A missed reading is
replaced by the next one a minute later; a missed *command* is worth the round
trip.

## Liveness

Every node declares a report interval at bind. A master marks a child **offline**
after three missed intervals and reports that upward, so a gateway can tell "this
value is old" from "this value is wrong". `RouteError` gives the same answer
faster when the break is between two mains-powered nodes.

## The gateway and the Wi-Fi channel

ESP-NOW needs every node on one Wi-Fi channel, and a device that is also a Wi-Fi
station has its channel dictated by the router — which can move the whole mesh
out from under itself.

**Serving a portal is not the problem.** ESP-NOW and softAP coexist: in AP mode
the channel is ours to choose, so a master set to the mesh channel keeps the mesh
running while a phone is connected to its portal. A user can stand there
configuring the heating without the sensors going quiet.

The conflict appears only when a device joins **somebody else's** Wi-Fi as a
station, because then the router dictates the channel — and can change it.

That is the gateway's problem and nobody else's, and the clean answer
is two chips: **an ESP32 for the radio side, and an RPi (or a second chip) for
Wi-Fi and the internet**, joined by a serial link. No channel conflict, no RAM or
CPU contention, and an air gap between the house mesh and anything facing the
internet.

The neat part is that the serial link can speak **HAP frames too**. The radio
chip becomes a pure bridge with no application logic of its own, and the host
side is simply another link in the tree — which is why `HAPLib` will put the
transport behind an interface, the way `HIFs` and `HIGpio` already are.

A single-chip gateway remains possible: pin the mesh to the router's channel and
republish it when the router moves. Sleeping children will miss that
announcement and need a channel scan when their parent goes quiet — a class of
bug the two-chip design never has.
