# Alarm — class `0x21`

A **latched stop**: the node is holding its outputs in a known, safe state, and
will keep doing so until somebody explicitly resets it. A relay controller's
emergency stop is the first user — an over-temperature trip, an E-stop input —
but the class says nothing about what caused it or what the outputs are.

| | |
| --- | --- |
| **Class id** | `0x21` |
| **Range** | node health, `0x20`–`0x2F` |
| **Value type** | Bool (`0x01`) |
| **Access** | read; the in port only **resets** |
| **Writable flag** | always set |

## Ports

| Port | Direction | Name | Quantity kind | Type |
| --- | --- | --- | --- | --- |
| 0 | out | `Active` | `OnOff` | Bool |
| 0 | in | `Reset` | `OnOff` | Bool |

## Fields of an instance

| Field | Type | Value for this class | Meaning |
| --- | --- | --- | --- |
| `classId` | u8 | `0x21` | fixed by this document |
| `instanceId` | u8 | 0…7 | unique **within the node**; one is normal |
| `flags` | u8 | bit 0 = 1 (writable)<br>bit 1 = 0 | |
| `valueType` | u8 | `0x01` Bool | out port 0's type |
| `name` | NAME | e.g. `"Safe state"` | the user's label, ≤ 31 bytes |

## The values

### Out port 0 — `Active`

| | |
| --- | --- |
| Encoding | VALUE, type Bool (`0x01`) |
| `true` | the node is in its safe state; its outputs ignore commands |
| `false` | normal operation |

This is the answer to "why did my command not take". A master that writes a
relay `true` and reads `false` back can look here: while `Active` is true, the
node reports its outputs' real, safe states and does not act on writes to them.

### In port 0 — `Reset`

| | |
| --- | --- |
| Encoding | VALUE, type Bool (`0x01`) |
| Acted on | `true` — each write of `true` is one reset request |
| `false`, Null | nothing |

**A request, not a command.** The node leaves its safe state only when every
cause has cleared — "release, then reset", the rule every emergency stop
follows. A reset while the condition that tripped it still holds is refused:
the write is answered `Ok` (it was a well-formed request), and `Active` stays
true. A master learns the outcome from out port 0, not from the answer.

## No way to trip it over the air

There is deliberately **no in port that trips the alarm**. A spoofed or misrouted
frame must not be able to stop a plant, and "any node may stop any other" needs a
design of its own — authentication, scope, what a gateway is allowed to ask.
Tripping is local: logic on the node, an input wired to it, or its own
application.

## Reporting

An alarm reports **on change**, both ways, and with the node's heartbeat. The
moment it trips matters more than anything else the node says, and it goes out
on the next tick rather than waiting for an interval.

## In this ecosystem

HRelayController carries one instance, `Safe state`, bridged to HUnitLib's
`HUnitSafety`. The state survives a reboot, so a node that comes back up
latched reports `Active` = true from its first report.
