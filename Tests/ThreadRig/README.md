# Thread rig

A flashable test image and a scenario runner for `HAPThread`, the Thread
backend of `HAPITransport`, on ESP32-C6 boards.

**This is a test image, not a product and not a library default.** It joins a
hardcoded network (name `HAPRig`, channel 15) whose network key is committed in
`main/main.cpp`. Anybody who reads this repository can join that network. Never
reuse the dataset for anything real.

Every board runs the same image. It starts OpenThread, applies the rig dataset
on every boot, opens a `HAPThread`, and offers two consoles on the USB port:

- `ot <line>` is the OpenThread CLI (`ot state`, `ot ipaddr mleid`, ...).
- `hap <command>` drives the transport (below).

## Requirements

- ESP-IDF v5.5.3 and its Python environment (it has esptool and pyserial).
- A checkout folder named `HAPLib` (IDF names a component after its folder; the
  build says so if it is called anything else).
- HCoreLib. `Tests/HCoreLib` is a submodule that may be empty; point the build
  at any checkout with `-DHCORELIB_DIR=<path>`.
- Network access for the first build (the `joltwallet/littlefs` managed
  component that HCoreLib asks for).

## Build

From a Windows `cmd` or PowerShell with the IDF environment loaded. Git Bash
does not work as it is: `idf.py` sees `MSYSTEM`, prints that MSys/Mingw is
unsupported and exits. Clear `MSYSTEM` and call it from `cmd`, or use the IDF
PowerShell.

```
idf.py -C Tests\ThreadRig -B <build dir> -DHCORELIB_DIR=<path to HCoreLib> set-target esp32c6 build
```

Keep the build directory outside the source tree. `Tests/ThreadRig/sdkconfig`
and `managed_components/` land in the tree and are ignored by git;
`dependencies.lock` is committed.

`idf.py fullclean` also forgets the cached `HCORELIB_DIR`: pass `-DHCORELIB_DIR`
again on the next build.

## Flash

Use the Python from the Espressif environment (for example
`C:\Espressif\tools\python\v5.5.3\venv\Scripts\python.exe`), because it has
esptool and pyserial. Avoid `idf.py -p` with several boards attached: it has
been seen sending every flash to the same port.

```
python Tests\ThreadRig\tools\rigflash.py <build dir> --erase
```

`--erase` wipes each board first. A board needs that once after it held another
image: foreign NVS data makes OpenThread's settings unusable. Without it the
script only writes the image. The same by hand, from the build directory:

```
python -m esptool --chip esp32c6 -p COM6 erase_flash
python -m esptool --chip esp32c6 -p COM6 write_flash "@flash_args"
```

Ports default to `A=COM6,B=COM7,C=COM8`; change them with `--ports`.

## Run scenarios

```
python Tests\ThreadRig\tools\rigrun.py Tests\ThreadRig\tools\scenarios\b02_unicast.txt
```

The runner opens the boards a scenario uses, types the steps, and checks the
lines the boards print. Every check prints `PASS` or `FAIL`; the run ends with
`SUMMARY pass=N fail=N` and the exit code is 1 on any `FAIL`. Options:
`--ports`, `--boards`, `--log <file>` (the merged, timestamped log), `--quiet`
(verdicts only), `--check` (parse a scenario, open no port). The step language
is described at the top of `tools/rigrun.py`.

Scenarios start by waiting for all boards to attach (up to 30 s) and keep the
boards' ids and addresses in variables. They assume the boards run the rig
image on the rig network and leave them that way.

| File | Case |
| --- | --- |
| `b01_attach.txt` | attach, ids, address text |
| `b02_unicast.txt` | unicast A to B, B to C, nothing on the third board |
| `b03_bigframe.txt` | 20 frames of 250 bytes (6LoWPAN fragments) |
| `b04_anycast.txt` | `master()` with and without the service |
| `b05_multicast.txt` | multicast, also right after a reboot |
| `b06_detach.txt` | `thread stop` and `start` |
| `b07_reboot.txt` | id and ML-EID survive `ot reset` |
| `b08_sleepy.txt` | sleepy end device, poll period 3 s |
| `b09_overflow.txt` | receive queue overflow while delivery is stalled |
| `b10_soak.txt` | 500 frames each way at 20 per second; loss and heap are reported |

After an `ot reset` the board's USB port may disappear for a moment. The reader
reopens it and writes `# reopen @X` into the log.

## The `hap` command

| Command | Does |
| --- | --- |
| `hap info` | prints `HAPINFO` |
| `hap send <ipv6\|master> <text...>` | sends the text as one frame |
| `hap sendhex <ipv6\|master> <hex>` | sends the bytes |
| `hap mcast <text...>` | multicast |
| `hap burst <ipv6\|master\|mcast> <count> <size> [intervalMs]` | `count` frames of `size` bytes (6 to 250), one per interval, in a worker task; one burst at a time |
| `hap stall <ms>` | stops delivering received frames for that long, so they pile up in the queue |

`master` is `HAPAddress::master()`; an IPv6 address is copied 1:1 into a
`HAPAddress`. Anything else is `HAPERR`.

## Lines the rig prints

One `printf` per line. Addresses are in the compressed form `ot ipaddr` uses.

| Line | When |
| --- | --- |
| `HAPBOOT id=<eui64>` | once at start, after `begin()` succeeded |
| `HAPERR <reason>` | `begin` failed, or a command was malformed (`usage...`, `bad address`, `busy`, ...) |
| `HAPSTATE attached=<0\|1>` | when the attachment changes, and once at start |
| `HAPINFO id=<16 hex> addr=<ml-eid or -> attached=<0\|1> dropped=<n> heap=<free bytes>` | `hap info` |
| `HAPTX ok=<0\|1>` | after `send`, `sendhex`, `mcast` |
| `HAPBURST sent=<n> failed=<n>` | when a burst ends; there is no per-frame line |
| `HAPRX from=<ipv6> len=<n> seq=<n or -> fill=<ok\|bad\|-> data=<text or hex:..>` | for every received frame |

`id` is the 802.15.4 EUI-64 as 16 lowercase hex digits, the same text as
`ot eui64`. `data` is the text when every byte is printable (first 48 shown),
otherwise `hex:` and the first 16 bytes.

### Burst frames

A burst frame is rig-private, carried as the UDP payload: the bytes `H`, `B`, a
4-byte big-endian sequence number, then filler where byte `i` is
`(seq + i) & 0xFF`. A receiver that sees the `HB` magic prints the sequence in
`seq=` and checks all the filler: `fill=ok` or `fill=bad`. Any other frame
prints `seq=-` and `fill=-`.

## Notes

- The tick and burst tasks run at priority 3: above the console (2), below
  OpenThread (5), so a burst cannot starve the stack that carries it.
- B5 relies on `multicast()` reaching a freshly attached router-capable child;
  B8 on it *not* reaching a sleepy one.
