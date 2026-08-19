# HAPLib

The Hatynka Air Protocol: a tree network over ESP-NOW, for devices that have no
router, no IP and no pairing app — two button presses bind a device to its
parent, and every node knows only its parent and its own children.

Built on [HCoreLib](../HCoreLib/README.md), to the same rules: no heap after start-up, no
exceptions, ETL containers, and one platform backend compiled in rather than
switched at runtime.

```
Gateway ──1── Heating controller ──2── Thermometer (bedroom, battery)
        │                         └─4── Lamp (hall)
        └─3── Thermometer (hall)
```

## What it does

- **Addresses by path, not by identity.** The bedroom thermometer is `1.2` from
  the gateway and `2` from the controller above it. No node knows its own global
  address, and re-binding a subtree tells nobody anything.
- **Builds the return route on the way up.** Each node prepends the child a frame
  arrived from, so a climbing frame's source path is — at every node it passes —
  the downward path from *that* node to the origin.
- **Binds with a press at each end.** A 60 s window on the master, an announcement
  from the slave that sweeps channels to find it, and a link key that makes
  everything after the handshake encrypted.
- **Reaches devices that are not there.** A battery node is awake for ~250 ms a
  minute; its parent queues what it cannot send and delivers it in the node's
  next listen window. A command is not lost, it is *late*.
- **Wires things together.** One port's output feeds another's input, with the
  link living at the lowest common ancestor of the two — so a control loop one
  hop from its sensor keeps running when the gateway is unplugged.
- **Notices when it is wrong.** Every node carries a CRC of its own descriptor,
  so a master can tell whether what it cached is still true without asking.

## Documentation

| | |
| --- | --- |
| [Docs/HowItWorks.md](Docs/HowItWorks.md) | the reasoning: roles, addressing, discovery, sleep, security |
| [Docs/Protocol.md](Docs/Protocol.md) | the wire: byte layouts, all 26 messages, worked examples in hex |
| [Docs/Links.md](Docs/Links.md) | ports, quantity kinds, and where a link lives |
| [Docs/Classes/](Docs/Classes/) | what each device class reports, and its ports |
| [Docs/TestPlan.md](Docs/TestPlan.md) | the three-board bench rig, and what only hardware can prove |
| [Docs/ImplementingStatus.md](Docs/ImplementingStatus.md) | what is built, what is not, and every bug the tests have caught |

## What is in it

| Module | Does |
| --- | --- |
| `HAPStack` | all of the below as one object — what an application actually holds |
| `HAPPath` | the 5-hop relative address, and the shift/prepend pair routing rests on |
| `HAPFrame` | the 18-byte header; the payload is a view, not a copy |
| `HAPCodec` | bounds-checked primitives, with sticky failure so one check covers a whole message |
| `HAPMessages` | all 26 payloads as typed structures |
| `HAPRouter` | parent, children, and the two forwarding rules — no radio, no timers |
| `HAPITransport` | the radio seam: ESP-NOW on target, an in-memory bus on the host |
| `HAPNode` · `HAPInstance` · `HAPClasses` | this device: its instances, their ports, its descriptor revision |
| `HAPBinder` · `HAPRandom` | both halves of the handshake, keys, and the channel sweep |
| `HAPStore` | a bind that survives a reboot: three config files and an RTC mirror |
| `HAPReporter` · `HAPQueue` · `HAPListenWindow` | when to speak, what to hold, and when a sleeper is reachable |
| `HAPModel` | what a root knows about everything beneath it, built only by listening |
| `HAPLinks` | the wires this node holds, and the byte comparison that fires them |

## Using it

```cmake
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/HCoreLib"
    "${CMAKE_CURRENT_LIST_DIR}/HAPLib")
```

Then `REQUIRES HAPLib` in whichever component uses it. The whole of a leaf node
is:

```cpp
HAPEspNow radio;
HAPNode node;
HAPStack stack(radio, node);

node.begin(HAPDeviceType::Sensor, HAPCaps::BatteryPowered, "Bedroom");
node.setReportIntervalSec(60);
node.addInstance(HAPClassId::Thermometer, "Temp");

stack.begin(HAP_DEFAULT_CHANNEL);

for (;;) {
  radio.update();     // drains the Wi-Fi task's queue onto this one
  stack.update();     // every HCORELIB_TICK_MS
}
```

A root adds `stack.setModel(model)`; a device with a screen or a portal reads
`stack.model()` and `stack.router()`. Nothing in the library decides when to
sleep, what to draw or which REST route to publish — it offers `maySleep()` and
a handful of hooks, and the device does the rest.

### What an application must provide

`HCoreLibConfig.h` and `HGpioConfig.h`, as HCoreLib requires. `HAPConfig.h` is
**optional** — every limit here has a default declared next to the code that
consumes it, and an application supplies the file only when it disagrees with a
number. One requirement is enforced at compile time:

```cpp
static_assert(HVALUE_MAX_STRING_LEN >= 32, ...);
```

A 16-byte link key is 32 hexadecimal characters, and an `HValue` too small to
hold them would truncate one silently — producing a device that binds, saves,
reboots and can never speak to its parent again.

## Tests

Everything except the radio itself is tested on a desktop, against the same
sources the firmware is built from.

```
cmake -S HAPLib/Tests -B build-haptests
cmake --build build-haptests
ctest --test-dir build-haptests --output-on-failure
```

No hardware, no ESP-IDF, no toolchain beyond CMake ≥ 3.16 and a C++17 compiler.
The binary exits non-zero on any failure and prints the file, line and expression
that failed, so `ctest` is a thin wrapper over it — running `haptests` directly
works just as well.

**1098 checks in fourteen suites**, in about eleven seconds. Most of that is
deliberate waiting: timers are real `HTimer`s against the real clock, so a test
about a bind window or a listen window actually waits for one.
`Tests/Config/HAPConfig.h` shortens those *cadences* — never a rule, a limit or a
byte on the wire — for exactly that reason.

### What makes them worth running

- **The simulated radio is deliberately unhelpful.** `HAPLoopback` enforces what
  ESP-NOW enforces: a unicast to a non-peer fails, two nodes on different
  channels are simply deaf to each other, a frame encrypted with one key is not
  received by a peer holding another, and nothing is delivered until the test
  pumps the bus. Code that passes against a permissive simulator fails on the
  first real board.
- **The documents are executable.** Every worked example in `Protocol.md` and
  `Links.md` is rebuilt from the message structures and compared against the
  hexadecimal those documents print — then decoded back and checked field by
  field, because an encoder and a decoder sharing a misreading would agree with
  each other perfectly.
- **Whole networks fit in one process.** `HAPStackTest` builds three complete
  devices on one bus and drives them the way three boards are driven: bind,
  report, command, sleep, link, restart. `Tests/HAPTreeTest` goes to five hops
  deep, which would need six boards.

### The one thing they cannot cover

`HAPEspNow` — the radio. It compiles for the ESP32-C6 and is exercised only on
the bench; [Docs/TestPlan.md](Docs/TestPlan.md) is how, and its §11 maps each
bench milestone to the simulated test that already covers the logic behind it.

## Continuous integration

The host tests are the regression suite. They need a compiler and nothing else,
which makes the workflow short:

```yaml
name: tests

on: [push, pull_request]

jobs:
  host:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive          # HCoreLib, and ETL inside it

      - name: Configure
        run: cmake -S HAPLib/Tests -B build

      - name: Build
        run: cmake --build build --parallel

      - name: Test
        run: ctest --test-dir build --output-on-failure

  firmware:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive
      - uses: espressif/esp-idf-ci-action@v1
        with:
          esp_idf_version: v5.5.2
          target: esp32c6
```

Three things a runner needs to know:

1. **Run from a clean build directory.** The `HAPStore` tests write real
   `config/*.cfg` files through `HFs`, relative to the working directory. A
   fresh `build/` per run keeps them out of the source tree and out of each
   other's way.
2. **`__has_include` misses are not build dependencies.** Adding or removing
   `HAPConfig.h` will not, on its own, rebuild the objects that read it — the
   library ends up compiled with one set of constants and the tests with
   another, and the failures make no sense. CI configures from scratch every
   time and never sees this; a developer who adds that file locally should wipe
   the build directory.
3. **The firmware job proves compilation, not behaviour.** It catches what the
   host build cannot — target-only code such as `HAPEspNow` and the RTC mirror —
   and nothing more.

On a developer's machine there is one more: **do not run the host tests from a
shell that has ESP-IDF's environment loaded.** IDF puts its own toolchain ahead
of everything on `PATH`, and the test binary then finds the wrong runtime
libraries and fails to start — with no output and an exit code that looks like a
crash. Two terminals, or run the tests first. CI keeps the two jobs separate and
never meets this.

## When this becomes its own repository

The library is self-contained; only the test project's paths assume this layout.
`Tests/CMakeLists.txt` reaches out with

```cmake
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../HCoreLib" HCoreLib)
```

so a standalone repository wants HCoreLib as a submodule — and ETL inside *it*, which
is where HCoreLib already looks first. Nothing else here refers to anything outside
`HAPLib/`.

## Dependencies

`HCoreLib`, and through it [ETL](https://www.etlcpp.com/) — header-only, so there is
nothing to link. On target, `esp_wifi` is a **private** requirement:
`HAPEspNow.hpp` deliberately declares no ESP type, so the radio's headers stop at
this component and nothing including a HAPLib header inherits them.
