# BatteryState — class `0x20`

How much of a battery is left, as a fraction, with the terminal voltage it was
measured at.

| | |
| --- | --- |
| **Class id** | `0x20` |
| **Range** | node health, `0x20`–`0x2F` |
| **Value type** | Float (`0x03`) |
| **Unit** | **fraction** — 0.0…1.0, *not* percent |
| **Access** | read-only |
| **Writable flag** | never set |

## Ports

| Port | Direction | Name | Quantity kind |
| --- | --- | --- | --- |
| 0 | out | `SoC` | `Ratio` (0.0…1.0) |
| 1 | out | `Voltage` | `Voltage` (V) |

Two outputs and no inputs. Port 0 is the primary value — it is what appears in a
descriptor's `valueType`, and what a master shows when it has room for one
number. Ports come from this document, not from the wire: see
[Links.md](../Links.md).

**Both ports report, every time.** [`HAPNode::fillReport`](../../HAPNode/HAPNode.cpp)
walks every out port an instance has, so one `BatteryState` instance costs two
of the eight entries a report can carry.

## Fields of an instance

| Field | Type | Value for this class | Meaning |
| --- | --- | --- | --- |
| `classId` | u8 | `0x20` | fixed by this document |
| `instanceId` | u8 | 0…7 | unique **within the node** |
| `flags` | u8 | bit 0 = 0 (not writable)<br>bit 1 = 0 (timer-driven) | |
| `valueType` | u8 | `0x03` Float | **port 0's** type; port 1 happens to share it |
| `name` | NAME | e.g. `"Cell"`, `"Батарея"` | the user's label, ≤ 31 bytes |

A descriptor carries exactly one `valueType` byte, and it describes out port 0.
A master learns port 1's type from this document rather than from the wire —
which is the general rule for every class with more than one port, and costs
nothing as long as the document is the only place ports are ever defined.

## The values

### Port 0 — `SoC`

| | |
| --- | --- |
| Encoding | VALUE, type Float, `f32` little-endian |
| Sane range | 0.0 … 1.0 |
| Typical resolution | 0.01 (1 %) |
| Typical accuracy | ±5 % at best, and much worse near either end |
| No reading | VALUE type Null (`0x00`), **not** 0.0 |

**A fraction, not a percentage.** 0.87 is 87 %, and the multiplication happens
where the number is displayed — the same rule that keeps °C and pascals on the
wire. `Ratio` is documented as 0.0…1.0 throughout, and a class that quietly sent
0…100 on a `Ratio` port would be wireable into a `Regulator`'s `Demand` and
mean eighty-seven times what the far end thought.

Like humidity's, this range is **hard**: a gauge that computes 1.04 is
mis-calibrated rather than informative, and a node may clamp to 1.0.

**0.0 is a flat battery. Null is a battery nobody asked.** An unread gauge, a
failed I²C transaction, a node that has not sampled since boot — all Null, all
rendered as "—". Reporting 0.0 for a gauge that did not answer produces a
low-battery alert for a healthy cell, and teaches the user to ignore the alert
that matters.

### Port 1 — `Voltage`

| | |
| --- | --- |
| Encoding | VALUE, type Float, `f32` little-endian |
| Sane range | 0.0 … 60.0 V |
| Typical resolution | 0.001 V |
| Typical accuracy | ±1 % of reading |
| No reading | VALUE type Null (`0x00`) |

Volts, always — never millivolts, whatever the gauge's register holds.

Port 1 may be Null while port 0 is not, and the reverse. A fuel gauge that lost
its voltage channel still has a coulomb count worth reporting, and a node with
only a divider on the cell has volts and no idea of charge — which is the case
this class exists to *not* force into a guess.

## SoC is an estimate, and voltage is why both ports are here

State of charge is **computed**, not measured. A gauge arrives at it from
coulomb counting, an open-circuit-voltage curve, a temperature correction and a
model of a cell that is ageing out from under it — so two identical nodes at the
same voltage can legitimately disagree by ten points, and a gauge reset by a
brownout can be wrong by far more until it re-learns.

Voltage is the opposite: crude, nearly flat across the middle of a lithium
discharge curve, and *directly measured*. Sending both means a master can catch
what neither shows alone — a cell reading 0.6 SoC while sagging to 3.1 V under
radio transmit is a battery with high internal resistance and days rather than
weeks left, and no percentage on its own says so.

It is also why the two are **ports of one class** rather than two classes, or
two instances. They are a single measurement of a single cell, taken by one chip
at one moment. Split apart they can be sampled minutes apart and paired back up
by a master that has no way of knowing they belong together at all.

## One battery class, and what a node without a gauge sends

`0x20` is the only battery class there is, and it is deliberately the one with
room for both numbers — a node fills in what it can measure and sends Null for
what it cannot:

| Node has | Port 0 `SoC` | Port 1 `Voltage` |
| --- | --- | --- |
| A fuel-gauge IC (MAX17048, BQ27441, …) | the gauge's estimate | the voltage it used |
| A resistor divider on the cell | **Null** | the measured volts |

The second row is the case worth being explicit about. A node with nothing but a
divider **must not** derive an SoC from its voltage: a lithium discharge curve is
nearly flat between 20 % and 80 %, so the arithmetic turns ±0.05 V of divider
tolerance into ±30 points of invented charge. Null says "this node cannot know",
which a master can render as "—" and a user can act on. A guess says 62 % in a
tone of voice that sounds measured.

## Reporting

| | Default | |
| --- | --- | --- |
| Interval | 3600 s | once an hour; a mains node may be far slower still |
| Deadband | 0.01 (1 point of SoC) | below the gauge's own accuracy is noise |
| Policy | `SetPolicyRequest` (`0x17`) | per port — SoC and voltage are throttled separately |

The slowest thing on any node, and deliberately: a battery that visibly moves in
under an hour is a battery about to be replaced anyway. A sensor waking every
60 s to report a temperature should send its charge on perhaps one wake in
sixty — the radio time spent reporting a battery is taken out of that battery.

Note that a policy names `(classId, instanceId, portId)`, so voltage can be
silenced entirely while SoC keeps its hourly slot. On a node tight for report
entries, that is the first thing to turn down.

## Conversion

Percent is a display decision:

```
% = SoC × 100
```

Never on the wire, never in storage, never in a report. Remaining runtime in
hours is **derived** — from this value, the node's own duty cycle and a cell
capacity the protocol does not carry — and is a master's estimate to make and to
own, not a value to send as a `BatteryState`.

## In this ecosystem

A battery-powered sensor node carries one instance of this class alongside
whatever it actually measures, and sets `HAPCaps::BatteryPowered` in its
`BindAnnounce` — the capability bit is what makes a parent queue for it, and is
independent of whether it exports this class at all. A mains node has no reason
to carry it.

The `Bedroom` node in [Protocol.md §8.1](../Protocol.md) is one of these: a
thermometer, a hygrometer and one of these named `"Bat"`, announced as
`20 02 00 03 03 42 61 74` — class `0x20`, instance 2, not writable, Float,
because a descriptor declares out port 0 and out port 0 is `SoC`.
