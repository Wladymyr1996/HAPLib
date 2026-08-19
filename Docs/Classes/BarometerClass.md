# Barometer — class `0x03`

Atmospheric pressure, in pascals.

| | |
| --- | --- |
| **Class id** | `0x03` |
| **Range** | environmental sensors, `0x01`–`0x0F` |
| **Value type** | Float (`0x03`) |
| **Unit** | **Pa** — pascals, canonical, always |
| **Access** | read-only |
| **Writable flag** | never set |

## Ports

| Port | Direction | Name | Quantity kind |
| --- | --- | --- | --- |
| 0 | out | `Pressure` | `Pressure` (Pa) |

One output and no inputs. See [Links.md](../Links.md).

## Fields of an instance

| Field | Type | Value for this class | Meaning |
| --- | --- | --- | --- |
| `classId` | u8 | `0x03` | fixed by this document |
| `instanceId` | u8 | 0…7 | unique **within the node** |
| `flags` | u8 | bit 0 = 0 (not writable)<br>bit 1 = 0 (timer-driven) | |
| `valueType` | u8 | `0x03` Float | |
| `name` | NAME | e.g. `"Outdoor"`, `"Атм. тиск"` | the user's label, ≤ 31 bytes |

## The value

| | |
| --- | --- |
| Encoding | VALUE, type Float, `f32` little-endian |
| Sane range | 30 000 … 110 000 Pa |
| Typical resolution | 1 Pa (0.01 hPa) |
| Typical accuracy | ±100 Pa absolute (BMP280); ±12 Pa relative |
| No reading | VALUE type Null (`0x00`), **not** 0.0 |

**Pascals, not hectopascals.** The number a weather forecast quotes — 1013 hPa —
is 101 325 Pa here. It is the SI unit, it is what the sensor's datasheet works
in, and having exactly one unit on the wire is worth more than having a
convenient one.

`f32` holds 101 325 with about 0.008 Pa of room to spare, so full pascal
resolution survives the encoding with three orders of magnitude in hand.

## Station pressure, never sea-level

A barometer reports **what it measured, where it is** — station pressure, with no
altitude correction whatsoever.

Sea-level pressure (QNH) is what forecasts quote, and getting from one to the
other needs the sensor's altitude, which the sensor does not know and must not
guess. Pressure falls by roughly 12 Pa per metre near sea level, so a node
"helpfully" correcting for an altitude it assumed would be wrong by 1 200 Pa on
the third floor — larger than the weather signal anyone is looking for.

If a master knows a node's altitude, it applies the correction itself, at the
point of display. A corrected value must never be sent as a `Barometer`.

## Reporting

| | Default | |
| --- | --- | --- |
| Interval | 60 s | |
| Deadband | 50 Pa | ~0.5 hPa; below that it reports its own noise and the weather |
| Policy | `SetPolicyRequest` (`0x17`) | |

Pressure is the slowest thing in this ecosystem: a stormy day moves it by
2 000 Pa over several hours, roughly 5 Pa a minute. A deadband larger than the
sensor's relative accuracy will therefore hold a battery node quiet for a long
time — which is the right outcome, since nothing acts on a pressure reading in
under an hour.

A short interval buys nothing here. If a battery node reports only one class
often, it should not be this one.

## Conversion

Both are display decisions, applied where the value is displayed:

```
hPa  = Pa / 100
mmHg = Pa / 133.322387415
```

101 325 Pa is 1013.25 hPa, or 760 mmHg.

## Altitude

`HBMP280` and its relatives are sold as altimeters as much as barometers, and the
barometric formula will give a height from a pressure. It is not a class here,
and should not become one: the same reading is a different altitude on a
different day, so an altitude derived from a single station reading tracks the
weather rather than the sensor's position. Anything needing height wants a
reference pressure, and that is a master's problem.

## In this ecosystem

`HBMP280` reports one instance of this class. It also measures temperature —
which the driver needs internally, since pressure compensation is defined in
terms of it — but a node that already carries an [AHT20](ThermometerClass.md)
normally leaves that unexported rather than shipping two thermometers that
disagree.
