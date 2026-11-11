#!/usr/bin/env python3
"""Checks of the crash-test harness logic (no engine needed). Run: python3 tests/test_harness.py"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import crash_test as ct  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", what)


A, B = (1, 0), (1, 1)
cycle1 = [(1, [("I", A, "a")], True),    # acknowledged
          (2, [("I", B, "b")], False)]   # killed during commit (in doubt)

# The hole the startup check closes: cycles 1 and 2 are not dump-checked. Cycle 2's startup saw
# txn 2 applied, then txn 2's row is gone by the dump after cycle 3. Judged only at that dump,
# "txn 2 absent" is still an allowed state, so the loss passes.
check(any(st == {A: "a"} for st, _ in ct.candidates({}, cycle1)), "without the startup check the lost row passes")

# With the startup check, cycle 2's scan settles txn 2 as applied ...
hit = ct.resolve_startup({}, cycle1, 2, ct.state_crc({A: "a", B: "b"}))
check(hit is not None and hit[1] == [2], "startup scan with txn 2 applied resolves it as applied")
model = hit[0] if hit else {}
# ... and cycle 3's scan without it no longer matches anything.
check(ct.resolve_startup(model, [], 1, ct.state_crc({A: "a"})) is None, "row that disappeared after being seen is a mismatch")

# Absent is also a legal resolution, and it sticks the same way.
hit = ct.resolve_startup({}, cycle1, 1, ct.state_crc({A: "a"}))
check(hit is not None and hit[1] == [], "startup scan without txn 2 resolves it as absent")
check(ct.resolve_startup(hit[0] if hit else {}, [], 2, ct.state_crc({A: "a", B: "b"})) is None,
      "row that appears after being settled absent is a mismatch")

# An acknowledged transaction can never be resolved away, and the row count is part of the match.
check(ct.resolve_startup({}, cycle1, 0, ct.state_crc({})) is None, "losing an acknowledged row is a mismatch")
check(ct.resolve_startup({}, cycle1, 3, ct.state_crc({A: "a", B: "b"})) is None, "row count must match too")

# The CRC, not just the row count, decides: same count with different contents is a mismatch ...
check(ct.resolve_startup({A: "a"}, [], 1, ct.state_crc({A: "z"})) is None, "same row count, different value")
check(ct.resolve_startup({A: "a"}, [], 1, ct.state_crc({B: "a"})) is None, "same row count, different row id")
# ... and an in-doubt transaction that keeps the row count (delete one row, insert another) is
# settled by the digest alone.
C = (2, 0)
swap = [(3, [("D", A, None), ("I", C, "c")], False)]
hit = ct.resolve_startup({A: "a"}, swap, 1, ct.state_crc({C: "c"}))
check(hit is not None and hit[1] == [3] and hit[0] == {C: "c"}, "digest selects the applied state")
hit = ct.resolve_startup({A: "a"}, swap, 1, ct.state_crc({A: "a"}))
check(hit is not None and hit[1] == [] and hit[0] == {A: "a"}, "digest selects the omitted state")

# A transaction that inserts a row and deletes it again leaves the same state either way: it is
# counted as undecidable, not as applied.
s = {"indoubt_present": 0, "indoubt_absent": 0, "indoubt_noop": 0}
ct.count_indoubt(s, [(4, [("I", C, "c"), ("D", C, None)], False), (5, [("I", (3, 0), "d")], False),
                     (6, [("I", (3, 1), "e")], False), (7, [("I", (3, 2), "f")], True)], [4, 5])
check(s == {"indoubt_present": 1, "indoubt_absent": 1, "indoubt_noop": 1}, f"in-doubt counters {s}")

# A broken control only passes through a real mismatch: failures that come only from the workload
# exiting before the kill do not count as detecting the bug.
import argparse  # noqa: E402
import tempfile  # noqa: E402
with tempfile.TemporaryDirectory(dir=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")) as tmp:
    base = {"checks": 5, "startup_checks": 5, "acked_txns": 10, "indoubt_seen": 2, "txn_phase_kills": 10,
            "cycles": 10, "harness_errors": 0, "exited_before_kill": 3, "startup_mismatch": 0}
    early = dict(base, fail=3, state_mismatch=0)
    check(ct.verdict(early, argparse.Namespace(bug="steal", out=tmp)) == 1, "control with only early-exit failures fails")
    real = dict(base, fail=4, state_mismatch=1)
    check(ct.verdict(real, argparse.Namespace(bug="steal", out=tmp)) == 0, "control with a real mismatch passes")

# The CRC follows the workload's format: "<page> <slot> <value>\n" in (page, slot) order.
import zlib  # noqa: E402
check(ct.state_crc({(2, 0): "y", (1, 5): "x"}) == zlib.crc32(b"1 5 x\n2 0 y\n"), "crc uses scan order and row format")


# --- With the real binary: the workload's digest must match state_crc, and dump must refuse a
# damaged page. Skipped (and reported) if the engine has not been built.
import shutil  # noqa: E402
import signal  # noqa: E402
import struct  # noqa: E402
import subprocess  # noqa: E402
import time  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
BIN = os.path.join(ROOT, "build", "rowdb")


def run_workload_until(db, ledger, seed, base, ready):
    """Start the workload, kill -9 it once ready(ledger lines) is true (or after 5 s)."""
    p = subprocess.Popen([BIN, "workload", "--dir", db, "--ledger", ledger, "--seed", str(seed),
                          "--txn-base", str(base)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 5
    while time.time() < deadline:
        lines = open(ledger).read().splitlines() if os.path.exists(ledger) else []
        if ready(lines):
            break
        time.sleep(0.01)
    p.send_signal(signal.SIGKILL)
    p.wait()
    return open(ledger).read().splitlines()


def run_dump(db):
    r = subprocess.run([BIN, "dump", "--dir", db], capture_output=True, text=True)
    rows = {}
    for line in r.stdout.splitlines():
        pg, sl, val = line.split(" ", 2)
        rows[(int(pg), int(sl))] = val
    return r.returncode, rows, r.stderr


if not os.path.exists(BIN):
    print("SKIP: build/rowdb not found, binary checks not run")
else:
    work = os.path.join(ROOT, "build", "harness-test")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    db = os.path.join(work, "db")
    # 1. some committed data, then a recovered state D listed by dump
    run_workload_until(db, os.path.join(work, "l1.txt"), 3, 1,
                       lambda ls: sum(1 for x in ls if x.startswith("C ")) >= 50)
    rc, d_rows, err = run_dump(db)
    check(rc == 0 and len(d_rows) > 0, f"dump of a healthy db exits 0 with rows (rc={rc}, {err.strip()[-80:]})")
    # 2. the next workload's startup scan sees exactly D: its L line must carry state_crc(D)
    l2 = run_workload_until(db, os.path.join(work, "l2.txt"), 4, 1_000_000,
                            lambda ls: any(x.startswith("L ") for x in ls))
    lline = [x.split() for x in l2 if x.startswith("L ")]
    check(len(lline) == 1 and len(lline[0]) == 4, f"workload wrote one L line with a digest ({lline})")
    if lline and len(lline[0]) == 4:
        check(int(lline[0][1]) == len(d_rows), "L line row count equals the dumped rows")
        check(int(lline[0][3], 16) == ct.state_crc(d_rows), "C++ startup digest equals state_crc of the same rows")
    # 3. damage one slot (value would run past the page end), re-seal the page CRC: dump must fail
    rc, d_rows, err = run_dump(db)
    check(rc == 0, "dump still exits 0 before the damage")
    (pg, sl) = max(d_rows)
    with open(os.path.join(db, "data.db"), "r+b") as f:
        f.seek(pg * 4096)
        page = bytearray(f.read(4096))
        page[12 + 4 * sl:16 + 4 * sl] = struct.pack("<HH", 4090, 100)
        page[0:4] = struct.pack("<I", zlib.crc32(bytes(page[4:])))
        f.seek(pg * 4096)
        f.write(page)
    rc, _, err = run_dump(db)
    check(rc == 3 and "out of bounds" in err, f"dump of a damaged slot exits 3 with the bounds error (rc={rc}, {err.strip()[-80:]})")
    shutil.rmtree(work, ignore_errors=True)

if failures:
    print(f"{failures} harness check(s) failed")
    sys.exit(1)
print("all harness checks passed")
