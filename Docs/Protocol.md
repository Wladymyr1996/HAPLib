# HAP — wire protocol, version 1

Byte layouts, the message catalogue, and worked examples. For *why* it is shaped
this way, read [HowItWorks.md](HowItWorks.md) first. For what individual classes
report, see [Classes/](Classes/).

Version 1, and implemented. Every worked example in §8 is rebuilt from the code
by `Tests/HAPSpecTest.cpp` and compared against the bytes printed here, so a
difference between this document and the library is a test failure rather than a
surprise on a bench.

---

## 1. Conventions

- **Little-endian** for every multi-byte integer, which is what both ends are.
- `u8`, `u16`, `u32`, `i32` — unsigned/signed integers. `f32` — IEEE-754 single.
- Offsets are decimal, byte values hexadecimal.
- A field marked *reserved* is written as zero and ignored on receipt. That is
  what lets version 1 grow without a version 2.
- All lengths are in bytes, never characters: a name is UTF-8, so "Кімната" is
  14 bytes and 7 characters.

### Encodings used throughout

**NAME** — a user-visible string.

| Size | Field |
| --- | --- |
| 1 | length, 0…31 |
| n | UTF-8 bytes, **no** terminator |

**VALUE** — the same five types `HValue` carries, so nothing needs a second
representation anywhere in the ecosystem.

| Type byte | Meaning | Body |
| --- | --- | --- |
| `0x00` | Null | — (no bytes) |
| `0x01` | Bool | `u8`, 0 or 1 |
| `0x02` | Int | `i32` |
| `0x03` | Float | `f32` |
| `0x04` | String | `u8` length + UTF-8 bytes |

**Null is not zero.** It means *no reading*: a sensor that failed, an instance
never yet measured, a policy with no deadband. A master must render it as "—",
never as 0.

## 2. Frame header

One HAP frame per ESP-NOW packet. 18 bytes of header, up to **232 bytes** of
payload.

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 2 | magic | `0x48 0x41` — `'H'`, `'A'` |
| 2 | 1 | version | `0x01` |
| 3 | 1 | type | §4 |
| 4 | 1 | flags | below |
| 5 | 2 | seq | `u16`, wraps; unique per originator for a few seconds |
| 7 | 1 | pathLens | destination length in the **high** nibble, source in the **low** |
| 8 | 5 | destPath | hop indices; bytes past the length are zero |
| 13 | 5 | srcPath | filled in as the frame climbs |
| 18 | … | payload | ≤ 232 bytes |

### Flags

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 | `ACK_REQ` | the originator wants an end-to-end acknowledgement |
| 1 | `UPSTREAM` | route toward the root, not by `destPath` |
| 2 | `MORE` | another frame of this response follows |
| 3 | `QUEUED` | delivered from a parent's queue, not sent live |
| 4–7 | reserved | zero |

`ACK_REQ` is satisfied by a **natural response** where one exists — a
`WriteResponse` is the acknowledgement of a `WriteRequest`. A bare `Ack` (`0x30`)
is only for frames that have no response of their own.

### Paths

A path is up to 5 hop indices, each 1…254. `0xFF` is reserved. Length 0 means
*me* (downstream) or *the root* (upstream).

**Downstream** — `UPSTREAM` clear, routed by `destPath`:

```
destLen == 0                 → this frame is mine
destLen  > 0                 → child = destPath[0]
                               shift destPath left one, destLen -= 1
                               send to that child
                               no such child        → RouteError(NoSuchChild)
                               child not answering  → RouteError(ChildUnreachable)
                               child is asleep      → queue it (§6)
```

**Upstream** — `UPSTREAM` set, `destPath` unused:

```
prepend the index of the child it arrived from to srcPath, srcLen += 1
have a parent  → forward to it
am the root    → this frame is mine, and srcPath is the sender's address
srcLen == 5    → drop (loop guard)
```

The child index is known from the sender's MAC, which ESP-NOW hands to the
receive callback. Nothing in the frame can lie about where it came from.

### Why 232 and not 1470

ESP-NOW v2 (IDF 5.4+) carries 1470 bytes, but a v1 device can only receive a v2
packet if it is ≤ 250 bytes. Designing to 250 keeps nodes interoperable whatever
IDF they were built against. Anything longer paginates — see `MORE`.

## 3. Constants

| Name | Value |
| --- | --- |
| `HAP_VERSION` | 1 |
| Header size | 18 bytes |
| Maximum payload | 232 bytes |
| Maximum depth | 5 hops |
| Maximum children per node | **6 at the root, 5 anywhere else** — see below |
| Maximum class instances per node | 8 |
| Maximum links per node | 16 |
| Maximum name length | 31 bytes |
| Bind window | 60 s |
| Listen window after an upstream frame | 120 ms |
| Hop retries | 3 |
| Offline after | 3 missed report intervals |
| Broadcast address | `FF:FF:FF:FF:FF:FF` |

### Why a bound node has one child fewer

ESP-NOW holds **six encrypted peers**, and every link in HAP is encrypted —
including the one going *up*. So the budget is:

| Node | Spent upward | Left for children |
| --- | --- | --- |
| the root | none | **6** |
| anything bound | 1 | **5** |

This is worth knowing because of how it fails otherwise. The sixth child of a
bound node binds *almost* correctly: the acceptance goes out — an unencrypted
peer still fits — and then the radio refuses to make that peer encrypted, the
child's `BindConfirm` is dropped below the protocol where nothing can log it,
and the only symptom is a master reporting that nobody confirmed.

The broadcast peer is unencrypted and comes out of the separate total of 20, so
it costs a child slot nothing.

## 4. Message catalogue

| Code | Name | Direction | Payload |
| --- | --- | --- | --- |
| `0x01` | `BindAnnounce` | slave → broadcast | §4.1 |
| `0x02` | `BindAccept` | master → slave | §4.2 |
| `0x03` | `BindConfirm` | slave → master | §4.3 |
| `0x04` | `ChildAttached` | node → root | §4.4 |
| `0x10` | `DescribeRequest` | master → node | §4.5 |
| `0x11` | `DescribeResponse` | node → master | §4.6 |
| `0x12` | `Report` | node → root | §4.7 |
| `0x13` | `ReadRequest` | master → node | §4.8 |
| `0x14` | `ReadResponse` | node → master | as §4.7 |
| `0x15` | `WriteRequest` | master → node | §4.9 |
| `0x16` | `WriteResponse` | node → master | §4.10 |
| `0x17` | `SetPolicyRequest` | master → node | §4.11 |
| `0x18` | `SetPolicyResponse` | node → master | §4.12 |
| `0x19` | `SetNameRequest` | master → node | §4.13 |
| `0x1A` | `SetNameResponse` | node → master | §4.14 |
| `0x20` | `Ping` | either | empty |
| `0x21` | `Pong` | either | §4.15 |
| `0x30` | `Ack` | either | §4.16 |
| `0x31` | `Nack` | either | §4.17 |
| `0x40` | `RouteError` | node → originator | §4.18 |
| `0x50` | `SetLinkRequest` | master → LCA | [Links.md §5](Links.md) |
| `0x51` | `SetLinkResponse` | node → master | [Links.md §5](Links.md) |
| `0x52` | `ClearLinkRequest` | master → LCA | [Links.md §5](Links.md) |
| `0x53` | `ClearLinkResponse` | node → master | [Links.md §5](Links.md) |
| `0x54` | `ListLinksRequest` | master → node | [Links.md §5](Links.md) |
| `0x55` | `ListLinksResponse` | node → master | [Links.md §5](Links.md) |

### 4.1 BindAnnounce

Broadcast, unencrypted, by a slave whose Bind button was pressed.

| Size | Field | |
| --- | --- | --- |
| 1 | deviceType | `0x00` sensor · `0x01` controller · `0x02` gateway |
| 1 | capabilities | bit 0 battery-powered · bit 1 can be a master |
| 2 | reportIntervalSec | `u16`; 0 = event-driven only |
| 2 | descriptorRev | `u16` CRC-16 of the descriptor |
| 1 | instanceCount | total across all pages, ≤ 8 |
| 1 | pageIndex | 0-based |
| 1 | pageCount | |
| n | NAME | the node's user name |
| … | instance descriptors | as many as fit |

**Instance descriptor** — the same 4 + NAME everywhere it appears:

| Size | Field | |
| --- | --- | --- |
| 1 | classId | §5 |
| 1 | instanceId | 0…7, unique **within the node**, not within the class |
| 1 | flags | bit 0 writable · bit 1 event-driven |
| 1 | valueType | the VALUE type this instance reports |
| n | NAME | the instance's user name |

The node name repeats on every page, so a page can be parsed on its own.

### 4.2 BindAccept

Unicast to the announcing MAC, unencrypted — it carries the key that makes
everything after it encrypted.

**The peer becomes encrypted the moment this frame has left the radio**, not
when it was handed over and not on a timer: earlier and the accept itself is
encrypted, later and the confirmation is dropped by the radio for arriving
encrypted at a peer that is not. See [HowItWorks.md](HowItWorks.md#binding).

The `channel` field exists because the announcing node was probably sweeping and
has no other way to learn where the network lives.

| Size | Field | |
| --- | --- | --- |
| 1 | childIndex | 1…6, assigned by the master |
| 1 | channel | the Wi-Fi channel the network runs on |
| 16 | linkKey | the ESP-NOW LMK for this parent↔child link |
| n | NAME | the master's user name, for the slave's screen |

### 4.3 BindConfirm

Encrypted with the new key — which is what proves the key arrived intact.

| Size | Field |
| --- | --- |
| 2 | descriptorRev |

### 4.4 ChildAttached

Sent upstream by a parent when a child binds or reattaches, so the root does not
have to wait for a sleeping node's first report.

| Size | Field | |
| --- | --- | --- |
| 1 | childIndex | relative to the sender |
| 1 | deviceType | |
| 1 | capabilities | |
| 2 | reportIntervalSec | from the child's announcement |
| 2 | descriptorRev | |
| 6 | mac | the child's MAC — its permanent identity |

`reportIntervalSec` is here because a `BindAnnounce` is only ever heard by the
node that adopts it. Without it the root has nothing to measure a silence
against, and "offline after three missed reports" would only work one hop down.

The root's address for that child is the frame's accumulated `srcPath` with
`childIndex` appended.

### 4.5 DescribeRequest

| Size | Field | |
| --- | --- | --- |
| 1 | fromPage | 0 for the whole descriptor |

### 4.6 DescribeResponse

`MORE` set on every frame but the last.

| Size | Field |
| --- | --- |
| 2 | descriptorRev |
| 1 | instanceCount |
| 1 | pageIndex |
| 1 | pageCount |
| n | NAME — the node's name |
| … | instance descriptors |

### 4.7 Report — and ReadResponse

| Size | Field |
| --- | --- |
| 2 | descriptorRev |
| 1 | count |
| … | entries |

**Entry:** `u8` classId, `u8` instanceId, `u8` portId, VALUE.

**On `portId`.** Every message that names a value names a port with it. Port 0 is
the instance's primary value — what a single-valued class like `Thermometer`
reports, and the only port such a class has — so a device that never heard of
ports simply writes 0 everywhere and is correct. Classes with more to say
(a `Regulator` has `Measured` and `Setpoint` in, `Demand` out) use the rest.
Ports are defined by the class and never transmitted: see [Links.md](Links.md).

### 4.8 ReadRequest

| Size | Field | |
| --- | --- | --- |
| 1 | classId | `0xFF` = every class |
| 1 | instanceId | `0xFF` = every instance of that class |
| 1 | portId | `0xFF` = every out port |

### 4.9 WriteRequest

| Size | Field | |
| --- | --- | --- |
| 1 | classId | |
| 1 | instanceId | |
| 1 | portId | must be an **in** port |
| … | VALUE | |

### 4.10 WriteResponse

| Size | Field | |
| --- | --- | --- |
| 1 | result | §7 |
| 1 | classId | |
| 1 | instanceId | |
| 1 | portId | |
| … | VALUE | **what was actually taken**, which may not be what was asked |

### 4.11 SetPolicyRequest

| Size | Field | |
| --- | --- | --- |
| 1 | classId | |
| 1 | instanceId | |
| 1 | portId | the out port being throttled |
| 2 | intervalSec | `u16`; 0 = only on change |
| … | VALUE deadband | Null for none |

### 4.12 SetPolicyResponse

`result`, `classId`, `instanceId`, `portId`, `intervalSec`, VALUE deadband — the
policy in force after the request, which a node may clamp to what its battery
allows.

### 4.13 SetNameRequest

| Size | Field | |
| --- | --- | --- |
| 1 | target | `0x00` the node · `0x01` an instance |
| 1 | classId | ignored when target is the node |
| 1 | instanceId | ignored when target is the node |
| n | NAME | |

### 4.14 SetNameResponse

| Size | Field | |
| --- | --- | --- |
| 1 | result | |
| 2 | descriptorRev | **recomputed** — the name is part of it |

### 4.15 Pong

| Size | Field |
| --- | --- |
| 2 | descriptorRev |

### 4.16 Ack

| Size | Field |
| --- | --- |
| 2 | the acknowledged seq |

### 4.17 Nack

| Size | Field |
| --- | --- |
| 2 | the rejected seq |
| 1 | reason, §7 |

### 4.18 RouteError

| Size | Field | |
| --- | --- | --- |
| 2 | seq | of the frame that could not be forwarded |
| 1 | failedHop | the child index that could not be reached |
| 1 | reason | §7 |

Sent back toward the originator — downstream failures travel up, and the
`srcPath` of the failed frame says where to send it.

## 5. Class identifiers

| Range | For |
| --- | --- |
| `0x01`–`0x0F` | environmental sensors |
| `0x10`–`0x1F` | actuators and binary inputs |
| `0x20`–`0x2F` | node health |
| `0x30`–`0x3F` | control functions — classes with real inputs, such as `Regulator` |
| `0x80`–`0xFF` | private to a vendor; never interoperable |

| Id | Class | Value | Unit | Access |
| --- | --- | --- | --- | --- |
| `0x01` | [Thermometer](Classes/ThermometerClass.md) | Float | °C | read |
| `0x02` | [Hygrometer](Classes/HygrometerClass.md) | Float | % RH | read |
| `0x03` | [Barometer](Classes/BarometerClass.md) | Float | Pa | read |
| `0x10` | Switch | Bool | — | read |
| `0x11` | Lamp | Bool | — | read/write |
| `0x12` | Door | Bool | open = true | read |
| `0x20` | [BatteryState](Classes/BatteryStateClass.md) | Float | 0.0…1.0 | read |

Units are canonical on the wire — **°C and pascals, always**. Fahrenheit and
millimetres of mercury are display decisions, made where the value is displayed.

**Value** and **Unit** above describe **port 0**, which is every class's primary
value and the type its descriptor declares. A class may carry more:
`BatteryState` also reports volts on out port 1, and a `Regulator` takes two
inputs. What ports a class has is [Classes/](Classes/)'s business, never the
wire's.

## 6. Delivery to a sleeping child

A parent that must send to a child whose `capabilities` say battery-powered does
not send. It **queues**, and delivers on the next upstream frame from that child:

1. the child wakes and sends `Report`;
2. the parent replies with the queued frame, `QUEUED` set;
3. the child's listen window (120 ms) is open long enough to receive it;
4. the child answers, and that answer climbs as any upstream frame does.

**A parent never answers on behalf of a child.** No proxy `Ack`, no cached value
passed off as fresh. A command to a sleeping node is *pending* until the node
itself says otherwise, and the originator must be able to see that.

Queue depth is one frame per child in version 1. A second frame for a child that
already has one queued is refused with `Nack(Busy)` — better an honest refusal
than a queue that reorders commands nobody is watching.

## 7. Result and reason codes

| Code | Meaning |
| --- | --- |
| `0x00` | Ok |
| `0x01` | NoSuchChild |
| `0x02` | ChildUnreachable |
| `0x03` | NoSuchClass — no such class/instance on this node |
| `0x04` | NotWritable |
| `0x05` | BadValue — right type, impossible value |
| `0x06` | BadRequest — malformed, or a type that does not belong |
| `0x07` | NotBound |
| `0x08` | Busy — queue full, or a bind window already in use |
| `0x09` | Unsupported — a message type this node does not implement |
| `0x0A` | NoRoom — no encrypted peer slot left for another child |
| `0x0B` | NoSuchPort |
| `0x0C` | TypeMismatch — the two ports carry different quantity kinds |
| `0x0D` | InputBusy — that input already has a link |
| `0x0E` | NoLinkSlot — the link table is full |

---

## 8. Worked examples

The network throughout:

```
Gateway ──1── Heating controller ──2── Thermometer (bedroom, battery)
                                  └─4── Lamp (hall)
```

Every byte below is checked against the implementation by the host tests, with
two illustrative exceptions: the **link key**, which is random, and
**`descriptorRev` (`0x9C4A`)**, which a node computes from its own content. A
real board will carry a different revision and the same everything else.

### 8.1 Binding a thermometer to the controller

The user presses **Bind** on the controller, then **Bind** on the thermometer.

**① BindAnnounce** — thermometer → `FF:FF:FF:FF:FF:FF`, unencrypted, 60 bytes.

```
48 41                    magic 'H' 'A'
01                       version 1
01                       type BindAnnounce
00                       flags — none: not upstream, it has no parent yet
01 00                    seq 1
00                       pathLens: dest 0, src 0
00 00 00 00 00           destPath
00 00 00 00 00           srcPath
-- payload ---------------------------------------------------------------
00                       deviceType sensor
01                       capabilities: battery-powered
3C 00                    reportInterval 60 s
4A 9C                    descriptorRev 0x9C4A
03                       3 instances
00 01                    page 0 of 1
07 42 65 64 72 6F 6F 6D  name "Bedroom"
01 00 00 03 04 54 65 6D 70   Thermometer,  inst 0, —, Float, "Temp"
02 01 00 03 03 48 75 6D      Hygrometer,   inst 1, —, Float, "Hum"
20 02 00 03 03 42 61 74      BatteryState, inst 2, —, Float, "Bat"
```

Note `instanceId` — 0, 1, 2. It is unique **within the node**, not within the
class, so `(classId, instanceId)` addresses one thing unambiguously and a second
thermometer on the same node would simply be instance 3.

**② BindAccept** — controller → thermometer, unicast, unencrypted, 47 bytes.

```
48 41 01 02 00  05 00  00  00×5  00×5      header, seq 5
-- payload ---------------------------------------------------------------
02                       childIndex 2 — the controller's 2nd child
01                       channel 1
7A 1F … (16 bytes)       linkKey, this link's LMK
0A 48 65 61 74 69 6E 67 20 30 31    name "Heating 01"
```

Both nodes now add each other as encrypted ESP-NOW peers with that key.

**③ BindConfirm** — thermometer → controller, **encrypted**, 20 bytes.

```
48 41 01 03 02  02 00  00  00×5  00×5      header: flags UPSTREAM, seq 2
4A 9C                                       descriptorRev
```

That it decrypts at all is the proof the key arrived intact. The controller
stores MAC + index, closes its bind window, and the thermometer stores parent
MAC + index + key — for life, or until a factory reset.

**④ ChildAttached** — controller → gateway, so the root hears about it now
rather than in a minute.

```
48 41 01 04 02  06 00  00  00×5  00×5      as sent by the controller
-- payload ---------------------------------------------------------------
02                       childIndex 2
00 01                    deviceType sensor, battery-powered
3C 00                    reportInterval 60 s, from its announcement
4A 9C                    descriptorRev
24 6F 28 AA BB CC        the thermometer's MAC
```

The gateway receives it with `srcLen` 1 and `srcPath = [1]`, so the new node is
at `1` + `2` = **`1.2`**, and it has a rev it has never seen — a `DescribeRequest`
is now due.

### 8.2 A report going up

The thermometer wakes, measures 21.5 °C and 44 % RH, and sends.

**① thermometer → controller**, 37 bytes:

```
48 41 01 12 02  07 01  00  00×5  00×5
   │  │  │  │    │     └── pathLens: both empty
   │  │  │  │    └──────── seq 0x0107
   │  │  │  └───────────── flags: UPSTREAM
   │  │  └──────────────── type Report
   │  └─────────────────── version
-- payload ---------------------------------------------------------------
4A 9C                       descriptorRev
02                          2 entries
01 00 00 03 00 00 AC 41     Thermometer inst 0, port 0 = Float 21.5
02 01 00 03 00 00 30 42     Hygrometer  inst 1, port 0 = Float 44.0
```

`00 00 AC 41` is 21.5f little-endian (`0x41AC0000`); `00 00 30 42` is 44.0f.

**② controller → gateway** — the identical frame, with one byte changed and one
byte written:

```
48 41 01 12 02  07 01  01  00×5  02 00 00 00 00
                       ▲         ▲
                       │         └── srcPath = [2]: it arrived from child 2
                       └──────────── pathLens: dest 0, src 1
```

**③ the gateway receives** it from its child 1 and prepends that hop:

```
                       02  00×5  01 02 00 00 00
                       ▲         ▲
                       │         └── srcPath = [1, 2]
                       └──────────── src length 2
```

The reading came from `1.2`. The gateway never had to be told the network's
shape — the frame drew its own return route on the way up. And the controller
read the temperature as it passed, which is how it drives its heating without
asking anyone.

### 8.3 A command going down

The gateway turns on the hall lamp at `1.4`.

**① gateway → controller.** The gateway resolves the first hop itself — hop 1 is
its own child — so the frame it emits already carries the remainder:

```
48 41 01 15 01  20 00  10  04 00 00 00 00  00×5
   │  │  │  │           │  ▲
   │  │  │  │           │  └── destPath = [4]
   │  │  │  │           └───── pathLens: dest 1, src 0
   │  │  │  └──────────────── flags: ACK_REQ
   │  │  └─────────────────── type WriteRequest
-- payload ---------------------------------------------------------------
11 00 00                 Lamp, instance 0, in port 0
01 01                    Bool true
```

**② controller → lamp.** Hop 4 is its child; the path is now empty, which means
*for you*:

```
48 41 01 15 01  20 00  00  00 00 00 00 00  00×5
                       ▲
                       └── dest length 0 — the destination is the receiver
```

**③ lamp → gateway**, `WriteResponse`, climbing as any upstream frame does and
arriving with `srcPath = [1, 4]`:

```
48 41 01 16 02  20 00  02  00×5  01 04 00 00 00
-- payload ---------------------------------------------------------------
00                       result Ok
11 00 00                 Lamp, instance 0, port 0
01 01                    Bool true — what was actually taken
```

The response *is* the acknowledgement `ACK_REQ` asked for; no separate `Ack`
follows. Had the controller's child 4 been unplugged, the controller would have
answered `RouteError(seq 0x0020, failedHop 4, ChildUnreachable)` instead — and
the gateway would know exactly which link broke rather than merely that
something timed out.

### 8.4 Renaming a sleeping sensor

The user renames instance 0 of `1.2` from "Temp" to "Ліжко". The node is asleep
and will be for another 40 seconds.

```
gateway ── SetNameRequest ─► controller        dest [2], ACK_REQ
                             child 2 is battery-powered → QUEUED
                             (nothing is acknowledged: the parent
                              never answers for a child)

        …40 s later, the thermometer wakes…

controller ◄── Report ──── thermometer         rev 0x9C4A, as always
controller ── SetNameRequest ─► thermometer    flags QUEUED, inside the
                                               120 ms listen window
                       payload: 01 01 00 0A D0 9B D1 96 D0 B6 D0 BA D0 BE
                                │  │  │  └── NAME "Ліжко", 10 bytes / 5 chars
                                │  │  └───── instanceId 0
                                │  └──────── classId 0x01
                                └─────────── target: an instance

thermometer ── SetNameResponse ─► … ─► gateway
                       payload: 00 E2 17     Ok, descriptorRev now 0x17E2
```

Three things worth noticing:

- **the rename cost one report cycle**, exactly like any other command to a
  sleeping node, and the gateway's UI showed it as *pending* until the node
  itself confirmed;
- **the rev changed**, because names are part of the descriptor. The gateway
  sees `0x17E2` where it had `0x9C4A` cached and schedules a `Describe` — which
  will itself be queued for the next wake;
- **nothing else in the network had to be told anything.** The controller does
  not care what its child's instances are called, and no node between here and
  the root holds a copy that could go stale.
