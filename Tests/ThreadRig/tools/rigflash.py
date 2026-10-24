"""Flashes the rig image to the bench boards, one after the other.

    python rigflash.py <build dir> [--erase] [--ports A=COM6,B=COM7,C=COM8] [--boards ABC]

Run it with the ESP-IDF Python (it has esptool). `--erase` wipes the whole flash
first, which a board needs once after it held another image: foreign NVS data
makes OpenThread's settings unusable. `idf.py -p` is avoided on purpose - with
several boards attached it has been seen sending every flash to the same port.
"""

import argparse
import subprocess
import sys

DEFAULT_PORTS = "A=COM6,B=COM7,C=COM8"


def esptool(port, *arguments, cwd=None):
    command = [sys.executable, "-m", "esptool", "--chip", "esp32c6", "-p", port, *arguments]
    return subprocess.run(command, cwd=cwd).returncode == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("build_dir")
    parser.add_argument("--erase", action="store_true")
    parser.add_argument("--ports", default=DEFAULT_PORTS)
    parser.add_argument("--boards", default="ABC")
    args = parser.parse_args()

    ports = dict(pair.split("=") for pair in args.ports.split(","))
    failed = []
    for name in args.boards:
        port = ports[name]
        print(f"== board {name} on {port}", flush=True)
        if args.erase and not esptool(port, "erase_flash"):
            failed.append(name)
            continue
        if not esptool(port, "write_flash", "@flash_args", cwd=args.build_dir):
            failed.append(name)

    if failed:
        print(f"FAILED: {''.join(failed)}")
        return 1
    print("flashed: " + args.boards)
    return 0


if __name__ == "__main__":
    sys.exit(main())
