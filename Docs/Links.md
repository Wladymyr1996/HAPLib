# HAP — links

How one thing's output becomes another thing's input, anywhere in the tree.

A link is a **wire**: it carries a value from one port to another and does
nothing else to it. No scaling, no thresholds, no expressions. Logic lives in
classes — which are firmware, and testable — because the moment a wire can carry
an expression, every node needs an interpreter.

See [Protocol.md](Protocol.md) for the frame format and
[HowItWorks.md](HowItWorks.md) for the network it runs on.

---

## 1. Ports

Ports are not a new mechanism. They are a name for what is already on the wire:

> **out port 0** is the value an instance puts in `Report`
> **in port 0** is the value `WriteRequest` sets

Everything that worked before is the port-0 case. What ports add is *more than
one* of each: a `Regulator` has `Measured` and `Setpoint` in, and `Demand` out.

**Ports are defined by the class, not transmitted.** `classId` implies how many
ports an instance has, their directions, their types and their names — all of it
written down in [Classes/](Classes/). That keeps descriptors small and keeps
port names out of user-editable data: a user names *instances*, and the class
names its ports.

The exception is the private range `0x80`–`0xFF`, where nobody else has the
documentation. Those classes must enumerate their ports in `DescribeResponse` or
they cannot be wired by anything but their own vendor's software.

### Addressing a port

Four fields, and the first is what makes it work anywhere in the tree:

| | |
| --- | --- |
| `path` | downward, relative to the node holding the link |
| `classId` | |
| `instanceId` | unique within the node |
| `portId` | 0 is the primary value |

## 2. Types are quantities, not primitives

`Float → Float` is not a type check. A hygrometer's 44.0 and a thermometer's
21.5 are both `f32`, and a UI that matches on primitives will happily let a user
wire humidity into a heating setpoint and produce something that looks connected.

So a port's type is a **quantity kind**:

| Id | Kind | Carried as | Unit |
| --- | --- | --- | --- |
| `0x01` | `Temperature` | Float | °C |
| `0x02` | `Humidity` | Float | % RH |
| `0x03` | `Pressure` | Float | Pa |
| `0x04` | `OnOff` | Bool | — |
| `0x05` | `Ratio` | Float | 0.0…1.0 |
| `0x06` | `Count` | Int | — |
| `0x07` | `Voltage` | Float | V |
| `0x08` | `Text` | String | — |

A link is legal when both ends carry the same kind. Nothing converts, ever —
which is only safe because the canonical-units rule already guarantees °C and
pascals everywhere on the wire. Matching kinds is therefore sufficient: there is
no unit negotiation left to get wrong.

## 3. Where a link lives

**A link is installed at the lowest common ancestor of its two endpoints.**

That node is the only one that both *sees* the source value and *has a route* to
the destination — and there is always exactly one of it.

The mechanism rests on an invariant of upstream routing. Each node prepends the
child it received from, so:

> after a node has prepended its hop, a climbing frame's `srcPath` is **exactly
> the downward path from that node to the origin**.

Which is the same thing a link stores. Matching a report against a link is a
byte comparison, not a computation. And because both endpoints are at or below
the LCA, **both paths in a link are ordinary downward paths** — no link anywhere
needs to address something above it.

### Every case, and what it costs

| The link | Installed at | Extra traffic |
| --- | --- | --- |
| my out → my in | **me**, internally | **none — never touches the radio** |
| child out → my in | me | none — the report already climbs to me |
| child out → another child's in | me | 1 frame per hop down the other branch |
| my out → child or grandchild in | me | 1 frame per hop down |
| child out → parent's in | the parent | none — it already climbs through me |
| parent or grandparent out → my in | that ancestor | 1 frame per hop down |
| one subtree → a sibling subtree | the ancestor above both | 1 frame per hop down |

The pattern: **upstream is free.** Reports climb whether anyone wants them or
not, so a consumer that is an ancestor of its source already holds the data and
only needs telling that it cares. Only the downward leg costs anything, and a
link's whole cost is the number of hops from the LCA to the destination.

Note the first row especially. A regulator running on its own node's thermometer
is a link whose LCA is itself: no frames, no dependency on anything, works with
the rest of the network unplugged. Hence the rule of thumb —

> **put a control loop as low in the tree as it will go.**

### Who may install one

Any node that is a common ancestor of both endpoints. It needs the model to know
both exist, and it can always address the LCA because the LCA lies below it.

The root always qualifies. So does an intermediate controller, for links inside
its own subtree — which is what lets a Heat controller with no gateway above it
wire its own sensor from its own portal.

## 4. Rules

- **An input accepts at most one link. An output may feed many.** Fan-in is a
  last-writer-wins bug generator. Enforced by the configuring node, which is the
  one holding the model.
- **Every input has a staleness timeout and a failsafe**, both defined by its
  class. A regulator whose `Measured` input has been silent for three intervals
  must fall back to something stated, not hold the last value forever. This is
  the one place in the design where being wrong burns fuel or freezes pipes, so
  it belongs in the class definition rather than in user configuration.
- **Do not put control inputs on battery nodes.** Delivery is queued to the
  node's next wake, so the loop's latency is a whole report cycle. The protocol
  permits it; a class that expects to be driven should say not to.
- **Links carry values, never commands.** A delivery is a `WriteRequest` with no
  `ACK_REQ`: acknowledging every reading would double the traffic to confirm
  something the staleness timer already measures better.

## 5. Messages

| Code | Name | |
| --- | --- | --- |
| `0x50` | `SetLinkRequest` | master → LCA |
| `0x51` | `SetLinkResponse` | |
| `0x52` | `ClearLinkRequest` | |
| `0x53` | `ClearLinkResponse` | |
| `0x54` | `ListLinksRequest` | so a UI can show what is already wired |
| `0x55` | `ListLinksResponse` | paginated with `MORE` |

### 5.1 SetLinkRequest — 19 bytes

| Size | Field | |
| --- | --- | --- |
| 1 | linkId | 0…15, the slot to occupy; overwrites |
| 1 | srcPathLen | |
| 5 | srcPath | downward from the receiving node; 0 = the node itself |
| 1 | srcClassId | |
| 1 | srcInstanceId | |
| 1 | srcPortId | must be an **out** |
| 1 | dstPathLen | |
| 5 | dstPath | downward from the receiving node |
| 1 | dstClassId | |
| 1 | dstInstanceId | |
| 1 | dstPortId | must be an **in** |

Refused with `NoSuchPort`, `TypeMismatch` (the quantity kinds differ),
`InputBusy` (that input already has a link) or `NoLinkSlot`.

A node validates what it can: it knows its own ports exactly, and it knows the
classes of its direct children. Ports further down it must take on trust from the
configuring node, which has the descriptors — so a link into a grandchild is
checked where the model is, not where the wire is.

### 5.2 SetLinkResponse

`result`, `linkId`.

### 5.3 ClearLinkRequest / ClearLinkResponse

`linkId` — `0xFF` clears every link on that node. Response: `result`.

### 5.4 ListLinksRequest / ListLinksResponse

Request: `fromPage`. Response: `count`, `pageIndex`, `pageCount`, then that many
records, each the 19 bytes of §5.1.

## 6. Delivery

When the LCA sees a value on a linked source — a report passing through, or one
of its own instances producing — it copies the value into a `WriteRequest`
addressed by the link's `dstPath`, and sends it. A destination path of length 0
means the link never leaves the node.

The report continues upward regardless. A link is a tap, not a diversion: the
gateway still sees every reading whether or not something downstream is wired to
it.

## 7. When things break

| What happened | What the link does |
| --- | --- |
| the LCA loses power | the link stops — it lives there, and nowhere else |
| the destination is unreachable | `RouteError` to the LCA; deliveries keep being attempted, the input goes stale |
| the source falls silent | nothing is delivered; the destination's staleness timeout fires and its failsafe takes over |
| the destination is asleep | queued to its next wake, like anything else |
| a node is re-bound elsewhere | its path changes, so the configuring node rewrites the links that referenced it — it recognises the device by the MAC in `ChildAttached` |

The middle row is the one to design applications around: **a link cannot promise
delivery, only attempt it.** Everything that matters therefore hangs off the
destination's staleness timeout, which is the only mechanism that notices all
five failures with one timer.

---

## 8. Worked example

```
Gateway ──1── Heat controller ──1── Thermometer (bedroom, battery)
                              └─2── Valve
```

The Heat controller carries a `Regulator` instance (`0x30`, control-function
range): `Measured` and `Setpoint` in, `Demand` out. The user wants the bedroom
thermometer to drive it, and configures that from the gateway's UI.

**The LCA is the Heat controller** — the thermometer is below it, the regulator
is on it. So the gateway does not relay anything; it installs a link and steps
out of the way.

**① SetLinkRequest** — gateway → Heat controller, 37 bytes. The gateway resolves
hop 1 itself, so the frame arrives with an empty destination path, meaning
*for you*:

```
48 41 01 50 01  31 00  00  00 00 00 00 00  00 00 00 00 00
   │  │  │  │           └── pathLens: dest 0 — the receiver is the target
   │  │  │  └────────────── flags: ACK_REQ
   │  │  └───────────────── type SetLinkRequest
-- payload ---------------------------------------------------------------
00                       link slot 0
01 01 00 00 00 00        srcPathLen 1, srcPath [1] — its own child
01 00 00                 Thermometer, instance 0, out port 0
00 00 00 00 00 00        dstPathLen 0 — itself
30 00 00                 Regulator, instance 0, in port 0 (Measured)
```

**② SetLinkResponse** — Heat controller → gateway, `00 00`: accepted in slot 0.
The kinds matched, both are `Temperature`.

**③ Afterwards, every minute**, with the gateway doing nothing at all:

```
thermometer ── Report ──► Heat controller
                          srcPath after prepend = [1]
                          matches link 0 exactly
                          → Regulator.Measured = 21.5 °C   (no radio)
                          → and the report still climbs
Heat controller ── Report ──► Gateway
```

The regulation runs on the Heat controller, one hop from its sensor. **Unplug
the gateway and the heating carries on** — it was never in the loop, it only
wired it.

### The sibling case, for contrast

```
Gateway ──1── Heat controller
        └─2── Door controller ──1── Thermometer
```

Same intent, worse topology. The LCA is now the **gateway**, so the link is
installed there and every reading costs one extra frame down branch 1 — and the
control loop stops when the gateway does.

It works, and the protocol will let you build it. But it is the argument, in one
diagram, for binding a sensor beneath the controller that depends on it.
