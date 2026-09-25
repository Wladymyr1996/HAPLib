# Relay — class `0x13`

A switched contact: a relay or a contactor coil on a controller, closing a
circuit that may power anything — a pump, a heater, a valve, a light. The class
says what the contact is doing and takes a command to change it. What it
switches is not the protocol's business.

| | |
| --- | --- |
| **Class id** | `0x13` |
| **Range** | actuators, `0x10`–`0x1F` |
| **Value type** | Bool (`0x01`) |
| **Access** | read **and** write |
| **Writable flag** | always set |

## Ports

| Port | Direction | Name | Quantity kind | Type |
| --- | --- | --- | --- | --- |
| 0 | out | `State` | `OnOff` | Bool |
| 0 | in | `State` | `OnOff` | Bool |

Out 0 and in 0 are the same contact seen from two sides: what it is doing, and
what it is told. They share a number because nothing on the wire names a port
without saying which way it faces — see `HAPClasses.hpp`.

## Why not a `Lamp`

The ports are exactly a [Lamp](../Protocol.md)'s, on purpose: anything that can
drive a lamp can drive a relay, and a link between the two is legal in both
directions. It is still a class of its own, for two reasons:

* **What a gateway shows.** A contact switching a circulation pump is not a
  lamp, and a user interface that draws a bulb for it is wrong in a way the user
  notices. The class id is the only thing a master has to go on.
* **Room to grow.** A relay may later say more than a lamp should — who is
  driving it, for instance, the network or local logic. A new port here changes
  nothing about what a lamp is, and nothing for a master written against `0x11`.

## Fields of an instance

| Field | Type | Value for this class | Meaning |
| --- | --- | --- | --- |
| `classId` | u8 | `0x13` | fixed by this document |
| `instanceId` | u8 | 0…7 | unique **within the node** |
| `flags` | u8 | bit 0 = 1 (writable)<br>bit 1 = 0 | |
| `valueType` | u8 | `0x01` Bool | out port 0's type |
| `name` | NAME | e.g. `"Relay 1"`, `"Насос"` | the user's label, ≤ 31 bytes |

## The values

### Out port 0 — `State`

| | |
| --- | --- |
| Encoding | VALUE, type Bool (`0x01`) |
| `true` | the contact is closed: the load is energised |
| `false` | the contact is open |
| No reading | VALUE type Null (`0x00`) |

**What the contact IS doing, not what it was last told.** A node that refuses or
overrides a command — because local logic owns the relay, or because the node is
in a safe state — reports the state it actually holds. A master that writes
`true` and reads `false` back has learned something true.

Null is a node that does not know, which for a relay driven by its own pin should
not happen after boot. It is not `false`.

### In port 0 — `State`

| | |
| --- | --- |
| Encoding | VALUE, type Bool (`0x01`) |
| Acted on | the **level** — `true` closes, `false` opens |
| Null | accepted by the protocol, as on every port, and **changes nothing**: a command that does not say which is not a command |

**A level, not an edge**, unlike a [Door](DoorClass.md)'s buttons. Writing `true`
twice leaves the contact closed; it does not toggle. A master that wants a toggle
reads out 0 and writes the opposite.

Writing the state the contact already holds is not an error: it is answered `Ok`
and changes nothing.

A node may take a written value and still not apply it — it is the node's own
policy, not the protocol's, whether a network command wins over a local one. What
the master learns is out port 0, as above.

## Reporting

| | Default | |
| --- | --- | --- |
| Interval | node heartbeat | so a silent relay is distinguishable from a dead one |
| Deadband | — | not applicable to a Bool |
| Policy | `SetPolicyRequest` (`0x17`) | per port |

A relay reports **on change**, whatever changed it: a network command, a button
on the node, or the node's own logic. That is how a master sees a relay somebody
switched by hand.

## In this ecosystem

A relay controller carries one instance per contact. The coil pin, the power-on
state and whatever local wiring drives the relay are properties of the
*controller*, and none of them travel. `HUnitLib`'s relay unit is the reference
implementation on that side.
