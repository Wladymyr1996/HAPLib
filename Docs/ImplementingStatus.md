# HAPLib — implementation status

What exists, what is next, and what is known to be unresolved. The protocol
itself is [Protocol.md](Protocol.md), [HowItWorks.md](HowItWorks.md) and
[Links.md](Links.md); this file tracks how much of it is real. What gets checked
on two real boards, and how, is [TestPlan.md](TestPlan.md).

**Last updated:** 2026-08-19 · **Phases 0-9 complete and validated on three
boards. Eleven faults found there, all fixed — see "What hardware has proved".**

---

## The plan

Host-first, and each phase verifiable before the next depends on it.

| # | Phase | Delivers | Status |
| --- | --- | --- | --- |
| 0 | Skeleton | component, umbrella header, test harness | **done** |
| 1 | Codec | `HAPPath`, `HAPCodec`, `HAPFrame`, `HAPMessages` | **done** |
| 2 | Router + simulator | `HAPRouter`, `HAPLoopback`, child table | **done** |
| 4 | ESP-NOW | `HAPEspNow`, peers, channel, broadcast | **validated** — 2026-08-18 on three boards; see below | 
| 5 | Bind | `HAPBinder`, key generation, channel sweep, `HAPStore` | **done** |
| 6 | Report + sleep | `HAPReporter`, `HAPQueue`, `HAPListenWindow` | **done** — the `HSleep` wiring belongs to an application (phase 10) |
| 7 | Master model | `HAPModel`, lazy Describe, offline detection | **done** |
| 8 | Links | `HAPLinks`, matching, delivery, staleness | **done** |
| 9 | **Stack** | `HAPStack` — the facade that wires the pieces together | **done** |
| 10 | **Application** | REST routes, WebUi screens, HTHP as a real slave node | **not started, and not library** — after the rig |

Hardware enters at phase 4. Three bare ESP32-C6 boards are available, which is
the number that makes a tree rather than a link - see TestPlan.md.

### Where the line between library and application falls

Phases 0-9 are HAPLib. **Phase 10 is not**, and is listed here only because this
file tracks the project rather than the library.

Everything HAPLib builds is a mechanism with no opinion about a product: a
router that says where a frame goes, a binder that runs a handshake, a model
that caches what it hears. What it deliberately does not contain is any device's
idea of what to do with those - which screen to draw, which REST route to
publish, when to sleep, what a button means. That is what
`HCoreLib`'s rule "the library never depends on the application" buys, and HAPLib
keeps to it.

So phase 10 lives in `App/` here, and in `main/` in the test rig: registering
HAP routes with `HRestApi`, drawing HAP state on the panel, calling
`maySleep()` before sleeping, and giving this thermometer a `HAPNode` with its
real instances.

**Phase 9 was missing from the original plan**, and writing phases 5-8 is what
exposed it: every test was hand-rolling the same twenty lines - decode, offer to
the binder, else route, else deliver locally - because nothing in the library
assembled the pieces. That wiring is identical on every device in the ecosystem,
which made it library work by the same rule that put the portal in HCoreLib.

## The library is finished

Nothing in `Protocol.md`, `HowItWorks.md` or `Links.md` is now unimplemented,
and `HAPStack` is the whole of it in one object:

```cpp
HAPEspNow radio;
HAPNode node;
HAPStack stack(radio, node);

node.begin(HAPDeviceType::Sensor, HAPCaps::BatteryPowered, "Bedroom");
node.addInstance(HAPClassId::Thermometer, "Temp");
stack.begin(1);

for (;;) {
  stack.update();          // every HCORELIB_TICK_MS
}
```

**What is left is the rig.** `HAPEspNow` is still the one module with no test,
because a radio cannot be simulated honestly - and everything above it now has a
counterpart in `Tests/HAPStackTest.cpp` built from the same code the boards will
run, which is what makes a bench failure *informative* rather than a starting
point. See [TestPlan.md](TestPlan.md) §11 for which milestone maps to which
test.

## What is built

| Module | Does |
| --- | --- |
| `HAP.h` | the specification's vocabulary in code: wire constants, message codes, result codes, class ids, quantity kinds |
| `HAPPath` | the 5-hop relative address, with the `shift()` / `prepend()` pair the whole routing design rests on |
| `HAPCodec` | bounds-checked reading and writing of every primitive: little-endian integers, IEEE-754 floats, UTF-8 names, HValue-compatible values |
| `HAPFrame` | the 18-byte header, encode and decode, with a payload that is a view rather than a copy |
| `HAPMessages` | all 26 payloads as typed structures, grouped into bind, data, control and link headers |
| `HAPMac` | an address that can be compared, copied and printed by value |
| `HAPITransport` | the radio seam, with a memory-only backend on the host |
| `HAPRouter` | parent, children, and the two forwarding rules — no radio, no timers, no state machine |
| `HAPCrc16` | CRC-16/CCITT-FALSE, checked against the published vector |
| `HAPClasses` | `Docs/Classes/` as code: every class, its ports, their quantity kinds |
| `HAPInstance` | one live instance: its name, and what its ports currently read |
| `HAPNode` | this device — instances, revision, paging, and the answer to every request |
| `HAPEspNow` | the real radio, target only — the one module here with no test |
| `HAPRandom` | link keys: the hardware generator on target, a repeatable one on the host |
| `HAPBinder` | both halves of the handshake, including the channel sweep |
| `HAPStore` | a bind that survives a reboot: three config files and an RTC mirror |
| `HAPReporter` | whether a reading is worth a transmission: intervals and deadbands |
| `HAPQueue` | one frame held per sleeping child, refused rather than stacked |
| `HAPListenWindow` | the short moment after speaking when a sleeping node is reachable |
| `HAPModel` | what the root knows about everything beneath it, built only by listening |
| `HAPLinks` | the wires this node holds, and the byte comparison that fires them |
| `HAPStack` | all of the above as one object — what an application actually holds |

Eight decisions worth knowing about before reading the code:

- **The deadband decides whether to SPEAK, not what to say.** Once anything is
  due, a report carries every out port: the transmission is the expensive part
  and it has already been paid for, so a master gets a consistent snapshot
  rather than whichever single value happened to move. The one thing no deadband
  may suppress is a reading appearing or disappearing — Null is not a small
  change from 21.5.
- **Sticky failure.** Neither the reader nor the writer returns a bool. The
  first overflow latches, everything after it is a no-op, and a caller checks
  `ok()` once at the end. Bytes arriving from a radio are attacker-shaped by
  definition, so "cannot read past the buffer" is a property of the class rather
  than of whoever writes the next message type.
- **The payload is not copied.** A decoded frame points into the buffer it was
  decoded from, so forwarding costs no memcpy — at the price of a lifetime rule:
  read from the receive buffer, encode into a different one.
- **Anything holding an `HValue` is constructed, never assigned.** `HValue`
  fixes its type at construction and *coerces* on assignment, so a struct with
  one inherits that from its generated `operator=` — a decoded 21.5 landing on a
  default-constructed entry would read back as Null. The decoders build fresh
  objects and hand them back by value; `setValue()` is the only correct way to
  replace one in place. There is a test named after this trap.
- **The simulated radio is deliberately unhelpful.** `HAPLoopback` enforces what
  ESP-NOW enforces: a unicast to a non-peer fails, two nodes on different
  channels are simply deaf to each other, a frame encrypted with one key is not
  received by a peer holding another, and nothing is delivered until the test
  pumps the bus. Each of those has cost somebody a day with two boards and a
  serial log; each is now a test that runs in a millisecond.
- **The RTC mirror holds exactly what a wake needs to transmit** — the link
  upward, and deliberately not the children or the names. A battery node wakes,
  measures, reports to its parent and sleeps; mounting a filesystem to look up
  the parent's address would cost more than the measurement did. A master is
  mains-powered and can afford the file.
- **Nothing happens in the radio's callback.** ESP-NOW delivers on the Wi-Fi
  task, so `HAPEspNow` copies the frame into a queue and returns — no parsing,
  no logging, no callbacks, no blocking. `update()` drains it on the task that
  owns the transport. That is why `HAPEspNow` has an `update()` the interface
  does not: the simulated transport has nothing to drain.
- **A port number means nothing without a direction.** A lamp's state is out
  port 0 *and* in port 0 — the same lamp from two sides. Nothing on the wire
  ever names a port without saying which way it faces (a report names outs, a
  write names ins, a link names one of each), so the two id spaces are separate
  and a class's ports read the way its documentation does.

## Tests

`Tests/` is a standalone CMake project — the real HCoreLib and HAPLib sources, no
reimplementation:

```
cmake -S HAPLib/Tests -B build-haptests
cmake --build build-haptests
ctest --test-dir build-haptests --output-on-failure
```

**1098 checks, all passing.** Fourteen suites: `HAPPath`, `HAPCodec`, `HAPFrame`,
`HAPMessages`, `HAPRouter`, `Simulated tree`, `HAPNode`, `HAPBinder`,
`HAPStore`, `HAPReporter, HAPQueue, HAPListenWindow`, `HAPModel`, `HAPLinks`,
`HAPStack` and `Documented examples`.

**`HAPLinks` ends with the argument the whole design was built around.** Its
last test puts a thermometer one hop below a controller, installs the link at
the controller — the lowest common ancestor — watches a real report climb, feed
the regulator *and* carry on to the gateway, and then cuts the gateway off and
watches the loop keep running. A tap, not a diversion, and a control loop that
does not depend on the root.

**`HAPModel` is the root's whole picture, built without anybody describing the
network to it.** A report arriving from `1.2` is the discovery; the revision it
carries says whether what is cached is still true; `Describe` is pulled only for
what turns out to be unknown. The suite covers the cases that make that hold up:
a stale descriptor staying readable while it waits to be refreshed, a
half-arrived paginated descriptor not counting as one, a device recognised by
MAC at a new path being *moved* rather than duplicated, and a node that promised
a report interval going offline after three missed ones — while a node that
promised nothing never does, because inventing a fault from silence is worse
than saying nothing.

**`Simulated tree` is the one the transport seam exists for.** Gateway,
controller, thermometer and lamp, each with its own router and radio on a shared
in-memory bus, inside one process — and with it, the questions two boards can
never answer: a report climbing two hops and arriving with `src = 1.2` that
nobody was told, a command descending to `1.4`, a dead middle node hiding its
whole subtree, a missing child answered with `NoSuchChild` rather than a
timeout, a five-deep chain filling the path exactly, and the three radio
failures — stranger, channel, key — that look identical from the far end.

`Documented examples` is the other half. Every worked example in `Protocol.md`
and `Links.md` is rebuilt **from the message structures** and compared against the hexadecimal
those documents print — the bind handshake, a report climbing two hops with its
source path building itself, a command descending, a rename, a link
installation. Each then goes the other way: the document's own bytes are decoded
and checked field by field, because an encoder and a decoder that share a
misreading of the specification would agree with each other perfectly and only
the document catches that.

A protocol document full of byte dumps is worth exactly as much as the last time
somebody checked them by hand, and now nobody has to.

**`HAPBinder` and `HAPStore` run the two things a user actually does.** Ten bind
tests on the deliberately-unhelpful bus, so a passing handshake has established a
*working encrypted link* — one of them sends a ping afterwards to prove it —
rather than merely exchanging the right message types. Then nine storage tests
against the real HConfig on the real filesystem: a bind restored from the file,
a bind restored from the mirror, a factory reset that clears **both**, children
and names round-tripping, and a half-written bind refused rather than believed.

It has already earned its keep four times:

- **A defect in the specification.** `Protocol.md` §8.1 declared instance id 0
  three times while §8.2 reported instance 1 for the same hygrometer. The
  document was wrong and has been corrected.
- **A defect in the code.** `HAPReader::value()` returned a truncated Float as a
  genuine `0.0` instead of Null — turning a damaged frame into a confident
  reading of zero degrees, which is the exact failure the "Null is not zero"
  rule in the class documents exists to prevent.
- **A race in the handshake.** The first run of the bind suite failed twenty
  checks for one reason: the slave's encrypted `BindConfirm` arrived before the
  master had switched its peer to encrypted, so the radio dropped it *below* the
  protocol, where nothing could log it. The key switch had been hung on a timer;
  it now hangs on the transport's send-completion report, with the timer only as
  a fallback. On hardware this is a bind that never completes with nothing in
  either log to explain why.
- **A listen window that would not close.** `HAPListenWindow::extend()` tested
  `HTimer::isRunning()`, which stays true after a timer expires — it is `stop()`
  that ends a run, not the timeout. So a frame arriving *after* the window had
  closed silently reopened it, and a battery node would have kept its radio on
  for traffic it should have slept through. That one shows up as a flat cell
  three weeks later, with nothing in any log.

## Building

The component compiles for the ESP32-C6 and is listed in the root
`CMakeLists.txt`, so it keeps building while nothing uses it yet. It links no
code into the image until an application calls something: `HTHP.bin` is byte for
byte the size it was before HAPLib existed.


---

## What hardware has proved, and what it has not

**Last hardware run: 2026-08-18**, three ESP32-C6 modules, against
[TestPlan.md](TestPlan.md) driven from `AppTest/` (see its `test_list.md` for the
keystrokes and the log evidence, milestone by milestone).

**24 of 25 milestones pass.** Phase 4 - `HAPEspNow`, written but never run - is
no longer "unvalidated": every message type in the protocol has now crossed a
real radio, through one hop and two.

### Eleven faults, and where they were hiding

Every one of these passed the host suite before the bench found it. That is the
number worth remembering when deciding what CI can be trusted to catch.

| # | Fault | Why the simulator missed it |
| --- | --- | --- |
| 1 | binder set `pending_` *after* sending the accept, losing the send report | the callback is instant in simulation; on hardware it pre-empts, and a log line was enough to open the window |
| 2 | `factoryReset()` left the radio's peer table intact - a reset node could never be re-adopted | `HAPLoopback` did not model plain text arriving at an encrypted peer. It does now |
| 3 | five response types were answered with `Nack` | nothing had ever originated a `SetPolicy` or read a `SetLink` answer |
| 4 | a master forgot its children's link keys across a reboot - the whole network deaf, every table perfect | no host test restarted a master and then checked a reading still crossed |
| 5 | `onDiscovered` fired on a blank record | no test read the record from inside the hook |
| 6 | a node was declared offline for obeying a policy it had been given | needed a policy round trip and a clock |
| 7 | a rebooted master lost the intervals its children had promised | same shape as 4: the tables looked right |
| 8 | `HAPStack::requestSetPolicy()` did not exist | the responder was tested; nothing could send one |
| 9 | a controller adopted *last* never announced its children | every test built its tree top-down |
| 10 | a factory-reset device could not rejoin the parent that still listed it | no host test reset a child and re-bound it to the same master |
| 11 | a rebooted master lost every node below its own children | the harness shares one store between simulated nodes, which hid it |

Eight of the eleven are the same underlying shape: **state that only one message ever
carries, and no way to re-derive it.** `ChildAttached` carries a child's MAC,
capabilities and interval, and is sent exactly once.

### What Stage B left open, and how it was settled

Two faults survived the run as questions rather than repairs, because both were
protocol decisions. Both are now decided and implemented - the reasoning is
below, kept because the alternatives are worth knowing about if the protocol is
ever revised.

- **A rebooted master was blind to its grandchildren.** `begin()` seeded the
  model from the child table, which holds direct children only; everything deeper
  was re-learnt from reports, which carry no MAC and no interval. Such an entry
  could neither be recognised when the device moved nor ever be marked offline.
  **Settled by persisting the model.**
- **A factory-reset device could not rejoin the parent that still listed it.**
  `handleAnnounce()` refused `already a child`, and the stale entry won over the
  node that had genuinely been reset. **Settled by re-adopting in place.**

### How the last two were settled

Both came from one cause. **`ChildAttached` is the only message carrying a
child's MAC, capabilities and interval, it is sent exactly once, and nothing can
ask for it again.** Everything below follows from that.

#### A factory-reset device now rejoins the parent that still lists it

`handleAnnounce()` refused an announcement from an address already in the child
table. But a node only announces when it is **unbound**, so such an announcement
is not ambiguous: it is proof that the child no longer believes it has a parent,
and the entry here is the stale one. The refusal let a dead record outvote a live
device, and the only way back was a factory reset of the parent - which drops its
other children too, meaning every device in that room.

It was also worse than an inconvenience: a ghost holds one of six **encrypted
peer slots**, so a parent that collects a few eventually cannot adopt anybody,
and the symptom is §8's "the accept went out and nobody confirmed".

The stale entry is now replaced, **keeping its index**. Links, paths and anything
a master has cached name a child by index; a device that came back as 1.3 instead
of 1.2 would break every reference silently, which is the failure B8 exists to
prevent. The replacement happens in `handleConfirm()` rather than at the
announcement, so a handshake that fails leaves the working child where it was.

No new exposure: an attacker inside a bind window can already bind as a new
child, and the window still needs a press on the master.

**Rejected: a `Leaving` message** sent on factory reset. It is worth having - it
would also clear the ghost left at the OLD parent when a device rebinds elsewhere
- but it is best-effort by nature, since a device reset by pulling the battery
sends nothing. Re-adoption has to exist regardless, so it went first.

#### A master now keeps what it was told about the tree below it

`hapmodel`, a fourth store file: sixteen bytes a node - path, MAC, capabilities,
device type, interval - which is 32 hexadecimal characters, exactly what
`HVALUE_MAX_STRING_LEN` was already required to hold for the link key.

Restored in `begin()` **before** the child table is seeded, so that seeding
refreshes those entries rather than creating rivals, and so that a device which
moved while the master was off is recognised by its MAC instead of appearing
twice.

Deliberately not stored: names, instances, values, `descriptorRev`. Those are a
cache. `described` stays false on restore, so the master pulls the descriptor
again rather than showing what may have changed while it was away. **Only what
somebody said, never what was inferred.**

Written only when identity changes - a node discovered, a node moved, a factory
reset - and never on a report. A report changes values and never identity;
saving on one would be a flash write every ten seconds for a value that had not
moved.

Why this over the alternatives:

| Considered | Why not |
| --- | --- |
| a `ListChildren` request | a new message and a version bump, to re-derive what the master could simply have kept |
| the MAC in every `Report` | six bytes for ever, on the one message the design keeps small, for a value that never changes |
| the MAC in `DescribeResponse` | the closest runner-up: a restarted master re-describes everything anyway, so it would heal for free. Still a wire-format change, and it does not carry the interval. Worth folding in if that message is ever revised for another reason |

**What this does not fix.** A master that has never been told - because the
notice was lost in the air, not because it forgot - still has no way to ask. The
gap is narrower than it was, and closing it completely needs a message the
protocol does not have.

#### Both were then proved on the boards

Not a substitute for the host tests, and not a formality either: neither fault
could have been found without hardware, so neither fix could be believed without
it. Run 2026-08-19 on the three-board rig, logs in `AppTest/Logs/A6-B10-B11/`.

- **A6** - factory-reset the leaf, rebind it, nothing else touched. Re-adopted at
  **the same index**, one node known rather than two, and a reading crossed
  thirty seconds later. That last clause is the one that matters: the parent's
  peer for that address held the old key, was modified to plain text to carry the
  acceptance, and modified again to the new key on the send callback. ESP-NOW
  followed all three without losing a frame.
- **B10** - power-cycle the master alone. Both nodes were listed at **270 ms**,
  two seconds before the first frame arrived, with real MACs and real intervals;
  the re-Describe went out immediately. Then, with the leaf stopped,
  `! OFFLINE 1.1 - missed 3 intervals of 10s`. An entry rebuilt from reports has
  an interval of zero, which promises nothing, which means it can never be called
  silent - the old code was structurally incapable of that line.
- **B11** - both fixes in one exchange, one hop apart: reset the leaf, rebind it
  under the middle node, and the `ChildAttached` the middle node re-originated
  landed **on** the root's restored entry, matched by MAC, rather than beside it.
  Two nodes known, not three. This is the shape a sensor reset in a room and put
  back would actually take.

`saveModel` writes nine `HConfigEntry` in one call and did not trouble the rig's
8 KB task. That is not a clean bill for a 4 KB one.

### Not tested at all

Three boards cannot reach these, and no amount of host testing substitutes for
them:

- **depth past 3.** A five-deep chain needs six boards. `HAP_MAX_DEPTH` is 5 and
  the loop guard that drops a sixth hop has only ever run in simulation;
- **fan-out.** Five children on a bound node needs six boards. The
  `childCapacity()` arithmetic - six encrypted peers, minus one for the link
  upward - is the thing that decides it, and the failure it protects against
  ("the accept went out and nobody confirmed") has never been seen;
- **B9, an hour left alone.** Not run. It is the only test that can catch a slow
  leak, a sequence number wrapping badly, or a queue that fills and never drains.
  Nothing in a five-minute session will ever show those;
- **the wake cost in milliamp-milliseconds.** Not measured. It needs a meter in
  series, and it is the one number that decides whether the reporting interval is
  survivable on a cell.

### What CI can and cannot do

The host suite is **1174 checks** and runs in eleven seconds
(`cmake -S HAPLib/Tests -B build-haptests && ctest --test-dir build-haptests`).
It is worth running on every commit and it is where every fix above is now
pinned so that none of them can come back quietly.

But it found none of them first. `HAPLoopback` is deliberately unhelpful - it
refuses to unicast to a stranger, refuses to cross channels, refuses a frame
encrypted with the wrong key, and now refuses plain text from an encrypted peer -
and it still could not produce a pre-empting send callback, a peer table that
outlives an object, or a board that reboots. **Green CI means no regression, not
a working radio.** The rig is what says the second thing, and it has to be re-run
against hardware whenever the binder, the store or the transport changes.
## Known gaps

Things the documents do not yet answer, in the order they will have to be
answered:

1. ~~**The bind channel.**~~ **Resolved in phase 5.** An unbound node sweeps
   1, 6, 11 and then the rest while it announces, dwelling on each long enough
   for a master sitting there to answer, and adopts whatever channel the
   acceptance names. `Protocol.md` still needs this written into §6.
2. **Security of the bind window.** Still the honest weak point: the announce is
   broadcast in the clear and the accept carries the link key. An install code
   printed on the device would close it, at the cost of the user typing six
   characters. Undecided.
3. **Timestamps.** Nothing carries a clock. Readings are "as of the last
   report", and if that is not good enough the gateway has to distribute time
   and every frame grows.
4. **Battery cost.** Bringing Wi-Fi up and sending one report is expected to add
   roughly as much again to a wake as the current radio-less ~250 ms, at ~100 mA.
   That may want `APP_SLEEP_INTERVAL_MS` revisited, and it is the argument for
   the RTC mirror in `HAPStore` — parent MAC, index, key and channel held in RTC
   memory so a wake can transmit without mounting LittleFS first. Measured in
   phase 6.
