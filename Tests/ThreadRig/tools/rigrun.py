"""Scenario runner for the Thread rig: drives the boards over their USB consoles.

    python rigrun.py scenarios/b02_unicast.txt [--ports A=COM6,B=COM7,C=COM8]
                     [--boards ABC] [--log run.log] [--check]

Needs pyserial. A scenario is a text file, one step per line; `#` starts a
comment that is echoed into the log as a marker, `${NAME}` is replaced by a
variable set earlier with `grab` or `calc`.

  @A <line>              send `ot <line>` to board A (B, C alike)
  @A !<line>             send a raw console line, e.g. `@A !hap info`
  sleep <seconds>
  mark NAME              remember the current time, for `since=NAME`
  print <text>           echo into the log
  grab VAR @A /re(group)/ [timeout=10] [since=NAME]
                         VAR = group 1 of the last matching line
  calc VAR = <a> [+|-] <b> ...   integer arithmetic on numbers or ${VARS}
  expect @A /re/ [timeout=10] [since=NAME]
                         PASS once a line matching re shows up
  absent @A /re/ [window=3] [since=NAME]
                         PASS if no line matching re shows up for `window` s
  until @A <line> => /re/ [timeout=30] [every=2]
                         send <line> (as `@A` would) every `every` s until a
                         line matching re answers it; PASS on a match, FAIL on timeout
  untilabsent @A <line> => /re/ [timeout=30] [every=2]
                         the same, but passes once an OpenThread CLI answer
                         (ending in `Done`) has no line matching re
  assert <a> <op> <b>    op is one of >= <= == > <, on numbers or ${VARS}
  count @A /re/ n=<N> [timeout=5] [since=NAME] [save=VAR]
                         PASS if exactly N lines match: waits until N were seen
                         (or the timeout), then 1 s more for stragglers.
                         Without n= it only counts, and `save=` stores the number.

By default a step looks at lines that arrived after the last command sent;
`since=NAME` looks back to a `mark` instead, which is what a step needs when
several commands were sent in between.

Every assertion prints PASS or FAIL; the run ends with `SUMMARY pass=N fail=N`
and exits 1 on any FAIL. A board that resets can drop off the USB bus for a
moment: its reader reopens the port and writes `# reopen @X` into the log.
"""

import argparse
import re
import sys
import threading
import time

DEFAULT_PORTS = "A=COM6,B=COM7,C=COM8"
SEND_GAP = 0.3
REOPEN_TRIES = 120
ASSERT_VERBS = ("expect", "absent", "count", "grab")
OPTION = re.compile(r"(\w+)=(\S+)")
ASSERT_LINE = re.compile(r"^(\w+)\s+(?:(\w+)\s+)?@(\w)\s+/(.*)/((?:\s+\w+=\S+)*)\s*$")
UNTIL_LINE = re.compile(r"^(until|untilabsent)\s+@(\w)\s+(.+?)\s+=>\s+/(.*)/((?:\s+\w+=\S+)*)\s*$")
ASSERT_OPS = {
    ">=": lambda a, b: a >= b,
    "<=": lambda a, b: a <= b,
    "==": lambda a, b: a == b,
    ">": lambda a, b: a > b,
    "<": lambda a, b: a < b,
}
VARIABLE = re.compile(r"\$\{(\w+)\}")


class ScenarioError(Exception):
    pass


class Step:
    def __init__(self, number, text):
        self.number = number
        self.text = text


def parse(path):
    """Reads and validates a scenario; returns the steps and the boards it uses."""
    steps = []
    boards = set()
    defined = set()
    with open(path, encoding="utf-8") as handle:
        for number, raw in enumerate(handle, 1):
            text = raw.strip()
            if not text:
                continue
            where = f"{path}:{number}"
            for name in VARIABLE.findall(text):
                if name not in defined:
                    raise ScenarioError(f"{where}: ${{{name}}} is used before it is set")
            verb = text.split()[0]
            if text.startswith("#"):
                pass
            elif text.startswith("@"):
                boards.add(text[1])
            elif verb == "sleep":
                float(text.split()[1])
            elif verb in ("mark", "print"):
                pass
            elif verb == "calc":
                match = re.match(r"^calc\s+(\w+)\s*=\s*(.+)$", text)
                if not match:
                    raise ScenarioError(f"{where}: calc VAR = a [+|-] b")
                defined.add(match.group(1))
            elif verb == "assert":
                if len(text.split()) != 4 or text.split()[2] not in ASSERT_OPS:
                    raise ScenarioError(f"{where}: assert <a> <op> <b>")
            elif verb in ("until", "untilabsent"):
                match = UNTIL_LINE.match(text)
                if not match:
                    raise ScenarioError(f"{where}: {verb} @X <line> => /re/ [key=value ...]")
                boards.add(match.group(2))
            elif verb in ASSERT_VERBS:
                match = ASSERT_LINE.match(text)
                if not match:
                    raise ScenarioError(f"{where}: {verb} @X /re/ [key=value ...]")
                boards.add(match.group(3))
                try:
                    re.compile(VARIABLE.sub("x", match.group(4)))
                except re.error as error:
                    raise ScenarioError(f"{where}: bad regex: {error}")
                if verb == "count":
                    saved = dict(OPTION.findall(match.group(5))).get("save")
                    if saved:
                        defined.add(saved)
                if verb == "grab":
                    if not match.group(2):
                        raise ScenarioError(f"{where}: grab VAR @X /re(group)/")
                    defined.add(match.group(2))
            else:
                raise ScenarioError(f"{where}: unknown step '{verb}'")
            steps.append(Step(number, text))
    return steps, boards


class Board:
    """One serial port: a reader thread that keeps every line with its time."""

    def __init__(self, name, port, runner):
        self.name = name
        self.port = port
        self.runner = runner
        self.lines = []
        self.serial = None
        self.open_event = threading.Event()
        self.lock = threading.Lock()

    def open(self):
        import serial

        port = serial.Serial()
        port.port = self.port
        port.baudrate = 115200
        port.timeout = 0.2
        # Held low before opening, so that opening the port does not reset the board.
        port.dtr = False
        port.rts = False
        port.open()
        self.serial = port

    def start(self):
        self.open()
        threading.Thread(target=self.read_loop, daemon=True).start()

    def read_loop(self):
        pending = b""
        while True:
            try:
                data = self.serial.read(256)
            except Exception:
                self.runner.log(f"# reopen @{self.name}")
                if not self.reopen():
                    self.runner.log(f"# @{self.name} lost: {self.port} did not come back")
                    return
                continue
            if not data:
                continue
            pending += data
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                self.add(line.decode(errors="replace").rstrip())

    def reopen(self):
        try:
            self.serial.close()
        except Exception:
            pass
        for _ in range(REOPEN_TRIES):
            time.sleep(0.5)
            try:
                self.open()
                return True
            except Exception:
                continue
        return False

    def add(self, text):
        now = time.monotonic()
        with self.lock:
            self.lines.append((now, text))
        self.runner.log(f"{self.name}| {text}")

    def matches(self, pattern, since):
        with self.lock:
            return [text for when, text in self.lines
                    if when >= since and pattern.search(text)]

    def write(self, text):
        self.serial.write((text + "\r\n").encode())


class Runner:
    def __init__(self, ports, log_path, quiet):
        self.ports = ports
        self.start = time.monotonic()
        self.print_lock = threading.Lock()
        self.log_file = open(log_path, "w", encoding="utf-8") if log_path else None
        self.quiet = quiet
        self.boards = {}
        self.variables = {}
        self.marks = {}
        self.last_send = time.monotonic()
        self.passed = 0
        self.failed = 0

    def log(self, text):
        line = f"[{time.monotonic() - self.start:8.2f}] {text}"
        with self.print_lock:
            if not self.quiet or text.startswith(("PASS", "FAIL", "SUMMARY", "====")):
                print(line, flush=True)
            if self.log_file:
                self.log_file.write(line + "\n")
                self.log_file.flush()

    def verdict(self, ok, description):
        if ok:
            self.passed += 1
        else:
            self.failed += 1
        self.log(f"{'PASS' if ok else 'FAIL'} {description}")

    def substitute(self, text, escape=False):
        def value(match):
            found = str(self.variables[match.group(1)])
            return re.escape(found) if escape else found
        return VARIABLE.sub(value, text)

    def since(self, options):
        name = options.get("since")
        if name is None:
            return self.last_send
        if name not in self.marks:
            raise ScenarioError(f"since={name}: no such mark")
        return self.marks[name]

    def run_step(self, step):
        text = step.text
        verb = text.split()[0]
        if text.startswith("#"):
            self.log(f"==== {text}")
        elif text.startswith("@"):
            self.send(text)
        elif verb == "sleep":
            time.sleep(float(text.split()[1]))
        elif verb == "mark":
            self.marks[text.split()[1]] = time.monotonic()
        elif verb == "print":
            self.log(self.substitute(text[len("print"):].strip()))
        elif verb == "calc":
            self.calc(self.substitute(text))
        elif verb == "assert":
            self.check(self.substitute(text))
        elif verb in ("until", "untilabsent"):
            self.until(step)
        else:
            self.assertion(step)

    def send(self, text):
        text = self.substitute(text)
        name, command = text[1], text[2:].strip()
        board = self.boards[name]
        self.log(f"{name}< {command}")
        wire = command[1:] if command.startswith("!") else "ot " + command
        # Stamped before the write: the answer can arrive before the write returns.
        self.last_send = time.monotonic()
        try:
            board.write(wire)
        except Exception as error:
            self.verdict(False, f"send to @{name} failed: {error}")
        time.sleep(SEND_GAP)

    def calc(self, text):
        match = re.match(r"^calc\s+(\w+)\s*=\s*(.+)$", text)
        tokens = match.group(2).split()
        total = int(tokens[0])
        for operator, operand in zip(tokens[1::2], tokens[2::2]):
            total = total + int(operand) if operator == "+" else total - int(operand)
        self.variables[match.group(1)] = total
        self.log(f"{match.group(1)} = {total}")

    def check(self, text):
        _, left, operator, right = text.split()
        ok = ASSERT_OPS[operator](int(left), int(right))
        self.verdict(ok, f"assert {left} {operator} {right}")

    def until(self, step):
        match = UNTIL_LINE.match(step.text)
        verb, name, command, body, tail = match.groups()
        options = dict(OPTION.findall(tail))
        pattern = re.compile(self.substitute(body, escape=True))
        board = self.boards[name]
        every = float(options.get("every", 2))
        deadline = time.monotonic() + float(options.get("timeout", 30))
        done = re.compile(r"^Done$")
        label = f"{verb} @{name} {command} => /{pattern.pattern}/"

        while True:
            self.send(f"@{name} {command}")
            answered = time.monotonic() + every
            while time.monotonic() < answered:
                if verb == "until" and board.matches(pattern, self.last_send):
                    self.verdict(True, label)
                    return
                if verb == "untilabsent" and board.matches(done, self.last_send):
                    if not board.matches(pattern, self.last_send):
                        self.verdict(True, label)
                        return
                    break
                time.sleep(0.1)
            if time.monotonic() >= deadline:
                self.verdict(False, label)
                return

    def assertion(self, step):
        match = ASSERT_LINE.match(step.text)
        verb, variable, name, body, tail = match.groups()
        options = dict(OPTION.findall(tail))
        pattern = re.compile(self.substitute(body, escape=True))
        board = self.boards[name]
        since = self.since(options)
        label = f"{verb} @{name} /{pattern.pattern}/"

        if verb == "expect":
            self.wait_for(lambda: board.matches(pattern, since),
                          float(options.get("timeout", 10)))
            self.verdict(bool(board.matches(pattern, since)), label)
        elif verb == "absent":
            time.sleep(float(options.get("window", 3)))
            found = board.matches(pattern, since)
            self.verdict(not found, label + (f" (saw: {found[0]})" if found else ""))
        elif verb == "grab":
            self.wait_for(lambda: board.matches(pattern, since),
                          float(options.get("timeout", 10)))
            found = board.matches(pattern, since)
            ok = bool(found) and pattern.groups >= 1
            if ok:
                self.variables[variable] = pattern.search(found[-1]).group(1)
            self.verdict(ok, f"{label} -> {self.variables.get(variable)}")
        else:
            self.count(board, pattern, since, options, label)

    def count(self, board, pattern, since, options, label):
        expected = int(options["n"]) if "n" in options else None
        timeout = float(options.get("timeout", 5))
        if expected:
            self.wait_for(lambda: len(board.matches(pattern, since)) >= expected, timeout)
            time.sleep(1.0)
        else:
            time.sleep(timeout)
        seen = len(board.matches(pattern, since))
        if "save" in options:
            self.variables[options["save"]] = seen
        if expected is None:
            self.log(f"count @{board.name} /{pattern.pattern}/ = {seen}")
        else:
            self.verdict(seen == expected, f"{label} n={expected} (saw {seen})")

    @staticmethod
    def wait_for(condition, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if condition():
                return
            time.sleep(0.1)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("scenario")
    parser.add_argument("--ports", default=DEFAULT_PORTS)
    parser.add_argument("--boards", help="boards to open; default: those the scenario uses")
    parser.add_argument("--log", help="also write the merged log to this file")
    parser.add_argument("--quiet", action="store_true", help="print only verdicts")
    parser.add_argument("--check", action="store_true",
                        help="parse the scenario and exit; opens no port")
    args = parser.parse_args()

    try:
        steps, used = parse(args.scenario)
    except (ScenarioError, OSError, ValueError, IndexError) as error:
        print(f"scenario error: {error}")
        return 2
    if args.check:
        print(f"{args.scenario}: {len(steps)} steps, boards {''.join(sorted(used))}")
        return 0

    ports = dict(pair.split("=") for pair in args.ports.split(","))
    wanted = set(args.boards) if args.boards else used
    runner = Runner(ports, args.log, args.quiet)
    for name in sorted(wanted):
        board = Board(name, ports[name], runner)
        board.start()
        runner.boards[name] = board
    time.sleep(0.5)

    try:
        for step in steps:
            runner.run_step(step)
    except ScenarioError as error:
        runner.verdict(False, f"line {step.number}: {error}")
    time.sleep(1)
    runner.log(f"SUMMARY pass={runner.passed} fail={runner.failed}")
    return 1 if runner.failed else 0


if __name__ == "__main__":
    sys.exit(main())
