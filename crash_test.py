#!/usr/bin/env python3
"""Crash test for rowdb: run the workload, SIGKILL it at a random moment, recover, compare.

Every checked state must equal the state built from the ledger:
  - transactions with "C" (commit acknowledged) are fully applied;
  - a transaction with "S" but no "C" (killed during commit) is either fully applied or fully absent;
  - nothing else (aborted, never committed) is present.
Sometimes the check is skipped so that the next workload run performs recovery itself and can be
killed in the middle of it; the following check then covers both runs.
"""
import argparse
import json
import os
import random
import shutil
import signal
import subprocess
import sys
import time


def parse_ledger(path):
    txns, order, recov = {}, [], []
    times = {"S": {}, "C": {}, "L": None}
    if not os.path.exists(path):
        return txns, order, recov, times
    with open(path) as f:
        for line in f:
            if not line.endswith("\n"):
                break
            p = line.split()
            if p[0] == "R":
                recov.append([int(x) for x in p[1:]])
                continue
            if p[0] == "L":
                times["L"] = int(p[2])
                continue
            t = int(p[1])
            if t not in txns:
                txns[t] = {"ops": [], "S": False, "C": False}
                order.append(t)
            if p[0] == "I":
                txns[t]["ops"].append(("I", (int(p[2]), int(p[3])), p[4]))
            elif p[0] == "D":
                txns[t]["ops"].append(("D", (int(p[2]), int(p[3])), None))
            elif p[0] == "S":
                txns[t]["S"] = True
                if len(p) > 2:
                    times["S"][t] = int(p[2])
            elif p[0] == "C":
                txns[t]["C"] = True
                if len(p) > 2:
                    times["C"][t] = int(p[2])
    return txns, order, recov, times


class Conflict(Exception):
    pass


def apply_ops(model, ops):
    for kind, rid, val in ops:
        if kind == "I":
            if rid in model:
                raise Conflict(f"insert into live rid {rid}")
            model[rid] = val
        else:
            if rid not in model:
                raise Conflict(f"delete of missing rid {rid}")
            del model[rid]


def candidates(model, segments):
    """segments: list of (txn, ops, definite). Yields (state, chosen in-doubt txns)."""
    states = [(dict(model), [])]
    for txn, ops, definite in segments:
        nxt = []
        for st, chosen in states:
            if definite:
                try:
                    apply_ops(st, ops)
                    nxt.append((st, chosen))
                except Conflict:
                    pass  # this branch is inconsistent with the ledger
            else:
                applied = dict(st)
                try:
                    apply_ops(applied, ops)
                    nxt.append((applied, chosen + [txn]))
                except Conflict:
                    pass
                nxt.append((st, chosen))
        states = nxt
    return states


def dump(binary, db, engine_args):
    r = subprocess.run([binary, "dump", "--dir", db] + engine_args, capture_output=True, text=True)
    state = {}
    if r.returncode == 0:
        for line in r.stdout.splitlines():
            pg, sl, val = line.split(" ", 2)
            state[(int(pg), int(sl))] = val
    return r.returncode, state, r.stderr.strip(), r.stdout


def parse_recovery(stderr):
    out = {}
    for tok in stderr.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            if v.isdigit():
                out[k] = int(v)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(os.path.dirname(__file__), "build", "rowdb"))
    ap.add_argument("--bug", default="none", choices=["none", "no-wal", "skip-redo", "steal", "no-fsync"])
    ap.add_argument("--frames", type=int, default=32)
    ap.add_argument("--checkpoint-wal-bytes", type=int, default=512 * 1024)
    ap.add_argument("--cycles", type=int, default=300)
    ap.add_argument("--max-delay-ms", type=float, default=250.0)
    ap.add_argument("--check-prob", type=float, default=0.7)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    random.seed(a.seed)
    engine_args = ["--bug", a.bug, "--frames", str(a.frames), "--checkpoint-wal-bytes", str(a.checkpoint_wal_bytes)]
    os.makedirs(a.out, exist_ok=True)
    db = os.path.join(a.out, "db")
    shutil.rmtree(db, ignore_errors=True)
    results = open(os.path.join(a.out, "results.jsonl"), "w")
    model, segments, pending_cycles = {}, [], []
    s = {"cycles": 0, "killed_by_sigkill": 0, "exited_before_kill": 0, "killed_before_recovery_marker": 0, "txn_phase_kills": 0,
         "checks": 0, "pass": 0, "fail": 0, "indoubt_seen": 0, "indoubt_present": 0, "indoubt_absent": 0,
         "acked_txns": 0, "wal_tail_seen": 0, "stalled_cycles": 0, "killed_before_load": 0, "harness_errors": 0, "pages_redone_total": 0, "pages_redone_max": 0, "first_fail_cycle": None, "resets": 0}
    t0 = time.time()

    for cyc in range(a.cycles):
        ledger = os.path.join(a.out, f"ledger_{cyc:05d}.txt")
        seed = a.seed * 1_000_000 + cyc
        delay = random.uniform(0, a.max_delay_ms) / 1000.0
        proc = subprocess.Popen([a.binary, "workload", "--dir", db, "--ledger", ledger, "--seed", str(seed),
                                 "--txn-base", str((cyc + 1) * 10_000_000)] + engine_args,
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        t_start = time.monotonic()
        time.sleep(delay)  # note: on this Mac the actual sleep can be much longer (timer coalescing)
        rec = {"cycle": cyc, "seed": seed, "delay_ms": round(delay * 1000, 1)}
        if proc.poll() is None:
            os.kill(proc.pid, signal.SIGKILL)
        rec["kill_at_ms"] = round((time.monotonic() - t_start) * 1000, 1)  # measured, not requested
        rc = proc.wait()
        err = proc.stderr.read().decode(errors="replace").strip()
        rec["returncode"] = rc
        s["cycles"] += 1
        if rc == -signal.SIGKILL:
            s["killed_by_sigkill"] += 1
        else:
            s["exited_before_kill"] += 1
            rec["workload_stderr"] = err

        txns, order, recov, times = parse_ledger(ledger)
        rec["workload_recovery"] = recov[0] if recov else None
        # progress / latency: startup = R line time; commit latency = C - S
        rec["startup_ms"] = round(recov[0][5] / 1000, 1) if recov and len(recov[0]) > 5 else None
        rec["loaded_ms"] = round(times["L"] / 1000, 1) if times["L"] is not None else None
        if rec["loaded_ms"] is None:
            s["killed_before_load"] += 1  # kill landed during recovery or the startup scan
        lat = [times["C"][t] - times["S"][t] for t in times["C"] if t in times["S"]]
        rec["max_commit_ms"] = round(max(lat) / 1000, 1) if lat else None
        open_s = [t for t in times["S"] if t not in times["C"]]
        if open_s and rec["startup_ms"] is not None:
            rec["open_commit_started_ms"] = round(times["S"][open_s[0]] / 1000, 1)
        if rec["loaded_ms"] is not None and rec["kill_at_ms"] - rec["loaded_ms"] > 50 and not [t for t in order if txns[t]["C"]]:
            s["stalled_cycles"] += 1  # >50ms of transaction time after the startup scan but no commit acknowledged
        elif rc == -signal.SIGKILL and rec["loaded_ms"] is not None:
            s["txn_phase_kills"] += 1  # the kill landed while transactions were running
        if recov and len(recov[0]) > 4 and recov[0][4] > 0:
            s["wal_tail_seen"] += 1
        if not recov:
            s["killed_before_recovery_marker"] += 1  # no R line: killed before (or during) recovery, or before the marker was written
        acked = [t for t in order if txns[t]["C"]]
        indoubt = [t for t in order if txns[t]["S"] and not txns[t]["C"]]
        if len(indoubt) > 1 or (indoubt and order and indoubt[0] != [t for t in order if txns[t]["S"]][-1]):
            rec["harness_error"] = f"unexpected in-doubt set {indoubt}"
            s["harness_errors"] += 1
        rec["acked_txns"] = len(acked)
        rec["indoubt_txn"] = indoubt[0] if indoubt else None
        s["acked_txns"] += len(acked)
        s["indoubt_seen"] += len(indoubt)
        for t in order:
            if txns[t]["S"]:
                segments.append((t, txns[t]["ops"], txns[t]["C"]))
        pending_cycles.append(cyc)

        do_check = rc != -signal.SIGKILL or cyc == a.cycles - 1 or random.random() < a.check_prob
        rec["checked"] = do_check
        if do_check:
            s["checks"] += 1
            drc, state, derr, dout = dump(a.binary, db, engine_args)
            rec["dump_returncode"] = drc
            rec["recovery"] = parse_recovery(derr)
            if rec["recovery"].get("wal_tail_bytes", 0) > 0:
                s["wal_tail_seen"] += 1
            redone = rec["recovery"].get("pages_redone", 0)
            s["pages_redone_total"] += redone
            s["pages_redone_max"] = max(s["pages_redone_max"], redone)
            cands = candidates(model, segments)
            match = None
            if drc == 0 and rc == -signal.SIGKILL:
                for st, chosen in cands:
                    if st == state:
                        match = (st, chosen)
                        break
            if match is not None:
                s["pass"] += 1
                rec["result"] = "pass"
                n_ind = sum(1 for t, _, d in segments if not d)
                s["indoubt_present"] += len(match[1])
                s["indoubt_absent"] += n_ind - len(match[1])
                rec["indoubt_applied"] = match[1]
                model = match[0]
                rec["live_rows"] = len(model)
            else:
                s["fail"] += 1
                rec["result"] = "fail"
                if s["first_fail_cycle"] is None:
                    s["first_fail_cycle"] = cyc
                ref, conflict = dict(model), None
                for t, o, d in segments:
                    if not d:
                        continue
                    try:
                        apply_ops(ref, o)
                    except Conflict as e:
                        # the workload saw a state that contradicts the acknowledged history
                        conflict = f"txn {t}: {e}"
                        break
                acked_ins = {rid for t, o, d in segments if d for k, rid, v in o if k == "I"}
                rec["diff"] = {
                    "dump_error": derr if drc != 0 else None,
                    "ledger_conflict": conflict,
                    "missing_rows": len([r for r in ref if r not in state]) if not conflict else None,
                    "extra_rows": len([r for r in state if r not in ref]) if not conflict else None,
                    "changed_rows": len([r for r in ref if r in state and state[r] != ref[r]]) if not conflict else None,
                    "acked_inserts_missing": len([r for r in acked_ins if r in ref and r not in state]) if not conflict else None,
                }
                # partial transactions: acked txns whose inserts are only partly present
                partial = 0
                for t, o, d in (segments if not conflict else []):
                    ins = [rid for k, rid, v in o if k == "I" and rid in ref]
                    if d and ins:
                        present = sum(1 for r in ins if state.get(r) == ref[r])
                        if 0 < present < len(ins):
                            partial += 1
                rec["diff"]["acked_txns_partially_present"] = partial
                fdir = os.path.join(a.out, "fails", f"cycle_{cyc:05d}")
                os.makedirs(fdir, exist_ok=True)
                for c in pending_cycles:
                    src = os.path.join(a.out, f"ledger_{c:05d}.txt")
                    if os.path.exists(src):
                        shutil.copy(src, fdir)
                with open(os.path.join(fdir, "dump_stdout.txt"), "w") as f:
                    f.write(dout)
                with open(os.path.join(fdir, "dump_stderr.txt"), "w") as f:
                    f.write(derr)
                shutil.rmtree(db, ignore_errors=True)
                model = {}
                s["resets"] += 1
            segments, pending_cycles = [], []
        results.write(json.dumps(rec) + "\n")
        results.flush()

    s["elapsed_s"] = round(time.time() - t0, 1)
    s["final_live_rows"] = len(model)
    for name in ("data.db", "wal.log"):
        p = os.path.join(db, name)
        s[name + "_bytes"] = os.path.getsize(p) if os.path.exists(p) else None
    s["bug"] = a.bug
    s["seed"] = a.seed
    s["frames"] = a.frames
    s["checkpoint_wal_bytes"] = a.checkpoint_wal_bytes
    with open(os.path.join(a.out, "summary.json"), "w") as f:
        json.dump(s, f, indent=1)
    print(json.dumps(s))
    return verdict(s, a)


# What each mode must show. Correct engine and no-fsync (kill -9 cannot see fsync bugs): no
# mismatch and no exit before the kill. Broken controls: at least one mismatch, else the test is weak.
MUST_MATCH = {"none", "no-fsync"}
COVERAGE_MIN_TXN_PHASE = 0.5  # at least half the cycles must be killed while transactions run


def verdict(s, a):
    problems = []
    if a.bug in MUST_MATCH:
        if s["fail"]:
            problems.append(f"{s['fail']} mismatches")
        if s["exited_before_kill"]:
            problems.append(f"{s['exited_before_kill']} workload exits before the kill")
    elif s["fail"] == 0:
        problems.append("control produced no mismatch")
    coverage = []
    if s["checks"] == 0:
        coverage.append("no checks")
    if s["acked_txns"] == 0:
        coverage.append("no acknowledged commits")
    if s["indoubt_seen"] == 0:
        coverage.append("no kill during a commit")
    if s["txn_phase_kills"] < COVERAGE_MIN_TXN_PHASE * s["cycles"]:
        coverage.append(f"only {s['txn_phase_kills']}/{s['cycles']} kills during transactions")
    if s["harness_errors"]:
        s["verdict"], code = "harness_error", 2  # impossible ledger; results are not trustworthy
    elif problems:
        s["verdict"], code = "expectation_failed: " + "; ".join(problems), 1
    elif coverage and a.bug in MUST_MATCH:
        s["verdict"], code = "insufficient_coverage: " + "; ".join(coverage), 3
    else:
        s["verdict"], code = "ok" + (" (coverage notes: " + "; ".join(coverage) + ")" if coverage else ""), 0
    with open(os.path.join(a.out, "summary.json"), "w") as f:
        json.dump(s, f, indent=1)
    print("verdict:", s["verdict"], file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
