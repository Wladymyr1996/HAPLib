# HAP — hardware test plan

A separate project, three bare ESP32-C6 modules, and three logs. What this rig
is for is everything a simulator cannot tell the truth about: a real radio, real
ESP-NOW semantics, real timing, and real deep sleep.

Three boards is the number that matters. Two prove a link; **three prove a
tree** — forwarding, a path that builds itself over two hops, a `RouteError`
from a node that is neither end, and a link living at a common ancestor. Those
are the design's load-bearing claims, and until now they existed only in
simulation.

---

## 1. What three boards can and cannot prove

| Can | Cannot |
| --- | --- |
| the radio comes up, and frames arrive | depth past 3 — a five-deep chain needs six boards |
| broadcast and unicast both work | full fan-out — five children on a bound node needs six boards |
| peer tables, keys and channels behave | failures combined deliberately, or injected at an exact moment |
| the bind handshake completes and persists | |
| **forwarding through a middle node** | |
| **a source path accumulating over two hops** | |
| **`RouteError` from a node that is not an endpoint** | |
| **a dead middle node hiding its whole subtree** | |
| **a link installed at a lowest common ancestor** | |
| a sleeping node reachable in its listen window | |
| the wake cost of a report, in milliamp-milliseconds | |

The right-hand column stays in `Tests/`, where a six-node chain costs nothing and
a channel change or a wrong key can be injected between two exact frames.

**Before anything else, check the antenna path.** Several C6 boards carry an RF
switch between an on-board and an external antenna, driven by a GPIO. If yours
does and it is left at its default, an external antenna is connected to nothing
and every symptom below looks like a protocol bug. Confirm it against your
board's schematic and note which antenna is in use in each log header.

## 2. The rig

Three ESP32-C6 modules, USB for power and console, antennas attached. No
sensors, no buttons, no display — the values are invented and the "buttons" are
console keys.

```
NODE1 (root) ──1── NODE2 (middle) ──1── NODE3 (leaf, battery)
```

| Board | Role | Is | Powered |
| --- | --- | --- | --- |
| **NODE1** | `root` | master only, the gateway's job | first, and stays up |
| **NODE2** | `middle` | slave upward, master downward | second |
| **NODE3** | `leaf` | slave only, a fake thermometer | third |

The order is the test: each board comes up and finds the one above it already
listening.

**NODE2 is the interesting board.** It is the only one that forwards, the only
one that prepends a hop, the only one that queues for a sleeping child, and the
only one that can hold a link at a common ancestor. Most of what follows is
about it.

## 3. The project

A new project, with **copies** of `HCoreLib/` and `HAPLib/` taken from here.

```
HapRig/
  CMakeLists.txt
  HCoreLib/            copied
  HAPLib/          copied
  main/            the rig firmware, one source for all three roles
    Config/HCoreLibConfig.h, HGpioConfig.h
```

**One firmware, one flag.** All three roles build from the same source and the
same library revision, chosen at build time.

**Find the ports first.** In PowerShell:

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

Run it with the boards unplugged and again with each one attached, so you know
which port is which. Write them on the boards — three identical modules with
three identical cables is how an evening gets lost.

**Build and flash**, once per board:

```
idf.py -B build-root   -DHAP_ROLE=root   -p COM3 build flash
idf.py -B build-middle -DHAP_ROLE=middle -p COM4 build flash
idf.py -B build-leaf   -DHAP_ROLE=leaf   -p COM5 build flash
```

**Open a console**, one terminal window per board, left running for the whole
session:

```
idf.py -B build-root   -p COM3 monitor --timestamps
idf.py -B build-middle -p COM4 monitor --timestamps
idf.py -B build-leaf   -p COM5 monitor --timestamps
```

`-B` matters on `monitor` too: that is where the `.elf` lives, and without it a
crash prints raw addresses instead of function names.

`--timestamps` is what makes three logs into one. Each board's own clock counts
from **its own** boot, so the `(12350)` in a log line means nothing across
boards — but `--timestamps` stamps every line from the **host's** clock, and
three windows then share one timeline. Use it every run.

Separate build directories, because they hold different sdkconfigs and mixing
them is a way to flash yesterday's firmware to one board and today's to another.

> **A mismatched set is the most likely false failure**, and it gets likelier
> with three. When anything changes here, re-copy and reflash **all three**, and
> check that the version line each board prints at boot matches before believing
> anything else in the logs.

## 4. The console

**Yes — one window per board does both.** `idf.py monitor` is a terminal, not a
log viewer: it prints what the board sends and forwards what you type straight
to it. So the logs scroll past, you press `b`, and the result of pressing `b`
scrolls past underneath.

Three things have to be true for that to work, and all three are the firmware's
job:

- **the USB Serial/JTAG driver must be installed** — `usb_serial_jtag_driver_install()`
  and `esp_vfs_usb_serial_jtag_use_driver()`. Without them a C6 console can
  print perfectly well and read nothing at all, which looks exactly like a dead
  keyboard;
- **stdin must be unbuffered** — `setvbuf(stdin, nullptr, _IONBF, 0)`, or the
  key sits in a buffer until you press Enter;
- **reading must not block the task it is on.** The rig polls for a character
  each tick and carries on when there is none.

Two keys belong to the monitor and never reach the board: **Ctrl+]** exits it,
and **Ctrl+T** opens its menu. Neither is used as a command below.

**Capturing a log.** The monitor has no log-file option, and piping it to one
takes its stdin away — the keys stop working. So raise the terminal's scrollback
to a few thousand lines before you start, and copy from the buffer when
something goes wrong. What matters is that the capture starts at the **boot
header**, not at the interesting part.

## 5. Console keys

The devices have no buttons, so the console is the user. One key, no Enter:

| Key | root (NODE1) | middle (NODE2) | leaf (NODE3) |
| --- | --- | --- | --- |
| `b` | open a 60 s bind window | **announce upward** (be adopted) | announce (bind) now |
| `B` | — | **open a window downward** (adopt) | — |
| `p` | ping a node by path | ping the parent or a child | ping the parent |
| `d` | describe a node by path | describe a child | — |
| `r` | read values by path | read a child | send a report now |
| `w` | write to a node by path | write to a child | — |
| `n` | rename a node's instance | — | — |
| `y` | set a report policy by path | — | — |
| `k` | install a link | — | — |
| `f` | forget all children | factory reset: unbind and forget | factory reset: unbind |
| `s` | — | — | toggle deep sleep between reports |
| `v` | — | — | make the invented sensor "fail" — report Null |
| `c` | move to another channel | — | — |
| `l` | list the tree as known | list parent and children | print bind state |
| `h` | help | help | help |

`p`, `d`, `r`, `w` on the root take a **path**, typed as digits: `1` is NODE2,
`11` is NODE3. That is the one place the dotted form is not used, because a
single keypress cannot carry a dot.

## 6. Log format

Every board logs every frame, both directions, in one line, at `INFO`:

```
I (12350) HAP: < RX from 24:6f:28:aa:bb:cc  Report       seq=0107 dst=. src=.   len=37 rssi=-41
I (12351) HAP: > TX   to 24:6f:28:00:00:01  Report       seq=0107 dst=. src=1   len=37 ok
```

and the raw bytes at `DEBUG`.

Four things make three logs correlatable, which is what makes them debuggable
from here:

1. **`seq`** — the same number appears as TX on one board and RX on the next.
2. **`src` and `dst`** — printed on every line, TX and RX both. On NODE2 the two
   lines for one forwarded frame must differ by exactly one hop, and that
   difference *is* the routing rule. It is the single most valuable field in
   these logs.
3. **`len`** — a length that changes between TX and RX means the radio truncated
   something, which is a different bug from a parse failure.
4. **the boot header** — MAC, role, channel, library version, antenna, printed
   by all three before anything else happens.

If a frame is refused, log **why**, using the decoder's or the router's own
words:

```
W (12360) HAP: < RX from 24:6f:28:aa:bb:cc  DROPPED: not bound (len=60)
W (12361) HAP: ! ROUTE 1.3 failed at hop 3: no such child -> RouteError to 1
```

## 7. Milestones

Two stages. **Prove one link before adding the board that forwards** — every
multi-hop symptom has a single-hop cause, and chasing one through three logs
when two would have shown it is a wasted evening.

### Stage A — NODE1 and NODE3 only, bound directly

Leave NODE2 unpowered. NODE3 binds straight to NODE1, and the tree is one hop
deep.

**A1 — the radio exists.** Both up. NODE1 logs a `BindAnnounce` arriving from
NODE3's MAC and refuses it, no window being open.
*Passes when* NODE1's RX line matches NODE3's TX line — same seq, same length —
and RSSI is better than −70 dBm at a metre. **This is the milestone that fails**;
§8 is the list of reasons.

**A2 — bind.** `b` on NODE1, then `b` on NODE3.
*Passes when* `BindAnnounce` → `BindAccept` → `BindConfirm` appear in order
across the two logs and NODE1 lists a child at index 1. Then **power-cycle
both**: the bind survives and neither board announces again.

> **If the accept goes out and the confirmation never arrives, suspect the key
> switch.** The accept must be sent in the CLEAR — it carries the key — and the
> link must become encrypted the instant it has left. Too early and the accept
> itself is encrypted; too late and the confirmation is dropped **by the radio**,
> below the protocol, where nothing can log it. The library hangs this on the
> transport's send-completion report rather than a timer, so what to check is
> that ESP-NOW's send callback is actually firing: a transport that never
> reports one falls back to a 200 ms timeout, and the log says so.

**A2b — the channel sweep.** Factory-reset NODE3 (`f`), then move **NODE1 to
channel 6** before opening its window. NODE3 comes up on 1 and has no way to
know.
*Passes when* NODE3's log shows it announcing on 1, then 6, and binding there —
and afterwards reports that it has adopted channel 6 from the acceptance. This
is the answer to the one gap the protocol documents left open, and two boards on
one channel will never exercise it.

**A3 — reports.** NODE3 reports an invented temperature every 10 s.
*Passes when* NODE1 prints the class, instance, port and value, and the value it
prints is the value NODE3 says it sent. A report arriving with the right length
and the wrong value is an endianness bug, and would be the first thing 1098 host
checks missed.

**A3b — the deadband.** `y` on NODE1 to tighten NODE3's policy to a 3600 s
interval and a 0.2 °C deadband, then nudge NODE3's invented temperature by small
amounts and then by a large one.
*Passes when* the small changes produce **no traffic at all** and the large one
produces a report at once — and when NODE3's answer states the policy it
actually took, which for a battery node may be clamped to its wake period rather
than what was asked. Then make NODE3's sensor "fail" (`v` sends Null): that must
report immediately whatever the deadband says, because a reading disappearing is
not a small change.

**A4 — commands.** `w` on NODE1.
*Passes when* NODE3 logs the write, answers, and NODE1 matches the response to
its request by `seq`. Then unplug NODE3 and press `w` again: NODE1 must report
the link down within three report intervals rather than hang.

**A4b — the revision does the work.** With NODE1 having already described NODE3,
press `n` to rename an instance.
*Passes when* NODE1 notices on the **next ordinary report** that the revision has
moved, and asks for a fresh `Describe` **without being told to** — and when what
it showed in between was the old name rather than nothing. Then power-cycle
NODE3: the new name survives, and NODE1 does *not* re-describe, because the
revision did not move.

**A5 — sleep and the queue.** `s` on NODE3: it now deep-sleeps between reports.
*Passes when*

- NODE3 reports, sleeps, wakes and reports again with its bind intact — and the
  wake path did **not** mount the filesystem, which is what the RTC mirror is
  for;
- `n` on NODE1 while NODE3 sleeps queues the rename, and it lands in the **next**
  listen window, not the one after;
- NODE1 never acknowledges on NODE3's behalf: the rename shows pending until
  NODE3 itself answers.

- **the window closes.** After the exchange finishes, NODE3 sleeps rather than
  holding the radio on. A window that reopens for a frame arriving *after* it
  closed is a flat cell three weeks later and nothing in any log — the host tests
  caught exactly that bug once, and only a current meter would catch it here.

**Measure the wake** with a meter in series, from wake to sleep, in
milliamp-milliseconds. That number decides whether the reporting interval is
survivable on a cell, and it is the one figure this rig exists to produce.

Take it **twice**: once on a timer wake and once after a power cut. The first
must be cheaper, because the bind comes out of RTC memory and the filesystem is
never mounted. If the two are the same, the mirror is not working and the log
will say `restored parent …` on both.

**A5b — a second command while asleep.** Press `n` twice while NODE3 sleeps.
*Passes when* the second is **refused** rather than queued behind the first, and
NODE1 shows the refusal. One frame per sleeping child is deliberate: a deeper
queue delivers commands minutes apart in an order nobody chose.

### Stage B — all three, as a chain

Factory-reset NODE3 (`f`), power NODE2, and rebuild the tree as
`NODE1 ← NODE2 ← NODE3`.

**B1 — a two-hop bind.** Bind NODE2 to NODE1, then NODE3 to NODE2.
*Passes when* NODE1 logs a `ChildAttached` it did not ask for, naming NODE3's
MAC, and lists the new node at **1.1** — an address nobody computed and nobody
sent. It must also print NODE3's **report interval**, which travelled in that
notice: a `BindAnnounce` is only ever heard by the node that adopts it, so
without this field the root has nothing to measure a silence against and B5
below cannot work.

> NODE2 has two roles and therefore two Bind actions. `b` announces upward while
> it has no parent and opens a window downward once it has one — but a
> controller being given its *second* child wants the other one, so the rig
> offers `b` and `B` separately rather than guessing.

**B2 — forwarding, and the path that builds itself.** NODE3 reports.
*Passes when* NODE2's log shows the same `seq` arriving with `src=.` and leaving
with `src=1`, and NODE1 receives it with `src=1.1`. **This is the milestone the
third board was bought for.** If those three lines are right, the addressing
design is right.

**B3 — a command two hops down.** `w` on NODE1 addressed to `11`.
*Passes when* NODE2 shows it arriving with `dst=1` and leaving with `dst=.`, and
the response climbs back carrying `src=1.1`.

**B4 — a diagnosis rather than a timeout.** `r` on NODE1 addressed to `13`,
which does not exist.
*Passes when* NODE2 answers a `RouteError` naming hop 3 and reason *no such
child*, and NODE1 prints which link failed — not a timeout.

**B5 — a dead middle hides its subtree.** Unplug NODE2 while NODE3 keeps
reporting.
*Passes when* NODE1 loses NODE3 as well as NODE2 — there is no second way in —
and NODE3 keeps trying without wedging. Power NODE2 back up: everything recovers
with no re-bind and no user action.

**B6 — a link at a common ancestor.** `k` on NODE1: wire NODE3's thermometer out
port to a Regulator input on **NODE2**. The lowest common ancestor is NODE2, so
that is where the link is installed and NODE1 steps out of the way.
*Passes when*

- NODE2 logs the link accepted, with matching quantity kinds;
- each of NODE3's reports both climbs to NODE1 **and** updates NODE2's regulator
  — a tap, not a diversion;
- **NODE1 is then unplugged and NODE2 keeps regulating.** That is the whole
  argument for putting a control loop as low in the tree as it goes, and this is
  the only way to actually see it.

**B6b — the failsafe.** With B6's link running, unplug NODE3.
*Passes when* NODE2's regulator input goes **stale** after its timeout and the
regulator falls back to whatever its class defines — rather than holding the last
temperature it ever saw. One timer has to catch all of it: a dead sensor, a
broken hop, and an ancestor that lost power look identical from the destination,
and that is the point.

**B7 — queueing through a middle node.** With NODE3 asleep again, `n` on NODE1
to rename an instance on NODE3.
*Passes when* NODE2 holds the frame — logging it as queued — delivers it in
NODE3's next listen window with the `QUEUED` flag set, and **never answers on
NODE3's behalf**. NODE1 shows it pending until the real response climbs back.

**B8 — a device that moved.** Factory-reset NODE3 (`f`) and bind it directly to
**NODE1** instead of NODE2.
*Passes when* NODE1 shows **one** device, at its new address `2`, rather than two
— it recognises the MAC from `ChildAttached` and moves the entry. Two entries for
one device would show the same thermometer twice in a UI, and anything still
pointing at `1.1` would fail quietly for ever.

**B9 — everything at once, left alone.** With the chain bound, the link
installed and NODE3 sleeping, leave the rig running for **an hour** and come
back.
*Passes when* nothing has restarted, nothing has re-bound, NODE1's view of the
tree is unchanged, and NODE3's reports are still arriving at the interval it
promised. This is the only test here that can catch a slow leak, a sequence
number wrapping badly, or a queue that fills and never drains — none of which a
five-minute session will ever show.

## 8. What goes wrong first

Ranked by how often it is the answer.

### Any board

| Symptom | Usually |
| --- | --- |
| **nothing arrives at all** | the boards are on different channels; or Wi-Fi was never started; or the mode is not STA; or `esp_now_init()` came before `esp_wifi_start()` |
| broadcast arrives, unicast does not | the target was never added as a peer — ESP-NOW will not unicast to a stranger |
| `ESP_ERR_ESPNOW_NOT_FOUND` on send | the same thing, said out loud |
| encrypted send fails, unencrypted works | no PMK set, or the peer was added with `encrypt = true` before its LMK was known |
| the first frame works, then silence | the peer table filled (20 total, **6 encrypted**), or the far side went to sleep |
| a bind gets as far as the acceptance and then "nobody confirmed" | out of encrypted peer slots. Six per radio, and a node with a parent has already spent one going upward — so a **bound** node adopts five children, not six. The library refuses the sixth when the window opens; if you see this on the bench, something bypassed that |
| works touching, fails at two metres | the antenna path — see §2 |
| right length, wrong contents | the copies of HAPLib differ. Check the three boot headers |
| a board reboots every few seconds | `HTaskManager` restarting it — look for the "has not responded" line, which names the task |
| everything stalls | work done inside the ESP-NOW receive callback. It runs in the Wi-Fi task and must only copy into a queue |

### NODE2, the forwarder

| Symptom | Usually |
| --- | --- |
| frames arrive and vanish | NODE2 has the sender as a peer but not the *destination* — a middle node needs **both** its parent and each child in the peer table |
| the report reaches NODE1 with `src=.` | the hop was not prepended: NODE2 forwarded the bytes it received instead of re-encoding the frame it routed |
| the payload is right but the header is rubbish | the encode wrote back into the receive buffer. **A decoded frame's payload is a VIEW into that buffer** — forward from the receive buffer into a *different* transmit buffer |
| the second hop never happens | the routing decision was taken but not acted on, or the child's send failed silently. Look for the TX line that should follow every forwarding RX line |
| a queued frame for NODE3 never arrives | the listen window closed first, or NODE2 sent it live instead of queueing because it did not know NODE3 is battery-powered — check the capabilities in NODE3's `BindAnnounce` |
| NODE1 sees NODE3 at `1` instead of `1.1` | `ChildAttached` was forwarded rather than re-originated, so its own hop was never accumulated |
| a second command to NODE3 is refused | **correct** — one frame per sleeping child. The first has not been delivered yet |
| NODE1 never offers to describe NODE3 | the `reportIntervalSec` or the revision did not reach it. Both travel in `ChildAttached`, which only NODE2 can send |
| everything works, then stops after an hour | look for a sequence wrap, a queue that filled, or `HAPModel` full — it refuses new nodes rather than evicting, and says so |

## 9. What to send back

When a milestone fails:

1. **all three logs, whole**, from boot to failure — not the interesting part,
   the whole thing. The boot headers are half the diagnosis, and with a
   forwarder the *absence* of a line on the middle board is often the finding.
2. **which milestone**, and which key press it was on.
3. **the version line** from each board.
4. whether it reproduces, and whether it survives a power cycle.

For anything in Stage B, the three `seq`/`src`/`dst` triples for one frame —
NODE3's TX, NODE2's RX and TX, NODE1's RX — are usually enough to find it
without reading anything else.

## 10. Where this feeds back

Anything this rig finds that the simulator could have caught becomes a host test
first, then a fix. The hardware's job is to find what only hardware knows —
channels, peers, keys, timing, current — and the moment a bug is understood it
belongs in `Tests/`, where it can never come back unnoticed.

## 11. What has already been simulated

Every milestone here except A1 has a counterpart in `Tests/HAPStackTest.cpp`,
built from the same `HAPStack` the rig runs, on a bus that refuses to unicast to
a stranger, refuses to cross channels, and refuses to deliver a frame encrypted
with the wrong key.

| Milestone | Simulated as |
| --- | --- |
| A2 bind, A2b sweep | `testABoundPairReportsByItself`, and `HAPBinder`'s sweep test |
| A3 reports, A3b deadband | `testABoundPairReportsByItself`, `HAPReporter`'s suite |
| A4 commands | `testACommandReachesADriver` |
| A4b revision | `testTheMasterFillsInWhatItDoesNotKnow` |
| A5 sleep and queue, A5b refusal | `testASleepingChildIsQueuedFor`, `testASecondCommandForASleepingChildIsRefused` |
| A2 persistence | `testABindSurvivesARestart` |
| B1-B4 chain | `testAThreeNodeChain` |
| B6, B6b link and failsafe | `testALinkAtTheCommonAncestor`, `HAPLinks`' staleness test |
| B8 moved device | `HAPModel`'s `testADeviceThatMovedIsOneDeviceStill` |

That is not a reason to skip them. **It is what makes a failure here
informative**: the logic is known to work, so anything that goes wrong on the
bench is about the radio, the timing, the power or the wiring — which is exactly
the list this rig exists to test. A1 has no counterpart at all, and that is why
it is the milestone most likely to cost the evening.
