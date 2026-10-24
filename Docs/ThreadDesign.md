# HAP over Thread - design

How HAPLib moves from the ESP-NOW tree described in [HowItWorks.md](HowItWorks.md),
[Protocol.md](Protocol.md) and [Links.md](Links.md) to a **star over OpenThread**
on the ESP32-C6.

**Status: design, not implemented.** The other documents still describe
protocol version 1 (the tree) and stay authoritative for the code as it is today
until they are rewritten at the end of the migration (section 17, HT-17). Where
this document says *proposal*, the point is a recommendation that has not been
confirmed yet. Where it says *HT-2*, the answer depends on findings from the
OpenThread spike on real boards, and no number here pretends to be measured.

---

## 1. Summary

- **One radio, one mesh, one master.** Every node joins one Thread network.
  Thread does the meshing, routing, retries and link encryption. HAP sits on top
  as a **star**: every node talks to the master directly, by IPv6 over UDP,
  however many radio hops Thread needs underneath.
- **The master is a HAP role**, not a Thread role. It is the single writer of the
  network model. The Thread Leader is elected by Thread on its own and has
  nothing to do with it.
- **The master is found through Thread Network Data**: it registers a
  `HAP-Master` Service, which gives it an anycast address (Service ALOC). The
  entry carries a `term`; when two nodes claim the role, the higher term wins.
- **The model is replicated** to every mains-powered node, versioned by
  `(term, revision)`, so any of them can become master.
- **The role moves** by a two-phase handover, or - when the master is gone - by
  an emergency takeover with a deterministic successor rule.
- **Configuration enters through a channel-neutral command API**: bytes in, bytes
  out, with an authorization hook that allows everything for now. UART, BLE or a
  web portal are the application's business, not the library's.
- **Gone**: hop paths, forwarding, the child table, the channel sweep, per-link
  ESP-NOW keys and the six-peer limit that shaped the whole tree.

## 2. Topology

```
                 HAP view (star)                      Thread view (mesh)

                   [Master]                     R1 ---- R2 ---- R3
                  /   |    \                    |        |       \
           [Member] [Member] [Sleepy]          SED      REED      SED
             (mains)  (mains) (battery)
```

At the HAP level there is no depth: a node is either the master or it is not,
and every frame goes **straight to its destination node**. Whether it crosses one
802.15.4 hop or four is Thread's concern, invisible above `HAPITransport`.

Two consequences replace the tree's two:

- **A dead mains node hides nothing.** Thread routes around it. In the tree, a
  dead controller took its whole subtree with it.
- **A dead master stops configuration, not control.** Reports have nowhere to go
  until a master exists again, but links (section 12) run node to node and keep
  running.

## 3. Roles

### HAP roles

| HAP role | Who | Holds | Can be master |
| --- | --- | --- | --- |
| **Master** | exactly one mains node per network | the authoritative model, the live picture (descriptors, values, liveness) | is |
| **Member** | every other mains node | a replica of the model, its own instances, the links it sources | yes, by handover or takeover |
| **Sleepy member** | every battery node | its own instances and the links it sources; no replica | **never** |

A node's eligibility is its existing `HAPCaps::CanBeMaster` bit, which is only
honoured on a node without `HAPCaps::BatteryPowered`.

### Thread roles, and why they are independent

| Thread role | Typical HAP node |
| --- | --- |
| Leader | any router; elected by Thread; may or may not be the HAP master |
| Router / REED (FTD) | mains nodes: master and members |
| Sleepy End Device (MTD, rx-off-when-idle) | battery nodes |

The Leader moves whenever Thread decides, and HAP must not care. Tying the master
to the Leader would make configuration ownership follow radio conditions, and
the model would have to move every time a router re-partitioned. The only thing
HAP uses from the Leader is what it distributes: the Network Data that carries
the master's Service entry.

**Answer to "does `HAPStack` keep a notion of depth?" - No (proposal).** It keeps
the role (master, member, sleepy member, or not yet joined) and nothing else.
`HAPStack::isRoot()` becomes `isMaster()`; `HAP_MAX_DEPTH`, `HAPPath` and every
path-shaped API go.

## 4. Identity and addressing

Two separate things that the tree conflated in `HAPMac`:

| | What it is | Lifetime | Used for |
| --- | --- | --- | --- |
| **Node id** (`HAPNodeId`) | 8 bytes, the node's factory IEEE 802.15.4 EUI-64 | permanent | identity in the model, in frames, in links; the successor tie-break |
| **Transport address** | opaque to HAP; an IPv6 address on Thread, a MAC on the loopback bus | may change | where `HAPITransport` sends bytes |

- The node id replaces the MAC as "permanent identity". It is carried **inside
  every frame** (section 5), because on Thread the sender's IPv6 source address
  is not a stable identity (RLOC-based addresses change when a node's Thread role
  changes).
- The stack learns `node id -> transport address` from received frames; it never
  needs to be told. A member can always reach the master without knowing its
  address, through the special transport address **`master`**, which the Thread
  backend maps to the `HAP-Master` Service ALOC.
- "Lowest MAC" in the successor rule means **lowest node id**, compared as an
  8-byte big-endian number.

## 5. Framing (protocol version 2)

The header loses both paths and gains both node ids. As implemented by HT-6:

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 2 | magic | `'H' 'A'`, unchanged |
| 2 | 1 | version | `0x02` |
| 3 | 1 | type | message catalogue, below |
| 4 | 1 | flags | below |
| 5 | 2 | seq | unchanged |
| 7 | 8 | dst | node id; all-zero means *the master, whoever it is* |
| 15 | 8 | src | node id of the originator |
| 23 | ... | payload | up to `HAP_MAX_PAYLOAD_SIZE` |

- **Payload budget stays 232 bytes**, so every v1 message structure,
  including the one-frame `OtaRequest`, keeps fitting unchanged. The frame grows
  to 255 bytes, which UDP does not care about. 6LoWPAN fragments anything longer
  than one 802.15.4 frame, so the *common* frames - reports - should stay small;
  how small is worth knowing from HT-2, not from guessing.
- **Flags**: `ACK_REQ` (`0x01`) and `MORE` (`0x04`) stay. `UPSTREAM` (`0x02`) is
  removed and its bit is reserved with the other unused ones (`0xF2`, written as
  zero). `QUEUED` (`0x08`) is kept as a diagnostic bit (section 11).
- **Ids on the wire** are their eight bytes in stored (canonical EUI-64) order,
  not a little-endian integer. `src` is the originator, is never zero and is not
  rewritten by a node that relays the frame. `decode()` rejects a zero `src`
  (`NoSource`), `encode()` refuses to write one, and a version 1 frame is
  `BadVersion` - there is no compatibility mode.
- **Interim tree (until HT-7):** upstream traffic still climbs hop by hop with
  `dst` zero. Nothing forwards downward any more: a frame for a node is sent
  straight to its id, except that a direct child still goes through the child
  table so a sleeping one can be queued for. A node that is part of a tree
  accepts a frame addressed to it from any sender; the network key is the
  boundary, as the trust model above says.
- A receiver drops a frame whose `dst` is neither its own id nor zero-while-I-am-
  master. That guards against a transport address that has been reassigned to
  another node. (Not enforced yet; HT-7.)

### Message catalogue changes

| v1 | v2 |
| --- | --- |
| `BindAnnounce` (broadcast) | `Enrol`: same payload, unicast to the master after Thread joining |
| `BindAccept` | `EnrolAccept`: no child index, no channel, no key; carries the master's name |
| `BindConfirm` | removed: Thread joining already proved the node holds the network key |
| `ChildAttached` | removed: the enrolling node tells the master itself |
| `RouteError` | removed (HT-6, message `0x40` is unassigned): transport send failure or an `ACK_REQ` timeout says the same |
| Describe, Report, Read, Write, SetPolicy, SetName, Ping, Ack, Nack, Ota | unchanged payloads |
| Set/Clear/ListLinks | node ids instead of paths (section 12). Interim layout from HT-6: a port reference is a node id plus class, instance and port (11 bytes, a link spec 23); a zero node means the node holding the link |
| - | new: replication, handover and takeover messages (sections 8-10), proposed range `0x70`-`0x7F` |

Result codes `NoSuchChild`, `ChildUnreachable` and `NoRoom` become reserved.
Proposed additions: `NotMaster` (a model write reached a node that is not the
master) and `StaleVersion` (a snapshot older than what the receiver holds).

### Transport interface v2

What `HAPITransport` becomes in HT-4 (sketch, not final):

```cpp
class HAPITransport {
 public:
  using Receiver   = etl::delegate<void(const HAPAddress& from, const uint8_t* data, size_t size)>;
  using SendResult = etl::delegate<void(const HAPAddress& to, bool delivered)>;

  virtual bool begin() = 0;                       // no channel
  virtual const HAPNodeId& localId() const = 0;
  virtual bool isAttached() const = 0;            // joined a network and can send
  virtual bool send(const HAPAddress& to, const uint8_t* data, size_t size) = 0;
  virtual bool multicast(const uint8_t* data, size_t size) = 0;   // mains nodes only, used by takeover
  virtual void onReceive(Receiver) = 0;
  virtual void onSendResult(SendResult) = 0;
};
```

Removed: `begin(channel)`, `channel()`, `setChannel()`, `addPeer()`,
`removePeer()`, `hasPeer()` and `broadcast()`. There is no peer table and no
per-peer key: Thread encrypts every frame with the network key.
`HAPAddress::master()` is the anycast destination. `HAPLoopback` keeps being
deliberately unhelpful, now about what Thread enforces: no delivery before
`begin()`/attach, no delivery to an unknown address, sleepy nodes that only
receive while polling.

`HAPEspNow` cannot honour this interface (its six encrypted peers are exactly
what the star removes), so it is **retired** when the interface changes
(proposal; section 16).

## 6. Master discovery: the `HAP-Master` Service and `term`

The master registers one entry in Thread Network Data (OpenThread server API):

| Part | Content |
| --- | --- |
| enterprise number | a private enterprise number for HAP (which one: to be decided, HT-11) |
| service data | the ASCII tag `HAP-Master` (or a 1-byte code under that enterprise number) |
| server data | `term` (u32), master node id (8 bytes), model revision (u32) |
| stable | **yes**, so sleepy end devices, which only receive stable Network Data, see it |

Registering it gives the master the Service's **anycast ALOC**. Every member sends
"to the master" by sending to that address, so a change of master needs no
rediscovery on the members: the next frame simply lands on the new one.

### The term rule

- `term` is persisted on every mains node, and increases by one on every change
  of master (handover or takeover). It never decreases.
- Every node watches Network Data changes (OpenThread state-changed callback).
- **More than one `HAP-Master` entry** means two nodes claim the role - after a
  partition heals, after a forced takeover while the old master was merely
  unreachable, or after a handover interrupted at the wrong moment.
  - **The higher term wins.** A master that sees an entry with a higher term than
    its own unregisters, becomes a member, and accepts the winner's snapshot.
  - **Equal terms** (two simultaneous takeovers): the **lower node id wins**
    (proposal - the same tie-break as the successor rule, so there is one
    ordering to learn).
- While two entries exist, anycast could reach either, so members address the
  winning entry's server **directly** (its RLOC16 is in Network Data) until only
  one entry is left.
- **Writes accepted by the losing master since the split are lost.** Merging is
  not attempted; configuration writes are rare and a user repeats one far more
  reliably than a merge would resolve one. The losing master logs what it
  discarded.

How long Network Data takes to converge after a register/unregister, and how
long the Leader keeps the entry of a server that has vanished, are HT-2 findings.
They decide timeouts in sections 9 and 10, which are left symbolic here.

## 7. The model, and the master as single writer

**The model is what a new master needs and could not re-learn by listening.**
Following the rule the tree already settled on for its persisted model - *only
what somebody said, never what was inferred* - it holds:

| In the model (replicated) | Not in the model (cache, rebuilt by the master) |
| --- | --- |
| network identity: name, `term`, `revision`, master node id | descriptors, instance names, `descriptorRev` |
| members: node id, device type, capabilities, report interval | values and their age |
| links: the network-wide list (section 12) | liveness |
| network-level settings the application stores through the command API | policies (they live on the nodes) |

Names stay where v1 put them, **on the nodes**: a new master re-describes every
member exactly as a rebooted master does today, and the `descriptorRev`
mechanism keeps working unchanged. That keeps the replica small (section 15, Q4).

### Single writer

- Only the master changes the model. Every change increments `revision` and is
  written to the master's own flash **before** it is acknowledged to the caller.
- A model write reaching a member (through its command API) is **forwarded to
  the master** (section 13). A member never edits its replica in place; it only
  replaces it with a snapshot the master sent.
- Commands that are not model writes - `Write`, `SetName`, `SetPolicy`, `Ota` -
  are relayed by the master to the target node as today, and do not move the
  revision.
- No write throttling: writes happen while somebody configures the house, and
  then almost never.

### Version

`(term, revision)`, both u32, compared **lexicographically**. `revision` keeps
counting across terms (it is not reset when the term moves), so "highest
revision" in the successor rule and "highest `(term, revision)`" agree whenever
the replicas descend from the same history.

## 8. Replication

1. The master serialises the model into a **snapshot**: header (format version,
   term, revision, master id, length, CRC) and records (section 15, Q4).
2. After every committed write it pushes the snapshot to every mains member, in
   `MORE`-paged blocks of at most 232 bytes, `ACK_REQ` on the last.
3. A member writes the blocks into a **staging slot**, checks length and CRC, and
   compares versions:
   - newer than what it holds: makes staging current (A/B swap, so a power cut
     mid-write leaves the previous replica intact), acks with its new version;
   - same or older: `Nack(StaleVersion)` with the version it holds.
4. The master records each member's acknowledged version and retries
   unacknowledged members with back-off. Snapshots are full, not deltas: writes
   are rare, a full snapshot is a few hundred bytes to a couple of kilobytes, and
   a full copy cannot drift.
5. A member that was off tells the master its version on boot, and whenever it
   sees a new `term` in Network Data; the master pushes if it is behind.

Sleepy members receive nothing. They hold only what they need to work alone:
their own configuration and the links they source.

Members keep the replica **in flash only**; it is loaded into RAM when a member
becomes master. A member pays storage, not RAM.

## 9. Two-phase handover

A planned move of the role from master **M** (term N) to member **T**, started by
a command on either (section 13).

```
  M (master, term N)                           T (member)
  freeze model writes      -- HandoverPrepare(N, r) -->
  push snapshot (N, r)     -- SnapshotBlock... ------->  stage, verify
                           <-- SnapshotAck(N, r) -------
  ---------------------------- phase 2 -----------------------------
                           -- HandoverCommit(N+1) ---->  persist {master, N+1, r}   <- commit point
                                                         register HAP-Master, term N+1
                           <-- HandoverDone(N+1) ------
  unregister, persist {member, N+1}
  (or: sees term N+1 in Network Data and does the same)
```

- **Freeze**: from `HandoverPrepare` on, M refuses model writes with
  `Nack(Busy)`. Node commands and reports keep flowing.
- If T already holds `(N, r)`, phase 1 transfers nothing but the ack.
- **The commit point is T persisting `N+1`.** From then on the term rule settles
  every disagreement, which is what makes the power-loss cases below short.

| Power is lost at | What happens |
| --- | --- |
| M, before sending Commit | T discards staging after a timeout. M reboots as master at N, unfrozen. Nothing changed. |
| T, before persisting Commit | M times out and resumes at **N**, unfrozen. Nothing changed. |
| T, after persisting Commit | T reboots as master at N+1 and registers. M, if it resumed meanwhile, sees N+1 and steps down - see the next row for why it must not resume at N. |
| M, after sending Commit | T completes. M reboots believing it is master at N, registers, sees N+1, steps down, receives the snapshot. |
| nobody, but `HandoverDone` is lost | M sees N+1 in Network Data and steps down; T's `Done` is only a shortcut. |

**One rule makes the third row safe (proposal):** once M has sent
`HandoverCommit`, it must not resume at term N. If neither `HandoverDone` nor an
`N+1` entry appears within the timeout, M resumes as master at **N+2**. A T that
comes back later at N+1 then loses, and nothing M accepted after resuming is
lost. Resuming at N instead would let a late T silently roll back those writes.

## 10. Emergency takeover

The master is gone (dead, removed, flash failed) and is not coming back. This is
**operator-triggered** (proposal): a `Takeover` command through the command API
on any mains member. There is no automatic takeover in this version, because a
master rebooting for ten seconds must not cause term churn; a timeout-based
trigger can be added on top later with the same algorithm.

1. The node receiving the command checks Network Data. If a `HAP-Master` entry
   exists and that master answers a `Ping`, it refuses (`Busy`) unless the
   command carries a `force` flag.
2. It multicasts `CandidateQuery` to the mains nodes (realm-local all-nodes), and
   collects `CandidateInfo(term, revision, node id)` for a fixed window (value from
   HT-2 timings).
3. **Successor rule**: the highest `(term, revision)`; on a tie, the **lowest
   node id**. It includes itself.
4. If it is the successor, it becomes master with term = (highest term seen) + 1,
   restores the model from its **local replica**, registers the Service, and
   pushes its snapshot to every member. Otherwise it forwards `Takeover` to the
   successor, which runs from step 3 with its own view.
5. The new master re-describes every member and starts measuring liveness from
   scratch, as a rebooted master does today.

Every node applying the rule to the same answers picks the same successor, so two
operators at two nodes converge rather than race. If they still collide (a
partition, a lost multicast), both end at the same term and the equal-term
tie-break in section 6 resolves it.

**What is lost**: model writes the dead master committed but had not yet
replicated to the successor. With synchronous push after every write (section 8)
that window is one write wide.

## 11. Sleepy devices and battery nodes

**Answer to "how does a battery node reach the master?" - both, in layers
(proposal, to be confirmed by HT-2):**

- **Radio**: the node is a Thread Sleepy End Device. Its Thread parent (any router,
  chosen by Thread) holds frames for it in its indirect-transmission queue - the
  "parent mailbox" - until the node polls. HAP does not see the parent.
- **Protocol**: the node keeps the v1 initiative. It wakes, sends `Report` to the
  master, and **polls quickly for a short listen window** before sleeping. The
  master keeps **one pending command per sleepy member**, refuses a second with
  `Nack(Busy)`, and sends it the moment that member's report arrives - so it is in
  the parent's mailbox while the node is still polling.

Why not rely on the mailbox alone: the master would have to send blind, the
frame would sit in a parent's buffer for up to a whole report interval with
nothing at HAP level knowing it is there, and the parent's buffer space and
retention are Thread implementation limits rather than protocol promises. Why
not direct UDP wake windows without SED: a node that is not a Thread child is not
reachable at all, because Thread routes to children only through their parent.

What stays exactly as in v1: the master **never answers for a sleeping node**, a
command to one is *pending* until the node itself replies, and a command lands
within one report interval.

What HT-2 has to measure before HT-5/HT-9 fix the numbers:

- whether a C6 SED can deep-sleep and resume its Thread child state on wake, or
  must light-sleep (or re-attach, which costs far more radio time);
- the fast-poll period and listen window that reliably catch a reply;
- the wake cost in charge per report, compared with the v1 ESP-NOW figure once
  that has been measured.

Sleepy members are never master, hold no replica, are not takeover candidates,
and do not receive multicast takeover traffic. They enter configuration mode by a
local button; that is the device's logic, not this library's (it calls the
command API, section 13).

## 12. Links in a star

The lowest common ancestor no longer exists, so a link needs a new home.
**Proposal: a link lives at its source node.** When the source produces a value
on a linked out port - reporting or not - it sends a `WriteRequest` without
`ACK_REQ` directly to the destination node id.

| | Tree (v1) | Star (proposal) |
| --- | --- | --- |
| where it lives | lowest common ancestor | the source node |
| addressing | two downward paths | the destination's node id |
| master needed to run it | no, unless the master is the LCA | **never** |
| extra frames per value | one per hop down | one, unicast, routed by Thread |
| internal (same node) | no radio | no radio |

- The master installs links with `SetLinkRequest` sent to the source node; the
  link record replaces `srcPath`/`dstPath` with the destination node id (proposal
  layout: `linkId`, src class/instance/port, dst node id, dst
  class/instance/port).
- The master keeps the network-wide list in the model, because enforcing "an
  input accepts at most one link" needs the whole picture, and a new master needs
  it to show and edit links.
- A battery source pays one extra unicast per wake. A battery **destination**
  stays discouraged exactly as in v1: delivery waits for its next poll.
- Staleness timeouts and failsafes are unchanged and remain the only mechanism
  that catches every failure with one timer.

## 13. Command API

The library's single entry point for configuration, independent of the channel
it arrived on.

```cpp
struct HAPCaller {
  uint8_t channel;       // opaque to the library: the application numbers UART, BLE, portal...
  const char* identity;  // optional; whatever the channel authenticated, or nullptr
};

using HAPAuthHook     = etl::delegate<bool(const HAPCaller&, const HAPCommand&)>;
using HAPResponseSink = etl::delegate<void(const uint8_t* data, size_t size)>;

// bytes in; the response, possibly much later, through the sink
bool HAPCommandApi::submit(const uint8_t* request, size_t size,
                           const HAPCaller& caller, HAPResponseSink sink);
void HAPCommandApi::setAuthorization(HAPAuthHook hook);   // default: allow everything
```

- **Request bytes** (proposal): a small command header - command code, request
  id, target node id - followed by an ordinary HAP message payload, so the
  command API reuses the message catalogue rather than inventing a second
  vocabulary.
- **Asynchronous by design**: a network command may need the master, and through
  the master a sleeping node, so the answer can arrive one report interval later.
- **Dispatch**:

| Kind | Examples | Where it runs |
| --- | --- | --- |
| local | status, role, replica version, form/join network, factory reset, `Takeover`, `HandoverTo` | this node |
| network | enrol window, links, node commands (read, write, rename, policy, OTA), model reads | **the master**: executed here if this node is master, otherwise forwarded to it over Thread and the answer relayed back |

- **Authorization** is evaluated once, at the node where the command enters, before
  dispatch. The hook allows everything for now; the seam exists so that a later
  policy (per channel, per identity, per command) does not change any call site.
  Forwarded commands are trusted because they arrived over the Thread network
  (see the limitation in section 14).

## 14. Joining, enrolment and security

Replaces the bind handshake, the bind window and the channel sweep.

1. **Forming**: the first master forms the Thread network (generates the
   operational dataset) on a local command.
2. **Joining** (proposal, HT-2 to confirm on ESP-IDF): the master opens a
   time-limited commissioning window - the successor of the 60 s bind window -
   and acts as Thread Commissioner; the new node runs the Thread Joiner with its
   PSKd. A channel-neutral fallback: the dataset is handed to the new node
   through its own command API (for example over its UART), which also suits the
   automated rig.
3. **Enrolment**: once attached, the node sends `Enrol` to the master's address.
   The master adds it to the model (a write: revision, persist, replicate) and
   answers `EnrolAccept`, then pulls `Describe` as today.

Thread scans for the network itself; no node ever needs to know a channel.

### Known limitations

- **Secrets on every node.** Any mains node may become master, so every one holds
  everything a master needs, and every Thread node holds the network key anyway.
  Extracting one node's flash compromises the whole network. Accepted for now.
- **No per-node trust.** Any node inside the Thread network can send any HAP
  frame; per-link keys are gone and nothing signs a command. Membership in the
  network is the security boundary.
- **Authorization at ingress only**, and the default hook allows everything.
- **Writes of a losing master are discarded** after a split (section 6).
- **No automatic takeover**: a dead master is replaced only when somebody asks.

## 15. Open questions - proposed answers

| Question | Proposed answer | Needs HT-2 |
| --- | --- | --- |
| **Q1.** Does `HAPStack` keep a notion of depth? | No. Roles only: master, member, sleepy member, unjoined (section 3). | no |
| **Q2.** Max network size vs the old 5-6 children per node? | The per-node child limit disappears with ESP-NOW peers. The HAP limit becomes the master's model capacity (`HAP_MODEL_MAX_NODES`, today 8) - proposal: raise the design target to **32 nodes**, tunable. Thread's 32-router ceiling is not binding for a house; how many sleepy children one C6 router can hold is an OpenThread configuration limit to check. | yes: router child-table limit, RAM per master-side node |
| **Q3.** Battery node: parent mailbox or direct UDP wake windows? | Both, layered: Thread SED with the parent's indirect queue as the carrier, and the v1 HAP rule on top - master-side queue of one, released when the node reports, node fast-polls in a listen window (section 11). | yes: sleep mode, poll timing, wake cost |
| **Q4.** NVS size for a replicated model? | Small, because descriptors and names stay out of it (section 7). See the computed bound below. | measured in HT-13 |

**Q4, computed upper bound (not measured).** With the proposal's record layouts:

| Part | Bytes |
| --- | --- |
| header: format, term, revision, master id, length, CRC | ~24 |
| member: id 8, device type 1, capabilities 1, interval 2, flags 1 | 13 per node |
| link: id 1, src node id 8, src ports 3, dst node id 8, dst ports 3 | 23 per link |
| 32 nodes, 64 links | ~24 + 416 + 1472 ~= **1.9 KB** |
| two slots (A/B) for power-loss safety | ~3.8 KB |

That fits comfortably in the default ESP-IDF NVS partition alongside
OpenThread's own settings, or as a file in the store `HAPStore` already uses.
Which backend, and the real encoded size, are HT-13's to measure; a network-wide
link cap (64 above) is needed for the bound to be a bound.

## 16. What is removed from the tree design

| Removed | Where it lives today | Replaced by |
| --- | --- | --- |
| hop paths, path lengths, `UPSTREAM` | `HAPPath`, `HAPFrame` header, `HAPCodec` path helpers | src/dst node ids (section 5) |
| forwarding rules, `route()`, `resolveDownstream()` | `HAPRouter` | direct delivery; Thread routes |
| child table, child index, `childCapacity()` (6 / 5) | `HAPRouter`, `HAPStore`, `HAPModel` paths | the model's member list, keyed by node id |
| `RouteError`, `NoSuchChild`, `ChildUnreachable`, `NoRoom` | messages, results | send failure / `ACK_REQ` timeout |
| `ChildAttached` and its re-origination | `HAPStack`, messages | `Enrol` straight to the master |
| channel sweep, `setChannel()`, the `channel` field of `BindAccept` | `HAPBinder`, `HAPITransport` | Thread scanning and joining |
| per-link LMKs, peer table, encrypt-on-send-complete switch, `BindConfirm` | `HAPBinder`, `HAPITransport`, `HAPEspNow`, `HAPStore` | the Thread network key |
| parent-side queue for sleeping children | `HAPQueue` in every parent | one queue, at the master (section 11) |
| links at the lowest common ancestor | `HAPLinks` | links at the source node (section 12) |
| `HAP_MAX_DEPTH`, the loop guard | `HAP.h`, `HAPRouter` | nothing: no hops at HAP level |
| `HAPEspNow` | `HAPITransport/HAPEspNow` | `HAPThread` (proposal: retired in HT-4) |
| the RTC mirror of parent MAC, index, key, channel | `HAPStore` | OpenThread's own persisted state; what a SED needs on wake is an HT-2 finding |

Kept unchanged: classes, ports, quantity kinds, values and the "Null is not zero"
rule, `descriptorRev`, report policies and deadbands, paging with `MORE`, the
one-frame OTA request, staleness and failsafes, and `HAPNode`'s answers to every
request.

## 17. Mapping to tasks

| Task | Design sections it implements |
| --- | --- |
| HT-3 Address abstraction | 4: `HAPNodeId` (EUI-64) replaces `HAPMac` as identity |
| HT-4 Transport interface v2 | 5 (transport sketch), 16: no channel, no peers, `master()` address, multicast; `HAPLoopback` updated; `HAPEspNow` retired |
| HT-5 HAPThread backend | 5, 6 (ALOC mapping), 11 (SED polling): OpenThread UDP behind the v2 interface |
| HT-6 Star framing | 5: header v2, flags, catalogue changes; `HAPPath` removed |
| HT-7 Drop routing | 16: `HAPRouter` forwarding and child table removed |
| HT-8 Join/bind redesign | 14: commissioning window, Joiner or dataset fallback, `Enrol`/`EnrolAccept` |
| HT-9 Stack adaptation | 3 (roles), 11 (master-side queue), 12 (links at source), `HAPStore` contents |
| HT-10 Tests: tree -> star | replaces tree/router suites with star equivalents for sections 3-12 |
| HT-11 Master service + term | 6: Service registration, term rule, equal-term tie-break, discovery on members |
| HT-12 Command API | 13: submit/sink, local vs network dispatch, authorization hook |
| HT-13 Snapshot format | 7, 15 Q4: content, `(term, revision)`, CRC, A/B slots, measured size |
| HT-14 Replication | 8: push, staging, stale rejection, catch-up on boot |
| HT-15 Master handover | 9: freeze, transfer, commit point, N+2 resume rule, power-loss table |
| HT-16 Emergency takeover | 10: `Takeover`, candidate query, successor rule, restore from replica |
| HT-17 Docs update | rewrite Protocol, HowItWorks, Links, ImplementingStatus, TestPlan from this document |
| HT-18 Hardware validation | section 18 |

## 18. Verifying it on hardware

The rig is three ESP32-C6 boards on USB, flashed and driven automatically.
Anything that needs a hand - a button, a power cut, moving a board - is out of
scope for automated runs, so every action below is a console command reaching
the command API, and every check is a log line.

| What | Passes when |
| --- | --- |
| form and join | board 1 forms the network and becomes master at term 1; boards 2 and 3 join and enrol; board 1 lists three members |
| discovery | boards 2 and 3 log the `HAP-Master` entry with term 1 and the master's node id, and their reports arrive at board 1 |
| replication | a link added on board 1 raises the revision, and boards 2 and 3 log the same `(term, revision)` |
| link without master | board 2's value drives board 3's input; board 1 is told to step down/stop its stack, and the link keeps delivering |
| handover | `HandoverTo` board 2: board 2 logs term 2 as master, board 1 logs stepping down, board 3 reports to board 2 without being told |
| takeover | board 2's HAP stack is stopped by command; `Takeover` on board 3 makes board 3 master at term 3 from its replica |
| term conflict | two boards forced to claim the role: the lower term (or, equal, the higher id) steps down and receives the winner's snapshot |
| sleepy node | one board as SED: reports arrive, one pending command is delivered after its next report, a second is refused |

Unit tests on `HAPLoopback` remain the development check for all of the above;
the boards are what says the radio, Thread and the timings agree.
