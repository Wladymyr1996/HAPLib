# Thermometer — class `0x01`

Air or surface temperature, in degrees Celsius.

| | |
| --- | --- |
| **Class id** | `0x01` |
| **Range** | environmental sensors, `0x01`–`0x0F` |
| **Value type** | Float (`0x03`) |
| **Unit** | **°C** — canonical, always |
| **Access** | read-only |
| **Writable flag** | never set |

## Ports

| Port | Direction | Name | Quantity kind |
| --- | --- | --- | --- |
| 0 | out | `Temperature` | `Temperature` (°C) |

One output and no inputs — a thermometer measures, it is not driven. Port 0 is
the value that appears in `Report`. Ports come from this document, not from the
wire: see [Links.md](../Links.md).

## Fields of an instance

What every `Thermometer` instance carries in a descriptor
([Protocol.md §4.1](../Protocol.md)):

| Field | Type | Value for this class | Meaning |
| --- | --- | --- | --- |
| `classId` | u8 | `0x01` | fixed by this document |
| `instanceId` | u8 | 0…7 | unique **within the node**, not within the class |
| `flags` | u8 | bit 0 = 0 (not writable)<br>bit 1 = 0 (timer-driven) | |
| `valueType` | u8 | `0x03` Float | what its reports will contain |
| `name` | NAME | e.g. `"Flow"`, `"Ліжко"` | the user's label, editable, ≤ 31 bytes |

## The value

| | |
| --- | --- |
| Encoding | VALUE, type Float, `f32` little-endian |
| Sane range | −40.0 … +125.0 °C |
| Typical resolution | 0.01 °C |
| Typical accuracy | ±0.3 °C (AHT20) |
| No reading | VALUE type Null (`0x00`), **not** 0.0 |

**Null means the sensor did not answer** — an unplugged probe, a failed
conversion, an instance that has not measured since boot. A master must render
it as "—". Reporting 0 °C for a broken sensor is how a heating system ends up
running all night.

A node **must not** clamp a reading into the sane range. A value outside it is
information: it is either a genuinely extreme environment or a broken sensor, and
in both cases the master should be the one to decide.

## Reporting

| | Default | |
| --- | --- | --- |
| Interval | 60 s | battery nodes; mains nodes commonly 10 s |
| Deadband | 0.2 °C | roughly the sensor's accuracy — smaller reports noise |
| Policy | `SetPolicyRequest` (`0x17`) | a node may clamp what its battery cannot afford |

With a deadband, a node reports when **either** the interval has elapsed **or**
the value has moved by more than the deadband since the last report. A battery
node that is asleep can only honour the second condition when it wakes, so its
effective floor is its wake period whatever the policy says.

## Several thermometers on one node

Ordinary and expected: a heating controller might carry flow and return probes.

```
01 00 00 03 04 46 6C 6F 77        Thermometer, inst 0, Float, "Flow"
01 01 00 03 06 52 65 74 75 72 6E  Thermometer, inst 1, Float, "Return"
```

Same class, different instance ids, different names. `(classId, instanceId)`
addresses one probe; `classId` alone does not.

## Conversion

Fahrenheit is a display decision, applied where the value is displayed:

```
°F = °C × 9/5 + 32
```

Never on the wire, never in storage, never in a report. A gateway with a
Fahrenheit user and a Celsius user shows both from the same bytes.

## In this ecosystem

`HAHT20` (temperature + humidity) reports one instance of this class alongside
one [Hygrometer](HygrometerClass.md); `HBMP280` also measures temperature, but on
a node carrying both it is normally left unexported — two thermometers reading
the same air, disagreeing by 0.4 °C, produce support questions rather than
information.
