# Hygrometer — class `0x02`

Relative humidity, in percent.

| | |
| --- | --- |
| **Class id** | `0x02` |
| **Range** | environmental sensors, `0x01`–`0x0F` |
| **Value type** | Float (`0x03`) |
| **Unit** | **% RH** — relative humidity, 0…100 |
| **Access** | read-only |
| **Writable flag** | never set |

## Ports

| Port | Direction | Name | Quantity kind |
| --- | --- | --- | --- |
| 0 | out | `Humidity` | `Humidity` (% RH) |

One output and no inputs. The kind is `Humidity`, so a link into a `Temperature`
input is refused with `TypeMismatch` rather than quietly wired — which matters
here more than anywhere, since both are floats in a plausible range. See
[Links.md](../Links.md).

## Fields of an instance

| Field | Type | Value for this class | Meaning |
| --- | --- | --- | --- |
| `classId` | u8 | `0x02` | fixed by this document |
| `instanceId` | u8 | 0…7 | unique **within the node** |
| `flags` | u8 | bit 0 = 0 (not writable)<br>bit 1 = 0 (timer-driven) | |
| `valueType` | u8 | `0x03` Float | |
| `name` | NAME | e.g. `"Bathroom"`, `"Вологість"` | the user's label, ≤ 31 bytes |

## The value

| | |
| --- | --- |
| Encoding | VALUE, type Float, `f32` little-endian |
| Sane range | 0.0 … 100.0 % |
| Typical resolution | 0.1 % |
| Typical accuracy | ±2 % (AHT20), and worse below 20 % or above 80 % |
| No reading | VALUE type Null (`0x00`), **not** 0.0 |

0 % RH is a physical value, not an error — which is precisely why a failed sensor
must send Null. Nothing else in the encoding can tell the two apart.

Unlike temperature, this range is **hard**: relative humidity above 100 % is
meaningless, and a sensor reporting 103 % is saturated rather than informative. A
node may clamp to 100.0, and should say in its own logs that it did.

## Relative to what

**Relative** humidity is a fraction of what the air could hold at its current
temperature, so the number means nothing on its own. The same absolute moisture
reads 60 % at 20 °C and 100 % at 12 °C — which is condensation, mould, and the
reason anybody instruments a bathroom in the first place.

So: **a Hygrometer instance should sit on a node that also carries a
[Thermometer](ThermometerClass.md)**, and a master computing dew point must use
the temperature from the *same node*, taken at as near the same moment as it can
manage. Pairing a bathroom hygrometer with a hallway thermometer produces
confident nonsense.

The protocol does not enforce the pairing, and cannot: a sensor is entitled to
report what it measures. But `HAHT20` measures both in one conversion, and a node
built on it should export both.

## Reporting

| | Default | |
| --- | --- | --- |
| Interval | 60 s | |
| Deadband | 1.0 % | half the sensor's accuracy; below that it reports its own noise |
| Policy | `SetPolicyRequest` (`0x17`) | |

Humidity moves in steps — a shower, a kettle, an opened window — so the deadband
earns its place here more than anywhere else: a bathroom sensor is silent for
hours, then reports three times in five minutes, which is exactly the behaviour
worth having.

## Conversion

None. Percent is percent, in every language and every locale. Dew point, absolute
humidity in g/m³, and comfort indices are all **derived** — computed by whoever
displays them, from this value and a temperature, and never sent as a
`Hygrometer`.

## In this ecosystem

`HAHT20` reports one instance of this class and one
[Thermometer](ThermometerClass.md) from a single conversion, ~80 ms apart at
most — close enough together that dew point from the pair is trustworthy.
