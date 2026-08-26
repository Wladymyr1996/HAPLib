# Door — class `0x12`

A powered opening: a gate, a garage door, a roller shutter. The first class in
this ecosystem that is a **machine** rather than a reading — it takes time to
move, it can be interrupted, and where it is between the two ends is a thing
somebody wants to see.

| | |
| --- | --- |
| **Class id** | `0x12` |
| **Range** | actuators, `0x10`–`0x1F` |
| **Value type** | Bool (`0x01`) |
| **Access** | read **and** write |
| **Writable flag** | always set |

## Ports

| Port | Direction | Name | Quantity kind | Type |
| --- | --- | --- | --- | --- |
| 0 | out | `Open` | `OnOff` | Bool |
| 1 | out | `State` | `Text` | String |
| 0 | in | `Open` | `OnOff` | Bool |
| 1 | in | `Close` | `OnOff` | Bool |
| 2 | in | `Stop` | `OnOff` | Bool |

Five ports — the widest class in the table, and the one
`HAP_MAX_PORTS_PER_INSTANCE` is sized for. `HAPClasses.cpp` static_asserts that
no class outgrows an instance, so adding a sixth port here is a build error
rather than a node that quietly refuses to have a door.

## Fields of an instance

| Field | Type | Value for this class | Meaning |
| --- | --- | --- | --- |
| `classId` | u8 | `0x12` | fixed by this document |
| `instanceId` | u8 | 0…7 | unique **within the node** |
| `flags` | u8 | bit 0 = 1 (writable)<br>bit 1 = 0 | |
| `valueType` | u8 | `0x01` Bool | **port 0's** type; port 1 is String |
| `name` | NAME | e.g. `"Garage"`, `"Ворота"` | the user's label, ≤ 31 bytes |

A descriptor carries one `valueType` byte and it describes out port 0. A master
learns port 1 is a String from this document, which is the same rule
[BatteryState](BatteryStateClass.md) follows for its second port.

## The values

### Out port 0 — `Open`

| | |
| --- | --- |
| Encoding | VALUE, type Bool (`0x01`) |
| `true` | fully open |
| `false` | anything else — closed, moving, or stopped part-way |
| No reading | VALUE type Null (`0x00`) |

**Deliberately coarse, and unchanged from the first version of this class.** It
is what every master already written against `0x12` reads, and "is it open" is
the question most things want answered. A door halfway up is not open, so it is
`false` — the nuance lives on port 1, where something that wants nuance can find
it.

Null is a door that does not know: a controller that has booted and not yet read
its limit switches. Not `false`, which would report a moving gate as shut.

### Out port 1 — `State`

| | |
| --- | --- |
| Encoding | VALUE, type String (`0x04`) |
| Values | one of the six below, exactly, lower case, ASCII |
| No reading | `"unknown"`, **not** Null |

| State | Means |
| --- | --- |
| `unknown` | no limit switch has been read yet — the state at boot |
| `opening` | the motor is running toward open |
| `opened` | the open limit is made |
| `closing` | the motor is running toward closed |
| `closed` | the closed limit is made |
| `stopped` | halted between the two, by `Stop` or by a travel timeout |

A closed vocabulary, not free text. A master may match on these six strings and
a node may send nothing else; a translation into the user's language is the
master's job, exactly as it is for every other label in this ecosystem.

`unknown` rather than Null, uniquely in this ecosystem, because it *is* a state
the door is genuinely in — a controller that has just powered up beside a door
somebody moved by hand does not know where it is, and that is worth showing as a
word rather than as a dash.

**Why a string and not an enum byte.** The protocol has no enum type, and
inventing one for a single class would put a number on the wire that means
nothing without this document — which is exactly what happens when somebody
reads `3` in a log at two in the morning. Seven bytes of `opening` costs one
report entry and reads correctly everywhere, forever.

### In ports 0, 1, 2 — `Open`, `Close`, `Stop`

| | |
| --- | --- |
| Encoding | VALUE, type Bool (`0x01`) |
| Acted on | the **rising edge** — `false` → `true` |
| `false` | nothing; releasing a button is not a command |

**Momentary, like the buttons they exist to be wired to.** A `true` says "do
this now", once, however long it is held — so a master that writes `true` and
never writes `false` again has pressed the button once, not held the door open
forever. A physical button wired to a controller's terminal behaves identically,
which is the point: nothing downstream can tell the two apart.

**`Stop` wins.** When more than one arrives in the same moment, the one that
halts the machine takes precedence — the same rule every actuator in this
ecosystem follows, and on a gearbox the difference between defined behaviour and
an expensive noise.

Writing an in port that is already being satisfied is not an error: `Open` while
`opened` is answered `Ok` and changes nothing.

## Three in ports, not one command

The obvious alternative is a single Text `Command` port taking `"open"`,
`"close"` and `"stop"`. It is one port rather than three, and it is wrong.

**Links match on quantity kind.** Three `OnOff` inputs can be driven by anything
in the ecosystem that produces an `OnOff` — a `Switch`, a `Lamp`'s state, a
button on another node, a threshold's output — with no code anywhere that knows
what a door is. A Text `Command` port could only ever be written by a master that
knew this document's vocabulary, which would put a door outside the reach of the
very wiring [Links.md](../Links.md) exists for.

The cost is two extra ports on the widest class in the table. The alternative
costs a class that cannot be wired to anything.

## Reporting

| | Default | |
| --- | --- | --- |
| Interval | 900 s | a heartbeat, so a silent door is distinguishable from a dead one |
| Deadband | — | not applicable to a Bool or a String |
| Policy | `SetPolicyRequest` (`0x17`) | per port |

**A door reports on change, and the changes are the point.** Both out ports go
out together — `HAPNode::fillReport` walks every out port an instance has — so a
door in motion produces `false`/`opening`, then `true`/`opened`, and a master
sees the transition rather than inferring it.

That makes a door a *bursty* reporter: silent for a day, then four entries in
twenty seconds. A node carrying one should size its report interval for the
heartbeat and let the changes arrive when they arrive.

## In this ecosystem

A controller node carries one instance per opening, alongside whatever else it
drives. The five ports are the class's face on the network; what the installer
actually wires — the two limit switches, the three buttons, the motor contactors
— is a property of the *controller*, not of the protocol, and none of it
travels.

`HUnitLib`'s `HDoorUnit` is the reference implementation on that side: it owns
the state machine, the travel timeout and the limit-switch arbitration, and
projects its result onto this class's five ports. A master needs to know nothing
about any of it beyond this document.
