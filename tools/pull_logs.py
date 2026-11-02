"""
Downloads the ride logs (and fillups.csv, the fuel calibration points) from
the board's microSD card over the USB cable, without resetting it (the
firmware's log_export.c answers). Files that are already complete locally
are skipped; a shorter local copy is resumed. Each ride's ride.json is
fetched whole every time, and its uid must match the local copy's: a card
ride never lands in a folder that holds a different ride. fillups.csv is
merged row by row, so a formatted card never shortens it. When out_dir is
logs/, site/ is rebuilt afterwards (tools/build_site.py).

First it syncs the board: its clock gets the computer's time (UTC), so rides get
a date, and its ride numbers are kept above the highest ride already in
out_dir, so a freshly formatted card can't start again at ride 1.

    python tools/pull_logs.py /dev/cu.usbmodem* [out_dir]

out_dir defaults to logs/ (git-ignored). Needs pyserial.
"""
import base64
import csv
import io
import json
import re
import sys
import time
import zlib
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parent.parent


# Lines are cut from big reads. pyserial's readline() takes a byte at a time
# and fell behind a 10.9 MB obd_raw.log: lines arrived garbled.
_buf = bytearray()
_pos = 0


def command(s: serial.Serial, cmd: str) -> None:
    global _pos
    s.reset_input_buffer()
    _buf.clear()
    _pos = 0
    s.write(f"\nobd2 {cmd}\n".encode())


def read_line(s: serial.Serial, deadline: float) -> str:
    global _pos
    while True:
        i = _buf.find(b"\n", _pos)
        if i >= 0:
            line = bytes(_buf[_pos:i])
            _pos = i + 1
            return line.decode("ascii", "replace").strip()
        del _buf[:_pos]
        _pos = 0
        if time.time() >= deadline:
            raise TimeoutError("no reply from the board (is this firmware built with log_export?)")
        _buf.extend(s.read(s.in_waiting or 1))


def drain(s: serial.Serial) -> None:
    """Waits for the port to go quiet. A transfer that was given up on keeps
    streaming from the board, and a command sent into it times out."""
    global _pos
    end = time.time() + 120
    quiet_since = time.time()
    while time.time() < end and time.time() - quiet_since < 1.0:
        if s.read(s.in_waiting or 1):
            quiet_since = time.time()
    _buf.clear()
    _pos = 0


def sync(s: serial.Serial, out: Path) -> None:
    rides = [int(m.group(1)) for p in out.glob("ride_*") if (m := re.fullmatch(r"ride_(\d+)", p.name))]
    next_ride = max(rides, default=0) + 1
    deadline = time.time() + 10
    command(s, f"sync {int(time.time())} {next_ride}")
    while True:
        line = read_line(s, deadline)
        if line.startswith("@ERR"):
            print(f"clock: {line[5:]}")
            return
        if line.startswith("@OK "):
            _, was_set, board_next = line.split()
            print(f"clock {'re' if was_set == '1' else ''}set to the Mac's time; the next ride will be "
                  f"ride_{int(board_next):04d}")
            return


def list_files(s: serial.Serial) -> dict[str, int]:
    command(s, "ls")
    files, deadline = {}, time.time() + 10
    while True:
        line = read_line(s, deadline)
        if line == "@ERR no card" and time.time() < deadline - 5:
            time.sleep(2)   # just booted: the card mounts ~2 s in
            command(s, "ls")
            continue
        if line.startswith("@ERR"):
            sys.exit(f"board: {line[5:]}")
        if line == "@END":
            return files
        if line.startswith("@LS "):
            _, name, size = line.split()
            files[name] = int(size)


def get(s: serial.Serial, name: str, offset: int) -> bytes:
    command(s, f"get {name} {offset}")
    deadline = time.time() + 10
    while True:
        line = read_line(s, deadline)
        if line.startswith("@ERR"):
            raise IOError(line[5:])
        if line.startswith("@FILE "):
            size = int(line.split()[2])
            break
    chunks = []
    while True:
        line = read_line(s, time.time() + 10)
        if line.startswith("@EOF "):
            data = b"".join(chunks)
            crc = int(line.split()[1], 16)
            if len(data) != size or zlib.crc32(data) != crc:
                raise IOError(f"{name}: got {len(data)} of {size} bytes, crc {zlib.crc32(data):08x} vs {crc:08x}")
            return data
        if line and not line.startswith(("@", "I (", "W (", "E (")):
            try:
                chunks.append(base64.b64decode(line))
            except ValueError as e:   # a log line cut into a data line
                raise IOError(f"{name}: bad line ({e})") from None


def fetch(s: serial.Serial, name: str, offset: int = 0) -> bytes | None:
    for attempt in range(2):
        try:
            return get(s, name, offset)
        except IOError as e:
            print(f"  retry: {e}")
            drain(s)
    print(f"  {name}: failed")
    return None


def uid_of(data: bytes) -> str | None:
    try:
        return json.loads(data).get("uid")
    except ValueError:
        return None


def check_folder(folder: str, local: Path, card_json: bytes | None) -> str | None:
    """Why the card's folder can't go into the local one, or None if it can."""
    local_json = local / "ride.json"
    if not local.exists():
        return None
    if local_json.exists():
        if card_json is None:
            return "the card's ride has no ride.json yet, the local one does"
        if uid_of(local_json.read_bytes()) != uid_of(card_json):
            return "a different ride (uid) is already there"
        return None
    return None


def merge_fillups(path: Path, data: bytes) -> int:
    """Adds the card's fill-ups that path doesn't have yet; returns how many."""
    card = list(csv.DictReader(io.StringIO(data.decode())))
    fields = data.decode().splitlines()[0].split(",") if data else []
    rows = []
    if path.exists():
        with open(path, newline="") as f:
            reader = csv.DictReader(f)
            rows = list(reader)
            fields += [c for c in reader.fieldnames or [] if c not in fields]
    # The same fill-up whichever header it was written under.
    key = lambda r: tuple(r.get(c) or "" for c in ("fill", "ride", "uptime_s", "litres_pumped"))
    seen = {key(r) for r in rows}
    new = [r for r in card if key(r) not in seen]
    if new or not path.exists():
        with open(path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fields, restval="", extrasaction="ignore", lineterminator="\n")
            w.writeheader()
            w.writerows(rows + new)
    return len(new)


def main() -> None:
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    out = Path(sys.argv[2]) if len(sys.argv) == 3 else ROOT / "logs"
    # pyserial's default open leaves the board running; forcing DTR/RTS low
    # before opening makes the USB-Serial-JTAG reset it.
    s = serial.Serial(sys.argv[1], 115200, timeout=0.5)
    out.mkdir(parents=True, exist_ok=True)
    drain(s)   # what an interrupted run left streaming
    sync(s, out)
    files = list_files(s)
    print(f"{len(files)} files on the card")
    total, t0 = 0, time.time()
    folders: dict[str, dict[str, int]] = {}
    for name, size in files.items():
        if "/" in name:
            folder, base = name.split("/", 1)
            folders.setdefault(folder, {})[base] = size

    for folder, names in sorted(folders.items()):
        local = out / folder
        # ride.json is rewritten in place every 30 s: fetch it whole each time.
        # A power cut mid-rename can leave only ride.json.tmp.
        card_json = None
        for base in ("ride.json", "ride.json.tmp"):
            if base in names:
                card_json = fetch(s, f"{folder}/{base}")
                if card_json is not None and uid_of(card_json):
                    total += len(card_json)
                    break
                card_json = None
        problem = check_folder(folder, local, card_json)
        if problem:
            print(f"  {folder}: skipped, {problem}. Move logs/{folder} aside or pull to another folder.")
            continue
        local.mkdir(parents=True, exist_ok=True)
        if card_json is not None:
            path = local / "ride.json"
            if not path.exists() or path.read_bytes() != card_json:
                path.write_bytes(card_json)
                print(f"  {folder}/ride.json: {len(card_json)} bytes")
        for base, size in sorted(names.items()):
            if base.startswith("ride.json"):
                continue
            path = local / base
            have = path.stat().st_size if path.exists() else 0
            # The others only grow, so resume them.
            if have >= size:
                continue
            offset = have
            data = fetch(s, f"{folder}/{base}", offset)
            if data is None:
                continue
            with open(path, "ab" if offset else "wb") as f:
                f.write(data)
            total += len(data)
            print(f"  {folder}/{base}: {offset + len(data)} bytes" + (f" (resumed at {offset})" if offset else ""))

    # fillups.csv starts again after a format: merge its rows, never overwrite.
    if "fillups.csv" in files:
        data = fetch(s, "fillups.csv")
        if data is not None:
            total += len(data)
            n = merge_fillups(out / "fillups.csv", data)
            if n:
                print(f"  fillups.csv: {n} new fill-up{'s' if n > 1 else ''}")
    dt = time.time() - t0
    print(f"downloaded {total} bytes in {dt:.1f} s" + (f" ({total / dt / 1024:.0f} KB/s)" if total else "") + f" to {out}")
    if out.resolve() == (ROOT / "logs").resolve():
        import build_site
        build_site.main_with(out, ROOT / "site")


if __name__ == "__main__":
    main()
